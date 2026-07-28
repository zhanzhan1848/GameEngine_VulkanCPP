// EngineTest/IntegrationTests/Graphics/WFC/TestWFC3DParametric.cpp
//
// Task 11 (Phase A.3): End-to-end integration test for the parametric WFC
// pipeline. Exercises the full chain that previous tasks landed piecewise:
//
//   WFCTileCatalog::Populate  (Task 6/7)  -> registry + adjacency
//   WFCSolver                 (Task 3-5)  -> multi-tile collapse + propagate
//   WFCOutput::ConsumeSteps   (Task 10)   -> drain step buffer into PCGPointSet
//
// The test runs a 4x4x4 solve to completion (or GivenUp), then verifies the
// emitted PCGPointSet has plausible cell count and that every instance's
// MeshIndex attr lands inside the catalog's placeholder range (1000-1004).
//
// This is the first WFC test that lives under IntegrationTests/ rather than
// UnitTests/, because it stitches multiple subsystems together and serves as
// the canonical "Phase A.3 works end to end" checkpoint.
#include "../../../UnitTests/TestFramework.h"
#include "Engine/Graphics/WFC/WFCTileCatalog.h"
#include "Engine/Graphics/WFC/WFCTileRegistry.h"
#include "Engine/Graphics/WFC/TileAdjacency.h"
#include "Engine/Graphics/WFC/WaveGrid.h"
#include "Engine/Graphics/WFC/WFCSolver.h"
#include "Engine/Graphics/WFC/WFCStepBuffer.h"
#include "Engine/Graphics/WFC/WFCConfig.h"
#include "Engine/Graphics/WFC/WFCSolveBudget.h"
#include "Engine/Graphics/WFC/WFCOutput.h"
#include "Engine/Graphics/PCG/PCGTypes.h"

using namespace primal::graphics::wfc;
using namespace primal::graphics::pcg;
using namespace Engine::Test;

TestResult TestWFC3DParametric_Full_Pipeline_4x4x4() {
    // 1. Build catalog -> registry + adjacency
    WFCTileRegistry reg;
    TileAdjacencyTable adj;
    WFCTileCatalog::Populate(reg, adj);

    // 2. Configure solver
    WFCConfig config;
    config.grid_size = {4, 4, 4};
    config.max_cells_per_frame = 256;
    config.max_ms_per_frame = 1000;
    config.seed = 7;
    config.max_generations = 8;

    WaveGrid grid;
    grid.Initialize(config.grid_size, 8);

    WFCStepBuffer buf;
    WFCSolver solver;
    solver.Initialize(config, grid, reg, adj, buf);

    // 3. Run solver to completion
    WFCSolveBudget budget(config.max_cells_per_frame, config.max_ms_per_frame);
    budget.Reset();
    WFCSolver::StepResult result = WFCSolver::StepResult::InProgress;
    u32 steps = 0;
    while (result == WFCSolver::StepResult::InProgress && steps < 1000) {
        result = solver.Step(budget);
        ++steps;
    }

    TEST_ASSERT(result == WFCSolver::StepResult::Done ||
                result == WFCSolver::StepResult::GivenUp,
                "Solver terminates");

    // 4. Drain step buffer into point set
    PCGPointSet instances = WFCOutput::ConsumeSteps(buf, reg, 1.0f);

    // 5. Verify point set
    TEST_ASSERT(instances.count > 0, "At least one instance emitted");
    TEST_ASSERT(instances.count <= 64u, "No more than 64 cells in 4x4x4");

    // 6. Verify MeshIndex attrs are catalog placeholders (1000-1004)
    for (u32 i = 0; i < instances.count; ++i) {
        f32 mesh_idx = instances.GetAttr(i, PCGAttr::MeshIndex);
        TEST_ASSERT(mesh_idx >= 1000.0f && mesh_idx <= 1004.0f,
                    "Instance MeshIndex within catalog placeholder range");
    }

    return TestResult::Passed;
}

int main() {
    TestSuite suite("WFC3DParametric");
    TEST_CASE(suite, "Full_Pipeline_4x4x4",
              TestWFC3DParametric_Full_Pipeline_4x4x4);
    suite.RunAllTests();
    return 0;
}
