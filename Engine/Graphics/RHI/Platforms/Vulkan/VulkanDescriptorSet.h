/**
 * @file VulkanDescriptorSet.h
 * @brief Vulkan RHI DescriptorSet 实现
 * @details Phase 4: VkDescriptorSet 从 layout 的 per-layout pool allocate。
 *          Update(writes) 直接 vkUpdateDescriptorSets。
 *          内部缓存 image/buffer 引用以便 GC 防释放(简化版:Phase 5 加更强引用追踪)。
 * @author GameEngine VulkanCPP Team
 * @date 2026-07-26
 */

#pragma once

#include "VulkanCommon.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

#include "../../Core/RHIDescriptorSet.h"

namespace primal::graphics::rhi {

class VulkanDevice;

class VulkanDescriptorSet : public RHIDescriptorSet {
    friend class VulkanDevice;
    friend class VulkanCommandBuffer;
public:
    VulkanDescriptorSet(VulkanDevice& device, const DescriptorSetDesc& desc);
    VulkanDescriptorSet(VulkanDescriptorSet&&) = delete;
    VulkanDescriptorSet& operator=(VulkanDescriptorSet&&) = delete;
    VulkanDescriptorSet(const VulkanDescriptorSet&) = delete;
    VulkanDescriptorSet& operator=(const VulkanDescriptorSet&) = delete;
    virtual ~VulkanDescriptorSet() { destroyImpl(); }

    bool Initialize() override;
    VkDescriptorSet GetNativeSet() const { return set_; }

    /// 应用 writes(从 layout 取 binding 元数据校验 type)
    void Update(const WriteDescriptorSet* writes, u32 writeCount);

protected:
    void destroyImpl() override;
    void* mapImpl(u64, u64) override { return nullptr; }
    void unmapImpl() override {}
    bool updateDataImpl(const void*, u64, u64) override { return false; }

private:
    VkDescriptorSet set_{VK_NULL_HANDLE};
};

} // namespace primal::graphics::rhi

#endif // ENABLE_VULKAN
