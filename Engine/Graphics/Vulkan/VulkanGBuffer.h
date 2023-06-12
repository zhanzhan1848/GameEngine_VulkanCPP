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

		void setupPipeline(VkPipelineLayout layout, vulkan_framebuffer framebuffer);

		void runRenderpass(vulkan_cmd_buffer cmd_buffer, vulkan_surface* surface);

		[[nodiscard]] constexpr vulkan_texture getTexture() { return _image; }


	private:
		u32									_shadowmapping_dim{ 2048 };
		f32									_depthBiasConstant{ 1.25f };
		f32									_depthBiasSlope{ 1.75f };
		UniformBufferObject					_ubo;
		uniformBuffer						_uniformbuffer;
		VkPipeline							_pipeline;
		VkDescriptorSet						_descriptorSet;
		vulkan_framebuffer					_framebuffer;
		vulkan_renderpass					_renderpass;
		vulkan_texture						_image;
		shaders::vulkan_shader				_shader;
	};
}