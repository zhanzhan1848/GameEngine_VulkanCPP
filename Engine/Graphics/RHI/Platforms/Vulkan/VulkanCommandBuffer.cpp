/**
 * @file VulkanCommandBuffer.cpp
 * @brief VulkanCommandBuffer 实现 — Phase 3
 * @details Begin/End/Reset/Submit/WaitForCompletion + Copy/Blit/Barrier/Mipmap。
 *          Submit 路径:本对象独占 VkFence + device graphics queue + vkQueueSubmit。
 *          Layout transition:VulkanTexture 自带 currentLayout_,TransitionImageLayout 在
 *          Copy/Blit 前后自动 emit VkImageMemoryBarrier(避免上层显式管理)。
 * @author GameEngine VulkanCPP Team
 * @date 2026-07-26
 */

#include "VulkanCommandBuffer.h"
#include "VulkanDevice.h"
#include "VulkanBuffer.h"
#include "VulkanTexture.h"
#include "VulkanRenderPass.h"
#include "VulkanPipelineLayout.h"
#include "VulkanPipeline.h"
#include "VulkanDescriptorSet.h"
#include "VulkanMath.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

#include <iostream>
#include <algorithm>

namespace primal::graphics::rhi {

VulkanCommandBuffer::VulkanCommandBuffer(VulkanDevice& device, CommandQueueType type)
    : RHICommandBuffer(device, type) {
}

VulkanCommandBuffer::VulkanCommandBuffer(VulkanCommandBuffer&& other) noexcept
    : RHICommandBuffer(std::move(other)),
      cmdPool_(other.cmdPool_),
      cmdBuffer_(other.cmdBuffer_),
      submitFence_(other.submitFence_),
      queueFamily_(other.queueFamily_),
      scope_(other.scope_) {
    other.cmdPool_ = VK_NULL_HANDLE;
    other.cmdBuffer_ = VK_NULL_HANDLE;
    other.submitFence_ = VK_NULL_HANDLE;
    other.queueFamily_ = UINT32_MAX;
    other.scope_ = Scope::None;
}

VulkanCommandBuffer& VulkanCommandBuffer::operator=(VulkanCommandBuffer&& other) noexcept {
    if (this != &other) {
        destroyImpl();
        RHICommandBuffer::operator=(std::move(other));
        cmdPool_ = other.cmdPool_;
        cmdBuffer_ = other.cmdBuffer_;
        submitFence_ = other.submitFence_;
        queueFamily_ = other.queueFamily_;
        scope_ = other.scope_;
        other.cmdPool_ = VK_NULL_HANDLE;
        other.cmdBuffer_ = VK_NULL_HANDLE;
        other.submitFence_ = VK_NULL_HANDLE;
        other.queueFamily_ = UINT32_MAX;
        other.scope_ = Scope::None;
    }
    return *this;
}

VulkanCommandBuffer::~VulkanCommandBuffer() {
    destroyImpl();
}

bool VulkanCommandBuffer::Initialize() {
    VulkanDevice& vk = static_cast<VulkanDevice&>(device_);
    VkDevice dev = vk.GetNativeDevice();
    if (!dev) return false;

    // Phase 1-3 都用 graphics queue family
    queueFamily_ = vk.GetGraphicsQueueFamily();
    if (queueFamily_ == UINT32_MAX) {
        std::cerr << "[VulkanCommandBuffer] No graphics queue family" << std::endl;
        return false;
    }

    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;  // 允许 vkResetCommandBuffer
    pci.queueFamilyIndex = queueFamily_;
    if (vkCreateCommandPool(dev, &pci, nullptr, &cmdPool_) != VK_SUCCESS) {
        std::cerr << "[VulkanCommandBuffer] vkCreateCommandPool failed" << std::endl;
        return false;
    }

    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = cmdPool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(dev, &ai, &cmdBuffer_) != VK_SUCCESS) {
        std::cerr << "[VulkanCommandBuffer] vkAllocateCommandBuffers failed" << std::endl;
        vkDestroyCommandPool(dev, cmdPool_, nullptr);
        cmdPool_ = VK_NULL_HANDLE;
        return false;
    }

    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = 0;
    if (vkCreateFence(dev, &fci, nullptr, &submitFence_) != VK_SUCCESS) {
        std::cerr << "[VulkanCommandBuffer] vkCreateFence failed" << std::endl;
        vkFreeCommandBuffers(dev, cmdPool_, 1, &cmdBuffer_);
        vkDestroyCommandPool(dev, cmdPool_, nullptr);
        cmdBuffer_ = VK_NULL_HANDLE;
        cmdPool_ = VK_NULL_HANDLE;
        return false;
    }

    state_ = CommandBufferState::Reset;
    return true;
}

void VulkanCommandBuffer::destroyImpl() {
    VulkanDevice& vk = static_cast<VulkanDevice&>(device_);
    VkDevice dev = vk.GetNativeDevice();
    if (submitFence_ != VK_NULL_HANDLE) {
        vkDestroyFence(dev, submitFence_, nullptr);
        submitFence_ = VK_NULL_HANDLE;
    }
    if (cmdBuffer_ != VK_NULL_HANDLE && cmdPool_ != VK_NULL_HANDLE) {
        vkFreeCommandBuffers(dev, cmdPool_, 1, &cmdBuffer_);
        cmdBuffer_ = VK_NULL_HANDLE;
    }
    if (cmdPool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(dev, cmdPool_, nullptr);
        cmdPool_ = VK_NULL_HANDLE;
    }
}

bool VulkanCommandBuffer::resetImpl() {
    if (cmdBuffer_ == VK_NULL_HANDLE) return false;
    VkDevice dev = static_cast<VulkanDevice&>(device_).GetNativeDevice();

    // Reset command buffer (commands cleared, ready to begin)
    VkResult res = vkResetCommandBuffer(cmdBuffer_, VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT);
    if (res != VK_SUCCESS) {
        std::cerr << "[VulkanCommandBuffer] vkResetCommandBuffer failed: " << res << std::endl;
        return false;
    }

    // Reset fence too (so next Submit can ping it)
    if (submitFence_ != VK_NULL_HANDLE) {
        vkResetFences(dev, 1, &submitFence_);
    }
    scope_ = Scope::None;
    return true;
}

bool VulkanCommandBuffer::beginImpl() {
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    bi.pInheritanceInfo = nullptr;
    VkResult res = vkBeginCommandBuffer(cmdBuffer_, &bi);
    if (res != VK_SUCCESS) {
        std::cerr << "[VulkanCommandBuffer] vkBeginCommandBuffer failed: " << res << std::endl;
        return false;
    }
    scope_ = Scope::None;
    return true;
}

bool VulkanCommandBuffer::endImpl() {
    VkResult res = vkEndCommandBuffer(cmdBuffer_);
    if (res != VK_SUCCESS) {
        std::cerr << "[VulkanCommandBuffer] vkEndCommandBuffer failed: " << res << std::endl;
        return false;
    }
    return true;
}

bool VulkanCommandBuffer::submitImpl(u32 /*waitFlags*/) {
    VulkanDevice& vk = static_cast<VulkanDevice&>(device_);
    VkQueue queue = vk.GetGraphicsQueue();
    if (!queue || !cmdBuffer_) return false;

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = 0;
    si.pWaitSemaphores = nullptr;
    si.pWaitDstStageMask = nullptr;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmdBuffer_;
    si.signalSemaphoreCount = 0;
    si.pSignalSemaphores = nullptr;

    VkResult res = vkQueueSubmit(queue, 1, &si, submitFence_);
    if (res != VK_SUCCESS) {
        std::cerr << "[VulkanCommandBuffer] vkQueueSubmit failed: " << res << std::endl;
        return false;
    }
    return true;
}

bool VulkanCommandBuffer::waitForCompletionImpl() {
    VkDevice dev = static_cast<VulkanDevice&>(device_).GetNativeDevice();
    if (submitFence_ == VK_NULL_HANDLE) return false;
    VkResult res = vkWaitForFences(dev, 1, &submitFence_, VK_TRUE, 5ULL * 1000ULL * 1000ULL * 1000ULL);
    if (res == VK_SUCCESS) return true;
    if (res == VK_TIMEOUT) {
        std::cerr << "[VulkanCommandBuffer] WaitForCompletion TIMEOUT (5s)" << std::endl;
        return false;
    }
    std::cerr << "[VulkanCommandBuffer] vkWaitForFences failed: " << res << std::endl;
    return false;
}

// ============================================================================
// 资源操作命令
// ============================================================================

void VulkanCommandBuffer::MemoryBarrier(PipelineStage srcStageMask, PipelineStage dstStageMask,
                                         AccessFlag srcAccessMask, AccessFlag dstAccessMask) {
    VkMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    b.pNext = nullptr;
    b.srcAccessMask = vulkan::ToVkAccessFlags(srcAccessMask);
    b.dstAccessMask = vulkan::ToVkAccessFlags(dstAccessMask);

    vkCmdPipelineBarrier(cmdBuffer_,
        vulkan::ToVkPipelineStageFlags(srcStageMask),
        vulkan::ToVkPipelineStageFlags(dstStageMask),
        0,                          // dependencyFlags
        1, &b,
        0, nullptr,                 // buffer barriers
        0, nullptr);                // image barriers
    UpdateStats(CommandType::Barrier);
}

void VulkanCommandBuffer::CopyBuffer(ResourceHandle src, ResourceHandle dst,
                                     u64 srcOffset, u64 dstOffset, u64 size) {
    VulkanDevice& vk = static_cast<VulkanDevice&>(device_);
    VulkanBuffer* srcBuf = vk.GetBuffer(src);
    VulkanBuffer* dstBuf = vk.GetBuffer(dst);
    if (!srcBuf || !dstBuf || !srcBuf->GetNativeBuffer() || !dstBuf->GetNativeBuffer()) return;

    VkBufferCopy region{};
    region.srcOffset = srcOffset;
    region.dstOffset = dstOffset;
    region.size = (size == 0)
        ? std::min<u64>(srcBuf->GetSize() - srcOffset, dstBuf->GetSize() - dstOffset)
        : size;

    vkCmdCopyBuffer(cmdBuffer_, srcBuf->GetNativeBuffer(), dstBuf->GetNativeBuffer(), 1, &region);
    UpdateStats(CommandType::CopyBuffer);
}

void VulkanCommandBuffer::CopyBufferToTexture(ResourceHandle srcBuffer, ResourceHandle dstTexture,
                                              const BufferTextureCopyRegion* regions, u32 regionCount) {
    VulkanDevice& vk = static_cast<VulkanDevice&>(device_);
    VulkanBuffer* buf = vk.GetBuffer(srcBuffer);
    VulkanTexture* tex = vk.GetTexture(dstTexture);
    if (!buf || !tex || !buf->GetNativeBuffer() || !tex->GetNativeImage()) return;

    // Transition dst texture to TRANSFER_DST_OPTIMAL(若还没在那里)
    TransitionImageLayout(tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    std::vector<VkBufferImageCopy> copies(regionCount);
    for (u32 i = 0; i < regionCount; ++i) {
        const BufferTextureCopyRegion& r = regions[i];
        VkBufferImageCopy& c = copies[i];
        c.bufferOffset = r.bufferOffset;
        c.bufferRowLength = r.bufferRowLength;
        c.bufferImageHeight = r.bufferImageHeight;
        c.imageSubresource.aspectMask = tex->GetAspectMask();
        c.imageSubresource.mipLevel = r.imageSubresource.mipLevel;
        c.imageSubresource.baseArrayLayer = r.imageSubresource.baseArrayLayer;
        c.imageSubresource.layerCount = r.imageSubresource.layerCount;
        c.imageOffset = { r.imageOffset.x, r.imageOffset.y, r.imageOffset.z };
        c.imageExtent = { r.imageExtent.width, r.imageExtent.height, r.imageExtent.depth };
    }
    vkCmdCopyBufferToImage(cmdBuffer_, buf->GetNativeBuffer(), tex->GetNativeImage(),
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           regionCount, copies.data());
    UpdateStats(CommandType::CopyBufferToTexture);
}

void VulkanCommandBuffer::CopyTextureToBuffer(ResourceHandle srcTexture, ResourceHandle dstBuffer,
                                              const BufferTextureCopyRegion* regions, u32 regionCount) {
    VulkanDevice& vk = static_cast<VulkanDevice&>(device_);
    VulkanTexture* tex = vk.GetTexture(srcTexture);
    VulkanBuffer* buf = vk.GetBuffer(dstBuffer);
    if (!tex || !buf || !tex->GetNativeImage() || !buf->GetNativeBuffer()) return;

    // Transition src to TRANSFER_SRC_OPTIMAL
    TransitionImageLayout(tex, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    std::vector<VkBufferImageCopy> copies(regionCount);
    for (u32 i = 0; i < regionCount; ++i) {
        const BufferTextureCopyRegion& r = regions[i];
        VkBufferImageCopy& c = copies[i];
        c.bufferOffset = r.bufferOffset;
        c.bufferRowLength = r.bufferRowLength;
        c.bufferImageHeight = r.bufferImageHeight;
        c.imageSubresource.aspectMask = tex->GetAspectMask();
        c.imageSubresource.mipLevel = r.imageSubresource.mipLevel;
        c.imageSubresource.baseArrayLayer = r.imageSubresource.baseArrayLayer;
        c.imageSubresource.layerCount = r.imageSubresource.layerCount;
        c.imageOffset = { r.imageOffset.x, r.imageOffset.y, r.imageOffset.z };
        c.imageExtent = { r.imageExtent.width, r.imageExtent.height, r.imageExtent.depth };
    }
    vkCmdCopyImageToBuffer(cmdBuffer_, tex->GetNativeImage(),
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           buf->GetNativeBuffer(),
                           regionCount, copies.data());
    UpdateStats(CommandType::CopyTextureToBuffer);
}

void VulkanCommandBuffer::BlitTexture(ResourceHandle src, ResourceHandle dst,
                                      const TextureBlitRegion* regions, u32 regionCount,
                                      FilterMode filter) {
    VulkanDevice& vk = static_cast<VulkanDevice&>(device_);
    VulkanTexture* srcTex = vk.GetTexture(src);
    VulkanTexture* dstTex = vk.GetTexture(dst);
    if (!srcTex || !dstTex || !srcTex->GetNativeImage() || !dstTex->GetNativeImage()) return;

    TransitionImageLayout(srcTex, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    TransitionImageLayout(dstTex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    std::vector<VkImageBlit> blits(regionCount);
    for (u32 i = 0; i < regionCount; ++i) {
        const TextureBlitRegion& r = regions[i];
        VkImageBlit& b = blits[i];
        b.srcSubresource.aspectMask = srcTex->GetAspectMask();
        b.srcSubresource.mipLevel = r.srcSubresource.mipLevel;
        b.srcSubresource.baseArrayLayer = r.srcSubresource.baseArrayLayer;
        b.srcSubresource.layerCount = r.srcSubresource.layerCount;
        b.srcOffsets[0] = { r.srcOffsets[0].x, r.srcOffsets[0].y, r.srcOffsets[0].z };
        b.srcOffsets[1] = { r.srcOffsets[1].x, r.srcOffsets[1].y, r.srcOffsets[1].z };
        b.dstSubresource.aspectMask = dstTex->GetAspectMask();
        b.dstSubresource.mipLevel = r.dstSubresource.mipLevel;
        b.dstSubresource.baseArrayLayer = r.dstSubresource.baseArrayLayer;
        b.dstSubresource.layerCount = r.dstSubresource.layerCount;
        b.dstOffsets[0] = { r.dstOffsets[0].x, r.dstOffsets[0].y, r.dstOffsets[0].z };
        b.dstOffsets[1] = { r.dstOffsets[1].x, r.dstOffsets[1].y, r.dstOffsets[1].z };
    }
    VkFilter vkFilter = vulkan::ToVkFilter(filter);
    vkCmdBlitImage(cmdBuffer_,
                   srcTex->GetNativeImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   dstTex->GetNativeImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   regionCount, blits.data(), vkFilter);
    UpdateStats(CommandType::BlitTexture);
}

void VulkanCommandBuffer::GenerateMipmaps(ResourceHandle texture) {
    VulkanDevice& vk = static_cast<VulkanDevice&>(device_);
    VulkanTexture* tex = vk.GetTexture(texture);
    if (!tex || !tex->GetNativeImage()) return;

    // 简化版:仅支持 2D,full-mipchain 链式 blit。Format 必须支持 linear filtering(2D 单采样)。
    // 步骤:先把全图 transition 到 TRANSFER_SRC( base mip 也作为 src 用),然后
    //       循环:mip[i] → mip[i+1] blit,每步之间 barrier mip[i+1] DST→SRC。
    // 实际上更精确的做法是 base mip 转到 SRC,后续 mip 转 DST 再 blit,最后所有转回 SHADER_READ。

    const u32 mipCount = static_cast<u32>(tex->mipLayouts_.size());
    if (mipCount <= 1) return;

    // 整图 layout 一致:这里先转 base mip 到 TRANSFER_SRC_OPTIMAL
    // (GenerateMipmaps 假设调用方刚上传完 base mip,base mip 当前是 TRANSFER_DST_OPTIMAL)
    // 全图 transition:base → SRC,其它 mip → DST,后续链 blit。
    VkImageMemoryBarrier whole{};
    whole.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    whole.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    whole.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    whole.image = tex->GetNativeImage();
    whole.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;  // 假设刚 Upload
    whole.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    whole.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    whole.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    whole.subresourceRange.aspectMask = tex->GetAspectMask();
    whole.subresourceRange.baseMipLevel = 0;
    whole.subresourceRange.levelCount = mipCount;
    whole.subresourceRange.baseArrayLayer = 0;
    whole.subresourceRange.layerCount = 1;

    vkCmdPipelineBarrier(cmdBuffer_,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
        0, nullptr, 0, nullptr, 1, &whole);

    for (u32 i = 0; i + 1 < mipCount; ++i) {
        VkImageBlit blit{};
        blit.srcSubresource.aspectMask = tex->GetAspectMask();
        blit.srcSubresource.mipLevel = i;
        blit.srcSubresource.baseArrayLayer = 0;
        blit.srcSubresource.layerCount = 1;
        blit.srcOffsets[0] = { 0, 0, 0 };
        // src width/height:第 i mip 的尺寸
        const u32 srcW = std::max<u32>(1u, tex->GetTextureDesc().size.x >> i);
        const u32 srcH = std::max<u32>(1u, tex->GetTextureDesc().size.y >> i);
        blit.srcOffsets[1] = { static_cast<s32>(srcW), static_cast<s32>(srcH), 1 };

        blit.dstSubresource.aspectMask = tex->GetAspectMask();
        blit.dstSubresource.mipLevel = i + 1;
        blit.dstSubresource.baseArrayLayer = 0;
        blit.dstSubresource.layerCount = 1;
        blit.dstOffsets[0] = { 0, 0, 0 };
        const u32 dstW = std::max<u32>(1u, tex->GetTextureDesc().size.x >> (i + 1));
        const u32 dstH = std::max<u32>(1u, tex->GetTextureDesc().size.y >> (i + 1));
        blit.dstOffsets[1] = { static_cast<s32>(dstW), static_cast<s32>(dstH), 1 };

        // dst mip 当前 layout 还是 TRANSFER_DST_OPTIMAL(只有 base 是 SRC)
        // 但我们刚把整图转到 SRC — 修正:重新 transition mip i+1 到 DST
        VkImageMemoryBarrier toDst{};
        toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDst.image = tex->GetNativeImage();
        toDst.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;  // 整图刚到 SRC
        toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toDst.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toDst.subresourceRange.aspectMask = tex->GetAspectMask();
        toDst.subresourceRange.baseMipLevel = i + 1;  // 只影响 i+1 这层
        toDst.subresourceRange.levelCount = 1;
        toDst.subresourceRange.baseArrayLayer = 0;
        toDst.subresourceRange.layerCount = 1;

        vkCmdPipelineBarrier(cmdBuffer_,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
            0, nullptr, 0, nullptr, 1, &toDst);

        vkCmdBlitImage(cmdBuffer_,
            tex->GetNativeImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            tex->GetNativeImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &blit, VK_FILTER_LINEAR);

        // blit 完后把 mip i+1 从 DST 转回 SRC,供下一次循环用
        VkImageMemoryBarrier toSrc{};
        toSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toSrc.image = tex->GetNativeImage();
        toSrc.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toSrc.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toSrc.subresourceRange.aspectMask = tex->GetAspectMask();
        toSrc.subresourceRange.baseMipLevel = i + 1;
        toSrc.subresourceRange.levelCount = 1;
        toSrc.subresourceRange.baseArrayLayer = 0;
        toSrc.subresourceRange.layerCount = 1;

        vkCmdPipelineBarrier(cmdBuffer_,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
            0, nullptr, 0, nullptr, 1, &toSrc);
    }

    // 最后把整图转回 SHADER_READ_ONLY(假设调用方期望 mipmap 完成、采样就绪)
    VkImageMemoryBarrier toRead{};
    toRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toRead.image = tex->GetNativeImage();
    toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toRead.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toRead.subresourceRange.aspectMask = tex->GetAspectMask();
    toRead.subresourceRange.baseMipLevel = 0;
    toRead.subresourceRange.levelCount = mipCount;
    toRead.subresourceRange.baseArrayLayer = 0;
    toRead.subresourceRange.layerCount = 1;

    vkCmdPipelineBarrier(cmdBuffer_,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
        0, nullptr, 0, nullptr, 1, &toRead);

    tex->SetCurrentLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    UpdateStats(CommandType::GenerateMipmaps);
}

void VulkanCommandBuffer::InsertBarrier(const ResourceBarrier* barriers, u32 barrierCount) {
    if (!barriers || barrierCount == 0) return;
    VulkanDevice& vk = static_cast<VulkanDevice&>(device_);

    // 这里只对 texture 做完整的 ImageMemoryBarrier;Buffer 路径 phase 5 再加(暂未在测试用到)
    std::vector<VkImageMemoryBarrier> imgBarriers;
    imgBarriers.reserve(barrierCount);

    for (u32 i = 0; i < barrierCount; ++i) {
        const ResourceBarrier& rb = barriers[i];
        VulkanTexture* tex = vk.GetTexture(rb.resource);
        if (!tex) continue;

        vulkan::VkLayoutAccess srcLA = vulkan::ResourceStateToVkLayout(rb.beforeState);
        vulkan::VkLayoutAccess dstLA = vulkan::ResourceStateToVkLayout(rb.afterState);

        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = tex->GetNativeImage();
        b.oldLayout = srcLA.layout;
        b.newLayout = dstLA.layout;
        b.srcAccessMask = srcLA.access;
        b.dstAccessMask = dstLA.access;
        b.subresourceRange.aspectMask = tex->GetAspectMask();
        b.subresourceRange.baseMipLevel = (rb.subresource == 0xFFFFFFFF) ? 0 : rb.subresource;
        b.subresourceRange.levelCount   = (rb.subresource == 0xFFFFFFFF) ? VK_REMAINING_MIP_LEVELS : 1;
        b.subresourceRange.baseArrayLayer = 0;
        b.subresourceRange.layerCount   = VK_REMAINING_ARRAY_LAYERS;
        imgBarriers.push_back(b);

        tex->SetCurrentLayout(dstLA.layout);
    }

    if (!imgBarriers.empty()) {
        vkCmdPipelineBarrier(cmdBuffer_,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
            0, nullptr, 0, nullptr,
            static_cast<u32>(imgBarriers.size()), imgBarriers.data());
    }
    UpdateStats(CommandType::Barrier);
}

void VulkanCommandBuffer::TransitionImageLayout(VulkanTexture* tex, VkImageLayout newLayout,
                                                u32 baseMip, u32 mipCount) {
    if (!tex || !tex->GetNativeImage()) return;
    VkImageLayout oldLayout = tex->GetCurrentLayout();
    if (oldLayout == newLayout) return;

    if (mipCount == 0xFFFFFFFFu) mipCount = static_cast<u32>(tex->mipLayouts_.size()) - baseMip;

    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = tex->GetNativeImage();
    b.oldLayout = oldLayout;
    b.newLayout = newLayout;

    // 选择 access mask + stage mask
    switch (oldLayout) {
        case VK_IMAGE_LAYOUT_UNDEFINED:
            b.srcAccessMask = 0;
            break;
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            break;
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            break;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            break;
        default:
            b.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            break;
    }
    switch (newLayout) {
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            break;
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            break;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            break;
        default:
            b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            break;
    }
    b.subresourceRange.aspectMask = tex->GetAspectMask();
    b.subresourceRange.baseMipLevel = baseMip;
    b.subresourceRange.levelCount = mipCount;
    b.subresourceRange.baseArrayLayer = 0;
    b.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;

    VkPipelineStageFlags srcStage, dstStage;
    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
        srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    } else {
        srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    }
    if (newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else {
        dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    }

    vkCmdPipelineBarrier(cmdBuffer_, srcStage, dstStage, 0,
                         0, nullptr, 0, nullptr, 1, &b);

    tex->SetCurrentLayout(newLayout);
}

// ============================================================================
// Phase 4: Render-pass 命令
// ============================================================================

void VulkanCommandBuffer::BeginRenderPass(const RenderPassDesc& desc) {
    VulkanDevice& vk = static_cast<VulkanDevice&>(device_);
    VkDevice dev = vk.GetNativeDevice();

    // 通过 desc 临时构建 VkRenderPass + VkFramebuffer
    // (Phase 5+ 改为 cache by hash 或 dynamic rendering)
    std::vector<VkAttachmentDescription> attachments;
    std::vector<VkAttachmentReference> colorRefs;
    std::vector<VkClearValue> clears;
    std::vector<VkImageView> fbAttachments;

    const u32 colorCount = static_cast<u32>(desc.colorAttachments.size());
    colorRefs.reserve(colorCount);
    for (u32 i = 0; i < colorCount; ++i) {
        const auto& a = desc.colorAttachments[i];
        VulkanTexture* tex = vk.GetTexture(a.texture);
        if (!tex) continue;

        // 隐式 transition 到 COLOR_ATTACHMENT_OPTIMAL(若还没在)
        if (tex->GetCurrentLayout() != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
            TransitionImageLayout(tex, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        }

        // Caller may leave format=Unknown (Metal backend derives from texture).
        // Fall back to the texture's actual format so VK_FORMAT_UNDEFINED is never passed in.
        const DataFormat effectiveFormat = (a.format != DataFormat::Unknown)
            ? a.format : tex->GetTextureDesc().format;

        VkAttachmentDescription d{};
        d.format = vulkan::ToVkFormat(effectiveFormat);
        d.samples = VK_SAMPLE_COUNT_1_BIT;
        d.loadOp = (a.loadOp == LoadAction::Load) ? VK_ATTACHMENT_LOAD_OP_LOAD :
                   (a.loadOp == LoadAction::Clear) ? VK_ATTACHMENT_LOAD_OP_CLEAR :
                                                    VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        d.storeOp = (a.storeOp == StoreAction::Store) ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        d.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        d.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        d.initialLayout = (a.loadOp == LoadAction::Load) ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
        d.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachments.push_back(d);
        colorRefs.push_back({ i, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL });

        VkClearValue cv{};
        cv.color.float32[0] = a.clearValue.color.x;
        cv.color.float32[1] = a.clearValue.color.y;
        cv.color.float32[2] = a.clearValue.color.z;
        cv.color.float32[3] = a.clearValue.color.w;
        clears.push_back(cv);
        // Per-layer view: array textures pick the slice; single-layer textures fall through to vkView_.
        fbAttachments.push_back(tex->GetLayerView(a.arrayLayer));
    }

    VkAttachmentReference depthRef{};
    bool hasDepth = (desc.depthAttachment.texture != handles::INVALID_RESOURCE &&
                     (desc.depthAttachment.format != DataFormat::Unknown ||
                      vk.GetTexture(desc.depthAttachment.texture) != nullptr));
    if (hasDepth) {
        const auto& a = desc.depthAttachment;
        VulkanTexture* tex = vk.GetTexture(a.texture);
        if (tex) {
            if (tex->GetCurrentLayout() != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
                TransitionImageLayout(tex, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
            }
            // See color-attachment note: derive format from texture when caller left it Unknown.
            const DataFormat effectiveDepthFormat = (a.format != DataFormat::Unknown)
                ? a.format : tex->GetTextureDesc().format;
            VkAttachmentDescription d{};
            d.format = vulkan::ToVkFormat(effectiveDepthFormat);
            d.samples = VK_SAMPLE_COUNT_1_BIT;
            d.loadOp = (a.loadOp == LoadAction::Load) ? VK_ATTACHMENT_LOAD_OP_LOAD :
                       (a.loadOp == LoadAction::Clear) ? VK_ATTACHMENT_LOAD_OP_CLEAR :
                                                        VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            d.storeOp = (a.storeOp == StoreAction::Store) ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
            d.stencilLoadOp = d.loadOp;
            d.stencilStoreOp = d.storeOp;
            d.initialLayout = (a.loadOp == LoadAction::Load) ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
            d.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            attachments.push_back(d);
            depthRef.attachment = colorCount;
            depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

            VkClearValue cv{};
            cv.depthStencil.depth = a.clearValue.depth;
            cv.depthStencil.stencil = a.clearValue.stencil;
            clears.push_back(cv);
            fbAttachments.push_back(tex->GetLayerView(a.arrayLayer));
        }
    }

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = colorCount;
    subpass.pColorAttachments = colorRefs.data();
    subpass.pDepthStencilAttachment = hasDepth ? &depthRef : nullptr;

    VkRenderPassCreateInfo rpci{};
    rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = static_cast<u32>(attachments.size());
    rpci.pAttachments = attachments.data();
    rpci.subpassCount = 1;
    rpci.pSubpasses = &subpass;
    VkRenderPass rp;
    if (vkCreateRenderPass(dev, &rpci, nullptr, &rp) != VK_SUCCESS) {
        std::cerr << "[VulkanCommandBuffer] BeginRenderPass vkCreateRenderPass failed" << std::endl;
        return;
    }

    // ForwardRenderer doesn't set desc.viewport/scissor on passDesc (relies on
    // SetViewport/SetScissor after BeginRenderPass — Metal derives dimensions
    // from the drawable). Fall back through: explicit desc → texture dims.
    u32 fbW = static_cast<u32>(desc.viewport.size.x);
    u32 fbH = static_cast<u32>(desc.viewport.size.y);
    if (fbW == 0 || fbH == 0) {
        // Pull from the first valid attachment's texture desc.
        for (const auto viewTex : {vk.GetTexture(desc.colorAttachments.empty() ? handles::INVALID_RESOURCE : desc.colorAttachments[0].texture),
                                    vk.GetTexture(desc.depthAttachment.texture)}) {
            if (viewTex) {
                fbW = viewTex->GetTextureDesc().size.x;
                fbH = viewTex->GetTextureDesc().size.y;
                break;
            }
        }
    }
    fbW = std::max<u32>(1u, fbW);
    fbH = std::max<u32>(1u, fbH);

    VkFramebufferCreateInfo fbci{};
    fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbci.renderPass = rp;
    fbci.attachmentCount = static_cast<u32>(fbAttachments.size());
    fbci.pAttachments = fbAttachments.data();
    fbci.width  = fbW;
    fbci.height = fbH;
    fbci.layers = 1;
    VkFramebuffer fb;
    if (vkCreateFramebuffer(dev, &fbci, nullptr, &fb) != VK_SUCCESS) {
        std::cerr << "[VulkanCommandBuffer] vkCreateFramebuffer failed" << std::endl;
        vkDestroyRenderPass(dev, rp, nullptr);
        return;
    }

    VkRenderPassBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    bi.renderPass = rp;
    bi.framebuffer = fb;
    const u32 areaW = (desc.scissor.extent.x > 0) ? desc.scissor.extent.x : fbW;
    const u32 areaH = (desc.scissor.extent.y > 0) ? desc.scissor.extent.y : fbH;
    bi.renderArea.offset.x = desc.scissor.offset.x;
    bi.renderArea.offset.y = desc.scissor.offset.y;
    bi.renderArea.extent.width = areaW;
    bi.renderArea.extent.height = areaH;
    bi.clearValueCount = static_cast<u32>(clears.size());
    bi.pClearValues = clears.data();

    vkCmdBeginRenderPass(cmdBuffer_, &bi, VK_SUBPASS_CONTENTS_INLINE);
    scope_ = Scope::RenderPass;

    // 暂存 rp/fb 以便 EndRenderPass 销毁
    pendingRenderPass_ = rp;
    pendingFramebuffer_ = fb;

    // 自动设置 viewport/scissor(pipeline 用 dynamic state)
    // Apply the same fallback as renderArea: if caller left desc.viewport at {0,0},
    // use the framebuffer dimensions we derived.
    ViewportDesc effectiveVP = desc.viewport;
    if (effectiveVP.size.x <= 0.0f || effectiveVP.size.y <= 0.0f) {
        effectiveVP.size.x = static_cast<float>(fbW);
        effectiveVP.size.y = static_cast<float>(fbH);
    }
    SetViewport(effectiveVP);
    Rect effectiveScissor = desc.scissor;
    if (effectiveScissor.extent.x == 0) effectiveScissor.extent.x = areaW;
    if (effectiveScissor.extent.y == 0) effectiveScissor.extent.y = areaH;
    SetScissor(effectiveScissor);
    UpdateStats(CommandType::BeginRenderPass);
}

void VulkanCommandBuffer::BeginRenderPass(RenderPassHandle renderPass) {
    VulkanDevice& vk = static_cast<VulkanDevice&>(device_);
    VulkanRenderPass* rp = vk.GetRenderPass(renderPass);
    if (!rp) {
        std::cerr << "[VulkanCommandBuffer] BeginRenderPass(handle): invalid handle" << std::endl;
        return;
    }
    // Phase 4 简化:RenderPassHandle 重载用 desc 版本走一遍
    BeginRenderPass(rp->GetDesc());
}

void VulkanCommandBuffer::EndRenderPass() {
    if (scope_ != Scope::RenderPass) return;
    vkCmdEndRenderPass(cmdBuffer_);
    scope_ = Scope::None;

    // 销毁临时 rp/fb(它们是 BeginRenderPass 内部创建的)
    if (pendingRenderPass_ != VK_NULL_HANDLE || pendingFramebuffer_ != VK_NULL_HANDLE) {
        VkDevice dev = static_cast<VulkanDevice&>(device_).GetNativeDevice();
        VkRenderPass rp = pendingRenderPass_;
        VkFramebuffer fb = pendingFramebuffer_;
        device_.GetGarbageCollector().DeferredDestroy([dev, rp, fb]() {
            if (dev != VK_NULL_HANDLE && fb != VK_NULL_HANDLE) vkDestroyFramebuffer(dev, fb, nullptr);
            if (dev != VK_NULL_HANDLE && rp != VK_NULL_HANDLE) vkDestroyRenderPass(dev, rp, nullptr);
        });
        pendingRenderPass_ = VK_NULL_HANDLE;
        pendingFramebuffer_ = VK_NULL_HANDLE;
    }
    UpdateStats(CommandType::EndRenderPass);
}

void VulkanCommandBuffer::SetViewport(const ViewportDesc& viewport) {
    if (scope_ != Scope::RenderPass) return;
    VkViewport vp{};
    vp.x = viewport.topLeft.x;
    vp.y = viewport.topLeft.y;
    vp.width  = viewport.size.x;
    vp.height = viewport.size.y;
    vp.minDepth = viewport.minDepth;
    vp.maxDepth = viewport.maxDepth;
    vkCmdSetViewport(cmdBuffer_, 0, 1, &vp);
}

void VulkanCommandBuffer::SetScissor(const Rect& scissor) {
    if (scope_ != Scope::RenderPass) return;
    VkRect2D r{};
    r.offset.x = scissor.offset.x;
    r.offset.y = scissor.offset.y;
    r.extent.width = scissor.extent.x;
    r.extent.height = scissor.extent.y;
    vkCmdSetScissor(cmdBuffer_, 0, 1, &r);
}

void VulkanCommandBuffer::BindGraphicsPipeline(PipelineHandle pipeline) {
    VulkanDevice& vk = static_cast<VulkanDevice&>(device_);
    VulkanPipeline* p = vk.GetPipeline(pipeline);
    if (!p || p->IsCompute()) return;
    vkCmdBindPipeline(cmdBuffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, p->GetNativePipeline());
    boundPipelineLayout_ = p->GetNativePipelineLayout();
}

void VulkanCommandBuffer::BindVertexBuffers(u32 firstSlot, u32 slotCount,
                                            const ResourceHandle* buffers, const u64* offsets) {
    if (!buffers || slotCount == 0) return;
    VulkanDevice& vk = static_cast<VulkanDevice&>(device_);
    std::vector<VkBuffer> bufObjs(slotCount);
    std::vector<VkDeviceSize> offs(slotCount, 0);
    for (u32 i = 0; i < slotCount; ++i) {
        VulkanBuffer* b = vk.GetBuffer(buffers[i]);
        bufObjs[i] = b ? b->GetNativeBuffer() : VK_NULL_HANDLE;
        offs[i] = offsets ? offsets[i] : 0;
    }
    vkCmdBindVertexBuffers(cmdBuffer_, firstSlot, slotCount, bufObjs.data(), offs.data());
}

void VulkanCommandBuffer::BindIndexBuffer(ResourceHandle buffer, DataFormat format, u64 offset) {
    VulkanDevice& vk = static_cast<VulkanDevice&>(device_);
    VulkanBuffer* b = vk.GetBuffer(buffer);
    if (!b) return;
    VkIndexType idxType = (format == DataFormat::R16_UInt) ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
    vkCmdBindIndexBuffer(cmdBuffer_, b->GetNativeBuffer(), offset, idxType);
}

void VulkanCommandBuffer::BindDescriptorSets(PipelineBindPoint bindPoint, PipelineLayoutHandle /*layout*/,
                                             u32 firstSet, u32 setCount,
                                             const DescriptorSetHandle* descriptorSets,
                                             u32 dynamicOffsetCount, const u32* dynamicOffsets) {
    if (!descriptorSets || setCount == 0) return;
    VulkanDevice& vk = static_cast<VulkanDevice&>(device_);
    std::vector<VkDescriptorSet> sets(setCount);
    for (u32 i = 0; i < setCount; ++i) {
        VulkanDescriptorSet* ds = vk.GetDescriptorSet(descriptorSets[i]);
        sets[i] = ds ? ds->GetNativeSet() : VK_NULL_HANDLE;
    }
    VkPipelineBindPoint pbp = (bindPoint == PipelineBindPoint::Compute)
                              ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS;
    vkCmdBindDescriptorSets(cmdBuffer_, pbp, boundPipelineLayout_,
                            firstSet, setCount, sets.data(),
                            dynamicOffsetCount, dynamicOffsets);
}

void VulkanCommandBuffer::Draw(u32 vertexCount, u32 startVertex, u32 instanceCount, u32 startInstance) {
    if (scope_ != Scope::RenderPass) return;
    vkCmdDraw(cmdBuffer_, vertexCount, instanceCount, startVertex, startInstance);
    UpdateStats(CommandType::Draw);
}

void VulkanCommandBuffer::DrawIndexed(u32 indexCount, u32 startIndex, u32 baseVertex,
                                      u32 instanceCount, u32 startInstance) {
    if (scope_ != Scope::RenderPass) return;
    vkCmdDrawIndexed(cmdBuffer_, indexCount, instanceCount, startIndex, baseVertex, startInstance);
    UpdateStats(CommandType::DrawIndexed);
}

void VulkanCommandBuffer::DrawIndirect(ResourceHandle buffer, u64 offset, u32 drawCount) {
    VulkanDevice& vk = static_cast<VulkanDevice&>(device_);
    VulkanBuffer* b = vk.GetBuffer(buffer);
    if (!b) return;
    vkCmdDrawIndirect(cmdBuffer_, b->GetNativeBuffer(), offset, drawCount, sizeof(VkDrawIndirectCommand));
}

void VulkanCommandBuffer::BindComputePipeline(PipelineHandle pipeline) {
    VulkanDevice& vk = static_cast<VulkanDevice&>(device_);
    VulkanPipeline* p = vk.GetPipeline(pipeline);
    if (!p || !p->IsCompute()) return;
    vkCmdBindPipeline(cmdBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE, p->GetNativePipeline());
    boundPipelineLayout_ = p->GetNativePipelineLayout();
    scope_ = Scope::Compute;
}

void VulkanCommandBuffer::PushConstants(PipelineLayoutHandle layout, ShaderStage stageFlags,
                                        u32 offset, u32 size, const void* pValues) {
    VulkanDevice& vk = static_cast<VulkanDevice&>(device_);
    VkPipelineLayout pl = VK_NULL_HANDLE;
    if (layout != handles::INVALID_PIPELINE_LAYOUT) {
        VulkanPipelineLayout* l = vk.GetPipelineLayout(layout);
        pl = l ? l->GetNativeLayout() : VK_NULL_HANDLE;
    }
    if (pl == VK_NULL_HANDLE) pl = boundPipelineLayout_;
    if (pl == VK_NULL_HANDLE || !pValues || size == 0) return;

    VkShaderStageFlags stage = 0;
    const u32 v = static_cast<u32>(stageFlags);
    if (v & static_cast<u32>(ShaderStage::Vertex))   stage |= VK_SHADER_STAGE_VERTEX_BIT;
    if (v & static_cast<u32>(ShaderStage::Pixel))    stage |= VK_SHADER_STAGE_FRAGMENT_BIT;
    if (v & static_cast<u32>(ShaderStage::Geometry)) stage |= VK_SHADER_STAGE_GEOMETRY_BIT;
    if (v & static_cast<u32>(ShaderStage::Compute))  stage |= VK_SHADER_STAGE_COMPUTE_BIT;

    vkCmdPushConstants(cmdBuffer_, pl, stage, offset, size, pValues);
}

void VulkanCommandBuffer::SetComputeBytes(u32 index, const void* data, u32 size) {
    // Metal-ism:setBytes(index, bytes, size) 在 Vulkan 没有直接对应。
    // 简化映射:当成 push constant 写到 offset = index*16 字节,stage = Compute。
    // (caller 端的 push constant block 通常以 16B 对齐分槽)
    // 局限:size + offset 不能超 VkPhysicalDeviceLimits::maxPushConstantsSize(通常 128B);
    // 超出走 staging UBO 是 Phase 6 工作。
    if (!data || size == 0) return;
    if (boundPipelineLayout_ == VK_NULL_HANDLE) return;
    VkDeviceSize offset = static_cast<VkDeviceSize>(index) * 16;
    vkCmdPushConstants(cmdBuffer_, boundPipelineLayout_,
                       VK_SHADER_STAGE_COMPUTE_BIT,
                       static_cast<u32>(offset), size, data);
}

void VulkanCommandBuffer::Dispatch(u32 groupCountX, u32 groupCountY, u32 groupCountZ) {
    if (scope_ != Scope::Compute) return;
    vkCmdDispatch(cmdBuffer_, groupCountX, groupCountY, groupCountZ);
    UpdateStats(CommandType::Dispatch);
}

void VulkanCommandBuffer::DispatchIndirect(ResourceHandle buffer, u64 offset) {
    VulkanDevice& vk = static_cast<VulkanDevice&>(device_);
    VulkanBuffer* b = vk.GetBuffer(buffer);
    if (!b) return;
    vkCmdDispatchIndirect(cmdBuffer_, b->GetNativeBuffer(), offset);
}

void VulkanCommandBuffer::WriteTimestamp(QueryPoolHandle queryPool, u32 queryIndex) {
    VulkanDevice& vk = static_cast<VulkanDevice&>(device_);
    VulkanQueryPool* qp = vk.GetQueryPool(queryPool);
    if (!qp) return;
    vkCmdWriteTimestamp(cmdBuffer_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                        qp->GetNativePool(), queryIndex);
}

} // namespace primal::graphics::rhi

#endif // ENABLE_VULKAN
