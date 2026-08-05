// KenneyTileCatalog.cpp — see header for architecture rationale.

#include "KenneyTileCatalog.h"

#include "Engine/Content/ContentToEngine.h"
#include "Engine/Graphics/SceneDataAdapter.h"
#include "Engine/Graphics/RenderPipeline/StandardRenderPipeline.h"
#include "Engine/Graphics/RenderPipeline/Modules/ForwardSceneRenderer.h"
#include "Engine/Graphics/WFC/WFCTileRegistry.h"
#include "Engine/Graphics/WFC/TileAdjacency.h"
#include "Engine/Graphics/WFC/WFCTypes.h"
#include "Engine/Graphics/WFC/WFCCategory.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace primal::test::kenney {

namespace fs = std::filesystem;

namespace {

bool ReadFileToBuffer(const fs::path& path, std::vector<u8>& out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) return false;
    const std::streamoff size = f.tellg();
    if (size <= 0) return false;
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    return static_cast<bool>(f.read(reinterpret_cast<char*>(out.data()), size));
}

// Load a PNG via stb_image, build the engine texture blob
// (u32 width | u32 height | u32 array_size | u32 flags | u32 mip_levels |
//  u32 format | per-mip: u32 row_pitch | u32 slice_pitch | pixel bytes),
// and register via content::create_resource(asset_type::texture).
// Returns invalid_id on failure.
primal::id::id_type LoadTextureContentId(const std::string& path, bool is_srgb) {
    int width = 0, height = 0, channels = 0;
    unsigned char* data = stbi_load(path.c_str(), &width, &height, &channels, 4);
    if (!data || width <= 0 || height <= 0) {
        std::cerr << "[KenneyTileCatalog] stbi_load failed: " << path << std::endl;
        if (data) stbi_image_free(data);
        return primal::id::invalid_id;
    }

    constexpr u32 kArraySize = 1;
    constexpr u32 kFlags = 0;
    constexpr u32 kMipLevels = 1;
    // 28 = R8G8B8A8_UNORM (Linear), 29 = R8G8B8A8_UNORM_SRGB
    const u32 format = is_srgb ? 29u : 28u;
    const u32 row_pitch = static_cast<u32>(width) * 4u;
    const u32 slice_pitch = static_cast<u32>(height) * row_pitch;

    std::vector<u8> blob;
    blob.resize(6 * sizeof(u32) + 2 * sizeof(u32) + slice_pitch);
    u8* p = blob.data();
    auto write_u32 = [&](u32 v) {
        std::memcpy(p, &v, sizeof(u32));
        p += sizeof(u32);
    };
    write_u32(static_cast<u32>(width));
    write_u32(static_cast<u32>(height));
    write_u32(kArraySize);
    write_u32(kFlags);
    write_u32(kMipLevels);
    write_u32(format);
    write_u32(row_pitch);
    write_u32(slice_pitch);
    std::memcpy(p, data, slice_pitch);

    stbi_image_free(data);

    const primal::id::id_type tex_id = primal::content::create_resource(
        blob.data(), primal::content::asset_type::texture);
    if (tex_id == primal::id::invalid_id) {
        std::cerr << "[KenneyTileCatalog] create_resource(texture) failed: " << path << std::endl;
    }
    return tex_id;
}

} // namespace

u32 KenneyTileCatalog::LoadFromDirectory(
    primal::graphics::StandardRenderPipeline* pipeline,
    const std::string& dir_path,
    const std::string& colormap_path) {

    if (!pipeline) {
        std::cerr << "[KenneyTileCatalog] null pipeline" << std::endl;
        return 0;
    }

    if (!fs::exists(dir_path) || !fs::is_directory(dir_path)) {
        std::cerr << "[KenneyTileCatalog] directory not found: " << dir_path << std::endl;
        return 0;
    }

    // Load colormap atlas once. All tiles share this texture_content_id.
    primal::id::id_type colormap_id = primal::id::invalid_id;
    if (!colormap_path.empty()) {
        colormap_id = LoadTextureContentId(colormap_path, /*is_srgb=*/true);
        if (colormap_id != primal::id::invalid_id) {
            std::cout << "[KenneyTileCatalog] colormap atlas loaded: "
                      << colormap_path << " (tex_id=" << colormap_id << ")" << std::endl;
        }
    }

    // Collect .engine_mesh files, sorted for deterministic slot ordering.
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(dir_path)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() == ".engine_mesh") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());

    if (files.empty()) {
        std::cerr << "[KenneyTileCatalog] no .engine_mesh files in " << dir_path << std::endl;
        return 0;
    }

    // Capture slot base BEFORE any registration so we can compute the slot
    // index for each tile = slot_base + position_in_load_order.
    auto* fwd = pipeline->GetForwardRenderer();
    const u32 slot_base = fwd ? fwd->GetMeshInfoCount() : 0;

    // Texture set: colormap atlas as albedo (slot 0). Slots 1/2 (normal/ORM)
    // remain invalid_id — renderer falls back to default normal-up/neutral ORM.
    // This gives every tile proper UV-mapped albedo instead of flat white.
    primal::id::id_type tex_ids[3] = {
        colormap_id,
        primal::id::invalid_id,
        primal::id::invalid_id,
    };

    u32 loaded = 0;
    u32 failed = 0;
    for (const auto& path : files) {
        std::vector<u8> buffer;
        if (!ReadFileToBuffer(path, buffer)) {
            std::cerr << "[KenneyTileCatalog] read failed: " << path << std::endl;
            ++failed;
            continue;
        }

        // ImportResources parses the pack_geometry.py format (materials prefix
        // + LODs + submeshes + MSHL sections) and registers RHIMeshAssets
        // with the content system. Returns content IDs compatible with
        // StandardRenderPipeline::RegisterMeshEntity.
        auto imported = primal::graphics::SceneDataAdapter::ImportResources(
            buffer.data(), static_cast<u32>(buffer.size()));
        if (imported.meshes.empty()) {
            std::cerr << "[KenneyTileCatalog] ImportResources returned 0 meshes: "
                      << path << std::endl;
            ++failed;
            continue;
        }

        // One slot per file. If the file has multiple submeshes we only render
        // the first; tile-level multi-mesh support is a follow-up.
        const primal::id::id_type geo_id = imported.meshes[0].mesh_content_id;
        if (geo_id == primal::id::invalid_id) {
            std::cerr << "[KenneyTileCatalog] invalid mesh_content_id: " << path << std::endl;
            ++failed;
            continue;
        }

        // Register mesh RESOURCE only — not RegisterMeshEntity. The entity
        // variant additionally creates an ECS game_entity at world origin
        // with identity transform and adds it to StandardRenderPipeline's
        // static_entity_ids_ list. Those entities would get rendered every
        // frame at (0,0,0) on top of the actual PCG-generated tiles. The
        // test only needs the mesh slot index for mesh_infos_ lookup.
        const u32 slot_index = fwd->RegisterMeshResource(
            geo_id, tex_ids[0], tex_ids[1], tex_ids[2]);
        if (slot_index == (u32)-1) {
            std::cerr << "[KenneyTileCatalog] RegisterMeshResource failed: " << path << std::endl;
            ++failed;
            continue;
        }

        KenneyTileSlot slot;
        slot.name                = path.stem().string();
        slot.file_path           = path.string();
        slot.geometry_content_id = geo_id;
        slot.texture_content_id  = colormap_id;
        slot.entity_id           = primal::id::invalid_id;  // unused — see comment above
        slot.render_slot         = slot_index;
        slots_.push_back(std::move(slot));
        ++loaded;
    }

    std::cout << "[KenneyTileCatalog] loaded " << loaded << " / "
              << files.size() << " tiles from " << dir_path
              << " (failed=" << failed << ", slot_base=" << slot_base
              << ", colormap=" << colormap_id << ")" << std::endl;
    return loaded;
}

// ============================================================================
// FindSlotByName
// ============================================================================

s32 KenneyTileCatalog::FindSlotByName(const std::string& name) const {
    for (u32 i = 0; i < slots_.size(); ++i) {
        if (slots_[i].name == name) return static_cast<s32>(i);
    }
    return -1;
}

// ============================================================================
// BuildWFCRegistry
// ============================================================================
//
// Socket scheme — each face is a single byte:
//   0x00 = wall            (matches another wall, mirror-symmetric)
//   0xFF = opening         (matches another opening, mirror-symmetric)
//   0xAA = vertical sentinel (self-matching; lets any tile stack on any tile)
//
// Y-axis rotation moves side sockets between faces. With variant 0 side
// sockets (sP, sN, zP, zN) = (posX, negX, posZ, negZ), the rotated layouts
// are (derived from WFCSocketOps.cpp's RotateCornerY convention):
//   v0: (sP, sN, zP, zN)            identity
//   v1: (zP, zN, sN, sP)            +90° about Y
//   v2: (sN, sP, zN, zP)            180°
//   v3: (zN, zP, sP, sN)            -90°
// Vertical faces (posY, negY) are unaffected by Y-axis rotation and share
// the 0xAA sentinel across all variants/tiles, so each Y layer solves as an
// independent 2D dungeon and the showcase reads as N stacked floors.

namespace {

constexpr u8 kW = 0x00;   // wall socket
constexpr u8 kO = 0xFF;   // opening socket

struct SideSockets {
    u8 pos_x;
    u8 neg_x;
    u8 pos_z;
    u8 neg_z;
};

struct KenneyTileSpec {
    const char*  name;
    SideSockets  v0;
    u32          variant_count;
    bool         is_rotationally_symmetric;
};

// 8 hand-picked Kenney tile types. Together they cover every connectivity
// a 2D dungeon needs: solid wall, straight corridors, corners, T-junctions,
// 4-way intersections, dead-ends, rooms, and stairs.
const KenneyTileSpec kSpecs[] = {
    { "template-wall",         { kW, kW, kW, kW }, 1, true  },  // 0: solid wall
    { "corridor",              { kW, kW, kO, kO }, 2, false },  // 1: straight N-S
    { "corridor-corner",       { kO, kW, kO, kW }, 4, false },  // 2: L corner
    { "corridor-junction",     { kO, kO, kO, kW }, 4, false },  // 3: T junction
    { "corridor-intersection", { kO, kO, kO, kO }, 1, true  },  // 4: 4-way
    { "corridor-end",          { kW, kW, kO, kW }, 4, false },  // 5: dead-end
    { "room-small",            { kO, kO, kO, kO }, 1, true  },  // 6: open room
    { "stairs",                { kW, kW, kO, kO }, 2, false },  // 7: stairs up
};

SideSockets RotateV1(SideSockets s) { return { s.pos_z, s.neg_z, s.neg_x, s.pos_x }; }
SideSockets RotateV2(SideSockets s) { return { s.neg_x, s.pos_x, s.neg_z, s.pos_z }; }
SideSockets RotateV3(SideSockets s) { return { s.neg_z, s.pos_z, s.pos_x, s.neg_x }; }

SideSockets RotateVariant(SideSockets base, u32 variant) {
    switch (variant & 3u) {
        case 0: return base;
        case 1: return RotateV1(base);
        case 2: return RotateV2(base);
        case 3: return RotateV3(base);
    }
    return base;
}

// Pack 6 face bytes into a u64. Layout per WFCSocketOps.cpp DeriveSocketEncoding:
// byte f (LSB=byte 0) = signature for WFCFace f.
//
// Vertical sockets use sentinel 0xAA (all four quartiles = 2, so MirrorSignature
// returns the same value → strict-equality match works). This makes any tile
// stackable on any tile vertically: each Y layer solves as an independent 2D
// dungeon, and the showcase reads as N stacked floors. To enforce vertical
// roles (e.g. floor-below-wall-only), introduce more sentinels here and per
// tile — see TestWFCRuinsRendering for the strict-vertical pattern.
primal::graphics::wfc::SocketEncoding EncodeSockets(SideSockets sides) {
    using primal::graphics::wfc::SocketEncoding;
    constexpr u8 kLayerStack = 0xAA;  // self-matching sentinel for vertical faces
    SocketEncoding enc = 0;
    enc |= static_cast<SocketEncoding>(sides.pos_x) << 0;   // PosX
    enc |= static_cast<SocketEncoding>(sides.neg_x) << 8;   // NegX
    enc |= static_cast<SocketEncoding>(kLayerStack) << 16;  // PosY
    enc |= static_cast<SocketEncoding>(kLayerStack) << 24;  // NegY
    enc |= static_cast<SocketEncoding>(sides.pos_z) << 32;  // PosZ
    enc |= static_cast<SocketEncoding>(sides.neg_z) << 40;  // NegZ
    return enc;
}

} // namespace

u32 KenneyTileCatalog::BuildWFCRegistry(
    primal::graphics::wfc::WFCTileRegistry& registry,
    primal::graphics::wfc::TileAdjacencyTable& adjacency) const {

    using namespace primal::graphics::wfc;
    constexpr u32 kSpecCount = sizeof(kSpecs) / sizeof(kSpecs[0]);

    // Spec index → registered tile id. We register in spec order so they line up.
    u32 registered = 0;
    for (u32 i = 0; i < kSpecCount; ++i) {
        const KenneyTileSpec& spec = kSpecs[i];
        const s32 slot_idx = FindSlotByName(spec.name);
        if (slot_idx < 0) {
            std::cerr << "[KenneyTileCatalog] BuildWFCRegistry: missing tile '"
                      << spec.name << "' (have you called LoadFromDirectory?)"
                      << std::endl;
            return 0;
        }
        const KenneyTileSlot& slot = slots_[slot_idx];

        WFCTile tile{};
        tile.name                = spec.name;
        tile.category            = WFCCategory::Dungeon;
        tile.is_organic          = false;
        tile.is_rotationally_symmetric = spec.is_rotationally_symmetric;
        tile.variant_count       = spec.variant_count;
        tile.bounds_extents      = primal::math::v3{0.5f, 0.5f, 0.5f};

        // All variants of a tile point at the same render slot. The variant
        // index still rotates the WFC socket signature for adjacency purposes;
        // the renderer also rotates the entity by variant * 90° via
        // PCGAttr::RotationY (unless is_rotationally_symmetric).
        for (u32 v = 0; v < WFCTile::MaxVariants; ++v) {
            tile.mesh_handles[v] = primal::geometry::geometry_id{slot.render_slot};
        }
        // sockets[v] stored for future engine use, but the current engine
        // AddAutoFromSockets recomputes signatures from bounds_extents.y
        // (cube-derived, gives PosY=0xFF and NegY=0x00 which are mutually
        // incompatible — forbidding vertical stacking). We bypass that by
        // populating adjacency manually below using our per-face side bytes.
        for (u32 v = 0; v < spec.variant_count; ++v) {
            tile.sockets[v] = EncodeSockets(RotateVariant(spec.v0, v));
        }

        registry.Register(tile);
        ++registered;
    }

    // Manually populate adjacency from our own side socket bytes. We do NOT
    // call AddAutoFromSockets — that helper ignores tile.sockets[] and
    // recomputes from bounds_extents, which produces cube-derived signatures
    // that break vertical stacking.
    //
    // For each pair (ta, va) × (tb, vb) and each face f:
    //   - Vertical faces (PosY / NegY): always compatible (layers stack freely).
    //   - Side faces: compatible iff my byte on f (from ta/va) equals my byte
    //     on opposite(f) (from tb/vb). Bytes are kW=0x00 / kO=0xFF, both
    //     mirror-symmetric, so strict equality suffices.
    u32 compat = 0;
    for (u32 ta = 0; ta < registered; ++ta) {
        const u32 vc_a = kSpecs[ta].variant_count;
        for (u32 tb = 0; tb < registered; ++tb) {
            const u32 vc_b = kSpecs[tb].variant_count;
            for (u32 va = 0; va < vc_a; ++va) {
                const SideSockets a = RotateVariant(kSpecs[ta].v0, va);
                for (u32 vb = 0; vb < vc_b; ++vb) {
                    const SideSockets b = RotateVariant(kSpecs[tb].v0, vb);

                    // Side faces — strict byte equality at the shared seam.
                    if (a.pos_x == b.neg_x) {
                        adjacency.AddCompatibility(wfc_tile_id{ta}, va, WFCFace::PosX,
                                                   wfc_tile_id{tb}, vb);
                        ++compat;
                    }
                    if (a.neg_x == b.pos_x) {
                        adjacency.AddCompatibility(wfc_tile_id{ta}, va, WFCFace::NegX,
                                                   wfc_tile_id{tb}, vb);
                        ++compat;
                    }
                    if (a.pos_z == b.neg_z) {
                        adjacency.AddCompatibility(wfc_tile_id{ta}, va, WFCFace::PosZ,
                                                   wfc_tile_id{tb}, vb);
                        ++compat;
                    }
                    if (a.neg_z == b.pos_z) {
                        adjacency.AddCompatibility(wfc_tile_id{ta}, va, WFCFace::NegZ,
                                                   wfc_tile_id{tb}, vb);
                        ++compat;
                    }

                    // Vertical faces — any tile stacks on any tile.
                    adjacency.AddCompatibility(wfc_tile_id{ta}, va, WFCFace::PosY,
                                               wfc_tile_id{tb}, vb);
                    ++compat;
                    adjacency.AddCompatibility(wfc_tile_id{ta}, va, WFCFace::NegY,
                                               wfc_tile_id{tb}, vb);
                    ++compat;
                }
            }
        }
    }

    std::cout << "[KenneyTileCatalog] BuildWFCRegistry: registered " << registered
              << " tiles, " << compat << " adjacency entries (manual side-socket match)"
              << std::endl;
    return registered;
}

} // namespace primal::test::kenney
