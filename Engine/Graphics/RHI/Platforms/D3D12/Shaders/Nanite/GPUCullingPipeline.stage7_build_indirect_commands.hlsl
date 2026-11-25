// Vulkan reference: Nanite/GPUCullingPipeline.spv (stage7_build_indirect_commands).
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
    uint visible_count = 0u;
    if (input.dispatchThreadID.x != 0u)
    {
        return;
    }
    uint _1037;
    visible_counter.InterlockedAdd(0, 0, _1037);
    visible_count = _1037;
    if (visible_count > uniforms_1_m0.cluster_count)
    {
        visible_count = uniforms_1_m0.cluster_count;
    }
    if (visible_count > 100000u)
    {
        visible_count = 100000u;
    }
    indirect_commands.Store(0, 384u);
    indirect_commands.Store(4, visible_count);
    indirect_commands.Store(8, 0u);
    indirect_commands.Store(12, 0u);
}
