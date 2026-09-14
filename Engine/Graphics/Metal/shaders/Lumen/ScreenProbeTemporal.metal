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
    float maxGradient;      // reserved legacy field (kept for constant-buffer layout)
    uint  gridW;            // T5.2: probe grid dims for prev-cell lookup
    uint  gridH;
    float mode;             // F3 (M7): 0=EMA (default), 1=EMAVarianceClamp, 2=Reprojection
    float _pad0;
};

// F3 (M7): neighborhood variance clamp — byte-equivalent to
// TemporalLib.glsl::varianceClampAABB (Metal SSGITemporal.metal lineage).
static inline float3 varianceClampAABB(float3 history, float3 mean, float3 sigma,
                                       float scale) {
    float3 sigmaFloor = max(abs(mean) * 0.1f, float3(0.01f));
    float3 aabbMin = mean - sigma * scale - sigmaFloor;
    float3 aabbMax = mean + sigma * scale + sigmaFloor;
    return clamp(history, aabbMin, aabbMax);
}

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
                    // C7: a zero normal (never-written probe) must not
                    // normalize() to NaN and fail the gate for valid neighbours.
                    if (hNorm.w > 0.0f && curNorm.w > 0.0f &&
                        dot(curNorm.xyz, curNorm.xyz) > 1e-8f && dot(hNorm.xyz, hNorm.xyz) > 1e-8f &&
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
    // F3 (M7) EMAVarianceClamp (mode 1): clamp the reprojected history L0
    // into the current frame's neighborhood statistics AABB, then shrink
    // the L1 bands by the same per-channel factor (sign preserved).
    // Stats over self + horizontal neighbors' L0: 2 extra reads — budget
    // 31/32 worst case.
    // =================================================================
    if (params.mode > 0.5f && params.mode < 1.5f) {
        float3 sum = curSH0.rgb;
        float3 sumSq = curSH0.rgb * curSH0.rgb;
        float n = 1.0f;
        uint gx = global_id % params.gridW;
        if (gx > 0u) {
            float4 v = currentSH[(global_id - 1u) * 4u];
            if (v.w > 0.0f) { sum += v.rgb; sumSq += v.rgb * v.rgb; n += 1.0f; }
        }
        if (gx + 1u < params.gridW) {
            float4 v = currentSH[(global_id + 1u) * 4u];
            if (v.w > 0.0f) { sum += v.rgb; sumSq += v.rgb * v.rgb; n += 1.0f; }
        }
        float3 mean = sum / n;
        float3 sigma = sqrt(max(sumSq / n - mean * mean, float3(0.0f)));
        float3 cl0 = varianceClampAABB(histSH[0].rgb, mean, sigma, params.clampScale);
        float3 shrink = clamp(abs(cl0) / max(abs(histSH[0].rgb), float3(1e-4f)),
                              float3(0.0f), float3(1.0f));
        histSH[0].rgb = cl0;
        histSH[1].rgb *= shrink;
        histSH[2].rgb *= shrink;
        histSH[3].rgb *= shrink;
    }

    // History already passed position/normal disocclusion checks. Monte Carlo
    // sample variation is not a lighting change: increasing alpha with a
    // single-frame SH difference selects noisy bright samples and kills EMA.
    // Mode 1 retains the neighborhood variance clamp above; reset uses alpha=1.
    float alpha = clamp(params.baseAlpha, 0.0f, 1.0f);
    for (int b = 0; b < 4; b++) {
        float4 cur = currentSH[base + b];
        float3 blended = mix(histSH[b].rgb, cur.rgb, alpha);
        outputSH[base + b] = (b == 0) ? float4(blended, probeConfidence)
            : float4(blended, cur.w);
    }
}
