#pragma once

#include "Graphics/Vulkan/VulkanCommonHeaders.h"

namespace primal::graphics::vulkan::glsl
{
	using mat4 = math::m4x4;
	using mat3 = math::m3x3;
	using vec4 = math::v4;
	using vec3 = math::v3;
	using vec2 = math::v2;
	using ivec4 = math::u32v4;
	using ivec3 = math::u32v3;
	using ivec2 = math::u32v2;
	using uint = u32;
	
#include "CommonTypes.glsli"
}