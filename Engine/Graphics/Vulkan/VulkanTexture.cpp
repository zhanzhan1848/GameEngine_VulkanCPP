#include "VulkanTexture.h"
#include "VulkanHelpers.h"
#include "VulkanResources.h"
#define STB_IMAGE_IMPLEMENTATION
#include "Content/stb_image.h"

namespace primal::graphics::vulkan
{
	namespace
	{

	} // anontmous namespace

	void createTextureImage(VkDevice device, u32 index, VkCommandPool pool, std::string filePath, vulkan_texture& tex)
	{
		int texWidth, texHeight, texChannels;
		void* pixels = stbi_load(filePath.c_str(), &texWidth, &texHeight, &texChannels, STBI_rgb_alpha);
		VkDeviceSize imageSize = 0;
		VkFormat imageFormat = VK_FORMAT_UNDEFINED;

		switch (texChannels)
		{
		case 3:
		{
			imageSize = texWidth * texHeight * 4;
			imageFormat = VK_FORMAT_R8G8B8A8_SRGB;
		}break;
		case 4:
		{
			imageSize = texWidth * texHeight * 4;
			imageFormat = VK_FORMAT_R8G8B8A8_SRGB;
		}break;
		default:
			throw std::runtime_error("The texture do not support...");
			break;
		}

		if (!pixels) throw std::runtime_error("Failed to load texture data...");

		baseBuffer staging;
		createBuffer(device, imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging.buffer, staging.memory);
		vkBindBufferMemory(device, staging.buffer, staging.memory, 0);

		void* data;
		vkMapMemory(device, staging.memory, 0, imageSize, 0, &data);
		memcpy(data, pixels, static_cast<size_t>(imageSize));
		vkUnmapMemory(device, staging.memory);

		stbi_image_free(pixels);

		create_image(device, VK_IMAGE_TYPE_2D, texWidth, texHeight, imageFormat, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true, VK_IMAGE_ASPECT_COLOR_BIT, tex);
		transitionImageLayout(device, index, pool, tex.image, imageFormat, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		copyBufferToImage(device, index, pool, staging.buffer, tex.image, static_cast<u32>(texWidth), static_cast<u32>(texHeight));
		transitionImageLayout(device, index, pool, tex.image, imageFormat, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

		vkDestroyBuffer(device, staging.buffer, nullptr);
		vkFreeMemory(device, staging.memory, nullptr);
	}

	void transitionImageLayout(VkDevice device, u32 index, VkCommandPool pool, VkImage image, VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout)
	{
		VkCommandBuffer commandBuffer = beginSingleCommand(device, pool);

		VkImageMemoryBarrier barrier;
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.pNext = nullptr;
		barrier.oldLayout = oldLayout;
		barrier.newLayout = newLayout;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = image;
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.baseMipLevel = 0;
		barrier.subresourceRange.levelCount = 1;
		barrier.subresourceRange.baseArrayLayer = 0;
		barrier.subresourceRange.layerCount = 1;

		VkPipelineStageFlags srcStage;
		VkPipelineStageFlags dstStage;

		if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
		{
			barrier.srcAccessMask = 0;
			barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

			srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
			dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		}
		else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
		{
			barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

			srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
			dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		}
		else
		{
			throw std::invalid_argument("Unsupported layout transition!");
		}

		vkCmdPipelineBarrier(commandBuffer, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);

		endSingleCommand(device, index, pool, commandBuffer);
	}

	void copyBufferToImage(VkDevice device, u32 index, VkCommandPool pool, VkBuffer buffer, VkImage image, u32 width, u32 height)
	{
		VkCommandBuffer commandBuffer = beginSingleCommand(device, pool);

		VkBufferImageCopy region;
		region.bufferOffset = 0;
		region.bufferRowLength = 0;
		region.bufferImageHeight = 0;
		region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.imageSubresource.mipLevel = 0;
		region.imageSubresource.baseArrayLayer = 0;
		region.imageSubresource.layerCount = 1;
		region.imageOffset = { 0, 0, 0 };
		region.imageExtent = { width, height, 1 };

		vkCmdCopyBufferToImage(commandBuffer, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

		endSingleCommand(device, index, pool, commandBuffer);
	}

	void createTextureSampler(VkPhysicalDevice physicalDevice, VkDevice device, vulkan_texture& tex)
	{
		VkPhysicalDeviceProperties properties;
		vkGetPhysicalDeviceProperties(physicalDevice, &properties);

		VkSamplerCreateInfo samplerInfo;
		samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		samplerInfo.pNext = nullptr;
		samplerInfo.flags = /*VK_SAMPLER_CREATE_SUBSAMPLED_BIT_EXT | */VK_SAMPLER_CREATE_SUBSAMPLED_COARSE_RECONSTRUCTION_BIT_EXT;
		samplerInfo.magFilter = VK_FILTER_LINEAR;
		samplerInfo.minFilter = VK_FILTER_LINEAR;
		samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		samplerInfo.mipLodBias = 0.0f;
		samplerInfo.minLod = 0.0f;
		samplerInfo.maxLod = 0.0f;
		samplerInfo.anisotropyEnable = VK_TRUE;
		samplerInfo.maxAnisotropy = properties.limits.maxSamplerAnisotropy;
		samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
		samplerInfo.unnormalizedCoordinates = VK_FALSE;
		samplerInfo.compareEnable = VK_FALSE;
		samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
		samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;

		VkResult result{ VK_SUCCESS };
		VkCall(result = vkCreateSampler(device, &samplerInfo, nullptr, &tex.sampler), "Failed to create texture sampler...");
	}
}