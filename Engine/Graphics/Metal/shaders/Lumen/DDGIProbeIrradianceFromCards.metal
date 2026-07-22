/**
 * @file DDGIProbeIrradianceFromCards.metal
 * @brief Projects pre-computed card radiance contributions onto DDGI probe SH.
 *
 * Reads per-probe contribution lists (pre-computed on CPU) containing card indices
 * and SH projection weights, then multiplies by per-card radiance (from
 * DDGICardRadianceAvg) to produce updated probe irradiance SH coefficients.
 *
 * Confidence-based blending with Path A (SDF Ray Trace):
 *   - Reads convergenceAge from confidence buffer
 *   - Young probes (age < 0.33): SC projection dominates (bootstrap)
 *   - Mature probes (age >= 0.33): Path A result preserved, B contribution = 0
 *
 * Uses only L0+L1 (4 SH bands) — L2 coefficients preserved from Path A.
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
    device const uint* probeUpdateList           [[buffer(6)]],
    device const float* confidenceBuffer         [[buffer(7)]]
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

    uint base = probeIdx * 9u;
    float alpha = vol.ProbeHysteresis;
    float3 sh0 = sh[0];
    float l1Limit = max(length(sh0) * 3.0f, 0.1f);

    // Step 1: Compute SC-filtered results (temporal blend with history + energy clamp)
    float3 scResult[4];
    for (uint i = 0; i < 4u; ++i) {
        float3 history = irradiance_history[base + i];
        uint3 bits = as_type<uint3>(history);
        if (((bits.x | bits.y | bits.z) & 0x7F800000u) == 0x7F800000u)
            history = float3(0.0);

        float3 filtered = mix(history, sh[i], alpha);
        if (i == 0u) {
            float mag = length(filtered);
            if (mag > 10.0f) filtered *= 10.0f / mag;
        } else {
            float mag = length(filtered);
            if (mag > l1Limit) filtered *= l1Limit / mag;
        }
        scResult[i] = filtered;
    }

    // Step 2: Read Path A result (SDF ray trace already wrote to output this frame)
    float3 pathA[4];
    for (uint i = 0; i < 4u; ++i) {
        pathA[i] = irradiance_output[base + i];
    }

    // Step 3: Confidence-based blend
    // convergenceAge ramps 0→1 over ~30 frames; young probes get SC bootstrap
    float convergenceAge = confidenceBuffer[probeIdx * 4u + 3u];
    float bootstrapWeight = saturate(1.0 - convergenceAge * 3.0);

    // Write blended L0 + L1
    for (uint i = 0; i < 4u; ++i) {
        irradiance_output[base + i] = mix(pathA[i], scResult[i], bootstrapWeight);
    }

    // L2 bands (4-8): preserve Path A values (already in output from SDF ray trace)
}
