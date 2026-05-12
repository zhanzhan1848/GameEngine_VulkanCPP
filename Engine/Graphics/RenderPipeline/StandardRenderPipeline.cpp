#include "StandardRenderPipeline.h"
#include "Graphics/RenderPipeline/RenderPasses/ForwardPass.h"
#include "Graphics/RenderScene.h"
#include "Graphics/RenderView.h"
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

    // 初始化 GPU 优化器
    gpuOptimizer_ = std::make_unique<rhi::RHIGPUOptimizer>(*device);
    if (!gpuOptimizer_->Initialize()) {
        std::cerr << "Failed to initialize GPU Optimizer" << std::endl;
        // 允许失败，非关键组件
    }

    // Initialize Lumen GI passes based on quality preset
    InitializeLumenPasses();

    return true;
}

void StandardRenderPipeline::Shutdown() {
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
    
    // Import BackBuffer into RenderGraph
    RGResourceHandle backBuffer = renderGraph_->ImportTexture("BackBuffer", backBufferHandle, backBufferDesc);

    // Setup Passes
    // 1. Depth PrePass (Optional, skipping for now)

    // 1.5 Lumen Surface Cache (High quality and above)
    if (surface_cache_pass_ && surface_cache_pass_->IsInitialized()) {
        lumen::SurfaceCacheFrameData frame_data{};
        // TODO: Wire actual camera position and light count from RenderView/RenderScene
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

        // Feed surface cache data to screen probe pass
        if (screen_probe_pass_ && screen_probe_pass_->IsInitialized()) {
            screen_probe_pass_->SetSurfaceCacheData(
                surface_cache_pass_->GetLightingAtlas(static_cast<u32>(frameCount_)),
                surface_cache_pass_->GetCardDataBuffer(),
                surface_cache_pass_->GetCardLookupBuffer(),
                lumen_config_.surface_cache_atlas_size,
                lumen_config_.surface_cache_max_cards);
        }
    }

    // 2. Forward Pass
    ForwardPass::AddPass(*renderGraph_, scene, view, backBuffer);

    // 3. UI Pass (ToDo)

    // 4. Present (Handled by RHI usually, but we can have a Present Pass or just ensure BackBuffer is written)
    // For RenderGraph, we just ensure the last pass writes to BackBuffer or transitions it to Present.
    
    // Compile and Execute
    renderGraph_->Compile();
    
    // We need a command buffer to execute
    // In a real engine, we'd get this from a CommandQueue
    rhi::CommandBufferHandle cmdBufferHandle = device_->CreateCommandBuffer(rhi::CommandQueueType::Graphics);
    if (cmdBufferHandle == rhi::handles::INVALID_COMMAND_BUFFER) return;
    
    rhi::RHICommandBuffer* cmdBuffer = rhi::GetCommandBuffer(cmdBufferHandle);
    if (cmdBuffer) {
        if (cmdBuffer->Initialize()) {
            if (!cmdBuffer->Begin()) {
                std::cerr << "Failed to begin command buffer!" << std::endl;
                return;
            }
            // std::cout << "Executing RenderGraph..." << std::endl;
            renderGraph_->Execute(cmdBuffer);
            // std::cout << "RenderGraph Executed." << std::endl;
            cmdBuffer->End(); // Ensure command buffer is closed
            
            rhi::QueueSubmitInfo submitInfo;
            submitInfo.cmdBuffer = cmdBufferHandle;
            submitInfo.signalFence = signalFence;
            
            device_->Submit(submitInfo); // Need fences in real usage
            
            // Collect statistics
            const auto& cmdStats = cmdBuffer->GetStats();
            stats_.drawCallCount = cmdStats.drawCallCount;
            stats_.gpuFrameTimeMs = cmdStats.commandExecutionTime;
            
            // Get GPU time from queries
            stats_.passExecutionTimes = renderGraph_->GetPassExecutionTimes();

            // Feed metrics to GPU Optimizer
            if (gpuOptimizer_) {
                for (const auto& [passName, timeMs] : stats_.passExecutionTimes) {
                    gpuOptimizer_->RecordPassExecutionTime(passName, timeMs);
                }
                
                // Update optimizer per frame
                // Assuming ~16.6ms per frame for now or calculate actual delta time
                // Here we use CPU time as an approximation or fixed step
                gpuOptimizer_->Update(frameCount_, 16.6f / 1000.0f);
            }

            // Calculate CPU time
            auto endTime = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double, std::milli> cpuTime = endTime - startTime;
            stats_.cpuFrameTimeMs = cpuTime.count();

        } else {
             std::cerr << "Failed to begin command buffer!" << std::endl;
        }
    }
    
    // Cleanup command buffer (should rely on frame/allocator but here we destroy it for now to avoid leak if pool not used)
    // Actually device_->DestroyCommandBuffer(handle) if available, or just leave it for GC/Pool.
    // Assuming simple lifetime for now.
    
    frameCount_++;

    auto endTime = std::chrono::high_resolution_clock::now();
    stats_.cpuFrameTimeMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();
    // TODO: Get GPU time from queries
}

void StandardRenderPipeline::InitializeLumenPasses() {
    if (!device_) return;

    const auto& config = lumen_config_;

    // Surface Cache: High quality and above
    if (config.quality >= lumen::LumenQualityPreset::High) {
        surface_cache_pass_ = std::make_unique<lumen::SurfaceCachePass>();
        if (!surface_cache_pass_->Initialize(device_, config)) {
            std::cerr << "[Lumen] Failed to initialize SurfaceCachePass" << std::endl;
            surface_cache_pass_.reset();
        }
    }

    // Screen Probes: Ultra quality and above
    if (config.quality >= lumen::LumenQualityPreset::Ultra) {
        screen_probe_pass_ = std::make_unique<lumen::ScreenProbeGIPass>();
        // TODO: Wire actual render dimensions from the viewport
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
