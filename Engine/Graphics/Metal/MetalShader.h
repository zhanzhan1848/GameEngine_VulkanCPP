#pragma once

#include "MetalCommonHeaders.h"

namespace primal::graphics::metal::shader
{
	using engine_metal_shader_function = std::unique_ptr<MTL::Function, void(*)(MTL::Function*)>;

    struct engine_shader
	{
		enum id : u32
		{
			fullscreen_triangle_vs = 0,
			post_process_ps,

			count
		};
	};

    bool initialize();
    void shutdown();

	engine_metal_shader_function get_engine_shader(engine_shader::id id);
}