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

    // P4.1:mesh_volume 内点云(开放网格/退化 → 空,采样端退化 surface)
    bake_interior_points(1024, 512);
    return true;
}

// ---- 内点云(mesh_volume)------------------------------------------------------

bool mesh_geometry::contains_point(const math::v3& p) const {
    // +X 射线奇偶计数(2D 投影 yz 平面交叉法)
    u32 crossings = 0;
    const u32 tri = triangle_count();
    for (u32 t = 0; t < tri; ++t) {
        const math::v3& a = positions[indices[t * 3]];
        const math::v3& b = positions[indices[t * 3 + 1]];
        const math::v3& c = positions[indices[t * 3 + 2]];
        // 射线沿 +X:解 2D 射线-三角形(yz 平面),交点 x 须 > p.x
        const f32 ay = a.y, az = a.z, by = b.y, bz = b.z, cy = c.y, cz = c.z;
        const f32 d1 = (p.y - by) * (cz - bz) - (p.z - bz) * (cy - by);
        const f32 d2 = (p.y - cy) * (az - cz) - (p.z - cz) * (ay - cz);
        const f32 d3 = (p.y - ay) * (bz - az) - (p.z - az) * (by - az);
        const bool has_neg = d1 < 0 || d2 < 0 || d3 < 0;
        const bool has_pos = d1 > 0 || d2 > 0 || d3 > 0;
        if (has_neg && has_pos) continue;                       // 射线不穿该三角形投影
        // 交点 x
        const f32 denom = (b.y - a.y) * (c.z - a.z) - (c.y - a.y) * (b.z - a.z);
        if (std::fabs(denom) < 1e-12f) continue;                // 投影退化(与射线平行)
        const f32 u = ((p.y - a.y) * (c.z - a.z) - (p.z - a.z) * (c.y - a.y)) / denom;
        const f32 v = ((b.y - a.y) * (p.z - a.z) - (b.z - a.z) * (p.y - a.y)) / denom;
        if (u < 0.f || v < 0.f || u + v > 1.f) continue;
        const f32 x_hit = a.x + u * (b.x - a.x) + v * (c.x - a.x);
        if (x_hit > p.x) ++crossings;
    }
    return (crossings % 2) == 1;
}

void mesh_geometry::bake_interior_points(u32 candidates, u32 cap) {
    interior.clear();
    if (positions.empty()) return;
    math::v3 lo = positions[0], hi = positions[0];
    for (const auto& p : positions) {
        lo.x = std::min(lo.x, p.x); lo.y = std::min(lo.y, p.y); lo.z = std::min(lo.z, p.z);
        hi.x = std::max(hi.x, p.x); hi.y = std::max(hi.y, p.y); hi.z = std::max(hi.z, p.z);
    }
    pcg32 rng;
    rng.seed(0x9E3779B9ULL);   // 固定种子:同网格烘焙结果确定
    for (u32 i = 0; i < candidates && interior.size() < cap; ++i) {
        const math::v3 p{ lo.x + (hi.x - lo.x) * rng.next_f32(),
                          lo.y + (hi.y - lo.y) * rng.next_f32(),
                          lo.z + (hi.z - lo.z) * rng.next_f32() };
        if (contains_point(p)) interior.push_back(p);
    }
}

} // namespace primal::pfx

#endif // !DISABLE_PARTICLE_FX
