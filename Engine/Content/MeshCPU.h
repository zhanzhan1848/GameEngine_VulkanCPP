// 文件说明: 声明 CPU 侧 Mesh 资产结构与 CPU→GPU 打包上传框架。
// 目标: 在 CPU 保留网格副本以便优化（缓存重排、LOD、meshlet、octree、BVH、voxel），需要时打包为引擎现有几何资源格式并上传。

#pragma once

#include "Common/CommonHeaders.h"
#include "Graphics/Renderer.h"
#include "Content/ContentToEngine.h"

namespace primal::content
{
    // 说明: 单个子网格的 CPU 数据（位置流 + 其他元素流 + 索引）
    struct SubmeshCPU
    {
        utl::vector<math::v3>     positions;
        utl::vector<u8>           elements;       // 每顶点按 element_size 串行排列
        utl::vector<u32>          indices;        // 存储为 u32，打包时根据顶点数选择 16/32 位输出
        u32                       element_size{0};
        u32                       elements_type{0};
        u32                       primitive_topology{ graphics::primitive_topology::triangle_list };
    };

    // 说明: 一个 LOD，包含阈值与若干子网格
    struct LODCPU
    {
        f32                       threshold{0.f};
        utl::vector<SubmeshCPU>   submeshes;
    };

    // 说明: CPU Mesh 资产，包含多级 LOD
    struct MeshAsset
    {
        utl::vector<LODCPU>       lods;
    };

    // 函数说明: 针对单个子网格进行索引缓存友好重排/量化等优化（示例骨架，实际算法可替换）
    void optimize_submesh(SubmeshCPU& submesh);

    // 函数说明: 根据目标质量与预算生成 LOD（示例骨架）
    void generate_lods(MeshAsset& asset);

    // 函数说明: 构建 meshlet/cluster 数据（示例骨架）
    void build_meshlets(MeshAsset& asset);

    // 函数说明: 构建空间加速结构（Octree/BVH/Voxel）（示例骨架）
    void build_spatial_structures(MeshAsset& asset);

    // 函数说明: 将 MeshAsset 打包为 ContentToEngine 的几何资源二进制格式（含 16 字节对齐）
    void pack_geometry_resource_blob(const MeshAsset& asset, utl::vector<u8>& out_blob);

    // 函数说明: 上传 CPU Mesh 资产到 GPU，返回 geometry_content_id（可用于 MeshComponent 绑定）
    id::id_type upload_mesh_asset_to_gpu(const MeshAsset& asset);
}