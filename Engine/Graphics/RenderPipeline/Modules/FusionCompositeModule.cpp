#include "FusionCompositeModule.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include <iostream>
#include <cstdlib>
#include "Engine/Common/CoreLogStream.h"

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

// RGBA16F 输出按四竖条统计亮度 mean/max + RGB 分量 mean（颜色对齐用），与
// SC_DUMP_CARDS 的 IND_STATS 同一数据面（ddgi|ssgi|spgi|full），供异步路径复用。
static void PrintIndirectStripStats(const u16* p, u32 w, u32 h, const char* prefix, u32 tick) {
    auto f16 = [](u16 h16) -> float {
        u32 sign = (h16 >> 15) & 1u, exp = (h16 >> 10) & 0x1Fu, mant = h16 & 0x3FFu;
        if (exp == 0u) return 0.0f;
        u32 bits = (sign << 31) | ((exp + 112u) << 23) | (mant << 13);
        float f; std::memcpy(&f, &bits, 4); return f;
    };
    const char* names[4] = { "ddgi-strip", "ssgi-strip", "spgi-strip", "full-strip" };
    for (int q = 0; q < 4; ++q) {
        u32 x0 = w * q / 4, x1 = w * (q + 1) / 4;
        double sum = 0.0, sr = 0.0, sg = 0.0, sb = 0.0;
        float mx = 0.0f; u64 n = 0;
        for (u32 y = 0; y < h; y += 4)
            for (u32 x = x0; x < x1; x += 4) {
                const u16* t = p + ((size_t)y * w + x) * 4;
                float r = f16(t[0]), g = f16(t[1]), b = f16(t[2]);
                if (std::isnan(r)) continue;
                sum += 0.2126f * r + 0.7152f * g + 0.0722f * b;
                sr += r; sg += g; sb += b;
                mx = std::max(mx, std::max(r, std::max(g, b)));
                ++n;
            }
        if (n) PRIMAL_LOG_STREAM(Error, "Render") << "[" << prefix << "] tick=" << tick << " " << names[q]
                         << " mean=" << (sum / n)
                         << " rgb=(" << (sr / n) << "," << (sg / n) << "," << (sb / n) << ")"
                         << " max=" << mx << std::endl;
    }
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
            PRIMAL_LOG_STREAM(Error, "Render") << "[FusionComposite] Failed to create default_sampler_" << std::endl;
        }
    }

    // --- Pass 1: FusionIndirect (full-res, 5 texture reads + 1 sampler on Vulkan) ---
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

        // Full-res indirect output.  SPGI and material data contain thin
        // receiver geometry that cannot survive an unguided half-res resolve.
        // DDGI itself remains half-res and is linearly sampled as low-frequency
        // lighting by the fragment shader.
        TextureDesc indDesc{};
        indDesc.size = {render_width, render_height, 1};
        indDesc.format = DataFormat::RGBA16_Float;
        indDesc.usage = TextureUsage::ShaderResource | TextureUsage::RenderTarget |
                        TextureUsage::CopySource;  // TEMP: IND_STATS readback
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

    // --- Pass 2: FusionComposite (full-res, 7 texture reads + 1 sampler on Vulkan) ---
    {
        // bindings: 0=scene, 1=indirect, 2=volume_scatter, 3=ssr, 4=orm,
        //           5=albedo, 6=ddgi_specular (T4.3), [7=sampler(Vk)]
        const u32 compositeBindingCount = isVk ? 8 : 7;
        DescriptorSetLayoutBinding bindings[8];
        bindings[0] = {0, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr};  // scene
        bindings[1] = {1, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr};  // indirect
        bindings[2] = {2, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr};  // volume_scatter
        bindings[3] = {3, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr};  // ssr
        bindings[4] = {4, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr};  // orm
        bindings[5] = {5, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr};  // albedo (T4.3)
        bindings[6] = {6, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr};  // ddgi_specular (T4.3)
        if (isVk) {
            bindings[7] = {7, DescriptorType::Sampler, 1, ShaderStage::Pixel, nullptr};   // sampler
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
    for (auto& staging : async_stats_staging_) {
        if (device_ && staging != handles::INVALID_RESOURCE) {
            device_->DestroyBuffer(staging);
            staging = handles::INVALID_RESOURCE;
        }
    }
    for (auto* ring : {&primary_stats_staging_, &fusion_stats_staging_, &ddgi_stats_staging_}) {
        for (auto& staging : *ring) {
            if (device_ && staging != handles::INVALID_RESOURCE) {
                device_->DestroyBuffer(staging);
                staging = handles::INVALID_RESOURCE;
            }
        }
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

    // --- Pass 1: FusionIndirect (full-res) ---
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
            if (s_fusion_diag < 3) { PRIMAL_LOG_STREAM(Error, "Render") << "[FUSION_INDIRECT] exec " << s_fusion_diag << std::endl; s_fusion_diag++; }
            cmd->SetViewport({{0, 0}, {static_cast<float>(render_width_), static_cast<float>(render_height_)}, 0, 1});
            cmd->SetScissor({{0, 0}, {render_width_, render_height_}});

            const bool isVk = device_->GetPlatform() == RHIPlatform::Vulkan;
            const u32 paramCount = isVk ? 6 : 5;

            // Resolve SSGI/DDGI/SPGI from RG handles when physical tex is INVALID.
            ResourceHandle ssgiTex = inputs.ssgi_tex;
            if (ssgiTex == handles::INVALID_RESOURCE && inputs.ssgi_rg.IsValid())
                ssgiTex = resolve(inputs.ssgi_rg);
            if (ssgiTex == handles::INVALID_RESOURCE) ssgiTex = inputs.black_texture;

            static int s_ssgi_resolve = 0;
            if (s_ssgi_resolve < 5) {
                PRIMAL_LOG_STREAM(Error, "Render") << "[FUSION_SSGI] ssgi_tex=" << inputs.ssgi_tex
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
            // TEMP diagnosis: what really lands in each GI texture slot.
            static int s_fusion_slots = 0;
            if (s_fusion_slots < 3) {
                ++s_fusion_slots;
                ResourceHandle spgiTex = inputs.spgi_tex != handles::INVALID_RESOURCE
                    ? inputs.spgi_tex : inputs.black_texture;
                PRIMAL_LOG_STREAM(Error, "Render") << "[FUSION_SLOTS] black=" << inputs.black_texture
                          << " ssgi=" << ssgiTex
                          << " ddgi=" << (inputs.ddgi_tex != handles::INVALID_RESOURCE ? inputs.ddgi_tex : inputs.black_texture)
                          << " spgi_in=" << inputs.spgi_tex
                          << " spgi_rg_valid=" << inputs.spgi_rg.IsValid()
                          << " spgi_bound=" << spgiTex
                          << " albedo=" << inputs.gbuffer_albedo
                          << " ssao=" << inputs.ssao_tex << std::endl;
            }
            UpdateDesc(device_, indirect_ds_[cbIdx], params, paramCount);
            cmd->BindGraphicsPipeline(indirect_pipeline_);
            const DescriptorSetHandle sets[] = { indirect_ds_[cbIdx] };
            cmd->BindDescriptorSets(PipelineBindPoint::Graphics, indirect_layout_, 0, 1, sets, 0, nullptr);
            cmd->Draw(3, 0, 1, 0);

            // TEMP diagnosis: one-shot per-strip stats of the indirect output
            // (RGBA16F full-res). Strips when GI_DEBUG_STRIP=1: ddgi|ssgi|spgi|full.
            static const bool s_stripDump = std::getenv("SC_DUMP_CARDS") != nullptr;  // TEMP: decouple from strip env
            static u32 s_stripTick = 0;
            static bool s_stripDumped = false;
            const u32 stripTickNow = ++s_stripTick;
            if (s_stripDump && !s_stripDumped && stripTickNow > 120) {
                s_stripDumped = true;
                const u32 w = render_width_, h = render_height_;
                // Read LAST frame's output (triple-buffered, GPU-complete —
                // this frame's [cbIdx] hasn't been submitted yet).
                const u32 readIdx = (cbIdx + 2) % 3;
                BufferDesc bd{}; bd.size = (u64)w * h * 8;
                bd.bindFlags = (u32)BufferUsageFlags::TransferDst;
                bd.memoryUsage = GPUMemoryUsage::Readback;
                auto stage = device_->CreateBuffer(bd);
                if (stage != handles::INVALID_RESOURCE) {
                    auto ch = device_->CreateCommandBuffer(CommandQueueType::Graphics);
                    auto* c2 = rhi::GetCommandBuffer(ch);
                    if (c2) {
                        c2->Begin();
                        BufferTextureCopyRegion rr{};
                        rr.imageSubresource.mipLevel = 0;
                        rr.imageSubresource.layerCount = 1;
                        rr.imageExtent = {w, h, 1};
                        c2->CopyTextureToBuffer(indirect_output_[readIdx], stage, &rr, 1);
                        c2->End(); c2->Submit(); c2->WaitForCompletion();
                        device_->DestroyCommandBuffer(ch);
                        if (auto* p = static_cast<const u16*>(device_->MapBuffer(stage, 0, bd.size))) {
                            auto f16 = [](u16 h16) -> float {
                                u32 sign = (h16 >> 15) & 1u, exp = (h16 >> 10) & 0x1Fu, mant = h16 & 0x3FFu;
                                if (exp == 0u) return 0.0f;
                                u32 bits = (sign << 31) | ((exp + 112u) << 23) | (mant << 13);
                                float f; std::memcpy(&f, &bits, 4); return f;
                            };
                            const char* names[4] = { "ddgi-strip", "ssgi-strip", "spgi-strip", "full-strip" };
                            for (int q = 0; q < 4; ++q) {
                                u32 x0 = w * q / 4, x1 = w * (q + 1) / 4;
                                double sum = 0.0; float mx = 0.0f; u64 n = 0;
                                for (u32 y = 0; y < h; y += 4)
                                    for (u32 x = x0; x < x1; x += 4) {
                                        const u16* t = p + ((size_t)y * w + x) * 4;
                                        float r = f16(t[0]), g = f16(t[1]), b = f16(t[2]);
                                        if (std::isnan(r)) continue;
                                        float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                                        sum += lum; mx = std::max(mx, lum); ++n;
                                    }
                                if (n) PRIMAL_LOG_STREAM(Error, "Render") << "[IND_STATS] " << names[q]
                                                 << " mean=" << (sum / n) << " max=" << mx << std::endl;
                            }
                            device_->UnmapBuffer(stage);
                        }
                    }
                    device_->DestroyBuffer(stage);
                }
            }

            // TEMP diagnosis: readback the "black" texture — if the heap-
            // recycled contents are non-zero, the SPGI/AO fallback slots
            // sample recycled scene data instead of black.
            static bool s_blackDumped = false;
            if (!s_blackDumped && inputs.black_texture != handles::INVALID_RESOURCE) {
                s_blackDumped = true;
                auto ch2 = device_->CreateCommandBuffer(CommandQueueType::Graphics);
                auto* c3 = rhi::GetCommandBuffer(ch2);
                if (c3) {
                    c3->Begin();
                    BufferTextureCopyRegion br{};
                    br.imageSubresource.mipLevel = 0;
                    br.imageSubresource.layerCount = 1;
                    br.imageExtent = {1, 1, 1};
                    auto bsDesc = BufferDesc{};
                    bsDesc.size = 512;
                    bsDesc.bindFlags = (u32)BufferUsageFlags::TransferDst;
                    bsDesc.memoryUsage = GPUMemoryUsage::Readback;
                    auto bs = device_->CreateBuffer(bsDesc);
                    if (bs != handles::INVALID_RESOURCE) {
                        c3->CopyTextureToBuffer(inputs.black_texture, bs, &br, 1);
                        c3->End(); c3->Submit(); c3->WaitForCompletion();
                        device_->DestroyCommandBuffer(ch2);
                        if (auto* q = static_cast<const u16*>(device_->MapBuffer(bs, 0, 512))) {
                            PRIMAL_LOG_STREAM(Error, "Render") << "[BLACK_TEX] first 8 texels rgba16:";
                            for (int k = 0; k < 32; ++k) PRIMAL_LOG_STREAM(Error, "Render") << " " << q[k];
                            PRIMAL_LOG_STREAM(Error, "Render") << std::endl;
                            device_->UnmapBuffer(bs);
                        }
                    } else {
                        device_->DestroyCommandBuffer(ch2);
                    }
                    device_->DestroyBuffer(bs);
                }
            }
        }
    );

    // --- Pass 2: FusionComposite (full-res) ---
    struct ComposeData {
        rendergraph::RGResourceHandle output;
    };

    graph.AddPass<ComposeData>("FusionComposite",
        rendergraph::RGPassType::Graphics,
        rendergraph::RGPassCategory::PostProcess,
        [primaryRG = inputs.primary_input_rg, volumeRG = inputs.volume_scatter_rg, ssrRG = inputs.ssr_rg,
         specRG = inputs.ddgi_specular_rg, indirectRG, fusionOutRG](ComposeData& data, rendergraph::RenderGraphBuilder& builder) {
            builder.SideEffect(); // prevent RG culling
            if (primaryRG.IsValid()) builder.Read(primaryRG, ResourceState::ShaderResource);
            builder.Read(indirectRG, ResourceState::ShaderResource);
            if (volumeRG.IsValid()) builder.Read(volumeRG, ResourceState::ShaderResource);
            if (ssrRG.IsValid()) builder.Read(ssrRG, ResourceState::ShaderResource);
            if (specRG.IsValid()) builder.Read(specRG, ResourceState::ShaderResource);
            data.output = builder.Write(fusionOutRG, ResourceState::RenderTarget);

            rendergraph::RGRenderPassDesc rpDesc;
            rpDesc.colors.push_back({.texture = data.output, .loadOp = LoadAction::DontCare, .storeOp = StoreAction::Store, .clearColor = {math::v4{0, 0, 0, 1}}});
            builder.DeclareRenderPass(rpDesc);
        },
        [this, inputs, cbIdx, indirectTex = indirect_output_[cbIdx], resolve](const ComposeData&, rendergraph::RenderGraphContext& context) {
            auto cmd = context.cmdBuffer;
            static int s_composite_diag = 0;
            if (s_composite_diag < 3) { PRIMAL_LOG_STREAM(Error, "Render") << "[FUSION_COMPOSITE] exec " << s_composite_diag << std::endl; s_composite_diag++; }
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

            // T4.3: DDGI glossy specular + albedo (metallic F0). Black fallbacks
            // degrade the glossy relay to SSR-only.
            ResourceHandle specTex = inputs.ddgi_specular_tex;
            if (specTex == handles::INVALID_RESOURCE && inputs.ddgi_specular_rg.IsValid())
                specTex = resolve(inputs.ddgi_specular_rg);
            if (specTex == handles::INVALID_RESOURCE || specTex == inputs.black_texture)
                specTex = inputs.black_texture;
            ResourceHandle albedoTex = inputs.gbuffer_albedo;
            if (albedoTex == handles::INVALID_RESOURCE) albedoTex = inputs.black_texture;

            const bool isVk = device_->GetPlatform() == RHIPlatform::Vulkan;
            const u32 paramCount = isVk ? 8 : 7;
            DescData params[8] = {
                {0, DescriptorType::SampledImage, inputs.primary_input_tex},
                {1, DescriptorType::SampledImage, indirectTex},
                {2, DescriptorType::SampledImage, volTex},
                {3, DescriptorType::SampledImage, ssrTex},
                {4, DescriptorType::SampledImage, ormTex},
                {5, DescriptorType::SampledImage, albedoTex},
                {6, DescriptorType::SampledImage, specTex},
                {7, DescriptorType::Sampler, static_cast<ResourceHandle>(default_sampler_)},
            };
            UpdateDesc(device_, composite_ds_[cbIdx], params, paramCount);
            cmd->BindGraphicsPipeline(composite_pipeline_);
            const DescriptorSetHandle sets[] = { composite_ds_[cbIdx] };
            cmd->BindDescriptorSets(PipelineBindPoint::Graphics, composite_layout_, 0, 1, sets, 0, nullptr);
            cmd->Draw(3, 0, 1, 0);
        }
    );

    // --- Pass 3: GI 可见性 flush（生产修复）+ PRIMAL_IND_ASYNC 多路条带统计（诊断）---
    //
    // 生产修复（Metal 白爆，2026-09-02）：DDGI gather 的 compute 写纹理对后续
    // render encoder 的采样不可见（fragmentFusion 读到堆回收垃圾，条带 mean 高达
    // 199/1724 → 直射区+GI 区全屏爆白+噪点）。UAV 纹理已是 HazardTrackingModeTracked、
    // RG 也已声明 ShaderResource 读边，但只有对本纹理的一次全幅 blit 读能强制
    // 可见化（部分区域拷贝无效、仅声明 RG 边无效——已按位掩码逐项实测）。
    // 故 Metal 平台默认执行 ddgi 全幅 CopyTextureToBuffer（半res RGBA16F ≈1.4MB/帧，
    // Apple Silicon 带宽下可忽略）。Vulkan 无此问题，保持零改动。
    //
    // 诊断门 PRIMAL_IND_ASYNC（位掩码，覆盖默认行为，用于回归定位）：
    //   1 = 全部边（ddgi|primary|indirect|fusion）  2 = 仅 ddgi（=生产默认）
    //   4 = 仅 primary   8 = 仅 indirect            16 = 仅 fusion
    //  32 = 仅 ddgi 边声明、不发拷贝命令           64 = 仅 ddgi 拷 256×256 角落
    // 设了该环境变量时同时打印 [*_ASYNC] 条带统计（读回环：拷贝挂本帧 CB，
    // 3 帧后同槽映射，零额外同步——SC_DUMP_CARDS 的帧中 WaitForCompletion 会
    // 掩盖时序问题，不可用于此类定位）。
    {
        static const u32 s_envMask = [] {
            const char* e = std::getenv("PRIMAL_IND_ASYNC");
            if (!e) return 0u;
            u32 m = (u32)std::atoi(e);
            return m == 1u ? 15u : m;
        }();
        static const bool s_printStats = [] {
            return std::getenv("PRIMAL_IND_ASYNC") != nullptr;
        }();
        const bool isMetal = device_->GetPlatform() == RHIPlatform::Metal;
        // Vulkan 的 DDGI gather 是半分辨率纹理，全幅 extent 拷贝会越界——
        // 非 Metal 平台强制清掉 ddgi 位（间接/合成输出都是全分辨率，安全）。
        u32 maskEff = s_envMask ? s_envMask : (isMetal ? 2u : 0u);
        if (!isMetal) maskEff &= ~(2u | 64u);
        const u32 s_asyncMask = maskEff;
        static u32 s_asyncTick = 0;
        if (s_asyncMask) {
            struct AsyncStatsData {
                rendergraph::RGResourceHandle source;
                rendergraph::RGResourceHandle primary;
            };
            graph.AddPass<AsyncStatsData>("FusionIndirectAsyncStats",
                rendergraph::RGPassType::Compute,
                rendergraph::RGPassCategory::PostProcess,
                [inputs, indirectRG, fusionOutRG, s_asyncMask](AsyncStatsData& data, rendergraph::RenderGraphBuilder& builder) {
                    builder.SideEffect(); // 诊断 pass，禁止被 RG 裁剪
                    if (s_asyncMask & 8u) data.source = builder.Read(indirectRG, ResourceState::CopySource);
                    if ((s_asyncMask & 4u) && inputs.primary_input_rg.IsValid())
                        data.primary = builder.Read(inputs.primary_input_rg, ResourceState::CopySource);
                    // fusion 输出在 composite pass 里写入；ddgi 来自 GIGather。
                    // 声明 CopySource 读依赖，保证拷贝排在写入之后。
                    if ((s_asyncMask & 16u) && fusionOutRG.IsValid())
                        builder.Read(fusionOutRG, ResourceState::CopySource);
                    if ((s_asyncMask & 2u) && inputs.ddgi_rg.IsValid())
                        builder.Read(inputs.ddgi_rg, ResourceState::CopySource);
                },
                [this, inputs, cbIdx, indirectTex = indirect_output_[cbIdx],
                 fusionTex = fusion_output_[cbIdx], s_asyncMask,
                 printStats = s_printStats](const AsyncStatsData&, rendergraph::RenderGraphContext& context) {
                    auto cmd = context.cmdBuffer;
                    const u32 tick = ++s_asyncTick;
                    const u32 w = render_width_, h = render_height_;
                    if (w == 0 || h == 0) return;

                    struct StatTarget {
                        const char* tag;
                        rhi::ResourceHandle tex;
                        rhi::ResourceHandle* ring;
                    };
                    // ddgi = GIGather 原始输出；primary = deferred 直射；
                    // indirect = FusionIndirect 合并结果；fusion = 合成帧（进 TAA 前）。
                    rhi::ResourceHandle ddgiTex = inputs.ddgi_tex != handles::INVALID_RESOURCE
                        ? inputs.ddgi_tex : inputs.black_texture;
                    StatTarget targets[4] = {
                        {"DDGI_ASYNC",   ddgiTex,     ddgi_stats_staging_},
                        {"PRIMARY_ASYNC", inputs.primary_input_tex, primary_stats_staging_},
                        {"IND_STATS_ASYNC", indirectTex, async_stats_staging_},
                        {"FUSION_ASYNC",  fusionTex,   fusion_stats_staging_},
                    };

                    // 1) 映射 3 帧前同槽拷贝的成果并出数（该帧 fence 已等过）。
                    //    统计打印只随显式诊断门开启，生产 flush 静默。
                    if (printStats) {
                        for (auto& t : targets) {
                            if (t.tex == handles::INVALID_RESOURCE) continue;
                            if (t.ring[cbIdx] != handles::INVALID_RESOURCE && tick > 8) {
                                if (auto* p = static_cast<const u16*>(
                                        device_->MapBuffer(t.ring[cbIdx], 0, (u64)w * h * 8))) {
                                    PrintIndirectStripStats(p, w, h, t.tag, tick);
                                    device_->UnmapBuffer(t.ring[cbIdx]);
                                }
                            }
                        }
                    }

                    // 2) 惰性建本槽 readback 缓冲（尺寸固定——诊断门不做 resize 跟踪）。
                    auto ensureStaging = [&](rhi::ResourceHandle& slot) {
                        if (slot == handles::INVALID_RESOURCE) {
                            BufferDesc bd{};
                            bd.size = (u64)w * h * 8;
                            bd.bindFlags = (u32)BufferUsageFlags::TransferDst;
                            bd.memoryUsage = GPUMemoryUsage::Readback;
                            slot = device_->CreateBuffer(bd);
                        }
                        return slot != handles::INVALID_RESOURCE;
                    };

                    // 3) 把本帧输出拷进本槽，3 帧后由本 pass 读出（按掩码选边）。
                    // mask 64：ddgi 只拷 256×256 角落——验证可见性 flush 不需要
                    // 全幅拷贝（同步点是访问本身而非数据量），以便低代价上生产。
                    BufferTextureCopyRegion rr{};
                    rr.imageSubresource.mipLevel = 0;
                    rr.imageSubresource.layerCount = 1;
                    rr.imageExtent = {w, h, 1};
                    if (s_asyncMask & 64u) rr.imageExtent = {std::min(256u, w), std::min(256u, h), 1};
                    const u32 targetBits[4] = {2u, 4u, 8u, 16u};
                    for (u32 i = 0; i < 4; ++i) {
                        auto& t = targets[i];
                        const bool copyThis = (s_asyncMask & targetBits[i]) ||
                                              ((s_asyncMask & 64u) && targetBits[i] == 2u);
                        if (!copyThis) continue;
                        if (t.tex == handles::INVALID_RESOURCE) continue;
                        if (!ensureStaging(t.ring[cbIdx])) continue;
                        cmd->CopyTextureToBuffer(t.tex, t.ring[cbIdx], &rr, 1);
                    }
                    // mask 32：仅声明 RG 读边（setup），不执行任何拷贝命令。
                });
        }
    }

    return outputs;
}

} // namespace primal::graphics
