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

f32 RestartPolicy::BiasForCell(WFCGridCoord coord) const {
    f32 sum = 0.0f;
    for (const auto& r : conflicts_) {
        if (r.coord.x == coord.x && r.coord.y == coord.y && r.coord.z == coord.z) {
            sum += static_cast<f32>(r.occurrence_count);
        }
    }
    return sum;
}

f32 RestartPolicy::BiasForTileInCell(WFCGridCoord coord, wfc_tile_id tile) const {
    for (const auto& r : conflicts_) {
        if (r.coord.x == coord.x && r.coord.y == coord.y && r.coord.z == coord.z
            && r.tile == tile) {
            return static_cast<f32>(r.occurrence_count);
        }
    }
    return 0.0f;
}

void RestartPolicy::DecayAll() {
    for (auto& r : conflicts_) {
        r.occurrence_count /= 2u;
    }
    // Drop records that have decayed to zero — they no longer influence bias.
    // utl::vector::erase takes a single element pointer (not an iterator pair),
    // so we compact in-place: walk once, keep records with count > 0.
    u32 write = 0;
    for (u32 read = 0; read < conflicts_.size(); ++read) {
        if (conflicts_[read].occurrence_count != 0) {
            if (write != read) {
                conflicts_[write] = conflicts_[read];
            }
            ++write;
        }
    }
    while (conflicts_.size() > write) {
        conflicts_.erase(conflicts_.size() - 1);
    }
}

} // namespace primal::graphics::wfc
