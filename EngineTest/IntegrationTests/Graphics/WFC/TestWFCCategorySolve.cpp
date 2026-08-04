// EngineTest/IntegrationTests/Graphics/WFC/TestWFCCategorySolve.cpp
// Phase C.1 T23: integration test — solves with active_category_mask=Ruins
// and verifies every collapsed cell holds a Ruins-category tile.
//
// This proves the Phase C.1 T5 PopulateAllCandidates category-mask plumbing
// (cached active_category_mask_ in WFCSolver, applied in PopulateAllCandidates
// on init + every restart) actually flows through end-to-end: not just that
// the candidates are filtered, but that the resulting wave actually collapses
// to a Ruins-only grid.
//
// Adapted from the T23 plan to match the real WFCSolver API:
//   * Plan used WFCSolver(cfg) + Initialize(cfg, reg, &adj) + RunToCompletion.
//   * Real: WFCSolver solver + Initialize(cfg, grid, reg, adj, step_buffer)
//     + Step(budget) loop until Done/GivenUp.
//   * Plan read cells via solver.CellCollapsedAt/TileAt; real API reads
//     grid.Cells()[i].collapsed / .collapsed_tile directly.
//
// Step-loop pattern (InProgress + Restarted with ceiling 2000) is mirrored
// from TestWFCTileCatalog_Solves_4x4x4_With_Catalog (commit e2d5a0a).
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

TestResult TestCategorySolve_RuinsOnly_AllCollapsedTilesAreRuins() {
    WFCTileRegistry reg;
    TileAdjacencyTable adj;
    WFCTileCatalog::Populate(reg, adj);

    // Sanity: catalog must contain at least one Ruins tile for the mask to
    // have anything to collapse to. Catalog layout (T22): tiles 5..14 are
    // Ruins (10 of 15 total).
    TEST_ASSERT(reg.Count() >= 15u, "catalog populated with >= 15 tiles");

    WFCConfig cfg;
    cfg.grid_size = WFCGridCoord{4, 2, 4};
    cfg.seed = 1;
    cfg.max_generations = 8;
    cfg.max_cells_per_frame = 256;
    cfg.max_ms_per_frame = 1000;
    // Phase C.1 T5 hook: active_category_mask filters which tiles enter the
    // initial candidate set in PopulateAllCandidates. Restricting to Ruins
    // means only tiles 5..14 (10 tiles × 4 variants = 40 candidate bits)
    // are eligible.
    cfg.active_category_mask = CategoryMaskFor(WFCCategory::Ruins);
    TEST_ASSERT(cfg.active_category_mask != 0,
                "Ruins category mask is non-zero");

    WaveGrid grid;
    grid.Initialize(cfg.grid_size, 8);

    WFCStepBuffer buf;
    WFCSolver solver;
    solver.Initialize(cfg, grid, reg, adj, buf);

    WFCSolveBudget budget(cfg.max_cells_per_frame, cfg.max_ms_per_frame);
    budget.Reset();

    // Step loop: continue on InProgress + Restarted; stop on Done/GivenUp.
    // Ceiling 2000 is generous — a 4×2×4 = 32-cell grid collapses in tens
    // of steps even with restarts (max_generations=8).
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

    // Solver must have terminated (not still InProgress at step ceiling).
    TEST_ASSERT(result == WFCSolver::StepResult::Done ||
                result == WFCSolver::StepResult::GivenUp,
                "solver terminates within 2000 steps");

    // Scan every cell; every collapsed cell's tile must be Ruins-category.
    // This is the core assertion: even if the solver gives up partway, the
    // cells it DID collapse must respect the category mask. A non-Ruins tile
    // in the collapsed set would indicate the mask filter leaked (e.g.
    // PopulateAllCandidates used the wrong field, or restart re-init forgot
    // to re-apply the mask).
    const auto& cells = grid.Cells();
    u32 collapsed_count = 0;
    bool all_ruins = true;
    for (u32 i = 0; i < cells.size(); ++i) {
        if (!cells[i].collapsed) continue;
        ++collapsed_count;
        wfc_tile_id tid = cells[i].collapsed_tile;
        const WFCTile& t = reg.Get(tid);
        if (t.category != WFCCategory::Ruins) {
            all_ruins = false;
        }
    }
    TEST_ASSERT(collapsed_count > 0, "at least one cell collapsed");
    TEST_ASSERT(all_ruins,
                "all collapsed tiles are Ruins-category");
    return TestResult::Passed;
}

int main() {
    TestSuite suite("WFCCategorySolve");
    TEST_CASE(suite, "RuinsOnly_AllCollapsedTilesAreRuins",
              TestCategorySolve_RuinsOnly_AllCollapsedTilesAreRuins);
    suite.RunAllTests();
    return 0;
}
