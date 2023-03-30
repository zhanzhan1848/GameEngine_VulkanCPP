#pragma once
#include "VulkanCommonHeaders.h"

namespace primal::graphics::vulkan
{
	void createTextureImage(VkDevice device, u32 index, VkCommandPool pool, std::string filePath, vulkan_texture& tex);

	void transitionImageLayout(VkDevice device, u32 index, VkCommandPool pool, VkImage image, VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout);

	void copyBufferToImage(VkDevice device, u32 index, VkCommandPool pool, VkBuffer buffer, VkImage image, u32 width, u32 height);

	void createTextureSampler(VkPhysicalDevice physicalDevice, VkDevice device, vulkan_texture& tex);
}