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

MaterialInstance* RenderSystem::GetMaterialInstance(id::id_type id) const {
    auto it = materialInstances_.find(id);
    if (it != materialInstances_.end()) {
        return it->second;
    }
    return nullptr;
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

void RenderSystem::Resize(uint32_t width, uint32_t height) {
    if (swapChain_) {
        device_->WaitIdle();
        swapChain_->Resize(width, height);
        
        // Resize Depth Buffer
        if (depthStencilTexture_ != rhi::handles::INVALID_RESOURCE) {
            device_->DestroyTexture(depthStencilTexture_);
        }
        
        rhi::TextureDesc depthDesc{};
        depthDesc.size.x = width;
        depthDesc.size.y = height;
        depthDesc.size.z = 1;
        depthDesc.format = rhi::DataFormat::D32_Float;
        depthDesc.type = rhi::TextureType::Texture2D;
        depthDesc.usage = rhi::TextureUsage::DepthStencil;
        depthDesc.memoryUsage = rhi::GPUMemoryUsage::Static;
        
        depthStencilTexture_ = device_->CreateTexture(depthDesc);
    }
}

bool RenderSystem::BeginFrame(rhi::ResourceHandle& outBackBuffer, rhi::SyncHandle& outSignalFence) {
    if (!device_ || !swapChain_) return false;

    // Internal Frame Sync
    Wait(currentFrameIndex_);

    // Acquire Next Image
    if (!swapChain_->AcquireNextImage(&currentImageIndex_)) {
        std::cerr << "RenderSystem: Failed to acquire next image." << std::endl;
        return false;
    }
    
    outBackBuffer = swapChain_->GetBackBuffer(currentImageIndex_);
    outSignalFence = frameFences_[currentFrameIndex_];
    return true;
}

void RenderSystem::EndFrame() {
    if (swapChain_) {
        swapChain_->Present(rhi::handles::INVALID_SYNC);
        // Advance frame index
        currentFrameIndex_ = (currentFrameIndex_ + 1) % rhi::MAX_FRAMES_IN_FLIGHT;
    }
}

rhi::TextureDesc RenderSystem::GetBackBufferDesc() const {
    rhi::TextureDesc desc{};
    if (swapChain_) {
        const auto& scDesc = swapChain_->GetDesc();
        desc.size.x = scDesc.width;
        desc.size.y = scDesc.height;
        desc.size.z = 1;
        desc.format = scDesc.format;
        desc.type = rhi::TextureType::Texture2D;
        desc.usage = rhi::TextureUsage::RenderTarget;
        desc.arraySize = 1;
        desc.mipLevels = 1;
    }
    return desc;
}

void RenderSystem::Render(RenderScene& scene, RenderView& view) {
    // Note: frameIndex argument is now effectively ignored for sync purposes 
    // as RenderSystem manages it internally via BeginFrame/EndFrame, 
    // but we can still use it for passed-in state if needed.
    // Ideally, we should deprecate the frameIndex argument or verify it matches.
    
    static uint64_t frameCount = 0;
    frameCount++;
    auto renderStart = std::chrono::high_resolution_clock::now();

    rhi::ResourceHandle backBuffer;
    rhi::SyncHandle signalFence;
    if (!BeginFrame(backBuffer, signalFence)) {
        return;
    }

    if (cmdBuffers_.empty() || !cmdBuffers_[currentFrameIndex_]) {
        std::cerr << "RenderSystem: Invalid command buffer state." << std::endl;
        return;
    }

    rhi::RHICommandBuffer* cmdBuffer = cmdBuffers_[currentFrameIndex_];
    rhi::CommandBufferHandle cmdBufferHandle = cmdBufferHandles_[currentFrameIndex_];

    // 0. Update GC
    auto gcStart = std::chrono::high_resolution_clock::now();
    {
        auto& gc = device_->GetGarbageCollector();
        gc.SetCurrentFrame(frameCount);
        
        if (frameCount >= rhi::MAX_FRAMES_IN_FLIGHT) {
            gc.Update(frameCount - rhi::MAX_FRAMES_IN_FLIGHT, 2.0);
        }
    }
    auto gcEnd = std::chrono::high_resolution_clock::now();

    // 2. Update View & 3. Cull
    auto cullStart = std::chrono::high_resolution_clock::now();
    view.UpdateFrustum();
    view.Cull(scene);
    auto cullEnd = std::chrono::high_resolution_clock::now();

    // 4. Command Buffer Recording
    auto recordStart = std::chrono::high_resolution_clock::now();
    
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
    forwardRenderer_.Render(cmdBuffer, scene, view, backBuffer, depthStencilTexture_, materialInstances_, currentFrameIndex_, swapDesc.width, swapDesc.height);
    cmdBuffer->End();
    auto recordEnd = std::chrono::high_resolution_clock::now();
    
    // Submit
    rhi::QueueSubmitInfo submitInfo{};
    submitInfo.cmdBuffer = cmdBufferHandle;
    submitInfo.signalFence = frameFences_[currentFrameIndex_]; // Use current frame fence
    auto submitStart = std::chrono::high_resolution_clock::now();
    device_->Submit(submitInfo);
    auto submitEnd = std::chrono::high_resolution_clock::now();
    
    // 5. Present
    auto presentStart = std::chrono::high_resolution_clock::now();
    EndFrame();
    auto presentEnd = std::chrono::high_resolution_clock::now();

    auto renderEnd = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> renderTime = renderEnd - renderStart;
}


} // namespace primal::graphics
