/**
 * @file VulkanSync.h
 * @brief Vulkan 同步原语实现
 * @details 同时持 VkFence(CPU 等) + VkSemaphore(GPU-GPU 同步)。
 *          Metal 把两者合一(SharedEvent);Vulkan 必须分开:
 *           - CPU 端 vkWaitForFences 等 GPU 完成
 *           - GPU 端 vkQueueSubmit 的 wait/signal semaphore 半环同步
 *          两者由 QueueSubmitInfo::signalFence / waitSemaphore / signalSemaphore 分别选用。
 * @author GameEngine VulkanCPP Team
 * @date 2026-07-26
 */

#pragma once

#include "VulkanCommon.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

namespace primal::graphics::rhi {

class VulkanSync {
public:
    VulkanSync(VkDevice device, bool signaled = false);
    ~VulkanSync();

    VulkanSync(const VulkanSync&) = delete;
    VulkanSync& operator=(const VulkanSync&) = delete;

    VkFence GetFence() const { return fence_; }
    VkSemaphore GetSemaphore() const { return semaphore_; }

    /// CPU 等待 fence — 阻塞直到 GPU signal 或 timeout
    bool WaitFence(u64 timeoutNs) const;

    /// Reset fence 到 unsignaled 状态(下次 submit 前必须 reset)
    void ResetFence();

    /// 是否已被 signaled(fence 已 ping)
    bool IsSignaled() const { return signaled_; }

    /// 把 fence ownership 转移给调用者(GC 延迟销毁时用)
    /// 调用者负责 vkDestroyFence/vkDestroySemaphore。
    void DetachNatives(VkFence* outFence, VkSemaphore* outSem);

private:
    VkDevice device_{VK_NULL_HANDLE};
    VkFence fence_{VK_NULL_HANDLE};
    VkSemaphore semaphore_{VK_NULL_HANDLE};
    bool signaled_{false};
};

} // namespace primal::graphics::rhi

#endif // ENABLE_VULKAN
