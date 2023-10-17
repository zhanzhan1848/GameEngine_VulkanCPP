#pragma once
#include "VulkanCommonHeaders.h"

namespace primal::graphics::vulkan
{
	// Vulkan :: Descriptor Pool Set SetLayout / Pipeline layout / Renderpass / framebuffer 等都放在这里

	struct UniformBuffer
	{
		VkBuffer buffer;
		VkDeviceMemory memory;
		void* mapped;
		size_t	size;

		void update(const void* const data, size_t size);

		void resize(size_t size);
	};

	struct engine_vulkan_data
	{
		enum data_type : u32
		{
			vulkan_renderpass = 0,
			vulkan_framebuffer,
			vulkan_descriptor_pool,
			vulkan_descriptor_set_layout,
			vulkan_descriptor_sets,
			vulkan_pipeline_layout,
			vulkan_pipeline,
			vulkan_uniform_buffer,

			count
		};
	};

	id::id_type create_data(engine_vulkan_data::data_type type, const void* const data, [[maybe_unused]] u32 size);

	void remove_data(engine_vulkan_data::data_type type, id::id_type id);

	template<typename T>
	T& get_data(id::id_type id);
}



