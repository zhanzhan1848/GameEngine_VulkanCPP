// Engine/Graphics/WFC/WFCPropagator.cpp
#include "WFCPropagator.h"
#include <bit>
#include "WaveGrid.h"
#include "TileAdjacency.h"
#include "WFCTileRegistry.h"

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
                           const WFCTileRegistry& registry,
                           u32 face_count, bool& out_contradiction) {
    (void)registry;  // static helpers only; instance reserved for Phase B
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
        //
        // Phase C.1 Task 2: candidate_mask is now u64[kMaskWords] (256 bits),
        // so the snapshot / working set / writeback are all per-word arrays.
        u64 old_mask[WFCCell::kMaskWords];
        u64 new_mask[WFCCell::kMaskWords];
        for (u32 w = 0; w < WFCCell::kMaskWords; ++w) {
            old_mask[w] = cell.candidate_mask[w];
            new_mask[w] = old_mask[w];
        }

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
        // face_count: 6 = full 3D, 4 = 2D (skip ±Z entries at indices 4, 5).
        // kFaces[] still has all 6 entries; the loop bound controls which are queried.
        for (u32 fi = 0; fi < face_count && fi < 6; ++fi) {
            const auto& f = kFaces[fi];
            WFCGridCoord n{c.x + f.dx, c.y + f.dy, c.z + f.dz};
            if (n.x < 0 || n.x >= size.x) continue;
            if (n.y < 0 || n.y >= size.y) continue;
            if (n.z < 0 || n.z >= size.z) continue;
            const WFCCell& neighbor = grid.CellAt(n);
            if (!neighbor.collapsed) continue;

            // Build the per-word allowed mask by scanning set bits of new_mask[]
            // across all kMaskWords words. Each set bit b at word w / in-word
            // position p decodes to global bit (w*64 + p), which maps to
            // (tile, variant) via the registry's static packing.
            u64 allowed[WFCCell::kMaskWords] = {0, 0, 0, 0};
            for (u32 w = 0; w < WFCCell::kMaskWords; ++w) {
                u64 m = new_mask[w];
                while (m) {
                    u32 in_word = CountrZeroU64(m);
                    m &= m - 1;
                    u32 bit = w * WFCTileRegistry::kBitsPerMaskWord + in_word;
                    wfc_tile_id my_tile = WFCTileRegistry::TileForBit(bit);
                    u32 my_variant = WFCTileRegistry::VariantForBit(bit);
                    if (adjacency.Compatible(my_tile, my_variant, f.my_face,
                                             neighbor.collapsed_tile,
                                             neighbor.collapsed_variant)) {
                        allowed[w] |= (1ULL << in_word);
                    }
                }
            }
            for (u32 w = 0; w < WFCCell::kMaskWords; ++w) {
                new_mask[w] &= allowed[w];
            }
        }

        // Detect change + writeback across all words; accumulate popcount.
        bool cell_changed = false;
        u32 total_count = 0;
        for (u32 w = 0; w < WFCCell::kMaskWords; ++w) {
            if (new_mask[w] != old_mask[w]) {
                cell.candidate_mask[w] = new_mask[w];
                cell_changed = true;
            }
            total_count += PopcountU64(new_mask[w]);
        }

        if (cell_changed) {
            cell.candidate_count = total_count;
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
