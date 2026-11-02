// Bridges.cpp — 见 Bridges.h。

#include "Bridges.h"
#include "Particles/ParticleTypes.h"
#include "Content/ContentToEngine.h"
#include "Content/CookedMeshAdapter.h"
#include "Content/ProceduralMesh.h"

#ifndef DISABLE_PARTICLE_FX

#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace primal::pfx::render {

namespace rhi = primal::graphics::rhi;

std::vector<graphics::volume::VolumeParticle> pack_volume_particles(
    const attribute_store& store, f32 radius_at_scale_one, f32 density_scale) {
    std::vector<graphics::volume::VolumeParticle> result;
    if (!store.is_initialized()) return result;
    result.reserve(store.alive_count());
    const auto* pos = store.v4_channel(attr::position);
    const auto* scale = store.v4_channel(attr::scale_rotation);
    const auto* color = store.v4_channel(attr::color);
    for (u32 row=0; row<store.capacity(); ++row) {
        if (!store.is_row_alive(row)) continue;
        const float radius=std::abs(scale[row].x)*radius_at_scale_one;
        const float density=std::max(color[row].w,0.0f)*density_scale;
        if (radius==0 || density==0) continue;
        result.push_back({pos[row].x,pos[row].y,pos[row].z,radius,density});
    }
    return result;
}

u32 pack_sprite_instances(const attribute_store& store, void* out, u32 cap_bytes) {
    const u32 alive = store.alive_count();
    const u32 bytes_needed = alive * (u32)sizeof(particles::particle_data);
    if (!out || cap_bytes < bytes_needed) return alive;

    auto* rows = (particles::particle_data*)out;
    const math::v4* pos = store.v4_channel(attr::position);
    const math::v4* vel = store.v4_channel(attr::velocity);
    const math::v4* col = store.v4_channel(attr::color);
    const math::v4* scl = store.v4_channel(attr::scale_rotation);
    const math::v4* uv = store.v4_channel(attr::uv_params);

    u32 n = 0;
    const u32 cap_rows = cap_bytes / (u32)sizeof(particles::particle_data);
    for (u32 row = 0; row < store.capacity() && n < cap_rows; ++row) {
        if (!store.is_row_alive(row)) continue;
        rows[n].position = pos[row];
        rows[n].velocity = vel[row];
        rows[n].color = col[row];
        rows[n].scale_rotation = scl[row];
        rows[n].uv_params = uv[row];
        ++n;
    }
    return n;
}

u32 pack_mesh_instances(const attribute_store& store, mesh_instance_data* out, u32 cap) {
    if (!out) return store.alive_count();
    const math::v4* pos = store.v4_channel(attr::position);
    const math::v4* scl = store.v4_channel(attr::scale_rotation);
    const math::v4* col = store.v4_channel(attr::color);

    u32 n = 0;
    for (u32 row = 0; row < store.capacity() && n < cap; ++row) {
        if (!store.is_row_alive(row)) continue;
        mesh_instance_data& inst = out[n];
        inst.pos[0] = pos[row].x;
        inst.pos[1] = pos[row].y;
        inst.pos[2] = pos[row].z;
        // 实例色（color 通道 rgb；alpha 不进 48B 布局）
        inst.col_r = col[row].x;
        inst.col_gb[0] = col[row].y;
        inst.col_gb[1] = col[row].z;
        // 旋转：scale_rotation.z = 弧度角，绕 +Y（P1.2；未写旋转 ≈ 0 → 单位姿态）
        const f32 half = scl[row].z * 0.5f;
        inst.rot[0] = 0.f;
        inst.rot[1] = std::sin(half);
        inst.rot[2] = 0.f;
        inst.rot[3] = std::cos(half);
        inst.scale[0] = scl[row].x;
        inst.scale[1] = scl[row].y;
        ++n;
    }
    return n;
}

// ---- 渲染 instance 几何加载 ----------------------------------------------------
// cooked .mesh → create_resource → RHIMeshAsset 解包。进程级缓存（同路径只读盘一次，
// 失败也缓存防每帧重试）。锁保护：渲染收集与编辑器推送可能并发首遇同路径。

namespace {

struct instance_geometry_cache {
    std::mutex mu;
    std::unordered_map<std::string, instance_geometry> loaded;   // 成功
    std::unordered_map<std::string, bool> failed;                // 失败（值无意义，占位）
};

instance_geometry_cache& geom_cache() {
    static instance_geometry_cache c;
    return c;
}

} // namespace

// 已注册 geometry_content_id → 三角形汤（cooked 与 builtin 共用尾巴）
static bool instance_geometry_from_content(id::id_type geometry_id, instance_geometry& out,
                                           std::string& err) {
    rhi::RHIMeshAsset asset;
    if (!content::get_rhi_mesh_asset(content::get_rhi_mesh_id(geometry_id), asset) ||
        asset.num_vertices == 0 || asset.position_buffer.empty() || asset.num_indices == 0) {
        err = "RHIMeshAsset unavailable/empty";
        return false;
    }

    out.positions.resize(asset.num_vertices);
    std::memcpy(out.positions.data(), asset.position_buffer.data(),
                (size_t)asset.num_vertices * sizeof(math::v3));   // packed float3

    // 法线：element_buffer 每顶点前 12B（无法线资产 → 零向量，shader 光照退化为平面感）
    out.normals.assign(asset.num_vertices, math::v3{ 0.f, 1.f, 0.f });
    if (!asset.element_buffer.empty() && asset.element_buffer.size() / asset.num_vertices >= 12) {
        const u32 elem_stride = (u32)(asset.element_buffer.size() / asset.num_vertices);
        for (u32 v = 0; v < asset.num_vertices; ++v)
            std::memcpy(&out.normals[v], asset.element_buffer.data() + (size_t)v * elem_stride,
                        sizeof(math::v3));
    }

    // 索引统一扩为 u32
    out.indices.resize(asset.num_indices);
    if (asset.index_size == 2) {
        const u16* src = (const u16*)asset.index_buffer.data();
        for (u32 i = 0; i < asset.num_indices; ++i) out.indices[i] = src[i];
    } else {
        std::memcpy(out.indices.data(), asset.index_buffer.data(),
                    (size_t)asset.num_indices * sizeof(u32));
    }
    out.index_count = asset.num_indices;
    return true;
}

// builtin:<name> → 程序化几何（默认小尺寸，与内置立方体回退同量级）
static id::id_type create_builtin_geometry(const std::string& name, std::string& err) {
    if (name == "box" || name == "cube") return content::create_box_mesh(0.1f, 0.1f, 0.1f);
    if (name == "sphere") return content::create_sphere_mesh(0.05f, 16, 12);
    if (name == "plane") return content::create_plane_mesh(0.1f, 0.1f, 1, 1);
    if (name == "capsule") return content::create_capsule_mesh(0.05f, 0.1f, 8, 4);
    err = "unknown builtin mesh '" + name + "' (box/sphere/plane/capsule)";
    return id::invalid_id;
}

bool load_instance_geometry(const char* cooked_mesh_path, instance_geometry& out, std::string& err) {
    if (!cooked_mesh_path || !cooked_mesh_path[0]) {
        err = "empty instance mesh path";
        return false;
    }
    const std::string path = cooked_mesh_path;
    auto& cache = geom_cache();
    std::lock_guard<std::mutex> lock(cache.mu);
    if (auto it = cache.loaded.find(path); it != cache.loaded.end()) {
        out = it->second;
        return true;
    }
    if (cache.failed.count(path)) {
        err = "previously failed";
        return false;
    }

    // builtin: 前缀 → 程序化几何注册进 content 系统（免盘 IO），解包尾巴与 cooked 相同
    if (path.compare(0, 8, "builtin:") == 0) {
        const id::id_type geometry_id = create_builtin_geometry(path.substr(8), err);
        if (geometry_id == id::invalid_id) {
            cache.failed.emplace(path, true);
            return false;
        }
        const bool ok = instance_geometry_from_content(geometry_id, out, err);
        content::destroy_resource(geometry_id, content::asset_type::mesh);   // 数组已拷出,立即回收
        if (!ok) {
            cache.failed.emplace(path, true);
            return false;
        }
        cache.loaded.emplace(path, out);
        return true;
    }

    // 读文件（ContentTools legacy scene container 原始字节）
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        err = std::string("cannot open '") + path + "'";
        cache.failed.emplace(path, true);
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<u8> bytes;
    if (size > 0) {
        bytes.resize((size_t)size);
        const size_t rd = std::fread(bytes.data(), 1, bytes.size(), f);
        bytes.resize(rd);
    }
    std::fclose(f);
    if (bytes.empty()) {
        err = std::string("empty cooked mesh '") + path + "'";
        cache.failed.emplace(path, true);
        return false;
    }

    // legacy blob → geometry blob（边界检查过的解析器）
    std::vector<u8> geometry_blob;
    std::string norm_err;
    if (!content::normalize_cooked_mesh_scene(bytes.data(), bytes.size(), geometry_blob, &norm_err)) {
        err = std::string("normalize failed: ") + norm_err;
        cache.failed.emplace(path, true);
        return false;
    }

    // geometry blob → content 系统 → 解包（RenderMesh::CreateFromAsset 同链路）
    const id::id_type geometry_id = content::create_resource(geometry_blob.data(), content::asset_type::mesh);
    if (geometry_id == id::invalid_id) {
        err = "create_resource(mesh) failed";
        cache.failed.emplace(path, true);
        return false;
    }
    const bool ok = instance_geometry_from_content(geometry_id, out, err);
    content::destroy_resource(geometry_id, content::asset_type::mesh);   // 数组已拷出,立即回收
    if (!ok) {
        cache.failed.emplace(path, true);
        return false;
    }
    cache.loaded.emplace(path, out);
    return true;
}

} // namespace primal::pfx::render

#endif // !DISABLE_PARTICLE_FX
