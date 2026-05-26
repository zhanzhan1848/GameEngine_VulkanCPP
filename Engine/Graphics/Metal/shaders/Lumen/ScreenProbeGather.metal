/**
 * @file ScreenProbeGather.metal
 * @brief Screen Probe GI - Gather pass
 *
 * Gathers irradiance from screen-space probes using bilateral filtering.
 * Uses constant buffer at slot 3 (set via setBytes, not pool buffer).
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

    // Find which probe this pixel belongs to
    uint2 probeCoord = uint2(tid.x / uint(downsample), tid.y / uint(downsample));
    if (probeCoord.x >= gridW || probeCoord.y >= gridH) {
        outputTexture.write(float4(0.0f, 0.0f, 0.0f, 0.0f), tid);
        return;
    }

    uint probeIdx = probeCoord.y * gridW + probeCoord.x;

    // Sample depth to check for sky pixels
    float2 invRes = 1.0f / float2(renderW, renderH);
    float2 pixelUV = (float2(tid) + 0.5f) * invRes;

    sampler bilinear(coord::normalized, filter::linear, address::clamp_to_edge);
    float depth = depthTexture.sample(bilinear, pixelUV).r;

    if (depth <= 0.0001f || depth >= 0.999f) {
        outputTexture.write(float4(0.0f, 0.0f, 0.0f, 0.0f), tid);
        return;
    }

    // Check if probe is active
    float4 posData = probePositions[probeIdx];
    if (posData.w <= 0.0f) {
        outputTexture.write(float4(0.0f, 0.0f, 0.0f, 0.0f), tid);
        return;
    }

    // Read SH coefficients for this probe
    // Layout: probeIdx * 4 + band
    uint shBase = probeIdx * 4;
    float3 irradiance = probeSH[shBase].xyz * 0.282095f; // L0 coefficient

    // Add L1 band contribution
    irradiance += probeSH[shBase + 1].xyz * 0.488603f;
    irradiance += probeSH[shBase + 2].xyz * 0.488603f;
    irradiance += probeSH[shBase + 3].xyz * 0.488603f;

    // Confidence from SH alpha channel
    float confidence = probeSH[shBase].w;

    outputTexture.write(float4(max(irradiance, 0.0f), confidence), tid);
}
