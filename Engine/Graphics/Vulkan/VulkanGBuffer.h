#pragma once
#include "VulkanCommonHeaders.h"
#include "VulkanShader.h"

namespace primal::graphics::vulkan
{
	class vulkan_surface;

	class vulkan_base_pass
	{
	public:
		virtual void createRenderpassAndFramebuffer() = 0;
		virtual void createPoolAndLayout() = 0;
		virtual void runRenderpass(vulkan_cmd_buffer cmd_buffer, vulkan_surface* surface) = 0;
	};

	class vulkan_geometry_pass
	{
	public:
		vulkan_geometry_pass() = default;

		explicit vulkan_geometry_pass(u32 width, u32 height) : _width{ width }, _height{ height } {}

		DISABLE_COPY_AND_MOVE(vulkan_geometry_pass);

		~vulkan_geometry_pass();

		void setSize(u32 width, u32 height) { _width = width; _height = height; }

		void createUniformBuffer();

		void create_semaphore();

		void setupRenderpassAndFramebuffer();

		void setupPoolAndLayout();

		void runRenderpass(vulkan_cmd_buffer cmd_buffer, vulkan_surface* surface);

		void submit(vulkan_cmd_buffer cmd_buffer, VkQueue graphics_queue);


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

		// Output
		utl::vector<id::id_type>						_image_ids;
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
}