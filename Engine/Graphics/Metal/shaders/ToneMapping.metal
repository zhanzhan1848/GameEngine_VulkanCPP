#include <metal_stdlib>
using namespace metal;

struct VertexOut {
    float4 position [[position]];
    float2 uv;
};

// Full Screen Quad Vertex Shader
vertex VertexOut tonemap_vs(uint vertexID [[vertex_id]]) {
    float4 positions[3] = {
        float4(-1.0, -1.0, 0.0, 1.0),
        float4( 3.0, -1.0, 0.0, 1.0),
        float4(-1.0,  3.0, 0.0, 1.0)
    };
    float2 uvs[3] = {
        float2(0.0, 1.0),
        float2(2.0, 1.0),
        float2(0.0, -1.0)
    };

    VertexOut out;
    out.position = positions[vertexID];
    out.uv = uvs[vertexID];
    return out;
}

// ACES Tone Mapping
float3 ACESFilm(float3 x) {
    float a = 2.51f;
    float b = 0.03f;
    float c = 2.43f;
    float d = 0.59f;
    float e = 0.14f;
    return saturate((x*(a*x+b))/(x*(c*x+d)+e));
}

// Fragment Shader
fragment float4 tonemap_fs(VertexOut in [[stage_in]],
                           texture2d<float> sceneTexture [[texture(0)]],
                           texture2d<float> bloomTexture [[texture(1)]]) // Optional
{
    constexpr sampler s(filter::linear, address::clamp_to_edge);

    float3 color = sceneTexture.sample(s, in.uv).rgb;

    // Add Bloom if available (simple addition)
    // Check if texture is bound by checking dimensions or assuming it is valid if passed
    // Metal doesn't allow checking for null texture easily in shader without flags
    // Assuming bloomTexture is bound (can be black 1x1 if disabled)
    float3 bloom = bloomTexture.sample(s, in.uv).rgb;
    color += bloom;

    // Tone Mapping
    color = ACESFilm(color);

    // Gamma Correction
    color = pow(color, float3(1.0/2.2));

    return float4(color, 1.0);
}

// Passthrough variant: bloom composite only, NO ACES/gamma.
// Metal 的上游（DeferredLighting.metal fragmentLighting / FusionComposite）
// 已经做过 exposure+ACES+gamma（LDR 输出）；全链再过一次 tonemap 是双重
// 转换，暗部被抬到近白（2026-09-02 同机位截图实证）。Vulkan 上游输出线性
// HDR，仍走 tonemap_fs。
fragment float4 tonemap_fs_passthrough(VertexOut in [[stage_in]],
                                       texture2d<float> sceneTexture [[texture(0)]],
                                       texture2d<float> bloomTexture [[texture(1)]])
{
    constexpr sampler s(filter::linear, address::clamp_to_edge);
    float3 color = sceneTexture.sample(s, in.uv).rgb;
    float3 bloom = bloomTexture.sample(s, in.uv).rgb;
    return float4(color + bloom, 1.0);
}
