#pragma once

#include "CommonHeaders.h"
#include "Graphics/Volume/FroxelTypes.h"
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

struct FroxelInputs {
    rendergraph::RGResourceHandle gbuffer_depth;
    rendergraph::RGResourceHandle scene_color;
    rendergraph::RGResourceHandle shadow_map;
    rendergraph::RGResourceHandle sdf_cascade_0;
    rendergraph::RGResourceHandle sdf_cascade_1;
    rendergraph::RGResourceHandle sdf_cascade_2;
    VolumeCameraData camera_data;
    math::m4x4 shadow_matrix0;
    math::m4x4 shadow_matrix1;
    u32 width = 0;
    u32 height = 0;
};

struct FroxelOutput {
    rendergraph::RGResourceHandle volume_scatter;
    bool valid = false;
};

class FroxelFogPass {
public:
    FroxelFogPass() = default;
    ~FroxelFogPass();

    bool Initialize(rhi::RHIDeviceBase* device, u32 render_width, u32 render_height,
                    const FroxelGridConfig& config = {});
    void Shutdown();
    FroxelOutput AddPass(rendergraph::RenderGraph& graph, const FroxelInputs& inputs);
    bool IsInitialized() const { return initialized_; }

private:
    void CreateDescriptorSetLayouts();
    void CreatePipelines();
    void CreateConstantBuffers();
    void CreateNoiseTexture();
    void CreateFroxelTextures();
    void CreateScatterTextures();

    bool              initialized_{ false };
    rhi::RHIDeviceBase* device_{ nullptr };
    u32               render_width_{ 0 };
    u32               render_height_{ 0 };
    FroxelGridConfig  config_{};

    // Pipelines (3: density_inject, light_integrate, resolve)
    rhi::PipelineHandle density_inject_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineHandle light_integrate_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineHandle resolve_pipeline_{ rhi::handles::INVALID_PIPELINE };

    // Pipeline layouts
    rhi::PipelineLayoutHandle density_inject_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineLayoutHandle light_integrate_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineLayoutHandle resolve_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };

    // Descriptor set layouts
    rhi::DescriptorSetLayoutHandle density_inject_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetLayoutHandle light_integrate_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetLayoutHandle resolve_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };

    // Descriptor sets (triple-buffered: 3 types x 3 frames)
    rhi::DescriptorSetHandle density_inject_ds_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET
    };
    rhi::DescriptorSetHandle light_integrate_ds_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET
    };
    rhi::DescriptorSetHandle resolve_ds_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET
    };

    // Constant buffers (triple-buffered)
    rhi::ResourceHandle params_cb_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };

    // Persistent noise texture
    rhi::ResourceHandle noise_texture_{ rhi::handles::INVALID_RESOURCE };

    // Froxel 3D textures (triple-buffered)
    rhi::ResourceHandle froxel_density_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };
    rhi::ResourceHandle froxel_scatter_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };

    // Output scatter texture (triple-buffered, RGBA16_Float 2D, half-res)
    rhi::ResourceHandle scatter_texture_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };
};

} // namespace primal::graphics::volume
