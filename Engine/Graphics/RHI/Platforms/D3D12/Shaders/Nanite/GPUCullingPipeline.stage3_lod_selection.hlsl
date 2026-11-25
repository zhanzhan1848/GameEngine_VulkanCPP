// Vulkan reference: Nanite/GPUCullingPipeline.spv (stage3_lod_selection).
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

uint select_lod_level(float _distance, float screen_space_error, float lod_bias, uint max_lod_levels)
{
    float threshold = 0.0f;
    uint lod = 0u;
    uint2 loop_bound = uint2(4294967295u, 4294967295u);
    threshold = lod_bias;
    for (;;)
    {
        if (all(bool2(uint2(0u, 0u).x == loop_bound.x, uint2(0u, 0u).y == loop_bound.y)))
        {
            break;
        }
        loop_bound -= uint2(uint(loop_bound.y == 0u), 1u);
        if (!(lod < (max_lod_levels - 1u)))
        {
            break;
        }
        if (screen_space_error < threshold)
        {
            return lod;
        }
        threshold *= 2.0f;
        lod++;
        continue;
    }
    return max_lod_levels - 1u;
}

[numthreads(64, 1, 1)]
void main(StageInput input)
{
    InstanceVis v = _490;
    InstanceVis v_1 = _494;
    bool _487 = false;
    uint lod_level = 0u;
    if (!(input.dispatchThreadID.x >= uniforms_1_m0.instance_count))
    {
        _487 = input.dispatchThreadID.x >= 100000u;
    }
    else
    {
        _487 = true;
    }
    if (_487)
    {
        return;
    }
    if (uniforms_1_m0.enable_lod_selection == 0u)
    {
        v = load_instance_vis(input.dispatchThreadID.x);
        v.lod_level = 0u;
        store_instance_vis(input.dispatchThreadID.x, v);
        return;
    }
    InstanceVis _517 = load_instance_vis(input.dispatchThreadID.x);
    if (_517.is_visible == 0u)
    {
        return;
    }
    InstanceData _523;
    _523.world_matrix = asfloat(uint4x4(instances.Load4(input.dispatchThreadID.x * 192), instances.Load4(input.dispatchThreadID.x * 192 + 16), instances.Load4(input.dispatchThreadID.x * 192 + 32), instances.Load4(input.dispatchThreadID.x * 192 + 48)));
    _523.inverse_world_matrix = asfloat(uint4x4(instances.Load4(input.dispatchThreadID.x * 192 + 64), instances.Load4(input.dispatchThreadID.x * 192 + 80), instances.Load4(input.dispatchThreadID.x * 192 + 96), instances.Load4(input.dispatchThreadID.x * 192 + 112)));
    _523.meta0 = instances.Load4(input.dispatchThreadID.x * 192 + 128);
    _523.meta1 = instances.Load4(input.dispatchThreadID.x * 192 + 144);
    _523.bounds_center_x = asfloat(instances.Load(input.dispatchThreadID.x * 192 + 160));
    _523.bounds_center_y = asfloat(instances.Load(input.dispatchThreadID.x * 192 + 164));
    _523.bounds_center_z = asfloat(instances.Load(input.dispatchThreadID.x * 192 + 168));
    _523._pad_after_center = instances.Load(input.dispatchThreadID.x * 192 + 172);
    _523.bounds_radius = asfloat(instances.Load(input.dispatchThreadID.x * 192 + 176));
    _523._pad_br0 = instances.Load(input.dispatchThreadID.x * 192 + 180);
    _523._pad_br1 = instances.Load(input.dispatchThreadID.x * 192 + 184);
    _523._pad_br2 = instances.Load(input.dispatchThreadID.x * 192 + 188);
    float _533 = length(float3(_523.bounds_center_x, _523.bounds_center_y, _523.bounds_center_z) - uniforms_1_m0.camera_position.xyz);
    lod_level = select_lod_level(_533, 100.0f / max(_533, 0.001f), uniforms_1_m0.lod_bias, uniforms_1_m0.max_lod_levels);
    if (lod_level >= uniforms_1_m0.max_lod_levels)
    {
        lod_level = uniforms_1_m0.max_lod_levels - 1u;
    }
    v_1 = _517;
    v_1.lod_level = lod_level;
    store_instance_vis(input.dispatchThreadID.x, v_1);
}
