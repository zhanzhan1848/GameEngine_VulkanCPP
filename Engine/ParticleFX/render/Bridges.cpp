// Bridges.cpp — 见 Bridges.h。

#include "Bridges.h"
#include "Particles/ParticleTypes.h"
#include "Content/ContentToEngine.h"
#include "Content/CookedMeshAdapter.h"

#ifndef DISABLE_PARTICLE_FX

#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace primal::pfx::render {

namespace rhi = primal::graphics::rhi;

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

    u32 n = 0;
    for (u32 row = 0; row < store.capacity() && n < cap; ++row) {
        if (!store.is_row_alive(row)) continue;
        mesh_instance_data& inst = out[n];
        inst.pos[0] = pos[row].x;
        inst.pos[1] = pos[row].y;
        inst.pos[2] = pos[row].z;
        inst.pad0 = 0.f;
        inst.rot[0] = 0.f;
        inst.rot[1] = 0.f;
        inst.rot[2] = 0.f;
        inst.rot[3] = 1.f;   // 单位四元数
        inst.scale[0] = scl[row].x;
        inst.scale[1] = scl[row].y;
        inst.pad1[0] = inst.pad1[1] = 0.f;
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

bool load_instance_geometry(const char* cooked_mesh_path, instance_geometry& out, std::string& err) {
    if (!cooked_mesh_path || !cooked_mesh_path[0]) {
        err = "empty instance mesh path";
        return false;
    }
    auto& cache = geom_cache();
    std::lock_guard<std::mutex> lock(cache.mu);
    if (auto it = cache.loaded.find(cooked_mesh_path); it != cache.loaded.end()) {
        out = it->second;
        return true;
    }
    if (cache.failed.count(cooked_mesh_path)) {
        err = "previously failed";
        return false;
    }

    // 读文件（ContentTools legacy scene container 原始字节）
    std::FILE* f = std::fopen(cooked_mesh_path, "rb");
    if (!f) {
        err = std::string("cannot open '") + cooked_mesh_path + "'";
        cache.failed.emplace(cooked_mesh_path, true);
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
        err = std::string("empty cooked mesh '") + cooked_mesh_path + "'";
        cache.failed.emplace(cooked_mesh_path, true);
        return false;
    }

    // legacy blob → geometry blob（边界检查过的解析器）
    std::vector<u8> geometry_blob;
    std::string norm_err;
    if (!content::normalize_cooked_mesh_scene(bytes.data(), bytes.size(), geometry_blob, &norm_err)) {
        err = std::string("normalize failed: ") + norm_err;
        cache.failed.emplace(cooked_mesh_path, true);
        return false;
    }

    // geometry blob → content 系统 → RHIMeshAsset（RenderMesh::CreateFromAsset 同链路）
    const id::id_type geometry_id = content::create_resource(geometry_blob.data(), content::asset_type::mesh);
    if (geometry_id == id::invalid_id) {
        err = "create_resource(mesh) failed";
        cache.failed.emplace(cooked_mesh_path, true);
        return false;
    }
    rhi::RHIMeshAsset asset;
    if (!content::get_rhi_mesh_asset(content::get_rhi_mesh_id(geometry_id), asset) ||
        asset.num_vertices == 0 || asset.position_buffer.empty() || asset.num_indices == 0) {
        err = "RHIMeshAsset unavailable/empty";
        cache.failed.emplace(cooked_mesh_path, true);
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

    cache.loaded.emplace(cooked_mesh_path, out);
    return true;
}

} // namespace primal::pfx::render

#endif // !DISABLE_PARTICLE_FX
