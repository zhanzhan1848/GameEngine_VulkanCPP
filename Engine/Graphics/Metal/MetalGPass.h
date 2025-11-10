#pragma once

#include "MetalCommonHeaders.h"

namespace primal::graphics::metal
{
	struct metal_frame_info;
}

namespace primal::graphics::metal::gpass
{
    constexpr MTL::PixelFormat main_buffer_format{ MTL::PixelFormatRGBA16Float };
    constexpr MTL::PixelFormat depth_buffer_format{ MTL::PixelFormatDepth32Float };
    
    struct opaque_root_parameter
	{
		enum parameter : u32
		{
			global_shader_data,
			per_object_data,
			position_buffer,
			element_buffer,
			srv_indices,
			directional_lights,
			// cullable_lights,
			// light_grid,
			// light_index_list,

			count
		};
	};

    bool initialize();
	void shutdown();

    [[nodiscard]] const metal_render_texture& get_main_buffer();
    [[nodiscard]] const metal_texture& get_depth_buffer();
	[[nodiscard]] const metal_render_texture& get_normal_depth_buffer();
	[[nodiscard]] const metal_render_texture& get_albedo_buffer();
    
    // NOTE: call this every frame before rendering anything in gpass
	void set_size(math::u32v2 size);
    void depth_prepass(MTL::CommandBuffer* buffer, const metal_frame_info& frame_info);
    void render(MTL::CommandBuffer* buffer, const metal_frame_info& frame_info);
}