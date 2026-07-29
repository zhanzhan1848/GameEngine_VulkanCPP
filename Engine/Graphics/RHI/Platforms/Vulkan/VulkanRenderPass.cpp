/**
 * @file VulkanRenderPass.cpp
 * @brief VulkanRenderPass 实现
 * @author GameEngine VulkanCPP Team
 * @date 2026-07-26
 */

#include "VulkanRenderPass.h"
#include "VulkanDevice.h"
#include "VulkanMath.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

#include <iostream>
#include <vector>

namespace primal::graphics::rhi {

namespace {

VkAttachmentLoadOp ToVkLoadOp(LoadAction a) {
    switch (a) {
        case LoadAction::Load:     return VK_ATTACHMENT_LOAD_OP_LOAD;
        case LoadAction::Clear:    return VK_ATTACHMENT_LOAD_OP_CLEAR;
        case LoadAction::DontCare:
        default:                   return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    }
}

VkAttachmentStoreOp ToVkStoreOp(StoreAction a) {
    switch (a) {
        case StoreAction::Store:   return VK_ATTACHMENT_STORE_OP_STORE;
        case StoreAction::DontCare:
        default:                   return VK_ATTACHMENT_STORE_OP_DONT_CARE;
    }
}

bool IsDepthFormat(DataFormat f) {
    return f == DataFormat::D32_Float ||
           f == DataFormat::D24_UNorm_S8_UInt ||
           f == DataFormat::D32_Float_S8X24_UInt;
}

bool IsStencilFormat(DataFormat f) {
    return f == DataFormat::D24_UNorm_S8_UInt ||
           f == DataFormat::D32_Float_S8X24_UInt;
}

} // anonymous namespace

VulkanRenderPass::VulkanRenderPass(VulkanDevice& device, const RenderPassDesc& desc)
    : RHIRenderPass(device, desc) {}

bool VulkanRenderPass::Initialize() {
    VkDevice dev = static_cast<VulkanDevice&>(device_).GetNativeDevice();

    std::vector<VkAttachmentDescription> attachments;
    std::vector<VkAttachmentReference> colorRefs;

    const u32 colorCount = static_cast<u32>(desc_.colorAttachments.size());
    colorRefs.reserve(colorCount);

    for (u32 i = 0; i < colorCount; ++i) {
        const auto& a = desc_.colorAttachments[i];
        VkAttachmentDescription d{};
        d.format         = vulkan::ToVkFormat(a.format);
        d.samples        = VK_SAMPLE_COUNT_1_BIT;
        d.loadOp         = ToVkLoadOp(a.loadOp);
        d.storeOp        = ToVkStoreOp(a.storeOp);
        d.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        d.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        d.initialLayout  = (a.loadOp == LoadAction::Load) ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                                          : VK_IMAGE_LAYOUT_UNDEFINED;
        d.finalLayout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachments.push_back(d);
        colorRefs.push_back({ i, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL });
    }

    VkAttachmentReference depthRef{};
    bool hasDepth = (desc_.depthAttachment.texture != handles::INVALID_RESOURCE &&
                     desc_.depthAttachment.format != DataFormat::Unknown);
    if (hasDepth) {
        const auto& a = desc_.depthAttachment;
        VkAttachmentDescription d{};
        d.format         = vulkan::ToVkFormat(a.format);
        d.samples        = VK_SAMPLE_COUNT_1_BIT;
        d.loadOp         = ToVkLoadOp(a.loadOp);
        d.storeOp        = ToVkStoreOp(a.storeOp);
        d.stencilLoadOp  = IsStencilFormat(a.format) ? ToVkLoadOp(a.loadOp) : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        d.stencilStoreOp = IsStencilFormat(a.format) ? ToVkStoreOp(a.storeOp) : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        d.initialLayout  = (a.loadOp == LoadAction::Load) ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                                                          : VK_IMAGE_LAYOUT_UNDEFINED;
        d.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        attachments.push_back(d);
        depthRef.attachment = colorCount;
        depthRef.layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint        = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount     = colorCount;
    subpass.pColorAttachments        = colorRefs.data();
    subpass.pDepthStencilAttachment  = hasDepth ? &depthRef : nullptr;

    VkRenderPassCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    ci.attachmentCount = static_cast<u32>(attachments.size());
    ci.pAttachments    = attachments.data();
    ci.subpassCount    = 1;
    ci.pSubpasses      = &subpass;

    if (vkCreateRenderPass(dev, &ci, nullptr, &renderPass_) != VK_SUCCESS) {
        std::cerr << "[VulkanRenderPass] vkCreateRenderPass failed" << std::endl;
        return false;
    }

    state_ = ResourceState::Ready;
    return true;
}

void VulkanRenderPass::destroyImpl() {
    if (renderPass_ == VK_NULL_HANDLE) return;
    VkDevice dev = static_cast<VulkanDevice&>(device_).GetNativeDevice();
    VkRenderPass rp = renderPass_;
    device_.GetGarbageCollector().DeferredDestroy([dev, rp]() {
        if (dev != VK_NULL_HANDLE && rp != VK_NULL_HANDLE) vkDestroyRenderPass(dev, rp, nullptr);
    });
    renderPass_ = VK_NULL_HANDLE;
}

} // namespace primal::graphics::rhi

#endif // ENABLE_VULKAN
