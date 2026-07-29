/**
 * @file VulkanPipelineLayout.cpp
 * @brief VulkanPipelineLayout 实现
 * @author GameEngine VulkanCPP Team
 * @date 2026-07-26
 */

#include "VulkanPipelineLayout.h"
#include "VulkanDevice.h"
#include "VulkanDescriptorSetLayout.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

#include <iostream>

namespace primal::graphics::rhi {

namespace {

VkShaderStageFlags StageFlagsFromRange(ShaderStage s) {
    VkShaderStageFlags f = 0;
    const u32 v = static_cast<u32>(s);
    if (v & static_cast<u32>(ShaderStage::Vertex))   f |= VK_SHADER_STAGE_VERTEX_BIT;
    if (v & static_cast<u32>(ShaderStage::Pixel))    f |= VK_SHADER_STAGE_FRAGMENT_BIT;
    if (v & static_cast<u32>(ShaderStage::Geometry)) f |= VK_SHADER_STAGE_GEOMETRY_BIT;
    if (v & static_cast<u32>(ShaderStage::Compute))  f |= VK_SHADER_STAGE_COMPUTE_BIT;
    return f;
}

} // anonymous namespace

VulkanPipelineLayout::VulkanPipelineLayout(VulkanDevice& device, const PipelineLayoutDesc& desc)
    : RHIPipelineLayout(device, desc) {}

bool VulkanPipelineLayout::Initialize() {
    VkDevice dev = static_cast<VulkanDevice&>(device_).GetNativeDevice();

    std::vector<VkDescriptorSetLayout> vkSetLayouts;
    vkSetLayouts.reserve(setLayouts_.size());
    for (auto h : setLayouts_) {
        if (h == handles::INVALID_RESOURCE) {
            vkSetLayouts.push_back(VK_NULL_HANDLE);
            continue;
        }
        VulkanDescriptorSetLayout* sl = static_cast<VulkanDevice&>(device_).GetDescriptorSetLayout(h);
        vkSetLayouts.push_back(sl ? sl->GetNativeLayout() : VK_NULL_HANDLE);
    }

    std::vector<VkPushConstantRange> vkPush;
    vkPush.reserve(pushConstantRanges_.size());
    for (const auto& r : pushConstantRanges_) {
        VkPushConstantRange vk{};
        vk.stageFlags = StageFlagsFromRange(r.stageFlags);
        vk.offset     = r.offset;
        vk.size       = r.size;
        vkPush.push_back(vk);
    }

    VkPipelineLayoutCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    ci.setLayoutCount = static_cast<u32>(vkSetLayouts.size());
    ci.pSetLayouts    = vkSetLayouts.data();
    ci.pushConstantRangeCount = static_cast<u32>(vkPush.size());
    ci.pPushConstantRanges    = vkPush.data();

    if (vkCreatePipelineLayout(dev, &ci, nullptr, &layout_) != VK_SUCCESS) {
        std::cerr << "[VulkanPipelineLayout] vkCreatePipelineLayout failed" << std::endl;
        return false;
    }

    state_ = ResourceState::Ready;
    return true;
}

void VulkanPipelineLayout::destroyImpl() {
    if (layout_ == VK_NULL_HANDLE) return;
    VkDevice dev = static_cast<VulkanDevice&>(device_).GetNativeDevice();
    VkPipelineLayout l = layout_;
    device_.GetGarbageCollector().DeferredDestroy([dev, l]() {
        if (dev != VK_NULL_HANDLE && l != VK_NULL_HANDLE) vkDestroyPipelineLayout(dev, l, nullptr);
    });
    layout_ = VK_NULL_HANDLE;
}

} // namespace primal::graphics::rhi

#endif // ENABLE_VULKAN
