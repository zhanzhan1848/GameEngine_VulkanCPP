#pragma once

#include "DawnCommon.h"

#if defined(ENABLE_WEBGPU) && ENABLE_WEBGPU

#include "../../Core/RHIResource.h"

namespace primal::graphics::rhi {

class DawnDevice;

/**
 * @brief Dawn WebGPU buffer resource implementation
 * @details Wraps WGPUBuffer with RHIResource lifecycle management.
 *          Supports vertex, index, constant, structured, raw, and indirect buffers
 *          with static, dynamic, staging, and readback memory usage patterns.
 */
class DawnBuffer : public RHIResource {
    friend class DawnDevice;

public:
    ~DawnBuffer() override;

    /**
     * @brief Get the native WGPUBuffer handle
     */
    WGPUBuffer GetNativeBuffer() const { return wgpuBuffer_; }

    /**
     * @brief Get the buffer descriptor
     */
    const BufferDesc& GetBufferDesc() const { return bufferDesc_; }

    /**
     * @brief Flush staging data to GPU without unmapping.
     * For persistently mapped GPU buffers (constant, vertex, etc.),
     * uploads the CPU staging data via wgpuQueueWriteBuffer.
     */
    void FlushStaging();

    /// Whether this buffer has an active staging allocation (persistently mapped).
    bool HasStaging() const { return stagingData_ != nullptr; }

    /**
     * @brief Set the dirty size for the next flush.
     * Call after writing to the mapped pointer to limit FlushStaging upload.
     */
    void SetDirtySize(u64 size) { dirtySize_ = size; }

protected:
    bool Initialize() override;
    void destroyImpl() override;
    void* mapImpl(u64 offset, u64 size) override;
    void unmapImpl() override;
    bool updateDataImpl(const void* data, u64 size, u64 offset) override;

public:
    /**
     * @brief Construct a DawnBuffer
     * @param device  Reference to the owning DawnDevice
     * @param desc    Buffer descriptor (type, size, memory usage, etc.)
     */
    DawnBuffer(DawnDevice& device, const BufferDesc& desc);

private:
    DawnDevice& device_;
    WGPUBuffer wgpuBuffer_ = nullptr;
    BufferDesc bufferDesc_;
    bool needsMapWrite_ = false;
    bool needsMapRead_ = false;
    bool canDirectMap_ = false;    ///< True if buffer was created with MapWrite/MapRead usage
    void* stagingData_ = nullptr;  ///< CPU staging for non-mappable GPU buffers
    u64 mappedOffset_ = 0;
    u64 mappedSize_ = 0;
    u64 dirtySize_ = 0;           ///< Actual bytes written, for partial flush
};

} // namespace primal::graphics::rhi

#endif // ENABLE_WEBGPU
