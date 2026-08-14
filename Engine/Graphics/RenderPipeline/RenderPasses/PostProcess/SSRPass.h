#pragma once

#include "Graphics/RenderGraph/RenderGraphDefinitions.h"
#include "Graphics/RHI/Core/RHIMath.h"

namespace primal::graphics::rendergraph {
    class RenderGraph;
}

namespace primal::graphics {

using namespace rendergraph;

namespace PostProcess {

struct SSRPassData {
    RGResourceHandle outputColor;   // HDR with reflections composited in
};

/**
 * @brief Adds a Screen-Space Reflections pass.
 * @details Three sub-passes internally:
 *            1. Trace  (half-res, Hi-Z ray march for reflection ray)
 *            2. Temporal (half-res, velocity-based reprojection + YCoCg clip)
 *            3. Composite (full-res, Fresnel-weighted additive blend into HDR)
 *
 * @param hdrTexture      TAA-resolved HDR (read for reflection source + composite base)
 * @param depthTexture    Full-res depth (read in trace + composite)
 * @param hzbTexture      Hierarchical Z buffer (read in trace)
 * @param velocityTexture Per-pixel motion vectors (read in temporal)
 * @param width,height    Full-resolution dimensions
 * @param frameIndex      Selects triple-buffered history slot
 * @param proj            Current frame projection matrix
 * @param invProj         Inverse projection matrix
 */
const SSRPassData& AddSSRPass(RenderGraph& graph,
                              RGResourceHandle hdrTexture,
                              RGResourceHandle depthTexture,
                              RGResourceHandle hzbTexture,
                              RGResourceHandle velocityTexture,
                              u32 width, u32 height, u32 frameIndex,
                              const math::m4x4& proj, const math::m4x4& invProj);

} // namespace PostProcess
} // namespace primal::graphics
