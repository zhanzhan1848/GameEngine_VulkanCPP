/**
 * @file DDGIUpdateIrradiance.metal
 * @brief Lumen DDGI Phase 2 - SH irradiance projection + hysteresis
 *
 * Changes vs original:
 * - Only stores L0+L1 (4 coefficients) per probe instead of SH3 (9)
 * - Energy clamping instead of per-coefficient clamp (preserves directionality)
 * - Stability from temporal filtering, not coefficient truncation
 *
 * Dispatch: (ProbeCountTotal, 1, 1), threadGroupSize = (1, 1, 1)
 */

#include <metal_stdlib>
using namespace metal;

#include "../CommonTypes.metal"
#include "DDGIVolumeData.metal"

kernel void ddgi_update_irradiance(
    uint gid [[thread_position_in_grid]],

    constant GlobalShaderData& gd [[buffer(0)]],
    constant DDGIVolumeData& vol [[buffer(1)]],
    device const DDGIRayData* rayData [[buffer(2)]],
    device const float3* irradianceHistory [[buffer(3)]],
    device float3* irradianceOutput [[buffer(4)]]
)
{
    if (gid >= vol.ProbeCountTotal) return;

    uint rayOffset = gid * vol.RaysPerProbe;

    // Accumulate L0+L1 only (4 coefficients)
    float3 shAccum[4];
    for (uint i = 0; i < 4u; ++i) shAccum[i] = float3(0.0f);

    for (uint r = 0; r < vol.RaysPerProbe; ++r) {
        uint rayIdx = rayOffset + r;
        float3 radiance = rayData[rayIdx].radiance_and_dist.xyz;
        float hitDist = rayData[rayIdx].radiance_and_dist.w;

        if (hitDist < 0.0f) {
            radiance = DDGI_SKY_COLOR * 0.5f;
        } else {
            float distWeight = 1.0f - smoothstep(0.0f, vol.RayMaxDistance, hitDist);
            radiance *= distWeight;
        }

        float3 rayDir = ddgiRayDirection(r, vol.RaysPerProbe, vol.FrameIndex);

        // L0+L1 SH basis
        float basis[4];
        basis[0] =  DDGI_SH_C0;
        basis[1] = -DDGI_SH_C1 * rayDir.y;
        basis[2] =  DDGI_SH_C1 * rayDir.z;
        basis[3] = -DDGI_SH_C1 * rayDir.x;

        float mcWeight = 4.0f * 3.14159265f / float(vol.RaysPerProbe);

        for (uint i = 0; i < 4u; ++i) {
            shAccum[i] += radiance * basis[i] * mcWeight;
        }
    }

    // Temporal blend with history
    uint probeBase = gid * 9u; // Keep stride 9 for buffer compatibility
    float alpha = (vol.FrameIndex < 6u) ? 1.0f : vol.ProbeHysteresis;

    // Energy clamping: limit the total irradiance energy, not individual coefficients
    // This preserves directional shape (L1 ratios) while preventing blowout
    float3 sh0 = shAccum[0]; // L0 = average irradiance (always positive for C0)
    float energyLimit = max(length(sh0) * 3.0f, 0.1f); // L1 energy capped relative to L0

    for (uint i = 0; i < 4u; ++i) {
        // Read history with NaN guard
        float3 history = irradianceHistory[probeBase + i];
        uint3 bits = as_type<uint3>(history);
        if (((bits.x | bits.y | bits.z) & 0x7F800000u) == 0x7F800000u) {
            history = float3(0.0f);
        }

        float3 filtered = mix(history, shAccum[i], alpha);

        // Energy-aware clamp: L0 clamped by absolute limit, L1 by relative energy
        if (i == 0u) {
            // L0: soft clamp on irradiance magnitude
            float mag = length(filtered);
            if (mag > 10.0f) filtered *= 10.0f / mag;
        } else {
            // L1: energy proportional to L0, preserves directionality
            float mag = length(filtered);
            if (mag > energyLimit) filtered *= energyLimit / mag;
        }

        irradianceOutput[probeBase + i] = filtered;
    }
}
