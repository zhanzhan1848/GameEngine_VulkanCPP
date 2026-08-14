#pragma once

#include "Graphics/RenderGraph/RenderGraphDefinitions.h"

namespace primal::graphics::rendergraph {
    class RenderGraph;
}

namespace primal::graphics {

using namespace rendergraph;

namespace PostProcess {

struct TAAPassData {
    RGResourceHandle output;
};

/**
 * @brief Adds a TAA (Temporal Anti-Aliasing) pass.
 * @details Resolves jittered HDR into clean HDR using velocity-based reprojection
 *          + YCoCg variance clip. Maintains triple-buffered history textures internally.
 *
 * @param inputHDR       Jittered HDR color from ForwardPass
 * @param velocityTexture Per-pixel motion vectors (MRT from ForwardPass)
 * @param width,height   Resolution
 * @param frameIndex     Advances Halton jitter sequence + selects history slot
 */
const TAAPassData& AddTAAPass(RenderGraph& graph, RGResourceHandle inputHDR,
                              RGResourceHandle velocityTexture,
                              u32 width, u32 height, u32 frameIndex);

// Marks all internal history slots as uninitialized so the next TAA pass treats
// the input as a fresh frame (no blend with stale HDR). Call on render-mode or
// scene transitions to prevent ghosting from the previous mode's image.
void ResetTAAHistory();

} // namespace PostProcess
} // namespace primal::graphics
