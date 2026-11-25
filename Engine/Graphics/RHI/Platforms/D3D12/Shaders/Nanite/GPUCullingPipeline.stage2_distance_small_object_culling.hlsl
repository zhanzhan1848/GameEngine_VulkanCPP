// Vulkan reference: Nanite/GPUCullingPipeline.spv (stage2_distance_small_object_culling).
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

InstanceVis load_instance_vis(uint idx)
{
    InstanceVis v = _140;
    uint _142 = idx * 4u;
    v.is_visible = instance_visibility.Load((_142 + 0u) * 4);
    v.instance_index = instance_visibility.Load((_142 + 1u) * 4);
    v.lod_level = instance_visibility.Load((_142 + 2u) * 4);
    v.distance_rank = instance_visibility.Load((_142 + 3u) * 4);
    return v;
}

void store_instance_vis(uint idx, InstanceVis v)
{
    uint _169 = idx * 4u;
    instance_visibility.Store((_169 + 0u) * 4 + 0, v.is_visible);
    instance_visibility.Store((_169 + 1u) * 4 + 0, v.instance_index);
    instance_visibility.Store((_169 + 2u) * 4 + 0, v.lod_level);
    instance_visibility.Store((_169 + 3u) * 4 + 0, v.distance_rank);
}

[numthreads(64, 1, 1)]
void main(StageInput input)
{
    bool _439 = false;
    InstanceVis v = _442;
    if (!(input.dispatchThreadID.x >= uniforms_1_m0.instance_count))
    {
        _439 = input.dispatchThreadID.x >= 100000u;
    }
    else
    {
        _439 = true;
    }
    if (_439)
    {
        return;
    }
    InstanceVis _456 = load_instance_vis(input.dispatchThreadID.x);
    if (_456.is_visible == 0u)
    {
        return;
    }
    InstanceData _462;
    _462.world_matrix = asfloat(uint4x4(instances.Load4(input.dispatchThreadID.x * 192), instances.Load4(input.dispatchThreadID.x * 192 + 16), instances.Load4(input.dispatchThreadID.x * 192 + 32), instances.Load4(input.dispatchThreadID.x * 192 + 48)));
    _462.inverse_world_matrix = asfloat(uint4x4(instances.Load4(input.dispatchThreadID.x * 192 + 64), instances.Load4(input.dispatchThreadID.x * 192 + 80), instances.Load4(input.dispatchThreadID.x * 192 + 96), instances.Load4(input.dispatchThreadID.x * 192 + 112)));
    _462.meta0 = instances.Load4(input.dispatchThreadID.x * 192 + 128);
    _462.meta1 = instances.Load4(input.dispatchThreadID.x * 192 + 144);
    _462.bounds_center_x = asfloat(instances.Load(input.dispatchThreadID.x * 192 + 160));
    _462.bounds_center_y = asfloat(instances.Load(input.dispatchThreadID.x * 192 + 164));
    _462.bounds_center_z = asfloat(instances.Load(input.dispatchThreadID.x * 192 + 168));
    _462._pad_after_center = instances.Load(input.dispatchThreadID.x * 192 + 172);
    _462.bounds_radius = asfloat(instances.Load(input.dispatchThreadID.x * 192 + 176));
    _462._pad_br0 = instances.Load(input.dispatchThreadID.x * 192 + 180);
    _462._pad_br1 = instances.Load(input.dispatchThreadID.x * 192 + 184);
    _462._pad_br2 = instances.Load(input.dispatchThreadID.x * 192 + 188);
    v = _456;
    v.distance_rank = uint(clamp(max(0.0f, -mul(float4(_462.bounds_center_x, _462.bounds_center_y, _462.bounds_center_z, 1.0f), uniforms_1_m0.view_matrix).z) * 10.0f, 0.0f, 4294967040.0f));
    store_instance_vis(input.dispatchThreadID.x, v);
}
