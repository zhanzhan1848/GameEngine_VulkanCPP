#include "../../TestFramework.h"
#include "Engine/Graphics/WFC/WFCTileCatalog.h"
#include "Engine/Graphics/WFC/WFCTileRegistry.h"
#include "Engine/Graphics/WFC/TileAdjacency.h"
#include "Engine/Graphics/WFC/WaveGrid.h"
#include "Engine/Graphics/WFC/WFCSolver.h"
#include "Engine/Graphics/WFC/WFCStepBuffer.h"
#include "Engine/Graphics/WFC/WFCConfig.h"
#include "Engine/Graphics/WFC/WFCSolveBudget.h"
#include "Engine/Graphics/WFC/WFCCategory.h"

using namespace primal::graphics::wfc;
using namespace Engine::Test;

TestResult TestWFCTileCatalog_Populate_Registers_Five_Tiles() {
    WFCTileRegistry reg;
    TileAdjacencyTable adj;
    WFCTileCatalog::Populate(reg, adj);

    TEST_ASSERT_EQ(5u, reg.Count(), "5 tile types registered");
    return TestResult::Passed;
}

TestResult TestWFCTileCatalog_Ramp_Has_Four_Variants() {
    WFCTileRegistry reg;
    TileAdjacencyTable adj;
    WFCTileCatalog::Populate(reg, adj);

    const WFCTile& ramp = reg.Get(wfc_tile_id{1});
    TEST_ASSERT_STR_EQ("ramp", ramp.name, "Tile 1 is ramp");
    TEST_ASSERT_EQ(4u, ramp.variant_count, "Ramp has 4 rotation variants");
    return TestResult::Passed;
}

TestResult TestWFCTileCatalog_Tiles_Have_Valid_Mesh_Handles() {
    WFCTileRegistry reg;
    TileAdjacencyTable adj;
    WFCTileCatalog::Populate(reg, adj);

    for (u32 i = 0; i < reg.Count(); ++i) {
        const WFCTile& t = reg.Get(wfc_tile_id{i});
        TEST_ASSERT(static_cast<u32>(t.mesh_handles[0]) != 0 || i == 0,
                    "Mesh handle[0] set (0 acceptable only for cube as placeholder)");
    }
    return TestResult::Passed;
}

TestResult TestWFCTileCatalog_Cube_Self_Compat_On_All_Faces() {
    WFCTileRegistry reg;
    TileAdjacencyTable adj;
    WFCTileCatalog::Populate(reg, adj);

    const wfc_tile_id cube{0};
    for (u32 face = 0; face < 6; ++face) {
        WFCFace f = static_cast<WFCFace>(face);
        TEST_ASSERT(adj.Compatible(cube, 0, f, cube, 0),
                    "Cube self-compatible on all faces");
    }
    return TestResult::Passed;
}

TestResult TestWFCTileCatalog_Ramp_Self_Compat_On_PosZ() {
    WFCTileRegistry reg;
    TileAdjacencyTable adj;
    WFCTileCatalog::Populate(reg, adj);

    const wfc_tile_id ramp{1};
    TEST_ASSERT(adj.Compatible(ramp, 0, WFCFace::PosZ, ramp, 0),
                "Ramp var 0 self-compat +Z");
    TEST_ASSERT(adj.Compatible(ramp, 0, WFCFace::NegZ, ramp, 0),
                "Ramp var 0 self-compat -Z (mirror)");
    return TestResult::Passed;
}

TestResult TestWFCTileCatalog_Solves_4x4x4_With_Catalog() {
    WFCTileRegistry reg;
    TileAdjacencyTable adj;
    WFCTileCatalog::Populate(reg, adj);

    WFCConfig config;
    config.grid_size = {4, 4, 4};
    config.max_cells_per_frame = 256;
    config.max_ms_per_frame = 1000;
    config.seed = 7;
    config.max_generations = 8;

    WaveGrid grid;
    grid.Initialize(config.grid_size, 8);

    WFCStepBuffer buf;
    WFCSolver solver;
    solver.Initialize(config, grid, reg, adj, buf);

    WFCSolveBudget budget(config.max_cells_per_frame, config.max_ms_per_frame);
    budget.Reset();

    WFCSolver::StepResult result = WFCSolver::StepResult::InProgress;
    u32 steps = 0;
    while (result == WFCSolver::StepResult::InProgress && steps < 1000) {
        result = solver.Step(budget);
        ++steps;
    }

    TEST_ASSERT(result == WFCSolver::StepResult::Done ||
                result == WFCSolver::StepResult::GivenUp,
                "Solver terminates with catalog tiles");
    return TestResult::Passed;
}

TestResult TestWFCTileCatalog_Corners_Has_Four_Variants_Each() {
    WFCTileRegistry reg;
    TileAdjacencyTable adj;
    WFCTileCatalog::Populate(reg, adj);

    const WFCTile& corner_in = reg.Get(wfc_tile_id{2});
    TEST_ASSERT_EQ(4u, corner_in.variant_count, "corner_in has 4 variants (Phase B.1)");

    const WFCTile& corner_out = reg.Get(wfc_tile_id{3});
    TEST_ASSERT_EQ(4u, corner_out.variant_count, "corner_out has 4 variants (Phase B.1)");
    return TestResult::Passed;
}

TestResult TestWFCTileCatalog_Primitive_Tiles_Have_Primitive_Category() {
    WFCTileRegistry reg;
    TileAdjacencyTable adj;
    WFCTileCatalog::Populate(reg, adj);

    TEST_ASSERT_EQ(5u, reg.Count(), "5 primitive tiles registered");
    for (u32 i = 0; i < 5u; ++i) {
        const WFCTile& t = reg.Get(wfc_tile_id{i});
        TEST_ASSERT_EQ(static_cast<u32>(WFCCategory::Primitive),
                       static_cast<u32>(t.category),
                       "primitive tile category == Primitive");
        TEST_ASSERT(t.mesh_handles[0] != primal::geometry::geometry_id{0},
                    "primitive tile mesh_handles[0] populated");
    }
    return TestResult::Passed;
}

TestResult TestMakeBrokenCubeTile_FourVariants_RuinsCategory() {
    WFCTileRegistry reg;
    reg.Register(MakeBrokenCubeTile());
    TEST_ASSERT_EQ(1u, reg.Count(), "1 tile registered");
    const WFCTile& t = reg.Get(wfc_tile_id{0});
    TEST_ASSERT_EQ(4u, t.variant_count, "broken_cube has 4 variants");
    TEST_ASSERT_EQ(static_cast<u32>(WFCCategory::Ruins),
                   static_cast<u32>(t.category), "broken_cube is Ruins");
    TEST_ASSERT(!t.is_rotationally_symmetric, "broken_cube NOT rotationally symmetric");
    for (u32 v = 0; v < 4u; ++v) {
        TEST_ASSERT(t.mesh_handles[v] != primal::geometry::geometry_id{0},
                    "broken_cube variant has non-zero mesh_handles");
    }
    return TestResult::Passed;
}

TestResult TestMakeMossyCubeTile_SingleVariant_RuinsCategory() {
    WFCTileRegistry reg;
    reg.Register(MakeMossyCubeTile());
    const WFCTile& t = reg.Get(wfc_tile_id{0});
    TEST_ASSERT_EQ(1u, t.variant_count, "mossy_cube has 1 variant");
    TEST_ASSERT_EQ(static_cast<u32>(WFCCategory::Ruins),
                   static_cast<u32>(t.category), "mossy_cube is Ruins");
    TEST_ASSERT(t.mesh_handles[0] != primal::geometry::geometry_id{0},
                "mossy_cube mesh_handles[0] set");
    return TestResult::Passed;
}

TestResult TestMakeVineCubeTile_FourVariants_RuinsCategory() {
    WFCTileRegistry reg;
    reg.Register(MakeVineCubeTile());
    const WFCTile& t = reg.Get(wfc_tile_id{0});
    TEST_ASSERT_EQ(4u, t.variant_count, "vine_cube has 4 variants");
    TEST_ASSERT_EQ(static_cast<u32>(WFCCategory::Ruins),
                   static_cast<u32>(t.category), "vine_cube is Ruins");
    for (u32 v = 0; v < 4u; ++v) {
        TEST_ASSERT(t.mesh_handles[v] != primal::geometry::geometry_id{0},
                    "vine_cube variant has non-zero mesh_handles");
    }
    return TestResult::Passed;
}

TestResult TestMakeCollapsedPillarTile_FourVariants_RuinsCategory() {
    WFCTileRegistry reg;
    reg.Register(MakeCollapsedPillarTile());
    const WFCTile& t = reg.Get(wfc_tile_id{0});
    TEST_ASSERT_EQ(4u, t.variant_count, "collapsed_pillar has 4 variants");
    TEST_ASSERT_EQ(static_cast<u32>(WFCCategory::Ruins),
                   static_cast<u32>(t.category), "collapsed_pillar is Ruins");
    TEST_ASSERT(!t.is_rotationally_symmetric, "collapsed_pillar NOT rotationally symmetric");
    for (u32 v = 0; v < 4u; ++v) {
        TEST_ASSERT(t.mesh_handles[v] != primal::geometry::geometry_id{0},
                    "collapsed_pillar variant has non-zero mesh_handles");
    }
    return TestResult::Passed;
}

TestResult TestMakeCrackedWallTile_SingleVariant_RuinsCategory() {
    WFCTileRegistry reg;
    reg.Register(MakeCrackedWallTile());
    const WFCTile& t = reg.Get(wfc_tile_id{0});
    TEST_ASSERT_EQ(1u, t.variant_count, "cracked_wall has 1 variant");
    TEST_ASSERT_EQ(static_cast<u32>(WFCCategory::Ruins),
                   static_cast<u32>(t.category), "cracked_wall is Ruins");
    TEST_ASSERT(t.mesh_handles[0] != primal::geometry::geometry_id{0},
                "cracked_wall mesh_handles[0] set");
    return TestResult::Passed;
}

TestResult TestMakeWeatheredStoneTile_SingleVariant_RuinsCategory() {
    WFCTileRegistry reg;
    reg.Register(MakeWeatheredStoneTile());
    const WFCTile& t = reg.Get(wfc_tile_id{0});
    TEST_ASSERT_EQ(1u, t.variant_count, "weathered_stone has 1 variant");
    TEST_ASSERT_EQ(static_cast<u32>(WFCCategory::Ruins),
                   static_cast<u32>(t.category), "weathered_stone is Ruins");
    TEST_ASSERT(t.mesh_handles[0] != primal::geometry::geometry_id{0},
                "weathered_stone mesh_handles[0] set");
    return TestResult::Passed;
}

TestResult TestMakeRubblePileTile_SingleVariant_RuinsCategory() {
    WFCTileRegistry reg;
    reg.Register(MakeRubblePileTile());
    const WFCTile& t = reg.Get(wfc_tile_id{0});
    TEST_ASSERT_EQ(1u, t.variant_count, "rubble_pile has 1 variant");
    TEST_ASSERT_EQ(static_cast<u32>(WFCCategory::Ruins),
                   static_cast<u32>(t.category), "rubble_pile is Ruins");
    TEST_ASSERT(t.mesh_handles[0] != primal::geometry::geometry_id{0},
                "rubble_pile mesh_handles[0] set");
    // rubble_pile has flat extents (sy=0.5 < sx=sz=1.0)
    TEST_ASSERT(t.bounds_extents.y < t.bounds_extents.x, "rubble_pile y < x");
    TEST_ASSERT(t.bounds_extents.y < t.bounds_extents.z, "rubble_pile y < z");
    return TestResult::Passed;
}

TestResult TestMakeDebrisSmallTile_SingleVariant_RuinsCategory() {
    WFCTileRegistry reg;
    reg.Register(MakeDebrisSmallTile());
    const WFCTile& t = reg.Get(wfc_tile_id{0});
    TEST_ASSERT_EQ(1u, t.variant_count, "debris_small has 1 variant");
    TEST_ASSERT_EQ(static_cast<u32>(WFCCategory::Ruins),
                   static_cast<u32>(t.category), "debris_small is Ruins");
    TEST_ASSERT(t.mesh_handles[0] != primal::geometry::geometry_id{0},
                "debris_small mesh_handles[0] set");
    // debris_small is smaller than rubble_pile overall
    TEST_ASSERT(t.bounds_extents.x < 1.0f, "debris_small x < 1");
    return TestResult::Passed;
}

TestResult TestMakeBrokenCornerInTile_FourVariants_RuinsCategory() {
    WFCTileRegistry reg;
    reg.Register(MakeBrokenCornerInTile());
    const WFCTile& t = reg.Get(wfc_tile_id{0});
    TEST_ASSERT_EQ(4u, t.variant_count, "broken_corner_in has 4 variants");
    TEST_ASSERT_EQ(static_cast<u32>(WFCCategory::Ruins),
                   static_cast<u32>(t.category), "broken_corner_in is Ruins");
    TEST_ASSERT(!t.is_rotationally_symmetric,
                "broken_corner_in NOT rotationally symmetric (4 distinct corners)");
    for (u32 v = 0; v < 4u; ++v) {
        TEST_ASSERT(t.mesh_handles[v] != primal::geometry::geometry_id{0},
                    "broken_corner_in variant has non-zero mesh_handles");
    }
    return TestResult::Passed;
}

TestResult TestMakeBrokenCornerOutTile_FourVariants_RuinsCategory() {
    WFCTileRegistry reg;
    reg.Register(MakeBrokenCornerOutTile());
    const WFCTile& t = reg.Get(wfc_tile_id{0});
    TEST_ASSERT_EQ(4u, t.variant_count, "broken_corner_out has 4 variants");
    TEST_ASSERT_EQ(static_cast<u32>(WFCCategory::Ruins),
                   static_cast<u32>(t.category), "broken_corner_out is Ruins");
    TEST_ASSERT(!t.is_rotationally_symmetric,
                "broken_corner_out NOT rotationally symmetric");
    for (u32 v = 0; v < 4u; ++v) {
        TEST_ASSERT(t.mesh_handles[v] != primal::geometry::geometry_id{0},
                    "broken_corner_out variant has non-zero mesh_handles");
    }
    return TestResult::Passed;
}

int main() {
    TestSuite suite("WFCTileCatalog");
    TEST_CASE(suite, "Populate_Registers_Five_Tiles", TestWFCTileCatalog_Populate_Registers_Five_Tiles);
    TEST_CASE(suite, "Ramp_Has_Four_Variants", TestWFCTileCatalog_Ramp_Has_Four_Variants);
    TEST_CASE(suite, "Tiles_Have_Valid_Mesh_Handles", TestWFCTileCatalog_Tiles_Have_Valid_Mesh_Handles);
    TEST_CASE(suite, "Cube_Self_Compat_On_All_Faces", TestWFCTileCatalog_Cube_Self_Compat_On_All_Faces);
    TEST_CASE(suite, "Ramp_Self_Compat_On_PosZ", TestWFCTileCatalog_Ramp_Self_Compat_On_PosZ);
    TEST_CASE(suite, "Solves_4x4x4_With_Catalog", TestWFCTileCatalog_Solves_4x4x4_With_Catalog);
    TEST_CASE(suite, "Corners_Has_Four_Variants_Each",
              TestWFCTileCatalog_Corners_Has_Four_Variants_Each);
    TEST_CASE(suite, "Primitive_Tiles_Have_Primitive_Category",
              TestWFCTileCatalog_Primitive_Tiles_Have_Primitive_Category);
    TEST_CASE(suite, "MakeBrokenCubeTile_FourVariants_RuinsCategory",
              TestMakeBrokenCubeTile_FourVariants_RuinsCategory);
    TEST_CASE(suite, "MakeMossyCubeTile_SingleVariant_RuinsCategory",
              TestMakeMossyCubeTile_SingleVariant_RuinsCategory);
    TEST_CASE(suite, "MakeVineCubeTile_FourVariants_RuinsCategory",
              TestMakeVineCubeTile_FourVariants_RuinsCategory);
    TEST_CASE(suite, "MakeCollapsedPillarTile_FourVariants_RuinsCategory",
              TestMakeCollapsedPillarTile_FourVariants_RuinsCategory);
    TEST_CASE(suite, "MakeCrackedWallTile_SingleVariant_RuinsCategory",
              TestMakeCrackedWallTile_SingleVariant_RuinsCategory);
    TEST_CASE(suite, "MakeWeatheredStoneTile_SingleVariant_RuinsCategory",
              TestMakeWeatheredStoneTile_SingleVariant_RuinsCategory);
    TEST_CASE(suite, "MakeRubblePileTile_SingleVariant_RuinsCategory",
              TestMakeRubblePileTile_SingleVariant_RuinsCategory);
    TEST_CASE(suite, "MakeDebrisSmallTile_SingleVariant_RuinsCategory",
              TestMakeDebrisSmallTile_SingleVariant_RuinsCategory);
    TEST_CASE(suite, "MakeBrokenCornerInTile_FourVariants_RuinsCategory",
              TestMakeBrokenCornerInTile_FourVariants_RuinsCategory);
    TEST_CASE(suite, "MakeBrokenCornerOutTile_FourVariants_RuinsCategory",
              TestMakeBrokenCornerOutTile_FourVariants_RuinsCategory);
    suite.RunAllTests();
    return 0;
}
