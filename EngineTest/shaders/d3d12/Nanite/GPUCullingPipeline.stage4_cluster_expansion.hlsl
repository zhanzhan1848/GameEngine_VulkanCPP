// GPUCullingPipeline.stage4_cluster_expansion — R3c 手写 HLSL 直译
//
// Provenance: Engine/Graphics/Dawn/shaders/Nanite/GPUCullingPipeline.wgsl
//   fn stage4_cluster_expansion (:374-469)，语义基准逐语句对照见 agent-S 报告。
// 为什么手写：同 stage0（naga AtomicExchange → cross 产物 dxc 报未声明标识符）。
//
// 绑定寄存器 = 同文件已成功入口的 cross 产物逐项核对一致：
//   @binding(0) instances              → t0 (ByteAddressBuffer, 192B InstanceData)
//   @binding(1) instance_visibility    → u1 (RWByteAddressBuffer, 4 u32/entry)
//   @binding(2) uniforms               → b2 (cbuffer, row_major 矩阵)
//   @binding(4) cluster_visibility     → u4 (RWByteAddressBuffer, 12 u32/entry)
//   @binding(9) cluster_visibility_counter → u9 (atomic u32 @ 0)
//   @binding(11) meshlets              → t11 (ByteAddressBuffer, 64B MeshletData)
//   @binding(12) cluster_map           → t12 (ByteAddressBuffer, 16B ClusterMapEntry)
//
// 内存布局（与 C++ InstanceData/RHIMeshlet、naga spv 的 cross 产物一致）：
//   InstanceData: world @0 (4×16B 列主序), inverse @64, meta0 @128 (uint4),
//                 meta1 @144 (uint4), center_xyz @160/164/168, pad @172,
//                 radius @176, pad @180-188。
//   MeshletData:  header @0, cone0 @16, cone1 @32 (.w=center.x), cone2 @48
//                 (.x=center.y, .y=center.z, .z=radius)。
//     ——注意：WGSL 注释说 cone1/cone2 是"vec4 字段"，naga 把 WGSL struct
//     数组元素拍平后每字段 16B 步进（header/cone0/cone1/cone2 依次
//     0/16/32/48），与 C++ RHIMeshlet 64B 对齐一致。
//   ClusterMapEntry: (globalMeshletIndex, instanceIndex, materialID, pad) @0。

cbuffer CullingUniforms : register(b2)
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
    uint _pad_cm0; uint _pad_cm1; uint _pad_cm2;
};

ByteAddressBuffer instances : register(t0);
RWByteAddressBuffer instance_visibility : register(u1);
RWByteAddressBuffer cluster_visibility : register(u4);
RWByteAddressBuffer cluster_visibility_counter : register(u9);
ByteAddressBuffer meshlets : register(t11);
ByteAddressBuffer cluster_map : register(t12);

static const uint MAX_CLUSTERS_PER_INSTANCE = 256u;
static const uint MAX_INSTANCES = 100000u;

// WGSL :180-188 load_instance_vis —— 只取需要字段。
uint load_instance_vis_is_visible(uint idx)
{
    return instance_visibility.Load((idx * 4u) * 4u);
}
uint load_instance_vis_lod_level(uint idx)
{
    return instance_visibility.Load((idx * 4u + 2u) * 4u);
}

// WGSL :359-372 store_cluster_vis —— 12 u32s/entry 打包写。
void store_cluster_vis(uint write_pos, uint is_visible, uint cluster_index,
                       uint instance_index, uint lod_level, uint frame_idx,
                       float3 center, float radius)
{
    const uint base = (write_pos * 12u) * 4u;
    cluster_visibility.Store(base + 0u, is_visible);
    cluster_visibility.Store(base + 4u, cluster_index);
    cluster_visibility.Store(base + 8u, instance_index);
    cluster_visibility.Store(base + 12u, lod_level);
    cluster_visibility.Store(base + 16u, frame_idx);
    cluster_visibility.Store(base + 20u, asuint(center.x));
    cluster_visibility.Store(base + 24u, asuint(center.y));
    cluster_visibility.Store(base + 28u, asuint(center.z));
    cluster_visibility.Store(base + 32u, asuint(radius));
}

[numthreads(64, 1, 1)]
void main(uint3 gid : SV_DispatchThreadID)
{
    // WGSL :377-379 — single-threaded deterministic expansion (Matches Metal:766).
    if (gid.x != 0u) {
        return;
    }

    uint write_pos = 0u;
    const bool fpa = (force_pass_all != 0u);  // WGSL :382

    // WGSL :384-466 — for instance_id in 0..instance_count.
    for (uint instance_id = 0u;
         instance_id < instance_count && instance_id < MAX_INSTANCES;
         instance_id = instance_id + 1u) {

        // WGSL :389-394 — skip culled instances unless force_pass_all.
        if (!fpa) {
            if (load_instance_vis_is_visible(instance_id) == 0u) {
                continue;
            }
        }

        const uint inst_base = instance_id * 192u;
        // WGSL :397-400 — lod_level only needed when !force_pass_all.
        uint lod_level = 0u;
        if (!fpa) {
            lod_level = load_instance_vis_lod_level(instance_id);
        }

        // WGSL :402-405 — cluster_count guard.
        const uint4 meta0 = instances.Load4(inst_base + 128u);
        const uint cluster_count_i = meta0.w;
        if (cluster_count_i > MAX_CLUSTERS_PER_INSTANCE) {
            continue;
        }
        // WGSL :406 — global_cluster_base.
        const uint4 meta1 = instances.Load4(inst_base + 144u);
        const uint global_cluster_base = meta1.x;

        // WGSL :408-465 — per-cluster expansion.
        for (uint i = 0u; i < cluster_count_i; i = i + 1u) {
            const uint cluster_map_idx = global_cluster_base + i;
            if (cluster_map_idx >= cluster_count) {
                continue;
            }

            // WGSL :424-442 — per-cluster bounds via cluster_map → meshlets,
            // fallback to instance bounds (first-frame defense).
            const float inst_cx = asfloat(instances.Load(inst_base + 160u));
            const float inst_cy = asfloat(instances.Load(inst_base + 164u));
            const float inst_cz = asfloat(instances.Load(inst_base + 168u));
            const float inst_radius = asfloat(instances.Load(inst_base + 176u));
            float3 cluster_world_center = float3(inst_cx, inst_cy, inst_cz);
            float cluster_world_radius = inst_radius;

            const uint4 cmap_entry = cluster_map.Load4(cluster_map_idx * 16u);
            const uint meshlet_idx = cmap_entry.x;
            if (meshlet_idx < total_meshlet_count) {
                const uint m_base = meshlet_idx * 64u;
                const float4 cone1 = asfloat(meshlets.Load4(m_base + 32u));
                const float4 cone2 = asfloat(meshlets.Load4(m_base + 48u));
                const float3 local_center = float3(cone1.w, cone2.x, cone2.y);
                const float local_radius = cone2.z;
                // WGSL 列主序 world_matrix：naga 内存按列存（col0 @0, col1 @16,
                // col2 @32, col3 @48）。逐列读出后按列构造 float4x4，mul(M, v)
                // 即 WGSL 的 inst.world_matrix * vec4(center, 1.0)。
                const float4x4 wm = float4x4(
                    asfloat(instances.Load4(inst_base + 0u)),
                    asfloat(instances.Load4(inst_base + 16u)),
                    asfloat(instances.Load4(inst_base + 32u)),
                    asfloat(instances.Load4(inst_base + 48u)));
                cluster_world_center = mul(wm, float4(local_center, 1.0f)).xyz;
                // WGSL :437-441 — radius scales by max axis scale of world matrix.
                const float col0_len = length(wm[0].xyz);  // 数学列向量 = 内存列
                const float col1_len = length(wm[1].xyz);
                const float col2_len = length(wm[2].xyz);
                const float max_scale = max(max(col0_len, col1_len), col2_len);
                cluster_world_radius = local_radius * max_scale;
            }

            // WGSL :447-454 — per-cluster far-plane cull only.
            bool cluster_visible = true;
            if (!fpa) {
                const float4 view_center_4 = mul(view_matrix,
                    float4(cluster_world_center, 1.0f));
                if (view_center_4.z < -far_plane - cluster_world_radius) {
                    cluster_visible = false;
                }
            }

            // WGSL :456-464.
            if (cluster_visible) {
                if (write_pos >= cluster_count) {
                    continue;
                }
                store_cluster_vis(write_pos, 1u, cluster_map_idx, instance_id,
                                  lod_level, frame_index,
                                  cluster_world_center, cluster_world_radius);
                write_pos = write_pos + 1u;
            }
        }
    }

    // WGSL :468 — publish the expanded cluster count.
    uint dummy;
    cluster_visibility_counter.InterlockedExchange(0, write_pos, dummy);  // atomicStore
}
