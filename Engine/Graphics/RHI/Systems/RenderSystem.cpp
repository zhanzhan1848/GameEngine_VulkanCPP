/**
 * @file RenderSystem.cpp
 * @brief 渲染系统核心实现
 * @details 实现渲染循环、Command Buffer 录制和 Draw Call 提交
 * @author GameEngine VulkanCPP Team
 * @date 2026-01-12
 */

#include "RenderSystem.h"
#include "Graphics/RenderMesh.h"
#include "Graphics/RenderView.h"
#include "Graphics/RenderScene.h"
#include "Graphics/MaterialInstance.h"
#include "Graphics/RHI/Core/RHITypes.h"
#include <iostream>
#include <chrono>

namespace primal::graphics {

RenderSystem::RenderSystem() = default;
RenderSystem::~RenderSystem() = default;

bool RenderSystem::Initialize(const RenderSystemInitInfo& info) {
    if (!info.device) {
        std::cerr << "RenderSystem::Initialize failed: Device is null." << std::endl;
        return false;
    }
    device_ = info.device;

    // Create SwapChain
    rhi::SwapChainDesc swapChainDesc;
    swapChainDesc.window = info.window;
    swapChainDesc.width = info.width;
    swapChainDesc.height = info.height;
    swapChainDesc.format = rhi::DataFormat::BGRA8_UNorm; // Or appropriate format
    swapChainDesc.bufferCount = 3;
    swapChainDesc.presentMode = rhi::PresentMode::FIFO;

    swapChain_ = device_->CreateSwapChain(swapChainDesc);
    if (!swapChain_) {
        std::cerr << "RenderSystem::Initialize failed: Could not create swap chain." << std::endl;
        return false;
    }
    
    if (!swapChain_->Initialize()) {
         std::cerr << "RenderSystem::Initialize failed: Could not initialize swap chain." << std::endl;
         return false;
    }

    // Update dimensions from initialized SwapChain (handles High DPI)
    const auto& scDesc = swapChain_->GetDesc();
    uint32_t width = scDesc.width;
    uint32_t height = scDesc.height;

    // Create Depth Stencil Texture
    rhi::TextureDesc depthDesc{};
    depthDesc.size.x = width;
    depthDesc.size.y = height;
    depthDesc.size.z = 1;
    depthDesc.format = rhi::DataFormat::D32_Float; // Assume D32 support for Metal
    depthDesc.type = rhi::TextureType::Texture2D;
    depthDesc.usage = rhi::TextureUsage::DepthStencil;
    depthDesc.memoryUsage = rhi::GPUMemoryUsage::Static;
    
    depthStencilTexture_ = device_->CreateTexture(depthDesc);
    if (depthStencilTexture_ == rhi::handles::INVALID_RESOURCE) {
        std::cerr << "RenderSystem::Initialize failed: Could not create depth stencil texture." << std::endl;
        return false;
    }

    // Create Command Buffers for Multi-Buffering
    cmdBufferHandles_.resize(rhi::MAX_FRAMES_IN_FLIGHT, rhi::handles::INVALID_COMMAND_BUFFER);
    cmdBuffers_.resize(rhi::MAX_FRAMES_IN_FLIGHT, nullptr);
    frameFences_.resize(rhi::MAX_FRAMES_IN_FLIGHT, rhi::handles::INVALID_SYNC);

    for (uint32_t i = 0; i < rhi::MAX_FRAMES_IN_FLIGHT; ++i) {
        cmdBufferHandles_[i] = device_->CreateCommandBuffer(rhi::CommandQueueType::Graphics);
        if (cmdBufferHandles_[i] == rhi::handles::INVALID_COMMAND_BUFFER) {
            std::cerr << "RenderSystem::Initialize failed: Could not create command buffer " << i << std::endl;
            return false;
        }
        cmdBuffers_[i] = rhi::GetCommandBuffer(cmdBufferHandles_[i]);
        if (!cmdBuffers_[i]) {
            std::cerr << "RenderSystem::Initialize failed: Could not retrieve command buffer pointer for " << i << std::endl;
            return false;
        }

        frameFences_[i] = device_->CreateSync();
        if (frameFences_[i] == rhi::handles::INVALID_SYNC) {
             std::cerr << "RenderSystem::Initialize failed: Could not create fence " << i << std::endl;
             return false;
        }
    }

    if (!forwardRenderer_.Initialize(device_)) {
        std::cerr << "RenderSystem::Initialize failed: ForwardRenderer initialization failed." << std::endl;
        return false;
    }

    return true;
}

void RenderSystem::Shutdown() {
    forwardRenderer_.Shutdown();

    if (depthStencilTexture_ != rhi::handles::INVALID_RESOURCE) {
        device_->DestroyTexture(depthStencilTexture_);
        depthStencilTexture_ = rhi::handles::INVALID_RESOURCE;
    }

    if (swapChain_) {
        device_->DestroySwapChain(swapChain_);
        swapChain_ = nullptr;
    }
    
    for (uint32_t i = 0; i < cmdBuffers_.size(); ++i) {
        if (cmdBuffers_[i]) {
            cmdBuffers_[i]->Destroy();
            cmdBuffers_[i] = nullptr;
        }
        cmdBufferHandles_[i] = rhi::handles::INVALID_COMMAND_BUFFER;
    }
    cmdBufferHandles_.clear();
    cmdBuffers_.clear();

    for (auto fence : frameFences_) {
        device_->DestroySync(fence);
    }
    frameFences_.clear();

    device_ = nullptr;
}

void RenderSystem::RegisterMaterialInstance(id::id_type id, MaterialInstance* materialInstance) {
    if (materialInstance) {
        materialInstances_[id] = materialInstance;
    }
}

void RenderSystem::Wait(uint32_t frameIndex) {
    if (!device_ || frameFences_.empty()) return;
    
    uint32_t idx = frameIndex % rhi::MAX_FRAMES_IN_FLIGHT;
    rhi::SyncHandle fence = frameFences_[idx];
    if (fence != rhi::handles::INVALID_SYNC) {
        auto waitStart = std::chrono::high_resolution_clock::now();
        if (!device_->WaitForSync(fence, 1000)) {
            std::cerr << "RenderSystem: Wait timeout for frame " << frameIndex << std::endl;
        }
        auto waitEnd = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::milli> waitTime = waitEnd - waitStart;
        if (waitTime.count() > 5.0) {
            std::cout << "[Performance] RenderSystem::Wait blocked: " << waitTime.count() << " ms (Frame " << frameIndex << ")" << std::endl;
        }
    }
}

void RenderSystem::Render(RenderScene& scene, RenderView& view, uint32_t frameIndex) {
    static uint64_t frameCount = 0;
    frameCount++;
    auto renderStart = std::chrono::high_resolution_clock::now();

    // Ensure frameIndex is within bounds
    currentFrameIndex_ = frameIndex % rhi::MAX_FRAMES_IN_FLIGHT;

    if (!device_ || cmdBuffers_.empty() || !cmdBuffers_[currentFrameIndex_] || !swapChain_) {
        std::cerr << "RenderSystem: Invalid state." << std::endl;
        return;
    }

    rhi::RHICommandBuffer* cmdBuffer = cmdBuffers_[currentFrameIndex_];
    rhi::CommandBufferHandle cmdBufferHandle = cmdBufferHandles_[currentFrameIndex_];

    // 0. Update GC
    auto gcStart = std::chrono::high_resolution_clock::now();
    {
        auto& gc = device_->GetGarbageCollector();
        gc.SetCurrentFrame(frameCount);
        
        // In triple buffering, frame N-3 is definitely done if we are starting frame N.
        if (frameCount >= rhi::MAX_FRAMES_IN_FLIGHT) {
            // Budget GC time to 2.0ms to prevent spikes
            gc.Update(frameCount - rhi::MAX_FRAMES_IN_FLIGHT, 2.0);
        }
    }
    auto gcEnd = std::chrono::high_resolution_clock::now();

    // 1. Acquire Next Image
    uint32_t imageIndex = 0;
    auto acquireStart = std::chrono::high_resolution_clock::now();
    if (!swapChain_->AcquireNextImage(&imageIndex)) {
        std::cerr << "RenderSystem: Failed to acquire next image." << std::endl;
        return;
    }
    auto acquireEnd = std::chrono::high_resolution_clock::now();

    // 2. Update View & 3. Cull
    auto cullStart = std::chrono::high_resolution_clock::now();
    view.UpdateFrustum();
    view.Cull(scene);
    auto cullEnd = std::chrono::high_resolution_clock::now();

    // 4. Command Buffer Recording
    auto recordStart = std::chrono::high_resolution_clock::now();
    
    // Wait for the PREVIOUS use of this specific command buffer/frame resources to finish.
    rhi::SyncHandle fence = frameFences_[currentFrameIndex_];
    double fenceWaitTimeMs = 0.0;
    if (fence != rhi::handles::INVALID_SYNC) {
        auto waitStart = std::chrono::high_resolution_clock::now();
        if (!device_->WaitForSync(fence, 1000)) {
            static bool loggedTimeout = false;
            if (!loggedTimeout) {
                 std::cerr << "RenderSystem: WaitForSync timed out for frame " << currentFrameIndex_ << " (Logged once)" << std::endl;
                 loggedTimeout = true;
            }
        }
        auto waitEnd = std::chrono::high_resolution_clock::now();
        fenceWaitTimeMs = std::chrono::duration<double, std::milli>(waitEnd - waitStart).count();

        if (cmdBuffer->GetState() == rhi::CommandBufferState::Submitted) {
            cmdBuffer->SetState(rhi::CommandBufferState::Executed);
        }
    }

    if (!cmdBuffer->Reset()) {
        std::cerr << "RenderSystem: Failed to reset command buffer." << std::endl;
        return;
    }

    if (!cmdBuffer->Begin()) {
        std::cerr << "RenderSystem: Failed to begin command buffer." << std::endl;
        return;
    }

    // Forward Rendering
    const auto& swapDesc = swapChain_->GetDesc();
    rhi::ResourceHandle backBuffer = swapChain_->GetBackBuffer(imageIndex);
    forwardRenderer_.Render(cmdBuffer, scene, view, backBuffer, depthStencilTexture_, materialInstances_, currentFrameIndex_, swapDesc.width, swapDesc.height);
    cmdBuffer->End();
    auto recordEnd = std::chrono::high_resolution_clock::now();
    
    // Submit
    rhi::QueueSubmitInfo submitInfo{};
    submitInfo.cmdBuffer = cmdBufferHandle;
    submitInfo.signalFence = fence;
    auto submitStart = std::chrono::high_resolution_clock::now();
    device_->Submit(submitInfo);
    auto submitEnd = std::chrono::high_resolution_clock::now();
    
    // 5. Present
    auto presentStart = std::chrono::high_resolution_clock::now();
    swapChain_->Present(rhi::handles::INVALID_SYNC);
    auto presentEnd = std::chrono::high_resolution_clock::now();

    auto renderEnd = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> renderTime = renderEnd - renderStart;
    
    // Detailed profiling log
    // Only log if total frame time > 20ms (dropping below 50 FPS) or periodically
    static int logCounter = 0;
    logCounter++;
    
    /*
    if (renderTime.count() > 20.0 || logCounter % 60 == 0) {
        std::chrono::duration<double, std::milli> gcTime = gcEnd - gcStart;
        std::chrono::duration<double, std::milli> acquireTime = acquireEnd - acquireStart;
        std::chrono::duration<double, std::milli> cullTime = cullEnd - cullStart;
        std::chrono::duration<double, std::milli> recordTime = recordEnd - recordStart;
        std::chrono::duration<double, std::milli> submitTime = submitEnd - submitStart;
        std::chrono::duration<double, std::milli> presentTime = presentEnd - presentStart;
        
        double cpuWorkTime = renderTime.count() - fenceWaitTimeMs - acquireTime.count();

        std::cout << "[Profile] Frame " << frameCount 
                  << " | Total: " << renderTime.count() << "ms"
                  << " | Wait: " << fenceWaitTimeMs << "ms"
                  << " | Acquire: " << acquireTime.count() << "ms"
                  << " | Work: " << cpuWorkTime << "ms"
                  << " | GC: " << gcTime.count() << "ms"
                  << std::endl;
    }
    */
}


} // namespace primal::graphics
