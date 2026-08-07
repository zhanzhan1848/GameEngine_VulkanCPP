// EngineTest/IntegrationTests/Graphics/WFC/Styles/KenneyDungeonStyle.cpp
#include "KenneyDungeonStyle.h"

#include "Engine/Content/ContentToEngine.h"
#include "Engine/Graphics/RenderPipeline/StandardRenderPipeline.h"
#include "Engine/Graphics/WFC/WFCTileRegistry.h"
#include "Engine/Graphics/WFC/WFCTypes.h"
#include "Engine/Graphics/WFC/WFCCategory.h"
#include "Engine/Utilities/MathTypes.h"

#include <iostream>

namespace primal::test::kenney {

namespace {

// 8 hand-picked Kenney tile types — same names/variants as
// KenneyTileCatalog::BuildWFCRegistry's kSpecs (kept in sync). The side-socket
// bytes and rotation-symmetry flag from kSpecs are NOT needed here: this style
// uses the AutoSocketClassifier, which derives socket signatures from mesh
// geometry rather than hand-written bytes.
struct KenneyTileSpec {
    const char* name;
    u32         variant_count;
};

constexpr KenneyTileSpec kSpecs[] = {
    { "template-wall",         1 },
    { "corridor",              2 },
    { "corridor-corner",       4 },
    { "corridor-junction",     4 },
    { "corridor-intersection", 1 },
    { "corridor-end",          4 },
    { "room-small",            1 },
    { "stairs",                2 },
};

constexpr u32 kSpecCount = sizeof(kSpecs) / sizeof(kSpecs[0]);

} // namespace

u32 KenneyDungeonStyle::AppendTiles(
    graphics::wfc::WFCTileRegistry& registry,
    std::vector<const graphics::rhi::RHIMeshAsset*>& assets_out) const {

    if (!loaded_) {
        const u32 loaded_count = catalog_.LoadFromDirectory(
            pipeline_, dir_path_, colormap_path_);
        if (loaded_count == 0) {
            std::cerr << "[KenneyDungeonStyle] LoadFromDirectory returned 0 tiles for "
                      << dir_path_ << std::endl;
            return 0;
        }

        // Fetch RHIMeshAsset for each spec by looking up the catalog slot,
        // then content::get_rhi_mesh_asset on its geometry_content_id. The
        // catalog returns slots keyed by file stem (e.g. "template-wall").
        meshes_.clear();
        meshes_.resize(kSpecCount);
        bool all_loaded = true;
        for (u32 i = 0; i < kSpecCount; ++i) {
            const s32 slot_idx = catalog_.FindSlotByName(kSpecs[i].name);
            if (slot_idx < 0) {
                std::cerr << "[KenneyDungeonStyle] missing tile '"
                          << kSpecs[i].name << "'" << std::endl;
                all_loaded = false;
                break;
            }
            const auto geo_id = catalog_.Get(slot_idx).geometry_content_id;
            if (!primal::content::get_rhi_mesh_asset(geo_id, meshes_[i])) {
                std::cerr << "[KenneyDungeonStyle] get_rhi_mesh_asset failed for '"
                          << kSpecs[i].name << "' (geo_id=" << geo_id << ")" << std::endl;
                all_loaded = false;
                break;
            }
        }
        if (!all_loaded) {
            meshes_.clear();
            return 0;
        }
        loaded_ = true;
    }

    const u32 start = registry.Count();

    // Register each tile. variant_count is preserved (so the solver still
    // rotates multi-variant tiles via variant_transform), but the AutoSocket-
    // Classifier only inspects variant-0's mesh. mesh_handles[] are populated
    // from the catalog's render slots (LoadFromDirectory already registered
    // each tile via RegisterMeshResource).
    for (u32 i = 0; i < kSpecCount; ++i) {
        graphics::wfc::WFCTile t{};
        t.name          = kSpecs[i].name;
        t.category      = graphics::wfc::WFCCategory::Dungeon;
        t.variant_count = kSpecs[i].variant_count;
        t.bounds_extents = primal::math::v3{0.5f, 0.5f, 0.5f};

        const s32 slot_idx = catalog_.FindSlotByName(kSpecs[i].name);
        if (slot_idx >= 0) {
            const u32 render_slot = catalog_.Get(slot_idx).render_slot;
            for (u32 v = 0; v < graphics::wfc::WFCTile::MaxVariants; ++v) {
                t.mesh_handles[v] = primal::geometry::geometry_id{render_slot};
            }
        }
        registry.Register(t);
    }

    const u32 added = registry.Count() - start;
    if (added == 0) return 0;

    if (assets_out.size() < start + added) {
        assets_out.resize(start + added, nullptr);
    }
    const u32 mesh_count = static_cast<u32>(meshes_.size());
    for (u32 i = 0; i < added && i < mesh_count; ++i) {
        assets_out[start + i] = &meshes_[i];
    }

    return added;
}

} // namespace primal::test::kenney
