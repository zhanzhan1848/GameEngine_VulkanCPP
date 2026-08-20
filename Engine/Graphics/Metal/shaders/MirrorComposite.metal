// MirrorComposite.metal — Metal counterpart of
// Vulkan/shaders/PostProcess/MirrorComposite.vert/.frag.
// Entry points: mirror_composite_vs / mirror_composite_fs.
// Draws the mirror quad through the main camera; fragments project through
// the reflected VP to sample the reflection RT (Metal NDC→UV flips Y).
#include <metal_stdlib>
using namespace metal;

struct CompositeParams {
    float4x4 view_projection;
    float4x4 reflected_view_projection;
    float4 plane_position;
    float4 plane_normal;
    float4 plane_axes;
    float4 camera_position;   // w = reflectivity
};

struct VSOut {
    float4 position [[position]];
    float3 worldPos [[user(loc0)]];
};

static constant float2 quadCorners[6] = {
    float2(-1.0, -1.0), float2( 1.0, -1.0), float2(-1.0,  1.0),
    float2(-1.0,  1.0), float2( 1.0, -1.0), float2( 1.0,  1.0)
};

vertex VSOut mirror_composite_vs(
    uint vertexID [[vertex_id]],
    constant CompositeParams& params [[buffer(0)]]
) {
    VSOut out;
    float2 corner = quadCorners[vertexID];

    float3 N = normalize(params.plane_normal.xyz);
    float3 up = abs(N.y) < 0.99f ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    float3 U = normalize(cross(up, N));
    float3 V = cross(N, U);

    float3 worldPos = params.plane_position.xyz +
                      U * (corner.x * params.plane_axes.x) +
                      V * (corner.y * params.plane_axes.y);

    out.worldPos = worldPos;
    out.position = params.view_projection * float4(worldPos, 1.0);
    return out;
}

fragment float4 mirror_composite_fs(
    VSOut in [[stage_in]],
    constant CompositeParams& params [[buffer(0)]],
    texture2d<float> reflectionTex [[texture(1)]],
    sampler reflectionSampler [[sampler(2)]]
) {
    // Metal NDC y=+1 is top, texture v=0 is top → v = 0.5 - 0.5*ndc.y.
    float4 clip = params.reflected_view_projection * float4(in.worldPos, 1.0);
    float3 ndc = clip.xyz / clip.w;
    float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);

    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        discard_fragment();
    }

    float3 reflection = reflectionTex.sample(reflectionSampler, uv).rgb;

    float3 viewDir = normalize(in.worldPos - params.camera_position.xyz);
    float cosTheta = abs(dot(viewDir, normalize(params.plane_normal.xyz)));
    float fresnel = mix(0.35, 0.9, pow(1.0 - cosTheta, 3.0));

    float alpha = clamp(params.camera_position.w * fresnel, 0.0, 1.0);
    return float4(reflection, alpha);
}
