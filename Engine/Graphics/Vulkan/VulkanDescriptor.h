#pragma once
#include "VulkanCommonHeaders.h"

namespace primal::graphics::vulkan::descriptor
{
	void initialize();
	void shutdown();

	VkDescriptorPool get_pool();

	// Descriptor Set Layout
	id::id_type add_layout(VkDescriptorSetLayoutCreateInfo);
	id::id_type add_layout(VkDescriptorSetLayout);
	void remove_layout(id::id_type);
	VkDescriptorSetLayout get_layout(id::id_type);

	// Descriptor Sets
	id::id_type add(VkDescriptorSetAllocateInfo);
	id::id_type add(VkDescriptorSet set);
	void remove(id::id_type);
	VkDescriptorSet get(id::id_type);
}