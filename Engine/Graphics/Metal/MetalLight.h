#pragma once
#include "MetalCommonHeaders.h"

namespace primal::graphics::metal
{
    struct metal_frame_info;
}

namespace primal::graphics::metal::light
{
    bool initialize();
    void shutdown();

    void create_light_set(u64 light_set_key);
	void remove_light_set(u64 light_set_key);
	graphics::light create(light_init_info& info);
	void remove(light_id id, u64 light_set_key);
	void set_parameter(light_id id, u64 light_set_key, light_parameter::parameter parameter, const void *const data, u32 data_size);
	void get_parameter(light_id id, u64 light_set_key, light_parameter::parameter parameter, void *const data, u32 data_size);

	void update_light_buffers(const metal_frame_info& metal_info);
	MTL::Buffer* non_cullable_light_buffer(u32 frame_index);
	MTL::Buffer* cullable_light_buffer(u32 frame_index);
	MTL::Buffer* culling_info_buffer(u32 frame_index);
	MTL::Buffer* bounding_spheres_buffer(u32 frame_index);
	u32 non_cullable_light_count(u64 light_set_key);
	u32 cullable_light_count(u64 light_set_key);
}