// Windows-only: requires ENABLE_D3D12 (_WIN32)
//
// D3D12Buffer.cpp — buffer 资源实现（M1 F1-3）。
#include "D3D12Buffer.h"

#if defined(_WIN32) && defined(ENABLE_D3D12) && ENABLE_D3D12

#include <cstring>

#include "D3D12Device.h"
#include "D3D12Upload.h"

#include "Engine/Common/CoreDiag.h"

#ifndef PRIMAL_LOG_STREAM
#define PRIMAL_LOG_STREAM(level, channel) primal::diag::StreamProxy(level, channel)
#endif

namespace primal::graphics::rhi::d3d12 {

// ============================================================================
// 构造 / 生命周期
// ============================================================================

D3D12Buffer::D3D12Buffer(D3D12Device& device, const BufferDesc& desc)
    : RHIResource(device, ResourceDesc(ResourceType::Buffer, ResourceUsage::None,
                                       desc.memoryUsage != GPUMemoryUsage::Unknown
                                           ? desc.memoryUsage : desc.usage,
                                       desc.size, desc.name.c_str())),
      bufferDesc_(desc) {
    // GPUMemoryUsage → heap type（memoryUsage 为兼容字段，Unknown 时回退 usage）。
    const GPUMemoryUsage mem =
        desc.memoryUsage != GPUMemoryUsage::Unknown ? desc.memoryUsage : desc.usage;
    switch (mem) {
        case GPUMemoryUsage::Staging:
        case GPUMemoryUsage::Dynamic:
            heapType_ = D3D12_HEAP_TYPE_UPLOAD;
            break;
        case GPUMemoryUsage::Readback:
            heapType_ = D3D12_HEAP_TYPE_READBACK;
            break;
        case GPUMemoryUsage::Static:
        case GPUMemoryUsage::Immutable:
        case GPUMemoryUsage::Unknown:
        case GPUMemoryUsage::SwapChain:
        default:
            heapType_ = D3D12_HEAP_TYPE_DEFAULT;
            break;
    }
}

D3D12Buffer::D3D12Buffer(D3D12Buffer&& other) noexcept
    : RHIResource(std::move(other)),
      resource_(std::move(other.resource_)),
      allocation_(other.allocation_),
      mappedPtr_(other.mappedPtr_),
      heapType_(other.heapType_),
      warnedMapUnsupported_(other.warnedMapUnsupported_),
      bufferDesc_(std::move(other.bufferDesc_)) {
    other.allocation_ = nullptr;
    other.mappedPtr_ = nullptr;
}

D3D12Buffer& D3D12Buffer::operator=(D3D12Buffer&& other) noexcept {
    if (this != &other) {
        RHIResource::operator=(std::move(other));
        resource_ = std::move(other.resource_);
        allocation_ = other.allocation_;
        mappedPtr_ = other.mappedPtr_;
        heapType_ = other.heapType_;
        warnedMapUnsupported_ = other.warnedMapUnsupported_;
        bufferDesc_ = std::move(other.bufferDesc_);
        other.allocation_ = nullptr;
        other.mappedPtr_ = nullptr;
    }
    return *this;
}

D3D12Buffer::~D3D12Buffer() { destroyImpl(); }

bool D3D12Buffer::Initialize() {
    if (bufferDesc_.size == 0) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[D3D12Buffer] Initialize failed: size is 0" << std::endl;
        return false;
    }
    D3D12Device& dev = static_cast<D3D12Device&>(device_);
    D3D12MA::Allocator* allocator = dev.GetD3D12MAAllocator();
    if (!allocator) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[D3D12Buffer] D3D12MA allocator not initialized" << std::endl;
        return false;
    }

    D3D12MA::ALLOCATION_DESC allocDesc{};
    allocDesc.HeapType = heapType_;

    D3D12_RESOURCE_DESC resDesc{};
    resDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resDesc.Alignment = 0;
    // M7 F7-7 静态终审修复：UPLOAD heap buffer 的资源宽度向上取整到 256B。
    // D3D12 CBV 契约：SizeInBytes 必须为 256 倍数且不得超出资源末尾——非 256
    // 倍数的 UBO（如 M4 light UBO 112B）在 updateDescriptorSetsImpl 按
    // round-up 造 CBV 时会越过未 padding 资源边界（debug-layer 必错、GBV
    // 越界读）。Vulkan 无此约束，共享宿主按逻辑尺寸创建；故 padding 只发生在
    // 资源本体，GetBufferDesc().size/写路径边界检查仍按逻辑尺寸。
    // READBACK/DEFAULT 不作 CBV 源，无需 padding。
    resDesc.Width = heapType_ == D3D12_HEAP_TYPE_UPLOAD
        ? ((bufferDesc_.size + 255) & ~u64(255))
        : bufferDesc_.size;
    resDesc.Height = 1;
    resDesc.DepthOrArraySize = 1;
    resDesc.MipLevels = 1;
    resDesc.Format = DXGI_FORMAT_UNKNOWN;
    resDesc.SampleDesc.Count = 1;
    resDesc.SampleDesc.Quality = 0;
    resDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resDesc.Flags = D3D12_RESOURCE_FLAG_NONE;  // buffer 无 RTV/DSV/UAV 限制位

    // 初始状态按 heap type 规定值（见文件头映射表）。
    const D3D12_RESOURCE_STATES initialState =
        heapType_ == D3D12_HEAP_TYPE_UPLOAD   ? D3D12_RESOURCE_STATE_GENERIC_READ
        : heapType_ == D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST
                                                : D3D12_RESOURCE_STATE_COMMON;

    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    HRESULT hr = allocator->CreateResource(&allocDesc, &resDesc, initialState,
                                           nullptr, &allocation_, IID_PPV_ARGS(&resource));
    if (FAILED(hr) || !resource) {
        PRIMAL_LOG_STREAM(Error, "RHI")
            << "[D3D12Buffer] CreateResource failed hr=0x" << std::hex
            << static_cast<unsigned long>(hr) << std::dec << " size=" << bufferDesc_.size
            << " name='" << bufferDesc_.name << "'" << std::endl;
        allocation_ = nullptr;
        return false;
    }
    resource_ = std::move(resource);

    // UPLOAD/READBACK 持久映射 + 显式清零（避免 GPU 在 producer 写前读脏数据，
    // 与 VulkanBuffer 的 persistent-map 行为对齐）。
    if (heapType_ == D3D12_HEAP_TYPE_UPLOAD || heapType_ == D3D12_HEAP_TYPE_READBACK) {
        if (FAILED(resource_->Map(0, nullptr, &mappedPtr_)) || !mappedPtr_) {
            PRIMAL_LOG_STREAM(Error, "RHI") << "[D3D12Buffer] persistent Map failed" << std::endl;
            return false;
        }
        std::memset(mappedPtr_, 0, bufferDesc_.size);
    }

    // Buffer 的 StateTracker 注册在设备侧 createBufferImpl（M5 F5-4：
    // UAV→INDIRECT_ARGUMENT 等 buffer 显式转换需 tracker 记账；COMMON 提
    // 升/提交边界衰减规则同 D3D12Buffer.h 文件头注释）。此处仅置 Ready。
    state_ = ResourceState::Ready;
    return true;
}

void D3D12Buffer::destroyImpl() {
    if (!resource_ && !allocation_) return;
    // GC 延迟释放（与 VulkanBuffer 的 DeferredDestroy vmaDestroyBuffer 同款
    // 护栏——in-flight 提交引用期间不释放）。
    Microsoft::WRL::ComPtr<ID3D12Resource> resource = std::move(resource_);
    D3D12MA::Allocation* allocation = allocation_;
    allocation_ = nullptr;
    mappedPtr_ = nullptr;
    device_.GetGarbageCollector().DeferredDestroy([resource, allocation]() {
        // ComPtr/allocation 各持一份引用（CreateResource 契约），均需释放。
        if (allocation) allocation->Release();
    });
}

// ============================================================================
// 映射 / 更新
// ============================================================================

void* D3D12Buffer::mapImpl(u64 offset, u64 /*size*/) {
    if (mappedPtr_) return static_cast<u8*>(mappedPtr_) + offset;
    if (!warnedMapUnsupported_) {
        warnedMapUnsupported_ = true;
        PRIMAL_LOG_STREAM(Warn, "RHI")
            << "[D3D12Buffer] Map on DEFAULT-heap buffer unsupported "
            << "(use UpdateData staging path) name='" << bufferDesc_.name << "'" << std::endl;
    }
    return nullptr;
}

void D3D12Buffer::unmapImpl() {
    // 持久映射：no-op（与 Vulkan VMA persistent-map 语义一致）。
}

bool D3D12Buffer::updateDataImpl(const void* data, u64 size, u64 offset) {
    if (!data || size == 0 || offset + size > bufferDesc_.size) return false;
    if (!resource_) return false;
    D3D12Device& dev = static_cast<D3D12Device&>(device_);

    if (mappedPtr_) {
        // UPLOAD/READBACK：直接写持久映射。
        std::memcpy(static_cast<u8*>(mappedPtr_) + offset, data, size);
        return true;
    }
    // DEFAULT heap：走设备上传环（帧内排队 / 帧外阻塞）。
    return dev.UploadBufferDataViaRing(resource_.Get(), data, size, offset);
}

} // namespace primal::graphics::rhi::d3d12

#endif // defined(_WIN32) && defined(ENABLE_D3D12) && ENABLE_D3D12
