/**
 * @file DDGIUpdateDepth.metal
 * @brief Lumen DDGI Phase 2 - Octahedral depth update + temporal filtering
 *
 * Each thread handles one probe: reads all rays, classifies hit distances
 * into 8x8 octahedral texels (64 bins), computes mean + variance per texel,
 * applies temporal EMA, and writes to depth storage buffer.
 *
 * Buffer layout per probe: 128 floats
 *   Indices [0..63]:   mean distance per octahedral texel
 *   Indices [64..127]: variance per octahedral texel
 * Index: probeIdx * 128 + texelIdx (mean) / probeIdx * 128 + 64 + texelIdx (variance)
 *
 * Dispatch: (ProbeUpdateCount, 1, 1), threadGroupSize = (1, 1, 1)
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
    device float* depth_output [[buffer(4)]],

    // Probe update list (sparse indices of probes to update this frame)
    device const uint* probeUpdateList [[buffer(5)]]
)
{
    if (global_id >= volume.ProbeUpdateCount) return;

    uint probeIdx = probeUpdateList[global_id];

    // -----------------------------------------------------------------------
    // Accumulate per-texel mean + variance from all rays (octahedral binning)
    // -----------------------------------------------------------------------

    constexpr uint texels = DDGI_DEPTH_TEXELS;
    float sumDist[texels];
    float sumDistSq[texels];
    uint rayCount[texels];
    for (uint t = 0; t < texels; ++t) {
        sumDist[t] = 0.0f;
        sumDistSq[t] = 0.0f;
        rayCount[t] = 0u;
    }

    constexpr uint res = DDGI_DEPTH_RES;

    for (uint r = 0; r < volume.RaysPerProbe; ++r) {
        DDGIRayData ray = ray_buffer[probeIdx * volume.RaysPerProbe + r];

        // Skip miss rays (negative distance)
        if (ray.radiance_and_dist.w < 0.0f) continue;

        // Compute ray direction and map to octahedral texel
        float3 rayDir = ddgiRayDirection(r, volume.RaysPerProbe, volume.FrameIndex);
        float2 uv = octahedralEncode(rayDir);
        uint2 texel = uint2(clamp(uint(uv.x * float(res)), 0u, res - 1u),
                            clamp(uint(uv.y * float(res)), 0u, res - 1u));
        uint texelIdx = texel.y * res + texel.x;

        float dist = ray.radiance_and_dist.w;
        sumDist[texelIdx] += dist;
        sumDistSq[texelIdx] += dist * dist;
        rayCount[texelIdx]++;
    }

    // -----------------------------------------------------------------------
    // Temporal filter with history and write output
    // -----------------------------------------------------------------------

    constexpr uint floatsPerProbe = texels * 2u; // 128
    uint probeBase = probeIdx * floatsPerProbe;

    // Smooth alpha ramp for depth
    float rampFrames = 60.0f;
    float t = saturate(float(max(volume.FrameIndex, 1u) - 1u) / rampFrames);
    float alpha = mix(1.0f, volume.DepthBlurSigma, t);

    for (uint texelIdx = 0; texelIdx < texels; ++texelIdx) {
        uint meanIdx = probeBase + texelIdx;
        uint varIdx  = probeBase + texels + texelIdx;

        // No rays hit in this texel: keep history unchanged
        if (rayCount[texelIdx] == 0u) {
            depth_output[meanIdx] = depth_history[meanIdx];
            depth_output[varIdx]  = depth_history[varIdx];
            continue;
        }

        float mean     = sumDist[texelIdx] / float(rayCount[texelIdx]);
        float variance = abs(sumDistSq[texelIdx] / float(rayCount[texelIdx]) - mean * mean);

        // Few samples: inflate variance to avoid overconfidence
        if (rayCount[texelIdx] < 4u) {
            variance = max(variance, volume.ProbeSpacing * volume.ProbeSpacing * 0.1f);
        }
        variance = max(variance, 0.001f);

        // Exponential moving average
        depth_output[meanIdx] = mix(depth_history[meanIdx], mean, alpha);
        depth_output[varIdx]  = mix(depth_history[varIdx],  variance, alpha);
    }
}
