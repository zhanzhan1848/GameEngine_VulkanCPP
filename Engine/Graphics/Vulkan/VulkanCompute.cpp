#include "VulkanCompute.h"
#include "VulkanCore.h"
#include "VulkanHelpers.h"
#include "VulkanData.h"
#include "VulkanCommandBuffer.h"
#include "VulkanShader.h"
#include "VulkanContent.h"
#include "VulkanTexture.h"

namespace primal::graphics::vulkan::compute
{
	namespace
	{
		id::id_type						descriptor_pool_id;
		id::id_type						descriptor_set_layout_id;
		id::id_type						descriptor_set_ids;
		id::id_type						pipeline_layout_id;
		id::id_type						pipeline_id;
		utl::vector<id::id_type>		compute_shader_ids;
		id::id_type						output_tex_id;
		VkQueue							computeQueue;
		VkSemaphore						semaphore;
		VkCommandPool					cmd_pool;
		VkCommandBuffer					cmd_buffer;
		VkBuffer						storageBuffer;
		VkDeviceMemory					storageBufferMemory;
		VkBuffer						outputBuffer;
		VkDeviceMemory					outputBufferMemory;

		const std::string test_shader_path{"C:/Users/zy/Desktop/PrimalMerge/PrimalEngine/Engine/Graphics/Vulkan/Shaders/spv/test.comp.spv"};

		bool get_compute_queue()
		{
			vkGetDeviceQueue(core::logical_device(), core::compute_family_queue_index(), 0, &computeQueue);
			return true;
		}

		bool create_storage_buffer()
		{
			VkBufferUsageFlagBits flags = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
			createBuffer(core::logical_device(), sizeof(UniformBufferObjectPlus), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, storageBuffer, storageBufferMemory);
			//id::id_type storage_buffer_id = data::create_data(data::engine_vulkan_data::vulkan_buffer, (void*)&flags, sizeof(UniformBufferObjectPlus));
			createBuffer(core::logical_device(), sizeof(math::v4) * 22500, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, outputBuffer, outputBufferMemory);
			//id::id_type output_buffer_id = data::create_data(data::engine_vulkan_data::vulkan_buffer, (void*)&flags, sizeof(math::v4) * 22500);
			return true;
		}

		void flush_storage_buffer(const void* const data, u32 size)
		{
			vkQueueWaitIdle(computeQueue);

			void* d;
			vkMapMemory(core::logical_device(), storageBufferMemory, 0, size, 0, &d);
			memcpy(d, data, size);
			vkUnmapMemory(core::logical_device(), storageBufferMemory);
		}

		bool create_descriptor_set_layout()
		{
			std::vector<VkDescriptorSetLayoutBinding> bindings;
			bindings.emplace_back(descriptor::descriptorSetLayoutBinding(0, VK_SHADER_STAGE_COMPUTE_BIT, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER));
			bindings.emplace_back(descriptor::descriptorSetLayoutBinding(1, VK_SHADER_STAGE_COMPUTE_BIT, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER));
			//bindings.emplace_back(descriptor::descriptorSetLayoutBinding(1, VK_SHADER_STAGE_COMPUTE_BIT, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE));
			
			VkDescriptorSetLayoutCreateInfo descriptorLayout = descriptor::descriptorSetLayoutCreate(bindings);
			return id::is_valid(descriptor_set_layout_id = data::create_data(data::engine_vulkan_data::vulkan_descriptor_set_layout, static_cast<void*>(&descriptorLayout), 0));
		}

		bool create_descripotr_pool()
		{
			//descriptor::descriptorPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, static_cast<u32>(frame_buffer_count * 3) + (u32)100),
			//descriptor::descriptorPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, static_cast<u32>(frame_buffer_count) + (u32)100),
			utl::vector<VkDescriptorPoolSize>	poolSize;
			poolSize.emplace_back(descriptor::descriptorPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, static_cast<u32>(frame_buffer_count) * 2));
			//poolSize.emplace_back(descriptor::descriptorPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, static_cast<u32>(frame_buffer_count)));
			VkDescriptorPoolCreateInfo poolInfo;
			poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
			poolInfo.pNext = VK_NULL_HANDLE;
			poolInfo.flags = 0;
			poolInfo.poolSizeCount = static_cast<u32>(poolSize.size());
			poolInfo.pPoolSizes = poolSize.data();
			poolInfo.maxSets = static_cast<u32>(frame_buffer_count) * 2;
			return id::is_valid(descriptor_pool_id = data::create_data(data::engine_vulkan_data::vulkan_descriptor_pool, static_cast<void*>(&poolInfo), 0));
		}

		bool create_pipeline_layout()
		{
			auto ds = data::get_data<VkDescriptorSetLayout>(descriptor_set_layout_id);
			VkPipelineLayoutCreateInfo info{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 1, &ds, 0, nullptr };
			return id::is_valid(pipeline_layout_id = data::create_data(data::engine_vulkan_data::vulkan_pipeline_layout, static_cast<void*>(&info), 0));
		}

		bool import_shader()
		{
			id::id_type shader_id;
			if (!id::is_valid(shader_id = shaders::add(test_shader_path, shader_type::compute))) return false;
			compute_shader_ids.emplace_back(shader_id);
			return true;
		}

		bool create_pipeline()
		{
			if (!import_shader()) return false;
			auto ps = data::get_data<VkPipelineLayout>(pipeline_layout_id);
			VkComputePipelineCreateInfo info;
			info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
			info.pNext = nullptr;
			info.layout = ps;
			info.stage = shaders::get_shader(compute_shader_ids[0]).getShaderStage();
			info.flags = 0;
			info.basePipelineHandle = nullptr;

			return id::is_valid(pipeline_id = data::create_data(data::engine_vulkan_data::vulkan_pipeline, static_cast<void*>(&info), 1));
		}

		bool create_descriptor_sets()
		{
			VkDescriptorSetAllocateInfo info;
			info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
			info.pNext = nullptr;
			info.descriptorPool = data::get_data<VkDescriptorPool>(descriptor_pool_id);
			info.pSetLayouts = &data::get_data<VkDescriptorSetLayout>(descriptor_set_layout_id);
			info.descriptorSetCount = 1;

			return id::is_valid(descriptor_set_ids = data::create_data(data::engine_vulkan_data::vulkan_descriptor_sets, static_cast<void*>(&info), 0));
		}

		bool update_descriptor_sets()
		{
			auto set = data::get_data<VkDescriptorSet>(descriptor_set_ids);
			utl::vector<VkWriteDescriptorSet> writes;
			VkDescriptorBufferInfo bInfo;
			bInfo.buffer = storageBuffer;
			bInfo.offset = 0;
			bInfo.range = sizeof(UniformBufferObjectPlus);
			writes.emplace_back(descriptor::setWriteDescriptorSet(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, set, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bInfo));
			VkDescriptorBufferInfo outInfo;
			outInfo.buffer = outputBuffer;
			outInfo.offset = 0;
			outInfo.range = sizeof(math::v4) * 22500;
			writes.emplace_back(descriptor::setWriteDescriptorSet(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, set, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &outInfo));
			/*VkDescriptorImageInfo info;
			info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
			info.imageView = textures::get_texture(output_tex_id).getTexture().view;
			info.sampler = textures::get_texture(output_tex_id).getTexture().sampler;
			writes.emplace_back(descriptor::setWriteDescriptorSet(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, set, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &info));*/
			vkUpdateDescriptorSets(core::logical_device(), (u32)writes.size(), writes.data(), 0, nullptr);
			return true;
		}

		bool create_output_texture()
		{
			vulkan_texture tex;
			VkImageCreateInfo image{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
			image.imageType = VK_IMAGE_TYPE_2D;
			image.extent.height = 900;
			image.extent.width = 1600;
			image.extent.depth = 1;
			image.mipLevels = 1;
			image.arrayLayers = 1;
			image.samples = VK_SAMPLE_COUNT_1_BIT;
			image.tiling = VK_IMAGE_TILING_OPTIMAL;
			image.format = VK_FORMAT_R8G8B8A8_UNORM;
			image.flags = 0;
			image.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT; // ! VK_IMAGE_USAGE_STORAGE_BIT for storage image
			image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			if (core::graphics_family_queue_index() != core::compute_family_queue_index())
			{
				u32 queue_family_indices[]{ (u32)core::graphics_family_queue_index(),
											(u32)core::presentation_family_queue_index() };
				image.sharingMode = VK_SHARING_MODE_CONCURRENT;
				image.queueFamilyIndexCount = 2;
				image.pQueueFamilyIndices = queue_family_indices;
			}
			VkResult result{ VK_SUCCESS };
			VkCall(result = vkCreateImage(core::logical_device(), &image, nullptr, &tex.image), "Failed to create compute image...");

			VkMemoryRequirements memReq;
			vkGetImageMemoryRequirements(core::logical_device(), tex.image, &memReq);
			VkMemoryAllocateInfo alloc{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
			alloc.pNext = nullptr;
			alloc.allocationSize = memReq.size;
			alloc.memoryTypeIndex = core::find_memory_index(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
			result = VK_SUCCESS;
			VkCall(result = vkAllocateMemory(core::logical_device(), &alloc, nullptr, &tex.memory), "Failed to allocate compute memory...");
			result = VK_SUCCESS;
			VkCall(result = vkBindImageMemory(core::logical_device(), tex.image, tex.memory, 0), "Failed to bind compute to memory...");

			vulkan_cmd_buffer cmdBuffer = allocate_cmd_buffer_begin_single_use(core::logical_device(), core::get_current_command_pool());

			VkImageSubresourceRange subresourceRange = {};
			subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			subresourceRange.baseMipLevel = 0;
			subresourceRange.levelCount = 1;
			subresourceRange.layerCount = 1;

			setImageLayout(cmdBuffer.cmd_buffer, tex.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, subresourceRange);

			VkQueue queue;
			vkGetDeviceQueue(core::logical_device(), core::graphics_family_queue_index(), 0, &queue);

			end_cmd_buffer_single_use(core::logical_device(), core::get_current_command_pool(), cmdBuffer, queue);

			VkImageViewCreateInfo imageView{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
			imageView.pNext = nullptr;
			imageView.viewType = VK_IMAGE_VIEW_TYPE_2D;
			imageView.format = VK_FORMAT_R8G8B8A8_UNORM;
			imageView.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			imageView.subresourceRange.baseMipLevel = 0;
			imageView.subresourceRange.levelCount = 1;
			imageView.subresourceRange.baseArrayLayer = 0;
			imageView.subresourceRange.layerCount = 1;
			imageView.image = tex.image;
			result = VK_SUCCESS;
			VkCall(result = vkCreateImageView(core::logical_device(), &imageView, nullptr, &tex.view), "Failed to create compute image view...");

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
			VkCall(result = vkCreateSampler(core::logical_device(), &sampler, nullptr, &tex.sampler), "Failed to create compute image sampler...");

			if (!id::is_valid(output_tex_id = textures::add(tex))) return false;
		}

		bool create_command_pool_and_semaphore()
		{
			VkCommandPoolCreateInfo poolInfo;
			poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
			poolInfo.pNext = nullptr;
			poolInfo.queueFamilyIndex = core::compute_family_queue_index();
			poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
			VkResult result{ VK_SUCCESS };
			VkCall(result = vkCreateCommandPool(core::logical_device(), &poolInfo, nullptr, &cmd_pool), "Failed to create compute command pool...");

			VkSemaphoreCreateInfo sInfo;
			sInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
			sInfo.pNext = nullptr;
			sInfo.flags = 0;
			VkCall(result = vkCreateSemaphore(core::logical_device(), &sInfo, nullptr, &semaphore), "Failed to create compute semaphore...");

			return true;
		}

		void flushCommandBuffer()
		{
			vkQueueWaitIdle(computeQueue);

			cmd_buffer = beginSingleCommand(core::logical_device(), cmd_pool);

			auto pipeline = data::get_data<VkPipeline>(pipeline_id);
			auto pipelineLayout = data::get_data<VkPipelineLayout>(pipeline_layout_id);
			auto set = data::get_data<VkDescriptorSet>(descriptor_set_ids);
			vkCmdBindPipeline(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
			vkCmdBindDescriptorSets(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &set, 0, 0);

			vkCmdDispatch(cmd_buffer, 7, 4, 1);

			vkEndCommandBuffer(cmd_buffer);
		}
	} // anonymous namespace

	bool initialize()
	{
		if (!get_compute_queue()) return false;
		if (!create_storage_buffer()) return false;
		if (!create_descripotr_pool()) return false;
		if (!create_descriptor_set_layout()) return false;
		if (!create_pipeline_layout()) return false;
		if (!create_pipeline()) return false;
		if (!create_output_texture()) return false;
		if (!create_descriptor_sets()) return false;
		if (!update_descriptor_sets()) return false;
		if (!create_command_pool_and_semaphore()) return false;
		return true;
	}

	void shutdown()
	{
		vkQueueWaitIdle(computeQueue);

		data::remove_data(data::engine_vulkan_data::vulkan_descriptor_sets, descriptor_set_ids);
		data::remove_data(data::engine_vulkan_data::vulkan_pipeline, pipeline_id);
		data::remove_data(data::engine_vulkan_data::vulkan_pipeline_layout, pipeline_layout_id);
		data::remove_data(data::engine_vulkan_data::vulkan_descriptor_set_layout, descriptor_set_layout_id);
		data::remove_data(data::engine_vulkan_data::vulkan_descriptor_pool, descriptor_pool_id);
		vkFreeCommandBuffers(core::logical_device(), cmd_pool, 1, &cmd_buffer);
		vkFreeMemory(core::logical_device(), storageBufferMemory, nullptr);
		vkDestroyBuffer(core::logical_device(), storageBuffer, nullptr);
		vkDestroyBuffer(core::logical_device(), outputBuffer, nullptr);
		vkDestroySemaphore(core::logical_device(), semaphore, nullptr);
		vkDestroyCommandPool(core::logical_device(), cmd_pool, nullptr);
	}

	id::id_type get_output_tex_id()
	{
		return output_tex_id;
	}

	VkSemaphore get_compute_semaphore()
	{
		return semaphore;
	}

	void run(const void* const data, u32 size)
	{
		flush_storage_buffer(data, size);

		flushCommandBuffer();
	}

	void submit()
	{
		VkPipelineStageFlags waitStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;

		VkSubmitInfo submitInfo;
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submitInfo.pNext = nullptr;
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &cmd_buffer;
		submitInfo.waitSemaphoreCount = 0;
		submitInfo.pWaitSemaphores = nullptr;
		submitInfo.pWaitDstStageMask = &waitStageMask;
		submitInfo.signalSemaphoreCount = 1;
		submitInfo.pSignalSemaphores = &semaphore;
		VkResult result{ VK_SUCCESS };
		VkCall(result = vkQueueSubmit(computeQueue, 1, &submitInfo, nullptr), "Failed to submit compute queue...");
	}

	void outputImageData()
	{
		auto& texture = textures::get_texture(output_tex_id).getTexture();
		transitionImageLayout(core::logical_device(), core::graphics_family_queue_index(), core::get_current_command_pool(), texture.image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		copyBufferToImage(core::logical_device(), core::graphics_family_queue_index(), core::get_current_command_pool(), outputBuffer, texture.image, 1600, 900);
		transitionImageLayout(core::logical_device(), core::graphics_family_queue_index(), core::get_current_command_pool(), texture.image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

		/*VkBufferMemoryBarrier memoryBarrier;
		memoryBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
		memoryBarrier.pNext = nullptr;
		memoryBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		memoryBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		memoryBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		memoryBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		memoryBarrier.buffer = outputBuffer;
		memoryBarrier.offset = 0;
		memoryBarrier.size = sizeof(math::v4) * 150;
		vkCmdPipelineBarrier(cmd_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 1, &memoryBarrier, 0, nullptr);*/
	}

	VkBuffer& get_output_buffer()
	{
		return outputBuffer;
	}
}