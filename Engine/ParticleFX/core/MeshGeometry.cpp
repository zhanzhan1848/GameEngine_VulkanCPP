// MeshGeometry.cpp — OBJ → 三角形汤 + 采样 CDF 烘焙。
// TINYOBJLOADER_IMPLEMENTATION 在 Engine 内只定义于此（ContentTools/ObjImporter.h
// 是独立二进制里的另一份）。

#include "MeshGeometry.h"

#ifndef DISABLE_PARTICLE_FX

#include <algorithm>
#include <cmath>

#define TINYOBJLOADER_IMPLEMENTATION
#include "Content/tiny_obj_loader.h"

namespace primal::pfx {

namespace {

f32 tri_area(const math::v3& a, const math::v3& b, const math::v3& c) {
    const math::v3 ab{ b.x - a.x, b.y - a.y, b.z - a.z };
    const math::v3 ac{ c.x - a.x, c.y - a.y, c.z - a.z };
    const math::v3 cr{ ab.y * ac.z - ab.z * ac.y, ab.z * ac.x - ab.x * ac.z,
                       ab.x * ac.y - ab.y * ac.x };
    return 0.5f * std::sqrt(cr.x * cr.x + cr.y * cr.y + cr.z * cr.z);
}

// 三角形权重 = 面积 × 三角形顶点权重均值（密度加权：单位面积密度 ∝ 权重）。
// src 为 nullptr（面积分布）时均值恒 1；src 里任一顶点权重缺失（负值）→ 均值取 1。
mesh_geometry::weight_cdf build_cdf(const mesh_geometry& m, const std::vector<f32>* src) {
    mesh_geometry::weight_cdf w;
    const u32 tri = m.triangle_count();
    if (tri == 0) return w;
    w.means.resize(tri);
    w.dens.resize(tri);
    f32 max_dens = 0.f;
    for (u32 t = 0; t < tri; ++t) {
        const f32 area = tri_area(m.positions[m.indices[t * 3]], m.positions[m.indices[t * 3 + 1]],
                                  m.positions[m.indices[t * 3 + 2]]);
        f32 density = 1.f;
        if (src && !src->empty()) {
            f32 sum = 0.f;
            bool ok = true;
            for (u32 k = 0; k < 3; ++k) {
                const f32 v = (*src)[t * 3 + k];
                if (v < 0.f) {
                    ok = false;
                    break;
                }
                sum += v;
            }
            density = ok ? sum / 3.f : 1.f;
        }
        w.dens[t] = density;
        w.means[t] = area * density;
        max_dens = std::max(max_dens, density);
    }
    w.max_dens = max_dens;
    w.cdf.resize(tri);
    f32 acc = 0.f;
    for (u32 t = 0; t < tri; ++t) {
        acc += w.means[t];
        w.cdf[t] = acc;
    }
    w.total = acc;
    // 全零权重（如顶点色全黑）：CDF 无分辨率，调用方视为退化
    w.valid = acc > 0.f;
    return w;
}

} // namespace

u32 mesh_geometry::sample_triangle(const weight_cdf& w, f32 u) const {
    if (w.cdf.empty() || w.total <= 0.f) return 0;
    const auto it = std::upper_bound(w.cdf.begin(), w.cdf.end(), u);
    return (u32)std::min<f32>((f32)(it - w.cdf.begin()), (f32)w.cdf.size() - 1.f);
}

bool mesh_geometry::load_obj(const char* path, std::string& err) {
    positions.clear();
    indices.clear();
    corner_r.clear();
    corner_u.clear();

    tinyobj::ObjReaderConfig cfg;
    cfg.triangulate = true;
    cfg.vertex_color = false;   // 顶点色不并入 position，走 corner_r 权重通道
    tinyobj::ObjReader reader;
    if (!reader.ParseFromFile(path, cfg)) {
        err = reader.Error().empty() ? "obj parse failed" : reader.Error();
        return false;
    }
    if (!reader.Warning().empty()) err = reader.Warning();   // 非致命，交调用方记录

    const tinyobj::attrib_t& attrib = reader.GetAttrib();
    const std::vector<tinyobj::shape_t>& shapes = reader.GetShapes();
    if (attrib.vertices.empty() || shapes.empty()) {
        err = "obj has no geometry";
        return false;
    }

    positions.reserve(attrib.vertices.size() / 3);
    for (size_t i = 0; i + 2 < attrib.vertices.size(); i += 3)
        positions.push_back(math::v3{ attrib.vertices[i], attrib.vertices[i + 1],
                                      attrib.vertices[i + 2] });
    const bool has_color = attrib.colors.size() >= attrib.vertices.size();
    const size_t vtx_count = positions.size();

    for (const tinyobj::shape_t& shape : shapes) {
        const tinyobj::mesh_t& mesh = shape.mesh;
        for (const tinyobj::index_t& idx : mesh.indices) {
            if (idx.vertex_index < 0 || (size_t)idx.vertex_index >= vtx_count) {
                err = "obj index out of range";
                return false;
            }
            indices.push_back((u32)idx.vertex_index);
            corner_r.push_back(has_color ? attrib.colors[(size_t)idx.vertex_index * 3] : -1.f);
            corner_u.push_back(idx.texcoord_index >= 0
                                   ? attrib.texcoords[(size_t)idx.texcoord_index * 2]
                                   : -1.f);
        }
    }
    if (triangle_count() == 0) {
        err = "obj has no triangles";
        return false;
    }

    // 面积均匀分布恒可用；权重分布在源数据完整时才 valid，采样端退化为面积。
    area = build_cdf(*this, nullptr);
    area.valid = true;
    color = build_cdf(*this, &corner_r);
    uv = build_cdf(*this, &corner_u);
    return true;
}

} // namespace primal::pfx

#endif // !DISABLE_PARTICLE_FX
