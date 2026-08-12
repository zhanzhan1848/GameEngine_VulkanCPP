// EngineTest/IntegrationTests/Graphics/WFC/TestWFCRuinsMixedCategories.cpp
// Phase C.1 T26: integration test — Primitive+Ruins mixed-category solve.
//
// With cube wildcard in place (T24) + ruins vertical wildcard (T25), a
// mixed Primitive+Ruins grid should collapse fully and land on a tile mix
// where neither category dominates outright. The cube is the universal
// connector, but there are 10 ruins tiles vs 5 primitives, so ruins
// should plausibly outnumber primitives on a free collapse — yet the
// cube's wildcard bias (it stacks with every tile on every face) keeps
// primitives from being washed out.
//
// Asserts primitive share in [10%, 50%]. Loose bounds — the goal is to
// catch catastrophic regressions (cube dominating, or primitives wiped
// out by adjacency bugs), not to lock in a specific distribution.
//
// Plan-bug adaptation (mirrors T23/T24/T25):
//   * Plan used WFCSolver(cfg) + Initialize(cfg, reg, &adj) +
//     RunToCompletionOrBudget + solver.GridCellCount()/CellCollapsedAt().
//   * Real: WFCSolver solver + Initialize(cfg, grid, reg, adj, step_buffer)
//     + Step(budget) loop until Done/GivenUp + grid.Cells()[i].collapsed.
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

TestResult TestMixedCategories_PrimitiveAndRuins_ReasonableShare() {
    WFCTileRegistry reg;
    TileAdjacencyTable adj;
    WFCTileCatalog::Populate(reg, adj);

    WFCConfig cfg;
    cfg.grid_size = WFCGridCoord{6, 3, 6};
    cfg.seed = 42;
    cfg.max_generations = 32;
    cfg.max_cells_per_frame = 256;
    cfg.max_ms_per_frame = 1000;
    cfg.active_category_mask =
        CategoryMaskFor(WFCCategory::Primitive) | CategoryMaskFor(WFCCategory::Ruins);

    WaveGrid grid;
    grid.Initialize(cfg.grid_size, 8);

    WFCStepBuffer buf;
    WFCSolver solver;
    solver.Initialize(cfg, grid, reg, adj, buf);

    WFCSolveBudget budget(cfg.max_cells_per_frame, cfg.max_ms_per_frame);
    budget.Reset();

    WFCSolver::StepResult result = WFCSolver::StepResult::InProgress;
    for (u32 step = 0; step < 2000; ++step) {
        result = solver.Step(budget);
        if (result == WFCSolver::StepResult::Done ||
            result == WFCSolver::StepResult::GivenUp) {
            break;
        }
        if (result == WFCSolver::StepResult::Restarted) {
            budget.Reset();
        }
    }

    TEST_ASSERT(result == WFCSolver::StepResult::Done ||
                result == WFCSolver::StepResult::GivenUp,
                "solver terminates on mixed-category grid");

    const u32 total_cells = cfg.grid_size.x * cfg.grid_size.y * cfg.grid_size.z;
    const auto& cells = grid.Cells();
    u32 prim_count = 0, collapsed = 0;
    for (u32 i = 0; i < cells.size(); ++i) {
        if (!cells[i].collapsed) continue;
        ++collapsed;
        const WFCTile& t = reg.Get(cells[i].collapsed_tile);
        if (t.category == WFCCategory::Primitive) ++prim_count;
    }

    TEST_ASSERT(collapsed > 0, "at least one cell collapsed");
    TEST_ASSERT(collapsed <= total_cells, "no more than total cells");

    f32 prim_ratio = static_cast<f32>(prim_count) / static_cast<f32>(collapsed);
    TEST_ASSERT(prim_ratio >= 0.10f && prim_ratio <= 0.50f,
                "primitive share in [10%, 50%]");
    return TestResult::Passed;
}

int main() {
    TestSuite suite("WFCRuinsMixedCategories");
    TEST_CASE(suite, "MixedCategories_PrimitiveAndRuins_ReasonableShare",
              TestMixedCategories_PrimitiveAndRuins_ReasonableShare);
    suite.RunAllTests();
    return 0;
}
