/**
 * @file DDGIProbeIrradianceFromCards.metal
 * @brief Projects pre-computed card radiance contributions onto DDGI probe SH.
 *
 * Reads per-probe contribution lists (pre-computed on CPU) containing card indices
 * and SH projection weights, then multiplies by per-card radiance (from
 * DDGICardRadianceAvg) to produce updated probe irradiance SH coefficients.
 *
 * Replaces DDGIUpdateIrradiance when using Surface Cache as radiance source.
 * Uses only L0+L1 (4 SH bands) — L2 coefficients are copied from history.
 *
 * Dispatch: (ProbeUpdateCount, 1, 1), threadGroupSize = (64, 1, 1)
 */

#include <metal_stdlib>
using namespace metal;

#include "DDGIVolumeData.metal"

struct ProbeCardContrib {
    uint  card_index;
    float sh_weights[4];  // pre-computed: weight * SH_basis(direction)
};

struct ProbeContribRange {
    uint start;
    uint count;
};

kernel void ddgi_probe_irradiance_from_cards(
    uint gid [[thread_position_in_grid]],

    constant DDGIVolumeData& vol        [[buffer(0)]],
    device const ProbeContribRange* ranges       [[buffer(1)]],
    device const ProbeCardContrib* contribs      [[buffer(2)]],
    device const float3* card_radiance           [[buffer(3)]],
    device const float3* irradiance_history      [[buffer(4)]],
    device float3* irradiance_output             [[buffer(5)]],
    device const uint* probeUpdateList           [[buffer(6)]]
) {
    if (gid >= vol.ProbeUpdateCount) return;

    uint probeIdx = probeUpdateList[gid];
    auto& range = ranges[probeIdx];

    // Accumulate SH from card contributions
    float3 sh[4];
    for (uint i = 0; i < 4u; ++i) sh[i] = float3(0.0);

    for (uint i = 0; i < range.count && i < 6u; ++i) {
        auto& c = contribs[range.start + i];
        float3 radiance = card_radiance[c.card_index];
        sh[0] += c.sh_weights[0] * radiance;
        sh[1] += c.sh_weights[1] * radiance;
        sh[2] += c.sh_weights[2] * radiance;
        sh[3] += c.sh_weights[3] * radiance;
    }

    // Temporal blend with history (same logic as UpdateIrradiance)
    uint base = probeIdx * 9u;

    float alpha = vol.ProbeHysteresis;

    // Energy clamping limits
    float3 sh0 = sh[0];
    float l1Limit = max(length(sh0) * 3.0f, 0.1f);

    // Write L0 + L1 (bands 0-3)
    for (uint i = 0; i < 4u; ++i) {
        float3 history = irradiance_history[base + i];

        // NaN guard
        uint3 bits = as_type<uint3>(history);
        if (((bits.x | bits.y | bits.z) & 0x7F800000u) == 0x7F800000u)
            history = float3(0.0);

        float3 filtered = mix(history, sh[i], alpha);

        // Energy clamp
        if (i == 0u) {
            float mag = length(filtered);
            if (mag > 10.0f) filtered *= 10.0f / mag;
        } else {
            float mag = length(filtered);
            if (mag > l1Limit) filtered *= l1Limit / mag;
        }

        irradiance_output[base + i] = filtered;
    }

    // L2 bands (4-8): copy from history (not enough card samples for reliable L2)
    for (uint i = 4u; i < 9u; ++i) {
        irradiance_output[base + i] = irradiance_history[base + i];
    }
}
