// Windows-only: requires ENABLE_D3D12 (_WIN32)
//
// D3D12DescriptorHeaps.cpp — 描述符堆管理实现（M1 F1-5）。
#include "D3D12DescriptorHeaps.h"

#if defined(_WIN32) && defined(ENABLE_D3D12) && ENABLE_D3D12

#include <algorithm>
#include <cassert>
#include <cstdlib>  // R21-C：FrameTraceEnabled 的 std::getenv
#include <string>   // R22-S1：LogSamplerSharingStats 的逐槽高水位拼接

#include "D3D12Abi.h"
#include "D3D12StateMapping.h"
#include "D3D12Fence.h"

#include "D3D12Device.h"

#include "Engine/Common/CoreLogStream.h"

namespace primal::graphics::rhi::d3d12 {

// ============================================================================
// SlotPool：持久槽位 free-list
// ============================================================================

void D3D12DescriptorHeapManager::SlotPool::Init(u32 capacity) {
    next_ = 0;
    capacity_ = capacity;
    free_.clear();
    free_.reserve(capacity);
}

u32 D3D12DescriptorHeapManager::SlotPool::Alloc() {
    if (!free_.empty()) {
        const u32 slot = free_.back();
        free_.pop_back();
        return slot;
    }
    if (next_ < capacity_) return next_++;
    return kInvalidSlot;
}

void D3D12DescriptorHeapManager::SlotPool::Free(u32 slot) {
    if (slot != kInvalidSlot && slot < capacity_) free_.push_back(slot);
}

// ============================================================================
// 初始化 / 关闭
// ============================================================================

namespace {

bool CreateTypedHeap(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type,
                     u32 capacity, bool shaderVisible,
                     Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>& out) {
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = type;
    desc.NumDescriptors = capacity;
    // shader-visible 标志：CBV/SRV/UAV 主堆与 Sampler 堆需要；RTV/DSV 不允许。
    desc.Flags = shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
                               : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    desc.NodeMask = 0;
    return SUCCEEDED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&out)));
}

} // namespace

bool D3D12DescriptorHeapManager::Initialize(ID3D12Device* device, const Config& config) {
    if (!device || config.frameSlotCount == 0) return false;

    if (!CreateTypedHeap(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                         config.cbvSrvUavCapacity, true, cbvSrvUavHeap_)) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[D3D12DescriptorHeaps] CBV/SRV/UAV heap creation failed" << std::endl;
        return false;
    }
    if (!CreateTypedHeap(device, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
                         config.samplerCapacity, true, samplerHeap_)) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[D3D12DescriptorHeaps] sampler heap creation failed" << std::endl;
        return false;
    }
    if (!CreateTypedHeap(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
                         config.rtvCapacity, false, rtvHeap_)) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[D3D12DescriptorHeaps] RTV heap creation failed" << std::endl;
        return false;
    }
    if (!CreateTypedHeap(device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV,
                         config.dsvCapacity, false, dsvHeap_)) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[D3D12DescriptorHeaps] DSV heap creation failed" << std::endl;
        return false;
    }
    // shadow 堆（R13/T1 双写模型，语义见头文件）：与 visible 堆同容量、
    // 非 shader-visible。失败=fatal（后续所有 Create*View 双写与 CopyDescriptors
    // 源读都依赖它们，缺省必然回到 #654 非法读）。
    if (!CreateTypedHeap(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                         config.cbvSrvUavCapacity, false, cbvSrvUavShadowHeap_)) {
        PRIMAL_LOG_STREAM(Error, "RHI")
            << "[D3D12DescriptorHeaps] CBV/SRV/UAV shadow heap creation failed" << std::endl;
        return false;
    }
    if (!CreateTypedHeap(device, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
                         config.samplerCapacity, false, samplerShadowHeap_)) {
        PRIMAL_LOG_STREAM(Error, "RHI")
            << "[D3D12DescriptorHeaps] sampler shadow heap creation failed" << std::endl;
        return false;
    }

    device_ = device;
    // R19-4：容量镜像（写入点断言上界；与 shadow/visible 两堆同值——两堆
    // 创建时均用同一 config 字段，见上方 CreateTypedHeap 调用）。
    cbvSrvUavCapacity_ = config.cbvSrvUavCapacity;
    samplerCapacity_ = config.samplerCapacity;
    cbvSrvUavDescriptorSize_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    samplerDescriptorSize_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    rtvDescriptorSize_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    dsvDescriptorSize_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    cbvSrvUavCpuStart_ = cbvSrvUavHeap_->GetCPUDescriptorHandleForHeapStart();
    cbvSrvUavGpuStart_ = cbvSrvUavHeap_->GetGPUDescriptorHandleForHeapStart();
    samplerCpuStart_ = samplerHeap_->GetCPUDescriptorHandleForHeapStart();
    samplerGpuStart_ = samplerHeap_->GetGPUDescriptorHandleForHeapStart();
    cbvSrvUavShadowCpuStart_ = cbvSrvUavShadowHeap_->GetCPUDescriptorHandleForHeapStart();
    samplerShadowCpuStart_ = samplerShadowHeap_->GetCPUDescriptorHandleForHeapStart();
    rtvCpuStart_ = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    dsvCpuStart_ = dsvHeap_->GetCPUDescriptorHandleForHeapStart();

    frameSlotCount_ = config.frameSlotCount;
    perSlotGuardTarget_.assign(frameSlotCount_, 0);
    // CBV/SRV/UAV 主堆布局：前段持久（纹理 SRV/UAV 拷贝源），后段帧槽线性
    // （set 实例表）。
    if (config.cbvSrvUavPersistentCapacity >= config.cbvSrvUavCapacity) {
        PRIMAL_LOG_STREAM(Error, "RHI")
            << "[D3D12DescriptorHeaps] cbvSrvUavPersistentCapacity >= capacity" << std::endl;
        return false;
    }
    cbvSrvUavLinearBase_ = config.cbvSrvUavPersistentCapacity;
    currentSlot_ = 0;
    mainLinear_.Init(cbvSrvUavLinearBase_,
                     (config.cbvSrvUavCapacity - cbvSrvUavLinearBase_) / frameSlotCount_,
                     frameSlotCount_);

    // sampler 堆布局：前段持久 SlotPool（SamplerHandle 对象），后段帧槽线性
    // （set 实例的 sampler 表——descriptor table 需要连续槽位）。
    if (config.samplerPersistentCapacity >= config.samplerCapacity) {
        PRIMAL_LOG_STREAM(Error, "RHI")
            << "[D3D12DescriptorHeaps] samplerPersistentCapacity >= samplerCapacity" << std::endl;
        return false;
    }
    samplerPersistentCapacity_ = config.samplerPersistentCapacity;
    samplerLinearBase_ = config.samplerPersistentCapacity;
    samplerLinear_.Init(samplerLinearBase_,
                        (config.samplerCapacity - samplerLinearBase_) / frameSlotCount_,
                        frameSlotCount_);
    // R22-S1：sampler 表内容级共享记账（策略见 D3D12DescriptorSlotPolicy.h）。
    samplerSharing_.Init(kSamplerSharingMaxEntries);

    cbvSrvUavPersistentSlots_.Init(config.cbvSrvUavPersistentCapacity);
    rtvSlots_.Init(config.rtvCapacity);
    dsvSlots_.Init(config.dsvCapacity);
    samplerSlots_.Init(config.samplerPersistentCapacity);
    // R18-B：容量随初始化落日志——十七测"31+2>32"式耗尽在 Windows 侧
    // 可直接对账（采样器堆 2048 规范上限满配，见头文件容量规划注）。
    PRIMAL_LOG_STREAM(Info, "RHI")
        << "[D3D12DescriptorHeaps] heaps ready: cbvSrvUav=" << config.cbvSrvUavCapacity
        << " (persistent " << config.cbvSrvUavPersistentCapacity
        << ", per-slot " << mainLinear_.SlotCapacity() << ")"
        << ", sampler=" << config.samplerCapacity
        << " (persistent " << config.samplerPersistentCapacity
        << ", per-slot " << samplerLinear_.SlotCapacity() << ")"
        << ", rtv=" << config.rtvCapacity << ", dsv=" << config.dsvCapacity << std::endl;
    return true;
}

void D3D12DescriptorHeapManager::Shutdown() {
    // R22-S1：关机统计（对无 BeginFrame 的宿主——M 系列测试 harness 的
    // 实测形态，帧槽静止、bump 单调——本行是唯一的全周期高水位总账）。
    if (device_) LogSamplerSharingStats("shutdown");
    device_ = nullptr;
    cbvSrvUavHeap_.Reset();
    samplerHeap_.Reset();
    rtvHeap_.Reset();
    dsvHeap_.Reset();
    cbvSrvUavShadowHeap_.Reset();
    samplerShadowHeap_.Reset();
    perSlotGuardTarget_.clear();
}

// ============================================================================
// 帧槽线性分配（fence 完成值复用判定——F0-11 契约 §3.4；bump/epoch 数学在
// DescriptorFrameSlotPolicy，R21-C）
// ============================================================================

namespace {
/// R21-C 帧级埋点开关（默认关；PRIMAL_D3D12_FRAME_TRACE=1 开——逐帧输出
/// 堆 high-water/纪元/护拦，黑帧帧号与堆事件对齐用，日志走 stderr）。
bool FrameTraceEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("PRIMAL_D3D12_FRAME_TRACE");
        return v && v[0] == '1';
    }();
    return enabled;
}
} // namespace

void D3D12DescriptorHeapManager::BeginFrame(u32 frameSlot, D3D12FencePool& fence) {
    currentSlot_ = frameSlot % frameSlotCount_;
    // R21-C：策略类推进（bump 归零 + 槽纪元自增——旧纪元的 set 区段全部
    // 作废，RegionIsCurrent 按 (slot, epoch) 双键判定，见策略头注）。
    mainLinear_.BeginFrame(frameSlot);
    samplerLinear_.BeginFrame(frameSlot);
    // 复用判定：该槽上一轮分配的护拦目标值是否已被 GPU 完成。
    // RenderSystem::BeginFrame 已等过本槽 fence（正常路径立即通过）；
    // 未完成（旁路提交顺序）则 CPU 等待——绝不覆写在飞描述符。
    const u64 guard = perSlotGuardTarget_[currentSlot_];
    if (guard != 0 && fence.GetCompletedValue() < guard) {
        PRIMAL_LOG_STREAM(Warn, "RHI")
            << "[D3D12DescriptorHeaps] slot " << currentSlot_
            << " descriptor region still in flight (fence target " << guard
            << ") — CPU waiting before reuse" << std::endl;
        fence.Wait(guard, 10000);
    }
    if (FrameTraceEnabled()) {
        PRIMAL_LOG_STREAM(Info, "RHI.D3D12")
            << "[D3D12FrameTrace] BeginFrame: slot=" << currentSlot_
            << " epoch=" << mainLinear_.SlotEpoch(currentSlot_)
            << " mainLastCycle=" << mainLinear_.LastCycleHighFor(currentSlot_)
            << "/" << mainLinear_.SlotCapacity()
            << " mainAllTimeHW=" << mainLinear_.HighWaterFor(currentSlot_)
            << " samplerLastCycle=" << samplerLinear_.LastCycleHighFor(currentSlot_)
            << "/" << samplerLinear_.SlotCapacity()
            << " guard=" << guard
            << " fenceCompleted=" << fence.GetCompletedValue() << std::endl;
    }
}

bool D3D12DescriptorHeapManager::AllocShaderVisible(u32 count, u32& outBaseSlot) {
    if (!mainLinear_.Alloc(count, outBaseSlot)) {
        PRIMAL_LOG_STREAM(Error, "RHI")
            << "[D3D12DescriptorHeaps] frame slot " << currentSlot_
            << " descriptor capacity exhausted (bump=" << mainLinear_.Bump()
            << " need=" << count << " capacity=" << mainLinear_.SlotCapacity()
            << ")" << std::endl;
        return false;  // 显式失败，不静默回绕
    }
    return true;
}

u32 D3D12DescriptorHeapManager::AllocCbvSrvUavPersistent() {
    return cbvSrvUavPersistentSlots_.Alloc();
}

void D3D12DescriptorHeapManager::FreeCbvSrvUavPersistent(u32 slot) {
    cbvSrvUavPersistentSlots_.Free(slot);
}

// ============================================================================
// 句柄换算
// ============================================================================

D3D12_CPU_DESCRIPTOR_HANDLE D3D12DescriptorHeapManager::CbvSrvUavCpuHandle(u32 slot) const {
    D3D12_CPU_DESCRIPTOR_HANDLE h = cbvSrvUavCpuStart_;
    h.ptr += static_cast<SIZE_T>(slot) * cbvSrvUavDescriptorSize_;
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE D3D12DescriptorHeapManager::CbvSrvUavGpuHandle(u32 slot) const {
    D3D12_GPU_DESCRIPTOR_HANDLE h = cbvSrvUavGpuStart_;
    h.ptr += static_cast<UINT64>(slot) * cbvSrvUavDescriptorSize_;
    return h;
}

// ============================================================================
// shadow 堆句柄换算 + 双写拷贝（R13/T1；语义见头文件双堆模型）
// R19-4 断言布防：所有 shadow 写入/拷贝入口 assert 槽位+数量不越容量——
// 十八测 M3/M4/M6 的 D3D12SDKLayers.dll 内部 0xC0000005 若源于越界描述符
// 写入（R18-B 容量变更冲突假说族的残余），Debug 断言先崩在我们这一行，
// 崩溃栈可直接定位（Windows Debug 配置；Release 的 NDEBUG 静默移除）。
// ============================================================================

D3D12_CPU_DESCRIPTOR_HANDLE D3D12DescriptorHeapManager::CbvSrvUavShadowCpuHandle(u32 slot) const {
    assert(slot < cbvSrvUavCapacity_);
    D3D12_CPU_DESCRIPTOR_HANDLE h = cbvSrvUavShadowCpuStart_;
    h.ptr += static_cast<SIZE_T>(slot) * cbvSrvUavDescriptorSize_;
    return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12DescriptorHeapManager::SamplerShadowCpuHandle(u32 slot) const {
    assert(slot < samplerCapacity_);
    D3D12_CPU_DESCRIPTOR_HANDLE h = samplerShadowCpuStart_;
    h.ptr += static_cast<SIZE_T>(slot) * samplerDescriptorSize_;
    return h;
}

void D3D12DescriptorHeapManager::CopyCbvSrvUavShadowToVisible(u32 slot, u32 count) {
    if (!device_ || count == 0) return;
    assert(slot < cbvSrvUavCapacity_);
    assert(count <= cbvSrvUavCapacity_ - slot);
    D3D12_CPU_DESCRIPTOR_HANDLE src = cbvSrvUavShadowCpuStart_;
    src.ptr += static_cast<SIZE_T>(slot) * cbvSrvUavDescriptorSize_;
    // 目的=visible 堆同槽位（write-only 合法方向）；源=shadow（可读）。
    D3D12_CPU_DESCRIPTOR_HANDLE dst = cbvSrvUavCpuStart_;
    dst.ptr += static_cast<SIZE_T>(slot) * cbvSrvUavDescriptorSize_;
    device_->CopyDescriptorsSimple(count, dst, src, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void D3D12DescriptorHeapManager::CopySamplerShadowToVisible(u32 slot, u32 count) {
    if (!device_ || count == 0) return;
    assert(slot < samplerCapacity_);
    assert(count <= samplerCapacity_ - slot);
    D3D12_CPU_DESCRIPTOR_HANDLE src = samplerShadowCpuStart_;
    src.ptr += static_cast<SIZE_T>(slot) * samplerDescriptorSize_;
    D3D12_CPU_DESCRIPTOR_HANDLE dst = samplerCpuStart_;
    dst.ptr += static_cast<SIZE_T>(slot) * samplerDescriptorSize_;
    device_->CopyDescriptorsSimple(count, dst, src, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
}

u32 D3D12DescriptorHeapManager::AllocRtv() { return rtvSlots_.Alloc(); }
void D3D12DescriptorHeapManager::FreeRtv(u32 slot) { rtvSlots_.Free(slot); }
D3D12_CPU_DESCRIPTOR_HANDLE D3D12DescriptorHeapManager::RtvCpuHandle(u32 slot) const {
    D3D12_CPU_DESCRIPTOR_HANDLE h = rtvCpuStart_;
    h.ptr += static_cast<SIZE_T>(slot) * rtvDescriptorSize_;
    return h;
}

u32 D3D12DescriptorHeapManager::AllocDsv() { return dsvSlots_.Alloc(); }
void D3D12DescriptorHeapManager::FreeDsv(u32 slot) { dsvSlots_.Free(slot); }
D3D12_CPU_DESCRIPTOR_HANDLE D3D12DescriptorHeapManager::DsvCpuHandle(u32 slot) const {
    D3D12_CPU_DESCRIPTOR_HANDLE h = dsvCpuStart_;
    h.ptr += static_cast<SIZE_T>(slot) * dsvDescriptorSize_;
    return h;
}

u32 D3D12DescriptorHeapManager::AllocSampler() { return samplerSlots_.Alloc(); }
void D3D12DescriptorHeapManager::FreeSampler(u32 slot) {
    // R22-S1：先 purge 引用该持久槽的共享条目——槽位回收进 free-list 后可被
    // 新 sampler 复用；处方若仍指向它，后续按处方重建会拷到别人的描述符
    // （引用已销毁 sampler 的 set 属 Vulkan 同款 UB：spec 禁止销毁仍被
    // descriptor set 引用的 sampler——purge 让"UB"停留在不可解析，而不是
    // 静默拷错内容）。
    if (slot != kInvalidSlot) PurgeSamplerSharingForPersistentSlot(slot);
    samplerSlots_.Free(slot);
}
D3D12_CPU_DESCRIPTOR_HANDLE D3D12DescriptorHeapManager::SamplerCpuHandle(u32 slot) const {
    D3D12_CPU_DESCRIPTOR_HANDLE h = samplerCpuStart_;
    h.ptr += static_cast<SIZE_T>(slot) * samplerDescriptorSize_;
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE D3D12DescriptorHeapManager::SamplerGpuHandle(u32 slot) const {
    D3D12_GPU_DESCRIPTOR_HANDLE h = samplerGpuStart_;
    h.ptr += static_cast<UINT64>(slot) * samplerDescriptorSize_;
    return h;
}

bool D3D12DescriptorHeapManager::AllocSamplerRange(u32 count, u32& outBaseSlot) {
    if (!samplerLinear_.Alloc(count, outBaseSlot)) {
        PRIMAL_LOG_STREAM(Error, "RHI")
            << "[D3D12DescriptorHeaps] frame slot " << currentSlot_
            << " sampler range capacity exhausted (bump=" << samplerLinear_.Bump()
            << " need=" << count << " capacity=" << samplerLinear_.SlotCapacity()
            << ")" << std::endl;
        return false;
    }
    return true;
}

bool D3D12DescriptorHeapManager::MaterializeSamplerRange(const u32* srcSlots,
                                                         u32 count,
                                                         u32& outBaseSlot) {
    // R22-S1：内容处方的"每内容每槽一次"物化（语义见策略类注）。分配失败
    // 的日志沿用 AllocSamplerRange 的耗尽签名（补 unique 上下文）。
    if (!srcSlots || count == 0) return false;
    if (!samplerLinear_.Alloc(count, outBaseSlot)) {
        PRIMAL_LOG_STREAM(Error, "RHI")
            << "[D3D12DescriptorHeaps] frame slot " << currentSlot_
            << " sampler range capacity exhausted (bump=" << samplerLinear_.Bump()
            << " need=" << count << " capacity=" << samplerLinear_.SlotCapacity()
            << "; unique sampler contents exceed per-slot capacity)" << std::endl;
        return false;
    }
    assert(outBaseSlot < samplerCapacity_);
    assert(count <= samplerCapacity_ - outBaseSlot);
    // 内容物化：逐项双写（R13/T1）——源读 sampler 持久槽的 shadow 副本，先
    // 落区间 shadow 再镜像 visible 同槽。只处理已写项：未写项的 shadow/
    // visible 均保持区间原样（未定义描述符——与"未写入绑定的采样器未定义"
    // 语义一致），且不把未初始化的 shadow 槽当拷贝源（与原逐项写入路径
    // 相同的 debug-layer 暴露面：原路径也只对已写项做 shadow→visible）。
    for (u32 i = 0; i < count; ++i) {
        if (srcSlots[i] == SamplerTableSharingPolicy::kUnwritten) continue;
        assert(srcSlots[i] < samplerPersistentCapacity_);
        device_->CopyDescriptorsSimple(
            1, SamplerShadowCpuHandle(outBaseSlot + i),
            SamplerShadowCpuHandle(srcSlots[i]),
            D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        CopySamplerShadowToVisible(outBaseSlot + i);
    }
    return true;
}

void D3D12DescriptorHeapManager::LogSamplerSharingStats(const char* when) {
    // R22-S1 init 诊断（任务要求保留并补充）：唯一 sampler 内容数、当前槽
    // 活跃共享区间数、每槽 sampler 线性区高水位、主堆每槽高水位——耗尽
    // 类问题的第一手对账数据。逐槽高水位按 frameSlotCount_ 展开。
    std::string samplerHighWater, mainHighWater;
    for (u32 s = 0; s < frameSlotCount_; ++s) {
        if (s != 0) { samplerHighWater += ", "; mainHighWater += ", "; }
        samplerHighWater += std::to_string(samplerLinear_.HighWaterFor(s));
        mainHighWater += std::to_string(mainLinear_.HighWaterFor(s));
    }
    PRIMAL_LOG_STREAM(Info, "RHI")
        << "[D3D12DescriptorHeaps] sampler sharing stats ("
        << (when ? when : "?") << "): uniqueSamplerContents="
        << samplerSharing_.UniqueKeyCount()
        << " activeSharedRanges(slot " << currentSlot_ << ")="
        << samplerSharing_.ActiveEntryCount(currentSlot_,
                                            samplerLinear_.SlotEpoch(currentSlot_))
        << " samplerHighWaterPerSlot=[" << samplerHighWater << "]/"
        << samplerLinear_.SlotCapacity()
        << " mainHighWaterPerSlot=[" << mainHighWater << "]/"
        << mainLinear_.SlotCapacity() << std::endl;
}

// ============================================================================
// D3D12DescriptorSetLayout — 绑定分类与区段排列（契约见 D3D12Pipeline.h）
// ============================================================================

namespace {

/// DescriptorType → (主段类, 进 sampler 段)。
/// CombinedImageSampler：图像进 SRV 段 + 采样器进 sampler 段（D3D12 分离）。
void ClassifyBinding(const DescriptorSetLayoutBinding& b,
                     D3D12_DESCRIPTOR_RANGE_TYPE& outMain, bool& outHasMain,
                     bool& outHasSampler) {
    outHasMain = false;
    outHasSampler = false;
    switch (b.descriptorType) {
        case DescriptorType::UniformBuffer:
        case DescriptorType::UniformBufferDynamic:
            outMain = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
            outHasMain = true;
            return;
        case DescriptorType::SampledImage:
        case DescriptorType::SampledDepthImage:
        case DescriptorType::InputAttachment:
        case DescriptorType::UniformTexelBuffer:
            outMain = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            outHasMain = true;
            return;
        case DescriptorType::CombinedImageSampler:
            outMain = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            outHasMain = true;
            outHasSampler = true;
            return;
        case DescriptorType::Sampler:
            outHasSampler = true;
            outMain = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;  // 未用（hasMain=false）
            return;
        case DescriptorType::StorageImage:
        case DescriptorType::StorageTexelBuffer:
        case DescriptorType::StorageBuffer:
        case DescriptorType::Unknown:
        default:
            // 写类 → UAV；readonly → SRV。
            outMain = b.readonly ? D3D12_DESCRIPTOR_RANGE_TYPE_SRV
                                 : D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
            outHasMain = true;
            return;
    }
}

} // namespace

D3D12DescriptorSetLayout::D3D12DescriptorSetLayout(RHIDeviceBase& device,
                                                   const DescriptorSetLayoutDesc& desc)
    : RHIDescriptorSetLayout(device, desc) {}

bool D3D12DescriptorSetLayout::Initialize() {
    // 段排列：主段（CBV 类 binding 升序 → SRV 类 → UAV 类）→ sampler 段
    // （Sampler/Combined binding 升序）。偏移在段内累计。
    struct Item {
        u32 binding;
        u32 count;
        D3D12_DESCRIPTOR_RANGE_TYPE mainType;
        bool hasMain;
        bool hasSampler;
    };
    utl::vector<Item> items;
    for (const auto& b : bindings_) {
        D3D12_DESCRIPTOR_RANGE_TYPE mainType;
        bool hasMain, hasSampler;
        ClassifyBinding(b, mainType, hasMain, hasSampler);
        if (!hasMain && !hasSampler) continue;  // Unknown 类跳过（防御）
        items.push_back({b.binding, b.descriptorCount ? b.descriptorCount : 1,
                         mainType, hasMain, hasSampler});
    }
    // 主段排序：类优先级（CBV<SRV<UAV）→ binding 升序。
    const auto typeRank = [](D3D12_DESCRIPTOR_RANGE_TYPE t) {
        return t == D3D12_DESCRIPTOR_RANGE_TYPE_CBV ? 0
             : t == D3D12_DESCRIPTOR_RANGE_TYPE_SRV ? 1 : 2;
    };
    std::sort(items.begin(), items.end(), [&](const Item& a, const Item& b) {
        const u32 mainA = a.hasMain ? typeRank(a.mainType) : 3;
        const u32 mainB = b.hasMain ? typeRank(b.mainType) : 3;
        if (mainA != mainB) return mainA < mainB;
        return a.binding < b.binding;
    });

    u32 mainCursor = 0, samplerCursor = 0;
    slotTable_.clear();
    for (const Item& item : items) {
        BindingClass info;
        info.count = item.count;
        if (item.hasMain) {
            info.hasMain = true;
            info.mainType = item.mainType;
            info.mainOffset = mainCursor;
            mainCursor += item.count;
        }
        if (item.hasSampler) {
            info.hasSampler = true;
            info.samplerOffset = samplerCursor;
            samplerCursor += item.count;
        }
        slotTable_.push_back({item.binding, info});
    }
    // slotTable_ 按 (类序, binding) 排序——QueryBinding 二分前再按 binding 排
    // （查询键只有 binding；同类内 binding 已升序，重复 binding 不存在）。
    std::sort(slotTable_.begin(), slotTable_.end(),
              [](const std::pair<u32, BindingClass>& a,
                 const std::pair<u32, BindingClass>& b) {
                  return a.first < b.first;
              });
    totalDescriptors_ = mainCursor;
    samplerCount_ = samplerCursor;
    return true;
}

bool D3D12DescriptorSetLayout::QueryBinding(u32 binding, BindingClass& out) const {
    for (const auto& [b, info] : slotTable_) {
        if (b == binding) {
            out = info;
            return true;
        }
    }
    return false;
}

// ============================================================================
// D3D12DescriptorSet — set 实例（shader-visible 堆连续区段）
// ============================================================================

D3D12DescriptorSet::D3D12DescriptorSet(RHIDeviceBase& device, const DescriptorSetDesc& desc)
    : RHIDescriptorSet(device, desc) {}

bool D3D12DescriptorSet::Initialize() {
    D3D12Device& dev = static_cast<D3D12Device&>(device_);
    D3D12DescriptorHeapManager& heaps = dev.GetDescriptorHeaps();
    D3D12DescriptorSetLayout* layout = dev.GetDescriptorSetLayout(layoutHandle_);
    if (!layout) return false;

    mainCount_ = layout->TotalDescriptorCount();
    samplerCount_ = layout->SamplerCount();
    if (mainCount_ > 0) {
        if (!heaps.AllocShaderVisible(mainCount_, baseSlot_)) {
            return false;
        }
    }
    if (samplerCount_ > 0) {
        // R22-S1：sampler 区段懒物化——创建时尚无写入内容，这里只建内容
        // 处方（全"未写"）；区间在首个 sampler 写入时按内容解析（共享/
        // 物化，D3D12Device::ResolveSamplerRegionForWrite）。原实现创建期
        // 即 AllocSamplerRange(samplerCount_)：init 期所有 set 落 slot 0，
        // 393-mesh 声明 Sponza 的 ~170 组 material set(1)/lit set(2) 加上
        // init 预置 set 共 512 个 sampler 描述符恰好打满每槽 512 → 第
        // 171 组 material set 创建失败（b115034 实测 "bump=512 need=1"）。
        // 内容共享后 init 消耗 = O(唯一 sampler 内容数)。
        samplerSrcSlots_.assign(samplerCount_, SamplerTableSharingPolicy::kUnwritten);
        samplerRegionShared_ = false;
    }
    // R21-C：分配戳 = (帧槽, 纪元) 双键——EnsureSetRegionCurrent 的时效
    // 判定依据（只比槽号的旧判定会放行隔周期回归的陈旧区段）。
    ownerSlot_ = dev.GetCurrentFrameIndex();
    ownerEpoch_ = heaps.SlotEpoch(ownerSlot_);
    return true;
}

void D3D12DescriptorSet::destroyImpl() {
    // 主堆区段与 sampler 线性区段（私有或共享）均随帧槽轮转整体回收——
    // 无逐槽释放动作（持久 sampler 对象槽位由 D3D12Sampler 自管；共享
    // 条目按内容存续，不随单个 set 析构撤除——R22-S1）。
}

// ============================================================================
// D3D12Sampler
// ============================================================================

bool D3D12Sampler::Initialize(ID3D12Device* device, D3D12DescriptorHeapManager& heaps,
                              const SamplerDesc& desc) {
    if (!device) return false;
    slot_ = heaps.AllocSampler();
    if (slot_ == D3D12DescriptorHeapManager::kInvalidSlot) return false;

    D3D12_SAMPLER_DESC s{};
    // F2-3（M2）：过滤器与比较函数走平台中立映射（D3D12StateMapping，
    // macOS 可单测）。比较谓词 = comparisonFunc != Never，与 VulkanSampler
    // 的 compareEnable 谓词逐字对齐（M1 版 "!= Always && != Unknown" 谓词
    // 在 Never/Always 两值上与 Vulkan 口径分歧，M2 修正——详见映射头注）。
    s.Filter = static_cast<D3D12_FILTER>(SelectD3D12SamplerFilter(desc));
    const auto address = [](TextureAddressMode m) -> D3D12_TEXTURE_ADDRESS_MODE {
        switch (m) {
            case TextureAddressMode::Wrap:       return static_cast<D3D12_TEXTURE_ADDRESS_MODE>(kD3D12TextureAddressMode_Wrap);
            case TextureAddressMode::Mirror:     return static_cast<D3D12_TEXTURE_ADDRESS_MODE>(kD3D12TextureAddressMode_Mirror);
            case TextureAddressMode::Clamp:      return static_cast<D3D12_TEXTURE_ADDRESS_MODE>(kD3D12TextureAddressMode_Clamp);
            case TextureAddressMode::Border:     return static_cast<D3D12_TEXTURE_ADDRESS_MODE>(kD3D12TextureAddressMode_Border);
            case TextureAddressMode::MirrorOnce: return static_cast<D3D12_TEXTURE_ADDRESS_MODE>(kD3D12TextureAddressMode_MirrorOnce);
            default:                             return static_cast<D3D12_TEXTURE_ADDRESS_MODE>(kD3D12TextureAddressMode_Wrap);
        }
    };
    s.AddressU = address(desc.addressU);
    s.AddressV = address(desc.addressV);
    s.AddressW = address(desc.addressW);
    s.MipLODBias = desc.mipLodBias;
    s.MaxAnisotropy = desc.maxAnisotropy > 1 ? desc.maxAnisotropy : 1;
    s.ComparisonFunc = static_cast<D3D12_COMPARISON_FUNC>(
        ToD3D12ComparisonFunc(desc.comparisonFunc));
    s.BorderColor[0] = desc.borderColor.x;
    s.BorderColor[1] = desc.borderColor.y;
    s.BorderColor[2] = desc.borderColor.z;
    s.BorderColor[3] = desc.borderColor.w;
    s.MinLOD = desc.minLod;
    s.MaxLOD = desc.maxLod;

    // R13/T1 双写：先写 shadow（持久槽位同时是未来 set 写入的拷贝源），
    // 再 shadow→visible（原实现直写 visible 堆，作源即 #654）。
    device->CreateSampler(&s, heaps.SamplerShadowCpuHandle(slot_));
    heaps.CopySamplerShadowToVisible(slot_);
    return true;
}

void D3D12Sampler::Destroy(D3D12DescriptorHeapManager& heaps) {
    if (slot_ != D3D12DescriptorHeapManager::kInvalidSlot) {
        heaps.FreeSampler(slot_);
        slot_ = D3D12DescriptorHeapManager::kInvalidSlot;
    }
}

} // namespace primal::graphics::rhi::d3d12

#endif // defined(_WIN32) && defined(ENABLE_D3D12) && ENABLE_D3D12
