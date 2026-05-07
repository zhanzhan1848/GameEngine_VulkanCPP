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

/// SSAO (GTAO) quality / performance tuning parameters.
struct SSAOParams {
    float radius = 2.0f;                // Medium-scale geometric occlusion only
    float power = 1.0f;                 // Linear AO — let tone mapping handle contrast
    u32   direction_count = 6;
    u32   sample_count = 3;
    float filter_sigma_depth = 2.0f;    // Relaxed depth tolerance for smoother filtering
    float filter_sigma_normal = 32.0f;  // Relaxed normal tolerance to blur out grain
    u32   filter_kernel_radius = 2;     // 5x5 — stays within Apple Silicon texture read limits
};

/// Per-frame camera data that the caller must provide.
struct SSAOCameraData {
    math::m4x4 view_matrix;
    math::m4x4 proj_matrix;
    math::m4x4 prev_view_matrix;
    math::m4x4 prev_proj_matrix;
    u32        frame_index;
    float      delta_time;
};

/// Opaque result returned from AddPass.
struct LumenSSAOOutput {
    rendergraph::RGResourceHandle ssao_output;   ///< Full-res R16_Float filtered AO
};

/**
 * @brief Self-contained SSAO pass: Trace (half-res) -> Filter (full-res).
 *
 * Lifecycle:
 *   1. Initialize(device, width, height, params)   -- once
 *   2. AddPass(graph, ..., camera_data, ...)        -- every frame
 *   3. Shutdown()                                    -- once
 *
 * The pass owns all persistent GPU resources (pipelines, descriptor sets,
 * constant buffers, textures).
 */
class LumenSSAOPass {
public:
    LumenSSAOPass() = default;
    ~LumenSSAOPass();

    bool Initialize(rhi::RHIDeviceBase* device, u32 render_width, u32 render_height,
                    const SSAOParams& params = {});
    void Shutdown();

    LumenSSAOOutput AddPass(
        rendergraph::RenderGraph& graph,
        rendergraph::RGResourceHandle gbuffer_normal,
        rendergraph::RGResourceHandle gbuffer_depth,
        const SSAOCameraData& camera_data,
        u32 current_frame_index);

    bool IsInitialized() const { return initialized_; }

    /// Get the filter output texture for binding in downstream passes.
    rhi::ResourceHandle GetFilterTexture() const { return filter_texture_; }

private:
    void CreateDescriptorSetLayouts();
    void CreatePipelines();
    void CreateConstantBuffers();
    void CreatePersistentTextures();

    bool              initialized_{ false };
    rhi::RHIDeviceBase* device_{ nullptr };
    u32               render_width_{ 0 };
    u32               render_height_{ 0 };
    SSAOParams        params_{};

    // Pipelines
    rhi::PipelineHandle    trace_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineHandle    filter_pipeline_{ rhi::handles::INVALID_PIPELINE };

    // Pipeline layouts
    rhi::PipelineLayoutHandle trace_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineLayoutHandle filter_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };

    // Descriptor set layouts
    rhi::DescriptorSetLayoutHandle trace_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetLayoutHandle filter_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };

    // Descriptor sets (triple-buffered)
    rhi::DescriptorSetHandle trace_ds_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET
    };
    rhi::DescriptorSetHandle filter_ds_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET
    };

    // Constant buffers (triple-buffered)
    rhi::ResourceHandle global_cb_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };
    rhi::ResourceHandle trace_params_cb_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };
    rhi::ResourceHandle filter_params_cb_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };

    // Persistent output textures
    rhi::ResourceHandle trace_texture_{ rhi::handles::INVALID_RESOURCE };       // Half-res R16_Float
    rhi::ResourceHandle filter_texture_{ rhi::handles::INVALID_RESOURCE };      // Full-res R16_Float
};

} // namespace primal::graphics::lumen
