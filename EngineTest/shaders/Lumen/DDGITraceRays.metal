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

// 2026-09-03：对齐 Vulkan DDGITraceFinalize.comp 的完整辐射层级
// （SC-first 带能量门 → prev-screen+材质弹射 → 解析项；miss 以探针为中心
// 搜卡带门 + 暗灰天光）。旧版是无门无解析的纯 SC 采样——SC 未点亮区域
// 采成黑、miss 探针填纯蓝，GI 区因此死灰且不随时间变化。
// 绑定与 LumenDDGIPass.cpp 的 Metal finalize 分支一致：
//   texture(3)=SC lighting_atlas, texture(4)=prev_frame_color,
//   texture(5)=gbuffer_albedo, buffer(9)=ViewProjection,
//   buffer(10)=irradiance_history（多反弹反馈）。
kernel void ddgi_trace_finalize(
    uint2 tid2d [[thread_position_in_threadgroup]],
    uint2 gid2d [[threadgroup_position_in_grid]],

    texture2d<float, access::sample> lighting_atlas [[texture(3)]],

    constant DDGIVolumeData& volume [[buffer(1)]],
    device const float* hit_distance_buffer [[buffer(2)]],
    device const uint* probeUpdateList [[buffer(3)]],
    device DDGIRayData* ray_buffer [[buffer(4)]],
    device const SurfaceCacheCardLookup* card_lookups [[buffer(5)]],
    device const SurfaceCacheCard* card_data [[buffer(6)]],
    device const uint* sc_grid_offsets [[buffer(7)]],  // T1.3: header + prefix sums
    device const uint* sc_grid_cards   [[buffer(8)]],  // T1.3: card indices
    texture2d<float, access::sample> prev_frame_color [[texture(4)]],
    texture2d<float, access::sample> gbuffer_albedo [[texture(5)]],
    constant float4x4& view_projection [[buffer(9)]],
    device const float4* irradiance_history [[buffer(10)]]
)
{
    uint tid = tid2d.y * 8u + tid2d.x;
    uint group_id = gid2d.x;

    if (group_id >= volume.ProbeUpdateCount) return;
    if (tid >= volume.RaysPerProbe) return;

    uint probeIdx = probeUpdateList[group_id];
    if (probeIdx >= volume.ProbeCountTotal) return;
    uint globalRayIdx = group_id * volume.RaysPerProbe + tid;
    float hitDistance = hit_distance_buffer[globalRayIdx];

    DDGIRayData result;

    if (hitDistance >= 0.0f) {
        result.radiance_and_dist.w = hitDistance;

        uint3 gc = ddgiProbeGridCoord(probeIdx, ddgiGetProbeCounts(volume));
        float3 probePos = ddgiProbeWorldPos(gc, volume.ProbeOrigin.xyz, volume.ProbeSpacing);
        float3 rayDir = ddgiRayDirection(tid, volume.RaysPerProbe, volume.FrameIndex);
        float3 hitPos = probePos + rayDir * hitDistance;

        float3 N = -rayDir;

        // ---- hit albedo：屏幕空间 GBuffer 采样，离屏回退 volume.Albedo ----
        float3 albedo = volume.Albedo.xyz;
        {
            float4 clip = view_projection * float4(hitPos, 1.0);
            if (clip.w > 0.0) {
                float3 ndc = clip.xyz / clip.w;
                if (ndc.x >= -1.0 && ndc.x <= 1.0 && ndc.y >= -1.0 && ndc.y <= 1.0 &&
                    ndc.z >= 0.0 && ndc.z <= 1.0) {
                    float2 uv = float2(ndc.x * 0.5 + 0.5, 1.0 - (ndc.y * 0.5 + 0.5));
                    uint2 dims = uint2(gbuffer_albedo.get_width(), gbuffer_albedo.get_height());
                    uint2 texel = clamp(uint2(uv * float2(dims)), uint2(0), dims - 1u);
                    albedo = gbuffer_albedo.read(texel).rgb;
                }
            }
        }

        // ---- prev-frame probe irradiance at hit（多反弹自循环项）----
        float3 L_i_prev = float3(0.0);
        {
            float3 origin = volume.ProbeOrigin.xyz;
            float spacing = volume.ProbeSpacing;
            uint3 counts = ddgiGetProbeCounts(volume);
            float3 gp = (hitPos - origin) / spacing;
            float3 gridMax = float3(float(counts.x - 1u), float(counts.y - 1u), float(counts.z - 1u));
            bool inside = all(gp >= float3(0.0)) && all(gp <= gridMax);
            if (inside) {
                // tetrahedral 4-probe（与 DDGIUpdateIrradiance 投影一致的 9 系数）
                uint3 b0 = min(uint3(floor(gp)), counts - 1u);
                uint3 b1 = min(b0 + 1u, counts - 1u);
                uint gx = counts.x;
                uint gxy = counts.x * counts.y;
                uint p8[8];
                p8[0] = b0.x + b0.y * gx + b0.z * gxy;
                p8[1] = b1.x + b0.y * gx + b0.z * gxy;
                p8[2] = b0.x + b1.y * gx + b0.z * gxy;
                p8[3] = b1.x + b1.y * gx + b0.z * gxy;
                p8[4] = b0.x + b0.y * gx + b1.z * gxy;
                p8[5] = b1.x + b0.y * gx + b1.z * gxy;
                p8[6] = b0.x + b1.y * gx + b1.z * gxy;
                p8[7] = b1.x + b1.y * gx + b1.z * gxy;
                float fx = fract(gp.x), fy = fract(gp.y), fz = fract(gp.z);
                uint pi[4]; float bw[4];
                if (fx >= fy && fy >= fz)      { pi[0]=p8[0];pi[1]=p8[1];pi[2]=p8[3];pi[3]=p8[7]; bw[0]=1.0-fx;bw[1]=fx-fy;bw[2]=fy-fz;bw[3]=fz; }
                else if (fx >= fz && fz >= fy) { pi[0]=p8[0];pi[1]=p8[1];pi[2]=p8[5];pi[3]=p8[7]; bw[0]=1.0-fx;bw[1]=fx-fz;bw[2]=fz-fy;bw[3]=fy; }
                else if (fy >= fx && fx >= fz) { pi[0]=p8[0];pi[1]=p8[2];pi[2]=p8[3];pi[3]=p8[7]; bw[0]=1.0-fy;bw[1]=fy-fx;bw[2]=fx-fz;bw[3]=fz; }
                else if (fy >= fz && fz >= fx) { pi[0]=p8[0];pi[1]=p8[2];pi[2]=p8[6];pi[3]=p8[7]; bw[0]=1.0-fy;bw[1]=fy-fz;bw[2]=fz-fx;bw[3]=fx; }
                else if (fz >= fx && fx >= fy) { pi[0]=p8[0];pi[1]=p8[4];pi[2]=p8[5];pi[3]=p8[7]; bw[0]=1.0-fz;bw[1]=fz-fx;bw[2]=fx-fy;bw[3]=fy; }
                else                           { pi[0]=p8[0];pi[1]=p8[4];pi[2]=p8[6];pi[3]=p8[7]; bw[0]=1.0-fz;bw[1]=fz-fy;bw[2]=fy-fx;bw[3]=fx; }
                float totalWeight = 0.0;
                for (uint k = 0u; k < 4u; ++k) {
                    bw[k] = max(bw[k], 0.0);
                    if (bw[k] < 0.001) continue;
                    uint base = pi[k] * 9u;
                    // 9 系数 SH 评估（L0-L2，与 UpdateIrradiance 投影一致）
                    float3 c0 = irradiance_history[base + 0u].xyz;
                    float3 c1 = irradiance_history[base + 1u].xyz;
                    float3 c2 = irradiance_history[base + 2u].xyz;
                    float3 c3 = irradiance_history[base + 3u].xyz;
                    float3 c4 = irradiance_history[base + 4u].xyz;
                    float3 c5 = irradiance_history[base + 5u].xyz;
                    float3 c6 = irradiance_history[base + 6u].xyz;
                    float3 c7 = irradiance_history[base + 7u].xyz;
                    float3 c8 = irradiance_history[base + 8u].xyz;
                    float3 irr = c0 * DDGI_SH_C0
                               + c1 * (-DDGI_SH_C1 * N.y)
                               + c2 * ( DDGI_SH_C1 * N.z)
                               + c3 * (-DDGI_SH_C1 * N.x)
                               + c4 * ( DDGI_SH_C2_0 * N.y * N.x)
                               + c5 * (-DDGI_SH_C2_0 * N.y * N.z)
                               + c6 * ( DDGI_SH_C2_1 * (3.0 * N.z * N.z - 1.0))
                               + c7 * (-DDGI_SH_C2_0 * N.x * N.z)
                               + c8 * ( DDGI_SH_C2_2 * (N.x * N.x - N.y * N.y));
                    L_i_prev += irr * bw[k];
                    totalWeight += bw[k];
                }
                if (totalWeight > 0.0) L_i_prev /= totalWeight;
            }
        }

        float3 radiance = DDGI_SKY_COLOR;
        bool radianceFound = false;

        // ---- 1) SC-first：能量门 + 首个覆盖卡即停（与 Vulkan 一致）----
        {
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
                        float2 normUV = atlasUV / atlasSize;
                        sampler samp(coord::normalized, filter::linear, address::clamp_to_edge);
                        float3 scRadiance = lighting_atlas.sample(samp, normUV).xyz;
                        if (dot(scRadiance, scRadiance) > 0.0001f) {
                            radiance = scRadiance;
                            radianceFound = true;
                        }
                        break;
                    }
                }
            }

            // ---- 2) prev-screen + material bounce + self-circulating ----
            if (!radianceFound) {
                float3 prevRadiance = float3(0.0);
                {
                    float4 clip = view_projection * float4(hitPos, 1.0);
                    if (clip.w > 0.0) {
                        float3 ndc = clip.xyz / clip.w;
                        if (ndc.x >= -1.0 && ndc.x <= 1.0 && ndc.y >= -1.0 && ndc.y <= 1.0 &&
                            ndc.z >= 0.0 && ndc.z <= 1.0) {
                            float2 uv = float2(ndc.x * 0.5 + 0.5, 1.0 - (ndc.y * 0.5 + 0.5));
                            uint2 dims = uint2(prev_frame_color.get_width(), prev_frame_color.get_height());
                            uint2 texel = clamp(uint2(uv * float2(dims)), uint2(0), dims - 1u);
                            prevRadiance = prev_frame_color.read(texel).rgb;
                        }
                    }
                }
                if (all(isfinite(prevRadiance)) && dot(prevRadiance, prevRadiance) > 0.0001f) {
                    float sourceLum = dot(prevRadiance, float3(0.2126f, 0.7152f, 0.0722f));
                    float albedoMax = max(max(albedo.r, albedo.g), albedo.b);
                    float albedoMin = min(min(albedo.r, albedo.g), albedo.b);
                    float saturation = (albedoMax - albedoMin) / max(albedoMax, 1e-3f);
                    float materialWeight = mix(0.15f, 0.85f, saturation);
                    float3 materialBounce = albedo * max(sourceLum, 0.12f) * materialWeight;
                    radiance = prevRadiance + materialBounce + albedo * L_i_prev * 0.70f;
                    radianceFound = true;
                }
            }

            // ---- 3) analytic: direct + sky + multi-bounce 兜底 ----
            if (!radianceFound) {
                float3 L = normalize(-volume.LightDirection.xyz);
                float NdotL = max(0.0f, dot(N, L));
                float3 E_direct = volume.LightColor.xyz * volume.LightColor.w * NdotL;
                float NdotUp = max(0.0f, N.y);
                float3 E_sky = volume.SkyColor.xyz * (0.5f + 0.5f * NdotUp);
                constexpr float PI_VAL = 3.14159265358979f;
                radiance = (albedo / PI_VAL) * (E_direct + E_sky + PI_VAL * L_i_prev * 0.70f);
            }
        }
        result.radiance_and_dist.xyz = radiance;
    } else {
        result.radiance_and_dist.w = -1.0f;

        // ---- Miss：以探针为中心搜卡（带能量门），兜底暗灰天光 ----
        float3 fallbackRadiance = DDGI_SKY_COLOR;
        uint3 gc = ddgiProbeGridCoord(probeIdx, ddgiGetProbeCounts(volume));
        float3 probePos = ddgiProbeWorldPos(gc, volume.ProbeOrigin.xyz, volume.ProbeSpacing);
        float atlasSize = float(volume.SCAtlasSize);

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
                    float3 missRadiance = lighting_atlas.sample(samp, normUV).xyz;
                    if (dot(missRadiance, missRadiance) > 0.0001f) {
                        fallbackRadiance = missRadiance;
                        break;
                    }
                }
            }
        }
        result.radiance_and_dist.xyz = fallbackRadiance;
    }

    ray_buffer[probeIdx * volume.RaysPerProbe + tid] = result;
}
