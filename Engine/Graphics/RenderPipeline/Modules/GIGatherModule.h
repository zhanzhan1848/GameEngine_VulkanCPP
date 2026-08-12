#pragma once

#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RenderGraph/RenderGraphDefinitions.h"

namespace primal::graphics {
namespace rendergraph { class RenderGraph; }

namespace lumen { class LumenDDGIPass; class StaticProbeVolume; }

struct GIGatherInputs {
    lumen::LumenDDGIPass* ddgi_pass = nullptr;
    lumen::StaticProbeVolume* static_probe_volume = nullptr;

    rhi::ResourceHandle gbuffer_depth;
    rhi::ResourceHandle gbuffer_normal;
    rendergraph::RGResourceHandle gbuffer_depth_rg;
    rendergraph::RGResourceHandle gbuffer_normal_rg;

    math::m4x4 view_matrix{};
    math::m4x4 proj_matrix{};
    u32 current_buffer_index = 0;
};

struct GIGatherOutputs {
    rendergraph::RGResourceHandle gi_output_rg;
    rhi::ResourceHandle gi_output_tex;
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
    rhi::ResourceHandle probe_cb_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle static_probe_cb_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle dummy_buffer_{rhi::handles::INVALID_RESOURCE};
};

} // namespace primal::graphics
