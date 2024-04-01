#include "VulkanCompute.h"
#include "VulkanCore.h"
#include "VulkanHelpers.h"
#include "VulkanData.h"
#include "VulkanCommandBuffer.h"
#include "VulkanShader.h"
#include "VulkanContent.h"
#include "VulkanTexture.h"
#include "Shaders/ShaderTypes.h"
#include "VulkanSurface.h"
#include "VulkanLight.h"
#include "VulkanGBuffer.h"

namespace primal::graphics::vulkan::compute
{
	namespace
	{
		VkCommandPool					_compute_pool;

		class vulkan_base_compute_pass
		{
		public:
			enum data_io : u32
			{
				input = 0,
				output
			};

			enum data_type : u32
			{
				buffer = 0,
				image,
				shader
			};

			struct set_data
			{
				u32					set_num;
				u32					binding_num;
				id::id_type			data_id;
			};

			vulkan_base_compute_pass& add_resource(data_io io_type, data_type resource_type, u32 set_num, u32 binding_num, id::id_type data_id)
			{
				switch (resource_type)
				{
				case vulkan_base_compute_pass::buffer:
				{
					switch (io_type)
					{
					case vulkan_base_compute_pass::input:
						_input_buffers.emplace_back(set_data{ set_num, binding_num, data_id });
						break;
					case vulkan_base_compute_pass::output:
						_output_buffers.emplace_back(set_data{ set_num, binding_num, data_id });
						break;
					default:
						std::runtime_error("This must be a legitimate type, input or output");
						break;
					}
				}
				break;
				case vulkan_base_compute_pass::image:
				{
					switch (io_type)
					{
					case vulkan_base_compute_pass::input:
						_input_images.emplace_back(set_data{ set_num, binding_num, data_id });
						break;
					case vulkan_base_compute_pass::output:
						_output_images.emplace_back(set_data{ set_num, binding_num, data_id });
						break;
					default:
						std::runtime_error("This must be a legitimate type, input or output");
						break;
					}
				}
				break;
				case vulkan_base_compute_pass::shader:
					_shader_id = data_id;
					break;
				default:
					std::runtime_error("This must be a legitimate type, image or buffer or shader");
					break;
				}

				_set_count = std::max(_set_count, set_num);

				return *this;
			}

			virtual void initialize()
			{
				_set_count += 1;

				assert(!_input_buffers.empty() || !_input_images.empty() || !_output_buffers.empty() || !_output_images.empty() || id::is_valid(_shader_id));

				get_graphics_queue();

				create_semaphore_and_fence();

				createPoolAndLayout();

				create_pipeline();

				create_command_buffer();

				create_descriptor_set();
			}

			virtual void shutdown()
			{
				using namespace data;
				vkQueueWaitIdle(_compute_queue);

				free_cmd_buffer(core::logical_device(), _compute_pool, _cmd_buffer);

				for (auto id : _descriptor_set_ids)
				{
					remove_data(engine_vulkan_data::vulkan_descriptor_sets, id);
				}

				remove_data(engine_vulkan_data::vulkan_descriptor_set_layout, _descriptor_set_layout_id);

				remove_data(engine_vulkan_data::vulkan_descriptor_pool, _descriptor_pool_id);

				remove_data(engine_vulkan_data::vulkan_pipeline, _pipeline_id);

				remove_data(engine_vulkan_data::vulkan_pipeline_layout, _pipeline_layout_id);

				vkDestroySemaphore(core::logical_device(), _signal_semaphore, nullptr);
				vkDestroySemaphore(core::logical_device(), _wait_semaphore, nullptr);
				
				vkDestroyFence(core::logical_device(), _fence.fence, nullptr);

				_input_buffers.clear();
				_input_images.clear();
				_output_buffers.clear();
				_output_images.clear();
				_shader_id = id::invalid_id;
				_set_count = 0;
			}

			virtual void rebuild_pipeline(id::id_type shader_id)
			{
				vkQueueWaitIdle(_compute_queue);

				data::remove_data(data::engine_vulkan_data::vulkan_pipeline, _pipeline_id);

				_shader_id = shader_id;

				create_pipeline();
			}

			virtual void run(u32 dispatch_x, u32 dispatch_y, u32 dispatch_z)
			{
				runRenderpass(dispatch_x, dispatch_y, dispatch_z);
			}

			virtual void submit(u32 wait_num, VkSemaphore* pWait)
			{
				compute_submit(wait_num, pWait);
			}

			[[nodiscard]] constexpr VkSemaphore const signal_semaphore() const { return _signal_semaphore; }

		protected:
			[[nodiscard]] utl::vector<id::id_type> const get_descriptor_sets() const { return _descriptor_set_ids; }
			[[nodiscard]] constexpr id::id_type const get_descriptor_pool() const { return _descriptor_pool_id; }
			[[nodiscard]] constexpr id::id_type const get_descriptor_set_layout() const { return _descriptor_set_layout_id; }

		private:
			utl::vector<set_data>							_input_buffers;
			utl::vector<set_data>							_input_images;
			utl::vector<set_data>							_output_buffers;
			utl::vector<set_data>							_output_images;
			id::id_type										_shader_id{ id::invalid_id };

			u32												_set_count{ 0 };
			id::id_type										_descriptor_pool_id;
			id::id_type										_descriptor_set_layout_id;
			id::id_type										_pipeline_layout_id;
			id::id_type										_pipeline_id;
			utl::vector<id::id_type>						_descriptor_set_ids;
			vulkan_cmd_buffer								_cmd_buffer;
			VkSemaphore										_signal_semaphore;
			VkSemaphore										_wait_semaphore;
			VkQueue											_compute_queue;
			vulkan_fence									_fence;

		private:

			virtual void createPoolAndLayout()
			{
				utl::vector<VkDescriptorPoolSize> poolSize;

				if ((_input_buffers.size() + _output_buffers.size()) > 0)
				{
					u32 uniform_count{ 0 };
					u32 storage_count{ 0 };
					for (auto buffer : _input_buffers)
					{
						switch (data::get_data<data::vulkan_buffer>(buffer.data_id).own_type)
						{
						case data::vulkan_buffer::static_uniform_buffer:
						case data::vulkan_buffer::per_frame_update_uniform_buffer:
						{
							uniform_count++;
						}	break;
						case data::vulkan_buffer::static_storage_buffer:
						case data::vulkan_buffer::per_frame_update_storage_buffer:
						{
							storage_count++;
						}	break;
						default:
							std::runtime_error("This is invalid buffer type!!!");
							break;
						}
					}
					poolSize.emplace_back(descriptor::descriptorPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, static_cast<u32>(frame_buffer_count * 3 * (storage_count + _output_buffers.size()))));
					if(uniform_count != 0)
						poolSize.emplace_back(descriptor::descriptorPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, static_cast<u32>(frame_buffer_count * 3 * (uniform_count))));
				}
				if (_input_images.size() > 0)
					poolSize.emplace_back(descriptor::descriptorPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, static_cast<u32>(frame_buffer_count * 3 * _input_images.size())));
				if (_output_images.size() > 0)
					poolSize.emplace_back(descriptor::descriptorPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, static_cast<u32>(frame_buffer_count * 3 * _output_buffers.size())));

				VkDescriptorPoolCreateInfo poolInfo;
				poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
				poolInfo.pNext = VK_NULL_HANDLE;
				poolInfo.flags = 0;
				poolInfo.poolSizeCount = static_cast<u32>(poolSize.size());
				poolInfo.pPoolSizes = poolSize.data();
				poolInfo.maxSets = _set_count;

				_descriptor_pool_id = data::create_data(data::engine_vulkan_data::vulkan_descriptor_pool, static_cast<void*>(&poolInfo), 0);

				{
					utl::vector<VkDescriptorSetLayoutBinding> setLayoutBindings;
					for (auto buffer : _input_buffers)
					{
						switch (data::get_data<data::vulkan_buffer>(buffer.data_id).own_type)
						{
						case data::vulkan_buffer::static_uniform_buffer:
						case data::vulkan_buffer::per_frame_update_uniform_buffer:
						{
							setLayoutBindings.emplace_back(descriptor::descriptorSetLayoutBinding(buffer.binding_num, VK_SHADER_STAGE_COMPUTE_BIT, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER));
						}	break;
						case data::vulkan_buffer::static_storage_buffer:
						case data::vulkan_buffer::per_frame_update_storage_buffer:
						{
							setLayoutBindings.emplace_back(descriptor::descriptorSetLayoutBinding(buffer.binding_num, VK_SHADER_STAGE_COMPUTE_BIT, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER));
						}	break;
						default:
							std::runtime_error("This is invalid buffer type!!!");
							break;
						}
					}
					for (auto image : _input_images)
					{
						setLayoutBindings.emplace_back(descriptor::descriptorSetLayoutBinding(image.binding_num, VK_SHADER_STAGE_COMPUTE_BIT, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER));
					}
					for (auto buffer : _output_buffers)
					{
						setLayoutBindings.emplace_back(descriptor::descriptorSetLayoutBinding(buffer.binding_num, VK_SHADER_STAGE_COMPUTE_BIT, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER));
					}
					for (auto image : _output_images)
					{
						setLayoutBindings.emplace_back(descriptor::descriptorSetLayoutBinding(image.binding_num, VK_SHADER_STAGE_COMPUTE_BIT, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE));
					}

					VkDescriptorSetLayoutCreateInfo descriptorLayout = descriptor::descriptorSetLayoutCreate(setLayoutBindings);
					_descriptor_set_layout_id = data::create_data(data::engine_vulkan_data::vulkan_descriptor_set_layout, static_cast<void*>(&descriptorLayout), 0);
				}

				VkPipelineLayoutCreateInfo pipelineLayoutInfo;
				pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
				pipelineLayoutInfo.pNext = nullptr;
				pipelineLayoutInfo.flags = 0;
				pipelineLayoutInfo.setLayoutCount = 1;
				pipelineLayoutInfo.pSetLayouts = &data::get_data<VkDescriptorSetLayout>(_descriptor_set_layout_id);
				pipelineLayoutInfo.pushConstantRangeCount = 0;
				pipelineLayoutInfo.pPushConstantRanges = nullptr;
				_pipeline_layout_id = data::create_data(data::engine_vulkan_data::vulkan_pipeline_layout, static_cast<void*>(&pipelineLayoutInfo), 0);
			}

			virtual void create_descriptor_set()
			{
				VkDescriptorSetAllocateInfo allocInfo;

				allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
				allocInfo.pNext = VK_NULL_HANDLE;
				allocInfo.descriptorPool = data::get_data<VkDescriptorPool>(_descriptor_pool_id);
				allocInfo.descriptorSetCount = 1;
				allocInfo.pSetLayouts = &data::get_data<VkDescriptorSetLayout>(_descriptor_set_layout_id);

				for (u32 i{ 0 }; i < _set_count; ++i)
				{
					_descriptor_set_ids.emplace_back(data::create_data(data::engine_vulkan_data::vulkan_descriptor_sets, static_cast<void*>(&allocInfo), 0));
				}

				for (auto buffer : _input_buffers)
				{
					VkDescriptorBufferInfo bufferInfo;
					auto data = data::get_data<data::vulkan_buffer>(buffer.data_id);
					bufferInfo.buffer = data.cpu_address;
					bufferInfo.offset = 0;
					bufferInfo.range = data.size;
					VkWriteDescriptorSet write = descriptor::setWriteDescriptorSet(
						VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
						data::get_data<VkDescriptorSet>(_descriptor_set_ids[buffer.set_num]),
						buffer.binding_num,
						VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
						&bufferInfo);
					vkUpdateDescriptorSets(core::logical_device(), 1, &write, 0, nullptr);
				}

				for (auto img : _input_images)
				{
					VkDescriptorImageInfo imageInfo;
					auto data = textures::get_texture(img.data_id).getTexture();
					imageInfo.imageLayout = (data.format == VK_FORMAT_D16_UNORM || data.format == VK_FORMAT_D32_SFLOAT) ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
					imageInfo.imageView = data.view;
					imageInfo.sampler = data.sampler;
					VkWriteDescriptorSet write = descriptor::setWriteDescriptorSet(
						VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
						data::get_data<VkDescriptorSet>(_descriptor_set_ids[img.set_num]),
						img.binding_num,
						VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
						&imageInfo);
					vkUpdateDescriptorSets(core::logical_device(), 1, &write, 0, nullptr);
				}

				for (auto buffer : _output_buffers)
				{
					VkDescriptorBufferInfo bufferInfo;
					auto data = data::get_data<data::vulkan_buffer>(buffer.data_id);
					bufferInfo.buffer = data.cpu_address;
					bufferInfo.offset = 0;
					bufferInfo.range = data.size;
					VkWriteDescriptorSet write = descriptor::setWriteDescriptorSet(
						VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
						data::get_data<VkDescriptorSet>(_descriptor_set_ids[buffer.set_num]),
						buffer.binding_num,
						VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
						&bufferInfo);
					vkUpdateDescriptorSets(core::logical_device(), 1, &write, 0, nullptr);
				}

				for (auto img : _output_images)
				{
					VkDescriptorImageInfo imageInfo;
					auto data = textures::get_texture(img.data_id).getTexture();
					imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
					imageInfo.imageView = data.view;
					VkWriteDescriptorSet write = descriptor::setWriteDescriptorSet(
						VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
						data::get_data<VkDescriptorSet>(_descriptor_set_ids[img.set_num]),
						img.binding_num,
						VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
						&imageInfo);
					vkUpdateDescriptorSets(core::logical_device(), 1, &write, 0, nullptr);
				}
			}

			virtual void create_pipeline()
			{
				assert(id::is_valid(_shader_id));
				auto ps = data::get_data<VkPipelineLayout>(_pipeline_layout_id);
				VkComputePipelineCreateInfo info;
				info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
				info.pNext = nullptr;
				info.layout = ps;
				info.stage = shaders::get_shader(_shader_id).getShaderStage();
				info.flags = 0;
				info.basePipelineHandle = nullptr;

				_pipeline_id = data::create_data(data::engine_vulkan_data::vulkan_pipeline, static_cast<void*>(&info), 1);
			}

			virtual void create_command_buffer()
			{
				if (_cmd_buffer.cmd_buffer)
				{
					vkFreeCommandBuffers(core::logical_device(), _compute_pool, 1, &_cmd_buffer.cmd_buffer);

					_cmd_buffer.cmd_buffer = nullptr;
					_cmd_buffer.cmd_state = vulkan_cmd_buffer::CMD_NOT_ALLOCATED;
				}
				_cmd_buffer = allocate_cmd_buffer(core::logical_device(), _compute_pool, true);
			}

			virtual void runRenderpass(u32 dispatch_x, u32 dispatch_y, u32 dispatch_z)
			{
				_cmd_buffer.cmd_buffer = beginSingleCommand(core::logical_device(), _compute_pool);
				_cmd_buffer.cmd_state = vulkan_cmd_buffer::CMD_IN_RENDER_PASS;
				auto pipeline = data::get_data<VkPipeline>(_pipeline_id);
				auto pipelineLayout = data::get_data<VkPipelineLayout>(_pipeline_layout_id);
				auto set = data::get_data<VkDescriptorSet>(_descriptor_set_ids.font());
				vkCmdBindPipeline(_cmd_buffer.cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
				vkCmdBindDescriptorSets(_cmd_buffer.cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &set, 0, 0);

				vkCmdDispatch(_cmd_buffer.cmd_buffer, dispatch_x, dispatch_y, dispatch_z);

				vkEndCommandBuffer(_cmd_buffer.cmd_buffer);
			}

			virtual void compute_submit(u32 wait_num, VkSemaphore* pWait)
			{
				VkSubmitInfo submitInfo;
				submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
				submitInfo.pNext = nullptr;
				submitInfo.commandBufferCount = 1;
				submitInfo.pCommandBuffers = &_cmd_buffer.cmd_buffer;
				submitInfo.waitSemaphoreCount = wait_num;
				submitInfo.pWaitSemaphores = pWait;
				submitInfo.signalSemaphoreCount = 1;
				submitInfo.pSignalSemaphores = &_signal_semaphore;
				VkPipelineStageFlags flags[4]{ VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT ,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT };
				submitInfo.pWaitDstStageMask = flags;
				VkResult result{ VK_SUCCESS };
				VkCall(result = vkQueueSubmit(_compute_queue, 1, &submitInfo, nullptr), "Failed to submit compute queue...");
			}

			void create_semaphore_and_fence()
			{
				VkResult result{ VK_SUCCESS };
				VkSemaphoreCreateInfo sInfo;
				sInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
				sInfo.pNext = nullptr;
				sInfo.flags = 0;
				VkCall(result = vkCreateSemaphore(core::logical_device(), &sInfo, nullptr, &_signal_semaphore), "Failed to create compute signal semaphore...");
				VkCall(result = vkCreateSemaphore(core::logical_device(), &sInfo, nullptr, &_wait_semaphore), "Failed to create compute wait semaphore...");

				_fence.signaled = true;

				VkFenceCreateInfo f_info{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
				f_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;	// this ensures the fence starts signaled as open

				VkCall(result = vkCreateFence(core::logical_device(), &f_info, nullptr, &_fence.fence), "Failed to create fence");
			}

			void get_graphics_queue()
			{
				vkGetDeviceQueue(core::logical_device(), core::compute_family_queue_index(), 0, &_compute_queue);
			}
		};

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

		class frustum_pass : public vulkan_base_compute_pass
		{
		public:
			frustum_pass() = default;

			~frustum_pass()
			{

			}

			virtual void shutdown() override
			{
				vulkan_base_compute_pass::shutdown();
				is_rendered = false;
				is_submited = false;
			}

			virtual void run(u32 dispatch_x = 0, u32 dispatch_y = 0, u32 dispatch_z = 0) override
			{
				const u32 tile_size{ 16 };
				const math::u32v2 tile_count
				{
					(u32)math::align_size_up<tile_size>(1600) / tile_size,
					(u32)math::align_size_up<tile_size>(900) / tile_size,
				};

				u32 NumThreadGroupX = (u32)math::align_size_up<tile_size>(tile_count.x) / tile_size;
				u32 NumThreadGroupY = (u32)math::align_size_up<tile_size>(tile_count.y) / tile_size;

				if (!is_rendered)
				{
					vulkan_base_compute_pass::run(NumThreadGroupX, NumThreadGroupY, 1);
					is_rendered = !is_rendered;
				}
			}

			virtual void submit(u32 wait_num, VkSemaphore* pWait) override
			{
				if (!is_submited)
				{
					vulkan_base_compute_pass::submit(wait_num, pWait);
					is_submited = !is_submited;
				}
			}

			[[nodiscard]] constexpr VkSemaphore const get_signal_semaphore() const { return vulkan_base_compute_pass::signal_semaphore(); }
			[[nodiscard]] constexpr bool const Is_Rendered() const { return is_rendered; }

		private:
			bool							is_rendered{ false };
			bool							is_submited{ false };

		private:
		};

		class culling_light_pass
		{
		public:
			culling_light_pass() = default;

			~culling_light_pass()
			{

			}

			void shutdown()
			{
				using namespace data;
				vkQueueWaitIdle(_compute_queue);

				free_cmd_buffer(core::logical_device(), _compute_pool, _cmd_buffer);

				for (auto id : _descriptor_set_ids)
				{
					remove_data(engine_vulkan_data::vulkan_descriptor_sets, id);
				}

				remove_data(engine_vulkan_data::vulkan_descriptor_set_layout, _descriptor_set_layout_id);

				remove_data(engine_vulkan_data::vulkan_descriptor_pool, _descriptor_pool_id);

				remove_data(engine_vulkan_data::vulkan_pipeline, _pipeline_id);

				remove_data(engine_vulkan_data::vulkan_pipeline_layout, _pipeline_layout_id);

				vkDestroySemaphore(core::logical_device(), _signal_semaphore, nullptr);
				vkDestroySemaphore(core::logical_device(), _wait_semaphore, nullptr);

				vkDestroyFence(core::logical_device(), _fence.fence, nullptr);

				_input_buffers.clear();
				_input_images.clear();
				_output_buffers.clear();
				_output_images.clear();
				_shader_id = id::invalid_id;
				_set_count = 0;
				is_rendered = false;
				is_submited = false;
			}

			void run(u32 dispatch_x = 0, u32 dispatch_y = 0, u32 dispatch_z = 0)
			{
				const u32 tile_size{ 16 };
				const math::u32v2 tile_count
				{
					(u32)math::align_size_up<tile_size>(1600) / tile_size,
					(u32)math::align_size_up<tile_size>(900) / tile_size,
				};

				u32 NumThreadGroupX = tile_count.x;
				u32 NumThreadGroupY = tile_count.y;

				if (!is_rendered)
				{
					is_rendered = !is_rendered;
					return;
				}

				runRenderpass(NumThreadGroupX, NumThreadGroupY, 1);
			}

			void submit(u32 wait_num, VkSemaphore* pWait)
			{
				if (!is_submited)
				{
					is_submited = !is_submited;
					return;
				}
				compute_submit(wait_num, pWait);
			}

			[[nodiscard]] constexpr VkSemaphore const get_singal_semaphore() const { return _signal_semaphore; }
			[[nodiscard]] constexpr bool const Is_Rendered() const { return is_rendered; }

		public:
			enum data_io : u32
			{
				input = 0,
				output
			};

			enum data_type : u32
			{
				buffer = 0,
				image,
				shader
			};

			struct set_data
			{
				u32					set_num;
				u32					binding_num;
				id::id_type			data_id;
			};

			culling_light_pass& add_resource(data_io io_type, data_type resource_type, u32 set_num, u32 binding_num, id::id_type data_id)
			{
				switch (resource_type)
				{
				case culling_light_pass::buffer:
				{
					switch (io_type)
					{
					case culling_light_pass::input:
						_input_buffers.emplace_back(set_data{ set_num, binding_num, data_id });
						break;
					case culling_light_pass::output:
						_output_buffers.emplace_back(set_data{ set_num, binding_num, data_id });
						break;
					default:
						std::runtime_error("This must be a legitimate type, input or output");
						break;
					}
				}
				break;
				case culling_light_pass::image:
				{
					switch (io_type)
					{
					case culling_light_pass::input:
						_input_images.emplace_back(set_data{ set_num, binding_num, data_id });
						break;
					case culling_light_pass::output:
						_output_images.emplace_back(set_data{ set_num, binding_num, data_id });
						break;
					default:
						std::runtime_error("This must be a legitimate type, input or output");
						break;
					}
				}
				break;
				case culling_light_pass::shader:
					_shader_id = data_id;
					break;
				default:
					std::runtime_error("This must be a legitimate type, image or buffer or shader");
					break;
				}

				_set_count = std::max(_set_count, set_num);

				return *this;
			}

			void initialize()
			{
				_set_count += 1;

				assert(!_input_buffers.empty() || !_input_images.empty() || !_output_buffers.empty() || !_output_images.empty() || id::is_valid(_shader_id));

				get_graphics_queue();

				create_semaphore_and_fence();

				createPoolAndLayout();

				create_pipeline();

				create_command_buffer();

				create_descriptor_set();
			}

			void rebuild_pipeline(id::id_type shader_id)
			{
				vkQueueWaitIdle(_compute_queue);

				data::remove_data(data::engine_vulkan_data::vulkan_pipeline, _pipeline_id);

				_shader_id = shader_id;

				create_pipeline();
			}


			[[nodiscard]] constexpr VkSemaphore const signal_semaphore() const { return _signal_semaphore; }

		protected:
			[[nodiscard]] utl::vector<id::id_type> const get_descriptor_sets() const { return _descriptor_set_ids; }
			[[nodiscard]] constexpr id::id_type const get_descriptor_pool() const { return _descriptor_pool_id; }
			[[nodiscard]] constexpr id::id_type const get_descriptor_set_layout() const { return _descriptor_set_layout_id; }

		private:
			utl::vector<set_data>							_input_buffers;
			utl::vector<set_data>							_input_images;
			utl::vector<set_data>							_output_buffers;
			utl::vector<set_data>							_output_images;
			id::id_type										_shader_id{ id::invalid_id };

			u32												_set_count{ 0 };
			id::id_type										_descriptor_pool_id;
			id::id_type										_descriptor_set_layout_id;
			id::id_type										_light_descriptor_set_layout_id;
			id::id_type										_pipeline_layout_id;
			id::id_type										_pipeline_id;
			utl::vector<id::id_type>						_descriptor_set_ids;
			vulkan_cmd_buffer								_cmd_buffer;
			VkSemaphore										_signal_semaphore;
			VkSemaphore										_wait_semaphore;
			VkQueue											_compute_queue;
			vulkan_fence									_fence;

			bool											is_submited{ false };
			bool											is_rendered{ false };

		private:

			void createPoolAndLayout()
			{
				utl::vector<VkDescriptorPoolSize> poolSize;

				if ((_input_buffers.size() + _output_buffers.size()) > 0)
				{
					u32 uniform_count{ 0 };
					u32 storage_count{ 0 };
					for (auto buffer : _input_buffers)
					{
						switch (data::get_data<data::vulkan_buffer>(buffer.data_id).own_type)
						{
						case data::vulkan_buffer::static_uniform_buffer:
						case data::vulkan_buffer::per_frame_update_uniform_buffer:
						{
							uniform_count++;
						}	break;
						case data::vulkan_buffer::static_storage_buffer:
						case data::vulkan_buffer::per_frame_update_storage_buffer:
						{
							storage_count++;
						}	break;
						default:
							std::runtime_error("This is invalid buffer type!!!");
							break;
						}
					}
					poolSize.emplace_back(descriptor::descriptorPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, static_cast<u32>(frame_buffer_count * 3 * (storage_count + _output_buffers.size()))));
					if (uniform_count != 0)
						poolSize.emplace_back(descriptor::descriptorPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, static_cast<u32>(frame_buffer_count * 3 * (uniform_count))));
				}
				if (_input_images.size() > 0)
					poolSize.emplace_back(descriptor::descriptorPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, static_cast<u32>(frame_buffer_count * 3 * _input_images.size())));
				if (_output_images.size() > 0)
					poolSize.emplace_back(descriptor::descriptorPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, static_cast<u32>(frame_buffer_count * 3 * _output_buffers.size())));

				VkDescriptorPoolCreateInfo poolInfo;
				poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
				poolInfo.pNext = VK_NULL_HANDLE;
				poolInfo.flags = 0;
				poolInfo.poolSizeCount = static_cast<u32>(poolSize.size());
				poolInfo.pPoolSizes = poolSize.data();
				poolInfo.maxSets = _set_count;

				_descriptor_pool_id = data::create_data(data::engine_vulkan_data::vulkan_descriptor_pool, static_cast<void*>(&poolInfo), 0);

				{
					utl::vector<VkDescriptorSetLayoutBinding> setLayoutBindings;
					for (auto buffer : _input_buffers)
					{
						switch (data::get_data<data::vulkan_buffer>(buffer.data_id).own_type)
						{
						case data::vulkan_buffer::static_uniform_buffer:
						case data::vulkan_buffer::per_frame_update_uniform_buffer:
						{
							setLayoutBindings.emplace_back(descriptor::descriptorSetLayoutBinding(buffer.binding_num, VK_SHADER_STAGE_COMPUTE_BIT, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER));
						}	break;
						case data::vulkan_buffer::static_storage_buffer:
						case data::vulkan_buffer::per_frame_update_storage_buffer:
						{
							setLayoutBindings.emplace_back(descriptor::descriptorSetLayoutBinding(buffer.binding_num, VK_SHADER_STAGE_COMPUTE_BIT, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER));
						}	break;
						default:
							std::runtime_error("This is invalid buffer type!!!");
							break;
						}
					}
					for (auto image : _input_images)
					{
						setLayoutBindings.emplace_back(descriptor::descriptorSetLayoutBinding(image.binding_num, VK_SHADER_STAGE_COMPUTE_BIT, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER));
					}
					for (auto buffer : _output_buffers)
					{
						setLayoutBindings.emplace_back(descriptor::descriptorSetLayoutBinding(buffer.binding_num, VK_SHADER_STAGE_COMPUTE_BIT, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER));
					}
					for (auto image : _output_images)
					{
						setLayoutBindings.emplace_back(descriptor::descriptorSetLayoutBinding(image.binding_num, VK_SHADER_STAGE_COMPUTE_BIT, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE));
					}

					VkDescriptorSetLayoutCreateInfo descriptorLayout = descriptor::descriptorSetLayoutCreate(setLayoutBindings);
					_descriptor_set_layout_id = data::create_data(data::engine_vulkan_data::vulkan_descriptor_set_layout, static_cast<void*>(&descriptorLayout), 0);
				}

				{
					std::vector<VkDescriptorSetLayoutBinding> lightSetLayoutBindings{
						descriptor::descriptorSetLayoutBinding(0, VK_SHADER_STAGE_COMPUTE_BIT, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
						descriptor::descriptorSetLayoutBinding(1, VK_SHADER_STAGE_COMPUTE_BIT, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
						descriptor::descriptorSetLayoutBinding(2, VK_SHADER_STAGE_COMPUTE_BIT, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
						descriptor::descriptorSetLayoutBinding(3, VK_SHADER_STAGE_COMPUTE_BIT, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
					};
					VkDescriptorSetLayoutCreateInfo lightDescriptorLayout = descriptor::descriptorSetLayoutCreate(lightSetLayoutBindings.data(), lightSetLayoutBindings.size());
					lightDescriptorLayout.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
					_light_descriptor_set_layout_id = data::create_data(data::engine_vulkan_data::vulkan_descriptor_set_layout, static_cast<void*>(&lightDescriptorLayout), 0);
				}

				std::vector<VkDescriptorSetLayout> descriptorSetArray{ data::get_data<VkDescriptorSetLayout>(_descriptor_set_layout_id), data::get_data<VkDescriptorSetLayout>(_light_descriptor_set_layout_id) };

				VkPipelineLayoutCreateInfo pipelineLayoutInfo;
				pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
				pipelineLayoutInfo.pNext = nullptr;
				pipelineLayoutInfo.flags = 0;
				pipelineLayoutInfo.setLayoutCount = descriptorSetArray.size();
				pipelineLayoutInfo.pSetLayouts = descriptorSetArray.data();
				pipelineLayoutInfo.pushConstantRangeCount = 0;
				pipelineLayoutInfo.pPushConstantRanges = nullptr;
				_pipeline_layout_id = data::create_data(data::engine_vulkan_data::vulkan_pipeline_layout, static_cast<void*>(&pipelineLayoutInfo), 0);
			}

			void create_descriptor_set()
			{
				VkDescriptorSetAllocateInfo allocInfo;

				allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
				allocInfo.pNext = VK_NULL_HANDLE;
				allocInfo.descriptorPool = data::get_data<VkDescriptorPool>(_descriptor_pool_id);
				allocInfo.descriptorSetCount = 1;
				allocInfo.pSetLayouts = &data::get_data<VkDescriptorSetLayout>(_descriptor_set_layout_id);

				for (u32 i{ 0 }; i < _set_count; ++i)
				{
					_descriptor_set_ids.emplace_back(data::create_data(data::engine_vulkan_data::vulkan_descriptor_sets, static_cast<void*>(&allocInfo), 0));
				}

				for (auto buffer : _input_buffers)
				{
					VkDescriptorBufferInfo bufferInfo;
					auto data = data::get_data<data::vulkan_buffer>(buffer.data_id);
					bufferInfo.buffer = data.cpu_address;
					bufferInfo.offset = 0;
					bufferInfo.range = data.size;
					VkWriteDescriptorSet write = descriptor::setWriteDescriptorSet(
						VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
						data::get_data<VkDescriptorSet>(_descriptor_set_ids[buffer.set_num]),
						buffer.binding_num,
						VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
						&bufferInfo);
					vkUpdateDescriptorSets(core::logical_device(), 1, &write, 0, nullptr);
				}

				for (auto img : _input_images)
				{
					VkDescriptorImageInfo imageInfo;
					auto data = textures::get_texture(img.data_id).getTexture();
					imageInfo.imageLayout = (data.format == VK_FORMAT_D16_UNORM || data.format == VK_FORMAT_D32_SFLOAT) ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
					imageInfo.imageView = data.view;
					imageInfo.sampler = data.sampler;
					VkWriteDescriptorSet write = descriptor::setWriteDescriptorSet(
						VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
						data::get_data<VkDescriptorSet>(_descriptor_set_ids[img.set_num]),
						img.binding_num,
						VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
						&imageInfo);
					vkUpdateDescriptorSets(core::logical_device(), 1, &write, 0, nullptr);
				}

				for (auto buffer : _output_buffers)
				{
					VkDescriptorBufferInfo bufferInfo;
					auto data = data::get_data<data::vulkan_buffer>(buffer.data_id);
					bufferInfo.buffer = data.cpu_address;
					bufferInfo.offset = 0;
					bufferInfo.range = data.size;
					VkWriteDescriptorSet write = descriptor::setWriteDescriptorSet(
						VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
						data::get_data<VkDescriptorSet>(_descriptor_set_ids[buffer.set_num]),
						buffer.binding_num,
						VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
						&bufferInfo);
					vkUpdateDescriptorSets(core::logical_device(), 1, &write, 0, nullptr);
				}

				for (auto img : _output_images)
				{
					VkDescriptorImageInfo imageInfo;
					auto data = textures::get_texture(img.data_id).getTexture();
					imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
					imageInfo.imageView = data.view;
					VkWriteDescriptorSet write = descriptor::setWriteDescriptorSet(
						VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
						data::get_data<VkDescriptorSet>(_descriptor_set_ids[img.set_num]),
						img.binding_num,
						VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
						&imageInfo);
					vkUpdateDescriptorSets(core::logical_device(), 1, &write, 0, nullptr);
				}
			}

			void create_pipeline()
			{
				assert(id::is_valid(_shader_id));
				auto ps = data::get_data<VkPipelineLayout>(_pipeline_layout_id);
				VkComputePipelineCreateInfo info;
				info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
				info.pNext = nullptr;
				info.layout = ps;
				info.stage = shaders::get_shader(_shader_id).getShaderStage();
				info.flags = 0;
				info.basePipelineHandle = nullptr;

				_pipeline_id = data::create_data(data::engine_vulkan_data::vulkan_pipeline, static_cast<void*>(&info), 1);
			}

			void create_command_buffer()
			{
				if (_cmd_buffer.cmd_buffer)
				{
					vkFreeCommandBuffers(core::logical_device(), _compute_pool, 1, &_cmd_buffer.cmd_buffer);

					_cmd_buffer.cmd_buffer = nullptr;
					_cmd_buffer.cmd_state = vulkan_cmd_buffer::CMD_NOT_ALLOCATED;
				}
				_cmd_buffer = allocate_cmd_buffer(core::logical_device(), _compute_pool, true);
			}

			void runRenderpass(u32 dispatch_x, u32 dispatch_y, u32 dispatch_z)
			{
				_cmd_buffer.cmd_buffer = beginSingleCommand(core::logical_device(), _compute_pool);
				_cmd_buffer.cmd_state = vulkan_cmd_buffer::CMD_IN_RENDER_PASS;
				auto pipeline = data::get_data<VkPipeline>(_pipeline_id);
				auto pipelineLayout = data::get_data<VkPipelineLayout>(_pipeline_layout_id);
				auto set = data::get_data<VkDescriptorSet>(_descriptor_set_ids.font());
				vkCmdBindDescriptorSets(_cmd_buffer.cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &set, 0, 0);

				VkWriteDescriptorSet write;
				VkDescriptorBufferInfo lightBuffer;
				auto buffer = data::get_data<data::vulkan_buffer>(light::culling_info_buffer_id());
				lightBuffer.buffer = buffer.cpu_address;
				lightBuffer.offset = 0;
				lightBuffer.range = buffer.size;
				write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
				write.pNext = nullptr;
				write.dstSet = 0;
				write.dstBinding = 0;
				write.descriptorCount = 1;
				write.dstArrayElement = 0;
				write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
				write.pBufferInfo = &lightBuffer;
				vkCmdPushDescriptorSetKHR(_cmd_buffer.cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 1, 1, &write);

				VkWriteDescriptorSet write1;
				VkDescriptorBufferInfo lightgridBuffer;
				auto lightgrid = data::get_data<data::vulkan_buffer>(compute::culling_light_grid());
				lightgridBuffer.buffer = lightgrid.cpu_address;
				lightgridBuffer.offset = 0;
				lightgridBuffer.range = lightgrid.size;
				write1.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
				write1.pNext = nullptr;
				write1.dstSet = 0;
				write1.dstBinding = 1;
				write1.descriptorCount = 1;
				write1.dstArrayElement = 0;
				write1.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
				write1.pBufferInfo = &lightgridBuffer;
				vkCmdPushDescriptorSetKHR(_cmd_buffer.cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 1, 1, &write1);

				VkWriteDescriptorSet write2;
				VkDescriptorBufferInfo lightIndexBuffer;
				auto lightIndex = data::get_data<data::vulkan_buffer>(compute::culling_light_list());
				lightIndexBuffer.buffer = lightIndex.cpu_address;
				lightIndexBuffer.offset = 0;
				lightIndexBuffer.range = lightIndex.size;
				write2.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
				write2.pNext = nullptr;
				write2.dstSet = 0;
				write2.dstBinding = 2;
				write2.descriptorCount = 1;
				write2.dstArrayElement = 0;
				write2.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
				write2.pBufferInfo = &lightIndexBuffer;
				vkCmdPushDescriptorSetKHR(_cmd_buffer.cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 1, 1, &write2);

				VkWriteDescriptorSet write3;
				VkDescriptorBufferInfo boundingSpheresBuffer;
				auto boundingSpheres = data::get_data<data::vulkan_buffer>(light::bounding_spheres_buffer_id());
				boundingSpheresBuffer.buffer = boundingSpheres.cpu_address;
				boundingSpheresBuffer.offset = 0;
				boundingSpheresBuffer.range = boundingSpheres.size;
				write3.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
				write3.pNext = nullptr;
				write3.dstSet = 0;
				write3.dstBinding = 3;
				write3.descriptorCount = 1;
				write3.dstArrayElement = 0;
				write3.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
				write3.pBufferInfo = &boundingSpheresBuffer;
				vkCmdPushDescriptorSetKHR(_cmd_buffer.cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 1, 1, &write3);

				vkCmdBindPipeline(_cmd_buffer.cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);

				vkCmdDispatch(_cmd_buffer.cmd_buffer, dispatch_x, dispatch_y, dispatch_z);

				vkEndCommandBuffer(_cmd_buffer.cmd_buffer);
			}

			void compute_submit(u32 wait_num, VkSemaphore* pWait)
			{
				VkSubmitInfo submitInfo;
				submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
				submitInfo.pNext = nullptr;
				submitInfo.commandBufferCount = 1;
				submitInfo.pCommandBuffers = &_cmd_buffer.cmd_buffer;
				submitInfo.waitSemaphoreCount = wait_num;
				submitInfo.pWaitSemaphores = pWait;
				submitInfo.signalSemaphoreCount = 1;
				submitInfo.pSignalSemaphores = &_signal_semaphore;
				VkPipelineStageFlags flags[4]{ VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT ,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT };
				submitInfo.pWaitDstStageMask = flags;
				VkResult result{ VK_SUCCESS };
				VkCall(result = vkQueueSubmit(_compute_queue, 1, &submitInfo, nullptr), "Failed to submit compute queue...");
			}

			void create_semaphore_and_fence()
			{
				VkResult result{ VK_SUCCESS };
				VkSemaphoreCreateInfo sInfo;
				sInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
				sInfo.pNext = nullptr;
				sInfo.flags = 0;
				VkCall(result = vkCreateSemaphore(core::logical_device(), &sInfo, nullptr, &_signal_semaphore), "Failed to create compute signal semaphore...");
				VkCall(result = vkCreateSemaphore(core::logical_device(), &sInfo, nullptr, &_wait_semaphore), "Failed to create compute wait semaphore...");

				_fence.signaled = true;

				VkFenceCreateInfo f_info{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
				f_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;	// this ensures the fence starts signaled as open

				VkCall(result = vkCreateFence(core::logical_device(), &f_info, nullptr, &_fence.fence), "Failed to create fence");
			}

			void get_graphics_queue()
			{
				vkGetDeviceQueue(core::logical_device(), core::compute_family_queue_index(), 0, &_compute_queue);
			}
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

		//vulkan_compute					_frustums_compute;
		id::id_type						_storage_in_id{ id::invalid_id };
		id::id_type						_storage_out_id{ id::invalid_id };

		frustum_pass					_frustum_pass;

		culling_light_pass				_cull_light_pass;
		id::id_type						_storage_in_light_info{ id::invalid_id }; // compute::culling_info_buffer_id()
		id::id_type						_storage_in_light_count{ id::invalid_id };
		id::id_type						_stroage_light_count{ id::invalid_id };
		id::id_type						_storage_out_light_grid[frame_buffer_count]{ id::invalid_id, id::invalid_id, id::invalid_id };
		id::id_type						_storage_out_light_list[frame_buffer_count]{ id::invalid_id, id::invalid_id, id::invalid_id };
	} // anonymous namespace

	void initialize(vulkan_geometry_pass* gpass)
	{
		create_command_pool_and_semaphore();
		auto flags = data::vulkan_buffer::static_storage_buffer;
		_storage_in_id = data::create_data(data::engine_vulkan_data::vulkan_buffer, (void*)(&flags), sizeof(glsl::GlobalShaderData));
		_storage_out_id = data::create_data(data::engine_vulkan_data::vulkan_buffer, (void*)(&flags), sizeof(math::v4) * 4 * 5700);
		//utl::vector<id::id_type> local_buffer_ids;
		//local_buffer_ids.emplace_back(_storage_in_id);
		//local_buffer_ids.emplace_back(_storage_out_id);
		id::id_type compute_shader_id = shaders::add("C:/Users/zy/Desktop/PrimalMerge/PrimalEngine/Engine/Graphics/Vulkan/Shaders/spv/test.comp.spv", shader_type::compute);
		//_frustums_compute.setup(local_buffer_ids, utl::vector<id::id_type>(), compute_shader_id);
		_frustum_pass.add_resource(vulkan_base_compute_pass::data_io::input,
			vulkan_base_compute_pass::data_type::buffer,
			0,
			0,
			_storage_in_id);
		_frustum_pass.add_resource(vulkan_base_compute_pass::data_io::output,
			vulkan_base_compute_pass::data_type::buffer,
			0,
			1,
			_storage_out_id);
		_frustum_pass.add_resource(vulkan_base_compute_pass::data_io::input,
			vulkan_base_compute_pass::data_type::shader,
			0,
			0,
			compute_shader_id);
		_frustum_pass.initialize();

		_storage_in_light_count = data::create_data(data::engine_vulkan_data::vulkan_buffer, (void*)(&flags), sizeof(u32));
		_stroage_light_count = data::create_data(data::engine_vulkan_data::vulkan_buffer, (void*)(&flags), sizeof(u32));
		_storage_out_light_grid[0] = data::create_data(data::engine_vulkan_data::vulkan_buffer, (void*)(&flags), (u32)math::align_size_up<sizeof(math::v4)>(sizeof(math::u32v2) * 5700));
		_storage_out_light_grid[1] = data::create_data(data::engine_vulkan_data::vulkan_buffer, (void*)(&flags), (u32)math::align_size_up<sizeof(math::v4)>(sizeof(math::u32v2) * 5700));
		_storage_out_light_grid[2] = data::create_data(data::engine_vulkan_data::vulkan_buffer, (void*)(&flags), (u32)math::align_size_up<sizeof(math::v4)>(sizeof(math::u32v2) * 5700));
		_storage_out_light_list[0] = data::create_data(data::engine_vulkan_data::vulkan_buffer, (void*)(&flags), (u32)math::align_size_up<sizeof(math::v4)>(sizeof(u32) * 5700 * 256));
		_storage_out_light_list[1] = data::create_data(data::engine_vulkan_data::vulkan_buffer, (void*)(&flags), (u32)math::align_size_up<sizeof(math::v4)>(sizeof(u32) * 5700 * 256));
		_storage_out_light_list[2] = data::create_data(data::engine_vulkan_data::vulkan_buffer, (void*)(&flags), (u32)math::align_size_up<sizeof(math::v4)>(sizeof(u32) * 5700 * 256));

		id::id_type culling_light_shader_id = shaders::add("C:/Users/zy/Desktop/PrimalMerge/PrimalEngine/Engine/Graphics/Vulkan/Shaders/spv/culling_light.comp.spv", shader_type::compute);

		_cull_light_pass.add_resource(
			culling_light_pass::data_io::input,
			culling_light_pass::data_type::buffer,
			0,
			0,
			_storage_in_id
		);
		_cull_light_pass.add_resource(
			culling_light_pass::data_io::input,
			culling_light_pass::data_type::buffer,
			0,
			1,
			_storage_out_id
		);
		_cull_light_pass.add_resource(
			culling_light_pass::data_io::input,
			culling_light_pass::data_type::buffer,
			0,
			3,
			_storage_in_light_count
		);
		_cull_light_pass.add_resource(
			culling_light_pass::data_io::input,
			culling_light_pass::data_type::image,
			0,
			4,
			gpass->getTexture()[4]
		);
		_cull_light_pass.add_resource(
			culling_light_pass::data_io::input,
			culling_light_pass::data_type::buffer,
			0,
			5,
			_stroage_light_count
		);

		_cull_light_pass.add_resource(
			culling_light_pass::data_io::input,
			culling_light_pass::data_type::shader,
			0,
			0,
			culling_light_shader_id
		);
		_cull_light_pass.initialize();
	}

	void shutdown()
	{
		_frustum_pass.shutdown();

		_cull_light_pass.shutdown();

		vkDestroyCommandPool(core::logical_device(), _compute_pool, nullptr);
	}

	VkSemaphore get_compute_signal_semaphore()
	{
		return _frustum_pass.get_signal_semaphore();
	}

	VkSemaphore get_culling_signal_semaphore()
	{
		return _cull_light_pass.get_singal_semaphore();
	}

	void frustum_run()
	{
		_frustum_pass.run();
	}

	void frustum_submit()
	{
		_frustum_pass.submit(0, nullptr);
	}

	void culling_light_run()
	{
		u32 clear_value{ 0 };
		data::get_data<data::vulkan_buffer>(_stroage_light_count).update(&clear_value, sizeof(u32));
		_cull_light_pass.run();
	}

	void culling_light_submit(u32 wait_nums, VkSemaphore* pWait)
	{

		_cull_light_pass.submit(wait_nums, pWait);
	}

	void run(const void* const data, u32 size)
	{
		//_frustums_compute.run()->submit();
	}

	id::id_type get_input_buffer_id()
	{
		return _storage_in_id;
	}

	id::id_type get_output_buffer_id()
	{
		return _storage_out_id;
	}

	id::id_type culling_in_light_count_id()
	{
		return _storage_in_light_count;
	}

	id::id_type culling_light_grid()
	{
		return _storage_out_light_grid[core::get_frame_index()];
	}

	id::id_type culling_light_list()
	{
		return _storage_out_light_list[core::get_frame_index()];
	}

	bool is_rendered()
	{
		return _frustum_pass.Is_Rendered();
	}
}