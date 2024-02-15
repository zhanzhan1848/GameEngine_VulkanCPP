#include "VulkanTexture.h"
#include "VulkanHelpers.h"
#include "VulkanResources.h"
#include "VulkanCommandBuffer.h"
#include "VulkanCore.h"
//#define STB_IMAGE_IMPLEMENTATION
//#include "Content/stb_image.h"

namespace primal::graphics::vulkan
{
	namespace
	{

	} // anontmous namespace

	void transitionImageLayout(VkDevice device, u32 index, VkCommandPool pool, VkImage image, VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout)
	{
		vulkan_cmd_buffer cmdBuffer{ allocate_cmd_buffer_begin_single_use(device, pool) };

		VkImageSubresourceRange subresourceRange = {};
		subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		subresourceRange.baseMipLevel = 0;
		subresourceRange.levelCount = 1;
		subresourceRange.layerCount = 1;

		if (newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
		{
			subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;

			if (format == VK_FORMAT_D32_SFLOAT_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT)
			{
				subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
			}
		}
		else
		{
			subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		}

		VkPipelineStageFlags srcStage;
		VkPipelineStageFlags dstStage;

		if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
		{
			srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
			dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		}
		else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
		{
			srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
			dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		}
		else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
		{
			srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
			dstStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
		}
		else
		{
			throw std::invalid_argument("Unsupported layout transition!");
		}

		setImageLayout(cmdBuffer.cmd_buffer, image, oldLayout, newLayout, subresourceRange, srcStage, dstStage);

		VkQueue queue;
		vkGetDeviceQueue(device, index, 0, &queue);

		end_cmd_buffer_single_use(device, pool, cmdBuffer, queue);
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