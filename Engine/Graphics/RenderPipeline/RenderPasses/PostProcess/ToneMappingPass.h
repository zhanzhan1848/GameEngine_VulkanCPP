#pragma once

#include "Graphics/RenderGraph/RenderGraphDefinitions.h"

namespace primal::graphics::rendergraph {
    class RenderGraph;
}

namespace primal::graphics::PostProcess {

using namespace rendergraph;

struct ToneMappingPassData {
    RGResourceHandle output;
};

/**
 * @brief Adds a Tone Mapping Pass
 * @details Applies tone mapping (ACES, Reinhard, etc.) and gamma correction. 
 *          Can optionally composite Bloom and other effects.
 */
const ToneMappingPassData& AddToneMappingPass(RenderGraph& graph, RGResourceHandle inputHDR, RGResourceHandle bloomTexture = {}, RGResourceHandle aoTexture = {}, RGResourceHandle ssgiTexture = {}, u32 frameIndex = 0);

} // namespace primal::graphics::PostProcess
