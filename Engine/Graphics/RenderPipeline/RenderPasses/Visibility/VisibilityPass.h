#pragma once

#include "Graphics/RenderGraph/RenderGraphDefinitions.h"

namespace primal::graphics::rendergraph {
    class RenderGraph;
}

namespace primal::graphics::Visibility {

using namespace rendergraph;

struct VisibilityPassData {
    RGResourceHandle visibilityBuffer; // R32_UINT (TriangleID) + Depth
};

/**
 * @brief Adds a Visibility Pass
 * @details Renders scene geometry to a Visibility Buffer (Triangle IDs)
 */
const VisibilityPassData& AddVisibilityPass(RenderGraph& graph);

} // namespace primal::graphics::Visibility
