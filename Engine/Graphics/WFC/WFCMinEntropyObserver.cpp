// Engine/Graphics/WFC/WFCMinEntropyObserver.cpp
#include "WFCMinEntropyObserver.h"
#include "WaveGrid.h"

#include <algorithm>

namespace primal::graphics::wfc {

namespace {
// std::*_heap use comparator with "greater" semantics for a min-heap:
// returns true when a should be ordered AFTER (i.e. deeper than) b.
bool ComparePairGreater(const std::pair<u8, WFCGridCoord>& a,
                        const std::pair<u8, WFCGridCoord>& b) {
    return a.first > b.first;
}
} // namespace

void WFCMinEntropyObserver::Initialize(const WaveGrid& grid) {
    heap_.clear();
    const auto& cells = grid.Cells();
    const WFCGridCoord size = grid.Size();
    const u32 total = static_cast<u32>(size.x) * size.y * size.z;
    for (u32 i = 0; i < total && i < cells.size(); ++i) {
        const WFCCell& c = cells[i];
        if (!c.collapsed && c.entropy > 0) {
            // Decode row-major index → (x, y, z). Matches WaveGrid::CoordToIndex.
            s32 z = static_cast<s32>(i / (static_cast<u32>(size.x) * size.y));
            s32 y = static_cast<s32>((i / static_cast<u32>(size.x)) % size.y);
            s32 x = static_cast<s32>(i % static_cast<u32>(size.x));
            heap_.push_back({c.entropy, {x, y, z}});
        }
    }
    std::make_heap(heap_.begin(), heap_.end(), ComparePairGreater);
}

void WFCMinEntropyObserver::OnCellChanged(WFCGridCoord c) {
    // Push a sentinel with entropy=0 so it bubbles to the top of the min-heap
    // and gets re-evaluated first. PickNextCollapse validates against the
    // live cell and re-pushes with the actual current entropy if needed.
    // The caller MUST have updated the cell BEFORE calling this.
    heap_.push_back({0, c});
    std::push_heap(heap_.begin(), heap_.end(), ComparePairGreater);
}

WFCGridCoord WFCMinEntropyObserver::PickNextCollapse(const WaveGrid& grid) {
    while (!heap_.empty()) {
        std::pop_heap(heap_.begin(), heap_.end(), ComparePairGreater);
        auto [ent, coord] = heap_.back();
        heap_.pop_back();

        // Defensive bounds check (forward-declared WaveGrid cannot assume
        // negative coords are invalid; the test for "all collapsed" relies
        // on us returning {-1,-1,-1}).
        if (coord.x < 0 || coord.y < 0 || coord.z < 0) continue;
        WFCGridCoord size = grid.Size();
        if (coord.x >= size.x || coord.y >= size.y || coord.z >= size.z) continue;

        const WFCCell& c = grid.CellAt(coord);
        if (c.collapsed) continue;
        if (c.entropy == 0) continue;        // contradiction cell, skip
        if (c.entropy != ent) {
            // Stale entry; re-push with current entropy and try next.
            // We rely on the heap invariant to surface the true minimum
            // on a subsequent pop. The push is bounded by mutation count.
            heap_.push_back({c.entropy, coord});
            std::push_heap(heap_.begin(), heap_.end(), ComparePairGreater);
            continue;
        }
        return coord;
    }
    return {-1, -1, -1};
}

} // namespace primal::graphics::wfc
