#pragma once
#include "VulkanCommonHeaders.h"
#include "VulkanShader.h"

namespace primal::graphics::vulkan
{
	class vulkan_surface;

	struct uboSSAOParam
	{
		math::m4x4		projection;
		u32				ssao = true;
		u32				ssaoOnly = false;
		u32				ssaoblur = true;
	};

	struct uboGBuffer : UniformBufferObject
	{
		float nearPlane = 0.1f;
		float farPlane = 64.0f;
	};

	class vulkan_base_pass
	{
	public:
		virtual void createUniformbuffer() = 0;
		virtual void setupRenderpassAndFramebuffer() = 0;
		virtual void setupPoolAndLayout() = 0;
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

		void setupRenderpassAndFramebuffer();

		void setupPoolAndLayout();

		void runRenderpass(vulkan_cmd_buffer cmd_buffer, vulkan_surface* surface);


		[[nodiscard]] utl::vector<id::id_type> getTexture() { return _image_ids; }

		[[nodiscard]] constexpr id::id_type getFramebuffer() { return _framebuffer_id; }

		[[nodiscard]] constexpr id::id_type getRenderpass() { return _renderpass_id; }

		[[nodiscard]] constexpr id::id_type getDescriptorPool() { return _descriptor_pool_id; }

		[[nodiscard]] constexpr id::id_type getDescriptorSetLayout() { return _descriptor_set_layout_id; }

		[[nodiscard]] constexpr id::id_type getLightDescriptorSetLayout() { return _light_descriptor_set_layout_id; }

		[[nodiscard]] constexpr id::id_type getPipelineLayout() { return _pipeline_layout_id; }

		[[nodiscard]] constexpr id::id_type getUniformbuffer() { return _ub_id; }

	private:
		u32												_width;
		u32												_height;
		id::id_type										_ub_id;
		id::id_type										_framebuffer_id;
		id::id_type										_renderpass_id;
		id::id_type										_descriptor_pool_id;
		id::id_type										_descriptor_set_layout_id;
		id::id_type										_light_descriptor_set_layout_id;
		id::id_type										_pipeline_layout_id;

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
	};
}