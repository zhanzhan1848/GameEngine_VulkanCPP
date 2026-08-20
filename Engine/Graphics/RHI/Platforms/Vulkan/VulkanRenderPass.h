/**
 * @file VulkanRenderPass.h
 * @brief Vulkan RHI RenderPass 实现
 * @details Phase 4: RenderPassDesc → VkRenderPass。
 *          Vulkan 1.0 强制 RenderPass 兼容性(VkRenderPass 在 VkGraphicsPipelineCreateInfo 必填),
 *          Phase 5+ 改 VK_KHR_dynamic_rendering 可去除。
 *          本类只关心 attachment formats + loadOp/storeOp — 由 CommandBuffer::BeginRenderPass
 *          实际使用,texture 引用在 BeginRenderPass 时从 RenderPassDesc 再读一次。
 * @author GameEngine VulkanCPP Team
 * @date 2026-07-26
 */

#pragma once

#include "VulkanCommon.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

#include "../../Core/RHIRenderPass.h"

namespace primal::graphics::rhi {

class VulkanDevice;

class VulkanRenderPass : public RHIRenderPass {
    friend class VulkanDevice;
    friend class VulkanCommandBuffer;
public:
    VulkanRenderPass(VulkanDevice& device, const RenderPassDesc& desc);
    VulkanRenderPass(VulkanRenderPass&&) = delete;
    VulkanRenderPass& operator=(VulkanRenderPass&&) = delete;
    VulkanRenderPass(const VulkanRenderPass&) = delete;
    VulkanRenderPass& operator=(const VulkanRenderPass&) = delete;
    virtual ~VulkanRenderPass() { destroyImpl(); }

    bool Initialize() override;
    VkRenderPass GetNativeRenderPass() const { return renderPass_; }

protected:
    void destroyImpl() override;
    void* mapImpl(u64, u64) override { return nullptr; }
    void unmapImpl() override {}
    bool updateDataImpl(const void*, u64, u64) override { return false; }

private:
    VkRenderPass renderPass_{VK_NULL_HANDLE};
};

} // namespace primal::graphics::rhi

#endif // ENABLE_VULKAN
