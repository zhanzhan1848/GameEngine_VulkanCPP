// PrintAPI.cpp — 3d-printing S1 资产桥接（2026-10-15）：cooked .mesh →
// 三角形数组的 EngineDLL 导出。两段式（先计数后取数，调用方分配缓冲，
// 无跨 DLL free 约定）；文件读两次换取接口稳定。
// 消费方：PrimalEditor_Avalonia print.check / print.export（PrintMeshChecks/
// MeshExporters 接实际资产）。

#include "Common.h"
#include "CommonHeaders.h"
#include "Content/CookedMeshAdapter.h"

#include <cstdio>
#include <vector>

namespace {

bool load_and_extract(const char* path,
                      std::vector<float>& positions,
                      std::vector<std::uint32_t>& indices,
                      std::string& error)
{
    if (!path || !path[0])
    {
        error = "path is empty";
        return false;
    }
    std::FILE* f = std::fopen(path, "rb");
    if (!f)
    {
        error = "cannot open file";
        return false;
    }
    std::vector<std::uint8_t> bytes;
    char buf[65536];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
        bytes.insert(bytes.end(), buf, buf + n);
    std::fclose(f);
    return primal::content::extract_cooked_mesh_triangles(
        bytes.data(), bytes.size(), positions, indices, &error);
}

} // anonymous namespace

extern "C" {

// 第一段：取计数。out_position_count = 顶点数×3（xyz 三元组数）；
// out_index_count = 三角索引数（= 三角形×3）。返回 0 = 失败（stderr 有原因）。
EDITOR_INTERFACE u32 CookedMeshTriangleCounts(const char* path,
                                              u32* out_position_count,
                                              u32* out_index_count)
{
    if (!out_position_count || !out_index_count) return 0;
    *out_position_count = 0;
    *out_index_count = 0;
    std::vector<float> positions;
    std::vector<std::uint32_t> indices;
    std::string error;
    if (!load_and_extract(path, positions, indices, error))
    {
        std::fprintf(stderr, "[PrintAPI] %s: %s\n", path ? path : "(null)", error.c_str());
        return 0;
    }
    *out_position_count = static_cast<u32>(positions.size());
    *out_index_count = static_cast<u32>(indices.size());
    return 1;
}

// 第二段：取数据。positions = float[position_count]（xyz）；
// indices = u32[index_count]。缓冲不足 → 0（不越界写）。
EDITOR_INTERFACE u32 CookedMeshTriangleData(const char* path,
                                            float* positions, u32 position_capacity,
                                            u32* indices, u32 index_capacity)
{
    std::vector<float> p;
    std::vector<std::uint32_t> i;
    std::string error;
    if (!load_and_extract(path, p, i, error))
    {
        std::fprintf(stderr, "[PrintAPI] %s: %s\n", path ? path : "(null)", error.c_str());
        return 0;
    }
    if (p.size() > position_capacity || i.size() > index_capacity) return 0;
    for (size_t k = 0; k < p.size(); ++k) positions[k] = p[k];
    for (size_t k = 0; k < i.size(); ++k) indices[k] = i[k];
    return 1;
}

} // extern "C"
