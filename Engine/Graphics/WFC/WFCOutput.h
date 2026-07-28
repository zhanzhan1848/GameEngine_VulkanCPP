// Engine/Graphics/WFC/WFCOutput.h
//
// Task 10 (Phase A.3): Bridge from WFC solver output to PCG point set.
//
// The WFC solver emits a stream of WFCStep records (Collapse / Propagate /
// Restart) into a WFCStepBuffer. WFCOutput::ConsumeSteps drains the buffer
// once per frame and converts the Collapse records into a PCGPointSet of
// tile instances ready for PCGEntityFactory consumption.
//
// Position encoding: world_pos = cell_coord * cell_size (uniform grid)
// MeshIndex attr:    tile.mesh_handle from registry
// Scale attrs:       1.0 (uniform)
// RotationY attr:    0.0 (Phase A.3 ignores variant rotation; Phase B can
//                     derive from variant later)
#pragma once

#include "../../Common/CommonHeaders.h"
#include "../PCG/PCGTypes.h"

namespace primal::graphics::wfc {

class WFCStepBuffer;
class WFCTileRegistry;

// Converts solver output (WFCStepBuffer of Collapse records) into a PCGPointSet
// of tile instances.
class WFCOutput {
public:
    // Drains `buf` and emits a PCGPointSet with one point per Collapse step.
    // Non-Collapse steps (Propagate / Restart) are skipped.
    // `cell_size` scales grid coords to world positions.
    static primal::graphics::pcg::PCGPointSet ConsumeSteps(
        WFCStepBuffer& buf,
        const WFCTileRegistry& registry,
        f32 cell_size);
};

} // namespace primal::graphics::wfc
