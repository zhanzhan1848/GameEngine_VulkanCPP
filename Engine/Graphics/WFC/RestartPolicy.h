// Engine/Graphics/WFC/RestartPolicy.h
#pragma once

#include "../../Common/CommonHeaders.h"
#include "../../Utilities/Vector.h"
#include "WFCTypes.h"

namespace primal::graphics::wfc {

// RestartPolicy decides what to do when WFC propagation hits a contradiction.
//
// Phase A.2 uses a simple no-backtracking strategy: on contradiction we either
// restart the solve from scratch (consuming a generation) or, after exhausting
// `max_generations` restarts, give up and let the caller decide on a fallback
// (e.g. seed-fill the unresolved cells).
//
// Each contradiction is recorded as a coordinate for later biasing (Phase B
// will steer the next restart away from previously-failing cells). The full
// ConflictRecord (tile + generation) is intentionally omitted until a consumer
// needs it — see the project Core Principle "Simplicity First".
class RestartPolicy {
public:
    enum class Decision : u8 {
        Continue = 0, // unused in Phase A.2 (we never backtrack, only restart-or-giveup)
        Restart  = 1,
        GiveUp   = 2,
    };

    explicit RestartPolicy(u32 max_generations) : max_generations_(max_generations) {}

    // Called when propagation hits a contradiction. Records the conflict coord,
    // then decides: restart if at least one generation remains, else give up.
    //
    // `current_generation` is the 0-based index of the generation that just
    // failed (0 == first attempt). After this returns Restart, the caller is
    // expected to bump generation and re-solve from scratch.
    Decision OnContradiction(WFCGridCoord coord, wfc_tile_id attempted_tile,
                             u32 current_generation);

    void Reset() { conflicts_.clear(); }

    u32 ConflictCount() const { return static_cast<u32>(conflicts_.size()); }
    const utl::vector<WFCGridCoord>& ConflictCoords() const { return conflicts_; }

private:
    utl::vector<WFCGridCoord> conflicts_;   // coords only for Phase A.2 biasing
    u32                       max_generations_;
};

} // namespace primal::graphics::wfc
