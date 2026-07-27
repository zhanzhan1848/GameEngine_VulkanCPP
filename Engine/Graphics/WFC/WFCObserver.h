// Engine/Graphics/WFC/WFCObserver.h
//
// Task 2 (Phase A.2): Min-entropy cell selector for the WFC solver.
//
// The Observer tracks the per-cell entropy of a WaveGrid and answers
// "which cell should I collapse next?" in O(log N) amortised time.
//
// Design notes:
//   * Backing store is a binary min-heap keyed by (entropy, coord).
//   * OnCellChanged pushes a (0, coord) sentinel — PickNextCollapse lazily
//     validates the popped entry against the live cell and discards stale
//     ones. This avoids requiring the caller to thread the grid through
//     OnCellChanged, and the cost is bounded by the number of mutations.
//   * The lazy filter is the only correctness guarantee; entries in the
//     heap may be stale and that is fine.
#pragma once

#include "../../Common/CommonHeaders.h"
#include "WFCTypes.h"

#include <vector>
#include <utility>

namespace primal::graphics::wfc {

class WaveGrid;  // forward declaration

class WFCObserver {
public:
    // Populate heap from all non-collapsed cells.
    void Initialize(const class WaveGrid& grid);

    // Notify that a cell's entropy changed; will be re-pushed onto heap.
    // (Stale entries in the heap are filtered lazily on Pop.)
    void OnCellChanged(WFCGridCoord c);

    // Returns coord with minimum entropy, or {-1,-1,-1} if grid is fully
    // collapsed (or all remaining cells are contradictions with entropy 0).
    WFCGridCoord PickNextCollapse(const class WaveGrid& grid);

    bool Empty() const { return heap_.empty(); }

private:
    // Min-heap of (entropy, coord). Lazy deletion: when we pop, check if
    // entropy still matches the current cell. If not, discard and pop again.
    std::vector<std::pair<u8, WFCGridCoord>> heap_;
};

} // namespace primal::graphics::wfc
