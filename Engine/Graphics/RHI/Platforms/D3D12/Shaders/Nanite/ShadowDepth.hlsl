// Vulkan reference: Nanite/ShadowDepth.spv (shadow_depth_vs).
struct InstanceData
{
    row_major float4x4 world_matrix;
    row_major float4x4 inverse_world_matrix;
    uint4 meta0;
    uint4 meta1;
    float3 bounds_center;
    float bounds_radius;
    uint4 _pad_br;
};

struct MeshletData
{
    uint4 header;
    float4 cone0;
    float4 cone1;
    float4 cone2;
};

struct ClusterMap
{
    uint4 data;
};

struct ShadowDepthUniforms
{
    row_major float4x4 light_view_projection;
    uint4 counts;
};

struct VSOut
{
    float4 position;
};

static const VSOut _98 = { 0.0f.xxxx };

cbuffer uniforms : register(b0, space0)
{
    ShadowDepthUniforms uniforms_1_m0 : packoffset(c0);
};

ByteAddressBuffer visible_clusters : register(t1, space0);
ByteAddressBuffer cluster_map : register(t2, space0);
ByteAddressBuffer instances : register(t3, space0);
ByteAddressBuffer meshlets : register(t4, space0);
ByteAddressBuffer meshlet_vertex_indices : register(t5, space0);
ByteAddressBuffer meshlet_triangle_indices : register(t6, space0);
ByteAddressBuffer vertex_positions : register(t7, space0);

struct StageInput
{
    uint vertexID : SV_VertexID;
    uint instanceID : SV_InstanceID;
};

struct StageOutput
{
    float4 positionCS : SV_Position;
};

uint naga_div(uint lhs, uint rhs)
{
    return lhs / ((rhs == 0u) ? 1u : rhs);
}

uint naga_mod(uint lhs, uint rhs)
{
    return lhs % ((rhs == 0u) ? 1u : rhs);
}

StageOutput main(StageInput input)
{
    StageOutput output = (StageOutput)0;
    VSOut _out = _98;
    float4 clip = 0.0f.xxxx;
    uint _103 = naga_div(uint(int(input.vertexID)), 3u);
    ClusterMap _110;
    _110.data = cluster_map.Load4(visible_clusters.Load(uint(int(input.instanceID)) * 4) * 16);
    InstanceData _117;
    _117.world_matrix = asfloat(uint4x4(instances.Load4(_110.data.y * 192), instances.Load4(_110.data.y * 192 + 16), instances.Load4(_110.data.y * 192 + 32), instances.Load4(_110.data.y * 192 + 48)));
    _117.inverse_world_matrix = asfloat(uint4x4(instances.Load4(_110.data.y * 192 + 64), instances.Load4(_110.data.y * 192 + 80), instances.Load4(_110.data.y * 192 + 96), instances.Load4(_110.data.y * 192 + 112)));
    _117.meta0 = instances.Load4(_110.data.y * 192 + 128);
    _117.meta1 = instances.Load4(_110.data.y * 192 + 144);
    _117.bounds_center = asfloat(instances.Load3(_110.data.y * 192 + 160));
    _117.bounds_radius = asfloat(instances.Load(_110.data.y * 192 + 172));
    _117._pad_br = instances.Load4(_110.data.y * 192 + 176);
    MeshletData _120;
    _120.header = meshlets.Load4(_110.data.x * 64);
    _120.cone0 = asfloat(meshlets.Load4(_110.data.x * 64 + 16));
    _120.cone1 = asfloat(meshlets.Load4(_110.data.x * 64 + 32));
    _120.cone2 = asfloat(meshlets.Load4(_110.data.x * 64 + 48));
    if (_103 >= _120.header.w)
    {
        _out.position = float4(0.0f, 0.0f, 2.0f, 1.0f);
        output.positionCS = _out.position;
        output.positionCS.y = -output.positionCS.y;
        return output;
    }
    uint _137 = (_120.header.y + (_103 * 3u)) + naga_mod(uint(int(input.vertexID)), 3u);
    uint _147 = _120.header.x + ((meshlet_triangle_indices.Load(naga_div(_137, 4u) * 4) >> (naga_mod(_137, 4u) * 8u)) & 255u);
    clip = mul(mul(float4(float3(asfloat(vertex_positions.Load((meshlet_vertex_indices.Load(_147 * 4) * 3u) * 4)), asfloat(vertex_positions.Load(((meshlet_vertex_indices.Load(_147 * 4) * 3u) + 1u) * 4)), asfloat(vertex_positions.Load(((meshlet_vertex_indices.Load(_147 * 4) * 3u) + 2u) * 4))), 1.0f), _117.world_matrix), uniforms_1_m0.light_view_projection);
    clip.z += 0.001f;
    _out.position = clip;
    output.positionCS = _out.position;
    output.positionCS.y = -output.positionCS.y;
    return output;
}
