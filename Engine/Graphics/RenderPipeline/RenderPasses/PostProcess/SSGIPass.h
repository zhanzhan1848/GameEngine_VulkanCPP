#pragma once

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
