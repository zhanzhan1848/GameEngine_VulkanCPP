#include <metal_stdlib>
using namespace metal;

struct VertexOut {
    float4 position [[position]];
    float2 uv;
};

// Full screen triangle
vertex VertexOut vertexMain(uint vertexID [[vertex_id]]) {
    VertexOut out;
    float2 pos = float2((vertexID << 1) & 2, vertexID & 2);
    out.position = float4(pos * 2.0f - 1.0f, 0.0f, 1.0f);
    out.uv = float2(pos.x, 1.0f - pos.y);
    return out;
}

fragment float4 fragmentMain(VertexOut in [[stage_in]],
                             texture2d<float> ssrTexture [[texture(0)]]) {
    constexpr sampler s(mag_filter::linear, min_filter::linear);
    float4 ssr = ssrTexture.sample(s, in.uv);
    // Additive blending is done via pipeline state usually, 
    // but here we just output SSR color and alpha.
    // If pipeline is set to Additive (SrcAlpha, One), we can control strength via Alpha.
    return ssr; 
}
