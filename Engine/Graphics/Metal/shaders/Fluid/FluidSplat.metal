#include <metal_stdlib>
using namespace metal;

// ============================================================================
// FluidParams — must match C++ FluidTypes.h exactly (224 bytes)
// ============================================================================

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
// Fluid Splat — Graphics pipeline
// Renders each particle as a sphere billboard. Fragment outputs sphere depth
// to gl_FragDepth and additive thickness to color.
// ============================================================================

struct FluidSplatVSOut {
    float4 position [[position]];
    float  sphere_z;
    float  thickness;
    float2 uv;
    float  alpha;
};

// Vertex shader: project particle as billboard quad with sphere depth
vertex FluidSplatVSOut fluid_splat_vertex(
    device const float4* particles         [[buffer(0)]],  // xyz=pos, w=radius
    constant FluidParams& params           [[buffer(1)]],
    uint vid [[vertex_id]],
    uint iid [[instance_id]])
{
    FluidSplatVSOut out;

    float3 worldPos = particles[iid].xyz;
    float radius = max(particles[iid].w, params.FluidParams1.x);

    // Project center to clip space
    float4 centerClip = float4(
        dot(params.ViewProj[0], float4(worldPos, 1.0)),
        dot(params.ViewProj[1], float4(worldPos, 1.0)),
        dot(params.ViewProj[2], float4(worldPos, 1.0)),
        dot(params.ViewProj[3], float4(worldPos, 1.0))
    );

    // Billboard quad vertices (-1 to 1)
    float2 quadPos[6] = {
        float2(-1, -1), float2(1, -1), float2(-1, 1),
        float2(-1, 1),  float2(1, -1), float2(1, 1)
    };
    float2 q = quadPos[vid];

    // Screen-space splat radius from world radius
    float clipW = max(centerClip.w, 0.001);
    float screenRadius = radius * params.FluidParams1.y * params.ScreenParams.y / clipW;

    // Offset vertex in NDC
    float4 clipPos = centerClip;
    clipPos.x += q.x * screenRadius * clipW;
    clipPos.y += q.y * screenRadius * clipW;

    out.position = clipPos;
    out.uv = q * 0.5 + 0.5;
    out.sphere_z = centerClip.z / clipW;

    // Sphere depth at this UV point
    float r2 = q.x * q.x + q.y * q.y;
    if (r2 < 1.0) {
        float sphereDepth = sqrt(1.0 - r2) * radius;
        float dz = sphereDepth * clipW / params.ScreenParams.y;
        out.sphere_z = (centerClip.z + dz) / clipW;
        out.thickness = sqrt(1.0 - r2) * radius * 2.0;
    } else {
        out.sphere_z = 2.0;
        out.thickness = 0.0;
    }

    out.alpha = 1.0;
    return out;
}

// Fragment shader: output depth + thickness
fragment float4 fluid_splat_fragment(
    FluidSplatVSOut in [[stage_in]])
{
    // Discard outside unit circle
    float2 centered = in.uv - 0.5;
    float r2 = dot(centered, centered) * 4.0;
    if (r2 > 1.0) discard_fragment();

    // Thickness with soft edge
    float softEdge = 1.0 - smoothstep(0.7, 1.0, r2);
    return float4(in.thickness * softEdge, 0.0, 0.0, in.sphere_z);
}
