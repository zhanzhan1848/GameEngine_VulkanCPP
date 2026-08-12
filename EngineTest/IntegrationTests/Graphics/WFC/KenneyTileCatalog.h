// KenneyTileCatalog.h — Application-layer loader for Kenney dungeon tile pack.
//
// This is NOT engine core. It lives under EngineTest/ because it is demo /
// evaluation code that composes engine capabilities (content::create_resource
// + SceneDataAdapter::ImportResources + StandardRenderPipeline::RegisterMeshEntity)
// to load a specific asset pack off disk. The engine core provides the APIs;
// this class is a consumer.
//
// Per the project's Core Layer as Capability Provider principle, no engine
// changes are needed to support a new asset pack — just write a new catalog
// like this one alongside the test that uses it.
//
// Asset path note: pack_geometry.py writes .engine_mesh in its own format
// (materials prefix + LODs + submeshes + MSHL sections). The matching reader
// is SceneDataAdapter::ImportResources, NOT content::create_resource(mesh)
// (which expects a different format). All Kenney .engine_mesh files go
// through ImportResources.
//
// Texture path note: Kenney dungeon tiles ship with a single colormap.png
// atlas that every tile samples via UVs. Pass the atlas path to
// LoadFromDirectory and the catalog will register it once, wiring the same
// texture_content_id into every tile's material.
//
// Lifecycle:
//   KenneyTileCatalog catalog;
//   u32 n = catalog.LoadFromDirectory(
//       pipeline,
//       "assets/Processed/kenney_dungeon_tiles",
//       "assets/Raw/kenney_dungeon_tiles/Textures/colormap.png");
//   // catalog.Slots()[0..n-1] now usable for rendering

#pragma once

#include "Engine/Common/CommonHeaders.h"  // Id.h + u32 transitively
#include <string>
#include <vector>

namespace primal::graphics { class StandardRenderPipeline; }
namespace primal::graphics::wfc {
    class WFCTileRegistry;
    class TileAdjacencyTable;
}

namespace primal::test::kenney {

struct KenneyTileSlot {
    std::string      name;           // file stem, e.g. "template-floor"
    std::string      file_path;      // full path to .engine_mesh
    primal::id::id_type geometry_content_id{primal::id::invalid_id};  // from ImportResources
    primal::id::id_type texture_content_id{primal::id::invalid_id};  // colormap atlas (shared)
    primal::id::id_type entity_id{primal::id::invalid_id};            // from RegisterMeshEntity
    u32              render_slot{0}; // index in ForwardSceneRenderer::mesh_infos_
};

class KenneyTileCatalog {
public:
    KenneyTileCatalog() = default;
    ~KenneyTileCatalog() = default;

    KenneyTileCatalog(const KenneyTileCatalog&) = delete;
    KenneyTileCatalog& operator=(const KenneyTileCatalog&) = delete;

    // Scan `dir_path` for *.engine_mesh files (sorted alphabetically for
    // deterministic slot ordering), feed each through SceneDataAdapter::ImportResources
    // + render-pipeline registration, and append a slot per file.
    //
    // pipeline: must outlive this catalog (we hold a raw pointer for
    //           diagnostic access; resources themselves are owned by pipeline).
    // dir_path: relative to current working directory, or absolute.
    // colormap_path: optional path to colormap.png atlas. If non-empty and
    //                loads successfully, every tile's material references it;
    //                otherwise tiles fall back to the renderer's default white
    //                material.
    //
    // Returns the number of tiles successfully loaded. Files that fail to
    // read / parse / register are skipped with a stderr warning; the rest
    // still load.
    u32 LoadFromDirectory(primal::graphics::StandardRenderPipeline* pipeline,
                          const std::string& dir_path,
                          const std::string& colormap_path = {});

    u32 Count() const { return static_cast<u32>(slots_.size()); }
    const std::vector<KenneyTileSlot>& Slots() const { return slots_; }
    const KenneyTileSlot& Get(u32 i) const { return slots_[i]; }

    // Find a slot by file stem (e.g. "template-wall"). Returns -1 if missing.
    // Used by BuildWFCRegistry to wire WFC tiles to specific mesh slots.
    s32 FindSlotByName(const std::string& name) const;

    // Populate `registry` with 8 hand-picked Kenney tile types (~19 variants
    // total) and auto-generate `adjacency` compatibility entries from each
    // tile's per-face socket signatures. Tiles are wired to the mesh slots
    // already loaded by LoadFromDirectory; call this AFTER loading.
    //
    // Socket scheme: each face is encoded as a single byte.
    //   0x00 = wall             (matches another wall, mirror-symmetric)
    //   0xFF = opening          (matches another opening, mirror-symmetric)
    //   0xAA = vertical sentinel (self-matching; any tile stacks on any tile)
    // Variant rotation is handled by permuting the side labels (variant 0 =
    // identity; variants 1-3 rotate the tile 90°/180°/270° around +Y).
    //
    // Vertical sentinel 0xAA makes each Y layer solve as an independent 2D
    // dungeon. For vertical roles (floor/wall/ceiling), introduce more
    // sentinels here and per-tile.
    //
    // Returns the number of tiles registered (8 on success). 0 on failure
    // (any of the 8 required Kenney tile files missing from the catalog).
    u32 BuildWFCRegistry(primal::graphics::wfc::WFCTileRegistry& registry,
                         primal::graphics::wfc::TileAdjacencyTable& adjacency) const;

private:
    std::vector<KenneyTileSlot> slots_;
};

} // namespace primal::test::kenney
