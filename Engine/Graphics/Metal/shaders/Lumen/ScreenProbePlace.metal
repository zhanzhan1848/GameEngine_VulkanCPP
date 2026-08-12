/**
 * @file ScreenProbePlace.metal
 * @brief Screen Probe GI - Probe placement in screen space
 *
 * Places probes on a uniform screen-space grid. Each probe samples the
 * GBuffer depth and normal at its center to determine its world position.
 * Sky pixels (depth = 0 or very far) are marked inactive.
 *
 * Dispatch: (gridW * gridH + 63) / 64, 1, 1
 * ThreadGroupSize: (64, 1, 1)
 *
 * Uses 1D dispatch to avoid Metal validation errors when sharing a compute
 * encoder with other passes that use scalar thread_position_in_grid.
 */

#include <metal_stdlib>
using namespace metal;

// ============================================================================
// Constant buffer layout (must match C++ ScreenProbeGlobalData)
// ============================================================================

struct ScreenProbeGlobalData {
    float4x4 view_projection;
    float4x4 inv_view_projection;
    float4   camera_position;

    float4   grid_params;     // x=gridW, y=gridH, z=downsample, w=raysPerProbe
    float4   trace_params;    // x=maxRayDist, y=gatherRadius, z=renderWidth, w=renderHeight

    float4   sdf_origins[3];
    float4   sdf_voxel_sizes[3];
    float4   sdf_extents[3];
    float4   sdf_resolutions; // x=res0, y=res1, z=res2, w=cascadeCount
};

// ============================================================================
// Helper: reconstruct world position from depth + invViewProj
// ============================================================================

static float3 reconstructWorldPos(float2 uv, float depth,
                                  constant float4x4& invVP,
                                  float2 screenDims) {
    // NDC coordinates [-1, 1]
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);

    // Clip-space position
    float4 clipPos = float4(ndc, depth, 1.0f);

    // World-space position
    float4 worldPos = invVP * clipPos;
    return worldPos.xyz / worldPos.w;
}

// ============================================================================
// Main kernel
// ============================================================================

kernel void screen_probe_place(
    // Input textures
    texture2d<float, access::sample>  depthTexture  [[texture(0)]],
    texture2d<float, access::sample>  normalTexture [[texture(1)]],

    // Output buffers
    device float4* probePositions  [[buffer(0)]],
    device float4* probeNormals    [[buffer(1)]],

    // Constant buffer
    constant ScreenProbeGlobalData& global [[buffer(3)]],

    // Thread positioning (1D to be compatible with shared Metal compute encoder)
    uint global_id [[thread_position_in_grid]])
{
    uint gridW = uint(global.grid_params.x);
    uint gridH = uint(global.grid_params.y);
    uint totalProbes = gridW * gridH;

    if (global_id >= totalProbes) return;

    // Compute 2D grid coordinates from 1D index
    uint gx = global_id % gridW;
    uint gy = global_id / gridW;

    float downsample = global.grid_params.z;
    float renderW = global.trace_params.z;
    float renderH = global.trace_params.w;

    uint probeIdx = global_id;

    // Screen-space center of this probe
    float2 pixelCenter = float2(gx * downsample + downsample * 0.5f,
                                gy * downsample + downsample * 0.5f);

    // UV coordinates [0, 1]
    float2 uv = pixelCenter / float2(renderW, renderH);

    // Sample depth with POINT filtering — linear interpolation on depth
    // produces ghost values at object edges, creating floating probes
    sampler pointSamp(coord::normalized, filter::nearest, address::clamp_to_edge);
    sampler linearSamp(coord::normalized, filter::linear, address::clamp_to_edge);
    float depth = depthTexture.sample(pointSamp, uv).r;

    // Check for sky / far plane
    bool active = (depth > 0.0001f && depth < 0.999f);

    if (!active) {
        // Mark as inactive: position = (0,0,0,0), normal = (0,0,0,0)
        probePositions[probeIdx] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        probeNormals[probeIdx]   = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    // Reconstruct world position
    float3 worldPos = reconstructWorldPos(uv, depth, global.inv_view_projection, float2(renderW, renderH));

    // Sample normal (world-space, encoded as RGB)
    float3 normal = normalTexture.sample(linearSamp, uv).rgb;
    normal = normalize(normal * 2.0f - 1.0f);  // Decode from [0,1] to [-1,1]

    // Write outputs: w = 1.0 = active flag
    probePositions[probeIdx] = float4(worldPos, 1.0f);
    probeNormals[probeIdx]   = float4(normal, 1.0f);
}
