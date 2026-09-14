/**
 * @file ScreenProbeGather.metal
 * @brief Screen Probe GI - Gather pass (SH evaluation + probe bilateral filter)
 *
 * Evaluates radiance SH at the receiver normal, then continuously reconstructs
 * the adaptive probe lattice with a radius-two tent over 4x4 probe centers.
 * The former fixed five-tap cross changed its entire footprint at cell edges,
 * producing downsample-sized blocks even when its input SH was filtered.
 * Inactive probes are omitted and remaining weights normalized. Radius two
 * retains coarse-tier coverage around inactive checkerboard fine probes.
 *
 * Up to 16 probes are read per pixel (formerly 5); the extra gather bandwidth
 * buys continuous reconstruction without blurring full-resolution materials.
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
//   Y1 = -0.488603 * d.y
//   Y2 = 0.488603 * d.z
//   Y3 = -0.488603 * d.x
// Average stores radiance SH; irradiance requires the per-band cosine convolution.
// B1/B4: per-band radiance→irradiance evaluation (A0=π, A1=2π/3) — the
// probe field stores radiance SH (DDGI-compatible projection) and the
// diffuse consumer integrates it over the cosine lobe.
static inline float3 evalSHIrradiance(float3 sh0, float3 sh1, float3 sh2, float3 sh3,
                                      float3 n) {
    return sh0 * (0.282095f * 3.14159265f)
         + sh1 * (-0.488603f * 2.09439510f * n.y)
         + sh2 * ( 0.488603f * 2.09439510f * n.z)
         + sh3 * (-0.488603f * 2.09439510f * n.x);
}

static inline float3 evalSH(float3 sh0, float3 sh1, float3 sh2, float3 sh3,
                            float3 N)
{
    return sh0 * 0.282095f
         + sh1 * (-0.488603f * N.y)
         + sh2 * (0.488603f * N.z)
         + sh3 * (-0.488603f * N.x);
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

    // Compare actual receiver/probe surface positions, not only screen
    // proximity. A foreground column and the rear wall may share normals.
    float2 ndc = float2(pixelUV.x * 2.0 - 1.0, 1.0 - pixelUV.y * 2.0);
    float4 receiverH = global.inv_view_projection * float4(ndc, depth, 1.0);
    float3 receiverPos = receiverH.xyz / receiverH.w;
    float4 footprintH = global.inv_view_projection * float4(
        ndc + float2(2.0 * downsample / float(renderW), 0.0), depth, 1.0);
    float planeSigma = max(0.02, distance(receiverPos, footprintH.xyz / footprintH.w) * 0.25);

    // Continuous tent reconstruction on the probe-center lattice. Radius two
    // includes diagonal/coarse probes when the adaptive fine tier is inactive.
    // A tap enters/leaves the 4x4 footprint at zero weight, avoiding the old
    // fixed cross's discontinuities at every downsample-sized pixel block.
    float2 probeCoord = (float2(tid) + 0.5) / downsample - 0.5;
    int2 baseProbe = int2(floor(probeCoord));

    float3 irradianceSum = float3(0.0f);
    float  confidenceOut = 0.0f;
    float  weightSum     = 0.0f;

    for (int i = 0; i < 16; ++i) {
        int2 npc = baseProbe + int2(i % 4 - 1, i / 4 - 1);
        float2 tent = max(float2(0.0), 1.0 - abs(float2(npc) - probeCoord) * 0.5);
        float spatialWeight = tent.x * tent.y;
        if (spatialWeight <= 0.0) continue;
        if (npc.x < 0 || npc.y < 0 ||
            npc.x >= int(gridW) || npc.y >= int(gridH)) {
            continue;
        }

        uint probeIdx = uint(npc.y) * gridW + uint(npc.x);

        float4 posData = probePositions[probeIdx];
        if (posData.w <= 0.0f) continue;

        float3 surfaceDelta = posData.xyz - receiverPos;
        float planeDistance = abs(dot(surfaceDelta, surfaceNormal));
        float geometryWeight = exp(-0.5 * planeDistance * planeDistance / (planeSigma * planeSigma));
        // Underflow means this tap lies on a different surface; do not let
        // confidence normalization resurrect an entirely rejected neighbour.
        if (geometryWeight < 1e-5) continue;

        uint shBase = probeIdx * 4;
        float sh0w = probeSH[shBase + 0].w;
        if (sh0w <= 0.0f) continue;

        float3 sh0 = probeSH[shBase + 0].rgb;
        float3 sh1 = probeSH[shBase + 1].rgb;
        float3 sh2 = probeSH[shBase + 2].rgb;
        float3 sh3 = probeSH[shBase + 3].rgb;

        // Proper SH evaluation at the pixel's surface normal.
        float3 irr = evalSHIrradiance(sh0, sh1, sh2, sh3, surfaceNormal);   // B1
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

        float w = spatialWeight * geometryWeight * normalWeight * sh0w;
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
