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
#include "MetalSync.h"
#include "Engine/Platform/PlatformTypes.h"

// 使用 Objective-C 运行时或 Metal-CPP 的桥接
#include <OSAPI/MAC/AppKit/AppKit.hpp>
#include <MetalKit/MetalKit.hpp>
#include <QuartzCore/CAMetalDrawable.hpp>
#include <iostream>
#include <objc/runtime.h>
#include <objc/message.h>

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
    
    // Handle Retina Display / High DPI
    // Get Screen Scale
    CGFloat scale = 1.0;
    
    ::id windowId = reinterpret_cast<::id>(window);
    ::SEL screenSel = sel_registerName("screen");
    
    // Cast objc_msgSend
    using ObjectMsgSend = ::id (*)(::id, ::SEL);
    auto objMsg = reinterpret_cast<ObjectMsgSend>(objc_msgSend);
    ::id screen = objMsg(windowId, screenSel);
    
    if (screen) {
        ::SEL scaleSel = sel_registerName("backingScaleFactor");
        using ScaleMsgSend = CGFloat (*)(::id, ::SEL);
        auto scaleMsg = reinterpret_cast<ScaleMsgSend>(objc_msgSend);
        scale = scaleMsg(screen, scaleSel);
    }
    
    // Set Layer Contents Scale
    ::id viewId = reinterpret_cast<::id>(mtkView_);
    ::SEL layerSel = sel_registerName("layer");
    ::id layer = objMsg(viewId, layerSel);
    
    if (layer) {
        ::SEL setScaleSel = sel_registerName("setContentsScale:");
        using SetScaleMsgSend = void (*)(::id, ::SEL, CGFloat);
        auto setScaleMsg = reinterpret_cast<SetScaleMsgSend>(objc_msgSend);
        setScaleMsg(layer, setScaleSel, scale);
    }
    
    // Update SwapChainDesc with actual drawable size
    CGSize drawableSize = mtkView_->drawableSize();
    swapChainDesc_.width = static_cast<uint32_t>(drawableSize.width);
    swapChainDesc_.height = static_cast<uint32_t>(drawableSize.height);
    
    CGRect windowFrame = window->frame();
    std::cout << "[MetalSwapChain] Window Logical Size: " << windowFrame.size.width << "x" << windowFrame.size.height 
              << ", Scale: " << scale 
              << ", Drawable Size: " << drawableSize.width << "x" << drawableSize.height << std::endl;
    
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

bool MetalSwapChain::AcquireNextImage(uint32_t* imageIndex, SyncHandle semaphore, SyncHandle fence) {
    // 1. 获取 Drawable
    if (!currentDrawable_) {
        GetCurrentDrawable();
    }
    if (!currentDrawable_) return false;

    // 2. 更新当前帧索引
    uint32_t index = currentFrameIndex_;
    if (imageIndex) {
        *imageIndex = index;
    }

    // 3. 更新 Texture Handle 的底层资源
    if (index < backBufferHandles_.size()) {
        ResourceHandle handle = backBufferHandles_[index];
        MetalTexture* texture = metalDevice_.GetTexture(handle);
        if (texture) {
            // Cast MTL::Drawable to CA::MetalDrawable to access texture
            CA::MetalDrawable* metalDrawable = reinterpret_cast<CA::MetalDrawable*>(currentDrawable_);
            MTL::Texture* mtlTex = metalDrawable->texture();
            texture->SetNativeTexture(mtlTex);
        } else {
            std::cerr << "[MetalSwapChain] Failed to get texture for handle " << handle << std::endl;
        }
    }

    // 4. 处理信号量同步
    if (semaphore != handles::INVALID_SYNC) {
        MetalSync* sync = metalDevice_.GetSync(semaphore);
        if (sync) {
            // 简单递增信号量值，模拟 Signal 行为
            uint64_t val = sync->GetValue();
            sync->SetValue(val + 1);
        }
    }

    return true;
}

void MetalSwapChain::Present(SyncHandle semaphore) {
    if (!currentDrawable_) return;
    
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
    
    // 等待渲染完成信号量
    if (semaphore != handles::INVALID_SYNC) {
        MetalSync* sync = metalDevice_.GetSync(semaphore);
        if (sync && sync->GetNativeEvent()) {
             // 等待当前信号量的值
             cmdBuffer->encodeSignalEvent(sync->GetNativeEvent(), sync->GetValue());
        }
    }
    
    cmdBuffer->presentDrawable(currentDrawable_);
    cmdBuffer->commit();
    
    // static int presentCounter = 0;
    // if (presentCounter++ % 60 == 0) {
    //     std::cout << "[MetalSwapChain] Presented frame " << currentFrameIndex_ << std::endl;
    // }

    mtkView_->draw();
    
    // 更新帧索引
    currentFrameIndex_ = (currentFrameIndex_ + 1) % swapChainDesc_.bufferCount;
    
    // 释放当前 drawable
    currentDrawable_->release();
    currentDrawable_ = nullptr;
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
