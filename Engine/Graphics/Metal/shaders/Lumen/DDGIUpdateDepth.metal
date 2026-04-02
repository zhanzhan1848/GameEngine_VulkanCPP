/**
 * @file DDGIUpdateDepth.metal
 * @brief Lumen DDGI Phase 2 - 8-direction depth update + temporal filtering
 *
 * Each thread handles one probe: reads all rays, classifies hit distances
 * into 8 octants (keeps closest per octant), applies temporal EMA, and
 * writes 4 texels to depth Texture3D.
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

    // Previous frame depth (history)
    texture3d<float, access::read> depth_history [[texture(0)]],

    // Output depth
    texture3d<float, access::write> depth_output [[texture(1)]],

    // Global shader data (matches C++ descriptor layout at buffer(0))
    constant GlobalShaderData& gd [[buffer(0)]],

    // DDGI volume data
    constant DDGIVolumeData& volume [[buffer(1)]],

    // Ray data from trace pass
    device const DDGIRayData* ray_buffer [[buffer(2)]]
)
{
    uint probeIdx = global_id;
    if (probeIdx >= volume.ProbeCountTotal) return;

    // Probe grid coordinates
    uint3 gc = ddgiProbeGridCoord(probeIdx, volume.ProbeCounts);

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
        float3 rayDir = ddgiFibonacciSphereDir(r, volume.RaysPerProbe, volume.FrameIndex);

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

    float alpha = volume.DepthBlurSigma;

    // Write 4 texels: each RG16F stores 2 depth values = 8 total
    for (uint i = 0; i < 4; ++i) {
        uint3 coord = uint3(gc.x, gc.y, gc.z * 4 + i);

        // Read history
        float2 history = float2(volume.RayMaxDistance, volume.RayMaxDistance);
        if (coord.z < depth_history.get_depth()) {
            history = depth_history.read(coord).rg;
        }

        // Current depths for this texel's two octants
        float2 currentDepths = float2(depthPerOctant[i * 2], depthPerOctant[i * 2 + 1]);

        // Exponential moving average
        float2 filtered = mix(history, currentDepths, alpha);

        depth_output.write(float4(filtered, 0.0f, 0.0f), coord);
    }
}
