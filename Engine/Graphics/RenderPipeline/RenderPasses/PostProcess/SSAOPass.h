#pragma once

#include "Graphics/RenderGraph/RenderGraphDefinitions.h"

namespace primal::graphics::rendergraph {
    class RenderGraph;
}

namespace primal::graphics {

using namespace rendergraph;

struct SSAOPassData {
    RGResourceHandle ssaoOutput;
};

namespace PostProcess {

const SSAOPassData& AddSSAOPass(RenderGraph& graph, RGResourceHandle depthTexture,
                                  u32 width, u32 height, u32 frameIndex,
                                  const math::m4x4& projMatrix, const math::m4x4& invProjMatrix);

void ShutdownSSAOPass();

} // namespace PostProcess
} // namespace primal::graphics
