#pragma once
#include "VulkanCommonHeaders.h"

namespace primal::graphics::vulkan
{
	class vulkan_surface;
	class vulkan_geometry_pass;
}

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

	void initialize(vulkan_geometry_pass* gpass);
	void shutdown();
	void run(const void* const data, u32 size);
	void frustum_run();
	void frustum_submit();

	void culling_light_run();
	void culling_light_submit(u32 wait_nums, VkSemaphore* pWait);

	VkSemaphore get_compute_signal_semaphore();
	VkSemaphore get_culling_signal_semaphore();

	id::id_type get_input_buffer_id();
	id::id_type get_output_buffer_id();
	id::id_type culling_light_grid();
	id::id_type culling_light_list();
	id::id_type culling_in_light_count_id();

	bool is_rendered();
}