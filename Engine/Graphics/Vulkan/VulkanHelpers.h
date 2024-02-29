// Copyright (c) Contributors of Primal+
// Distributed under the MIT license. See the LICENSE file in the project root for more information.
#pragma once
#include "VulkanCommonHeaders.h"

namespace primal::graphics::vulkan
{
	namespace descriptor
	{
		VkDescriptorPoolSize descriptorPoolSize(VkDescriptorType type, u32 descriptorCount);
		VkWriteDescriptorSet setWriteDescriptorSet(VkStructureType type, VkDescriptorSet& set, u32 binding, VkDescriptorType dType, VkDescriptorBufferInfo * buffer);
		VkWriteDescriptorSet setWriteDescriptorSet(VkStructureType type, VkDescriptorSet& set, u32 binding, VkDescriptorType dType, const VkDescriptorImageInfo* const image);
		VkWriteDescriptorSet setWriteDescriptorSet(VkStructureType type, VkDescriptorSet& set, u32 binding, u32 count, VkDescriptorType dType, const VkDescriptorImageInfo* const image);
		VkWriteDescriptorSet setWriteDescriptorSet(VkStructureType type, utl::vector<VkDescriptorSet>& sets, u32 num, u32 binding, VkDescriptorType dType, VkDescriptorBufferInfo * buffer, VkDescriptorImageInfo * image);
		VkDescriptorSetAllocateInfo descriptorSetAllocate(VkDescriptorPool descriptorPool, const VkDescriptorSetLayout * pSetLayouts, u32 descriptorSetCount);
		VkWriteDescriptorSet writeDescriptorSets(VkDescriptorSet dstSet, VkDescriptorType type, u32 binding, VkDescriptorBufferInfo * bufferInfo, u32 descriptorCount);
		VkDescriptorSetLayoutBinding descriptorSetLayoutBinding(u32 binding, VkShaderStageFlags stageFlags, VkDescriptorType type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VkSampler * sampler = nullptr, u32 descriptorCount = 1);
		VkDescriptorSetLayoutCreateInfo descriptorSetLayoutCreate(const VkDescriptorSetLayoutBinding * pBindings, u32 bindingCount);
		VkDescriptorSetLayoutCreateInfo descriptorSetLayoutCreate(const std::vector<VkDescriptorSetLayoutBinding>& bindings);
		VkDescriptorSetLayoutCreateInfo descriptorSetLayoutCreate(const utl::vector<VkDescriptorSetLayoutBinding>& bindings);
		VkPipelineLayoutCreateInfo pipelineLayoutCreate(u32 setLayoutCount, const VkDescriptorSetLayout * pSetLayouts, u32 pushCount, VkPushConstantRange * constant);
		VkPipelineVertexInputStateCreateInfo pipelineVertexInputStateCreate(const std::vector<VkVertexInputBindingDescription>& vertexBindingDescriptions, const std::vector<VkVertexInputAttributeDescription>& vertexAttributeDescriptions);
		VkPushConstantRange pushConstantRange(VkShaderStageFlags stageFlags, u32 size, u32 offset);
		VkPipelineInputAssemblyStateCreateInfo pipelineInputAssemblyStateCreate(VkPrimitiveTopology topology, VkPipelineInputAssemblyStateCreateFlags flags, VkBool32 primitiveRestartEnable);
		VkPipelineRasterizationStateCreateInfo pipelineRasterizationStateCreate(VkPolygonMode polygonMode = VK_POLYGON_MODE_FILL, VkCullModeFlags cullMode = VK_CULL_MODE_BACK_BIT, VkFrontFace frontFace = VK_FRONT_FACE_CLOCKWISE, VkPipelineRasterizationStateCreateFlags flags = 0);
		VkPipelineColorBlendAttachmentState pipelineColorBlendAttachmentState(VkColorComponentFlags colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT, VkBool32 blendEnable = false);
		VkPipelineColorBlendStateCreateInfo pipelineColorBlendStateCreate(u32 attachmentCount, const VkPipelineColorBlendAttachmentState & pAttachments, VkBool32 logicOpEnable = VK_FALSE, VkLogicOp logicOp = VK_LOGIC_OP_COPY);
		VkPipelineDepthStencilStateCreateInfo pipelineDepthStencilStateCreateInfo(VkBool32 depthTestEnable, VkBool32 depthWriteEnable, VkBool32 depthBoundsTest, VkBool32 stencilTest, VkCompareOp depthCompareOp);
		VkPipelineViewportStateCreateInfo pipelineViewportStateCreate(u32 viewportCount, u32 scissorCount, VkPipelineViewportStateCreateFlags flags = 0);
		VkPipelineMultisampleStateCreateInfo pipelineMultisampleStateCreate(VkSampleCountFlagBits rasterizationSamples = VK_SAMPLE_COUNT_1_BIT, VkPipelineMultisampleStateCreateFlags flags = 0);
		VkPipelineDynamicStateCreateInfo pipelineDynamicStateCreate(const std::vector<VkDynamicState>& pDynamicStates, VkPipelineDynamicStateCreateFlags flags = 0);
		VkPipelineTessellationStateCreateInfo pipelineTessellationStateCreate(u32 patchControlPoints);
		VkGraphicsPipelineCreateInfo pipelineCreate(VkPipelineLayout layout, VkRenderPass renderPass, VkPipelineCreateFlags flags = 0);
		VkShaderModule loadShader(VkDevice device, const char * fileName);
		VkPipelineShaderStageCreateInfo loadShader(VkDevice device, std::string filename, VkShaderStageFlagBits stage);
		VkVertexInputBindingDescription vertexInputBindingDescription(u32 binding, u32 stride, VkVertexInputRate inputRate);
	}

	bool vulkan_success(VkResult result);

	void copyBuffer(VkDevice device, u32 index, const VkCommandPool& pool, VkBuffer& srcBuffer, VkBuffer& dstBuffer, VkDeviceSize size);

	void createBuffer(VkDevice device, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& bufferMemory);

	VkCommandBuffer beginSingleCommand(VkDevice device, const VkCommandPool& pool);

	void endSingleCommand(VkDevice device, u32 index, const VkCommandPool& pool, VkCommandBuffer commandBuffer);

	u32 formatIsFilterable(VkPhysicalDevice physicalDevice, VkFormat format, VkImageTiling tilling);

	utl::vector<VkVertexInputBindingDescription> getVertexInputBindDescriptor();

	utl::vector<VkVertexInputAttributeDescription> getVertexInputAttributeDescriptor();

	void setImageLayout(VkCommandBuffer cmdbuffer, VkImage image, VkImageLayout oldImageLayout, VkImageLayout newImageLayout, VkImageSubresourceRange subresourceRange, VkPipelineStageFlags srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VkPipelineStageFlags dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
}