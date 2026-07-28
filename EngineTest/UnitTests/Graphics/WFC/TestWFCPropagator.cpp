#include "../../TestFramework.h"
#include "Engine/Graphics/WFC/WFCPropagator.h"
#include "Engine/Graphics/WFC/WaveGrid.h"
#include "Engine/Graphics/WFC/TileAdjacency.h"
#include "Engine/Graphics/WFC/WFCTileRegistry.h"

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

TestResult TestWFCPropagator_RunPass_Removes_Incompatible_Candidates() {
    WaveGrid grid;
    grid.Initialize({2, 1, 1}, 8);
    // Cell 0 has candidates {A=bit0, B=bit1, C=bit2}
    SetCellCandidates(grid, {0, 0, 0}, 0b111u, 3);
    SetCellCandidates(grid, {1, 0, 0}, 0b111u, 3);

    // Cell 1 collapses to variant 0 (tile A)
    WFCCell& c1 = grid.CellAt({1, 0, 0});
    c1.candidate_mask = 0b001u;
    c1.candidate_count = 1;
    c1.collapsed = true;
    c1.collapsed_tile = wfc_tile_id{0};
    c1.collapsed_variant = 0;
    c1.entropy = 0;

    // Single-tile registry (Phase A.2 layout): bit b <-> (tile 0, variant b).
    WFCTileRegistry registry;
    WFCTile tile{};
    tile.variant_count = 8;
    registry.Register(tile);

    // Set up adjacency: (tile 0, var 0) -X (tile 0, var 1)
    // (mirror auto-adds: (tile 0, var 1) +X (tile 0, var 0))
    TileAdjacencyTable adjacency;
    const wfc_tile_id A{0};
    adjacency.AddCompatibility(A, 0, WFCFace::NegX, A, 1);

    WFCPropagator prop;
    prop.Initialize(grid);
    prop.OnCellCollapsed(grid, {1, 0, 0}, A, 0);

    bool contradiction = false;
    u32 changed = prop.RunPass(grid, adjacency, registry, contradiction);

    // Cell 0 should have only (tile 0, var 1) -> bit 1 remaining as candidate.
    const WFCCell& result = grid.CellAt({0, 0, 0});
    TEST_ASSERT(!result.collapsed, "Cell 0 not collapsed, just reduced candidates");
    TEST_ASSERT_EQ(0b010u, result.candidate_mask, "Cell 0 should have only bit 1 remaining");
    TEST_ASSERT_EQ(1u, result.candidate_count, "Cell 0 has 1 candidate now");
    TEST_ASSERT(changed >= 1u, "At least one cell changed");
    TEST_ASSERT(!contradiction, "No contradiction expected");
    return TestResult::Passed;
}

TestResult TestWFCPropagator_RunPass_Detects_Contradiction() {
    WaveGrid grid;
    grid.Initialize({2, 1, 1}, 8);
    SetCellCandidates(grid, {0, 0, 0}, 0b001u, 1);  // only A
    SetCellCandidates(grid, {1, 0, 0}, 0b001u, 1);  // only A

    WFCCell& c1 = grid.CellAt({1, 0, 0});
    c1.collapsed = true;
    c1.candidate_mask = 0b001u;
    c1.candidate_count = 1;
    c1.collapsed_tile = wfc_tile_id{0};
    c1.collapsed_variant = 0;
    c1.entropy = 0;

    WFCTileRegistry registry;
    WFCTile tile{};
    tile.variant_count = 8;
    registry.Register(tile);

    // Empty adjacency: A's -X face compatible with NOTHING
    TileAdjacencyTable adjacency;
    WFCPropagator prop;
    prop.Initialize(grid);
    prop.OnCellCollapsed(grid, {1, 0, 0}, wfc_tile_id{0}, 0);

    bool contradiction = false;
    prop.RunPass(grid, adjacency, registry, contradiction);

    // Cell 0 has only candidate A, but A's +X is not compatible with A's -X → all candidates removed
    TEST_ASSERT(contradiction, "Should detect contradiction when cell has no candidates");
    TEST_ASSERT_EQ(0u, grid.CellAt({0, 0, 0}).candidate_count, "Cell 0 reduced to 0 candidates");
    return TestResult::Passed;
}

TestResult TestWFCPropagator_RunPass_No_Change_On_Already_Collapsed() {
    WaveGrid grid;
    grid.Initialize({2, 1, 1}, 8);
    SetCellCandidates(grid, {0, 0, 0}, 0b11u, 2);
    SetCellCandidates(grid, {1, 0, 0}, 0b11u, 2);

    // Mark cell 0 as collapsed (shouldn't be re-modified)
    WFCCell& c0 = grid.CellAt({0, 0, 0});
    c0.collapsed = true;
    c0.candidate_mask = 0b001u;
    c0.candidate_count = 1;
    c0.collapsed_tile = wfc_tile_id{0};
    c0.collapsed_variant = 0;
    c0.entropy = 0;

    WFCTileRegistry registry;
    WFCTile tile{};
    tile.variant_count = 8;
    registry.Register(tile);

    TileAdjacencyTable adjacency;
    WFCPropagator prop;
    prop.Initialize(grid);
    prop.OnCellCollapsed(grid, {1, 0, 0}, wfc_tile_id{1}, 0);
    // Manually queue cell 0 too (simulating it being a neighbor of cell 1)
    // — but it's already collapsed so RunPass should skip it.
    bool contradiction = false;
    u32 changed = prop.RunPass(grid, adjacency, registry, contradiction);
    TEST_ASSERT_EQ(0u, changed, "Already-collapsed cell should not be modified");
    TEST_ASSERT(!contradiction, "No contradiction from collapsed cells");
    return TestResult::Passed;
}

TestResult TestWFCPropagator_RunPass_Multi_Tile_Filter() {
    WaveGrid grid;
    grid.Initialize({2, 1, 1}, 8);

    // Cell 0 has candidates: cube(var 0) at bit 0, ramp(var 0) at bit 8
    WFCCell& c0 = grid.CellAt({0, 0, 0});
    c0.candidate_mask = (1ULL << 0) | (1ULL << 8);
    c0.candidate_count = 2;
    c0.entropy = 2;
    c0.collapsed = false;

    // Cell 1 collapsed to ramp variant 0 (tile=1, variant=0 -> bit 8)
    WFCCell& c1 = grid.CellAt({1, 0, 0});
    c1.candidate_mask = (1ULL << 8);
    c1.candidate_count = 1;
    c1.collapsed = true;
    c1.collapsed_tile = wfc_tile_id{1};  // ramp
    c1.collapsed_variant = 0;
    c1.entropy = 0;

    // Set up registry with cube (1 variant) + ramp (4 variants)
    WFCTileRegistry registry;
    WFCTile cube{};
    cube.variant_count = 1;
    registry.Register(cube);
    WFCTile ramp{};
    ramp.variant_count = 4;
    registry.Register(ramp);

    // Adjacency: cube(NegX) compatible with ramp(PosX) mirror + ramp self-compat
    TileAdjacencyTable adj;
    const wfc_tile_id cube_id{0};
    const wfc_tile_id ramp_id{1};
    adj.AddCompatibility(ramp_id, 0, WFCFace::NegX, cube_id, 0);  // cube at +X accepts ramp at -X via mirror
    adj.AddCompatibility(ramp_id, 0, WFCFace::PosX, ramp_id, 0);  // ramp self-compat +X
    adj.AddCompatibility(ramp_id, 0, WFCFace::PosY, ramp_id, 0);
    adj.AddCompatibility(ramp_id, 0, WFCFace::PosZ, ramp_id, 0);

    WFCPropagator prop;
    prop.Initialize(grid);
    prop.OnCellCollapsed(grid, {1, 0, 0}, ramp_id, 0);

    bool contradiction = false;
    prop.RunPass(grid, adj, registry, contradiction);

    // Cell 0 sits at +X of cell 1 (ramp). Cell 0 candidates {cube, ramp}.
    // Cube(var 0) at +X is compatible with ramp(var 0) at -X (rule added).
    // Ramp(var 0) at +X is compatible with ramp(var 0) at -X (self-compat).
    // -> Both candidates survive -> no contradiction
    TEST_ASSERT(!contradiction, "No contradiction when cube+ramp both compatible with ramp neighbor");
    return TestResult::Passed;
}

int main() {
    TestSuite suite("WFCPropagator");
    TEST_CASE(suite, "OnCellCollapsed_Queues_Dirty_Neighbors", TestWFCPropagator_OnCellCollapsed_Queues_Dirty_Neighbors);
    TEST_CASE(suite, "Initialize_Clears_State", TestWFCPropagator_Initialize_Clears_State);
    TEST_CASE(suite, "No_Neighbors_For_Isolated_Cell", TestWFCPropagator_No_Neighbors_For_Isolated_Cell);
    TEST_CASE(suite, "RunPass_Removes_Incompatible_Candidates", TestWFCPropagator_RunPass_Removes_Incompatible_Candidates);
    TEST_CASE(suite, "RunPass_Detects_Contradiction", TestWFCPropagator_RunPass_Detects_Contradiction);
    TEST_CASE(suite, "RunPass_No_Change_On_Already_Collapsed", TestWFCPropagator_RunPass_No_Change_On_Already_Collapsed);
    TEST_CASE(suite, "RunPass_Multi_Tile_Filter", TestWFCPropagator_RunPass_Multi_Tile_Filter);
    suite.RunAllTests();
    return 0;
}
