#include "../../TestFramework.h"
#include "Engine/Graphics/WFC/WFCTileCatalog.h"
#include "Engine/Graphics/WFC/WFCTileRegistry.h"
#include "Engine/Graphics/WFC/TileAdjacency.h"

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
        TEST_ASSERT(static_cast<u32>(t.mesh_handle) != 0 || i == 0,
                    "Mesh handle set (0 acceptable only for cube as placeholder)");
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

int main() {
    TestSuite suite("WFCTileCatalog");
    TEST_CASE(suite, "Populate_Registers_Five_Tiles", TestWFCTileCatalog_Populate_Registers_Five_Tiles);
    TEST_CASE(suite, "Ramp_Has_Four_Variants", TestWFCTileCatalog_Ramp_Has_Four_Variants);
    TEST_CASE(suite, "Tiles_Have_Valid_Mesh_Handles", TestWFCTileCatalog_Tiles_Have_Valid_Mesh_Handles);
    TEST_CASE(suite, "Cube_Self_Compat_On_All_Faces", TestWFCTileCatalog_Cube_Self_Compat_On_All_Faces);
    TEST_CASE(suite, "Ramp_Self_Compat_On_PosZ", TestWFCTileCatalog_Ramp_Self_Compat_On_PosZ);
    suite.RunAllTests();
    return 0;
}
