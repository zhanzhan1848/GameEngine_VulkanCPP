#pragma once

#include "Graphics/RenderGraph/RenderGraphDefinitions.h"

namespace primal::graphics::rendergraph {
    class RenderGraph;
}

namespace primal::graphics::renderpass {

using namespace rendergraph;

struct SSDOPassData {
    RGResourceHandle ssdoOutput;
};

// Screen Space Directional Occlusion
const SSDOPassData& AddSSDOPass(RenderGraph& graph, RGResourceHandle depth, RGResourceHandle normal, RGResourceHandle color);

} // namespace primal::graphics::renderpass
