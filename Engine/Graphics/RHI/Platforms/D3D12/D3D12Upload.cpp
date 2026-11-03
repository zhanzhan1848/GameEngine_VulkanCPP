// Windows-only: requires ENABLE_D3D12 (_WIN32)
//
// D3D12Upload.cpp — 帧槽 UPLOAD staging 环实现（M1 F1-2）。
#include "D3D12Upload.h"

#if defined(_WIN32) && defined(ENABLE_D3D12) && ENABLE_D3D12

#include "D3D12Device.h"
#include "D3D12Fence.h"
#include "D3D12StateTracker.h"

#include "Engine/Common/CoreDiag.h"

#ifndef PRIMAL_LOG_STREAM
#define PRIMAL_LOG_STREAM(level, channel) primal::diag::StreamProxy(level, channel)
#endif

namespace primal::graphics::rhi::d3d12 {

D3D12UploadRing::~D3D12UploadRing() { Shutdown(); }

// ============================================================================
// upload heap 区域布局（Footprint / RowPitch 计算）
// ============================================================================

D3D12UploadRing::TextureUploadLayout D3D12UploadRing::ComputeRegionLayout(
        u32 dxgiFormat, u32 bytesPerPixel, u32 width, u32 height, u32 depth,
        u64 baseOffset) {
    TextureUploadLayout out{};
    if (width == 0 || height == 0 || depth == 0 || bytesPerPixel == 0) return out;
    const u64 rowBytes = u64(width) * bytesPerPixel;
    const u64 rowPitch = (rowBytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)
                         & ~u64(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);  // 256 对齐
    out.footprint.Offset = baseOffset;
    out.footprint.Footprint.Format = static_cast<DXGI_FORMAT>(dxgiFormat);
    out.footprint.Footprint.Width = width;
    out.footprint.Footprint.Height = height;
    out.footprint.Footprint.Depth = depth;
    out.footprint.Footprint.RowPitch = static_cast<UINT>(rowPitch);
    // 末行不 pad；每层末行后的 padding 属下一层首行（标准 footprint 尺寸公式）。
    const u64 lastRowSize = rowBytes;
    out.totalBytes = depth > 0
        ? u64(depth - 1) * rowPitch * height + (height - 1) * rowPitch + lastRowSize
        : 0;
    out.rowPitch = rowPitch;
    return out;
}

// ============================================================================
// 生命周期
// ============================================================================

void D3D12UploadRing::Initialize(D3D12Device* device, u64 poolSize) {
    device_ = device;
    D3D12MA::Allocator* allocator = device->GetD3D12MAAllocator();
    ID3D12Device* native = device->GetNativeDevice();
    if (!allocator || !native) return;

    for (u32 i = 0; i < FRAME_SLOTS; ++i) {
        FramePool& pool = pools_[i];
        pool.capacity = poolSize;

        D3D12MA::ALLOCATION_DESC allocDesc{};
        allocDesc.HeapType = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC resDesc{};
        resDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        resDesc.Alignment = 0;
        resDesc.Width = poolSize;
        resDesc.Height = 1;
        resDesc.DepthOrArraySize = 1;
        resDesc.MipLevels = 1;
        resDesc.Format = DXGI_FORMAT_UNKNOWN;
        resDesc.SampleDesc.Count = 1;
        resDesc.SampleDesc.Quality = 0;
        resDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        resDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        // UPLOAD heap buffer 规定初始态 GENERIC_READ；该状态含 COPY_SOURCE 位，
        // 因此 staging 源永远无需 barrier。
        // 注意 D3D12MA v3.0.1 参数序：ppAllocation 在 riidResource/ppvResource 之前。
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        HRESULT hr = allocator->CreateResource(
            &allocDesc, &resDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, &pool.allocation, IID_PPV_ARGS(&resource));
        if (SUCCEEDED(hr)) pool.buffer = resource.Detach();
        if (FAILED(hr) || !pool.buffer) {
            PRIMAL_LOG_STREAM(Error, "RHI")
                << "[D3D12Upload] staging pool " << i << " allocation failed hr=0x"
                << std::hex << static_cast<unsigned long>(hr) << std::dec << std::endl;
            pool.buffer = nullptr;
            pool.allocation = nullptr;
            continue;
        }
        // 持久映射（UPLOAD heap 允许，且 M1 上传路径全程 CPU 写 → GPU 拷贝）。
        if (FAILED(pool.buffer->Map(0, nullptr, &pool.mappedPtr))) {
            PRIMAL_LOG_STREAM(Error, "RHI")
                << "[D3D12Upload] staging pool " << i << " Map failed" << std::endl;
            pool.buffer->Release();
            pool.buffer = nullptr;
            pool.allocation->Release();
            pool.allocation = nullptr;
            continue;
        }
        pool.offset = 0;
        pool.lastUseTarget = 0;
        pool.pending.clear();
    }
    currentSlot_ = 0;
}

void D3D12UploadRing::Shutdown() {
    for (u32 i = 0; i < FRAME_SLOTS; ++i) {
        FramePool& pool = pools_[i];
        if (pool.buffer) pool.buffer->Unmap(0, nullptr);
        // D3D12MA CreateResource 的所有权契约：ppvResource 与 Allocation 各持
        // 一份引用，两者都要 Release（顺序：先资源后 allocation）。
        if (pool.buffer) {
            pool.buffer->Release();
            pool.buffer = nullptr;
        }
        if (pool.allocation) {
            pool.allocation->Release();
            pool.allocation = nullptr;
        }
        pool.mappedPtr = nullptr;
        pool.offset = 0;
        pool.capacity = 0;
        pool.lastUseTarget = 0;
        pool.pending.clear();
    }
    device_ = nullptr;
}

// ============================================================================
// 帧槽轮转（fence 完成值复用判定）
// ============================================================================

void D3D12UploadRing::BeginFrame() {
    if (!device_) return;
    currentSlot_ = (currentSlot_ + 1) % FRAME_SLOTS;
    FramePool& pool = pools_[currentSlot_];
    if (!pool.buffer) return;

    // 复用判定：该槽上次消费提交的 fence 目标值已完成才归零 bump。
    // 正常路径 RenderSystem::BeginFrame 已等过帧槽 fence（立即通过）；
    // 未完成则 CPU 等待——绝不覆写可能在飞的 staging 数据。
    const u64 guard = pool.lastUseTarget;
    if (guard != 0) {
        D3D12FencePool& fence = device_->GetFencePool();
        if (fence.GetCompletedValue() < guard) {
            PRIMAL_LOG_STREAM(Warn, "RHI")
                << "[D3D12Upload] slot " << currentSlot_
                << " staging still in flight (fence target " << guard
                << ") — CPU waiting before reuse" << std::endl;
            fence.Wait(guard, 10000);
        }
    }
    pool.offset = 0;
}

D3D12UploadRing::Allocation D3D12UploadRing::Allocate(u64 size, u64 alignment) {
    Allocation out{};
    FramePool& pool = pools_[currentSlot_];
    if (!pool.buffer || size == 0) {
        out.overflow = true;
        return out;
    }
    const u64 align = alignment > 1 ? alignment : 1;
    u64 bump = (pool.offset + align - 1) & ~(align - 1);
    if (bump + size > pool.capacity) {
        out.overflow = true;  // 调用方约定：FlushBlocking 后重试一次
        return out;
    }
    pool.offset = bump + size;
    out.cpuPtr = static_cast<u8*>(pool.mappedPtr) + bump;
    out.stagingResource = pool.buffer;
    out.offset = bump;
    out.overflow = false;
    return out;
}

// ============================================================================
// 挂起拷贝队列
// ============================================================================

void D3D12UploadRing::QueueCopyBuffer(const Allocation& alloc, ID3D12Resource* dst,
                                      u64 dstOffset, u64 size) {
    if (alloc.overflow || !alloc.stagingResource || !dst || size == 0) return;
    FramePool& pool = pools_[currentSlot_];
    PendingCopy copy{};
    copy.srcStaging = alloc.stagingResource;
    copy.srcOffset = alloc.offset;
    copy.isTexture = false;
    copy.dstBuffer = dst;
    copy.dstBufferOffset = dstOffset;
    copy.size = size;
    pool.pending.push_back(copy);
}

void D3D12UploadRing::QueueCopyTexture(const Allocation& alloc,
                                       ID3D12Resource* dstTexture, u64 textureId,
                                       u32 subresource,
                                       const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& layout,
                                       u32 offsetX, u32 offsetY, u32 offsetZ,
                                       u32 width, u32 height, u32 depth,
                                       ResourceState backState) {
    if (alloc.overflow || !alloc.stagingResource || !dstTexture) return;
    FramePool& pool = pools_[currentSlot_];

    PendingCopy copy{};
    copy.srcStaging = alloc.stagingResource;
    copy.srcOffset = alloc.offset;
    copy.isTexture = true;
    copy.dstTexture = dstTexture;
    copy.textureId = textureId;
    copy.footprint = layout;
    copy.footprint.Offset = alloc.offset;  // 调用方预算时已带；此处确保一致
    copy.subresource = subresource;
    copy.copyX = offsetX;
    copy.copyY = offsetY;
    copy.copyZ = offsetZ;
    copy.copyW = width;
    copy.copyH = height;
    copy.copyD = depth;
    copy.backState = backState;
    pool.pending.push_back(copy);
}

// ============================================================================
// 编码（StateTracker 集成）
// ============================================================================

void D3D12UploadRing::EncodePoolCopies(ID3D12GraphicsCommandList* cmd, FramePool& pool) {
    if (!device_ || !cmd || pool.pending.empty()) return;

    for (const PendingCopy& copy : pool.pending) {
        if (copy.isTexture) {
            // 纹理目标：显式 transition → COPY_DEST（写类目标，promotion 不覆盖
            // 纹理）→ CopyTextureRegion（upload heap placed footprint 源）→
            // transition → backState。全部经 M0 StateTracker 产出 op 列表，
            // 同一批 op 一次 ResourceBarrier 下发（与 CommandBuffer 层同构）。
            D3D12SubresourceKey key{};
            key.resource = copy.textureId;
            key.subresource = copy.subresource;
            key.isTexture = true;

            D3D12Device& dev = *device_;
            utl::vector<D3D12ResourceBarrierOp> ops =
                dev.GetStateTracker().Transition(key, ResourceState::CopyDest);
            for (const D3D12ResourceBarrierOp& op : ops) {
                if (op.kind != D3D12ResourceBarrierOp::Kind::Transition) continue;
                D3D12_RESOURCE_BARRIER barrier{};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                barrier.Transition.pResource = copy.dstTexture;
                barrier.Transition.StateBefore = static_cast<D3D12_RESOURCE_STATES>(op.stateBefore);
                barrier.Transition.StateAfter = static_cast<D3D12_RESOURCE_STATES>(op.stateAfter);
                barrier.Transition.Subresource = op.subresource;
                cmd->ResourceBarrier(1, &barrier);
            }

            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = copy.srcStaging;
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint = copy.footprint;

            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = copy.dstTexture;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = copy.subresource;

            D3D12_BOX box{};
            box.left = copy.copyX;
            box.top = copy.copyY;
            box.front = copy.copyZ;
            box.right = copy.copyX + copy.copyW;
            box.bottom = copy.copyY + copy.copyH;
            box.back = copy.copyZ + copy.copyD;
            cmd->CopyTextureRegion(&dst, copy.copyX, copy.copyY, copy.copyZ, &src, &box);

            ops = dev.GetStateTracker().Transition(key, copy.backState);
            for (const D3D12ResourceBarrierOp& op : ops) {
                if (op.kind != D3D12ResourceBarrierOp::Kind::Transition) continue;
                D3D12_RESOURCE_BARRIER barrier{};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                barrier.Transition.pResource = copy.dstTexture;
                barrier.Transition.StateBefore = static_cast<D3D12_RESOURCE_STATES>(op.stateBefore);
                barrier.Transition.StateAfter = static_cast<D3D12_RESOURCE_STATES>(op.stateAfter);
                barrier.Transition.Subresource = op.subresource;
                cmd->ResourceBarrier(1, &barrier);
            }
        } else {
            // buffer 目标：无 barrier——源 UPLOAD heap 恒 GENERIC_READ（含
            // COPY_SOURCE 位），目标 buffer 从 COMMON 提升到 COPY_DEST
            // （buffer 的 COMMON→任意状态提升规则），提交边界自动衰减回 COMMON。
            cmd->CopyBufferRegion(copy.dstBuffer, copy.dstBufferOffset,
                                  copy.srcStaging, copy.srcOffset, copy.size);
        }
    }
    pool.pending.clear();
}

void D3D12UploadRing::EncodePendingCopies(ID3D12GraphicsCommandList* cmd) {
    if (!device_ || !cmd) return;
    EncodePoolCopies(cmd, pools_[currentSlot_]);
}

// ============================================================================
// 阻塞 flush（资产加载期；无帧上下文）
// ============================================================================

void D3D12UploadRing::FlushBlocking() {
    if (!device_) return;
    FramePool& pool = pools_[currentSlot_];
    if (pool.pending.empty()) return;

    using Microsoft::WRL::ComPtr;
    ID3D12Device* native = device_->GetNativeDevice();
    ID3D12CommandQueue* queue = device_->GetGraphicsQueue();
    if (!native || !queue || !pool.buffer) return;

    ComPtr<ID3D12CommandAllocator> allocator;
    if (FAILED(native->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              IID_PPV_ARGS(&allocator)))) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[D3D12Upload] FlushBlocking allocator failed" << std::endl;
        return;
    }
    ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(native->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                         allocator.Get(), nullptr, IID_PPV_ARGS(&list)))) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[D3D12Upload] FlushBlocking list failed" << std::endl;
        return;
    }
    EncodePoolCopies(list.Get(), pool);
    list->Close();

    // 提交 + fence 目标值登记（复用判定依据）+ CPU 等待。
    D3D12FencePool& fence = device_->GetFencePool();
    const u64 target = fence.NextTarget();
    ID3D12CommandList* lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    if (!fence.Signal(queue, target)) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[D3D12Upload] FlushBlocking signal failed" << std::endl;
        return;
    }
    pool.lastUseTarget = target;
    fence.Wait(target, 10000);
}

} // namespace primal::graphics::rhi::d3d12

#endif // defined(_WIN32) && defined(ENABLE_D3D12) && ENABLE_D3D12
