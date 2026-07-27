// EngineTest/UnitTests/Graphics/WFC/TestWFCSolver.cpp
//
// Task 7 (Phase A.2): WFCSolver orchestrator tests.
//
// Verifies:
//   * Initialize populates candidate masks across all cells.
//   * Step collapses a single cell in a trivial 1x1x1 grid and returns Done.
//   * Collapse pushes a WFCStepKind::Collapse record into the step buffer.
//
// The trivial fixture registers a single self-compatible tile so propagation
// has nothing to prune and the first Step collapses the only cell.
#include "../../TestFramework.h"
#include "Engine/Graphics/WFC/WFCSolver.h"
#include "Engine/Graphics/WFC/WaveGrid.h"
#include "Engine/Graphics/WFC/TileAdjacency.h"
#include "Engine/Graphics/WFC/WFCTileRegistry.h"
#include "Engine/Graphics/WFC/WFCStepBuffer.h"
#include "Engine/Graphics/WFC/WFCSolveBudget.h"
#include "Engine/Graphics/WFC/WFCConfig.h"

using namespace primal::graphics::wfc;
using namespace Engine::Test;

namespace {
// Set up a trivial 1x1x1 grid with 1 tile that's self-compatible (or no constraints).
WFCSolver MakeTrivialSolver(WFCConfig& config, WaveGrid& grid, WFCTileRegistry& reg,
                            TileAdjacencyTable& adj, WFCStepBuffer& buf) {
    config.grid_size = {1, 1, 1};
    config.max_cells_per_frame = 1;
    config.max_ms_per_frame = 100;
    config.seed = 42;
    config.max_generations = 4;

    WFCTile t{};
    t.name = "trivial";
    t.variant_count = 1;
    t.sockets[0] = 0xFFFFFFFFFFFFFFFFULL;  // wildcard socket
    reg.Register(t);

    WFCSolver solver;
    solver.Initialize(config, grid, reg, adj, buf);
    return solver;
}
}  // namespace

TestResult TestWFCSolver_Initialize_Populates_Candidate_Masks() {
    WFCConfig config;
    WaveGrid grid;
    WFCTileRegistry reg;
    TileAdjacencyTable adj;
    WFCStepBuffer buf;
    auto solver = MakeTrivialSolver(config, grid, reg, adj, buf);

    // After Initialize, the single cell should have candidate_mask=1, candidate_count=1
    const WFCCell& c = grid.CellAt({0, 0, 0});
    TEST_ASSERT_EQ(1u, c.candidate_count, "Cell has 1 candidate (the 1 registered tile variant)");
    TEST_ASSERT_EQ(1ULL, c.candidate_mask, "Candidate mask = bit 0 set");
    TEST_ASSERT(!c.collapsed, "Cell not collapsed yet");
    return TestResult::Passed;
}

TestResult TestWFCSolver_Step_Collapses_Single_Cell() {
    WFCConfig config;
    WaveGrid grid;
    WFCTileRegistry reg;
    TileAdjacencyTable adj;
    WFCStepBuffer buf;
    auto solver = MakeTrivialSolver(config, grid, reg, adj, buf);

    WFCSolveBudget budget(/*max_cells=*/10, /*max_ms=*/100);
    budget.Reset();
    WFCSolver::StepResult result = solver.Step(budget);

    TEST_ASSERT(result == WFCSolver::StepResult::Done, "1x1x1 grid should be Done after 1 collapse");
    TEST_ASSERT(grid.CellAt({0, 0, 0}).collapsed, "Cell is collapsed after Step");
    TEST_ASSERT(!buf.Empty(), "Step buffer has at least 1 step");
    return TestResult::Passed;
}

TestResult TestWFCSolver_Step_Pushes_Collapse_Record() {
    WFCConfig config;
    WaveGrid grid;
    WFCTileRegistry reg;
    TileAdjacencyTable adj;
    WFCStepBuffer buf;
    auto solver = MakeTrivialSolver(config, grid, reg, adj, buf);

    WFCSolveBudget budget(10, 100);
    budget.Reset();
    solver.Step(budget);

    WFCStep steps[8];
    u32 n = buf.Consume(steps, 8);
    TEST_ASSERT(n >= 1u, "At least 1 step recorded");
    bool found_collapse = false;
    for (u32 i = 0; i < n; ++i) {
        if (steps[i].kind == WFCStepKind::Collapse) found_collapse = true;
    }
    TEST_ASSERT(found_collapse, "Collapse step present");
    return TestResult::Passed;
}

int main() {
    TestSuite suite("WFCSolver");
    TEST_CASE(suite, "Initialize_Populates_Candidate_Masks", TestWFCSolver_Initialize_Populates_Candidate_Masks);
    TEST_CASE(suite, "Step_Collapses_Single_Cell", TestWFCSolver_Step_Collapses_Single_Cell);
    TEST_CASE(suite, "Step_Pushes_Collapse_Record", TestWFCSolver_Step_Pushes_Collapse_Record);
    suite.RunAllTests();
    return 0;
}
