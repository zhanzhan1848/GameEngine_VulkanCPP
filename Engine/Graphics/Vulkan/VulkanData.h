#pragma once
#include "VulkanCommonHeaders.h"

namespace primal::graphics::vulkan::data
{
	// Vulkan :: Descriptor Pool Set SetLayout / Pipeline layout / Renderpass / framebuffer 等都放在这里

	struct vulkan_buffer
	{
		enum type : u32
		{
			static_vertex_buffer,
			static_index_buffer,
			static_instance_buffer,
			static_uniform_buffer,
			per_frame_update_uniform_buffer,
			static_storage_buffer,
			per_frame_update_storage_buffer,

			count
		};
		VkBuffer					cpu_address;
		VkDeviceMemory				cpu_memory;
		VkBuffer					gpu_address;
		VkDeviceMemory				gpu_memory;

		void*						data{ nullptr };
		u32							size;
		u32							flags;
		type						own_type;

		vulkan_buffer() = delete;
		vulkan_buffer(type type, [[maybe_unused]] u32 size = 0);
		~vulkan_buffer();

		void resize(u64 size);
		void update(const void* const data, size_t size, u32 offset_count = 0);
		void convert_to_local_device_buffer();
		void release();

		id::id_type convert_to_image();
	};

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
			vulkan_buffer,

			count
		};
	};

	bool initialize();
	void shutdown();

	id::id_type create_data(engine_vulkan_data::data_type type, const void* const data, [[maybe_unused]] u32 size = 0);

	void remove_data(engine_vulkan_data::data_type type, id::id_type id);

	void get_data(engine_vulkan_data::data_type type, void* const data, id::id_type id, [[maybe_unused]] u32 size = 0);

	template<typename T>
	T& get_data(id::id_type id);
}


namespace primal::graphics::vulkan
{
	
}