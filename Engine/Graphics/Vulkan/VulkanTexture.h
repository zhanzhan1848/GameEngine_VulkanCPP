#pragma once
#include "VulkanCommonHeaders.h"

#include "VulkanCore.h"
#include "VulkanResources.h"

namespace primal::graphics::vulkan
{
	
	void transitionImageLayout(VkDevice device, u32 index, VkCommandPool pool, VkImage image, VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout);

	void copyBufferToImage(VkDevice device, u32 index, VkCommandPool pool, VkBuffer buffer, VkImage image, u32 width, u32 height);

	void createTextureSampler(VkPhysicalDevice physicalDevice, VkDevice device, vulkan_texture& tex);

	//class vulkan_texture
	//{
	//public:
	//	enum class type : u32
	//	{
	//		texture_1D = 1,
	//		texture_2D,
	//		texture_3D,
	//		texture_cube,
	//		texture_1D_array,
	//		texture_2D_array,
	//		texture_cube_array,

	//		count
	//	};

	//	explicit vulkan_texture(const image_init_info *const init_info, type type)
	//	{
	//		create_image(init_info, type);
	//		allocate_image_memory(init_info);
	//		if (init_info->create_view)
	//		{
	//			create_image_view(init_info, type);
	//			create_sampler();
	//		}
	//	}

	//	~vulkan_texture()
	//	{
	//		vkDeviceWaitIdle(core::logical_device());
	//		if (_sampler) vkDestroySampler(core::logical_device(), _sampler, nullptr);
	//		if (_view) vkDestroyImageView(core::logical_device(), _view, nullptr);
	//		if (_image) vkDestroyImage(core::logical_device(), _image, nullptr);
	//		if (_memory) vkFreeMemory(core::logical_device(), _memory, nullptr);
	//	}

	//	bool update_memory(const u8* const data)
	//	{

	//	}

	//	[[nodiscard]] constexpr VkImage get_image() const { return _image; }
	//	[[nodiscard]] constexpr VkImageView get_view() const { return _view; }
	//	[[nodiscard]] constexpr VkSampler get_sampler() const { return _sampler; }

	//private:
	//	void create_image(const image_init_info *const init_info, type type)
	//	{
	//		VkResult result{ VK_SUCCESS };
	//		vulkan_texture_setting texture_type{ type };

	//		// Create image
	//		VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	//		info.pNext = nullptr;
	//		info.imageType = texture_type.image_type;					// TODO: This should be configurable
	//		info.extent.width = init_info->width;
	//		info.extent.height = init_info->height;
	//		info.extent.depth = 1;								// TODO: should be configurable and supported
	//		info.mipLevels = init_info->mipmap;										// TODO: should be configurable, and need to support mip maps
	//		info.arrayLayers = init_info->layer_count;								// TODO: should be configurable and offer image layer support
	//		info.format = init_info->format;
	//		info.tiling = init_info->tiling;
	//		info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	//		info.usage = init_info->usage_flags;
	//		info.samples = VK_SAMPLE_COUNT_1_BIT;				// TODO: make configurable
	//		info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;		// TODO: make configurable

	//		VkCall(result = vkCreateImage(core::logical_device(), &info, nullptr, &_image), "Failed to create image...");
	//	}

	//	void allocate_image_memory(const image_init_info *const init_info)
	//	{
	//		assert(_image);
	//		if (!_image) return;
	//		VkResult result{ VK_SUCCESS };

	//		VkMemoryRequirements memory_reqs;
	//		vkGetImageMemoryRequirements(core::logical_device(), _image, &memory_reqs);

	//		s32 index{ core::find_memory_index(memory_reqs.memoryTypeBits, init_info->memory_flags) };
	//		if (index == -1)
	//		{
	//			ERROR_MSSG("The required memory type was not found...");
	//		}

	//		// Allocate memory for image
	//		VkMemoryAllocateInfo info{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	//		info.allocationSize = memory_reqs.size;
	//		info.memoryTypeIndex = index;

	//		VkCall(result = vkAllocateMemory(core::logical_device(), &info, nullptr, &_memory), "Failed to allocate memory for image...");

	//		// TODO: make memory offset configurable, for use in things like image pooling.
	//		VkCall(result = vkBindImageMemory(core::logical_device(), _image, _memory, 0), "Failed to bind image memory...");
	//	}

	//	void create_image_view(const image_init_info *const init_info, type type)
	//	{
	//		VkImageViewCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	//		info.image = _image;
	//		info.viewType = VK_IMAGE_VIEW_TYPE_2D;								// TODO: This should be configurable
	//		info.format = init_info->format;
	//		info.subresourceRange.aspectMask = init_info->view_aspect_flags;
	//		info.subresourceRange.baseMipLevel = 0;													// TODO: This should be configurable
	//		info.subresourceRange.levelCount = init_info->mipmap;									// TODO: This should be configurable
	//		info.subresourceRange.baseArrayLayer = 0;												// TODO: This should be configurable
	//		info.subresourceRange.layerCount = init_info->layer_count;								// TODO: This should be configurable

	//		VkResult result{ VK_SUCCESS };
	//		VkCall(result = vkCreateImageView(core::logical_device(), &info, nullptr, &_view), "Failed to create image view...");
	//	}

	//	void create_sampler()
	//	{
	//		VkResult result{ VK_SUCCESS };
	//		VkSamplerCreateInfo sampler{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	//		sampler.pNext = nullptr;
	//		sampler.magFilter = VK_FILTER_LINEAR;
	//		sampler.minFilter = VK_FILTER_LINEAR;
	//		sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	//		sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	//		sampler.addressModeV = sampler.addressModeU;
	//		sampler.addressModeW = sampler.addressModeU;
	//		sampler.mipLodBias = 0.f;
	//		sampler.maxAnisotropy = 1.f;
	//		sampler.minLod = 0.f;
	//		sampler.maxLod = 1.f;
	//		sampler.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
	//		result = VK_SUCCESS;
	//		VkCall(result = vkCreateSampler(core::logical_device(), &sampler, nullptr, &_sampler), "Failed to create GBuffer(shadow mapping) image sampler...");
	//	}

	//	struct vulkan_texture_setting
	//	{
	//		VkImageType			image_type;
	//		VkImageViewType		view_type;
	//		vulkan_texture_setting(type type)
	//		{
	//			switch (type)
	//			{
	//			case primal::graphics::vulkan::vulkan_texture::type::texture_1D:
	//			{
	//				image_type = VK_IMAGE_TYPE_1D;
	//				view_type = VK_IMAGE_VIEW_TYPE_1D;
	//			}	break;
	//			case primal::graphics::vulkan::vulkan_texture::type::texture_2D:
	//			{
	//				image_type = VK_IMAGE_TYPE_2D;
	//				view_type = VK_IMAGE_VIEW_TYPE_2D;
	//			} break;
	//			case primal::graphics::vulkan::vulkan_texture::type::texture_3D:
	//			{
	//				image_type = VK_IMAGE_TYPE_3D;
	//				view_type = VK_IMAGE_VIEW_TYPE_3D;
	//			} break;
	//			case primal::graphics::vulkan::vulkan_texture::type::texture_cube:
	//			{
	//				image_type = VK_IMAGE_TYPE_2D;
	//				view_type = VK_IMAGE_VIEW_TYPE_CUBE;
	//			} break;
	//			case primal::graphics::vulkan::vulkan_texture::type::texture_1D_array:
	//			{
	//				image_type = VK_IMAGE_TYPE_1D;
	//				view_type = VK_IMAGE_VIEW_TYPE_1D_ARRAY;
	//			} break;
	//			case primal::graphics::vulkan::vulkan_texture::type::texture_2D_array:
	//			{
	//				image_type = VK_IMAGE_TYPE_2D;
	//				view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
	//			} break;
	//			case primal::graphics::vulkan::vulkan_texture::type::texture_cube_array:
	//			{
	//				image_type = VK_IMAGE_TYPE_2D;
	//				view_type = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
	//			} break;
	//			assert(image_type != VkImageType::VK_IMAGE_TYPE_MAX_ENUM && image_type < 3 && image_type > -1);
	//			assert(view_type != VkImageViewType::VK_IMAGE_VIEW_TYPE_MAX_ENUM && view_type < 7 && view_type > -1);
	//			}
	//		}
	//	};

	//	VkImage					_image;
	//	VkImageView				_view;
	//	VkSampler				_sampler;
	//	VkDeviceMemory			_memory;
	//	u32						_width;
	//	u32						_height;
	//};
}