/**
 * @file DDGIUpdateDepth.metal
 * @brief Lumen DDGI Phase 3 - Cooperative octahedral depth update + temporal filtering
 *
 * Restructured for Apple Silicon: 8x8 threadgroup cooperative mode.
 * Each group handles one probe. Thread (x,y) reads ray y*8+x.
 * Thread (x,y) then processes texel y*8+x.
 *
 * Per-thread device reads: ≤4 (well within Apple Silicon ~32 limit).
 * Previous single-thread approach had ~193 reads (causing flickering).
 *
 * Dispatch: (ProbeUpdateCount, 1, 1), threadGroupSize = (8, 8, 1)
 */

#include <metal_stdlib>
using namespace metal;

#include "../CommonTypes.metal"
#include "DDGIVolumeData.metal"

struct RayInfo {
    float distance;
    uint  texelIdx;
    uint  hit;
};

kernel void ddgi_update_depth(
    uint2 tid2d [[thread_position_in_threadgroup]],
    uint2 gid2d [[threadgroup_position_in_grid]],

    constant GlobalShaderData& gd [[buffer(0)]],
    constant DDGIVolumeData& volume [[buffer(1)]],
    device const DDGIRayData* ray_buffer [[buffer(2)]],
    device const float* depth_history [[buffer(3)]],
    device float* depth_output [[buffer(4)]],
    device const uint* probeUpdateList [[buffer(5)]]
)
{
    uint tid = tid2d.y * 8u + tid2d.x;
    uint group_id = gid2d.x;

    if (group_id >= volume.ProbeUpdateCount) return;

    uint probeIdx = probeUpdateList[group_id];
    if (probeIdx >= volume.ProbeCountTotal) return;
    uint rayOffset = probeIdx * volume.RaysPerProbe;

    // =====================================================================
    // Phase 1: Each thread reads one ray and stores in threadgroup
    // =====================================================================
    threadgroup RayInfo tg_rays[64];

    tg_rays[tid].distance = 0.0f;
    tg_rays[tid].texelIdx = 0u;
    tg_rays[tid].hit = 0u;

    if (tid < volume.RaysPerProbe) {
        DDGIRayData ray = ray_buffer[rayOffset + tid];

        if (ray.radiance_and_dist.w >= 0.0f) {
            float3 rayDir = ddgiRayDirection(tid, volume.RaysPerProbe, volume.FrameIndex);
            float2 uv = octahedralEncode(rayDir);
            uint2 texel = uint2(clamp(uint(uv.x * float(DDGI_DEPTH_RES)), 0u, DDGI_DEPTH_RES - 1u),
                                clamp(uint(uv.y * float(DDGI_DEPTH_RES)), 0u, DDGI_DEPTH_RES - 1u));

            tg_rays[tid].distance = ray.radiance_and_dist.w;
            tg_rays[tid].texelIdx = texel.y * DDGI_DEPTH_RES + texel.x;
            tg_rays[tid].hit = 1u;
        }
    }

    threadgroup_barrier(mem_flags::mem_threadgroup);

    // =====================================================================
    // Phase 2: Each thread processes one octahedral texel
    // Thread tid handles texel tid: aggregates from threadgroup + history blend
    // =====================================================================
    if (tid >= DDGI_DEPTH_TEXELS) return;

    uint texelIdx = tid;
    uint probeBase = probeIdx * DDGI_DEPTH_TEXELS * 2u;

    float sumDist = 0.0f;
    float sumDistSq = 0.0f;
    uint rayCount = 0u;

    for (uint r = 0; r < volume.RaysPerProbe; ++r) {
        if (tg_rays[r].hit && tg_rays[r].texelIdx == texelIdx) {
            float d = tg_rays[r].distance;
            sumDist += d;
            sumDistSq += d * d;
            rayCount++;
        }
    }

    uint meanIdx = probeBase + texelIdx;
    uint varIdx  = probeBase + DDGI_DEPTH_TEXELS + texelIdx;

    if (rayCount == 0u) {
        depth_output[meanIdx] = depth_history[meanIdx];
        depth_output[varIdx]  = depth_history[varIdx];
        return;
    }

    float mean     = sumDist / float(rayCount);
    float variance = abs(sumDistSq / float(rayCount) - mean * mean);

    if (rayCount < 4u) {
        variance = max(variance, volume.ProbeSpacing * volume.ProbeSpacing * 0.02f);
    }
    variance = max(variance, 0.001f);

    float rampFrames = 60.0f;
    float rampT = saturate(float(max(volume.FrameIndex, 1u) - 1u) / rampFrames);
    float alpha = mix(1.0f, volume.DepthBlurSigma, rampT);

    depth_output[meanIdx] = mix(depth_history[meanIdx], mean, alpha);
    depth_output[varIdx]  = mix(depth_history[varIdx],  variance, alpha);
}
