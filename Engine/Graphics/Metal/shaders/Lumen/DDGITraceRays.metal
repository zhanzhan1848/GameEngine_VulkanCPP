/**
 * @file DDGITraceRays.metal
 * @brief Lumen DDGI - Probe ray tracing through GlobalSDF
 *
 * Each thread traces one ray from one probe through the GlobalSDF volume.
 * On hit, looks up radiance from the Surface Cache lighting atlas via
 * CardLookup → worldToCardUV → sample lighting_atlas.
 * Falls back to sky color for miss or when Surface Cache is unavailable.
 * Backface rejection: zero radiance when dot(normal, rayDir) > 0.
 *
 * Dispatch: (ProbeUpdateCount * RaysPerProbe, 1, 1)
 * ThreadGroupSize: (64, 1, 1)
 */

#include <metal_stdlib>
using namespace metal;

#include "../CommonTypes.metal"
#include "../CommonFunction.metal"
#include "DDGIVolumeData.metal"
#include "SurfaceCacheData.metal"
#include "SDFTraceCommon.metal"

// ============================================================================
// DDGI-specific SDF trace wrapper (adapts DDGIVolumeData → shared trace params)
// ============================================================================

static SDFHitResult ddgiTraceSDF(
    float3 rayOrigin, float3 rayDir, float maxDist,
    texture3d<float, access::sample> sdf0,
    texture3d<float, access::sample> sdf1,
    texture3d<float, access::sample> sdf2,
    constant DDGIVolumeData& vol)
{
    uint coarsest = min(2u, vol.SdfCascadeCount - 1u);
    float fallbackStride = vol.SdfExtents[coarsest].x * 0.1f;

    SDFHitResult result = traceSDF_relaxed(
        rayOrigin, rayDir, maxDist,
        vol.SdfVoxelSizes[0].x, fallbackStride,
        sdf0, sdf1, sdf2,
        vol.SdfOrigins[0], vol.SdfOrigins[1], vol.SdfOrigins[2],
        vol.SdfExtents[0], vol.SdfExtents[1], vol.SdfExtents[2],
        vol.SdfCascadeCount);

    // No gradient normal — saves 6×sampleBestSDF = 18 texture3D reads.
    return result;
}

// ============================================================================
// Main Kernel
// ============================================================================

kernel void ddgi_trace_rays(
    uint global_id [[thread_position_in_grid]],
    uint tid [[thread_position_in_threadgroup]],
    uint groupSize [[threads_per_threadgroup]],

    // SDF cascade textures
    texture3d<float, access::sample> sdf_cascade_0 [[texture(0)]],
    texture3d<float, access::sample> sdf_cascade_1 [[texture(1)]],
    texture3d<float, access::sample> sdf_cascade_2 [[texture(2)]],
    // Surface Cache lighting atlas (view-independent radiance)
    texture2d<float, access::sample> lighting_atlas [[texture(3)]],

    // Global shader data (matrices)
    device GlobalShaderData& gd [[buffer(0)]],

    // DDGI volume data (includes light direction + color)
    constant DDGIVolumeData& volume [[buffer(1)]],

    // Output: ray data storage buffer
    device DDGIRayData* ray_buffer [[buffer(2)]],

    // Probe update list (sparse indices of probes to update this frame)
    device const uint* probeUpdateList [[buffer(3)]],

    // Surface Cache card lookup data (mesh AABBs → card indices)
    device const SurfaceCacheCardLookup* card_lookups [[buffer(4)]],
    // Surface Cache card data (atlas regions + axis info)
    device const SurfaceCacheCard* card_data [[buffer(5)]]
)
{
    // =====================================================================
    // Phase 0: Cooperatively load card lookup AABBs into threadgroup memory.
    // ThreadGroupSize=64, RaysPerProbe=64 → each group = one probe.
    // Each thread loads ~2 lookups → ~2 device buffer reads per thread
    // (well within Apple Silicon ~32 read limit).
    // ALL threads must reach the barrier — no early returns before it.
    // =====================================================================
    constexpr uint kMaxLookups = 128;
    threadgroup float4 tg_aabb_min[kMaxLookups];
    threadgroup float4 tg_aabb_max[kMaxLookups];
    threadgroup uint2 tg_card_info[kMaxLookups]; // x=card_start, y=card_count

    uint totalRays = volume.ProbeUpdateCount * volume.RaysPerProbe;
    uint lookupCount = min(volume.SCLookupCount, kMaxLookups);

    for (uint i = tid; i < lookupCount; i += groupSize) {
        tg_aabb_min[i] = card_lookups[i].aabb_min;
        tg_aabb_max[i] = card_lookups[i].aabb_max;
        tg_card_info[i] = uint2(card_lookups[i].card_start, card_lookups[i].card_count);
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    // Safe to early-return after the barrier
    if (global_id >= totalRays) return;

    // =====================================================================
    // Phase 1: Per-ray SDF tracing + radiance lookup
    // =====================================================================
    uint localProbeIdx = global_id / volume.RaysPerProbe;
    uint localRayIdx   = global_id % volume.RaysPerProbe;

    // Map sparse update index to real probe index
    uint probeIdx = probeUpdateList[localProbeIdx];
    if (probeIdx >= volume.ProbeCountTotal) return;

    uint3 gc = ddgiProbeGridCoord(probeIdx, ddgiGetProbeCounts(volume));
    float3 probePos = ddgiProbeWorldPos(gc, volume.ProbeOrigin.xyz, volume.ProbeSpacing);

    float3 rayDir = ddgiRayDirection(localRayIdx, volume.RaysPerProbe, volume.FrameIndex);

    // Trace ray through SDF
    SDFHitResult hit = ddgiTraceSDF(
        probePos, rayDir, volume.RayMaxDistance,
        sdf_cascade_0, sdf_cascade_1, sdf_cascade_2, volume);

    DDGIRayData result;

    if (hit.hit) {
        result.radiance_and_dist.w = hit.distance;

        float3 radiance = DDGI_SKY_COLOR;
        float atlasSize = float(volume.SCAtlasSize);

        // Search threadgroup-cached lookups for Surface Cache radiance
        // Apple Silicon: cap per-ray device reads to ~16 to stay within ~32 limit
        uint deviceReads = 0u;
        constexpr uint kMaxDeviceReads = 16u;
        for (uint li = 0; li < lookupCount && deviceReads < kMaxDeviceReads; ++li) {
            if (hit.position.x < tg_aabb_min[li].x || hit.position.x > tg_aabb_max[li].x ||
                hit.position.y < tg_aabb_min[li].y || hit.position.y > tg_aabb_max[li].y ||
                hit.position.z < tg_aabb_min[li].z || hit.position.z > tg_aabb_max[li].z)
                continue;

            bool found = false;
            for (uint ci = 0; ci < min(tg_card_info[li].y, 6u) && deviceReads < kMaxDeviceReads; ++ci) {
                uint cardIdx = tg_card_info[li].x + ci;
                if (cardIdx >= 4096u) continue;
                device const SurfaceCacheCard& card = card_data[cardIdx];
                ++deviceReads;
                float2 atlasUV;
                if (worldToCardUVDevice(hit.position, card, atlasUV)) {
                    float2 normUV = atlasUV / atlasSize;
                    sampler samp(coord::normalized, filter::linear, address::clamp_to_edge);
                    radiance = lighting_atlas.sample(samp, normUV).xyz;
                    found = true;
                    break;
                }
            }
            if (found) break;
        }

        result.radiance_and_dist.xyz = radiance;
    } else {
        // Miss: negative distance signals miss
        result.radiance_and_dist.w = -1.0f;
        result.radiance_and_dist.xyz = DDGI_SKY_COLOR;
    }

    ray_buffer[probeIdx * volume.RaysPerProbe + localRayIdx] = result;
}

// ============================================================================
// Split Trace Kernels (Apple Silicon stable)
//
// Apple Silicon limits texture3D reads to ~4 per thread. Mixing texture3D
// reads with buffer reads in the same dispatch causes flickering.
//
// Solution: split into two dispatches:
//   1. ddgi_trace_sdf: texture3D-only (4 SDF steps) → writes hit_distance_buffer
//   2. ddgi_trace_finalize: buffer-only + texture2D → reads hit_distance_buffer,
//      looks up Surface Cache radiance, writes ray_buffer
//
// Both use (8,8,1) threadgroups, one group per probe.
// ============================================================================

kernel void ddgi_trace_sdf(
    uint2 tid2d [[thread_position_in_threadgroup]],
    uint2 gid2d [[threadgroup_position_in_grid]],

    texture3d<float, access::sample> sdf_cascade_0 [[texture(0)]],
    texture3d<float, access::sample> sdf_cascade_1 [[texture(1)]],
    texture3d<float, access::sample> sdf_cascade_2 [[texture(2)]],

    constant DDGIVolumeData& volume [[buffer(1)]],
    device float* hit_distance_buffer [[buffer(2)]],
    device const uint* probeUpdateList [[buffer(3)]]
)
{
    uint tid = tid2d.y * 8u + tid2d.x;
    uint group_id = gid2d.x;

    if (group_id >= volume.ProbeUpdateCount) return;
    if (tid >= volume.RaysPerProbe) return;

    uint probeIdx = probeUpdateList[group_id];
    if (probeIdx >= volume.ProbeCountTotal) return;
    uint globalRayIdx = group_id * volume.RaysPerProbe + tid;

    uint3 gc = ddgiProbeGridCoord(probeIdx, ddgiGetProbeCounts(volume));
    float3 probePos = ddgiProbeWorldPos(gc, volume.ProbeOrigin.xyz, volume.ProbeSpacing);
    float3 rayDir = ddgiRayDirection(tid, volume.RaysPerProbe, volume.FrameIndex);

    // Relaxed sphere tracing (texture3D reads only)
    float t = 0.0f;
    float hitDistance = -1.0f;

    float hitThreshold = volume.SdfVoxelSizes[0].x * 0.5f;
    float minStep = volume.SdfVoxelSizes[0].x * 0.25f;
    uint coarsest = min(2u, volume.SdfCascadeCount - 1u);
    float fallbackStride = volume.SdfExtents[coarsest].x * 0.1f;

    for (uint step = 0; step < DDGI_MAX_SDF_STEPS; ++step) {
        float3 pos = probePos + rayDir * t;

        // else-if cascade selection — 1 texture3D read
        float d = sampleBestSDF_elseIf(pos, sdf_cascade_0, sdf_cascade_1, sdf_cascade_2,
                                        volume.SdfOrigins[0], volume.SdfOrigins[1], volume.SdfOrigins[2],
                                        volume.SdfExtents[0], volume.SdfExtents[1], volume.SdfExtents[2],
                                        volume.SdfCascadeCount);

        // Outside all cascades — fallback stride
        if (d >= 1e9f) {
            t += fallbackStride;
            if (t > volume.RayMaxDistance) break;
            continue;
        }

        if (d < hitThreshold) {
            hitDistance = t;
            break;
        }

        // Relaxed advancement: overstep when far, conservative when close
        float advance;
        if (d > hitThreshold * 4.0f) {
            advance = d * 1.2f;
        } else {
            advance = max(d, minStep);
        }
        t += advance;
        if (t > volume.RayMaxDistance) break;
    }

    hit_distance_buffer[globalRayIdx] = hitDistance;
}

// ============================================================================
// Canonical radiance chain helpers (T3.1) — ported from DDGITraceFinalize.comp.
// Chain: prev_frame_color (screen) → SC atlas (grid) → analytic (direct+sky),
// plus the self-circulating multi-bounce term albedo × L_i_prev × 0.85.
// ============================================================================

static float3 scSamplePrevColor(float3 hitPos, float4x4 vp,
                                texture2d<float, access::sample> prev_color) {
    float4 clip = vp * float4(hitPos, 1.0f);
    if (clip.w <= 0.0f) return float3(0.0f);
    float3 ndc = clip.xyz / clip.w;
    if (ndc.x < -1.0f || ndc.x > 1.0f || ndc.y < -1.0f || ndc.y > 1.0f ||
        ndc.z < 0.0f || ndc.z > 1.0f) {
        return float3(0.0f);  // off-screen hit
    }
    float2 uv = float2(ndc.x * 0.5f + 0.5f, 1.0f - (ndc.y * 0.5f + 0.5f));
    uint2 dims = uint2(prev_color.get_width(), prev_color.get_height());
    if (dims.x == 0u || dims.y == 0u) return float3(0.0f);
    uint2 texel = clamp(uint2(uv * float2(dims)), uint2(0), dims - uint2(1));
    return prev_color.read(texel).rgb;
}

static float3 scSampleHitAlbedo(float3 hitPos, float4x4 vp, float3 fallback,
                                texture2d<float, access::sample> gbuffer_albedo) {
    float4 clip = vp * float4(hitPos, 1.0f);
    if (clip.w <= 0.0f) return fallback;
    float3 ndc = clip.xyz / clip.w;
    if (ndc.x < -1.0f || ndc.x > 1.0f || ndc.y < -1.0f || ndc.y > 1.0f ||
        ndc.z < 0.0f || ndc.z > 1.0f) return fallback;
    float2 uv = float2(ndc.x * 0.5f + 0.5f, 1.0f - (ndc.y * 0.5f + 0.5f));
    uint2 dims = uint2(gbuffer_albedo.get_width(), gbuffer_albedo.get_height());
    if (dims.x == 0u || dims.y == 0u) return fallback;
    uint2 texel = clamp(uint2(uv * float2(dims)), uint2(0), dims - uint2(1));
    return gbuffer_albedo.read(texel).rgb;
}

// Tetrahedral 4-probe weights (same case split as the GLSL version).
struct SCTetraResult {
    uint pi[4];
    float bw[4];
};

static SCTetraResult scTetrahedral(float3 gp, uint3 gd) {
    SCTetraResult r;
    uint3 b0 = clamp(uint3(floor(gp)), uint3(0u), gd - 1u);
    uint3 b1 = min(b0 + 1u, gd - 1u);
    uint gx = gd.x;
    uint gxy = gd.x * gd.y;

    uint p[8];
    p[0] = b0.x + b0.y * gx + b0.z * gxy;
    p[1] = b1.x + b0.y * gx + b0.z * gxy;
    p[2] = b0.x + b1.y * gx + b0.z * gxy;
    p[3] = b1.x + b1.y * gx + b0.z * gxy;
    p[4] = b0.x + b0.y * gx + b1.z * gxy;
    p[5] = b1.x + b0.y * gx + b1.z * gxy;
    p[6] = b0.x + b1.y * gx + b1.z * gxy;
    p[7] = b1.x + b1.y * gx + b1.z * gxy;

    float fx = fract(gp.x);
    float fy = fract(gp.y);
    float fz = fract(gp.z);

    if (fx >= fy && fy >= fz) {
        r.pi[0]=p[0]; r.pi[1]=p[1]; r.pi[2]=p[3]; r.pi[3]=p[7];
        r.bw[0]=1.0f-fx; r.bw[1]=fx-fy; r.bw[2]=fy-fz; r.bw[3]=fz;
    } else if (fx >= fz && fz >= fy) {
        r.pi[0]=p[0]; r.pi[1]=p[1]; r.pi[2]=p[5]; r.pi[3]=p[7];
        r.bw[0]=1.0f-fx; r.bw[1]=fx-fz; r.bw[2]=fz-fy; r.bw[3]=fy;
    } else if (fy >= fx && fx >= fz) {
        r.pi[0]=p[0]; r.pi[1]=p[2]; r.pi[2]=p[3]; r.pi[3]=p[7];
        r.bw[0]=1.0f-fy; r.bw[1]=fy-fx; r.bw[2]=fx-fz; r.bw[3]=fz;
    } else if (fy >= fz && fz >= fx) {
        r.pi[0]=p[0]; r.pi[1]=p[2]; r.pi[2]=p[6]; r.pi[3]=p[7];
        r.bw[0]=1.0f-fy; r.bw[1]=fy-fz; r.bw[2]=fz-fx; r.bw[3]=fx;
    } else if (fz >= fx && fx >= fy) {
        r.pi[0]=p[0]; r.pi[1]=p[4]; r.pi[2]=p[5]; r.pi[3]=p[7];
        r.bw[0]=1.0f-fz; r.bw[1]=fz-fx; r.bw[2]=fx-fy; r.bw[3]=fy;
    } else {
        r.pi[0]=p[0]; r.pi[1]=p[4]; r.pi[2]=p[6]; r.pi[3]=p[7];
        r.bw[0]=1.0f-fz; r.bw[1]=fz-fy; r.bw[2]=fy-fx; r.bw[3]=fx;
    }
    for (uint i = 0u; i < 4u; ++i) r.bw[i] = max(r.bw[i], 0.0f);
    return r;
}

static float3 scShDot4(float3 c0, float3 c1, float3 c2, float3 c3, float3 d) {
    constexpr float C0 = 0.282095f;
    constexpr float C1 = 0.488603f;
    return c0 * C0 + c1 * (-C1 * d.y) + c2 * (C1 * d.z) + c3 * (-C1 * d.x);
}

// Previous-frame probe irradiance at a world position (multi-bounce feedback).
// irradiance_history is the vec4-strided SH3 buffer: 9 coefficients per probe.
static float3 scSamplePrevProbeGrid(float3 pos, float3 N,
                                    constant DDGIVolumeData& volume,
                                    device const float4* irradiance_history) {
    float3 origin = volume.ProbeOrigin.xyz;
    float spacing = volume.ProbeSpacing;
    uint3 counts = ddgiGetProbeCounts(volume);
    float3 gp = (pos - origin) / spacing;
    float3 gridMax = float3(counts - 1u);
    if (any(gp < float3(0.0f)) || any(gp > gridMax)) return float3(0.0f);

    SCTetraResult tet = scTetrahedral(gp, counts);
    float3 result = float3(0.0f);
    float totalWeight = 0.0f;
    for (uint p = 0u; p < 4u; ++p) {
        if (tet.bw[p] < 0.001f) continue;
        uint base = tet.pi[p] * 9u;
        float3 sh0 = irradiance_history[base + 0u].xyz;
        float3 sh1 = irradiance_history[base + 1u].xyz;
        float3 sh2 = irradiance_history[base + 2u].xyz;
        float3 sh3 = irradiance_history[base + 3u].xyz;
        result += scShDot4(sh0, sh1, sh2, sh3, N) * tet.bw[p];
        totalWeight += tet.bw[p];
    }
    if (totalWeight > 0.0f) return result / totalWeight;
    return float3(0.0f);
}

kernel void ddgi_trace_finalize(
    uint2 tid2d [[thread_position_in_threadgroup]],
    uint2 gid2d [[threadgroup_position_in_grid]],

    texture2d<float, access::sample> lighting_atlas [[texture(3)]],
    texture2d<float, access::sample> prev_frame_color [[texture(4)]],
    texture2d<float, access::sample> gbuffer_albedo [[texture(5)]],

    constant DDGIVolumeData& volume [[buffer(1)]],
    device const float* hit_distance_buffer [[buffer(2)]],
    device const uint* probeUpdateList [[buffer(3)]],
    device DDGIRayData* ray_buffer [[buffer(4)]],
    device const SurfaceCacheCardLookup* card_lookups [[buffer(5)]],
    device const SurfaceCacheCard* card_data [[buffer(6)]],
    device const uint* sc_grid_offsets [[buffer(7)]],  // T1.3: header + prefix sums
    device const uint* sc_grid_cards   [[buffer(8)]],  // T1.3: card indices
    constant float4x4& view_projection [[buffer(9)]],  // T3.1: screen projection
    device const float4* irradiance_history [[buffer(10)]]  // T3.1: multi-bounce
)
{
    uint tid = tid2d.y * 8u + tid2d.x;
    uint group_id = gid2d.x;

    // T1.3: card lookup via uniform grid — no cooperative preload, no
    // threadgroup cache, no barrier. The grid buffers self-describe from a
    // 16-u32 header (world AABB, cell size, dims).

    if (group_id >= volume.ProbeUpdateCount) return;
    if (tid >= volume.RaysPerProbe) return;

    uint probeIdx = probeUpdateList[group_id];
    if (probeIdx >= volume.ProbeCountTotal) return;
    uint globalRayIdx = group_id * volume.RaysPerProbe + tid;
    float hitDistance = hit_distance_buffer[globalRayIdx];

    DDGIRayData result;

    if (hitDistance >= 0.0f) {
        result.radiance_and_dist.w = hitDistance;

        // Recompute hit position from probe data + distance
        uint3 gc = ddgiProbeGridCoord(probeIdx, ddgiGetProbeCounts(volume));
        float3 probePos = ddgiProbeWorldPos(gc, volume.ProbeOrigin.xyz, volume.ProbeSpacing);
        float3 rayDir = ddgiRayDirection(tid, volume.RaysPerProbe, volume.FrameIndex);
        float3 hitPos = probePos + rayDir * hitDistance;

        // ---- Canonical radiance chain (T3.1, mirrors DDGITraceFinalize.comp):
        // base = prev_frame_color (screen) → SC atlas (grid) → analytic
        // (direct + sky); multi-bounce = albedo × L_i_prev × 0.85. ----
        float3 N = -rayDir;
        float3 albedo = scSampleHitAlbedo(hitPos, view_projection, volume.Albedo.xyz, gbuffer_albedo);
        float3 L_i_prev = scSamplePrevProbeGrid(hitPos, N, volume, irradiance_history);

        float3 base = scSamplePrevColor(hitPos, view_projection, prev_frame_color);
        if (dot(base, base) <= 0.0001f) {
            // Off-screen: SC atlas via uniform grid (T1.3)
            base = DDGI_SKY_COLOR;   // provisional; analytic below is the true fallback
            float atlasSize = float(volume.SCAtlasSize);
            uint deviceReads = 0u;
            constexpr uint kMaxDeviceReads = 16u;
            uint4 gh0 = uint4(sc_grid_offsets[0], sc_grid_offsets[1],
                              sc_grid_offsets[2], sc_grid_offsets[3]);
            uint4 gh1 = uint4(sc_grid_offsets[4], sc_grid_offsets[5],
                              sc_grid_offsets[6], sc_grid_offsets[7]);
            uint4 gh2 = uint4(sc_grid_offsets[8], sc_grid_offsets[9],
                              sc_grid_offsets[10], sc_grid_offsets[11]);
            SCGridParams gp = scGridDecode(gh0, gh1, gh2);
            uint cell = scGridCellIndex(gp, hitPos);
            bool scHit = false;
            if (cell != 0xFFFFFFFFu) {
                uint begin = sc_grid_offsets[16u + cell];
                uint end   = sc_grid_offsets[17u + cell];
                for (uint gi = begin; gi < end && deviceReads < kMaxDeviceReads; ++gi) {
                    uint cardIdx = sc_grid_cards[gi];
                    if (cardIdx >= 4096u) continue;
                    device const SurfaceCacheCard& card = card_data[cardIdx];
                    ++deviceReads;
                    float2 atlasUV;
                    if (worldToCardUVDevice(hitPos, card, atlasUV)) {
                        float2 normUV = atlasUV / atlasSize;
                        sampler samp(coord::normalized, filter::linear, address::clamp_to_edge);
                        base = lighting_atlas.sample(samp, normUV).xyz;
                        scHit = true;
                        break;
                    }
                }
            }
            if (!scHit) {
                // Analytic: self-computed direct + sky (GLSL-parity formula)
                float3 L = normalize(-volume.LightDirection.xyz);
                float NdotL = max(0.0f, dot(N, L));
                float3 E_direct = volume.LightColor.xyz * volume.LightColor.w * NdotL;
                float NdotUp = max(0.0f, N.y);
                float3 E_sky = volume.SkyColor.xyz * (0.5f + 0.5f * NdotUp);
                constexpr float PI_VAL = 3.14159265358979f;
                base = (albedo / PI_VAL) * (E_direct + E_sky);
            }
        }

        result.radiance_and_dist.xyz = base + albedo * L_i_prev * 0.85f;
    } else {
        result.radiance_and_dist.w = -1.0f;

        // Miss fallback: search for nearby SC cards centered on probe position
        float3 fallbackRadiance = DDGI_SKY_COLOR;
        uint3 gc = ddgiProbeGridCoord(probeIdx, ddgiGetProbeCounts(volume));
        float3 probePos = ddgiProbeWorldPos(gc, volume.ProbeOrigin.xyz, volume.ProbeSpacing);
        float atlasSize = float(volume.SCAtlasSize);

        bool missFound = false;
        uint missReads = 0u;
        constexpr uint kMaxMissReads = 8u;
        uint4 gh0 = uint4(sc_grid_offsets[0], sc_grid_offsets[1],
                          sc_grid_offsets[2], sc_grid_offsets[3]);
        uint4 gh1 = uint4(sc_grid_offsets[4], sc_grid_offsets[5],
                          sc_grid_offsets[6], sc_grid_offsets[7]);
        uint4 gh2 = uint4(sc_grid_offsets[8], sc_grid_offsets[9],
                          sc_grid_offsets[10], sc_grid_offsets[11]);
        SCGridParams gp = scGridDecode(gh0, gh1, gh2);
        uint cell = scGridCellIndex(gp, probePos);
        if (cell != 0xFFFFFFFFu) {
            uint begin = sc_grid_offsets[16u + cell];
            uint end   = sc_grid_offsets[17u + cell];
            for (uint gi = begin; gi < end && missReads < kMaxMissReads; ++gi) {
                uint cardIdx = sc_grid_cards[gi];
                if (cardIdx >= 4096u) continue;
                device const SurfaceCacheCard& card = card_data[cardIdx];
                ++missReads;
                float2 atlasUV;
                if (worldToCardUVDevice(probePos, card, atlasUV)) {
                    float2 normUV = atlasUV / atlasSize;
                    sampler samp(coord::normalized, filter::linear, address::clamp_to_edge);
                    fallbackRadiance = lighting_atlas.sample(samp, normUV).xyz;
                    missFound = true;
                    break;
                }
            }
        }
        result.radiance_and_dist.xyz = fallbackRadiance;
    }

    ray_buffer[probeIdx * volume.RaysPerProbe + tid] = result;
}
