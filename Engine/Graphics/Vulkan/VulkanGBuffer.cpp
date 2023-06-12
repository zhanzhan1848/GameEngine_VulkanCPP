#include "VulkanGBuffer.h"
#include "VulkanHelpers.h"
#include "VulkanCore.h"
#include "VulkanSurface.h"

#include <array>

namespace primal::graphics::vulkan
{
	namespace
	{
	
	} // anonymous namespace


	vulkan_shadowmapping::~vulkan_shadowmapping()
	{
		vkDestroySampler(core::logical_device(), _image.sampler, nullptr);

		vkDestroyImageView(core::logical_device(), _image.view, nullptr);
		vkDestroyImage(core::logical_device(), _image.image, nullptr);
		vkFreeMemory(core::logical_device(), _image.memory, nullptr);

		vkDestroyFramebuffer(core::logical_device(), _framebuffer.framebuffer, nullptr);

		vkDestroyRenderPass(core::logical_device(),_renderpass.render_pass, nullptr);

		vkDestroyPipeline(core::logical_device(), _pipeline, nullptr);

		vkDestroyBuffer(core::logical_device(), _uniformbuffer.buffer, nullptr);
		vkFreeMemory(core::logical_device(), _uniformbuffer.memory, nullptr);
	}

	void vulkan_shadowmapping::createUniformBuffer()
	{
		VkDeviceSize bufferSize = sizeof(UniformBufferObject);

		createBuffer(core::logical_device(), bufferSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, _uniformbuffer.buffer, _uniformbuffer.memory);

		//vkBindBufferMemory(core::logical_device(), _uniformBuffer.buffer, _uniformBuffer.memory, 0);

		vkMapMemory(core::logical_device(), _uniformbuffer.memory, 0, bufferSize, 0, &_uniformbuffer.mapped);

		//add_camera({ 128, graphics::camera::type::perspective, math::v3{0, 1, 0}, 1280, 720, 0.01, 10000 });
		DirectX::XMMATRIX modelMatrix = DirectX::XMMatrixIdentity();
		//modelMatrix = DirectX::XMMatrixMultiply(DirectX::XMMatrixRotationY(DirectX::XM_PIDIV2), modelMatrix);
		DirectX::XMStoreFloat4x4(&_ubo.model, modelMatrix);
		DirectX::XMMATRIX viewMatrix = DirectX::XMMatrixLookAtLH({ 0.f, 5.f, 0.f }, { 0.f, 0.f, 0.f }, { 0.f, 0.f, 1.f });
		DirectX::XMStoreFloat4x4(&_ubo.view, viewMatrix);
		DirectX::XMMATRIX projectionMatrix = DirectX::XMMatrixPerspectiveFovLH(DirectX::XM_PIDIV4, (f32)_shadowmapping_dim / (f32)_shadowmapping_dim, 0.01, 1000.0);
		DirectX::XMStoreFloat4x4(&_ubo.projection, projectionMatrix);
		memcpy(_uniformbuffer.mapped, &_ubo, sizeof(UniformBufferObject));
	}

	void vulkan_shadowmapping::updateUniformBuffer()
	{
	}

	void vulkan_shadowmapping::setupRenderpass()
	{
		VkAttachmentDescription attachmentDescription;
		attachmentDescription.format = VK_FORMAT_D16_UNORM;
		attachmentDescription.samples = VK_SAMPLE_COUNT_1_BIT;
		attachmentDescription.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		attachmentDescription.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		attachmentDescription.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachmentDescription.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachmentDescription.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		attachmentDescription.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL; // ! Attachment will be transitioned to shader read at render pass end
		attachmentDescription.flags = 0;

		VkAttachmentReference depthReference;
		depthReference.attachment = 0;
		depthReference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

		VkSubpassDescription subpass;
		subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.colorAttachmentCount = 0;
		subpass.pColorAttachments = nullptr;
		subpass.pDepthStencilAttachment = &depthReference;
		subpass.inputAttachmentCount = 0;
		subpass.pInputAttachments = nullptr;
		subpass.preserveAttachmentCount = 0;
		subpass.pPreserveAttachments = nullptr;
		subpass.pResolveAttachments = nullptr;
		subpass.flags = 0;

		// Use subpass dependencies for layout transitions
		std::array<VkSubpassDependency, 2> dependencies;
		dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
		dependencies[0].dstSubpass = 0;
		dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		dependencies[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
		dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
		dependencies[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

		dependencies[1].srcSubpass = 0;
		dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
		dependencies[1].srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		dependencies[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

		VkRenderPassCreateInfo info{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
		info.pNext = nullptr;
		info.flags = 0;
		info.attachmentCount = 1;
		info.pAttachments = &attachmentDescription;
		info.subpassCount = 1;
		info.pSubpasses = &subpass;
		info.dependencyCount = static_cast<u32>(dependencies.size());
		info.pDependencies = dependencies.data();

		_renderpass.clear_color = { 0.0f, 0.0f, 0.0f, 0.0f };
		_renderpass.render_area = { _shadowmapping_dim, 0, 0, _shadowmapping_dim };
		_renderpass.depth = 1.f;
		_renderpass.stencil = 0;
		VkResult result{ VK_SUCCESS };
		VkCall(result = vkCreateRenderPass(core::logical_device(), &info, nullptr, &_renderpass.render_pass), "Failed to create GBuffer(shadow mapping) renderpass...");
	}

	void vulkan_shadowmapping::setupFramebuffer()
	{
		_image.height = _image.width = _shadowmapping_dim;

		VkImageCreateInfo image{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
		image.imageType = VK_IMAGE_TYPE_2D;
		image.extent.height = _shadowmapping_dim;
		image.extent.width = _shadowmapping_dim;
		image.extent.depth = 1;
		image.mipLevels = 1;
		image.arrayLayers = 1;
		image.samples = VK_SAMPLE_COUNT_1_BIT;
		image.tiling = VK_IMAGE_TILING_OPTIMAL;
		image.format = VK_FORMAT_D16_UNORM;
		image.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT; // ! VK_IMAGE_USAGE_SAMPLED_BIT use to sample directly from the depth attachment for shadow mapping
		VkResult result{ VK_SUCCESS };
		VkCall(result = vkCreateImage(core::logical_device(), &image, nullptr, &_image.image), "Failed to create GBuffer(shadow mapping) image...");

		VkMemoryRequirements memReq;
		vkGetImageMemoryRequirements(core::logical_device(), _image.image, &memReq);
		VkMemoryAllocateInfo alloc{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
		alloc.pNext = nullptr;
		alloc.allocationSize = memReq.size;
		alloc.memoryTypeIndex = core::find_memory_index(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		result = VK_SUCCESS;
		VkCall(result = vkAllocateMemory(core::logical_device(), &alloc, nullptr, &_image.memory), "Failed to allocate GBuffer(shadow mapping) memory...");
		result = VK_SUCCESS;
		VkCall(result = vkBindImageMemory(core::logical_device(), _image.image, _image.memory, 0), "Failed to bind GBuffer(shadow mapping) to memory...");

		VkImageViewCreateInfo imageView{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
		imageView.pNext = nullptr;
		imageView.viewType = VK_IMAGE_VIEW_TYPE_2D;
		imageView.format = VK_FORMAT_D16_UNORM;
		imageView.subresourceRange = {};
		imageView.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
		imageView.subresourceRange.baseMipLevel = 0;
		imageView.subresourceRange.levelCount = 1;
		imageView.subresourceRange.baseArrayLayer = 0;
		imageView.subresourceRange.layerCount = 1;
		imageView.image = _image.image;
		result = VK_SUCCESS;
		VkCall(result = vkCreateImageView(core::logical_device(), &imageView, nullptr, &_image.view), "Failed to create GBuffer(shadow mapping) image view...");

		// Create sampler to sample from to depth attachment
		// Used to sample in the fragment shader for shadowed rendering
		VkFilter filter = formatIsFilterable(core::physical_device(), VK_FORMAT_D16_UNORM, VK_IMAGE_TILING_OPTIMAL) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
		VkSamplerCreateInfo sampler{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
		sampler.pNext = nullptr;
		sampler.magFilter = filter;
		sampler.minFilter = filter;
		sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		sampler.addressModeW = sampler.addressModeV = sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		sampler.mipLodBias = 0.f;
		sampler.minLod = 0.f;
		sampler.maxLod = 1.f;
		sampler.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
		result = VK_SUCCESS;
		VkCall(result = vkCreateSampler(core::logical_device(), &sampler, nullptr, &_image.sampler), "Failed to create GBuffer(shadow mapping) image sampler...");

		setupRenderpass();

		// Create frame buffer
		VkFramebufferCreateInfo framebuffer{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
		framebuffer.pNext = nullptr;
		framebuffer.renderPass = _renderpass.render_pass;
		framebuffer.attachmentCount = 1;
		framebuffer.pAttachments = &_image.view;
		framebuffer.width = _image.width;
		framebuffer.height = _image.height;
		framebuffer.layers = 1;
		result = VK_SUCCESS;
		VkCall(result = vkCreateFramebuffer(core::logical_device(), &framebuffer, nullptr, &_framebuffer.framebuffer), "Failed to create GBuffer(shadow mapping) framebuffer...");

	}

	void vulkan_shadowmapping::setupDescriptorSets(VkDescriptorPool pool, VkDescriptorSetLayout layout)
	{
		VkDescriptorSetAllocateInfo allocInfo;
		utl::vector<VkWriteDescriptorSet> writeDescriptors;

		allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		allocInfo.pNext = VK_NULL_HANDLE;
		allocInfo.descriptorPool = pool;
		allocInfo.descriptorSetCount = 1;
		allocInfo.pSetLayouts = &layout;

		VkResult result{ VK_SUCCESS };
		VkCall(result = vkAllocateDescriptorSets(core::logical_device(), &allocInfo, &_descriptorSet), "Failed to create descriptor set...");

		VkDescriptorBufferInfo bufferInfo;
		bufferInfo.buffer = _uniformbuffer.buffer;
		bufferInfo.offset = 0;
		bufferInfo.range = sizeof(UniformBufferObject);


		std::vector<VkWriteDescriptorSet> descriptorWrites = {
			descriptor::setWriteDescriptorSet(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, _descriptorSet, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &bufferInfo, nullptr),
		};

		vkUpdateDescriptorSets(core::logical_device(), static_cast<u32>(descriptorWrites.size()), descriptorWrites.data(), 0, nullptr);
	}

	void vulkan_shadowmapping::setupPipeline(VkPipelineLayout layout, vulkan_framebuffer framebuffer)
	{
		VkPipelineInputAssemblyStateCreateInfo inputAssemblyState = descriptor::pipelineInputAssemblyStateCreate(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, VK_FALSE);
		VkPipelineViewportStateCreateInfo viewportState = descriptor::pipelineViewportStateCreate(1, 1);
		VkPipelineRasterizationStateCreateInfo rasterizationState = descriptor::pipelineRasterizationStateCreate(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
		rasterizationState.depthBiasEnable = VK_TRUE;
		VkPipelineMultisampleStateCreateInfo multisampleState = descriptor::pipelineMultisampleStateCreate(VK_SAMPLE_COUNT_1_BIT);
		VkPipelineColorBlendAttachmentState blendAttachmentState = descriptor::pipelineColorBlendAttachmentState(VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT, VK_FALSE);
		VkPipelineColorBlendStateCreateInfo colorBlendState = descriptor::pipelineColorBlendStateCreate(1, blendAttachmentState);

		std::vector<VkDynamicState> dynamicStateEnables{ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS };
		VkPipelineDynamicStateCreateInfo dynamicState = descriptor::pipelineDynamicStateCreate(dynamicStateEnables);
		VkPipelineDepthStencilStateCreateInfo depthStencilState = descriptor::pipelineDepthStencilStateCreateInfo(VK_TRUE, VK_TRUE, VK_FALSE, VK_FALSE, VK_COMPARE_OP_LESS_OR_EQUAL);

		utl::vector<VkVertexInputBindingDescription>			bindingDescription;
		bindingDescription.emplace_back([]() {
			VkVertexInputBindingDescription bBind{ 0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX };
		return bBind;
		}());
		utl::vector<VkVertexInputAttributeDescription>			attributeDescriptions;
		attributeDescriptions.emplace_back([]() {
			VkVertexInputAttributeDescription attributeDescriptions{ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 };
		return attributeDescriptions;
		}());

		VkPipelineVertexInputStateCreateInfo vertexInputInfo;
		vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
		vertexInputInfo.pNext = nullptr;
		vertexInputInfo.flags = 0;
		vertexInputInfo.vertexBindingDescriptionCount = static_cast<u32>(bindingDescription.size());
		vertexInputInfo.pVertexBindingDescriptions = bindingDescription.data();
		vertexInputInfo.vertexAttributeDescriptionCount = static_cast<u32>(attributeDescriptions.size());
		vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();

		_shader.loadFile(std::string{ "C:/Users/27042/Desktop/DX_Test/PrimalMerge/Engine/Graphics/Vulkan/Shaders/shadowmapping.vert.spv" }, shader_type::vertex);
		auto stage = _shader.getShaderStage();

		VkGraphicsPipelineCreateInfo pipelineCI;
		pipelineCI.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
		pipelineCI.pNext = nullptr;
		pipelineCI.flags = 0;
		pipelineCI.stageCount = 1;
		pipelineCI.pStages = &stage;
		pipelineCI.pVertexInputState = &vertexInputInfo;
		pipelineCI.pInputAssemblyState = &inputAssemblyState;
		pipelineCI.pTessellationState = VK_NULL_HANDLE;
		pipelineCI.pViewportState = &viewportState;
		pipelineCI.pRasterizationState = &rasterizationState;
		pipelineCI.pMultisampleState = &multisampleState;
		pipelineCI.pDepthStencilState = &depthStencilState;
		pipelineCI.pColorBlendState = &colorBlendState;
		pipelineCI.pDynamicState = &dynamicState;
		pipelineCI.layout = layout;
		pipelineCI.renderPass = _renderpass.render_pass;
		pipelineCI.subpass = 0;
		pipelineCI.basePipelineHandle = VK_NULL_HANDLE;
		pipelineCI.basePipelineIndex = -1;

		VkResult result{ VK_SUCCESS };
		VkCall(result = vkCreateGraphicsPipelines(core::logical_device(), VK_NULL_HANDLE, 1, &pipelineCI, nullptr, &_pipeline), "Failed to create graphics pipelines...");

		_shader.~vulkan_shader();
	}

	void vulkan_shadowmapping::runRenderpass(vulkan_cmd_buffer cmd_buffer, vulkan_surface* surface)
	{
		VkClearValue clearValue;
		clearValue.depthStencil = { 1.f, 0 };

		VkRenderPassBeginInfo info{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
		info.pNext = nullptr;
		info.renderPass = _renderpass.render_pass;
		info.framebuffer = _framebuffer.framebuffer;
		info.renderArea.extent.width = _shadowmapping_dim;
		info.renderArea.extent.height = _shadowmapping_dim;
		info.clearValueCount = 1;
		info.pClearValues = &clearValue;

		vkCmdBeginRenderPass(cmd_buffer.cmd_buffer, &info, VK_SUBPASS_CONTENTS_INLINE);

		VkViewport viewport;
		viewport.x = 0.0f;
		viewport.y = 0.0f;
		viewport.width = _shadowmapping_dim;
		viewport.height = _shadowmapping_dim;
		viewport.minDepth = 0.f;
		viewport.maxDepth = 1.f;
		vkCmdSetViewport(cmd_buffer.cmd_buffer, 0, 1, &viewport);

		VkRect2D scissor;
		scissor.extent.height = _shadowmapping_dim;
		scissor.extent.width = _shadowmapping_dim;
		scissor.offset.x = 0;
		scissor.offset.y = 0;
		vkCmdSetScissor(cmd_buffer.cmd_buffer, 0, 1, &scissor);

		vkCmdSetDepthBias(cmd_buffer.cmd_buffer, 1.25f, 0.f, 1.75f);

		vkCmdBindPipeline(cmd_buffer.cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, _pipeline);
		vkCmdBindDescriptorSets(cmd_buffer.cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, surface->layout_and_pool().pipelineLayout, 0, 1, &_descriptorSet, 0, nullptr);

		surface->getScene().drawGBuffer(cmd_buffer);

		vkCmdEndRenderPass(cmd_buffer.cmd_buffer);
	}

}