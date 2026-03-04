#pragma once

#include "Graphics/RenderGraph/RenderGraphDefinitions.h"

namespace primal::graphics::rendergraph {
    class RenderGraph;
}

namespace primal::graphics {
    class RenderScene;
    class RenderView;
}

namespace primal::graphics::ForwardPass {

using namespace rendergraph;

/**
 * @brief Adds a Forward Rendering Pass to the Render Graph
 * @param graph The RenderGraph to add the pass to
 * @param scene The Scene to render
 * @param view The View to render from
 * @param renderTarget The Render Target to draw into
 * @return The handle to the output resource (usually same as renderTarget)
 */
RGResourceHandle AddPass(RenderGraph& graph, RenderScene& scene, RenderView& view, RGResourceHandle renderTarget);

} // namespace primal::graphics::ForwardPass
