#pragma once

#include "Graphics/RenderGraph/RenderGraphDefinitions.h"

namespace primal::graphics::rendergraph {
    class RenderGraph;
}

namespace primal::graphics::PostProcess {

using namespace rendergraph;

struct SSAOPassData {
    RGResourceHandle ssaoOutput;
};

/**
 * @brief Adds a Screen Space Ambient Occlusion (SSAO/SSDO) Pass
 * @details Uses Compute Shader to calculate occlusion based on Depth and Normal
 */
const SSAOPassData& AddSSAOPass(RenderGraph& graph, RGResourceHandle normalDepth, RGResourceHandle albedo);

} // namespace primal::graphics::PostProcess
