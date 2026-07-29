/**
 * @file VulkanPipelineLayout.h
 * @brief Vulkan RHI PipelineLayout 实现
 * @details Phase 4: PipelineLayoutDesc(setLayouts + pushConstantRanges) → VkPipelineLayout。
 * @author GameEngine VulkanCPP Team
 * @date 2026-07-26
 */

#pragma once

#include "VulkanCommon.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

#include "../../Core/RHIPipelineLayout.h"

namespace primal::graphics::rhi {

class VulkanDevice;

class VulkanPipelineLayout : public RHIPipelineLayout {
    friend class VulkanDevice;
    friend class VulkanCommandBuffer;
public:
    VulkanPipelineLayout(VulkanDevice& device, const PipelineLayoutDesc& desc);
    VulkanPipelineLayout(VulkanPipelineLayout&&) = delete;
    VulkanPipelineLayout& operator=(VulkanPipelineLayout&&) = delete;
    VulkanPipelineLayout(const VulkanPipelineLayout&) = delete;
    VulkanPipelineLayout& operator=(const VulkanPipelineLayout&) = delete;
    virtual ~VulkanPipelineLayout() { destroyImpl(); }

    bool Initialize() override;
    VkPipelineLayout GetNativeLayout() const { return layout_; }

protected:
    void destroyImpl() override;
    void* mapImpl(u64, u64) override { return nullptr; }
    void unmapImpl() override {}
    bool updateDataImpl(const void*, u64, u64) override { return false; }

private:
    VkPipelineLayout layout_{VK_NULL_HANDLE};
};

} // namespace primal::graphics::rhi

#endif // ENABLE_VULKAN
