#pragma once

#include "Graphics/RenderGraph/RenderGraphDefinitions.h"
#include <functional>

namespace primal::graphics::rendergraph {
    class RenderGraph;
}

namespace primal::graphics {

using namespace rendergraph;

namespace PostProcess {

struct TAAPassData {
    RGResourceHandle output;
};

/**
 * @brief Optional depth-history inputs for matrix reprojection + disocclusion.
 * @details When provided (and prevDepth valid), the TAA shader reprojects each
 *          pixel through currInvViewProj/prevViewProj instead of relying on the
 *          velocity buffer, and rejects history pixels whose reprojected depth
 *          disagrees with the previous frame's depth copy (DepthHistoryManager).
 */
struct TAADepthInputs {
    rhi::ResourceHandle currDepth{ rhi::handles::INVALID_RESOURCE }; // GBuffer depth (sampleable)
    rhi::ResourceHandle prevDepth{ rhi::handles::INVALID_RESOURCE }; // previous frame R32 depth copy
    math::m4x4 currInvViewProj{};
    math::m4x4 prevViewProj{};
    // Vulkan only: evaluated after the GBuffer producer executed. Never use
    // an expected Halton value or a getter evaluated while constructing RG.
    std::function<bool(math::v2&)> raster_jitter_provider;
    bool require_raster_jitter{false}; // explicit Vulkan contract, missing provider fails closed
    u32 previous_depth_frame{~u32(0)};
    RGResourceHandle current_depth_rg{}; // canonical GBuffer producer identity
};

/**
 * @brief Adds a TAA (Temporal Anti-Aliasing) pass.
 * @details Resolves jittered HDR into clean HDR using velocity-based reprojection
 *          + YCoCg variance clip. Maintains triple-buffered history textures internally.
 *
 * @param inputHDR       Jittered HDR color from ForwardPass
 * @param velocityTexture Per-pixel motion vectors (MRT from ForwardPass)
 * @param width,height   Resolution
 * @param frameIndex     Advances Halton jitter sequence + selects history slot
 * @param depth          Optional depth-history inputs (Vulkan/Metal only; Dawn
 *                       keeps the velocity-only shader and ignores this)
 */
const TAAPassData& AddTAAPass(RenderGraph& graph, RGResourceHandle inputHDR,
                              RGResourceHandle velocityTexture,
                              u32 width, u32 height, u32 frameIndex,
                              const TAADepthInputs* depth = nullptr);

// Marks all internal history slots as uninitialized so the next TAA pass treats
// the input as a fresh frame (no blend with stale HDR). Call on render-mode or
// scene transitions to prevent ghosting from the previous mode's image.
void ResetTAAHistory();

} // namespace PostProcess
} // namespace primal::graphics
