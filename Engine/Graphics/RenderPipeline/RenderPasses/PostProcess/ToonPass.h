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

// Toon Shading / Cel Shading Post Process
// Usually involves Edge Detection (Sobel) and Color Quantization
const ToonPassData& AddToonPass(RenderGraph& graph, RGResourceHandle inputColor, RGResourceHandle depth, RGResourceHandle normal);

} // namespace primal::graphics::renderpass
