/**
 * @file ScreenProbeGather.metal
 * @brief Screen Probe GI - 4-probe bilinear interpolation gather
 *
 * For each screen pixel, finds the 4 nearest probes (2x2 grid neighborhood)
 * and computes bilinearly weighted irradiance with soft depth rejection.
 *
 * Buffer reads: 4 (positions) + 4 (normals) + 4 (radiance) = 12 total,
 * per-buffer max 4, well within Apple Silicon limits.
 *
 * Dispatch: (renderW * renderH + 63) / 64, 1, 1
 * ThreadGroupSize: (64, 1, 1)
 */

#include <metal_stdlib>
using namespace metal;

struct ScreenProbeGlobalData {
    float4x4 view_projection;
    float4x4 inv_view_projection;
    float4   camera_position;

    float4   grid_params;     // x=gridW, y=gridH, z=downsample, w=raysPerProbe
    float4   trace_params;    // x=maxRayDist, y=gatherRadius, z=renderWidth, w=renderHeight

    float4   sdf_origins[3];
    float4   sdf_voxel_sizes[3];
    float4   sdf_extents[3];
    float4   sdf_resolutions;
};

static float3 reconstructWorldPos(float2 uv, float depth,
                                  constant float4x4& invVP) {
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 clipPos = float4(ndc, depth, 1.0f);
    float4 worldPos = invVP * clipPos;
    return worldPos.xyz / worldPos.w;
}

kernel void screen_probe_gather(
    texture2d<float, access::sample>  depthTexture  [[texture(0)]],
    texture2d<float, access::sample>  normalTexture [[texture(1)]],
    texture2d<float, access::write>   outputTexture [[texture(2)]],

    // Pre-averaged (and temporally accumulated) per-probe radiance
    device const float4* probePositions   [[buffer(0)]],
    device const float4* probeNormals     [[buffer(1)]],
    device const float4* probeAvgRadiance [[buffer(2)]],

    constant ScreenProbeGlobalData& global [[buffer(3)]],

    uint global_id [[thread_position_in_grid]])
{
    uint renderW = uint(global.trace_params.z);
    uint renderH = uint(global.trace_params.w);
    uint totalPixels = renderW * renderH;

    if (global_id >= totalPixels) return;

    uint px = global_id % renderW;
    uint py = global_id / renderW;
    uint2 gid = uint2(px, py);

    uint  gridW = uint(global.grid_params.x);
    uint  gridH = uint(global.grid_params.y);
    float downsample = global.grid_params.z;

    float2 uv = float2(float(px) + 0.5f, float(py) + 0.5f) / float2(renderW, renderH);

    sampler pointSamp(coord::normalized, filter::nearest, address::clamp_to_edge);
    sampler linearSamp(coord::normalized, filter::linear, address::clamp_to_edge);

    float depth = depthTexture.sample(pointSamp, uv).r;

    if (depth <= 0.0001f || depth >= 0.999f) {
        outputTexture.write(float4(0.0f, 0.0f, 0.0f, 1.0f), gid);
        return;
    }

    float3 worldPos = reconstructWorldPos(uv, depth, global.inv_view_projection);

    float3 normal = normalTexture.sample(linearSamp, uv).rgb;
    normal = normalize(normal * 2.0f - 1.0f);

    // Grid-space position: fractional coordinate within the probe grid
    float2 gridPos = float2(float(px), float(py)) / downsample;
    int2 baseProbe = int2(floor(gridPos));
    float2 frac = gridPos - float2(baseProbe);

    // 4-probe bilinear interpolation (2x2 neighborhood)
    float3 totalGI = float3(0.0f);
    float totalWeight = 0.0f;

    for (int dy = 0; dy <= 1; ++dy) {
        for (int dx = 0; dx <= 1; ++dx) {
            int kx = baseProbe.x + dx;
            int ky = baseProbe.y + dy;

            if (kx < 0 || kx >= int(gridW) || ky < 0 || ky >= int(gridH)) continue;

            uint pidx = uint(ky) * gridW + uint(kx);

            // Check probe active (position.w > 0)
            float4 posData = probePositions[pidx];
            if (posData.w <= 0.0f) continue;

            // Check radiance valid
            float4 avgData = probeAvgRadiance[pidx];
            if (avgData.w <= 0.0f) continue;

            float3 probePos = posData.xyz;

            // Bilinear weight based on fractional position within grid cell
            float wx = (dx == 0) ? (1.0f - frac.x) : frac.x;
            float wy = (dy == 0) ? (1.0f - frac.y) : frac.y;
            float w_bilinear = wx * wy;

            // Soft depth rejection: perpendicular plane distance
            // (0.3 + 0.7 * exp(...)) ensures even rejected probes contribute 30%,
            // preventing hard block boundaries at depth discontinuities
            float planeDist = abs(dot(probePos - worldPos, normal));
            float w_depth = exp(-planeDist * planeDist * 0.1f);

            float weight = w_bilinear * (0.3f + 0.7f * w_depth);

            totalGI += avgData.rgb * weight;
            totalWeight += weight;
        }
    }

    if (totalWeight > 0.001f) {
        totalGI /= totalWeight;
    }

    constexpr float GI_INTENSITY = 2.0f;
    float3 irradiance = totalGI * GI_INTENSITY;

    outputTexture.write(float4(irradiance, 1.0f), gid);
}
