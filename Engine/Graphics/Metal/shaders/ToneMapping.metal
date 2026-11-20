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

// Match the existing 32-byte CPU ToneParamsUBO. The opt-in adaptive entry
// binds the native R32 state; legacy entries do not reference that texture.
struct ToneParams {
    float exposure;
    float autoAdapt;
    float key;
    float contrast;
    float saturation;
    float vignette;
    float bloomIntensity;
    uint _pad1;
};
static_assert(sizeof(ToneParams) == 32, "Tone parameters CPU/MSL ABI");
static_assert(__builtin_offsetof(ToneParams, bloomIntensity) == 24, "Bloom gain ABI");

// HDR Fusion output requires ACES exactly once. The Metal Sponza reference
// launcher explicitly selects this entry while legacy LDR users retain the
// passthrough entry below. Exposure EV remains the CPU's unmodified value.
float4 apply_tone(float3 color, float3 bloom, float2 uv,
                  constant ToneParams& params, float exposure) {
    if (!all(isfinite(color))) color = float3(0.0f);
    color = (color + bloom * params.bloomIntensity) * exposure;
    color = pow(ACESFilm(color), float3(1.0f / 2.2f));

    // Same display-domain grading as the frozen reference tone shader.
    if (params.contrast != 1.0f)
        color = clamp((color - 0.5f) * params.contrast + 0.5f, 0.0f, 1.0f);
    if (params.saturation != 1.0f) {
        float luminance = dot(color, float3(0.2126f, 0.7152f, 0.0722f));
        color = mix(float3(luminance), color, params.saturation);
    }
    if (params.vignette > 0.0f) {
        float distanceFromCenter = distance(uv, float2(0.5f));
        color *= 1.0f - params.vignette * smoothstep(0.35f, 0.85f, distanceFromCenter);
    }
    return float4(color, 1.0f);
}

fragment float4 tonemap_fs(VertexOut in [[stage_in]],
                           texture2d<float> sceneTexture [[texture(0)]],
                           texture2d<float> bloomTexture [[texture(1)]],
                           constant ToneParams& params [[buffer(6)]]) {
    constexpr sampler s(filter::linear, address::clamp_to_edge);
    return apply_tone(sceneTexture.sample(s,in.uv).rgb,bloomTexture.sample(s,in.uv).rgb,
                      in.uv,params,params.exposure);
}

fragment float4 tonemap_fs_adapt(VertexOut in [[stage_in]],
                                 texture2d<float> sceneTexture [[texture(0)]],
                                 texture2d<float> bloomTexture [[texture(1)]],
                                 constant ToneParams& params [[buffer(6)]],
                                 texture2d<float, access::read> adaptState [[texture(7)]]) {
    constexpr sampler s(filter::linear, address::clamp_to_edge);
    float exposure = params.exposure;
    if (params.autoAdapt > .5f) {
        const float average = exp2(adaptState.read(uint2(0)).r);
        if (average > 1e-6f && average < 1e30f) exposure *= params.key/average;
    }
    return apply_tone(sceneTexture.sample(s,in.uv).rgb,bloomTexture.sample(s,in.uv).rgb,
                      in.uv,params,exposure);
}

// Preserve the legacy LDR/bloom contract; this is not an HDR display transform.
fragment float4 tonemap_fs_passthrough(VertexOut in [[stage_in]],
                                       texture2d<float> sceneTexture [[texture(0)]],
                                       texture2d<float> bloomTexture [[texture(1)]],
                                       constant ToneParams& params [[buffer(6)]])
{
    constexpr sampler s(filter::linear, address::clamp_to_edge);
    float3 color = sceneTexture.sample(s, in.uv).rgb;
    float3 bloom = bloomTexture.sample(s, in.uv).rgb;
    return float4((color + bloom) * params.exposure, 1.0f);
}
