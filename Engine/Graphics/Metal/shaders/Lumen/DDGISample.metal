/**
 * @file DDGISample.metal
 * @brief DDGI probe sampling: 4-probe tetrahedral interpolation with SH3
 *
 * Uses storage buffer (not texture3D) for irradiance data.
 * Each probe stores 9 float3 SH3 coefficients.
 * Tetrahedral interpolation reads 4 probes (vs 8 for trilinear).
 */

#include "DDGIVolumeData.metal"

// Tetrahedral interpolation: find 4 probes and barycentric weights
static void tetrahedralProbes(
    float3 gridPos,
    uint3 gridDim,
    thread uint* probeIndices,
    thread float* baryWeights)
{
    uint3 base = clamp(uint3(floor(gridPos)), uint3(0u), gridDim - 1u);
    uint3 base1 = min(base + 1u, gridDim - 1u);

    uint probes[8];
    probes[0] = base.x  + base.y  * gridDim.x + base.z  * gridDim.x * gridDim.y;
    probes[1] = base1.x + base.y  * gridDim.x + base.z  * gridDim.x * gridDim.y;
    probes[2] = base.x  + base1.y * gridDim.x + base.z  * gridDim.x * gridDim.y;
    probes[3] = base1.x + base1.y * gridDim.x + base.z  * gridDim.x * gridDim.y;
    probes[4] = base.x  + base.y  * gridDim.x + base1.z * gridDim.x * gridDim.y;
    probes[5] = base1.x + base.y  * gridDim.x + base1.z * gridDim.x * gridDim.y;
    probes[6] = base.x  + base1.y * gridDim.x + base1.z * gridDim.x * gridDim.y;
    probes[7] = base1.x + base1.y * gridDim.x + base1.z * gridDim.x * gridDim.y;

    float fx = fract(gridPos.x), fy = fract(gridPos.y), fz = fract(gridPos.z);

    // 6 tetrahedra split by main diagonal
    if (fx >= fy && fy >= fz) {
        probeIndices[0]=probes[0]; probeIndices[1]=probes[1]; probeIndices[2]=probes[3]; probeIndices[3]=probes[7];
        baryWeights[0]=1.0f-fx; baryWeights[1]=fx-fy; baryWeights[2]=fy-fz; baryWeights[3]=fz;
    } else if (fx >= fz && fz >= fy) {
        probeIndices[0]=probes[0]; probeIndices[1]=probes[1]; probeIndices[2]=probes[5]; probeIndices[3]=probes[7];
        baryWeights[0]=1.0f-fx; baryWeights[1]=fx-fz; baryWeights[2]=fz-fy; baryWeights[3]=fy;
    } else if (fy >= fx && fx >= fz) {
        probeIndices[0]=probes[0]; probeIndices[1]=probes[2]; probeIndices[2]=probes[3]; probeIndices[3]=probes[7];
        baryWeights[0]=1.0f-fy; baryWeights[1]=fy-fx; baryWeights[2]=fx-fz; baryWeights[3]=fz;
    } else if (fy >= fz && fz >= fx) {
        probeIndices[0]=probes[0]; probeIndices[1]=probes[2]; probeIndices[2]=probes[6]; probeIndices[3]=probes[7];
        baryWeights[0]=1.0f-fy; baryWeights[1]=fy-fz; baryWeights[2]=fz-fx; baryWeights[3]=fx;
    } else if (fz >= fx && fx >= fy) {
        probeIndices[0]=probes[0]; probeIndices[1]=probes[4]; probeIndices[2]=probes[5]; probeIndices[3]=probes[7];
        baryWeights[0]=1.0f-fz; baryWeights[1]=fz-fx; baryWeights[2]=fx-fy; baryWeights[3]=fy;
    } else { // fz >= fy && fy >= fx
        probeIndices[0]=probes[0]; probeIndices[1]=probes[4]; probeIndices[2]=probes[6]; probeIndices[3]=probes[7];
        baryWeights[0]=1.0f-fz; baryWeights[1]=fz-fy; baryWeights[2]=fy-fx; baryWeights[3]=fx;
    }

    for (uint i = 0; i < 4u; ++i) baryWeights[i] = max(baryWeights[i], 0.0f);
}

// Visibility weighting: Chebyshev upper bound test using mean + variance depth.
// Prevents irradiance leaking through thin geometry with probabilistic occlusion.
static float ddgiVisibilityWeight(float dist, float mean, float variance, float spacing) {
    if (dist <= mean) return 1.0f;
    float d_minus_mean = dist - mean;
    float chebyshev = variance / (variance + d_minus_mean * d_minus_mean);
    float bias = spacing * 0.3f;
    float threshold = mean + bias + chebyshev * spacing;
    float falloff = spacing * 0.5f;
    return saturate((threshold - dist + falloff) / falloff);
}

// Sample DDGI irradiance at world position for given normal
static float3 ddgiSampleIrradiance(
    float3 worldPos,
    float3 normal,
    constant DDGIVolumeData& vol,
    device const float3* irradianceBuffer,
    device const float* ddgiDepthBuffer)
{
    float3 gridPos = (worldPos - vol.ProbeOrigin.xyz) / vol.ProbeSpacing;
    uint3 counts = ddgiGetProbeCounts(vol);
    float3 gridMax = float3(float(counts.x - 1u),
                            float(counts.y - 1u),
                            float(counts.z - 1u));

    if (any(gridPos < 0.0f) || any(gridPos > gridMax)) return float3(0.0f);

    uint probeIdx[4];
    float baryW[4];
    tetrahedralProbes(gridPos, counts, probeIdx, baryW);

    float3 n = normalize(normal);
    float3 result = float3(0.0f);

    // Normal bias to prevent self-shadowing acne (small to avoid thin wall penetration)
    float3 biasedPos = worldPos + n * vol.ProbeSpacing * 0.05f;

    float totalWeight = 0.0f;

    for (uint p = 0; p < 4u; ++p) {
        if (baryW[p] < 0.001f) continue;

        // Compute probe world position for visibility check
        uint3 gc = ddgiProbeGridCoord(probeIdx[p], counts);
        float3 probePos = ddgiProbeWorldPos(gc, vol.ProbeOrigin.xyz, vol.ProbeSpacing);

        uint base = probeIdx[p] * 9u;
        float3 sh[4];
        for (uint i = 0; i < 4u; ++i) {
            sh[i] = irradianceBuffer[base + i];
        }

        // Direction FROM probe TO biased shading point
        float3 fromProbeDir = normalize(biasedPos - probePos);
        float distToProbe = length(biasedPos - probePos);

        // Octahedral depth sampling with bilinear interpolation (64 directions)
        float2 depthMV = sampleDepthOctahedral(ddgiDepthBuffer, probeIdx[p], fromProbeDir);
        float visWeight = ddgiVisibilityWeight(distToProbe, depthMV.x, depthMV.y, vol.ProbeSpacing);

        float3 irradiance = shDot(sh, n);
        float w = baryW[p] * visWeight;
        result += irradiance * w;
        totalWeight += w;
    }

    if (totalWeight > 0.0f) {
        result /= totalWeight;
    }

    return max(result, float3(0.0f));
}
