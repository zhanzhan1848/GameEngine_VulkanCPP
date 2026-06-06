#include "ToneMappingPass.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/Utils/ShaderRegistry.h"

#if defined(ENABLE_WEBGPU) && ENABLE_WEBGPU
#include "Graphics/RHI/Platforms/Dawn/DawnDevice.h"
#endif
#include <fstream>
#include <sstream>

namespace primal::graphics::PostProcess {

using namespace rhi;

static PipelineHandle s_ToneMapPipeline = handles::INVALID_PIPELINE;
static PipelineLayoutHandle s_ToneMapLayout = handles::INVALID_PIPELINE_LAYOUT;
static DescriptorSetLayoutHandle s_ToneMapDSL = handles::INVALID_RESOURCE;
static SamplerHandle s_ToneMapSampler = handles::INVALID_SAMPLER;
static ResourceHandle s_DummyTexture = handles::INVALID_RESOURCE;
static ResourceHandle s_DummyTextureView = handles::INVALID_RESOURCE;
static ResourceHandle s_DummyAOTexture = handles::INVALID_RESOURCE;
static ResourceHandle s_DummyAOTextureView = handles::INVALID_RESOURCE;
static ResourceHandle s_DummySSGITexture = handles::INVALID_RESOURCE;
static ResourceHandle s_DummySSGITextureView = handles::INVALID_RESOURCE;

// Triple-buffered deferred destruction for descriptor sets.
static constexpr u32 MAX_FRAMES = 3;
static utl::vector<DescriptorSetHandle> s_DeferredDescriptorSetDestroys[MAX_FRAMES];

static void FlushDeferredDestroys(RHIDeviceBase& device, u32 fi) {
    for (auto& h : s_DeferredDescriptorSetDestroys[fi]) {
        device.DestroyDescriptorSet(h);
    }
    s_DeferredDescriptorSetDestroys[fi].clear();
}

static std::string LoadShaderSource(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return "";
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

static void EnsurePipeline(RHIDeviceBase& device) {
    if (s_ToneMapPipeline != handles::INVALID_PIPELINE) return;

    auto platform = device.GetPlatform();
    std::string shaderPath = utils::ShaderRegistry::GetShaderPath(platform, "ToneMapping");
    std::string shaderSource = LoadShaderSource(shaderPath);
    if (shaderSource.empty()) return;

    ShaderHandle vs = device.CreateShader(shaderSource.data(), shaderSource.size(), ShaderStage::Vertex, "tonemap_vs");
    ShaderHandle fs = device.CreateShader(shaderSource.data(), shaderSource.size(), ShaderStage::Pixel, "tonemap_fs");
    if (vs == handles::INVALID_SHADER || fs == handles::INVALID_SHADER) return;

    // DSL: binding 0 = scene texture, binding 1 = bloom texture, binding 2 = sampler, binding 3 = AO texture, binding 4 = SSGI texture
    DescriptorSetLayoutBinding bindings[5]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = DescriptorType::SampledImage;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = ShaderStage::Pixel;

    bindings[1].binding = 1;
    bindings[1].descriptorType = DescriptorType::SampledImage;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = ShaderStage::Pixel;

    bindings[2].binding = 2;
    bindings[2].descriptorType = DescriptorType::Sampler;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = ShaderStage::Pixel;

    bindings[3].binding = 3;
    bindings[3].descriptorType = DescriptorType::SampledImage;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags = ShaderStage::Pixel;

    bindings[4].binding = 4;
    bindings[4].descriptorType = DescriptorType::SampledImage;
    bindings[4].descriptorCount = 1;
    bindings[4].stageFlags = ShaderStage::Pixel;

    DescriptorSetLayoutDesc dslDesc;
    dslDesc.bindingCount = 5;
    dslDesc.bindings = bindings;
    s_ToneMapDSL = device.CreateDescriptorSetLayout(dslDesc);

    // Sampler
    SamplerDesc samplerDesc;
    samplerDesc.minFilter = FilterMode::Linear;
    samplerDesc.magFilter = FilterMode::Linear;
    samplerDesc.addressU = TextureAddressMode::Clamp;
    samplerDesc.addressV = TextureAddressMode::Clamp;
    samplerDesc.addressW = TextureAddressMode::Clamp;
    samplerDesc.comparisonFunc = ComparisonFunc::Never;
    s_ToneMapSampler = device.CreateSampler(samplerDesc);

    // Dummy 1x1 black texture for missing bloom
    TextureDesc dummyTexDesc;
    dummyTexDesc.size = {1, 1, 1};
    dummyTexDesc.format = DataFormat::RGBA8_UNorm;
    dummyTexDesc.type = TextureType::Texture2D;
    dummyTexDesc.mipLevels = 1;
    dummyTexDesc.usage = TextureUsage::ShaderResource;
    s_DummyTexture = device.CreateTexture(dummyTexDesc);
    if (s_DummyTexture != handles::INVALID_RESOURCE) {
        TextureViewDesc viewDesc;
        viewDesc.texture = s_DummyTexture;
        viewDesc.viewType = TextureType::Texture2D;
        viewDesc.format = DataFormat::RGBA8_UNorm;
        s_DummyTextureView = device.CreateTextureView(viewDesc);
    }

    // Pipeline layout
    PipelineLayoutDesc layoutDesc;
    layoutDesc.setLayoutCount = 1;
    layoutDesc.setLayouts = &s_ToneMapDSL;
    s_ToneMapLayout = device.CreatePipelineLayout(layoutDesc);

    // Dummy 1x1 white texture for AO (AO=1.0, no occlusion)
    TextureDesc dummyAODesc;
    dummyAODesc.size = {1, 1, 1};
    dummyAODesc.format = DataFormat::RGBA8_UNorm;
    dummyAODesc.type = TextureType::Texture2D;
    dummyAODesc.mipLevels = 1;
    dummyAODesc.usage = TextureUsage::ShaderResource;
    s_DummyAOTexture = device.CreateTexture(dummyAODesc);
    if (s_DummyAOTexture != handles::INVALID_RESOURCE) {
#if defined(ENABLE_WEBGPU) && ENABLE_WEBGPU
        unsigned char white[4] = {255, 255, 255, 255};
        auto* dawnDev = dynamic_cast<DawnDevice*>(&device);
        if (dawnDev) {
            dawnDev->UpdateTextureData(s_DummyAOTexture, white, 0, 0, 0, 1, 1, 1, 4, 0);
        }
#endif
        TextureViewDesc aoViewDesc;
        aoViewDesc.texture = s_DummyAOTexture;
        aoViewDesc.viewType = TextureType::Texture2D;
        aoViewDesc.format = DataFormat::RGBA8_UNorm;
        s_DummyAOTextureView = device.CreateTextureView(aoViewDesc);
    }

    // Dummy 1x1 black texture for SSGI (no indirect lighting)
    TextureDesc dummySSGIDesc;
    dummySSGIDesc.size = {1, 1, 1};
    dummySSGIDesc.format = DataFormat::RGBA8_UNorm;
    dummySSGIDesc.type = TextureType::Texture2D;
    dummySSGIDesc.mipLevels = 1;
    dummySSGIDesc.usage = TextureUsage::ShaderResource;
    s_DummySSGITexture = device.CreateTexture(dummySSGIDesc);
    if (s_DummySSGITexture != handles::INVALID_RESOURCE) {
        TextureViewDesc ssgiViewDesc;
        ssgiViewDesc.texture = s_DummySSGITexture;
        ssgiViewDesc.viewType = TextureType::Texture2D;
        ssgiViewDesc.format = DataFormat::RGBA8_UNorm;
        s_DummySSGITextureView = device.CreateTextureView(ssgiViewDesc);
    }

    // Pipeline
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

const ToneMappingPassData& AddToneMappingPass(RenderGraph& graph, RGResourceHandle inputHDR, RGResourceHandle bloomTexture, RGResourceHandle aoTexture, RGResourceHandle ssgiTexture, u32 frameIndex) {
    u32 fi = frameIndex % MAX_FRAMES;
    // Flush deferred destroys from 3 frames ago (safe: GPU has finished using them)
    FlushDeferredDestroys(graph.GetDevice(), fi);

    return graph.AddPass<ToneMappingPassData>("ToneMappingPass", RGPassType::Graphics, RGPassCategory::PostProcess,
        [&](ToneMappingPassData& data, RenderGraphBuilder& builder) {
            builder.Read(inputHDR, ResourceState::ShaderResource);
            if (bloomTexture != kInvalidRGResourceHandle) {
                builder.Read(bloomTexture, ResourceState::ShaderResource);
            }
            if (aoTexture != kInvalidRGResourceHandle) {
                builder.Read(aoTexture, ResourceState::ShaderResource);
            }
            if (ssgiTexture != kInvalidRGResourceHandle) {
                builder.Read(ssgiTexture, ResourceState::ShaderResource);
            }

            // Create output texture
            TextureDesc outputDesc;
            auto* inputRes = builder.GetGraph().GetResource(inputHDR);
            if (inputRes && inputRes->GetType() == RGResourceType::Texture) {
                const auto& desc = static_cast<RenderGraphTexture*>(inputRes)->GetDesc();
                outputDesc.size.x = desc.size.x;
                outputDesc.size.y = desc.size.y;
                outputDesc.size.z = 1;
                outputDesc.format = DataFormat::RGBA8_UNorm;
                outputDesc.type = TextureType::Texture2D;
                outputDesc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
            }

            data.output = builder.CreateTexture("ToneMapping_Output", outputDesc, ResourceState::RenderTarget);

            EnsurePipeline(builder.GetGraph().GetDevice());
        },
        [inputHDR, bloomTexture, aoTexture, ssgiTexture, fi](const ToneMappingPassData& data, RenderGraphContext& context) {
            if (s_ToneMapPipeline == handles::INVALID_PIPELINE) return;

            auto& device = context.graph->GetDevice();
            auto* cmd = context.cmdBuffer;

            // Get output texture
            auto* outputTex = context.graph->GetResource(data.output);
            if (!outputTex || outputTex->GetType() != RGResourceType::Texture) return;
            auto* texture = static_cast<RenderGraphTexture*>(outputTex);

            // Get input HDR texture physical handle
            auto* inputRes = context.graph->GetResource(inputHDR);
            ResourceHandle inputHandle = handles::INVALID_RESOURCE;
            if (inputRes) inputHandle = inputRes->GetPhysicalHandle();

            // Get bloom texture physical handle (or use dummy)
            ResourceHandle bloomHandle = s_DummyTextureView;
            if (bloomTexture != kInvalidRGResourceHandle) {
                auto* bloomRes = context.graph->GetResource(bloomTexture);
                if (bloomRes) bloomHandle = bloomRes->GetPhysicalHandle();
            }
            if (bloomHandle == handles::INVALID_RESOURCE) bloomHandle = s_DummyTextureView;

            // Use physical handles directly — updateDescriptorSetsImpl resolves
            // via DawnTexture::GetDefaultView(), no per-frame WGPU objects created.
            ResourceHandle inputView = inputHandle;

            ResourceHandle bloomViewHandle = bloomHandle;

            ResourceHandle aoViewHandle = s_DummyAOTextureView;
            if (aoTexture != kInvalidRGResourceHandle) {
                auto* aoRes = context.graph->GetResource(aoTexture);
                if (aoRes && aoRes->GetType() == RGResourceType::Texture) {
                    aoViewHandle = aoRes->GetPhysicalHandle();
                }
            }
            if (aoViewHandle == handles::INVALID_RESOURCE) aoViewHandle = s_DummyAOTextureView;

            ResourceHandle ssgiViewHandle = s_DummySSGITextureView;
            if (ssgiTexture != kInvalidRGResourceHandle) {
                auto* ssgiRes = context.graph->GetResource(ssgiTexture);
                if (ssgiRes && ssgiRes->GetType() == RGResourceType::Texture) {
                    ssgiViewHandle = ssgiRes->GetPhysicalHandle();
                }
            }
            if (ssgiViewHandle == handles::INVALID_RESOURCE) ssgiViewHandle = s_DummySSGITextureView;

            // Create descriptor set
            DescriptorSetDesc dsDesc;
            dsDesc.layout = s_ToneMapDSL;
            DescriptorSetHandle ds = device.CreateDescriptorSet(dsDesc);

            if (ds != handles::INVALID_RESOURCE && inputView != handles::INVALID_RESOURCE) {
                DescriptorImageInfo imageInfos[5];
                imageInfos[0].imageView = inputView;
                imageInfos[0].sampler = s_ToneMapSampler;
                imageInfos[1].imageView = bloomViewHandle;
                imageInfos[1].sampler = s_ToneMapSampler;
                imageInfos[2].imageView = bloomViewHandle;
                imageInfos[2].sampler = s_ToneMapSampler;
                imageInfos[3].imageView = aoViewHandle;
                imageInfos[3].sampler = s_ToneMapSampler;
                imageInfos[4].imageView = ssgiViewHandle;
                imageInfos[4].sampler = s_ToneMapSampler;

                WriteDescriptorSet writes[5];
                writes[0].dstSet = ds; writes[0].dstBinding = 0; writes[0].dstArrayElement = 0;
                writes[0].descriptorCount = 1; writes[0].descriptorType = DescriptorType::SampledImage;
                writes[0].imageInfo = &imageInfos[0];
                writes[1].dstSet = ds; writes[1].dstBinding = 1; writes[1].dstArrayElement = 0;
                writes[1].descriptorCount = 1; writes[1].descriptorType = DescriptorType::SampledImage;
                writes[1].imageInfo = &imageInfos[1];
                writes[2].dstSet = ds; writes[2].dstBinding = 2; writes[2].dstArrayElement = 0;
                writes[2].descriptorCount = 1; writes[2].descriptorType = DescriptorType::Sampler;
                writes[2].imageInfo = &imageInfos[2];
                writes[3].dstSet = ds; writes[3].dstBinding = 3; writes[3].dstArrayElement = 0;
                writes[3].descriptorCount = 1; writes[3].descriptorType = DescriptorType::SampledImage;
                writes[3].imageInfo = &imageInfos[3];
                writes[4].dstSet = ds; writes[4].dstBinding = 4; writes[4].dstArrayElement = 0;
                writes[4].descriptorCount = 1; writes[4].descriptorType = DescriptorType::SampledImage;
                writes[4].imageInfo = &imageInfos[4];

                device.UpdateDescriptorSets(5, writes);
            }

            // Begin render pass
            RenderPassDesc passDesc;
            passDesc.colorAttachments.resize(1);
            passDesc.colorAttachments[0].texture = texture->GetPhysicalHandle();
            passDesc.colorAttachments[0].loadOp = LoadAction::Clear;
            passDesc.colorAttachments[0].storeOp = StoreAction::Store;

            ViewportDesc viewport;
            viewport.size.x = texture->GetDesc().size.x;
            viewport.size.y = texture->GetDesc().size.y;
            viewport.minDepth = 0.0f;
            viewport.maxDepth = 1.0f;

            cmd->BeginRenderPass(passDesc);
            cmd->SetViewport(viewport);
            cmd->SetScissor({ { 0, 0 }, { (u32)viewport.size.x, (u32)viewport.size.y } });
            cmd->BindGraphicsPipeline(s_ToneMapPipeline);

            if (ds != handles::INVALID_RESOURCE) {
                cmd->BindDescriptorSets(PipelineBindPoint::Graphics, s_ToneMapLayout, 0, 1, &ds, 0, nullptr);
            }

            cmd->Draw(3, 0, 1, 0);
            cmd->EndRenderPass();

            // No per-frame texture views created — default views are reused via GetDefaultView().
            if (ds != handles::INVALID_RESOURCE) s_DeferredDescriptorSetDestroys[fi].push_back(ds);
        }
    );
}

} // namespace primal::graphics::PostProcess
