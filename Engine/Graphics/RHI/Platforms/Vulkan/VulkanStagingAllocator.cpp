/**
 * @file VulkanStagingAllocator.cpp
 * @brief VulkanStagingAllocator 实现
 * @details P4c-F4:Allocate(pool 耗尽 → FlushBlocking 重试)+
 *          QueueBlit_Buffer/Texture + EncodePendingBlits(布局统一闭合)+
 *          FlushBlocking(transient cmdbuf + queue wait)。
 *
 * VMA 集成:
 *   - 每 frame pool 用 vmaCreateBuffer + HOST_VISIBLE_SEQUENTIAL_WRITE | MAPPED 分配一块
 *   - Allocate 用线性 bump allocator 取子块,返回 persistent-mapped ptr + offset
 *   - BeginFrame 把下一 pool 的 offset 归零(旧 pool 由 4-frame fence 链保护)
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

void VulkanStagingAllocator::Initialize(VkDevice device, VmaAllocator allocator,
                                        VkQueue queue, u32 queueFamily, u64 poolSize) {
    if (initialized_) return;
    if (!device || !allocator) {
        std::cerr << "[VulkanStagingAllocator] Initialize: null device or allocator" << std::endl;
        return;
    }

    device_ = device;
    allocator_ = allocator;
    queue_ = queue;
    queueFamily_ = queueFamily;

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
        pool.pending.clear();
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
        pool.pending.clear();
    }
    allocator_ = nullptr;
    device_ = VK_NULL_HANDLE;
    queue_ = VK_NULL_HANDLE;
    initialized_ = false;
}

void VulkanStagingAllocator::BeginFrame() {
    currentFrame_ = (currentFrame_ + 1) % FRAME_COUNT;
    pools_[currentFrame_].offset = 0;
    // 注:pending 队列理论上已被 EncodePendingBlits 清空;若上一帧没跑任何
    // cmdbuf Begin(极端情况),残留 pending 丢弃前无法恢复 — FlushBlocking
    // 是调用方责任。debug 下留 assert 级日志。
    if (!pools_[currentFrame_].pending.empty()) {
        std::cerr << "[VulkanStagingAllocator] BeginFrame: dropping "
                  << pools_[currentFrame_].pending.size()
                  << " pending blits from an un-encoded frame" << std::endl;
        pools_[currentFrame_].pending.clear();
    }
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
        // P4c-F4 overflow 约定(同 Metal):pool 耗尽/单次超容量 → overflow=true,
        // 调用方负责 fallback:先 FlushBlocking() 排干已 queue 的数据,再用
        // 一次性 VMA staging 路径完成本次上传(见 VulkanTexture::updateDataImpl)。
        // bump 空间要到 BeginFrame 才回收,这里不重试。
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

// ============================================================================
// P4c-F4: pending blit 队列
// ============================================================================
void VulkanStagingAllocator::QueueBlit_Buffer(Allocation alloc,
                                              VkBuffer dstBuffer, u64 dstOffset, u64 size) {
    PendingBlit b{};
    b.srcBuffer = alloc.stagingBuffer;
    b.srcOffset = alloc.offset;
    b.dstBuffer = dstBuffer;
    b.dstBufferOffset = dstOffset;
    b.size = size;
    b.isTexture = false;
    pools_[currentFrame_].pending.push_back(b);
}

void VulkanStagingAllocator::QueueBlit_Texture(Allocation alloc,
                                               VkImage dstTexture, u32 mipLevel, u32 slice,
                                               u32 originX, u32 originY, u32 originZ,
                                               u32 width, u32 height, u32 depth,
                                               u32 bytesPerRow, u32 bytesPerImage,
                                               VkImageLayout currentLayout,
                                               VkImageLayout backLayout,
                                               VkImageAspectFlags aspect,
                                               u32 texelSize) {
    PendingBlit b{};
    b.srcBuffer = alloc.stagingBuffer;
    b.srcOffset = alloc.offset;
    b.dstTexture = dstTexture;
    b.mipLevel = mipLevel;
    b.slice = slice;
    b.originX = originX;
    b.originY = originY;
    b.originZ = originZ;
    b.width = width;
    b.height = height;
    b.depth = depth;
    b.bytesPerRow = bytesPerRow;
    b.bytesPerImage = bytesPerImage;
    b.texelSize = texelSize ? texelSize : 4;
    b.currentLayout = currentLayout;
    b.backLayout = backLayout;
    b.aspect = aspect;
    b.isTexture = true;
    pools_[currentFrame_].pending.push_back(b);
}

void VulkanStagingAllocator::EncodePendingBlits(VkCommandBuffer cmd) {
    if (!initialized_ || cmd == VK_NULL_HANDLE) return;
    FramePool& pool = pools_[currentFrame_];
    if (pool.pending.empty()) return;

    auto textureBarrier = [&](const PendingBlit& b, VkImageLayout oldL, VkImageLayout newL,
                              VkAccessFlags srcAccess, VkPipelineStageFlags srcStage,
                              VkAccessFlags dstAccess, VkPipelineStageFlags dstStage) {
        VkImageMemoryBarrier bar{};
        bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        bar.srcAccessMask = srcAccess;
        bar.dstAccessMask = dstAccess;
        bar.oldLayout = oldL;
        bar.newLayout = newL;
        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.image = b.dstTexture;
        bar.subresourceRange.aspectMask = b.aspect;
        bar.subresourceRange.baseMipLevel = b.mipLevel;
        bar.subresourceRange.levelCount = 1;
        bar.subresourceRange.baseArrayLayer = b.slice;
        bar.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &bar);
    };

    for (const PendingBlit& b : pool.pending) {
        if (!b.isTexture) {
            VkBufferCopy region{};
            region.srcOffset = b.srcOffset;
            region.dstOffset = b.dstBufferOffset;
            region.size = b.size;
            vkCmdCopyBuffer(cmd, b.srcBuffer, b.dstBuffer, 1, &region);
            continue;
        }
        // 布局统一在此闭合:cur → TRANSFER_DST → copy → back
        textureBarrier(b, b.currentLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       0, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

        VkBufferImageCopy copy{};
        copy.bufferOffset = b.srcOffset;
        // Vulkan 的 rowLength/imageHeight 单位是 texel/行,不是字节 —
        // 从 Metal 风格的字节参数换算(0 = 紧密布局,直传)。
        copy.bufferRowLength = b.bytesPerRow ? (b.bytesPerRow / b.texelSize) : 0;
        copy.bufferImageHeight = (b.bytesPerImage && b.bytesPerRow)
                                     ? (b.bytesPerImage / b.bytesPerRow) : 0;
        copy.imageSubresource.aspectMask = b.aspect;
        copy.imageSubresource.mipLevel = b.mipLevel;
        copy.imageSubresource.baseArrayLayer = b.slice;
        copy.imageSubresource.layerCount = 1;
        copy.imageOffset = {int32_t(b.originX), int32_t(b.originY), int32_t(b.originZ)};
        copy.imageExtent = {b.width, b.height, b.depth};
        vkCmdCopyBufferToImage(cmd, b.srcBuffer, b.dstTexture,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

        textureBarrier(b, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, b.backLayout,
                       VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_ACCESS_SHADER_READ_BIT,
                       VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }
    pool.pending.clear();
}

void VulkanStagingAllocator::FlushBlocking() {
    if (!initialized_) return;
    if (pools_[currentFrame_].pending.empty()) return;
    if (queue_ == VK_NULL_HANDLE || device_ == VK_NULL_HANDLE) {
        std::cerr << "[VulkanStagingAllocator] FlushBlocking: no queue available" << std::endl;
        pools_[currentFrame_].pending.clear();
        return;
    }

    // transient pool + one-shot cmdbuf(VulkanBuffer/VulkanTexture 慢路径同款)
    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pci.queueFamilyIndex = queueFamily_;
    VkCommandPool pool = VK_NULL_HANDLE;
    if (vkCreateCommandPool(device_, &pci, nullptr, &pool) != VK_SUCCESS) {
        std::cerr << "[VulkanStagingAllocator] FlushBlocking: vkCreateCommandPool failed" << std::endl;
        return;
    }
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    ai.commandPool = pool;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(device_, &ai, &cmd) != VK_SUCCESS) {
        vkDestroyCommandPool(device_, pool, nullptr);
        return;
    }
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    EncodePendingBlits(cmd);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue_);

    vkFreeCommandBuffers(device_, pool, 1, &cmd);
    vkDestroyCommandPool(device_, pool, nullptr);
}

bool VulkanStagingAllocator::HasPendingBlits() const {
    return initialized_ && !pools_[currentFrame_].pending.empty();
}

} // namespace primal::graphics::rhi
