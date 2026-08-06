#pragma once

#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RenderGraph/RenderGraphDefinitions.h"

namespace primal::graphics {
namespace rendergraph { class RenderGraph; }

struct FusionInputs {
    // Scene color (deferred output or raw albedo)
    rendergraph::RGResourceHandle primary_input_rg{};
    rhi::ResourceHandle primary_input_tex{rhi::handles::INVALID_RESOURCE};

    // GI textures (may be invalid if feature disabled)
    rendergraph::RGResourceHandle ssgi_rg{};
    rhi::ResourceHandle ssgi_tex{rhi::handles::INVALID_RESOURCE};
    rendergraph::RGResourceHandle ddgi_rg{};
    rhi::ResourceHandle ddgi_tex{rhi::handles::INVALID_RESOURCE};
    rendergraph::RGResourceHandle spgi_rg{};
    rhi::ResourceHandle spgi_tex{rhi::handles::INVALID_RESOURCE};
    rendergraph::RGResourceHandle ssao_rg{};
    rhi::ResourceHandle ssao_tex{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle gbuffer_albedo{rhi::handles::INVALID_RESOURCE};

    // Volume scatter (RGBA16_Float: RGB=scatter, A=transmittance)
    rendergraph::RGResourceHandle volume_scatter_rg{};
    rhi::ResourceHandle volume_scatter_tex{rhi::handles::INVALID_RESOURCE};

    u32 current_buffer_index = 0;
    u32 render_width = 0;
    u32 render_height = 0;
    rhi::ResourceHandle black_texture{rhi::handles::INVALID_RESOURCE};
};

// T4.6.5 part 30.6 (X4 fix): default member initializers prevent stack garbage.
// Call sites that skip AddPasses (e.g. fusion_module_==nullptr OR Lumen disabled)
// leave FusionOutputs uninitialized → garbage output_tex=0 resolves via FreeList
// to slot 0 (first swapchain backbuffer) → FinalBlit samples a non-SAMPLED image.
struct FusionOutputs {
    rendergraph::RGResourceHandle output_rg{};
    rhi::ResourceHandle output_tex{rhi::handles::INVALID_RESOURCE};
};

class FusionCompositeModule {
public:
    bool Initialize(rhi::RHIDeviceBase* device,
                    rhi::ShaderHandle indirect_ps, rhi::ShaderHandle composite_vs,
                    rhi::ShaderHandle composite_ps,
                    u32 render_width, u32 render_height);
    void Shutdown();

    FusionOutputs AddPasses(rendergraph::RenderGraph& graph, const FusionInputs& inputs);

private:
    rhi::RHIDeviceBase* device_ = nullptr;
    u32 render_width_ = 0;
    u32 render_height_ = 0;

    // Pass 1: FusionIndirect (half-res, 5 textures)
    rhi::PipelineHandle indirect_pipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::PipelineLayoutHandle indirect_layout_{rhi::handles::INVALID_PIPELINE_LAYOUT};
    rhi::DescriptorSetLayoutHandle indirect_set_layout_{rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    rhi::DescriptorSetHandle indirect_ds_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET
    };
    rhi::ResourceHandle indirect_output_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };

    // Pass 2: FusionComposite (full-res, 2 textures)
    rhi::PipelineHandle composite_pipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::PipelineLayoutHandle composite_layout_{rhi::handles::INVALID_PIPELINE_LAYOUT};
    rhi::DescriptorSetLayoutHandle composite_set_layout_{rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    rhi::DescriptorSetHandle composite_ds_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET
    };
    rhi::ResourceHandle fusion_output_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };

    // Shared vertex shader (full-screen triangle)
    rhi::ShaderHandle vertex_shader_{rhi::handles::INVALID_SHADER};

    // 1x1 identity texture for volume scatter fallback: RGBA = (0,0,0,1)
    rhi::ResourceHandle volume_identity_tex_{rhi::handles::INVALID_RESOURCE};
};

} // namespace primal::graphics
