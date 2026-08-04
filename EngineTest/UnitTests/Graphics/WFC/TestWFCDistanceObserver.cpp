#include "../../TestFramework.h"
#include "Engine/Graphics/WFC/WFCDistanceObserver.h"
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

// 3x1x3 grid, all cells uncollapsed, origin = (1,0,1).
// PickNextCollapse should return the origin first, then the 4-neighbor ring
// at distance 1, then the 4 corners at distance sqrt(2).
TestResult TestWFCDistanceObserver_PicksClosestFirst() {
    WaveGrid grid;
    grid.Initialize({3, 1, 3}, 8);
    for (s32 x = 0; x < 3; ++x)
        for (s32 z = 0; z < 3; ++z)
            SetCellCandidates(grid, {x, 0, z}, 0b00001111u, 3);

    WFCDistanceObserver observer({1, 0, 1});
    observer.Initialize(grid);

    WFCGridCoord first = observer.PickNextCollapse(grid);
    TEST_ASSERT_EQ(1, first.x, "First pick X should be origin X");
    TEST_ASSERT_EQ(0, first.y, "First pick Y should be origin Y");
    TEST_ASSERT_EQ(1, first.z, "First pick Z should be origin Z");

    // Collapse origin in the grid, then verify next pick is one of the
    // 4 neighbors at Manhattan distance 1 (Euclidean distance also 1).
    WFCCell& origin = grid.CellAt({1, 0, 1});
    origin.collapsed = true;
    origin.entropy = 0;
    origin.candidate_count = 0;
    observer.OnCellChanged({1, 0, 1});  // distance observer: no-op but must not crash

    WFCGridCoord second = observer.PickNextCollapse(grid);
    const int dx = abs(second.x - 1);
    const int dz = abs(second.z - 1);
    TEST_ASSERT(dx + dz == 1, "Second pick should be a 4-neighbor of origin");

    return TestResult::Passed;
}

// Tie-break: among equidistant cells, row-major (X varies fastest, then Z)
// order wins. In the 3x1x3 grid above, after origin and 4 neighbors are
// collapsed, the 4 corners are all at distance sqrt(2). Row-major picks
// (0,0,0) first.
TestResult TestWFCDistanceObserver_TieBreakRowMajor() {
    WaveGrid grid;
    grid.Initialize({3, 1, 3}, 8);
    for (s32 x = 0; x < 3; ++x)
        for (s32 z = 0; z < 3; ++z)
            SetCellCandidates(grid, {x, 0, z}, 0b00001111u, 3);

    WFCDistanceObserver observer({1, 0, 1});
    observer.Initialize(grid);

    // Collapse origin + 4 neighbors.
    auto collapse = [&](WFCGridCoord c) {
        WFCCell& cell = grid.CellAt(c);
        cell.collapsed = true;
        cell.entropy = 0;
        cell.candidate_count = 0;
    };
    collapse({1, 0, 1});
    collapse({0, 0, 1});
    collapse({2, 0, 1});
    collapse({1, 0, 0});
    collapse({1, 0, 2});

    WFCGridCoord corner = observer.PickNextCollapse(grid);
    TEST_ASSERT_EQ(0, corner.x, "Tie-break should pick X=0 first (row-major)");
    TEST_ASSERT_EQ(0, corner.z, "Tie-break should pick Z=0 first (row-major)");

    return TestResult::Passed;
}

// All cells collapsed -> returns {-1,-1,-1} and Empty()==true.
TestResult TestWFCDistanceObserver_AllCollapsed() {
    WaveGrid grid;
    grid.Initialize({2, 1, 2}, 8);
    for (s32 x = 0; x < 2; ++x)
        for (s32 z = 0; z < 2; ++z) {
            WFCCell& c = grid.CellAt({x, 0, z});
            c.collapsed = true;
            c.entropy = 0;
            c.candidate_count = 0;
        }

    WFCDistanceObserver observer({0, 0, 0});
    observer.Initialize(grid);
    WFCGridCoord picked = observer.PickNextCollapse(grid);
    TEST_ASSERT(picked.x < 0 || picked.y < 0 || picked.z < 0,
                "Should return invalid coord when all collapsed");
    TEST_ASSERT(observer.Empty(), "Empty() should be true after no candidate found");
    return TestResult::Passed;
}

int main() {
    TestSuite suite("WFCDistanceObserver");
    TEST_CASE(suite, "PicksClosestFirst", TestWFCDistanceObserver_PicksClosestFirst);
    TEST_CASE(suite, "TieBreakRowMajor", TestWFCDistanceObserver_TieBreakRowMajor);
    TEST_CASE(suite, "AllCollapsed", TestWFCDistanceObserver_AllCollapsed);
    suite.RunAllTests();
    return 0;
}
