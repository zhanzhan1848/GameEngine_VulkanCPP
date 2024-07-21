// Copyright (c) Contributors of Primal+
// Distributed under the MIT license. See the LICENSE file in the project root for more information.
#pragma once
#include "VulkanCommonHeaders.h"

namespace primal::graphics::vulkan
{

    struct image_init_info
    {
        VkImageType             image_type;
        u32                     width;
        u32                     height;
        u32                     mipmap;
        u32                     layer_count;
        VkFormat                format;
        VkImageTiling           tiling;
        VkImageUsageFlags       usage_flags;
        VkMemoryPropertyFlags   memory_flags;
        bool                    create_view;
        VkImageAspectFlags      view_aspect_flags;

        image_init_info() : image_type{ VK_IMAGE_TYPE_2D }, width{ 800 }, height{ 600 }, mipmap{ 1 }, layer_count{ 1 }, format{VK_FORMAT_R8G8_SRGB}, tiling{VK_IMAGE_TILING_OPTIMAL}, usage_flags{VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT},
            memory_flags{ VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT }, create_view{ true }, view_aspect_flags{ VK_IMAGE_ASPECT_COLOR_BIT } {}
    };

bool create_image(VkDevice device, const image_init_info *const init_info, vulkan_image& image);
bool create_image_view(VkDevice device, VkFormat format, vulkan_image* image, VkImageAspectFlags view_aspect_flags);
void destroy_image(VkDevice device, vulkan_image* image);

bool create_framebuffer(VkDevice device, vulkan_renderpass& renderpass, u32 width, u32 height, u32 attach_count, utl::vector<VkImageView> attachments, vulkan_framebuffer& framebuffer);
void destroy_framebuffer(VkDevice device, vulkan_framebuffer& framebuffer);
}