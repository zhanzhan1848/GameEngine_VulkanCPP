// Engine/Graphics/WFC/RestartPolicy.h
#pragma once

#include "../../Common/CommonHeaders.h"
#include "../../Utilities/Vector.h"
#include "WFCTypes.h"

namespace primal::graphics::wfc {

// Per-conflict record kept by RestartPolicy so the observer can bias future
// restarts away from historically-failing (cell, tile) pairs.
//
// Records are deduplicated by (coord, tile): re-failing the same cell with the
// same tile increments occurrence_count instead of pushing a new record.
// DecayAll() halves every count and drops records that hit zero, so transient
// failures don't permanently block tiles.
struct ConflictRecord {
    WFCGridCoord coord{};
    wfc_tile_id  tile{};
    u32          occurrence_count{0};
};

// RestartPolicy decides what to do when WFC propagation hits a contradiction.
//
// Phase A.2 strategy: no backtracking, only restart-or-giveup. On contradiction
// we either restart the solve from scratch (consuming a generation) or, after
// exhausting `max_generations` restarts, give up and let the caller decide on a
// fallback (e.g. seed-fill the unresolved cells).
//
// Phase C.1 Mixed (Task 12): each contradiction is recorded as a full
// ConflictRecord (coord + tile + occurrence_count) — BiasForCell and
// BiasForTileInCell (Task 13) read these to weight observer picks away from
// repeated failure sites. DecayAll fades the memory across restarts.
class RestartPolicy {
public:
    enum class Decision : u8 {
        Continue = 0, // unused in Phase A.2 (we never backtrack, only restart-or-giveup)
        Restart  = 1,
        GiveUp   = 2,
    };

    explicit RestartPolicy(u32 max_generations) : max_generations_(max_generations) {}

    // Called when propagation hits a contradiction. Records the conflict (deduped
    // by coord+tile), then decides: restart if at least one generation remains,
    // else give up.
    //
    // `current_generation` is the 0-based index of the generation that just
    // failed (0 == first attempt). After this returns Restart, the caller is
    // expected to bump generation and re-solve from scratch.
    Decision OnContradiction(WFCGridCoord coord, wfc_tile_id attempted_tile,
                             u32 current_generation);

    void Reset() { conflicts_.clear(); }

    // Number of distinct (coord, tile) conflict records stored. Calls to
    // OnContradiction with a previously-seen (coord, tile) do NOT grow this —
    // they increment the existing record's occurrence_count.
    u32 ConflictCount() const { return static_cast<u32>(conflicts_.size()); }

    // Phase C.1 Task 12: full records for observer biasing.
    const utl::vector<ConflictRecord>& ConflictRecords() const { return conflicts_; }

private:
    utl::vector<ConflictRecord> conflicts_;
    u32                         max_generations_;
};

} // namespace primal::graphics::wfc
