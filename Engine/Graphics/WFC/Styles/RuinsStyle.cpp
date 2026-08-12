// Engine/Graphics/WFC/Styles/RuinsStyle.cpp
#include "RuinsStyle.h"
#include "../TileAdjacency.h"
#include "../WFCTileCatalog.h"
#include "../WFCTileRegistry.h"
#include "../../../Content/ProceduralMesh.h"

namespace primal::graphics::wfc {

namespace {

// Per-tile factory dispatch. Lifted from the prior TestKenneyTilePreview
// `build_tile_asset` lambda (Phase C.1 Ruins Distinction). Params chosen for
// visual variety within each factory's output range (different seeds, slopes,
// tilt axes). Tile IDs 0..14 match WFCTileCatalog::Populate's emit order:
//   0 cube, 1 ramp, 2 corner_in, 3 corner_out, 4 pillar,
//   5 broken_cube, 6 mossy_cube, 7 collapsed_pillar_alt, 8 rubble_pile,
//   9 cracked_wall (stub), 10 vine_cube, 11 weathered_stone,
//   12 broken_corner_in (stub), 13 broken_corner_out (stub),
//   14 debris_small.
void BuildRuinsTileAsset(rhi::RHIMeshAsset& out, u32 tid) {
    using namespace primal::content;
    switch (tid) {
        case 0:
            create_weathered_cube_mesh(out, 1, 1, 1, 100u, 0.020f);
            break;
        case 1:
            create_ramp_mesh(out, 1.0f, 1.0f, 1.0f, 0.0f);
            break;
        case 2:
            create_corner_in_mesh(out, 1.0f, 1.0f, 1.0f);
            break;
        case 3:
            create_corner_out_mesh(out, 1.0f, 1.0f, 1.0f);
            break;
        case 4:
            create_collapsed_pillar_mesh(out, 0.5f, 1.0f, TiltAxis::PlusX, 0.05f);
            break;
        case 5:
            create_broken_cube_mesh(out, 1, 1, 1, BrokenCorner::PosXYZ);
            break;
        case 6:
            create_weathered_cube_mesh(out, 1, 1, 1, 600u, 0.005f);
            break;
        case 7:
            create_collapsed_pillar_mesh(out, 0.5f, 1.0f, TiltAxis::PlusZ, 0.20f);
            break;
        case 8:
            create_rubble_pile_mesh(out, 800u, 0.5f);
            break;
        case 9:
            // Phase C.1 stub: emits plain cube. See ProceduralMesh.h:699.
            create_cracked_wall_mesh(out, 1.0f, 1.0f, 1.0f, 900u);
            break;
        case 10:
            create_weathered_cube_mesh(out, 1, 1, 1, 700u, 0.030f);
            break;
        case 11:
            create_weathered_cube_mesh(out, 1, 1, 1, 800u, 0.015f);
            break;
        case 12:
            // Phase C.1 stub: delegates to broken_cube. See ProceduralMesh.h:617.
            create_broken_corner_in_mesh(out, 1.0f, 1.0f, 1.0f,
                                         BrokenCorner::PosXNegZ);
            break;
        case 13:
            // Phase C.1 stub: delegates to broken_cube. See ProceduralMesh.h:623.
            create_broken_corner_out_mesh(out, 1.0f, 1.0f, 1.0f,
                                          BrokenCorner::NegXPosZ);
            break;
        case 14:
            create_debris_small_mesh(out, 900u, 0.35f);
            break;
        default:
            create_weathered_cube_mesh(out, 1, 1, 1, 999u, 0.020f);
            break;
    }
}

} // namespace

u32 RuinsStyle::AppendTiles(WFCTileRegistry& registry,
                            std::vector<const rhi::RHIMeshAsset*>& assets_out) const {
    const u32 start = registry.Count();

    // Populate hands us 15 tiles with hand-written metadata (name, variant_count,
    // bounds_extents). The adjacency it writes is discarded — Compose rebuilds
    // adjacency uniformly via BuildFromClassifier over all selected styles.
    TileAdjacencyTable dummy_adjacency;
    WFCTileCatalog::Populate(registry, dummy_adjacency);

    const u32 added = registry.Count() - start;
    if (added == 0) return 0;

    // Lazy-build the per-tile assets on first call; reuse on subsequent calls.
    if (ruins_meshes_.empty()) {
        ruins_meshes_.resize(15);
        for (u32 tid = 0; tid < 15; ++tid) {
            BuildRuinsTileAsset(ruins_meshes_[tid], tid);
        }
    }

    // Force category = Ruins for the entire set. Populate already sets this for
    // tiles 5..14 but tiles 0..4 inherit Primitive from the WFCTile default.
    for (u32 i = 0; i < added; ++i) {
        WFCTile& t = registry.GetMutable(wfc_tile_id{start + i});
        t.category = WFCCategory::Ruins;
    }

    // Expose mesh pointers in assets_out at tile-ID indices.
    if (assets_out.size() < start + added) {
        assets_out.resize(start + added, nullptr);
    }
    const u32 mesh_count = static_cast<u32>(ruins_meshes_.size());
    for (u32 i = 0; i < added && i < mesh_count; ++i) {
        assets_out[start + i] = &ruins_meshes_[i];
    }

    return added;
}

} // namespace primal::graphics::wfc
