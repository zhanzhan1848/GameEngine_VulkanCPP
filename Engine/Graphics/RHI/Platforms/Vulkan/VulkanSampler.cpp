/**
 * @file VulkanSampler.cpp
 * @brief VulkanSampler 实现
 * @author GameEngine VulkanCPP Team
 * @date 2026-07-26
 */

#include "VulkanSampler.h"
#include "VulkanDevice.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

#include <iostream>

namespace primal::graphics::rhi {

namespace {

VkFilter ToVkFilterMode(FilterMode f) {
    switch (f) {
        case FilterMode::Point:    return VK_FILTER_NEAREST;
        case FilterMode::Linear:
        case FilterMode::Anisotropic:
        default:                   return VK_FILTER_LINEAR;
    }
}

VkSamplerMipmapMode ToVkMipFilterMode(FilterMode f) {
    switch (f) {
        case FilterMode::Point:    return VK_SAMPLER_MIPMAP_MODE_NEAREST;
        default:                   return VK_SAMPLER_MIPMAP_MODE_LINEAR;
    }
}

VkSamplerAddressMode ToVkAddressMode(TextureAddressMode m) {
    switch (m) {
        case TextureAddressMode::Wrap:        return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case TextureAddressMode::Mirror:      return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case TextureAddressMode::Clamp:       return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case TextureAddressMode::Border:      return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        case TextureAddressMode::MirrorOnce:  return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
        default:                              return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

VkCompareOp ToVkCompareOp(ComparisonFunc c) {
    switch (c) {
        case ComparisonFunc::Never:        return VK_COMPARE_OP_NEVER;
        case ComparisonFunc::Less:         return VK_COMPARE_OP_LESS;
        case ComparisonFunc::Equal:        return VK_COMPARE_OP_EQUAL;
        case ComparisonFunc::LessEqual:    return VK_COMPARE_OP_LESS_OR_EQUAL;
        case ComparisonFunc::Greater:      return VK_COMPARE_OP_GREATER;
        case ComparisonFunc::NotEqual:     return VK_COMPARE_OP_NOT_EQUAL;
        case ComparisonFunc::GreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
        case ComparisonFunc::Always:       return VK_COMPARE_OP_ALWAYS;
        default:                           return VK_COMPARE_OP_NEVER;
    }
}

} // anonymous namespace

VulkanSampler::VulkanSampler(VulkanDevice& device) : device_(device) {}

VulkanSampler::VulkanSampler(VulkanSampler&& other) noexcept
    : device_(other.device_), handle_(other.handle_), sampler_(other.sampler_) {
    other.sampler_ = VK_NULL_HANDLE;
    other.handle_ = handles::INVALID_SAMPLER;
}

VulkanSampler& VulkanSampler::operator=(VulkanSampler&& other) noexcept {
    if (this != &other) {
        Destroy();
        handle_ = other.handle_;
        sampler_ = other.sampler_;
        other.sampler_ = VK_NULL_HANDLE;
        other.handle_ = handles::INVALID_SAMPLER;
    }
    return *this;
}

VulkanSampler::~VulkanSampler() { Destroy(); }

bool VulkanSampler::Initialize(const SamplerDesc& desc) {
    VkSamplerCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    ci.magFilter    = ToVkFilterMode(desc.magFilter);
    ci.minFilter    = ToVkFilterMode(desc.minFilter);
    ci.mipmapMode   = ToVkMipFilterMode(desc.mipFilter);
    ci.addressModeU = ToVkAddressMode(desc.addressU);
    ci.addressModeV = ToVkAddressMode(desc.addressV);
    ci.addressModeW = ToVkAddressMode(desc.addressW);
    ci.mipLodBias   = desc.mipLodBias;
    ci.anisotropyEnable = (desc.maxAnisotropy > 1) ? VK_TRUE : VK_FALSE;
    ci.maxAnisotropy    = static_cast<float>(desc.maxAnisotropy);
    ci.compareEnable    = (desc.comparisonFunc != ComparisonFunc::Never) ? VK_TRUE : VK_FALSE;
    ci.compareOp        = ToVkCompareOp(desc.comparisonFunc);
    ci.minLod           = desc.minLod;
    ci.maxLod           = desc.maxLod;
    ci.borderColor      = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    ci.unnormalizedCoordinates = VK_FALSE;

    VkDevice dev = static_cast<VulkanDevice&>(device_).GetNativeDevice();
    if (vkCreateSampler(dev, &ci, nullptr, &sampler_) != VK_SUCCESS) {
        std::cerr << "[VulkanSampler] vkCreateSampler failed" << std::endl;
        return false;
    }
    return true;
}

void VulkanSampler::Destroy() {
    if (sampler_ == VK_NULL_HANDLE) return;
    VkDevice dev = static_cast<VulkanDevice&>(device_).GetNativeDevice();
    VkSampler s = sampler_;
    device_.GetGarbageCollector().DeferredDestroy([dev, s]() {
        if (dev != VK_NULL_HANDLE && s != VK_NULL_HANDLE) vkDestroySampler(dev, s, nullptr);
    });
    sampler_ = VK_NULL_HANDLE;
}

} // namespace primal::graphics::rhi

#endif // ENABLE_VULKAN
