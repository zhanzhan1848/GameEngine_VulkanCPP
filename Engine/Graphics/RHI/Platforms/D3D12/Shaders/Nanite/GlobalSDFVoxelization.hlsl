// Vulkan reference: Nanite/GlobalSDFVoxelization.spv (voxelize_sdf).
struct CascadeUniforms
{
    float4 origin;
    uint4 params;
};

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

cbuffer cascade : register(b1, space0)
{
    CascadeUniforms cascade_1_m0 : packoffset(c0);
};

ByteAddressBuffer vertex_positions : register(t2, space0);
ByteAddressBuffer meshlets : register(t3, space0);
ByteAddressBuffer meshlet_vertex_indices : register(t4, space0);
ByteAddressBuffer meshlet_triangle_indices : register(t5, space0);
ByteAddressBuffer cluster_map : register(t6, space0);
ByteAddressBuffer instance_data : register(t7, space0);
RWTexture3D<float> sdf_output : register(u0, space0);

struct StageInput
{
    uint3 dispatchThreadID : SV_DispatchThreadID;
};

uint naga_div(uint lhs, uint rhs)
{
    return lhs / ((rhs == 0u) ? 1u : rhs);
}

uint naga_mod(uint lhs, uint rhs)
{
    return lhs % ((rhs == 0u) ? 1u : rhs);
}

float point_to_triangle_distance(float3 p, float3 a, float3 b, float3 c)
{
    bool _68 = false;
    bool _62 = false;
    bool _55 = false;
    bool _70 = false;
    bool _64 = false;
    bool _58 = false;
    bool _72 = false;
    bool _66 = false;
    bool _60 = false;
    float3 _75 = b - a;
    float3 _76 = c - a;
    float3 _77 = p - a;
    float _78 = dot(_75, _77);
    float _79 = dot(_76, _77);
    if (_78 <= 0.0f)
    {
        _55 = _79 <= 0.0f;
    }
    else
    {
        _55 = false;
    }
    if (_55)
    {
        return length(_77);
    }
    float3 _89 = p - b;
    float _90 = dot(_75, _89);
    float _91 = dot(_76, _89);
    if (_90 >= 0.0f)
    {
        _58 = _91 <= _90;
    }
    else
    {
        _58 = false;
    }
    if (_58)
    {
        return length(_89);
    }
    float _103 = (_78 * _91) - (_90 * _79);
    if (_103 <= 0.0f)
    {
        _60 = _78 >= 0.0f;
    }
    else
    {
        _60 = false;
    }
    if (_60)
    {
        _62 = _90 <= 0.0f;
    }
    else
    {
        _62 = false;
    }
    if (_62)
    {
        return length(_77 + (_75 * (_78 / (_78 - _90))));
    }
    float3 _122 = p - c;
    float _123 = dot(_75, _122);
    float _124 = dot(_76, _122);
    if (_124 >= 0.0f)
    {
        _64 = _123 <= _124;
    }
    else
    {
        _64 = false;
    }
    if (_64)
    {
        return length(_122);
    }
    float _136 = (_123 * _79) - (_78 * _124);
    if (_136 <= 0.0f)
    {
        _66 = _79 >= 0.0f;
    }
    else
    {
        _66 = false;
    }
    if (_66)
    {
        _68 = _124 <= 0.0f;
    }
    else
    {
        _68 = false;
    }
    if (_68)
    {
        return length(_77 + (_76 * (_79 / (_79 - _124))));
    }
    float _157 = (_90 * _124) - (_123 * _91);
    if (_157 <= 0.0f)
    {
        _70 = (_91 - _90) >= 0.0f;
    }
    else
    {
        _70 = false;
    }
    if (_70)
    {
        _72 = (_123 - _124) >= 0.0f;
    }
    else
    {
        _72 = false;
    }
    if (_72)
    {
        return length((b + ((c - b) * ((_91 - _90) / ((_91 - _90) + (_123 - _124))))) - p);
    }
    float _185 = 1.0f / ((_157 + _136) + _103);
    return length(((_77 + (_75 * (_136 * _185))) + (_76 * (_103 * _185))) - p);
}

[numthreads(4, 4, 4)]
void main(StageInput input)
{
    uint inst_idx = 0u;
    float min_dist = 1000000.0f;
    uint2 loop_bound = uint2(4294967295u, 4294967295u);
    uint2 loop_bound_1 = uint2(4294967295u, 4294967295u);
    uint2 loop_bound_2 = uint2(4294967295u, 4294967295u);
    if (any(bool3(input.dispatchThreadID.x >= cascade_1_m0.params.xyz.x, input.dispatchThreadID.y >= cascade_1_m0.params.xyz.y, input.dispatchThreadID.z >= cascade_1_m0.params.xyz.z)))
    {
        return;
    }
    float3 _272 = cascade_1_m0.origin.xyz + ((float3(input.dispatchThreadID) + 0.5f.xxx) * cascade_1_m0.origin.w);
    for (;;)
    {
        if (all(bool2(uint2(0u, 0u).x == loop_bound.x, uint2(0u, 0u).y == loop_bound.y)))
        {
            break;
        }
        loop_bound -= uint2(uint(loop_bound.y == 0u), 1u);
        if (!(inst_idx < cascade_1_m0.params.w))
        {
            break;
        }
        if (min_dist <= 0.0f)
        {
            break;
        }
        InstanceData _307;
        _307.world_matrix = asfloat(uint4x4(instance_data.Load4(inst_idx * 192), instance_data.Load4(inst_idx * 192 + 16), instance_data.Load4(inst_idx * 192 + 32), instance_data.Load4(inst_idx * 192 + 48)));
        _307.inverse_world_matrix = asfloat(uint4x4(instance_data.Load4(inst_idx * 192 + 64), instance_data.Load4(inst_idx * 192 + 80), instance_data.Load4(inst_idx * 192 + 96), instance_data.Load4(inst_idx * 192 + 112)));
        _307.meta0 = instance_data.Load4(inst_idx * 192 + 128);
        _307.meta1 = instance_data.Load4(inst_idx * 192 + 144);
        _307.bounds_center_x = asfloat(instance_data.Load(inst_idx * 192 + 160));
        _307.bounds_center_y = asfloat(instance_data.Load(inst_idx * 192 + 164));
        _307.bounds_center_z = asfloat(instance_data.Load(inst_idx * 192 + 168));
        _307._pad_after_center = instance_data.Load(inst_idx * 192 + 172);
        _307.bounds_radius = asfloat(instance_data.Load(inst_idx * 192 + 176));
        _307._pad_br0 = instance_data.Load(inst_idx * 192 + 180);
        _307._pad_br1 = instance_data.Load(inst_idx * 192 + 184);
        _307._pad_br2 = instance_data.Load(inst_idx * 192 + 188);
        if ((length(_272 - float3(_307.bounds_center_x, _307.bounds_center_y, _307.bounds_center_z)) - _307.bounds_radius) > (cascade_1_m0.origin.w * 4.0f))
        {
            uint _511 = inst_idx;
            inst_idx = _511 + 1u;
            continue;
        }
        float _337 = max(max(length(_307.world_matrix[0].xyz), length(_307.world_matrix[1].xyz)), length(_307.world_matrix[2].xyz));
        uint c = 0u;
        for (;;)
        {
            if (all(bool2(uint2(0u, 0u).x == loop_bound_1.x, uint2(0u, 0u).y == loop_bound_1.y)))
            {
                break;
            }
            loop_bound_1 -= uint2(uint(loop_bound_1.y == 0u), 1u);
            if (!(c < _307.meta0.w))
            {
                break;
            }
            ClusterMap _363;
            _363.data = cluster_map.Load4((_307.meta0.z + c) * 16);
            MeshletData _368;
            _368.header = meshlets.Load4(_363.data.x * 64);
            _368.cone0 = asfloat(meshlets.Load4(_363.data.x * 64 + 16));
            _368.cone1 = asfloat(meshlets.Load4(_363.data.x * 64 + 32));
            _368.cone2 = asfloat(meshlets.Load4(_363.data.x * 64 + 48));
            if ((length(_272 - mul(float4(float3(_368.cone1.w, _368.cone2.x, _368.cone2.y), 1.0f), _307.world_matrix).xyz) - (_368.cone2.z * _337)) > (cascade_1_m0.origin.w * 2.0f))
            {
                uint _509 = c;
                c = _509 + 1u;
                continue;
            }
            uint t = 0u;
            for (;;)
            {
                if (all(bool2(uint2(0u, 0u).x == loop_bound_2.x, uint2(0u, 0u).y == loop_bound_2.y)))
                {
                    break;
                }
                loop_bound_2 -= uint2(uint(loop_bound_2.y == 0u), 1u);
                if (!(t < _368.header.w))
                {
                    break;
                }
                uint _419 = _368.header.y + (t * 3u);
                uint _446 = _368.header.x + ((meshlet_triangle_indices.Load(naga_div(_419, 4u) * 4) >> (naga_mod(_419, 4u) * 8u)) & 255u);
                uint _449 = _368.header.x + ((meshlet_triangle_indices.Load(naga_div(_419 + 1u, 4u) * 4) >> (naga_mod(_419 + 1u, 4u) * 8u)) & 255u);
                uint _452 = _368.header.x + ((meshlet_triangle_indices.Load(naga_div(_419 + 2u, 4u) * 4) >> (naga_mod(_419 + 2u, 4u) * 8u)) & 255u);
                min_dist = min(min_dist, point_to_triangle_distance(_272, mul(float4(float3(asfloat(vertex_positions.Load((meshlet_vertex_indices.Load(_446 * 4) * 3u) * 4)), asfloat(vertex_positions.Load(((meshlet_vertex_indices.Load(_446 * 4) * 3u) + 1u) * 4)), asfloat(vertex_positions.Load(((meshlet_vertex_indices.Load(_446 * 4) * 3u) + 2u) * 4))), 1.0f), _307.world_matrix).xyz, mul(float4(float3(asfloat(vertex_positions.Load((meshlet_vertex_indices.Load(_449 * 4) * 3u) * 4)), asfloat(vertex_positions.Load(((meshlet_vertex_indices.Load(_449 * 4) * 3u) + 1u) * 4)), asfloat(vertex_positions.Load(((meshlet_vertex_indices.Load(_449 * 4) * 3u) + 2u) * 4))), 1.0f), _307.world_matrix).xyz, mul(float4(float3(asfloat(vertex_positions.Load((meshlet_vertex_indices.Load(_452 * 4) * 3u) * 4)), asfloat(vertex_positions.Load(((meshlet_vertex_indices.Load(_452 * 4) * 3u) + 1u) * 4)), asfloat(vertex_positions.Load(((meshlet_vertex_indices.Load(_452 * 4) * 3u) + 2u) * 4))), 1.0f), _307.world_matrix).xyz));
                t++;
                continue;
            }
            uint _509 = c;
            c = _509 + 1u;
            continue;
        }
        uint _511 = inst_idx;
        inst_idx = _511 + 1u;
        continue;
    }
    sdf_output[int3(input.dispatchThreadID)] = float4(min_dist, 0.0f, 0.0f, 0.0f).x;
}
