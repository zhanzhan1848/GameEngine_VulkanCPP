#pragma once

#include "CommonHeaders.h"
#include "Graphics/RHI/Core/RHITypes.h"
#include "Graphics/RenderGraph/RenderGraphDefinitions.h"

namespace primal::graphics::rhi {
    class RHIDeviceBase;
}

namespace primal::graphics::rendergraph {
    class RenderGraph;
}

namespace primal::graphics::lumen {

/// SSGI quality / performance tuning parameters.
struct SSGIParams {
    u32   ray_count = 4;
    float radius = 2.0f;
    float thickness = 0.25f;
    float temporal_feedback = 0.93f;
    float filter_sigma_depth = 10.0f;
    float filter_sigma_normal = 16.0f;
    float filter_sigma_hit_dist = 8.0f;
    float filter_sigma_spatial = 2.5f;
    u32   filter_kernel_radius = 2;
};

/// Per-frame camera data that the caller must provide.
struct SSGICameraData {
    math::m4x4 view_matrix;
    math::m4x4 proj_matrix;
    math::m4x4 prev_view_matrix;
    math::m4x4 prev_proj_matrix;
    u32        frame_index;
    float      delta_time;
};

/// Opaque result returned from AddPass — callers hand this to downstream passes.
struct LumenSSGIOutput {
    rendergraph::RGResourceHandle ssgi_output;   ///< Final filtered RGBA16F texture (full-res)
};

/**
 * @brief Self-contained SSGI pass: Trace -> Temporal -> Filter.
 *
 * Lifecycle:
 *   1. Initialize(device, width, height, params)   -- once
 *   2. AddPass(graph, ..., camera_data, ...)        -- every frame
 *   3. Shutdown()                                    -- once
 *
 * The pass owns all persistent GPU resources (pipelines, descriptor sets,
 * constant buffers, temporal history textures).
 */
class LumenSSGIPass {
public:
    LumenSSGIPass() = default;
    ~LumenSSGIPass();

    bool Initialize(rhi::RHIDeviceBase* device, u32 render_width, u32 render_height,
                    const SSGIParams& params = {});
    void Shutdown();

    LumenSSGIOutput AddPass(
        rendergraph::RenderGraph& graph,
        rendergraph::RGResourceHandle gbuffer_normal,
        rendergraph::RGResourceHandle gbuffer_depth,
        rendergraph::RGResourceHandle gbuffer_velocity,
        rendergraph::RGResourceHandle hzb_texture,
        rendergraph::RGResourceHandle prev_frame_color,
        const SSGICameraData& camera_data,
        u32 current_frame_index,
        u32 hzb_mip_levels);

    bool IsInitialized() const { return initialized_; }

private:
    void CreateDescriptorSetLayouts();
    void CreatePipelines();
    void CreateConstantBuffers();
    void CreatePersistentTextures();

    bool              initialized_{ false };
    rhi::RHIDeviceBase* device_{ nullptr };
    u32               render_width_{ 0 };
    u32               render_height_{ 0 };
    SSGIParams        params_{};

    // Pipelines
    rhi::PipelineHandle    trace_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineHandle    temporal_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineHandle    filter_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineHandle    halfres_denoise_pipeline_{ rhi::handles::INVALID_PIPELINE };

    // Pipeline layouts
    rhi::PipelineLayoutHandle trace_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineLayoutHandle temporal_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineLayoutHandle filter_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineLayoutHandle halfres_denoise_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };

    // Descriptor set layouts
    rhi::DescriptorSetLayoutHandle trace_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetLayoutHandle temporal_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetLayoutHandle filter_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetLayoutHandle halfres_denoise_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };

    // Descriptor sets (triple-buffered)
    rhi::DescriptorSetHandle trace_ds_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET
    };
    rhi::DescriptorSetHandle temporal_ds_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET
    };
    rhi::DescriptorSetHandle filter_ds_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET
    };
    rhi::DescriptorSetHandle halfres_denoise_ds_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET
    };

    // Constant buffers (triple-buffered)
    rhi::ResourceHandle global_cb_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };
    rhi::ResourceHandle params_cb_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };
    rhi::ResourceHandle temporal_params_cb_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };
    rhi::ResourceHandle filter_params_cb_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };
    rhi::ResourceHandle halfres_denoise_cb_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };

    // Persistent output textures
    rhi::ResourceHandle trace_texture_{ rhi::handles::INVALID_RESOURCE };       // Half-res RGBA16F
    rhi::ResourceHandle trace_denoised_texture_{ rhi::handles::INVALID_RESOURCE }; // Half-res denoised RGBA16F
    rhi::ResourceHandle temporal_textures_[3]{                                   // Triple-buffered full-res
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };
    rhi::ResourceHandle filter_texture_{ rhi::handles::INVALID_RESOURCE };      // Full-res RGBA16F
};

} // namespace primal::graphics::lumen
