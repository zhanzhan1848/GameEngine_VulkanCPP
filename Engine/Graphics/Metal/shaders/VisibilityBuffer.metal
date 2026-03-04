#include <metal_stdlib>
using namespace metal;

struct VertexIn {
    float3 position [[attribute(0)]];
};

struct VertexOut {
    float4 position [[position]];
    uint instanceID;
};

struct PerObjectData {
    float4x4 worldViewProjection;
};

struct GlobalShaderData {
    float4x4 viewProjection;
};

vertex VertexOut visibility_vs(VertexIn in [[stage_in]],
                               constant PerObjectData* perObject [[buffer(1)]],
                               uint instanceID [[instance_id]])
{
    VertexOut out;
    out.position = perObject[instanceID].worldViewProjection * float4(in.position, 1.0);
    out.instanceID = instanceID;
    return out;
}

struct FragmentOut {
    uint visibility [[color(0)]];
};

fragment FragmentOut visibility_fs(VertexOut in [[stage_in]],
                                   uint primitiveID [[primitive_id]])
{
    FragmentOut out;
    // Pack InstanceID and PrimitiveID
    // Assuming 16 bits for each
    out.visibility = (in.instanceID << 16) | (primitiveID & 0xFFFF);
    return out;
}
