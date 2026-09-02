/**
 * @file RHIReadbackRing.cpp
 * @brief Fenced 异步回读 ring 实现（VT M0.1）
 * @author GameEngine VulkanCPP Team
 * @date 2026-08-21
 */

#include "RHIReadbackRing.h"
#include "RHIDevice.h"

#include <iostream>
#include "Engine/Common/CoreLogStream.h"

namespace primal::graphics::rhi {

bool rhi_readback_ring::initialize(RHIDeviceBase* device, u64 bufferSize, u32 ringSize) {
    if (!device || bufferSize == 0) return false;
    if (ringSize == 0 || ringSize > MAX_FRAMES_IN_FLIGHT) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[rhi_readback_ring] ringSize must be in [1, "
                  << MAX_FRAMES_IN_FLIGHT << "] (got " << ringSize << ")" << std::endl;
        return false;
    }
    shutdown(device);  // 幂等：重复 initialize 先释放旧资源

    for (u32 i = 0; i < ringSize; ++i) {
        BufferDesc bufDesc{};
        bufDesc.size = bufferSize;
        bufDesc.type = BufferType::Raw;
        bufDesc.memoryUsage = GPUMemoryUsage::Readback;
        bufDesc.name = "ReadbackRing_Slot";
        buffers_[i] = device->CreateBuffer(bufDesc);
        if (buffers_[i] == handles::INVALID_RESOURCE) {
            PRIMAL_LOG_STREAM(Error, "RHI") << "[rhi_readback_ring] CreateBuffer slot " << i << " failed" << std::endl;
            shutdown(device);
            return false;
        }
        fences_[i] = device->CreateSync();
        if (fences_[i] == handles::INVALID_SYNC) {
            PRIMAL_LOG_STREAM(Error, "RHI") << "[rhi_readback_ring] CreateSync slot " << i << " failed" << std::endl;
            shutdown(device);
            return false;
        }
    }
    ringSize_   = ringSize;
    bufferSize_ = bufferSize;
    return true;
}

void rhi_readback_ring::shutdown(RHIDeviceBase* device) {
    if (!device) return;
    for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        if (buffers_[i] != handles::INVALID_RESOURCE) {
            device->DestroyBuffer(buffers_[i]);
            buffers_[i] = handles::INVALID_RESOURCE;
        }
        if (fences_[i] != handles::INVALID_SYNC) {
            device->DestroySync(fences_[i]);
            fences_[i] = handles::INVALID_SYNC;
        }
    }
    ringSize_   = 0;
    bufferSize_ = 0;
}

ResourceHandle rhi_readback_ring::buffer(u32 slot) const {
    if (slot >= ringSize_) return handles::INVALID_RESOURCE;
    return buffers_[slot];
}

void rhi_readback_ring::attach_fence(QueueSubmitInfo& info, u32 slot) const {
    if (slot >= ringSize_) return;
    info.signalFence = fences_[slot];
}

bool rhi_readback_ring::wait(RHIDeviceBase* device, u32 slot, u32 timeoutMs) const {
    if (!device || slot >= ringSize_) return false;
    return device->WaitForSync(fences_[slot], timeoutMs);
}

void* rhi_readback_ring::map(RHIDeviceBase* device, u32 slot) {
    if (!device || slot >= ringSize_) return nullptr;
    return device->MapBuffer(buffers_[slot], 0, bufferSize_);
}

void rhi_readback_ring::unmap(RHIDeviceBase* device, u32 slot) {
    if (!device || slot >= ringSize_) return;
    device->UnmapBuffer(buffers_[slot]);
}

} // namespace primal::graphics::rhi
