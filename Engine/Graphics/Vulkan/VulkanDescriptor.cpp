#include "VulkanDescriptor.h"

#include "VulkanUtility.h"

namespace primal::graphics::vulkan::descriptor
{
	namespace
	{
		PE_VkDescriptorPool								pool;

		utl::free_list<VkDescriptorSetLayout>			descriptor_set_layout_list;

		utl::free_list<VkDescriptorSet>					descriptor_sets_list;
	} // anonymous namespace

	void initialize()
	{
		utl::vector<VkDescriptorPoolSize>	poolSize;
		poolSize.emplace_back(VkDescriptorPoolSize({ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,16 }));
		poolSize.emplace_back(VkDescriptorPoolSize({ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 24 }));
		pool.setter({
			// Vulkan Descriptor Pool Create Info
			VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,                                  //sType
			nullptr,                                                                        //pNext
			0,                                                                              //flags
			36,                                                                              //maxSets
			static_cast<u32>(poolSize.size()),                                                                              //poolSizeCount
			poolSize.data()                                                                         //pPoolSizes
			});
		pool.compile();
	}

	void shutdown()
	{
		pool.release();
	}

	VkDescriptorPool get_pool()
	{
		return pool.getter();
	}

	id::id_type add_layout(VkDescriptorSetLayoutCreateInfo info)
	{
		PE_VkDescriptorSetLayout layout;
		layout.setter(info);
		layout.compile();
		return descriptor_set_layout_list.add(layout.getter());
	}

	id::id_type add_layout(VkDescriptorSetLayout layout)
	{
		return descriptor_set_layout_list.add(layout);
	}

	void remove_layout(id::id_type id)
	{
		descriptor_set_layout_list.remove(id);
	}

	VkDescriptorSetLayout get_layout(id::id_type id)
	{
		return descriptor_set_layout_list[id];
	}

	id::id_type add(VkDescriptorSetAllocateInfo info)
	{
		VkDescriptorSet set;
		VkResult result{ VK_SUCCESS };
		VkCall(result = vkAllocateDescriptorSets(core::logical_device(), &info, &set), "Failed to allocate descriptor set...");
		return descriptor_sets_list.add(set);
	}

	id::id_type add(VkDescriptorSet set)
	{
		return descriptor_sets_list.add(set);
	}

	void remove(id::id_type id)
	{
		descriptor_sets_list.remove(id);
	}

	VkDescriptorSet get(id::id_type id)
	{
		return descriptor_sets_list[id];
	}
}