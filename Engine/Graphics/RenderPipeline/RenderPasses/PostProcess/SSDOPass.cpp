#include "SSDOPass.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"
#include "Graphics/RHI/Core/RHIDevice.h"

namespace primal::graphics::renderpass {

using namespace rendergraph;
using namespace rhi;

static PipelineHandle s_SSDOPipeline = handles::INVALID_PIPELINE;

const SSDOPassData& AddSSDOPass(RenderGraph& graph, RGResourceHandle depth, RGResourceHandle normal, RGResourceHandle color) {
    if (s_SSDOPipeline == handles::INVALID_PIPELINE) {
        // auto& device = graph.GetDevice();
        // ComputePipelineDesc desc;
        // desc.computeKernel = { "ssdo_main", "main" };
        // TODO: Create pipeline using device
        // s_SSDOPipeline = device.CreateComputePipeline(desc);
    }

    return graph.AddPass<SSDOPassData>("SSDOPass", RGPassType::Compute, RGPassCategory::Lighting,
        [&](SSDOPassData& data, RenderGraphBuilder& builder) {
            builder.Read(depth, ResourceState::ShaderResource);
            builder.Read(normal, ResourceState::ShaderResource);
            builder.Read(color, ResourceState::ShaderResource);
            
            TextureDesc ssdoDesc;
            ssdoDesc.size.x = 0; // TODO: Get size from input
            ssdoDesc.size.y = 0;
            ssdoDesc.size.z = 1;
            ssdoDesc.format = DataFormat::RGBA8_UNorm; // Indirect Light + Occlusion
            ssdoDesc.type = TextureType::Texture2D;
            ssdoDesc.usage = TextureUsage::UnorderedAccess | TextureUsage::ShaderResource;
            
            // Get size from input if possible
            auto* depthRes = builder.GetGraph().GetResource(depth);
            if (depthRes && depthRes->GetType() == RGResourceType::Texture) {
                const auto& desc = static_cast<RenderGraphTexture*>(depthRes)->GetDesc();
                ssdoDesc.size.x = desc.size.x;
                ssdoDesc.size.y = desc.size.y;
            }

            data.ssdoOutput = builder.CreateTexture("SSDO_Output", ssdoDesc, ResourceState::UnorderedAccess);
        },
        [&](const SSDOPassData& data, RenderGraphContext& context) {
            if (s_SSDOPipeline != handles::INVALID_PIPELINE) {
                context.cmdBuffer->BindComputePipeline(s_SSDOPipeline);
                // Bind resources
                // Dispatch
                // context.cmdBuffer->Dispatch(...);
            }
        }
    );
}

} // namespace primal::graphics::renderpass
