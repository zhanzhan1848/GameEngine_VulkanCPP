#include <metal_stdlib>
using namespace metal;

struct VertexIn {
    float3 position [[attribute(0)]];
};

struct VertexInput {
    packed_float3 position;
    uint padding[5]; // 20 bytes padding to match 32-byte stride (12 pos + 20 element)
};

struct ViewData {
    float4x4 viewProjection;
    float4x4 invViewProjection;
    float4x4 previousViewProjection;
};

struct PushConsts {
    float4x4 mvp;
    uint use_instances;
    uint _pad[3];
};

struct VertexOut {
    float4 position [[position]];
};

vertex VertexOut shadow_mapping_vs(
    uint vertexId [[vertex_id]],
    uint instanceId [[instance_id]],
    constant ViewData& viewData [[buffer(0)]],
    constant PushConsts& pushConsts [[buffer(2)]],
    constant float4x4* instanceModels [[buffer(3)]],
    constant VertexInput* vertices [[buffer(20)]]
) {
    VertexOut out;

    float3 rawPos = vertices[vertexId].position;

    float4x4 model = (pushConsts.use_instances != 0)
                     ? instanceModels[instanceId]
                     : pushConsts.mvp;

    float4 worldPos = model * float4(rawPos, 1.0);
    out.position = viewData.viewProjection * worldPos;

    return out;
}
