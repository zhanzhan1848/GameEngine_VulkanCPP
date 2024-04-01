#include "VulkanData.h"

#include "Utilities/FreeList.h"

#include "VulkanCore.h"
#include "VulkanHelpers.h"

#include <type_traits>

namespace primal::graphics::vulkan::data
{
	namespace
	{
		VkQueue					_transfer_queue{ nullptr };
		VkCommandPool			_transfer_cmd_pool{ nullptr };

		// --------------------------------------------- UNIFORM BUFFER ------------------------------------------------------------------------- //
		utl::free_list<vulkan_buffer>		vulkan_buffer_list;

		id::id_type create_vulkan_buffer(const void* const data, [[maybe_unused]] u32 size)
		{
			vulkan_buffer::type flags{ *(vulkan_buffer::type*)data };
			assert(flags < vulkan_buffer::count);
			vulkan_buffer buffer{ flags, size };

			return vulkan_buffer_list.add(buffer);
		}

		void remove_vulkan_buffer(id::id_type id)
		{
			vulkan_buffer_list[id].release();
			vulkan_buffer_list.remove(id);
		}

		void get_vulkan_buffer(void* const data, id::id_type id, [[maybe_unused]] u32 size)
		{
			vulkan_buffer *const output{ (vulkan_buffer *const)data };
			assert(sizeof(vulkan_buffer) == size);
			*output = vulkan_buffer_list[id];
		}

		// --------------------------------------------- VULKAN RENDERPASS ------------------------------------------------------------------------- //
		utl::free_list<VkRenderPass>            renderpass_list;

		id::id_type create_renderpass(const void* const data, [[maybe_unused]] u32 size)
		{
			VkRenderPassCreateInfo info{ *(VkRenderPassCreateInfo*)data };
			VkRenderPass pass;
			VkResult result{ VK_SUCCESS };
			VkCall(result = vkCreateRenderPass(core::logical_device(), &info, nullptr, &pass), "Failed to create render pass...");
			MESSAGE("Created renderpass");
			return renderpass_list.add(pass);
		}

		void remove_renderpass(id::id_type id)
		{
			vkDestroyRenderPass(core::logical_device(), renderpass_list[id], nullptr);
			renderpass_list.remove(id);
		}

		void get_renderpass(void* const data, id::id_type id, [[maybe_unused]] u32 size)
		{
			VkRenderPass *const output{ (VkRenderPass *const)data };
			assert(sizeof(VkRenderPass) == size);
			*output = renderpass_list[id];
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
			vkDestroyFramebuffer(core::logical_device(), framebuffer_list[id], nullptr);
			framebuffer_list.remove(id);
		}

		void get_framebuffer(void* const data, id::id_type id, [[maybe_unused]] u32 size)
		{
			VkFramebuffer *const output{ (VkFramebuffer *const)data };
			assert(sizeof(VkFramebuffer) == size);
			*output = framebuffer_list[id];
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
			vkDestroyDescriptorPool(core::logical_device(), descriptor_pool_list[id], nullptr);
			descriptor_pool_list.remove(id);
		}

		void get_descriptor_pool(void* const data, id::id_type id, [[maybe_unused]] u32 size)
		{
			VkDescriptorPool *const output{ (VkDescriptorPool *const)data };
			assert(sizeof(VkDescriptorPool) == size);
			*output = descriptor_pool_list[id];
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
			vkDestroyDescriptorSetLayout(core::logical_device(), descriptor_set_layout_list[id], nullptr);
			descriptor_set_layout_list.remove(id);
		}

		void get_descriptor_set_layout(void* const data, id::id_type id, [[maybe_unused]] u32 size)
		{
			VkDescriptorSetLayout *const output{ (VkDescriptorSetLayout *const)data };
			assert(sizeof(VkDescriptorSetLayout) == size);
			*output = descriptor_set_layout_list[id];
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

		void get_descriptor_set(void* const data, id::id_type id, [[maybe_unused]] u32 size)
		{
			VkDescriptorSet *const output{ (VkDescriptorSet *const)data };
			assert(sizeof(VkDescriptorSet) == size);
			*output = descriptor_set_list[id];
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
			vkDestroyPipelineLayout(core::logical_device(), pipeline_layout_list[id], nullptr);
			pipeline_layout_list.remove(id);
		}

		void get_pipeline_layout(void* const data, id::id_type id, [[maybe_unused]] u32 size)
		{
			VkPipelineLayout *const output{ (VkPipelineLayout *const)data };
			assert(sizeof(VkPipelineLayout) == size);
			*output = pipeline_layout_list[id];
		}

		// --------------------------------------------- VULKAN PIPELINE ------------------------------------------------------------------------- //
		utl::free_list<VkPipeline> pipeline_list;

		id::id_type create_pipeline(const void* const data, [[maybe_unused]] u32 size = 0)
		{
			VkPipeline pipeline;
			VkResult result{ VK_SUCCESS };
			if (size == 0)
			{
				VkGraphicsPipelineCreateInfo info{ *(VkGraphicsPipelineCreateInfo*)data };
				VkCall(result = vkCreateGraphicsPipelines(core::logical_device(), nullptr, 1, &info, nullptr, &pipeline), "Failed to create graphics pipeline...");
			}
			else if (size == 1)
			{
				VkComputePipelineCreateInfo info{ *(VkComputePipelineCreateInfo*)data };
				VkCall(result = vkCreateComputePipelines(core::logical_device(), nullptr, 1, &info, nullptr, &pipeline), "Failed to create compute pipeline...");
			}
			return pipeline_list.add(pipeline);
		}

		void remove_pipeline(id::id_type id)
		{
			vkDestroyPipeline(core::logical_device(), pipeline_list[id], nullptr);
			pipeline_list.remove(id);
		}

		void get_pipeline(void* const data, id::id_type id, [[maybe_unused]] u32 size)
		{
			VkPipeline *const output{ (VkPipeline *const)data };
			assert(sizeof(VkPipeline) == size);
			*output = pipeline_list[id];
		}

		using create_function = id::id_type(*)(const void* const, u32);
		using remove_function = void(*)(id::id_type);
		using get_function = void(*)(void* const, id::id_type, u32);

		constexpr create_function create_functions[]
		{
			create_renderpass,
			create_framebuffer,
			create_descriptor_pool,
			create_descriptor_set_layout,
			create_descriptor_set,
			create_pipeline_layout,
			create_pipeline,
			create_vulkan_buffer,
		};
		static_assert(_countof(create_functions) == data::engine_vulkan_data::data_type::count);

		constexpr remove_function remove_functions[]
		{
			remove_renderpass,
			remove_framebuffer,
			remove_descriptor_pool,
			remove_descriptor_set_layout,
			remove_descriptor_set,
			remove_pipeline_layout,
			remove_pipeline,
			remove_vulkan_buffer
		};
		static_assert(_countof(remove_functions) == data::engine_vulkan_data::data_type::count);

		constexpr get_function get_functions[]
		{
			get_renderpass,
			get_framebuffer,
			get_descriptor_pool,
			get_descriptor_set_layout,
			get_descriptor_set,
			get_pipeline_layout,
			get_pipeline,
			get_vulkan_buffer
		};
		static_assert(_countof(get_functions) == data::engine_vulkan_data::data_type::count);

	} // anonymous namespace

	bool initialize()
	{
		vkGetDeviceQueue(core::logical_device(), core::transfer_family_queue_index(), 0, &_transfer_queue);
		VkCommandPoolCreateInfo info{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
		info.queueFamilyIndex = core::transfer_family_queue_index();
		info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		VkResult result{ VK_SUCCESS };
		VkCall(result = vkCreateCommandPool(core::logical_device(), &info, nullptr, &_transfer_cmd_pool), "Failed to create transfer command pool...");
		return true;
	}

	void shutdown()
	{
		vkQueueWaitIdle(_transfer_queue);

		vkDestroyCommandPool(core::logical_device(), _transfer_cmd_pool, nullptr);
	}

	vulkan_buffer::vulkan_buffer(type type, [[maybe_unused]] u32 size)
	{
		switch (type)
		{
		case primal::graphics::vulkan::data::vulkan_buffer::static_vertex_buffer:							this->flags = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
			break;
		case primal::graphics::vulkan::data::vulkan_buffer::static_index_buffer:							this->flags = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
			break;
		case primal::graphics::vulkan::data::vulkan_buffer::static_instance_buffer:							this->flags = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
			break;
		case primal::graphics::vulkan::data::vulkan_buffer::static_uniform_buffer:							this->flags = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
			break;
		case primal::graphics::vulkan::data::vulkan_buffer::per_frame_update_uniform_buffer:				this->flags = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
			break;
		case primal::graphics::vulkan::data::vulkan_buffer::static_storage_buffer:							this->flags = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
			break;
		case primal::graphics::vulkan::data::vulkan_buffer::per_frame_update_storage_buffer:				this->flags = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
			break;
		}

		createBuffer(core::logical_device(), size <= 0 ? 1 : size, this->flags, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, this->cpu_address, this->cpu_memory);
		this->size = size;
		this->own_type = type;
		this->data = (void*)malloc(size);

		switch (type)
		{
		case primal::graphics::vulkan::data::vulkan_buffer::static_vertex_buffer:
		case primal::graphics::vulkan::data::vulkan_buffer::static_index_buffer:
		case primal::graphics::vulkan::data::vulkan_buffer::static_instance_buffer:
		case primal::graphics::vulkan::data::vulkan_buffer::static_uniform_buffer:
		case primal::graphics::vulkan::data::vulkan_buffer::static_storage_buffer:
		case primal::graphics::vulkan::data::vulkan_buffer::per_frame_update_storage_buffer:
			break;
		case primal::graphics::vulkan::data::vulkan_buffer::per_frame_update_uniform_buffer:
		{
			//vkMapMemory(core::logical_device(), this->cpu_memory, 0, this->size, 0, &this->data);
		}	break;
		}
	}

	void vulkan_buffer::update(const void* const data, size_t size, u32 offset_count)
	{
		assert(this->cpu_address);
		assert(size <= this->size);

		switch (this->own_type)
		{
		case primal::graphics::vulkan::data::vulkan_buffer::static_vertex_buffer:
		case primal::graphics::vulkan::data::vulkan_buffer::static_index_buffer:
		case primal::graphics::vulkan::data::vulkan_buffer::static_uniform_buffer:
		case primal::graphics::vulkan::data::vulkan_buffer::static_storage_buffer:
		case primal::graphics::vulkan::data::vulkan_buffer::per_frame_update_storage_buffer:
		{
			//this->data = (void*)malloc(size);

			vkMapMemory(core::logical_device(), this->cpu_memory, 0, size, 0, &this->data);

			memcpy(this->data, data, size);

			vkUnmapMemory(core::logical_device(), this->cpu_memory);
		}	break;
		case primal::graphics::vulkan::data::vulkan_buffer::per_frame_update_uniform_buffer:
		{
			//memcpy(this->data, data, size);

			vkMapMemory(core::logical_device(), this->cpu_memory, 0, size, 0, &this->data);

			memcpy(this->data, data, size);

			vkUnmapMemory(core::logical_device(), this->cpu_memory);
		}	break;
		}
	}

	void vulkan_buffer::convert_to_local_device_buffer()
	{
		u32 flag = VK_BUFFER_USAGE_FLAG_BITS_MAX_ENUM;
		switch (this->own_type)
		{
		case primal::graphics::vulkan::data::vulkan_buffer::static_vertex_buffer:							flag = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
			break;
		case primal::graphics::vulkan::data::vulkan_buffer::static_index_buffer:							flag = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
			break;
		case primal::graphics::vulkan::data::vulkan_buffer::static_instance_buffer:							flag = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
			break;
		case primal::graphics::vulkan::data::vulkan_buffer::static_uniform_buffer:							flag = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
			break;
		case primal::graphics::vulkan::data::vulkan_buffer::per_frame_update_uniform_buffer:				flag = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
			break;
		case primal::graphics::vulkan::data::vulkan_buffer::static_storage_buffer:							flag = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
			break;
		case primal::graphics::vulkan::data::vulkan_buffer::per_frame_update_storage_buffer:				flag = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
			break;
		}

		assert(flag != VK_BUFFER_USAGE_FLAG_BITS_MAX_ENUM);
		createBuffer(core::logical_device(), this->size, flag, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, this->gpu_address, this->gpu_memory);
		copyBuffer(core::logical_device(), core::transfer_family_queue_index(), _transfer_cmd_pool, this->cpu_address, this->gpu_address, size);
		vkDestroyBuffer(core::logical_device(), this->cpu_address, nullptr);
		vkFreeMemory(core::logical_device(), this->cpu_memory, nullptr);
	}

	void vulkan_buffer::resize(size_t size)
	{
		assert(size != this->size);
		
		assert(this->cpu_address && this->cpu_memory);

		vkDestroyBuffer(core::logical_device(), this->cpu_address, nullptr);
		vkFreeMemory(core::logical_device(), this->cpu_memory, nullptr);

		createBuffer(core::logical_device(), size , this->flags, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, this->cpu_address, this->cpu_memory);

		this->size = size;
	}

	vulkan_buffer::~vulkan_buffer()
	{
		 /*vkFreeMemory(core::logical_device(), this->gpu_memory, nullptr);
		 vkFreeMemory(core::logical_device(), this->cpu_memory, nullptr);
		 
		 vkDestroyBuffer(core::logical_device(), this->gpu_address, nullptr);
		 vkDestroyBuffer(core::logical_device(), this->cpu_address, nullptr);*/
	}

	void vulkan_buffer::release()
	{
		vkFreeMemory(core::logical_device(), this->gpu_memory, nullptr);
		vkFreeMemory(core::logical_device(), this->cpu_memory, nullptr);

		vkDestroyBuffer(core::logical_device(), this->gpu_address, nullptr);
		vkDestroyBuffer(core::logical_device(), this->cpu_address, nullptr);

		this->flags = id::invalid_id;
		this->data = nullptr;
	}

	void UniformBuffer::update(const void* const data, size_t size)
	{
		assert(size <= this->size);

		vkMapMemory(core::logical_device(), this->memory, 0, size, 0, &this->mapped);

		memcpy(this->mapped, data, size);

		vkUnmapMemory(core::logical_device(), this->memory);
	}

	void UniformBuffer::resize(size_t size)
	{
		assert(size != this->size);
		assert(this->buffer && this->memory);

		vkDestroyBuffer(core::logical_device(), this->buffer, nullptr);
		vkFreeMemory(core::logical_device(), this->memory, nullptr);

		createBuffer(core::logical_device(), size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, this->buffer, this->memory);

		this->size = size;
	}

	id::id_type create_data(engine_vulkan_data::data_type type, const void* const data, [[maybe_unused]] u32 size)
	{
		assert(type < engine_vulkan_data::count);
		return create_functions[type](data, size);
	}

	void remove_data(engine_vulkan_data::data_type type, id::id_type id)
	{
		assert(type < engine_vulkan_data::count);
		remove_functions[type](id);
	}

	void get_data(engine_vulkan_data::data_type type, void* const data, id::id_type id, [[maybe_unused]] u32 size)
	{
		assert(data && size);
		assert(type < engine_vulkan_data::count);
		get_functions[type](data, id, size);
	}

	template<typename T>
	T& get_data(id::id_type id)
	{
		if constexpr (std::is_same_v<T, vulkan_buffer>)
		{
			return vulkan_buffer_list[id];
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

	template vulkan_buffer& data::get_data(id::id_type id);
	template VkRenderPass& data::get_data(id::id_type id);
	template VkFramebuffer& data::get_data(id::id_type id);
	template VkDescriptorPool& data::get_data(id::id_type id);
	template VkDescriptorSetLayout& data::get_data(id::id_type id);
	template VkDescriptorSet& data::get_data(id::id_type id);
	template VkPipelineLayout& data::get_data(id::id_type id);
	template VkPipeline& data::get_data(id::id_type id);
}