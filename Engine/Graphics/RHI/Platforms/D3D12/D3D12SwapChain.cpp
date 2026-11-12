// Windows-only: requires ENABLE_D3D12 (_WIN32)
//
// D3D12SwapChain.cpp — DXGI 交换链实现（M1 F1-1）。
#include "D3D12SwapChain.h"

#if defined(_WIN32) && defined(ENABLE_D3D12) && ENABLE_D3D12

#include "D3D12DescriptorHeaps.h"
#include "D3D12Device.h"
#include "D3D12Format.h"
#include "D3D12Texture.h"

#include "Engine/Common/CoreLogStream.h"

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

    // === R6（D3D12 视觉对齐第六轮）：factory 优先从设备派生 ===
    // 原路径拿 selectAdapter() 里 CreateDXGIFactory2 枚举期新建的 factory
    // 建交换链；该 factory 与设备/队列不保证同一 driver 实例，错配时
    // CreateSwapChainForHwnd 返回 E_ACCESSDENIED（0x80070005，R6 Windows
    // GTX1060 实测）。规范做法（D3D12 官方 sample）：ID3D12Device → QI
    // IDXGIDevice → GetAdapter → GetParent 从设备自身派生 factory，
    // 构造上保证 factory/adapter/queue 同 driver 实例。派生失败回退
    // 枚举 factory（原路径）并 Warn。
    Microsoft::WRL::ComPtr<IDXGIFactory2> factoryFromDevice;
    LUID derivedAdapterLuid{};
    bool haveDerivedAdapterLuid = false;
    {
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
        Microsoft::WRL::ComPtr<IDXGIAdapter> deviceAdapter;
        ID3D12Device* nativeDevice = dev.GetNativeDevice();
        if (nativeDevice
            && SUCCEEDED(nativeDevice->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))
            && SUCCEEDED(dxgiDevice->GetAdapter(&deviceAdapter))
            && SUCCEEDED(deviceAdapter->GetParent(IID_PPV_ARGS(&factoryFromDevice)))) {
            // 派生成功；deviceAdapter 即设备所在 adapter（LUID 留作失败诊断）。
            DXGI_ADAPTER_DESC derivedDesc{};
            if (SUCCEEDED(deviceAdapter->GetDesc(&derivedDesc))) {
                derivedAdapterLuid = derivedDesc.AdapterLuid;
                haveDerivedAdapterLuid = true;
            }
        }
    }
    // 实际用于创建的 factory（借用指针，不持有引用）。
    IDXGIFactory2* factory = nullptr;
    if (factoryFromDevice) {
        factory = factoryFromDevice.Get();
    } else if (!dev.GetDxgiFactory(&factory) || !factory) {
        PRIMAL_LOG_STREAM(Error, "RHI")
            << "[D3D12SwapChain] DXGI factory unavailable "
            << "(device-derived and enumeration both failed)" << std::endl;
        return false;
    } else {
        PRIMAL_LOG_STREAM(Warn, "RHI")
            << "[D3D12SwapChain] device-derived DXGI factory failed; "
            << "falling back to enumeration factory (driver-instance mismatch "
            << "possible — R6)" << std::endl;
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

    // R6 预检证据（成功路径也留痕）：factory 来源与关键创建参数一目了然。
    PRIMAL_LOG_STREAM(Info, "RHI")
        << "[D3D12SwapChain] create: factory="
        << (factoryFromDevice ? "device-derived" : "enumeration")
        << " hwnd=0x" << std::hex << static_cast<void*>(hwnd) << std::dec
        << " size=" << scDesc.Width << "x" << scDesc.Height
        << " BufferCount=" << scDesc.BufferCount << std::endl;

    Microsoft::WRL::ComPtr<IDXGISwapChain1> sc1;
    // 呈现与 DIRECT 队列绑定：flip model 的 Present 在创建队列上排序，
    // 与该队列先前的 ExecuteCommandLists 天然有序（canonical 模式）。
    HRESULT hr = factory->CreateSwapChainForHwnd(
        dev.GetGraphicsQueue(), hwnd, &scDesc, nullptr, nullptr, &sc1);
    if (FAILED(hr)) {
        PRIMAL_LOG_STREAM(Error, "RHI")
            << "[D3D12SwapChain] CreateSwapChainForHwnd failed hr=0x" << std::hex
            << static_cast<unsigned long>(hr) << std::dec << std::endl;
        // R6 失败自诊断（一次性，供下次 Windows 复跑定位 E_ACCESSDENIED）：
        // ① hwnd 属主 pid/tid vs 本进程（他进程窗口）；
        // ② factory 既有窗口关联（非空且 != hwnd → hwnd 已被别的
        //    交换链/工厂占用）；
        // ③ 设备 adapter LUID vs 创建 factory 的 EnumAdapters1(0) LUID
        //    （不一致 = adapter/factory 错配疑点；注意枚举序 0 未必是设备
        //    由 EnumAdapterByGpuPreference 选中的序号，需结合判读）。
        DWORD ownerPid = 0;
        const DWORD ownerTid = GetWindowThreadProcessId(hwnd, &ownerPid);
        HWND assoc = nullptr;
        factory->GetWindowAssociation(&assoc);
        PRIMAL_LOG_STREAM(Error, "RHI")
            << "[D3D12SwapChain] diag window: hwnd=0x" << std::hex
            << static_cast<void*>(hwnd) << std::dec
            << " owner pid=" << ownerPid << " tid=" << ownerTid
            << " vs cur pid=" << GetCurrentProcessId()
            << " tid=" << GetCurrentThreadId()
            << "; factory assoc hwnd=0x" << std::hex
            << static_cast<void*>(assoc) << std::dec
            << (assoc && assoc != hwnd
                    ? " (assoc!=hwnd: bound to another swapchain/factory)"
                    : "")
            << std::endl;
        DXGI_ADAPTER_DESC1 devAdapterDesc{};
        const bool haveDevAdapter =
            dev.GetNativeAdapter()
            && SUCCEEDED(dev.GetNativeAdapter()->GetDesc1(&devAdapterDesc));
        Microsoft::WRL::ComPtr<IDXGIAdapter1> factoryAdapter0;
        DXGI_ADAPTER_DESC1 factoryAdapterDesc{};
        const bool haveFactoryAdapter =
            SUCCEEDED(factory->EnumAdapters1(0, &factoryAdapter0))
            && SUCCEEDED(factoryAdapter0->GetDesc1(&factoryAdapterDesc));
        if (haveDevAdapter && haveFactoryAdapter) {
            const bool luidMatch =
                devAdapterDesc.AdapterLuid.HighPart == factoryAdapterDesc.AdapterLuid.HighPart
                && devAdapterDesc.AdapterLuid.LowPart == factoryAdapterDesc.AdapterLuid.LowPart;
            PRIMAL_LOG_STREAM(Error, "RHI")
                << "[D3D12SwapChain] diag adapter LUID: device=0x" << std::hex
                << static_cast<unsigned long>(devAdapterDesc.AdapterLuid.HighPart)
                << ':' << devAdapterDesc.AdapterLuid.LowPart
                << " factoryEnum0=0x"
                << static_cast<unsigned long>(factoryAdapterDesc.AdapterLuid.HighPart)
                << ':' << factoryAdapterDesc.AdapterLuid.LowPart << std::dec
                << (luidMatch ? " (match)" : " (MISMATCH: adapter/factory)")
                << std::endl;
        } else {
            PRIMAL_LOG_STREAM(Error, "RHI")
                << "[D3D12SwapChain] diag adapter LUID unavailable"
                << " (deviceDesc=" << haveDevAdapter
                << " factoryEnum0Desc=" << haveFactoryAdapter << ")" << std::endl;
        }
        if (haveDerivedAdapterLuid) {
            PRIMAL_LOG_STREAM(Error, "RHI")
                << "[D3D12SwapChain] diag derivation chain LUID=0x" << std::hex
                << static_cast<unsigned long>(derivedAdapterLuid.HighPart)
                << ':' << derivedAdapterLuid.LowPart << std::dec
                << " (device-derived factory; must equal device LUID above)"
                << std::endl;
        }
        return false;
    }
    if (FAILED(sc1.As(&swapChain_))) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[D3D12SwapChain] IDXGISwapChain3 QI failed" << std::endl;
        return false;
    }
    // 关闭 Alt+Enter 自动全屏切换（窗口宿主自管 resize 语义）——
    // 在实际用于创建的 factory 上调用（R6：关联随 factory 实例走）。
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
