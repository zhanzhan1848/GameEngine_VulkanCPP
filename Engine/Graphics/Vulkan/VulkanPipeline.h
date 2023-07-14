#pragma once
#include "VulkanCommonHeaders.h"

namespace primal::graphics::vulkan::pipeline
{
	// Pipeline Layout
	id::id_type add_layout(std::initializer_list<id::id_type>, VkPipelineLayoutCreateInfo);
	id::id_type add_layout(VkPipelineLayout);
	void remove_layout(id::id_type);
	VkPipelineLayout get_layout(id::id_type);

	// Pipeline Cache
	[[maybe_unused]] id::id_type add_cache();
	[[maybe_unused]] void remove_cache(id::id_type);
	[[maybe_unused]] VkPipelineCache get_cache(id::id_type);

	// Pipeline
	id::id_type add_pipeline(VkGraphicsPipelineCreateInfo);
	void remove_pipeline(id::id_type);
	VkPipeline get_pipeline(id::id_type);

}