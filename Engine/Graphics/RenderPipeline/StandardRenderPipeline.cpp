#include "StandardRenderPipeline.h"
#include "Graphics/RenderPipeline/RenderPasses/ForwardPass.h"
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
    return true;
}

void StandardRenderPipeline::Shutdown() {
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
    
    // 2. Forward Pass
    RGResourceHandle output = ForwardPass::AddPass(*renderGraph_, scene, view, backBuffer);

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

} // namespace primal::graphics
