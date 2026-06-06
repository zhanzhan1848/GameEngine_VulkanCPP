#pragma once

#include "Graphics/RenderGraph/RenderGraphDefinitions.h"

namespace primal::graphics::rendergraph {
    class RenderGraph;
}

namespace primal::graphics {
    class RenderScene;
    class RenderView;
    class ForwardRenderer;
    class MaterialInstance;
}

namespace primal::graphics::ForwardPass {

using namespace rendergraph;

struct ForwardPassOutput {
    RGResourceHandle hdrTexture;
    RGResourceHandle depthTexture;
};

/**
 * @brief Adds a Forward Rendering Pass to the Render Graph
 * @param graph The RenderGraph to add the pass to
 * @param scene The Scene to render
 * @param view The View to render from
 * @param renderer The ForwardRenderer to delegate drawing to
 * @param materials Map of material ID to MaterialInstance
 * @param frameIndex Current frame index for buffer rotation
 * @param width Render target width
 * @param height Render target height
 * @return Output struct containing HDR and depth texture handles
 */
const ForwardPassOutput& AddPass(
    RenderGraph& graph,
    RenderScene& scene,
    RenderView& view,
    ForwardRenderer& renderer,
    const std::unordered_map<id::id_type, std::shared_ptr<MaterialInstance>>& materials,
    u32 frameIndex,
    u32 width,
    u32 height);

} // namespace primal::graphics::ForwardPass
