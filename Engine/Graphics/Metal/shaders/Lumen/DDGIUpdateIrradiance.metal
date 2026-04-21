/**
 * @file DDGIUpdateIrradiance.metal
 * @brief Lumen DDGI Phase 2 - SH3 irradiance projection + hysteresis
 *
 * Each thread handles one probe: reads all rays from storage buffer,
 * projects radiance onto 3rd-order SH (9 coefficients), applies probe
 * hysteresis with history, and writes to irradiance storage buffer.
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

    // Accumulate SH3 coefficients
    float3 shAccum[9];
    for (uint i = 0; i < 9u; ++i) shAccum[i] = float3(0.0f);

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

        // Project onto SH3
        float basis[9];
        shEvaluate(rayDir, basis);
        float mcWeight = 4.0f * 3.14159265f / float(vol.RaysPerProbe);

        for (uint i = 0; i < 9u; ++i) {
            shAccum[i] += radiance * basis[i] * mcWeight;
        }
    }

    // Read history with NaN guard
    uint probeBase = gid * 9u;
    float alpha = (vol.FrameIndex < 6u) ? 1.0f : vol.ProbeHysteresis;

    for (uint i = 0; i < 9u; ++i) {
        float3 history = irradianceHistory[probeBase + i];
        // Bitwise NaN guard (survives -ffast-math)
        uint3 bits = as_type<uint3>(history);
        if (((bits.x | bits.y | bits.z) & 0x7F800000u) == 0x7F800000u) {
            history = float3(0.0f);
        }
        float3 filtered = mix(history, shAccum[i], alpha);
        // Clamp to prevent SH ringing / extreme values
        filtered = clamp(filtered, float3(0.0f), float3(10.0f));
        irradianceOutput[probeBase + i] = filtered;
    }
}
