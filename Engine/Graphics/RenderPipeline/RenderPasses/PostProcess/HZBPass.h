#pragma once

#include "Graphics/RenderGraph/RenderGraphDefinitions.h"

namespace primal::graphics::rendergraph {
    class RenderGraph;
}

namespace primal::graphics::PostProcess {

using namespace rendergraph;

struct HZBPassData {
    RGResourceHandle hzbTexture;
    u32 mipLevels{0};
};

const HZBPassData& AddHZBPass(RenderGraph& graph, RGResourceHandle depthTexture,
                                u32 width, u32 height);

} // namespace primal::graphics::PostProcess
