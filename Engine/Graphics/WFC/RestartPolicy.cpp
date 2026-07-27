// Engine/Graphics/WFC/RestartPolicy.cpp
#include "RestartPolicy.h"

namespace primal::graphics::wfc {

RestartPolicy::Decision RestartPolicy::OnContradiction(WFCGridCoord coord,
                                                       wfc_tile_id /*attempted_tile*/,
                                                       u32 current_generation) {
    conflicts_.push_back(coord);

    // current_generation+1 == number of generations consumed after this restart.
    // If that still leaves room below the cap, restart; otherwise surrender.
    return (current_generation + 1 < max_generations_)
        ? Decision::Restart
        : Decision::GiveUp;
}

} // namespace primal::graphics::wfc
