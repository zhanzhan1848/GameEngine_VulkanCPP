// Vulkan reference: Nanite/GPUCullingPipeline.spv (stage5_occlusion_culling).
#include "../Common/TextureDimensions.hlsli"

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

struct ClusterRef
{
    uint geometry_id;
    uint cluster_index;
    uint meshlet_id;
    uint _pad;
};

struct ClusterMapEntry
{
    uint globalMeshletIndex;
    uint instanceIndex;
    uint materialID;
    uint _pad;
};

struct CullingUniforms
{
    row_major float4x4 view_matrix;
    row_major float4x4 projection_matrix;
    row_major float4x4 view_projection_matrix;
    float4 camera_position;
    float near_plane;
    float far_plane;
    uint frame_index;
    uint enable_occlusion_culling;
    uint enable_lod_selection;
    uint enable_small_object_culling;
    float small_object_threshold;
    float lod_bias;
    uint max_lod_levels;
    uint instance_count;
    uint cluster_count;
    uint force_pass_all;
    uint enable_debug_output;
    uint total_meshlet_count;
    uint _pad_cm0;
    uint _pad_cm1;
    uint _pad_cm2;
};

struct ClusterVisibility
{
    uint is_visible;
    uint cluster_index;
    uint instance_index;
    uint lod_level;
    uint frame_index;
    float center_x;
    float center_y;
    float center_z;
    float cluster_radius;
    uint _pad0;
    uint _pad1;
    uint _pad2;
};

struct InstanceVis
{
    uint is_visible;
    uint instance_index;
    uint lod_level;
    uint distance_rank;
};

static const float4 _100[6] = { 0.0f.xxxx, 0.0f.xxxx, 0.0f.xxxx, 0.0f.xxxx, 0.0f.xxxx, 0.0f.xxxx };
static const InstanceVis _140 = { 0u, 0u, 0u, 0u };
static const InstanceVis _292 = { 0u, 0u, 0u, 0u };
static const InstanceVis _356 = { 0u, 0u, 0u, 0u };
static const InstanceVis _358 = { 0u, 0u, 0u, 0u };
static const InstanceVis _360 = { 0u, 0u, 0u, 0u };
static const InstanceVis _442 = { 0u, 0u, 0u, 0u };
static const InstanceVis _490 = { 0u, 0u, 0u, 0u };
static const InstanceVis _494 = { 0u, 0u, 0u, 0u };

ByteAddressBuffer instances : register(t0, space0);
RWByteAddressBuffer instance_visibility : register(u1, space0);
cbuffer uniforms : register(b2, space0)
{
    CullingUniforms uniforms_1_m0 : packoffset(c0);
};

ByteAddressBuffer cluster_refs : register(t3, space0);
RWByteAddressBuffer cluster_visibility : register(u4, space0);
RWByteAddressBuffer visible_counter : register(u5, space0);
RWByteAddressBuffer visible_cluster_list : register(u6, space0);
RWByteAddressBuffer indirect_commands : register(u7, space0);
RWByteAddressBuffer cluster_visibility_counter : register(u9, space0);
ByteAddressBuffer meshlets : register(t11, space0);
ByteAddressBuffer cluster_map : register(t12, space0);
Texture2D<float4> hzb_texture : register(t8, space0);

struct StageInput
{
    uint3 dispatchThreadID : SV_DispatchThreadID;
};

[numthreads(64, 1, 1)]
void main(StageInput input)
{
    float min_occlusion = 0.1f;
    bool _792 = false;
    bool _786 = false;
    bool _798 = false;
    int selected_mip = 0;
    bool _788 = false;
    float dist_factor = 1.0f;
    bool _790 = false;
    uint _802;
    cluster_visibility_counter.InterlockedAdd(0, 0, _802);
    if (input.dispatchThreadID.x >= _802)
    {
        return;
    }
    uint _806 = input.dispatchThreadID.x * 12u;
    if (!(cluster_visibility.Load((_806 + 0u) * 4) == 0u))
    {
        _786 = uniforms_1_m0.enable_occlusion_culling == 0u;
    }
    else
    {
        _786 = true;
    }
    if (_786)
    {
        return;
    }
    float3 _833 = float3(asfloat(cluster_visibility.Load((_806 + 5u) * 4)), asfloat(cluster_visibility.Load((_806 + 6u) * 4)), asfloat(cluster_visibility.Load((_806 + 7u) * 4)));
    float _837 = asfloat(cluster_visibility.Load((_806 + 8u) * 4));
    float4 _841 = mul(float4(_833, 1.0f), uniforms_1_m0.view_projection_matrix);
    if (_841.w <= 0.0f)
    {
        return;
    }
    float2 _849 = _841.xy / _841.w.xx;
    float2 _856 = float2((_849.x * 0.5f) + 0.5f, 0.5f - (_849.y * 0.5f));
    if (!(_856.x < 0.0f))
    {
        _788 = _856.x > 1.0f;
    }
    else
    {
        _788 = true;
    }
    if (!_788)
    {
        _790 = _856.y < 0.0f;
    }
    else
    {
        _790 = true;
    }
    if (!_790)
    {
        _792 = _856.y > 1.0f;
    }
    else
    {
        _792 = true;
    }
    if (_792)
    {
        return;
    }
    if ((_837 / _841.w) < 0.08f)
    {
        return;
    }
    float _891 = length(_833 - uniforms_1_m0.camera_position.xyz);
    if (_891 < 5.0f)
    {
        selected_mip = 0;
    }
    else
    {
        if (_891 < 15.0f)
        {
            selected_mip = 1;
        }
        else
        {
            if (_891 < 30.0f)
            {
                selected_mip = 2;
            }
            else
            {
                selected_mip = 3;
            }
        }
    }

    uint2 _905 = textureDimensions(hzb_texture, uint(selected_mip));
    float4 _922 = hzb_texture.Load(int3(int2(uint2(uint(clamp(_856.x * float(_905.x), 0.0f, 4294967040.0f)), uint(clamp(_856.y * float(_905.y), 0.0f, 4294967040.0f)))), selected_mip));
    float _923 = _922.x;
    float _926 = _841.z / _841.w;
    if (_891 < 10.0f)
    {
        dist_factor = 2.0f;
    }
    else
    {
        if (_891 < 25.0f)
        {
            dist_factor = 1.5f;
        }
    }
    if (_923 < (_926 - (((0.02f * dist_factor) * (1.0f + (float(selected_mip) * 0.2f))) + (_837 * 0.15f))))
    {
        if (_891 < 8.0f)
        {
            min_occlusion = 0.3f;
        }
        else
        {
            if (_891 < 20.0f)
            {
                min_occlusion = 0.2f;
            }
        }
        if ((_926 - _923) > min_occlusion)
        {
            if (_891 < 5.0f)
            {
                _798 = selected_mip > 0;
            }
            else
            {
                _798 = false;
            }
            if (_798)
            {
                return;
            }
            cluster_visibility.Store((_806 + 0u) * 4 + 0, 0u);
            return;
        }
        else
        {
            return;
        }
    }
    else
    {
        return;
    }
}
