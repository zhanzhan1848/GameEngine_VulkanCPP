#pragma once

#include "Graphics/RenderGraph/RenderGraphDefinitions.h"

namespace primal::graphics::rendergraph {
    class RenderGraph;
}

namespace primal::graphics {

using namespace rendergraph;

struct LumenSSGIData {
    RGResourceHandle ssgiOutput;
};

namespace PostProcess {

const LumenSSGIData& AddLumenSSGIPass(RenderGraph& graph,
    RGResourceHandle depthTexture,
    RGResourceHandle hzbTexture,
    RGResourceHandle velocityTexture,
    RGResourceHandle prevFrameColor,
    u32 width, u32 height, u32 frameIndex,
    const math::m4x4& proj, const math::m4x4& invProj);

void ShutdownLumenSSGIPass();

} // namespace PostProcess
} // namespace primal::graphics
