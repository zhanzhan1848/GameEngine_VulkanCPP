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

namespace {

/// R7：desc 转储用常见 DXGI_FORMAT 名（未命中返回 "other"，数值由调用方
/// 一并打印——两值并列即可读）。
const char* DxgiFormatName(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_UNKNOWN: return "UNKNOWN";
        case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
        case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
        case DXGI_FORMAT_R8G8B8A8_SNORM: return "R8G8B8A8_SNORM";
        case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
        case DXGI_FORMAT_R16G16B16A16_UNORM: return "R16G16B16A16_UNORM";
        case DXGI_FORMAT_R16G16_FLOAT: return "R16G16_FLOAT";
        case DXGI_FORMAT_R8_UNORM: return "R8_UNORM";
        case DXGI_FORMAT_R32_FLOAT: return "R32_FLOAT";
        case DXGI_FORMAT_D32_FLOAT: return "D32_FLOAT";
        case DXGI_FORMAT_D24_UNORM_S8_UINT: return "D24_UNORM_S8_UINT";
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return "D32_FLOAT_S8X24_UINT";
        default: return "other";
    }
}

/// R7：desc 转储用 SwapEffect 名（前两代是 BLT 模型，flip 才支持同 HWND
/// 单交换链约束的语义对照）。
const char* SwapEffectName(DXGI_SWAP_EFFECT effect) {
    switch (effect) {
        case DXGI_SWAP_EFFECT_DISCARD: return "BLT_DISCARD";
        case DXGI_SWAP_EFFECT_SEQUENTIAL: return "BLT_SEQUENTIAL";
        case DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL: return "FLIP_SEQUENTIAL";
        case DXGI_SWAP_EFFECT_FLIP_DISCARD: return "FLIP_DISCARD";
        default: return "other";
    }
}

} // namespace

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
    //
    // === R7（第七轮）结论：派生在 D3D12 上机制性不可能成功 ===
    // ID3D12Device 不实现 IDXGIDevice（那是 D3D11-era 接口；设备侧
    // QI 必然 E_NOINTERFACE）——R6 的派生永远走不到，实际建链的始终是
    // 枚举 factory，且 LUID 已证一致（R7 排除 driver-instance 错配为
    // E_ACCESSDENIED 根因）。保留本块仅为完整留痕三步 HRESULT（每步
    // 失败 Warn 一行），回退枚举 factory 是预期正路，不是降级。
    Microsoft::WRL::ComPtr<IDXGIFactory2> factoryFromDevice;
    LUID derivedAdapterLuid{};
    bool haveDerivedAdapterLuid = false;
    // 首个失败步（默认 QI：D3D12 下必失败的那一步）——回退日志引用。
    static constexpr const char* kDeriveStepQi = "QI IDXGIDevice";
    HRESULT deriveFailHr = E_NOINTERFACE;
    const char* deriveFailStep = kDeriveStepQi;
    {
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
        Microsoft::WRL::ComPtr<IDXGIAdapter> deviceAdapter;
        ID3D12Device* nativeDevice = dev.GetNativeDevice();
        const HRESULT hrQi =
            nativeDevice ? nativeDevice->QueryInterface(IID_PPV_ARGS(&dxgiDevice))
                         : E_POINTER;
        if (FAILED(hrQi)) {
            deriveFailHr = hrQi;
            deriveFailStep = kDeriveStepQi;
            PRIMAL_LOG_STREAM(Warn, "RHI")
                << "[D3D12SwapChain] derive factory 1/3 ID3D12Device->QI"
                   "(IDXGIDevice) hr=0x" << std::hex
                << static_cast<unsigned long>(hrQi) << std::dec
                << " (ID3D12Device does not implement IDXGIDevice — D3D11-era"
                   " path, E_NOINTERFACE expected on D3D12; R7)" << std::endl;
        } else {
            const HRESULT hrAdapter = dxgiDevice->GetAdapter(&deviceAdapter);
            if (FAILED(hrAdapter)) {
                deriveFailHr = hrAdapter;
                deriveFailStep = "GetAdapter";
                PRIMAL_LOG_STREAM(Warn, "RHI")
                    << "[D3D12SwapChain] derive factory 2/3 IDXGIDevice->"
                       "GetAdapter hr=0x" << std::hex
                    << static_cast<unsigned long>(hrAdapter) << std::dec
                    << " (R7)" << std::endl;
            } else {
                const HRESULT hrParent =
                    deviceAdapter->GetParent(IID_PPV_ARGS(&factoryFromDevice));
                if (FAILED(hrParent)) {
                    deriveFailHr = hrParent;
                    deriveFailStep = "GetParent(IDXGIFactory2)";
                    PRIMAL_LOG_STREAM(Warn, "RHI")
                        << "[D3D12SwapChain] derive factory 3/3 IDXGIAdapter->"
                           "GetParent(IDXGIFactory2) hr=0x" << std::hex
                        << static_cast<unsigned long>(hrParent) << std::dec
                        << " (R7)" << std::endl;
                } else {
                    // 派生成功；deviceAdapter 即设备所在 adapter（LUID 留作
                    // 失败诊断）。
                    DXGI_ADAPTER_DESC derivedDesc{};
                    if (SUCCEEDED(deviceAdapter->GetDesc(&derivedDesc))) {
                        derivedAdapterLuid = derivedDesc.AdapterLuid;
                        haveDerivedAdapterLuid = true;
                    }
                }
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
        // QI 步失败（D3D12 常态）打规格口径文案；后续步失败打实际步名。
        if (deriveFailStep == kDeriveStepQi) {
            PRIMAL_LOG_STREAM(Warn, "RHI")
                << "[D3D12SwapChain] device-derived factory unavailable "
                   "(QI IDXGIDevice hr=0x" << std::hex
                << static_cast<unsigned long>(deriveFailHr) << std::dec
                << ": ID3D12Device does not implement IDXGIDevice — D3D11-era"
                   " path; using enumeration factory, R7)" << std::endl;
        } else {
            PRIMAL_LOG_STREAM(Warn, "RHI")
                << "[D3D12SwapChain] device-derived factory unavailable "
                   "(" << deriveFailStep << " hr=0x" << std::hex
                << static_cast<unsigned long>(deriveFailHr) << std::dec
                << "; using enumeration factory, R7)" << std::endl;
        }
    }

    // 窗口句柄契约（M0 §8.1）：platform::window_handle（Win32=HWND）直传。
    HWND hwnd = static_cast<HWND>(swapChainDesc_.window);
    if (!hwnd) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[D3D12SwapChain] SwapChainDesc.window is null" << std::endl;
        return false;
    }

    // R7 窗口证据（创建前一次）：style/exstyle（WS_CHILD=0x40000000 /
    // WS_POPUP=0x80000000 判读）、顶层判定（GetAncestor(GA_PARENT)==NULL 即
    // 无父窗口）与可见性——E_ACCESSDENIED 复跑时排除"子窗口/隐形窗"变量。
    {
        const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
        const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        const HWND parent = GetAncestor(hwnd, GA_PARENT);
        const BOOL visible = IsWindowVisible(hwnd);
        PRIMAL_LOG_STREAM(Info, "RHI")
            << "[D3D12SwapChain] window: hwnd=0x" << std::hex
            << static_cast<void*>(hwnd) << std::dec
            << " style=0x" << std::hex << static_cast<unsigned long long>(style)
            << " exstyle=0x" << static_cast<unsigned long long>(exStyle) << std::dec
            << " parent=" << (parent ? "yes" : "NULL(top-level)")
            << " visible=" << (visible ? 1 : 0) << std::endl;
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

    // R7 desc 全量转储（一行）：失败复跑时逐字段比对调用方预期。
    PRIMAL_LOG_STREAM(Info, "RHI")
        << "[D3D12SwapChain] desc: Format=" << static_cast<unsigned>(scDesc.Format)
        << "(" << DxgiFormatName(scDesc.Format) << ")"
        << " SwapEffect=" << static_cast<unsigned>(scDesc.SwapEffect)
        << "(" << SwapEffectName(scDesc.SwapEffect) << ")"
        << " BufferCount=" << scDesc.BufferCount
        << " BufferUsage=0x" << std::hex
        << static_cast<unsigned long>(scDesc.BufferUsage) << std::dec
        << " Scaling=" << static_cast<unsigned>(scDesc.Scaling)
        << " AlphaMode=" << static_cast<unsigned>(scDesc.AlphaMode)
        << " Stereo=" << scDesc.Stereo
        << " SampleDesc=" << scDesc.SampleDesc.Count << "x"
        << scDesc.SampleDesc.Quality
        << " Size=" << scDesc.Width << "x" << scDesc.Height
        << " Flags=0x" << std::hex
        << static_cast<unsigned long>(scDesc.Flags) << std::dec << std::endl;

    // R7 队列证据：flip model 的 Present 在创建队列上排序，传非 DIRECT
    // 队列是排障高频错配——创建前留 type（D3D12_COMMAND_LIST_TYPE 枚举值，
    // DIRECT=0；4 是 VIDEO_DECODE，r9 勘误此前写错的枚举注记）。
    if (ID3D12CommandQueue* queue = dev.GetGraphicsQueue()) {
        // R8：MSVC/Agility SDK 的 ID3D12CommandQueue::GetDesc() 按值返回
        // D3D12_COMMAND_QUEUE_DESC（无参）；R7 误用 MinGW 出参风格
        // GetDesc(&desc) 导致 C2660，改为按值接收。
        D3D12_COMMAND_QUEUE_DESC queueDesc = queue->GetDesc();
        PRIMAL_LOG_STREAM(Info, "RHI")
            << "[D3D12SwapChain] queue: type=" << static_cast<unsigned>(queueDesc.Type)
            << " (0=DIRECT)"
            << " priority=" << static_cast<unsigned>(queueDesc.Priority)
            << " flags=0x" << std::hex
            << static_cast<unsigned long>(queueDesc.Flags) << std::dec
            << std::endl;
    }

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
        // R7：E_ACCESSDENIED 的三个已知成因按命中概率排列（首位=本轮根因，
        // 双 Initialize 已由幂等守卫防护；仍出现时查后两项）。
        if (hr == E_ACCESSDENIED) {
            PRIMAL_LOG_STREAM(Error, "RHI")
                << "[D3D12SwapChain] hint: E_ACCESSDENIED = flip-model "
                   "swapchain already exists on this hwnd (double init — "
                   "now guarded), another process owns the window, or "
                   "exclusive fullscreen is held" << std::endl;
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
    // R7 幂等守卫（对齐 VulkanSwapChain::Initialize 的既有模式）：
    // D3D12Device::createSwapChainImpl 返回前已内调本函数完成真建；
    // RenderSystem::Initialize 随后对同一对象再调一次。DXGI 规定每个
    // HWND 至多一个 flip-model 交换链——第二次 CreateSwapChainForHwnd
    // 必 E_ACCESSDENIED（R7 Windows GTX1060 实测根因：首个交换链仍存活
    // 于同一 HWND）。swapChain_ 非空 = 已完成真建（createBackBuffers 的
    // backbuffer wrap 随其之后），直接返回成功。
    if (swapChain_) {
        PRIMAL_LOG_STREAM(Info, "RHI")
            << "[D3D12SwapChain] Initialize: swap chain already created — "
               "skipping (RenderSystem double-init guard, R7: second "
               "flip-model swapchain on same HWND would fail E_ACCESSDENIED)"
            << std::endl;
        return true;
    }
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
