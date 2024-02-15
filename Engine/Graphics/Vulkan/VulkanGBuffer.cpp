#include "VulkanGBuffer.h"
#include "VulkanHelpers.h"
#include "VulkanCore.h"
#include "VulkanSurface.h"
#include "VulkanCamera.h"
#include "VulkanContent.h"
#include "VulkanResources.h"
#include "VulkanData.h"

#include "EngineAPI/GameEntity.h"
#include "Components/Transform.h"
#include "VulkanCompute.h"

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
		data::remove_data(data::engine_vulkan_data::vulkan_buffer, _ub_id);
	}

	void vulkan_geometry_pass::createUniformBuffer()
	{
		
	}

	void vulkan_geometry_pass::setupRenderpassAndFramebuffer()
	{
		utl::vector<VkAttachmentDescription>	attachmentDescs;

		// Position
		_image_ids.emplace_back(createOffscreenTexture(_width, _height, true, false, attachmentDescs));

		// Normal
		_image_ids.emplace_back(createOffscreenTexture(_width, _height, false, false, attachmentDescs));

		// Color
		_image_ids.emplace_back(createOffscreenTexture(_width, _height, false, false, attachmentDescs));

		// Depth
		_image_ids.emplace_back(createOffscreenTexture(_width, _height, false, true, attachmentDescs));


		utl::vector<VkAttachmentReference> colorReferences;
		colorReferences.push_back({ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL });
		colorReferences.push_back({ 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL });
		colorReferences.push_back({ 2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL });

		VkAttachmentReference depthReference = {};
		depthReference.attachment = 3;
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
		attachments.resize(4);
		attachments[0] = textures::get_texture(_image_ids[0]).getTexture().view;
		attachments[1] = textures::get_texture(_image_ids[1]).getTexture().view;
		attachments[2] = textures::get_texture(_image_ids[2]).getTexture().view;
		attachments[3] = textures::get_texture(_image_ids[3]).getTexture().view;

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

		{
			// Mutable Descriptor Set Layout
			VkDescriptorSetLayoutBinding lightSetLayoutBindings = descriptor::descriptorSetLayoutBinding(0, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
			VkDescriptorSetLayoutCreateInfo lightDescriptorLayout = descriptor::descriptorSetLayoutCreate(&lightSetLayoutBindings, 1);
			lightDescriptorLayout.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
			_light_descriptor_set_layout_id = data::create_data(data::engine_vulkan_data::vulkan_descriptor_set_layout, static_cast<void*>(&lightDescriptorLayout), 0);
		}

		std::vector<VkDescriptorSetLayout> descriptorSetArray{ data::get_data<VkDescriptorSetLayout>(_descriptor_set_layout_id), data::get_data<VkDescriptorSetLayout>(_light_descriptor_set_layout_id) };

		VkPushConstantRange push{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(u32) };
		VkPipelineLayoutCreateInfo pipelineLayoutInfo;
		pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		pipelineLayoutInfo.pNext = nullptr;
		pipelineLayoutInfo.flags = 0;
		pipelineLayoutInfo.setLayoutCount = (u32)descriptorSetArray.size();
		pipelineLayoutInfo.pSetLayouts = descriptorSetArray.data();
		pipelineLayoutInfo.pushConstantRangeCount = 1;
		pipelineLayoutInfo.pPushConstantRanges = &push;
		_pipeline_layout_id = data::create_data(data::engine_vulkan_data::vulkan_pipeline_layout, static_cast<void*>(&pipelineLayoutInfo), 0);
	}

	void vulkan_geometry_pass::runRenderpass(vulkan_cmd_buffer cmd_buffer, vulkan_surface * surface)
	{
		// Clear values for all attachments written in the fragment shader
		std::vector<VkClearValue> clearValues(4);
		clearValues[0].color = { { 0.0f, 0.0f, 0.0f, 1.0f } };
		clearValues[1].color = { { 0.0f, 0.0f, 0.0f, 1.0f } };
		clearValues[2].color = { { 0.0f, 0.0f, 0.0f, 1.0f } };
		clearValues[3].depthStencil = { 1.0f, 0 };

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
			descriptor::descriptorSetLayoutBinding(1, VK_SHADER_STAGE_FRAGMENT_BIT, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, 3),
			//descriptor::descriptorSetLayoutBinding(1, VK_SHADER_STAGE_FRAGMENT_BIT, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
			descriptor::descriptorSetLayoutBinding(2, VK_SHADER_STAGE_FRAGMENT_BIT, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
		};
		
		/*for (u32 i{ 0 }; i < image_id.size() + 1; ++i)
		{
			setLayoutBindings.emplace_back(descriptor::descriptorSetLayoutBinding(i, VK_SHADER_STAGE_FRAGMENT_BIT, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER));
		}*/

		VkDescriptorSetLayoutCreateInfo descriptorLayout = descriptor::descriptorSetLayoutCreate(setLayoutBindings);

		_descriptorSet_layout_id = data::create_data(data::engine_vulkan_data::vulkan_descriptor_set_layout, static_cast<void*>(&descriptorLayout), 0);

		VkPipelineLayoutCreateInfo pipelineLayoutInfo;
		pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		pipelineLayoutInfo.pNext = nullptr;
		pipelineLayoutInfo.flags = 0;
		pipelineLayoutInfo.setLayoutCount = 1;
		pipelineLayoutInfo.pSetLayouts = &data::get_data<VkDescriptorSetLayout>(_descriptorSet_layout_id);
		pipelineLayoutInfo.pushConstantRangeCount = 0;
		pipelineLayoutInfo.pPushConstantRanges = VK_NULL_HANDLE;
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
		bufferInfo.range = sizeof(UniformBufferObjectPlus);
		descriptorWrites.emplace_back(descriptor::setWriteDescriptorSet(
			VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			data::get_data<VkDescriptorSet>(_descriptorSet_id),
			0,
			VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			&bufferInfo));

		utl::vector<VkDescriptorImageInfo>	imageInfos4;
		for (u32 j{0}; j < image_id.size(); ++j)
		{
			if (j >= 3) break;
			VkDescriptorImageInfo imageInfo4;
			imageInfo4.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			imageInfo4.imageView = textures::get_texture(image_id[j]).getTexture().view;
			imageInfo4.sampler = textures::get_texture(image_id[j]).getTexture().sampler;
			imageInfos4.emplace_back(imageInfo4);
			//descriptorWrites.emplace_back(descriptor::setWriteDescriptorSet(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, data::get_data<VkDescriptorSet>(_descriptorSet_id), count, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &imageInfo4));
			//count++;
		}
		descriptorWrites.emplace_back(descriptor::setWriteDescriptorSet(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, data::get_data<VkDescriptorSet>(_descriptorSet_id), 1, 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, imageInfos4.data()));

		/*VkDescriptorImageInfo imageInfo5;
		imageInfo5.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		imageInfo5.imageView = textures::get_texture(compute::get_output_tex_id()).getTexture().view;
		imageInfo5.sampler = textures::get_texture(compute::get_output_tex_id()).getTexture().sampler;
		descriptorWrites.emplace_back(descriptor::setWriteDescriptorSet(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, data::get_data<VkDescriptorSet>(_descriptorSet_id), 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &imageInfo5));*/
		VkDescriptorBufferInfo outInfo;
		outInfo.buffer = compute::get_output_buffer();
		outInfo.offset = 0;
		outInfo.range = sizeof(math::v4) *22500;
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
		vkCmdBindDescriptorSets(cmd_buffer.cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, data::get_data<VkPipelineLayout>(_pipeline_layout_id), 0, 1, &descriptorSet, 0, nullptr);
		vkCmdBindPipeline(cmd_buffer.cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
		vkCmdDraw(cmd_buffer.cmd_buffer, 3, 1, 0, 0);
	}

}