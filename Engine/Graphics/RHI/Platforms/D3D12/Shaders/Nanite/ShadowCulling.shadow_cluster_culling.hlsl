// Vulkan reference: Nanite/ShadowCulling.spv (shadow_cluster_culling).
struct InstanceData
{
    row_major float4x4 world_matrix;
    row_major float4x4 inverse_world_matrix;
    uint4 meta0;
    uint4 meta1;
    float bounds_center_x;
    float bounds_center_y;
    float bounds_center_z;
    uint _pad_after_center;
    float bounds_radius;
    uint _pad_br0;
    uint _pad_br1;
    uint _pad_br2;
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

struct ShadowCullUniforms
{
    float4 frustum_planes[6];
    row_major float4x4 light_view_projection;
    uint4 counts;
};

ByteAddressBuffer instances : register(t0, space0);
ByteAddressBuffer meshlets : register(t1, space0);
ByteAddressBuffer cluster_map : register(t2, space0);
cbuffer uniforms : register(b3, space0)
{
    ShadowCullUniforms uniforms_1_m0 : packoffset(c0);
};

RWByteAddressBuffer visible_counter : register(u4, space0);
RWByteAddressBuffer visible_clusters : register(u5, space0);
RWByteAddressBuffer indirect_args : register(u6, space0);

struct StageInput
{
    uint3 dispatchThreadID : SV_DispatchThreadID;
};

bool sphere_vs_frustum(float3 center, float radius)
{
    int i = 0;
    uint2 loop_bound = uint2(4294967295u, 4294967295u);
    for (;;)
    {
        if (all(bool2(uint2(0u, 0u).x == loop_bound.x, uint2(0u, 0u).y == loop_bound.y)))
        {
            break;
        }
        loop_bound -= uint2(uint(loop_bound.y == 0u), 1u);
        if (!(i < 6))
        {
            break;
        }
        if (dot(float4(center, 1.0f), uniforms_1_m0.frustum_planes[i]) < (-radius))
        {
            return false;
        }
        i++;
        continue;
    }
    return true;
}

[numthreads(64, 1, 1)]
void main(StageInput input)
{
    uint c = 0u;
    uint2 loop_bound = uint2(4294967295u, 4294967295u);
    if (input.dispatchThreadID.x >= uniforms_1_m0.counts.x)
    {
        return;
    }
    InstanceData _135;
    _135.world_matrix = asfloat(uint4x4(instances.Load4(input.dispatchThreadID.x * 192), instances.Load4(input.dispatchThreadID.x * 192 + 16), instances.Load4(input.dispatchThreadID.x * 192 + 32), instances.Load4(input.dispatchThreadID.x * 192 + 48)));
    _135.inverse_world_matrix = asfloat(uint4x4(instances.Load4(input.dispatchThreadID.x * 192 + 64), instances.Load4(input.dispatchThreadID.x * 192 + 80), instances.Load4(input.dispatchThreadID.x * 192 + 96), instances.Load4(input.dispatchThreadID.x * 192 + 112)));
    _135.meta0 = instances.Load4(input.dispatchThreadID.x * 192 + 128);
    _135.meta1 = instances.Load4(input.dispatchThreadID.x * 192 + 144);
    _135.bounds_center_x = asfloat(instances.Load(input.dispatchThreadID.x * 192 + 160));
    _135.bounds_center_y = asfloat(instances.Load(input.dispatchThreadID.x * 192 + 164));
    _135.bounds_center_z = asfloat(instances.Load(input.dispatchThreadID.x * 192 + 168));
    _135._pad_after_center = instances.Load(input.dispatchThreadID.x * 192 + 172);
    _135.bounds_radius = asfloat(instances.Load(input.dispatchThreadID.x * 192 + 176));
    _135._pad_br0 = instances.Load(input.dispatchThreadID.x * 192 + 180);
    _135._pad_br1 = instances.Load(input.dispatchThreadID.x * 192 + 184);
    _135._pad_br2 = instances.Load(input.dispatchThreadID.x * 192 + 188);
    if (!sphere_vs_frustum(float3(_135.bounds_center_x, _135.bounds_center_y, _135.bounds_center_z), _135.bounds_radius))
    {
        return;
    }
    for (;;)
    {
        if (all(bool2(uint2(0u, 0u).x == loop_bound.x, uint2(0u, 0u).y == loop_bound.y)))
        {
            break;
        }
        loop_bound -= uint2(uint(loop_bound.y == 0u), 1u);
        if (!(c < _135.meta0.w))
        {
            break;
        }
        uint _170 = c;
        uint _171 = _135.meta0.z + _170;
        ClusterMap _174;
        _174.data = cluster_map.Load4(_171 * 16);
        MeshletData _179;
        _179.header = meshlets.Load4(_174.data.x * 64);
        _179.cone0 = asfloat(meshlets.Load4(_174.data.x * 64 + 16));
        _179.cone1 = asfloat(meshlets.Load4(_174.data.x * 64 + 32));
        _179.cone2 = asfloat(meshlets.Load4(_174.data.x * 64 + 48));
        if (!sphere_vs_frustum(mul(float4(float3(_179.cone1.w, _179.cone2.x, _179.cone2.y), 1.0f), _135.world_matrix).xyz, _179.cone2.z))
        {
            uint _199 = c;
            c = _199 + 1u;
            continue;
        }
        uint _197;
        visible_counter.InterlockedAdd(0, 1u, _197);
        visible_clusters.Store(_197 * 4 + 0, _171);
        uint _199 = c;
        c = _199 + 1u;
        continue;
    }
    if (input.dispatchThreadID.x == 0u)
    {
        indirect_args.Store(0, 378u);
        indirect_args.Store(4, 0u);
        indirect_args.Store(8, 0u);
        indirect_args.Store(12, 0u);
        return;
    }
    else
    {
        return;
    }
}
