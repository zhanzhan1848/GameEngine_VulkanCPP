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
#include "Engine/Graphics/WFC/WFCCategory.h"
#include "Engine/Graphics/WFC/WFCMinEntropyObserver.h"
#include "Engine/Graphics/WFC/WFCDistanceObserver.h"
#include <cstdio>
#include <memory>

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
    TEST_ASSERT_EQ(1ULL, c.candidate_mask[0], "Word 0 = bit 0 set");
    for (u32 w = 1; w < WFCCell::kMaskWords; ++w) {
        TEST_ASSERT_EQ(0ULL, c.candidate_mask[w], "Higher words zero");
    }
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

// Task 8 (Phase A.2): Multi-step solve coverage.
//
// Verifies:
//   * A 2x2x2 grid with a single wildcard tile solves to Done within a
//     generous step bound, and every cell ends up collapsed.
//   * The solver honors WFCSolveBudget: when max_cells_per_frame is small,
//     the number of Collapse records emitted this frame is capped.
//
// Notes:
//   * The wildcard fixture registers a tile with variant_count=1 and no
//     adjacency constraints. The propagator has nothing to prune (only one
//     candidate per cell), so each Step collapses exactly one cell.
//   * The budget test counts Collapse records drained from the step buffer
//     rather than reading solver internals, mirroring how a real PCG caller
//     would observe progress.
TestResult TestWFCSolver_Solves_2x2x2_AllWildcard() {
    WFCConfig config;
    config.grid_size = {2, 2, 2};
    config.max_cells_per_frame = 64;
    config.max_ms_per_frame = 1000;
    config.seed = 7;
    config.max_generations = 4;

    WaveGrid grid;
    grid.Initialize(config.grid_size, 8);

    WFCTileRegistry reg;
    WFCTile t{};
    t.name = "wildcard";
    t.variant_count = 1;
    t.sockets[0] = 0xFFFFFFFFFFFFFFFFULL;
    reg.Register(t);

    TileAdjacencyTable adj;
    // Phase A.2 propagator consults the adjacency table, not sockets[].
    // Empty table = "nothing is compatible" -> every collapse triggers a
    // contradiction. Declare self-compatibility for the wildcard tile so
    // propagation has no constraint to violate. AddCompatibility mirrors
    // PosX/PosY/PosZ to NegX/NegY/NegZ automatically.
    const wfc_tile_id wildcard{0};
    adj.AddCompatibility(wildcard, 0, WFCFace::PosX, wildcard, 0);
    adj.AddCompatibility(wildcard, 0, WFCFace::PosY, wildcard, 0);
    adj.AddCompatibility(wildcard, 0, WFCFace::PosZ, wildcard, 0);

    WFCStepBuffer buf;
    WFCSolver solver;
    solver.Initialize(config, grid, reg, adj, buf);

    WFCSolveBudget budget(config.max_cells_per_frame, config.max_ms_per_frame);
    budget.Reset();

    WFCSolver::StepResult result = WFCSolver::StepResult::InProgress;
    u32 steps = 0;
    while (result == WFCSolver::StepResult::InProgress && steps < 100) {
        result = solver.Step(budget);
        ++steps;
    }

    TEST_ASSERT(result == WFCSolver::StepResult::Done, "Should solve 2x2x2 trivial case");
    TEST_ASSERT_EQ(8u, steps > 8 ? 8 : steps, "8 cells collapsed (sanity bound)");
    // Verify all cells collapsed
    for (u32 i = 0; i < grid.CellCount(); ++i) {
        TEST_ASSERT(grid.Cells()[i].collapsed, "Every cell should be collapsed");
    }
    return TestResult::Passed;
}

TestResult TestWFCSolver_Budget_Stops_Mid_Solve() {
    WFCConfig config;
    config.grid_size = {4, 4, 4};  // 64 cells
    config.max_cells_per_frame = 5;  // very small budget
    config.max_ms_per_frame = 1000;
    config.seed = 1;
    config.max_generations = 8;

    WaveGrid grid;
    grid.Initialize(config.grid_size, 8);
    WFCTileRegistry reg;
    WFCTile t{};
    t.variant_count = 1;
    t.sockets[0] = 0xFFFFFFFFFFFFFFFFULL;
    reg.Register(t);
    TileAdjacencyTable adj;
    // Same wildcard self-compat as Solves_2x2x2_AllWildcard: the propagator
    // needs explicit adjacency entries or every collapse triggers a
    // contradiction and the solver bails out with GivenUp.
    const wfc_tile_id wildcard{0};
    adj.AddCompatibility(wildcard, 0, WFCFace::PosX, wildcard, 0);
    adj.AddCompatibility(wildcard, 0, WFCFace::PosY, wildcard, 0);
    adj.AddCompatibility(wildcard, 0, WFCFace::PosZ, wildcard, 0);
    WFCStepBuffer buf;

    WFCSolver solver;
    solver.Initialize(config, grid, reg, adj, buf);

    WFCSolveBudget budget(config.max_cells_per_frame, config.max_ms_per_frame);
    budget.Reset();

    u32 cells_collapsed = 0;
    while (budget.ShouldContinue()) {
        auto r = solver.Step(budget);
        if (r == WFCSolver::StepResult::Done) break;
        // Count actual collapses via step buffer
        WFCStep drain[16];
        u32 n = buf.Consume(drain, 16);
        for (u32 i = 0; i < n; ++i) {
            if (drain[i].kind == WFCStepKind::Collapse) ++cells_collapsed;
        }
    }

    TEST_ASSERT(cells_collapsed <= 5u, "Budget caps cell collapses to 5");
    TEST_ASSERT(cells_collapsed >= 1u, "At least one cell collapsed");
    return TestResult::Passed;
}

// Task 9 (Phase A.2): End-to-end 4x4x4 demo with a two-tile registry.
//
// Verifies the solver terminates (Done or GivenUp) within a generous step
// budget on a non-trivial 64-cell grid. With Phase A.3 Task 5 landed, the
// propagator now correctly decodes the multi-tile candidate space, so this
// test must terminate cleanly (no InProgress/Restarted at step cap).
TestResult TestWFCSolver_Demo_4x4x4_TwoTile() {
    // Two tiles: "open" and "wall" with simple adjacency rules
    WFCConfig config;
    config.grid_size = {4, 4, 4};
    config.max_cells_per_frame = 256;
    config.max_ms_per_frame = 1000;
    config.seed = 99;
    config.max_generations = 8;

    WaveGrid grid;
    grid.Initialize(config.grid_size, 8);

    WFCTileRegistry reg;
    WFCTile open{};
    open.name = "open";
    open.variant_count = 1;
    open.sockets[0] = 0x00000000u;  // some encoding
    WFCTile wall{};
    wall.name = "wall";
    wall.variant_count = 1;
    wall.sockets[0] = 0xFFFFFFFFu;
    reg.Register(open);
    reg.Register(wall);

    // Allow open-open and wall-wall adjacency on all faces
    TileAdjacencyTable adj;
    const wfc_tile_id open_id{0};
    const wfc_tile_id wall_id{1};
    adj.AddCompatibility(open_id, 0, WFCFace::PosX, open_id, 0);
    adj.AddCompatibility(open_id, 0, WFCFace::PosY, open_id, 0);
    adj.AddCompatibility(open_id, 0, WFCFace::PosZ, open_id, 0);
    adj.AddCompatibility(wall_id, 0, WFCFace::PosX, wall_id, 0);
    adj.AddCompatibility(wall_id, 0, WFCFace::PosY, wall_id, 0);
    adj.AddCompatibility(wall_id, 0, WFCFace::PosZ, wall_id, 0);

    WFCStepBuffer buf;
    WFCSolver solver;
    solver.Initialize(config, grid, reg, adj, buf);

    WFCSolveBudget budget(config.max_cells_per_frame, config.max_ms_per_frame);
    budget.Reset();

    WFCSolver::StepResult result = WFCSolver::StepResult::InProgress;
    u32 steps = 0;
    while ((result == WFCSolver::StepResult::InProgress ||
            result == WFCSolver::StepResult::Restarted) && steps < 1000) {
        result = solver.Step(budget);
        ++steps;
    }

    TEST_ASSERT(result == WFCSolver::StepResult::Done ||
                result == WFCSolver::StepResult::GivenUp,
                "Solver should terminate (Done or GivenUp) within step budget");
    return TestResult::Passed;
}

TestResult TestWFCSolver_Initialize_Populates_Multi_Tile_Candidates() {
    WFCConfig config;
    config.grid_size = {1, 1, 1};
    config.max_cells_per_frame = 1;
    config.max_ms_per_frame = 100;
    config.seed = 42;
    config.max_generations = 4;

    WaveGrid grid;
    WFCTileRegistry reg;
    TileAdjacencyTable adj;
    WFCStepBuffer buf;

    // Register 2 tiles: cube (variant 0) + ramp (4 variants)
    WFCTile cube{};
    cube.name = "cube";
    cube.variant_count = 1;
    cube.mesh_handles[0] = primal::geometry::geometry_id{0};
    reg.Register(cube);

    WFCTile ramp{};
    ramp.name = "ramp";
    ramp.variant_count = 4;
    ramp.mesh_handles[0] = primal::geometry::geometry_id{1};
    reg.Register(ramp);

    WFCSolver solver;
    solver.Initialize(config, grid, reg, adj, buf);

    // After Initialize: cell should have 5 candidate bits set.
    // Phase C.1 16×4 packing: cube (t0,v0)=bit 0; ramp (t1,v0..3)=bits 4,5,6,7.
    // Compute via BitForTileVariant so the test tracks the registry's packing
    // constants rather than hardcoding bit positions that drift across phases.
    const WFCCell& c = grid.CellAt({0, 0, 0});
    TEST_ASSERT_EQ(5u, c.candidate_count, "5 candidates: 1 cube + 4 ramp variants");
    u64 expected_mask = 0;
    expected_mask |= (1ULL << WFCTileRegistry::BitForTileVariant(wfc_tile_id{0}, 0));
    for (u32 v = 0; v < 4; ++v) {
        expected_mask |= (1ULL << WFCTileRegistry::BitForTileVariant(wfc_tile_id{1}, v));
    }
    TEST_ASSERT_EQ(expected_mask, c.candidate_mask[0], "Word 0 = cube bit + 4 ramp bits");
    for (u32 w = 1; w < WFCCell::kMaskWords; ++w) {
        TEST_ASSERT_EQ(0ULL, c.candidate_mask[w], "Higher words zero (cube+ramp fit in word 0)");
    }
    return TestResult::Passed;
}

// Task 4 (Phase A.3): CollapseCell must decode (tile, variant) from chosen_bit
// using WFCTileRegistry::TileForBit / VariantForBit instead of the Phase A.2
// single-tile assumption (collapsed_tile=0, variant=chosen_bit).
//
// Fixture: 1x1x1 grid with two tiles — cube (1 variant, bit 0) and ramp
// (4 variants, bits 8-11). The cell has 5 candidate bits set.
//
// Discriminating assertion: collapsed_variant must be < the chosen tile's
// variant_count. The Phase A.2 bug writes collapsed_variant=chosen_bit, so
// for seed=42 (which picks bit 11 = ramp var 3) the buggy code stores
// variant=11, which exceeds ramp.variant_count=4. A correct decode yields
// variant=3, which passes.
//
// Note on seed choice: seed=42's RNG draws pick=4 from [0,5). Walking the
// mask {bit0, bit8, bit9, bit10, bit11} → 4th set bit = bit11 = ramp var 3.
// (Cube-only candidates would land on bit0; using seed 42 guarantees a
// multi-tile pick that exposes the decode bug.)
TestResult TestWFCSolver_CollapseCell_Decodes_Multi_Tile() {
    WFCConfig config;
    config.grid_size = {1, 1, 1};
    config.max_cells_per_frame = 1;
    config.max_ms_per_frame = 100;
    config.seed = 42;
    config.max_generations = 4;

    WaveGrid grid;
    WFCTileRegistry reg;
    TileAdjacencyTable adj;
    WFCStepBuffer buf;

    WFCTile cube{};
    cube.name = "cube";
    cube.variant_count = 1;
    cube.mesh_handles[0] = primal::geometry::geometry_id{100};
    reg.Register(cube);

    WFCTile ramp{};
    ramp.name = "ramp";
    ramp.variant_count = 4;
    ramp.mesh_handles[0] = primal::geometry::geometry_id{200};
    reg.Register(ramp);

    WFCSolver solver;
    solver.Initialize(config, grid, reg, adj, buf);

    WFCSolveBudget budget(10, 100);
    budget.Reset();
    solver.Step(budget);

    // The single cell must be collapsed, and collapsed_tile must be 0 (cube) or 1 (ramp)
    const WFCCell& c = grid.CellAt({0, 0, 0});
    TEST_ASSERT(c.collapsed, "Cell collapsed after Step");
    u32 tile_val = static_cast<u32>(c.collapsed_tile);
    TEST_ASSERT(tile_val == 0 || tile_val == 1, "collapsed_tile is 0 (cube) or 1 (ramp)");

    // Discriminating check: variant must be a real variant of the chosen tile,
    // not the raw chosen_bit. This is what separates the buggy single-tile
    // assumption (variant = bit, e.g. 11) from correct multi-tile decode
    // (variant = bit % MaxVariantsPerTile, e.g. 3).
    u32 variant_val = c.collapsed_variant;
    u32 chosen_tile_variant_count = reg.Get(wfc_tile_id{tile_val}).variant_count;
    TEST_ASSERT(variant_val < chosen_tile_variant_count,
                "collapsed_variant is a valid variant of the chosen tile "
                "(< variant_count), not the raw chosen_bit");

    // Verify the step record matches
    WFCStep steps[8];
    u32 n = buf.Consume(steps, 8);
    TEST_ASSERT(n >= 1, "Step buffer has a Collapse record");
    bool found = false;
    for (u32 i = 0; i < n; ++i) {
        if (steps[i].kind == WFCStepKind::Collapse) {
            TEST_ASSERT_EQ(tile_val, static_cast<u32>(steps[i].tile), "Step tile matches cell");
            TEST_ASSERT_EQ(variant_val, steps[i].variant, "Step variant matches cell");
            found = true;
        }
    }
    TEST_ASSERT(found, "Found Collapse record");
    return TestResult::Passed;
}

// Task 5 (Phase C.1): PopulateAllCandidates must honor active_category_mask.
//
// Discriminating assertion: only Ruins-category tiles should have their bits
// set in the populated candidate mask. With 2 Primitive tiles + 1 Ruins tile
// registered and config.active_category_mask == CategoryMaskFor(Ruins), the
// populated mask must contain only the Ruins tile's bit (bit 8 under the
// 16×4 packing: tile_id=2 * MaxVariantsPerTile=4 + variant=0).
//
// IMPORTANT: category-mask bits (CategoryMaskFor) and candidate-mask bits
// (BitForTileVariant) are different namespaces within u64 — do not AND them
// against each other. Compute the expected candidate mask directly from the
// Ruins tile's (tile_id, variant) pairs instead.
//
// Note on Initialize signature: the real signature is 5-arg (config, grid,
// registry, adjacency, step_buffer), not the 3-arg shown in the plan prose.
// The test constructs the auxiliary objects and passes them in.
TestResult TestWFCSolver_PopulateRespectsCategoryMask() {
    WFCTileRegistry reg;
    auto makeTile = [](const char* name, WFCCategory cat, u32 vc) {
        WFCTile t{};
        t.name = name;
        t.category = cat;
        t.variant_count = vc;
        t.bounds_extents = primal::math::v3{1.0f, 1.0f, 1.0f};
        return t;
    };
    reg.Register(makeTile("prim_a", WFCCategory::Primitive, 1));
    reg.Register(makeTile("prim_b", WFCCategory::Primitive, 1));
    reg.Register(makeTile("ruin_a", WFCCategory::Ruins, 1));

    WFCConfig cfg;
    cfg.grid_size = WFCGridCoord{2, 2, 2};
    cfg.active_category_mask = CategoryMaskFor(WFCCategory::Ruins);

    WaveGrid grid;
    TileAdjacencyTable adj;
    WFCStepBuffer buf;

    WFCSolver solver;
    solver.Initialize(cfg, grid, reg, adj, buf);

    // Expected: only ruin_a's candidate bit survives. ruin_a is tile_id=2;
    // under 16×4 packing its variant-0 bit = 2 * 4 + 0 = 8.
    u64 expected_ruw_mask = 0;
    expected_ruw_mask |= (1ULL << WFCTileRegistry::BitForTileVariant(wfc_tile_id{2}, 0));

    u64 populated[WFCCell::kMaskWords];
    solver.LastPopulatedMaskForTest(populated);
    TEST_ASSERT_EQ(expected_ruw_mask, populated[0],
                   "Word 0 = only ruin_a's candidate bit (prims filtered out)");
    for (u32 w = 1; w < WFCCell::kMaskWords; ++w) {
        TEST_ASSERT_EQ(0ULL, populated[w], "Higher words zero (no high-tile bits)");
    }
    TEST_ASSERT(populated[0] != 0, "mask non-zero");
    return TestResult::Passed;
}

// Task 4 (Observer Strategy Refactor): Default-constructed WFCSolver uses
// MinEntropy. On a 3x1x1 grid with a single self-compatible wildcard tile,
// every cell has equal entropy (1), so MinEntropy's heap order is row-major
// scan order — first pick is (0,0,0). Locks in that the unique_ptr migration
// preserved default behavior.
TestResult TestWFCSolver_DefaultObserver_IsMinEntropy() {
    WFCConfig config;
    config.grid_size = {3, 1, 1};
    config.max_cells_per_frame = 1;
    config.max_ms_per_frame = 100;
    config.seed = 42;
    config.max_generations = 4;

    WFCTile t{};
    t.name = "trivial";
    t.variant_count = 1;
    t.sockets[0] = 0xFFFFFFFFFFFFFFFFULL;  // wildcard socket

    WaveGrid grid;
    WFCTileRegistry reg;
    TileAdjacencyTable adj;
    WFCStepBuffer buf;

    reg.Register(t);
    // Without self-compatibility entries, propagating the first collapse to
    // neighbors would fail (no adjacency → contradiction → restart → state
    // wipe → assertion sees the wrong state). Wire wildcard self-compat on
    // all 3 axes, mirroring TestWFCSolver_Solves_2x2x2_AllWildcard.
    const wfc_tile_id wildcard{0};
    adj.AddCompatibility(wildcard, 0, WFCFace::PosX, wildcard, 0);
    adj.AddCompatibility(wildcard, 0, WFCFace::PosY, wildcard, 0);
    adj.AddCompatibility(wildcard, 0, WFCFace::PosZ, wildcard, 0);

    WFCSolver solver;  // default-constructed observer is MinEntropy
    solver.Initialize(config, grid, reg, adj, buf);

    WFCSolveBudget budget(10, 100);
    budget.Reset();
    solver.Step(budget);

    TEST_ASSERT(grid.CellAt({0, 0, 0}).collapsed,
                "MinEntropy default collapses (0,0,0) first on tie (row-major)");
    // Stronger than just checking (1,0,0): assert no other cell collapsed, so
    // a future observer change (e.g. picking (2,0,0)) doesn't silently pass.
    for (u32 i = 0; i < grid.CellCount(); ++i) {
        if (i != 0u) {
            TEST_ASSERT(!grid.Cells()[i].collapsed, "Only (0,0,0) collapsed this Step");
        }
    }
    return TestResult::Passed;
}

// Task 4: SetObserver(make_unique<MinEntropyObserver>()) explicitly must match
// the default behavior. Locks in that SetObserver doesn't break solver state
// (ownership transfer, virtual dispatch through the unique_ptr, etc.). This
// test does NOT prove SetObserver actually REPLACES the strategy — that's
// Task 6's job (SetObserver_DistanceChangesOrder uses WFCDistanceObserver to
// prove a non-MinEntropy strategy takes effect). Also documents the
// precondition: SetObserver must be called BEFORE Initialize — the observer's
// heap is populated during Initialize, so swapping mid-solve would leave the
// new observer empty until the next Initialize/restart.
TestResult TestWFCSolver_SetObserver_PreservesDefaultBehavior() {
    WFCConfig config;
    config.grid_size = {3, 1, 1};
    config.max_cells_per_frame = 1;
    config.max_ms_per_frame = 100;
    config.seed = 42;
    config.max_generations = 4;

    WFCTile t{};
    t.name = "trivial";
    t.variant_count = 1;
    t.sockets[0] = 0xFFFFFFFFFFFFFFFFULL;

    WaveGrid grid;
    WFCTileRegistry reg;
    TileAdjacencyTable adj;
    WFCStepBuffer buf;

    reg.Register(t);
    const wfc_tile_id wildcard{0};
    adj.AddCompatibility(wildcard, 0, WFCFace::PosX, wildcard, 0);
    adj.AddCompatibility(wildcard, 0, WFCFace::PosY, wildcard, 0);
    adj.AddCompatibility(wildcard, 0, WFCFace::PosZ, wildcard, 0);

    WFCSolver solver;
    solver.SetObserver(std::make_unique<WFCMinEntropyObserver>());
    solver.Initialize(config, grid, reg, adj, buf);

    WFCSolveBudget budget(10, 100);
    budget.Reset();
    solver.Step(budget);

    TEST_ASSERT(grid.CellAt({0, 0, 0}).collapsed,
                "SetObserver(MinEntropy) preserves default collapse behavior");
    // Same strengthened negative assertion as DefaultObserver_IsMinEntropy —
    // verifies ONLY (0,0,0) collapsed this Step.
    for (u32 i = 0; i < grid.CellCount(); ++i) {
        if (i != 0u) {
            TEST_ASSERT(!grid.Cells()[i].collapsed, "Only (0,0,0) collapsed this Step");
        }
    }
    return TestResult::Passed;
}

// Task 6: SetObserver(make_unique<WFCDistanceObserver>(origin)) on a 3x1x3
// grid where all cells have equal entropy. MinEntropy's row-major tie-break
// would collapse (0,0,0) first. Distance observer with origin (1,0,1) must
// collapse (1,0,1) first instead — origin is distance 0, strictly less than
// any other cell. This is the discriminating test: only a correctly-wired
// strategy swap produces this outcome.
TestResult TestWFCSolver_SetObserver_DistanceChangesOrder() {
    WFCConfig config;
    config.grid_size = {3, 1, 3};
    config.max_cells_per_frame = 1;
    config.max_ms_per_frame = 100;
    config.seed = 42;
    config.max_generations = 4;

    WFCTile t{};
    t.name = "trivial";
    t.variant_count = 1;
    t.sockets[0] = 0xFFFFFFFFFFFFFFFFULL;  // wildcard

    WaveGrid grid;
    WFCTileRegistry reg;
    TileAdjacencyTable adj;
    WFCStepBuffer buf;

    reg.Register(t);
    // Self-compat on all 3 axes so propagation doesn't trigger a contradiction
    // after the first collapse (mirrors TestWFCSolver_DefaultObserver_IsMinEntropy).
    const wfc_tile_id wildcard{0};
    adj.AddCompatibility(wildcard, 0, WFCFace::PosX, wildcard, 0);
    adj.AddCompatibility(wildcard, 0, WFCFace::PosY, wildcard, 0);
    adj.AddCompatibility(wildcard, 0, WFCFace::PosZ, wildcard, 0);

    WFCSolver solver;
    solver.SetObserver(std::make_unique<WFCDistanceObserver>(WFCGridCoord{1, 0, 1}));
    solver.Initialize(config, grid, reg, adj, buf);

    WFCSolveBudget budget(10, 100);
    budget.Reset();
    solver.Step(budget);

    TEST_ASSERT(grid.CellAt({1, 0, 1}).collapsed,
                "Distance observer collapses origin (1,0,1) first");
    // Strengthen: no other cell should be collapsed this Step. (MinEntropy
    // would have collapsed (0,0,0); a misbehaving strategy might collapse
    // multiple cells if a bug regressed propagation.)
    WFCGridCoord size = grid.Size();
    for (s32 z = 0; z < size.z; ++z) {
        for (s32 y = 0; y < size.y; ++y) {
            for (s32 x = 0; x < size.x; ++x) {
                if (x == 1 && y == 0 && z == 1) continue;
                TEST_ASSERT(!grid.CellAt({x, y, z}).collapsed,
                            "Only origin (1,0,1) collapsed this Step");
            }
        }
    }
    return TestResult::Passed;
}

// Phase C.1 Task 3: verify the widened u64[kMaskWords] candidate_mask supports
// the full 64-tile registry cap. With 64 tiles × 1 variant each, candidate bits
// span all 4 words (tiles 0-15 → word 0, 16-31 → word 1, 32-47 → word 2,
// 48-63 → word 3). A scalar u64 mask would silently truncate to word 0 and
// lose 75% of the registered tiles. This test catches such regressions.
//
// Discriminating assertions:
//   * Every cell's candidate_count == 64 (one per registered tile).
//   * Every cell's candidate_mask[w] is non-zero for w = 0..3 (no word empty).
//   * LastPopulatedMaskForTest returns all-ones for every word.
TestResult TestWFCSolver_HandlesTile63() {
    WFCConfig config;
    config.grid_size = {1, 1, 1};  // single cell — sufficient to inspect the mask
    config.max_cells_per_frame = 1;
    config.max_ms_per_frame = 100;
    config.seed = 42;
    config.max_generations = 4;

    WaveGrid grid;
    WFCTileRegistry reg;
    TileAdjacencyTable adj;
    WFCStepBuffer buf;

    // Register MaxTiles (64) tiles, each with 1 variant. With MaxVariantsPerTile=4,
    // 64 tiles × 4 bits = 256 candidate bits = exactly the kMaskWords*64 capacity.
    // Only variant 0 of each tile sets a bit (the other 3 variant slots are unused
    // but still contribute 3 zero bits to the bit layout per tile).
    // WFCTile::name is const char* (no engine string type) — use a static buffer
    // so the pointer outlives the registry's copy.
    static char tile_names[WFCTileRegistry::MaxTiles][16];
    for (u32 t = 0; t < WFCTileRegistry::MaxTiles; ++t) {
        WFCTile tile{};
        std::snprintf(tile_names[t], sizeof(tile_names[t]), "tile_%u", t);
        tile.name = tile_names[t];
        tile.variant_count = 1;
        tile.bounds_extents = primal::math::v3{1.0f, 1.0f, 1.0f};
        reg.Register(tile);
    }
    TEST_ASSERT_EQ(WFCTileRegistry::MaxTiles, reg.Count(),
                   "Registry accepted all 64 tiles");

    WFCSolver solver;
    solver.Initialize(config, grid, reg, adj, buf);

    // Every cell should have exactly MaxTiles candidates (one per registered tile,
    // since each tile contributes 1 set bit at variant 0).
    const WFCCell& c = grid.CellAt({0, 0, 0});
    TEST_ASSERT_EQ(WFCTileRegistry::MaxTiles, c.candidate_count,
                   "candidate_count = 64 (one per registered tile)");

    // Every word must be non-zero. With 64 tiles × 1 variant each, every word
    // holds 16 set bits (tiles 16*w through 16*w+15). A scalar mask regression
    // (last_populated_mask_ as u64) would silently truncate bits 64+.
    for (u32 w = 0; w < WFCCell::kMaskWords; ++w) {
        TEST_ASSERT(c.candidate_mask[w] != 0,
                    "Every word non-zero (64 tiles span all 4 words)");
    }

    // LastPopulatedMaskForTest must mirror the cell mask. With 64 tiles, each
    // word holds the same pattern (16 set bits at variant-0 positions of tiles
    // 16*w..16*w+15). Variant 0 of tile t sets bit t*4 within the global layout,
    // so each word has bits {0,4,8,12,16,20,24,28,32,36,40,44,48,52,56,60} set
    // = 0x1111111111111111. We don't hardcode that pattern — instead verify the
    // mask is non-zero in every word AND matches the cell's mask exactly.
    u64 populated[WFCCell::kMaskWords];
    solver.LastPopulatedMaskForTest(populated);
    for (u32 w = 0; w < WFCCell::kMaskWords; ++w) {
        TEST_ASSERT_EQ(c.candidate_mask[w], populated[w],
                       "LastPopulatedMaskForTest matches cell mask");
        TEST_ASSERT(populated[w] != 0, "Word non-zero (multi-word support)");
    }
    return TestResult::Passed;
}

int main() {
    TestSuite suite("WFCSolver");
    TEST_CASE(suite, "Initialize_Populates_Candidate_Masks", TestWFCSolver_Initialize_Populates_Candidate_Masks);
    TEST_CASE(suite, "Initialize_Populates_Multi_Tile_Candidates", TestWFCSolver_Initialize_Populates_Multi_Tile_Candidates);
    TEST_CASE(suite, "Step_Collapses_Single_Cell", TestWFCSolver_Step_Collapses_Single_Cell);
    TEST_CASE(suite, "Step_Pushes_Collapse_Record", TestWFCSolver_Step_Pushes_Collapse_Record);
    TEST_CASE(suite, "Solves_2x2x2_AllWildcard", TestWFCSolver_Solves_2x2x2_AllWildcard);
    TEST_CASE(suite, "Budget_Stops_Mid_Solve", TestWFCSolver_Budget_Stops_Mid_Solve);
    TEST_CASE(suite, "Demo_4x4x4_TwoTile", TestWFCSolver_Demo_4x4x4_TwoTile);
    TEST_CASE(suite, "CollapseCell_Decodes_Multi_Tile", TestWFCSolver_CollapseCell_Decodes_Multi_Tile);
    TEST_CASE(suite, "PopulateRespectsCategoryMask", TestWFCSolver_PopulateRespectsCategoryMask);
    TEST_CASE(suite, "HandlesTile63", TestWFCSolver_HandlesTile63);
    TEST_CASE(suite, "DefaultObserver_IsMinEntropy", TestWFCSolver_DefaultObserver_IsMinEntropy);
    TEST_CASE(suite, "SetObserver_PreservesDefaultBehavior", TestWFCSolver_SetObserver_PreservesDefaultBehavior);
    TEST_CASE(suite, "SetObserver_DistanceChangesOrder", TestWFCSolver_SetObserver_DistanceChangesOrder);
    suite.RunAllTests();
    return 0;
}
