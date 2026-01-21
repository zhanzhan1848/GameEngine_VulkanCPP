#include <metal_stdlib>
using namespace metal;

struct VertexOut {
    float4 position [[position]];
    float2 uv;
};

vertex VertexOut bloom_vs(uint vertexID [[vertex_id]]) {
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

fragment float4 bright_pass_fs(VertexOut in [[stage_in]],
                               texture2d<float> inputTexture [[texture(0)]])
{
    constexpr sampler s(filter::linear, address::clamp_to_edge);
    float4 color = inputTexture.sample(s, in.uv);
    
    // Threshold
    float threshold = 1.0f;
    float brightness = dot(color.rgb, float3(0.2126, 0.7152, 0.0722));
    if (brightness > threshold) {
        return color;
    } else {
        return float4(0.0);
    }
}
