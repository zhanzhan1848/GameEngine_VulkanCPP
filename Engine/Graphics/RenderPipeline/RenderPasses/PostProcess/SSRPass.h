#pragma once

#include "Graphics/RenderGraph/RenderGraphDefinitions.h"
#include "Graphics/RHI/Core/RHIMath.h"
#include "Graphics/RenderPipeline/PipelineQualityConfig.h"  // SSRConfig

namespace primal::graphics::rendergraph {
    class RenderGraph;
}

namespace primal::graphics {

using namespace rendergraph;

namespace PostProcess {

struct SSRPassData {
    RGResourceHandle outputColor;   // Full-res RGBA16F: RGB=reflection color, A=strength mask
};

/**
 * @brief Adds a Screen-Space Reflections pass.
 * @details Three sub-passes internally:
 *            1. Trace    (half-res, Hi-Z ray march for reflection ray)
 *            2. Temporal (half-res, velocity-based reprojection + YCoCg clip)
 *            3. Upsample (full-res, bilinear upsample + roughness attenuation)
 *
 *          The output is a pure reflection-color texture (NOT blended into HDR).
 *          FusionComposite reads it and additively blends into the scene.
 *
 * @param hdrTexture         TAA-resolved HDR (reflection radiance source)
 * @param depthTexture       Full-res depth (read in trace + upsample)
 * @param hzbTexture         Hierarchical Z buffer (read in trace)
 * @param velocityTexture    Per-pixel motion vectors (read in temporal)
 * @param gbufferOrmTexture  Full-res ORM: .g=roughness, .b=metallic (trace + upsample)
 * @param width,height       Full-resolution dimensions
 * @param frameIndex         Selects triple-buffered history slot
 * @param proj               Current frame projection matrix
 * @param invProj            Inverse projection matrix
 * @param params             SSR tuning (strength, max_roughness, etc.)
 */
const SSRPassData& AddSSRPass(RenderGraph& graph,
                              RGResourceHandle hdrTexture,
                              RGResourceHandle depthTexture,
                              RGResourceHandle hzbTexture,
                              RGResourceHandle velocityTexture,
                              RGResourceHandle gbufferOrmTexture,
                              u32 width, u32 height, u32 frameIndex,
                              const math::m4x4& proj, const math::m4x4& invProj,
                              const SSRConfig& params);

} // namespace PostProcess
} // namespace primal::graphics
