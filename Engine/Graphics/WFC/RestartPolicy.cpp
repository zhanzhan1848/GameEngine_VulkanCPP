// Engine/Graphics/WFC/RestartPolicy.cpp
#include "RestartPolicy.h"

namespace primal::graphics::wfc {

RestartPolicy::Decision RestartPolicy::OnContradiction(WFCGridCoord coord,
                                                       wfc_tile_id attempted_tile,
                                                       u32 current_generation) {
    // Dedup by (coord, tile): re-failing the same cell with the same tile
    // strengthens the bias against it instead of pushing a new record. Different
    // tile at same cell → new record (observer needs to know both failed picks).
    bool found = false;
    for (auto& r : conflicts_) {
        if (r.coord.x == coord.x && r.coord.y == coord.y && r.coord.z == coord.z
            && r.tile == attempted_tile) {
            ++r.occurrence_count;
            found = true;
            break;
        }
    }
    if (!found) {
        conflicts_.push_back(ConflictRecord{coord, attempted_tile, /*count=*/1});
    }

    // current_generation+1 == number of generations consumed after this restart.
    // If that still leaves room below the cap, restart; otherwise surrender.
    return (current_generation + 1 < max_generations_)
        ? Decision::Restart
        : Decision::GiveUp;
}

} // namespace primal::graphics::wfc
