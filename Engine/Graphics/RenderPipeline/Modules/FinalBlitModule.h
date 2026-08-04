#pragma once

#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RenderGraph/RenderGraphDefinitions.h"

namespace primal::graphics {
namespace rendergraph { class RenderGraph; }

struct FinalBlitInputs {
    rendergraph::RGResourceHandle input_rg;
    rhi::ResourceHandle input_tex;
    rendergraph::RGResourceHandle backbuffer_rg;
    u32 current_buffer_index = 0;
    u32 render_width = 0;
    u32 render_height = 0;
};

class FinalBlitModule {
public:
    bool Initialize(rhi::RHIDeviceBase* device,
                    rhi::ShaderHandle vertex_shader, rhi::ShaderHandle pixel_shader);
    void Shutdown();

    void AddPass(rendergraph::RenderGraph& graph, const FinalBlitInputs& inputs);

    rhi::PipelineHandle GetPipeline() const { return pipeline_; }
    rhi::PipelineLayoutHandle GetPipelineLayout() const { return layout_; }
    rhi::DescriptorSetHandle GetDescriptorSet(u32 idx) const { return idx < 3 ? descriptor_sets_[idx] : rhi::handles::INVALID_DESCRIPTOR_SET; }

private:
    rhi::RHIDeviceBase* device_ = nullptr;

    rhi::PipelineHandle pipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::PipelineLayoutHandle layout_{rhi::handles::INVALID_PIPELINE_LAYOUT};
    rhi::DescriptorSetLayoutHandle set_layout_{rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    rhi::DescriptorSetHandle descriptor_sets_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET
    };
    // T4.6.5 part 24.10: separate Sampler binding for Vulkan — Metal has
    // implicit default samplers, Vulkan requires an explicit descriptor.
    rhi::SamplerHandle default_sampler_{rhi::handles::INVALID_SAMPLER};
};

} // namespace primal::graphics
