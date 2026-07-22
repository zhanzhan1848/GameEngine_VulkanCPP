/**
 * @file StaticProbeSampling.metal
 * @brief Static probe sampling: tetrahedral interpolation with Chebyshev
 *        visibility weighting, reading from separate mean/var buffers.
 *
 * Algorithm mirrors DDGISample.metal exactly, but reads static probe data
 * from split buffers (separate mean and variance arrays) instead of the
 * DDGI interleaved layout [mean0..63, var0..63].
 *
 * Static probes store full L0+L1+L2 (9 SH coefficients) per probe.
 */

#ifndef STATIC_PROBE_SAMPLING_METAL
#define STATIC_PROBE_SAMPLING_METAL

#include "DDGIVolumeData.metal"
#include "ProbeConfidence.metal"
#include "DDGISample.metal"

// ============================================================================
// StaticProbeData — constant buffer for static baked probe grid params
// (duplicated here for self-contained inclusion; matches DDGIGIGather.metal)
// ============================================================================
struct StaticProbeData {
    float4 ProbeOrigin;       // xyz=origin, w=unused
    float  ProbeSpacing;
    uint   GridDimX;
    uint   GridDimY;
    uint   GridDimZ;
    float  _pad[2];
    float4 SkySH[9];
};

// ============================================================================
// Octahedral depth sampling for static buffers (separate mean/var arrays)
// ============================================================================

/**
 * Sample depth from two separate arrays instead of one interleaved buffer.
 * DDGI layout: buf[probeIdx * 128 + octantIdx] for mean, buf[probeIdx * 128 + 64 + octantIdx] for var
 * Static layout: meanBuf[probeIdx * 64 + octantIdx], varBuf[probeIdx * 64 + octantIdx]
 */
static float2 sampleStaticDepthOctahedral(
    device const float* depthMean,
    device const float* depthVar,
    uint probeIdx,
    float3 direction)
{
    constexpr uint res = DDGI_DEPTH_RES;
    constexpr uint texels = res * res;

    float2 uv = octahedralEncode(direction);
    uint2 texel = uint2(clamp(uint(uv.x * float(res)), 0u, res - 1u),
                        clamp(uint(uv.y * float(res)), 0u, res - 1u));
    uint idx = texel.y * res + texel.x;

    float mean = depthMean[probeIdx * texels + idx];
    float vari = depthVar[probeIdx * texels + idx];
    return float2(mean, vari);
}

// ============================================================================
// Full L0+L1+L2 SH dot product (9 coefficients)
// ============================================================================

static float3 shDot9(thread const float3* c, float3 d) {
    float x = d.x, y = d.y, z = d.z;
    float x2 = x*x, y2 = y*y, z2 = z*z;
    return c[0] * DDGI_SH_C0
         + c[1] * (-DDGI_SH_C1 * y)
         + c[2] * ( DDGI_SH_C1 * z)
         + c[3] * (-DDGI_SH_C1 * x)
         + c[4] * ( DDGI_SH_C2_0 * y * x)
         + c[5] * (-DDGI_SH_C2_0 * y * z)
         + c[6] * ( DDGI_SH_C2_1 * (3.0f * z2 - 1.0f))
         + c[7] * (-DDGI_SH_C2_0 * x * z)
         + c[8] * ( DDGI_SH_C2_2 * (x2 - y2));
}

// Overload accepting float3 array (for threadgroup arrays which can't be thread const float3*)
static float3 shDot9(const float3 c[9], float3 d) {
    float x = d.x, y = d.y, z = d.z;
    float x2 = x*x, y2 = y*y, z2 = z*z;
    return c[0] * DDGI_SH_C0
         + c[1] * (-DDGI_SH_C1 * y)
         + c[2] * ( DDGI_SH_C1 * z)
         + c[3] * (-DDGI_SH_C1 * x)
         + c[4] * ( DDGI_SH_C2_0 * y * x)
         + c[5] * (-DDGI_SH_C2_0 * y * z)
         + c[6] * ( DDGI_SH_C2_1 * (3.0f * z2 - 1.0f))
         + c[7] * (-DDGI_SH_C2_0 * x * z)
         + c[8] * ( DDGI_SH_C2_2 * (x2 - y2));
}

// ============================================================================
// Interpolate sky factor from 4 probes using barycentric weights
// ============================================================================

static float interpolateSkyFactor(
    float3 worldPos,
    float baryWeights[4],
    uint probeIndices[4],
    constant StaticProbeData& sp,
    device const float* skyFactor)
{
    float result = 0.0f;
    float totalW = 0.0f;
    for (uint i = 0; i < 4u; ++i) {
        float w = baryWeights[i];
        result += skyFactor[probeIndices[i]] * w;
        totalW += w;
    }
    return (totalW > 0.0f) ? result / totalW : 0.0f;
}

// ============================================================================
// Sample static probe irradiance — tetrahedral 4-probe interpolation
// with Chebyshev visibility (same algorithm as DDGISample.metal)
// ============================================================================

static float3 sampleStaticIrradiance(
    float3 worldPos,
    float3 normal,
    constant StaticProbeData& sp,
    device const float3* staticIrradiance,
    device const float* staticDepthMean,
    device const float* staticDepthVar)
{
    float3 origin = sp.ProbeOrigin.xyz;
    float spacing = sp.ProbeSpacing;
    uint3 counts  = uint3(sp.GridDimX, sp.GridDimY, sp.GridDimZ);

    float3 gridPos = (worldPos - origin) / spacing;
    float3 gridMax = float3(float(counts.x - 1u),
                            float(counts.y - 1u),
                            float(counts.z - 1u));

    if (any(gridPos < 0.0f) || any(gridPos > gridMax)) return float3(0.0f);

    uint probeIdx[4];
    float baryW[4];
    tetrahedralProbes(gridPos, counts, probeIdx, baryW);

    float3 n = normalize(normal);
    float3 result = float3(0.0f);

    // Normal bias — same as DDGISample
    float3 biasedPos = worldPos + n * spacing * 0.05f;

    float totalWeight = 0.0f;

    for (uint p = 0; p < 4u; ++p) {
        if (baryW[p] < 0.001f) continue;

        uint3 gc = ddgiProbeGridCoord(probeIdx[p], counts);
        float3 probePos = ddgiProbeWorldPos(gc, origin, spacing);

        // Read 9 SH coefficients (full L0+L1+L2)
        uint base = probeIdx[p] * 9u;
        float3 sh[9];
        for (uint i = 0; i < 9u; ++i) {
            sh[i] = staticIrradiance[base + i];
        }

        float3 irradiance = shDot9(sh, n);

        // Direction FROM probe TO biased shading point
        float3 fromProbeDir = normalize(biasedPos - probePos);
        float distToProbe = length(biasedPos - probePos);

        // Static depth: separate mean/var buffers
        float2 depthMV = sampleStaticDepthOctahedral(
            staticDepthMean, staticDepthVar, probeIdx[p], fromProbeDir);

        float visWeight = ddgiVisibilityWeight(
            distToProbe, depthMV.x, depthMV.y, spacing);

        float w = baryW[p] * visWeight;
        result += irradiance * w;
        totalWeight += w;
    }

    if (totalWeight > 0.0f) {
        result /= totalWeight;
    }

    return max(result, float3(0.0f));
}

#endif // STATIC_PROBE_SAMPLING_METAL
