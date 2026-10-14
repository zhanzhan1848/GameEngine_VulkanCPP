#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace primal::content
{
    // Convert ContentTools' cooked scene container into the geometry blob
    // consumed by create_resource(asset_type::mesh). The parser is fully
    // bounds checked so untrusted/truncated files never reach assert-driven
    // legacy blob readers.
    bool normalize_cooked_mesh_scene(
        const void* data,
        std::size_t size,
        std::vector<std::uint8_t>& geometry_blob,
        std::string* error = nullptr);

    // 提取容器内全部网格的三角形汤（3d-printing S1 资产桥接，2026-10-15）：
    //   positions_out：xyz 三元组浮点（所有 LOD 组的全部 mesh 顶点拼接）；
    //   indices_out：u32 三角索引（逐 mesh 以累计顶点基址偏移后拼接；
    //                u16 容器索引统一升为 u32）。
    // 只读、确定性、不创建引擎资源。错误语义与 normalize 同源（同一解析
    // 路径）；输入是"已归一化几何 blob"（而非容器）时返回类型化错误——
    // 本提取面向磁盘上的 .mesh cooked 容器。
    bool extract_cooked_mesh_triangles(
        const void* data,
        std::size_t size,
        std::vector<float>& positions_out,
        std::vector<std::uint32_t>& indices_out,
        std::string* error = nullptr);
}
