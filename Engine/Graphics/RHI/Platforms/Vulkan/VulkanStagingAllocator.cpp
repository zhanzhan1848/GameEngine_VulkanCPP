/**
 * @file VulkanStagingAllocator.cpp
 * @brief VulkanStagingAllocator 实现
 * @details Phase 2:实现 Allocate(从当前 frame pool 子分配 CPU-writable 内存)。
 *          Phase 3 将接入 vkCmdCopyBuffer 的 deferred blit。
 *
 * VMA 集成:
 *   - 每 frame pool 用 vmaCreateBuffer + HOST_VISIBLE_SEQUENTIAL_WRITE | MAPPED 分配一块
 *   - Allocate 用线性 bump allocator 取子块,返回 persistent-mapped ptr + offset
 *   - BeginFrame 把下一 pool 的 offset 归零
 *   - Shutdown 走 vmaDestroyBuffer
 *
 * @author GameEngine VulkanCPP Team
 * @date 2026-07-26
 */

#include "VulkanStagingAllocator.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN
// VMA_IMPLEMENTATION 由 VulkanDevice.cpp 单 TU 提供;此处仅用 vk_mem_alloc.h 声明
#include <vk_mem_alloc.h>
#endif

#include <iostream>

namespace primal::graphics::rhi {

VulkanStagingAllocator::~VulkanStagingAllocator() {
    Shutdown();
}

void VulkanStagingAllocator::Initialize(VkDevice device, VmaAllocator allocator, u64 poolSize) {
    if (initialized_) return;
    if (!device || !allocator) {
        std::cerr << "[VulkanStagingAllocator] Initialize: null device or allocator" << std::endl;
        return;
    }

    device_ = device;
    allocator_ = allocator;

    for (u32 i = 0; i < FRAME_COUNT; ++i) {
        FramePool& pool = pools_[i];

        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = poolSize;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo aci{};
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                  | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        aci.usage = VMA_MEMORY_USAGE_AUTO;

        VmaAllocationInfo info{};
        VkResult res = vmaCreateBuffer(allocator_, &bci, &aci,
                                        &pool.buffer, &pool.allocation, &info);
        if (res != VK_SUCCESS) {
            std::cerr << "[VulkanStagingAllocator] vmaCreateBuffer failed for pool " << i
                      << ": " << res << std::endl;
            pool.buffer = VK_NULL_HANDLE;
            pool.allocation = nullptr;
            continue;
        }

        pool.mappedPtr = info.pMappedData;
        pool.capacity = poolSize;
        pool.offset = 0;
    }

    currentFrame_ = 0;
    initialized_ = true;
}

void VulkanStagingAllocator::Shutdown() {
    if (!allocator_) return;
    for (u32 i = 0; i < FRAME_COUNT; ++i) {
        FramePool& pool = pools_[i];
        if (pool.buffer != VK_NULL_HANDLE && pool.allocation) {
            vmaDestroyBuffer(allocator_, pool.buffer, pool.allocation);
        }
        pool.buffer = VK_NULL_HANDLE;
        pool.allocation = nullptr;
        pool.mappedPtr = nullptr;
        pool.offset = 0;
        pool.capacity = 0;
    }
    allocator_ = nullptr;
    device_ = VK_NULL_HANDLE;
    initialized_ = false;
}

void VulkanStagingAllocator::BeginFrame() {
    currentFrame_ = (currentFrame_ + 1) % FRAME_COUNT;
    pools_[currentFrame_].offset = 0;
}

VulkanStagingAllocator::Allocation VulkanStagingAllocator::Allocate(u64 size, u64 alignment) {
    Allocation ret{};
    if (!initialized_) {
        ret.overflow = true;
        return ret;
    }
    if (size == 0) return ret;

    FramePool& pool = pools_[currentFrame_];
    if (!pool.mappedPtr || pool.capacity == 0) {
        ret.overflow = true;
        return ret;
    }

    u64 alignedOffset = (pool.offset + alignment - 1) & ~(alignment - 1);
    if (alignedOffset + size > pool.capacity) {
        ret.overflow = true;
        return ret;
    }

    pool.offset = alignedOffset + size;
    ret.cpuPtr = static_cast<u8*>(pool.mappedPtr) + alignedOffset;
    ret.stagingBuffer = pool.buffer;
    ret.offset = alignedOffset;
    ret.overflow = false;
    return ret;
}

} // namespace primal::graphics::rhi
