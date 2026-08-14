/**
 * @file VulkanCommandBuffer.h
 * @brief Vulkan RHI 命令缓冲实现
 * @details Phase 3: Begin/End/Reset/Submit/WaitForCompletion + Copy-Blit-Barrier-Mipmap。
 *          Metal 把三 encoder(Render/Blit/Compute)坍缩为单一 VkCommandBuffer,scope 状态机
 *          追踪当前是否在 RenderPass/Compute/Blit 中(Phase 4+ RenderPass 才会真实使用)。
 *
 *          每个 VulkanCommandBuffer 拥有自己的 VkCommandPool(简化版本,Phase 5+ 可改 per-frame pool)。
 *          Submit 时用 device 的 graphics queue + 一个临时 VkFence(本对象独占),WaitForCompletion 等 fence。
 *
 *          SyncHandle(waits/signalSemaphore/signalFence)由 VulkanDevice::submitImpl 处理,
 *          VulkanCommandBuffer::submitImpl 仅接收 waitFlags(Metal 语义),Phase 4 把它接到 device 的 submit。
 * @author GameEngine VulkanCPP Team
 * @date 2026-07-26
 */

#pragma once

#include "VulkanCommon.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

#include "../../Core/RHICommand.h"

namespace primal::graphics::rhi {

class VulkanDevice;
class VulkanTexture;

class VulkanCommandBuffer : public RHICommandBuffer {
    friend class VulkanDevice;
public:
    VulkanCommandBuffer(VulkanDevice& device, CommandQueueType type);
    VulkanCommandBuffer(VulkanCommandBuffer&& other) noexcept;
    VulkanCommandBuffer& operator=(VulkanCommandBuffer&& other) noexcept;
    VulkanCommandBuffer(const VulkanCommandBuffer&) = delete;
    VulkanCommandBuffer& operator=(const VulkanCommandBuffer&) = delete;
    virtual ~VulkanCommandBuffer();

    bool Initialize() override;

    VkCommandBuffer GetNativeCmdBuffer() const { return cmdBuffer_; }

    // === 渲染命令(Phase 4 实现)===
    void BeginRenderPass(const RenderPassDesc& desc) override;
    void BeginRenderPass(RenderPassHandle renderPass) override;
    void EndRenderPass() override;
    void SetViewport(const ViewportDesc& viewport) override;
    void SetScissor(const Rect& scissor) override;
    void BindGraphicsPipeline(PipelineHandle pipeline) override;
    void BindVertexBuffers(u32 firstSlot, u32 slotCount, const ResourceHandle* buffers, const u64* offsets) override;
    void BindIndexBuffer(ResourceHandle buffer, DataFormat format, u64 offset = 0) override;
    void BindDescriptorSets(PipelineBindPoint bindPoint, PipelineLayoutHandle layout,
                            u32 firstSet, u32 setCount,
                            const DescriptorSetHandle* descriptorSets,
                            u32 dynamicOffsetCount, const u32* dynamicOffsets) override;
    void Draw(u32 vertexCount, u32 startVertex = 0, u32 instanceCount = 1, u32 startInstance = 0) override;
    void DrawIndexed(u32 indexCount, u32 startIndex = 0, u32 baseVertex = 0,
                     u32 instanceCount = 1, u32 startInstance = 0) override;
    void DrawIndirect(ResourceHandle buffer, u64 offset = 0, u32 drawCount = 1) override;
    void BindComputePipeline(PipelineHandle pipeline) override;
    void PushConstants(PipelineLayoutHandle layout, ShaderStage stageFlags,
                       u32 offset, u32 size, const void* pValues) override;
    void SetComputeBytes(u32 index, const void* data, u32 size) override;
    void Dispatch(u32 groupCountX, u32 groupCountY, u32 groupCountZ) override;
    void DispatchIndirect(ResourceHandle buffer, u64 offset = 0) override;
    void WriteTimestamp(QueryPoolHandle queryPool, u32 queryIndex) override;
    // T4.6.5 part 24.2 (B6 fix): GPU-side query pool reset.
    void ResetQueryPool(QueryPoolHandle queryPool, u32 firstQuery, u32 queryCount) override;

    // === 资源操作命令(Phase 3 实现)===
    void MemoryBarrier(PipelineStage srcStageMask, PipelineStage dstStageMask,
                       AccessFlag srcAccessMask, AccessFlag dstAccessMask) override;
    void CopyBuffer(ResourceHandle src, ResourceHandle dst,
                    u64 srcOffset = 0, u64 dstOffset = 0, u64 size = 0) override;
    void CopyBufferToTexture(ResourceHandle srcBuffer, ResourceHandle dstTexture,
                             const BufferTextureCopyRegion* regions, u32 regionCount) override;
    void CopyTextureToBuffer(ResourceHandle srcTexture, ResourceHandle dstBuffer,
                             const BufferTextureCopyRegion* regions, u32 regionCount) override;
    void BlitTexture(ResourceHandle src, ResourceHandle dst,
                     const TextureBlitRegion* regions, u32 regionCount, FilterMode filter) override;
    void GenerateMipmaps(ResourceHandle texture) override;
    void InsertBarrier(const ResourceBarrier* barriers, u32 barrierCount) override;

protected:
    void destroyImpl() override;
    bool resetImpl() override;
    bool beginImpl() override;
    bool endImpl() override;
    bool submitImpl(u32 waitFlags) override;
    bool waitForCompletionImpl() override;

private:
    /// 把当前 texture 的 layout 显式 transition 到 newLayout(implicit barrier)
    void TransitionImageLayout(VulkanTexture* tex, VkImageLayout newLayout,
                               u32 baseMip = 0, u32 mipCount = 0xFFFFFFFFu);

    // T4.6.5 part 24.6 (B8 fix): expose internal fence so submitImpl can
    // signal it when caller doesn't provide one. Without this, device-side
    // Submit + WaitForCompletion pair can't synchronize (cmd's fence isn't
    // signaled), and destroying the cmd buffer triggers
    // VUID-vkFreeCommandBuffers-pCommandBuffers-00047.
    VkFence GetSubmitFence() const { return submitFence_; }

    VkCommandPool   cmdPool_{VK_NULL_HANDLE};
    VkCommandBuffer cmdBuffer_{VK_NULL_HANDLE};
    VkFence         submitFence_{VK_NULL_HANDLE};
    u32             queueFamily_{UINT32_MAX};

    // T4.6.5 part 30.6 (X7 fix): tracks whether the last device->Submit
    // call supplied an external signalFence. If true, vkQueueSubmit signals
    // that external fence (NOT this cmd buffer's internal submitFence_),
    // so waitForCompletionImpl must NOT wait on submitFence_ — it would
    // never be signaled. Caller's WaitForSync(external_fence) is the
    // authoritative sync. Reset to false in resetImpl.
    bool            externalFenceSignaled_{false};

    // scope 状态(Phase 4 用,Phase 3 仅记录)
    enum class Scope : u8 { None, RenderPass, Compute, Blit };
    Scope scope_{Scope::None};

    // Phase 4:BeginRenderPass 内部临时 rp/fb,EndRenderPass 销毁
    VkRenderPass       pendingRenderPass_{VK_NULL_HANDLE};
    VkFramebuffer      pendingFramebuffer_{VK_NULL_HANDLE};
    VkPipelineLayout   boundPipelineLayout_{VK_NULL_HANDLE};  // BindGraphicsPipeline 设置

    // Tracks attachment textures + their finalLayouts so EndRenderPass can
    // update currentLayout_ (Vulkan render pass does implicit layout transitions
    // that the texture tracking must reflect).
    struct PendingAttachment { VulkanTexture* tex; VkImageLayout finalLayout; };
    std::vector<PendingAttachment> pendingAttachments_;
};

} // namespace primal::graphics::rhi

#endif // ENABLE_VULKAN
