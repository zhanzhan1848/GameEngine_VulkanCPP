/**
 * @file DDGIUpdateIrradiance.metal
 * @brief Lumen DDGI Phase 2 - SH irradiance projection + temporal filtering
 *
 * Each thread handles one probe: reads all rays from storage buffer,
 * projects radiance onto 2nd-order SH (4 coefficients), applies temporal
 * EMA with history, and writes 4 texels to irradiance Texture3D.
 *
 * Dispatch: (ProbeCountTotal, 1, 1), threadGroupSize = (1, 1, 1)
 */

#include <metal_stdlib>
using namespace metal;

#include "../CommonTypes.metal"
#include "DDGIVolumeData.metal"

// ============================================================================
// Main Kernel
// ============================================================================

kernel void ddgi_update_irradiance(
    uint global_id [[thread_position_in_grid]],

    // Previous frame irradiance (history)
    texture3d<float, access::read> irradiance_history [[texture(0)]],

    // Output irradiance
    texture3d<float, access::write> irradiance_output [[texture(1)]],

    // Global shader data (matches C++ descriptor layout at buffer(0))
    constant GlobalShaderData& gd [[buffer(0)]],

    // DDGI volume data
    device DDGIVolumeData& volume [[buffer(1)]],

    // Ray data from trace pass
    device const DDGIRayData* ray_buffer [[buffer(2)]]
)
{
    uint probeIdx = global_id;
    if (probeIdx >= volume.ProbeCountTotal) return;

    uint3 gc = ddgiProbeGridCoord(probeIdx, volume.ProbeCounts);

    // -------------------------------------------------------------------
    // Accumulate SH coefficients from all rays
    // -------------------------------------------------------------------
    float3 shCoeffs[4] = { float3(0), float3(0), float3(0), float3(0) };
    float  validCount = 0.0f;

    uint baseOffset = probeIdx * volume.RaysPerProbe;

    for (uint r = 0; r < volume.RaysPerProbe; ++r) {
        DDGIRayData ray = ray_buffer[baseOffset + r];

        float3 radiance;
        float3 rayDir = ddgiFibonacciSphereDir(r, volume.RaysPerProbe, volume.FrameIndex);

        if (ray.radiance_and_dist.w >= 0.0f) {
            // Hit: use sampled radiance with distance attenuation
            float distWeight = 1.0f - smoothstep(0.0f, volume.RayMaxDistance, ray.radiance_and_dist.w);
            radiance = ray.radiance_and_dist.xyz * distWeight;
        } else {
            // Miss: sky contribution with constant weight
            radiance = DDGI_SKY_COLOR;
        }

        // Project onto 2nd-order SH basis
        // Y0 = C0                        (constant)
        // Y1 = -C1 * y                   (linear y)
        // Y2 =  C1 * z                   (linear z)
        // Y3 =  C1 * x                   (linear x)
        shCoeffs[0] += radiance * DDGI_SH_C0;
        shCoeffs[1] += radiance * (-DDGI_SH_C1 * rayDir.y);
        shCoeffs[2] += radiance * ( DDGI_SH_C1 * rayDir.z);
        shCoeffs[3] += radiance * ( DDGI_SH_C1 * rayDir.x);

        validCount += 1.0f;
    }

    // Normalize: Monte Carlo weight = 4π / N
    float normFactor = (4.0f * 3.14159265f) / max(validCount, 1.0f);
    for (uint i = 0; i < 4; ++i) {
        shCoeffs[i] *= normFactor;
        shCoeffs[i] = max(shCoeffs[i], float3(0.0f)); // Clamp negatives
    }

    // -------------------------------------------------------------------
    // Temporal filter with history
    // -------------------------------------------------------------------
    float alpha = volume.IrradianceBlurSigma;

    for (uint i = 0; i < DDGI_SH_COEFF_COUNT; ++i) {
        uint3 coord = uint3(gc.x, gc.y, gc.z * DDGI_SH_COEFF_COUNT + i);

        // Read previous frame
        float3 history = float3(0.0f);
        if (coord.z < irradiance_history.get_depth()) {
            history = irradiance_history.read(coord).rgb;
        }

        // Exponential moving average
        float3 filtered = mix(history, shCoeffs[i], alpha);

        irradiance_output.write(float4(filtered, 1.0f), coord);
    }
}
