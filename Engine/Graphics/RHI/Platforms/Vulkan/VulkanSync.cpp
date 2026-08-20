/**
 * @file VulkanSync.cpp
 * @brief VulkanSync 实现
 * @author GameEngine VulkanCPP Team
 * @date 2026-07-26
 */

#include "VulkanSync.h"

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
        std::cerr << "[VulkanSync] vkCreateFence failed" << std::endl;
        fence_ = VK_NULL_HANDLE;
    }

    VkSemaphoreCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    sci.flags = 0;
    if (vkCreateSemaphore(device_, &sci, nullptr, &semaphore_) != VK_SUCCESS) {
        std::cerr << "[VulkanSync] vkCreateSemaphore failed" << std::endl;
        semaphore_ = VK_NULL_HANDLE;
    }
}

VulkanSync::~VulkanSync() {
    if (fence_ != VK_NULL_HANDLE) {
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
    VkResult res = vkWaitForFences(device_, 1, &fence_, VK_TRUE, timeoutNs);
    if (res == VK_SUCCESS) {
        return true;
    }
    if (res == VK_TIMEOUT) {
        return false;
    }
    std::cerr << "[VulkanSync] vkWaitForFences failed: " << res << std::endl;
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
    fence_ = VK_NULL_HANDLE;
    semaphore_ = VK_NULL_HANDLE;
    signaled_ = false;
}

} // namespace primal::graphics::rhi

#endif // ENABLE_VULKAN
