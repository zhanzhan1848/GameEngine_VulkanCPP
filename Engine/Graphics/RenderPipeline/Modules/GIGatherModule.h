#pragma once

#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RenderGraph/RenderGraphDefinitions.h"

namespace primal::graphics {
namespace rendergraph { class RenderGraph; }

namespace lumen { class LumenDDGIPass; class StaticProbeVolume; }

struct GIGatherInputs {
    lumen::LumenDDGIPass* ddgi_pass = nullptr;
    lumen::LumenDDGIPass* ddgi_far_pass = nullptr;  // T4.4 far-field fallback
    lumen::StaticProbeVolume* static_probe_volume = nullptr;

    rhi::ResourceHandle gbuffer_depth;
    rhi::ResourceHandle gbuffer_normal;
    rhi::ResourceHandle gbuffer_orm;      // T4.3: roughness for lobe widening
    rendergraph::RGResourceHandle gbuffer_depth_rg;
    rendergraph::RGResourceHandle gbuffer_normal_rg;
    rendergraph::RGResourceHandle gbuffer_orm_rg;

    math::m4x4 view_matrix{};
    math::m4x4 proj_matrix{};
    u32 current_buffer_index = 0;
    rhi::ResourceHandle fallback_texture{rhi::handles::INVALID_RESOURCE};  // when ORM missing
};

struct GIGatherOutputs {
    rendergraph::RGResourceHandle gi_output_rg;
    rhi::ResourceHandle gi_output_tex;
    // T4.3: half-res glossy specular (RGB = SH(R) lobe radiance, A = NdotV).
    rendergraph::RGResourceHandle gi_specular_rg;
    rhi::ResourceHandle gi_specular_tex;
};

class GIGatherModule {
public:
    bool Initialize(rhi::RHIDeviceBase* device, rhi::ShaderHandle shader,
                    u32 render_width, u32 render_height);
    void Shutdown();

    GIGatherOutputs AddPasses(rendergraph::RenderGraph& graph, const GIGatherInputs& inputs);

    rhi::ResourceHandle GetOutputTexture() const { return gi_halfres_texture_; }
    bool IsInitialized() const { return pipeline_ != rhi::handles::INVALID_PIPELINE; }

private:
    rhi::RHIDeviceBase* device_ = nullptr;
    u32 render_width_ = 0;
    u32 render_height_ = 0;

    rhi::PipelineHandle pipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::PipelineLayoutHandle layout_{rhi::handles::INVALID_PIPELINE_LAYOUT};
    rhi::DescriptorSetLayoutHandle set_layout_{rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    rhi::DescriptorSetHandle descriptor_set_{rhi::handles::INVALID_DESCRIPTOR_SET};

    rhi::ResourceHandle gi_halfres_texture_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle gi_halfres_history_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle gi_specular_texture_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle probe_cb_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle far_cb_{rhi::handles::INVALID_RESOURCE};      // T4.4 far volume params
    rhi::ResourceHandle static_probe_cb_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle dummy_buffer_{rhi::handles::INVALID_RESOURCE};
};

} // namespace primal::graphics
