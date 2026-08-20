#pragma once

#include "Graphics/RenderGraph/RenderGraphDefinitions.h"

namespace primal::graphics::rendergraph {
    class RenderGraph;
}

namespace primal::graphics::renderpass {

using namespace rendergraph;

struct ToonPassData {
    RGResourceHandle toonOutput;
};

struct ToonParams {
    float edgeThreshold = 0.01f;  // depth-delta sum above this = silhouette edge
    float colorLevels   = 6.0f;   // cel bands per channel
};

// Toon Shading / Cel Shading Post Process
// Color quantization + depth-based Sobel edge detection, compute dispatch.
// Input color should already be tone-mapped (LDR); output is RGBA8.
// width/height must be passed explicitly — legacy ImportResource handles
// don't carry a queryable Texture type.
const ToonPassData& AddToonPass(RenderGraph& graph, RGResourceHandle inputColor,
                                RGResourceHandle depth, RGResourceHandle normal,
                                const ToonParams& params, u32 bufferIndex,
                                u32 width, u32 height);

} // namespace primal::graphics::renderpass
