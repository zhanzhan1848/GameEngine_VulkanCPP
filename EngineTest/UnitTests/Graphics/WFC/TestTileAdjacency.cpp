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

int main() {
    TestSuite suite("TileAdjacency");
    TEST_CASE(suite, "Add_And_Check_Compatible", TestTileAdjacency_Add_And_Check_Compatible);
    TEST_CASE(suite, "Incompatible_Returns_False", TestTileAdjacency_Incompatible_Returns_False);
    TEST_CASE(suite, "Clear", TestTileAdjacency_Clear);
    suite.RunAllTests();
    return 0;
}
