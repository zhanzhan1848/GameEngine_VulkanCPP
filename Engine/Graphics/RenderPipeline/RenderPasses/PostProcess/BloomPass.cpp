#include "BloomPass.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/Passes/BlurPass.h"
#include "Graphics/Utils/ShaderRegistry.h"
#include "Graphics/Dawn/ShaderLoader.h"
#include <fstream>
#include <sstream>
#include <iostream>

namespace primal::graphics::PostProcess {

using namespace rhi;

static PipelineHandle s_BrightPassPipeline = handles::INVALID_PIPELINE;
static PipelineLayoutHandle s_BrightPassLayout = handles::INVALID_PIPELINE_LAYOUT;
static DescriptorSetLayoutHandle s_BrightPassDSL = handles::INVALID_RESOURCE;
static SamplerHandle s_BrightPassSampler = handles::INVALID_SAMPLER;

static std::unique_ptr<BlurPass> s_BlurPass = nullptr;
static constexpr u32 MAX_FRAMES = 3;

// Pre-allocated descriptor set pool (matching SSAO/SSGI pattern — no per-frame allocation)
static constexpr u32 MAX_SETS = 2;
static DescriptorSetHandle s_BrightPassSets[MAX_FRAMES][MAX_SETS] = {};
static u32 s_BrightPassSetIdx[MAX_FRAMES] = {};

static std::string LoadShaderSource(const std::string& path) {
#ifdef __EMSCRIPTEN__
    auto lastSlash = path.find_last_of('/');
    auto lastDot = path.find_last_of('.');
    if (lastSlash != std::string::npos && lastDot != std::string::npos && lastDot > lastSlash) {
        return dawn::LoadWGSL(path.substr(lastSlash + 1, lastDot - lastSlash - 1).c_str());
    }
    return "";
#else
    std::ifstream file(path);
    if (!file.is_open()) return "";
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
#endif
}

struct BloomSetupData {
    RGResourceHandle brightTexture;
};

struct BloomBlurData : public BloomPassData {
    RGResourceHandle tempTexture;
};

const BloomPassData& AddBloomPass(RenderGraph& graph, RGResourceHandle inputColor, u32 frameIndex) {
    u32 fi = frameIndex % MAX_FRAMES;
    s_BrightPassSetIdx[fi] = 0;

    // 1. Bright Pass
    auto& setupData = graph.AddPass<BloomSetupData>("BloomBrightPass", RGPassType::Graphics, RGPassCategory::PostProcess,
        [&](BloomSetupData& data, RenderGraphBuilder& builder) {
            builder.Read(inputColor, ResourceState::ShaderResource);

            TextureDesc brightDesc;
            auto* inputRes = builder.GetGraph().GetResource(inputColor);
            if (inputRes && inputRes->GetType() == RGResourceType::Texture) {
                const auto& desc = static_cast<RenderGraphTexture*>(inputRes)->GetDesc();
                brightDesc.size.x = desc.size.x / 2;
                brightDesc.size.y = desc.size.y / 2;
                brightDesc.size.z = 1;
                brightDesc.format = desc.format;
                brightDesc.type = TextureType::Texture2D;
                brightDesc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
            }
            data.brightTexture = builder.CreateTexture("Bloom_Bright", brightDesc, ResourceState::RenderTarget);

            if (s_BrightPassPipeline == handles::INVALID_PIPELINE) {
                auto& device = builder.GetGraph().GetDevice();
                auto platform = device.GetPlatform();
                std::string shaderPath = utils::ShaderRegistry::GetShaderPath(platform, "Bloom");
                std::string shaderSource = LoadShaderSource(shaderPath);

                if (!shaderSource.empty()) {
                    ShaderHandle vs = device.CreateShader(shaderSource.data(), shaderSource.size(), ShaderStage::Vertex, "bloom_vs");
                    ShaderHandle fs = device.CreateShader(shaderSource.data(), shaderSource.size(), ShaderStage::Pixel, "bright_pass_fs");

                    if (vs != handles::INVALID_SHADER && fs != handles::INVALID_SHADER) {
                        // DSL: binding 0 = input HDR texture, binding 1 = sampler
                        DescriptorSetLayoutBinding bindings[2]{};
                        bindings[0].binding = 0;
                        bindings[0].descriptorType = DescriptorType::SampledImage;
                        bindings[0].descriptorCount = 1;
                        bindings[0].stageFlags = ShaderStage::Pixel;

                        bindings[1].binding = 1;
                        bindings[1].descriptorType = DescriptorType::Sampler;
                        bindings[1].descriptorCount = 1;
                        bindings[1].stageFlags = ShaderStage::Pixel;

                        DescriptorSetLayoutDesc dslDesc;
                        dslDesc.bindingCount = 2;
                        dslDesc.bindings = bindings;
                        s_BrightPassDSL = device.CreateDescriptorSetLayout(dslDesc);

                        // Sampler
                        SamplerDesc samplerDesc;
                        samplerDesc.minFilter = FilterMode::Linear;
                        samplerDesc.magFilter = FilterMode::Linear;
                        samplerDesc.addressU = TextureAddressMode::Clamp;
                        samplerDesc.addressV = TextureAddressMode::Clamp;
                        samplerDesc.addressW = TextureAddressMode::Clamp;
                        samplerDesc.comparisonFunc = ComparisonFunc::Never;
                        s_BrightPassSampler = device.CreateSampler(samplerDesc);

                        // Pipeline layout
                        PipelineLayoutDesc layoutDesc;
                        layoutDesc.setLayoutCount = 1;
                        layoutDesc.setLayouts = &s_BrightPassDSL;
                        s_BrightPassLayout = device.CreatePipelineLayout(layoutDesc);

                        GraphicsPipelineDesc pipelineDesc;
                        pipelineDesc.vertexShader = vs;
                        pipelineDesc.pixelShader = fs;
                        pipelineDesc.layout = s_BrightPassLayout;
                        pipelineDesc.topology = PrimitiveTopology::TriangleList;
                        pipelineDesc.renderTargetCount = 1;
                        pipelineDesc.renderTargetFormats[0] = brightDesc.format;
                        pipelineDesc.enableDepthTest = false;
                        pipelineDesc.enableDepthWrite = false;
                        pipelineDesc.cullMode = CullMode::None;
                        s_BrightPassPipeline = device.CreateGraphicsPipeline(pipelineDesc);

                        // Pre-allocate descriptor set pool
                        for (u32 f = 0; f < MAX_FRAMES; ++f)
                            for (u32 s = 0; s < MAX_SETS; ++s) {
                                DescriptorSetDesc dsDesc;
                                dsDesc.layout = s_BrightPassDSL;
                                s_BrightPassSets[f][s] = device.CreateDescriptorSet(dsDesc);
                            }
                    }
                }
            }
        },
        [inputColor, fi](const BloomSetupData& data, RenderGraphContext& context) {
            if (s_BrightPassPipeline == handles::INVALID_PIPELINE) return;
            auto& device = context.graph->GetDevice();
            auto* cmd = context.cmdBuffer;

            auto* brightTex = context.graph->GetResource(data.brightTexture);
            if (!brightTex || brightTex->GetType() != RGResourceType::Texture) return;

            auto* texRes = static_cast<RenderGraphTexture*>(brightTex);

            // Resolve input HDR texture
            auto* inputRes = context.graph->GetResource(inputColor);
            ResourceHandle inputHandle = handles::INVALID_RESOURCE;
            if (inputRes && inputRes->GetType() == RGResourceType::Texture) {
                inputHandle = inputRes->GetPhysicalHandle();
            }

            // Use physical handle directly — updateDescriptorSetsImpl resolves via GetDefaultView()
            ResourceHandle inputView = inputHandle;

            // Use pre-allocated descriptor set from pool
            u32 idx = s_BrightPassSetIdx[fi]++;
            if (idx >= MAX_SETS) { idx = 0; s_BrightPassSetIdx[fi] = 1; }
            DescriptorSetHandle ds = s_BrightPassSets[fi][idx];

            if (inputView != handles::INVALID_RESOURCE && ds != handles::INVALID_RESOURCE) {
                DescriptorImageInfo imageInfos[2];
                imageInfos[0].imageView = inputView;
                imageInfos[0].sampler = s_BrightPassSampler;
                imageInfos[1].imageView = inputView;
                imageInfos[1].sampler = s_BrightPassSampler;

                WriteDescriptorSet writes[2];
                writes[0].dstSet = ds; writes[0].dstBinding = 0; writes[0].dstArrayElement = 0;
                writes[0].descriptorCount = 1; writes[0].descriptorType = DescriptorType::SampledImage;
                writes[0].imageInfo = &imageInfos[0];
                writes[1].dstSet = ds; writes[1].dstBinding = 1; writes[1].dstArrayElement = 0;
                writes[1].descriptorCount = 1; writes[1].descriptorType = DescriptorType::Sampler;
                writes[1].imageInfo = &imageInfos[1];

                device.UpdateDescriptorSets(2, writes);
            }

            // Render
            RenderPassDesc passDesc;
            passDesc.colorAttachments.resize(1);
            passDesc.colorAttachments[0].texture = texRes->GetPhysicalHandle();
            passDesc.colorAttachments[0].loadOp = LoadAction::Clear;
            passDesc.colorAttachments[0].storeOp = StoreAction::Store;
            passDesc.colorAttachments[0].clearValue = ClearValue{ math::v4{ 0.0f, 0.0f, 0.0f, 0.0f } };

            ViewportDesc viewport;
            viewport.size.x = (float)texRes->GetDesc().size.x;
            viewport.size.y = (float)texRes->GetDesc().size.y;
            viewport.minDepth = 0.0f; viewport.maxDepth = 1.0f;

            cmd->BeginRenderPass(passDesc);
            cmd->SetViewport(viewport);
            cmd->SetScissor({ { 0, 0 }, { (u32)viewport.size.x, (u32)viewport.size.y } });
            cmd->BindGraphicsPipeline(s_BrightPassPipeline);

            if (ds != handles::INVALID_RESOURCE) {
                cmd->BindDescriptorSets(PipelineBindPoint::Graphics, s_BrightPassLayout, 0, 1, &ds, 0, nullptr);
            }

            cmd->Draw(3, 0, 1, 0);
            cmd->EndRenderPass();
        }
    );

    // 2. Blur Pass
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
        [&setupData, frameIndex](const BloomBlurData& data, RenderGraphContext& context) {
            if (!s_BlurPass) return;

            auto* inputRes = context.graph->GetResource(setupData.brightTexture);
            auto* outputRes = context.graph->GetResource(data.bloomOutput);
            auto* tempRes = context.graph->GetResource(data.tempTexture);

            if (inputRes && outputRes && tempRes && inputRes->GetType() == RGResourceType::Texture) {
                auto* texRes = static_cast<RenderGraphTexture*>(inputRes);

                s_BlurPass->Execute(context.cmdBuffer,
                                    inputRes->GetPhysicalHandle(),
                                    outputRes->GetPhysicalHandle(),
                                    tempRes->GetPhysicalHandle(),
                                    texRes->GetDesc().size.x,
                                    texRes->GetDesc().size.y,
                                    1,
                                    frameIndex,
                                    5, 2.0f);
            }
        }
    );

    return static_cast<const BloomPassData&>(blurData);
}

void ShutdownBloomPass() {
    if (s_BlurPass) {
        s_BlurPass->Shutdown();
        s_BlurPass.reset();
    }
    s_BrightPassPipeline = handles::INVALID_PIPELINE;
    s_BrightPassLayout = handles::INVALID_PIPELINE_LAYOUT;
    s_BrightPassDSL = handles::INVALID_RESOURCE;
    s_BrightPassSampler = handles::INVALID_SAMPLER;
}

} // namespace primal::graphics::PostProcess
