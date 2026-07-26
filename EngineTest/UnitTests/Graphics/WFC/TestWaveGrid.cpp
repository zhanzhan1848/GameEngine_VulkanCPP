#include "../../TestFramework.h"
#include "Engine/Graphics/WFC/WaveGrid.h"
#include "Engine/Graphics/WFC/WFCTypes.h"

using namespace primal::graphics::wfc;
using namespace Engine::Test;

TestResult TestWaveGrid_Initialize_And_Size() {
    WaveGrid grid;
    grid.Initialize({4, 4, 4}, 16);
    TEST_ASSERT_EQ(4, grid.Size().x, "Size X");
    TEST_ASSERT_EQ(4, grid.Size().y, "Size Y");
    TEST_ASSERT_EQ(4, grid.Size().z, "Size Z");
    TEST_ASSERT_EQ(64u, grid.CellCount(), "CellCount");
    TEST_ASSERT_EQ(32u, grid.BytesPerCell(), "BytesPerCell (WFCCell size)");
    return TestResult::Passed;
}

TestResult TestWaveGrid_CellAt_Valid_Coord() {
    WaveGrid grid;
    grid.Initialize({2, 2, 2}, 8);
    WFCGridCoord coord{1, 0, 1};
    WFCCell& cell = grid.CellAt(coord);
    cell.collapsed = true;
    TEST_ASSERT(grid.CellAt(coord).collapsed, "CellAt should return mutable reference");
    return TestResult::Passed;
}

TestResult TestWaveGrid_Initialize_Clears_Cells() {
    WaveGrid grid;
    grid.Initialize({2, 2, 2}, 8);
    for (u32 i = 0; i < grid.CellCount(); ++i) {
        const WFCCell& c = grid.Cells()[i];
        TEST_ASSERT(!c.collapsed, "Cells start uncollapsed");
        TEST_ASSERT_EQ(0u, c.candidate_count, "Cells start with 0 candidates");
    }
    return TestResult::Passed;
}

int main() {
    TestSuite suite("WaveGrid");
    TEST_CASE(suite, "Initialize_And_Size", TestWaveGrid_Initialize_And_Size);
    TEST_CASE(suite, "CellAt_Valid_Coord", TestWaveGrid_CellAt_Valid_Coord);
    TEST_CASE(suite, "Initialize_Clears_Cells", TestWaveGrid_Initialize_Clears_Cells);
    suite.RunAllTests();
    return 0;
}
