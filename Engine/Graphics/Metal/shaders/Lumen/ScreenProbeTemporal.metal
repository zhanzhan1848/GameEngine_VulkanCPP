/**
 * @file ScreenProbeTemporal.metal
 * @brief Screen Probe GI - Temporal Accumulation at probe level
 *
 * Blends current-frame probe radiance with history using exponential moving
 * average with disocclusion rejection. Runs after ScreenProbeAverage and
 * before ScreenProbeGather.
 *
 * Each thread processes one probe: reads current avg radiance, history avg
 * radiance, and both positions for disocclusion detection. Writes blended
 * result back to the current avg radiance buffer.
 *
 * Effective samples: 64 / alpha ≈ 640 at alpha = 0.1
 *
 * Dispatch: (totalProbes + 63) / 64, 1, 1
 * ThreadGroupSize: (64, 1, 1)
 */

#include <metal_stdlib>
using namespace metal;

struct TemporalConstants {
    uint  totalProbes;
    float blendAlpha;      // 0.1 = 90% history, 10% current
    float posThreshold;    // World-space distance threshold for disocclusion
    uint  pad;
};

kernel void screen_probe_temporal(
    device const float4* currentAvgRadiance  [[buffer(0)]],  // from Average pass
    device const float4* historyAvgRadiance  [[buffer(1)]],  // previous frame's temporal output
    device const float4* currentPositions    [[buffer(2)]],  // current probe positions
    device const float4* historyPositions    [[buffer(3)]],  // previous frame probe positions
    device float4*       outputRadiance      [[buffer(4)]],  // blended output
    constant TemporalConstants& params       [[buffer(5)]],
    uint global_id [[thread_position_in_grid]])
{
    if (global_id >= params.totalProbes) return;

    float4 current = currentAvgRadiance[global_id];
    float4 history = historyAvgRadiance[global_id];

    // If current probe is inactive, output zero and skip
    float4 curPos = currentPositions[global_id];
    if (curPos.w <= 0.0f) {
        outputRadiance[global_id] = float4(0.0f);
        return;
    }

    // If current radiance is invalid (all rays missed), output zero
    if (current.w <= 0.0f) {
        outputRadiance[global_id] = float4(0.0f);
        return;
    }

    // If history is invalid (probe was inactive or all-miss last frame),
    // use current value directly (no history to blend with)
    if (history.w <= 0.0f) {
        outputRadiance[global_id] = current;
        return;
    }

    // Disocclusion check: world-space distance between probe positions
    // at the same grid cell across frames. Large distance = camera moved
    // significantly or scene changed at this location.
    float4 histPos = historyPositions[global_id];
    if (histPos.w > 0.0f) {
        float posDist = distance(curPos.xyz, histPos.xyz);
        if (posDist > params.posThreshold) {
            // Disocclusion: reset to current value
            outputRadiance[global_id] = current;
            return;
        }
    }

    // Exponential blend: 90% history, 10% current
    float3 blended = mix(current.rgb, history.rgb, 1.0f - params.blendAlpha);
    outputRadiance[global_id] = float4(blended, current.w);
}
