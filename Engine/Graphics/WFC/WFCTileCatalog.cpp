#include "WFCTileCatalog.h"
#include "WFCTileRegistry.h"
#include "TileAdjacency.h"
#include "WFCTypes.h"
#include "WFCSocketOps.h"

namespace primal::graphics::wfc {

namespace {
// Phase A.3 placeholder mesh handles. Phase A.4 replaces with register_mesh_asset results.
constexpr geometry::geometry_id kCubeMeshPlaceholder{1000};
constexpr geometry::geometry_id kRampMeshPlaceholder{1001};
constexpr geometry::geometry_id kCornerInMeshPlaceholder{1002};
constexpr geometry::geometry_id kCornerOutMeshPlaceholder{1003};
constexpr geometry::geometry_id kPillarMeshPlaceholder{1004};

// Phase C.1 T18: Ruins tile placeholders. Real mesh registration happens in
// T22/T27 integration (engine-booted), not here — see primitive factories
// above for the same pattern.
constexpr geometry::geometry_id kBrokenCubeMeshBase{2000};   // +v per variant
constexpr geometry::geometry_id kMossyCubeMeshPlaceholder{2100};
constexpr geometry::geometry_id kVineCubeMeshBase{2200};     // +v per variant

// T19: more Ruins tile placeholders.
constexpr geometry::geometry_id kCollapsedPillarMeshBase{2300};  // +v per variant
constexpr geometry::geometry_id kCrackedWallMeshPlaceholder{2400};
constexpr geometry::geometry_id kWeatheredStoneMeshPlaceholder{2500};

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

WFCTile MakeBrokenCubeTile() {
    WFCTile t{};
    t.name = "broken_cube";
    t.bounds_extents = math::v3{1.0f, 1.0f, 1.0f};
    t.category = WFCCategory::Ruins;
    t.variant_count = 4;
    t.is_organic = false;
    t.is_rotationally_symmetric = false;
    for (u32 v = 0; v < 4; ++v) {
        t.mesh_handles[v] = geometry::geometry_id{
            static_cast<u32>(kBrokenCubeMeshBase) + v};
    }
    for (u32 v = 0; v < WFCTile::MaxVariants; ++v) {
        t.sockets[v] = DeriveSocketEncoding(t, v);
    }
    return t;
}

WFCTile MakeMossyCubeTile() {
    WFCTile t{};
    t.name = "mossy_cube";
    t.bounds_extents = math::v3{1.0f, 1.0f, 1.0f};
    t.category = WFCCategory::Ruins;
    t.variant_count = 1;
    t.is_organic = false;
    t.is_rotationally_symmetric = true;  // mossy cube = cube geometry, Y-symmetric
    t.mesh_handles[0] = kMossyCubeMeshPlaceholder;
    for (u32 v = 0; v < WFCTile::MaxVariants; ++v) {
        t.sockets[v] = DeriveSocketEncoding(t, v);
    }
    return t;
}

WFCTile MakeVineCubeTile() {
    WFCTile t{};
    t.name = "vine_cube";
    t.bounds_extents = math::v3{1.0f, 1.0f, 1.0f};
    t.category = WFCCategory::Ruins;
    t.variant_count = 4;
    t.is_organic = false;
    t.is_rotationally_symmetric = true;  // vine cube = cube geometry, Y-symmetric
    for (u32 v = 0; v < 4; ++v) {
        t.mesh_handles[v] = geometry::geometry_id{
            static_cast<u32>(kVineCubeMeshBase) + v};
    }
    for (u32 v = 0; v < WFCTile::MaxVariants; ++v) {
        t.sockets[v] = DeriveSocketEncoding(t, v);
    }
    return t;
}

WFCTile MakeCollapsedPillarTile() {
    WFCTile t{};
    t.name = "collapsed_pillar";
    t.bounds_extents = math::v3{1.0f, 1.0f, 1.0f};
    t.category = WFCCategory::Ruins;
    t.variant_count = 4;
    t.is_organic = false;
    t.is_rotationally_symmetric = false;  // 4 distinct tilt axes -> NOT Y-symmetric
    for (u32 v = 0; v < 4; ++v) {
        t.mesh_handles[v] = geometry::geometry_id{
            static_cast<u32>(kCollapsedPillarMeshBase) + v};
    }
    for (u32 v = 0; v < WFCTile::MaxVariants; ++v) {
        t.sockets[v] = DeriveSocketEncoding(t, v);
    }
    return t;
}

WFCTile MakeCrackedWallTile() {
    WFCTile t{};
    t.name = "cracked_wall";
    t.bounds_extents = math::v3{1.0f, 1.0f, 1.0f};
    t.category = WFCCategory::Ruins;
    t.variant_count = 1;
    t.is_organic = false;
    t.is_rotationally_symmetric = true;  // box geometry, Y-symmetric
    t.mesh_handles[0] = kCrackedWallMeshPlaceholder;
    for (u32 v = 0; v < WFCTile::MaxVariants; ++v) {
        t.sockets[v] = DeriveSocketEncoding(t, v);
    }
    return t;
}

WFCTile MakeWeatheredStoneTile() {
    WFCTile t{};
    t.name = "weathered_stone";
    t.bounds_extents = math::v3{1.0f, 1.0f, 1.0f};
    t.category = WFCCategory::Ruins;
    t.variant_count = 1;
    t.is_organic = false;
    t.is_rotationally_symmetric = true;  // box geometry, Y-symmetric
    t.mesh_handles[0] = kWeatheredStoneMeshPlaceholder;
    for (u32 v = 0; v < WFCTile::MaxVariants; ++v) {
        t.sockets[v] = DeriveSocketEncoding(t, v);
    }
    return t;
}

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
