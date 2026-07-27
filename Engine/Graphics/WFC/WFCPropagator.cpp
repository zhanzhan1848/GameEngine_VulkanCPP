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

u32 WFCPropagator::RunPass(WaveGrid& grid, const TileAdjacencyTable& adjacency,
                           bool& out_contradiction) {
    out_contradiction = false;
    u32 changed = 0;

    // Snapshot the queue at pass start. Any neighbors enqueued during this pass
    // (via OnCellCollapsed below) land in dirty_queue_ for the NEXT pass — the
    // caller is expected to invoke RunPass in a loop until DirtyQueueSize()==0.
    utl::vector<WFCGridCoord> current_queue;
    current_queue.swap(dirty_queue_);

    for (WFCGridCoord c : current_queue) {
        WFCCell& cell = grid.CellAt(c);
        if (cell.collapsed) continue;

        // For each face direction, look at the neighbor. If the neighbor is
        // collapsed, prune any of our candidates that are not compatible with
        // the neighbor's (tile, variant) on the opposite face.
        u64 old_mask = cell.candidate_mask;
        u64 new_mask = old_mask;

        static const struct {
            WFCFace my_face;
            WFCFace neighbor_face;
            s32 dx, dy, dz;
        } kFaces[] = {
            {WFCFace::PosX, WFCFace::NegX,  1,  0,  0},
            {WFCFace::NegX, WFCFace::PosX, -1,  0,  0},
            {WFCFace::PosY, WFCFace::NegY,  0,  1,  0},
            {WFCFace::NegY, WFCFace::PosY,  0, -1,  0},
            {WFCFace::PosZ, WFCFace::NegZ,  0,  0,  1},
            {WFCFace::NegZ, WFCFace::PosZ,  0,  0, -1},
        };

        WFCGridCoord size = grid.Size();
        for (auto& f : kFaces) {
            WFCGridCoord n{c.x + f.dx, c.y + f.dy, c.z + f.dz};
            if (n.x < 0 || n.x >= size.x) continue;
            if (n.y < 0 || n.y >= size.y) continue;
            if (n.z < 0 || n.z >= size.z) continue;
            const WFCCell& neighbor = grid.CellAt(n);
            if (!neighbor.collapsed) continue;

            // Build the mask of candidates that survive this face's filter.
            //
            // Phase A.2 candidate space convention: each bit b represents the
            // (tile_id == b, variant == b) pair. This collapses the multi-tile
            // candidate space into a single bit index so the foundation can be
            // exercised without a full tile registry. Phase A.3 will introduce
            // proper (tile, variant) candidate packing.
            u64 allowed = 0;
            u64 m = new_mask;
            while (m) {
                u32 bit = __builtin_ctzll(m);
                m &= m - 1;
                wfc_tile_id my_tile{bit};
                u32 my_variant = bit;
                if (adjacency.Compatible(my_tile, my_variant, f.my_face,
                                         neighbor.collapsed_tile,
                                         neighbor.collapsed_variant)) {
                    allowed |= (1ULL << bit);
                }
            }
            new_mask &= allowed;
        }

        if (new_mask != old_mask) {
            cell.candidate_mask = new_mask;
            cell.candidate_count = static_cast<u32>(__builtin_popcountll(new_mask));
            cell.entropy = static_cast<u8>(cell.candidate_count);
            ++changed;

            if (cell.candidate_count == 0) {
                out_contradiction = true;
            } else {
                // Our candidate set shrank — neighbors may now need to
                // re-check their compatibility against us. OnCellCollapsed's
                // only effect is enqueueing face-neighbors, which is exactly
                // the re-queue semantics we want here. (The name is mildly
                // misleading for the non-collapsed case; revisit in Phase B.)
                OnCellCollapsed(grid, c, cell.collapsed_tile,
                                cell.collapsed_variant);
            }
        }
    }

    return changed;
}

} // namespace primal::graphics::wfc
