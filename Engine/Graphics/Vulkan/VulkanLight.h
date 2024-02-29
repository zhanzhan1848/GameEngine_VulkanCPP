#pragma once
#include "VulkanCommonHeaders.h"

namespace primal::graphics::vulkan
{
	class vulkan_surface;
}

namespace primal::graphics::vulkan::light
{
	bool initialize();

	graphics::light create(light_init_info);
	void remove(light_id id, u64 light_set_key);
	void set_parameter(light_id id, u64 light_set_key, light_parameter::parameter parameter, const void* const data, u32 data_size);
	void get_parameter(light_id id, u64 light_set_key, light_parameter::parameter parameter, void* const data, u32 data_size);

	void update_light_buffers(const frame_info& info);
	u32 non_cullable_light_count(u64 light_set_key);
	u32 cullable_light_count(u64 light_set_key);
	id::id_type non_cullable_light_buffer_id();
	id::id_type cullable_light_buffer_id();
	id::id_type culling_info_buffer_id();
}