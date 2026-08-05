#include "../../TestFramework.h"
#include "Engine/Graphics/WFC/WFCMinEntropyObserver.h"
#include "Engine/Graphics/WFC/WaveGrid.h"

using namespace primal::graphics::wfc;
using namespace Engine::Test;

namespace {
void SetCellCandidates(WaveGrid& grid, WFCGridCoord c, u64 mask, u32 count) {
    WFCCell& cell = grid.CellAt(c);
    cell.candidate_mask = mask;
    cell.candidate_count = count;
    cell.entropy = static_cast<u8>(count);
    cell.collapsed = false;
}
}

TestResult TestWFCObserver_Picks_Minimum_Entropy() {
    WaveGrid grid;
    grid.Initialize({3, 1, 1}, 8);
    // Three cells: entropy 3, 1, 2
    SetCellCandidates(grid, {0, 0, 0}, 0b00001111u, 3);
    SetCellCandidates(grid, {1, 0, 0}, 0b00000011u, 1);
    SetCellCandidates(grid, {2, 0, 0}, 0b00000111u, 2);

    WFCMinEntropyObserver observer;
    observer.Initialize(grid);
    WFCGridCoord picked = observer.PickNextCollapse(grid);
    TEST_ASSERT_EQ(1, picked.x, "Should pick cell with entropy 1 (lowest)");
    return TestResult::Passed;
}

TestResult TestWFCObserver_Picks_Invalid_When_All_Collapsed() {
    WaveGrid grid;
    grid.Initialize({2, 1, 1}, 8);
    // Both cells collapsed
    WFCCell& a = grid.CellAt({0, 0, 0});
    WFCCell& b = grid.CellAt({1, 0, 0});
    a.collapsed = true; a.entropy = 0; a.candidate_count = 0;
    b.collapsed = true; b.entropy = 0; b.candidate_count = 0;

    WFCMinEntropyObserver observer;
    observer.Initialize(grid);
    WFCGridCoord picked = observer.PickNextCollapse(grid);
    // Invalid coord: any negative value or out-of-range
    TEST_ASSERT(picked.x < 0 || picked.y < 0 || picked.z < 0,
                "Should return invalid coord when grid is fully collapsed");
    return TestResult::Passed;
}

TestResult TestWFCObserver_OnCellChanged_Updates_Heap() {
    WaveGrid grid;
    grid.Initialize({3, 1, 1}, 8);
    SetCellCandidates(grid, {0, 0, 0}, 0b00001111u, 3);
    SetCellCandidates(grid, {1, 0, 0}, 0b00000011u, 1);
    SetCellCandidates(grid, {2, 0, 0}, 0b00000111u, 2);

    WFCMinEntropyObserver observer;
    observer.Initialize(grid);
    // Now lower cell 2's entropy to 0 (collapsed)
    SetCellCandidates(grid, {2, 0, 0}, 0b00000001u, 1);
    observer.OnCellChanged({2, 0, 0});

    // The original min (cell 1, entropy 1) and cell 2 (entropy 1) tie.
    // We can't predict which, but both should have entropy 1 —
    // verify neither cell 0 (entropy 3) nor any unexpected cell is picked.
    WFCGridCoord picked = observer.PickNextCollapse(grid);
    TEST_ASSERT(picked.x == 1 || picked.x == 2, "Should pick from entropy-1 cells");
    return TestResult::Passed;
}

int main() {
    TestSuite suite("WFCMinEntropyObserver");
    TEST_CASE(suite, "Picks_Minimum_Entropy", TestWFCObserver_Picks_Minimum_Entropy);
    TEST_CASE(suite, "Picks_Invalid_When_All_Collapsed", TestWFCObserver_Picks_Invalid_When_All_Collapsed);
    TEST_CASE(suite, "OnCellChanged_Updates_Heap", TestWFCObserver_OnCellChanged_Updates_Heap);
    suite.RunAllTests();
    return 0;
}
