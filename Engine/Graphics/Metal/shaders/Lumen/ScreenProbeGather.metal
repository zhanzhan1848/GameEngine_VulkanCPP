/**
 * @file ScreenProbeGather.metal
 * @brief Screen Probe GI - Gather pass (SH evaluation + probe bilateral filter)
 *
 * Two bugs fixed vs. prior version:
 *  1. SH evaluation now actually uses the surface normal. Old code summed all
 *     SH bands as scalars (L0 + |L1|), which collapsed to a spatially-uniform
 *     result — every pixel sharing a probe got the same color. The correct
 *     irradiance reconstruction is E(N) = Σ SH_C_i * Y_i(N).
 *  2. Pixel-level probe gather. Old code read exactly one probe per pixel
 *     (the one the pixel fell inside), so pixels whose assigned probe was
 *     inactive got zero. We now gather center + 4 cardinal neighbors
 *     (5-tap cross, matching SpatialFilter's probe-level kernel) with
 *     bilateral weights, which fills sparse coverage.
 *
 * Apple Silicon constraint: keep per-thread device buffer reads ≤ 32.
 *   5 probes × (4 SH + 1 pos + 1 normal) = 30 reads.
 *
 * Buffers:
 *   texture(0): depthTexture       (full-res, sampled)
 *   texture(1): normalTexture      (full-res, sampled) -- GBuffer normals, N*0.5+0.5
 *   texture(2): outputTexture      (full-res RGBA16F, storage)
 *   buffer(0):  probePositions     (gridW * gridH float4, xyz=pos w=active)
 *   buffer(2):  probeSH            (gridW * gridH * 4 float4, filtered SH)
 *   buffer(3):  global data        (ScreenProbeGlobalData, set via setBytes)
 *   buffer(4):  probeNormals       (gridW * gridH float4, xyz=normal w=valid)
 */

#include <metal_stdlib>
using namespace metal;

struct ScreenProbeGlobalData {
    float4x4 view_projection;
    float4x4 inv_view_projection;
    float4   camera_position;
    float4   grid_params;      // x=gridW, y=gridH, z=downsample, w=raysPerProbe
    float4   trace_params;     // x=maxRayDist, y=gatherRadius, z=renderWidth, w=renderHeight
    float4   sdf_origins[3];
    float4   sdf_voxel_sizes[3];
    float4   sdf_extents[3];
    float4   sdf_resolutions;
    float4   surface_cache_params; // x=atlasSize, y=cardCount, z=surfaceCacheAvailable, w=unused
};

// SH basis (must match ScreenProbeAverage::shBasis):
//   Y0 = 0.282095
//   Y1 = 0.488603 * d.y
//   Y2 = 0.488603 * d.z
//   Y3 = 0.488603 * d.x
// Average stores SH coefficients C_i such that E(N) = Σ C_i * Y_i(N).
static inline float3 evalSH(float3 sh0, float3 sh1, float3 sh2, float3 sh3,
                            float3 N)
{
    return sh0 * 0.282095f
         + sh1 * (0.488603f * N.y)
         + sh2 * (0.488603f * N.z)
         + sh3 * (0.488603f * N.x);
}

kernel void screen_probe_gather(
    texture2d<float, access::sample>  depthTexture  [[texture(0)]],
    texture2d<float, access::sample>  normalTexture [[texture(1)]],
    texture2d<float, access::write>   outputTexture [[texture(2)]],

    device const float4* probePositions   [[buffer(0)]],
    device const float4* probeSH          [[buffer(2)]],
    constant ScreenProbeGlobalData& global [[buffer(3)]],
    device const float4* probeNormals     [[buffer(4)]],

    uint2 tid [[thread_position_in_grid]])
{
    uint renderW = uint(global.trace_params.z);
    uint renderH = uint(global.trace_params.w);

    if (tid.x >= renderW || tid.y >= renderH) return;

    uint gridW = uint(global.grid_params.x);
    uint gridH = uint(global.grid_params.y);
    float downsample = global.grid_params.z;

    // ---------------------------------------------------------------
    // Read pixel depth + normal (normalTexture was previously unused)
    // ---------------------------------------------------------------
    float2 invRes = 1.0f / float2(renderW, renderH);
    float2 pixelUV = (float2(tid) + 0.5f) * invRes;

    constexpr sampler screenSamp(coord::normalized, filter::linear, address::clamp_to_edge);
    float depth = depthTexture.sample(screenSamp, pixelUV).r;

    if (depth <= 0.0001f || depth >= 0.999f) {
        outputTexture.write(float4(0.0f, 0.0f, 0.0f, 0.0f), tid);
        return;
    }

    float4 normalEncoded = normalTexture.sample(screenSamp, pixelUV);
    float3 surfaceNormal = normalize(normalEncoded.xyz * 2.0f - 1.0f);

    // ---------------------------------------------------------------
    // Center probe coord for this pixel
    // ---------------------------------------------------------------
    int2 centerProbe = int2(int(tid.x) / int(downsample),
                            int(tid.y) / int(downsample));

    // 5-tap cross pattern: center + 4 cardinals (matches SpatialFilter kernel).
    thread const int2 offsets[5] = {
        int2( 0,  0),
        int2(-1,  0), int2( 1,  0),
        int2( 0, -1), int2( 0,  1),
    };

    // Spatial weights for the cross (center gets 2.0, neighbors 1.0).
    // This mirrors SpatialFilter's "center-weighted" blending so we don't
    // double-smooth — Gather just evaluates the already-filtered SH at this
    // pixel's actual surface normal.
    thread const float spatialW[5] = { 2.0f, 1.0f, 1.0f, 1.0f, 1.0f };

    float3 irradianceSum = float3(0.0f);
    float  confidenceOut = 0.0f;
    float  weightSum     = 0.0f;

    for (int i = 0; i < 5; ++i) {
        int2 npc = centerProbe + offsets[i];
        if (npc.x < 0 || npc.y < 0 ||
            npc.x >= int(gridW) || npc.y >= int(gridH)) {
            continue;
        }

        uint probeIdx = uint(npc.y) * gridW + uint(npc.x);

        float4 posData = probePositions[probeIdx];
        if (posData.w <= 0.0f) continue;

        uint shBase = probeIdx * 4;
        float sh0w = probeSH[shBase + 0].w;
        if (sh0w <= 0.0f) continue;

        float3 sh0 = probeSH[shBase + 0].rgb;
        float3 sh1 = probeSH[shBase + 1].rgb;
        float3 sh2 = probeSH[shBase + 2].rgb;
        float3 sh3 = probeSH[shBase + 3].rgb;

        // Proper SH evaluation at the pixel's surface normal.
        float3 irr = evalSH(sh0, sh1, sh2, sh3, surfaceNormal);
        irr = max(irr, float3(0.0f));

        // Reject probes whose normal faces away from the pixel's surface.
        float normalWeight = 1.0f;
        float4 probeNormData = probeNormals[probeIdx];
        if (probeNormData.w > 0.0f) {
            float3 probeN = normalize(probeNormData.xyz);
            float nDot = max(dot(surfaceNormal, probeN), 0.0f);
            // Gentle falloff: probes facing similar direction contribute fully,
            // orthogonal probes contribute half, opposite probes contribute ~0.
            normalWeight = pow(nDot, 2.0f);
        }

        float w = spatialW[i] * normalWeight * sh0w;
        irradianceSum += irr * w;
        confidenceOut += sh0w * w;
        weightSum += w;
    }

    if (weightSum <= 0.0f) {
        outputTexture.write(float4(0.0f, 0.0f, 0.0f, 0.0f), tid);
        return;
    }

    float3 finalIrr = irradianceSum / weightSum;
    float  finalConf = confidenceOut / weightSum;
    outputTexture.write(float4(max(finalIrr, float3(0.0f)), finalConf), tid);
}
