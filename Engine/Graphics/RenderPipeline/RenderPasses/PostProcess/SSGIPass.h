#pragma once

// DEPRECATED: This SSGI implementation is being replaced by Lumen SSGI.
// See Engine/Graphics/Lumen/SSGI/ for the new implementation.
// This file will be removed once Lumen SSGI Phase 1 is complete.

#include "Graphics/RenderGraph/RenderGraphDefinitions.h"

namespace primal::graphics::rendergraph {
    class RenderGraph;
}

namespace primal::graphics::PostProcess {

using namespace rendergraph;

struct SSGIPassData {
    RGResourceHandle ssgiOutput;
};

/**
 * @brief Adds a Screen Space Global Illumination (SSGI) Pass
 */
const SSGIPassData& AddSSGIPass(RenderGraph& graph, RGResourceHandle normalDepth, RGResourceHandle albedo, RGResourceHandle gpass);

} // namespace primal::graphics::PostProcess
