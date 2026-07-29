#pragma once

#include "../../Common/CommonHeaders.h"
#include "../PCG/PCGTypes.h"

namespace primal::graphics::wfc {

class WFCTileRegistry;
class WFCStepBuffer;

// Phase B.2: streaming drain result. Caller spawns entities from new_points,
// and (if restart_seen) destroys all previously-spawned entities first.
struct WFCStreamDrainResult {
    pcg::PCGPointSet new_points;
    bool             restart_seen{false};
    u32              restart_count{0};
};

class WFCOutput {
public:
    // Phase A.3: batch consume. Drains the buffer and returns one PCGPoint
    // per Collapse step. Used by one-shot solve-and-emit flows.
    static pcg::PCGPointSet ConsumeSteps(WFCStepBuffer& buf,
                                         const WFCTileRegistry& registry,
                                         f32 cell_size);

    // Phase B.2: streaming consume. Drains the buffer and returns only the
    // post-last-Restart Collapse points (prior Collapse steps within the
    // same snapshot are discarded). Caller uses restart_seen to decide
    // whether to destroy previously-spawned entities before appending.
    static WFCStreamDrainResult DrainStream(WFCStepBuffer& buf,
                                            const WFCTileRegistry& registry,
                                            f32 cell_size);
};

} // namespace primal::graphics::wfc
