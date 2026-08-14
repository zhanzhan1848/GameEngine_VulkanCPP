// Engine/Graphics/WFC/WFCDistanceObserver.cpp
#include "WFCDistanceObserver.h"
#include "WaveGrid.h"

namespace primal::graphics::wfc {

void WFCDistanceObserver::Initialize(const WaveGrid& /*grid*/) {
    // PickNextCollapse re-reads grid.Size() on every call (the grid may be
    // re-sized between Picks via WFCSolver::Initialize on restart), so we
    // don't cache it. Just reset the empty_ latch.
    empty_ = false;
}

void WFCDistanceObserver::OnCellChanged(WFCGridCoord /*c*/) {
    // Distance-from-origin doesn't depend on candidate state. No-op.
}

WFCGridCoord WFCDistanceObserver::PickNextCollapse(const WaveGrid& grid) {
    if (empty_) return {-1, -1, -1};

    const WFCGridCoord size = grid.Size();
    WFCGridCoord best{-1, -1, -1};
    u64 best_dist_sq = static_cast<u64>(-1);  // max u64

    for (s32 z = 0; z < size.z; ++z) {
        for (s32 y = 0; y < size.y; ++y) {
            for (s32 x = 0; x < size.x; ++x) {
                const WFCGridCoord c{x, y, z};
                const WFCCell& cell = grid.CellAt(c);
                if (cell.collapsed) continue;
                if (cell.entropy == 0) continue;  // contradiction
                const s32 dx = x - origin_.x;
                const s32 dy = y - origin_.y;
                const s32 dz = z - origin_.z;
                const u64 dist_sq = u64(dx * dx) + u64(dy * dy) + u64(dz * dz);
                if (dist_sq < best_dist_sq) {
                    best_dist_sq = dist_sq;
                    best = c;
                }
                // Ties: keep the first-found (row-major scan order). Strict <
                // means later equidistant cells do NOT replace the first.
            }
        }
    }

    if (best.x < 0) {
        empty_ = true;
        return {-1, -1, -1};
    }
    return best;
}

} // namespace primal::graphics::wfc
