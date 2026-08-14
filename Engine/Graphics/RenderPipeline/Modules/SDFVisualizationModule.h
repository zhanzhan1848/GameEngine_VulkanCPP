#pragma once

#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RenderGraph/RenderGraphDefinitions.h"

namespace primal::graphics {
namespace rendergraph { class RenderGraph; }

/// Inputs for the SDF visualization pass.
struct SDFVisualizationInputs {
    rendergraph::RGResourceHandle backbuffer_rg;
    u32 current_buffer_index = 0;
    u32 render_width = 0;
    u32 render_height = 0;

    math::v4 camera_position{ 0, 0, 0, 0 };
    math::m4x4 view_matrix{};
    math::m4x4 proj_matrix{};
    math::v4 light_direction{ 0, -1, 0, 0 };
    f32  light_intensity = 3.0f;

    // --- Offline SDF data source (optional) ---
    // When use_offline_sdf is true, the module uses the provided texture/metadata
    // instead of GlobalSDF cascades. All 3 cascade slots are bound to this single
    // texture with identical metadata.
    bool use_offline_sdf = false;
    rhi::ResourceHandle offline_sdf_texture{ rhi::handles::INVALID_RESOURCE };
    math::v3 offline_sdf_origin{ 0, 0, 0 };
    math::v3 offline_sdf_extent{ 0, 0, 0 };
    u32 offline_sdf_resolution = 0;
};

/// Two-pass GlobalSDF visualization:
///   Pass 1: compute shader sphere-traces SDF cascades → RGBA8 StorageImage.
///   Pass 2: graphics blit upscales the image to the backbuffer.
///
/// Uses compute shader (not fragment) for the ray march because the DDGI
/// tracer's texture3D + texelFetch pattern is proven safe on MoltenVK in
/// compute context, whereas the same pattern in a fragment shader caused
/// GPU device-loss.
class SDFVisualizationModule {
public:
    bool Initialize(rhi::RHIDeviceBase* device,
                    rhi::ShaderHandle blit_vertex_shader,
                    rhi::ShaderHandle sdf_viz_pixel_shader);
    void Shutdown();

    void AddPass(rendergraph::RenderGraph& graph, const SDFVisualizationInputs& inputs);

    bool IsInitialized() const { return compute_pipeline_ != rhi::handles::INVALID_PIPELINE; }
    bool HasOfflinePipeline() const { return offline_compute_pipeline_ != rhi::handles::INVALID_PIPELINE; }

private:
    rhi::RHIDeviceBase* device_ = nullptr;

    // Compute pipeline (SDF ray-march → StorageImage)
    rhi::PipelineHandle compute_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineHandle offline_compute_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineLayoutHandle compute_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::DescriptorSetLayoutHandle compute_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetHandle compute_sets_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET
    };

    // Blit pipeline (StorageImage → backbuffer)
    rhi::PipelineHandle blit_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineLayoutHandle blit_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::DescriptorSetLayoutHandle blit_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetHandle blit_sets_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET
    };
    rhi::SamplerHandle blit_sampler_{ rhi::handles::INVALID_SAMPLER };

    // Triple-buffered UBO
    rhi::ResourceHandle ubo_[3]{
        rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE
    };
};

} // namespace primal::graphics
