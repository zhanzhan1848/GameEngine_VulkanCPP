#pragma once

#include "Graphics/RenderGraph/RenderGraphDefinitions.h"

namespace primal::graphics::rendergraph {
    class RenderGraph;
}

namespace primal::graphics {

using namespace rendergraph;

namespace PostProcess {

struct VelocityPassData {
    RGResourceHandle velocityTexture;
};

const VelocityPassData& AddVelocityPass(RenderGraph& graph, RGResourceHandle depthTexture,
                                         u32 width, u32 height, u32 frameIndex,
                                         const math::m4x4& viewProj,
                                         const math::m4x4& prevViewProj,
                                         const math::m4x4& invProj);

} // namespace PostProcess
} // namespace primal::graphics
