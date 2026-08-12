/**
 * @file ProbeConfidence.metal
 * @brief Per-probe confidence metrics for adaptive temporal filtering.
 *
 * Confidence drives temporal alpha: low confidence probes update faster
 * (higher alpha), high confidence probes blend more slowly (lower alpha).
 * This prevents stale probes from persisting and reduces temporal lag
 * in dynamic scenes.
 *
 * Layout: 4 floats per probe in a flat storage buffer.
 *   [0] rayHitRatio      — fraction of rays that hit geometry
 *   [1] temporalStability — 1 - L1diff/max(L1hist,L1curr)
 *   [2] visibilityConf   — visibility confidence (externally set)
 *   [3] convergenceAge   — normalized age toward 1.0
 */

#pragma once
#include <metal_stdlib>
using namespace metal;

struct ProbeConfidence {
    float rayHitRatio;
    float temporalStability;
    float visibilityConf;
    float convergenceAge;
};

static float evaluateConfidence(float weights[4],
                                 threadgroup ProbeConfidence conf[4]) {
    float c = 0.0f;
    float totalW = 0.0f;
    for (uint i = 0; i < 4; i++) {
        float w = weights[i];
        float pc = conf[i].rayHitRatio;
        pc *= smoothstep(0.0f, 0.5f, conf[i].visibilityConf);
        pc *= smoothstep(0.0f, 0.3f, conf[i].temporalStability);
        pc *= saturate(conf[i].convergenceAge);
        c += w * pc;
        totalW += w;
    }
    return saturate(c / max(totalW, 0.001f));
}

static float computeTemporalAlpha(float baseAlpha, float confidence, uint probeAge) {
    float alpha = baseAlpha;
    if (confidence < 0.5f)
        alpha = max(alpha, 0.5f);
    if (probeAge < 10u)
        alpha = mix(1.0f, alpha, float(probeAge) / 10.0f);
    return alpha;
}
