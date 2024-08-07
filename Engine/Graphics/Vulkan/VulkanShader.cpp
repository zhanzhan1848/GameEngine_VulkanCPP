#include "VulkanShader.h"
#include <fstream>
#include <iostream>
#include <filesystem>
#include "VulkanCore.h"

namespace primal::graphics::vulkan::shaders
{
	namespace
	{
		utl::free_list<shaders::vulkan_shader>				shaders;

		std::mutex											shader_mutex;

		const std::string absolutePath = "C:/Users/27042/Desktop/DX_Test/PrimalMerge/Engine/Graphics/Vulkan/Shaders/";
	} // anonymous namespace

	void vulkan_shader::loadFile(std::string path, shader_type::type type)
	{
		_path = path;
		_function = "main";

		std::ifstream file(path, std::ios::binary | std::ios::in);
		if (file.is_open())
		{
			std::vector<u8> fileContents((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
			file.close();

			assert(fileContents.size() > 0);
			//_code_size = fileContents.size();
			//std::unique_ptr<u8[]> code{ std::make_unique<u8[]>(fileContents.size()) };
			//memcpy(code.get(), fileContents.data(), fileContents.size());
			//_code.swap(code);

			VkShaderStageFlagBits stage;
			//if (type == shader_type::vertex) stage = VK_SHADER_STAGE_VERTEX_BIT;
			//if (type == shader_type::pixel) stage = VK_SHADER_STAGE_FRAGMENT_BIT;

			switch(type)
			{
				case shader_type::vertex:
				{
					stage = VK_SHADER_STAGE_VERTEX_BIT;
				}	break;
				case shader_type::pixel:
				{
					stage = VK_SHADER_STAGE_FRAGMENT_BIT;
				}	break;
				case shader_type::compute:
				{
					stage = VK_SHADER_STAGE_COMPUTE_BIT;
				}	break;
				default:
					stage = VK_SHADER_STAGE_FLAG_BITS_MAX_ENUM;
			}

			VkShaderModule shaderModule;
			VkShaderModuleCreateInfo moduleCreateInfo;
			moduleCreateInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
			moduleCreateInfo.pNext = nullptr;
			moduleCreateInfo.flags = 0;
			moduleCreateInfo.codeSize = fileContents.size();
			moduleCreateInfo.pCode = reinterpret_cast<const u32*>(fileContents.data());

			VkResult result{ VK_SUCCESS };
			VkCall(result = vkCreateShaderModule(core::logical_device(), &moduleCreateInfo, NULL, &shaderModule), "Failed to create shader module...");

			_shaderstage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			_shaderstage.pNext = nullptr;
			_shaderstage.flags = 0;
			_shaderstage.stage = stage;
			_shaderstage.module = shaderModule;
			_shaderstage.pName = "main";
			_shaderstage.pSpecializationInfo = nullptr;
			assert(_shaderstage.module != VK_NULL_HANDLE);
		}
		else
		{
			std::cerr << "Error: Could not open shader file \"" << path.c_str() << "\"" << "\n";
		}
	}

	void vulkan_shader::freeModule()
	{
		vkDestroyShaderModule(core::logical_device(), _shaderstage.module, nullptr);
	}

	vulkan_shader::~vulkan_shader()
	{
		//_entity_id = id::invalid_id;
		//_path = nullptr;
		//freeModule();
	}

	/// <summary>
		// ! 生成shader原始ID
		/// </summary>
		/// <param name="path"></param>
		/// <returns></returns>
	id::id_type add(std::string path, shader_type::type type)
	{
		vulkan_shader shader;
		shader.loadFile(path, type);
		std::lock_guard lock{ shader_mutex };
		return shaders.add(shader);
	}

	/// <summary>
	// ！ 删除shader原始ID
	///	1、 执行关联的所有material里的remove_shader 操作
	/// 2、 执行删除shader原始ID的操作
	/// </summary>
	/// <param name="id"></param>
	void remove(id::id_type id)
	{
		std::lock_guard lock{ shader_mutex };
		assert(id::is_valid(id));
		shaders.remove(id);
	}

	vulkan_shader& get_shader(id::id_type id)
	{
		std::lock_guard lock{ shader_mutex };
		assert(id::is_valid(id));
		return shaders[id];
	}
}