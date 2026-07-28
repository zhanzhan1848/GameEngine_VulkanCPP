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
    t.mesh_handle = kCubeMeshPlaceholder;
    t.variant_count = 1;
    t.bounds_extents = math::v3{1.0f, 1.0f, 1.0f};
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
    t.mesh_handle = kRampMeshPlaceholder;
    t.variant_count = 4;
    t.bounds_extents = math::v3{1.0f, 1.0f, 1.0f};
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
    t.mesh_handle = kCornerInMeshPlaceholder;
    t.variant_count = 1;
    t.bounds_extents = math::v3{1.0f, 1.0f, 1.0f};
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
    t.mesh_handle = kCornerOutMeshPlaceholder;
    t.variant_count = 1;
    t.bounds_extents = math::v3{1.0f, 1.0f, 1.0f};
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
    t.mesh_handle = kPillarMeshPlaceholder;
    t.variant_count = 1;
    t.bounds_extents = math::v3{1.0f, 1.0f, 1.0f};
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

    // Adjacency rules added in Task 7 (next task).
    (void)adjacency;
}

} // namespace primal::graphics::wfc
