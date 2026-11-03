// Windows-only: requires ENABLE_D3D12 (_WIN32)
//
// D3D12SwapChain.h — DXGI 交换链（M1 F1-1）。
//
// 契约：
//   - 窗口句柄（M0 §8.1）：SwapChainDesc.window = platform::window_handle
//     （Win32 = HWND），直接作为 CreateSwapChainForHwnd 的 hwnd，零转换。
//   - 格式：BGRA8_UNorm（离屏直读路径；sRGB 呈现随 M7——阶段文档 §7.2-3）。
//   - 模型：DXGI_SWAP_EFFECT_FLIP_DISCARD；bufferCount = clamp(desc.bufferCount,2,3)。
//   - PresentMode 映射（§3.5）：FIFO/FIFO_Relaxed → SyncInterval=1（FIFO
//     等价）；Immediate → SyncInterval=0；Mailbox 无 D3D12 对应
//     （NOT-IMPLEMENTED 登记，按 FIFO 处理 + Warn）。
//   - AcquireNextImage：D3D12 无 acquire 语义——轮转索引由
//     GetCurrentBackBufferIndex() 读取（Present 后 DXGI 推进）。传入的
//     semaphore/fence 参数"接受并忽略"（一次性日志）：DIRECT 单队列上
//     ExecuteCommandLists→Present 的顺序即同步，无需 GPU-GPU 信号量
//     （RenderSystem::BeginFrame 的 imageSemaphores_[slot] 由此兼容）。
//   - Resize：调用方先 device WaitIdle（RenderSystem::Resize 契约）；
//     内部释放 RTV → ResizeBuffers(0,w,h,DXGI_FORMAT_UNKNOWN 保原格式) →
//     重建 backbuffer 包装 + RTV。
//   - backbuffer 包装：D3D12Texture wrap 模式（device 纹理分配器注册，
//     InsertBarrier/OMSetRenderTargets 按普通纹理路径消费）；销毁顺序
//     = 先销毁 wrap 纹理再销毁 IDXGISwapChain。
#pragma once
#if defined(_WIN32) && defined(ENABLE_D3D12) && ENABLE_D3D12

#include <dxgi1_6.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <vector>

#include "../../Core/RHISwapChain.h"

namespace primal::graphics::rhi {

class D3D12Device;

namespace d3d12 {

class D3D12SwapChain : public RHISwapChain {
    friend class primal::graphics::rhi::D3D12Device;
public:
    D3D12SwapChain(D3D12Device& device, const SwapChainDesc& desc);
    virtual ~D3D12SwapChain();

    bool Initialize() override;
    void Destroy() override;
    void Resize(u32 width, u32 height) override;

    bool AcquireNextImage(u32* imageIndex,
                          SyncHandle semaphore = handles::INVALID_SYNC,
                          SyncHandle fence = handles::INVALID_SYNC) override;
    void Present(SyncHandle semaphore) override;

    u32 GetCurrentBackBufferIndex() const override;
    ResourceHandle GetBackBuffer(u32 index) const override;

protected:
    // RHIResource no-op（交换链不支持 map/update）
    void* mapImpl(u64, u64) override { return nullptr; }
    void unmapImpl() override {}
    bool updateDataImpl(const void*, u64, u64) override { return false; }

private:
    bool createSwapChain();
    bool createBackBuffers();
    void destroyBackBuffers();

    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapChain_;
    std::vector<ResourceHandle> backBufferHandles_;  // D3D12Texture wrap 句柄
    u32 syncInterval_ = 1;        // FIFO 语义（§3.5 映射）
    bool warnedSemaphoreIgnored_ = false;
};

} // namespace primal::graphics::rhi::d3d12
} // namespace primal::graphics::rhi

#endif // defined(_WIN32) && defined(ENABLE_D3D12) && ENABLE_D3D12
