#include <metal_stdlib>
using namespace metal;

struct FluidParams {
    float4 CameraPos;
    float4 ScreenParams;
    float4 InvViewProj[4];
    float4 ViewProj[4];
    float4 FluidParams1;
    float4 FluidParams2;
    float4 FluidParams3;
    float4 DepthParams;
};

// ============================================================================
// Octahedral encoding (same as SurfaceCacheData.metal)
// ============================================================================

static float2 octEncode(float3 n) {
    float l1norm = abs(n.x) + abs(n.y) + abs(n.z);
    float2 result = n.xy / max(l1norm, 1e-6);
    if (n.z < 0.0) {
        result = (1.0 - abs(result.yx)) * select(float2(-1.0), float2(1.0), result.xy >= 0.0);
    }
    return result;
}

// ============================================================================
// Fluid Normal — depth → world-space normals via finite differences
// ============================================================================

static float3 reconstructWorldPos(float2 uv, float linearDepth, constant FluidParams& params) {
    float2 ndc = uv * 2.0 - 1.0;
    ndc.y = 1.0 - ndc.y;
    float4 world4 = params.InvViewProj[0] * ndc.x
                   + params.InvViewProj[1] * ndc.y
                   + params.InvViewProj[2] * linearDepth
                   + params.InvViewProj[3];
    return world4.xyz / max(world4.w, 1e-6);
}

kernel void fluid_normal(
    texture2d<float, access::sample>  smoothed_depth [[texture(0)]],
    texture2d<float, access::write>   fluid_normal   [[texture(1)]],
    constant FluidParams& params                     [[buffer(0)]],
    uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= uint(params.ScreenParams.x) ||
        gid.y >= uint(params.ScreenParams.y)) return;

    constexpr sampler s(coord::pixel, filter::nearest, address::clamp_to_edge);

    float centerDepth = smoothed_depth.sample(s, float2(gid)).r;

    // No fluid — mark invalid
    if (centerDepth <= 0.0) {
        fluid_normal.write(float4(0.0, 0.0, 0.0, 0.0), gid);
        return;
    }

    // Cross-pattern finite differences (4 texture reads)
    uint2 w = uint(params.ScreenParams.x), h = uint(params.ScreenParams.y);
    float dLeft   = smoothed_depth.sample(s, float2(max(int(gid.x) - 1, 0), gid.y)).r;
    float dRight  = smoothed_depth.sample(s, float2(min(gid.x + 1, w - 1u), gid.y)).r;
    float dTop    = smoothed_depth.sample(s, float2(gid.x, max(int(gid.y) - 1, 0))).r;
    float dBottom = smoothed_depth.sample(s, float2(gid.x, min(gid.y + 1, h - 1u))).r;

    // Use valid neighbors only
    float dx = 0.0, dy = 0.0;
    if (dLeft > 0.0 && dRight > 0.0) dx = (dRight - dLeft) * 0.5;
    else if (dLeft > 0.0) dx = centerDepth - dLeft;
    else if (dRight > 0.0) dx = dRight - centerDepth;

    if (dTop > 0.0 && dBottom > 0.0) dy = (dBottom - dTop) * 0.5;
    else if (dTop > 0.0) dy = centerDepth - dTop;
    else if (dBottom > 0.0) dy = dBottom - centerDepth;

    // Reconstruct world positions for normal estimation
    float2 uv      = (float2(gid) + 0.5) / params.ScreenParams.xy;
    float2 uvRight = (float2(gid) + float2(1.0, 0.5)) / params.ScreenParams.xy;
    float2 uvUp    = (float2(gid) + float2(0.5, 1.0)) / params.ScreenParams.xy;

    float3 posCenter = reconstructWorldPos(uv, centerDepth, params);
    float3 posRight  = reconstructWorldPos(uvRight, max(dRight, centerDepth), params);
    float3 posUp     = reconstructWorldPos(uvUp, max(dBottom, centerDepth), params);

    float3 normal = normalize(cross(posUp - posCenter, posRight - posCenter));

    // Flip normal to face camera
    float3 viewDir = normalize(params.CameraPos.xyz - posCenter);
    if (dot(normal, viewDir) < 0.0) normal = -normal;

    // Encode to octahedral
    float2 enc = octEncode(normal);
    fluid_normal.write(float4(enc * 0.5 + 0.5, 0.0, 1.0), gid);
}
