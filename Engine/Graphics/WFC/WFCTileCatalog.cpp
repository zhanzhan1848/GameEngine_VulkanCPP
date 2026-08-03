#include "WFCTileCatalog.h"
#include "WFCTileRegistry.h"
#include "TileAdjacency.h"
#include "WFCTypes.h"

namespace primal::graphics::wfc {

namespace {
// Phase A.3 placeholder mesh handles. Phase A.4 replaces with register_mesh_asset results.
constexpr geometry::geometry_id kCubeMeshPlaceholder{1000};
constexpr geometry::geometry_id kRampMeshPlaceholder{1001};
constexpr geometry::geometry_id kCornerInMeshPlaceholder{1002};
constexpr geometry::geometry_id kCornerOutMeshPlaceholder{1003};
constexpr geometry::geometry_id kPillarMeshPlaceholder{1004};

WFCTile MakeCubeTile() {
    WFCTile t{};
    t.name = "cube";
    t.mesh_handles[0] = kCubeMeshPlaceholder;
    t.variant_count = 1;
    t.bounds_extents = math::v3{1.0f, 1.0f, 1.0f};
    t.category = WFCCategory::Primitive;
    t.is_organic = false;
    t.is_rotationally_symmetric = true;
    for (u32 i = 0; i < WFCTile::MaxVariants; ++i) {
        t.sockets[i] = 0;
    }
    return t;
}

WFCTile MakeRampTile() {
    WFCTile t{};
    t.name = "ramp";
    t.mesh_handles[0] = kRampMeshPlaceholder;
    t.variant_count = 4;
    t.bounds_extents = math::v3{1.0f, 1.0f, 1.0f};
    t.category = WFCCategory::Primitive;
    t.is_organic = false;
    t.is_rotationally_symmetric = false;
    for (u32 i = 0; i < WFCTile::MaxVariants; ++i) {
        t.sockets[i] = 0;
    }
    return t;
}

WFCTile MakeCornerInTile() {
    WFCTile t{};
    t.name = "corner_in";
    t.mesh_handles[0] = kCornerInMeshPlaceholder;
    t.variant_count = 4;
    t.bounds_extents = math::v3{1.0f, 1.0f, 1.0f};
    t.category = WFCCategory::Primitive;
    t.is_organic = false;
    t.is_rotationally_symmetric = false;
    for (u32 i = 0; i < WFCTile::MaxVariants; ++i) {
        t.sockets[i] = 0;
    }
    return t;
}

WFCTile MakeCornerOutTile() {
    WFCTile t{};
    t.name = "corner_out";
    t.mesh_handles[0] = kCornerOutMeshPlaceholder;
    t.variant_count = 4;
    t.bounds_extents = math::v3{1.0f, 1.0f, 1.0f};
    t.category = WFCCategory::Primitive;
    t.is_organic = false;
    t.is_rotationally_symmetric = false;
    for (u32 i = 0; i < WFCTile::MaxVariants; ++i) {
        t.sockets[i] = 0;
    }
    return t;
}

WFCTile MakePillarTile() {
    WFCTile t{};
    t.name = "pillar";
    t.mesh_handles[0] = kPillarMeshPlaceholder;
    t.variant_count = 1;
    t.bounds_extents = math::v3{1.0f, 1.0f, 1.0f};
    t.category = WFCCategory::Primitive;
    t.is_organic = false;
    t.is_rotationally_symmetric = true;
    for (u32 i = 0; i < WFCTile::MaxVariants; ++i) {
        t.sockets[i] = 0;
    }
    return t;
}
} // namespace

void WFCTileCatalog::Populate(WFCTileRegistry& registry, TileAdjacencyTable& adjacency) {
    registry.Register(MakeCubeTile());
    registry.Register(MakeRampTile());
    registry.Register(MakeCornerInTile());
    registry.Register(MakeCornerOutTile());
    registry.Register(MakePillarTile());

    const wfc_tile_id cube{0};
    const wfc_tile_id ramp{1};
    const wfc_tile_id corner_in{2};
    const wfc_tile_id corner_out{3};
    const wfc_tile_id pillar{4};

    // Helper: full pairwise compat on all 6 faces (for tiles that always fit together)
    auto AddFullCompat = [&](wfc_tile_id a, u32 a_var, wfc_tile_id b, u32 b_var) {
        adjacency.AddCompatibility(a, a_var, WFCFace::PosX, b, b_var);
        adjacency.AddCompatibility(a, a_var, WFCFace::NegX, b, b_var);
        adjacency.AddCompatibility(a, a_var, WFCFace::PosY, b, b_var);
        adjacency.AddCompatibility(a, a_var, WFCFace::NegY, b, b_var);
        adjacency.AddCompatibility(a, a_var, WFCFace::PosZ, b, b_var);
        adjacency.AddCompatibility(a, a_var, WFCFace::NegZ, b, b_var);
    };

    // Phase A.3 simplified rules: cube is universally compatible (wildcard structural tile).
    // Other tiles self-compatible + cube-compatible on all faces.
    for (u32 v = 0; v < 4; ++v) {
        AddFullCompat(cube, 0, ramp, v);
        AddFullCompat(ramp, v, ramp, v);  // ramp self-compat (same variant)
    }
    for (u32 v = 0; v < 4; ++v) {
        AddFullCompat(cube, 0, corner_in, v);
        AddFullCompat(cube, 0, corner_out, v);
        AddFullCompat(corner_in, v, corner_in, v);   // self-compat same variant
        AddFullCompat(corner_out, v, corner_out, v); // self-compat same variant
    }
    AddFullCompat(cube, 0, pillar, 0);
    AddFullCompat(pillar, 0, pillar, 0);
    AddFullCompat(cube, 0, cube, 0);  // cube self-compat
}

} // namespace primal::graphics::wfc
