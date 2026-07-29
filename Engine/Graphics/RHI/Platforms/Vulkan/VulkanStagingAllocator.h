/**
 * @file VulkanStagingAllocator.h
 * @brief Per-frame ring-pooled staging allocator for Vulkan
 * @details 4-frame ring of HOST_VISIBLE staging buffers + pending blit queue.
 *          Phase 2 范围:只实现 Allocate + Zero(用于 GPU-only buffer 的 UpdateData)。
 *          Phase 3 在接入 VulkanCommandBuffer 后补 QueueBlit_* + EncodePendingBlits。
 *
 * 模式镜像 MetalStagingAllocator:
 *   - 4 frame pools (MAX_FRAMES_IN_FLIGHT=3 + 1 safety margin)
 *   - BeginFrame() 旋转到下一 pool 并把 bump offset 归零
 *   - Allocate(size) 从当前 frame 的 pool 线性 bump
 *   - QueueBlit_Buffer/Texture 记录到 pending 列表
 *   - EncodePendingBlits(cmd) submit 时统一 vkCmdCopyBuffer
 *
 * 同步:
 *   RenderSystem::BeginFrame 已等过 frame N-3 的 fence,所以当前 pool 是 GPU-safe 的。
 *
 * @author GameEngine VulkanCPP Team
 * @date 2026-07-26
 */

#pragma once

#include "VulkanCommon.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

#include "../../Core/RHITypes.h"
#include <vector>

namespace primal::graphics::rhi {

class VulkanStagingAllocator {
public:
    static constexpr u32 FRAME_COUNT = MAX_FRAMES_IN_FLIGHT + 1; // 4
    static constexpr u64 DEFAULT_POOL_SIZE = 16ULL * 1024ULL * 1024ULL; // 16MB per frame

    struct Allocation {
        void*     cpuPtr        = nullptr;
        VkBuffer  stagingBuffer = VK_NULL_HANDLE;
        u64       offset        = 0;
        bool      overflow      = false;
    };

    VulkanStagingAllocator() = default;
    ~VulkanStagingAllocator();

    VulkanStagingAllocator(const VulkanStagingAllocator&) = delete;
    VulkanStagingAllocator& operator=(const VulkanStagingAllocator&) = delete;

    /**
     * @brief 初始化 4 frame ring pool。
     * @param device    VkDevice(用于 vkCreateBuffer / vkMapMemory)
     * @param allocator VmaAllocator(实际分配 + 子分配)
     * @param poolSize  每 frame pool 字节大小
     */
    void Initialize(VkDevice device, VmaAllocator allocator,
                    u64 poolSize = DEFAULT_POOL_SIZE);

    void Shutdown();

    /// BeginFrame:旋转 pool 并归零 bump offset(每帧 BeginFrame 时调用)
    void BeginFrame();

    /**
     * @brief 从当前 frame pool 线性分配 CPU-writable staging
     * @param size      字节
     * @param alignment 字节对齐(默认 256 — buffer-to-texture blit 的最小要求)
     * @return Allocation.cpuPtr 已映射;overflow=true 时调用方需处理
     */
    Allocation Allocate(u64 size, u64 alignment = 256);

    /// Phase 3 接入:QueueBlit_Buffer / QueueBlit_Texture / EncodePendingBlits
    /// 当前 Phase 2 暂不需要,Phase 3 实现 VulkanCommandBuffer 时再补。

    bool IsInitialized() const { return device_ != VK_NULL_HANDLE; }

private:
    struct FramePool {
        VkBuffer        buffer        = VK_NULL_HANDLE;
        VmaAllocation   allocation   = nullptr;
        void*           mappedPtr     = nullptr;
        u64             offset        = 0;       // 当前 bump offset
        u64             capacity      = 0;
    };

    VkDevice       device_{VK_NULL_HANDLE};
    VmaAllocator   allocator_{nullptr};
    FramePool      pools_[FRAME_COUNT];
    u32            currentFrame_{0};
    bool           initialized_{false};
};

} // namespace primal::graphics::rhi

#endif // ENABLE_VULKAN
