// Windows-only: requires ENABLE_D3D12 (_WIN32)
//
// D3D12DescriptorHeaps.cpp — 描述符堆管理实现（M1 F1-5）。
#include "D3D12DescriptorHeaps.h"

#if defined(_WIN32) && defined(ENABLE_D3D12) && ENABLE_D3D12

#include <algorithm>

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

    device_ = device;
    cbvSrvUavDescriptorSize_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    samplerDescriptorSize_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    rtvDescriptorSize_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    dsvDescriptorSize_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    cbvSrvUavCpuStart_ = cbvSrvUavHeap_->GetCPUDescriptorHandleForHeapStart();
    cbvSrvUavGpuStart_ = cbvSrvUavHeap_->GetGPUDescriptorHandleForHeapStart();
    samplerCpuStart_ = samplerHeap_->GetCPUDescriptorHandleForHeapStart();
    samplerGpuStart_ = samplerHeap_->GetGPUDescriptorHandleForHeapStart();
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
    slotCapacity_ = (config.cbvSrvUavCapacity - cbvSrvUavLinearBase_) / frameSlotCount_;
    currentSlot_ = 0;
    slotBase_ = cbvSrvUavLinearBase_;
    slotBump_ = 0;

    // sampler 堆布局：前段持久 SlotPool（SamplerHandle 对象），后段帧槽线性
    // （set 实例的 sampler 表——descriptor table 需要连续槽位）。
    if (config.samplerPersistentCapacity >= config.samplerCapacity) {
        PRIMAL_LOG_STREAM(Error, "RHI")
            << "[D3D12DescriptorHeaps] samplerPersistentCapacity >= samplerCapacity" << std::endl;
        return false;
    }
    samplerPersistentCapacity_ = config.samplerPersistentCapacity;
    samplerLinearBase_ = config.samplerPersistentCapacity;
    samplerSlotCapacity_ = (config.samplerCapacity - samplerLinearBase_) / frameSlotCount_;
    samplerSlotBase_ = samplerLinearBase_;
    samplerSlotBump_ = 0;

    cbvSrvUavPersistentSlots_.Init(config.cbvSrvUavPersistentCapacity);
    rtvSlots_.Init(config.rtvCapacity);
    dsvSlots_.Init(config.dsvCapacity);
    samplerSlots_.Init(config.samplerPersistentCapacity);
    return true;
}

void D3D12DescriptorHeapManager::Shutdown() {
    device_ = nullptr;
    cbvSrvUavHeap_.Reset();
    samplerHeap_.Reset();
    rtvHeap_.Reset();
    dsvHeap_.Reset();
    perSlotGuardTarget_.clear();
}

// ============================================================================
// 帧槽线性分配（fence 完成值复用判定——F0-11 契约 §3.4）
// ============================================================================

void D3D12DescriptorHeapManager::BeginFrame(u32 frameSlot, D3D12FencePool& fence) {
    currentSlot_ = frameSlot % frameSlotCount_;
    slotBase_ = cbvSrvUavLinearBase_ + currentSlot_ * slotCapacity_;
    slotBump_ = 0;
    samplerSlotBase_ = samplerLinearBase_ + currentSlot_ * samplerSlotCapacity_;
    samplerSlotBump_ = 0;
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
}

bool D3D12DescriptorHeapManager::AllocShaderVisible(u32 count, u32& outBaseSlot) {
    if (slotBump_ + count > slotCapacity_) {
        PRIMAL_LOG_STREAM(Error, "RHI")
            << "[D3D12DescriptorHeaps] frame slot " << currentSlot_
            << " descriptor capacity exhausted (bump=" << slotBump_
            << " need=" << count << " capacity=" << slotCapacity_ << ")" << std::endl;
        return false;  // 显式失败，不静默回绕
    }
    outBaseSlot = slotBase_ + slotBump_;
    slotBump_ += count;
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
void D3D12DescriptorHeapManager::FreeSampler(u32 slot) { samplerSlots_.Free(slot); }
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
    if (samplerSlotBump_ + count > samplerSlotCapacity_) {
        PRIMAL_LOG_STREAM(Error, "RHI")
            << "[D3D12DescriptorHeaps] frame slot " << currentSlot_
            << " sampler range capacity exhausted (bump=" << samplerSlotBump_
            << " need=" << count << " capacity=" << samplerSlotCapacity_ << ")" << std::endl;
        return false;
    }
    outBaseSlot = samplerSlotBase_ + samplerSlotBump_;
    samplerSlotBump_ += count;
    return true;
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
        if (!heaps.AllocSamplerRange(samplerCount_, samplerBaseSlot_)) {
            return false;
        }
    }
    ownerSlot_ = dev.GetCurrentFrameIndex();
    return true;
}

void D3D12DescriptorSet::destroyImpl() {
    // 主堆区段与 sampler 线性区段均随帧槽轮转整体回收——无逐槽释放动作
    // （持久 sampler 对象槽位由 D3D12Sampler 自管）。
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

    device->CreateSampler(&s, heaps.SamplerCpuHandle(slot_));
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
