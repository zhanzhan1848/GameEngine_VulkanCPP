#include "../../TestFramework.h"
#include "Engine/Graphics/WFC/TileAdjacency.h"
#include "Engine/Graphics/WFC/WFCTypes.h"

using namespace primal::graphics::wfc;
using namespace Engine::Test;

TestResult TestTileAdjacency_Add_And_Check_Compatible() {
    TileAdjacencyTable table;
    const wfc_tile_id tileA{1};
    const wfc_tile_id tileB{2};
    table.AddCompatibility(tileA, 0, WFCFace::PosX, tileB, 0);

    TEST_ASSERT(table.Compatible(tileA, 0, WFCFace::PosX, tileB, 0),
                "Direct compatibility should hold after Add");
    TEST_ASSERT(table.Compatible(tileB, 0, WFCFace::NegX, tileA, 0),
                "Reverse direction should also be compatible (mirror)");
    return TestResult::Passed;
}

TestResult TestTileAdjacency_Incompatible_Returns_False() {
    TileAdjacencyTable table;
    const wfc_tile_id tileA{1};
    const wfc_tile_id tileB{2};
    TEST_ASSERT(!table.Compatible(tileA, 0, WFCFace::PosX, tileB, 0),
                "Unregistered pair should be incompatible");
    return TestResult::Passed;
}

TestResult TestTileAdjacency_Clear() {
    TileAdjacencyTable table;
    const wfc_tile_id tileA{1};
    const wfc_tile_id tileB{2};
    table.AddCompatibility(tileA, 0, WFCFace::PosX, tileB, 0);
    table.Clear();
    TEST_ASSERT(!table.Compatible(tileA, 0, WFCFace::PosX, tileB, 0),
                "After Clear, no compatibilities");
    return TestResult::Passed;
}

TestResult TestTileAdjacency_GetCompatible_Returns_All_Matches() {
    TileAdjacencyTable table;
    const wfc_tile_id tileA{1};
    const wfc_tile_id tileB{2};
    const wfc_tile_id tileC{3};
    // A's +X face accepts B and C
    table.AddCompatibility(tileA, 0, WFCFace::PosX, tileB, 0);
    table.AddCompatibility(tileA, 0, WFCFace::PosX, tileC, 1);

    auto matches = table.GetCompatible(tileA, 0, WFCFace::PosX);
    TEST_ASSERT_EQ(2u, matches.size(), "Should return 2 compatible pairs");

    bool foundB = false, foundC = false;
    for (const auto& m : matches) {
        if (static_cast<u32>(m.tile) == 2 && m.variant == 0) foundB = true;
        if (static_cast<u32>(m.tile) == 3 && m.variant == 1) foundC = true;
    }
    TEST_ASSERT(foundB, "B should be in matches");
    TEST_ASSERT(foundC, "C should be in matches");
    return TestResult::Passed;
}

TestResult TestTileAdjacency_GetCompatible_No_Matches() {
    TileAdjacencyTable table;
    auto matches = table.GetCompatible(wfc_tile_id{42}, 0, WFCFace::PosY);
    TEST_ASSERT_EQ(0u, matches.size(), "Empty table returns empty matches");
    return TestResult::Passed;
}

int main() {
    TestSuite suite("TileAdjacency");
    TEST_CASE(suite, "Add_And_Check_Compatible", TestTileAdjacency_Add_And_Check_Compatible);
    TEST_CASE(suite, "Incompatible_Returns_False", TestTileAdjacency_Incompatible_Returns_False);
    TEST_CASE(suite, "Clear", TestTileAdjacency_Clear);
    TEST_CASE(suite, "GetCompatible_Returns_All_Matches", TestTileAdjacency_GetCompatible_Returns_All_Matches);
    TEST_CASE(suite, "GetCompatible_No_Matches", TestTileAdjacency_GetCompatible_No_Matches);
    suite.RunAllTests();
    return 0;
}
