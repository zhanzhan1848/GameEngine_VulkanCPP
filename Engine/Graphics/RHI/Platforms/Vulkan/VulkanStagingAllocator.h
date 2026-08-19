/**
 * @file VulkanStagingAllocator.h
 * @brief Per-frame ring-pooled staging allocator for Vulkan
 * @details 4-frame ring of HOST_VISIBLE staging buffers + pending blit queue.
 *          P4c-F4:Allocate + QueueBlit_Buffer/QueueBlit_Texture +
 *          EncodePendingBlits + FlushBlocking 全量落地(对齐 MetalStagingAllocator)。
 *
 * 模式镜像 MetalStagingAllocator:
 *   - 4 frame pools (MAX_FRAMES_IN_FLIGHT=3 + 1 safety margin)
 *   - BeginFrame() 旋转到下一 pool 并把 bump offset 归零
 *   - Allocate(size) 从当前 frame 的 pool 线性 bump
 *     (pool 耗尽时自动 FlushBlocking 腾空队列后重试一次 — overflow 约定同 Metal)
 *   - QueueBlit_Buffer/Texture 记录到 pending 列表
 *   - EncodePendingBlits(cmd) 在 VulkanCommandBuffer::Begin(帧首)统一编码:
 *     纹理先 barrier 到 TRANSFER_DST_OPTIMAL → vkCmdCopy* → barrier 回
 *     调用方指定的 backLayout(通常 SHADER_READ_ONLY)
 *   - FlushBlocking() 立即模式:一次性 cmdbuf + queue wait(资产加载期)
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

class VulkanDevice;

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
     * @param device    VkDevice(用于 transient cmdbuf / FlushBlocking)
     * @param allocator VmaAllocator(实际分配 + 子分配)
     * @param queue     graphics queue(FlushBlocking 提交用;不持有)
     * @param queueFamily queue family index(FlushBlocking 的 transient pool)
     * @param poolSize  每 frame pool 字节大小
     */
    void Initialize(VkDevice device, VmaAllocator allocator,
                    VkQueue queue = VK_NULL_HANDLE, u32 queueFamily = 0,
                    u64 poolSize = DEFAULT_POOL_SIZE);

    void Shutdown();

    /// BeginFrame:旋转 pool 并归零 bump offset(每帧 BeginFrame 时调用)
    void BeginFrame();

    /**
     * @brief 从当前 frame pool 线性分配 CPU-writable staging
     * @param size      字节
     * @param alignment 字节对齐(默认 256 — buffer-to-texture blit 的最小要求)
     * @return Allocation.cpuPtr 已映射;overflow=true 时调用方需 fallback:
     *         FlushBlocking() 排干已 queue 数据 + 一次性 VMA staging 路径
     *         (bump 空间到 BeginFrame 才回收,此处不重试;Metal 同款约定)
     */
    Allocation Allocate(u64 size, u64 alignment = 256);

    // ========================================================================
    // P4c-F4: pending blit 队列(签名对齐 MetalStagingAllocator,
    // VkBuffer/VkImage 替换 MTL 类型;末尾的 layout/aspect 参数是 Vulkan
    // 特有 — Metal 无显式布局概念)
    // ========================================================================

    /**
     * @brief Queue buffer-to-buffer blit(vkCmdCopyBuffer,Encode 时统一执行)
     */
    void QueueBlit_Buffer(Allocation alloc,
                          VkBuffer dstBuffer, u64 dstOffset, u64 size);

    /**
     * @brief Queue buffer-to-texture blit(vkCmdCopyBufferToImage)
     * @param bytesPerRow   源布局(字节)— 紧密行主序时 0;内部换算为
     *                      VkBufferImageCopy 的 texel 单位 rowLength
     * @param bytesPerImage 源布局(字节)— 2D 单 slice 时 0
     * @param currentLayout queue 时纹理的 tracked layout(Encode 时 → TRANSFER_DST)
     * @param backLayout    Encode 完成后的回置 layout(通常 SHADER_READ_ONLY)
     * @param aspect        DEPTH 格式必须传 VK_IMAGE_ASPECT_DEPTH_BIT
     * @param texelSize     目标格式每 texel 字节数(bytesPerRow 非 0 时必须
     *                      能整除;Vulkan 的 rowLength 单位是 texel)
     * @note 契约:调用方在 queue 后负责把纹理的 tracked layout 同步为
     *       backLayout(VulkanTexture::SetCurrentLayout)——allocator 只持有
     *       裸 VkImage,无法触碰 RHI 层跟踪;updateDataImpl 已内建该同步。
     */
    void QueueBlit_Texture(Allocation alloc,
                           VkImage dstTexture, u32 mipLevel, u32 slice,
                           u32 originX, u32 originY, u32 originZ,
                           u32 width, u32 height, u32 depth,
                           u32 bytesPerRow, u32 bytesPerImage,
                           VkImageLayout currentLayout,
                           VkImageLayout backLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT,
                           u32 texelSize = 4);

    /**
     * @brief 把当前 frame 的 pending blits 编码进 cmd(帧首调用)
     * @details VulkanCommandBuffer::Begin 自动调用。纹理 blit 的布局转换由
     *          这里统一闭合(→ TRANSFER_DST → backLayout),完成后清空队列。
     *          必须在用户 render/compute pass 之前执行,后续 pass 才能看到
     *          上传内容(对齐 Metal "单 blit encoder + 用户 pass 之前" 语义)。
     */
    void EncodePendingBlits(VkCommandBuffer cmd);

    /**
     * @brief 阻塞 flush — 无帧上下文(资产加载期)调用
     * @details transient pool + one-shot cmdbuf + vkQueueWaitIdle。
     *          复用当前 frame pool 的分配(等待完成后 pool 数据已 GPU 消费)。
     */
    void FlushBlocking();

    /// 当前 frame 是否有 pending blit(接线方判断是否需要 Encode)
    bool HasPendingBlits() const;

    bool IsInitialized() const { return device_ != VK_NULL_HANDLE; }

private:
    struct PendingBlit {
        // Source
        VkBuffer  srcBuffer  = VK_NULL_HANDLE;
        u64       srcOffset  = 0;

        // Destination (one of)
        VkBuffer  dstBuffer  = VK_NULL_HANDLE;
        VkImage   dstTexture = VK_NULL_HANDLE;

        // Buffer blit params
        u64       dstBufferOffset = 0;
        u64       size            = 0;

        // Texture blit params
        u32       mipLevel        = 0;
        u32       slice           = 0;
        u32       originX         = 0;
        u32       originY         = 0;
        u32       originZ         = 0;
        u32       width           = 0;
        u32       height          = 0;
        u32       depth           = 0;
        u32       bytesPerRow     = 0;
        u32       bytesPerImage   = 0;
        u32       texelSize       = 4;
        VkImageLayout currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageLayout backLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkImageAspectFlags aspect   = VK_IMAGE_ASPECT_COLOR_BIT;

        bool      isTexture       = false;
    };

    struct FramePool {
        VkBuffer        buffer        = VK_NULL_HANDLE;
        VmaAllocation   allocation   = nullptr;
        void*           mappedPtr     = nullptr;
        u64             offset        = 0;       // 当前 bump offset
        u64             capacity      = 0;
        std::vector<PendingBlit> pending;         // per-frame blit 队列
    };

    VkDevice       device_{VK_NULL_HANDLE};
    VmaAllocator   allocator_{nullptr};
    VkQueue        queue_{VK_NULL_HANDLE};      // FlushBlocking 用(不持有)
    u32            queueFamily_{0};
    FramePool      pools_[FRAME_COUNT];
    u32            currentFrame_{0};
    bool           initialized_{false};
};

} // namespace primal::graphics::rhi

#endif // ENABLE_VULKAN
