#include <metal_stdlib>
using namespace metal;

struct VertexIn {
    float3 position [[attribute(0)]];
};

struct VertexInput {
    packed_float3 position;
    uint padding[5]; // 20 bytes padding to match 32-byte stride (12 pos + 20 element)
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
    constant VertexInput* vertices [[buffer(20)]]
) {
    VertexOut out;
    
    float3 rawPos = vertices[vertexId].position;
    out.position = pushConsts.mvp * float4(rawPos, 1.0);
    
    return out;
}
