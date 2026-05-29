#pragma once

#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RenderGraph/RenderGraphDefinitions.h"

namespace primal::graphics {
namespace rendergraph { class RenderGraph; }

namespace lumen { class LumenDDGIPass; class SurfaceCachePass; }

struct SCDDGIInputs {
    lumen::LumenDDGIPass* ddgi_pass = nullptr;
    lumen::SurfaceCachePass* surface_cache_pass = nullptr;

    u32 current_buffer_index = 0;
    rendergraph::RGResourceHandle sc_lighting_rg;  // Dependency on SC lighting output
};

class SCDDGIIntegrationModule {
public:
    bool Initialize(rhi::RHIDeviceBase* device,
                    rhi::ShaderHandle card_radiance_shader,
                    rhi::ShaderHandle probe_irradiance_shader,
                    u32 render_width, u32 render_height);
    void Shutdown();

    void AddPass(rendergraph::RenderGraph& graph, const SCDDGIInputs& inputs);

private:
    rhi::RHIDeviceBase* device_ = nullptr;

    rhi::PipelineHandle card_radiance_pipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::PipelineLayoutHandle card_radiance_layout_{rhi::handles::INVALID_PIPELINE_LAYOUT};
    rhi::DescriptorSetLayoutHandle card_radiance_set_layout_{rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    rhi::DescriptorSetHandle card_radiance_ds_{rhi::handles::INVALID_DESCRIPTOR_SET};

    rhi::PipelineHandle probe_irradiance_pipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::PipelineLayoutHandle probe_irradiance_layout_{rhi::handles::INVALID_PIPELINE_LAYOUT};
    rhi::DescriptorSetLayoutHandle probe_irradiance_set_layout_{rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    rhi::DescriptorSetHandle probe_irradiance_ds_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET
    };
};

} // namespace primal::graphics
