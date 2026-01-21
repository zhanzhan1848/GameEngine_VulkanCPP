#pragma once

#include "Graphics/RenderGraph/RenderGraphDefinitions.h"

namespace primal::graphics::rendergraph {
    class RenderGraph;
}

namespace primal::graphics::PostProcess {

using namespace rendergraph;

struct BloomPassData {
    RGResourceHandle bloomOutput;
};

/**
 * @brief Adds a Bloom Pass
 * @details Extracts bright areas, blurs them, and prepares for composition
 */
const BloomPassData& AddBloomPass(RenderGraph& graph, RGResourceHandle inputColor);

} // namespace primal::graphics::PostProcess
