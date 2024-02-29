#include "VulkanGBuffer.h"
#include "VulkanHelpers.h"
#include "VulkanCore.h"
#include "VulkanSurface.h"
#include "VulkanCamera.h"
#include "VulkanContent.h"
#include "VulkanResources.h"
#include "VulkanData.h"
#include "VulkanCommandBuffer.h"

#include "EngineAPI/GameEntity.h"
#include "Components/Transform.h"
#include "VulkanCompute.h"
#include "Shaders/ShaderTypes.h"
#include "VulkanLight.h"

#include <array>
#include <random>

namespace primal::graphics::vulkan
{
	namespace
	{
		id::id_type createOffscreenTexture(u32 width, u32 height, bool isPosition, bool singleChannel, OUT utl::vector<VkAttachmentDescription>& attach)
		{
			vulkan_texture tex;

			VkImageCreateInfo image{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
			image.imageType = VK_IMAGE_TYPE_2D;
			image.extent.height = height;
			image.extent.width = width;
			image.extent.depth = 1;
			image.mipLevels = 1;
			image.arrayLayers = 1;
			image.samples = VK_SAMPLE_COUNT_1_BIT;
			image.tiling = VK_IMAGE_TILING_OPTIMAL;
			image.format = singleChannel ? VK_FORMAT_D16_UNORM : (isPosition ? VK_FORMAT_R32G32B32A32_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM);
			image.flags = 0;
			image.usage = (singleChannel ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) | VK_IMAGE_USAGE_SAMPLED_BIT; // ! VK_IMAGE_USAGE_SAMPLED_BIT use to sample directly from the depth attachment for shadow mapping
			VkResult result{ VK_SUCCESS };
			VkCall(result = vkCreateImage(core::logical_device(), &image, nullptr, &tex.image), "Failed to create GBuffer(shadow mapping) image...");

			VkMemoryRequirements memReq;
			vkGetImageMemoryRequirements(core::logical_device(), tex.image, &memReq);
			VkMemoryAllocateInfo alloc{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
			alloc.pNext = nullptr;
			alloc.allocationSize = memReq.size;
			alloc.memoryTypeIndex = core::find_memory_index(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
			result = VK_SUCCESS;
			VkCall(result = vkAllocateMemory(core::logical_device(), &alloc, nullptr, &tex.memory), "Failed to allocate GBuffer(shadow mapping) memory...");
			result = VK_SUCCESS;
			VkCall(result = vkBindImageMemory(core::logical_device(), tex.image, tex.memory, 0), "Failed to bind GBuffer(shadow mapping) to memory...");

			VkImageViewCreateInfo imageView{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
			imageView.pNext = nullptr;
			imageView.viewType = VK_IMAGE_VIEW_TYPE_2D;
			imageView.format = singleChannel ? VK_FORMAT_D16_UNORM : (isPosition ? VK_FORMAT_R32G32B32A32_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM);
			imageView.subresourceRange.aspectMask = singleChannel ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
			imageView.subresourceRange.baseMipLevel = 0;
			imageView.subresourceRange.levelCount = 1;
			imageView.subresourceRange.baseArrayLayer = 0;
			imageView.subresourceRange.layerCount = 1;
			imageView.image = tex.image;
			result = VK_SUCCESS;
			VkCall(result = vkCreateImageView(core::logical_device(), &imageView, nullptr, &tex.view), "Failed to create GBuffer(shadow mapping) image view...");

			// Create sampler to sample from to depth attachment
			// Used to sample in the fragment shader for shadowed rendering
			VkSamplerCreateInfo sampler{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
			sampler.pNext = nullptr;
			sampler.magFilter = VK_FILTER_LINEAR;
			sampler.minFilter = VK_FILTER_LINEAR;
			sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
			sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			sampler.addressModeV = sampler.addressModeU;
			sampler.addressModeW = sampler.addressModeU;
			sampler.mipLodBias = 0.f;
			sampler.maxAnisotropy = 1.f;
			sampler.minLod = 0.f;
			sampler.maxLod = 1.f;
			sampler.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
			result = VK_SUCCESS;
			VkCall(result = vkCreateSampler(core::logical_device(), &sampler, nullptr, &tex.sampler), "Failed to create GBuffer(shadow mapping) image sampler...");

			VkAttachmentDescription attachDesc{
				0,
				singleChannel ? VK_FORMAT_D16_UNORM : (isPosition ? VK_FORMAT_R32G32B32A32_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM),
				VK_SAMPLE_COUNT_1_BIT,
				VK_ATTACHMENT_LOAD_OP_CLEAR,
				VK_ATTACHMENT_STORE_OP_STORE,
				VK_ATTACHMENT_LOAD_OP_DONT_CARE,
				VK_ATTACHMENT_STORE_OP_DONT_CARE,
				VK_IMAGE_LAYOUT_UNDEFINED,
				singleChannel ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
				};

			attach.emplace_back(attachDesc);

			return textures::add(tex);
		}

		class vulkan_base_graphics_pass
		{
		public:
			enum data_io : u32
			{
				input = 0,
				output,

				count
			};

			enum data_type : u32
			{
				buffer,
				image,
				shader,
				 
				count
			};

			struct set_data
			{
				u32					set_num;
				u32					binding_num;
				id::id_type			data_id;
			};

			vulkan_base_graphics_pass& add_resource(data_io io_type, data_type resource_type, u32 set_num, u32 binding_num, id::id_type data_id)
			{
				switch (resource_type)
				{
				case vulkan_base_graphics_pass::buffer:
				{
					switch (io_type)
					{
					case vulkan_base_graphics_pass::input:
						_input_buffers.emplace_back(set_data{ set_num, binding_num, data_id });
						break;
					case vulkan_base_graphics_pass::output:
						_output_buffers.emplace_back(set_data{ set_num, binding_num, data_id });
						break;
					default:
						std::runtime_error("This must be a legitimate type, input or output");
						break;
					}
				}
					break;
				case vulkan_base_graphics_pass::image:
				{
					switch (io_type)
					{
					case vulkan_base_graphics_pass::input:
						_input_images.emplace_back(set_data{ set_num, binding_num, data_id });
						break;
					case vulkan_base_graphics_pass::output:
						_output_images.emplace_back(set_data{ set_num, binding_num, data_id });
						break;
					default:
						std::runtime_error("This must be a legitimate type, input or output");
						break;
					}
				}
					break;
				case vulkan_base_graphics_pass::shader:
					_shader_ids.emplace_back(data_id);
					break;
				default:
					std::runtime_error("This must be a legitimate type, image or buffer or shader");
					break;
				}

				_set_count = std::max(_set_count, set_num);
			}

			void initialize(u32 width, u32 height)
			{
				_set_count += 1;

				_width = width;
				_height = _height;

				assert(!_input_buffers.empty() || !_input_images.empty() || !_shader_ids.empty());

				get_graphics_queue();

				create_semaphore_and_fence();

				createPoolAndLayout();

				createRenderpassAndFramebuffer();

				create_pipeline();

				create_command_buffer();
			}

			void run(vulkan_surface* surface)
			{
				runRenderpass(surface);
			}

			void submit()
			{
				submit();
			}

		private:
			utl::vector<set_data>							_input_buffers;
			utl::vector<set_data>							_input_images;
			utl::vector<set_data>							_output_buffers;
			utl::vector<set_data>							_output_images;
			utl::vector<id::id_type>						_shader_ids;

			u32												_set_count{ 0 };
			u32												_width;
			u32												_height;
			id::id_type										_framebuffer_id;
			id::id_type										_renderpass_id;
			id::id_type										_descriptor_pool_id;
			id::id_type										_descriptor_set_layout_id;
			id::id_type										_light_descriptor_set_layout_id;
			id::id_type										_pipeline_layout_id;
			id::id_type										_pipeline_id;
			utl::vector<id::id_type>						_descriptor_set_ids;
			vulkan_cmd_buffer								_cmd_buffer;
			VkSemaphore										_signal_semaphore;
			VkSemaphore										_wait_semaphore;
			VkQueue											_graphics_queue;
			vulkan_fence									_fence;

		private:
			virtual void createRenderpassAndFramebuffer()
			{
				utl::vector<VkAttachmentDescription>	attachmentDescs;
				utl::vector<VkAttachmentReference>		colorReferences;
				utl::vector<VkAttachmentReference>		depthReferences;
				utl::vector<VkImageView>				attachments;
				u32 count{ 0 };
				for (auto img_id : _output_images)
				{
					if((textures::get_texture(img_id.data_id).getTexture().format == VK_FORMAT_D16_UNORM ||
						textures::get_texture(img_id.data_id).getTexture().format == VK_FORMAT_D16_UNORM_S8_UINT ||
						textures::get_texture(img_id.data_id).getTexture().format == VK_FORMAT_D24_UNORM_S8_UINT ||
						textures::get_texture(img_id.data_id).getTexture().format == VK_FORMAT_D32_SFLOAT ||
						textures::get_texture(img_id.data_id).getTexture().format == VK_FORMAT_D32_SFLOAT_S8_UINT))
					{
						attachmentDescs.emplace_back(VkAttachmentDescription{
							0,
							textures::get_texture(img_id.data_id).getTexture().format,
							VK_SAMPLE_COUNT_1_BIT,
							VK_ATTACHMENT_LOAD_OP_CLEAR,
							VK_ATTACHMENT_STORE_OP_STORE,
							VK_ATTACHMENT_LOAD_OP_DONT_CARE,
							VK_ATTACHMENT_STORE_OP_DONT_CARE,
							VK_IMAGE_LAYOUT_UNDEFINED,
							VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
							});

						depthReferences.push_back({ count, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL });
						count++;
					}
					else
					{
						attachmentDescs.emplace_back(VkAttachmentDescription{
							0,
							textures::get_texture(img_id.data_id).getTexture().format,
							VK_SAMPLE_COUNT_1_BIT,
							VK_ATTACHMENT_LOAD_OP_CLEAR,
							VK_ATTACHMENT_STORE_OP_STORE,
							VK_ATTACHMENT_LOAD_OP_DONT_CARE,
							VK_ATTACHMENT_STORE_OP_DONT_CARE,
							VK_IMAGE_LAYOUT_UNDEFINED,
							VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
							});

						colorReferences.push_back({ count, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL });
					}

					attachments.emplace_back(textures::get_texture(img_id.data_id).getTexture().view);
				}

				VkSubpassDescription subpass;
				subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
				subpass.flags = 0;
				subpass.pColorAttachments = colorReferences.data();
				subpass.colorAttachmentCount = static_cast<uint32_t>(colorReferences.size());
				subpass.pDepthStencilAttachment = depthReferences.data();
				subpass.inputAttachmentCount = 0;
				subpass.pInputAttachments = nullptr;
				subpass.pPreserveAttachments = nullptr;
				subpass.preserveAttachmentCount = 0;
				subpass.pResolveAttachments = nullptr;

				utl::vector<VkSubpassDependency> dependencies;
				dependencies.resize(2);

				dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
				dependencies[0].dstSubpass = 0;
				dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
				dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
				dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
				dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
				dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

				dependencies[1].srcSubpass = 0;
				dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
				dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
				dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
				dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
				dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
				dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

				VkRenderPassCreateInfo renderPassInfo = {};
				renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
				renderPassInfo.pAttachments = attachmentDescs.data();
				renderPassInfo.attachmentCount = static_cast<uint32_t>(attachmentDescs.size());
				renderPassInfo.subpassCount = 1;
				renderPassInfo.pSubpasses = &subpass;
				renderPassInfo.dependencyCount = 2;
				renderPassInfo.pDependencies = dependencies.data();

				_renderpass_id = data::create_data(data::engine_vulkan_data::vulkan_renderpass, static_cast<void*>(&renderPassInfo), 0);

				// Create frame buffer
				VkFramebufferCreateInfo framebuffer{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
				framebuffer.pNext = nullptr;
				framebuffer.renderPass = data::get_data<VkRenderPass>(_renderpass_id);
				framebuffer.attachmentCount = static_cast<u32>(attachments.size());
				framebuffer.pAttachments = attachments.data();
				framebuffer.width = _width;
				framebuffer.height = _height;
				framebuffer.layers = 1;
				_framebuffer_id = data::create_data(data::engine_vulkan_data::vulkan_framebuffer, static_cast<void*>(&framebuffer), 0);
			}

			virtual void createPoolAndLayout()
			{
				utl::vector<VkDescriptorPoolSize> poolSize;

				poolSize.emplace_back(descriptor::descriptorPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, static_cast<u32>(frame_buffer_count * 3 * _input_buffers.size())));
				poolSize.emplace_back(descriptor::descriptorPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, static_cast<u32>(frame_buffer_count * 3 * _input_images.size())));

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
					u32 count{ 0 };
					for (auto buffer : _input_buffers)
					{
						setLayoutBindings.emplace_back(descriptor::descriptorSetLayoutBinding(count, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER));
						count++;
					}
					for (auto image : _input_images)
					{
						setLayoutBindings.emplace_back(descriptor::descriptorSetLayoutBinding(count, VK_SHADER_STAGE_FRAGMENT_BIT, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, 1));
						count++;
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
						VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
						&bufferInfo);
					vkUpdateDescriptorSets(core::logical_device(), 1, &write, 0, nullptr);
				}
				
				for (auto img : _input_images)
				{
					VkDescriptorImageInfo imageInfo;
					auto data = textures::get_texture(img.data_id).getTexture();
					imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
					imageInfo.imageView = data.view;
					imageInfo.sampler = data.sampler;
					VkWriteDescriptorSet write = descriptor::setWriteDescriptorSet(
						VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
						data::get_data<VkDescriptorSet>(_descriptor_set_ids[img.set_num]),
						img.binding_num,
						VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
						&imageInfo);
					vkUpdateDescriptorSets(core::logical_device(), 1, &write, 0, nullptr);
				}
			}

			virtual void create_pipeline()
			{
				assert(_shader_ids.size() >= 2);

				VkPipelineInputAssemblyStateCreateInfo inputAssemblyState = descriptor::pipelineInputAssemblyStateCreate(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, VK_FALSE);
				VkPipelineRasterizationStateCreateInfo rasterizationState = descriptor::pipelineRasterizationStateCreate(VK_POLYGON_MODE_FILL, VK_CULL_MODE_FRONT_BIT);
				VkPipelineColorBlendAttachmentState blendAttachmentState = descriptor::pipelineColorBlendAttachmentState(0xf, VK_FALSE);
				VkPipelineColorBlendStateCreateInfo colorBlendState = descriptor::pipelineColorBlendStateCreate(1, blendAttachmentState);
				VkPipelineDepthStencilStateCreateInfo depthStencilState = descriptor::pipelineDepthStencilStateCreateInfo(VK_TRUE, VK_TRUE, VK_FALSE, VK_FALSE, VK_COMPARE_OP_LESS_OR_EQUAL);
				VkPipelineViewportStateCreateInfo viewportState = descriptor::pipelineViewportStateCreate(1, 1, 0);
				VkPipelineMultisampleStateCreateInfo multisampleState = descriptor::pipelineMultisampleStateCreate();
				std::vector<VkDynamicState> dynamicStateEnables = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
				VkPipelineDynamicStateCreateInfo dynamicState = descriptor::pipelineDynamicStateCreate(dynamicStateEnables);
				VkPipelineVertexInputStateCreateInfo emptyVertexInputState{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO , nullptr, 0, 0, nullptr, 0, nullptr };
				utl::vector<VkPipelineShaderStageCreateInfo> shaderStages;
				std::string base_dir{ SOLUTION_DIR };
				for (auto shader_id : _shader_ids)
				{
					shaderStages.emplace_back(shaders::get_shader(shader_id).getShaderStage());
				}

				VkGraphicsPipelineCreateInfo pipelineCreateInfo{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
				pipelineCreateInfo.layout = data::get_data<VkPipelineLayout>(_pipeline_layout_id);
				pipelineCreateInfo.pNext = nullptr;
				pipelineCreateInfo.renderPass = data::get_data<VkRenderPass>(_renderpass_id);
				pipelineCreateInfo.flags = 0;
				pipelineCreateInfo.pInputAssemblyState = &inputAssemblyState;
				pipelineCreateInfo.pRasterizationState = &rasterizationState;
				pipelineCreateInfo.pColorBlendState = &colorBlendState;
				pipelineCreateInfo.pMultisampleState = &multisampleState;
				pipelineCreateInfo.pViewportState = &viewportState;
				pipelineCreateInfo.pDepthStencilState = &depthStencilState;
				pipelineCreateInfo.pDynamicState = &dynamicState;
				pipelineCreateInfo.stageCount = static_cast<uint32_t>(shaderStages.size());
				pipelineCreateInfo.pStages = shaderStages.data();
				pipelineCreateInfo.pVertexInputState = &emptyVertexInputState;
				_pipeline_id = data::create_data(data::engine_vulkan_data::vulkan_pipeline, static_cast<void*>(&pipelineCreateInfo), 0);
			}

			virtual void create_command_buffer()
			{
				if (_cmd_buffer.cmd_buffer)
				{
					vkFreeCommandBuffers(core::logical_device(), core::get_current_command_pool(), 1, &_cmd_buffer.cmd_buffer);

					_cmd_buffer.cmd_buffer = nullptr;
					_cmd_buffer.cmd_state = vulkan_cmd_buffer::CMD_NOT_ALLOCATED;
				}
				_cmd_buffer = allocate_cmd_buffer(core::logical_device(), core::get_current_command_pool(), true);
			}

			virtual void runRenderpass(vulkan_surface* surface)
			{
				// Clear values for all attachments written in the fragment shader
				std::vector<VkClearValue> clearValues(5);
				clearValues[0].color = { { 0.0f, 0.0f, 0.0f, 1.0f } };
				clearValues[1].color = { { 0.0f, 0.0f, 0.0f, 1.0f } };
				clearValues[2].color = { { 0.0f, 0.0f, 0.0f, 1.0f } };
				clearValues[3].color = { { 0.0f, 0.0f, 0.0f, 1.0f } };
				clearValues[4].depthStencil = { 1.0f, 0 };

				VkRenderPassBeginInfo info{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
				info.pNext = nullptr;
				info.renderPass = data::get_data<VkRenderPass>(_renderpass_id);
				info.framebuffer = data::get_data<VkFramebuffer>(_framebuffer_id);
				info.renderArea.extent.width = _width;
				info.renderArea.extent.height = _height;
				info.clearValueCount = static_cast<u32>(clearValues.size());
				info.pClearValues = clearValues.data();

				VkViewport viewport;
				viewport.x = 0.0f;
				viewport.y = (f32)surface->height();
				viewport.width = (f32)surface->width();
				viewport.height = (f32)surface->height() * -1.f;
				viewport.minDepth = 0.f;
				viewport.maxDepth = 1.f;
				vkCmdSetViewport(_cmd_buffer.cmd_buffer, 0, 1, &viewport);

				VkRect2D scissor;
				scissor.extent.height = _height;
				scissor.extent.width = _width;
				scissor.offset.x = 0;
				scissor.offset.y = 0;
				vkCmdSetScissor(_cmd_buffer.cmd_buffer, 0, 1, &scissor);

				vkCmdBeginRenderPass(_cmd_buffer.cmd_buffer, &info, VK_SUBPASS_CONTENTS_INLINE);

				surface->getScene().flushBuffer(_cmd_buffer, data::get_data<VkPipelineLayout>(_pipeline_layout_id));

				vkCmdEndRenderPass(_cmd_buffer.cmd_buffer);
			}

			virtual void submit()
			{
				VkSubmitInfo submitInfo;
				submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
				submitInfo.pNext = nullptr;
				submitInfo.commandBufferCount = 1;
				submitInfo.pCommandBuffers = &_cmd_buffer.cmd_buffer;
				submitInfo.waitSemaphoreCount = 0;
				submitInfo.pWaitSemaphores = nullptr;
				submitInfo.signalSemaphoreCount = 1;
				submitInfo.pSignalSemaphores = &_signal_semaphore;
				VkPipelineStageFlags flags[4]{ VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT ,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT };
				submitInfo.pWaitDstStageMask = flags;
				VkResult result{ VK_SUCCESS };
				VkCall(result = vkQueueSubmit(_graphics_queue, 1, &submitInfo, nullptr), "Failed to submit compute queue...");
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
				vkGetDeviceQueue(core::logical_device(), core::graphics_family_queue_index(), 0, &_graphics_queue);
			}
		};
	} // anonymous namespace
	
	vulkan_geometry_pass::~vulkan_geometry_pass()
	{
		for (auto id : _image_ids)
		{
			textures::remove(id);
		}
		_image_ids.clear();
		data::remove_data(data::engine_vulkan_data::vulkan_pipeline_layout, _pipeline_layout_id);
		data::remove_data(data::engine_vulkan_data::vulkan_descriptor_set_layout, _descriptor_set_layout_id);
		data::remove_data(data::engine_vulkan_data::vulkan_descriptor_pool, _descriptor_pool_id);
		data::remove_data(data::engine_vulkan_data::vulkan_framebuffer, _framebuffer_id);
		data::remove_data(data::engine_vulkan_data::vulkan_renderpass, _renderpass_id);
	}

	void vulkan_geometry_pass::createUniformBuffer()
	{
		
	}

	void vulkan_geometry_pass::create_semaphore()
	{
		VkResult result{ VK_SUCCESS };
		VkSemaphoreCreateInfo sInfo;
		sInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		sInfo.pNext = nullptr;
		sInfo.flags = 0;
		VkCall(result = vkCreateSemaphore(core::logical_device(), &sInfo, nullptr, &_signal_semaphore), "Failed to create compute signal semaphore...");
		VkCall(result = vkCreateSemaphore(core::logical_device(), &sInfo, nullptr, &_wait_semaphore), "Failed to create compute wait semaphore...");
	}

	void vulkan_geometry_pass::setupRenderpassAndFramebuffer()
	{
		utl::vector<VkAttachmentDescription>	attachmentDescs;

		// Position
		_image_ids.emplace_back(createOffscreenTexture(_width, _height, true, false, attachmentDescs));

		// Normal
		_image_ids.emplace_back(createOffscreenTexture(_width, _height, false, false, attachmentDescs));

		// Albedo
		_image_ids.emplace_back(createOffscreenTexture(_width, _height, false, false, attachmentDescs));

		// Specular
		_image_ids.emplace_back(createOffscreenTexture(_width, _height, false, false, attachmentDescs));

		// Depth
		_image_ids.emplace_back(createOffscreenTexture(_width, _height, false, true, attachmentDescs));


		utl::vector<VkAttachmentReference> colorReferences;
		colorReferences.push_back({ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL });
		colorReferences.push_back({ 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL });
		colorReferences.push_back({ 2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL });
		colorReferences.push_back({ 3, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL });

		VkAttachmentReference depthReference = {};
		depthReference.attachment = 4;
		depthReference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

		VkSubpassDescription subpass;
		subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.flags = 0;
		subpass.pColorAttachments = colorReferences.data();
		subpass.colorAttachmentCount = static_cast<uint32_t>(colorReferences.size());
		subpass.pDepthStencilAttachment = &depthReference;
		subpass.inputAttachmentCount = 0;
		subpass.pInputAttachments = nullptr;
		subpass.pPreserveAttachments = nullptr;
		subpass.preserveAttachmentCount = 0;
		subpass.pResolveAttachments = nullptr;

		utl::vector<VkSubpassDependency> dependencies;
		dependencies.resize(2);

		dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
		dependencies[0].dstSubpass = 0;
		dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
		dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

		dependencies[1].srcSubpass = 0;
		dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
		dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

		VkRenderPassCreateInfo renderPassInfo = {};
		renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
		renderPassInfo.pAttachments = attachmentDescs.data();
		renderPassInfo.attachmentCount = static_cast<uint32_t>(attachmentDescs.size());
		renderPassInfo.subpassCount = 1;
		renderPassInfo.pSubpasses = &subpass;
		renderPassInfo.dependencyCount = 2;
		renderPassInfo.pDependencies = dependencies.data();

		_renderpass_id = data::create_data(data::engine_vulkan_data::vulkan_renderpass, static_cast<void*>(&renderPassInfo), 0);

		utl::vector<VkImageView> attachments;
		attachments.resize(5);
		attachments[0] = textures::get_texture(_image_ids[0]).getTexture().view;
		attachments[1] = textures::get_texture(_image_ids[1]).getTexture().view;
		attachments[2] = textures::get_texture(_image_ids[2]).getTexture().view;
		attachments[3] = textures::get_texture(_image_ids[3]).getTexture().view;
		attachments[4] = textures::get_texture(_image_ids[4]).getTexture().view;

		// Create frame buffer
		VkFramebufferCreateInfo framebuffer{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
		framebuffer.pNext = nullptr;
		framebuffer.renderPass = data::get_data<VkRenderPass>(_renderpass_id);
		framebuffer.attachmentCount = static_cast<u32>(attachments.size());
		framebuffer.pAttachments = attachments.data();
		framebuffer.width = _width;
		framebuffer.height = _height;
		framebuffer.layers = 1;
		_framebuffer_id = data::create_data(data::engine_vulkan_data::vulkan_framebuffer, static_cast<void*>(&framebuffer), 0);
	}

	void vulkan_geometry_pass::setupPoolAndLayout()
	{
		std::vector<VkDescriptorPoolSize> poolSize = {
			descriptor::descriptorPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, static_cast<u32>(frame_buffer_count * 3) + (u32)100),
			descriptor::descriptorPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, static_cast<u32>(frame_buffer_count) + (u32)100),
		};

		VkDescriptorPoolCreateInfo poolInfo;
		poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		poolInfo.pNext = VK_NULL_HANDLE;
		poolInfo.flags = 0;
		poolInfo.poolSizeCount = static_cast<u32>(poolSize.size());
		poolInfo.pPoolSizes = poolSize.data();
		poolInfo.maxSets = static_cast<u32>(frame_buffer_count) + (u32)100;

		_descriptor_pool_id = data::create_data(data::engine_vulkan_data::vulkan_descriptor_pool, static_cast<void*>(&poolInfo), 0);

		{
			// Constant Descriptor Set Layout
			std::vector<VkDescriptorSetLayoutBinding> setLayoutBindings{
				descriptor::descriptorSetLayoutBinding(0, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
				descriptor::descriptorSetLayoutBinding(1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
				descriptor::descriptorSetLayoutBinding(2, VK_SHADER_STAGE_FRAGMENT_BIT, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, 3)
			};
			VkDescriptorSetLayoutCreateInfo descriptorLayout = descriptor::descriptorSetLayoutCreate(setLayoutBindings);
			_descriptor_set_layout_id = data::create_data(data::engine_vulkan_data::vulkan_descriptor_set_layout, static_cast<void*>(&descriptorLayout), 0);
		}

		//{
		//	// Mutable Descriptor Set Layout
		//	VkDescriptorSetLayoutBinding lightSetLayoutBindings = descriptor::descriptorSetLayoutBinding(0, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
		//	VkDescriptorSetLayoutCreateInfo lightDescriptorLayout = descriptor::descriptorSetLayoutCreate(&lightSetLayoutBindings, 1);
		//	lightDescriptorLayout.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
		//	_light_descriptor_set_layout_id = data::create_data(data::engine_vulkan_data::vulkan_descriptor_set_layout, static_cast<void*>(&lightDescriptorLayout), 0);
		//}

		//std::vector<VkDescriptorSetLayout> descriptorSetArray{ data::get_data<VkDescriptorSetLayout>(_descriptor_set_layout_id), data::get_data<VkDescriptorSetLayout>(_light_descriptor_set_layout_id) };

		//VkPushConstantRange push{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(u32) };
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

	void vulkan_geometry_pass::runRenderpass(vulkan_cmd_buffer cmd_buffer, vulkan_surface * surface)
	{
		// Clear values for all attachments written in the fragment shader
		std::vector<VkClearValue> clearValues(5);
		clearValues[0].color = { { 0.0f, 0.0f, 0.0f, 1.0f } };
		clearValues[1].color = { { 0.0f, 0.0f, 0.0f, 1.0f } };
		clearValues[2].color = { { 0.0f, 0.0f, 0.0f, 1.0f } };
		clearValues[3].color = { { 0.0f, 0.0f, 0.0f, 1.0f } };
		clearValues[4].depthStencil = { 1.0f, 0 };

		VkRenderPassBeginInfo info{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
		info.pNext = nullptr;
		info.renderPass = data::get_data<VkRenderPass>(_renderpass_id);
		info.framebuffer = data::get_data<VkFramebuffer>(_framebuffer_id);
		info.renderArea.extent.width = _width;
		info.renderArea.extent.height = _height;
		info.clearValueCount = static_cast<u32>(clearValues.size());
		info.pClearValues = clearValues.data();

		VkViewport viewport;
		viewport.x = 0.0f;
		viewport.y = (f32)surface->height();
		viewport.width = (f32)surface->width();
		viewport.height = (f32)surface->height() * -1.f;
		viewport.minDepth = 0.f;
		viewport.maxDepth = 1.f;
		vkCmdSetViewport(cmd_buffer.cmd_buffer, 0, 1, &viewport);

		VkRect2D scissor;
		scissor.extent.height = _height;
		scissor.extent.width = _width;
		scissor.offset.x = 0;
		scissor.offset.y = 0;
		vkCmdSetScissor(cmd_buffer.cmd_buffer, 0, 1, &scissor);

		vkCmdBeginRenderPass(cmd_buffer.cmd_buffer, &info, VK_SUBPASS_CONTENTS_INLINE);

		surface->getScene().flushBuffer(cmd_buffer, data::get_data<VkPipelineLayout>(_pipeline_layout_id));

		vkCmdEndRenderPass(cmd_buffer.cmd_buffer);
	}

	void vulkan_geometry_pass::submit(vulkan_cmd_buffer cmd_buffer, VkQueue graphics_queue)
	{
		VkSubmitInfo submitInfo;
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submitInfo.pNext = nullptr;
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &cmd_buffer.cmd_buffer;
		submitInfo.waitSemaphoreCount = 0;
		submitInfo.pWaitSemaphores = nullptr;
		submitInfo.signalSemaphoreCount = 1;
		submitInfo.pSignalSemaphores = &_signal_semaphore;
		VkPipelineStageFlags flags[4]{ VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT ,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT };
		submitInfo.pWaitDstStageMask = flags;
		VkResult result{ VK_SUCCESS };
		VkCall(result = vkQueueSubmit(graphics_queue, 1, &submitInfo, nullptr), "Failed to submit compute queue...");
	}

	vulkan_final_pass::~vulkan_final_pass()
	{
		data::remove_data(data::engine_vulkan_data::vulkan_pipeline, _pipeline_id);
		data::remove_data(data::engine_vulkan_data::vulkan_pipeline_layout, _pipeline_layout_id);
		data::remove_data(data::engine_vulkan_data::vulkan_descriptor_sets, _descriptorSet_id);
		data::remove_data(data::engine_vulkan_data::vulkan_descriptor_set_layout, _descriptorSet_layout_id);
		data::remove_data(data::engine_vulkan_data::vulkan_descriptor_pool, _descriptor_pool_id);
	}

	void vulkan_final_pass::setupDescriptorSets(utl::vector<id::id_type> image_id, id::id_type ubo_id)
	{
		std::vector<VkDescriptorPoolSize> poolSize = {
			descriptor::descriptorPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, static_cast<u32>(frame_buffer_count * 3)),
			descriptor::descriptorPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, static_cast<u32>(image_id.size()) + 10),
			descriptor::descriptorPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, static_cast<u32>(image_id.size()) + 10)
			//descriptor::descriptorPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, static_cast<u32>(image_id.size()))
		};

		VkDescriptorPoolCreateInfo poolInfo;
		poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		poolInfo.pNext = VK_NULL_HANDLE;
		poolInfo.flags = 0;
		poolInfo.poolSizeCount = static_cast<u32>(poolSize.size());
		poolInfo.pPoolSizes = poolSize.data();
		poolInfo.maxSets = static_cast<u32>(image_id.size()) + 10;
		_descriptor_pool_id = data::create_data(data::engine_vulkan_data::vulkan_descriptor_pool, static_cast<void*>(&poolInfo), 0);

		std::vector<VkDescriptorSetLayoutBinding> setLayoutBindings{
			descriptor::descriptorSetLayoutBinding(0, VK_SHADER_STAGE_FRAGMENT_BIT, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
			descriptor::descriptorSetLayoutBinding(1, VK_SHADER_STAGE_FRAGMENT_BIT, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, 4),
			//descriptor::descriptorSetLayoutBinding(1, VK_SHADER_STAGE_FRAGMENT_BIT, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
			descriptor::descriptorSetLayoutBinding(2, VK_SHADER_STAGE_FRAGMENT_BIT, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
		};
		
		/*for (u32 i{ 0 }; i < image_id.size() + 1; ++i)
		{
			setLayoutBindings.emplace_back(descriptor::descriptorSetLayoutBinding(i, VK_SHADER_STAGE_FRAGMENT_BIT, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER));
		}*/

		VkDescriptorSetLayoutCreateInfo descriptorLayout = descriptor::descriptorSetLayoutCreate(setLayoutBindings);

		_descriptorSet_layout_id = data::create_data(data::engine_vulkan_data::vulkan_descriptor_set_layout, static_cast<void*>(&descriptorLayout), 0);

		// Mutable Descriptor Set Layout
		VkDescriptorSetLayoutBinding lightSetLayoutBindings = descriptor::descriptorSetLayoutBinding(0, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
		VkDescriptorSetLayoutCreateInfo lightDescriptorLayout = descriptor::descriptorSetLayoutCreate(&lightSetLayoutBindings, 1);
		lightDescriptorLayout.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
		_light_descriptor_set_layout_id = data::create_data(data::engine_vulkan_data::vulkan_descriptor_set_layout, static_cast<void*>(&lightDescriptorLayout), 0);

		std::vector<VkDescriptorSetLayout> descriptorSetArray{ data::get_data<VkDescriptorSetLayout>(_descriptorSet_layout_id), data::get_data<VkDescriptorSetLayout>(_light_descriptor_set_layout_id) };

		VkPushConstantRange push{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(u32) };
		VkPipelineLayoutCreateInfo pipelineLayoutInfo;
		pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		pipelineLayoutInfo.pNext = nullptr;
		pipelineLayoutInfo.flags = 0;
		pipelineLayoutInfo.setLayoutCount = descriptorSetArray.size();
		pipelineLayoutInfo.pSetLayouts = descriptorSetArray.data();
		pipelineLayoutInfo.pushConstantRangeCount = 1;
		pipelineLayoutInfo.pPushConstantRanges = &push;
		_pipeline_layout_id = data::create_data(data::engine_vulkan_data::vulkan_pipeline_layout, static_cast<void*>(&pipelineLayoutInfo), 0);

		VkDescriptorSetAllocateInfo allocInfo;

		allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		allocInfo.pNext = VK_NULL_HANDLE;
		allocInfo.descriptorPool = data::get_data<VkDescriptorPool>(_descriptor_pool_id);
		allocInfo.descriptorSetCount = 1;
		allocInfo.pSetLayouts = &data::get_data<VkDescriptorSetLayout>(_descriptorSet_layout_id);

		_descriptorSet_id = data::create_data(data::engine_vulkan_data::vulkan_descriptor_sets, static_cast<void*>(&allocInfo), 0);

		std::vector<VkWriteDescriptorSet> descriptorWrites;

		VkDescriptorBufferInfo bufferInfo;
		bufferInfo.buffer = data::get_data<data::vulkan_buffer>(ubo_id).cpu_address;
		bufferInfo.offset = 0;
		bufferInfo.range = sizeof(glsl::GlobalShaderData);
		descriptorWrites.emplace_back(descriptor::setWriteDescriptorSet(
			VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			data::get_data<VkDescriptorSet>(_descriptorSet_id),
			0,
			VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			&bufferInfo));

		utl::vector<VkDescriptorImageInfo>	imageInfos4;
		for (u32 j{0}; j < image_id.size(); ++j)
		{
			if (j >= 4) break;
			VkDescriptorImageInfo imageInfo4;
			imageInfo4.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			imageInfo4.imageView = textures::get_texture(image_id[j]).getTexture().view;
			imageInfo4.sampler = textures::get_texture(image_id[j]).getTexture().sampler;
			imageInfos4.emplace_back(imageInfo4);
			//descriptorWrites.emplace_back(descriptor::setWriteDescriptorSet(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, data::get_data<VkDescriptorSet>(_descriptorSet_id), count, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &imageInfo4));
			//count++;
		}
		descriptorWrites.emplace_back(descriptor::setWriteDescriptorSet(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, data::get_data<VkDescriptorSet>(_descriptorSet_id), 1, 4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, imageInfos4.data()));

		/*VkDescriptorImageInfo imageInfo5;
		imageInfo5.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		imageInfo5.imageView = textures::get_texture(compute::get_output_tex_id()).getTexture().view;
		imageInfo5.sampler = textures::get_texture(compute::get_output_tex_id()).getTexture().sampler;
		descriptorWrites.emplace_back(descriptor::setWriteDescriptorSet(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, data::get_data<VkDescriptorSet>(_descriptorSet_id), 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &imageInfo5));*/
		VkDescriptorBufferInfo outInfo;
		auto buffer = data::get_data<data::vulkan_buffer>(compute::get_output_buffer_id());
		outInfo.buffer = buffer.cpu_address;
		outInfo.offset = 0;
		outInfo.range = buffer.size;
		descriptorWrites.emplace_back(descriptor::setWriteDescriptorSet(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, data::get_data<VkDescriptorSet>(_descriptorSet_id), 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &outInfo));

		vkUpdateDescriptorSets(core::logical_device(), static_cast<u32>(descriptorWrites.size()), descriptorWrites.data(), 0, nullptr);
	}

	void vulkan_final_pass::setupPipeline(vulkan_renderpass renderpass)
	{
		VkPipelineInputAssemblyStateCreateInfo inputAssemblyState = descriptor::pipelineInputAssemblyStateCreate(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, VK_FALSE);
		VkPipelineRasterizationStateCreateInfo rasterizationState = descriptor::pipelineRasterizationStateCreate(VK_POLYGON_MODE_FILL, VK_CULL_MODE_FRONT_BIT);
		VkPipelineColorBlendAttachmentState blendAttachmentState = descriptor::pipelineColorBlendAttachmentState(0xf, VK_FALSE);
		VkPipelineColorBlendStateCreateInfo colorBlendState = descriptor::pipelineColorBlendStateCreate(1, blendAttachmentState);
		VkPipelineDepthStencilStateCreateInfo depthStencilState = descriptor::pipelineDepthStencilStateCreateInfo(VK_TRUE, VK_TRUE, VK_FALSE, VK_FALSE, VK_COMPARE_OP_LESS_OR_EQUAL);
		VkPipelineViewportStateCreateInfo viewportState = descriptor::pipelineViewportStateCreate(1, 1, 0);
		VkPipelineMultisampleStateCreateInfo multisampleState = descriptor::pipelineMultisampleStateCreate();
		std::vector<VkDynamicState> dynamicStateEnables = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
		VkPipelineDynamicStateCreateInfo dynamicState = descriptor::pipelineDynamicStateCreate(dynamicStateEnables);
		VkPipelineVertexInputStateCreateInfo emptyVertexInputState{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO , nullptr, 0, 0, nullptr, 0, nullptr };
		std::array<VkPipelineShaderStageCreateInfo, 2> shaderStages;
		std::string base_dir{ SOLUTION_DIR };
		shaderStages[0] = shaders::get_shader(shaders::add(base_dir + "Engine\\Graphics\\Vulkan\\Shaders\\spv\\fullscreen.vert.spv", shader_type::vertex)).getShaderStage();
		shaderStages[1] = shaders::get_shader(shaders::add(base_dir + "Engine\\Graphics\\Vulkan\\Shaders\\spv\\composition.frag.spv", shader_type::pixel)).getShaderStage();

		VkGraphicsPipelineCreateInfo pipelineCreateInfo{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
		pipelineCreateInfo.layout = data::get_data<VkPipelineLayout>(_pipeline_layout_id);
		pipelineCreateInfo.pNext = nullptr;
		pipelineCreateInfo.renderPass = renderpass.render_pass;
		pipelineCreateInfo.flags = 0;
		pipelineCreateInfo.pInputAssemblyState = &inputAssemblyState;
		pipelineCreateInfo.pRasterizationState = &rasterizationState;
		pipelineCreateInfo.pColorBlendState = &colorBlendState;
		pipelineCreateInfo.pMultisampleState = &multisampleState;
		pipelineCreateInfo.pViewportState = &viewportState;
		pipelineCreateInfo.pDepthStencilState = &depthStencilState;
		pipelineCreateInfo.pDynamicState = &dynamicState;
		pipelineCreateInfo.stageCount = static_cast<uint32_t>(shaderStages.size());
		pipelineCreateInfo.pStages = shaderStages.data();
		pipelineCreateInfo.pVertexInputState = &emptyVertexInputState;
		_pipeline_id = data::create_data(data::engine_vulkan_data::vulkan_pipeline, static_cast<void*>(&pipelineCreateInfo), 0);
	}

	void vulkan_final_pass::render(vulkan_cmd_buffer cmd_buffer)
	{
		auto descriptorSet = data::get_data<VkDescriptorSet>(_descriptorSet_id);
		auto pipeline = data::get_data<VkPipeline>(_pipeline_id);
		auto layout = data::get_data<VkPipelineLayout>(_pipeline_layout_id);
		vkCmdBindDescriptorSets(cmd_buffer.cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, data::get_data<VkPipelineLayout>(_pipeline_layout_id), 0, 1, &descriptorSet, 0, nullptr);
		VkWriteDescriptorSet write;
		VkDescriptorBufferInfo lightBuffer;
		lightBuffer.buffer = data::get_data<data::vulkan_buffer>(light::non_cullable_light_buffer_id()).cpu_address;
		lightBuffer.offset = 0;
		lightBuffer.range = data::get_data<data::vulkan_buffer>(light::non_cullable_light_buffer_id()).size;
		write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		write.pNext = nullptr;
		write.dstSet = 0;
		write.dstBinding = 0;
		write.descriptorCount = 1;
		write.dstArrayElement = 0;
		write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		write.pBufferInfo = &lightBuffer;
		vkCmdPushDescriptorSetKHR(cmd_buffer.cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1, &write);
		u32 light_num = light::non_cullable_light_count(0);
		vkCmdPushConstants(cmd_buffer.cmd_buffer, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(u32), static_cast<void*>(&light_num));

		vkCmdBindPipeline(cmd_buffer.cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
		vkCmdDraw(cmd_buffer.cmd_buffer, 3, 1, 0, 0);
	}

}