#include "SSGIPass.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHIShaderCommon.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include <fstream>
#include <sstream>

// DEPRECATED: This SSGI implementation is being replaced by Lumen SSGI.
// See Engine/Graphics/Lumen/SSGI/ for the new implementation.
// This file will be removed once Lumen SSGI Phase 1 is complete.

namespace primal::graphics::PostProcess {

using namespace rhi;

static PipelineHandle s_SSGIPipeline = handles::INVALID_PIPELINE;
static PipelineLayoutHandle s_SSGILayout = handles::INVALID_PIPELINE_LAYOUT;

static std::string LoadShaderSource(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return "";
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

const SSGIPassData& AddSSGIPass(RenderGraph& graph, RGResourceHandle normalDepth, RGResourceHandle albedo, RGResourceHandle gpass) {
    return graph.AddPass<SSGIPassData>("SSGIPass", RGPassType::Compute, RGPassCategory::Lighting,
        [&](SSGIPassData& data, RenderGraphBuilder& builder) {
            builder.Read(normalDepth, ResourceState::ShaderResource);
            builder.Read(albedo, ResourceState::ShaderResource);
            if (gpass.IsValid()) builder.Read(gpass, ResourceState::ShaderResource);

            TextureDesc ssgiDesc;
            auto* ndRes = builder.GetGraph().GetResource(normalDepth);
            if (ndRes && ndRes->GetType() == RGResourceType::Texture) {
                const auto& ndDesc = static_cast<RenderGraphTexture*>(ndRes)->GetDesc();
                ssgiDesc.size.x = ndDesc.size.x;
                ssgiDesc.size.y = ndDesc.size.y;
                ssgiDesc.size.z = 1;
                ssgiDesc.format = DataFormat::RGBA8_UNorm; // SSGI usually needs color + confidence
                ssgiDesc.type = TextureType::Texture2D;
                ssgiDesc.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
            }

            data.ssgiOutput = builder.CreateTexture("SSGI_Output", ssgiDesc, ResourceState::UnorderedAccess);

            if (s_SSGIPipeline == handles::INVALID_PIPELINE) {
                auto& device = builder.GetGraph().GetDevice();
                std::string shaderSource = LoadShaderSource("Engine/Graphics/Metal/shaders/SSGIShader.metal");
                
                if (!shaderSource.empty()) {
                    ShaderHandle computeShader = device.CreateShader(shaderSource.data(), shaderSource.size(), ShaderStage::Compute, "ssgi_pass");
                    if (computeShader != handles::INVALID_SHADER) {
                        PipelineLayoutDesc layoutDesc;
                        s_SSGILayout = device.CreatePipelineLayout(layoutDesc);

                        ComputePipelineDesc pipelineDesc;
                        pipelineDesc.computeShader = computeShader;
                        pipelineDesc.layout = s_SSGILayout;
                        pipelineDesc.threadGroupSize = {8, 8, 1};
                        s_SSGIPipeline = device.CreateComputePipeline(pipelineDesc);
                    }
                }
            }
        },
        [&](const SSGIPassData& data, RenderGraphContext& context) {
            if (s_SSGIPipeline == handles::INVALID_PIPELINE) return;
            auto* cmd = context.cmdBuffer;
            cmd->BindComputePipeline(s_SSGIPipeline);

            // Bind Resources
            // Texture 0: Normal/Depth
            // Texture 1: Albedo
            // Texture 2: GPass (Extra)
            // Texture 3: SSGI Output (UAV)
            // Buffer 0: GlobalData
            // Buffer 1: Lights
            
            auto* ssgiTex = context.graph->GetResource(data.ssgiOutput);
            if (ssgiTex && ssgiTex->GetType() == RGResourceType::Texture) {
                const auto& desc = static_cast<RenderGraphTexture*>(ssgiTex)->GetDesc();
                u32 groupX = (desc.size.x + 7) / 8;
                u32 groupY = (desc.size.y + 7) / 8;
                cmd->Dispatch(groupX, groupY, 1);
            }
        }
    );
}

} // namespace primal::graphics::PostProcess
