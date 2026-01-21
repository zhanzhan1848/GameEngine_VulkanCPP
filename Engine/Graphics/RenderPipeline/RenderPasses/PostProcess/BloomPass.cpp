#include "BloomPass.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/Passes/BlurPass.h"
#include <fstream>
#include <sstream>

namespace primal::graphics::PostProcess {

using namespace rhi;

// Bright Pass Resources
static PipelineHandle s_BrightPassPipeline = handles::INVALID_PIPELINE;
static PipelineLayoutHandle s_BrightPassLayout = handles::INVALID_PIPELINE_LAYOUT;

// Blur Pass Instance
static std::unique_ptr<BlurPass> s_BlurPass = nullptr;

static std::string LoadShaderSource(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return "";
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

struct BloomSetupData {
    RGResourceHandle brightTexture;
};

struct BloomBlurData : public BloomPassData {
    RGResourceHandle tempTexture;
};

const BloomPassData& AddBloomPass(RenderGraph& graph, RGResourceHandle inputColor) {
    // 1. Bright Pass
    auto& setupData = graph.AddPass<BloomSetupData>("BloomBrightPass", RGPassType::Graphics, RGPassCategory::PostProcess,
        [&](BloomSetupData& data, RenderGraphBuilder& builder) {
            builder.Read(inputColor, ResourceState::ShaderResource);

            TextureDesc brightDesc;
            auto* inputRes = builder.GetGraph().GetResource(inputColor);
            if (inputRes && inputRes->GetType() == RGResourceType::Texture) {
                const auto& desc = static_cast<RenderGraphTexture*>(inputRes)->GetDesc();
                brightDesc.size.x = desc.size.x / 2; // Downsample
                brightDesc.size.y = desc.size.y / 2;
                brightDesc.size.z = 1;
                brightDesc.format = desc.format; // Keep format or use R11G11B10
                brightDesc.type = TextureType::Texture2D;
                brightDesc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
            }
            data.brightTexture = builder.CreateTexture("Bloom_Bright", brightDesc, ResourceState::RenderTarget);

            // Init Pipeline
            if (s_BrightPassPipeline == handles::INVALID_PIPELINE) {
                auto& device = builder.GetGraph().GetDevice();
                std::string shaderSource = LoadShaderSource("Engine/Graphics/Metal/shaders/Bloom.metal");
                
                if (!shaderSource.empty()) {
                    ShaderHandle vs = device.CreateShader(shaderSource.data(), shaderSource.size(), ShaderStage::Vertex, "bloom_vs");
                    ShaderHandle fs = device.CreateShader(shaderSource.data(), shaderSource.size(), ShaderStage::Pixel, "bright_pass_fs");
                    
                    if (vs != handles::INVALID_SHADER && fs != handles::INVALID_SHADER) {
                        PipelineLayoutDesc layoutDesc;
                        s_BrightPassLayout = device.CreatePipelineLayout(layoutDesc);

                        GraphicsPipelineDesc pipelineDesc;
                        pipelineDesc.vertexShader = vs;
                        pipelineDesc.pixelShader = fs;
                        pipelineDesc.layout = s_BrightPassLayout;
                        pipelineDesc.renderTargetCount = 1;
                        pipelineDesc.renderTargetFormats[0] = brightDesc.format;
                        pipelineDesc.enableDepthTest = false;
                        s_BrightPassPipeline = device.CreateGraphicsPipeline(pipelineDesc);
                    }
                }
            }
        },
        [&](const BloomSetupData& data, RenderGraphContext& context) {
            if (s_BrightPassPipeline == handles::INVALID_PIPELINE) return;
            auto* cmd = context.cmdBuffer;

            auto* brightTex = context.graph->GetResource(data.brightTexture);
            if (!brightTex || brightTex->GetType() != RGResourceType::Texture) return;
            
            auto* texRes = static_cast<RenderGraphTexture*>(brightTex);

            RenderPassDesc passDesc;
            passDesc.colorAttachments.resize(1);
            passDesc.colorAttachments[0].texture = texRes->GetPhysicalHandle();
            passDesc.colorAttachments[0].loadOp = LoadAction::Clear;
            passDesc.colorAttachments[0].storeOp = StoreAction::Store;
            passDesc.colorAttachments[0].clearValue = {0,0,0,0};

            ViewportDesc viewport;
            viewport.size.x = (float)texRes->GetDesc().size.x;
            viewport.size.y = (float)texRes->GetDesc().size.y;
            viewport.minDepth = 0.0f; viewport.maxDepth = 1.0f;

            cmd->BeginRenderPass(passDesc);
            cmd->SetViewport(viewport);
            cmd->SetScissor({0, 0, (uint32_t)viewport.size.x, (uint32_t)viewport.size.y});
            cmd->BindGraphicsPipeline(s_BrightPassPipeline);
            // TODO: Bind inputColor
            cmd->Draw(3, 0, 1, 0);
            cmd->EndRenderPass();
        }
    );

    // 2. Blur Pass (Using existing BlurPass class)
    auto& blurData = graph.AddPass<BloomBlurData>("BloomBlurPass", RGPassType::Compute, RGPassCategory::PostProcess,
        [&](BloomBlurData& data, RenderGraphBuilder& builder) {
            builder.Read(setupData.brightTexture, ResourceState::ShaderResource);

            TextureDesc desc;
            auto* inputRes = builder.GetGraph().GetResource(setupData.brightTexture);
            if (inputRes && inputRes->GetType() == RGResourceType::Texture) {
                desc = static_cast<RenderGraphTexture*>(inputRes)->GetDesc();
            }
            
            desc.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;

            data.bloomOutput = builder.CreateTexture("Bloom_Output", desc, ResourceState::UnorderedAccess);
            data.tempTexture = builder.CreateTexture("Bloom_Temp", desc, ResourceState::UnorderedAccess);

            if (!s_BlurPass) {
                s_BlurPass = std::make_unique<BlurPass>();
                s_BlurPass->Initialize(&builder.GetGraph().GetDevice());
            }
        },
        [&](const BloomBlurData& data, RenderGraphContext& context) {
            if (!s_BlurPass) return;
            
            auto* inputRes = context.graph->GetResource(setupData.brightTexture);
            auto* outputRes = context.graph->GetResource(data.bloomOutput);
            auto* tempRes = context.graph->GetResource(data.tempTexture);

            if (inputRes && outputRes && tempRes && inputRes->GetType() == RGResourceType::Texture) {
                // Execute Blur
                // Using frame index 0 for simplicity, ideally from context
                uint32_t frameIndex = 0; // TODO: Get from context
                
                auto* texRes = static_cast<RenderGraphTexture*>(inputRes);

                s_BlurPass->Execute(context.cmdBuffer,
                                    inputRes->GetPhysicalHandle(),
                                    outputRes->GetPhysicalHandle(),
                                    tempRes->GetPhysicalHandle(),
                                    texRes->GetDesc().size.x,
                                    texRes->GetDesc().size.y,
                                    1, // layers
                                    frameIndex,
                                    5, 2.0f); // Radius 5, Sigma 2.0
            }
        }
    );

    return static_cast<const BloomPassData&>(blurData);
}

} // namespace primal::graphics::PostProcess
