/**
 * @file DDGIUpdateIrradiance.metal
 * @brief Lumen DDGI Phase 2 - SH irradiance projection + hysteresis
 *
 * Projects ray radiance into L0+L1+L2 (9 coefficients) per probe.
 * GIGather already reads all 9 coeff, so L2 is essentially free.
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
    device float3* irradianceOutput [[buffer(4)]],

    // Probe update list (sparse indices of probes to update this frame)
    device const uint* probeUpdateList [[buffer(5)]]
)
{
    if (gid >= vol.ProbeUpdateCount) return;

    // Map sparse update index to real probe index
    uint probeIdx = probeUpdateList[gid];
    uint rayOffset = probeIdx * vol.RaysPerProbe;

    // Accumulate L0+L1+L2 (9 coefficients)
    float3 shAccum[9];
    for (uint i = 0; i < 9u; ++i) shAccum[i] = float3(0.0f);

    for (uint r = 0; r < vol.RaysPerProbe; ++r) {
        uint rayIdx = rayOffset + r;
        float3 radiance = rayData[rayIdx].radiance_and_dist.xyz;
        float hitDist = rayData[rayIdx].radiance_and_dist.w;

        if (hitDist < 0.0f) {
            radiance = DDGI_SKY_COLOR;
        } else {
            // Gentler distance falloff — only attenuate near max distance
            float distWeight = 1.0f - smoothstep(vol.RayMaxDistance * 0.8f, vol.RayMaxDistance, hitDist);
            radiance *= max(distWeight, 0.2f);
        }

        float3 d = ddgiRayDirection(r, vol.RaysPerProbe, vol.FrameIndex);
        float x = d.x, y = d.y, z = d.z;
        float x2 = x*x, y2 = y*y, z2 = z*z;

        // L0+L1+L2 SH basis
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

        for (uint i = 0; i < 9u; ++i) {
            shAccum[i] += radiance * basis[i] * mcWeight;
        }
    }

    // Temporal blend with history
    uint probeBase = probeIdx * 9u;

    // Smooth alpha ramp: 1.0 → ProbeHysteresis over ~60 frames
    float rampFrames = 60.0f;
    float t = saturate(float(max(vol.FrameIndex, 1u) - 1u) / rampFrames);
    float alpha = mix(1.0f, vol.ProbeHysteresis, t);

    // Energy clamping limits
    float3 sh0 = shAccum[0];
    float l1Limit = max(length(sh0) * 3.0f, 0.1f);
    float l2Limit = max(length(sh0) * 2.0f, 0.05f);

    for (uint i = 0; i < 9u; ++i) {
        // Read history with NaN guard
        float3 history = irradianceHistory[probeBase + i];
        uint3 bits = as_type<uint3>(history);
        if (((bits.x | bits.y | bits.z) & 0x7F800000u) == 0x7F800000u) {
            history = float3(0.0f);
        }

        float3 filtered = mix(history, shAccum[i], alpha);

        // Energy-aware clamp per band
        if (i == 0u) {
            // L0: absolute limit
            float mag = length(filtered);
            if (mag > 10.0f) filtered *= 10.0f / mag;
        } else if (i < 4u) {
            // L1: energy proportional to L0
            float mag = length(filtered);
            if (mag > l1Limit) filtered *= l1Limit / mag;
        } else {
            // L2: tighter clamp (higher order = more noise)
            float mag = length(filtered);
            if (mag > l2Limit) filtered *= l2Limit / mag;
        }

        irradianceOutput[probeBase + i] = filtered;
    }
}
