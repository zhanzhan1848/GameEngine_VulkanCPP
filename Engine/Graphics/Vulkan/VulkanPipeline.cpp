#include "VulkanPipeline.h"

#include "VulkanUtility.h"
#include "VulkanDescriptor.h"

namespace primal::graphics::vulkan::pipeline
{
	namespace
	{
		utl::free_list<VkPipelineLayout>			pipelineLayouts;

		utl::free_list<PE_VkPipelineCache>			pipelineCaches;

		utl::free_list<VkPipeline>					pipelines;
	} // anonymous namespace


	id::id_type add_layout(std::initializer_list<id::id_type> ids, VkPipelineLayoutCreateInfo info)
	{
		utl::vector<VkDescriptorSetLayout> d_layout;
		for (auto id = ids.begin(); id != ids.end(); ++id)
		{
			d_layout.emplace_back(descriptor::get_layout(*id));
		}
		PE_VkPipelineLayout layout;
		layout.setter(info);
		layout.compile();
		return pipelineLayouts.add(layout.getter());
	}

	id::id_type add_layout(VkPipelineLayout layout)
	{
		return pipelineLayouts.add(layout);
	}

	void remove_layout(id::id_type id)
	{
		pipelineLayouts.remove(id);
	}

	VkPipelineLayout get_layout(id::id_type id)
	{
		return pipelineLayouts[id];
	}

	id::id_type add_pipeline(VkGraphicsPipelineCreateInfo info)
	{
		VkPipeline pipeline;
		VkResult result{ VK_SUCCESS };
		VkCall(result = vkCreateGraphicsPipelines(core::logical_device(), nullptr, 1, &info, nullptr, &pipeline), "Failed to create pipeline...");
		return pipelines.add(pipeline);
	}

	void remove_pipeline(id::id_type id)
	{
		pipelines.remove(id);
	}

	VkPipeline get_pipeline(id::id_type id)
	{
		return pipelines[id];
	}
}