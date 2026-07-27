// Engine/Graphics/WFC/WFCPropagator.cpp
#include "WFCPropagator.h"
#include "WaveGrid.h"
#include "TileAdjacency.h"

namespace primal::graphics::wfc {

namespace {
// Enqueue (c) if it is inside the grid and not yet collapsed.
// Out-of-bounds and collapsed neighbors are filtered here so RunPass can
// assume every queued entry is a viable propagation target.
void TryEnqueueNeighbor(utl::vector<WFCGridCoord>& q, const WaveGrid& grid,
                        WFCGridCoord c) {
    WFCGridCoord size = grid.Size();
    if (c.x < 0 || c.x >= size.x) return;
    if (c.y < 0 || c.y >= size.y) return;
    if (c.z < 0 || c.z >= size.z) return;
    if (grid.CellAt(c).collapsed) return;
    q.push_back(c);
}
} // namespace

void WFCPropagator::Initialize(const WaveGrid& grid) {
    (void)grid;
    dirty_queue_.clear();
}

void WFCPropagator::OnCellCollapsed(const WaveGrid& grid, WFCGridCoord coord,
                                    wfc_tile_id tile, u32 variant) {
    (void)tile; (void)variant;
    // Queue all 6 face-neighbors that aren't already collapsed.
    // Order does not matter for correctness; RunPass drains the whole queue.
    TryEnqueueNeighbor(dirty_queue_, grid, {coord.x + 1, coord.y, coord.z});
    TryEnqueueNeighbor(dirty_queue_, grid, {coord.x - 1, coord.y, coord.z});
    TryEnqueueNeighbor(dirty_queue_, grid, {coord.x, coord.y + 1, coord.z});
    TryEnqueueNeighbor(dirty_queue_, grid, {coord.x, coord.y - 1, coord.z});
    TryEnqueueNeighbor(dirty_queue_, grid, {coord.x, coord.y, coord.z + 1});
    TryEnqueueNeighbor(dirty_queue_, grid, {coord.x, coord.y, coord.z - 1});
}

u32 WFCPropagator::RunPass(WaveGrid& grid, const TileAdjacencyTable& /*adjacency*/,
                           bool& out_contradiction) {
    // Phase A.2 Task 4 will fill this in with AC-4 logic.
    // For Task 3 we just clear the queue (no propagation yet) so tests pass.
    (void)grid;
    out_contradiction = false;
    u32 changed = 0;
    dirty_queue_.clear();
    return changed;
}

} // namespace primal::graphics::wfc
