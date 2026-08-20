/**
 * @file VulkanSampler.h
 * @brief Vulkan RHI sampler 实现
 * @details Phase 4: SamplerDesc → VkSampler。独立 class。
 * @author GameEngine VulkanCPP Team
 * @date 2026-07-26
 */

#pragma once

#include "VulkanCommon.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

#include "../../Core/RHITypes.h"

namespace primal::graphics::rhi {

class VulkanDevice;

class VulkanSampler {
    friend class VulkanDevice;
    friend class VulkanDescriptorSet;
public:
    explicit VulkanSampler(VulkanDevice& device);
    VulkanSampler(VulkanSampler&& other) noexcept;
    VulkanSampler& operator=(VulkanSampler&& other) noexcept;
    VulkanSampler(const VulkanSampler&) = delete;
    VulkanSampler& operator=(const VulkanSampler&) = delete;
    ~VulkanSampler();

    bool Initialize(const SamplerDesc& desc);
    void Destroy();

    VkSampler GetNativeSampler() const { return sampler_; }

    SamplerHandle GetHandle() const { return handle_; }
    void          SetHandle(SamplerHandle h) { handle_ = h; }

private:
    VulkanDevice& device_;
    SamplerHandle handle_{handles::INVALID_SAMPLER};
    VkSampler     sampler_{VK_NULL_HANDLE};
};

} // namespace primal::graphics::rhi

#endif // ENABLE_VULKAN
