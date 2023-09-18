#pragma once
#include "VulkanCommonHeaders.h"
#include "VulkanShader.h"

namespace primal::graphics::vulkan
{
	class vulkan_surface;

	class vulkan_shadowmapping
	{
	public:

		vulkan_shadowmapping() = default;

		DISABLE_COPY_AND_MOVE(vulkan_shadowmapping);

		~vulkan_shadowmapping();

		void createUniformBuffer();

		void updateUniformBuffer();

		void setupRenderpass();

		void setupFramebuffer();

		void setupDescriptorSets(VkDescriptorPool pool, VkDescriptorSetLayout layout);

		void setupPipeline(VkPipelineLayout layout);

		void runRenderpass(vulkan_cmd_buffer cmd_buffer, vulkan_surface* surface);

		[[nodiscard]] vulkan_texture getTexture();
		[[nodiscard]] constexpr UniformBufferObject const getDepthMVP() const { return _ubo; }
		[[nodiscard]] constexpr math::v3 const lightPos() const { return _lightPos; }
		[[nodiscard]] constexpr f32	const lightNear() const { return _near; }
		[[nodiscard]] constexpr f32 const lightFar() const { return _far; }


	private:
		u32												_shadowmapping_dim{ 2048 };
		f32												_depthBiasConstant{ 1.25f };
		f32												_depthBiasSlope{ 1.75f };
		UniformBufferObject								_ubo;
		uniformBuffer									_uniformbuffer;
		vulkan_framebuffer								_framebuffer;
		vulkan_renderpass								_renderpass;
		id::id_type										_image_id;
		id::id_type										_descriptorSet_id;
		id::id_type										_pipeline_id;
		shaders::vulkan_shader							_shader;
		math::v3										_lightPos;
		f32												_near{ 0.1f };
		f32												_far{ 64.f };
	};

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

	class vulkan_offscreen
	{
	public:
		vulkan_offscreen() = default;

		explicit vulkan_offscreen(u32 width, u32 height) : _width{width},  _height{height} {}

		DISABLE_COPY_AND_MOVE(vulkan_offscreen);

		~vulkan_offscreen();

		void setSize(u32 width, u32 height) { _width = width; _height = height; }

		void createUniformBuffer();

		void updateUniformBuffer();

		void setupRenderpass();

		void setupFramebuffer();

		void setupDescriptorSets(VkDescriptorPool pool, id::id_type id);

		void setupPipeline();

		void runRenderpass(vulkan_cmd_buffer cmd_buffer, vulkan_surface* surface);

		[[nodiscard]] uniformBuffer& get_uniform_buffer() { return _ub; }
		[[nodiscard]] uniformBuffer& get_ssao_param() { return _ssaoParams_ub; }

		[[nodiscard]] utl::vector<id::id_type> getTexture() { return _image_ids; }

	private:
		u32												_width;
		u32												_height;
		uboGBuffer										_ubo;
		uboSSAOParam									_ssaoParams_ubo;
		uniformBuffer									_ub;
		uniformBuffer									_ssaoKernel_ub;
		uniformBuffer									_ssaoParams_ub;
		utl::vector<vulkan_framebuffer>					_framebuffers;
		utl::vector<vulkan_renderpass>					_renderpasses;
		utl::vector<id::id_type>						_image_ids;
		id::id_type										_descriptorSet_id;
		id::id_type										_descriptorSet_color_id;
		id::id_type										_pipeline_id;
		utl::vector<id::id_type>						_descriptorSet_layout_ids;
		utl::vector<id::id_type>						_pipeline_layout_ids;
		id::id_type										_ssao_descriptorSet_id;
		id::id_type										_ssao_pipeline_id;
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

		void runRenderpass(vulkan_cmd_buffer cmd_buffer, vulkan_surface* surface);


		[[nodiscard]] utl::vector<id::id_type> getTexture() { return _image_ids; }

	private:
		u32												_width;
		u32												_height;
		id::id_type										_ub_id;
		id::id_type										_framebuffer_id;
		id::id_type										_renderpass_id;
		utl::vector<id::id_type>						_image_ids;
		id::id_type										_descriptorSet_id;
		id::id_type										_pipeline_id;
		utl::vector<id::id_type>						_descriptorSet_layout_id;
		id::id_type										_pipeline_layout_id;
	};

	class vulkan_final_pass
	{
	public:
		vulkan_final_pass() = default;

		explicit vulkan_final_pass(u32 width, u32 height) : _width{ width }, _height{ height } {}

		DISABLE_COPY_AND_MOVE(vulkan_final_pass);

		~vulkan_final_pass();

		void setSize(u32 width, u32 height) { _width = width; _height = height; }

		void setupDescriptorSets(std::initializer_list<id::id_type> image_id);

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