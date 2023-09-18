#pragma once
#include "VulkanData.h"

#include "Utilities/FreeList.h"

#include "VulkanCore.h"
#include "VulkanHelpers.h"

#include <type_traits>

namespace primal::graphics::vulkan
{
	namespace
	{
		// --------------------------------------------- UNIFORM BUFFER ------------------------------------------------------------------------- //
		utl::free_list<UniformBuffer>		uniformBuffer_list;

		id::id_type create_uniform_buffer(const void* const data, [[maybe_unused]] u32 size)
		{
			UniformBuffer ub;

			createBuffer(core::logical_device(), size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, ub.buffer, ub.memory);

			vkMapMemory(core::logical_device(), ub.memory, 0, size, 0, &ub.mapped);

			memcpy(ub.mapped, data, size);

			ub.size = size;

			return uniformBuffer_list.add(ub);
		}

		void remove_uniform_buffer(id::id_type id)
		{
			uniformBuffer_list.remove(id);
		}

		UniformBuffer get_uniform_buffer(id::id_type id)
		{
			return uniformBuffer_list[id];
		}

		// --------------------------------------------- VULKAN RENDERPASS ------------------------------------------------------------------------- //
		utl::free_list<VkRenderPass>            renderpass_list;

		id::id_type create_renderpass(const void* const data, [[maybe_unused]] u32 size)
		{
			VkRenderPassCreateInfo info{ *(VkRenderPassCreateInfo*)data };
			VkRenderPass pass;
			VkResult result{VK_SUCCESS};
			VkCall(result = vkCreateRenderPass(core::logical_device(), &info, nullptr, &pass), "Failed to create render pass...");
			MESSAGE("Created renderpass");
			return renderpass_list.add(pass);
		}

		void remove_renderpass(id::id_type id)
		{
			renderpass_list.remove(id);
		}

		VkRenderPass get_renderpass(id::id_type id)
		{
			return renderpass_list[id];
		}

		// --------------------------------------------- VULKAN FRAMEBUFFER ------------------------------------------------------------------------- //
		utl::free_list<VkFramebuffer>   framebuffer_list;

		id::id_type create_framebuffer(const void* const data, [[maybe_unused]] u32 size)
		{
			VkFramebufferCreateInfo info{ *(VkFramebufferCreateInfo*)data };
			VkFramebuffer fb;
			VkResult result{ VK_SUCCESS };
			VkCall(result = vkCreateFramebuffer(core::logical_device(), &info, nullptr, &fb), "Failed to create framebuffer...");
			MESSAGE("Created frambuffer");
			return framebuffer_list.add(fb);
		}

		void remove_framebuffer(id::id_type id)
		{
			framebuffer_list.remove(id);
		}

		VkFramebuffer get_framebuffer(id::id_type id)
		{
			return framebuffer_list[id];
		}

		// --------------------------------------------- VULKAN DESCRIPTOR POOL ------------------------------------------------------------------------- //
		utl::free_list<VkDescriptorPool>	descriptor_pool_list;

		id::id_type create_descriptor_pool(const void* const data, [[maybe_unused]] u32 size)
		{
			VkDescriptorPoolCreateInfo info{ *(VkDescriptorPoolCreateInfo*)data };
			VkDescriptorPool pool;
			VkResult result{ VK_SUCCESS };
			VkCall(result = vkCreateDescriptorPool(core::logical_device(), &info, nullptr, &pool), "Failed to create descriptor pool...");
			return descriptor_pool_list.add(pool);
		}

		void remove_descriptor_pool(id::id_type id)
		{
			descriptor_pool_list.remove(id);
		}

		VkDescriptorPool get_descriptor_pool(id::id_type id)
		{
			return descriptor_pool_list[id];
		}

		// --------------------------------------------- VULKAN DESCRIPTOR SET LAYOUT ------------------------------------------------------------------------- //
		utl::free_list<VkDescriptorSetLayout> descriptor_set_layout_list;

		id::id_type create_descriptor_set_layout(const void* const data, [[maybe_unused]] u32 size)
		{
			VkDescriptorSetLayoutCreateInfo info{ *(VkDescriptorSetLayoutCreateInfo*)data };
			VkDescriptorSetLayout layout;
			VkResult result{ VK_SUCCESS };
			VkCall(result = vkCreateDescriptorSetLayout(core::logical_device(), &info, nullptr, &layout), "Failed to create descriptor set layout...");
			return descriptor_set_layout_list.add(layout);
		}

		void remove_descriptor_set_layout(id::id_type id)
		{
			descriptor_set_layout_list.remove(id);
		}

		VkDescriptorSetLayout get_descriptor_set_layout(id::id_type id)
		{
			return descriptor_set_layout_list[id];
		}

		// --------------------------------------------- VULKAN DESCRIPTOR SET ------------------------------------------------------------------------- //
		utl::free_list<VkDescriptorSet>	descriptor_set_list;

		id::id_type create_descriptor_set(const void* const data, [[maybe_unused]] u32 size)
		{
			VkDescriptorSetAllocateInfo info{ *(VkDescriptorSetAllocateInfo*)data };
			VkDescriptorSet set;
			VkResult result{ VK_SUCCESS };
			VkCall(result = vkAllocateDescriptorSets(core::logical_device(), &info, &set), "Failed to allocate descriptor set...");
			return descriptor_set_list.add(set);
		}

		void remove_descriptor_set(id::id_type id)
		{
			descriptor_set_list.remove(id);
		}

		VkDescriptorSet get_descriptor_set(id::id_type id)
		{
			return descriptor_set_list[id];
		}

		// --------------------------------------------- VULKAN PIPELINE LAYOUT ------------------------------------------------------------------------- //
		utl::free_list<VkPipelineLayout>	pipeline_layout_list;

		id::id_type create_pipeline_layout(const void* const data, [[maybe_unused]] u32 size)
		{
			VkPipelineLayoutCreateInfo info{ *(VkPipelineLayoutCreateInfo*)data };
			VkPipelineLayout layout;
			VkResult result{ VK_SUCCESS };
			VkCall(result = vkCreatePipelineLayout(core::logical_device(), &info, nullptr, &layout), "Failed to create pipeline layout...");
			return pipeline_layout_list.add(layout);
		}

		void remove_pipeline_layout(id::id_type id)
		{
			pipeline_layout_list.remove(id);
		}

		VkPipelineLayout get_pipeline_layout(id::id_type id)
		{
			return pipeline_layout_list[id];
		}

		// --------------------------------------------- VULKAN PIPELINE ------------------------------------------------------------------------- //
		utl::free_list<VkPipeline> pipeline_list;

		id::id_type create_pipeline(const void* const data, [[maybe_unused]] u32 size)
		{
			VkGraphicsPipelineCreateInfo info{ *(VkGraphicsPipelineCreateInfo*)data };
			VkPipeline pipeline;
			VkResult result{ VK_SUCCESS };
			VkCall(result = vkCreateGraphicsPipelines(core::logical_device(), nullptr, 1, &info, nullptr, &pipeline), "Failed to create graphics pipeline...");
			return pipeline_list.add(pipeline);
		}

		void remove_pipeline(id::id_type id)
		{
			pipeline_list.remove(id);
		}

		VkPipeline get_pipeline(id::id_type id)
		{
			return pipeline_list[id];
		}

		using create_function = id::id_type(*)(const void* const, u32);
		using remove_function = void(*)(id::id_type);
		
		constexpr create_function create_functions[]
		{
			create_renderpass,
			create_framebuffer,
			create_descriptor_pool,
			create_descriptor_set_layout,
			create_descriptor_set,
			create_pipeline_layout,
			create_pipeline,
			create_uniform_buffer
		};
		static_assert(_countof(create_functions) == engine_vulkan_data::data_type::count);

		constexpr remove_function remove_functions[]
		{
			remove_renderpass,
			remove_framebuffer,
			remove_descriptor_pool,
			remove_descriptor_set_layout,
			remove_descriptor_set,
			remove_pipeline_layout,
			remove_pipeline,
			remove_uniform_buffer
		};
		static_assert(_countof(remove_functions) == engine_vulkan_data::data_type::count);

 	} // anonymous namespace

	
	id::id_type create_data(engine_vulkan_data::data_type type, const void * const data, [[maybe_unused]] u32 size)
	{
		return create_functions[type](data, size);
	}

	void remove_data(engine_vulkan_data::data_type type, id::id_type id)
	{
		remove_functions[type](id);
	}

	template<typename T>
	T& get_data(id::id_type id)
	{
		if constexpr (std::is_same_v<T, UniformBuffer>)
		{
			return uniformBuffer_list[id];
		}
		else if constexpr (std::is_same_v<T, VkRenderPass>)
		{
			return renderpass_list[id];
		}
		else if constexpr (std::is_same_v<T, VkFramebuffer>)
		{
			return framebuffer_list[id];
		}
		else if constexpr (std::is_same_v<T, VkDescriptorPool>)
		{
			return descriptor_pool_list[id];
		}
		else if constexpr (std::is_same_v<T, VkDescriptorSetLayout>)
		{
			return descriptor_set_layout_list[id];
		}
		else if constexpr (std::is_same_v<T, VkDescriptorSet>)
		{
			return descriptor_set_list[id];
		}
		else if constexpr (std::is_same_v<T, VkPipelineLayout>)
		{
			return pipeline_layout_list[id];
		}
		else if constexpr (std::is_same_v<T, VkPipeline>)
		{
			return pipeline_list[id];
		}
		else
		{
			return T{};
		}
	}
}