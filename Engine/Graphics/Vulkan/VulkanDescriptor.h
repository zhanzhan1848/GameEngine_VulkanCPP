#pragma once

#include "VulkanCommonHeaders.h"

namespace primal::graphics::vulkan::descriptor
{
	void createDescriptorSets(VkDevice device, utl::vector<VkDescriptorSet>& descriptorSets, VkDescriptorSetLayout& descriptorSetLayout, OUT VkDescriptorPool& descriptorPool, uniformBuffer& uniformBuffers);
	void createDescriptorPool(VkDevice device, VkDescriptorPool& descriptorPool);
	void createDescriptorSetLayout(VkDevice device, VkDescriptorSetLayout& descriptorSetLayout);

	void preparePipelines(VkDevice device, VkPipelineLayout& pipelineLayout, VkPipeline& pipeline, VkDescriptorSetLayout& descriptorSetLayout, vulkan_renderpass renderpass);

	void copyBuffer(VkDevice device, const VkCommandPool& pool, const VkQueue& queue, VkBuffer& srcBuffer, VkBuffer& dstBuffer, VkDeviceSize size);
}