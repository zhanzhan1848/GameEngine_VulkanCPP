#pragma once
#include "VulkanCommonHeaders.h"

namespace primal::graphics::vulkan::compute
{
	struct compute_pass_type
	{
		enum type : u32
		{
			no_input_buffer_output,
			no_input_image_output,
			buffer_input_buffer_output,
			buffer_input_image_output,
			image_input_buffer_output,
			image_input_image_output,

			count
		};
	};

	bool initialize();
	void shutdown();
	void run(const void* const data, u32 size);
	void submit();

	id::id_type get_output_tex_id();
	VkSemaphore get_compute_semaphore();
	void outputImageData();
	VkBuffer& get_output_buffer();
}