#pragma once
#include "VulkanCommonHeaders.h"
#include "VulkanShader.h"

namespace primal::graphics::vulkan
{
	class vulkan_surface;

	class vulkan_geometry_pass
	{
	public:
		vulkan_geometry_pass() = default;

		explicit vulkan_geometry_pass(u32 width, u32 height) : _width{ width }, _height{ height } {}

		DISABLE_COPY_AND_MOVE(vulkan_geometry_pass);

		~vulkan_geometry_pass();

		void setSize(u32 width, u32 height);

		void createUniformBuffer();

		void create_semaphore();

		void setupRenderpassAndFramebuffer();

		void setupPoolAndLayout();

		void create_command_buffer();

		void runRenderpass(vulkan_cmd_buffer cmd_buffer, vulkan_surface* surface);

		void run(vulkan_surface* surface);

		void submit(vulkan_surface * surface);


		[[nodiscard]] utl::vector<id::id_type> getTexture() { return _image_ids; }

		[[nodiscard]] constexpr id::id_type getFramebuffer() { return _framebuffer_id; }

		[[nodiscard]] constexpr id::id_type getRenderpass() { return _renderpass_id; }

		[[nodiscard]] constexpr id::id_type getDescriptorPool() { return _descriptor_pool_id; }

		[[nodiscard]] constexpr id::id_type getDescriptorSetLayout() { return _descriptor_set_layout_id; }

		[[nodiscard]] constexpr id::id_type getLightDescriptorSetLayout() { return _light_descriptor_set_layout_id; }

		[[nodiscard]] constexpr id::id_type getPipelineLayout() { return _pipeline_layout_id; }

		[[nodiscard]] constexpr VkSemaphore const get_signal_semaphore() const { return _signal_semaphore; }

	private:
		u32												_width;
		u32												_height;
		id::id_type										_framebuffer_id;
		id::id_type										_renderpass_id;
		id::id_type										_descriptor_pool_id;
		id::id_type										_descriptor_set_layout_id;
		id::id_type										_light_descriptor_set_layout_id;
		id::id_type										_pipeline_layout_id;
		VkSemaphore										_signal_semaphore;
		VkSemaphore										_wait_semaphore;
		VkQueue											_graphics_queue;

		utl::vector<vulkan_cmd_buffer>					_cmd_buffers;
		utl::vector<vulkan_fence>						_draw_fences;
		vulkan_fence**									_fences_in_flight;

		// Output
		utl::vector<id::id_type>						_image_ids;
	private:
		void reset_fence(VkDevice device, vulkan_fence& fence);
		bool wait_for_fence(VkDevice device, vulkan_fence& fence, u64 timeout);
		void destroy_fence(VkDevice device, vulkan_fence& fence);
		bool create_fence(VkDevice device, bool signaled, vulkan_fence& fence);
	};

	class vulkan_final_pass
	{
	public:
		vulkan_final_pass() = default;

		explicit vulkan_final_pass(u32 width, u32 height) : _width{ width }, _height{ height } {}

		DISABLE_COPY_AND_MOVE(vulkan_final_pass);

		~vulkan_final_pass();

		void setSize(u32 width, u32 height) { _width = width; _height = height; }

		void create_semaphore();

		void setupDescriptorSets(utl::vector<id::id_type> image_id, id::id_type ubo_id);

		void setupPipeline(vulkan_renderpass renderpass);

		void render(vulkan_cmd_buffer cmd_buffer);

	private:
		u32												_width;
		u32												_height;
		id::id_type										_descriptor_pool_id;
		id::id_type										_descriptorSet_id;
		id::id_type										_pipeline_id;
		id::id_type										_descriptorSet_layout_id;
		id::id_type										_pipeline_layout_id;
		id::id_type										_light_descriptor_set_layout_id;
	};


	void geometry_initialize(u32 width, u32 height);
	void geometry_run(vulkan_surface* surface);
	void geometry_submit();
	VkSemaphore geometry_semaphore();
	id::id_type geometry_descriptor_pool();
	id::id_type geometry_descriptor_setlayout();
	id::id_type geometry_renderpass();
	id::id_type geometry_pipeline_layout();
}