#pragma once

#include "VulkanCommonHeaders.h"

namespace primal::graphics::vulkan::descriptor
{
	void createDescriptorSets(VkDevice device, utl::vector<VkDescriptorSet>& descriptorSets, VkDescriptorSetLayout& descriptorSetLayout, OUT VkDescriptorPool& descriptorPool, uniformBuffer& uniformBuffers, vulkan_texture tex);
	void createDescriptorPool(VkDevice device, VkDescriptorPool& descriptorPool);
	void createDescriptorSetLayout(VkDevice device, VkDescriptorSetLayout& descriptorSetLayout);

	void preparePipelines(VkDevice device, VkPipelineLayout& pipelineLayout, VkPipeline& pipeline, VkDescriptorSetLayout& descriptorSetLayout, vulkan_renderpass renderpass);
}