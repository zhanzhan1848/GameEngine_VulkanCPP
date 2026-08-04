// Engine/Graphics/WFC/WFCSolver.cpp
//
// Task 7 (Phase A.2): WFCSolver orchestrator implementation.
//
// Self-review notes (caught before commit):
//   * The plan's CollapseCell used `TEST_ASSERT(candidate_count > 0, ...)`.
//     TEST_ASSERT expands to `return Engine::Test::TestResult::Failed;` which
//     is meaningless inside production (non-test) code and would compile-fail
//     anyway (TestResult isn't in scope here). Replaced with `assert()`.
//   * The plan's Initialize did NOT call grid.Initialize(), but the test
//     fixture constructs a default WaveGrid (empty cells) and immediately
//     reads grid.CellAt({0,0,0}) after solver.Initialize. The solver owns
//     sizing the grid to match config.grid_size — added the call.
#include "WFCSolver.h"
#include "WaveGrid.h"
#include "TileAdjacency.h"
#include "WFCTileRegistry.h"
#include "WFCStepBuffer.h"
#include "WFCSolveBudget.h"
#include "WFCMinEntropyObserver.h"

#include <cassert>

namespace primal::graphics::wfc {

WFCSolver::WFCSolver()
    : observer_(std::make_unique<WFCMinEntropyObserver>()) {}

void WFCSolver::Initialize(const WFCConfig& config,
                           WaveGrid& grid,
                           const WFCTileRegistry& registry,
                           const TileAdjacencyTable& adjacency,
                           WFCStepBuffer& step_buffer) {
    grid_         = &grid;
    registry_     = &registry;
    adjacency_    = &adjacency;
    step_buffer_  = &step_buffer;
    rng_          = WFCRandom{config.seed};
    generation_   = 0;
    max_generations_ = config.max_generations;
    restart_      = RestartPolicy{config.max_generations};
    // Phase C.1: cache the category mask so PopulateAllCandidates (which runs
    // here and on every restart) can filter without re-threading the config.
    active_category_mask_ = config.active_category_mask;

    // The solver owns the responsibility of sizing the grid to match the
    // config. This keeps the test fixture (and downstream PCG callers) from
    // having to thread MaxVariants through a separate Initialize call.
    // WaveGrid::Initialize asserts size > 0 and max_tile_variants > 0; we
    // defensively bump both so a misconfigured config doesn't crash.
    u32 max_variants = registry.MaxVariants();
    if (max_variants == 0) max_variants = 1;
    if (max_variants > WFCCell::MaxTileCandidates) {
        max_variants = WFCCell::MaxTileCandidates;
    }
    grid.Initialize(config.grid_size, max_variants);

    PopulateAllCandidates(grid, registry);
    observer_->Initialize(grid);
    propagator_.Initialize(grid);
}

void WFCSolver::PopulateAllCandidates(WaveGrid& grid, const WFCTileRegistry& registry) {
    // Phase A.3 multi-tile: iterate registry tiles, set bit for each (tile, variant).
    // Bit layout: bit = tile_id * MaxVariantsPerTile + variant (see WFCTileRegistry).
    // Phase C.1: skip tiles whose category isn't in active_category_mask_ so a
    // solver can restrict the wave to one thematic group (e.g. Ruins-only).
    u64 full_mask = 0;
    for (u32 t = 0; t < registry.Count(); ++t) {
        const WFCTile& tile = registry.Get(wfc_tile_id{t});
        if (!CategoryInMask(tile.category, active_category_mask_)) continue;
        for (u32 v = 0; v < tile.variant_count; ++v) {
            u32 bit = WFCTileRegistry::BitForTileVariant(wfc_tile_id{t}, v);
            if (bit < 64) {
                full_mask |= (1ULL << bit);
            }
        }
    }
    last_populated_mask_ = full_mask;
    u32 total_candidates = static_cast<u32>(__builtin_popcountll(full_mask));

    auto& cells = grid.CellsMutable();
    for (u32 i = 0; i < cells.size(); ++i) {
        WFCCell& c = cells[i];
        c.candidate_mask    = full_mask;
        c.candidate_count   = total_candidates;
        c.entropy           = static_cast<u8>(total_candidates);
        c.collapsed         = false;
        c.collapsed_tile    = wfc_tile_id{0};
        c.collapsed_variant = 0;
    }
}

void WFCSolver::CollapseCell(WaveGrid& grid, WFCGridCoord coord,
                             const WFCTileRegistry& /*registry*/) {
    // Phase A.3: registry is currently unused here — we decode (tile, variant)
    // from the chosen bit using WFCTileRegistry's static packing helpers, so
    // no per-tile lookup is required. The parameter is kept in the signature
    // for future extensions (e.g. weighted tile picks, tile-specific RNG).
    WFCCell& c = grid.CellAt(coord);
    u32 candidate_count = c.candidate_count;
    // FIXED: The plan had `TEST_ASSERT(candidate_count > 0, ...)` here, but
    // TEST_ASSERT is a test-only macro that expands to
    // `return Engine::Test::TestResult::Failed;`. In production code that
    // return-statement wouldn't even compile (TestResult not in scope), and
    // even if it did, silently "failing" a test from inside the solver is the
    // wrong behavior. assert() is the correct choice — it's a programmer-error
    // invariant (caller is required to skip already-collapsed cells via the
    // observer), not a runtime-recoverable condition.
    assert(candidate_count > 0);

    // Pick the Nth set bit in candidate_mask, where N is in [0, candidate_count).
    // Example: mask=0b1010 (bits 1,3 set), candidate_count=2.
    //   pick=0 → bit 1,   pick=1 → bit 3.
    u32 pick = rng_.NextRange(candidate_count);
    u64 m = c.candidate_mask;
    u32 chosen_bit = 0;
    while (m) {
        if (pick == 0) {
            chosen_bit = __builtin_ctzll(m);
            break;
        }
        m &= m - 1;             // clear the lowest set bit
        chosen_bit = __builtin_ctzll(m);
        --pick;
    }

    // Phase A.3: decode (tile, variant) from chosen_bit using registry packing.
    // Bit layout: bit = tile_id * MaxVariantsPerTile + variant (see
    // WFCTileRegistry::BitForTileVariant). The previous Phase A.2 code assumed
    // a single-tile registry and hard-coded collapsed_tile=0 with
    // variant=chosen_bit — wrong for any tile beyond the first.
    wfc_tile_id chosen_tile    = WFCTileRegistry::TileForBit(chosen_bit);
    u32         chosen_variant = WFCTileRegistry::VariantForBit(chosen_bit);

    c.candidate_mask    = (1ULL << chosen_bit);
    c.candidate_count   = 1;
    c.entropy           = 0;
    c.collapsed         = true;
    c.collapsed_tile    = chosen_tile;
    c.collapsed_variant = chosen_variant;

    // Record the step so the main thread (or test) can observe progress.
    WFCStep step{};
    step.kind     = WFCStepKind::Collapse;
    step.coord    = coord;
    step.tile     = chosen_tile;
    step.variant  = chosen_variant;
    step_buffer_->Push(step);

    // Queue face-neighbors for the propagation cascade. The propagator's
    // OnCellCollapsed takes (tile, variant) so Phase A.3 can swap in a real
    // multi-tile candidate picker without changing the propagator contract.
    propagator_.OnCellCollapsed(grid, coord, chosen_tile, chosen_variant);
}

bool WFCSolver::RunPropagationCascade(WaveGrid& grid, const TileAdjacencyTable& adjacency,
                                      WFCSolveBudget& /*budget*/) {
    // Loop until the dirty queue drains or a contradiction surfaces.
    // `changed == 0` is also a stop condition — it means the queue shrank
    // (because entries got filtered as collapsed) but no cell's mask changed,
    // so further passes won't make progress.
    bool contradiction = false;
    while (propagator_.HasDirty()) {
        u32 changed = propagator_.RunPass(grid, adjacency, *registry_,
                                          WFC_FACE_COUNT_3D, contradiction);
        if (contradiction) return false;
        if (changed == 0) break;
    }
    return true;
}

WFCSolver::StepResult WFCSolver::Step(WFCSolveBudget& budget) {
    // Honor the budget on entry — caller may have exhausted it on a previous
    // Step within the same frame.
    if (!budget.ShouldContinue()) return StepResult::InProgress;

    WFCGridCoord coord = observer_->PickNextCollapse(*grid_);
    if (coord.x < 0) {
        // Observer returns {-1,-1,-1} when every cell is collapsed (or every
        // remaining cell is a zero-candidate contradiction, which we treat as
        // "done" here — Phase B will likely want to distinguish).
        return StepResult::Done;
    }

    CollapseCell(*grid_, coord, *registry_);
    last_picked_ = coord;
    budget.OnCellCollapsed();

    bool ok = RunPropagationCascade(*grid_, *adjacency_, budget);
    if (!ok) {
        // Contradiction → ask RestartPolicy.
        auto decision = restart_.OnContradiction(coord,
            grid_->CellAt(coord).collapsed_tile, generation_);
        if (decision == RestartPolicy::Decision::GiveUp) {
            return StepResult::GivenUp;
        }
        // Restart: re-initialize candidate masks, rebuild observer heap, and
        // emit a Restart step so consumers can wipe their derived state.
        ++generation_;
        PopulateAllCandidates(*grid_, *registry_);
        observer_->Initialize(*grid_);
        propagator_.Initialize(*grid_);

        WFCStep step{};
        step.kind       = WFCStepKind::Restart;
        step.generation = generation_;
        step_buffer_->Push(step);
        return StepResult::Restarted;
    }

    // Propagation succeeded. If the observer's heap is empty, the grid is
    // fully collapsed — return Done on the same Step that collapsed the final
    // cell (otherwise a 1x1x1 grid would require two Steps: InProgress then
    // Done). We avoid calling PickNextCollapse here because that would pop a
    // real coord and force us to push it back; Empty() is a cheap peek.
    if (observer_->Empty()) {
        return StepResult::Done;
    }
    return StepResult::InProgress;
}

void WFCSolver::SetObserver(std::unique_ptr<WFCObserver> observer) {
    assert(observer && "SetObserver(nullptr) is not allowed; use ResetToDefaultObserver()");
    observer_ = std::move(observer);
}

void WFCSolver::ResetToDefaultObserver() {
    observer_ = std::make_unique<WFCMinEntropyObserver>();
}

} // namespace primal::graphics::wfc
