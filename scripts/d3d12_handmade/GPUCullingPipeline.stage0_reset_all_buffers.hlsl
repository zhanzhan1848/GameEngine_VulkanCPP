// GPUCullingPipeline.stage0_reset_all_buffers — R3c 手写 HLSL 直译
//
// Provenance: Engine/Graphics/Dawn/shaders/Nanite/GPUCullingPipeline.wgsl
//   fn stage0_reset_all_buffers (:200-232)，语义基准逐语句对照见 agent-S 报告。
// 为什么手写：naga spv 经 spirv-cross 的产物在 dxc 报
//   "use of undeclared identifier '_1090/_1091'"（naga 把 atomicStore 降级为
//   OpAtomicExchange，结果 SSA 未被 cross 物化为局部变量）。本文件按 WGSL
//   语义重写，原子写用 RWByteAddressBuffer::InterlockedExchange 3 参形态
//   （保旧值语义：dummy 丢弃）。
//
// 绑定寄存器 = 同文件已成功入口（stage1/5 等）的 cross 产物逐项核对一致：
//   WGSL @group(0)@binding(1) instance_visibility  → u1 (RWByteAddressBuffer)
//   WGSL @group(0)@binding(2) uniforms              → b2 (cbuffer, row_major 矩阵)
//   WGSL @group(0)@binding(4) cluster_visibility    → u4 (RWByteAddressBuffer)
//   WGSL @group(0)@binding(5) visible_counter       → u5 (atomic u32 @ byte 0)
//   WGSL @group(0)@binding(6) visible_cluster_list  → u6 (RWByteAddressBuffer)
//   WGSL @group(0)@binding(9) cluster_visibility_counter → u9 (atomic u32 @ 0)
// 未使用的绑定（0/3/7/8/10/11/12）不声明；C++ culling_descriptor_layout_ 13
// binding 表照写（描述符多余槽位 D3D12 合法）。

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
    uint _pad_cm0; uint _pad_cm1; uint _pad_cm2;  // WGSL 17 vec4s = 272B
};

RWByteAddressBuffer instance_visibility : register(u1);       // 4 u32/entry
RWByteAddressBuffer cluster_visibility : register(u4);        // 12 u32/entry
RWByteAddressBuffer visible_counter : register(u5);           // atomic u32
RWByteAddressBuffer visible_cluster_list : register(u6);      // u32/entry
RWByteAddressBuffer cluster_visibility_counter : register(u9); // atomic u32

static const uint MAX_INSTANCES = 100000u;

[numthreads(64, 1, 1)]
void main(uint3 gid : SV_DispatchThreadID)
{
    const uint idx = gid.x;

    // WGSL :204-207 — thread 0 clears the two atomic counters.
    if (idx == 0u) {
        uint dummy;
        visible_counter.InterlockedExchange(0, 0u, dummy);            // atomicStore
        cluster_visibility_counter.InterlockedExchange(0, 0u, dummy); // atomicStore
    }

    // WGSL :209-216 — clear per-instance visibility (4 u32s/entry).
    if (idx < instance_count && idx < MAX_INSTANCES) {
        const uint base = (idx * 4u) * 4u;  // byte offset of u32[idx*4+0]
        instance_visibility.Store(base + 0u, 0u);  // is_visible
        instance_visibility.Store(base + 4u, 0u);  // instance_index
        instance_visibility.Store(base + 8u, 0u);  // lod_level
        instance_visibility.Store(base + 12u, 0u); // distance_rank
    }

    // WGSL :219-227 — clear ClusterVisibility entries + sentinels (12 u32s/entry).
    if (idx < cluster_count) {
        const uint base = (idx * 12u) * 4u;
        cluster_visibility.Store(base + 0u, 0u);         // is_visible
        cluster_visibility.Store(base + 4u, 0xFFFFFFFFu); // cluster_index sentinel
        cluster_visibility.Store(base + 8u, 0u);          // instance_index
        cluster_visibility.Store(base + 12u, 0u);         // lod_level
        cluster_visibility.Store(base + 16u, 0xFFFFFFFFu);// frame_index sentinel
        // center/radius/pad left untouched — Stage 4 overwrites before reads.
    }

    // WGSL :229-231 — clear the visible cluster list.
    if (idx < cluster_count) {
        visible_cluster_list.Store(idx * 4u, 0u);
    }
}
