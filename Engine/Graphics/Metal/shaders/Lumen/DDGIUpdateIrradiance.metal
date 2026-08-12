/**
 * @file DDGIUpdateIrradiance.metal
 * @brief Lumen DDGI Phase 2 - Cooperative SH irradiance projection + hysteresis
 *
 * Restructured for Apple Silicon: 8x8 threadgroup cooperative mode.
 * Each group handles one probe. Thread (x,y) reads ray y*8+x.
 * Thread (0,0) aggregates, blends with history, writes output.
 *
 * Per-thread device reads: ≤12 (well within Apple Silicon ~32 limit).
 * Previous single-thread approach had ~221 reads (causing flickering).
 *
 * Dispatch: (ProbeUpdateCount, 1, 1), threadGroupSize = (8, 8, 1)
 */

#include <metal_stdlib>
using namespace metal;

#include "../CommonTypes.metal"
#include "DDGIVolumeData.metal"

struct PartialSH {
    float3 sh[DDGI_SH_COEFF_COUNT];
};

kernel void ddgi_update_irradiance(
    uint2 tid2d [[thread_position_in_threadgroup]],
    uint2 gid2d [[threadgroup_position_in_grid]],

    constant GlobalShaderData& gd [[buffer(0)]],
    constant DDGIVolumeData& vol [[buffer(1)]],
    device const DDGIRayData* rayData [[buffer(2)]],
    device const float3* irradianceHistory [[buffer(3)]],
    device float3* irradianceOutput [[buffer(4)]],
    device const uint* probeUpdateList [[buffer(5)]],
    device float* confidenceBuffer [[buffer(6)]]
)
{
    uint tid = tid2d.y * 8u + tid2d.x;
    uint group_id = gid2d.x;

    if (group_id >= vol.ProbeUpdateCount) return;

    uint probeIdx = probeUpdateList[group_id];
    if (probeIdx >= vol.ProbeCountTotal) return;
    uint rayOffset = probeIdx * vol.RaysPerProbe;

    // =====================================================================
    // Phase 1: Each thread reads one ray and computes partial SH
    // =====================================================================
    threadgroup PartialSH tg_partial[64];
    threadgroup uint tg_hitFlags[64];

    for (uint i = 0; i < DDGI_SH_COEFF_COUNT; ++i)
        tg_partial[tid].sh[i] = float3(0.0f);
    tg_hitFlags[tid] = 0u;

    if (tid < vol.RaysPerProbe) {
        DDGIRayData ray = rayData[rayOffset + tid];
        float3 radiance = ray.radiance_and_dist.xyz;
        float hitDist = ray.radiance_and_dist.w;

        if (hitDist < 0.0f) {
            radiance = DDGI_SKY_COLOR;
        } else {
            float distWeight = 1.0f - smoothstep(vol.RayMaxDistance * 0.8f, vol.RayMaxDistance, hitDist);
            radiance *= max(distWeight, 0.2f);
            tg_hitFlags[tid] = 1u;
        }

        float3 d = ddgiRayDirection(tid, vol.RaysPerProbe, vol.FrameIndex);
        float x = d.x, y = d.y, z = d.z;
        float x2 = x*x, y2 = y*y, z2 = z*z;

        float basis[9];
        basis[0] =  DDGI_SH_C0;
        basis[1] = -DDGI_SH_C1 * y;
        basis[2] =  DDGI_SH_C1 * z;
        basis[3] = -DDGI_SH_C1 * x;
        basis[4] =  DDGI_SH_C2_0 * y * x;
        basis[5] = -DDGI_SH_C2_0 * y * z;
        basis[6] =  DDGI_SH_C2_1 * (3.0f * z2 - 1.0f);
        basis[7] = -DDGI_SH_C2_0 * x * z;
        basis[8] =  DDGI_SH_C2_2 * (x2 - y2);

        float mcWeight = 4.0f * 3.14159265f / float(vol.RaysPerProbe);

        for (uint i = 0; i < DDGI_SH_COEFF_COUNT; ++i) {
            tg_partial[tid].sh[i] = radiance * basis[i] * mcWeight;
        }
    }

    threadgroup_barrier(mem_flags::mem_threadgroup);

    // =====================================================================
    // Phase 2: Thread (0,0) aggregates, blends with history, writes output
    // =====================================================================
    if (tid != 0u) return;

    float3 shAccum[DDGI_SH_COEFF_COUNT];
    for (uint i = 0; i < DDGI_SH_COEFF_COUNT; ++i) shAccum[i] = float3(0.0f);

    uint hitCount = 0u;
    for (uint t = 0; t < vol.RaysPerProbe; ++t) {
        for (uint i = 0; i < DDGI_SH_COEFF_COUNT; ++i) {
            shAccum[i] += tg_partial[t].sh[i];
        }
        hitCount += tg_hitFlags[t];
    }

    // Temporal blend with history
    // Cache history and output in registers to avoid redundant device reads
    // (was 18 extra reads for confidence — now 0 extra reads)
    uint probeBase = probeIdx * 9u;

    float rampFrames = 60.0f;
    float rampT = saturate(float(max(vol.FrameIndex, 1u) - 1u) / rampFrames);
    float alpha = mix(1.0f, vol.ProbeHysteresis, rampT);

    float3 sh0 = shAccum[0];
    float l1Limit = max(length(sh0) * 3.0f, 0.1f);
    float l2Limit = max(length(sh0) * 2.0f, 0.05f);

    float3 cachedHist[DDGI_SH_COEFF_COUNT];
    float3 cachedOut[DDGI_SH_COEFF_COUNT];

    for (uint i = 0; i < DDGI_SH_COEFF_COUNT; ++i) {
        float3 history = irradianceHistory[probeBase + i];
        uint3 bits = as_type<uint3>(history);
        if (((bits.x | bits.y | bits.z) & 0x7F800000u) == 0x7F800000u)
            history = float3(0.0f);

        cachedHist[i] = history;

        float3 filtered = mix(history, shAccum[i], alpha);

        if (i == 0u) {
            float mag = length(filtered);
            if (mag > 10.0f) filtered *= 10.0f / mag;
        } else if (i < 4u) {
            float mag = length(filtered);
            if (mag > l1Limit) filtered *= l1Limit / mag;
        } else {
            float mag = length(filtered);
            if (mag > l2Limit) filtered *= l2Limit / mag;
        }

        cachedOut[i] = filtered;
        irradianceOutput[probeBase + i] = filtered;
    }

    // --- Confidence computation (uses cached values, 0 extra device reads) ---
    confidenceBuffer[probeIdx * 4 + 0] = float(hitCount) / float(vol.RaysPerProbe);

    float historyL1 = 0.0f, currentL1 = 0.0f, diffL1 = 0.0f;
    for (uint i = 0; i < DDGI_SH_COEFF_COUNT; i++) {
        historyL1 += length(cachedHist[i]);
        currentL1 += length(cachedOut[i]);
        diffL1 += length(cachedOut[i] - cachedHist[i]);
    }
    float stability = 1.0f - diffL1 / max(max(historyL1, currentL1), 0.001f);
    confidenceBuffer[probeIdx * 4 + 1] = clamp(stability, 0.0f, 1.0f);

    float age = confidenceBuffer[probeIdx * 4 + 3];
    confidenceBuffer[probeIdx * 4 + 3] = min(age + 1.0f / 30.0f, 1.0f);
}
