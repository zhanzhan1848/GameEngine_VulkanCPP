// Copyright (c) Contributors of Primal+
// Distributed under the MIT license. See the LICENSE file in the project root for more information.
#include "VulkanRenderPass.h"
#include "VulkanCore.h"

#include <vector>

namespace primal::graphics::vulkan::renderpass
{
    namespace
    {
        VkSubpassDependency createSubpassDependency(
            VkPipelineStageFlags srcStageMask,
            VkPipelineStageFlags dstStageMask,
            VkAccessFlags srcAccessMask,
            VkAccessFlags dstAccessMask,
            uint32_t srcSubpass = VK_SUBPASS_EXTERNAL,
            uint32_t dstSubpass = 0,
            VkDependencyFlags flags = 0
        )
        {
            VkSubpassDependency dependency;
            dependency.srcSubpass = srcSubpass;
            dependency.dstSubpass = dstSubpass;
            dependency.srcStageMask = srcStageMask;
            dependency.dstStageMask = dstStageMask;
            dependency.srcAccessMask = srcAccessMask;
            dependency.dstAccessMask = dstAccessMask;
            dependency.dependencyFlags = flags;
            return dependency;
        }
    } // anonymous namespace

vulkan_renderpass
create_renderpass(VkDevice device, VkFormat swapchain_image_format, VkFormat depth_format, math::u32v4 render_area, math::v4 clear_color, f32 depth, u32 stencil)
{
    // Create the main subpass
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;

    // Attachments
    // TODO: these are hard coded for now - these need to be configurable in the future
    constexpr u32 attachment_desc_count{ 2 };
    VkAttachmentDescription attachment_desc[attachment_desc_count];

    // Color attachment descriptor
    {
        VkAttachmentDescription desc{};
        desc.format = swapchain_image_format;
        desc.samples = VK_SAMPLE_COUNT_1_BIT;
        desc.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        desc.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        desc.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        desc.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        desc.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;				// We do not expect any particular layout before render pass starts
        desc.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;			// Transitions to a present optomized layout after the render pass
        desc.flags = 0;

        attachment_desc[0] = desc;
    }

    VkAttachmentReference color_attach_ref;
    color_attach_ref.attachment = 0;	// attachment_desc[0] is color attachment
    color_attach_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    // Let subpass know about the color attachment
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_attach_ref;

    // Depth attachment descriptor
    // NOTE: This is being hard coded in... this is something we probably want to make configurable, as we
    // 		 will not always want a depth attachment
    {
        VkAttachmentDescription desc{};
        desc.format = depth_format;
        desc.samples = VK_SAMPLE_COUNT_1_BIT;
        desc.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        desc.storeOp = VK_ATTACHMENT_STORE_OP_STORE; // VK_ATTACHMENT_STORE_OP_DONT_CARE;
        desc.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; // VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        desc.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        desc.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;							// We do not expect any particular layout before render pass starts
        desc.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;	// Transitions to a present optomized layout after the render pass
        desc.flags = 0;

        attachment_desc[1] = desc;
    }

    VkAttachmentReference depth_attach_ref;
    depth_attach_ref.attachment = 1;	// attachment_desc[1] is depth attachment
    depth_attach_ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    // Let subpass know about the depth attachment
    subpass.pDepthStencilAttachment = &depth_attach_ref;
    //subpass.pDepthStencilAttachment = VK_NULL_HANDLE;

    // TODO: there are othere possible attachment types needed to configure here;
    //		 input, resolve, preserve...
    // Input from shader
    subpass.inputAttachmentCount = 0;
    subpass.pInputAttachments = nullptr;

    // Resolve attachments used for multisampling color attachments
    subpass.pResolveAttachments = nullptr;

    // Preverse attachments are not used in this subpass, but are preserved for the nextr subpass
    subpass.preserveAttachmentCount = 0;
    subpass.pPreserveAttachments = nullptr;

    // Render pass dependencies.
    // TODO: this need to be configurable
    std::vector<VkSubpassDependency> dependencies{
        createSubpassDependency(VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT),

        createSubpassDependency(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                0,
                                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT),
    };

    // TODO: Several of these fields are hard coded, and should be configurable in the future
    VkRenderPassCreateInfo info{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    info.pNext = 0;
    info.flags = 0;
    info.attachmentCount = attachment_desc_count;
    info.pAttachments = attachment_desc;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = static_cast<uint32_t>(dependencies.size());
    info.pDependencies = dependencies.data();

    vulkan_renderpass renderpass;
    renderpass.clear_color = clear_color;
    renderpass.render_area = render_area;
    renderpass.depth = depth;
    renderpass.stencil = stencil;

    VkResult result{ VK_SUCCESS };
    VkCall(result = vkCreateRenderPass(device, &info, nullptr, &renderpass.render_pass), "Failed to create render pass...");

    return renderpass;
}

void
destroy_renderpass(VkDevice device, vulkan_renderpass& renderpass)
{
    if (renderpass.render_pass)
    {
        vkDestroyRenderPass(device, renderpass.render_pass, nullptr);
        renderpass.render_pass = nullptr;
    }
}

void
begin_renderpass(VkCommandBuffer cmd_buffer, vulkan_cmd_buffer::state state, vulkan_renderpass& renderpass, VkFramebuffer frame_buffer)
{
    VkClearValue values[2]{};
    values[0].color.float32[0] = renderpass.clear_color.x;
    values[0].color.float32[1] = renderpass.clear_color.y;
    values[0].color.float32[2] = renderpass.clear_color.z;
    values[0].color.float32[3] = renderpass.clear_color.z;
    values[1].depthStencil.depth = renderpass.depth;
    values[1].depthStencil.stencil = renderpass.stencil;

    VkRenderPassBeginInfo info{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    info.renderPass = renderpass.render_pass;
    info.framebuffer = frame_buffer;
    info.renderArea.offset.x = renderpass.render_area.x;
    info.renderArea.offset.y = renderpass.render_area.y;
    info.renderArea.extent.width = renderpass.render_area.z;
    info.renderArea.extent.height = renderpass.render_area.w;
    info.clearValueCount = 2;
    info.pClearValues = values;

    vkCmdBeginRenderPass(cmd_buffer, &info, VK_SUBPASS_CONTENTS_INLINE);
    state = vulkan_cmd_buffer::CMD_IN_RENDER_PASS;
}

void
end_renderpass(VkCommandBuffer cmd_buffer, vulkan_cmd_buffer::state state, vulkan_renderpass& renderpass)
{
    vkCmdEndRenderPass(cmd_buffer);
    state = vulkan_cmd_buffer::CMD_RECORDING;
}
}