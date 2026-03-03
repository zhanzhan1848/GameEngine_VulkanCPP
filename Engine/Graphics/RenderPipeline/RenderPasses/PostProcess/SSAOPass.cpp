#include "SSAOPass.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHIShaderCommon.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include <fstream>
#include <sstream>

namespace primal::graphics::PostProcess {

using namespace rhi;

// Static resources for the pass
static PipelineHandle s_SSAOPipeline = handles::INVALID_PIPELINE;
static PipelineLayoutHandle s_SSAOLayout = handles::INVALID_PIPELINE_LAYOUT;

// Helper to load shader
static std::string LoadShaderSource(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return "";
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

const SSAOPassData& AddSSAOPass(RenderGraph& graph, RGResourceHandle normalDepth, RGResourceHandle albedo) {
    return graph.AddPass<SSAOPassData>("SSAOPass", RGPassType::Compute, RGPassCategory::Lighting,
        [&](SSAOPassData& data, RenderGraphBuilder& builder) {
            // Setup Resources
            builder.Read(normalDepth, ResourceState::ShaderResource);
            builder.Read(albedo, ResourceState::ShaderResource);

            // Create Output Texture
            TextureDesc ssaoDesc;
            auto* ndRes = builder.GetGraph().GetResource(normalDepth);
            if (ndRes && ndRes->GetType() == RGResourceType::Texture) {
                const auto& ndDesc = static_cast<RenderGraphTexture*>(ndRes)->GetDesc();
                ssaoDesc.size.x = ndDesc.size.x;
                ssaoDesc.size.y = ndDesc.size.y;
                ssaoDesc.size.z = 1;
                ssaoDesc.format = DataFormat::R8_UNorm; // SSAO is single channel
                ssaoDesc.type = TextureType::Texture2D;
                ssaoDesc.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
            }

            data.ssaoOutput = builder.CreateTexture("SSAO_Output", ssaoDesc, ResourceState::UnorderedAccess);

            // Initialize Pipeline if needed
            if (s_SSAOPipeline == handles::INVALID_PIPELINE) {
                auto& device = builder.GetGraph().GetDevice();
                
                // Load Shader
                std::string shaderSource = LoadShaderSource("Engine/Graphics/Metal/shaders/SSAOShader.metal");
                if (!shaderSource.empty()) {
                    ShaderHandle computeShader = device.CreateShader(shaderSource.data(), shaderSource.size(), ShaderStage::Compute, "ssao_pass");
                    
                    if (computeShader != handles::INVALID_SHADER) {
                        // Create Layout
                        PipelineLayoutDesc layoutDesc;
                        // TODO: Define layout based on shader reflection or manual definition
                        // For now, assuming empty layout works or default
                        s_SSAOLayout = device.CreatePipelineLayout(layoutDesc);

                        // Create Pipeline
                        ComputePipelineDesc pipelineDesc;
                        pipelineDesc.computeShader = computeShader;
                        pipelineDesc.layout = s_SSAOLayout;
                        pipelineDesc.threadGroupSize = {8, 8, 1}; // Standard 8x8 block

                        s_SSAOPipeline = device.CreateComputePipeline(pipelineDesc);
                    }
                }
            }
        },
        [&](const SSAOPassData& data, RenderGraphContext& context) {
            if (s_SSAOPipeline == handles::INVALID_PIPELINE) return;

            auto* cmd = context.cmdBuffer;
            
            cmd->BindComputePipeline(s_SSAOPipeline);

            // Bind Resources
            // Texture 0: Normal/Depth
            // Texture 1: Albedo
            // Texture 2: SSAO Output (UAV)
            auto* ndTex = context.graph->GetResource(normalDepth);
            auto* albedoTex = context.graph->GetResource(albedo);
            auto* ssaoTex = context.graph->GetResource(data.ssaoOutput);

            if (ndTex && albedoTex && ssaoTex) {
                // TODO: Bind textures. The RHICommand interface for binding textures needs to be verified.
                // Assuming BindDescriptorSets or similar.
                // Since we don't have a descriptor set management here yet, we might need direct binding if supported.
                // Or we create a transient descriptor set.
            }

            // Dispatch
            // uint3 dispatchSize = (width + 7) / 8, (height + 7) / 8, 1
            if (ssaoTex && ssaoTex->GetType() == RGResourceType::Texture) {
                const auto& desc = static_cast<RenderGraphTexture*>(ssaoTex)->GetDesc();
                u32 groupX = (desc.size.x + 7) / 8;
                u32 groupY = (desc.size.y + 7) / 8;
                cmd->Dispatch(groupX, groupY, 1);
            }
        }
    );
}

} // namespace primal::graphics::PostProcess
