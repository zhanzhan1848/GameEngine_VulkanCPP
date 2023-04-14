#include "VulkanContent.h"
#include "VulkanHelpers.h"
#include "VulkanResources.h"
#include "VulkanCore.h"
#include "VulkanTexture.h"
#define STB_IMAGE_IMPLEMENTATION
#include "Content/stb_image.h"


namespace primal::graphics::vulkan
{
	namespace
	{
		
	} // anonymous namespace

	namespace texture
	{
		namespace
		{
			
		} // anonymous namespace

		std::atomic<u32> vulkan_texture_2d::_count = 0;

		vulkan_texture_2d::vulkan_texture_2d(std::string path) : _entity_id{ ++_count }
		{
			loadTexture(path);
		}

		vulkan_texture_2d::~vulkan_texture_2d()
		{
			vkDestroySampler(core::logical_device(), _texture.sampler, nullptr);
			destroy_image(core::logical_device(), &_texture);
		}

		void vulkan_texture_2d::loadTexture(std::string path)
		{
			int texWidth, texHeight, texChannels;
			void* pixels = stbi_load(path.c_str(), &texWidth, &texHeight, &texChannels, STBI_rgb_alpha);
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
			createBuffer(core::logical_device(), imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging.buffer, staging.memory);
			vkBindBufferMemory(core::logical_device(), staging.buffer, staging.memory, 0);

			void* data;
			vkMapMemory(core::logical_device(), staging.memory, 0, imageSize, 0, &data);
			memcpy(data, pixels, static_cast<size_t>(imageSize));
			vkUnmapMemory(core::logical_device(), staging.memory);

			stbi_image_free(pixels);

			image_init_info image_info{};
			image_info.image_type = VK_IMAGE_TYPE_2D;
			image_info.width = texWidth;
			image_info.height = texHeight;
			image_info.format = imageFormat;
			image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
			image_info.usage_flags = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
			image_info.memory_flags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
			image_info.create_view = true;
			image_info.view_aspect_flags = VK_IMAGE_ASPECT_COLOR_BIT;

			create_image(core::logical_device(), &image_info, _texture);
			transitionImageLayout(core::logical_device(), core::graphics_family_queue_index(), core::get_current_command_pool(), _texture.image, imageFormat, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
			copyBufferToImage(core::logical_device(), core::graphics_family_queue_index(), core::get_current_command_pool(), staging.buffer, _texture.image, static_cast<u32>(texWidth), static_cast<u32>(texHeight));
			transitionImageLayout(core::logical_device(), core::graphics_family_queue_index(), core::get_current_command_pool(), _texture.image, imageFormat, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

			vkDestroyBuffer(core::logical_device(), staging.buffer, nullptr);
			vkFreeMemory(core::logical_device(), staging.memory, nullptr);

			VkPhysicalDeviceProperties properties;
			vkGetPhysicalDeviceProperties(core::physical_device(), &properties);

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
			VkCall(result = vkCreateSampler(core::logical_device(), &samplerInfo, nullptr, &_texture.sampler), "Failed to create texture sampler...");
		}
	}
}