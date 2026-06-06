#include "DawnSwapChain.h"

#if defined(ENABLE_WEBGPU) && ENABLE_WEBGPU

#include "DawnDevice.h"
#include "DawnTexture.h"
#include "Engine/Platform/PlatformTypes.h"
#include <iostream>

#ifdef __APPLE__
#include <OSAPI/MAC/AppKit/AppKit.hpp>
#include <objc/runtime.h>
#endif

namespace primal::graphics::rhi {

// Helper to call Objective-C methods via objc_msgSend on macOS
#ifdef __APPLE__
static void* GetLayerFromView(void* viewPtr) {
    // NSView::layer() — returns the backing CALayer/CAMetalLayer
    using MsgSendFunc = void* (*)(void*, void*);
    auto msgSend = reinterpret_cast<MsgSendFunc>(&objc_msgSend);
    auto layerSel = sel_registerName("layer");
    return msgSend(viewPtr, layerSel);
}

static void SetWantsLayer(void* viewPtr, bool wants) {
    using MsgSendFunc = void (*)(void*, void*, bool);
    auto msgSend = reinterpret_cast<MsgSendFunc>(&objc_msgSend);
    auto sel = sel_registerName("setWantsLayer:");
    msgSend(viewPtr, sel, wants);
}

static void SetLayerOnView(void* viewPtr, void* layer) {
    using MsgSendFunc = void (*)(void*, void*, void*);
    auto msgSend = reinterpret_cast<MsgSendFunc>(&objc_msgSend);
    auto sel = sel_registerName("setLayer:");
    msgSend(viewPtr, sel, layer);
}

static void* CreateMetalLayer() {
    using MsgSendFunc = void* (*)(void*, void*);
    auto msgSend = reinterpret_cast<MsgSendFunc>(&objc_msgSend);
    auto metalLayerClass = objc_getClass("CAMetalLayer");
    auto allocSel = sel_registerName("alloc");
    auto initSel = sel_registerName("init");
    void* obj = msgSend(metalLayerClass, allocSel);
    return msgSend(obj, initSel);
}

static void SetLayerDevice(void* layer, void* device) {
    using MsgSendFunc = void (*)(void*, void*, void*);
    auto msgSend = reinterpret_cast<MsgSendFunc>(&objc_msgSend);
    auto sel = sel_registerName("setDevice:");
    msgSend(layer, sel, device);
}

static void SetLayerDrawableSize(void* layer, double w, double h) {
    using MsgSendFunc = void (*)(void*, void*, CGSize);
    auto msgSend = reinterpret_cast<MsgSendFunc>(&objc_msgSend);
    auto sel = sel_registerName("setDrawableSize:");
    CGSize size{w, h};
    msgSend(layer, sel, size);
}

static void* GetContentView(void* windowPtr) {
    using MsgSendFunc = void* (*)(void*, void*);
    auto msgSend = reinterpret_cast<MsgSendFunc>(&objc_msgSend);
    auto sel = sel_registerName("contentView");
    return msgSend(windowPtr, sel);
}

static bool IsKindOfClass(void* obj, void* cls) {
    using MsgSendFunc = bool (*)(void*, void*, void*);
    auto msgSend = reinterpret_cast<MsgSendFunc>(&objc_msgSend);
    auto sel = sel_registerName("isKindOfClass:");
    return msgSend(obj, sel, cls);
}
#endif

DawnSwapChain::DawnSwapChain(DawnDevice& device, const SwapChainDesc& desc)
    : RHISwapChain(device, desc)
    , device_(device)
    , windowHandle_(desc.window)
    , bufferCount_(desc.bufferCount > 0 ? desc.bufferCount : 3)
    , format_(desc.format)
    , width_(desc.width)
    , height_(desc.height) {
    backBufferHandles_.resize(bufferCount_, handles::INVALID_RESOURCE);
}

DawnSwapChain::~DawnSwapChain() {
    Destroy();
}

bool DawnSwapChain::Initialize() {
#ifdef __APPLE__
    if (!windowHandle_) {
        std::cerr << "[DawnSwapChain] No window handle provided" << std::endl;
        return false;
    }

    // Get content view from NSWindow
    void* contentView = GetContentView(windowHandle_);
    if (!contentView) {
        std::cerr << "[DawnSwapChain] No content view" << std::endl;
        return false;
    }

    // Ensure the view wants a layer
    SetWantsLayer(contentView, true);

    // Get or create a CAMetalLayer
    void* layer = GetLayerFromView(contentView);
    void* metalLayerClass = objc_getClass("CAMetalLayer");

    if (!layer || (metalLayerClass && !IsKindOfClass(layer, metalLayerClass))) {
        layer = CreateMetalLayer();
        SetLayerOnView(contentView, layer);
    }

    // Configure the Metal layer drawable size
    SetLayerDrawableSize(layer, static_cast<double>(width_), static_cast<double>(height_));

    // Make the layer opaque so macOS doesn't pass mouse events through
    {
        using MsgSendBool = void (*)(void*, void*, bool);
        auto msgSend = reinterpret_cast<MsgSendBool>(&objc_msgSend);
        msgSend(layer, sel_registerName("setOpaque:"), true);
    }

    // Create WGPU surface from Metal layer
    WGPUSurfaceSourceMetalLayer metalDesc{};
    metalDesc.chain.next = nullptr;
    metalDesc.chain.sType = WGPUSType_SurfaceSourceMetalLayer;
    metalDesc.layer = layer;

    WGPUSurfaceDescriptor surfaceDesc{};
    surfaceDesc.nextInChain = reinterpret_cast<WGPUChainedStruct*>(&metalDesc);
    surfaceDesc.label = ToWGPUStringView("DawnSurface");

    wgpuSurface_ = wgpuInstanceCreateSurface(device_.GetInstance(), &surfaceDesc);
#endif

#ifdef __EMSCRIPTEN__
    WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvasDesc{};
    canvasDesc.chain.next = nullptr;
    canvasDesc.chain.sType = WGPUSType_EmscriptenSurfaceSourceCanvasHTMLSelector;
    canvasDesc.selector = ToWGPUStringView("canvas");

    WGPUSurfaceDescriptor surfaceDesc{};
    surfaceDesc.nextInChain = reinterpret_cast<WGPUChainedStruct*>(&canvasDesc);
    surfaceDesc.label = ToWGPUStringView("EmscriptenSurface");

    wgpuSurface_ = wgpuInstanceCreateSurface(device_.GetInstance(), &surfaceDesc);
#endif

    if (!wgpuSurface_) {
        std::cerr << "[DawnSwapChain] Failed to create surface" << std::endl;
        return false;
    }

    WGPUSurfaceConfiguration config{};
    config.nextInChain = nullptr;
    config.device = device_.GetNativeDevice();
    config.format = ToWGPUTextureFormat(format_);
    config.usage = WGPUTextureUsage_RenderAttachment;
    config.width = width_;
    config.height = height_;
    config.presentMode = WGPUPresentMode_Fifo;
    config.alphaMode = WGPUCompositeAlphaMode_Opaque;

    wgpuSurfaceConfigure(wgpuSurface_, &config);
    std::cout << "[DawnSwapChain] Configured surface "
              << width_ << "x" << height_ << std::endl;
    return true;
}

void DawnSwapChain::Destroy() {
    // Release any remaining wrapped surface textures
    for (auto& handle : backBufferHandles_) {
        if (handle != handles::INVALID_RESOURCE) {
            device_.ReleaseSurfaceTexture(handle);
            handle = handles::INVALID_RESOURCE;
        }
    }

    if (currentTextureView_) {
        wgpuTextureViewRelease(currentTextureView_);
        currentTextureView_ = nullptr;
    }

    if (wgpuSurface_) {
        wgpuSurfaceUnconfigure(wgpuSurface_);
        wgpuSurfaceRelease(wgpuSurface_);
        wgpuSurface_ = nullptr;
    }
}

bool DawnSwapChain::AcquireNextImage(u32* imageIndex, SyncHandle semaphore, SyncHandle fence) {
    if (!wgpuSurface_) return false;

    (void)semaphore;
    (void)fence;

    WGPUSurfaceTexture surfaceTexture{};
    wgpuSurfaceGetCurrentTexture(wgpuSurface_, &surfaceTexture);

    if (surfaceTexture.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
        surfaceTexture.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) {
        std::cerr << "[DawnSwapChain] Failed to acquire surface texture, status: "
                  << static_cast<int>(surfaceTexture.status) << std::endl;
        return false;
    }

    currentTexture_ = surfaceTexture.texture;

    if (currentTextureView_) {
        wgpuTextureViewRelease(currentTextureView_);
    }
    currentTextureView_ = wgpuTextureCreateView(currentTexture_, nullptr);

    if (backBufferHandles_[currentFrameIndex_] == handles::INVALID_RESOURCE) {
        // First time: allocate a persistent wrapper
        backBufferHandles_[currentFrameIndex_] =
            device_.WrapSurfaceTexture(currentTexture_, width_, height_, format_);
    } else {
        // Subsequent frames: just update the native WGPU handles
        device_.UpdateSurfaceTexture(
            backBufferHandles_[currentFrameIndex_],
            currentTexture_, width_, height_, format_);
    }

    if (imageIndex) {
        *imageIndex = currentFrameIndex_;
    }

    currentFrameIndex_ = (currentFrameIndex_ + 1) % bufferCount_;

    return true;
}

void DawnSwapChain::Present(SyncHandle semaphore) {
    if (!wgpuSurface_) return;
    (void)semaphore;

#ifndef __EMSCRIPTEN__
    wgpuSurfacePresent(wgpuSurface_);

    // Dawn requires event processing to flush GPU work and actually present the frame.
    // Without this, the Metal backend defers the present and the screen stays black.
    WGPUInstance instance = device_.GetInstance();
    if (instance) {
        wgpuInstanceProcessEvents(instance);
    }
#endif

    if (currentTextureView_) {
        wgpuTextureViewRelease(currentTextureView_);
        currentTextureView_ = nullptr;
    }
    currentTexture_ = nullptr;
}

void DawnSwapChain::Resize(u32 width, u32 height) {
    width_ = width;
    height_ = height;

    if (wgpuSurface_) {
        WGPUSurfaceConfiguration config{};
        config.nextInChain = nullptr;
        config.device = device_.GetNativeDevice();
        config.format = ToWGPUTextureFormat(format_);
        config.usage = WGPUTextureUsage_RenderAttachment;
        config.width = width_;
        config.height = height_;
        config.presentMode = WGPUPresentMode_Fifo;
        config.alphaMode = WGPUCompositeAlphaMode_Opaque;

        wgpuSurfaceConfigure(wgpuSurface_, &config);
    }
}

u32 DawnSwapChain::GetCurrentBackBufferIndex() const {
    return currentFrameIndex_;
}

ResourceHandle DawnSwapChain::GetBackBuffer(u32 index) const {
    if (index < backBufferHandles_.size()) {
        return backBufferHandles_[index];
    }
    return handles::INVALID_RESOURCE;
}

} // namespace primal::graphics::rhi

#endif // ENABLE_WEBGPU
