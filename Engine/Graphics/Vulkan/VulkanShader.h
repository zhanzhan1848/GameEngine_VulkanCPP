#pragma once
#include "VulkanCommonHeaders.h"
#include "Graphics/Renderer.h"

namespace primal::graphics::vulkan::shaders
{

	class vulkan_shader
	{
	public:
		vulkan_shader() = default;

		~vulkan_shader();

		bool operator!()
		{
			return _shaderstage.sType == VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO &&
				_shaderstage.module != nullptr;
		}

		//void setID(id::id_type id) { _entity_id = id; }
		void loadFile(std::string path, shader_type::type type);
		void freeModule();

		[[nodiscard]] const VkPipelineShaderStageCreateInfo getShaderStage() const { return _shaderstage; }

	private:
		//id::id_type								_entity_id;
		std::string								_path;
		std::string								_function;
		VkPipelineShaderStageCreateInfo			_shaderstage;
	};

}