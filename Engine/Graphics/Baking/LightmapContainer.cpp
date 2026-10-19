// LMAP 容器读写（2026-10-19 lightmap 计划，M1）。纯 std 实现，无引擎依赖
// ——单测与 primal-engine CLI 共用。

#include "LightmapContainer.h"

#include <cstdio>
#include <cstring>

namespace primal::graphics::baking {

namespace {

struct FileHeader {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t format;
    std::uint32_t atlas_count;
    std::uint32_t mesh_count;
    std::uint32_t texel_bytes;
};
static_assert(sizeof(FileHeader) == 32, "LMAP header layout frozen");

bool WriteAll(const std::string& path, const void* data, size_t size) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(data, 1, size, f) == size;
    std::fclose(f);
    return ok;
}

} // namespace

bool SaveLightmapContainer(const std::string& path, const LightmapContainer& c) {
    if (c.pages.empty() || c.pages.size() != c.atlas_count) return false;

    FileHeader h{};
    h.magic = kLightmapMagic;
    h.version = kLightmapVersion;
    h.width = c.pages[0].width;
    h.height = c.pages[0].height;
    h.format = c.pages[0].format;
    h.atlas_count = c.atlas_count;
    h.mesh_count = static_cast<std::uint32_t>(c.entries.size());

    std::uint64_t texelBytes = 0;
    for (const auto& p : c.pages) {
        if (p.width != h.width || p.height != h.height) return false;  // v1 同构页
        if (p.format != kLightmapFormatRGBA16F) return false;          // 预留格式禁写
        texelBytes += p.texels_rgba16f.size() * sizeof(std::uint16_t);
    }
    const std::uint64_t expectBytes =
        (std::uint64_t)h.width * h.height * 4 * sizeof(std::uint16_t) * c.atlas_count;
    if (texelBytes != expectBytes) return false;
    h.texel_bytes = static_cast<std::uint32_t>(texelBytes);  // v1 尺寸下必然 <4GB

    const size_t total = sizeof(FileHeader) + c.entries.size() * sizeof(LightmapMeshEntry) + texelBytes;
    std::vector<std::uint8_t> blob;
    blob.reserve(total);
    blob.insert(blob.end(), reinterpret_cast<const std::uint8_t*>(&h),
                reinterpret_cast<const std::uint8_t*>(&h) + sizeof(h));
    for (const auto& e : c.entries)
        blob.insert(blob.end(), reinterpret_cast<const std::uint8_t*>(&e),
                    reinterpret_cast<const std::uint8_t*>(&e) + sizeof(e));
    for (const auto& p : c.pages) {
        const auto* b = reinterpret_cast<const std::uint8_t*>(p.texels_rgba16f.data());
        blob.insert(blob.end(), b, b + p.texels_rgba16f.size() * sizeof(std::uint16_t));
    }
    return WriteAll(path, blob.data(), blob.size());
}

bool LoadLightmapContainer(const std::string& path, LightmapContainer& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    bool ok = false;
    do {
        FileHeader h{};
        if (std::fread(&h, 1, sizeof(h), f) != sizeof(h)) break;
        if (h.magic != kLightmapMagic || h.version != kLightmapVersion) break;
        if (h.format != kLightmapFormatRGBA16F) break;
        if (h.atlas_count == 0 || h.atlas_count > 16) break;

        out.atlas_count = h.atlas_count;
        out.entries.resize(h.mesh_count);
        if (std::fread(out.entries.data(), sizeof(LightmapMeshEntry), h.mesh_count, f) != h.mesh_count)
            break;

        out.pages.resize(h.atlas_count);
        const std::uint64_t perPage =
            (std::uint64_t)h.width * h.height * 4 * sizeof(std::uint16_t);
        std::uint64_t total = 0;
        for (auto& p : out.pages) {
            p.width = h.width;
            p.height = h.height;
            p.format = h.format;
            p.texels_rgba16f.resize(h.width * h.height * 4);
            if (std::fread(p.texels_rgba16f.data(), sizeof(std::uint16_t),
                           p.texels_rgba16f.size(), f) != p.texels_rgba16f.size())
                break;
            total += perPage;
        }
        if (total != h.texel_bytes) break;
        ok = true;
    } while (false);
    std::fclose(f);
    if (!ok) out = LightmapContainer{};
    return ok;
}

} // namespace primal::graphics::baking
