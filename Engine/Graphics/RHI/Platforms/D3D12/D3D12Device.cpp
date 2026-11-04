// Windows-only: requires ENABLE_D3D12 (_WIN32)
//
// D3D12Device.cpp — D3D12 设备实现（M0 F0-8 闭环 + M1 全资源层接线）。
#include "D3D12Device.h"

#if defined(_WIN32) && defined(ENABLE_D3D12) && ENABLE_D3D12

#include <cstdlib>
#include <cstring>
#include <string>

#include "D3D12Format.h"

#include "Engine/Common/CoreLogStream.h"

#if defined(PRIMAL_D3D12_AGILITY_RUNTIME) && PRIMAL_D3D12_AGILITY_RUNTIME
// Agility SDK 运行时导出（Engine/CMakeLists.txt 动态接入链在场时定义）：
// D3D12 系统运行时按二进制的 D3D12SDKVersion 导出值选择 side-load 的
// <exe 目录>\D3D12\D3D12Core.dll（官方 Agility 集成约定；该 DLL 由 Engine
// POST_BUILD copy_if_different 暂存到全部 exe 输出根的 D3D12/ 子目录，
// R8 起含 <build>/Tests/UnitTests——见 Engine/CMakeLists 暂存目的地表）。本 TU 经
// RHIDeviceFactory（CreateRHIDevice → new D3D12Device）必然被静态库链接
// 拉入，导出稳定可见。离线回退路径（PRIMAL_D3D12_AGILITY_RUNTIME=0）不
// 导出——无 side-load DLL 时导出反而令 d3d12.dll 找 .\D3D12\ 失败。
extern "C" {
__declspec(dllexport) extern const UINT D3D12SDKVersion = D3D12_SDK_VERSION;
}
#endif

namespace primal::graphics::rhi {

using Microsoft::WRL::ComPtr;

namespace {

bool EnvFlag(const char* name) {
    const char* v = std::getenv(name);
    return v && std::strcmp(v, "1") == 0;
}

/// DataFormat → 每 texel 字节数（与 D3D12Texture.cpp 的本地表一致；设备侧
/// 上传路径使用）。
u64 BytesPerTexel(DataFormat format) {
    switch (format) {
        case DataFormat::R8_UNorm: case DataFormat::R8_SNorm:
        case DataFormat::R8_UInt:  case DataFormat::R8_SInt:
            return 1;
        case DataFormat::R16_UNorm: case DataFormat::R16_SNorm:
        case DataFormat::R16_UInt:  case DataFormat::R16_SInt:
        case DataFormat::R16_Float:
        case DataFormat::RG8_UNorm: case DataFormat::RG8_SNorm:
        case DataFormat::RG8_UInt:  case DataFormat::RG8_SInt:
            return 2;
        case DataFormat::R32_UNorm: case DataFormat::R32_SNorm:
        case DataFormat::R32_UInt:  case DataFormat::R32_SInt:
        case DataFormat::R32_Float:
        case DataFormat::RG16_UNorm: case DataFormat::RG16_SNorm:
        case DataFormat::RG16_UInt:  case DataFormat::RG16_SInt:
        case DataFormat::RG16_Float:
        case DataFormat::BGRA8_UNorm: case DataFormat::BGRA8_UInt:
        case DataFormat::BGRA8_SInt:
        case DataFormat::RG8B8A8_UNorm: case DataFormat::RG8B8A8_SNorm:
        case DataFormat::RG8B8A8_UInt:  case DataFormat::RG8B8A8_SInt:
        case DataFormat::RGBA8_UNorm: case DataFormat::RGBA8_SNorm:
        case DataFormat::RGBA8_UInt:  case DataFormat::RGBA8_SInt:
        case DataFormat::RGBA8_sRGB:
        case DataFormat::D32_Float:
            return 4;
        case DataFormat::RG32_UNorm: case DataFormat::RG32_SNorm:
        case DataFormat::RG32_UInt:  case DataFormat::RG32_SInt:
        case DataFormat::RG32_Float:
        case DataFormat::RGB32_UNorm: case DataFormat::RGB32_SNorm:
        case DataFormat::RGB32_UInt:  case DataFormat::RGB32_SInt:
        case DataFormat::RGB32_Float:
        case DataFormat::RGBA16_UNorm: case DataFormat::RGBA16_SNorm:
        case DataFormat::RGBA16_UInt:  case DataFormat::RGBA16_SInt:
        case DataFormat::RGBA16_Float:
            return 8;
        case DataFormat::RGBA32_UNorm: case DataFormat::RGBA32_SNorm:
        case DataFormat::RGBA32_UInt:  case DataFormat::RGBA32_SInt:
        case DataFormat::RGBA32_Float:
            return 16;
        default:
            return 0;  // 压缩/24-bit/深度双平面/Unknown
    }
}

} // namespace

D3D12Device::D3D12Device(const DeviceDesc& desc)
    : RHIDevice<D3D12Device>(desc) {}

D3D12Device::~D3D12Device() {
    // Shutdown 由 CRTP 基类在 isValid_ 时调用；析构兜底。
    if (IsValid()) Shutdown();
}

// ============================================================================
// M0 最小闭环：initializeImpl = debug layer -> adapter -> device+queue ->
// fence -> allocator -> M1 资源层子系统
// ============================================================================

bool D3D12Device::initializeImpl() {
    // R8 分阶段埋点（八测定时超时定位用）：每阶段入口一行 Info 日志
    //（stderr，PRIMAL_LOG_STREAM 自带缓冲；日志尾部最后一行 = 挂起阶段）。
#if defined(PRIMAL_D3D12_AGILITY_RUNTIME) && PRIMAL_D3D12_AGILITY_RUNTIME
    constexpr const char* kAgilityRuntime = "1";
#else
    constexpr const char* kAgilityRuntime = "0";
#endif
    PRIMAL_LOG_STREAM(Info, "RHI.D3D12")
        << "[D3D12:init] stage 0 enter (agility_runtime=" << kAgilityRuntime
        << ", debug=" << (desc_.enableDebug ? 1 : 0)
        << ", validation=" << (desc_.enableValidation ? 1 : 0)
        << ", warp_env=" << (EnvFlag("PRIMAL_D3D12_WARP") ? 1 : 0) << ")" << std::endl;
    PRIMAL_LOG_STREAM(Info, "RHI.D3D12") << "[D3D12:init] stage 1 debug layer" << std::endl;
    if (!enableDebugLayer()) return false;
    PRIMAL_LOG_STREAM(Info, "RHI.D3D12") << "[D3D12:init] stage 2 adapter selection" << std::endl;
    if (!selectAdapter()) return false;
    PRIMAL_LOG_STREAM(Info, "RHI.D3D12") << "[D3D12:init] stage 3 device + queue" << std::endl;
    if (!createDeviceAndQueue()) return false;
    PRIMAL_LOG_STREAM(Info, "RHI.D3D12") << "[D3D12:init] stage 4 fence pool" << std::endl;
    if (!fencePool_.Initialize(device_.Get())) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[D3D12Device] fence pool init failed" << std::endl;
        return false;
    }
    PRIMAL_LOG_STREAM(Info, "RHI.D3D12") << "[D3D12:init] stage 5 allocator" << std::endl;
    if (!createAllocator()) return false;
    PRIMAL_LOG_STREAM(Info, "RHI.D3D12") << "[D3D12:init] stage 6 resource subsystems" << std::endl;
    if (!createResourceSubsystems()) return false;
    PRIMAL_LOG_STREAM(Info, "RHI.D3D12") << "[D3D12:init] stage 7 capability query" << std::endl;

    // M5 F5-3：descriptor indexing 能力真实查询（初始化期一次，缓存结果；
    // CheckFeatureSupport 失败=能力未知 → 按 tier0/不支持处理，不静默）。
    // [R6/Windows] ①D3D12_FEATURE_DATA_D3D12_OPTIONS 无 MaxDescriptorHeapSize
    // 字段（真头核对：vendored dxsdk d3d12.h L2095-2112 与官方 DirectX-Headers
    // 均 0 命中）——bindless 规模上限改记 ResourceBindingTier（Tier>=2 即无界
    // 数组；shader-visible 堆上限是 tier 决定的常量，非 feature 查询项），
    // 删除该查询。②TypedUAVLoadAdditionalFormats 在 OPTIONS（v1）不在
    // OPTIONS3（真头：OPTIONS v1 L2914 命中；OPTIONS3 五字段无此名）——
    // 查询迁入本块合并，避免重复 CheckFeatureSupport。
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
        if (SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS,
                                                   &options, sizeof(options)))) {
            resourceBindingTier_ = options.ResourceBindingTier;
            // 判定语义对齐 VulkanDevice（"引擎消费路径可用"而非 tier 透传）：
            // Tier>=2 = 无界描述符数组 + shader 内动态/非一致索引可用。
            // Tier 1 仅静态索引数组（等价 Vulkan 未开 descriptorIndexing）。
            descriptorIndexingSupported_ =
                options.ResourceBindingTier >= D3D12_RESOURCE_BINDING_TIER_2;
            // M6 F6-2/F6-3：类型化 UAV 附加格式（RGBA16F/RG16F 等 typed UAV
            // load/store 的先决能力；D3D12 仅保证 R32_FLOAT/UINT/SINT 三格式）。
            // 消费点：D3D12Texture 对需附加格式的 UAV 纹理显式失败（fail 语义）。
            typedUavAdditionalFormats_ = options.TypedUAVLoadAdditionalFormats != 0;
            PRIMAL_LOG_STREAM(Info, "RHI")
                << "[D3D12Device] descriptor indexing (ResourceBindingTier="
                << u32(options.ResourceBindingTier)
                << ", TypedUAVLoadAdditionalFormats="
                << (typedUavAdditionalFormats_ ? "yes" : "no")
                << ") — " << (descriptorIndexingSupported_ ? "enabled" : "NOT supported (tier<2)")
                << std::endl;
        } else {
            resourceBindingTier_ = 0;
            descriptorIndexingSupported_ = false;
            typedUavAdditionalFormats_ = false;
            PRIMAL_LOG_STREAM(Warn, "RHI")
                << "[D3D12Device] OPTIONS CheckFeatureSupport failed — "
                   "descriptor indexing + typed UAV additional formats "
                   "reported NOT supported" << std::endl;
        }
        PRIMAL_LOG_STREAM(Info, "RHI")
            << "[D3D12Device] typed UAV additional formats — "
            << (typedUavAdditionalFormats_
                    ? "enabled (RGBA16F/RG16F UAV load/store ok)"
                    : "NOT supported (typed UAV beyond R32_* family will fail)")
            << std::endl;
    }

    // M0 §5.1 教训：注册式桥的链接锚点（防静态库裁剪；幂等）。
    EnsureD3D12BackendLinksLinked();

    PRIMAL_LOG_STREAM(Info, "RHI.D3D12") << "[D3D12:init] stage 8 device removal check" << std::endl;
    // 首次设备丢失检查：创建即失败时带原因上报。
    const HRESULT reason = device_->GetDeviceRemovedReason();
    if (reason != S_OK) {
        reportDeviceRemoval(reason);
        return false;
    }
    PRIMAL_LOG_STREAM(Info, "RHI") << "[D3D12Device] initialized (M1 resource layer)"
        << (warp_ ? " (WARP)" : "") << std::endl;
    PRIMAL_LOG_STREAM(Info, "RHI.D3D12") << "[D3D12:init] stage 9 done" << std::endl;
    return true;
}

bool D3D12Device::enableDebugLayer() {
    if (!desc_.enableDebug && !desc_.enableValidation) return true;

    PRIMAL_LOG_STREAM(Info, "RHI.D3D12") << "[D3D12:init] stage 1.1 D3D12GetDebugInterface" << std::endl;
    ComPtr<ID3D12Debug> debug0;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug0)))) {
        debug0->EnableDebugLayer();
        PRIMAL_LOG_STREAM(Info, "RHI.D3D12") << "[D3D12:init] stage 1.2 debug layer enabled" << std::endl;
    } else {
        PRIMAL_LOG_STREAM(Error, "RHI")
            << "[D3D12Device] D3D12GetDebugInterface failed (debug tools missing?)" << std::endl;
        return false;  // fail-fast：请求了 debug 但拿不到 → 初始化失败
    }

    // GBV：仅显式 enableValidation（开销大）。
    if (desc_.enableValidation) {
        if (SUCCEEDED(debug0.As(&debugController_))) {  // ComPtr::As -> QI ID3D12Debug1
            debugController_->SetEnableGPUBasedValidation(TRUE);
            debugController_->SetEnableSynchronizedCommandQueueValidation(TRUE);
        } else {
            PRIMAL_LOG_STREAM(Warn, "RHI")
                << "[D3D12Device] ID3D12Debug1 unavailable: GBV off" << std::endl;
        }
    }
    return true;
}

bool D3D12Device::selectAdapter() {
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&dxgiFactory_)))) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[D3D12Device] CreateDXGIFactory2 failed" << std::endl;
        return false;
    }
    PRIMAL_LOG_STREAM(Info, "RHI.D3D12") << "[D3D12:init] stage 2.0 DXGI factory created" << std::endl;

    // WARP 回退：PRIMAL_D3D12_WARP=1 强制软件适配器（CI / 无 GPU 场景）。
    if (EnvFlag("PRIMAL_D3D12_WARP")) {
        PRIMAL_LOG_STREAM(Info, "RHI.D3D12") << "[D3D12:init] stage 2.1w EnumWarpAdapter" << std::endl;
        ComPtr<IDXGIAdapter> warp;
        if (SUCCEEDED(dxgiFactory_->EnumWarpAdapter(IID_PPV_ARGS(&warp)))
            && SUCCEEDED(warp.As(&adapter_))) {
            warp_ = true;
            return true;
        }
        PRIMAL_LOG_STREAM(Error, "RHI") << "[D3D12Device] WARP requested but EnumWarpAdapter failed" << std::endl;
        return false;
    }

    // 硬件适配器：按 desc_.adapterIndex 选；越界/枚举耗尽 → 回退索引 0，
    // 仍失败 → WARP 兜底（显式记录，不静默）。
    PRIMAL_LOG_STREAM(Info, "RHI.D3D12")
        << "[D3D12:init] stage 2.1 hardware adapter enum (want index "
        << desc_.adapterIndex << ")" << std::endl;
    ComPtr<IDXGIAdapter1> candidate;
    u32 index = 0;
    bool found = false;
    // R8 修正：原循环条件 `hr != DXGI_ERROR_NOT_FOUND` 把任何非 NOT_FOUND 的
    // 持续失败（INVALID_CALL/E_FAIL 等）当作"继续枚举" → 无限循环（八测
    // 补 DLL 后定时超时的静态挂起嫌疑之一）。标准形式 = SUCCEEDED 终止。
    while (SUCCEEDED(dxgiFactory_->EnumAdapterByGpuPreference(
               index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&candidate)))) {
        if (index == desc_.adapterIndex) {
            DXGI_ADAPTER_DESC1 desc1{};
            candidate->GetDesc1(&desc1);
            if ((desc1.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
                found = SUCCEEDED(candidate.As(&adapter_));
                break;
            }
        }
        ++index;
    }
    PRIMAL_LOG_STREAM(Info, "RHI.D3D12")
        << "[D3D12:init] stage 2.2 enum done (last_index=" << index
        << ", found=" << (found ? 1 : 0) << ")" << std::endl;
    if (!found) {
        PRIMAL_LOG_STREAM(Warn, "RHI")
            << "[D3D12Device] adapter " << desc_.adapterIndex
            << " unavailable; falling back to WARP" << std::endl;
        ComPtr<IDXGIAdapter> warp;
        if (SUCCEEDED(dxgiFactory_->EnumWarpAdapter(IID_PPV_ARGS(&warp)))
            && SUCCEEDED(warp.As(&adapter_))) {
            warp_ = true;
            return true;
        }
        return false;
    }
    return true;
}

bool D3D12Device::createDeviceAndQueue() {
    // D3D_FEATURE_LEVEL_12_0 起步（Master Plan：对等 Vulkan 1.2 级功能面）。
    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
    };
    HRESULT hr = E_FAIL;
    for (const D3D_FEATURE_LEVEL level : levels) {
        PRIMAL_LOG_STREAM(Info, "RHI.D3D12")
            << "[D3D12:init] stage 3.1 D3D12CreateDevice (FL=0x"
            << std::hex << static_cast<unsigned long>(level) << std::dec
            << ", adapter=" << (warp_ ? "WARP" : "hw") << ")" << std::endl;
        hr = D3D12CreateDevice(adapter_.Get(), level, IID_PPV_ARGS(&device_));
        if (SUCCEEDED(hr)) break;
    }
    if (FAILED(hr)) {
        PRIMAL_LOG_STREAM(Error, "RHI")
            << "[D3D12Device] D3D12CreateDevice failed hr=0x"
            << std::hex << static_cast<unsigned long>(hr) << std::dec << std::endl;
        return false;
    }
    PRIMAL_LOG_STREAM(Info, "RHI.D3D12") << "[D3D12:init] stage 3.2 device created" << std::endl;

    // InfoQueue fail-fast（debug/验证开启时）：腐蚀/错误级别直接断点 + 全量落日志。
    if (desc_.enableDebug || desc_.enableValidation) {
        if (SUCCEEDED(device_.As(&infoQueue_))) {
            infoQueue_->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
            infoQueue_->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
            infoQueue_->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, FALSE);
        }
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;   // M1 单 DIRECT 队列
    queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.NodeMask = 0;
    if (FAILED(device_->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&graphicsQueue_)))) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[D3D12Device] DIRECT queue creation failed" << std::endl;
        return false;
    }
    PRIMAL_LOG_STREAM(Info, "RHI.D3D12") << "[D3D12:init] stage 3.3 DIRECT queue created" << std::endl;
    return true;
}

bool D3D12Device::createAllocator() {
    // F0-4（M0 批次 B 接线）: D3D12MemAlloc v3.0.1。
    D3D12MA::ALLOCATOR_DESC allocDesc{};
    // [R6/Windows] MSVC C2440：宏展开为 enum|enum（提升 int），int→enum
    // 无隐式转换（D3D12MA v3.0.1 的 ALLOCATOR_FLAGS 是普通未限定作用域 enum，
    // 头文件 L1060）。显式函数式转换收窄回枚举类型。
    allocDesc.Flags = D3D12MA::ALLOCATOR_FLAGS(D3D12MA_RECOMMENDED_ALLOCATOR_FLAGS);
    allocDesc.pDevice = device_.Get();
    allocDesc.pAdapter = adapter_.Get();  // AddRef 由 allocator 内部管理
    allocDesc.PreferredBlockSize = 0;     // 0 = 默认 64 MiB 块
    allocDesc.pAllocationCallbacks = nullptr;

    HRESULT hr = D3D12MA::CreateAllocator(&allocDesc, &d3d12maAllocator_);
    if (FAILED(hr) || !d3d12maAllocator_) {
        PRIMAL_LOG_STREAM(Error, "RHI")
            << "[D3D12Device] D3D12MA::CreateAllocator failed hr=0x"
            << std::hex << static_cast<unsigned long>(hr) << std::dec << std::endl;
        d3d12maAllocator_ = nullptr;
        return false;  // fail-fast：allocator 是资源层前置，创建失败=初始化失败
    }
    PRIMAL_LOG_STREAM(Info, "RHI.D3D12") << "[D3D12:init] stage 5.1 allocator created" << std::endl;
    return true;
}

bool D3D12Device::createResourceSubsystems() {
    if (!descriptorHeaps_.Initialize(device_.Get(),
                                     d3d12::D3D12DescriptorHeapManager::Config{})) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[D3D12Device] descriptor heaps init failed" << std::endl;
        return false;
    }
    uploadRing_.Initialize(this);
    PRIMAL_LOG_STREAM(Info, "RHI.D3D12") << "[D3D12:init] stage 6.1 descriptor heaps + upload ring ready" << std::endl;
    return true;
}

void D3D12Device::reportDeviceRemoval(HRESULT reason) {
    // M7 杂项修复：原字符串 "hr=0x" 配十进制值（格式误导）；改十进制直书。
    std::string message = "[D3D12Device] device removed: hr="
        + std::to_string(static_cast<unsigned long>(reason));
    switch (reason) {
        case DXGI_ERROR_DEVICE_HUNG: message += " (DEVICE_HUNG)"; break;
        case DXGI_ERROR_DEVICE_REMOVED: message += " (DEVICE_REMOVED)"; break;
        case DXGI_ERROR_DEVICE_RESET: message += " (DEVICE_RESET)"; break;
        case DXGI_ERROR_DRIVER_INTERNAL_ERROR: message += " (DRIVER_INTERNAL_ERROR)"; break;
        case DXGI_ERROR_INVALID_CALL: message += " (INVALID_CALL)"; break;
        default: break;
    }
    // InfoQueue 尾部消息：丢失瞬间的驱动侧诊断。
    if (infoQueue_) {
        const u64 count = infoQueue_->GetNumStoredMessages();
        const u64 tail = count > 8 ? count - 8 : 0;  // 最多 8 条
        for (u64 i = tail; i < count; ++i) {
            SIZE_T size = 0;
            if (FAILED(infoQueue_->GetMessage(i, nullptr, &size)) || size == 0) continue;
            std::string buffer(size, '\0');
            if (FAILED(infoQueue_->GetMessage(
                    i, reinterpret_cast<D3D12_MESSAGE*>(buffer.data()), &size))) {
                continue;
            }
            const auto* msg = reinterpret_cast<const D3D12_MESSAGE*>(buffer.data());
            message += "\n  [InfoQueue] ";
            message.append(msg->pDescription ? msg->pDescription : "",
                           msg->pDescription ? msg->DescriptionByteLength : 0);
        }
    }
    fencePool_.OnDeviceRemoved();
    PRIMAL_LOG_STREAM(Error, "RHI") << message << std::endl;
}

// ============================================================================
// 生命周期 / 帧循环
// ============================================================================

void D3D12Device::shutdownImpl() {
    // 等 GPU 全安静再释放（fence 目标值语义，无帧数算术）。
    if (device_ && graphicsQueue_ && fencePool_.GetCompletedValue() != UINT64_MAX) {
        const u64 target = fencePool_.NextTarget();
        if (fencePool_.Signal(graphicsQueue_.Get(), target)) {
            if (!fencePool_.Wait(target, 5000)) {
                // R8 埋点：关闭期 fence 等待有限超时（5s）可见化。
                PRIMAL_LOG_STREAM(Warn, "RHI.D3D12")
                    << "[D3D12:shutdown] final fence wait TIMEOUT (5000ms, target="
                    << target << ")" << std::endl;
            }
        }
    }

    // M1 资源层子系统：逆初始化序（先 staging/描述符，再 allocator/device）。
    uploadRing_.Shutdown();
    descriptorHeaps_.Shutdown();

    // 各类型分配器整队释放（触发资源析构 → GC 延迟入队），随后强制排干
    // （基类 gc_.Shutdown 在本函数之前已跑过一次 Flush；析构期间新入队的
    // 延迟项在此显式收尾——WaitIdle 已保证 GPU 静止，立即执行安全）。
    bufferAllocator_.Shutdown();
    textureAllocator_.Shutdown();
    shaderAllocator_.Shutdown();
    pipelineAllocator_.Shutdown();
    samplerAllocator_.Shutdown();
    descriptorSetLayoutAllocator_.Shutdown();
    pipelineLayoutAllocator_.Shutdown();
    descriptorSetAllocator_.Shutdown();
    commandBufferAllocator_.Shutdown();
    syncAllocator_.Shutdown();
    renderPasses_.clear();
    stateTracker_ = d3d12::D3D12StateTracker{};
    gc_.Flush();

    // F0-4: D3D12MA allocator 释放（Release，与 createAllocator 严格成对）。
    // 先于 device_.Reset()——allocator 持有 device/adapter 引用。
    if (d3d12maAllocator_) {
        d3d12maAllocator_->Release();
        d3d12maAllocator_ = nullptr;
    }
    // Debug 对象泄漏报告：release 本体前先摘除设备引用。
    if (device_ && infoQueue_) {
        infoQueue_.Reset();
    }
    if (device_) {
        ComPtr<ID3D12DebugDevice> debugDevice;
        if (SUCCEEDED(device_.As(&debugDevice))) debugDevice->ReportLiveDeviceObjects(
            D3D12_RLDO_SUMMARY | D3D12_RLDO_IGNORE_INTERNAL);
    }
    fencePool_.Shutdown();
    graphicsQueue_.Reset();
    device_.Reset();
    adapter_.Reset();
    dxgiFactory_.Reset();
    debugController_.Reset();
    warp_ = false;
}

void D3D12Device::waitIdleImpl() const {
    // const 语义下的等待：fence 目标值递增 + CPU 事件等待。
    auto* self = const_cast<D3D12Device*>(this);
    if (!device_ || !graphicsQueue_) return;
    const HRESULT reason = device_->GetDeviceRemovedReason();
    if (reason != S_OK) {
        self->reportDeviceRemoval(reason);
        return;
    }
    const u64 target = self->fencePool_.NextTarget();
    if (self->fencePool_.Signal(graphicsQueue_.Get(), target)) {
        if (!self->fencePool_.Wait(target, 10000)) {
            // R8 埋点：有限超时（10s）返回 false 而非挂起——日志可见即证
            // "超时"类症状不在此路径的无限等待（区分挂起点用）。
            PRIMAL_LOG_STREAM(Warn, "RHI.D3D12")
                << "[D3D12:init/wait] waitIdle fence wait TIMEOUT (10000ms, target="
                << target << ")" << std::endl;
        }
    }
}

void D3D12Device::beginFrameImpl() {
    currentFrameIndex_ = static_cast<u32>(frameCount_ % desc_.maxFramesInFlight);
    // 帧槽推进：staging 环 + 描述符槽轮转（复用判定 = fence 完成值）。
    uploadRing_.BeginFrame();
    descriptorHeaps_.BeginFrame(currentFrameIndex_, fencePool_);
}

void D3D12Device::endFrameImpl() {
    // 提交边界状态衰减（buffer 全衰减；提升态纹理衰减——D3D12 提交边界
    // 隐式行为，tracker 建模之）。衰减记录（信息性）在此丢弃。
    // 注：Submit 时点也已推进；此处兜底非 RenderSystem 宿主的裸录制路径。
    (void)stateTracker_.OnSubmitBoundary();
    // GC 推进由 CRTP 基类 EndFrame 完成（kGcOffset=3 对齐 MAX_FRAMES_IN_FLIGHT）。
}

void D3D12Device::presentImpl() {
    // Present 由 RenderSystem 经 RHISwapChain::Present 驱动（Vulkan 同构）；
    // 设备层无动作。
}

void D3D12Device::queryDeviceInfo(DeviceInfo& info) {
    info.platform = RHIPlatform::D3D12;
    if (adapter_) {
        DXGI_ADAPTER_DESC3 desc3{};
        if (SUCCEEDED(adapter_->GetDesc3(&desc3))) {
            // WCHAR -> char（设备名 ASCII 化；非 ASCII 截断为 '?'，诊断用途足够）
            for (u32 i = 0; i < 255 && desc3.Description[i]; ++i) {
                info.deviceName[i] = static_cast<char>(
                    desc3.Description[i] < 128 ? desc3.Description[i] : L'?');
            }
            info.dedicatedVideoMemory = desc3.DedicatedVideoMemory;
            info.sharedSystemMemory = desc3.SharedSystemMemory;
        }
    }
    if (device_) {
        D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT va{};
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
        // 能力位按 Master Plan 功能面逐步填充；M0 仅记录 raytracing/mesh。
        // [R6/Windows] 真名核对（官方 DirectX-Headers d3d12.h）：OPTIONS7 字段
        // 是 MeshShaderTier（L3182），枚举是 D3D12_MESH_SHADER_TIER（L3166，
        // D3D12_MESH_SHADER_TIER_NOT_SUPPORTED=0 / _TIER_1=10）——
        // MeshShaderPipelineTier / D3D12_MESH_SHADER_PIPELINE_TIER_* 均不存在。
        D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7{};
        if (SUCCEEDED(device_->CheckFeatureSupport(
                D3D12_FEATURE_D3D12_OPTIONS7, &options7, sizeof(options7)))) {
            info.supportsMeshShaders = options7.MeshShaderTier
                != D3D12_MESH_SHADER_TIER_NOT_SUPPORTED;
        }
        (void)va; (void)options5;
    }
}

u32 D3D12Device::getCurrentFrameIndexImpl() const { return currentFrameIndex_; }

// ============================================================================
// M5 F5-2：indirect command signature（惰性创建）
// ============================================================================

ID3D12CommandSignature* D3D12Device::GetDrawIndirectSignature() {
    if (drawIndirectSignature_) return drawIndirectSignature_.Get();
    // 布局=VkDrawIndirectCommand 4×u32（vertexCount/instanceCount/firstVertex/
    // firstInstance）≡ D3D12_DRAW_ARGUMENTS 字段序（VertexCountPerInstance/
    // InstanceCount/StartVertexLocation/StartInstanceLocation）——字节级一致。
    D3D12_INDIRECT_ARGUMENT_DESC arg{};
    arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
    D3D12_COMMAND_SIGNATURE_DESC desc{};
    desc.ByteStride = kIndirectDrawArgStride;  // 16
    desc.NumArgumentDescs = 1;
    desc.pArgumentDescs = &arg;
    if (FAILED(device_->CreateCommandSignature(&desc, nullptr,
                                               IID_PPV_ARGS(&drawIndirectSignature_)))) {
        PRIMAL_LOG_STREAM(Error, "RHI")
            << "[D3D12Device] CreateCommandSignature(DRAW) failed" << std::endl;
        return nullptr;
    }
    return drawIndirectSignature_.Get();
}

ID3D12CommandSignature* D3D12Device::GetDrawIndexedIndirectSignature() {
    if (drawIndexedIndirectSignature_) return drawIndexedIndirectSignature_.Get();
    // 布局=VkDrawIndexedIndirectCommand 5×u32 = 20B（indexCount/instanceCount/
    // firstIndex/vertexOffset/firstInstance）≡ D3D12_DRAW_INDEXED_ARGUMENTS
    // 字段序（IndexCountPerInstance/InstanceCount/StartIndexLocation/
    // BaseVertexLocation/StartInstanceLocation）——字节级一致，零翻译。
    D3D12_INDIRECT_ARGUMENT_DESC arg{};
    arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    D3D12_COMMAND_SIGNATURE_DESC desc{};
    desc.ByteStride = kIndirectDrawIndexedArgStride;  // 20
    desc.NumArgumentDescs = 1;
    desc.pArgumentDescs = &arg;
    if (FAILED(device_->CreateCommandSignature(&desc, nullptr,
                                               IID_PPV_ARGS(&drawIndexedIndirectSignature_)))) {
        PRIMAL_LOG_STREAM(Error, "RHI")
            << "[D3D12Device] CreateCommandSignature(DRAW_INDEXED) failed" << std::endl;
        return nullptr;
    }
    return drawIndexedIndirectSignature_.Get();
}

// [R6/Windows] M5 只交付了 DRAW/DRAW_INDEXED 两个签名；DispatchIndirect 误写为
// 不存在的 ID3D12GraphicsCommandList::DispatchIndirect（MSVC C2039）。compute
// indirect 的正规形态 = DISPATCH 类型 CommandSignature + ExecuteIndirect。
// 布局=VkDispatchIndirectCommand 3×u32(x,y,z)=12B ≡ D3D12_DISPATCH_ARGUMENTS
// 字段序（ThreadGroupCountX/Y/Z）——字节级一致，零翻译。
ID3D12CommandSignature* D3D12Device::GetDispatchIndirectSignature() {
    if (dispatchIndirectSignature_) return dispatchIndirectSignature_.Get();
    D3D12_INDIRECT_ARGUMENT_DESC arg{};
    arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_COMMAND_SIGNATURE_DESC desc{};
    desc.ByteStride = kIndirectDispatchArgStride;  // 12
    desc.NumArgumentDescs = 1;
    desc.pArgumentDescs = &arg;
    if (FAILED(device_->CreateCommandSignature(&desc, nullptr,
                                               IID_PPV_ARGS(&dispatchIndirectSignature_)))) {
        PRIMAL_LOG_STREAM(Error, "RHI")
            << "[D3D12Device] CreateCommandSignature(DISPATCH) failed" << std::endl;
        return nullptr;
    }
    return dispatchIndirectSignature_.Get();
}
double D3D12Device::getTimestampPeriodImpl() const { return timestampPeriodNs_; }

// ============================================================================
// Per-type lookup
// ============================================================================

d3d12::D3D12Texture* D3D12Device::GetTexture(ResourceHandle handle) {
    if (handle == handles::INVALID_RESOURCE) return nullptr;
    return textureAllocator_.Get(static_cast<u32>(handle));
}
d3d12::D3D12Buffer* D3D12Device::GetBuffer(ResourceHandle handle) {
    if (handle == handles::INVALID_RESOURCE) return nullptr;
    return bufferAllocator_.Get(static_cast<u32>(handle));
}
d3d12::D3D12Shader* D3D12Device::GetShader(ShaderHandle handle) {
    if (handle == handles::INVALID_SHADER) return nullptr;
    return shaderAllocator_.Get(static_cast<u32>(handle));
}
d3d12::D3D12Pipeline* D3D12Device::GetPipeline(PipelineHandle handle) {
    if (handle == handles::INVALID_PIPELINE) return nullptr;
    return pipelineAllocator_.Get(static_cast<u32>(handle));
}
d3d12::D3D12PipelineLayout* D3D12Device::GetPipelineLayout(PipelineLayoutHandle handle) {
    if (handle == handles::INVALID_PIPELINE_LAYOUT) return nullptr;
    return pipelineLayoutAllocator_.Get(static_cast<u32>(handle));
}
d3d12::D3D12DescriptorSetLayout* D3D12Device::GetDescriptorSetLayout(
    DescriptorSetLayoutHandle handle) {
    if (handle == handles::INVALID_DESCRIPTOR_SET_LAYOUT) return nullptr;
    return descriptorSetLayoutAllocator_.Get(static_cast<u32>(handle));
}
d3d12::D3D12DescriptorSet* D3D12Device::GetDescriptorSet(DescriptorSetHandle handle) {
    if (handle == handles::INVALID_DESCRIPTOR_SET) return nullptr;
    return descriptorSetAllocator_.Get(static_cast<u32>(handle));
}
d3d12::D3D12Sampler* D3D12Device::GetSampler(SamplerHandle handle) {
    if (handle == handles::INVALID_SAMPLER) return nullptr;
    return samplerAllocator_.Get(static_cast<u32>(handle));
}
d3d12::D3D12CommandBufferImpl* D3D12Device::GetCommandBuffer(CommandBufferHandle handle) {
    if (handle == handles::INVALID_COMMAND_BUFFER) return nullptr;
    return commandBufferAllocator_.Get(static_cast<u32>(handle));
}
d3d12::D3D12SyncObject* D3D12Device::GetSync(SyncHandle handle) {
    if (handle == handles::INVALID_SYNC) return nullptr;
    return syncAllocator_.Get(static_cast<u32>(handle));
}
const RenderPassDesc* D3D12Device::GetRenderPassDesc(RenderPassHandle handle) const {
    for (const auto& [h, desc] : renderPasses_) {
        if (h == handle) return &desc;
    }
    return nullptr;
}

// ============================================================================
// F0-11 公共契约：诊断查询（M1 接通）
// ============================================================================

bool D3D12Device::GetTextureSize(ResourceHandle texture, u32& width, u32& height) {
    d3d12::D3D12Texture* tex = GetTexture(texture);
    if (!tex) return false;
    width = tex->GetTextureDesc().size.x;
    height = tex->GetTextureDesc().size.y;
    return true;
}

DataFormat D3D12Device::GetTextureFormat(ResourceHandle texture) {
    d3d12::D3D12Texture* tex = GetTexture(texture);
    return tex ? tex->GetTextureDesc().format : DataFormat::Unknown;
}

bool D3D12Device::GetBufferByteSize(ResourceHandle buffer, u64& bytes) {
    d3d12::D3D12Buffer* buf = GetBuffer(buffer);
    if (!buf) { bytes = 0; return false; }
    bytes = buf->GetBufferDesc().size;
    return true;
}

// ============================================================================
// F1-1 交换链
// ============================================================================

RHISwapChain* D3D12Device::createSwapChainImpl(const SwapChainDesc& desc) {
    auto* sc = new d3d12::D3D12SwapChain(*this, desc);
    if (!sc->Initialize()) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[D3D12Device] D3D12SwapChain::Initialize failed" << std::endl;
        delete sc;
        return nullptr;
    }
    return sc;
}

void D3D12Device::destroySwapChainImpl(RHISwapChain* swapChain) {
    // backbuffer wrap 纹理由 D3D12SwapChain::Destroy 先行释放（销毁顺序
    // 契约：wrap 先于 IDXGISwapChain）。
    delete swapChain;
}

ResourceHandle D3D12Device::WrapSwapChainBuffer(RHISwapChain& /*owner*/,
                                               const TextureDesc& desc,
                                               ID3D12Resource* buffer) {
    const u32 id = textureAllocator_.Allocate(*this, desc, buffer);
    d3d12::D3D12Texture* tex = textureAllocator_.Get(id);
    if (!tex || !tex->Initialize()) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[D3D12Device] backbuffer wrap init failed" << std::endl;
        textureAllocator_.Free(id);
        return handles::INVALID_RESOURCE;
    }
    // tracker 注册：Present 初始态（D3D12 位值 = COMMON；渲染前经
    // BeginRenderPass 显式转 RENDER_TARGET）。
    stateTracker_.RegisterResource(ResourceHandle(id), true, desc.mipLevels,
                                   desc.arraySize, 1, ResourceState::Present);
    return ResourceHandle(id);
}

void D3D12Device::UnwrapSwapChainBuffer(ResourceHandle handle) {
    if (handle == handles::INVALID_RESOURCE) return;
    stateTracker_.UnregisterResource(handle);
    textureAllocator_.Free(static_cast<u32>(handle));
}

// ============================================================================
// F1-3 Buffer / Texture
// ============================================================================

ResourceHandle D3D12Device::createBufferImpl(const BufferDesc& desc) {
    const u32 id = bufferAllocator_.Allocate(*this, desc);
    d3d12::D3D12Buffer* buffer = bufferAllocator_.Get(id);
    if (!buffer || !buffer->Initialize()) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[D3D12Device] buffer create failed name='"
                                        << desc.name << "'" << std::endl;
        bufferAllocator_.Free(id);
        return handles::INVALID_RESOURCE;
    }
    // M5 F5-2/F5-4：buffer 也进 StateTracker（D3D12Buffer.h 的 COMMON 提升
    // 注释仍成立——tracker 规则② COMMON→任意状态零 barrier 提升；注册后
    // UAV→INDIRECT_ARGUMENT 等 buffer 显式转换才能产出 op（F5-2 arg buffer
    // 状态流转，对齐 D3D12ExecuteIndirect 官方样例模式）。BufferDesc 无
    // initialState → 恒按 COMMON（General）注册。
    stateTracker_.RegisterResource(ResourceHandle(id), false, 1, 1, 1,
                                   ResourceState::General);
    return ResourceHandle(id);
}

void D3D12Device::destroyBufferImpl(ResourceHandle handle) {
    if (handle == handles::INVALID_RESOURCE) return;
    if (!GetBuffer(handle)) {
        PRIMAL_LOG_STREAM(Error, "RHI")
            << "[D3D12Device] double-free buffer handle=" << handle << " — skipping" << std::endl;
        return;
    }
    stateTracker_.UnregisterResource(handle);  // M5：与纹理对称
    bufferAllocator_.Free(static_cast<u32>(handle));
}

void* D3D12Device::mapBufferImpl(ResourceHandle handle, u64 offset, u64 size) {
    d3d12::D3D12Buffer* buffer = GetBuffer(handle);
    return buffer ? buffer->Map(offset, size) : nullptr;
}

void D3D12Device::unmapBufferImpl(ResourceHandle handle) {
    d3d12::D3D12Buffer* buffer = GetBuffer(handle);
    if (buffer) buffer->Unmap();
}

void D3D12Device::setBufferDirtySizeImpl(ResourceHandle /*handle*/, u64 /*size*/) {
    // 持久映射路径无需 dirty 通知（Vulkan/Metal 同为 no-op）。
}

ResourceHandle D3D12Device::createTextureImpl(const TextureDesc& desc) {
    const u32 id = textureAllocator_.Allocate(*this, desc);
    d3d12::D3D12Texture* texture = textureAllocator_.Get(id);
    if (!texture || !texture->Initialize()) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[D3D12Device] texture create failed name='"
                                        << desc.name << "'" << std::endl;
        textureAllocator_.Free(id);
        return handles::INVALID_RESOURCE;
    }
    // tracker 注册（mip/array/plane 几何 + COMMON 初始态——写类使用须显式
    // transition；读类可从 COMMON 自动提升，见 D3D12StateTracker 文件头）。
    // M6 F6-3：TEXTURE3D 无数组维（D3D12 DepthOrArraySize=depth；子资源数
    // = mipLevels×1×plane）——arraySize 恒按 1 注册（防御误传 >1 时模型
    // 与 D3D12 实际子资源几何脱钩；单测 TestM6Texture3DSubresourceModel）。
    d3d12::D3D12Texture* stored = textureAllocator_.Get(id);
    stateTracker_.RegisterResource(ResourceHandle(id), true, desc.mipLevels,
                                   desc.type == TextureType::Texture3D
                                       ? 1u : desc.arraySize,
                                   stored->PlaneCount(),
                                   ResourceState::General);
    return ResourceHandle(id);
}

void D3D12Device::destroyTextureImpl(ResourceHandle handle) {
    if (handle == handles::INVALID_RESOURCE) return;
    if (!GetTexture(handle)) {
        PRIMAL_LOG_STREAM(Error, "RHI")
            << "[D3D12Device] double-free texture handle=" << handle << " — skipping" << std::endl;
        return;
    }
    stateTracker_.UnregisterResource(handle);
    textureAllocator_.Free(static_cast<u32>(handle));
}

ResourceHandle D3D12Device::createTextureViewImpl(const TextureViewDesc& desc) {
    d3d12::D3D12Texture* src = GetTexture(desc.texture);
    if (!src) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[D3D12Device] texture view: src unresolved" << std::endl;
        return handles::INVALID_RESOURCE;
    }
    const u32 id = textureAllocator_.Allocate(*this, desc, *src);
    d3d12::D3D12Texture* view = textureAllocator_.Get(id);
    if (!view || !view->Initialize()) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[D3D12Device] texture view init failed" << std::endl;
        textureAllocator_.Free(id);
        return handles::INVALID_RESOURCE;
    }
    // view 不重复注册 tracker（状态与源纹理共享同一物理资源）。
    return ResourceHandle(id);
}

// ============================================================================
// F1-7 Shader（DXIL 承载；入口恒 main）
// ============================================================================

ShaderHandle D3D12Device::createShaderImpl(const void* data, size_t size, ShaderStage stage,
                                          const char* entryPoint) {
    const u32 id = shaderAllocator_.Allocate(*this, data, size, stage, entryPoint);
    d3d12::D3D12Shader* shader = shaderAllocator_.Get(id);
    if (!shader || !shader->Initialize()) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[D3D12Device] shader create failed (DXIL validate)" << std::endl;
        shaderAllocator_.Free(id);
        return handles::INVALID_SHADER;
    }
    shader->SetHandle(ShaderHandle(id));
    return ShaderHandle(id);
}

void D3D12Device::destroyShaderImpl(ShaderHandle handle) {
    if (handle == handles::INVALID_SHADER) return;
    shaderAllocator_.Free(static_cast<u32>(handle));
}

// ============================================================================
// F1-4 Pipeline / PipelineLayout
// ============================================================================

PipelineHandle D3D12Device::createGraphicsPipelineImpl(const GraphicsPipelineDesc& desc) {
    const u32 id = pipelineAllocator_.Allocate(*this);
    d3d12::D3D12Pipeline* pipeline = pipelineAllocator_.Get(id);
    if (!pipeline || !pipeline->InitializeGraphics(desc)) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[D3D12Device] graphics pipeline create failed" << std::endl;
        pipelineAllocator_.Free(id);
        return handles::INVALID_PIPELINE;
    }
    pipeline->SetHandle(PipelineHandle(id));
    return PipelineHandle(id);
}

PipelineHandle D3D12Device::createComputePipelineImpl(const ComputePipelineDesc& desc) {
    const u32 id = pipelineAllocator_.Allocate(*this);
    d3d12::D3D12Pipeline* pipeline = pipelineAllocator_.Get(id);
    if (!pipeline || !pipeline->InitializeCompute(desc)) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[D3D12Device] compute pipeline create failed" << std::endl;
        pipelineAllocator_.Free(id);
        return handles::INVALID_PIPELINE;
    }
    pipeline->SetHandle(PipelineHandle(id));
    return PipelineHandle(id);
}

void D3D12Device::destroyPipelineImpl(PipelineHandle handle) {
    if (handle == handles::INVALID_PIPELINE) return;
    if (d3d12::D3D12Pipeline* p = GetPipeline(handle)) p->Destroy();
    pipelineAllocator_.Free(static_cast<u32>(handle));
}

// ============================================================================
// RenderPass（desc 存储；BeginRenderPass(handle) 重放）
// ============================================================================

RenderPassHandle D3D12Device::createRenderPassImpl(const RenderPassDesc& desc) {
    const RenderPassHandle handle = RenderPassHandle(nextRenderPassHandle_++);
    renderPasses_.emplace_back(handle, desc);
    return handle;
}

void D3D12Device::destroyRenderPassImpl(RenderPassHandle handle) {
    for (auto it = renderPasses_.begin(); it != renderPasses_.end(); ++it) {
        if (it->first == handle) {
            renderPasses_.erase(it);
            return;
        }
    }
}

// ============================================================================
// F1-6 CommandBuffer / Submit
// ============================================================================

CommandBufferHandle D3D12Device::createCommandBufferImpl(CommandQueueType type) {
    const u32 id = commandBufferAllocator_.Allocate(*this, type);
    d3d12::D3D12CommandBufferImpl* cmd = commandBufferAllocator_.Get(id);
    if (!cmd || !cmd->Initialize()) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[D3D12Device] command buffer create failed" << std::endl;
        commandBufferAllocator_.Free(id);
        return handles::INVALID_COMMAND_BUFFER;
    }
    cmd->SetHandle(CommandBufferHandle(id));
    RegisterCommandBuffer(cmd);
    return CommandBufferHandle(id);
}

void D3D12Device::destroyCommandBufferImpl(CommandBufferHandle handle) {
    if (handle == handles::INVALID_COMMAND_BUFFER) return;
    d3d12::D3D12CommandBufferImpl* cmd = GetCommandBuffer(handle);
    if (!cmd) return;
    // Submitted 态兜底同步（Vulkan B8 同款：destroy 前补齐 fence 等待）。
    if (cmd->GetState() == CommandBufferState::Submitted) {
        cmd->WaitForCompletion();
    }
    UnregisterCommandBuffer(handle);
    commandBufferAllocator_.Free(static_cast<u32>(handle));
}

u64 D3D12Device::ExecuteAndSignal(ID3D12GraphicsCommandList* list) {
    if (!list || !graphicsQueue_) return 0;
    ID3D12CommandList* lists[] = {list};
    graphicsQueue_->ExecuteCommandLists(1, lists);
    const u64 target = fencePool_.NextTarget();
    if (!fencePool_.Signal(graphicsQueue_.Get(), target)) return 0;
    return target;
}

bool D3D12Device::submitImpl(const QueueSubmitInfo& info) {
    if (info.cmdBuffer == handles::INVALID_COMMAND_BUFFER) return false;
    d3d12::D3D12CommandBufferImpl* cmd = GetCommandBuffer(info.cmdBuffer);
    if (!cmd || !cmd->GetCommandList()) return false;

    // 设备丢失 fail-fast 观测。
    const HRESULT reason = device_->GetDeviceRemovedReason();
    if (reason != S_OK) {
        reportDeviceRemoval(reason);
        return false;
    }

    // wait/signalSemaphore：D3D12 无 GPU-GPU semaphore——接受并忽略
    // （单 DIRECT 队列有序；一次提示在 D3D12SwapChain/D3D12Sync 层）。
    // 提交边界状态衰减（tracker）：GPU 在 ExecuteCommandLists 完成后对
    // buffer 全衰减、提升态纹理衰减——tracker 在提交时点推进建模。
    (void)stateTracker_.OnSubmitBoundary();

    const u64 target = ExecuteAndSignal(cmd->GetCommandList());
    if (target == 0) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[D3D12Device] ExecuteCommandLists/Signal failed" << std::endl;
        return false;
    }

    // 帧槽设施的护栏登记（该提交消费本槽描述符/staging 分配）。
    descriptorHeaps_.OnSubmit(target);
    uploadRing_.OnSubmitConsumed(target);

    // signalFence（RenderSystem frameFences_）：目标值 = 该 sync 对象上次 +1，
    // 队列 Signal——WaitForSync 即 CPU 事件等待。无 signalFence 时同步等
    // 待共享 fence，供命令缓冲 destroy 兜底。
    if (info.signalFence != handles::INVALID_SYNC) {
        if (d3d12::D3D12SyncObject* sync = GetSync(info.signalFence)) {
            const u64 syncTarget = sync->NextSignalValue();
            if (FAILED(graphicsQueue_->Signal(sync->GetFence(), syncTarget))) {
                PRIMAL_LOG_STREAM(Error, "RHI") << "[D3D12Device] signalFence Signal failed" << std::endl;
                return false;
            }
        }
    }
    cmd->OnDeviceSubmit(target);
    cmd->SetState(CommandBufferState::Submitted);  // Vulkan B8 同款状态推进
    return true;
}

// ============================================================================
// Sync（per-object ID3D12Fence；单调目标值，无 reset 竞态）
// ============================================================================

SyncHandle D3D12Device::createSyncImpl() {
    const u32 id = syncAllocator_.Allocate();
    d3d12::D3D12SyncObject* sync = syncAllocator_.Get(id);
    if (!sync || !sync->Initialize(device_.Get())) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[D3D12Device] sync create failed" << std::endl;
        syncAllocator_.Free(id);
        return handles::INVALID_SYNC;
    }
    // 初始 completed=0 且 lastSignal=0：首帧 WaitForSync 立即通过（对齐
    // Vulkan createSyncImpl 的"初始 signaled"语义；单调值免 reset）。
    return SyncHandle(id);
}

bool D3D12Device::waitForSyncImpl(SyncHandle handle, u32 timeoutMs) {
    d3d12::D3D12SyncObject* sync = GetSync(handle);
    if (!sync) return false;
    return sync->Wait(sync->LastSignalValue(), timeoutMs);
}

void D3D12Device::destroySyncImpl(SyncHandle handle) {
    if (handle == handles::INVALID_SYNC) return;
    syncAllocator_.Free(static_cast<u32>(handle));
}

// ============================================================================
// Query pool（M1 清单外——显式不支持，M3+ timestamp 接管）
// ============================================================================

QueryPoolHandle D3D12Device::createQueryPoolImpl(const QueryPoolDesc&) {
    PRIMAL_LOG_STREAM(Warn, "RHI")
        << "[D3D12Device] query pool NOT-IMPLEMENTED in M1 (timestamp audit stub active)" << std::endl;
    return handles::INVALID_QUERY_POOL;
}
void D3D12Device::destroyQueryPoolImpl(QueryPoolHandle) {}
bool D3D12Device::getQueryPoolResultsImpl(QueryPoolHandle, u32, u32, void*, size_t) {
    return false;
}

// ============================================================================
// Sampler（F1-3/F1-5：sampler 堆持久槽位）
// ============================================================================

SamplerHandle D3D12Device::createSamplerImpl(const SamplerDesc& desc) {
    const u32 id = samplerAllocator_.Allocate();
    d3d12::D3D12Sampler* sampler = samplerAllocator_.Get(id);
    if (!sampler || !sampler->Initialize(device_.Get(), descriptorHeaps_, desc)) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[D3D12Device] sampler create failed" << std::endl;
        samplerAllocator_.Free(id);
        return handles::INVALID_SAMPLER;
    }
    sampler->SetHandle(SamplerHandle(id));
    return SamplerHandle(id);
}

void D3D12Device::destroySamplerImpl(SamplerHandle handle) {
    if (handle == handles::INVALID_SAMPLER) return;
    d3d12::D3D12Sampler* sampler = GetSampler(handle);
    if (!sampler) return;
    // 槽位释放走 GC（in-flight 帧可能仍引用该 sampler 描述符副本）。
    // 只捕获槽位值（不捕获对象指针——free_list 槽位可能先于 GC 被复用）。
    const u32 slot = sampler->Slot();
    d3d12::D3D12DescriptorHeapManager* heaps = &descriptorHeaps_;
    gc_.DeferredDestroy([slot, heaps]() { heaps->FreeSampler(slot); });
    samplerAllocator_.Free(static_cast<u32>(handle));
}

// ============================================================================
// Descriptor set 层（F1-5 + 绑定模型契约）
// ============================================================================

DescriptorSetLayoutHandle D3D12Device::createDescriptorSetLayoutImpl(
    const DescriptorSetLayoutDesc& desc) {
    const u32 id = descriptorSetLayoutAllocator_.Allocate(*this, desc);
    d3d12::D3D12DescriptorSetLayout* layout = descriptorSetLayoutAllocator_.Get(id);
    if (!layout || !layout->Initialize()) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[D3D12Device] descriptor set layout create failed" << std::endl;
        descriptorSetLayoutAllocator_.Free(id);
        return handles::INVALID_DESCRIPTOR_SET_LAYOUT;
    }
    return DescriptorSetLayoutHandle(id);
}

void D3D12Device::destroyDescriptorSetLayoutImpl(DescriptorSetLayoutHandle handle) {
    if (handle == handles::INVALID_DESCRIPTOR_SET_LAYOUT) return;
    descriptorSetLayoutAllocator_.Free(static_cast<u32>(handle));
}

PipelineLayoutHandle D3D12Device::createPipelineLayoutImpl(const PipelineLayoutDesc& desc) {
    const u32 id = pipelineLayoutAllocator_.Allocate(*this, desc);
    d3d12::D3D12PipelineLayout* layout = pipelineLayoutAllocator_.Get(id);
    if (!layout || !layout->Initialize()) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[D3D12Device] pipeline layout create failed "
                                        << "(root signature budget/serialize)" << std::endl;
        pipelineLayoutAllocator_.Free(id);
        return handles::INVALID_PIPELINE_LAYOUT;
    }
    return PipelineLayoutHandle(id);
}

void D3D12Device::destroyPipelineLayoutImpl(PipelineLayoutHandle handle) {
    if (handle == handles::INVALID_PIPELINE_LAYOUT) return;
    pipelineLayoutAllocator_.Free(static_cast<u32>(handle));
}

DescriptorSetHandle D3D12Device::createDescriptorSetImpl(const DescriptorSetDesc& desc) {
    const u32 id = descriptorSetAllocator_.Allocate(*this, desc);
    d3d12::D3D12DescriptorSet* set = descriptorSetAllocator_.Get(id);
    if (!set || !set->Initialize()) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[D3D12Device] descriptor set create failed "
                                        << "(frame-slot descriptor capacity?)" << std::endl;
        descriptorSetAllocator_.Free(id);
        return handles::INVALID_DESCRIPTOR_SET;
    }
    return DescriptorSetHandle(id);
}

void D3D12Device::destroyDescriptorSetImpl(DescriptorSetHandle handle) {
    if (handle == handles::INVALID_DESCRIPTOR_SET) return;
    descriptorSetAllocator_.Free(static_cast<u32>(handle));
}

void D3D12Device::EnsureSetRegionCurrent(d3d12::D3D12DescriptorSet* set) {
    if (!set) return;
    if (set->OwnerSlot() == currentFrameIndex_) return;  // 属主槽即当前槽
    ID3D12Device* native = device_.Get();
    d3d12::D3D12DescriptorHeapManager& heaps = descriptorHeaps_;

    // 主区段迁移（内容逐描述符拷贝——heap 描述符自包含）。
    if (set->MainCount() > 0) {
        u32 newBase = 0;
        if (!heaps.AllocShaderVisible(set->MainCount(), newBase)) return;
        for (u32 i = 0; i < set->MainCount(); ++i) {
            native->CopyDescriptorsSimple(
                1, heaps.CbvSrvUavCpuHandle(newBase + i),
                heaps.CbvSrvUavCpuHandle(set->BaseSlot() + i),
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
        set->baseSlot_ = newBase;
    }
    if (set->SamplerCount() > 0) {
        u32 newSamplerBase = 0;
        if (!heaps.AllocSamplerRange(set->SamplerCount(), newSamplerBase)) return;
        for (u32 i = 0; i < set->SamplerCount(); ++i) {
            native->CopyDescriptorsSimple(
                1, heaps.SamplerCpuHandle(newSamplerBase + i),
                heaps.SamplerCpuHandle(set->SamplerBaseSlot() + i),
                D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        }
        set->samplerBaseSlot_ = newSamplerBase;
    }
    set->ownerSlot_ = currentFrameIndex_;
}

void D3D12Device::updateDescriptorSetsImpl(u32 writeCount, const WriteDescriptorSet* writes) {
    if (!writes || writeCount == 0) return;
    ID3D12Device* native = device_.Get();

    for (u32 w = 0; w < writeCount; ++w) {
        const WriteDescriptorSet& write = writes[w];
        d3d12::D3D12DescriptorSet* set = GetDescriptorSet(write.dstSet);
        if (!set) continue;
        EnsureSetRegionCurrent(set);  // 长寿命 set 跨帧护栏（见声明注释）
        d3d12::D3D12DescriptorSetLayout* layout = GetDescriptorSetLayout(set->GetLayout());
        if (!layout) continue;

        d3d12::D3D12DescriptorSetLayout::BindingClass info;
        if (!layout->QueryBinding(write.dstBinding, info)) continue;

        for (u32 e = 0; e < write.descriptorCount; ++e) {
            // --- 主段（CBV/SRV/UAV）---
            if (info.hasMain && write.bufferInfo) {
                const DescriptorBufferInfo& bi = write.bufferInfo[e];
                d3d12::D3D12Buffer* buffer = GetBuffer(bi.buffer);
                if (!buffer) continue;
                const u32 slot = set->BaseSlot() + info.mainOffset + write.dstArrayElement + e;
                const D3D12_CPU_DESCRIPTOR_HANDLE dst = descriptorHeaps_.CbvSrvUavCpuHandle(slot);
                switch (info.mainType) {
                    case D3D12_DESCRIPTOR_RANGE_TYPE_CBV: {
                        // CBV 尺寸须 256 倍数；BufferLocation 256 对齐（烘焙偏移）。
                        const u64 range = bi.range ? bi.range
                                                   : buffer->GetBufferDesc().size - bi.offset;
                        D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{};
                        cbv.BufferLocation = buffer->GetGpuAddress() + bi.offset;
                        cbv.SizeInBytes = static_cast<UINT>((range + 255) & ~u64(255));
                        native->CreateConstantBufferView(&cbv, dst);
                        break;
                    }
                    case D3D12_DESCRIPTOR_RANGE_TYPE_SRV: {
                        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
                        srv.Format = DXGI_FORMAT_R32_TYPELESS;
                        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                        srv.Buffer.FirstElement = bi.offset / 4;
                        srv.Buffer.NumElements = static_cast<UINT>(
                            (bi.range ? bi.range : buffer->GetBufferDesc().size - bi.offset) / 4);
                        srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
                        native->CreateShaderResourceView(buffer->GetNativeResource(), &srv, dst);
                        break;
                    }
                    case D3D12_DESCRIPTOR_RANGE_TYPE_UAV: {
                        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
                        uav.Format = DXGI_FORMAT_R32_TYPELESS;
                        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
                        uav.Buffer.FirstElement = bi.offset / 4;
                        uav.Buffer.NumElements = static_cast<UINT>(
                            (bi.range ? bi.range : buffer->GetBufferDesc().size - bi.offset) / 4);
                        uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
                        native->CreateUnorderedAccessView(buffer->GetNativeResource(),
                                                          nullptr, &uav, dst);
                        break;
                    }
                    default: break;
                }
            }
            // --- 图像/sampler 段（拷贝现成描述符——纹理创建时已建 SRV/UAV）---
            else if (write.imageInfo) {
                const DescriptorImageInfo& ii = write.imageInfo[e];
                if (info.hasMain) {
                    d3d12::D3D12Texture* texture = GetTexture(ii.imageView);
                    if (!texture) continue;
                    const u32 srcSlot = info.mainType == D3D12_DESCRIPTOR_RANGE_TYPE_UAV
                        ? texture->UavSlot() : texture->SrvSlot();
                    if (srcSlot == d3d12::D3D12Texture::kInvalidDescriptorSlot) continue;
                    const u32 dstSlot = set->BaseSlot() + info.mainOffset
                                        + write.dstArrayElement + e;
                    native->CopyDescriptorsSimple(
                        1, descriptorHeaps_.CbvSrvUavCpuHandle(dstSlot),
                        descriptorHeaps_.CbvSrvUavCpuHandle(srcSlot),
                        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                }
                if (info.hasSampler) {
                    d3d12::D3D12Sampler* sampler = GetSampler(ii.sampler);
                    if (!sampler) continue;
                    const u32 dstSlot = set->SamplerBaseSlot() + info.samplerOffset
                                        + write.dstArrayElement + e;
                    native->CopyDescriptorsSimple(
                        1, descriptorHeaps_.SamplerCpuHandle(dstSlot),
                        descriptorHeaps_.SamplerCpuHandle(sampler->Slot()),
                        D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
                }
            }
        }
    }
}

// ============================================================================
// 上传路径（staging 环；帧内排队 / 帧外阻塞）
// ============================================================================

bool D3D12Device::UploadBufferDataViaRing(ID3D12Resource* dst, const void* data,
                                          u64 size, u64 dstOffset) {
    if (!dst || !data || size == 0) return false;
    d3d12::D3D12UploadRing::Allocation alloc = uploadRing_.Allocate(size);
    if (alloc.overflow) {
        // 容量耗尽：排干在飞后重试一次（Vulkan/Metal 同款约定）。
        uploadRing_.FlushBlocking();
        alloc = uploadRing_.Allocate(size);
        if (alloc.overflow) return false;
    }
    std::memcpy(alloc.cpuPtr, data, size);
    uploadRing_.QueueCopyBuffer(alloc, dst, dstOffset, size);
    if (!IsFrameRecording()) {
        uploadRing_.FlushBlocking();  // 资产加载期语义：立即落 GPU
    }
    return true;
}

bool D3D12Device::UploadTextureDataViaRing(ID3D12Resource* texture, ResourceHandle textureId,
                                           DataFormat format, u32 mipLevels,
                                           const void* data, u64 size,
                                           const TextureRegionUpdate& region) {
    if (!texture || !data || size == 0) return false;
    const u64 texelBytes = BytesPerTexel(format);
    if (texelBytes == 0) return false;

    // 区域级 upload 布局（RowPitch = align(区域宽×texel, 256)）。
    const d3d12::D3D12UploadRing::TextureUploadLayout layout =
        d3d12::D3D12UploadRing::ComputeRegionLayout(
            d3d12::ToDxgiFormat(format), static_cast<u32>(texelBytes),
            region.width, region.height, region.depth, 0);
    if (layout.totalBytes == 0 || size < layout.totalBytes) return false;

    d3d12::D3D12UploadRing::Allocation alloc = uploadRing_.Allocate(layout.totalBytes);
    if (alloc.overflow) {
        uploadRing_.FlushBlocking();
        alloc = uploadRing_.Allocate(layout.totalBytes);
        if (alloc.overflow) return false;
    }
    // 紧密行主序源 → footprint 行距布局（逐行拷贝；末行无 padding）。
    // M7 F7-7 静态终审修复：原实现只拷 depth 第 0 层——region.depth>1（M6
    // GI 3D 体积上传）时 staging 第 1..N-1 层保留上一轮脏数据，经
    // CopyTextureRegion 的全深度 BOX 上传到纹理即垃圾值。源布局=紧密深度
    // 平面（每层 w×texel×h 紧凑，与 updateRegion 的 expected 尺寸式一致）。
    const u64 srcRowBytes = u64(region.width) * texelBytes;
    const u64 srcSliceBytes = srcRowBytes * region.height;
    u8* dstPtr = static_cast<u8*>(alloc.cpuPtr);
    const u8* srcPtr = static_cast<const u8*>(data);
    for (u32 z = 0; z < region.depth; ++z) {
        u8* dstSlice = dstPtr + u64(z) * layout.rowPitch * region.height;
        const u8* srcSlice = srcPtr + u64(z) * srcSliceBytes;
        for (u32 y = 0; y < region.height; ++y) {
            std::memcpy(dstSlice + y * layout.rowPitch, srcSlice + y * srcRowBytes,
                        srcRowBytes);
        }
    }

    const u32 subresource = region.mipLevel + mipLevels * region.baseArrayLayer;
    // 拷贝后回置 ShaderResource（采样主路径；调用方 InsertBarrier 可再转）。
    uploadRing_.QueueCopyTexture(alloc, texture, textureId, subresource,
                                 layout.footprint, region.offsetX, region.offsetY,
                                 region.offsetZ, region.width, region.height,
                                 region.depth, ResourceState::ShaderResource);
    if (!IsFrameRecording()) {
        uploadRing_.FlushBlocking();
    }
    return true;
}

// ============================================================================
// ODR-bypass 虚接口（RHIDeviceBase）
// ============================================================================

bool D3D12Device::UpdateBufferData(ResourceHandle handle, const void* data, u64 size,
                                   u64 offset) {
    d3d12::D3D12Buffer* buffer = GetBuffer(handle);
    if (!buffer) return false;
    return buffer->UpdateData(data, size, offset);
}

bool D3D12Device::UpdateTextureRegion(ResourceHandle texture, const void* data, u64 size,
                                      const TextureRegionUpdate& region) {
    d3d12::D3D12Texture* tex = GetTexture(texture);
    if (!tex) return false;
    return tex->updateRegion(data, size, region);
}

} // namespace primal::graphics::rhi

#endif // defined(_WIN32) && defined(ENABLE_D3D12) && ENABLE_D3D12
