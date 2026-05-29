#include "SCDDGIIntegrationModule.h"
#include "Graphics/Lumen/DDGI/LumenDDGIPass.h"
#include "Graphics/Lumen/SurfaceCache/SurfaceCachePass.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"

namespace primal::graphics {

using namespace rhi;

bool SCDDGIIntegrationModule::Initialize(RHIDeviceBase* device,
                                           ShaderHandle card_radiance_shader,
                                           ShaderHandle probe_irradiance_shader,
                                           u32 render_width, u32 render_height) {
    device_ = device;

    // Card radiance averaging pipeline
    if (card_radiance_shader != handles::INVALID_SHADER) {
        // Minimal layout — actual bindings are determined by SurfaceCachePass
        // This module delegates the heavy lifting to SurfaceCachePass methods
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
            {1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
            {2, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},
        };
        card_radiance_set_layout_ = device->CreateDescriptorSetLayout({3, bindings});
        card_radiance_layout_ = device->CreatePipelineLayout({1, &card_radiance_set_layout_});

        ComputePipelineDesc pd{};
        pd.computeShader = card_radiance_shader;
        pd.layout = card_radiance_layout_;
        pd.threadGroupSize = {64, 1, 1};
        card_radiance_pipeline_ = device->CreateComputePipeline(pd);
        card_radiance_ds_ = device->CreateDescriptorSet({card_radiance_set_layout_});
    }

    // Probe irradiance from cards pipeline
    if (probe_irradiance_shader != handles::INVALID_SHADER) {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
            {1, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
            {2, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},
        };
        probe_irradiance_set_layout_ = device->CreateDescriptorSetLayout({3, bindings});
        probe_irradiance_layout_ = device->CreatePipelineLayout({1, &probe_irradiance_set_layout_});

        ComputePipelineDesc pd{};
        pd.computeShader = probe_irradiance_shader;
        pd.layout = probe_irradiance_layout_;
        pd.threadGroupSize = {64, 1, 1};
        probe_irradiance_pipeline_ = device->CreateComputePipeline(pd);

        for (int i = 0; i < 3; ++i)
            probe_irradiance_ds_[i] = device->CreateDescriptorSet({probe_irradiance_set_layout_});
    }

    return true;
}

void SCDDGIIntegrationModule::Shutdown() {}

void SCDDGIIntegrationModule::AddPass(rendergraph::RenderGraph& graph, const SCDDGIInputs& inputs) {
    if (!inputs.ddgi_pass || !inputs.surface_cache_pass) return;

    struct PassData {};

    // The SC→DDGI integration is a compute pass that reads SC lighting atlas
    // and writes DDGI probe irradiance. The actual descriptor binding and dispatch
    // is delegated to the SurfaceCachePass and DDGIPass methods which have full
    // context about buffer layouts and shader-specific bindings.
    graph.AddPass<PassData>("SC_DDGI_Integration",
        rendergraph::RGPassType::Compute,
        rendergraph::RGPassCategory::Lighting,
        [scRG = inputs.sc_lighting_rg](PassData&, rendergraph::RenderGraphBuilder& builder) {
            // Declare dependency on SC lighting output
            if (scRG.IsValid())
                builder.Read(scRG, ResourceState::ShaderResource);
            builder.SideEffect();
        },
        [inputs](const PassData&, rendergraph::RenderGraphContext& context) {
            auto cmd = context.cmdBuffer;
            u32 cbIdx = inputs.current_buffer_index % 3;

            // TODO: Implement SC→DDGI card radiance averaging + probe irradiance update
            // These will be delegated to SurfaceCachePass/DDGIPass methods once implemented.
            (void)cmd;
            (void)cbIdx;
        }
    );
}

} // namespace primal::graphics
