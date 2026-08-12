#pragma once

#include "DawnCommon.h"

#if defined(ENABLE_WEBGPU) && ENABLE_WEBGPU

#include "../../Core/RHISwapChain.h"

namespace primal::graphics::rhi {

class DawnDevice;

class DawnSwapChain : public RHISwapChain {
    friend class DawnDevice;
public:
    bool Initialize() override;
    void Destroy() override;
    bool AcquireNextImage(u32* imageIndex, SyncHandle semaphore, SyncHandle fence) override;
    void Present(SyncHandle semaphore) override;
    void Resize(u32 width, u32 height) override;
    u32 GetCurrentBackBufferIndex() const override;
    ResourceHandle GetBackBuffer(u32 index) const override;

private:
    DawnSwapChain(DawnDevice& device, const SwapChainDesc& desc);
    ~DawnSwapChain() override;

    // RHIResource pure virtual stubs (SwapChain does not support mapping)
    void* mapImpl(u64 offset, u64 size) override { (void)offset; (void)size; return nullptr; }
    void unmapImpl() override {}
    bool updateDataImpl(const void* data, u64 size, u64 offset) override { (void)data; (void)size; (void)offset; return false; }

    DawnDevice& device_;
    WGPUSurface wgpuSurface_ = nullptr;
    WGPUTexture currentTexture_ = nullptr;
    WGPUTextureView currentTextureView_ = nullptr;

    utl::vector<ResourceHandle> backBufferHandles_;
    platform::window_handle windowHandle_{nullptr};
    u32 currentFrameIndex_ = 0;
    u32 bufferCount_ = 3;
    DataFormat format_ = DataFormat::BGRA8_UNorm;
    u32 width_ = 0;
    u32 height_ = 0;
};

} // namespace primal::graphics::rhi

#endif // ENABLE_WEBGPU
