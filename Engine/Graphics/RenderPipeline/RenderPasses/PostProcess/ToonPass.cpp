#include "ToonPass.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"
#include "Graphics/RHI/Core/RHIDevice.h"

namespace primal::graphics::renderpass {

using namespace rendergraph;
using namespace rhi;

static PipelineHandle s_ToonPipeline = handles::INVALID_PIPELINE;

const ToonPassData& AddToonPass(RenderGraph& graph, RGResourceHandle inputColor, RGResourceHandle depth, RGResourceHandle normal) {
    if (s_ToonPipeline == handles::INVALID_PIPELINE) {
        // auto& device = graph.GetDevice();
        // ComputePipelineDesc desc;
        // desc.computeKernel = { "toon_main", "main" };
        // TODO: Create pipeline using device
        // s_ToonPipeline = device.CreateComputePipeline(desc);
    }

    return graph.AddPass<ToonPassData>("ToonPass", RGPassType::Compute, RGPassCategory::PostProcess,
        [&](ToonPassData& data, RenderGraphBuilder& builder) {
            builder.Read(inputColor, ResourceState::ShaderResource);
            builder.Read(depth, ResourceState::ShaderResource);
            builder.Read(normal, ResourceState::ShaderResource);
            
            TextureDesc outputDesc;
            outputDesc.size.x = 0; // Follows window size
            outputDesc.size.y = 0;
            outputDesc.size.z = 1;
            outputDesc.format = DataFormat::RGBA8_UNorm;
            outputDesc.type = TextureType::Texture2D;
            outputDesc.usage = TextureUsage::UnorderedAccess | TextureUsage::ShaderResource;

            // Get size from input
            auto* inputRes = builder.GetGraph().GetResource(inputColor);
            if (inputRes && inputRes->GetType() == RGResourceType::Texture) {
                const auto& desc = static_cast<RenderGraphTexture*>(inputRes)->GetDesc();
                outputDesc.size.x = desc.size.x;
                outputDesc.size.y = desc.size.y;
            }
            
            data.toonOutput = builder.CreateTexture("Toon_Output", outputDesc, ResourceState::UnorderedAccess);
        },
        [&](const ToonPassData& data, RenderGraphContext& context) {
            if (s_ToonPipeline != handles::INVALID_PIPELINE) {
                context.cmdBuffer->BindComputePipeline(s_ToonPipeline);
                // Bind resources: Input Color, Depth, Normal, Output
                // Dispatch
                // context.cmdBuffer->Dispatch(...);
            }
        }
    );
}

} // namespace primal::graphics::renderpass
