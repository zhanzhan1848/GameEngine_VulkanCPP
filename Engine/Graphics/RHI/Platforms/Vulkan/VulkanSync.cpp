/**
 * @file VulkanSync.cpp
 * @brief VulkanSync 实现
 * @author GameEngine VulkanCPP Team
 * @date 2026-07-26
 */

#include "VulkanSync.h"
#include "VulkanHangDiag.h"
#include "Engine/Common/CoreLogStream.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

#include <iostream>

namespace primal::graphics::rhi {

VulkanSync::VulkanSync(VkDevice device, bool signaled)
    : device_(device), signaled_(signaled) {
    if (!device) return;

    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = signaled ? VK_FENCE_CREATE_SIGNALED_BIT : 0;
    if (vkCreateFence(device_, &fci, nullptr, &fence_) != VK_SUCCESS) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanSync] vkCreateFence failed" << std::endl;
        fence_ = VK_NULL_HANDLE;
    }

    VkSemaphoreCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    sci.flags = 0;
    if (vkCreateSemaphore(device_, &sci, nullptr, &semaphore_) != VK_SUCCESS) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanSync] vkCreateSemaphore failed" << std::endl;
        semaphore_ = VK_NULL_HANDLE;
    }
}

VulkanSync::~VulkanSync() {
    if (fence_ != VK_NULL_HANDLE) {
        hangdiag::RetireFence(fence_);
        vkDestroyFence(device_, fence_, nullptr);
        fence_ = VK_NULL_HANDLE;
    }
    if (semaphore_ != VK_NULL_HANDLE) {
        vkDestroySemaphore(device_, semaphore_, nullptr);
        semaphore_ = VK_NULL_HANDLE;
    }
}

bool VulkanSync::WaitFence(u64 timeoutNs) const {
    if (fence_ == VK_NULL_HANDLE) return false;
    if (signaled_) return true;
    // R10 taskC（Windows 卡死诊断）：调用方传 UINT32_MAX ms（≈49.7 天）的
    // "无限"等待在 device lost 后永不返回。PRIMAL_VK_HANG_DIAG=1 时改为
    // 2s 步进有界轮询（超时 dump+_Exit(12)，lost 时 _Exit(13)）；门控
    // 关闭时保持原单次 vkWaitForFences 调用与超时语义逐字节一致。
    if (hangdiag::Enabled() && timeoutNs >= 3600000000000ULL) {
        return hangdiag::BoundedWaitFence(device_, "VulkanSync::WaitFence(infinite)", fence_);
    }
    VkResult res = vkWaitForFences(device_, 1, &fence_, VK_TRUE, timeoutNs);
    if (res == VK_SUCCESS) {
        return true;
    }
    if (res == VK_TIMEOUT) {
        return false;
    }
    if (res == VK_ERROR_DEVICE_LOST) hangdiag::ReportDeviceLost("VulkanSync::WaitFence", res);
    PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanSync] vkWaitForFences failed: " << res << std::endl;
    return false;
}

void VulkanSync::ResetFence() {
    if (fence_ == VK_NULL_HANDLE) return;
    // T4.6.5 part 30.6 (Bug B): call vkResetFences unconditionally. Spec
    // allows calling on an unsignaled fence (no-op). The prior `if (signaled_)`
    // guard was broken: WaitFence() returned true without flipping
    // signaled_, so the next ResetFence skipped vkResetFences — but the
    // actual VkFence was in fact signaled from the prior vkQueueSubmit.
    // vkQueueSubmit then fired VUID-vkQueueSubmit-fence-00063 every frame.
    vkResetFences(device_, 1, &fence_);
    signaled_ = false;
}

void VulkanSync::DetachNatives(VkFence* outFence, VkSemaphore* outSem) {
    if (outFence) *outFence = fence_;
    if (outSem)   *outSem   = semaphore_;
    // R10 taskC：句柄所有权移交 GC 延迟销毁——本对象不再持有，dump 侧
    // 由 VulkanDevice::destroySyncImpl 的 GC lambda retire 兜底。
    fence_ = VK_NULL_HANDLE;
    semaphore_ = VK_NULL_HANDLE;
    signaled_ = false;
}

} // namespace primal::graphics::rhi

#endif // ENABLE_VULKAN
