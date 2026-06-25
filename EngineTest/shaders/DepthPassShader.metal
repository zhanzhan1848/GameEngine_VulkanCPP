#include "Common.h"

struct VertexPosition
{
    device float3* positions;
};

struct VertexOut
{
    float4 HomogeneousPosition [[position]];	
	uint   layer [[render_target_array_index]];
};

struct VertexElement
{
    uint            ColorTSign;
    packed_ushort2         Normal;
    packed_ushort2         Tangent;
    packed_float2          UV;
};

struct GlobalData
{
    device const GlobalShaderData* global_shader_data [[id(0)]];
    device const PerObjectData* per_object_data [[id(1)]];
    device const packed_float3* vertices [[id(2)]];
    device const VertexElement* elements [[id(3)]];
    device const uint* srv_indices [[id(4)]];
    device const DirectionalLightParameters* directional_light_params [[id(5)]];
};

VertexOut vertex shadow_mapping_vs(device const GlobalData* global_data [[buffer(0)]],
                             uint vertex_index [[vertex_id]],
                             uint instanceID [[instance_id]])
{
    VertexOut vsOut;
    float4 pos = float4(global_data->vertices[vertex_index], 1.f);
    vsOut.HomogeneousPosition = global_data->directional_light_params[instanceID].LightMVP * pos;
    vsOut.layer = instanceID;
    return vsOut;
}