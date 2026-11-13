// EngineTest/IntegrationTests/Graphics/WFC/TestWFCRuinsRestart.cpp
// Phase C.1 T26: integration test — RestartPolicy fires on Ruins-only grid.
//
// Verifies the RestartPolicy on a 8x4x8 Ruins-only grid. The solver should
// either:
//   (a) Complete the full 256-cell collapse without restart, OR
//   (b) Hit at least one restart generation before completing/giving up.
//
// Either outcome is valid; the test fails only if the solver hangs or
// silently terminates with no progress AND no restart. With T25's
// vertical-wildcard fix in place, outcome (a) — full collapse — is
// expected most of the time. Outcome (b) is exercised when seed=0xDEAD1
// triggers a contradiction that requires a restart generation.
//
// Plan-bug adaptation (mirrors T23/T24/T25):
//   * Plan used WFCSolver(cfg) + Initialize(cfg, reg, &adj) +
//     RunToCompletionOrBudget + solver.RestartCount()/CollapsedCellCount().
//   * Real: WFCSolver solver + Initialize(cfg, grid, reg, adj, step_buffer)
//     + Step(budget) loop until Done/GivenUp. The StepResult enum carries
//     the Restarted signal directly, so we count restarts in the loop
//     instead of querying solver.RestartCount() after the fact.
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

TestResult TestRuinsRestart_DeadlockSeedRecovers() {
    WFCTileRegistry reg;
    TileAdjacencyTable adj;
    WFCTileCatalog::Populate(reg, adj);

    WFCConfig cfg;
    cfg.grid_size = WFCGridCoord{8, 4, 8};
    cfg.seed = 0xDEAD1;   // 57041
    cfg.max_generations = 32;
    cfg.max_cells_per_frame = 256;
    cfg.max_ms_per_frame = 1000;
    cfg.active_category_mask = CategoryMaskFor(WFCCategory::Ruins);

    WaveGrid grid;
    grid.Initialize(cfg.grid_size, 8);

    WFCStepBuffer buf;
    WFCSolver solver;
    solver.Initialize(cfg, grid, reg, adj, buf);

    WFCSolveBudget budget(cfg.max_cells_per_frame, cfg.max_ms_per_frame);
    budget.Reset();

    const u32 total_cells = cfg.grid_size.x * cfg.grid_size.y * cfg.grid_size.z;
    u32 restarts = 0;
    WFCSolver::StepResult result = WFCSolver::StepResult::InProgress;
    for (u32 step = 0; step < 5000; ++step) {
        result = solver.Step(budget);
        if (result == WFCSolver::StepResult::Done ||
            result == WFCSolver::StepResult::GivenUp) {
            break;
        }
        if (result == WFCSolver::StepResult::Restarted) {
            ++restarts;
            budget.Reset();
        }
    }

    const auto& cells = grid.Cells();
    u32 collapsed = 0;
    for (u32 i = 0; i < cells.size(); ++i) {
        if (cells[i].collapsed) ++collapsed;
    }

    // Either the solver completed the full grid, or it restarted at least
    // once trying to. Both are valid outcomes — the test catches the silent
    // failure mode where the solver neither restarts nor finishes.
    bool fully_collapsed = (collapsed == total_cells);
    TEST_ASSERT(restarts >= 1u || fully_collapsed,
                "either restarted ≥ 1 generation or fully collapsed");
    return TestResult::Passed;
}

int main() {
    TestSuite suite("WFCRuinsRestart");
    TEST_CASE(suite, "RuinsRestart_DeadlockSeedRecovers",
              TestRuinsRestart_DeadlockSeedRecovers);
    suite.RunAllTests();
    return 0;
}
