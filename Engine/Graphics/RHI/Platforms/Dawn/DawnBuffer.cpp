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

    // Always allow copy operations
    usage |= WGPUBufferUsage_CopyDst;
    usage |= WGPUBufferUsage_CopySrc;

    // WebGPU restriction: MapWrite can only be combined with CopyDst,
    // MapRead can only be combined with CopySrc.
    // GPU buffer types (Vertex, Index, Uniform, Storage, Indirect) are NOT
    // compatible with MapWrite/MapRead. For those, we use a CPU-side staging
    // buffer approach instead.
    bool gpuUsage = (bufferDesc_.type == BufferType::Vertex ||
                     bufferDesc_.type == BufferType::Index ||
                     bufferDesc_.type == BufferType::Constant ||
                     bufferDesc_.type == BufferType::Structured ||
                     bufferDesc_.type == BufferType::Raw ||
                     bufferDesc_.type == BufferType::Indirect ||
                     bufferDesc_.type == BufferType::AccelerationStructure);

    canDirectMap_ = false;

    if (needsMapWrite_ && !gpuUsage) {
        usage |= WGPUBufferUsage_MapWrite;
        canDirectMap_ = true;
    }

    if (needsMapRead_ && !gpuUsage) {
        usage |= WGPUBufferUsage_MapRead;
        canDirectMap_ = true;
    }

    // --- Create the buffer descriptor ---

    WGPUBufferDescriptor wgpuDesc{};
    wgpuDesc.nextInChain = nullptr;
    wgpuDesc.label = ToWGPUStringView(bufferDesc_.name.empty() ? "" : bufferDesc_.name.c_str());
    wgpuDesc.usage = usage;
    wgpuDesc.size = (bufferDesc_.size + 3) & ~3ull;  // WGPU requires 4-byte alignment
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

        wgpuBufferMapAsync(wgpuBuffer_, mapMode, offset, size, callbackInfo);

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

        return wgpuBufferGetMappedRange(wgpuBuffer_, offset, size);
    }

    // GPU buffer (Vertex/Index/Uniform/Storage) — use CPU staging.
    // Caller writes to the staging memory, then unmapImpl uploads via
    // wgpuQueueWriteBuffer.
    if (stagingData_) {
        free(stagingData_);
        stagingData_ = nullptr;
    }

    stagingData_ = malloc(size);
    if (!stagingData_) {
        std::cerr << "[DawnBuffer] Failed to allocate staging memory" << std::endl;
        return nullptr;
    }
    memset(stagingData_, 0, size);

    mappedOffset_ = offset;
    mappedSize_ = size;
    dirtySize_ = 0;
    return stagingData_;
}

// ============================================================
// unmapImpl
// ============================================================

void DawnBuffer::unmapImpl() {
    if (stagingData_) {
        // Upload CPU staging data to GPU buffer
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
    u64 uploadSize = dirtySize_ > mappedSize_ ? mappedSize_ : dirtySize_;
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
