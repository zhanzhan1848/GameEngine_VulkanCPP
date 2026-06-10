#include "StandardRenderPipeline.h"
#include "Graphics/RenderPipeline/RenderPasses/ForwardPass.h"
#include "Graphics/RenderPipeline/RenderPasses/PostProcess/SSAOPass.h"
#include "Graphics/RenderPipeline/RenderPasses/PostProcess/BloomPass.h"
#include "Graphics/RenderPipeline/RenderPasses/PostProcess/ToneMappingPass.h"
#include "Graphics/RenderPipeline/RenderPasses/PostProcess/HZBPass.h"
#include "Graphics/RenderPipeline/RenderPasses/PostProcess/VelocityPass.h"
#include "Graphics/RenderPipeline/RenderPasses/PostProcess/LumenSSGIDawnPass.h"
#include "Graphics/RenderScene.h"
#include "Graphics/RenderView.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Core/RHIMath.h"
#include <iostream>

#include <chrono>

namespace primal::graphics {

StandardRenderPipeline::~StandardRenderPipeline() {
    Shutdown();
}

bool StandardRenderPipeline::Initialize(rhi::RHIDeviceBase* device) {
    if (!device) return false;
    device_ = device;
    renderGraph_ = std::make_unique<rendergraph::RenderGraph>(*device);

    gpuOptimizer_ = std::make_unique<rhi::RHIGPUOptimizer>(*device);
    if (!gpuOptimizer_->Initialize()) {
        std::cerr << "Failed to initialize GPU Optimizer" << std::endl;
    }

    forwardRenderer_ = std::make_unique<ForwardRenderer>();
    if (!forwardRenderer_->Initialize(device)) {
        std::cerr << "Failed to initialize ForwardRenderer" << std::endl;
        forwardRenderer_.reset();
    }

    InitializeLumenPasses();

    return true;
}

void StandardRenderPipeline::Shutdown() {
    if (forwardRenderer_) {
        forwardRenderer_->Shutdown();
        forwardRenderer_.reset();
    }

    ShutdownLumenPasses();

    if (gpuOptimizer_) {
        gpuOptimizer_->Shutdown();
        gpuOptimizer_.reset();
    }
    renderGraph_.reset();
    device_ = nullptr;
}

using namespace rendergraph;

void StandardRenderPipeline::Render(RenderScene& scene, RenderView& view, rhi::ResourceHandle target, const rhi::TextureDesc& targetDesc, rhi::SyncHandle signalFence) {
    auto startTime = std::chrono::high_resolution_clock::now();

    if (!device_ || !renderGraph_) return;

    renderGraph_->Clear();

    // Import BackBuffer
    rhi::ResourceHandle backBufferHandle = target;
    rhi::TextureDesc backBufferDesc = targetDesc;

    RGResourceHandle backBuffer = renderGraph_->ImportTexture("BackBuffer", backBufferHandle, backBufferDesc);

    u32 width = targetDesc.size.x;
    u32 height = targetDesc.size.y;
    u32 frameIndex = static_cast<u32>(frameCount_ % rhi::MAX_FRAMES_IN_FLIGHT);

    // Lumen Surface Cache (High quality and above)
    if (surface_cache_pass_ && surface_cache_pass_->IsInitialized()) {
        lumen::SurfaceCacheFrameData frame_data{};
        frame_data.camera_position = { 0.0f, 0.0f, 0.0f };
        frame_data.frame_index = static_cast<u32>(frameCount_);
        frame_data.light_count = 0;

        rhi::ResourceHandle null_light_buffer{ rhi::handles::INVALID_RESOURCE };

        surface_cache_pass_->AddPass(
            *renderGraph_,
            backBuffer,
            null_light_buffer,
            frame_data,
            static_cast<u32>(frameCount_));

        if (screen_probe_pass_ && screen_probe_pass_->IsInitialized()) {
            screen_probe_pass_->SetSurfaceCacheData(
                surface_cache_pass_->GetLightingAtlas(static_cast<u32>(frameCount_)),
                surface_cache_pass_->GetCardDataBuffer(),
                surface_cache_pass_->GetCardLookupBuffer(),
                lumen_config_.surface_cache_atlas_size,
                lumen_config_.surface_cache_max_cards);
        }
    }

    // 1. Forward Pass → HDR + Depth
    RGResourceHandle hdrTexture = kInvalidRGResourceHandle;
    if (forwardRenderer_ && materials_) {
        const auto& fwdOutput = ForwardPass::AddPass(*renderGraph_, scene, view, *forwardRenderer_,
            *materials_, frameIndex, width, height);
        hdrTexture = fwdOutput.hdrTexture;

        // 2. HZB Generation from depth buffer
        RGResourceHandle hzbTexture = kInvalidRGResourceHandle;
        RGResourceHandle velTexture = kInvalidRGResourceHandle;
        RGResourceHandle ssgiTexture = kInvalidRGResourceHandle;
        if (hdrTexture != kInvalidRGResourceHandle && fwdOutput.depthTexture != kInvalidRGResourceHandle) {
            auto invProj = rhi::math::Inverse(view.GetProjectionMatrix());

            const auto& hzbOut = PostProcess::AddHZBPass(*renderGraph_, fwdOutput.depthTexture, width, height);
            hzbTexture = hzbOut.hzbTexture;

            auto viewProj = view.GetViewMatrix() * view.GetProjectionMatrix();
            const auto& velOut = PostProcess::AddVelocityPass(*renderGraph_, fwdOutput.depthTexture,
                width, height, frameIndex, viewProj, viewProj, invProj);
            velTexture = velOut.velocityTexture;

            // SSGI disabled for Dawn — causes rendering issues, ToneMapping uses dummy fallback
            ssgiTexture = kInvalidRGResourceHandle;
        }

        // 3. SSAO: Screen Space Ambient Occlusion
        RGResourceHandle aoTexture = kInvalidRGResourceHandle;
        if (hdrTexture != kInvalidRGResourceHandle && fwdOutput.depthTexture != kInvalidRGResourceHandle) {
            auto invProj = rhi::math::Inverse(view.GetProjectionMatrix());
            const auto& ssaoOutput = PostProcess::AddSSAOPass(*renderGraph_, fwdOutput.depthTexture,
                width, height, frameIndex, view.GetProjectionMatrix(), invProj);
            aoTexture = ssaoOutput.ssaoOutput;
        }

        // 3. Bloom: Extract bright areas + blur
        RGResourceHandle bloomTexture = kInvalidRGResourceHandle;
        if (hdrTexture != kInvalidRGResourceHandle) {
            const auto& bloomOutput = PostProcess::AddBloomPass(*renderGraph_, hdrTexture, frameIndex);
            bloomTexture = bloomOutput.bloomOutput;

            // 4. ToneMapping: HDR → LDR (with bloom + AO)
            const auto& tonemapOutput = PostProcess::AddToneMappingPass(*renderGraph_, hdrTexture, bloomTexture, aoTexture, ssgiTexture, frameIndex);

            // 3. Present: Blit LDR → BackBuffer
            struct PresentData {
                RGResourceHandle input;
                RGResourceHandle output;
            };
            renderGraph_->AddPass<PresentData>("PresentPass", RGPassType::Graphics, RGPassCategory::Present,
                [&](PresentData& data, RenderGraphBuilder& builder) {
                    data.input = tonemapOutput.output;
                    data.output = backBuffer;
                    builder.Read(data.input, rhi::ResourceState::ShaderResource);
                    builder.Write(data.output, rhi::ResourceState::RenderTarget);
                },
                [](const PresentData& data, RenderGraphContext& context) {
                    auto* inputRes = context.graph->GetResource(data.input);
                    auto* outputRes = context.graph->GetResource(data.output);
                    if (!inputRes || !outputRes) return;

                    auto& device = context.graph->GetDevice();
                    auto* cmd = context.cmdBuffer;

                    // Get input texture view for sampling
                    ResourceHandle inputHandle = inputRes->GetPhysicalHandle();
                    rhi::DataFormat inputFormat = rhi::DataFormat::RGBA8_UNorm;
                    if (inputRes->GetType() == RGResourceType::Texture) {
                        inputFormat = static_cast<RenderGraphTexture*>(inputRes)->GetDesc().format;
                    }

                    ResourceHandle inputView = rhi::handles::INVALID_RESOURCE;
                    if (inputHandle != rhi::handles::INVALID_RESOURCE) {
                        rhi::TextureViewDesc viewDesc;
                        viewDesc.texture = inputHandle;
                        viewDesc.viewType = rhi::TextureType::Texture2D;
                        viewDesc.format = inputFormat;
                        inputView = device.CreateTextureView(viewDesc);
                    }

                    // Use device's blit pipeline for the copy
                    // For now, just clear the backbuffer as a minimal present step
                    rhi::RenderPassDesc passDesc;
                    passDesc.colorAttachments.resize(1);
                    passDesc.colorAttachments[0].texture = outputRes->GetPhysicalHandle();
                    passDesc.colorAttachments[0].loadOp = rhi::LoadAction::Clear;
                    passDesc.colorAttachments[0].storeOp = rhi::StoreAction::Store;

                    cmd->BeginRenderPass(passDesc);
                    cmd->EndRenderPass();

                    if (inputView != rhi::handles::INVALID_RESOURCE) {
                        device.DestroyTexture(inputView);
                    }
                }
            );
        }
    } else {
        // Fallback: simple clear pass when ForwardRenderer or materials aren't available
        struct ClearData { RGResourceHandle target; };
        renderGraph_->AddPass<ClearData>("ClearPass", RGPassType::Graphics, RGPassCategory::None,
            [&](ClearData& data, RenderGraphBuilder& builder) {
                data.target = backBuffer;
                builder.Write(data.target, rhi::ResourceState::RenderTarget);
                builder.SideEffect();
            },
            [](const ClearData& data, RenderGraphContext& context) {
                auto* rtRes = context.graph->GetResource(data.target);
                if (!rtRes) return;
                rhi::RenderPassDesc passDesc;
                passDesc.colorAttachments.resize(1);
                passDesc.colorAttachments[0].texture = rtRes->GetPhysicalHandle();
                passDesc.colorAttachments[0].loadOp = rhi::LoadAction::Clear;
                passDesc.colorAttachments[0].storeOp = rhi::StoreAction::Store;
                context.cmdBuffer->BeginRenderPass(passDesc);
                context.cmdBuffer->EndRenderPass();
            }
        );
    }

    // Compile and Execute
    renderGraph_->Compile();

    rhi::CommandBufferHandle cmdBufferHandle = device_->CreateCommandBuffer(rhi::CommandQueueType::Graphics);
    if (cmdBufferHandle == rhi::handles::INVALID_COMMAND_BUFFER) return;

    rhi::RHICommandBuffer* cmdBuffer = rhi::GetCommandBuffer(cmdBufferHandle);
    if (cmdBuffer) {
        if (cmdBuffer->Initialize()) {
            if (!cmdBuffer->Begin()) {
                std::cerr << "Failed to begin command buffer!" << std::endl;
                return;
            }
            renderGraph_->Execute(cmdBuffer);
            cmdBuffer->End();

            rhi::QueueSubmitInfo submitInfo;
            submitInfo.cmdBuffer = cmdBufferHandle;
            submitInfo.signalFence = signalFence;

            device_->Submit(submitInfo);

            const auto& cmdStats = cmdBuffer->GetStats();
            stats_.drawCallCount = cmdStats.drawCallCount;
            stats_.gpuFrameTimeMs = cmdStats.commandExecutionTime;
            stats_.passExecutionTimes = renderGraph_->GetPassExecutionTimes();

            if (gpuOptimizer_) {
                for (const auto& [passName, timeMs] : stats_.passExecutionTimes) {
                    gpuOptimizer_->RecordPassExecutionTime(passName, timeMs);
                }
                gpuOptimizer_->Update(frameCount_, 16.6f / 1000.0f);
            }

            auto endTime = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double, std::milli> cpuTime = endTime - startTime;
            stats_.cpuFrameTimeMs = cpuTime.count();

        } else {
             std::cerr << "Failed to begin command buffer!" << std::endl;
        }
    }

    frameCount_++;

    auto endTime = std::chrono::high_resolution_clock::now();
    stats_.cpuFrameTimeMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();
}

void StandardRenderPipeline::InitializeLumenPasses() {
    if (!device_) return;

    const auto& config = lumen_config_;

    if (config.quality >= lumen::LumenQualityPreset::High) {
        surface_cache_pass_ = std::make_unique<lumen::SurfaceCachePass>();
        if (!surface_cache_pass_->Initialize(device_, config)) {
            std::cerr << "[Lumen] Failed to initialize SurfaceCachePass" << std::endl;
            surface_cache_pass_.reset();
        }
    }

    if (config.quality >= lumen::LumenQualityPreset::Ultra) {
        screen_probe_pass_ = std::make_unique<lumen::ScreenProbeGIPass>();
        lumen::ScreenProbeParams probe_params{};
        probe_params.downsample_factor = config.screen_probes_spacing;
        probe_params.rays_per_probe = config.screen_probes_rays;
        if (!screen_probe_pass_->Initialize(device_, 1920, 1080, probe_params)) {
            std::cerr << "[Lumen] Failed to initialize ScreenProbeGIPass" << std::endl;
            screen_probe_pass_.reset();
        }
    }
}

void StandardRenderPipeline::ShutdownLumenPasses() {
    if (screen_probe_pass_) {
        screen_probe_pass_->Shutdown();
        screen_probe_pass_.reset();
    }
    if (surface_cache_pass_) {
        surface_cache_pass_->Shutdown();
        surface_cache_pass_.reset();
    }
}

} // namespace primal::graphics
