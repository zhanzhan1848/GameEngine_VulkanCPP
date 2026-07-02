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

kernel void ddgi_trace_finalize(
    uint2 tid2d [[thread_position_in_threadgroup]],
    uint2 gid2d [[threadgroup_position_in_grid]],

    texture2d<float, access::sample> lighting_atlas [[texture(3)]],

    constant DDGIVolumeData& volume [[buffer(1)]],
    device const float* hit_distance_buffer [[buffer(2)]],
    device const uint* probeUpdateList [[buffer(3)]],
    device DDGIRayData* ray_buffer [[buffer(4)]],
    device const SurfaceCacheCardLookup* card_lookups [[buffer(5)]],
    device const SurfaceCacheCard* card_data [[buffer(6)]]
)
{
    uint tid = tid2d.y * 8u + tid2d.x;
    uint group_id = gid2d.x;

    // Phase 0: Cooperative card lookup loading (buffer reads only, no texture3D)
    constexpr uint kMaxLookups = 128;
    threadgroup float4 tg_aabb_min[kMaxLookups];
    threadgroup float4 tg_aabb_max[kMaxLookups];
    threadgroup uint2 tg_card_info[kMaxLookups];

    uint lookupCount = min(volume.SCLookupCount, kMaxLookups);
    for (uint i = tid; i < lookupCount; i += 64u) {
        tg_aabb_min[i] = card_lookups[i].aabb_min;
        tg_aabb_max[i] = card_lookups[i].aabb_max;
        tg_card_info[i] = uint2(card_lookups[i].card_start, card_lookups[i].card_count);
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

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

        float3 radiance = DDGI_SKY_COLOR;
        float atlasSize = float(volume.SCAtlasSize);

        // Apple Silicon: cap per-ray device reads to ~16 to stay within ~32 limit
        uint deviceReads = 0u;
        constexpr uint kMaxDeviceReads = 16u;
        for (uint li = 0; li < lookupCount && deviceReads < kMaxDeviceReads; ++li) {
            if (hitPos.x < tg_aabb_min[li].x || hitPos.x > tg_aabb_max[li].x ||
                hitPos.y < tg_aabb_min[li].y || hitPos.y > tg_aabb_max[li].y ||
                hitPos.z < tg_aabb_min[li].z || hitPos.z > tg_aabb_max[li].z)
                continue;

            bool found = false;
            for (uint ci = 0; ci < min(tg_card_info[li].y, 6u) && deviceReads < kMaxDeviceReads; ++ci) {
                uint cardIdx = tg_card_info[li].x + ci;
                if (cardIdx >= 4096u) continue;
                device const SurfaceCacheCard& card = card_data[cardIdx];
                ++deviceReads;
                float2 atlasUV;
                if (worldToCardUVDevice(hitPos, card, atlasUV)) {
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
        result.radiance_and_dist.w = -1.0f;

        // Miss fallback: search for nearby SC cards centered on probe position
        float3 fallbackRadiance = DDGI_SKY_COLOR;
        uint3 gc = ddgiProbeGridCoord(probeIdx, ddgiGetProbeCounts(volume));
        float3 probePos = ddgiProbeWorldPos(gc, volume.ProbeOrigin.xyz, volume.ProbeSpacing);
        float atlasSize = float(volume.SCAtlasSize);

        bool missFound = false;
        uint missReads = 0u;
        constexpr uint kMaxMissReads = 8u;
        for (uint li = 0; li < lookupCount && missReads < kMaxMissReads && !missFound; ++li) {
            if (probePos.x < tg_aabb_min[li].x || probePos.x > tg_aabb_max[li].x ||
                probePos.y < tg_aabb_min[li].y || probePos.y > tg_aabb_max[li].y ||
                probePos.z < tg_aabb_min[li].z || probePos.z > tg_aabb_max[li].z)
                continue;

            for (uint ci = 0; ci < min(tg_card_info[li].y, 4u) && missReads < kMaxMissReads; ++ci) {
                uint cardIdx = tg_card_info[li].x + ci;
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
