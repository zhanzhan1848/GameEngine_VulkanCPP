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

    // Single-tile registry (16×4 packing): bit b <-> (tile 0, variant b) for b in [0,4).
    // variant_count caps at MaxVariantsPerTile=4 (asserted in Register); these tests
    // only exercise variants 0-2, so capping at 4 preserves semantics.
    WFCTileRegistry registry;
    WFCTile tile{};
    tile.variant_count = 4;
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
    u32 changed = prop.RunPass(grid, adjacency, registry, WFC_FACE_COUNT_3D, contradiction);

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
    tile.variant_count = 4;
    registry.Register(tile);

    // Empty adjacency: A's -X face compatible with NOTHING
    TileAdjacencyTable adjacency;
    WFCPropagator prop;
    prop.Initialize(grid);
    prop.OnCellCollapsed(grid, {1, 0, 0}, wfc_tile_id{0}, 0);

    bool contradiction = false;
    prop.RunPass(grid, adjacency, registry, WFC_FACE_COUNT_3D, contradiction);

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
    tile.variant_count = 4;
    registry.Register(tile);

    TileAdjacencyTable adjacency;
    WFCPropagator prop;
    prop.Initialize(grid);
    prop.OnCellCollapsed(grid, {1, 0, 0}, wfc_tile_id{1}, 0);
    // Manually queue cell 0 too (simulating it being a neighbor of cell 1)
    // — but it's already collapsed so RunPass should skip it.
    bool contradiction = false;
    u32 changed = prop.RunPass(grid, adjacency, registry, WFC_FACE_COUNT_3D, contradiction);
    TEST_ASSERT_EQ(0u, changed, "Already-collapsed cell should not be modified");
    TEST_ASSERT(!contradiction, "No contradiction from collapsed cells");
    return TestResult::Passed;
}

TestResult TestWFCPropagator_RunPass_Multi_Tile_Filter() {
    WaveGrid grid;
    grid.Initialize({2, 1, 1}, 8);

    // Bit positions track the registry's packing constants via BitForTileVariant,
    // so this test stays correct if MaxVariantsPerTile changes. Under the current
    // 16×4 layout: cube(t0,v0)=bit 0, ramp(t1,v0)=bit 4.
    const wfc_tile_id cube_id{0};
    const wfc_tile_id ramp_id{1};
    const u32 cube_bit = WFCTileRegistry::BitForTileVariant(cube_id, 0);
    const u32 ramp_bit = WFCTileRegistry::BitForTileVariant(ramp_id, 0);

    // Cell 0 has candidates: cube(var 0) and ramp(var 0).
    WFCCell& c0 = grid.CellAt({0, 0, 0});
    c0.candidate_mask = (1ULL << cube_bit) | (1ULL << ramp_bit);
    c0.candidate_count = 2;
    c0.entropy = 2;
    c0.collapsed = false;

    // Cell 1 collapsed to ramp variant 0.
    WFCCell& c1 = grid.CellAt({1, 0, 0});
    c1.candidate_mask = (1ULL << ramp_bit);
    c1.candidate_count = 1;
    c1.collapsed = true;
    c1.collapsed_tile = ramp_id;
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
    adj.AddCompatibility(ramp_id, 0, WFCFace::NegX, cube_id, 0);  // cube at +X accepts ramp at -X via mirror
    adj.AddCompatibility(ramp_id, 0, WFCFace::PosX, ramp_id, 0);  // ramp self-compat +X
    adj.AddCompatibility(ramp_id, 0, WFCFace::PosY, ramp_id, 0);
    adj.AddCompatibility(ramp_id, 0, WFCFace::PosZ, ramp_id, 0);

    WFCPropagator prop;
    prop.Initialize(grid);
    prop.OnCellCollapsed(grid, {1, 0, 0}, ramp_id, 0);

    bool contradiction = false;
    prop.RunPass(grid, adj, registry, WFC_FACE_COUNT_3D, contradiction);

    // Cell 0 sits at -X of cell 1 (ramp). For cell 0's PosX face (pointing at
    // cell 1's NegX), each surviving candidate must be compatible with ramp(v0).
    // Cube(var 0) at PosX is compatible with ramp(var 0) at NegX (rule added).
    // Ramp(var 0) at PosX is compatible with ramp(var 0) at NegX (self-compat).
    // -> Both candidates survive -> no contradiction, mask unchanged.
    TEST_ASSERT(!contradiction, "No contradiction when cube+ramp both compatible with ramp neighbor");
    const WFCCell& result = grid.CellAt({0, 0, 0});
    const u64 expected_mask = (1ULL << cube_bit) | (1ULL << ramp_bit);
    TEST_ASSERT_EQ(expected_mask, result.candidate_mask,
                   "Both cube and ramp candidates must survive the multi-tile filter");
    TEST_ASSERT_EQ(2u, result.candidate_count,
                   "Cell 0 retains exactly 2 candidates after propagation");
    return TestResult::Passed;
}

TestResult TestWFCPropagator_RunPass_FaceCount2D_Still_Processes_XY_Neighbors() {
    // 3x1x1 grid. Cell at (1,0,0) collapses. In 2D mode (face_count=4), the
    // ±X neighbors at (0,0,0) and (2,0,0) are still processed because kFaces
    // indices 0,1 are PosX/NegX. This test verifies the 2D face_count doesn't
    // accidentally skip X-axis filtering.
    WaveGrid grid;
    grid.Initialize({3, 1, 1}, 8);
    SetCellCandidates(grid, {0, 0, 0}, 0b111u, 3);
    SetCellCandidates(grid, {1, 0, 0}, 0b111u, 3);
    SetCellCandidates(grid, {2, 0, 0}, 0b111u, 3);

    WFCCell& c1 = grid.CellAt({1, 0, 0});
    c1.candidate_mask = 0b001u;
    c1.candidate_count = 1;
    c1.collapsed = true;
    c1.collapsed_tile = wfc_tile_id{0};
    c1.collapsed_variant = 0;
    c1.entropy = 0;

    WFCTileRegistry registry;
    WFCTile tile{};
    tile.variant_count = 4;
    registry.Register(tile);

    TileAdjacencyTable adjacency;
    const wfc_tile_id A{0};
    // Symmetric ±X rules so both -X and +X neighbors of c1 reduce to bit 1.
    // (NegX rule alone would leave c1's +X face with no compatible variant,
    // emptying cell {2,0,0} and triggering a contradiction unrelated to 2D.)
    adjacency.AddCompatibility(A, 0, WFCFace::NegX, A, 1);
    adjacency.AddCompatibility(A, 0, WFCFace::PosX, A, 1);

    WFCPropagator prop;
    prop.Initialize(grid);
    prop.OnCellCollapsed(grid, {1, 0, 0}, A, 0);

    bool contradiction = false;
    u32 changed = prop.RunPass(grid, adjacency, registry,
                               WFC_FACE_COUNT_2D, contradiction);

    // Cell {0,0,0} is at -X of c1. Its filter uses my_face=+X, neighbor_face=-X.
    // Cell {2,0,0} is at +X of c1. Its filter uses my_face=-X, neighbor_face=+X.
    // 2D mode face_count=4 still includes ±X faces (indices 0,1 in kFaces).
    // With symmetric ±X rules, both X neighbors reduce to bit 1 only — no
    // contradiction — proving the 2D loop bound does not skip X-axis filtering.
    const WFCCell& c0 = grid.CellAt({0, 0, 0});
    TEST_ASSERT(!c0.collapsed, "Cell 0 not collapsed in 2D mode");
    TEST_ASSERT_EQ(0b010u, c0.candidate_mask, "Cell 0 has only bit 1 in 2D mode");
    TEST_ASSERT(changed >= 1u, "2D mode still processes X/Y neighbors");
    TEST_ASSERT(!contradiction, "No contradiction in 2D mode for X-axis filter");
    return TestResult::Passed;
}

TestResult TestWFCPropagator_RunPass_FaceCount2D_Ignores_Z_Compat() {
    // Grid {1,1,3} (3 cells along Z). Collapse cell at z=1. In 3D mode, the
    // ±Z neighbors (z=0, z=2) get filtered by adjacency on ±Z faces. In 2D
    // mode face_count=4, those entries (indices 4,5 in kFaces) are skipped,
    // so neighbors at ±Z are NOT filtered even if their compatibility is
    // empty.
    WaveGrid grid;
    grid.Initialize({1, 1, 3}, 8);
    SetCellCandidates(grid, {0, 0, 0}, 0b001u, 1);
    SetCellCandidates(grid, {0, 0, 1}, 0b001u, 1);
    SetCellCandidates(grid, {0, 0, 2}, 0b001u, 1);

    WFCCell& c1 = grid.CellAt({0, 0, 1});
    c1.collapsed = true;
    c1.candidate_mask = 0b001u;
    c1.candidate_count = 1;
    c1.collapsed_tile = wfc_tile_id{0};
    c1.collapsed_variant = 0;
    c1.entropy = 0;

    WFCTileRegistry registry;
    WFCTile tile{};
    tile.variant_count = 4;
    registry.Register(tile);

    // Empty adjacency: A's ±Z faces compatible with NOTHING.
    TileAdjacencyTable adjacency;
    WFCPropagator prop;
    prop.Initialize(grid);
    prop.OnCellCollapsed(grid, {0, 0, 1}, wfc_tile_id{0}, 0);

    bool contradiction = false;
    prop.RunPass(grid, adjacency, registry,
                 WFC_FACE_COUNT_2D, contradiction);

    // In 2D mode, ±Z entries skipped → cell {0,0,0} and {0,0,2} keep their
    // candidates unchanged despite the empty adjacency on ±Z faces.
    TEST_ASSERT(!contradiction, "2D mode ignores ±Z incompatibility");
    TEST_ASSERT_EQ(1u, grid.CellAt({0, 0, 0}).candidate_count,
                   "Cell z=0 unchanged in 2D mode");
    TEST_ASSERT_EQ(1u, grid.CellAt({0, 0, 2}).candidate_count,
                   "Cell z=2 unchanged in 2D mode");
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
    TEST_CASE(suite, "RunPass_FaceCount2D_Still_Processes_XY_Neighbors",
              TestWFCPropagator_RunPass_FaceCount2D_Still_Processes_XY_Neighbors);
    TEST_CASE(suite, "RunPass_FaceCount2D_Ignores_Z_Compat",
              TestWFCPropagator_RunPass_FaceCount2D_Ignores_Z_Compat);
    suite.RunAllTests();
    return 0;
}
