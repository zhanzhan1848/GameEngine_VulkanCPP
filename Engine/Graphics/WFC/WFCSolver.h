// Engine/Graphics/WFC/WFCSolver.h
//
// Task 7 (Phase A.2): WFC solver orchestrator.
//
// The solver ties together the four Phase A.2 primitives:
//   * WFCObserver     — picks the next cell to collapse (strategy-injected;
//                       default is MinEntropy; see SetObserver / ResetToDefaultObserver).
//   * WFCPropagator   — AC-4-style candidate pruning after each collapse.
//   * RestartPolicy   — decides restart-vs-give-up on contradiction.
//   * WFCRandom       — deterministic tie-breaking when picking variants.
//
// Plus the Phase A.1 primitives (WaveGrid, TileAdjacencyTable,
// WFCTileRegistry, WFCStepBuffer, WFCSolveBudget).
//
// Threading contract (Phase A.2):
//   * Initialize — main thread, called once per solve (and on each restart).
//   * Step       — solver thread, called repeatedly until Done/GivenUp.
//   * The WFCStepBuffer is the only cross-thread channel to the main thread.
//
// The solver is intentionally single-threaded in Phase A.2; the budget
// primitive + step buffer let it time-slice across frames without blocking
// the main thread, but the actual collapse/propagation runs synchronously.
#pragma once

#include "../../Common/CommonHeaders.h"
#include "WFCConfig.h"
#include "WFCCategory.h"
#include "WFCObserver.h"
#include "WFCPropagator.h"
#include "RestartPolicy.h"
#include "WFCRandom.h"
#include "WFCTypes.h"

#include <memory>

namespace primal::graphics::wfc {

class WaveGrid;
class TileAdjacencyTable;
class WFCTileRegistry;
class WFCStepBuffer;
class WFCSolveBudget;

class WFCSolver {
public:
    enum class StepResult : u8 {
        InProgress = 0,  // collapsed at least one cell this Step, more remain
        Done       = 1,  // grid is fully collapsed
        Restarted  = 2,  // hit a contradiction; re-initialized from scratch
        GivenUp    = 3,  // exhausted max_generations; caller must fall back
    };

    // Default-constructs the observer to WFCMinEntropyObserver. WFCSolver is
    // default-constructible so test fixtures and PCG callers can write
    // `WFCSolver solver;` without knowing about the strategy API. Use
    // SetObserver() to inject a custom strategy before Initialize().
    WFCSolver();

    // Wires up subsystem pointers and seeds the RNG. Idempotent: safe to call
    // again on restart (the test fixture relies on this — re-Initialize wipes
    // the grid back to the full-candidate state).
    void Initialize(const WFCConfig& config,
                    WaveGrid& grid,
                    const WFCTileRegistry& registry,
                    const TileAdjacencyTable& adjacency,
                    WFCStepBuffer& step_buffer);

    // Performs one collapse + propagation cascade. Returns:
    //   * InProgress — collapsed a cell, more cells remain
    //   * Done       — observer found no more cells to collapse
    //   * Restarted  — contradiction; grid re-initialized, generation bumped
    //   * GivenUp    — contradiction after max_generations restarts
    //
    // `budget` is consumed via OnCellCollapsed; the caller is expected to
    // Reset() it at frame start. If budget.ShouldContinue() is false on
    // entry, Step returns InProgress without doing any work.
    StepResult Step(WFCSolveBudget& budget);

    // Replace the current observer. Must be called before Initialize.
    // nullptr is rejected in debug builds; use ResetToDefaultObserver() to
    // restore the default MinEntropy observer.
    void SetObserver(std::unique_ptr<WFCObserver> observer);

    // Restore the default MinEntropy observer.
    void ResetToDefaultObserver();

    u32 Generation() const { return generation_; }

    // Debug/test only: fills `out` with the full u64[kMaskWords] candidate
    // mask computed by the last PopulateAllCandidates call (post category-mask
    // filtering). All-zero if PopulateAllCandidates has not run yet.
    // C++ arrays can't be returned by value, so we use an output parameter.
    void LastPopulatedMaskForTest(u64 out[WFCCell::kMaskWords]) const;

private:
    // Fills every cell with the full candidate mask derived from the registry's
    // MaxVariants(). Phase A.2 single-tile assumption: bit index == variant
    // index of tile 0. Phase A.3 will generalize to multi-tile packing.
    void PopulateAllCandidates(WaveGrid& grid, const WFCTileRegistry& registry);

    // Picks one variant out of the cell's candidate set using rng_, marks the
    // cell collapsed, pushes a WFCStepKind::Collapse record, and notifies the
    // propagator (which enqueues face-neighbors for the next RunPass).
    void CollapseCell(WaveGrid& grid, WFCGridCoord coord,
                      const WFCTileRegistry& registry);

    // Drains the propagator's dirty queue. Returns true on success, false if
    // any cell's candidate_count hits zero (contradiction).
    bool RunPropagationCascade(WaveGrid& grid, const TileAdjacencyTable& adjacency,
                               WFCSolveBudget& budget);

    WaveGrid*                  grid_{nullptr};
    const WFCTileRegistry*     registry_{nullptr};
    const TileAdjacencyTable*  adjacency_{nullptr};
    WFCStepBuffer*             step_buffer_{nullptr};

    std::unique_ptr<WFCObserver> observer_;
    WFCPropagator   propagator_;
    // RestartPolicy has no default ctor (requires max_generations); give it
    // a placeholder so WFCSolver itself remains default-constructible. The
    // test fixture relies on `WFCSolver solver;` working — Initialize() will
    // overwrite this with a properly-seeded policy.
    RestartPolicy   restart_{8};
    WFCRandom       rng_{1};  // seed=0 would xorshift to 0; ctor bumps to 1

    WFCGridCoord    last_picked_{-1, -1, -1};
    u32             generation_{0};
    u32             max_generations_{8};

    // Phase C.1: cached from WFCConfig in Initialize so PopulateAllCandidates
    // (also called on restart) can re-apply the category filter without needing
    // the config passed back in. Default ~0ULL = all categories eligible.
    u64             active_category_mask_{~0ULL};
    // Phase C.1 Task 3: widened to full u64[kMaskWords] so test introspection
    // can verify multi-word populated masks (e.g. 64-tile registrations where
    // bits span words 0..3). All-zero until PopulateAllCandidates runs.
    u64             last_populated_mask_[WFCCell::kMaskWords] = {0, 0, 0, 0};
};

} // namespace primal::graphics::wfc
