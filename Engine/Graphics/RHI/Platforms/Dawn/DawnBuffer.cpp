#include "DawnBuffer.h"

#if defined(ENABLE_WEBGPU) && ENABLE_WEBGPU

#include "DawnDevice.h"
#include <cstring>
#include <cstdlib>
#include <iostream>

namespace primal::graphics::rhi {

// ============================================================
// Construction / Destruction
// ============================================================

DawnBuffer::DawnBuffer(DawnDevice& device, const BufferDesc& desc)
    : RHIResource(device, ResourceDesc(ResourceType::Buffer,
                                       ResourceUsage::None,
                                       desc.memoryUsage,
                                       desc.size,
                                       desc.name.empty() ? nullptr : desc.name.c_str())),
      device_(device),
      bufferDesc_(desc) {

    needsMapWrite_ = (desc.memoryUsage == GPUMemoryUsage::Dynamic ||
                      desc.memoryUsage == GPUMemoryUsage::Staging);
    needsMapRead_ = (desc.memoryUsage == GPUMemoryUsage::Readback);
}

DawnBuffer::~DawnBuffer() {
    if (wgpuBuffer_) {
        destroyImpl();
    }
    if (stagingData_) {
        free(stagingData_);
        stagingData_ = nullptr;
    }
}

// ============================================================
// Initialize – create the WGPUBuffer
// ============================================================

bool DawnBuffer::Initialize() {
    if (wgpuBuffer_) {
        return true;
    }

    // Infer BufferType from bindFlags when caller didn't set it. Many engine
    // callers (e.g. GPUDrivenDrawPipeline) set bindFlags directly without
    // touching type; leaving type=Unknown forces MapWrite+CopySrc+CopyDst
    // which WebGPU rejects as incompatible.
    if (bufferDesc_.type == BufferType::Unknown) {
        if ((bufferDesc_.bindFlags & (u32)BufferUsageFlags::Indirect) != 0) {
            bufferDesc_.type = BufferType::Indirect;
        } else if ((bufferDesc_.bindFlags & (u32)BufferUsageFlags::Storage) != 0) {
            bufferDesc_.type = BufferType::Structured;
        } else if ((bufferDesc_.bindFlags & (u32)BufferUsageFlags::Uniform) != 0) {
            bufferDesc_.type = BufferType::Constant;
        } else if ((bufferDesc_.bindFlags & (u32)BufferUsageFlags::Index) != 0) {
            bufferDesc_.type = BufferType::Index;
        } else if ((bufferDesc_.bindFlags & (u32)BufferUsageFlags::Vertex) != 0) {
            bufferDesc_.type = BufferType::Vertex;
        }
    }

    // --- Build WGPUBufferUsage flags ---

    WGPUBufferUsage usage = WGPUBufferUsage_None;

    // Buffer-type-specific usage
    switch (bufferDesc_.type) {
        case BufferType::Vertex:
            usage |= WGPUBufferUsage_Vertex;
            break;
        case BufferType::Index:
            usage |= WGPUBufferUsage_Index;
            break;
        case BufferType::Constant:
            usage |= WGPUBufferUsage_Uniform;
            break;
        case BufferType::Structured:
        case BufferType::Raw:
            usage |= WGPUBufferUsage_Storage;
            break;
        case BufferType::Indirect:
            usage |= WGPUBufferUsage_Indirect;
            break;
        case BufferType::AccelerationStructure:
            usage |= WGPUBufferUsage_Storage;
            break;
        default:
            break;
    }

    // Honor explicit indirect-arg flag regardless of type — some callers
    // (GPUCullingPipeline indirect_args_buffer) declare type=Structured but
    // also pass ResourceUsage::IndirectArg in bindFlags so the buffer can
    // serve as both SSBO (compute writes) and indirect draw source.
    if ((bufferDesc_.bindFlags & (u32)ResourceUsage::IndirectArg) != 0) {
        usage |= WGPUBufferUsage_Indirect;
    }

    // Always allow copy operations EXCEPT when buffer is mappable.
    // WebGPU restriction: MapWrite can only combine with CopySrc, MapRead can
    // only combine with CopyDst. Adding CopyDst to MapWrite (or CopySrc to
    // MapRead) triggers a validation error at buffer creation, which cascades
    // to every frame because the staging buffer becomes Invalid.
    bool gpuUsage = (bufferDesc_.type == BufferType::Vertex ||
                     bufferDesc_.type == BufferType::Index ||
                     bufferDesc_.type == BufferType::Constant ||
                     bufferDesc_.type == BufferType::Structured ||
                     bufferDesc_.type == BufferType::Raw ||
                     bufferDesc_.type == BufferType::Indirect ||
                     bufferDesc_.type == BufferType::AccelerationStructure);

    canDirectMap_ = false;

    if (needsMapWrite_ && !gpuUsage) {
        // Mappable CPU-writable staging: MapWrite + CopySrc (so we can copy
        // from this buffer to GPU).
        usage |= WGPUBufferUsage_MapWrite | WGPUBufferUsage_CopySrc;
        canDirectMap_ = true;
    } else if (needsMapRead_ && !gpuUsage) {
        // Mappable CPU-readable readback: MapRead + CopyDst (so we can copy
        // from GPU to this buffer).
        usage |= WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
        canDirectMap_ = true;
    } else {
        // Plain GPU buffer — both copy directions are valid.
        usage |= WGPUBufferUsage_CopyDst | WGPUBufferUsage_CopySrc;
    }

    // --- Create the buffer descriptor ---

    // WGPU requires 4-byte alignment for buffer sizes.
    u64 alignedSize = (bufferDesc_.size + 3ull) & ~3ull;

    WGPUBufferDescriptor wgpuDesc{};
    wgpuDesc.nextInChain = nullptr;
    wgpuDesc.label = ToWGPUStringView(bufferDesc_.name.empty() ? "" : bufferDesc_.name.c_str());
    wgpuDesc.usage = usage;
    wgpuDesc.size = alignedSize;
    wgpuDesc.mappedAtCreation = false;

    wgpuBuffer_ = wgpuDeviceCreateBuffer(device_.GetNativeDevice(), &wgpuDesc);
    if (!wgpuBuffer_) {
        std::cerr << "[DawnBuffer] Failed to create buffer"
                  << (bufferDesc_.name.empty() ? "" : (" '" + bufferDesc_.name + "'").c_str())
                  << " size=" << bufferDesc_.size << std::endl;
        return false;
    }

    SetState(ResourceState::Ready);
    return true;
}

// ============================================================
// mapImpl
// ============================================================

void* DawnBuffer::mapImpl(u64 offset, u64 size) {
    if (!wgpuBuffer_) return nullptr;

    // MapBuffer(handle) without explicit size defaults to size=0. Use the
    // full buffer size in that case — otherwise malloc(0) returns an
    // impl-defined pointer and the caller writes past the allocation.
    if (size == 0) {
        size = bufferDesc_.size;
    }

    // WebGPU requires MapAsync offset AND size to be multiples of 4. The
    // underlying WGPUBuffer was created with an already-aligned size, so
    // padding the map request up is always safe and never exceeds the
    // allocation.
    u64 alignedSize = (size + 3ull) & ~3ull;
    u64 alignedOffset = (offset + 3ull) & ~3ull;

    if (canDirectMap_) {
        // Buffer has MapWrite/MapRead usage — use async WebGPU mapping
        WGPUMapMode mapMode = needsMapRead_ ? WGPUMapMode_Read : WGPUMapMode_Write;

        struct MapCallbackData {
            bool done{false};
            bool success{false};
        };

        MapCallbackData cbData;

        WGPUBufferMapCallbackInfo callbackInfo{};
        callbackInfo.nextInChain = nullptr;
        callbackInfo.mode = WGPUCallbackMode_AllowProcessEvents;
        callbackInfo.callback = [](WGPUMapAsyncStatus status, WGPUStringView message, void* userdata1, void* userdata2) {
            (void)message;
            (void)userdata2;
            auto* data = static_cast<MapCallbackData*>(userdata1);
            data->success = (status == WGPUMapAsyncStatus_Success);
            data->done = true;
        };
        callbackInfo.userdata1 = &cbData;
        callbackInfo.userdata2 = nullptr;

        wgpuBufferMapAsync(wgpuBuffer_, mapMode, alignedOffset, alignedSize, callbackInfo);

        WGPUInstance instance = device_.GetInstance();
        constexpr int maxPollIterations = 100000;
        int pollCount = 0;
        while (!cbData.done && pollCount < maxPollIterations) {
            wgpuInstanceProcessEvents(instance);
            ++pollCount;
        }

        if (!cbData.done || !cbData.success) {
            return nullptr;
        }

        return wgpuBufferGetMappedRange(wgpuBuffer_, alignedOffset, alignedSize);
    }

    // GPU buffer (Vertex/Index/Uniform/Storage) — use CPU staging.
    // Caller writes to the staging memory, then unmapImpl uploads via
    // wgpuQueueWriteBuffer.
    //
    // wgpuQueueWriteBuffer requires offset AND size to be multiples of 4.
    // Allocate aligned size and memset the full aligned region to zero so
    // the trailing padding bytes (which we still upload) are deterministic.
    if (stagingData_) {
        free(stagingData_);
        stagingData_ = nullptr;
    }

    stagingData_ = malloc(alignedSize);
    if (!stagingData_) {
        std::cerr << "[DawnBuffer] Failed to allocate staging memory" << std::endl;
        return nullptr;
    }
    memset(stagingData_, 0, alignedSize);

    mappedOffset_ = offset;
    mappedSize_ = alignedSize;
    dirtySize_ = 0;
    return stagingData_;
}

// ============================================================
// unmapImpl
// ============================================================

void DawnBuffer::unmapImpl() {
    if (stagingData_) {
        // Upload CPU staging data to GPU buffer.
        // mappedSize_ was already aligned up to a multiple of 4 in mapImpl,
        // so wgpuQueueWriteBuffer won't reject it on alignment grounds.
        WGPUQueue queue = device_.GetQueue();
        if (queue && wgpuBuffer_) {
            wgpuQueueWriteBuffer(queue, wgpuBuffer_, mappedOffset_,
                                 stagingData_, mappedSize_);
        }
        free(stagingData_);
        stagingData_ = nullptr;
    } else if (wgpuBuffer_) {
        wgpuBufferUnmap(wgpuBuffer_);
    }
}

void DawnBuffer::FlushStaging() {
    if (!stagingData_ || !wgpuBuffer_) return;
    WGPUQueue queue = device_.GetQueue();
    if (!queue) return;
    // Align upload size to 4 bytes — wgpuQueueWriteBuffer rejects non-multiple-of-4.
    u64 rawSize = dirtySize_ > mappedSize_ ? mappedSize_ : dirtySize_;
    u64 uploadSize = (rawSize + 3ull) & ~3ull;
    if (uploadSize > mappedSize_) uploadSize = mappedSize_;  // never exceed staging
    wgpuQueueWriteBuffer(queue, wgpuBuffer_, mappedOffset_,
                         stagingData_, uploadSize);
    dirtySize_ = 0;
}

// ============================================================
// updateDataImpl – upload via wgpuQueueWriteBuffer
// ============================================================

bool DawnBuffer::updateDataImpl(const void* data, u64 size, u64 offset) {
    if (!wgpuBuffer_ || !data || size == 0) return false;

    WGPUQueue queue = device_.GetQueue();
    if (!queue) return false;

    // wgpuQueueWriteBuffer requires size to be a multiple of 4.
    // Pad to 4-byte alignment using a temporary buffer if needed.
    u64 alignedSize = (size + 3) & ~3ull;
    if (alignedSize != size) {
        std::vector<u8> padded(alignedSize, 0);
        memcpy(padded.data(), data, size);
        wgpuQueueWriteBuffer(queue, wgpuBuffer_, offset, padded.data(), alignedSize);
    } else {
        wgpuQueueWriteBuffer(queue, wgpuBuffer_, offset, data, size);
    }
    return true;
}

// ============================================================
// destroyImpl – release the WGPUBuffer
// ============================================================

void DawnBuffer::destroyImpl() {
    if (stagingData_) {
        free(stagingData_);
        stagingData_ = nullptr;
    }
    if (wgpuBuffer_) {
        wgpuBufferRelease(wgpuBuffer_);
        wgpuBuffer_ = nullptr;
    }
}

} // namespace primal::graphics::rhi

#endif // ENABLE_WEBGPU
