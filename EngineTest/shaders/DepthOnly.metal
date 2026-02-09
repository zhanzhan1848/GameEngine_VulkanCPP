#include <metal_stdlib>
using namespace metal;

struct VertexIn {
    float3 position [[attribute(0)]];
};

struct VertexInput {
    packed_float3 position;
    packed_float3 normal;
    packed_float2 uv;
};

struct PushConsts {
    float4x4 mvp;
};

struct VertexOut {
    float4 position [[position]];
};

vertex VertexOut shadow_mapping_vs(
    uint vertexId [[vertex_id]],
    constant PushConsts& pushConsts [[buffer(2)]],
    device const VertexInput* vertices [[buffer(20)]]
) {
    VertexOut out;
    
    float3 rawPos = vertices[vertexId].position;
    out.position = pushConsts.mvp * float4(rawPos, 1.0);
    
    return out;
}
