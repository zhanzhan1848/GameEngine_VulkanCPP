// ShadowCulling.shadow_finalize_indirect — R3c 手写 HLSL 直译
//
// Provenance: Engine/Graphics/Dawn/shaders/Nanite/ShadowCulling.wgsl
//   fn shadow_finalize_indirect (:127-146)，语义基准逐语句对照见 agent-S 报告。
// 为什么手写：naga spv 经 spirv-cross 产物在 dxc 报
//   "use of undeclared identifier '_302'"（同 stage0 的 AtomicExchange 根因）。
//
// 绑定寄存器 = 同文件已成功入口（shadow_cluster_culling）的 cross 产物
// 逐项核对一致：
//   @binding(3) uniforms (ShadowCullUniforms) → b3（本入口 WGSL 未读，不声明；
//     C++ shadow_cull_set_layout_ 7-binding 表照写，多余槽位合法）
//   @binding(4) visible_counter  → u4 (atomic u32 @ byte 0)
//   @binding(5) visible_clusters → u5 (RWByteAddressBuffer, u32/entry)
//   @binding(6) indirect_args    → u6 (RWByteAddressBuffer, 4 u32s)

RWByteAddressBuffer visible_counter : register(u4);
RWByteAddressBuffer visible_clusters : register(u5);
RWByteAddressBuffer indirect_args : register(u6);

[numthreads(64, 1, 1)]
void main(uint3 gid : SV_DispatchThreadID)
{
    // WGSL :129 — only thread 0 does work.
    if (gid.x != 0u) { return; }

    // WGSL :131 — count = atomicLoad(&visible_counter)。
    // cross 参照（stage5 产物 _57.InterlockedAdd(0, 0, _)）同款 3 参形态。
    uint count;
    visible_counter.InterlockedAdd(0, 0u, count);  // atomicLoad

    // WGSL :133-142 — insertion sort visible_clusters[0..count-1]
    // (deterministic ordering).
    for (uint i = 1u; i < count; i = i + 1u) {
        const uint key = visible_clusters.Load(i * 4u);
        int j = int(i) - 1;
        while (j >= 0 && visible_clusters.Load(uint(j) * 4u) > key) {
            visible_clusters.Store((uint(j) + 1u) * 4u,
                                   visible_clusters.Load(uint(j) * 4u));
            j = j - 1;
        }
        // WGSL u32(j)+1u：j=-1 时 u32 回绕 0xFFFFFFFF，+1 → 0（HLSL uint 同回绕）。
        visible_clusters.Store((uint(j) + 1u) * 4u, key);
    }

    // WGSL :144 — instance_count = count（indirect_args[1]）。
    indirect_args.Store(4u, count);

    // WGSL :145 — atomicStore(&visible_counter, 0)。3 参形态保旧值语义。
    uint dummy;
    visible_counter.InterlockedExchange(0, 0u, dummy);
}
