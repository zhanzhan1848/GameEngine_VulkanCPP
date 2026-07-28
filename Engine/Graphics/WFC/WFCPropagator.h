// Engine/Graphics/WFC/WFCPropagator.h
//
// Task 3 (Phase A.2): Constraint propagation scaffolding for the WFC solver.
//
// The Propagator maintains a dirty queue of cell coords that need their
// candidate sets re-evaluated after a collapse. OnCellCollapsed enqueues the
// 6 face-neighbors of the just-collapsed cell; RunPass (Task 4) will drain
// the queue using AC-4-style constraint propagation against a
// TileAdjacencyTable.
//
// Design notes:
//   * State is intentionally minimal — just a utl::vector<WFCGridCoord>.
//     Task 4 will likely add an in-queue bitset to suppress duplicates.
//   * OOB and already-collapsed neighbors are filtered at enqueue time so
//     RunPass does not have to worry about them.
//   * RunPass is a no-op stub in Task 3; the real AC-4 logic lands in Task 4.
#pragma once

#include "../../Common/CommonHeaders.h"
#include "../../Utilities/Vector.h"
#include "WFCTypes.h"

namespace primal::graphics::wfc {

class WaveGrid;
class TileAdjacencyTable;
class WFCTileRegistry;

class WFCPropagator {
public:
    // Reset internal state. The grid is passed so future implementations can
    // pre-size auxiliary structures (e.g. an in-queue bitset sized to the
    // cell count); Task 3 just clears the queue.
    void Initialize(const class WaveGrid& grid);

    // Called when (coord) just collapsed to (tile, variant). Queues the
    // 6 face-neighbors of coord for processing. Already-collapsed neighbors
    // and out-of-bounds coords are skipped.
    void OnCellCollapsed(const class WaveGrid& grid, WFCGridCoord coord,
                         wfc_tile_id tile, u32 variant);

    // Process all queued neighbors. Returns the number of cells whose
    // candidate sets changed.
    //   grid:      mutable wave grid (candidates will be pruned)
    //   adjacency: source of compatible (tile, variant) pairs
    //   registry:  tile registry — provides the bit <-> (tile, variant) packing
    //              (Phase A.3 multi-tile candidate space)
    //   out_contradiction: set true if any cell's candidate_count hits zero
    u32 RunPass(class WaveGrid& grid, const class TileAdjacencyTable& adjacency,
                const class WFCTileRegistry& registry, bool& out_contradiction);

    // For test access / solver introspection
    u32 DirtyQueueSize() const { return static_cast<u32>(dirty_queue_.size()); }
    bool HasDirty() const { return !dirty_queue_.empty(); }

private:
    utl::vector<WFCGridCoord> dirty_queue_;
};

} // namespace primal::graphics::wfc
