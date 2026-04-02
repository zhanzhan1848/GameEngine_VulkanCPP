/**
 * @file DDGISample.metal
 * @brief DDGI probe sampling utilities (include file, not a kernel)
 *
 * Provides functions to:
 *  1. Trilinear-interpolate between 8 nearest probes
 *  2. Evaluate 2nd-order SH at a surface normal direction
 *  3. Apply depth-aware weighting to reduce light leaking
 *
 * Usage: #include "DDGISample.metal" in shaders that need DDGI GI.
 */

#ifndef DDGI_SAMPLE_METAL
#define DDGI_SAMPLE_METAL

#include <metal_stdlib>
using namespace metal;
#include "DDGIVolumeData.metal"

// ============================================================================
// SH Evaluation (2nd order, 4 coefficients)
// ============================================================================

static float3 ddgiEvalSH2(float3 N, float3 sh0, float3 sh1, float3 sh2, float3 sh3)
{
    float3 result = float3(0.0f);
    result += sh0 * DDGI_SH_C0;
    result += sh1 * (-DDGI_SH_C1 * N.y);
    result += sh2 * ( DDGI_SH_C1 * N.z);
    result += sh3 * ( DDGI_SH_C1 * N.x);
    return max(result, float3(0.0f));
}

// ============================================================================
// Sample a single probe's irradiance (read 4 SH texels)
// ============================================================================

static float3 ddgiSampleProbeIrradiance(
    uint3 probeCoord,
    float3 worldNormal,
    texture3d<float, access::sample> irradianceTexture,
    uint3 probeCounts)
{
    float3 sh[4];
    for (uint i = 0; i < DDGI_SH_COEFF_COUNT; ++i) {
        uint3 coord = uint3(probeCoord.x, probeCoord.y, probeCoord.z * DDGI_SH_COEFF_COUNT + i);
        sh[i] = irradianceTexture.read(coord).rgb;
    }

    return ddgiEvalSH2(worldNormal, sh[0], sh[1], sh[2], sh[3]);
}

// ============================================================================
// Depth-aware probe weight (reduces light leaking through walls)
// ============================================================================

static float ddgiProbeWeight(
    float3 surfaceWorldPos,
    float3 surfaceNormal,
    float3 probeWorldPos,
    texture3d<float, access::sample> depthTexture,
    float3 probeOrigin,
    float  probeSpacing,
    uint3  probeCounts)
{
    // Direction from surface to probe
    float3 toProbe = probeWorldPos - surfaceWorldPos;
    float dist = length(toProbe);
    float3 dir = toProbe / max(dist, 0.001f);

    // Backface check: probe behind surface gets less weight
    float NoL = dot(surfaceNormal, dir);
    float backfaceWeight = max(NoL + 1.0f, 0.0f) * 0.5f;

    // Distance weight: closer probes contribute more
    float distWeight = 1.0f / max(dist * dist, 0.01f);

    // Depth validity check: read octant depth
    float3 probeToSurf = -dir;
    uint octant = 0u;
    if (probeToSurf.x > 0.0f) octant |= 1u;
    if (probeToSurf.y > 0.0f) octant |= 2u;
    if (probeToSurf.z > 0.0f) octant |= 4u;

    // Read depth from the probe's depth texture
    uint depthTexelIdx = octant / 2u;
    uint3 probeCoord = uint3(
        uint((probeWorldPos.x - probeOrigin.x) / probeSpacing),
        uint((probeWorldPos.y - probeOrigin.y) / probeSpacing),
        uint((probeWorldPos.z - probeOrigin.z) / probeSpacing)
    );

    // Clamp to valid range
    probeCoord = clamp(probeCoord, uint3(0), probeCounts - 1u);

    float storedDepth = 100.0f;
    uint3 depthCoord = uint3(probeCoord.x, probeCoord.y, probeCoord.z * 4 + depthTexelIdx);
    if (depthCoord.z < probeCounts.z * 4) {
        float2 octantDepth = depthTexture.read(depthCoord).rg;
        storedDepth = (octant == depthTexelIdx * 2) ? octantDepth.x : octantDepth.y;
    }

    // Depth validity: does the probe "see through" to the surface?
    float depthValidity = smoothstep(0.0f, 0.5f, storedDepth / max(dist, 0.001f));

    return backfaceWeight * distWeight * depthValidity;
}

// ============================================================================
// Main Sampling Function: Trilinear interpolate DDGI irradiance
// ============================================================================

static float3 ddgiSampleIrradiance(
    float3 worldPos,
    float3 worldNormal,
    texture3d<float, access::sample> irradianceTexture,
    texture3d<float, access::sample> depthTexture,
    constant DDGIVolumeData& volume)
{
    // World position to probe grid continuous coordinates
    float3 gridPos = (worldPos - volume.ProbeOrigin) / volume.ProbeSpacing;

    // Base probe (floor) for trilinear interpolation
    int3 baseProbe = int3(floor(gridPos));
    float3 fracPart = fract(gridPos);

    float3 totalIrradiance = float3(0.0f);
    float  totalWeight = 0.0f;

    // Iterate over 8 corner probes
    for (uint corner = 0; corner < 8; ++corner) {
        int3 offset = int3(
            (corner & 1u) ? 1 : 0,
            (corner & 2u) ? 1 : 0,
            (corner & 4u) ? 1 : 0
        );

        int3 probeCoord = baseProbe + offset;

        // Skip out-of-bounds probes
        if (probeCoord.x < 0 || probeCoord.x >= int(volume.ProbeCounts.x) ||
            probeCoord.y < 0 || probeCoord.y >= int(volume.ProbeCounts.y) ||
            probeCoord.z < 0 || probeCoord.z >= int(volume.ProbeCounts.z)) {
            continue;
        }

        float3 probeWorldPos = ddgiProbeWorldPos(uint3(probeCoord), volume.ProbeOrigin, volume.ProbeSpacing);

        // Compute weight
        float weight = ddgiProbeWeight(
            worldPos, worldNormal, probeWorldPos,
            depthTexture,
            volume.ProbeOrigin, volume.ProbeSpacing, volume.ProbeCounts);

        // Trilinear blending weight
        float3 blendW;
        blendW.x = (corner & 1u) ? fracPart.x : (1.0f - fracPart.x);
        blendW.y = (corner & 2u) ? fracPart.y : (1.0f - fracPart.y);
        blendW.z = (corner & 4u) ? fracPart.z : (1.0f - fracPart.z);
        float trilinWeight = blendW.x * blendW.y * blendW.z;

        weight *= trilinWeight;

        // Sample irradiance
        float3 probeIrradiance = ddgiSampleProbeIrradiance(
            uint3(probeCoord), worldNormal, irradianceTexture, volume.ProbeCounts);

        totalIrradiance += probeIrradiance * weight;
        totalWeight += weight;
    }

    if (totalWeight > 0.0f) {
        return totalIrradiance / totalWeight;
    }

    return float3(0.0f);
}

#endif // DDGI_SAMPLE_METAL
