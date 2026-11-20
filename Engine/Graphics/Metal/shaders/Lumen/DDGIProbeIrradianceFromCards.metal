/**
 * @file DDGIProbeIrradianceFromCards.metal
 * @brief Projects pre-computed card radiance contributions onto DDGI probe SH.
 *
 * Reads per-probe contribution lists (pre-computed on CPU) containing card indices
 * and SH projection weights, then multiplies by per-card radiance (from
 * DDGICardRadianceAvg) to produce updated probe irradiance SH coefficients.
 *
 * Card projection bootstraps low-confidence probes and recovers missing trace
 * energy. In the default coldstart mode, converged nonzero traced probes keep
 * ownership; the host can also request the legacy or gated blending modes.
 * Only captured card blocks contribute coverage (card_radiance.w).
 *
 * All 9 SH bands (L0-L2) are projected and written; projection signs match
 * DDGIUpdateIrradiance's basis.
 *
 * Dispatch: (ProbeCountTotal, 1, 1), threadGroupSize = (64, 1, 1)
 */

#include <metal_stdlib>
using namespace metal;

#include "DDGIVolumeData.metal"

// 40 bytes tight — must match the C++ SCProbeCardContrib byte-for-byte.
struct ProbeCardContrib {
    uint  card_index;
    float sh_weights[9];  // pre-computed: weight * SH_basis(direction), L0-L2
};

struct ProbeContribRange {
    uint start;
    uint count;
};

kernel void ddgi_probe_irradiance_from_cards(
    uint probeIdx [[thread_position_in_grid]],

    constant DDGIVolumeData& vol        [[buffer(0)]],
    device const ProbeContribRange* ranges       [[buffer(1)]],
    device const ProbeCardContrib* contribs      [[buffer(2)]],
    device const float4* card_radiance           [[buffer(3)]],
    // vec4[9]/probe layout (144B): index float4, take xyz (MSL float3* strides 12B).
    device const float4* irradiance_history      [[buffer(4)]],
    device float4* irradiance_output             [[buffer(5)]],
    device const float4* probe_confidence        [[buffer(6)]],
    constant float4& sc_inject                    [[buffer(7)]]
) {
    if (probeIdx >= vol.ProbeCountTotal) return;

    device const ProbeContribRange& range = ranges[probeIdx];

    // Accumulate full L0-L2 SH from card contributions.
    float3 sh[9];
    for (uint i = 0; i < 9u; ++i) sh[i] = float3(0.0);

    uint n = min(range.count, 12u);
    float validSum = 0.0f;
    uint validCount = 0u;
    for (uint i = 0; i < n; ++i) {
        device const ProbeCardContrib& c = contribs[range.start + i];
        float4 cr = card_radiance[c.card_index];
        if (cr.w <= 0.0f) continue;
        ++validCount;
        validSum += cr.w;  // per-card atlas coverage (0..1)
        for (uint b = 0; b < 9u; ++b)
            sh[b] += c.sh_weights[b] * cr.xyz;
    }

    uint base = probeIdx * 9u;
    float alpha = vol.ProbeHysteresis;
    // CPU solid-angle weights have no extra 4pi. Match the trace L0
    // safety bound instead of discarding legitimate HDR scale at 4.
    constexpr float SC_L0_GATE = 64.0f;
    {
        float l0mag = length(sh[0]);
        if (l0mag > SC_L0_GATE) {
            float s = SC_L0_GATE / l0mag;
            for (uint i = 0; i < 9u; ++i) sh[i] *= s;
        }
    }
    float3 sh0 = sh[0];
    float l1Limit = max(length(sh0) * 3.0f, 0.1f);
    float l2Limit = max(length(sh0) * 2.0f, 0.1f);

    // Step 1: SC-filtered results — temporal blend with history + energy
    // clamps mirroring DDGIUpdateIrradiance (L0 <= 64, L1 <= 3|L0|, L2 <= 2|L0|).
    float3 scResult[9];
    for (uint i = 0; i < 9u; ++i) {
        float3 history = irradiance_history[base + i].xyz;
        uint3 bits = as_type<uint3>(history);
        if (((bits.x | bits.y | bits.z) & 0x7F800000u) == 0x7F800000u)
            history = float3(0.0);

        float3 filtered = mix(history, sh[i], alpha);
        float limit = (i == 0u) ? 64.0f : ((i < 4u) ? l1Limit : l2Limit);
        float mag = length(filtered);
        if (mag > limit) filtered *= limit / mag;
        scResult[i] = filtered;
    }

    // Step 2: Path A result (SDF ray trace already wrote to output this frame).
    float3 pathA[9];
    for (uint i = 0; i < 9u; ++i) {
        pathA[i] = irradiance_output[base + i].xyz;
    }

    // Step 3: match Vulkan's trace-first blend. Card averages are low-frequency
    // and occlusion-unaware, so cap them at 0.45 and leave the SDF trace in
    // control of the sun-patch/shade contrast.
    float countCoverage = saturate(float(validCount) / 6.0f);
    float validCoverage = (validCount > 0u) ? saturate(validSum / float(validCount)) : 0.0f;
    float scLum = dot(scResult[0], float3(0.2126f, 0.7152f, 0.0722f));
    float rawPathLum = dot(pathA[0], float3(0.2126f, 0.7152f, 0.0722f));
    float pathLum = max(rawPathLum, 1e-4f);
    float agree = smoothstep(0.0f, 1.0f, scLum / pathLum);
    constexpr float SC_TAKEOVER_MAX = 0.45f;
    // Frozen Vulkan policy: cards initialize and recover a missing source;
    // converged traced probes retain ownership in the default coldstart mode.
    const float SC_SETTLED_FLOOR = 0.15f;
    float settled = smoothstep(0.5f, 1.0f, probe_confidence[probeIdx].w);
    float injectBase = countCoverage * validCoverage;
    float takeover = SC_TAKEOVER_MAX;
    if (sc_inject.x >= 1.5f) {
        float recovery = rawPathLum <= 1e-3f ? SC_SETTLED_FLOOR : 0.0f;
        takeover = mix(SC_TAKEOVER_MAX, recovery, settled);
    } else if (sc_inject.x >= 0.5f) {
        takeover = mix(SC_TAKEOVER_MAX, SC_SETTLED_FLOOR, settled);
    }
    float scWeight = injectBase * agree * takeover;

    for (uint i = 0; i < 9u; ++i) {
        irradiance_output[base + i] = float4(mix(pathA[i], scResult[i], scWeight), irradiance_output[base + i].w);
    }
}
