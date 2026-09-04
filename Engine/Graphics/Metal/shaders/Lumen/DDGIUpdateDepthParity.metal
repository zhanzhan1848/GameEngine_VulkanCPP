/**
 * @file DDGIUpdateDepthParity.metal
 * @brief T8.13 parity track — faithful Metal port of DDGIUpdateDepth.comp
 *
 * Selected at runtime when PRIMAL_DDGI_PARITY=1 (LumenDDGIPass
 * LoadShaderBytecode prefers <name>Parity.metal, falling back to <name>.metal
 * per stage). Purpose: A/B the Vulkan probe-depth math on Metal against the
 * engine's DDGIUpdateDepth.metal WITHOUT touching either default.
 *
 * Faithful to the comp (differences vs the engine file are all deliberate):
 *   - bins rays with ddgiSampleRayDirection(r, N, FrameIndex, RaySampleMode,
 *     toLight) — the SAME sampler the trace side and UpdateIrradiance use.
 *     The engine file still bins with the legacy ddgiRayDirection (mode 0
 *     only), so under any non-uniform RaySampleMode its texel mapping
 *     desynchronizes from the traced directions (exactly the failure the comp
 *     header warns about). Mode 0 makes the two identical.
 *   - few-sample variance floor ProbeSpacing² × 0.1 (comp) vs × 0.02 (engine)
 *   - serial per-probe projection in lane (0,0) of the one-group-per-probe
 *     dispatch; the comp assigns one probe per invocation
 * Unchanged: miss-ray skip, octahedral binning, mean/variance math, min
 * variance 0.001, no-hit texels keep history, 60-frame alpha ramp towards
 * DepthBlurSigma, EMA blend.
 */

#include <metal_stdlib>
using namespace metal;

#include "../CommonTypes.metal"
#include "DDGIVolumeData.metal"

kernel void ddgi_update_depth(
    uint2 tid2d [[thread_position_in_threadgroup]],
    uint2 gid2d [[threadgroup_position_in_grid]],

    constant GlobalShaderData& gd [[buffer(0)]],
    constant DDGIVolumeData& volume [[buffer(1)]],
    device const DDGIRayData* ray_buffer [[buffer(2)]],
    device const float* depth_history [[buffer(3)]],
    device float* depth_output [[buffer(4)]],
    device const uint* probe_update_list [[buffer(5)]]
)
{
    // comp: one probe per invocation; the Metal module dispatches one (8,8,1)
    // group per probe, so lane (0,0) runs the comp body and the rest exit.
    if (!(tid2d.x == 0u && tid2d.y == 0u)) return;

    uint group_id = gid2d.x;
    if (group_id >= volume.ProbeUpdateCount) return;

    uint probeIdx = probe_update_list[group_id];
    if (probeIdx >= volume.ProbeCountTotal) return;

    // comp 153-163: per-texel accumulators
    constexpr uint texels = 64u;
    constexpr uint res = 8u;
    float sumDist[texels];
    float sumDistSq[texels];
    uint  rayCountArr[texels];
    for (uint t = 0u; t < texels; t++) {
        sumDist[t] = 0.0f;
        sumDistSq[t] = 0.0f;
        rayCountArr[t] = 0u;
    }

    // comp 165-188: bin all hit rays by their traced direction
    for (uint r = 0u; r < volume.RaysPerProbe; r++) {
        DDGIRayData ray = ray_buffer[probeIdx * volume.RaysPerProbe + r];

        // Skip miss rays (negative distance)
        if (ray.radiance_and_dist.w < 0.0f) {
            continue;
        }

        float3 rayDir = ddgiSampleRayDirection(r, volume.RaysPerProbe,
                                               volume.FrameIndex,
                                               uint(volume.RaySampleMode),
                                               normalize(-volume.LightDirection.xyz)).xyz;
        float2 uv = octahedralEncode(rayDir);
        uint2 texel = uint2(clamp(uint(uv.x * float(res)), 0u, res - 1u),
                            clamp(uint(uv.y * float(res)), 0u, res - 1u));
        uint texelIdx = texel.y * res + texel.x;

        float dist = ray.radiance_and_dist.w;
        sumDist[texelIdx] = sumDist[texelIdx] + dist;
        sumDistSq[texelIdx] = sumDistSq[texelIdx] + dist * dist;
        rayCountArr[texelIdx] = rayCountArr[texelIdx] + 1u;
    }

    // comp 194-225: temporal filter with history and write output
    uint floatsPerProbe = texels * 2u;  // 128
    uint probeBase = probeIdx * floatsPerProbe;

    // Smooth alpha ramp for depth
    const float rampFrames = 60.0f;
    float t = clamp(float(max(volume.FrameIndex, 1u) - 1u) / rampFrames, 0.0f, 1.0f);
    float alpha = mix(1.0f, volume.DepthBlurSigma, t);

    for (uint texelIdx = 0u; texelIdx < texels; texelIdx++) {
        uint meanIdx = probeBase + texelIdx;
        uint varIdx  = probeBase + texels + texelIdx;

        // No rays hit in this texel: keep history unchanged
        if (rayCountArr[texelIdx] == 0u) {
            depth_output[meanIdx] = depth_history[meanIdx];
            depth_output[varIdx]  = depth_history[varIdx];
            continue;
        }

        float mean = sumDist[texelIdx] / float(rayCountArr[texelIdx]);
        float variance = abs(sumDistSq[texelIdx] / float(rayCountArr[texelIdx]) - mean * mean);

        // Few samples: inflate variance to avoid overconfidence
        if (rayCountArr[texelIdx] < 4u) {
            variance = max(variance, volume.ProbeSpacing * volume.ProbeSpacing * 0.1f);
        }
        variance = max(variance, 0.001f);

        // Exponential moving average
        depth_output[meanIdx] = mix(depth_history[meanIdx], mean, alpha);
        depth_output[varIdx]  = mix(depth_history[varIdx], variance, alpha);
    }

    (void)gd;
}
