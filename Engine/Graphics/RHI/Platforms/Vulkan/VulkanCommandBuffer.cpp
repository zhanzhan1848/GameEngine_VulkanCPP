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
#include "Engine/Common/CoreLogStream.h"
#include "VulkanDDGIAuditPrefix.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

#include <cstring>
#include <iostream>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <unordered_set>

namespace primal::graphics::rhi {

static const char* firstPrimaryRasterAuditPrefix() {
    const char* enabled = std::getenv("DDGI_FIRST_PRIMARY_RASTER");
    const char* dump = std::getenv("DDGI_DUMP");
    const char* prefix = std::getenv("DDGI_FIRST_RECORD_PROOF_PREFIX");
    return enabled && std::strcmp(enabled, "1") == 0 &&
           dump && std::strcmp(dump, "2") == 0 && prefix && *prefix ? prefix : nullptr;
}

VulkanCommandBuffer::VulkanCommandBuffer(VulkanDevice& device, CommandQueueType type)
    : RHICommandBuffer(device, type) {
}

VulkanCommandBuffer::VulkanCommandBuffer(VulkanCommandBuffer&& other) noexcept
    : RHICommandBuffer(std::move(other)),
      cmdPool_(other.cmdPool_),
      cmdBuffer_(other.cmdBuffer_),
      submitFence_(other.submitFence_),
      queueFamily_(other.queueFamily_),
      scope_(other.scope_),
      rasterAuditPipeline_(other.rasterAuditPipeline_),
      rasterAuditSerial_(other.rasterAuditSerial_) {
    other.cmdPool_ = VK_NULL_HANDLE;
    other.cmdBuffer_ = VK_NULL_HANDLE;
    other.submitFence_ = VK_NULL_HANDLE;
    other.queueFamily_ = UINT32_MAX;
    other.scope_ = Scope::None;
    other.rasterAuditPipeline_ = VK_NULL_HANDLE;
    other.rasterAuditSerial_ = 0;
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
        rasterAuditPipeline_ = other.rasterAuditPipeline_;
        rasterAuditSerial_ = other.rasterAuditSerial_;
        other.cmdPool_ = VK_NULL_HANDLE;
        other.cmdBuffer_ = VK_NULL_HANDLE;
        other.submitFence_ = VK_NULL_HANDLE;
        other.queueFamily_ = UINT32_MAX;
        other.scope_ = Scope::None;
        other.rasterAuditPipeline_ = VK_NULL_HANDLE;
        other.rasterAuditSerial_ = 0;
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

    // T4.6.5 part 24.11 (B-leak fix): make Initialize idempotent. Metal's
    // Initialize short-circuits via state_ check; Vulkan must too — otherwise
    // StandardRenderPipeline::Render()'s `cmd->Initialize()` call (after
    // CreateCommandBuffer already invoked Initialize once) creates a 2nd set
    // of VkCommandPool/VkCommandBuffer/VkFence, leaking the 1st set
    // (3 objects per Render() call).
    if (cmdPool_ != VK_NULL_HANDLE && cmdBuffer_ != VK_NULL_HANDLE &&
        (submitFence_ != VK_NULL_HANDLE || isSecondary_)) {
        return true;
    }

    // Phase 1-3 都用 graphics queue family
    queueFamily_ = vk.GetGraphicsQueueFamily();
    if (queueFamily_ == UINT32_MAX) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanCommandBuffer] No graphics queue family" << std::endl;
        return false;
    }

    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;  // 允许 vkResetCommandBuffer
    pci.queueFamilyIndex = queueFamily_;
    if (vkCreateCommandPool(dev, &pci, nullptr, &cmdPool_) != VK_SUCCESS) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanCommandBuffer] vkCreateCommandPool failed" << std::endl;
        return false;
    }

    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = cmdPool_;
    // P4c-F7: secondary 级分配(RENDER_PASS_CONTINUE 继承)
    ai.level = isSecondary_ ? VK_COMMAND_BUFFER_LEVEL_SECONDARY
                            : VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(dev, &ai, &cmdBuffer_) != VK_SUCCESS) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanCommandBuffer] vkAllocateCommandBuffers failed" << std::endl;
        vkDestroyCommandPool(dev, cmdPool_, nullptr);
        cmdPool_ = VK_NULL_HANDLE;
        return false;
    }

    // P4c-F7: secondary 不拥有 fence(同步由执行它的 primary 承担)
    if (isSecondary_) {
        state_ = CommandBufferState::Reset;
        return true;
    }

    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = 0;
    if (vkCreateFence(dev, &fci, nullptr, &submitFence_) != VK_SUCCESS) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanCommandBuffer] vkCreateFence failed" << std::endl;
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

    // P4c-F4: recording 中被 Reset 视为放弃录制,回退计数
    if (isRecording_) {
        isRecording_ = false;
        static_cast<VulkanDevice&>(device_).DecrementRecording();
    }

    // Reset command buffer (commands cleared, ready to begin)
    VkResult res = vkResetCommandBuffer(cmdBuffer_, VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT);
    if (res != VK_SUCCESS) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanCommandBuffer] vkResetCommandBuffer failed: " << res << std::endl;
        return false;
    }

    // Reset fence too (so next Submit can ping it)
    if (submitFence_ != VK_NULL_HANDLE) {
        vkResetFences(dev, 1, &submitFence_);
    }
    // T4.6.5 part 30.6 (X7 fix): clear external-fence flag — fresh submit.
    externalFenceSignaled_ = false;
    externalFence_ = VK_NULL_HANDLE;  // P0.1: 同步清空,避免等待已失效的旧句柄
    scope_ = Scope::None;
    pendingSecondaryMode_ = false;
    rasterAuditPipeline_ = VK_NULL_HANDLE;
    rasterAuditSerial_ = 0;
    return true;
}

bool VulkanCommandBuffer::beginImpl() {
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    bi.pInheritanceInfo = nullptr;

    // P4c-F7: secondary 带 InheritanceInfo(RENDER_PASS_CONTINUE 让驱动
    // 在 pass 内重放时省去状态恢复;renderpass 须与 primary 的兼容)
    VkCommandBufferInheritanceInfo inheritanceInfo{};
    if (isSecondary_ && inheritRenderPass_ != VK_NULL_HANDLE) {
        inheritanceInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
        inheritanceInfo.renderPass = inheritRenderPass_;
        inheritanceInfo.subpass = inheritSubpass_;
        inheritanceInfo.framebuffer = VK_NULL_HANDLE;
        inheritanceInfo.occlusionQueryEnable = VK_FALSE;
        bi.flags |= VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
        bi.pInheritanceInfo = &inheritanceInfo;
    }
    VkResult res = vkBeginCommandBuffer(cmdBuffer_, &bi);
    if (res != VK_SUCCESS) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanCommandBuffer] vkBeginCommandBuffer failed: " << res << std::endl;
        return false;
    }
    // P4c-F7: secondary 的录制期整体处于"继承的 render pass 内"(其执行
    // 被强制发生在 primary 的 pass 实例中)— 置 RenderPass scope 让
    // Draw/SetViewport 等命令的 scope 守卫放行(secondary 不能显式
    // BeginRenderPass,否则永远无法录绘制命令)。
    scope_ = isSecondary_ ? Scope::RenderPass : Scope::None;
    isRecording_ = true;
    static_cast<VulkanDevice&>(device_).IncrementRecording();
    if (!isSecondary_) {
        // P4c-F4 接线:帧首把 staging 队列的 pending blits 编码进本 cmdbuf
        // (用户 pass 之前 — 对齐 Metal "单 blit encoder 前置" 语义)。
        // 帧内 Queue 的上传由此落 GPU,消除逐次 fence 等待。
        static_cast<VulkanDevice&>(device_).GetStagingAllocator().EncodePendingBlits(cmdBuffer_);
    }
    return true;
}

bool VulkanCommandBuffer::endImpl() {
    VkResult res = vkEndCommandBuffer(cmdBuffer_);
    if (res != VK_SUCCESS) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanCommandBuffer] vkEndCommandBuffer failed: " << res << std::endl;
        return false;
    }
    if (isRecording_) {
        isRecording_ = false;
        static_cast<VulkanDevice&>(device_).DecrementRecording();
    }
    return true;
}

bool VulkanCommandBuffer::submitImpl(u32 /*waitFlags*/) {
    VulkanDevice& vk = static_cast<VulkanDevice&>(device_);
    VkQueue queue = vk.GetGraphicsQueue();
    if (!queue || !cmdBuffer_) return false;

    // P4c-F7 契约:secondary 不拥有 fence,不能独立提交 — 由 primary 的
    // ExecuteSecondaryCommandBuffers + 提交统一执行
    if (isSecondary_) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanCommandBuffer] Submit rejected on secondary command "
                     "buffer (execute via primary)" << std::endl;
        return false;
    }

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
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanCommandBuffer] vkQueueSubmit failed: " << res << std::endl;
        return false;
    }
    return true;
}

bool VulkanCommandBuffer::waitForCompletionImpl() {
    // T4.6.5 part 30.6 (X7 fix): if the last device->Submit used an external
    // signalFence, this cmd buffer's internal submitFence_ was NOT signaled
    // by that submit. Caller's WaitForSync(external_fence) is authoritative;
    // waiting on our unused internal fence would always 5s-timeout.
    //
    // P0.1: 但 destroy 路径把这里当兜底同步点(逐帧 destroy 设计依赖它)。
    // externalFence_ 是上次提交真正被 signal 的 fence——等它:调用方已等过
    // 则立即返回,没等过则在此补齐,防止帧飞行中的 set 被 update(03047)。
    if (externalFenceSignaled_) {
        if (externalFence_ == VK_NULL_HANDLE) return true;
        VkDevice extDev = static_cast<VulkanDevice&>(device_).GetNativeDevice();
        VkResult extRes = vkWaitForFences(extDev, 1, &externalFence_, VK_TRUE, 5ULL * 1000ULL * 1000ULL * 1000ULL);
        if (extRes == VK_TIMEOUT) {
            PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanCommandBuffer] External fence WaitForCompletion TIMEOUT (5s)" << std::endl;
        }
        return extRes == VK_SUCCESS;
    }

    VkDevice dev = static_cast<VulkanDevice&>(device_).GetNativeDevice();
    if (submitFence_ == VK_NULL_HANDLE) return false;
    VkResult res = vkWaitForFences(dev, 1, &submitFence_, VK_TRUE, 5ULL * 1000ULL * 1000ULL * 1000ULL);
    if (res == VK_SUCCESS) return true;
    if (res == VK_TIMEOUT) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanCommandBuffer] WaitForCompletion TIMEOUT (5s)" << std::endl;
        return false;
    }
    PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanCommandBuffer] vkWaitForFences failed: " << res << std::endl;
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
    // P0-B（validation 预热瞬态收尾）: 拷贝完成后把源纹理收敛回
    // SHADER_READ_ONLY—— sampled 用例（历史环/调试读回）远多于继续 TRANSFER
    // 的用例；updateData 的 backLayout 惯例与此一致。truth-based，已 SRO 时
    // 后续 barrier 自然 no-op。
    TransitionImageLayout(tex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    UpdateStats(CommandType::CopyTextureToBuffer);
}

void VulkanCommandBuffer::BlitTexture(ResourceHandle src, ResourceHandle dst,
                                      const TextureBlitRegion* regions, u32 regionCount,
                                      FilterMode filter) {
    VulkanDevice& vk = static_cast<VulkanDevice&>(device_);
    VulkanTexture* srcTex = vk.GetTexture(src);
    VulkanTexture* dstTex = vk.GetTexture(dst);
    if (!srcTex || !dstTex || !srcTex->GetNativeImage() || !dstTex->GetNativeImage()) return;

    // P0-B 定位（PRIMAL_RG_LAYOUT_TRACE=1）: blit 布局责任追踪
    if (std::getenv("PRIMAL_RG_LAYOUT_TRACE")) {
        fprintf(stderr, "[BLIT] src=0x%llx (tracked %s) dst=0x%llx\n",
                (unsigned long long)(uintptr_t)srcTex->GetNativeImage(),
                [](VkImageLayout l) { return l == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ? "SRO" : "OTHER"; }(srcTex->GetCurrentLayout()),
                (unsigned long long)(uintptr_t)dstTex->GetNativeImage());
    }
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
    // P0-B: blit 完成后把两端收敛回 SHADER_READ_ONLY（与上面 CopyTextureTo
    // Buffer 的收尾同理）。调用方既有的回置 barrier（TAA histBarrier、
    // ColorHistory restore 等）由 InsertBarrier 的同布局 skip 变为 no-op。
    TransitionImageLayout(srcTex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    TransitionImageLayout(dstTex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    UpdateStats(CommandType::BlitTexture);
}

namespace vulkan {
namespace {
bool AlbedoCommandReady(VulkanDevice& device,VulkanCommandBuffer& cmd) {
    if(!cmd.GetNativeCmdBuffer()||!cmd.IsRecordingOutsideRenderPass())return false;
    u32 count=0;vkGetPhysicalDeviceQueueFamilyProperties(device.GetNativePhysicalDevice(),&count,nullptr);
    const u32 family=cmd.GetNativeQueueFamilyIndex();if(family>=count)return false;
    std::vector<VkQueueFamilyProperties> properties(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device.GetNativePhysicalDevice(),&count,properties.data());
    return (properties[family].queueFlags&VK_QUEUE_GRAPHICS_BIT)!=0;
}
bool AlbedoLayoutKnown(VkImageLayout l) {
    return l==VK_IMAGE_LAYOUT_UNDEFINED||l==VK_IMAGE_LAYOUT_PREINITIALIZED||
        l==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL||l==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL||
        l==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL||l==VK_IMAGE_LAYOUT_GENERAL||
        l==VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
}
void AlbedoMipBarrier(VulkanCommandBuffer& cmd,VulkanTexture& tex,u32 mip,VkImageLayout next) {
    const VkImageLayout old=tex.GetMipLayout(mip);if(old==next)return;
    VkImageMemoryBarrier b{};b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout=old;b.newLayout=next;b.image=tex.GetNativeImage();
    b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,mip,1,0,std::max(1u,tex.GetTextureDesc().arraySize)};
    VkPipelineStageFlags source=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,dest=VK_PIPELINE_STAGE_TRANSFER_BIT;
    switch(old) {
        case VK_IMAGE_LAYOUT_UNDEFINED:source=VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;b.srcAccessMask=0;break;
        case VK_IMAGE_LAYOUT_PREINITIALIZED:source=VK_PIPELINE_STAGE_HOST_BIT;b.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;break;
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:source=VK_PIPELINE_STAGE_TRANSFER_BIT;b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;break;
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:source=VK_PIPELINE_STAGE_TRANSFER_BIT;b.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;break;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:b.srcAccessMask=VK_ACCESS_SHADER_READ_BIT;break;
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:source=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;b.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;break;
        default:b.srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;break;
    }
    if(next==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)b.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    else if(next==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)b.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    else {dest=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;}
    vkCmdPipelineBarrier(cmd.GetNativeCmdBuffer(),source,dest,0,0,nullptr,0,nullptr,1,&b);
    // All layers of this mip now agree. Never use SetCurrentLayout here:
    // that would overwrite tracking for the other (possibly undefined) mips.
    tex.SetMipLayout(mip,next);
}
}

bool CanGenerateAlbedoMipChain(VulkanDevice& device,VulkanCommandBuffer& cmd,const TextureDesc& d) {
    if(!AlbedoCommandReady(device,cmd)||d.size.x==0||d.size.y==0||d.size.z!=1||
       (d.type!=TextureType::Texture2D&&d.type!=TextureType::Texture2DArray)||d.arraySize==0)return false;
    if(d.format!=DataFormat::RGBA8_UNorm&&d.format!=DataFormat::RGBA8_sRGB&&d.format!=DataFormat::BGRA8_UNorm)return false;
    const auto usage=ToVkImageUsageFlags(d.usage);
    if((usage&(VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT))!=
       (VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT))return false;
    u32 full=1;for(u32 n=std::max(d.size.x,d.size.y);n>1;n>>=1)++full;
    if(d.mipLevels==0||d.mipLevels>full)return false;
    VkFormatProperties format{};vkGetPhysicalDeviceFormatProperties(device.GetNativePhysicalDevice(),ToVkFormat(d.format),&format);
    const VkFormatFeatureFlags required=VK_FORMAT_FEATURE_BLIT_SRC_BIT|VK_FORMAT_FEATURE_BLIT_DST_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if((format.optimalTilingFeatures&required)!=required)return false;
    VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(device.GetNativePhysicalDevice(),&properties);
    return d.size.x<=properties.limits.maxImageDimension2D&&d.size.y<=properties.limits.maxImageDimension2D&&d.arraySize<=properties.limits.maxImageArrayLayers;
}

bool CopyAlbedoMip0Layer(VulkanDevice& device,VulkanCommandBuffer& cmd,ResourceHandle src,ResourceHandle dst,u32 layer,bool expandSingleTexel) {
    auto* s=device.GetTexture(src);auto* d=device.GetTexture(dst);
    if(src==dst||!s||!d||!s->OwnsImage()||!d->OwnsImage()||!s->GetNativeImage()||!d->GetNativeImage())return false;
    const auto& sd=s->GetTextureDesc();const auto& dd=d->GetTextureDesc();
    if(!CanGenerateAlbedoMipChain(device,cmd,dd)||sd.type!=TextureType::Texture2D||sd.size.z!=1||sd.format!=dd.format||layer>=dd.arraySize||
       s->GetMipLayout(0)==VK_IMAGE_LAYOUT_UNDEFINED||!AlbedoLayoutKnown(s->GetMipLayout(0))||!AlbedoLayoutKnown(d->GetMipLayout(0)))return false;
    if((ToVkImageUsageFlags(sd.usage)&VK_IMAGE_USAGE_TRANSFER_SRC_BIT)==0)return false;
    const bool same=sd.size.x==dd.size.x&&sd.size.y==dd.size.y;
    if(!same&&!(expandSingleTexel&&sd.size.x==1&&sd.size.y==1))return false;
    AlbedoMipBarrier(cmd,*s,0,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    AlbedoMipBarrier(cmd,*d,0,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    if(same) {
        VkImageCopy region{};region.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};region.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,layer,1};
        region.extent={dd.size.x,dd.size.y,1};
        vkCmdCopyImage(cmd.GetNativeCmdBuffer(),s->GetNativeImage(),VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,d->GetNativeImage(),VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);
    } else {
        // Only the explicit single-texel placeholder may expand. Real layers
        // never filter/convert mip0: they use the byte-exact image copy above.
        VkImageBlit region{};region.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};region.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,layer,1};
        region.srcOffsets[1]={1,1,1};region.dstOffsets[1]={s32(dd.size.x),s32(dd.size.y),1};
        vkCmdBlitImage(cmd.GetNativeCmdBuffer(),s->GetNativeImage(),VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,d->GetNativeImage(),VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region,VK_FILTER_NEAREST);
    }
    AlbedoMipBarrier(cmd,*s,0,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return true;
}

bool GenerateAlbedoMipChain(VulkanDevice& device,VulkanCommandBuffer& cmd,ResourceHandle texture) {
    auto* tex=device.GetTexture(texture);if(!tex||!tex->OwnsImage()||!tex->GetNativeImage())return false;
    const auto& d=tex->GetTextureDesc();if(!CanGenerateAlbedoMipChain(device,cmd,d)||tex->GetMipLayout(0)==VK_IMAGE_LAYOUT_UNDEFINED)return false;
    for(u32 mip=0;mip<d.mipLevels;++mip)if(!AlbedoLayoutKnown(tex->GetMipLayout(mip)))return false;
    // Caller has initialized every mip0 layer. Higher mip contents can be
    // undefined; only their actually tracked layouts are used for barriers.
    for(u32 mip=1;mip<d.mipLevels;++mip) {
        AlbedoMipBarrier(cmd,*tex,mip-1,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        AlbedoMipBarrier(cmd,*tex,mip,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageBlit blit{};blit.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,mip-1,0,d.arraySize};blit.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,mip,0,d.arraySize};
        blit.srcOffsets[1]={s32(std::max(1u,d.size.x>>(mip-1))),s32(std::max(1u,d.size.y>>(mip-1))),1};
        blit.dstOffsets[1]={s32(std::max(1u,d.size.x>>mip)),s32(std::max(1u,d.size.y>>mip)),1};
        // Vulkan performs sRGB decode before linear filtering and encode for
        // an sRGB destination. UNORM retains its declared linear semantics.
        vkCmdBlitImage(cmd.GetNativeCmdBuffer(),tex->GetNativeImage(),VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,tex->GetNativeImage(),VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,VK_FILTER_LINEAR);
    }
    for(u32 mip=0;mip<d.mipLevels;++mip)AlbedoMipBarrier(cmd,*tex,mip,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    tex->SetCurrentLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return true;
}
} // namespace vulkan

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

        vulkan::VkLayoutAccess dstLA = vulkan::ResourceStateToVkLayout(rb.afterState);

        // Determine oldLayout: for per-mip barriers (subresource != 0xFFFFFFFF),
        // use the per-mip layout from mipLayouts_. For full-resource barriers,
        // use the global GetCurrentLayout().
        VkImageLayout oldLayout;
        if (rb.subresource != 0xFFFFFFFF && tex->GetTextureDesc().mipLevels > 1) {
            oldLayout = tex->GetMipLayout(rb.subresource);
        } else {
            oldLayout = tex->GetCurrentLayout();
        }
        VkImageLayout newLayout = dstLA.layout;

        // P0-B diagnosis (PRIMAL_RG_LAYOUT_TRACE=1): one line per barrier with
        // the resource name, declared states, tracked oldLayout and the native
        // VkImage handle — correlates 1:1 with validation 01197 messages so a
        // tracked-vs-real desync can be attributed to the exact resource.
        static const bool s_layoutTrace = std::getenv("PRIMAL_RG_LAYOUT_TRACE") != nullptr;
        if (s_layoutTrace) {
            static const auto layoutName = [](VkImageLayout l) -> const char* {
                switch (l) {
                    case VK_IMAGE_LAYOUT_UNDEFINED: return "UNDEF";
                    case VK_IMAGE_LAYOUT_GENERAL: return "GENERAL";
                    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: return "SRO";
                    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return "XFER_DST";
                    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return "XFER_SRC";
                    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: return "COLOR";
                    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL: return "DS";
                    default: return "OTHER";
                }
            };
            PRIMAL_LOG_STREAM(Info, "RGTRACE")
                << "[RGTRACE] tex='" << tex->GetTextureDesc().name
                << "' img=0x" << std::hex << reinterpret_cast<uintptr_t>(tex->GetNativeImage()) << std::dec
                << " declared=" << layoutName(vulkan::ResourceStateToVkLayout(rb.beforeState).layout)
                << "->" << layoutName(newLayout)
                << " tracked_old=" << layoutName(oldLayout)
                << (oldLayout == newLayout ? " SKIP" : " EMIT")
                << std::endl;
        }

        // If old == new, skip this barrier (no-op, avoids validation warning).
        if (oldLayout == newLayout) {
            if (rb.subresource != 0xFFFFFFFF && tex->GetTextureDesc().mipLevels > 1) {
                tex->SetMipLayout(rb.subresource, newLayout);
            } else {
                tex->SetCurrentLayout(newLayout);
            }
            continue;
        }

        // Derive src access mask from the actual old layout for correct
        // synchronization. Map VkImageLayout → access mask + stage directly.
        VkAccessFlags srcAccess = 0;
        VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        switch (oldLayout) {
            case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
                srcAccess = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                srcStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
                break;
            case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
                srcAccess = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                srcStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
                break;
            case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
                srcAccess = VK_ACCESS_SHADER_READ_BIT;
                srcStage = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
                break;
            case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
                srcAccess = VK_ACCESS_TRANSFER_READ_BIT;
                srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
                break;
            case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
                srcAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
                srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
                break;
            case VK_IMAGE_LAYOUT_GENERAL:
                srcAccess = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                srcStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
                break;
            case VK_IMAGE_LAYOUT_UNDEFINED:
                srcAccess = 0;
                srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
                break;
            default:
                srcAccess = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
                srcStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
                break;
        }

        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = tex->GetNativeImage();
        b.oldLayout = oldLayout;
        b.newLayout = newLayout;
        b.srcAccessMask = srcAccess;
        b.dstAccessMask = dstLA.access;
        b.subresourceRange.aspectMask = tex->GetAspectMask();
        b.subresourceRange.baseMipLevel = (rb.subresource == 0xFFFFFFFF) ? 0 : rb.subresource;
        b.subresourceRange.levelCount   = (rb.subresource == 0xFFFFFFFF) ? VK_REMAINING_MIP_LEVELS : 1;
        b.subresourceRange.baseArrayLayer = 0;
        b.subresourceRange.layerCount   = VK_REMAINING_ARRAY_LAYERS;
        imgBarriers.push_back(b);

        // Update tracked layout: per-mip for specific mip, global for all-mip.
        if (rb.subresource != 0xFFFFFFFF && tex->GetTextureDesc().mipLevels > 1) {
            tex->SetMipLayout(rb.subresource, newLayout);
        } else {
            tex->SetCurrentLayout(newLayout);
        }
    }

    if (!imgBarriers.empty()) {
        vkCmdPipelineBarrier(cmdBuffer_,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
            0, nullptr, 0, nullptr,
            static_cast<u32>(imgBarriers.size()), imgBarriers.data());
    }
    UpdateStats(CommandType::Barrier);
}

// ============================================================================
// P4c-F7: Secondary CommandBuffer / 并行录制
// ============================================================================
CommandBufferHandle VulkanCommandBuffer::BeginSecondaryCommandBuffer(
    const SecondaryCommandBufferDesc& desc) {
    if (isSecondary_) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanCommandBuffer] secondary cannot nest another secondary" << std::endl;
        return handles::INVALID_COMMAND_BUFFER;
    }
    VulkanDevice& vk = static_cast<VulkanDevice&>(device_);

    VkRenderPass inheritRP = VK_NULL_HANDLE;
    if (desc.inheritRenderPass != handles::INVALID_RENDER_PASS) {
        VulkanRenderPass* rp = vk.GetRenderPass(desc.inheritRenderPass);
        if (rp) inheritRP = rp->GetNativeRenderPass();
    }

    // 经设备 allocator 建 VulkanCommandBuffer(secondary 标志先置,
    // Initialize 据此走 SECONDARY 分配 + 跳过 fence)
    u32 id = vk.commandBufferAllocator_->Allocate(vk, CommandQueueType::Graphics);
    VulkanCommandBuffer* sec = vk.commandBufferAllocator_->Get(id);
    if (!sec) return handles::INVALID_COMMAND_BUFFER;
    sec->isSecondary_ = true;
    sec->inheritRenderPass_ = inheritRP;
    sec->inheritSubpass_ = desc.subpass;
    pendingSecondaryMode_ = true;
    if (!sec->Initialize() || !sec->Begin()) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanCommandBuffer] secondary Initialize/Begin failed" << std::endl;
        vk.commandBufferAllocator_->Free(id);
        return handles::INVALID_COMMAND_BUFFER;
    }
    return static_cast<CommandBufferHandle>(id);
}

void VulkanCommandBuffer::ExecuteSecondaryCommandBuffers(u32 count,
                                                          CommandBufferHandle* secondaries) {
    if (isSecondary_ || count == 0 || !secondaries) return;
    if (scope_ != Scope::RenderPass) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanCommandBuffer] ExecuteSecondaryCommandBuffers must be "
                     "called inside a render pass instance" << std::endl;
        return;
    }
    VulkanDevice& vk = static_cast<VulkanDevice&>(device_);
    std::vector<VkCommandBuffer> buffers;
    buffers.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        VulkanCommandBuffer* sec = vk.GetCommandBuffer(static_cast<CommandBufferHandle>(secondaries[i]));
        if (sec && sec->isSecondary_ && sec->cmdBuffer_ != VK_NULL_HANDLE) {
            buffers.push_back(sec->cmdBuffer_);
        }
    }
    if (!buffers.empty()) {
        vkCmdExecuteCommands(cmdBuffer_, static_cast<u32>(buffers.size()), buffers.data());
    }
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

    // T4.6.5 part 22.1: stage masks must match the access masks above, or
    // validation rejects the barrier (VUID-vkCmdPipelineBarrier-pImageMemoryBarriers-02819).
    // The old code hardcoded TRANSFER_BIT for everything except UNDEFINED; that
    // broke SHADER_READ_ONLY_OPTIMAL src (access=SHADER_READ, stage=TRANSFER)
    // — sync2 reports it as ALL_TRANSFER + SHADER_READ mismatch. Stage must
    // reflect the LAST operation that wrote/read the texture in oldLayout.
    VkPipelineStageFlags srcStage, dstStage;
    switch (oldLayout) {
        case VK_IMAGE_LAYOUT_UNDEFINED:
            srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            break;
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            break;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            // Could be fragment or compute read; ALL_GRAPHICS_BIT doesn't
            // include compute. Use ALL_COMMANDS_BIT for the conservative case
            // — validation accepts ALL_COMMANDS paired with any access mask.
            srcStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            break;
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            srcStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            break;
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
            srcStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
            break;
        case VK_IMAGE_LAYOUT_GENERAL:
            // UA/storage image — typically compute, but could be any shader.
            srcStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            break;
        default:
            srcStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            break;
    }
    switch (newLayout) {
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            break;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            break;
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            dstStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            break;
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
            dstStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
            break;
        case VK_IMAGE_LAYOUT_GENERAL:
            dstStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            break;
        default:
            dstStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            break;
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

    // P4c-F7 契约:secondary 内 BeginRenderPass 被拒绝(继承式二级缓冲,
    // 非嵌套 pass;见 RHICommand.h 注释)
    if (isSecondary_) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanCommandBuffer] BeginRenderPass rejected on secondary "
                     "command buffer (inherited render pass)" << std::endl;
        return;
    }

    // 通过 desc 临时构建 VkRenderPass + VkFramebuffer
    // (Phase 5+ 改为 cache by hash 或 dynamic rendering)
    std::vector<VkAttachmentDescription> attachments;
    std::vector<VkAttachmentReference> colorRefs;
    std::vector<VkClearValue> clears;
    std::vector<VkImageView> fbAttachments;
    pendingAttachments_.clear();

    const u32 colorCount = static_cast<u32>(desc.colorAttachments.size());
    // P4c-F5: layered 渲染(renderTargetArrayLength > 1)时 attachment 改绑
    // 2D_ARRAY 全层视图 + framebuffer layers 对齐;shader 内 gl_Layer 选层。
    // Metal 等价物:setRenderTargetArrayLength(desc.renderTargetArrayLength)。
    const bool layered = desc.renderTargetArrayLength > 1;
    colorRefs.reserve(colorCount);
    for (u32 i = 0; i < colorCount; ++i) {
        const auto& a = desc.colorAttachments[i];
        VulkanTexture* tex = vk.GetTexture(a.texture);
        if (!tex) {
            // 物理纹理缺失：跳过附件但 subpass.colorAttachmentCount 仍按
            // colorCount 声明，refs 数不匹配会构造畸形 VkRenderPass——
            // validation 层 AttachmentTracker 崩溃 / 无验证时渲染未定义
            // （ToneMapping 瞬态纹理句柄无效黑屏，2026-09-02 排查记录）。
            static bool s_attachSkipLogged = false;
            if (!s_attachSkipLogged) {
                s_attachSkipLogged = true;
                std::cerr << "[VK] BeginRenderPass: color attachment " << i
                          << " physical texture missing (handle=" << a.texture << ")" << std::endl;
            }
            continue;
        }

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
        pendingAttachments_.push_back({tex, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL});

        VkClearValue cv{};
        cv.color.float32[0] = a.clearValue.color.x;
        cv.color.float32[1] = a.clearValue.color.y;
        cv.color.float32[2] = a.clearValue.color.z;
        cv.color.float32[3] = a.clearValue.color.w;
        clears.push_back(cv);
        // Per-layer view: array textures pick the slice; single-layer textures fall through to vkView_.
        // P4c-F5: layered 模式改绑全层 2D_ARRAY 视图。
        fbAttachments.push_back(layered ? tex->GetArrayView() : tex->GetLayerView(a.arrayLayer));
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
            pendingAttachments_.push_back({tex, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL});

            VkClearValue cv{};
            cv.depthStencil.depth = a.clearValue.depth;
            cv.depthStencil.stencil = a.clearValue.stencil;
            clears.push_back(cv);
            // P4c-F5: depth attachment 同样走 array 视图(layered CSM 深度)。
            fbAttachments.push_back(layered ? tex->GetArrayView() : tex->GetLayerView(a.arrayLayer));
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
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanCommandBuffer] BeginRenderPass vkCreateRenderPass failed" << std::endl;
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
    // P4c-F5: layered 模式 framebuffer layers = renderTargetArrayLength
    // (clamp 到 attachment 实际 layer 数 — 超出是调用方 bug,不静默)。
    fbci.layers = 1;
    if (layered) {
        u32 maxLayers = 1;
        for (const auto viewTex : {vk.GetTexture(desc.colorAttachments.empty() ? handles::INVALID_RESOURCE : desc.colorAttachments[0].texture),
                                    vk.GetTexture(desc.depthAttachment.texture)}) {
            if (viewTex) {
                maxLayers = std::max<u32>(maxLayers, viewTex->GetTextureDesc().arraySize);
            }
        }
        if (desc.renderTargetArrayLength > maxLayers) {
            PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanCommandBuffer] renderTargetArrayLength "
                      << desc.renderTargetArrayLength << " > attachment layers "
                      << maxLayers << " — clamping" << std::endl;
        }
        fbci.layers = std::min(desc.renderTargetArrayLength, maxLayers);
    }
    VkFramebuffer fb;
    if (vkCreateFramebuffer(dev, &fbci, nullptr, &fb) != VK_SUCCESS) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanCommandBuffer] vkCreateFramebuffer failed" << std::endl;
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

    // P4c-F7: 本 cmdbuf 创建过 secondary(BeginSecondaryCommandBuffer)时,
    // pass 以 SECONDARY_COMMAND_BUFFERS 模式开始 — Vulkan 每个 subpass 只
    // 能选一种模式(inline 或 secondary),契约:创建过 secondary 的 pass
    // 内只能 ExecuteSecondaryCommandBuffers,直接 draw 会被 validation 拒绝。
    const VkSubpassContents contents = pendingSecondaryMode_
        ? VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS
        : VK_SUBPASS_CONTENTS_INLINE;
    vkCmdBeginRenderPass(cmdBuffer_, &bi, contents);
    scope_ = Scope::RenderPass;

    // 暂存 rp/fb 以便 EndRenderPass 销毁
    pendingRenderPass_ = rp;
    pendingFramebuffer_ = fb;

    // 自动设置 viewport/scissor(pipeline 用 dynamic state)
    // Apply the same fallback as renderArea: if caller left desc.viewport at {0,0},
    // use the framebuffer dimensions we derived.
    // P4c-F7: SECONDARY 模式的 pass 内 primary 不能录任何内联命令(含
    // viewport/scissor)— 状态由各 secondary 自行设置(secondary 的
    // BeginRenderPass 被拒,viewport/scissor 在其 Execute 前由 secondary 的
    // SetViewport/SetScissor 提供)。
    if (!pendingSecondaryMode_) {
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
    }
    UpdateStats(CommandType::BeginRenderPass);
}

void VulkanCommandBuffer::BeginRenderPass(RenderPassHandle renderPass) {
    VulkanDevice& vk = static_cast<VulkanDevice&>(device_);
    VulkanRenderPass* rp = vk.GetRenderPass(renderPass);
    if (!rp) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanCommandBuffer] BeginRenderPass(handle): invalid handle" << std::endl;
        return;
    }
    // Phase 4 简化:RenderPassHandle 重载用 desc 版本走一遍
    BeginRenderPass(rp->GetDesc());
}

void VulkanCommandBuffer::EndRenderPass() {
    if (scope_ != Scope::RenderPass) return;
    vkCmdEndRenderPass(cmdBuffer_);
    scope_ = Scope::None;
    // P4c-F7: pass 结束回到 inline 模式(除非又创建了新 secondary)
    pendingSecondaryMode_ = false;

    // Update tracked layouts: the Vulkan render pass performed implicit
    // layout transitions to finalLayout for each attachment. The texture's
    // currentLayout_ must reflect this so subsequent InsertBarrier calls
    // use the correct oldLayout.
    for (const auto& att : pendingAttachments_) {
        if (att.tex) att.tex->SetCurrentLayout(att.finalLayout);
    }
    pendingAttachments_.clear();

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
    if (const char* prefix = firstPrimaryRasterAuditPrefix()) {
        std::ofstream trace(std::string(prefix) + ".raster.native-viewports.jsonl", std::ios::app);
        trace << "{\"native_command_buffer\":" << (u64)(uintptr_t)cmdBuffer_
              << ",\"viewport\":[" << vp.x << ',' << vp.y << ',' << vp.width
              << ',' << vp.height << ',' << vp.minDepth << ',' << vp.maxDepth << "]}\n";
        trace.close();
        if (!trace) PRIMAL_LOG_STREAM(Error, "RHI") << "[FirstPrimaryRasterAudit] viewport write failed" << std::endl;
    }
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
    // First real native binding of each audited creation serial. Creation state
    // alone is not evidence that this pipeline was used by a command buffer.
    if (const char* prefix = std::getenv("VK_NATIVE_PIPELINE_AUDIT"); prefix && *prefix && p->GetNativeAuditSerial()) {
        static std::mutex auditMutex;
        static std::unordered_set<u64> recorded;
        std::lock_guard<std::mutex> lock(auditMutex);
        const u64 serial=p->GetNativeAuditSerial();
        if (!recorded.count(serial)) {
            std::ofstream trace(std::string(prefix) + ".bindings.jsonl", std::ios::app);
            trace << "{\"serial\":" << serial << ",\"pipeline_handle\":" << pipeline
                  << ",\"native_pipeline\":" << (u64)(uintptr_t)p->GetNativePipeline()
                  << ",\"native_layout\":" << (u64)(uintptr_t)p->GetNativePipelineLayout()
                  << ",\"native_command_buffer\":" << (u64)(uintptr_t)cmdBuffer_
                  << ",\"bind_point\":0,\"scope_renderpass\":" << (scope_==Scope::RenderPass ? "true" : "false") << "}\n";
            trace.close();
            if (trace) recorded.insert(serial);
            else PRIMAL_LOG_STREAM(Error, "RHI") << "[NativePipelineAudit] binding evidence write failed: " << serial << std::endl;
        }
    }
    boundPipelineLayout_ = p->GetNativePipelineLayout();
    if (firstPrimaryRasterAuditPrefix()) {
        rasterAuditPipeline_ = p->GetNativePipeline();
        rasterAuditSerial_ = p->GetNativeAuditSerial();
    }
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

// P0.1 诊断:PRIMAL_DS_TRACE=1 时启用 set 更新/绑定追踪(03047 定罪用)。
static bool dsTraceBind() {
    static const bool on = [] {
        const char* e = std::getenv("PRIMAL_DS_TRACE");
        return e && e[0] == '1';
    }();
    return on;
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
        // P0.1 诊断:PRIMAL_DS_TRACE=1 时打印绑定(与 [DS-UPD] 对照)。
        if (ds && dsTraceBind()) {
            fprintf(stderr, "[DS-BIND] set=%p firstSet=%u\n", (void*)sets[i], firstSet + i);
        }
    }
    VkPipelineBindPoint pbp = (bindPoint == PipelineBindPoint::Compute)
                              ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS;
    vkCmdBindDescriptorSets(cmdBuffer_, pbp, boundPipelineLayout_,
                            firstSet, setCount, sets.data(),
                            dynamicOffsetCount, dynamicOffsets);
    if (const char* prefix = firstPrimaryRasterAuditPrefix();
        prefix && bindPoint == PipelineBindPoint::Graphics) {
        std::ofstream trace(std::string(prefix) + ".raster.native-descriptors.jsonl", std::ios::app);
        for (u32 i = 0; i < setCount; ++i) {
            trace << "{\"native_command_buffer\":" << (u64)(uintptr_t)cmdBuffer_
                  << ",\"native_pipeline\":" << (u64)(uintptr_t)rasterAuditPipeline_
                  << ",\"serial\":" << rasterAuditSerial_
                  << ",\"native_layout\":" << (u64)(uintptr_t)boundPipelineLayout_
                  << ",\"native_set\":" << (u64)(uintptr_t)sets[i]
                  << ",\"set_handle\":" << descriptorSets[i]
                  << ",\"first_set\":" << firstSet+i << ",\"dynamic_offsets\":[";
            for (u32 j = 0; j < dynamicOffsetCount; ++j) {
                if (j) trace << ',';
                trace << dynamicOffsets[j];
            }
            trace << "]}\n";
        }
        trace.close();
        if (!trace) PRIMAL_LOG_STREAM(Error, "RHI") << "[FirstPrimaryRasterAudit] descriptor write failed" << std::endl;
    }
    if (const char* prefix = ddgiRecordedComputeAuditPrefix();
        prefix && *prefix &&
        (bindPoint == PipelineBindPoint::Compute ||
         (std::getenv("DDGI_FIRST_RECORD_PROOF_PREFIX") &&
          *std::getenv("DDGI_FIRST_RECORD_PROOF_PREFIX"))) &&
        std::getenv("DDGI_DUMP") && std::strcmp(std::getenv("DDGI_DUMP"), "2") == 0) {
        std::ofstream trace(std::string(prefix) + ".native-descriptor-bindings.jsonl", std::ios::app);
        for (u32 i = 0; i < setCount; ++i)
            trace << "{\"native_command_buffer\":" << (u64)(uintptr_t)cmdBuffer_
                  << ",\"native_layout\":" << (u64)(uintptr_t)boundPipelineLayout_
                  << ",\"set_handle\":" << descriptorSets[i]
                  << ",\"native_set\":" << (u64)(uintptr_t)sets[i]
                  << ",\"first_set\":" << firstSet+i << ",\"bind_point\":"
                  << (bindPoint == PipelineBindPoint::Compute ? 1 : 0) << "}\n";
        trace.close();
        if (!trace) PRIMAL_LOG_STREAM(Error,"RHI") << "[FirstRhoNativeAudit] descriptor binding evidence failed" << std::endl;
    }
}

void VulkanCommandBuffer::Draw(u32 vertexCount, u32 startVertex, u32 instanceCount, u32 startInstance) {
    if (scope_ != Scope::RenderPass) return;
    vkCmdDraw(cmdBuffer_, vertexCount, instanceCount, startVertex, startInstance);
    if (const char* prefix = firstPrimaryRasterAuditPrefix()) {
        std::ofstream trace(std::string(prefix) + ".raster.native-draws.jsonl", std::ios::app);
        trace << "{\"native_command_buffer\":" << (u64)(uintptr_t)cmdBuffer_
              << ",\"native_pipeline\":" << (u64)(uintptr_t)rasterAuditPipeline_
              << ",\"serial\":" << rasterAuditSerial_
              << ",\"native_layout\":" << (u64)(uintptr_t)boundPipelineLayout_
              << ",\"scope_renderpass\":true,\"draw\":[" << vertexCount << ','
              << startVertex << ',' << instanceCount << ',' << startInstance << "]}\n";
        trace.close();
        if (!trace) PRIMAL_LOG_STREAM(Error, "RHI") << "[FirstPrimaryRasterAudit] draw write failed" << std::endl;
    }
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

// M5 F5-2：索引间接绘制。非 count 变体（RHI 无 countBuffer 参数——直译；
// D3D12 端 count 布局差异与适配策略见 D3D12CommandBuffer::DrawIndexedIndirect
// 注释）。stride = sizeof(VkDrawIndexedIndirectCommand) = 20B（arg buffer 布局
// 单一事实源）。调用方须已按 rhi-barrier-semantics §1 显式
// MemoryBarrier(ComputeShader→DrawIndirect, ShaderWrite→IndirectCommandRead)。
void VulkanCommandBuffer::DrawIndexedIndirect(ResourceHandle buffer, u64 offset, u32 drawCount) {
    if (scope_ != Scope::RenderPass) return;
    VulkanDevice& vk = static_cast<VulkanDevice&>(device_);
    VulkanBuffer* b = vk.GetBuffer(buffer);
    if (!b) return;
    vkCmdDrawIndexedIndirect(cmdBuffer_, b->GetNativeBuffer(), offset, drawCount,
                             sizeof(VkDrawIndexedIndirectCommand));
    UpdateStats(CommandType::DrawIndirect);
}

void VulkanCommandBuffer::BindComputePipeline(PipelineHandle pipeline) {
    VulkanDevice& vk = static_cast<VulkanDevice&>(device_);
    VulkanPipeline* p = vk.GetPipeline(pipeline);
    if (!p || !p->IsCompute()) return;
    vkCmdBindPipeline(cmdBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE, p->GetNativePipeline());
    if (const char* prefix = (p->IsAnalyticFirstRhoAudit() ? ddgiRecordedComputeAuditPrefix()
            : std::getenv("DDGI_DEFAULT_FIRST_ORACLE_PREFIX"));
        prefix && *prefix && p->GetNativeAuditSerial()) {
        std::ofstream trace(std::string(prefix) + ".native-compute-bindings.jsonl", std::ios::app);
        trace << "{\"serial\":" << p->GetNativeAuditSerial()
              << ",\"pipeline_handle\":" << pipeline
              << ",\"native_pipeline\":" << (u64)(uintptr_t)p->GetNativePipeline()
              << ",\"native_layout\":" << (u64)(uintptr_t)p->GetNativePipelineLayout()
              << ",\"native_command_buffer\":" << (u64)(uintptr_t)cmdBuffer_
              << ",\"bind_point\":1}\n";
        trace.close();
        if (!trace) PRIMAL_LOG_STREAM(Error, "RHI")
            << "[DefaultFirstOracleNativeAudit] binding evidence write failed" << std::endl;
    }
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
    if (!data || size == 0) return;
    if (boundPipelineLayout_ == VK_NULL_HANDLE) return;
    VulkanDevice& vk = static_cast<VulkanDevice&>(device_);
    VkDeviceSize offset = static_cast<VkDeviceSize>(index) * 16;
    if (offset + size > vk.GetMaxPushConstantsSize()) {
        // P4c-F6: >maxPushConstantsSize(通常 128B)回退到隐式 UBO。
        // 内容 = 完整 bytes blob,绑定为 set 3 / binding 0(UNIFORM_BUFFER_
        // DYNAMIC + 动态偏移)。shader 布局约定见 RHIShaderCommon.glsl。
        VulkanDevice::ImplicitUBOAlloc a = vk.AllocImplicitComputeUBO(size);
        if (!a.valid) {
            PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanCommandBuffer] SetComputeBytes: implicit UBO exhausted "
                         "(size=" << size << ") — constants dropped" << std::endl;
            return;
        }
        std::memcpy(a.dst, data, size);
        VkDescriptorSet ds = vk.GetImplicitComputeDescSet();
        u32 dynOff = static_cast<u32>(a.offset);
        vkCmdBindDescriptorSets(cmdBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                                boundPipelineLayout_,
                                VulkanDevice::kImplicitComputeSetIndex,
                                1, &ds, 1, &dynOff);
        return;
    }
    vkCmdPushConstants(cmdBuffer_, boundPipelineLayout_,
                       VK_SHADER_STAGE_COMPUTE_BIT,
                       static_cast<u32>(offset), size, data);
}

void VulkanCommandBuffer::Dispatch(u32 groupCountX, u32 groupCountY, u32 groupCountZ) {
    if (scope_ != Scope::Compute) return;
    vkCmdDispatch(cmdBuffer_, groupCountX, groupCountY, groupCountZ);
    if (const char* prefix = ddgiRecordedComputeAuditPrefix(); prefix && *prefix && std::getenv("DDGI_DUMP") &&
        std::strcmp(std::getenv("DDGI_DUMP"), "2") == 0) {
        std::ofstream trace(std::string(prefix) + ".native-dispatches.jsonl", std::ios::app);
        trace << "{\"native_command_buffer\":" << (u64)(uintptr_t)cmdBuffer_
              << ",\"native_layout\":" << (u64)(uintptr_t)boundPipelineLayout_
              << ",\"groups\":[" << groupCountX << ',' << groupCountY << ',' << groupCountZ
              << "],\"scope_compute\":true}\n";
        trace.close();
        if (!trace) PRIMAL_LOG_STREAM(Error,"RHI") << "[FirstRhoNativeAudit] dispatch evidence failed" << std::endl;
    }
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

void VulkanCommandBuffer::ResetQueryPool(QueryPoolHandle queryPool, u32 firstQuery, u32 queryCount) {
    VulkanDevice& vk = static_cast<VulkanDevice&>(device_);
    VulkanQueryPool* qp = vk.GetQueryPool(queryPool);
    if (!qp) return;
    vkCmdResetQueryPool(cmdBuffer_, qp->GetNativePool(), firstQuery, queryCount);
}

} // namespace primal::graphics::rhi

#endif // ENABLE_VULKAN
