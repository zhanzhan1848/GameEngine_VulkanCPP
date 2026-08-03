#include "../../TestFramework.h"
#include "Engine/Graphics/WFC/WFCTileCatalog.h"
#include "Engine/Graphics/WFC/WFCTileRegistry.h"
#include "Engine/Graphics/WFC/TileAdjacency.h"
#include "Engine/Graphics/WFC/WaveGrid.h"
#include "Engine/Graphics/WFC/WFCSolver.h"
#include "Engine/Graphics/WFC/WFCStepBuffer.h"
#include "Engine/Graphics/WFC/WFCConfig.h"
#include "Engine/Graphics/WFC/WFCSolveBudget.h"

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
    suite.RunAllTests();
    return 0;
}
