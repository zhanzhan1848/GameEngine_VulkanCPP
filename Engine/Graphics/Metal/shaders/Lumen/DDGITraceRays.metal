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
// Probe ray sampler: ddgiSampleRayDirection lives in DDGIVolumeData.metal
// (T8.13 sync) — single source shared with the SH projection side.
// ============================================================================

// ============================================================================
// Probe relocation (ray-origin only) — port of DDGITraceSDF.comp
// ddgiRelocateProbeOrigin. Pushes a buried ray origin out along the SDF until
// it clears the hit-bias skin, capped at 0.45 × spacing so gather-side grid-
// distance visibility tests stay valid. Searches 6 axes + 8 diagonals per
// round and keeps the candidate with the LARGEST SDF distance (actual
// free-air volume). Without this, probes sitting inside arcade masonry bake
// wall-interior black and starve the second-floor GI.
// ============================================================================
static float3 ddgiRelocateProbeOrigin(float3 probePos, float spacing,
                                      constant DDGIVolumeData& volume,
                                      texture3d<float, access::sample> sdf0,
                                      texture3d<float, access::sample> sdf1,
                                      texture3d<float, access::sample> sdf2) {
    float voxel = volume.SdfVoxelSizes[0].x;
    float skin = max(voxel * 1.5f, spacing * 0.05f);
    const float maxPush = spacing * 0.45f;
    float d0 = sampleBestSDF_elseIf(probePos, sdf0, sdf1, sdf2,
                                    volume.SdfOrigins[0], volume.SdfOrigins[1], volume.SdfOrigins[2],
                                    volume.SdfExtents[0], volume.SdfExtents[1], volume.SdfExtents[2],
                                    volume.SdfCascadeCount);
    if (d0 >= 1e9f) return probePos;  // 原点越界（哨兵）——不重定位，就地 march
    if (d0 > skin) return probePos;
    float3 best = probePos;
    float bestD = d0;
    const float3 dirs[14] = {
        float3( 1.0f, 0.0f, 0.0f), float3(-1.0f, 0.0f, 0.0f),
        float3( 0.0f, 1.0f, 0.0f), float3( 0.0f,-1.0f, 0.0f),
        float3( 0.0f, 0.0f, 1.0f), float3( 0.0f, 0.0f,-1.0f),
        normalize(float3( 1.0f, 1.0f, 1.0f)), normalize(float3(-1.0f, 1.0f, 1.0f)),
        normalize(float3( 1.0f,-1.0f, 1.0f)), normalize(float3(-1.0f,-1.0f, 1.0f)),
        normalize(float3( 1.0f, 1.0f,-1.0f)), normalize(float3(-1.0f, 1.0f,-1.0f)),
        normalize(float3( 1.0f,-1.0f,-1.0f)), normalize(float3(-1.0f,-1.0f,-1.0f))};
    const float stepLen = max(voxel, spacing * 0.1f);
    for (uint round = 0u; round < 3u; ++round) {
        float3 roundBest = best;
        float roundBestD = bestD;
        for (uint k = 0u; k < 14u; ++k) {
            float3 p = best;
            for (uint st = 0u; st < 6u; ++st) {
                p += dirs[k] * stepLen;
                float3 delta = p - probePos;
                if (dot(delta, delta) > maxPush * maxPush) break;
                float d = sampleBestSDF_elseIf(p, sdf0, sdf1, sdf2,
                                               volume.SdfOrigins[0], volume.SdfOrigins[1], volume.SdfOrigins[2],
                                               volume.SdfExtents[0], volume.SdfExtents[1], volume.SdfExtents[2],
                                               volume.SdfCascadeCount);
                // 2026-09-04 伪影1/GI冻结根因修复：越界哨兵（>=1e9，采样点
                // 在级联覆盖外）恒大于任何真实 SDF 距离——旧逻辑把"最大距离"
                // 当作最开阔候选，探针会被重定位到场外 → 所有射线从场外
                // fallbackStride 空走 → 97% miss（ray-path 直方图实测）。
                // 只接受覆盖内的真实距离作为重定位候选。
                if (d < 1e9f && d > roundBestD) { roundBestD = d; roundBest = p; }
                if (d > skin) break;
            }
        }
        if (roundBestD <= bestD) break;
        best = roundBest;
        bestD = roundBestD;
        if (bestD > skin) break;
    }
    return best;
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
                    // C4: half-texel inset keeps bilinear inside this card's rect
                    // (shelf-packed atlas bleeds from neighbours otherwise).
                    atlasUV = clamp(atlasUV,
                        float2(card.atlas_offset_x, card.atlas_offset_y) + 0.5,
                        float2(card.atlas_offset_x + card.resolution,
                               card.atlas_offset_y + card.resolution) - 0.5);
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
//   1. ddgi_trace_sdf: texture3D-only (64 relaxed steps, 1.25×voxel hit
//      band, probe relocation) → writes hit_distance_buffer
//   2. ddgi_trace_finalize: buffer-only + texture2D → reads hit_distance_buffer,
//      looks up Surface Cache radiance (SC-first hierarchy), writes ray_buffer
//
// Both use (8,8,1) threadgroups, one group per probe.
//
// T4.3: per-mesh detail SDF slots — up to 4 per-mesh R16_UNorm distance
// fields near the probe volume. At a coarse hit inside a slot's world AABB
// the trace refines t against the finer field and derives a gradient normal;
// finalize consumes it instead of the -rayDir fake. Slot count 0 (feature
// off) → zero normals, behavior bit-identical to pre-T4.3.
// ============================================================================

// Mirror of LumenDDGIPass.h DetailSDFSlot table (std140-aligned UBO on
// Vulkan; constant buffer here). Decode contract:
//   signed (params.z=1): d = unorm * 2*max_dim - max_dim
//   legacy unsigned    : d = unorm * max_dim
// max_dim = padded bounds max extent / 1.4 (ContentTools uniform 20%
// padding); bounds_min/max are the WORLD AABB of the mesh SDF grid.
struct DDGIDetailSDFTable {
    float4 header;               // x = active slot count (0..4)
    float4x4 inv_world[4];       // world → mesh-local
    float4 normal_matrix[4][3];  // rows of transpose(inverse(world 3x3))
    float4 bounds_min[4];
    float4 bounds_max[4];
    float4 params[4];            // resolution, max_dim, signed, voxel_size
};

static float ddgiDetailDecode(constant DDGIDetailSDFTable& dt, uint k, float raw) {
    float max_dim = dt.params[k].y;
    if (dt.params[k].z > 0.5f) return raw * 2.0f * max_dim - max_dim;
    return raw * max_dim;
}

// Normalized-coords linear sample; the unorm decode is affine so it commutes
// with linear filtering (parity with the Vulkan 8-tap manual trilinear).
static float ddgiDetailSample(texture3d<float, access::sample> tex,
                              constant float4& bmin, constant float4& bmax,
                              float res, float3 local) {
    float3 uvw = (local - bmin.xyz) / max(bmax.xyz - bmin.xyz, float3(1e-6f));
    if (any(uvw < float3(0.0f)) || any(uvw > float3(1.0f))) return 1.0f;  // far
    constexpr sampler s(coord::normalized, filter::linear, address::clamp_to_edge);
    return tex.sample(s, uvw).r;
}

// Refine the coarse GlobalSDF hit against the finest slot containing it.
// Returns true with refined t in outT and the gradient normal in outN
// (world space, oriented against the incident ray).
static bool ddgiDetailRefine(constant DDGIDetailSDFTable& dt,
                             texture3d<float, access::sample> tex0,
                             texture3d<float, access::sample> tex1,
                             texture3d<float, access::sample> tex2,
                             texture3d<float, access::sample> tex3,
                             float3 probePos, float3 rayDir,
                             thread float& t, thread float3& outN) {
    outN = float3(0.0f);
    const uint count = uint(dt.header.x);
    if (count == 0u) return false;

    const float3 hitPos = probePos + rayDir * t;

    // Slot search: containing world AABB, smallest |d| at the hit (wall
    // seams put the point in several grids; the tightest match wins).
    int best = -1;
    float bestAbs = 3.4e38f;
    for (uint k = 0u; k < count; ++k) {
        if (any(hitPos < dt.bounds_min[k].xyz) || any(hitPos > dt.bounds_max[k].xyz)) continue;
        float3 local = (dt.inv_world[k] * float4(hitPos, 1.0f)).xyz;
        float d;
        if (k == 0u)      d = ddgiDetailDecode(dt, k, ddgiDetailSample(tex0, dt.bounds_min[k], dt.bounds_max[k], dt.params[k].x, local));
        else if (k == 1u) d = ddgiDetailDecode(dt, k, ddgiDetailSample(tex1, dt.bounds_min[k], dt.bounds_max[k], dt.params[k].x, local));
        else if (k == 2u) d = ddgiDetailDecode(dt, k, ddgiDetailSample(tex2, dt.bounds_min[k], dt.bounds_max[k], dt.params[k].x, local));
        else              d = ddgiDetailDecode(dt, k, ddgiDetailSample(tex3, dt.bounds_min[k], dt.bounds_max[k], dt.params[k].x, local));
        if (fabs(d) < bestAbs) { bestAbs = fabs(d); best = int(k); }
    }
    if (best < 0) return false;
    const uint bk = uint(best);
    texture3d<float, access::sample> tex =
        (bk == 0u) ? tex0 : (bk == 1u) ? tex1 : (bk == 2u) ? tex2 : tex3;
    const float localVoxel = max(dt.params[bk].w, 1e-5f);
    const bool signedField = dt.params[bk].z > 0.5f;

    // Field value in t-parameter space; local(t) = inv_world * (p + r*t).
    #define DDGI_DETAIL_F(ti_) \
        ddgiDetailDecode(dt, bk, ddgiDetailSample(tex, dt.bounds_min[bk], dt.bounds_max[bk], \
            dt.params[bk].x, (dt.inv_world[bk] * float4(probePos + rayDir * (ti_), 1.0f)).xyz))

    float fBest = bestAbs;
    float tBest = t;
    if (signedField) {
        // Signed field: scan a ±5*localVoxel window for the zero crossing,
        // then bisect (the coarse hit sits inside the inflated hit band).
        float tLo = t, tHi = t;
        bool haveLo = false, haveHi = false;
        for (int i = -5; i <= 5; ++i) {
            float ti = t + float(i) * localVoxel;
            float fi = DDGI_DETAIL_F(ti);
            if (fi < 0.0f && (!haveLo || ti > tLo)) { tLo = ti; haveLo = true; }
            if (fi >= 0.0f && (!haveHi || ti < tHi)) { tHi = ti; haveHi = true; }
            if (fabs(fi) < fBest) { fBest = fabs(fi); tBest = ti; }
        }
        if (haveLo && haveHi) {
            for (int it = 0; it < 5; ++it) {
                float tm = 0.5f * (tLo + tHi);
                if (DDGI_DETAIL_F(tm) < 0.0f) tLo = tm; else tHi = tm;
            }
            tBest = 0.5f * (tLo + tHi);
        }
    } else {
        // Legacy unsigned field: descend on |d| — the surface is the local
        // minimum of the distance band.
        for (int i = 1; i <= 6; ++i) {
            float ti = t + float(i) * 0.5f * localVoxel;
            float fi = DDGI_DETAIL_F(ti);
            if (fabs(fi) < fBest) { fBest = fabs(fi); tBest = ti; } else break;
        }
    }
    #undef DDGI_DETAIL_F

    // Gradient normal at the refined point (central difference, 6 taps).
    const float e = 0.75f * localVoxel;
    float3 lp = (dt.inv_world[bk] * float4(probePos + rayDir * tBest, 1.0f)).xyz;
    float dx = ddgiDetailDecode(dt, bk, ddgiDetailSample(tex, dt.bounds_min[bk], dt.bounds_max[bk], dt.params[bk].x, lp + float3(e, 0, 0)))
             - ddgiDetailDecode(dt, bk, ddgiDetailSample(tex, dt.bounds_min[bk], dt.bounds_max[bk], dt.params[bk].x, lp - float3(e, 0, 0)));
    float dy = ddgiDetailDecode(dt, bk, ddgiDetailSample(tex, dt.bounds_min[bk], dt.bounds_max[bk], dt.params[bk].x, lp + float3(0, e, 0)))
             - ddgiDetailDecode(dt, bk, ddgiDetailSample(tex, dt.bounds_min[bk], dt.bounds_max[bk], dt.params[bk].x, lp - float3(0, e, 0)));
    float dz = ddgiDetailDecode(dt, bk, ddgiDetailSample(tex, dt.bounds_min[bk], dt.bounds_max[bk], dt.params[bk].x, lp + float3(0, 0, e)))
             - ddgiDetailDecode(dt, bk, ddgiDetailSample(tex, dt.bounds_min[bk], dt.bounds_max[bk], dt.params[bk].x, lp - float3(0, 0, e)));
    float3 gLocal = float3(dx, dy, dz);
    if (dot(gLocal, gLocal) < 1e-14f) return false;

    float3 n = normalize(
        dt.normal_matrix[bk][0].xyz * gLocal.x +
        dt.normal_matrix[bk][1].xyz * gLocal.y +
        dt.normal_matrix[bk][2].xyz * gLocal.z);
    if (dot(n, rayDir) > 0.0f) n = -n;   // orient against the incident ray
    outN = n;
    t = tBest;
    return true;
}


kernel void ddgi_trace_sdf(
    uint2 tid2d [[thread_position_in_threadgroup]],
    uint2 gid2d [[threadgroup_position_in_grid]],

    texture3d<float, access::sample> sdf_cascade_0 [[texture(0)]],
    texture3d<float, access::sample> sdf_cascade_1 [[texture(1)]],
    texture3d<float, access::sample> sdf_cascade_2 [[texture(2)]],

    // T4.3: per-mesh detail SDF slots (dummy-bound when feature off)
    texture3d<float, access::sample> detail_sdf_0 [[texture(3)]],
    texture3d<float, access::sample> detail_sdf_1 [[texture(4)]],
    texture3d<float, access::sample> detail_sdf_2 [[texture(5)]],
    texture3d<float, access::sample> detail_sdf_3 [[texture(6)]],

    constant DDGIVolumeData& volume [[buffer(1)]],
    device float* hit_distance_buffer [[buffer(2)]],
    device const uint* probeUpdateList [[buffer(3)]],
    device float4* hit_normal_buffer [[buffer(4)]],                    // T4.3
    constant DDGIDetailSDFTable& detail_table [[buffer(5)]]            // T4.3
)
{
    uint tid = tid2d.y * 8u + tid2d.x;
    uint group_id = gid2d.x;

    if (group_id >= volume.ProbeUpdateCount) return;
    if (tid >= volume.RaysPerProbe) return;

    uint probeIdx = probeUpdateList[group_id];
    if (probeIdx >= volume.ProbeCountTotal) return;

    uint3 gc = ddgiProbeGridCoord(probeIdx, ddgiGetProbeCounts(volume));
    float3 probePos = ddgiProbeWorldPos(gc, volume.ProbeOrigin.xyz, volume.ProbeSpacing);
    // TEMP diagnosis: record the raw SDF value at the grid origin on ray 0.
    float d0_dbg = sampleBestSDF_elseIf(probePos, sdf_cascade_0, sdf_cascade_1, sdf_cascade_2,
                                        volume.SdfOrigins[0], volume.SdfOrigins[1], volume.SdfOrigins[2],
                                        volume.SdfExtents[0], volume.SdfExtents[1], volume.SdfExtents[2],
                                        volume.SdfCascadeCount);
    // T8.13 sync: relocate buried ray origins (multi-axis deepest-free-point,
    // same policy as DDGITraceSDF.comp::ddgiRelocateProbeOrigin).
    float3 relocatedPos = ddgiRelocateProbeOrigin(probePos, volume.ProbeSpacing, volume,
                                       sdf_cascade_0, sdf_cascade_1, sdf_cascade_2);
    // TEMP diagnosis: also record the relocated origin's SDF value on ray 1.
    float d1_dbg = sampleBestSDF_elseIf(relocatedPos, sdf_cascade_0, sdf_cascade_1, sdf_cascade_2,
                                        volume.SdfOrigins[0], volume.SdfOrigins[1], volume.SdfOrigins[2],
                                        volume.SdfExtents[0], volume.SdfExtents[1], volume.SdfExtents[2],
                                        volume.SdfCascadeCount);
    float3 probePosRel = relocatedPos;
    probePos = probePosRel;
    float3 toLight = (volume.LightDirection.xyz[0]*volume.LightDirection.xyz[0] + volume.LightDirection.xyz[1]*volume.LightDirection.xyz[1] + volume.LightDirection.xyz[2]*volume.LightDirection.xyz[2] > 1e-8 ? normalize(-volume.LightDirection.xyz) : float3(0.0f, -1.0f, 0.0f));

    // T8.13 P2 sync: lane-strided ray loop — 64 lanes serve any RaysPerProbe
    // multiple of 64 (128 = two strided rays per lane).
    for (uint rayId = tid; rayId < volume.RaysPerProbe; rayId += 64u) {
    float3 rayDir = ddgiSampleRayDirection(rayId, volume.RaysPerProbe,
                                           volume.FrameIndex,
                                           uint(volume.RaySampleMode), toLight).xyz;

    // Relaxed sphere tracing (texture3D reads only)
    float t = 0.0f;
    float hitDistance = -1.0f;

    // T8.10b sync: hit band widened for the UNSIGNED shallow merged field
    // (voxel*0.5 let most rays march past surfaces → step exhaustion →
    // miss→sky; 1.25×voxel inflates surfaces by ~half a step-band —
    // invisible at GI frequencies).
    float hitThreshold = volume.SdfVoxelSizes[0].x * 1.25f;
    float minStep = volume.SdfVoxelSizes[0].x * 0.25f;
    uint coarsest = min(2u, volume.SdfCascadeCount - 1u);
    float fallbackStride = volume.SdfExtents[coarsest].x * 0.1f;

    // T8.13 sync: 64 relaxed steps (was 4 — the old march could not cross a
    // full atrium; deep-shadow arcades never reached sunlit geometry). The
    // shared DDGI_MAX_SDF_STEPS=4 stays untouched (other consumers); the
    // Vulkan port defines the same 64 locally in DDGITraceSDF.comp.
    const uint DDGI_SPLIT_MAX_SDF_STEPS = 64u;
    float prevD = 3.4e38f;   // B13: distance trend across steps
    for (uint step = 0; step < DDGI_SPLIT_MAX_SDF_STEPS; ++step) {
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

        // Relaxed advancement: overstep when far, conservative when close.
        // T8.13 sync: far-field overstep 1.2 → 1.5 (matches Vulkan).
        // B13: no ω overstep while the field shrinks (thin-wall jump guard).
        float advance;
        if (d > hitThreshold * 4.0f && d >= prevD) {
            advance = d * 1.5f;
        } else {
            advance = max(d, minStep);
        }
        prevD = d;
        t += advance;
        if (t > volume.RayMaxDistance) break;
    }

    // Indexed by update-slot (group) — finalize re-maps to absolute probe index.
    uint globalRayIdx = group_id * volume.RaysPerProbe + rayId;
    if (rayId == 0u && group_id < volume.ProbeUpdateCount) {
        hit_distance_buffer[group_id * volume.RaysPerProbe + 0u] =
            (d0_dbg >= 1e9f) ? 5555.0f : d0_dbg;               // ray0 = raw origin d
    } else if (rayId == 1u && group_id < volume.ProbeUpdateCount) {
        hit_distance_buffer[group_id * volume.RaysPerProbe + 1u] =
            (d1_dbg >= 1e9f) ? 5555.0f : d1_dbg;               // ray1 = relocated d
    } else {
        hit_distance_buffer[globalRayIdx] = hitDistance;
    }

    // T4.3: per-mesh detail refinement + gradient normal. Slot count 0
    // (feature off / no nearby per-mesh SDF) → zero normal, coarse t kept.
    float3 hitN = float3(0.0f);
    float hnValid = 0.0f;
    if (hitDistance >= 0.0f) {
        float tRefined = hitDistance;
        if (ddgiDetailRefine(detail_table, detail_sdf_0, detail_sdf_1,
                             detail_sdf_2, detail_sdf_3,
                             probePos, rayDir, tRefined, hitN)) {
            hitDistance = tRefined;
            hnValid = 1.0f;
        }
    }
    hit_normal_buffer[globalRayIdx] = float4(hitN, hnValid);
    } // T8.13 P2 stride loop
}

// ============================================================================
// Canonical radiance chain helpers (T3.1) — ported from DDGITraceFinalize.comp.
// T8.13 sync: SC-first hierarchy with 0.70 self-circulating feedback.
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
// (scShDot4 kept for reference; the finalize path now evaluates the full
// 9-coefficient basis via scShDot9 — see T8.13 sync note above.)

// T8.13 sync: full L0-L2 evaluation with the canonical basis — the old
// 4-coefficient evaluation left the directional colour detail the SC path
// projects into bands 4-8 unread (matches DDGITraceFinalize.comp shDot9).
static float3 scShDot9(thread const float3* c, float3 d) {
    constexpr float _C0   = 0.282095f;
    constexpr float _C1   = 0.488603f;
    constexpr float _C2_0 = 1.092548f;
    constexpr float _C2_1 = 0.315392f;
    constexpr float _C2_2 = 0.546274f;
    return c[0] * _C0
         + c[1] * (-_C1 * d.y)
         + c[2] * ( _C1 * d.z)
         + c[3] * (-_C1 * d.x)
         + c[4] * ( _C2_0 * d.y * d.x)
         + c[5] * (-_C2_0 * d.y * d.z)
         + c[6] * ( _C2_1 * (3.0f * d.z * d.z - 1.0f))
         + c[7] * (-_C2_0 * d.x * d.z)
         + c[8] * ( _C2_2 * (d.x * d.x - d.y * d.y));
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
        float3 sh[9];
        for (uint i = 0u; i < 9u; ++i) sh[i] = irradiance_history[base + i].xyz;
        result += scShDot9(sh, N) * tet.bw[p];
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
    texture2d<float, access::sample> depth_atlas [[texture(6)]],   // T3.4 (D3)

    constant DDGIVolumeData& volume [[buffer(1)]],
    device const float* hit_distance_buffer [[buffer(2)]],
    device const uint* probeUpdateList [[buffer(3)]],
    device DDGIRayData* ray_buffer [[buffer(4)]],
    device const SurfaceCacheCardLookup* card_lookups [[buffer(5)]],
    device const SurfaceCacheCard* card_data [[buffer(6)]],
    device const uint* sc_grid_offsets [[buffer(7)]],  // T1.3: header + prefix sums
    device const uint* sc_grid_cards   [[buffer(8)]],  // T1.3: card indices
    constant float4x4& view_projection [[buffer(9)]],  // T3.1: screen projection
    device const float4* irradiance_history [[buffer(10)]],  // T3.1: multi-bounce
    device const float4* hit_normal_buffer [[buffer(11)]]    // T4.3: refined normals
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

    uint3 gc = ddgiProbeGridCoord(probeIdx, ddgiGetProbeCounts(volume));
    float3 probePos = ddgiProbeWorldPos(gc, volume.ProbeOrigin.xyz, volume.ProbeSpacing);
    float3 toLight = (volume.LightDirection.xyz[0]*volume.LightDirection.xyz[0] + volume.LightDirection.xyz[1]*volume.LightDirection.xyz[1] + volume.LightDirection.xyz[2]*volume.LightDirection.xyz[2] > 1e-8 ? normalize(-volume.LightDirection.xyz) : float3(0.0f, -1.0f, 0.0f));

    // T8.13 P2 sync: lane-strided ray loop (mirrors ddgi_trace_sdf).
    for (uint rayId = tid; rayId < volume.RaysPerProbe; rayId += 64u) {
    uint globalRayIdx = group_id * volume.RaysPerProbe + rayId;
    float hitDistance = hit_distance_buffer[globalRayIdx];

    float3 rayDir = ddgiSampleRayDirection(rayId, volume.RaysPerProbe,
                                           volume.FrameIndex,
                                           uint(volume.RaySampleMode), toLight).xyz;

    DDGIRayData result;
#ifdef DDGI_PATH_DEBUG
    uint dbgPath = 0u;  // 1=SC atlas 2=prev-screen 3=analytic (miss stays 0)
#endif

    if (hitDistance >= 0.0f) {
        result.radiance_and_dist.w = hitDistance;
        // Recompute hit position (same recipe as DDGITraceFinalize.comp:
        // probePos is the UN-relocated grid position here).
        float3 hitPos = probePos + rayDir * hitDistance;

        // ---- 2026-09-02：SC-FIRST 曾因 SC atlas 覆盖不足被改为 prev-first。
        //
        // 2026-09-03 与 Vulkan DDGITraceFinalize.comp 逐行核对后确认 Vulkan
        // 的真实层级是 **SC-first**（0c2e34d9 注释"Vulkan 以 prev 为主"有
        // 误）。当年 SC-first 失败的根因是 SC atlas 覆盖不足（T1.4 页预算
        // + seed 未生效），现已由 seed+轮转修复，故回迁 Vulkan 的真实顺序：
        // 1. SC lighting atlas（能量门，首个覆盖卡即停；自身反馈已含多
        //    弹射，不再叠加 albedo×L_i_prev 项）。
        // 2. Prev-frame screen color + material bounce + self-circulating
        //    （isfinite 门防 NaN 静默失效）。
        // 3. Analytic direct + sky + multi-bounce 兜底。
        // T4.3: real gradient normal when the detail-SDF refinement ran;
        // -rayDir fake otherwise (bit-identical default behavior).
        float3 N = -rayDir;
        {
            const float4 hn = hit_normal_buffer[globalRayIdx];
            if (hn.w > 0.5f && dot(hn.xyz, hn.xyz) > 0.25f) {
                N = normalize(hn.xyz);
            }
        }
        float3 albedo = scSampleHitAlbedo(hitPos, view_projection, volume.Albedo.xyz, gbuffer_albedo);
        float3 L_i_prev = scSamplePrevProbeGrid(hitPos, N, volume, irradiance_history);

        float3 radiance;
        bool radianceFound = false;
        {
            // 1) SC-first（与 DDGITraceFinalize.comp 相同：首个覆盖卡采完
            //    即 break，黑卡/未写入卡能量门失败则落下一层）。
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
                        // C4: half-texel inset keeps bilinear inside this card's rect
                        // (shelf-packed atlas bleeds from neighbours otherwise).
                        atlasUV = clamp(atlasUV,
                            float2(card.atlas_offset_x, card.atlas_offset_y) + 0.5,
                            float2(card.atlas_offset_x + card.resolution,
                                   card.atlas_offset_y + card.resolution) - 0.5);
                        float2 normUV = atlasUV / atlasSize;
                        sampler samp(coord::normalized, filter::linear, address::clamp_to_edge);
                        // T3.4 (D3): plane-consistency test — the sampled texel
                        // must belong to the SAME surface the ray hit. Compare
                        // the hit point's axis depth against the card's stored
                        // texel depth; a card surface in front of the hit means
                        // this texel describes other geometry (occluder) — its
                        // radiance would poison the probe's incident estimate.
                        float texelDepth = depth_atlas.sample(samp, normUV).r;
                        float3 dl = hitPos - card.center.xyz;
                        uint dAxis = card.axis_direction & 0xFFu;
                        uint dDir = (card.axis_direction >> 8u) & 0xFFu;
                        float hitAxisDepth = (dAxis == 0u ? dl.x : dAxis == 1u ? dl.y : dl.z) * (dDir != 0u ? 1.0f : -1.0f);
                        float depthTol = 0.35f * max(card.extent.x, max(card.extent.y, card.extent.z));
                        bool planeOk = (hitAxisDepth <= texelDepth + depthTol);
                        float3 scRadiance = lighting_atlas.sample(samp, normUV).xyz;
                        if (planeOk && dot(scRadiance, scRadiance) > 0.0001f) {
                            radiance = scRadiance;
                            radianceFound = true;
#ifdef DDGI_PATH_DEBUG
                            dbgPath = 1u;
#endif
                        }
                        break;
                    }
                }
            }
            if (!radianceFound) {
                // 2) Prev-frame screen color（off-screen 或 SC 黑时）。
                // 门用 isfinite 收紧：屏幕颜色里若有 NaN（specular 除零等），
                // NaN 参与任何比较恒为 false——能量门会静默失效。
                float3 prevRadiance = scSamplePrevColor(hitPos, view_projection, prev_frame_color);
                if (all(isfinite(prevRadiance)) && dot(prevRadiance, prevRadiance) > 0.0001f) {
                    float sourceLum = dot(prevRadiance, float3(0.2126f, 0.7152f, 0.0722f));
                    float albedoMax = max(max(albedo.r, albedo.g), albedo.b);
                    float albedoMin = min(min(albedo.r, albedo.g), albedo.b);
                    float saturation = (albedoMax - albedoMin) / max(albedoMax, 1e-3f);
                    float materialWeight = mix(0.15f, 0.85f, saturation);
                    float3 materialBounce = albedo * max(sourceLum, 0.12f) * materialWeight;
                    // B6: prevRadiance already carries last frame's indirect
                    // (direct + bounce baked into the screen color) — adding
                    // the probe feedback again double-counted indirect light.
                    radiance = prevRadiance + materialBounce;
                    radianceFound = true;
#ifdef DDGI_PATH_DEBUG
                    dbgPath = 2u;
#endif
                }
            }
            if (!radianceFound) {
                // 3) Analytic: self-computed direct + sky + multi-bounce
                float3 L = (volume.LightDirection.xyz[0]*volume.LightDirection.xyz[0] + volume.LightDirection.xyz[1]*volume.LightDirection.xyz[1] + volume.LightDirection.xyz[2]*volume.LightDirection.xyz[2] > 1e-8 ? normalize(-volume.LightDirection.xyz) : float3(0.0f, -1.0f, 0.0f));
                float NdotL = max(0.0f, dot(N, L));
                float3 E_direct = volume.LightColor.xyz * volume.LightColor.w * NdotL;
                float NdotUp = max(0.0f, N.y);
                float3 E_sky = volume.SkyColor.xyz * (0.5f + 0.5f * NdotUp);
                constexpr float PI_VAL = 3.14159265358979f;
                radiance = (albedo / PI_VAL) * (E_direct + E_sky + PI_VAL * L_i_prev * 0.70f);
#ifdef DDGI_PATH_DEBUG
                dbgPath = 3u;
#endif
            }
        }
        result.radiance_and_dist.xyz = radiance;
    } else {
        result.radiance_and_dist.w = -1.0f;

        // Miss fallback: nearby SC cards centered on the PROBE (not hit).
        // T8.13 sync: only accept card light that actually has energy — in
        // shadowed arcades the atlas is still converging and a black card
        // must not override the sky term; try the next covering card instead.
        float3 fallbackRadiance = DDGI_SKY_COLOR;
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
                    // C4: half-texel inset keeps bilinear inside this card's rect
                    // (shelf-packed atlas bleeds from neighbours otherwise).
                    atlasUV = clamp(atlasUV,
                        float2(card.atlas_offset_x, card.atlas_offset_y) + 0.5,
                        float2(card.atlas_offset_x + card.resolution,
                               card.atlas_offset_y + card.resolution) - 0.5);
                    float2 normUV = atlasUV / atlasSize;
                    sampler samp(coord::normalized, filter::linear, address::clamp_to_edge);
                    float3 missRadiance = lighting_atlas.sample(samp, normUV).xyz;
                    if (dot(missRadiance, missRadiance) > 0.0001f) {
                        fallbackRadiance = missRadiance;
                        missFound = true;
                        break;
                    }
                }
            }
        }
        (void)missFound;
        result.radiance_and_dist.xyz = fallbackRadiance;
    }

    // Write at ABSOLUTE probe index (remap from update-slot is intentional).
#ifdef DDGI_PATH_DEBUG
    // 路径标签停到 d=40+k·0.001——UpdateIrradiance 的距离衰减
    // 1-smoothstep(40,50,d) 在 d>=40 恒为 1，替换真实命中距离不影响
    // 辐射权重，同时让标签在 CPU 端可解码（Vulkan comp 同款方案）。
    if (result.radiance_and_dist.w >= 0.0f)
        result.radiance_and_dist.w = 40.0f + float(dbgPath) * 0.001f;
#endif
    ray_buffer[probeIdx * volume.RaysPerProbe + rayId] = result;
    } // T8.13 P2 stride loop
}
