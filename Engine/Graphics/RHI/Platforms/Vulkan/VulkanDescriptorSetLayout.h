/**
 * @file VulkanDescriptorSetLayout.h
 * @brief Vulkan RHI DescriptorSetLayout 实现
 * @details Phase 4: DescriptorSetLayoutDesc → VkDescriptorSetLayout。
 *          每个 layout 持有自己的 VkDescriptorPool,供 VulkanDescriptorSet allocate。
 *          (Phase 5+ 可改全局 pool,简化先 per-layout)
 * @author GameEngine VulkanCPP Team
 * @date 2026-07-26
 */

#pragma once

#include "VulkanCommon.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

#include "../../Core/RHIDescriptorSetLayout.h"

namespace primal::graphics::rhi {

class VulkanDevice;

class VulkanDescriptorSetLayout : public RHIDescriptorSetLayout {
    friend class VulkanDevice;
    friend class VulkanDescriptorSet;
public:
    VulkanDescriptorSetLayout(VulkanDevice& device, const DescriptorSetLayoutDesc& desc);
    VulkanDescriptorSetLayout(VulkanDescriptorSetLayout&&) = delete;
    VulkanDescriptorSetLayout& operator=(VulkanDescriptorSetLayout&&) = delete;
    VulkanDescriptorSetLayout(const VulkanDescriptorSetLayout&) = delete;
    VulkanDescriptorSetLayout& operator=(const VulkanDescriptorSetLayout&) = delete;
    virtual ~VulkanDescriptorSetLayout() { destroyImpl(); }

    bool Initialize() override;
    VkDescriptorSetLayout GetNativeLayout() const { return layout_; }
    VkDescriptorPool       GetNativePool() const  { return pool_; }

    /// Allocate one VkDescriptorSet from per-layout pool. Returns VK_NULL_HANDLE on failure.
    VkDescriptorSet AllocateSet();

protected:
    void destroyImpl() override;
    void* mapImpl(u64, u64) override { return nullptr; }
    void unmapImpl() override {}
    bool updateDataImpl(const void*, u64, u64) override { return false; }

private:
    VkDescriptorSetLayout layout_{VK_NULL_HANDLE};
    VkDescriptorPool      pool_{VK_NULL_HANDLE};
};

} // namespace primal::graphics::rhi

#endif // ENABLE_VULKAN
