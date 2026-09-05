/**
 * @file DDGIProbeIrradianceFromCards.metal
 * @brief Projects pre-computed card radiance contributions onto DDGI probe SH.
 *
 * Reads per-probe contribution lists (pre-computed on CPU) containing card indices
 * and SH projection weights, then multiplies by per-card radiance (from
 * DDGICardRadianceAvg) to produce updated probe irradiance SH coefficients.
 *
 * SC-first hierarchy (Lumen-like sparse cache): the surface-cache projection is
 * the PRIMARY feed; the SDF ray trace result (Path A, already in the output
 * buffer) fills in proportionally to how thin the SC coverage is. Coverage =
 * assigned-card count × per-card atlas validity (card_radiance.w).
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
    device float4* irradiance_output             [[buffer(5)]]
) {
    if (probeIdx >= vol.ProbeCountTotal) return;

    device const ProbeContribRange& range = ranges[probeIdx];

    // Accumulate full L0-L2 SH from card contributions.
    float3 sh[9];
    for (uint i = 0; i < 9u; ++i) sh[i] = float3(0.0);

    uint n = min(range.count, 12u);
    float validSum = 0.0f;
    for (uint i = 0; i < n; ++i) {
        device const ProbeCardContrib& c = contribs[range.start + i];
        float4 cr = card_radiance[c.card_index];
        validSum += cr.w;  // per-card atlas coverage (0..1)
        for (uint b = 0; b < 9u; ++b)
            sh[b] += c.sh_weights[b] * cr.xyz;
    }

    uint base = probeIdx * 9u;
    float alpha = vol.ProbeHysteresis;
    // 能量门：卡片立体角权重（CPU 侧 solidAngle×NdotL×4π，Ω 不饱和）在
    // 近距探针上把原始投影 L0 抬到 trace 尺度的百倍以上（实测 ~1230 vs
    // ~1.2），且下方 l1/l2Limit 由它推导，随垃圾同步膨胀（实测 L1=1661）。
    // 先把 L0 门到 trace 工作点，再派生波段上限——全波段硬上界。
    constexpr float SC_L0_GATE = 1.5f;
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
    // clamps mirroring DDGIUpdateIrradiance (L0 <= 10, L1 <= 3|L0|, L2 <= 2|L0|).
    float3 scResult[9];
    for (uint i = 0; i < 9u; ++i) {
        float3 history = irradiance_history[base + i].xyz;
        uint3 bits = as_type<uint3>(history);
        if (((bits.x | bits.y | bits.z) & 0x7F800000u) == 0x7F800000u)
            history = float3(0.0);

        float3 filtered = mix(history, sh[i], alpha);
        float limit = (i == 0u) ? 10.0f : ((i < 4u) ? l1Limit : l2Limit);
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
    float countCoverage = saturate(float(n) / 6.0f);
    float validCoverage = (n > 0u) ? saturate(validSum / float(n)) : 0.0f;
    float scLum = dot(scResult[0], float3(0.2126f, 0.7152f, 0.0722f));
    float pathLum = max(dot(pathA[0], float3(0.2126f, 0.7152f, 0.0722f)), 1e-4f);
    float agree = smoothstep(0.0f, 1.0f, scLum / pathLum);
    constexpr float SC_TAKEOVER_MAX = 0.45f;
    float scWeight = countCoverage * validCoverage * SC_TAKEOVER_MAX * agree;

    for (uint i = 0; i < 9u; ++i) {
        irradiance_output[base + i] = float4(mix(pathA[i], scResult[i], scWeight), 1.0f);
    }
}
