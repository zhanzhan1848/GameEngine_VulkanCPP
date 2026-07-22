#pragma once

#include "CommonHeaders.h"
#include "Graphics/Volume/VolumeTypes.h"
#include "Graphics/RHI/Core/RHITypes.h"
#include "Graphics/RenderGraph/RenderGraphDefinitions.h"

namespace primal::graphics::rhi {
    class RHIDeviceBase;
}

namespace primal::graphics::rendergraph {
    class RenderGraph;
}

namespace primal::graphics::volume {

/// Per-frame inputs provided by the caller (StandardRenderPipeline).
struct VolumeInputs {
    rendergraph::RGResourceHandle gbuffer_depth;
    rendergraph::RGResourceHandle scene_color;
    rendergraph::RGResourceHandle shadow_map;
    rendergraph::RGResourceHandle sdf_cascade_0;
    rendergraph::RGResourceHandle sdf_cascade_1;
    rendergraph::RGResourceHandle sdf_cascade_2;
    VolumeCameraData camera_data;
    u32 width = 0;
    u32 height = 0;
};

/// Output from VolumePass — scatter + transmittance texture.
struct VolumeOutput {
    rendergraph::RGResourceHandle volume_scatter;   // RGBA16_Float: RGB=scatter, A=transmittance
    bool valid = false;
};

/**
 * @brief Self-contained Volume Rendering pass: Density Trace + Lighting Eval.
 *
 * Lifecycle:
 *   1. Initialize(device, width, height, params) -- once
 *   2. AddPass(graph, inputs)                      -- every frame
 *   3. Shutdown()                                   -- once
 *
 * Uses Split Trace pattern for Apple Silicon compatibility:
 *   Pass 1 (volume_density_trace): texture3D only, outputs density buffer
 *   Pass 2 (volume_lighting_eval): buffer + texture2D only, outputs scatter
 */
class VolumePass {
public:
    VolumePass() = default;
    ~VolumePass();

    bool Initialize(rhi::RHIDeviceBase* device, u32 render_width, u32 render_height,
                    const VolumeRuntimeParams& params = {});
    void Shutdown();

    VolumeOutput AddPass(rendergraph::RenderGraph& graph, const VolumeInputs& inputs);

    bool IsInitialized() const { return initialized_; }

private:
    void CreateDescriptorSetLayouts();
    void CreatePipelines();
    void CreateConstantBuffers();
    void CreateNoiseTexture();
    void CreatePersistentBuffers();

    bool              initialized_{ false };
    rhi::RHIDeviceBase* device_{ nullptr };
    u32               render_width_{ 0 };
    u32               render_height_{ 0 };
    VolumeRuntimeParams params_{};

    // Pipelines (2: density trace + lighting eval)
    rhi::PipelineHandle    density_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineHandle    lighting_pipeline_{ rhi::handles::INVALID_PIPELINE };

    // Pipeline layouts
    rhi::PipelineLayoutHandle density_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineLayoutHandle lighting_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };

    // Descriptor set layouts
    rhi::DescriptorSetLayoutHandle density_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetLayoutHandle lighting_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };

    // Descriptor sets (triple-buffered: 2 types x 3 frames)
    rhi::DescriptorSetHandle density_ds_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET
    };
    rhi::DescriptorSetHandle lighting_ds_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET
    };

    // Constant buffers (triple-buffered)
    rhi::ResourceHandle params_cb_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };

    // Persistent noise texture (generated once)
    rhi::ResourceHandle noise_texture_{ rhi::handles::INVALID_RESOURCE };

    // Persistent output buffers (triple-buffered)
    rhi::ResourceHandle density_accum_buffer_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };
    rhi::ResourceHandle transmittance_log_buffer_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };

    // Persistent output texture (triple-buffered)
    rhi::ResourceHandle scatter_texture_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };
};

} // namespace primal::graphics::volume
