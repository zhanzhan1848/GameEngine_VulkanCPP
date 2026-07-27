#include "../../TestFramework.h"
#include "Engine/Graphics/WFC/WFCPropagator.h"
#include "Engine/Graphics/WFC/WaveGrid.h"
#include "Engine/Graphics/WFC/TileAdjacency.h"

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

TestResult TestWFCPropagator_OnCellCollapsed_Queues_Dirty_Neighbors() {
    WaveGrid grid;
    grid.Initialize({3, 1, 1}, 8);
    // All cells start with candidates {0,1,2} (mask 0b111)
    SetCellCandidates(grid, {0, 0, 0}, 0b111u, 3);
    SetCellCandidates(grid, {1, 0, 0}, 0b111u, 3);
    SetCellCandidates(grid, {2, 0, 0}, 0b111u, 3);

    TileAdjacencyTable adjacency;  // empty — propagation will remove nothing
    WFCPropagator prop;
    prop.Initialize(grid);
    prop.OnCellCollapsed(grid, {1, 0, 0}, wfc_tile_id{0}, 0);

    // Should have queued neighbors {0,0,0} and {2,0,0} for processing
    u32 dirty_count = prop.DirtyQueueSize();
    TEST_ASSERT_EQ(2u, dirty_count, "Two neighbors should be dirty");
    return TestResult::Passed;
}

TestResult TestWFCPropagator_Initialize_Clears_State() {
    WaveGrid grid;
    grid.Initialize({2, 1, 1}, 8);
    SetCellCandidates(grid, {0, 0, 0}, 0b11u, 2);
    SetCellCandidates(grid, {1, 0, 0}, 0b11u, 2);

    WFCPropagator prop;
    prop.Initialize(grid);  // should result in empty dirty queue
    TEST_ASSERT_EQ(0u, prop.DirtyQueueSize(), "Dirty queue empty after Initialize");
    return TestResult::Passed;
}

TestResult TestWFCPropagator_No_Neighbors_For_Isolated_Cell() {
    WaveGrid grid;
    grid.Initialize({1, 1, 1}, 8);  // single cell
    SetCellCandidates(grid, {0, 0, 0}, 0b1u, 1);

    WFCPropagator prop;
    prop.Initialize(grid);
    prop.OnCellCollapsed(grid, {0, 0, 0}, wfc_tile_id{0}, 0);
    TEST_ASSERT_EQ(0u, prop.DirtyQueueSize(), "Single cell has no neighbors");
    return TestResult::Passed;
}

int main() {
    TestSuite suite("WFCPropagator");
    TEST_CASE(suite, "OnCellCollapsed_Queues_Dirty_Neighbors", TestWFCPropagator_OnCellCollapsed_Queues_Dirty_Neighbors);
    TEST_CASE(suite, "Initialize_Clears_State", TestWFCPropagator_Initialize_Clears_State);
    TEST_CASE(suite, "No_Neighbors_For_Isolated_Cell", TestWFCPropagator_No_Neighbors_For_Isolated_Cell);
    suite.RunAllTests();
    return 0;
}
