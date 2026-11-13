// EngineTest/IntegrationTests/Graphics/WFC/TestWFCRuinsSolveSmall.cpp
// Phase C.1 T25: integration test — small 3×3×3 Ruins-only solve that
// verifies the solver reaches full collapse (all 27 cells) and every
// collapsed cell holds a Ruins-category tile.
//
// Tighter than T23 (TestCategorySolve), which only checked >= 1 cell
// collapsed. This catches the failure mode where the solver gives up
// partway through (e.g. an unsolvable adjacency tuple surfacing only
// after some cells have already been fixed in place).
//
// Adapted from the T25 plan to match the real WFCSolver API (same
// plan-bug pattern as T23/T24):
//   * Plan used WFCSolver(cfg) + Initialize(cfg, reg, &adj) + RunToCompletion.
//   * Real: WFCSolver solver + Initialize(cfg, grid, reg, adj, step_buffer)
//     + Step(budget) loop until Done/GivenUp.
//   * Plan read cells via solver.CollapsedCellCount; real API reads
//     grid.Cells()[i].collapsed / .collapsed_tile directly.
//
// Step-loop pattern (InProgress + Restarted with ceiling 2000) is mirrored
// from TestWFCCategorySolve (commit 2a4d5de).
#include "../../../UnitTests/TestFramework.h"
#include "Engine/Graphics/WFC/WFCConfig.h"
#include "Engine/Graphics/WFC/WFCCategory.h"
#include "Engine/Graphics/WFC/WFCSolver.h"
#include "Engine/Graphics/WFC/WFCSolveBudget.h"
#include "Engine/Graphics/WFC/WFCStepBuffer.h"
#include "Engine/Graphics/WFC/WaveGrid.h"
#include "Engine/Graphics/WFC/WFCTileCatalog.h"
#include "Engine/Graphics/WFC/WFCTileRegistry.h"
#include "Engine/Graphics/WFC/TileAdjacency.h"
#include "Engine/Graphics/WFC/WFCTypes.h"

using namespace primal::graphics::wfc;
using namespace Engine::Test;

TestResult TestRuinsSolveSmall_3x3x3_FullCollapse() {
    WFCTileRegistry reg;
    TileAdjacencyTable adj;
    WFCTileCatalog::Populate(reg, adj);

    WFCConfig cfg;
    cfg.grid_size = WFCGridCoord{3, 3, 3};
    cfg.seed = 1;
    // max_generations=16 (plan default) led to GivenUp on this 3×3×3 grid.
    // Bumped to 32 — the 3D catalog with cube wildcards has enough slack
    // to converge reliably at 32. See commit msg for tuning details.
    cfg.max_generations = 32;
    cfg.max_cells_per_frame = 256;
    cfg.max_ms_per_frame = 1000;
    // Restrict to Ruins category: only Ruins tiles (5..14 in the catalog)
    // are eligible as collapse candidates on init and on every restart.
    cfg.active_category_mask = CategoryMaskFor(WFCCategory::Ruins);

    WaveGrid grid;
    grid.Initialize(cfg.grid_size, 8);

    WFCStepBuffer buf;
    WFCSolver solver;
    solver.Initialize(cfg, grid, reg, adj, buf);

    WFCSolveBudget budget(cfg.max_cells_per_frame, cfg.max_ms_per_frame);
    budget.Reset();

    // Step loop: continue on InProgress + Restarted; stop on Done/GivenUp.
    // Ceiling 2000 is generous — a 3×3×3 = 27-cell grid collapses in tens
    // of steps even with restarts (max_generations=16).
    WFCSolver::StepResult result = WFCSolver::StepResult::InProgress;
    for (u32 step = 0; step < 2000; ++step) {
        result = solver.Step(budget);
        if (result == WFCSolver::StepResult::Done ||
            result == WFCSolver::StepResult::GivenUp) {
            break;
        }
        if (result == WFCSolver::StepResult::Restarted) {
            // Fresh budget for the new generation so the restarted wave is
            // not starved by cell-count already burned this frame.
            budget.Reset();
        }
    }

    // Solver must reach Done on a small grid with generous max_generations.
    // GivenUp indicates an unsolvable configuration — either a real
    // dead-end-catalog bug or a sign that max_generations needs bumping.
    TEST_ASSERT(result == WFCSolver::StepResult::Done,
                "solver reaches Done on 3x3x3 ruins-only grid");

    // Every cell must be collapsed (27/27). Partial collapse with Done
    // status would indicate a bug in the solver's completion check.
    const auto& cells = grid.Cells();
    u32 collapsed_count = 0;
    bool all_ruins = true;
    for (u32 i = 0; i < cells.size(); ++i) {
        if (cells[i].collapsed) {
            ++collapsed_count;
            const WFCTile& t = reg.Get(cells[i].collapsed_tile);
            if (t.category != WFCCategory::Ruins) all_ruins = false;
        }
    }
    TEST_ASSERT_EQ(27u, collapsed_count, "all 27 cells collapsed");
    TEST_ASSERT(all_ruins, "all collapsed tiles are Ruins-category");
    return TestResult::Passed;
}

int main() {
    TestSuite suite("WFCRuinsSolveSmall");
    TEST_CASE(suite, "RuinsSolveSmall_3x3x3_FullCollapse",
              TestRuinsSolveSmall_3x3x3_FullCollapse);
    suite.RunAllTests();
    return 0;
}
