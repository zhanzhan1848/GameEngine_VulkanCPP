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

        if (hitDist <= -1.5f) {
            // C3.3：逃逸（w=-2，越出 RayMaxDistance）→ 天空。Phase 1.2 孪生
            // 同步（DDGIUpdateIrradiance.comp）：不再按 w 覆盖成 DDGI_SKY_COLOR
            // 常量——逃逸能量由 finalize 写入 ray_buffer（Vulkan 为
            // EvaluateSkyBase×SkyColor.xyz 单一事实源），这里直接沿用。
#ifdef PRIMAL_DDGI_PROJ_NOSKY
            // C 系列归因旋钮：逃逸/覆盖天空能量断电——A/B 实测 escape 天空
            // 在探针场平衡态的真实份额。
            radiance = float3(0.0f);
#endif
        } else if (hitDist < 0.0f) {
            // C3.3：步数耗尽（w=-1）——保留 finalize 的 SC 卡回退/黑，
            // 不再覆盖成天空（旧实现把两种 miss 一律换成 DDGI_SKY_COLOR，
            // 暗拱被 2.87 档天空整体填亮，Metal 拱廊 69.5 vs UE 22.5 的
            // 主能量源）。
        } else {
            float distWeight = 1.0f - smoothstep(vol.RayMaxDistance * 0.8f, vol.RayMaxDistance, hitDist);
            radiance *= max(distWeight, 0.2f);
            // T3.4 (D4): irradiance bailing — a hit inside ~2 texel-worlds
            // means the probe is buried in or hugging geometry; such rays
            // record self-light and destabilize the SH. Fade them out.
            float bail = smoothstep(0.0f, vol.SdfVoxelSizes[0].x * 2.0f, hitDist);
            radiance *= bail;
            // One lane can process multiple rays when RaysPerProbe > 64.
            // Count every hit rather than recording only a boolean.
            tg_hitFlags[tid] += 1u;
        }

        // T8.13 sync: same sampler the trace side used for this ray index —
        // the projection must reproduce the exact direction the radiance came
        // from (ProbeSampleLib port; ddgiRayDirection == mode 0 subset).
        // C7: LightDirection may be zero (mode 2 doesn't use it) — normalize(0)
        // is NaN and poisons the whole SH projection. Fall back to world down.
        float3 lightDir = vol.LightDirection.xyz;
        float3 toLight = normalize(dot(lightDir, lightDir) > 1e-8f ? -lightDir : float3(0.0f, -1.0f, 0.0f));
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
    // tg_partial/tg_hitFlags contain one aggregate per physical lane, not one
    // entry per ray.  Iterating RaysPerProbe here read past the 64-entry
    // threadgroup arrays at the 128-ray setting, producing unstable SH energy
    // and, on Apple GPUs, occasional address faults.
    for (uint t = 0; t < 64u; ++t) {
        for (uint i = 0; i < DDGI_SH_COEFF_COUNT; ++i) {
            shAccum[i] += tg_partial[t].sh[i];
        }
        hitCount += tg_hitFlags[t];
    }

    // NOTE (2026-09-10): the former +x/+y/+z neighbor-history blend
    // (SPATIAL_W = 0.04, the 2026-09-04 checkerboard/seam fix) is REMOVED to
    // match the Vulkan kernel. Reasons: a probe-update-stage blend bypasses
    // the gather-stage Chebyshev visibility test (light smears across walls),
    // and the +axis-only neighbor selection was a systematic directional
    // bias. Vulkan never had it and shows no seams. If Metal seams reappear,
    // the correct fix is a geometry/visibility-aware blend — NOT restoring
    // this unconditional history average.

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
    // DDGI_FREEZE=1 通过 TemporalAlpha=1.0 传入：alpha=0 → 100% history → 探针冻结
    float alpha = firstUpdate ? 1.0f : mix(0.35f, settledAlpha, min(conv * 2.0f, 1.0f));
    if (settledAlpha >= 1.0f) alpha = 0.0f;  // freeze mode

    float3 sh0 = shAccum[0];
    // 硬上界：l1/l2Limit 由未钳位的原始 sh0 推导时，prev-screen 反馈环的
    // 辐射尖峰会同步抬高上限（实测 L1=1661 = 3×原始L0 553.6），钳位失效。
    // L0 本身有上界 → L1/L2 上界随之封顶。
    // T2.4 (B7): 上限从 10/30/20 重定标至 64/192/128（×6.4）——旧 L0=10
    // 折算平均亮度 ≈2.8，恰好钉死在天空亮度，高能反弹（太阳池/HDR 高光）
    // 被整体截断。新值保留防 NaN 级发散的保护，普通探针（L0≈4）不受影响。
    float l1Limit = max(min(length(sh0) * 3.0f, 192.0f), 0.1f);
    float l2Limit = max(min(length(sh0) * 2.0f, 128.0f), 0.05f);

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
            if (mag > 64.0f) filtered *= 64.0f / mag;   // T2.4 (B7): was 10.0
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
