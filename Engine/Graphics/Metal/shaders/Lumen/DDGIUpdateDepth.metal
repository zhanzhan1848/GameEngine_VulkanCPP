/**
 * @file DDGIUpdateDepth.metal
 * @brief Lumen DDGI Phase 2 - 8-direction depth update + temporal filtering
 *
 * Each thread handles one probe: reads all rays, classifies hit distances
 * into 8 octants (keeps closest per octant), applies temporal EMA, and
 * writes to depth storage buffer.
 *
 * Buffer layout: depthBuffer[probeCount * 8], one float per octant.
 * Index: probeIdx * 8 + octant
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

kernel void ddgi_update_depth(
    uint global_id [[thread_position_in_grid]],

    // Global shader data
    constant GlobalShaderData& gd [[buffer(0)]],

    // DDGI volume data
    constant DDGIVolumeData& volume [[buffer(1)]],

    // Ray data from trace pass
    device const DDGIRayData* ray_buffer [[buffer(2)]],

    // Previous frame depth history buffer
    device const float* depth_history [[buffer(3)]],

    // Output depth buffer
    device float* depth_output [[buffer(4)]]
)
{
    uint probeIdx = global_id;
    if (probeIdx >= volume.ProbeCountTotal) return;

    // -----------------------------------------------------------------------
    // Accumulate per-octant depth from all rays
    // -----------------------------------------------------------------------

    float depthPerOctant[8];
    for (uint o = 0; o < 8; ++o) {
        depthPerOctant[o] = volume.RayMaxDistance; // Initialize to max
    }

    for (uint r = 0; r < volume.RaysPerProbe; ++r) {
        DDGIRayData ray = ray_buffer[probeIdx * volume.RaysPerProbe + r];

        // Skip miss rays (negative distance)
        if (ray.radiance_and_dist.w < 0.0f) continue;

        // Compute ray direction for octant classification
        float3 rayDir = ddgiRayDirection(r, volume.RaysPerProbe, volume.FrameIndex);

        // Octant classification: 3 bits from sign of x, y, z
        uint octant = 0u;
        if (rayDir.x > 0.0f) octant |= 1u;
        if (rayDir.y > 0.0f) octant |= 2u;
        if (rayDir.z > 0.0f) octant |= 4u;

        // Keep minimum distance per octant (closest surface)
        depthPerOctant[octant] = min(depthPerOctant[octant], ray.radiance_and_dist.w);
    }

    // -----------------------------------------------------------------------
    // Temporal filter with history and write output
    // -----------------------------------------------------------------------

    // Smooth alpha ramp for depth (same pattern as irradiance)
    float rampFrames = 60.0f;
    float t = saturate(float(max(volume.FrameIndex, 1u) - 1u) / rampFrames);
    float alpha = mix(1.0f, volume.DepthBlurSigma, t);

    for (uint o = 0; o < 8; ++o) {
        uint idx = probeIdx * 8 + o;
        float history = depth_history[idx];
        // Exponential moving average
        depth_output[idx] = mix(history, depthPerOctant[o], alpha);
    }
}
