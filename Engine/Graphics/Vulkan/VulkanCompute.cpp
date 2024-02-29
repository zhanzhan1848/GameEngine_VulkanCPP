#include "VulkanCompute.h"
#include "VulkanCore.h"
#include "VulkanHelpers.h"
#include "VulkanData.h"
#include "VulkanCommandBuffer.h"
#include "VulkanShader.h"
#include "VulkanContent.h"
#include "VulkanTexture.h"
#include "Shaders/ShaderTypes.h"

#define REPEAT_NAME(name, count, ext) (name##ext##count)

namespace primal::graphics::vulkan::compute
{
	namespace
	{
		VkCommandPool					_compute_pool;

		class vulkan_compute
		{
		public:
			vulkan_compute() = default;
			explicit vulkan_compute(utl::vector<id::id_type> buffer_ids, utl::vector<id::id_type> image_ids, id::id_type shader_id) 
			{
				vkGetDeviceQueue(core::logical_device(), core::compute_family_queue_index(), 0, &_compute_queue);

				create_descriptor_pool(buffer_ids.size(), image_ids.size());

				create_descriptor_set_layout(buffer_ids.size(), image_ids.size());

				create_pipeline_layout();
				
				create_pipeline(shader_id);

				create_semaphore();

				create_descriptor_sets(buffer_ids, image_ids);
			}

			~vulkan_compute() 
			{
				vkQueueWaitIdle(_compute_queue);

				 //data::remove_data(data::engine_vulkan_data::vulkan_descriptor_sets, _descriptor_set_id);
				 //_descriptor_set_id = id::invalid_id;
				 //data::remove_data(data::engine_vulkan_data::vulkan_pipeline, _pipeline_id);
				 //_pipeline_id = id::invalid_id;
				 //data::remove_data(data::engine_vulkan_data::vulkan_pipeline_layout, _pipeline_layout_id);
				 //_pipeline_layout_id = id::invalid_id;
				 //data::remove_data(data::engine_vulkan_data::vulkan_descriptor_set_layout, _descriptor_setlayout_id);
				 //_descriptor_setlayout_id = id::invalid_id;
				 //data::remove_data(data::engine_vulkan_data::vulkan_descriptor_pool, _descriptor_pool_id);
				 //_descriptor_pool_id = id::invalid_id;
				 ////vkFreeCommandBuffers(core::logical_device(), _compute_pool, 1, &_cmd_buffer.cmd_buffer);
				 //vkDestroySemaphore(core::logical_device(), _signal_semaphore, nullptr);
			}

			void setup(utl::vector<id::id_type> buffer_ids, utl::vector<id::id_type> image_ids, id::id_type shader_id)
			{
				vkGetDeviceQueue(core::logical_device(), core::compute_family_queue_index(), 0, &_compute_queue);

				create_descriptor_pool(buffer_ids.size(), image_ids.size());

				create_descriptor_set_layout(buffer_ids.size(), image_ids.size());

				create_pipeline_layout();

				create_pipeline(shader_id);

				create_semaphore();

				create_descriptor_sets(buffer_ids, image_ids);
			}

			vulkan_compute* run()
			{
				if (!_rendered)
				{
					_cmd_buffer.cmd_buffer = beginSingleCommand(core::logical_device(), _compute_pool);
					_cmd_buffer.cmd_state = vulkan_cmd_buffer::CMD_IN_RENDER_PASS;
					auto pipeline = data::get_data<VkPipeline>(_pipeline_id);
					auto pipelineLayout = data::get_data<VkPipelineLayout>(_pipeline_layout_id);
					auto set = data::get_data<VkDescriptorSet>(_descriptor_set_id);
					vkCmdBindPipeline(_cmd_buffer.cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
					vkCmdBindDescriptorSets(_cmd_buffer.cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &set, 0, 0);

					vkCmdDispatch(_cmd_buffer.cmd_buffer, 7, 4, 1);

					vkEndCommandBuffer(_cmd_buffer.cmd_buffer);
				}
				
				return this;
			}

			void submit()
			{
				if (!_rendered)
				{
					VkPipelineStageFlags waitStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;

					VkSubmitInfo submitInfo;
					submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
					submitInfo.pNext = nullptr;
					submitInfo.commandBufferCount = 1;
					submitInfo.pCommandBuffers = &_cmd_buffer.cmd_buffer;
					submitInfo.waitSemaphoreCount = 0;
					submitInfo.pWaitSemaphores = nullptr;
					submitInfo.pWaitDstStageMask = &waitStageMask;
					submitInfo.signalSemaphoreCount = 1;
					submitInfo.pSignalSemaphores = &_signal_semaphore;
					VkResult result{ VK_SUCCESS };
					VkCall(result = vkQueueSubmit(_compute_queue, 1, &submitInfo, nullptr), "Failed to submit compute queue...");

					vkQueueWaitIdle(_compute_queue);
					vkResetCommandBuffer(_cmd_buffer.cmd_buffer, 0);
					_rendered = true;
				}
			}

			[[nodiscard]] constexpr bool const is_rendered() const { return _rendered; }
			[[nodiscard]] constexpr VkSemaphore const get_signal_semaphore() const { return _signal_semaphore; }
			[[nodiscard]] constexpr VkSemaphore const get_wait_semaphore() const { return _wait_semaphore; }

		private:

			void create_descriptor_pool(u32 buffer_size, u32 image_size)
			{
				utl::vector<VkDescriptorPoolSize>	poolSize;
				for (u32 i{ 0 }; i < buffer_size; ++i)
				{
					poolSize.emplace_back(descriptor::descriptorPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, static_cast<u32>(frame_buffer_count)));
				}
				for (u32 j{ 0 }; j < image_size; ++j)
				{
					poolSize.emplace_back(descriptor::descriptorPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, static_cast<u32>(frame_buffer_count)));
				}
				VkDescriptorPoolCreateInfo poolInfo;
				poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
				poolInfo.pNext = VK_NULL_HANDLE;
				poolInfo.flags = 0;
				poolInfo.poolSizeCount = static_cast<u32>(poolSize.size());
				poolInfo.pPoolSizes = poolSize.data();
				poolInfo.maxSets = static_cast<u32>(frame_buffer_count) * 2;
				_descriptor_pool_id = data::create_data(data::engine_vulkan_data::vulkan_descriptor_pool, static_cast<void*>(&poolInfo), 0);
			}

			void create_descriptor_set_layout(u32 buffer_size, u32 image_size)
			{
				std::vector<VkDescriptorSetLayoutBinding> bindings;
				u32 count{ 0 };
				for (u32 i{ 0 }; i < buffer_size; ++i)
				{
					bindings.emplace_back(descriptor::descriptorSetLayoutBinding(count, VK_SHADER_STAGE_COMPUTE_BIT, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER));
					count++;
				}
				for (u32 j{ 0 }; j < image_size; ++j)
				{
					bindings.emplace_back(descriptor::descriptorSetLayoutBinding(count, VK_SHADER_STAGE_COMPUTE_BIT, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE));
					count++;
				}

				VkDescriptorSetLayoutCreateInfo descriptorLayout = descriptor::descriptorSetLayoutCreate(bindings);
				_descriptor_setlayout_id = data::create_data(data::engine_vulkan_data::vulkan_descriptor_set_layout, static_cast<void*>(&descriptorLayout), 0);
			}

			void create_pipeline_layout()
			{
				auto ds = data::get_data<VkDescriptorSetLayout>(_descriptor_setlayout_id);
				VkPipelineLayoutCreateInfo info{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 1, &ds, 0, nullptr };
				_pipeline_layout_id = data::create_data(data::engine_vulkan_data::vulkan_pipeline_layout, static_cast<void*>(&info), 0);
			}

			void create_pipeline(id::id_type shader_id)
			{
				auto ps = data::get_data<VkPipelineLayout>(_pipeline_layout_id);
				VkComputePipelineCreateInfo info;
				info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
				info.pNext = nullptr;
				info.layout = ps;
				info.stage = shaders::get_shader(shader_id).getShaderStage();
				info.flags = 0;
				info.basePipelineHandle = nullptr;

				_pipeline_id = data::create_data(data::engine_vulkan_data::vulkan_pipeline, static_cast<void*>(&info), 1);
			}

			void create_semaphore()
			{
				VkResult result{ VK_SUCCESS };
				VkSemaphoreCreateInfo sInfo;
				sInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
				sInfo.pNext = nullptr;
				sInfo.flags = 0;
				VkCall(result = vkCreateSemaphore(core::logical_device(), &sInfo, nullptr, &_signal_semaphore), "Failed to create compute signal semaphore...");
				VkCall(result = vkCreateSemaphore(core::logical_device(), &sInfo, nullptr, &_wait_semaphore), "Failed to create compute wait semaphore...");
			}

			void create_descriptor_sets(utl::vector<id::id_type> buffer_ids, utl::vector<id::id_type> image_ids)
			{
				VkDescriptorSetAllocateInfo info;
				info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
				info.pNext = nullptr;
				info.descriptorPool = data::get_data<VkDescriptorPool>(_descriptor_pool_id);
				info.pSetLayouts = &data::get_data<VkDescriptorSetLayout>(_descriptor_setlayout_id);
				info.descriptorSetCount = 1;

				_descriptor_set_id = data::create_data(data::engine_vulkan_data::vulkan_descriptor_sets, static_cast<void*>(&info), 0);

				auto set = data::get_data<VkDescriptorSet>(_descriptor_set_id);
				for (u32 i{ 0 }; i < buffer_ids.size(); ++i)
				{
					VkDescriptorBufferInfo bInfo;
					auto buffer = data::get_data<data::vulkan_buffer>(buffer_ids[i]);
					bInfo.buffer = buffer.cpu_address;
					bInfo.offset = 0;
					bInfo.range = buffer.size;
					VkWriteDescriptorSet writeDescriptorSet = descriptor::setWriteDescriptorSet(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, set, i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bInfo);
					vkUpdateDescriptorSets(core::logical_device(), 1, &writeDescriptorSet, 0, nullptr);
				}

				for (u32 j{ 0 }; j < image_ids.size(); ++j)
				{
					VkDescriptorImageInfo imageInfo;
					auto tex = textures::get_texture(image_ids[j]);
					imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
					imageInfo.imageView = tex.getTexture().view;
					VkWriteDescriptorSet writeDescriptorSet = descriptor::setWriteDescriptorSet(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, set, j, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &imageInfo);
					vkUpdateDescriptorSets(core::logical_device(), 1, &writeDescriptorSet, 0, nullptr);
				}
			}

			id::id_type					_descriptor_pool_id;
			id::id_type					_descriptor_setlayout_id;
			id::id_type					_descriptor_set_id;
			id::id_type					_pipeline_layout_id;
			id::id_type					_pipeline_id;
			
			VkSemaphore					_signal_semaphore;
			VkSemaphore					_wait_semaphore;
			vulkan_cmd_buffer			_cmd_buffer;
			VkQueue						_compute_queue;

			bool						_rendered{ false };
		};

		bool create_command_pool_and_semaphore()
		{
			VkCommandPoolCreateInfo poolInfo;
			poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
			poolInfo.pNext = nullptr;
			poolInfo.queueFamilyIndex = core::compute_family_queue_index();
			poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
			VkResult result{ VK_SUCCESS };
			VkCall(result = vkCreateCommandPool(core::logical_device(), &poolInfo, nullptr, &_compute_pool), "Failed to create compute command pool...");

			return true;
		}

		vulkan_compute					_frustums_compute;
		id::id_type						_storage_in_id;
		id::id_type						_storage_out_id;

		vulkan_compute					_culling_light_compute;
	} // anonymous namespace

	void initialize()
	{
		create_command_pool_and_semaphore();
		auto flags = data::vulkan_buffer::static_storage_buffer;
		_storage_in_id = data::create_data(data::engine_vulkan_data::vulkan_buffer, (void*)(&flags), sizeof(glsl::GlobalShaderData));
		_storage_out_id = data::create_data(data::engine_vulkan_data::vulkan_buffer, (void*)(&flags), sizeof(math::v4) * 22500);
		utl::vector<id::id_type> local_buffer_ids;
		local_buffer_ids.emplace_back(_storage_in_id);
		local_buffer_ids.emplace_back(_storage_out_id);
		id::id_type compute_shader_id = shaders::add("C:/Users/zy/Desktop/PrimalMerge/PrimalEngine/Engine/Graphics/Vulkan/Shaders/spv/test.comp.spv", shader_type::compute);
		_frustums_compute.setup(local_buffer_ids, utl::vector<id::id_type>(), compute_shader_id);
	}

	void shutdown()
	{
		vkDestroyCommandPool(core::logical_device(), _compute_pool, nullptr);
	}

	VkSemaphore get_compute_signal_semaphore()
	{
		return _frustums_compute.get_signal_semaphore();
	}

	VkSemaphore get_culling_signal_semaphore()
	{
		return _culling_light_compute.get_signal_semaphore();
	}

	void run(const void* const data, u32 size)
	{
		_frustums_compute.run()->submit();
	}

	id::id_type get_input_buffer_id()
	{
		return _storage_in_id;
	}

	id::id_type get_output_buffer_id()
	{
		return _storage_out_id;
	}

	bool is_rendered()
	{
		return _frustums_compute.is_rendered();
	}
}