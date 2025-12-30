// 文件说明: 实现 CPU Mesh 资产的优化骨架与打包上传到 GPU 的流程。
// 关键点: 严格遵循 ContentToEngine 的几何资源二进制格式，复用现有 graphics::add_submesh 路径与 D3D12/Vulkan/Metal 内容模块。

#include "MeshCPU.h"

namespace primal::content
{
    // 内部工具: 写入 u32 到 blob
    static inline void write_u32(utl::vector<u8>& out, u32 v)
    {
        const size_t p = out.size(); out.resize(p + sizeof(u32));
        memcpy(out.data() + p, &v, sizeof(u32));
    }

    // 内部工具: 16 字节对齐填充 0
    static inline void align16(utl::vector<u8>& out)
    {
        const u32 pad = (u32)((16 - (out.size() & 15)) & 15);
        if (pad) { const size_t p = out.size(); out.resize(p + pad); memset(out.data() + p, 0, pad); }
    }

    // 内部工具: 将一个子网格打包为 D3D12Content::submesh::add 期望的二进制布局，返回字节长度
    static u32 pack_submesh_blob(const SubmeshCPU& sub, utl::vector<u8>& out)
    {
        const u32 vtx_count = (u32)sub.positions.size();
        const u32 idx_count = (u32)sub.indices.size();
        const u32 elem_size = sub.element_size;
        const u32 elements_type = sub.elements_type;
        const u32 prim_topo = sub.primitive_topology;

        const size_t base = out.size();
        write_u32(out, elem_size);
        write_u32(out, vtx_count);
        write_u32(out, idx_count);
        write_u32(out, elements_type);
        write_u32(out, prim_topo);

        // 位置流
        if (vtx_count) {
            const size_t bytes = sizeof(math::v3) * vtx_count;
            const size_t p = out.size(); out.resize(p + bytes);
            memcpy(out.data() + p, sub.positions.data(), bytes);
            align16(out);
        }

        // 元素流
        if (elem_size && vtx_count) {
            const size_t bytes = (size_t)elem_size * vtx_count;
            const size_t p = out.size(); out.resize(p + bytes);
            memcpy(out.data() + p, sub.elements.data(), bytes);
            align16(out);
        }

        // 索引流（根据顶点数选择 16/32 位）
        if (idx_count) {
            const bool use16 = (vtx_count < (1u << 16));
            if (use16) {
                const size_t bytes = sizeof(u16) * idx_count;
                const size_t p = out.size(); out.resize(p + bytes);
                for (u32 i = 0; i < idx_count; ++i) {
                    const u16 v = (u16)sub.indices[i];
                    memcpy(out.data() + p + i * sizeof(u16), &v, sizeof(u16));
                }
            } else {
                const size_t bytes = sizeof(u32) * idx_count;
                const size_t p = out.size(); out.resize(p + bytes);
                memcpy(out.data() + p, sub.indices.data(), bytes);
            }
        }

        return (u32)(out.size() - base);
    }

    // 函数说明: 针对单个子网格进行索引缓存友好重排/量化等优化（示例骨架）
    void optimize_submesh(SubmeshCPU& submesh)
    {
        // 示例: 可接入 Tipsify/Forsyth 索引重排算法实现（避免引入第三方，建议自研简化版）
        // 示例: UV/法线量化到半精度或 10_10_10_2，更新 elements 与 element_size
        // 示例: 顶点重排映射应用到 positions 与 elements
        // 这里保留为空实现，作为扩展插桩点
        (void)submesh;
    }

    // 函数说明: 根据目标质量与预算生成 LOD（示例骨架）
    void generate_lods(MeshAsset& asset)
    {
        // 示例: 基于边坍塌/Quadric Error Metrics 的简化，生成多级 LOD 的 indices 与 positions/elements
        // 示例: 按场景距离阈值设置 LODCPU.threshold
        (void)asset;
    }

    // 函数说明: 构建 meshlet/cluster 数据（示例骨架）
    void build_meshlets(MeshAsset& asset)
    {
        // 示例: 将三角形分簇为 meshlets，计算各 meshlet 的包围锥与包围体，便于 GPU 剔除
        (void)asset;
    }

    // 函数说明: 构建空间加速结构（Octree/BVH/Voxel）（示例骨架）
    void build_spatial_structures(MeshAsset& asset)
    {
        // 示例: 用 AABB 层级构建 BVH（可复用 Graphics/Utilities/BVH.hpp）或 Octree/稀疏体素网格
        (void)asset;
    }

    // 函数说明: 将 MeshAsset 打包为 ContentToEngine 的几何资源二进制格式（含 16 字节对齐）
    void pack_geometry_resource_blob(const MeshAsset& asset, utl::vector<u8>& out_blob)
    {
        out_blob.clear();
        const u32 lod_count = (u32)asset.lods.size();
        assert(lod_count);

        // 写入 lod_count
        write_u32(out_blob, lod_count);

        // 预留 thresholds[lod_count] 与 lod_offsets[lod_count]
        const size_t thresholds_offset = out_blob.size();
        out_blob.resize(thresholds_offset + sizeof(f32) * lod_count);
        const size_t lod_offsets_offset = out_blob.size();
        out_blob.resize(lod_offsets_offset + sizeof(primal::content::lod_offset) * lod_count);

        // 记录 gpu_ids 将在 ContentToEngine::create_mesh_hierarchy 中填充，这里直接在几何资源中以 submesh 数据形式传入
        // 注意: ContentToEngine 会读取本二进制，遍历每个 LOD 的 submesh 数据并调用 graphics::add_submesh(at)

        u16 running_offset = 0;
        // 遍历 LOD
        for (u32 lod_idx = 0; lod_idx < lod_count; ++lod_idx) {
            const LODCPU& lod = asset.lods[lod_idx];
            // 写入阈值到预留区
            memcpy(out_blob.data() + thresholds_offset + sizeof(f32) * lod_idx, &lod.threshold, sizeof(f32));

            const u32 submesh_count = (u32)lod.submeshes.size();
            assert(submesh_count);

            // 写入 submesh_count
            write_u32(out_blob, submesh_count);

            // 计算并写入 size_of_submeshes（先占位，稍后回填）
            const size_t size_pos = out_blob.size();
            write_u32(out_blob, 0u);
            const size_t submesh_block_start = out_blob.size();

            // 写入当前 LOD 的 lod_offset
            primal::content::lod_offset lo{ running_offset, (u16)submesh_count };
            memcpy(out_blob.data() + lod_offsets_offset + sizeof(primal::content::lod_offset) * lod_idx, &lo, sizeof(lo));
            running_offset += (u16)submesh_count;

            // 依次写入每个子网格的二进制
            size_t bytes_written = 0;
            for (u32 si = 0; si < submesh_count; ++si) {
                bytes_written += (size_t)pack_submesh_blob(lod.submeshes[si], out_blob);
            }

            // 回填 size_of_submeshes
            const u32 size_of_submeshes = (u32)bytes_written;
            memcpy(out_blob.data() + size_pos, &size_of_submeshes, sizeof(u32));
        }
    }

    // 函数说明: 上传 CPU Mesh 资产到 GPU，返回 geometry_content_id（可用于 MeshComponent 绑定）
    id::id_type upload_mesh_asset_to_gpu(const MeshAsset& asset)
    {
        utl::vector<u8> blob;
        pack_geometry_resource_blob(asset, blob);
        // 通过 Content 层创建几何资源（内部会调用 graphics::add_submesh）
        return primal::content::create_resource(blob.data(), primal::content::asset_type::mesh);
    }
}