#include <metal_stdlib>
using namespace metal;

struct VertexOut {
    float4 position [[position]];
    float2 uv;
};

vertex VertexOut vertexDebug(uint vertexID [[vertex_id]]) {
    VertexOut out;
    out.uv = float2((vertexID << 1) & 2, vertexID & 2);
    out.position = float4(out.uv * 2.0f - 1.0f, 0.0f, 1.0f);
    return out;
}

fragment float4 fragmentDebug(VertexOut in [[stage_in]], texture2d<float> debugTexture [[texture(0)]]) {
    constexpr sampler s(min_filter::linear, mag_filter::linear);
    float4 color = debugTexture.sample(s, in.uv);
    return color;
}
