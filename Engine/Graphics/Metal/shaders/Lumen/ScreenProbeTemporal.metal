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
    float4x4 prev_view_projection;  // T5.2: previous frame VP for reprojection
    uint  totalProbes;
    float baseAlpha;        // base blend alpha (e.g. 0.1)
    float posThreshold;     // world-space distance threshold for disocclusion
    float normalThreshold;  // normal dot-product threshold (reject if below)
    float clampScale;       // history clamp range relative to current
    float maxGradient;      // maximum SH change per frame (gradient limiter)
    uint  gridW;            // T5.2: probe grid dims for prev-cell lookup
    uint  gridH;
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
    // T5.2 velocity reprojection: find this probe's history in the
    // PREVIOUS frame's screen grid instead of the same cell. Neighbors
    // pass the same world-position/normal disocclusion tests inline.
    // =================================================================
    float4 histSH[4];
    float histW = 0.0f;
    bool historyValid = false;
    {
        float4 clipP = params.prev_view_projection * float4(curPos.xyz, 1.0f);
        if (clipP.w > 0.0f) {
            float2 ndc = clipP.xy / clipP.w;
            float2 prevUV = float2(ndc.x * 0.5f + 0.5f, 1.0f - (ndc.y * 0.5f + 0.5f));
            if (all(prevUV >= float2(0.0f)) && all(prevUV <= float2(1.0f))) {
                float2 cellF = prevUV * float2(params.gridW, params.gridH) - 0.5f;
                int2 c0 = int2(floor(cellF));
                float2 fr = fract(cellF);
                float4 curNorm = currentNormals[global_id];
                for (int b = 0; b < 4; ++b) histSH[b] = float4(0.0f);

                for (int dy = 0; dy <= 1; ++dy)
                for (int dx = 0; dx <= 1; ++dx) {
                    int2 c = c0 + int2(dx, dy);
                    if (c.x < 0 || c.y < 0 || c.x >= int(params.gridW) || c.y >= int(params.gridH)) continue;
                    uint hIdx = uint(c.y) * params.gridW + uint(c.x);
                    float4 hPos = historyPositions[hIdx];
                    if (hPos.w <= 0.0f) continue;
                    if (distance(curPos.xyz, hPos.xyz) > params.posThreshold) continue;
                    float4 hNorm = historyNormals[hIdx];
                    if (hNorm.w > 0.0f && curNorm.w > 0.0f &&
                        dot(normalize(curNorm.xyz), normalize(hNorm.xyz)) < params.normalThreshold) continue;
                    uint hb = hIdx * 4u;
                    if (historySH[hb].w <= 0.0f) continue;
                    float w = ((dx == 0) ? (1.0f - fr.x) : fr.x)
                            * ((dy == 0) ? (1.0f - fr.y) : fr.y);
                    for (int b = 0; b < 4; ++b)
                        histSH[b] += historySH[hb + b] * w;
                    histW += w;
                }
                if (histW > 0.001f) {
                    historyValid = true;
                    for (int b = 0; b < 4; ++b) histSH[b] /= histW;
                }
            }
        }
    }

    // No valid reprojected history → use current directly (no blending)
    if (!historyValid) {
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
    float3 diff0 = curSH0.rgb - histSH[0].rgb;
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
        float4 hist = histSH[b];

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
