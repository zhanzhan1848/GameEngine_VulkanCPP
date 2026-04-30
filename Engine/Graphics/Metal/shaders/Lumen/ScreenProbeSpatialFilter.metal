/**
 * @file ScreenProbeSpatialFilter.metal
 * @brief Screen Probe GI - Bilateral spatial filter on probe SH coefficients
 *
 * Cross-pattern 5-probe bilateral blur: center + 4 cardinal neighbors.
 * Smooths probe grid boundaries while preserving edges at depth discontinuities.
 *
 * Buffer strategy:
 *   Reads from inputSH (temporal output at frameIdx)
 *   Writes to outputSH (repurposed histIdx buffer)
 *   Gather subsequently reads from outputSH
 *
 * Per-thread reads: 5 positions + 5×4 SH = 25 (within Apple Silicon ~32 limit)
 *
 * Dispatch: (totalProbes + 63) / 64, 1, 1
 * ThreadGroupSize: (64, 1, 1)
 */

#include <metal_stdlib>
using namespace metal;

struct SpatialConstants {
    uint  totalProbes;
    uint  gridW;
    float sigma;       // bilateral spatial sigma (world-space)
    float pad;
};

kernel void screen_probe_spatial_filter(
    // Input SH coefficients (4 float4 per probe, interleaved)
    device const float4* inputSH      [[buffer(0)]],

    // Output filtered SH (4 float4 per probe, interleaved)
    device float4*       outputSH     [[buffer(1)]],

    // Probe positions for bilateral weighting
    device const float4* probePos     [[buffer(2)]],

    // Constants
    constant SpatialConstants& params [[buffer(3)]],

    uint global_id [[thread_position_in_grid]])
{
    if (global_id >= params.totalProbes) return;

    uint gx = global_id % params.gridW;
    uint gy = global_id / params.gridW;

    // Read center probe position
    float4 centerPosData = probePos[global_id];

    // If center probe is inactive, output zeros
    if (centerPosData.w <= 0.0f) {
        uint base = global_id * 4;
        outputSH[base + 0] = float4(0.0f);
        outputSH[base + 1] = float4(0.0f);
        outputSH[base + 2] = float4(0.0f);
        outputSH[base + 3] = float4(0.0f);
        return;
    }

    // Read center SH
    uint centerBase = global_id * 4;
    float3 centerSH[4];
    for (int b = 0; b < 4; b++) {
        centerSH[b] = inputSH[centerBase + b].rgb;
    }

    // Check if center SH is valid
    if (inputSH[centerBase + 0].w <= 0.0f) {
        // Invalid center: output zeros
        outputSH[centerBase + 0] = float4(0.0f);
        outputSH[centerBase + 1] = float4(0.0f);
        outputSH[centerBase + 2] = float4(0.0f);
        outputSH[centerBase + 3] = float4(0.0f);
        return;
    }

    float3 centerPos = centerPosData.xyz;
    float invTwoSigmaSq = 1.0f / (2.0f * params.sigma * params.sigma);

    // Cardinal neighbor offsets (cross pattern)
    int2 offsets[4] = {
        int2(-1, 0),  // left
        int2( 1, 0),  // right
        int2( 0,-1),  // up
        int2( 0, 1),  // down
    };

    // Accumulate filtered SH + confidence
    // Center-weighted: center weight = 2.0 (strong self-preservation, light neighbor influence)
    float3 filteredSH[4] = { centerSH[0] * 2.0f, centerSH[1] * 2.0f, centerSH[2] * 2.0f, centerSH[3] * 2.0f };
    float totalWeight = 2.0f;  // center weight = 2.0
    float centerConf = inputSH[centerBase + 0].w;
    float filteredConf = centerConf * 2.0f;  // center weight = 2.0

    for (int n = 0; n < 4; n++) {
        int nx = int(gx) + offsets[n].x;
        int ny = int(gy) + offsets[n].y;

        // Bounds check
        if (nx < 0 || nx >= int(params.gridW) || ny < 0) continue;
        if (uint(ny) * params.gridW + uint(nx) >= params.totalProbes) continue;

        uint nidx = uint(ny) * params.gridW + uint(nx);

        // Check neighbor active
        float4 neighborPosData = probePos[nidx];
        if (neighborPosData.w <= 0.0f) continue;

        // Check neighbor SH valid
        uint neighborBase = nidx * 4;
        if (inputSH[neighborBase + 0].w <= 0.0f) continue;

        // Bilateral weight based on world-space distance
        float dist2 = distance_squared(centerPos, neighborPosData.xyz);
        float w = exp(-dist2 * invTwoSigmaSq);

        // Read and accumulate neighbor SH
        for (int b = 0; b < 4; b++) {
            filteredSH[b] += inputSH[neighborBase + b].rgb * w;
        }
        // Accumulate confidence from neighbor
        filteredConf += inputSH[neighborBase + 0].w * w;
        totalWeight += w;
    }

    // Normalize and write output
    float invWeight = 1.0f / totalWeight;
    uint outBase = global_id * 4;
    outputSH[outBase + 0] = float4(filteredSH[0] * invWeight, filteredConf * invWeight);
    outputSH[outBase + 1] = float4(filteredSH[1] * invWeight, 1.0f);
    outputSH[outBase + 2] = float4(filteredSH[2] * invWeight, 1.0f);
    outputSH[outBase + 3] = float4(filteredSH[3] * invWeight, 1.0f);
}
