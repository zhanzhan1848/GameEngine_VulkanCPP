/**
 * @file MetalSwapChain.cpp
 * @brief Metal 交换链实现
 * @author GameEngine VulkanCPP Team
 * @date 2026-01-07
 * @version 0.1.0
 */

#include "MetalSwapChain.h"
#include "MetalDevice.h"
#include "MetalTexture.h"
#include "Engine/Platform/PlatformTypes.h"

// 使用 Objective-C 运行时或 Metal-CPP 的桥接
#include <OSAPI/MAC/AppKit/AppKit.hpp>
#include <MetalKit/MetalKit.hpp>
#include <QuartzCore/CAMetalDrawable.hpp>
#include <iostream>
#include <objc/runtime.h>

namespace primal::graphics::rhi {

namespace {
    bool IsMTKView(NS::View* view) {
        if (!view) return false;
        // 使用 runtime 检查是否响应 setDevice: selector
        Class cls = object_getClass((::id)view);
        SEL sel = sel_registerName("setDevice:");
        return class_respondsToSelector(cls, sel);
    }
}

MetalSwapChain::MetalSwapChain(MetalDevice& device, const SwapChainDesc& desc)
    : RHISwapChain(device, desc), metalDevice_(device) {
}

MetalSwapChain::~MetalSwapChain() {
    Destroy();
}

bool MetalSwapChain::Initialize() {
    // 1. 获取窗口句柄
    if (!swapChainDesc_.window) {
        std::cerr << "[MetalSwapChain] Invalid window handle." << std::endl;
        return false;
    }

    NS::Window* window = static_cast<NS::Window*>(swapChainDesc_.window);
    
    // 2. 获取或创建 MTKView
    NS::View* contentView = window->contentView();
    
    // 检查 contentView 是否已经是 MTKView
    if (IsMTKView(contentView)) {
        mtkView_ = reinterpret_cast<MTK::View*>(contentView);
        mtkView_->setDevice(metalDevice_.GetNativeDevice());
    } else {
        // 如果不是 MTKView，创建一个新的
        CGRect frame = window->frame();
        frame.origin = {0, 0}; // Relative to window
        mtkView_ = MTK::View::alloc()->init(frame, metalDevice_.GetNativeDevice());
        window->setContentView(mtkView_);
        
        // 如果之前有 contentView，可能需要迁移子视图等（这里简化处理，直接替换）
    }

    if (!mtkView_) {
        std::cerr << "[MetalSwapChain] Failed to create MTKView." << std::endl;
        return false;
    }
    
    // 3. 配置 MTKView
    mtkView_->setColorPixelFormat(MTL::PixelFormatBGRA8Unorm); // 对应 DataFormat::BGRA8_UNorm
    mtkView_->setDepthStencilPixelFormat(MTL::PixelFormatDepth32Float_Stencil8); // 示例
    mtkView_->setClearColor(MTL::ClearColor::Make(0.0, 0.0, 0.0, 1.0));
    // 禁用内部循环，完全由外部控制渲染
    mtkView_->setPaused(true);
    mtkView_->setEnableSetNeedsDisplay(true);
    
    window->makeKeyAndOrderFront(nullptr);
    
    // 4. 创建后台缓冲区句柄
    // 我们预分配 bufferCount 个 ResourceHandle
    backBufferHandles_.resize(swapChainDesc_.bufferCount);
    
    for (uint32_t i = 0; i < swapChainDesc_.bufferCount; ++i) {
        // 创建一个空的 TextureDesc
        TextureDesc texDesc;
        texDesc.size.x = swapChainDesc_.width;
        texDesc.size.y = swapChainDesc_.height;
        texDesc.size.z = 1;
        texDesc.format = swapChainDesc_.format;
        texDesc.usage = TextureUsage::RenderTarget;
        texDesc.memoryUsage = GPUMemoryUsage::SwapChain;
        texDesc.type = TextureType::Texture2D;
        
        // 使用 Device 创建一个 "占位" 纹理资源
        // 注意：这里我们可能需要扩展 MetalDevice::CreateTexture 来支持 "外部" 或 "占位" 纹理
        // 或者我们直接创建并手动设置
        
        // 这是一个 hack：我们创建一个正常的纹理句柄，但在使用时替换其底层 MTL::Texture
        // 或者 MetalTexture 类支持 SetNativeTexture
        
        ResourceHandle handle = metalDevice_.CreateTexture(texDesc);
        backBufferHandles_[i] = handle;
    }
    
    return true;
}

void MetalSwapChain::Destroy() {
    for (auto handle : backBufferHandles_) {
        metalDevice_.DestroyTexture(handle);
    }
    backBufferHandles_.clear();
    
    if (mtkView_) {
        mtkView_->release();
        mtkView_ = nullptr;
    }
    
    currentDrawable_ = nullptr;
    RHISwapChain::Destroy();
}

void MetalSwapChain::Present(bool vsync) {
    if (!currentDrawable_) {
        GetCurrentDrawable();
    }
    
    if (!currentDrawable_) return;
    
    // 获取当前命令缓冲区
    // 在 Metal 中，Present 通常是 CommandBuffer 的最后一个操作
    // 我们需要获取一个 CommandBuffer 来执行 Present
    
    // 这里有两种策略：
    // 1. 从 MetalDevice 获取当前正在录制的 CommandBuffer（如果有）
    // 2. 创建一个新的 CommandBuffer 仅用于 Present
    
    // 用户提供的代码片段显示 Present 是添加到 cmdBuffer 的 scheduled handler
    // _cmd_buffer->addScheduledHandler(...)
    
    // 我们假设 MetalDevice 维护了当前的 CommandBuffer
    // 但是 MetalDevice 只有 CreateCommandBuffer，没有 GetCurrent
    
    // 简单实现：创建一个新的 CommandBuffer 用于 Present
    // 这可能不是最高效的，但能工作
    MTL::CommandQueue* queue = metalDevice_.GetGraphicsQueue();
    if (!queue) {
        std::cerr << "[MetalSwapChain] Graphics queue is null!" << std::endl;
        return;
    }
    MTL::CommandBuffer* cmdBuffer = queue->commandBuffer();
    if (!cmdBuffer) {
        std::cerr << "[MetalSwapChain] Failed to create command buffer!" << std::endl;
        return;
    }
    
    if (!mtkView_) {
        std::cerr << "[MetalSwapChain] mtkView_ is null!" << std::endl;
        return;
    }

    MTL::Drawable* drawable = currentDrawable_;
    if (!drawable) {
        std::cerr << "[MetalSwapChain] currentDrawable_ is null!" << std::endl;
        return;
    }
    
    // std::cout << "[MetalSwapChain] Presenting drawable..." << std::endl;
    cmdBuffer->presentDrawable(drawable);
    cmdBuffer->commit();
    
    if (vsync) {
        cmdBuffer->waitUntilCompleted();
    }
    
    // 在手动渲染模式下，必须调用 draw() 来更新 currentDrawable 到下一帧
    // 这确保了下一次调用 currentDrawable 时能获取到新的 drawable
    // 而不是已经 presented 的旧 drawable
    if (mtkView_) {
        mtkView_->draw();
    }

    // 释放当前 drawable
    // std::cout << "[MetalSwapChain] Releasing drawable..." << std::endl;
    currentDrawable_->release();
    currentDrawable_ = nullptr;
    
    // 更新帧索引
    currentFrameIndex_ = (currentFrameIndex_ + 1) % swapChainDesc_.bufferCount;
}

void MetalSwapChain::Resize(uint32_t width, uint32_t height) {
    RHISwapChain::Resize(width, height);
    if (mtkView_) {
        // MTKView 自动处理 resize，或者我们需要更新 drawableSize
        CGSize size;
        size.width = width;
        size.height = height;
        mtkView_->setDrawableSize(size);
    }
}

bool MetalSwapChain::AcquireNextImage(uint32_t* imageIndex, SyncHandle semaphore, SyncHandle fence) {
    // 确保获取了当前的可绘制对象
    // 这对于 Metal 很重要，因为我们需要在渲染之前获取 Drawable 并更新底层的 Native Texture
    if (!GetCurrentDrawable()) {
        std::cerr << "[MetalSwapChain] Failed to acquire next drawable" << std::endl;
        return false;
    }
    
    // 返回当前的后台缓冲区索引
    if (imageIndex) {
        *imageIndex = currentFrameIndex_;
    }
    
    return true;
}

uint32_t MetalSwapChain::GetCurrentBackBufferIndex() const {
    return currentFrameIndex_;
}

ResourceHandle MetalSwapChain::GetBackBuffer(uint32_t index) const {
    if (index >= backBufferHandles_.size()) return handles::INVALID_RESOURCE;
    return backBufferHandles_[index];
}

MTL::Drawable* MetalSwapChain::GetCurrentDrawable() {
    if (!currentDrawable_ && mtkView_) {
        currentDrawable_ = mtkView_->currentDrawable();
        if (currentDrawable_) {
            currentDrawable_->retain();
            
            // 关键：更新当前 BackBuffer Handle 对应的 MetalTexture 的底层 MTL::Texture
            ResourceHandle handle = backBufferHandles_[currentFrameIndex_];
            MetalTexture* texture = metalDevice_.GetTexture(handle);
            if (texture) {
                // 更新底层纹理
                CA::MetalDrawable* metalDrawable = static_cast<CA::MetalDrawable*>(currentDrawable_);
                texture->SetNativeTexture(metalDrawable->texture());
            }
        }
    }
    return currentDrawable_;
}

void* MetalSwapChain::mapImpl(uint64_t offset, uint64_t size) {
    return nullptr;
}

void MetalSwapChain::unmapImpl() {
}

bool MetalSwapChain::updateDataImpl(const void* data, uint64_t size, uint64_t offset) {
    return false;
}

} // namespace primal::graphics::rhi
