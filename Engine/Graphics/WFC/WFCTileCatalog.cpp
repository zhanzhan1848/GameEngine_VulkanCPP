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

// T20: rubble_pile + debris_small (single variant each).
constexpr geometry::geometry_id kRubblePileMeshPlaceholder{2600};
constexpr geometry::geometry_id kDebrisSmallMeshPlaceholder{2700};

// T21: broken_corner_in/out (4 variants each, one per corner).
constexpr geometry::geometry_id kBrokenCornerInMeshBase{2800};   // +v per variant
constexpr geometry::geometry_id kBrokenCornerOutMeshBase{2900};  // +v per variant

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

WFCTile MakeRubblePileTile() {
    WFCTile t{};
    t.name = "rubble_pile";
    t.bounds_extents = math::v3{1.0f, 0.5f, 1.0f};  // flat pile
    t.category = WFCCategory::Ruins;
    t.variant_count = 1;
    t.is_organic = false;
    t.is_rotationally_symmetric = true;  // rubble is unordered; Y-symmetric
    t.mesh_handles[0] = kRubblePileMeshPlaceholder;
    for (u32 v = 0; v < WFCTile::MaxVariants; ++v) {
        t.sockets[v] = DeriveSocketEncoding(t, v);
    }
    return t;
}

WFCTile MakeDebrisSmallTile() {
    WFCTile t{};
    t.name = "debris_small";
    t.bounds_extents = math::v3{0.8f, 0.25f, 0.8f};  // small flat debris
    t.category = WFCCategory::Ruins;
    t.variant_count = 1;
    t.is_organic = false;
    t.is_rotationally_symmetric = true;  // debris is unordered; Y-symmetric
    t.mesh_handles[0] = kDebrisSmallMeshPlaceholder;
    for (u32 v = 0; v < WFCTile::MaxVariants; ++v) {
        t.sockets[v] = DeriveSocketEncoding(t, v);
    }
    return t;
}

WFCTile MakeBrokenCornerInTile() {
    WFCTile t{};
    t.name = "broken_corner_in";
    t.bounds_extents = math::v3{1.0f, 1.0f, 1.0f};
    t.category = WFCCategory::Ruins;
    t.variant_count = 4;
    t.is_organic = false;
    t.is_rotationally_symmetric = false;  // 4 distinct broken corners
    for (u32 v = 0; v < 4; ++v) {
        t.mesh_handles[v] = geometry::geometry_id{
            static_cast<u32>(kBrokenCornerInMeshBase) + v};
    }
    for (u32 v = 0; v < WFCTile::MaxVariants; ++v) {
        t.sockets[v] = DeriveSocketEncoding(t, v);
    }
    return t;
}

WFCTile MakeBrokenCornerOutTile() {
    WFCTile t{};
    t.name = "broken_corner_out";
    t.bounds_extents = math::v3{1.0f, 1.0f, 1.0f};
    t.category = WFCCategory::Ruins;
    t.variant_count = 4;
    t.is_organic = false;
    t.is_rotationally_symmetric = false;  // 4 distinct broken corners
    for (u32 v = 0; v < 4; ++v) {
        t.mesh_handles[v] = geometry::geometry_id{
            static_cast<u32>(kBrokenCornerOutMeshBase) + v};
    }
    for (u32 v = 0; v < WFCTile::MaxVariants; ++v) {
        t.sockets[v] = DeriveSocketEncoding(t, v);
    }
    return t;
}

void WFCTileCatalog::Populate(WFCTileRegistry& registry, TileAdjacencyTable& adjacency) {
    // 5 Primitive tiles (tile_id = 0..4).
    registry.Register(MakeCubeTile());        // 0
    registry.Register(MakeRampTile());        // 1
    registry.Register(MakeCornerInTile());    // 2
    registry.Register(MakeCornerOutTile());   // 3
    registry.Register(MakePillarTile());      // 4

    // 10 Ruins tiles (tile_id = 5..14).
    registry.Register(MakeBrokenCubeTile());       // 5
    registry.Register(MakeMossyCubeTile());        // 6
    registry.Register(MakeCollapsedPillarTile());  // 7
    registry.Register(MakeRubblePileTile());       // 8
    registry.Register(MakeCrackedWallTile());      // 9
    registry.Register(MakeVineCubeTile());         // 10
    registry.Register(MakeWeatheredStoneTile());   // 11
    registry.Register(MakeBrokenCornerInTile());   // 12
    registry.Register(MakeBrokenCornerOutTile());  // 13
    registry.Register(MakeDebrisSmallTile());      // 14

    // Hand-written structural rules. Cube is the universal structural tile:
    // players expect cubes to stack and to abut every other tile type on
    // every face. Strict socket matching would reject cube-cube on +Y/-Y
    // (0xFF vs 0x00) — hand-write cube wildcard to override.
    const wfc_tile_id cube{0};

    // Helper: full 6-face wildcard compat between (a, a_var) and (b, b_var).
    auto AddFullCompat = [&](wfc_tile_id a, u32 a_var, wfc_tile_id b, u32 b_var) {
        adjacency.AddCompatibility(a, a_var, WFCFace::PosX, b, b_var);
        adjacency.AddCompatibility(a, a_var, WFCFace::NegX, b, b_var);
        adjacency.AddCompatibility(a, a_var, WFCFace::PosY, b, b_var);
        adjacency.AddCompatibility(a, a_var, WFCFace::NegY, b, b_var);
        adjacency.AddCompatibility(a, a_var, WFCFace::PosZ, b, b_var);
        adjacency.AddCompatibility(a, a_var, WFCFace::NegZ, b, b_var);
    };

    // Cube self-compat (all 6 faces) — overrides socket-signature mismatch.
    AddFullCompat(cube, 0, cube, 0);

    // Cube wildcard with every other tile (variants 0 only, to keep rule set
    // manageable — solver can pick variant via socket matching elsewhere).
    for (u32 t = 1; t < 15; ++t) {
        AddFullCompat(cube, 0, wfc_tile_id{t}, 0);
    }

    // Auto-derive remaining rules via socket compatibility.
    // skip_existing=true avoids double-counting already-hand-written pairs.
    adjacency.AddAutoFromSockets(registry, /*skip_existing=*/true);
}

} // namespace primal::graphics::wfc
