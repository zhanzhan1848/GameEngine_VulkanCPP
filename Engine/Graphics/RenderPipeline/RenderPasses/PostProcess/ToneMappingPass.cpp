#include "ToneMappingPass.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include <fstream>
#include <sstream>

namespace primal::graphics::PostProcess {

using namespace rhi;

static PipelineHandle s_ToneMapPipeline = handles::INVALID_PIPELINE;
static PipelineLayoutHandle s_ToneMapLayout = handles::INVALID_PIPELINE_LAYOUT;

static std::string LoadShaderSource(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return "";
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

const ToneMappingPassData& AddToneMappingPass(RenderGraph& graph, RGResourceHandle inputHDR, RGResourceHandle bloomTexture) {
    return graph.AddPass<ToneMappingPassData>("ToneMappingPass", RGPassType::Graphics, RGPassCategory::PostProcess,
        [&](ToneMappingPassData& data, RenderGraphBuilder& builder) {
            builder.Read(inputHDR, ResourceState::ShaderResource);
            if (bloomTexture != kInvalidRGResourceHandle) {
                builder.Read(bloomTexture, ResourceState::ShaderResource);
            }

            // Create Output (Backbuffer or Final Texture)
            // For now, create a new texture as output, assuming it will be blitted to swapchain later
            // OR we can make this pass write to the SwapChain directly if provided.
            // Let's create a texture.
            TextureDesc outputDesc;
            auto* inputRes = builder.GetGraph().GetResource(inputHDR);
            if (inputRes && inputRes->GetType() == RGResourceType::Texture) {
                const auto& desc = static_cast<RenderGraphTexture*>(inputRes)->GetDesc();
                outputDesc.size.x = desc.size.x;
                outputDesc.size.y = desc.size.y;
                outputDesc.size.z = 1;
                outputDesc.format = DataFormat::RGBA8_UNorm; // LDR
                outputDesc.type = TextureType::Texture2D;
                outputDesc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
            }

            data.output = builder.CreateTexture("ToneMapping_Output", outputDesc, ResourceState::RenderTarget);

            // Initialize Pipeline
            if (s_ToneMapPipeline == handles::INVALID_PIPELINE) {
                auto& device = builder.GetGraph().GetDevice();
                std::string shaderSource = LoadShaderSource("Engine/Graphics/Metal/shaders/ToneMapping.metal");
                
                if (!shaderSource.empty()) {
                    ShaderHandle vs = device.CreateShader(shaderSource.data(), shaderSource.size(), ShaderStage::Vertex, "tonemap_vs");
                    ShaderHandle fs = device.CreateShader(shaderSource.data(), shaderSource.size(), ShaderStage::Pixel, "tonemap_fs");
                    
                    if (vs != handles::INVALID_SHADER && fs != handles::INVALID_SHADER) {
                        PipelineLayoutDesc layoutDesc;
                        s_ToneMapLayout = device.CreatePipelineLayout(layoutDesc);

                        GraphicsPipelineDesc pipelineDesc;
                        pipelineDesc.vertexShader = vs;
                        pipelineDesc.pixelShader = fs;
                        pipelineDesc.layout = s_ToneMapLayout;
                        pipelineDesc.topology = PrimitiveTopology::TriangleList;
                        pipelineDesc.renderTargetCount = 1;
                        pipelineDesc.renderTargetFormats[0] = DataFormat::RGBA8_UNorm;
                        pipelineDesc.enableDepthTest = false;
                        pipelineDesc.enableDepthWrite = false;
                        pipelineDesc.cullMode = CullMode::None;

                        s_ToneMapPipeline = device.CreateGraphicsPipeline(pipelineDesc);
                    }
                }
            }
        },
        [&](const ToneMappingPassData& data, RenderGraphContext& context) {
            if (s_ToneMapPipeline == handles::INVALID_PIPELINE) return;
            auto* cmd = context.cmdBuffer;

            // Begin Render Pass
            auto* outputTex = context.graph->GetResource(data.output);
            if (!outputTex || outputTex->GetType() != RGResourceType::Texture) return;
            
            auto* texture = static_cast<RenderGraphTexture*>(outputTex);

            RenderPassDesc passDesc;
            passDesc.colorAttachments.resize(1);
            passDesc.colorAttachments[0].texture = texture->GetPhysicalHandle();
            passDesc.colorAttachments[0].loadOp = LoadAction::DontCare;
            passDesc.colorAttachments[0].storeOp = StoreAction::Store;
            
            // Viewport
            ViewportDesc viewport;
            viewport.size.x = texture->GetDesc().size.x;
            viewport.size.y = texture->GetDesc().size.y;
            viewport.minDepth = 0.0f;
            viewport.maxDepth = 1.0f;

            cmd->BeginRenderPass(passDesc);
            cmd->SetViewport(viewport);
            cmd->SetScissor({ { 0, 0 }, { (u32)viewport.size.x, (u32)viewport.size.y } });
            cmd->BindGraphicsPipeline(s_ToneMapPipeline);

            // Bind Textures
            // Texture 0: HDR Input
            // Texture 1: Bloom (Optional)
            // TODO: Bind Resources via Descriptors

            cmd->Draw(3, 0, 1, 0); // Full Screen Triangle
            cmd->EndRenderPass();
        }
    );
}

} // namespace primal::graphics::PostProcess
