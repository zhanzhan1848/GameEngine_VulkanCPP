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
    // vec4[9]/probe layout (144B): index float4, take xyz (MSL float3* strides 12B).
    device const float4* irradianceHistory [[buffer(3)]],
    device float4* irradianceOutput [[buffer(4)]],
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
    // Phase 1: Each thread processes rays tid, tid+64, ... (T4.2: supports
    // RaysPerProbe > 64 — e.g. 128 = two iterations per thread) and
    // accumulates partial SH into its threadgroup slot.
    // =====================================================================
    threadgroup PartialSH tg_partial[64];
    threadgroup uint tg_hitFlags[64];

    for (uint i = 0; i < DDGI_SH_COEFF_COUNT; ++i)
        tg_partial[tid].sh[i] = float3(0.0f);
    tg_hitFlags[tid] = 0u;

    for (uint r = tid; r < vol.RaysPerProbe; r += 64u) {
        DDGIRayData ray = rayData[rayOffset + r];
        float3 radiance = ray.radiance_and_dist.xyz;
        float hitDist = ray.radiance_and_dist.w;

        if (hitDist < 0.0f) {
            radiance = DDGI_SKY_COLOR;
        } else {
            float distWeight = 1.0f - smoothstep(vol.RayMaxDistance * 0.8f, vol.RayMaxDistance, hitDist);
            radiance *= max(distWeight, 0.2f);
            tg_hitFlags[tid] = 1u;
        }

        // T8.13 sync: same sampler the trace side used for this ray index —
        // the projection must reproduce the exact direction the radiance came
        // from (ProbeSampleLib port; ddgiRayDirection == mode 0 subset).
        float3 toLight = normalize(-vol.LightDirection.xyz);
        float4 raySample = ddgiSampleRayDirection(r, vol.RaysPerProbe,
                                                  vol.FrameIndex,
                                                  uint(vol.RaySampleMode), toLight);
        float3 d = raySample.xyz;
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

        // T8.13 sync: PDF-correct Monte Carlo weight for the radiance-SH
        // integral. Mode 0 keeps the legacy uniform expression bit-for-bit;
        // importance-sampled modes divide by the sampler's pdf instead.
        float mcWeight = (uint(vol.RaySampleMode) == 0u)
            ? (4.0f * 3.14159265f / float(vol.RaysPerProbe))
            : 1.0f / (float(vol.RaysPerProbe) * max(raySample.w, 1e-5f));

        for (uint i = 0; i < DDGI_SH_COEFF_COUNT; ++i) {
            tg_partial[tid].sh[i] += radiance * basis[i] * mcWeight;
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

    // =====================================================================
    // 2026-09-04 棋盘格/闪烁修复：探针空间平滑（irradiance blur）
    //
    // 每帧只更新 256/16384 探针（优先级调度），不同子集在不同帧更新。
    // SC atlas 辐射的逐帧微小变化 + 探针更新批次差异 → 相邻探针 SH 值
    // 不连续 → gather 插值在格边界产生可见的方块/棋盘格图案 + 高频闪烁。
    //
    // 修复：对每个探针的 SH 结果与其 +x/+y/+z 邻居的历史值混合。
    // 负方向邻居在下一帧作为正方向邻居来平滑当前探针——渐进收敛。
    // 标准 DDGI irradiance blur 的简化实现（只混合正方向，不协调权重）。
    // =====================================================================
    {
        constexpr float SPATIAL_W = 0.04f;  // 每个正轴邻居的混合权重
        uint3 counts = uint3(vol.ProbeCounts[0], vol.ProbeCounts[1], vol.ProbeCounts[2]);
        uint pz = probeIdx / (counts.x * counts.y);
        uint rem = probeIdx % (counts.x * counts.y);
        uint py = rem / counts.x;
        uint px = rem % counts.x;
        uint3 gc = uint3(px, py, pz);

        for (uint axis = 0u; axis < 3u; ++axis) {
            uint3 nc = gc;
            if (nc[axis] + 1u >= counts[axis]) continue;  // 边界跳过
            nc[axis] += 1u;
            uint nIdx = nc.x + counts.x * (nc.y + counts.y * nc.z);
            uint nBase = nIdx * 9u;
            // 跳过 NaN/Inf 邻居
            float3 n0 = irradianceHistory[nBase + 0u].xyz;
            uint3 nb = as_type<uint3>(n0);
            if (((nb.x | nb.y | nb.z) & 0x7F800000u) == 0x7F800000u) continue;
            for (uint i = 0; i < DDGI_SH_COEFF_COUNT; ++i) {
                float3 nSH = irradianceHistory[nBase + i].xyz;
                shAccum[i] = mix(shAccum[i], nSH, SPATIAL_W);
            }
        }
    }

    // T8.13 sync: temporal blend with history — Vulkan DDGIUpdateIrradiance
    // scheme. A probe that never converged must not EMA-blend against the
    // cleared buffer (first update publishes directly); young probes blend
    // aggressively (alpha 0.35, τ≈3 frames) so the multi-bounce SC↔probe loop
    // becomes visible within seconds; alpha settles to the configured
    // TemporalAlpha once the convergence ramp passes 0.5 (~15 updates).
    uint probeBase = probeIdx * 9u;

    uint confidenceBase = probeIdx * 4u;
    float conv = confidenceBuffer[confidenceBase + 3u];
    bool firstUpdate = conv < 0.5f;
    float settledAlpha = clamp(vol.TemporalAlpha, 0.0f, 1.0f);
    float alpha = firstUpdate ? 1.0f : mix(0.35f, settledAlpha, min(conv * 2.0f, 1.0f));

    float3 sh0 = shAccum[0];
    float l1Limit = max(length(sh0) * 3.0f, 0.1f);
    float l2Limit = max(length(sh0) * 2.0f, 0.05f);

    float3 cachedHist[DDGI_SH_COEFF_COUNT];
    float3 cachedOut[DDGI_SH_COEFF_COUNT];

    for (uint i = 0; i < DDGI_SH_COEFF_COUNT; ++i) {
        float3 history = irradianceHistory[probeBase + i].xyz;
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
        irradianceOutput[probeBase + i] = float4(filtered, 1.0f);
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
    // T8.13 sync: convergence ramp climbs 1/30 per update (Vulkan parity) —
    // also the SC bootstrap and the confidence debug view's signal.
    confidenceBuffer[probeIdx * 4 + 3] = min(age + 1.0f / 30.0f, 1.0f);
}
