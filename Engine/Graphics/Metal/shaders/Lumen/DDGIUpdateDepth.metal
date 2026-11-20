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
    float weight;
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
    // Phase 1: Lanes cooperatively load all rays. Keep enough shared entries
    // for the runtime-configurable upper bound; the old 64-entry array was
    // indexed up to RaysPerProbe and went out of bounds above 64 rays.
    // =====================================================================
    constexpr uint MAX_RAYS_PER_PROBE = 256u;
    threadgroup RayInfo tg_rays[MAX_RAYS_PER_PROBE];
    uint rayLimit = min(volume.RaysPerProbe, MAX_RAYS_PER_PROBE);
    const bool measured = volume.RaySampleMode == 7;

    for (uint r = tid; r < rayLimit; r += 64u) {
        tg_rays[r].distance = 0.0f;
        tg_rays[r].weight = 1.0f;
        tg_rays[r].texelIdx = 0u;
        tg_rays[r].hit = 0u;

        DDGIRayData ray = ray_buffer[rayOffset + r];

        float distance = ray.radiance_and_dist.w;
        if (measured && (distance == -2.0f || distance == -3.0f))
            distance = volume.RayMaxDistance;
        if (distance >= 0.0f && (!measured || isfinite(distance))) {
            // B8: binning must use the SAME mode-aware sampler as the trace
            // side — a fixed mode-0 direction mis-bins every ray when
            // RaySampleMode != 0 (directions no longer match traced radiance).
            float3 lightDir = volume.LightDirection.xyz;
            float3 toLight = (dot(lightDir, lightDir) > 1e-8f)
                ? normalize(-lightDir) : float3(0.0f, -1.0f, 0.0f);
            float4 raySample = ddgiSampleRayDirection(r, volume.RaysPerProbe,
                                                   volume.FrameIndex,
                                                   uint(volume.RaySampleMode),
                                                   toLight);
            float2 uv = octahedralEncode(raySample.xyz);
            uint2 texel = uint2(clamp(uint(uv.x * float(DDGI_DEPTH_RES)), 0u, DDGI_DEPTH_RES - 1u),
                                clamp(uint(uv.y * float(DDGI_DEPTH_RES)), 0u, DDGI_DEPTH_RES - 1u));

            tg_rays[r].distance = distance;
            tg_rays[r].weight = measured ? 0.25f / raySample.w : 1.0f;
            tg_rays[r].texelIdx = texel.y * DDGI_DEPTH_RES + texel.x;
            tg_rays[r].hit = 1u;
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
    float sumWeight = 0.0f;
    uint rayCount = 0u;

    for (uint r = 0; r < rayLimit; ++r) {
        if (tg_rays[r].hit && tg_rays[r].texelIdx == texelIdx) {
            float d = tg_rays[r].distance;
            if (measured) {
                // Weighted Welford: mean/M2 avoids cancellation for a narrow
                // depth distribution far from the probe (frozen mode 7).
                float weight = tg_rays[r].weight;
                float nextWeight = sumWeight + weight;
                if (sumWeight == 0.0f) {
                    sumDist = d;
                } else {
                    float delta = d - sumDist;
                    float nextMean = sumDist + delta * (weight / nextWeight);
                    sumDistSq += weight * delta * (d - nextMean);
                    sumDist = nextMean;
                }
                sumWeight = nextWeight;
            } else {
                sumDist += d;
                sumDistSq += d * d;
            }
            rayCount++;
        }
    }

    uint meanIdx = probeBase + texelIdx;
    uint varIdx  = probeBase + DDGI_DEPTH_TEXELS + texelIdx;

    if (rayCount == 0u) {
        // No current evidence invalidates measured visibility. Old positive
        // history must not turn unknown geometry into a clear direction.
        depth_output[meanIdx] = measured ? -1.0f : depth_history[meanIdx];
        depth_output[varIdx]  = measured ? 0.0f : depth_history[varIdx];
        return;
    }

    float rampFrames = 60.0f;
    float rampT = saturate(float(max(volume.FrameIndex, 1u) - 1u) / rampFrames);
    float alpha = mix(1.0f, volume.DepthBlurSigma, rampT);
    if (measured) {
        float variance = max(sumDistSq / sumWeight, 0.0f);
        float oldMean = depth_history[meanIdx], oldVariance = depth_history[varIdx];
        bool historyValid = oldMean >= 0.0f && isfinite(oldMean) &&
                            oldVariance >= 0.0f && isfinite(oldVariance);
        depth_output[meanIdx] = historyValid ? mix(oldMean, sumDist, alpha) : sumDist;
        depth_output[varIdx] = historyValid ? mix(oldVariance, variance, alpha) : variance;
        return;
    }

    float mean     = sumDist / float(rayCount);
    float variance = abs(sumDistSq / float(rayCount) - mean * mean);

    if (rayCount < 4u) {
        variance = max(variance, volume.ProbeSpacing * volume.ProbeSpacing * 0.02f);
    }
    variance = max(variance, 0.001f);

    depth_output[meanIdx] = mix(depth_history[meanIdx], mean, alpha);
    depth_output[varIdx]  = mix(depth_history[varIdx],  variance, alpha);
}
