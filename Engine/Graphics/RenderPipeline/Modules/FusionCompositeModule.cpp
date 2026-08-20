#include "FusionCompositeModule.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include <iostream>

namespace primal::graphics {

using namespace rhi;

struct DescData {
    u32 binding;
    DescriptorType type;
    ResourceHandle resource;
    u32 count = 1;
};

static void UpdateDesc(RHIDeviceBase* device, DescriptorSetHandle set, const DescData* params, u32 count) {
    std::vector<WriteDescriptorSet> writes(count);
    std::vector<DescriptorImageInfo> imageInfos(count);

    for (u32 i = 0; i < count; ++i) {
        writes[i].dstSet = set;
        writes[i].dstBinding = params[i].binding;
        writes[i].descriptorCount = params[i].count;
        writes[i].descriptorType = params[i].type;
        if (params[i].type == DescriptorType::Sampler) {
            imageInfos[i].sampler = static_cast<SamplerHandle>(params[i].resource);
        } else {
            imageInfos[i].imageView = params[i].resource;
            imageInfos[i].imageLayout = ResourceState::ShaderResource;
        }
        writes[i].imageInfo = &imageInfos[i];
    }
    device->UpdateDescriptorSets(count, writes.data());
}

bool FusionCompositeModule::Initialize(RHIDeviceBase* device,
                                         ShaderHandle indirect_ps, ShaderHandle composite_vs,
                                         ShaderHandle composite_ps,
                                         u32 render_width, u32 render_height) {
    device_ = device;
    render_width_ = render_width;
    render_height_ = render_height;
    vertex_shader_ = composite_vs;

    // Vulkan needs an explicit Sampler descriptor (Metal uses implicit samplers).
    // Create a linear-clamp sampler for the Fusion passes.
    const bool isVk = device->GetPlatform() == RHIPlatform::Vulkan;
    if (isVk) {
        SamplerDesc sDesc{};
        sDesc.minFilter = FilterMode::Linear;
        sDesc.magFilter = FilterMode::Linear;
        sDesc.mipFilter = FilterMode::Linear;
        sDesc.addressU = TextureAddressMode::Clamp;
        sDesc.addressV = TextureAddressMode::Clamp;
        sDesc.addressW = TextureAddressMode::Clamp;
        sDesc.comparisonFunc = ComparisonFunc::Never;
        default_sampler_ = device->CreateSampler(sDesc);
        if (default_sampler_ == handles::INVALID_SAMPLER) {
            std::cerr << "[FusionComposite] Failed to create default_sampler_" << std::endl;
        }
    }

    // --- Pass 1: FusionIndirect (half-res, 5 texture reads + 1 sampler on Vulkan) ---
    {
        const u32 indirectBindingCount = isVk ? 6 : 5;
        DescriptorSetLayoutBinding bindings[6];
        bindings[0] = {0, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr};  // SSGI
        bindings[1] = {1, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr};  // DDGI
        bindings[2] = {2, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr};  // SPGI
        bindings[3] = {3, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr};  // albedo
        bindings[4] = {4, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr};  // SSAO
        if (isVk) {
            bindings[5] = {5, DescriptorType::Sampler, 1, ShaderStage::Pixel, nullptr};   // sampler
        }
        indirect_set_layout_ = device->CreateDescriptorSetLayout({indirectBindingCount, bindings});
        indirect_layout_ = device->CreatePipelineLayout({1, &indirect_set_layout_});

        for (int i = 0; i < 3; ++i)
            indirect_ds_[i] = device->CreateDescriptorSet({indirect_set_layout_});

        // Half-res indirect output (RGBA16_Float)
        TextureDesc indDesc{};
        indDesc.size = {render_width / 2, render_height / 2, 1};
        indDesc.format = DataFormat::RGBA16_Float;
        indDesc.usage = TextureUsage::ShaderResource | TextureUsage::RenderTarget;
        for (int i = 0; i < 3; ++i)
            indirect_output_[i] = device->CreateTexture(indDesc);

        if (composite_vs != handles::INVALID_SHADER && indirect_ps != handles::INVALID_SHADER) {
            GraphicsPipelineDesc pd{};
            pd.layout = indirect_layout_;
            pd.vertexShader = composite_vs;
            pd.pixelShader = indirect_ps;
            pd.renderTargetFormats[0] = DataFormat::RGBA16_Float;
            pd.renderTargetCount = 1;
            pd.depthStencilFormat = DataFormat::Unknown;
            pd.enableDepthTest = false;
            pd.enableDepthWrite = false;
            pd.cullMode = CullMode::None;
            pd.vertexAttributes.clear();
            pd.vertexBindings.clear();
            indirect_pipeline_ = device->CreateGraphicsPipeline(pd);
        }
    }

    // --- Pass 2: FusionComposite (full-res, 5 texture reads + 1 sampler on Vulkan) ---
    {
        // bindings: 0=scene, 1=indirect, 2=volume_scatter, 3=ssr, 4=orm, [5=sampler(Vk)]
        const u32 compositeBindingCount = isVk ? 6 : 5;
        DescriptorSetLayoutBinding bindings[6];
        bindings[0] = {0, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr};  // scene
        bindings[1] = {1, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr};  // indirect
        bindings[2] = {2, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr};  // volume_scatter
        bindings[3] = {3, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr};  // ssr
        bindings[4] = {4, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr};  // orm
        if (isVk) {
            bindings[5] = {5, DescriptorType::Sampler, 1, ShaderStage::Pixel, nullptr};   // sampler
        }
        composite_set_layout_ = device->CreateDescriptorSetLayout({compositeBindingCount, bindings});
        composite_layout_ = device->CreatePipelineLayout({1, &composite_set_layout_});

        for (int i = 0; i < 3; ++i)
            composite_ds_[i] = device->CreateDescriptorSet({composite_set_layout_});

        // Full-res output (BGRA8_UNorm)
        TextureDesc outDesc{};
        outDesc.size = {render_width, render_height, 1};
        outDesc.format = DataFormat::RGBA16_Float;  // HDR — FinalBlit does tonemap
        outDesc.usage = TextureUsage::ShaderResource | TextureUsage::RenderTarget;
        for (int i = 0; i < 3; ++i)
            fusion_output_[i] = device->CreateTexture(outDesc);

        // Volume identity texture: 1x1 RGBA16_Float = (0,0,0,1)
        // transmittance=1 means scene passes through unchanged when no volume effect
        {
            TextureDesc desc{};
            desc.size = {1, 1, 1};
            desc.format = DataFormat::RGBA16_Float;
            desc.usage = TextureUsage::ShaderResource | TextureUsage::CopyDest;
            volume_identity_tex_ = device->CreateTexture(desc);

            if (volume_identity_tex_ != handles::INVALID_RESOURCE) {
                // Upload (0,0,0,1) via staging buffer + CopyBufferToTexture
                BufferDesc sbDesc{};
                sbDesc.size = 256; // Metal requires 256-byte row alignment
                sbDesc.usage = GPUMemoryUsage::Staging;
                sbDesc.memoryUsage = GPUMemoryUsage::Staging;
                auto staging = device->CreateBuffer(sbDesc);

                if (staging != handles::INVALID_RESOURCE) {
                    auto* mapped = static_cast<float*>(device->MapBuffer(staging));
                    if (mapped) {
                        memset(mapped, 0, 256);
                        // RGBA16_Float: each channel is 2 bytes (half float)
                        auto* halfData = reinterpret_cast<u16*>(mapped);
                        halfData[3] = 0x3C00; // A = 1.0 (half-float encoding)
                        device->UnmapBuffer(staging);

                        auto cmdHandle = device->CreateCommandBuffer(CommandQueueType::Graphics);
                        if (cmdHandle != handles::INVALID_COMMAND_BUFFER) {
                            auto* cmd = GetCommandBuffer(cmdHandle);
                            cmd->Begin();
                            BufferTextureCopyRegion region{};
                            region.bufferOffset = 0;
                            region.bufferRowLength = 256 / 4; // aligned row in pixels (RGBA16 = 4 bytes/pixel for 2 channels... actually 8 bytes for RGBA16)
                            region.imageSubresource = {0, 0, 1};
                            region.imageExtent = {1, 1, 1};
                            cmd->CopyBufferToTexture(staging, volume_identity_tex_, &region, 1);
                            cmd->End();
                            cmd->Submit();
                            cmd->WaitForCompletion();
                            device->DestroyCommandBuffer(cmdHandle);
                        }
                    }
                    device->DestroyBuffer(staging);
                }
            }
        }

        if (composite_vs != handles::INVALID_SHADER && composite_ps != handles::INVALID_SHADER) {
            GraphicsPipelineDesc pd{};
            pd.layout = composite_layout_;
            pd.vertexShader = composite_vs;
            pd.pixelShader = composite_ps;
            pd.renderTargetFormats[0] = DataFormat::RGBA16_Float;  // HDR — FinalBlit does tonemap
            pd.renderTargetCount = 1;
            pd.depthStencilFormat = DataFormat::Unknown;
            pd.enableDepthTest = false;
            pd.enableDepthWrite = false;
            pd.cullMode = CullMode::None;
            pd.vertexAttributes.clear();
            pd.vertexBindings.clear();
            composite_pipeline_ = device->CreateGraphicsPipeline(pd);
        }
    }

    return indirect_pipeline_ != handles::INVALID_PIPELINE && composite_pipeline_ != handles::INVALID_PIPELINE;
}

void FusionCompositeModule::Shutdown() {
    if (device_ && volume_identity_tex_ != handles::INVALID_RESOURCE) {
        device_->DestroyTexture(volume_identity_tex_);
        volume_identity_tex_ = handles::INVALID_RESOURCE;
    }
}

FusionOutputs FusionCompositeModule::AddPasses(rendergraph::RenderGraph& graph, const FusionInputs& inputs) {
    FusionOutputs outputs{};
    u32 cbIdx = inputs.current_buffer_index % 3;

    if (indirect_pipeline_ == handles::INVALID_PIPELINE || composite_pipeline_ == handles::INVALID_PIPELINE)
        return outputs;

    // Import textures into render graph
    auto indirectRG = graph.ImportResource("FusionIndirect", indirect_output_[cbIdx]);
    auto fusionOutRG = graph.ImportResource("FusionOutput", fusion_output_[cbIdx]);

    outputs.output_rg = fusionOutRG;
    outputs.output_tex = fusion_output_[cbIdx];

    // Resolve helper
    auto resolve = [&](rendergraph::RGResourceHandle h) -> ResourceHandle {
        if (!h.IsValid()) return inputs.black_texture;
        auto* res = graph.GetResource(h);
        return res ? res->GetPhysicalHandle() : inputs.black_texture;
    };

    // --- Pass 1: FusionIndirect (half-res) ---
    struct IndirectData {
        rendergraph::RGResourceHandle output;
    };

    graph.AddPass<IndirectData>("FusionIndirect",
        rendergraph::RGPassType::Graphics,
        rendergraph::RGPassCategory::PostProcess,
        [inputs, indirectRG](IndirectData& data, rendergraph::RenderGraphBuilder& builder) {
            builder.SideEffect(); // prevent RG culling
            if (inputs.ssgi_rg.IsValid()) builder.Read(inputs.ssgi_rg, ResourceState::ShaderResource);
            if (inputs.ddgi_rg.IsValid()) builder.Read(inputs.ddgi_rg, ResourceState::ShaderResource);
            if (inputs.spgi_rg.IsValid()) builder.Read(inputs.spgi_rg, ResourceState::ShaderResource);
            if (inputs.ssao_rg.IsValid()) builder.Read(inputs.ssao_rg, ResourceState::ShaderResource);
            data.output = builder.Write(indirectRG, ResourceState::RenderTarget);

            rendergraph::RGRenderPassDesc rpDesc;
            rpDesc.colors.push_back({.texture = data.output, .loadOp = LoadAction::DontCare, .storeOp = StoreAction::Store, .clearColor = {math::v4{0, 0, 0, 0}}});
            builder.DeclareRenderPass(rpDesc);
        },
        [this, inputs, cbIdx, resolve](const IndirectData&, rendergraph::RenderGraphContext& context) {
            auto cmd = context.cmdBuffer;
            static int s_fusion_diag = 0;
            if (s_fusion_diag < 3) { std::cerr << "[FUSION_INDIRECT] exec " << s_fusion_diag << std::endl; s_fusion_diag++; }
            u32 halfW = render_width_ / 2;
            u32 halfH = render_height_ / 2;

            cmd->SetViewport({{0, 0}, {static_cast<float>(halfW), static_cast<float>(halfH)}, 0, 1});
            cmd->SetScissor({{0, 0}, {halfW, halfH}});

            const bool isVk = device_->GetPlatform() == RHIPlatform::Vulkan;
            const u32 paramCount = isVk ? 6 : 5;

            // Resolve SSGI/DDGI/SPGI from RG handles when physical tex is INVALID.
            ResourceHandle ssgiTex = inputs.ssgi_tex;
            if (ssgiTex == handles::INVALID_RESOURCE && inputs.ssgi_rg.IsValid())
                ssgiTex = resolve(inputs.ssgi_rg);
            if (ssgiTex == handles::INVALID_RESOURCE) ssgiTex = inputs.black_texture;

            static int s_ssgi_resolve = 0;
            if (s_ssgi_resolve < 5) {
                std::cerr << "[FUSION_SSGI] ssgi_tex=" << inputs.ssgi_tex
                          << " ssgi_rg_valid=" << inputs.ssgi_rg.IsValid()
                          << " resolved=" << ssgiTex
                          << " black=" << inputs.black_texture << std::endl;
                s_ssgi_resolve++;
            }

            // Force SSGI texture to SRV layout — it may be left in TRANSFER_DST
            // by SSGI's internal blit or layout tracking issues.
            if (ssgiTex != inputs.black_texture) {
                ResourceBarrier ssgiBar{};
                ssgiBar.resource = ssgiTex;
                ssgiBar.beforeState = ResourceState::ShaderResource;
                ssgiBar.afterState = ResourceState::ShaderResource;
                ssgiBar.subresource = 0xFFFFFFFF;
                ssgiBar.queueFamily = 0xFFFFFFFF;
                cmd->InsertBarrier(&ssgiBar, 1);
            }

            DescData params[6] = {
                {0, DescriptorType::SampledImage, ssgiTex},
                {1, DescriptorType::SampledImage, inputs.ddgi_tex != handles::INVALID_RESOURCE ? inputs.ddgi_tex : inputs.black_texture},
                {2, DescriptorType::SampledImage, inputs.spgi_tex != handles::INVALID_RESOURCE ? inputs.spgi_tex : inputs.black_texture},
                {3, DescriptorType::SampledImage, inputs.gbuffer_albedo != handles::INVALID_RESOURCE ? inputs.gbuffer_albedo : inputs.black_texture},
                {4, DescriptorType::SampledImage, inputs.ssao_tex != handles::INVALID_RESOURCE ? inputs.ssao_tex : inputs.black_texture},
                {5, DescriptorType::Sampler, static_cast<ResourceHandle>(default_sampler_)},
            };
            UpdateDesc(device_, indirect_ds_[cbIdx], params, paramCount);
            cmd->BindGraphicsPipeline(indirect_pipeline_);
            const DescriptorSetHandle sets[] = { indirect_ds_[cbIdx] };
            cmd->BindDescriptorSets(PipelineBindPoint::Graphics, indirect_layout_, 0, 1, sets, 0, nullptr);
            cmd->Draw(3, 0, 1, 0);
        }
    );

    // --- Pass 2: FusionComposite (full-res) ---
    struct ComposeData {
        rendergraph::RGResourceHandle output;
    };

    graph.AddPass<ComposeData>("FusionComposite",
        rendergraph::RGPassType::Graphics,
        rendergraph::RGPassCategory::PostProcess,
        [primaryRG = inputs.primary_input_rg, volumeRG = inputs.volume_scatter_rg, ssrRG = inputs.ssr_rg, indirectRG, fusionOutRG](ComposeData& data, rendergraph::RenderGraphBuilder& builder) {
            builder.SideEffect(); // prevent RG culling
            if (primaryRG.IsValid()) builder.Read(primaryRG, ResourceState::ShaderResource);
            builder.Read(indirectRG, ResourceState::ShaderResource);
            if (volumeRG.IsValid()) builder.Read(volumeRG, ResourceState::ShaderResource);
            if (ssrRG.IsValid()) builder.Read(ssrRG, ResourceState::ShaderResource);
            data.output = builder.Write(fusionOutRG, ResourceState::RenderTarget);

            rendergraph::RGRenderPassDesc rpDesc;
            rpDesc.colors.push_back({.texture = data.output, .loadOp = LoadAction::DontCare, .storeOp = StoreAction::Store, .clearColor = {math::v4{0, 0, 0, 1}}});
            builder.DeclareRenderPass(rpDesc);
        },
        [this, inputs, cbIdx, indirectTex = indirect_output_[cbIdx], resolve](const ComposeData&, rendergraph::RenderGraphContext& context) {
            auto cmd = context.cmdBuffer;
            static int s_composite_diag = 0;
            if (s_composite_diag < 3) { std::cerr << "[FUSION_COMPOSITE] exec " << s_composite_diag << std::endl; s_composite_diag++; }
            cmd->SetViewport({{0, 0}, {static_cast<float>(render_width_), static_cast<float>(render_height_)}, 0, 1});
            cmd->SetScissor({{0, 0}, {render_width_, render_height_}});

            // Resolve volume scatter from RG, or fall back to identity texture (transmittance=1).
            // On Vulkan, volume pass is not active — force identity to avoid binding
            // an incorrectly-formatted texture from RG (causes R32_UINT validation error).
            ResourceHandle volTex;
            if (device_->GetPlatform() == RHIPlatform::Vulkan) {
                volTex = volume_identity_tex_;
            } else {
                volTex = resolve(inputs.volume_scatter_rg);
                if (volTex == inputs.black_texture || volTex == handles::INVALID_RESOURCE)
                    volTex = volume_identity_tex_;
            }

            // Resolve SSR from RG when physical tex is INVALID; fall back to black.
            ResourceHandle ssrTex = inputs.ssr_tex;
            if (ssrTex == handles::INVALID_RESOURCE && inputs.ssr_rg.IsValid())
                ssrTex = resolve(inputs.ssr_rg);
            if (ssrTex == handles::INVALID_RESOURCE) ssrTex = inputs.black_texture;

            // ORM texture (roughness/metallic) — fall back to white (roughness=1) when absent,
            // so SSR contribution is fully attenuated (smoothstep at roughness=1 → weight 0).
            ResourceHandle ormTex = inputs.gbuffer_orm;
            if (ormTex == handles::INVALID_RESOURCE) ormTex = inputs.black_texture;

            const bool isVk = device_->GetPlatform() == RHIPlatform::Vulkan;
            const u32 paramCount = isVk ? 6 : 5;
            DescData params[6] = {
                {0, DescriptorType::SampledImage, inputs.primary_input_tex},
                {1, DescriptorType::SampledImage, indirectTex},
                {2, DescriptorType::SampledImage, volTex},
                {3, DescriptorType::SampledImage, ssrTex},
                {4, DescriptorType::SampledImage, ormTex},
                {5, DescriptorType::Sampler, static_cast<ResourceHandle>(default_sampler_)},
            };
            UpdateDesc(device_, composite_ds_[cbIdx], params, paramCount);
            cmd->BindGraphicsPipeline(composite_pipeline_);
            const DescriptorSetHandle sets[] = { composite_ds_[cbIdx] };
            cmd->BindDescriptorSets(PipelineBindPoint::Graphics, composite_layout_, 0, 1, sets, 0, nullptr);
            cmd->Draw(3, 0, 1, 0);
        }
    );

    return outputs;
}

} // namespace primal::graphics
