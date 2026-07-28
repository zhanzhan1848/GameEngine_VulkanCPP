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

int main() {
    TestSuite suite("WFCTileCatalog");
    TEST_CASE(suite, "Populate_Registers_Five_Tiles", TestWFCTileCatalog_Populate_Registers_Five_Tiles);
    TEST_CASE(suite, "Ramp_Has_Four_Variants", TestWFCTileCatalog_Ramp_Has_Four_Variants);
    TEST_CASE(suite, "Tiles_Have_Valid_Mesh_Handles", TestWFCTileCatalog_Tiles_Have_Valid_Mesh_Handles);
    suite.RunAllTests();
    return 0;
}
