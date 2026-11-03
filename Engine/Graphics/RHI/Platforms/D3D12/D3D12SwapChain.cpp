// Windows-only: requires ENABLE_D3D12 (_WIN32)
//
// D3D12SwapChain.cpp — DXGI 交换链实现（M1 F1-1）。
#include "D3D12SwapChain.h"

#if defined(_WIN32) && defined(ENABLE_D3D12) && ENABLE_D3D12

#include "D3D12DescriptorHeaps.h"
#include "D3D12Device.h"
#include "D3D12Format.h"
#include "D3D12Texture.h"

#include "Engine/Common/CoreDiag.h"

#ifndef PRIMAL_LOG_STREAM
#define PRIMAL_LOG_STREAM(level, channel) primal::diag::StreamProxy(level, channel)
#endif

namespace primal::graphics::rhi::d3d12 {

D3D12SwapChain::D3D12SwapChain(D3D12Device& device, const SwapChainDesc& desc)
    : RHISwapChain(device, desc) {
    // PresentMode → SyncInterval（§3.5：Vulkan FIFO ↔ FLIP+1 等价）。
    switch (desc.presentMode) {
        case PresentMode::FIFO:
        case PresentMode::FIFO_Relaxed:
            syncInterval_ = 1;
            break;
        case PresentMode::Immediate:
            syncInterval_ = 0;
            break;
        case PresentMode::Mailbox:
        default:
            // D3D12 无 mailbox 对应（NOT-IMPLEMENTED 登记）——按 FIFO 处理。
            syncInterval_ = 1;
            PRIMAL_LOG_STREAM(Warn, "RHI")
                << "[D3D12SwapChain] Mailbox present mode has no D3D12 mapping "
                << "(NOT-IMPLEMENTED) — falling back to FIFO (SyncInterval=1)" << std::endl;
            break;
    }
}

D3D12SwapChain::~D3D12SwapChain() { Destroy(); }

bool D3D12SwapChain::createSwapChain() {
    D3D12Device& dev = static_cast<D3D12Device&>(device_);
    IDXGIFactory2* factory = nullptr;
    if (!dev.GetDxgiFactory(&factory) || !factory) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[D3D12SwapChain] DXGI factory unavailable" << std::endl;
        return false;
    }

    // 窗口句柄契约（M0 §8.1）：platform::window_handle（Win32=HWND）直传。
    HWND hwnd = static_cast<HWND>(swapChainDesc_.window);
    if (!hwnd) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[D3D12SwapChain] SwapChainDesc.window is null" << std::endl;
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 scDesc{};
    scDesc.Width = swapChainDesc_.width;
    scDesc.Height = swapChainDesc_.height;
    // BGRA8_UNorm（离屏直读；sRGB 视图随 M7 呈现验收）。
    scDesc.Format = static_cast<DXGI_FORMAT>(
        swapChainDesc_.format == DataFormat::Unknown
            ? kDXGIFormat_B8G8R8A8_UNorm
            : ToDxgiFormat(swapChainDesc_.format));
    scDesc.Stereo = FALSE;
    scDesc.SampleDesc.Count = 1;   // flip model 强制 1x
    scDesc.SampleDesc.Quality = 0;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    // bufferCount = clamp(desc.bufferCount, 2, 3)。
    scDesc.BufferCount = swapChainDesc_.bufferCount < 2 ? 2
                        : swapChainDesc_.bufferCount > 3 ? 3
                                                         : swapChainDesc_.bufferCount;
    scDesc.Scaling = DXGI_SCALING_STRETCH;
    scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    scDesc.Flags = 0;

    Microsoft::WRL::ComPtr<IDXGISwapChain1> sc1;
    // 呈现与 DIRECT 队列绑定：flip model 的 Present 在创建队列上排序，
    // 与该队列先前的 ExecuteCommandLists 天然有序（canonical 模式）。
    HRESULT hr = factory->CreateSwapChainForHwnd(
        dev.GetGraphicsQueue(), hwnd, &scDesc, nullptr, nullptr, &sc1);
    if (FAILED(hr)) {
        PRIMAL_LOG_STREAM(Error, "RHI")
            << "[D3D12SwapChain] CreateSwapChainForHwnd failed hr=0x" << std::hex
            << static_cast<unsigned long>(hr) << std::dec << std::endl;
        return false;
    }
    if (FAILED(sc1.As(&swapChain_))) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[D3D12SwapChain] IDXGISwapChain3 QI failed" << std::endl;
        return false;
    }
    // 关闭 Alt+Enter 自动全屏切换（窗口宿主自管 resize 语义）。
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    return true;
}

bool D3D12SwapChain::createBackBuffers() {
    D3D12Device& dev = static_cast<D3D12Device&>(device_);
    DXGI_SWAP_CHAIN_DESC1 scDesc{};
    if (FAILED(swapChain_->GetDesc1(&scDesc))) return false;

    backBufferHandles_.clear();
    for (UINT i = 0; i < scDesc.BufferCount; ++i) {
        Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
        if (FAILED(swapChain_->GetBuffer(i, IID_PPV_ARGS(&buffer)))) {
            PRIMAL_LOG_STREAM(Error, "RHI")
                << "[D3D12SwapChain] GetBuffer(" << i << ") failed" << std::endl;
            destroyBackBuffers();
            return false;
        }
        // wrap 纹理：RenderTarget usage + Present 初始状态（tracker 注册由
        // 设备 createBackBufferTexture 完成——Present 在 D3D12 = COMMON 位值）。
        TextureDesc texDesc{};
        texDesc.size.x = scDesc.Width;
        texDesc.size.y = scDesc.Height;
        texDesc.size.z = 1;
        texDesc.mipLevels = 1;
        texDesc.arraySize = 1;
        texDesc.format = swapChainDesc_.format;
        texDesc.type = TextureType::Texture2D;
        texDesc.usage = TextureUsage::RenderTarget | TextureUsage::Present;
        texDesc.memoryUsage = GPUMemoryUsage::SwapChain;
        texDesc.name = "SwapChainBackBuffer";

        ResourceHandle handle = dev.WrapSwapChainBuffer(*this, texDesc, buffer.Get());
        if (handle == handles::INVALID_RESOURCE) {
            destroyBackBuffers();
            return false;
        }
        backBufferHandles_.push_back(handle);
    }
    return true;
}

void D3D12SwapChain::destroyBackBuffers() {
    D3D12Device& dev = static_cast<D3D12Device&>(device_);
    for (ResourceHandle handle : backBufferHandles_) {
        dev.UnwrapSwapChainBuffer(handle);  // 先于交换链销毁释放 wrap 层
    }
    backBufferHandles_.clear();
}

bool D3D12SwapChain::Initialize() {
    if (!createSwapChain()) return false;
    if (!createBackBuffers()) {
        swapChain_.Reset();
        return false;
    }
    return true;
}

void D3D12SwapChain::Destroy() {
    destroyBackBuffers();
    swapChain_.Reset();
}

bool D3D12SwapChain::AcquireNextImage(u32* imageIndex, SyncHandle semaphore, SyncHandle fence) {
    if (!swapChain_ || !imageIndex) return false;
    // D3D12 无 acquire 信号量语义（单 DIRECT 队列有序）；参数接受并忽略。
    if ((semaphore != handles::INVALID_SYNC || fence != handles::INVALID_SYNC)
        && !warnedSemaphoreIgnored_) {
        warnedSemaphoreIgnored_ = true;
        PRIMAL_LOG_STREAM(Info, "RHI")
            << "[D3D12SwapChain] AcquireNextImage semaphore/fence args accepted "
            << "and ignored (D3D12: queue-ordered present; RenderSystem "
            << "imageSemaphores compatibility)" << std::endl;
    }
    *imageIndex = static_cast<u32>(swapChain_->GetCurrentBackBufferIndex());
    return true;
}

void D3D12SwapChain::Present(SyncHandle semaphore) {
    if (!swapChain_) return;
    if (semaphore != handles::INVALID_SYNC && !warnedSemaphoreIgnored_) {
        warnedSemaphoreIgnored_ = true;
        PRIMAL_LOG_STREAM(Info, "RHI")
            << "[D3D12SwapChain] Present wait-semaphore accepted and ignored "
            << "(queue-ordered)" << std::endl;
    }
    const HRESULT hr = swapChain_->Present(syncInterval_, 0);
    if (FAILED(hr)) {
        PRIMAL_LOG_STREAM(Error, "RHI")
            << "[D3D12SwapChain] Present failed hr=0x" << std::hex
            << static_cast<unsigned long>(hr) << std::dec << std::endl;
    }
    // DXGI_STATUS_OCCLUDED / DEVICE_REMOVED 等：M1 记日志即返；自动重建
    // （对齐 VulkanSwapChain needsRecreate_ 路径）登记 M2+。
}

void D3D12SwapChain::Resize(u32 width, u32 height) {
    if (!swapChain_) return;
    D3D12Device& dev = static_cast<D3D12Device&>(device_);

    // 契约：调用方已 WaitIdle（RenderSystem::Resize）。
    if (dev.GetFencePool().GetCompletedValue() != UINT64_MAX) {
        const u64 target = dev.GetFencePool().NextTarget();
        dev.GetFencePool().Signal(dev.GetGraphicsQueue(), target);
        dev.GetFencePool().Wait(target, 5000);
    }

    destroyBackBuffers();
    // 0/0 尺寸 = 沿用当前；格式 Unknown = 保留创建时格式。
    const HRESULT hr = swapChain_->ResizeBuffers(
        0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        PRIMAL_LOG_STREAM(Error, "RHI")
            << "[D3D12SwapChain] ResizeBuffers failed hr=0x" << std::hex
            << static_cast<unsigned long>(hr) << std::dec << std::endl;
        return;
    }
    swapChainDesc_.width = width;
    swapChainDesc_.height = height;
    if (!createBackBuffers()) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[D3D12SwapChain] backbuffer rebuild after resize failed" << std::endl;
    }
}

u32 D3D12SwapChain::GetCurrentBackBufferIndex() const {
    return swapChain_ ? static_cast<u32>(swapChain_->GetCurrentBackBufferIndex()) : 0;
}

ResourceHandle D3D12SwapChain::GetBackBuffer(u32 index) const {
    if (index >= backBufferHandles_.size()) return handles::INVALID_RESOURCE;
    return backBufferHandles_[index];
}

} // namespace primal::graphics::rhi::d3d12

#endif // defined(_WIN32) && defined(ENABLE_D3D12) && ENABLE_D3D12
