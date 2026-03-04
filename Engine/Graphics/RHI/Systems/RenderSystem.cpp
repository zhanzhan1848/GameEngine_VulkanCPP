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
#include "Graphics/Material.h"
#include "Graphics/RHI/Core/RHITypes.h"
#include <iostream>
#include <chrono>
#include <algorithm>

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
    u32 width = scDesc.width;
    u32 height = scDesc.height;

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

    for (u32 i = 0; i < rhi::MAX_FRAMES_IN_FLIGHT; ++i) {
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

    RHISystem::Initialize(info.entityManager, info.device);

    return true;
}

void RenderSystem::Shutdown() {
    std::cout << "[RenderSystem] Shutdown Start" << std::endl;
    forwardRenderer_.Shutdown();
    std::cout << "[RenderSystem] ForwardRenderer Shutdown Done" << std::endl;

    if (depthStencilTexture_ != rhi::handles::INVALID_RESOURCE) {
        std::cout << "[RenderSystem] Destroying DepthStencilTexture" << std::endl;
        device_->DestroyTexture(depthStencilTexture_);
        depthStencilTexture_ = rhi::handles::INVALID_RESOURCE;
    }

    if (swapChain_) {
        std::cout << "[RenderSystem] Destroying SwapChain" << std::endl;
        device_->DestroySwapChain(swapChain_);
        swapChain_ = nullptr;
    }
    
    std::cout << "[RenderSystem] Destroying CommandBuffers" << std::endl;
    for (u32 i = 0; i < cmdBuffers_.size(); ++i) {
        if (cmdBuffers_[i]) {
            cmdBuffers_[i]->Destroy();
            cmdBuffers_[i] = nullptr;
        }
        cmdBufferHandles_[i] = rhi::handles::INVALID_COMMAND_BUFFER;
    }
    cmdBufferHandles_.clear();
    cmdBuffers_.clear();

    std::cout << "[RenderSystem] Destroying Fences" << std::endl;
    for (auto fence : frameFences_) {
        device_->DestroySync(fence);
    }
    frameFences_.clear();

    device_ = nullptr;
    std::cout << "[RenderSystem] Shutdown End" << std::endl;
}

void RenderSystem::RegisterMaterialInstance(id::id_type id, std::shared_ptr<MaterialInstance> materialInstance) {
    if (materialInstance) {
        materialInstances_[id] = materialInstance;
    }
}

std::shared_ptr<MaterialInstance> RenderSystem::GetMaterialInstance(id::id_type id) const {
    auto it = materialInstances_.find(id);
    if (it != materialInstances_.end()) {
        return it->second;
    }
    return nullptr;
}

void RenderSystem::Wait(u32 frameIndex) {
    if (!device_ || frameFences_.empty()) return;
    
    u32 idx = frameIndex % rhi::MAX_FRAMES_IN_FLIGHT;
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

void RenderSystem::Resize(u32 width, u32 height) {
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
    
    static u64 frameCount = 0;
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

void RenderSystem::Update(float deltaTime) {
    // 可以在这里更新 RHI 实体状态，例如动画、物理模拟同步等
}

void RenderSystem::Render(rhi::RHICommandBuffer* cmdBuffer) {
    if (!entityManager || !cmdBuffer) return;

    // 检查是否有有效的 RenderPass，这对于获取 Pipeline 是必须的
    if (currentRenderPass_ == rhi::handles::INVALID_RESOURCE) {
        // 在开发阶段，可能有些测试没有设置 RenderPass，这里暂时允许继续，
        // 但实际渲染时 Material::GetPipeline 可能会失败或使用缓存
        // std::cerr << "RenderSystem::Render Warning: No current render pass set." << std::endl;
    }

    // 获取所有活动实体
    u32 maxEntities = entityManager->GetMaxEntityIndex();
    
    struct DrawCall {
        rhi::RHIEntityID entity;
        u32 layer;
        u32 priority;
        rhi::MaterialComponent* material;
        rhi::GPUBufferComponent* buffer;
        rhi::RenderLayerComponent* renderLayer;
        rhi::RHITransformComponent* transform;
    };

    // 使用 vector 存储 DrawCall，避免频繁分配
    // 可以考虑将其作为成员变量以重用内存
    static utl::vector<DrawCall> drawCalls;
    drawCalls.clear();
    drawCalls.reserve(maxEntities);

    // 1. 遍历实体，收集 DrawCall
    for (u32 i = 0; i < maxEntities; ++i) {
        // 检查实体是否存在且具有必要组件
        if (!entityManager->IsAlive(i)) continue;

        auto* renderLayer = entityManager->GetComponent<rhi::RenderLayerComponent>(i);
        if (!renderLayer) continue;

        auto* material = entityManager->GetComponent<rhi::MaterialComponent>(i);
        if (!material || !material->materialInstance) continue;

        auto* buffer = entityManager->GetComponent<rhi::GPUBufferComponent>(i);
        if (!buffer || buffer->vertexBuffer == rhi::handles::INVALID_RESOURCE) continue;

        auto* transform = entityManager->GetComponent<rhi::RHITransformComponent>(i);
        // Transform is optional but highly recommended. If missing, use Identity?
        // For now, let's require it or just pass nullptr and handle later.
        
        // TODO: 可见性剔除 (Culling)
        // 需要传入 RenderView 或 ViewFrustum
        // 目前简单的根据 layerMask 进行过滤 (假设 viewMask 全 1)
        // if ((renderLayer->layerMask & viewMask) == 0) continue;

        drawCalls.push_back({
            i, 
            renderLayer->layerMask, 
            static_cast<u32>(renderLayer->priority), 
            material, 
            buffer, 
            renderLayer,
            transform
        });
    }

    if (drawCalls.empty()) return;

    // 2. 排序 (Layer -> Priority -> Material -> Mesh)
    std::sort(drawCalls.begin(), drawCalls.end(), [](const DrawCall& a, const DrawCall& b) {
        if (a.layer != b.layer) return a.layer < b.layer;
        if (a.priority != b.priority) return a.priority < b.priority;
        // 简单按材质指针排序以减少状态切换
        if (a.material->materialInstance.get() != b.material->materialInstance.get()) 
            return a.material->materialInstance.get() < b.material->materialInstance.get();
        return a.entity < b.entity;
    });

    // 3. 执行绘制
    primal::graphics::MaterialInstance* currentMaterialInst = nullptr;
    primal::graphics::Material* currentMaterial = nullptr;
    rhi::PipelineHandle currentPipeline = rhi::handles::INVALID_PIPELINE;

    for (const auto& dc : drawCalls) {
        auto* matInst = dc.material->materialInstance.get();
        auto* mat = matInst->GetMaterial();

        if (!mat) continue;

        // 绑定材质（Pipeline 和 DescriptorSet）
        if (matInst != currentMaterialInst) {
            currentMaterialInst = matInst;
            
            // 绑定 Pipeline
            if (mat != currentMaterial) {
                currentMaterial = mat;
                
                // 获取 Pipeline
                // 需要 RenderPassHandle，如果 currentRenderPass_ 无效，这里可能无法创建正确的 Pipeline
                // PipelineFlags 暂时设为 None，后续应根据 RenderLayer 或 Pass 类型设置 (如 Shadow, DepthOnly)
                currentPipeline = mat->GetPipeline(device_, currentRenderPass_, 0, PipelineFlags::None);
                
                if (currentPipeline != rhi::handles::INVALID_PIPELINE) {
                    cmdBuffer->BindGraphicsPipeline(currentPipeline);
                }
            }

            // 绑定 DescriptorSet
            // Material 对应的 Set 索引通常是 2 (0: Global, 1: Pass, 2: Material, 3: Object)
            // 需要从 Material 获取 PipelineLayout
            rhi::PipelineLayoutHandle pipelineLayout = mat->GetPipelineLayout();
            rhi::DescriptorSetHandle set = matInst->GetDescriptorSet();
            
            if (pipelineLayout != rhi::handles::INVALID_RESOURCE && set != rhi::handles::INVALID_RESOURCE) {
                // 绑定 Set 2
                rhi::DescriptorSetHandle sets[] = { set };
                cmdBuffer->BindDescriptorSets(rhi::PipelineBindPoint::Graphics, pipelineLayout, 2, 1, sets, 0, nullptr);
            }
        }

        // 绑定顶点缓冲
        rhi::ResourceHandle vbs[] = { dc.buffer->vertexBuffer };
        u64 offsets[] = { dc.buffer->offset };
        // 假设 Binding 0 是 Vertex Buffer
        cmdBuffer->BindVertexBuffers(0, 1, vbs, offsets);

        // 绑定索引缓冲并绘制
        if (dc.buffer->indexBuffer != rhi::handles::INVALID_RESOURCE) {
            cmdBuffer->BindIndexBuffer(dc.buffer->indexBuffer, dc.buffer->indexType, dc.buffer->indexOffset);
            cmdBuffer->DrawIndexed(dc.buffer->indexCount, 0, 0, 1, 0);
        } else {
            cmdBuffer->Draw(dc.buffer->vertexCount, 0, 1, 0);
        }
    }
}

} // namespace primal::graphics
