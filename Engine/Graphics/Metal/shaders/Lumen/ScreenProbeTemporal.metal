/**
 * @file ScreenProbeTemporal.metal
 * @brief Screen Probe GI - Change-penalizing temporal accumulation
 *
 * Core principle: penalize change, don't reward stability.
 *
 * - Base alpha = slow accumulation (0.1)
 * - High gradient (|current - history|) → faster adaptation (trust current)
 * - Low confidence → reset history (clear bad data)
 * - Disocclusion → reset history
 * - History clamping + gradient limiter prevent energy explosion
 *
 * This prevents the system from converging to a wrong stable solution.
 *
 * Dispatch: (totalProbes + 63) / 64, 1, 1
 * ThreadGroupSize: (64, 1, 1)
 */

#include <metal_stdlib>
using namespace metal;

struct TemporalConstants {
    uint  totalProbes;
    float baseAlpha;        // base blend alpha (e.g. 0.1)
    float posThreshold;     // world-space distance threshold for disocclusion
    float normalThreshold;  // normal dot-product threshold (reject if below)
    float clampScale;       // history clamp range relative to current
    float maxGradient;      // maximum SH change per frame (gradient limiter)
    uint  pad[2];
};

kernel void screen_probe_temporal(
    device const float4* currentSH       [[buffer(0)]],  // from Average pass (4 per probe)
    device const float4* historySH       [[buffer(1)]],  // previous frame's temporal output
    device const float4* currentPositions [[buffer(2)]],  // current probe positions
    device const float4* historyPositions [[buffer(3)]],  // previous frame probe positions
    device float4*       outputSH        [[buffer(4)]],  // blended output
    constant TemporalConstants& params   [[buffer(5)]],
    device const float4* currentNormals  [[buffer(6)]],  // current probe normals
    device const float4* historyNormals  [[buffer(7)]],  // history probe normals
    uint global_id [[thread_position_in_grid]])
{
    if (global_id >= params.totalProbes) return;

    // If current probe is inactive, zero output
    float4 curPos = currentPositions[global_id];
    if (curPos.w <= 0.0f) {
        uint base = global_id * 4;
        outputSH[base + 0] = float4(0.0f);
        outputSH[base + 1] = float4(0.0f);
        outputSH[base + 2] = float4(0.0f);
        outputSH[base + 3] = float4(0.0f);
        return;
    }

    uint base = global_id * 4;

    // Read current SH and probe confidence (multi-factor from Average)
    float4 curSH0 = currentSH[base + 0];
    float probeConfidence = curSH0.w;

    if (probeConfidence <= 0.0f) {
        // No valid rays → output zeros
        outputSH[base + 0] = float4(0.0f);
        outputSH[base + 1] = float4(0.0f);
        outputSH[base + 2] = float4(0.0f);
        outputSH[base + 3] = float4(0.0f);
        return;
    }

    // =================================================================
    // Check history validity
    // =================================================================
    float4 histSH0 = historySH[base + 0];
    bool historyValid = (histSH0.w > 0.0f);

    // If no valid history → use current directly (no blending)
    if (!historyValid) {
        for (int b = 0; b < 4; b++) {
            float4 cur = currentSH[base + b];
            outputSH[base + b] = (b == 0) ? float4(cur.rgb, probeConfidence)
                                          : float4(cur.rgb, cur.w);
        }
        return;
    }

    // =================================================================
    // Disocclusion check 1: world-space position distance
    // =================================================================
    bool disoccluded = false;
    float4 histPos = historyPositions[global_id];
    if (histPos.w > 0.0f) {
        float posDist = distance(curPos.xyz, histPos.xyz);
        if (posDist > params.posThreshold) {
            disoccluded = true;
        }
    } else {
        disoccluded = true;
    }

    // =================================================================
    // Disocclusion check 2: normal similarity
    // =================================================================
    if (!disoccluded) {
        float4 curNorm = currentNormals[global_id];
        float4 histNorm = historyNormals[global_id];
        if (curNorm.w > 0.0f && histNorm.w > 0.0f) {
            float normalSim = dot(normalize(curNorm.xyz), normalize(histNorm.xyz));
            if (normalSim < params.normalThreshold) {
                disoccluded = true;
            }
        }
    }

    // If disoccluded → reset to current (no history blending)
    if (disoccluded) {
        for (int b = 0; b < 4; b++) {
            float4 cur = currentSH[base + b];
            outputSH[base + b] = (b == 0) ? float4(cur.rgb, probeConfidence)
                                          : float4(cur.rgb, cur.w);
        }
        return;
    }

    // =================================================================
    // Low confidence → reset history (clear bad accumulated data)
    // =================================================================
    if (probeConfidence < 0.05f) {
        for (int b = 0; b < 4; b++) {
            float4 cur = currentSH[base + b];
            outputSH[base + b] = (b == 0) ? float4(cur.rgb, probeConfidence)
                                          : float4(cur.rgb, cur.w);
        }
        return;
    }

    // =================================================================
    // Compute per-probe gradient (how much SH changed)
    // =================================================================
    float3 diff0 = curSH0.rgb - histSH0.rgb;
    float gradient = length(diff0);

    // =================================================================
    // Change-penalizing blend:
    //   High gradient → high alpha → trust current (adapt to change)
    //   Low gradient → base alpha → slow accumulation
    // =================================================================
    float changeAlpha = saturate(gradient / params.maxGradient);
    float alpha = max(params.baseAlpha, changeAlpha);

    // =================================================================
    // Blend 4 SH coefficients with clamping + gradient limiter
    // =================================================================
    for (int b = 0; b < 4; b++) {
        float4 cur = currentSH[base + b];
        float4 hist = historySH[base + b];

        // Clamp history to prevent energy drift
        float3 curAbs = max(abs(cur.rgb), float3(0.1f));
        float3 lo = cur.rgb - params.clampScale * curAbs;
        float3 hi = cur.rgb + params.clampScale * curAbs;
        float3 clampedHist = clamp(hist.rgb, lo, hi);

        // Gradient limiter: cap per-frame change
        float3 diff = cur.rgb - clampedHist;
        float diffLen = length(diff);
        if (diffLen > params.maxGradient && diffLen > 0.001f) {
            cur.rgb = clampedHist + normalize(diff) * params.maxGradient;
        }

        // Blend
        float3 blended = mix(clampedHist, cur.rgb, alpha);
        outputSH[base + b] = (b == 0) ? float4(blended, probeConfidence)
                                      : float4(blended, cur.w);
    }
}
