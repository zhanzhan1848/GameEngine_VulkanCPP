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

TestResult TestWaveGrid_Resize_Grows() {
    WaveGrid grid;
    grid.Initialize({2, 2, 2}, 8);
    grid.Resize({4, 4, 4});
    TEST_ASSERT_EQ(4, grid.Size().x, "Resize updates X");
    TEST_ASSERT_EQ(4, grid.Size().y, "Resize updates Y");
    TEST_ASSERT_EQ(4, grid.Size().z, "Resize updates Z");
    TEST_ASSERT_EQ(64u, grid.CellCount(), "Resize updates CellCount");
    return TestResult::Passed;
}

TestResult TestWaveGrid_Resize_Shrinks() {
    WaveGrid grid;
    grid.Initialize({4, 4, 4}, 8);
    grid.CellAt({0, 0, 0}).collapsed = true;
    grid.Resize({2, 2, 2});
    TEST_ASSERT_EQ(8u, grid.CellCount(), "Shrunk CellCount");
    TEST_ASSERT(!grid.Cells()[0].collapsed, "Shrink resets cells");
    return TestResult::Passed;
}

TestResult TestWaveGrid_Resize_Preserves_MaxTileVariants() {
    WaveGrid grid;
    grid.Initialize({2, 2, 2}, 16);
    grid.Resize({8, 8, 8});
    TEST_ASSERT_EQ(16u, grid.MaxTileVariants(), "Resize preserves max tile variants");
    return TestResult::Passed;
}

TestResult TestWaveGrid_Reset_Clears_Collapse_But_Keeps_Size() {
    WaveGrid grid;
    grid.Initialize({3, 3, 3}, 8);
    grid.CellAt({1, 1, 1}).collapsed = true;
    grid.CellAt({1, 1, 1}).candidate_count = 3;
    grid.Reset();
    TEST_ASSERT_EQ(27u, grid.CellCount(), "Reset preserves CellCount");
    for (const auto& c : grid.Cells()) {
        TEST_ASSERT(!c.collapsed, "Reset clears collapsed flag");
        TEST_ASSERT_EQ(0u, c.candidate_count, "Reset clears candidate_count");
    }
    return TestResult::Passed;
}

int main() {
    TestSuite suite("WaveGrid");
    TEST_CASE(suite, "Initialize_And_Size", TestWaveGrid_Initialize_And_Size);
    TEST_CASE(suite, "CellAt_Valid_Coord", TestWaveGrid_CellAt_Valid_Coord);
    TEST_CASE(suite, "Initialize_Clears_Cells", TestWaveGrid_Initialize_Clears_Cells);
    TEST_CASE(suite, "Resize_Grows", TestWaveGrid_Resize_Grows);
    TEST_CASE(suite, "Resize_Shrinks", TestWaveGrid_Resize_Shrinks);
    TEST_CASE(suite, "Resize_Preserves_MaxTileVariants", TestWaveGrid_Resize_Preserves_MaxTileVariants);
    TEST_CASE(suite, "Reset_Clears_Collapse_But_Keeps_Size", TestWaveGrid_Reset_Clears_Collapse_But_Keeps_Size);
    suite.RunAllTests();
    return 0;
}
