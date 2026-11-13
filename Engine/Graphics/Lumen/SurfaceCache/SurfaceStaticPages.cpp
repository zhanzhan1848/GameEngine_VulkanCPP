#include "SurfaceStaticPages.h"
#include "Engine/Graphics/RHI/Core/CardSurfaceIdentity.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>   // R3 IWYU: std::vector 使用点直接补齐

namespace primal::graphics::lumen {

SurfaceStaticPages::~SurfaceStaticPages()
{
    Detach();
}

void SurfaceStaticPages::Detach()
{
    if (file_) { std::fclose(file_); file_ = nullptr; }
    meshes_.clear();
    path_.clear();
}

bool SurfaceStaticPages::Attach(const char* path)
{
    Detach();
    if (!path || !*path) return false;
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;

    ScsHeaderRT header{};
    if (std::fread(&header, 1, sizeof(header), f) != sizeof(header) ||
        header.magic != kScsMagic || header.version != kScsVersion ||
        header.header_bytes != sizeof(header) || header.page_size != 32u ||
        header.min_resolution != 64u || header.max_resolution != 128u ||
        (header.flags & 3u) != 3u || !std::isfinite(header.texels_per_unit) ||
        header.texels_per_unit < 16.f || header.texels_per_unit > 256.f ||
        header.mesh_count > 100000u ||
        header.capture_near != rhi::card_surface_identity::CaptureNear ||
        header.capture_far_scale != rhi::card_surface_identity::CaptureFarScale ||
        header.capture_far_bias != rhi::card_surface_identity::CaptureFarBias)
    {
        std::fclose(f);
        return false;
    }

    if (std::fseek(f, 0, SEEK_END) != 0) { std::fclose(f); return false; }
    const long end = std::ftell(f);
    if (end < long(sizeof(header))) { std::fclose(f); return false; }
    const u64 file_bytes = u64(end);
    std::vector<MeshEntry> meshes;
    meshes.reserve(header.mesh_count);
    u64 pos = header.header_bytes;
    bool ok = true;
    for (u32 m = 0; ok && m < header.mesh_count; ++m)
    {
        ScsMeshRecordRT rec{};
        if (std::fseek(f, long(pos), SEEK_SET) != 0 ||
            std::fread(&rec, 1, sizeof(rec), f) != sizeof(rec))
        {
            ok = false;
            break;
        }
        if (rec.card_count > 4096u || pos + sizeof(rec) > file_bytes ||
            u64(rec.card_count) * sizeof(ScsCardPageEntryRT) > file_bytes - pos - sizeof(rec)) { ok = false; break; }
        MeshEntry e{};
        e.geometry_digest = rec.geometry_digest;
        e.layout_digest = rec.layout_digest;
        e.material_digest = rec.material_digest;
        e.pages.resize(rec.card_count);
        if (std::fread(e.pages.data(), sizeof(ScsCardPageEntryRT), rec.card_count, f) != rec.card_count) { ok = false; break; }
        for (u32 c = 0; c < rec.card_count && ok; ++c) {
            const auto& pg = e.pages[c];
            if (pg.card_index != c || pg.resolution < 64u || pg.resolution > 128u || pg.resolution % 32u) { ok = false; break; }
            const u64 bytes = u64(pg.resolution) * pg.resolution * 4u;
            for (u64 offset : {pg.albedo_offset, pg.normal_offset, pg.depth_offset, pg.params_offset, pg.triangle_offset})
                if (offset > file_bytes || bytes > file_bytes - offset) ok = false;
        }
        e.card_count = rec.card_count;
        e.entries_offset = pos + sizeof(ScsMeshRecordRT);
        meshes.push_back(e);
        pos += sizeof(ScsMeshRecordRT) + u64(rec.card_count) * sizeof(ScsCardPageEntryRT);
    }
    // Payloads must follow the complete directory, never alias its metadata.
    for (const auto& mesh : meshes) for (const auto& pg : mesh.pages)
        for (u64 offset : {pg.albedo_offset, pg.normal_offset, pg.depth_offset, pg.params_offset, pg.triangle_offset})
            if (offset < pos) ok = false;
    if (!ok) { std::fclose(f); return false; }

    file_ = f;
    path_ = path;
    header_ = header;
    meshes_ = std::move(meshes);
    return true;
}

u32 SurfaceStaticPages::FindMesh(u64 geometry_digest, u64 layout_digest, u64 material_digest) const
{
    if (!file_ || !material_digest) return kInvalidSlot;
    for (size_t i = 0; i < meshes_.size(); ++i)
    {
        if (meshes_[i].geometry_digest == geometry_digest &&
            meshes_[i].layout_digest == layout_digest && meshes_[i].material_digest == material_digest)
        {
            return u32(i);
        }
    }
    return kInvalidSlot;
}

u64 SurfaceStaticPages::TotalPageBytes() const
{
    u64 total = 0;
    if (!file_) return 0;
    for (const MeshEntry& m : meshes_)
    {
        // 记录级 pages_bytes 在 header 遍历时未缓存——按条目重算。
        std::vector<ScsCardPageEntryRT> entries(m.card_count);
        if (std::fseek(file_, long(m.entries_offset), SEEK_SET) != 0 ||
            std::fread(entries.data(), sizeof(ScsCardPageEntryRT), m.card_count, file_) != m.card_count)
        {
            continue;
        }
        if (!entries.empty())
        {
            const ScsCardPageEntryRT& first = entries.front();
            const ScsCardPageEntryRT& last = entries.back();
            total += (last.triangle_offset + u64(last.resolution) * last.resolution * 4u) - first.albedo_offset;
        }
    }
    return total;
}

bool SurfaceStaticPages::CardPageEntry(u32 slot, u32 page_card, ScsCardPageEntryRT& out) const
{
    if (!file_ || slot >= meshes_.size() || page_card >= meshes_[slot].card_count)
        return false;
    out = meshes_[slot].pages[page_card];
    return true;
}

bool SurfaceStaticPages::ReadCardPlanes(u32 slot, u32 page_card,
                                        std::vector<u8>& out_albedo,
                                        std::vector<u8>& out_normal,
                                        std::vector<f32>& out_depth,
                                        u32& out_resolution, std::vector<u32>* out_triangles) const
{
    ScsCardPageEntryRT entry{};
    if (!CardPageEntry(slot, page_card, entry)) return false;
    const u64 plane_bytes = u64(entry.resolution) * entry.resolution * 4u;
    if (!plane_bytes) return false;

    out_albedo.resize(size_t(plane_bytes));
    out_normal.resize(size_t(plane_bytes));
    out_depth.resize(size_t(plane_bytes / 4u));

    auto read_at = [&](u64 offset, void* dst, u64 bytes) -> bool
    {
        return std::fseek(file_, long(offset), SEEK_SET) == 0 &&
               std::fread(dst, 1, bytes, file_) == bytes;
    };
    if (!read_at(entry.albedo_offset, out_albedo.data(), plane_bytes)) return false;
    if (!read_at(entry.normal_offset, out_normal.data(), plane_bytes)) return false;
    if (!read_at(entry.depth_offset, out_depth.data(), plane_bytes)) return false;
    if (out_triangles) {
        out_triangles->resize(size_t(plane_bytes/4u));
        if (!read_at(entry.triangle_offset, out_triangles->data(), plane_bytes)) return false;
        for(size_t i=0;i<out_triangles->size();++i)
            if ((out_albedo[i*4+3]>=128) != ((*out_triangles)[i]!=0xffffffffu)) return false;
    }
    out_resolution = entry.resolution;
    return true;
}

}  // namespace primal::graphics::lumen
