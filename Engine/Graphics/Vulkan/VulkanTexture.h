#pragma once
#include "VulkanCommonHeaders.h"

#include "VulkanCore.h"
#include "VulkanResources.h"

namespace primal::graphics::vulkan
{
	
	void transitionImageLayout(VkDevice device, u32 index, VkCommandPool pool, VkImage image, VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout);

	void copyBufferToImage(VkDevice device, u32 index, VkCommandPool pool, VkBuffer buffer, VkImage image, u32 width, u32 height);

	void createTextureSampler(VkPhysicalDevice physicalDevice, VkDevice device, vulkan_texture& tex);

	template<typename T>
	class vulkan_texture_base
	{
		void create_image()
		{
			std::shared_ptr<T> image = static_cast<std::shared_ptr<T>>(this);
			image->create_image();
		}

		void create_image_view()
		{
			std::shared_ptr<T> image = static_cast<std::shared_ptr<T>>(this);
			image->create_image_view();
		}

		void create_sampler()
		{
			std::shared_ptr<T> image = static_cast<std::shared_ptr<T>>(this);
			image->create_sampler();
		}
	};

	class vulkan_texture_1d : public vulkan_texture_base<vulkan_texture_1d>
	{
	public:
		void create_image(image_init_info* init_info)
		{
            VkResult result{ VK_SUCCESS };
            this->_tex.width = init_info->width;
			this->_tex.height = init_info->height;

            VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
            info.pNext = nullptr;
            info.imageType = init_info->image_type;					// TODO: This should be configurable
            info.extent.width = init_info->width;
            info.extent.height = init_info->height;
            info.extent.depth = 1;								// TODO: should be configurable and supported
            info.mipLevels = 1;									// TODO: should be configurable, and need to support mip maps
            info.arrayLayers = 1;								// TODO: should be configurable and offer image layer support
            info.format = init_info->format;
            info.tiling = init_info->tiling;
            info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            info.usage = init_info->usage_flags;
            info.samples = VK_SAMPLE_COUNT_1_BIT;				// TODO: make configurable
            info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;		// TODO: make configurable

            VkCall(result = vkCreateImage(core::logical_device(), &info, nullptr, &this->_tex.image), "Failed to create image...");
		}
	private:
		vulkan_texture					_tex;
	};
}