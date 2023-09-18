#include "VulkanGBuffer.h"
#include "VulkanHelpers.h"
#include "VulkanCore.h"
#include "VulkanSurface.h"
#include "VulkanCamera.h"
#include "VulkanContent.h"
#include "VulkanPipeline.h"
#include "VulkanDescriptor.h"
#include "VulkanResources.h"
#include "VulkanUtility.h"
#include "VulkanData.h"
#include "VulkanData.cpp"

#include "EngineAPI/GameEntity.h"
#include "Components/Transform.h"

#include <array>
#include <random>

namespace primal::graphics::vulkan
{
	namespace
	{
		game_entity::entity create_one_game_entity(math::v3 position, math::v3 rotation)
		{
			transform::init_info transform_info{};
			DirectX::XMVECTOR quat{ DirectX::XMQuaternionRotationRollPitchYawFromVector(DirectX::XMLoadFloat3(&rotation)) };
			math::v4a rot_quat;
			DirectX::XMStoreFloat4A(&rot_quat, quat);
			memcpy(&transform_info.rotation[0], &rot_quat.x, sizeof(transform_info.rotation));
			memcpy(&transform_info.position[0], &position.x, sizeof(transform_info.position));

			game_entity::entity_info entity_info{};
			entity_info.transform = &transform_info;
			game_entity::entity ntt{ game_entity::create(entity_info) };
			assert(ntt.is_valid());
			return ntt;
		}

		float lerp(float a, float b, float f)
		{
			return a + f * (b - a);
		}

		VkCommandBuffer createCommandBuffer(VkCommandBufferLevel level, VkCommandPool pool, bool begin)
		{
			VkCommandBufferAllocateInfo cmdBufAllocateInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool, level, 1};
			VkCommandBuffer cmdBuffer;
			vkAllocateCommandBuffers(core::logical_device(), &cmdBufAllocateInfo, &cmdBuffer);
			// If requested, also start recording for the new command buffer
			if (begin)
			{
				VkCommandBufferBeginInfo cmdBufInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
				vkBeginCommandBuffer(cmdBuffer, &cmdBufInfo);
			}
			return cmdBuffer;
		}

		void setImageLayout(
			VkCommandBuffer cmdbuffer,
			VkImage image,
			VkImageLayout oldImageLayout,
			VkImageLayout newImageLayout,
			VkImageSubresourceRange subresourceRange,
			VkPipelineStageFlags srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
			VkPipelineStageFlags dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)
		{
			// Create an image barrier object
			VkImageMemoryBarrier imageMemoryBarrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr };
			imageMemoryBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			imageMemoryBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			imageMemoryBarrier.oldLayout = oldImageLayout;
			imageMemoryBarrier.newLayout = newImageLayout;
			imageMemoryBarrier.image = image;
			imageMemoryBarrier.subresourceRange = subresourceRange;

			// Source layouts (old)
			// Source access mask controls actions that have to be finished on the old layout
			// before it will be transitioned to the new layout
			switch (oldImageLayout)
			{
			case VK_IMAGE_LAYOUT_UNDEFINED:
				// Image layout is undefined (or does not matter)
				// Only valid as initial layout
				// No flags required, listed only for completeness
				imageMemoryBarrier.srcAccessMask = 0;
				break;

			case VK_IMAGE_LAYOUT_PREINITIALIZED:
				// Image is preinitialized
				// Only valid as initial layout for linear images, preserves memory contents
				// Make sure host writes have been finished
				imageMemoryBarrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
				break;

			case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
				// Image is a color attachment
				// Make sure any writes to the color buffer have been finished
				imageMemoryBarrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
				break;

			case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
				// Image is a depth/stencil attachment
				// Make sure any writes to the depth/stencil buffer have been finished
				imageMemoryBarrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
				break;

			case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
				// Image is a transfer source
				// Make sure any reads from the image have been finished
				imageMemoryBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
				break;

			case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
				// Image is a transfer destination
				// Make sure any writes to the image have been finished
				imageMemoryBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
				break;

			case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
				// Image is read by a shader
				// Make sure any shader reads from the image have been finished
				imageMemoryBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
				break;
			default:
				// Other source layouts aren't handled (yet)
				break;
			}

			// Target layouts (new)
			// Destination access mask controls the dependency for the new image layout
			switch (newImageLayout)
			{
			case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
				// Image will be used as a transfer destination
				// Make sure any writes to the image have been finished
				imageMemoryBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
				break;

			case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
				// Image will be used as a transfer source
				// Make sure any reads from the image have been finished
				imageMemoryBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
				break;

			case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
				// Image will be used as a color attachment
				// Make sure any writes to the color buffer have been finished
				imageMemoryBarrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
				break;

			case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
				// Image layout will be used as a depth/stencil attachment
				// Make sure any writes to depth/stencil buffer have been finished
				imageMemoryBarrier.dstAccessMask = imageMemoryBarrier.dstAccessMask | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
				break;

			case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
				// Image will be read in a shader (sampler, input attachment)
				// Make sure any writes to the image have been finished
				if (imageMemoryBarrier.srcAccessMask == 0)
				{
					imageMemoryBarrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
				}
				imageMemoryBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
				break;
			default:
				// Other source layouts aren't handled (yet)
				break;
			}

			// Put barrier inside setup command buffer
			vkCmdPipelineBarrier(
				cmdbuffer,
				srcStageMask,
				dstStageMask,
				0,
				0, nullptr,
				0, nullptr,
				1, &imageMemoryBarrier);
		}

		void flushCommandBuffer(VkCommandBuffer commandBuffer, VkCommandPool pool, bool free = true)
		{
			if (commandBuffer == VK_NULL_HANDLE)
			{
				return;
			}

			vkEndCommandBuffer(commandBuffer);

			VkSubmitInfo submitInfo{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
			submitInfo.commandBufferCount = 1;
			submitInfo.pCommandBuffers = &commandBuffer;
			// Create fence to ensure that the command buffer has finished executing
			VkFenceCreateInfo fenceInfo{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO , nullptr, 0 };
			VkFence fence;
			vkCreateFence(core::logical_device(), &fenceInfo, nullptr, &fence);

			VkQueue graphicsQueue;
			vkGetDeviceQueue(core::logical_device(), core::graphics_family_queue_index(), 0, &graphicsQueue);
			// Submit to the queue
			vkQueueSubmit(graphicsQueue, 1, &submitInfo, fence);
			// Wait for the fence to signal that command buffer has finished executing
			vkWaitForFences(core::logical_device(), 1, &fence, VK_TRUE, 1);
			vkDestroyFence(core::logical_device(), fence, nullptr);
			if (free)
			{
				vkFreeCommandBuffers(core::logical_device(), pool, 1, &commandBuffer);
			}
		}

		void imageFromBuffer(void* buffer, VkDeviceSize bufferSize, VkFormat format, u32 width, u32 height, VkCommandPool pool, VkFilter filter, VkImageUsageFlags imageUsageFlags, VkImageLayout imageLayout, vulkan_texture& tex)
		{
			assert(buffer);

			u32 mipLevels = 1;

			VkMemoryAllocateInfo memAllocInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
			VkMemoryRequirements memReqs;

			// Use a separate command buffer for texture loading
			VkCommandBuffer copyCmd = createCommandBuffer(VK_COMMAND_BUFFER_LEVEL_PRIMARY, pool, true);

			// Create a host-visible staging buffer that contains the raw image data
			VkBuffer stagingBuffer;
			VkDeviceMemory stagingMemory;

			VkBufferCreateInfo bufferCreateInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
			bufferCreateInfo.size = bufferSize;
			// This buffer is used as a transfer source for the buffer copy
			bufferCreateInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
			bufferCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

			vkCreateBuffer(core::logical_device(), &bufferCreateInfo, nullptr, &stagingBuffer);

			// Get memory requirements for the staging buffer (alignment, memory type bits)
			vkGetBufferMemoryRequirements(core::logical_device(), stagingBuffer, &memReqs);

			memAllocInfo.allocationSize = memReqs.size;
			// Get memory type index for a host visible buffer
			memAllocInfo.memoryTypeIndex = core::find_memory_index(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

			vkAllocateMemory(core::logical_device(), &memAllocInfo, nullptr, &stagingMemory);
			vkBindBufferMemory(core::logical_device(), stagingBuffer, stagingMemory, 0);

			// Copy texture data into staging buffer
			uint8_t* data;
			vkMapMemory(core::logical_device(), stagingMemory, 0, memReqs.size, 0, (void**)&data);
			memcpy(data, buffer, bufferSize);
			vkUnmapMemory(core::logical_device(), stagingMemory);

			VkBufferImageCopy bufferCopyRegion = {};
			bufferCopyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			bufferCopyRegion.imageSubresource.mipLevel = 0;
			bufferCopyRegion.imageSubresource.baseArrayLayer = 0;
			bufferCopyRegion.imageSubresource.layerCount = 1;
			bufferCopyRegion.imageExtent.width = width;
			bufferCopyRegion.imageExtent.height = height;
			bufferCopyRegion.imageExtent.depth = 1;
			bufferCopyRegion.bufferOffset = 0;

			// Create optimal tiled target image
			VkImageCreateInfo imageCreateInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
			imageCreateInfo.imageType = VK_IMAGE_TYPE_2D;
			imageCreateInfo.format = format;
			imageCreateInfo.mipLevels = mipLevels;
			imageCreateInfo.arrayLayers = 1;
			imageCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
			imageCreateInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
			imageCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
			imageCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			imageCreateInfo.extent = { width, height, 1 };
			imageCreateInfo.usage = imageUsageFlags;
			// Ensure that the TRANSFER_DST bit is set for staging
			if (!(imageCreateInfo.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
			{
				imageCreateInfo.usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
			}
			vkCreateImage(core::logical_device(), &imageCreateInfo, nullptr, &tex.image);

			vkGetImageMemoryRequirements(core::logical_device(), tex.image, &memReqs);

			VkDeviceMemory        deviceMemory;

			memAllocInfo.allocationSize = memReqs.size;

			memAllocInfo.memoryTypeIndex = core::find_memory_index(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
			vkAllocateMemory(core::logical_device(), &memAllocInfo, nullptr, &deviceMemory);
			vkBindImageMemory(core::logical_device(), tex.image, deviceMemory, 0);

			VkImageSubresourceRange subresourceRange = {};
			subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			subresourceRange.baseMipLevel = 0;
			subresourceRange.levelCount = mipLevels;
			subresourceRange.layerCount = 1;

			// Image barrier for optimal image (target)
			// Optimal image will be used as destination for the copy
			setImageLayout(
				copyCmd,
				tex.image,
				VK_IMAGE_LAYOUT_UNDEFINED,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				subresourceRange);

			// Copy mip levels from staging buffer
			vkCmdCopyBufferToImage(
				copyCmd,
				stagingBuffer,
				tex.image,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				1,
				&bufferCopyRegion
			);

			// Change texture image layout to shader read after all mip levels have been copied
			//this->imageLayout = imageLayout;
			setImageLayout(
				copyCmd,
				tex.image,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				imageLayout,
				subresourceRange);

			flushCommandBuffer(copyCmd, pool);

			// Clean up staging resources
			vkFreeMemory(core::logical_device(), stagingMemory, nullptr);
			vkDestroyBuffer(core::logical_device(), stagingBuffer, nullptr);

			// Create sampler
			VkSamplerCreateInfo samplerCreateInfo = {};
			samplerCreateInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
			samplerCreateInfo.magFilter = filter;
			samplerCreateInfo.minFilter = filter;
			samplerCreateInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
			samplerCreateInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
			samplerCreateInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
			samplerCreateInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
			samplerCreateInfo.mipLodBias = 0.0f;
			samplerCreateInfo.compareOp = VK_COMPARE_OP_NEVER;
			samplerCreateInfo.minLod = 0.0f;
			samplerCreateInfo.maxLod = 0.0f;
			samplerCreateInfo.maxAnisotropy = 1.0f;
			vkCreateSampler(core::logical_device(), &samplerCreateInfo, nullptr, &tex.sampler);

			// Create image view
			VkImageViewCreateInfo viewCreateInfo = {};
			viewCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
			viewCreateInfo.pNext = NULL;
			viewCreateInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
			viewCreateInfo.format = format;
			viewCreateInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
			viewCreateInfo.subresourceRange.levelCount = 1;
			viewCreateInfo.image = tex.image;
			vkCreateImageView(core::logical_device(), &viewCreateInfo, nullptr, &tex.view);

			// Update descriptor image info member that can be used for setting up descriptor sets
			//updateDescriptor();
		}

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
			image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT; // ! VK_IMAGE_USAGE_SAMPLED_BIT use to sample directly from the depth attachment for shadow mapping
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


	vulkan_shadowmapping::~vulkan_shadowmapping()
	{
		textures::remove(_image_id);

		vkDestroyFramebuffer(core::logical_device(), _framebuffer.framebuffer, nullptr);

		vkDestroyRenderPass(core::logical_device(),_renderpass.render_pass, nullptr);

		pipeline::remove_pipeline(_pipeline_id);

		vkDestroyBuffer(core::logical_device(), _uniformbuffer.buffer, nullptr);
		vkFreeMemory(core::logical_device(), _uniformbuffer.memory, nullptr);
	}

	void vulkan_shadowmapping::createUniformBuffer()
	{
		VkDeviceSize bufferSize = sizeof(UniformBufferObject);

		createBuffer(core::logical_device(), bufferSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, _uniformbuffer.buffer, _uniformbuffer.memory);

		//vkBindBufferMemory(core::logical_device(), _uniformBuffer.buffer, _uniformBuffer.memory, 0);

		vkMapMemory(core::logical_device(), _uniformbuffer.memory, 0, bufferSize, 0, &_uniformbuffer.mapped);


		game_entity::entity entity1{ create_one_game_entity({0.f, 5.f, 0.f}, {1.5f, 0.f, -0.5f})}; // {1.5f, 0.f, -0.5f}
		graphics::camera cam{ graphics::create_camera(graphics::perspective_camera_init_info{ entity1.get_id() }) };
		cam.field_of_view(0.75f);
		cam.aspect_ratio(1.0f);
		camera::get(cam.get_id()).update();
		_lightPos = entity1.position();
		//add_camera({ 128, graphics::camera::type::perspective, math::v3{0, 1, 0}, 1280, 720, 0.01, 10000 });
		DirectX::XMMATRIX modelMatrix = DirectX::XMMatrixIdentity();
		//modelMatrix = DirectX::XMMatrixMultiply(DirectX::XMMatrixRotationY(DirectX::XM_PIDIV2), modelMatrix);
		DirectX::XMStoreFloat4x4(&_ubo.model, modelMatrix);
		DirectX::XMMATRIX viewMatrix = DirectX::XMMatrixLookAtLH({ 0.f, 3.f, 0.f }, { 0.f, 0.f, 0.f }, { 1.f, 0.f, 0.f });
		//DirectX::XMStoreFloat4x4(&_ubo.view, cam.view());
		_ubo.view = cam.view();
		DirectX::XMMATRIX projectionMatrix = DirectX::XMMatrixPerspectiveFovLH(DirectX::XM_PIDIV4, (f32)_shadowmapping_dim / (f32)_shadowmapping_dim, _near, _far);
		//DirectX::XMStoreFloat4x4(&_ubo.projection, cam.projection());
		_ubo.projection = cam.projection();
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

		_renderpass.clear_color = { 0.0f, 0.0f, 0.0f, 1.0f };
		_renderpass.render_area = { 0, 0, _shadowmapping_dim, _shadowmapping_dim };
		_renderpass.depth = 1.f;
		_renderpass.stencil = 0;
		VkResult result{ VK_SUCCESS };
		VkCall(result = vkCreateRenderPass(core::logical_device(), &info, nullptr, &_renderpass.render_pass), "Failed to create GBuffer(shadow mapping) renderpass...");
	}

	void vulkan_shadowmapping::setupFramebuffer()
	{
		vulkan_texture tex;

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
		image.flags = 0;
		image.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT; // ! VK_IMAGE_USAGE_SAMPLED_BIT use to sample directly from the depth attachment for shadow mapping
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
		imageView.format = VK_FORMAT_D16_UNORM;
		imageView.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
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

		_image_id = textures::add(tex);

		setupRenderpass();

		// Create frame buffer
		VkFramebufferCreateInfo framebuffer{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
		framebuffer.pNext = nullptr;
		framebuffer.renderPass = _renderpass.render_pass;
		framebuffer.attachmentCount = 1;
		framebuffer.pAttachments = &(textures::get_texture(_image_id).getTexture().view);
		framebuffer.width = _shadowmapping_dim;
		framebuffer.height = _shadowmapping_dim;
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

		_descriptorSet_id =  descriptor::add(allocInfo);

		VkDescriptorBufferInfo bufferInfo;
		bufferInfo.buffer = _uniformbuffer.buffer;
		bufferInfo.offset = 0;
		bufferInfo.range = sizeof(UniformBufferObject);

		VkDescriptorSet set = descriptor::get(_descriptorSet_id);

		std::vector<VkWriteDescriptorSet> descriptorWrites = {
			descriptor::setWriteDescriptorSet(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, set, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &bufferInfo),
		};

		vkUpdateDescriptorSets(core::logical_device(), static_cast<u32>(descriptorWrites.size()), descriptorWrites.data(), 0, nullptr);
	}

	void vulkan_shadowmapping::setupPipeline(VkPipelineLayout layout)
	{
		VkPipelineInputAssemblyStateCreateInfo inputAssemblyState = descriptor::pipelineInputAssemblyStateCreate(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, VK_FALSE);
		VkPipelineViewportStateCreateInfo viewportState = descriptor::pipelineViewportStateCreate(1, 1);
		VkPipelineRasterizationStateCreateInfo rasterizationState = descriptor::pipelineRasterizationStateCreate(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, 0);
		rasterizationState.depthBiasEnable = VK_TRUE;
		VkPipelineMultisampleStateCreateInfo multisampleState = descriptor::pipelineMultisampleStateCreate(VK_SAMPLE_COUNT_1_BIT);
		VkPipelineColorBlendAttachmentState blendAttachmentState = descriptor::pipelineColorBlendAttachmentState(0xf, VK_FALSE);
		VkPipelineColorBlendStateCreateInfo colorBlendState = descriptor::pipelineColorBlendStateCreate(0, blendAttachmentState);

		std::vector<VkDynamicState> dynamicStateEnables{ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS };
		VkPipelineDynamicStateCreateInfo dynamicState = descriptor::pipelineDynamicStateCreate(dynamicStateEnables);
		VkPipelineDepthStencilStateCreateInfo depthStencilState = descriptor::pipelineDepthStencilStateCreateInfo(VK_TRUE, VK_TRUE, VK_FALSE, VK_FALSE, VK_COMPARE_OP_LESS_OR_EQUAL);

		auto bind = getVertexInputBindDescriptor();
		auto attr = getVertexInputAttributeDescriptor();

		VkPipelineVertexInputStateCreateInfo vertexInputInfo;
		vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
		vertexInputInfo.pNext = nullptr;
		vertexInputInfo.flags = 0;
		vertexInputInfo.vertexBindingDescriptionCount = static_cast<u32>(bind.size());
		vertexInputInfo.pVertexBindingDescriptions = bind.data();
		vertexInputInfo.vertexAttributeDescriptionCount = static_cast<u32>(attr.size());
		vertexInputInfo.pVertexAttributeDescriptions = attr.data();

		_shader.loadFile(std::string{ "C:/Users/zy/Desktop/PrimalMerge/PrimalEngine/Engine/Graphics/Vulkan/Shaders/shadowmapping.vert.spv" }, shader_type::vertex);
		auto stage = _shader.getShaderStage();

		rasterizationState.depthBiasEnable = VK_TRUE;

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
		pipelineCI.pColorBlendState = nullptr;
		pipelineCI.pDynamicState = &dynamicState;
		pipelineCI.layout = layout;
		pipelineCI.renderPass = _renderpass.render_pass;
		pipelineCI.subpass = 0;
		pipelineCI.basePipelineHandle = VK_NULL_HANDLE;
		pipelineCI.basePipelineIndex = -1;

		_pipeline_id = pipeline::add_pipeline(pipelineCI);

		_shader.~vulkan_shader();
	}

	void vulkan_shadowmapping::runRenderpass(vulkan_cmd_buffer cmd_buffer, vulkan_surface* surface)
	{
		VkClearValue clearValue;
		//clearValue.color = { 0.f, 0.f, 0.f, 0.f };
		clearValue.depthStencil = { 1.f, 0 };

		VkRenderPassBeginInfo info{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
		info.pNext = nullptr;
		info.renderPass = _renderpass.render_pass;
		info.framebuffer = _framebuffer.framebuffer;
		info.renderArea.extent.width = _shadowmapping_dim;
		info.renderArea.extent.height = _shadowmapping_dim;
		info.clearValueCount = 1;
		info.pClearValues = &clearValue;

		VkViewport viewport;
		viewport.x = 0.0f;
		viewport.y = 0.0f;
		viewport.width = (f32)_shadowmapping_dim;
		viewport.height = (f32)_shadowmapping_dim;
		viewport.minDepth = 0.f;
		viewport.maxDepth = 1.f;
		vkCmdSetViewport(cmd_buffer.cmd_buffer, 0, 1, &viewport);

		VkRect2D scissor;
		scissor.extent.height = _shadowmapping_dim;
		scissor.extent.width = _shadowmapping_dim;
		scissor.offset.x = 0;
		scissor.offset.y = 0;
		vkCmdSetScissor(cmd_buffer.cmd_buffer, 0, 1, &scissor);

		vkCmdBeginRenderPass(cmd_buffer.cmd_buffer, &info, VK_SUBPASS_CONTENTS_INLINE);

		vkCmdSetDepthBias(cmd_buffer.cmd_buffer, 0.25f, 0.f, 1.25f);

		auto set = descriptor::get(_descriptorSet_id);
		vkCmdBindDescriptorSets(cmd_buffer.cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, surface->layout_and_pool().pipelineLayout, 0, 1, &set, 0, nullptr);
		auto pipeline = pipeline::get_pipeline(_pipeline_id);
		vkCmdBindPipeline(cmd_buffer.cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

		surface->getScene().drawGBuffer(cmd_buffer);

		vkCmdEndRenderPass(cmd_buffer.cmd_buffer);
	}

	vulkan_texture vulkan_shadowmapping::getTexture()
	{
		return textures::get_texture(_image_id).getTexture();
	}

	vulkan_offscreen::~vulkan_offscreen()
	{
	}

	void vulkan_offscreen::createUniformBuffer()
	{
		VkDeviceSize bufferSize = sizeof(uboGBuffer);

		createBuffer(core::logical_device(), bufferSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, _ub.buffer, _ub.memory);

		vkMapMemory(core::logical_device(), _ub.memory, 0, bufferSize, 0, &_ub.mapped);

		game_entity::entity entity1{ create_one_game_entity({ 0.f, 5.f, 0.f }, { 1.5f, 0.f, -0.5f })}; // {1.5f, 0.f, -0.5f}
		graphics::camera cam{ graphics::create_camera(graphics::perspective_camera_init_info{ entity1.get_id() }) };
		cam.field_of_view(0.75f);
		cam.aspect_ratio(1.0f);
		camera::get(cam.get_id()).update();
		//add_camera({ 128, graphics::camera::type::perspective, math::v3{0, 1, 0}, 1280, 720, 0.01, 10000 });
		DirectX::XMMATRIX modelMatrix = DirectX::XMMatrixIdentity();
		//modelMatrix = DirectX::XMMatrixMultiply(DirectX::XMMatrixRotationY(DirectX::XM_PIDIV2), modelMatrix);
		DirectX::XMStoreFloat4x4(&_ubo.model, modelMatrix);
		DirectX::XMMATRIX viewMatrix = DirectX::XMMatrixLookAtLH({ 0.f, 3.f, 0.f }, { 0.f, 0.f, 0.f }, { 1.f, 0.f, 0.f });
		//DirectX::XMStoreFloat4x4(&_ubo.view, cam.view());
		_ubo.view = cam.view();
		DirectX::XMMATRIX projectionMatrix = DirectX::XMMatrixPerspectiveFovLH(DirectX::XM_PIDIV4, (f32)_width / (f32)_height, 0.1f, 64.f);
		//DirectX::XMStoreFloat4x4(&_ubo.projection, cam.projection());
		_ubo.projection = cam.projection();
		_ubo.nearPlane = 0.1f;
		_ubo.farPlane = 64.0f;
		memcpy(_ub.mapped, &_ubo, sizeof(uboGBuffer));

		bufferSize = sizeof(uboSSAOParam);

		createBuffer(core::logical_device(), bufferSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, _ssaoParams_ub.buffer, _ssaoParams_ub.memory);

		vkMapMemory(core::logical_device(), _ssaoParams_ub.memory, 0, bufferSize, 0, &_ssaoParams_ub.mapped);

		_ssaoParams_ubo.projection = cam.projection();
		memcpy(_ssaoParams_ub.mapped, &_ssaoParams_ubo, sizeof(uboSSAOParam));

		std::default_random_engine rndEngine((unsigned)time(nullptr));
		std::uniform_real_distribution<float> rndDist(0.f, 1.f);

		std::vector<math::v4> ssaoKernel(64);
		for (u32 i{ 0 }; i < 64; ++i)
		{
			math::v3 sample;
			DirectX::XMVECTOR sampleV = DirectX::XMVectorSet(rndDist(rndEngine) * 2.0f - 1.0f, rndDist(rndEngine) * 2.0f - 1.0f, rndDist(rndEngine), 0.f);
			DirectX::XMVector3Normalize(sampleV);
			DirectX::XMStoreFloat3(&sample, sampleV);
			sample.x *= rndDist(rndEngine);
			sample.y *= rndDist(rndEngine);
			sample.z *= rndDist(rndEngine);
			float scale = float(i) / float(64);
			scale = lerp(0.1f, 1.0f, scale * scale);
			ssaoKernel[i] = math::v4{ sample.x * scale, sample.y * scale, sample.z * scale, 0.f };
		}

		bufferSize = ssaoKernel.size() * sizeof(math::v4);

		createBuffer(core::logical_device(), bufferSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, _ssaoKernel_ub.buffer, _ssaoKernel_ub.memory);

		vkMapMemory(core::logical_device(), _ssaoKernel_ub.memory, 0, bufferSize, 0, &_ssaoKernel_ub.mapped);

		memcpy(_ssaoKernel_ub.mapped, ssaoKernel.data(), bufferSize);

		std::vector<math::v4>	ssaoNoise(4 * 4);
		for (u32 i{ 0 }; i < static_cast<u32>(ssaoNoise.size()); ++i)
		{
			ssaoNoise[i] = math::v4{ rndDist(rndEngine) * 2.f - 1.f, rndDist(rndEngine) * 2.f - 1.f, 0.f, 0.f };
		}

		vulkan_texture tex;
		imageFromBuffer(ssaoNoise.data(), ssaoNoise.size() * sizeof(math::v4), VK_FORMAT_R32G32B32A32_SFLOAT, 4, 4, core::get_current_command_pool(), VK_FILTER_NEAREST, VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, tex);

		_image_ids.resize(6);
		_image_ids[5] = textures::add(tex);
	}

	void vulkan_offscreen::updateUniformBuffer()
	{

	}

	void vulkan_offscreen::setupRenderpass()
	{
		_renderpasses.resize(2);

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

		VkResult result{ VK_SUCCESS };
		VkCall(result = vkCreateRenderPass(core::logical_device(), &renderPassInfo, nullptr, &_renderpasses[0].render_pass), "Failed to create GBuffert(offscreen) renderpass...");

		{
			// SSAO
			vulkan_texture tex;

			VkImageCreateInfo image{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
			image.imageType = VK_IMAGE_TYPE_2D;
			image.extent.height = _height;
			image.extent.width = _width;
			image.extent.depth = 1;
			image.mipLevels = 1;
			image.arrayLayers = 1;
			image.samples = VK_SAMPLE_COUNT_1_BIT;
			image.tiling = VK_IMAGE_TILING_OPTIMAL;
			image.format = VK_FORMAT_R8_UNORM;
			image.flags = 0;
			image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT; // ! VK_IMAGE_USAGE_SAMPLED_BIT use to sample directly from the depth attachment for shadow mapping
			result =  VK_SUCCESS ;
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
			imageView.format = VK_FORMAT_R8_UNORM;
			imageView.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
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

			_image_ids.emplace_back(textures::add(tex));
		}
		VkAttachmentDescription	attachmentDescs2;
		attachmentDescs2.flags = 0;
		attachmentDescs2.format = VK_FORMAT_R8_UNORM;
		attachmentDescs2.samples = VK_SAMPLE_COUNT_1_BIT;
		attachmentDescs2.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		attachmentDescs2.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		attachmentDescs2.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachmentDescs2.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachmentDescs2.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		attachmentDescs2.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

		VkAttachmentReference colorReference2{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };

		VkSubpassDescription subpass2;
		subpass2.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass2.flags = 0;
		subpass2.pColorAttachments = &colorReference2;
		subpass2.colorAttachmentCount = 1;
		subpass2.pDepthStencilAttachment = nullptr;
		subpass2.inputAttachmentCount = 0;
		subpass2.pInputAttachments = nullptr;
		subpass2.pPreserveAttachments = nullptr;
		subpass2.preserveAttachmentCount = 0;
		subpass2.pResolveAttachments = nullptr;

		utl::vector<VkSubpassDependency> dependencies2;
		dependencies2.resize(2);

		dependencies2[0].srcSubpass = VK_SUBPASS_EXTERNAL;
		dependencies2[0].dstSubpass = 0;
		dependencies2[0].srcStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
		dependencies2[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependencies2[0].srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
		dependencies2[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		dependencies2[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

		dependencies2[1].srcSubpass = 0;
		dependencies2[1].dstSubpass = VK_SUBPASS_EXTERNAL;
		dependencies2[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependencies2[1].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
		dependencies2[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		dependencies2[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
		dependencies2[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

		VkRenderPassCreateInfo renderPassInfo2;
		renderPassInfo2.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
		renderPassInfo2.pNext = nullptr;
		renderPassInfo2.flags = 0;
		renderPassInfo2.pAttachments = &attachmentDescs2;
		renderPassInfo2.attachmentCount = 1;
		renderPassInfo2.subpassCount = 1;
		renderPassInfo2.pSubpasses = &subpass2;
		renderPassInfo2.dependencyCount = 2;
		renderPassInfo2.pDependencies = dependencies2.data();

		result = VK_SUCCESS;
		VkCall(result = vkCreateRenderPass(core::logical_device(), &renderPassInfo2, nullptr, &_renderpasses[1].render_pass), "Failed to create GBuffert(SSAO) renderpass...");
	}

	void vulkan_offscreen::setupFramebuffer()
	{
		_framebuffers.resize(2);
		setupRenderpass();

		utl::vector<VkImageView> attachments;
		attachments.resize(4);
		attachments[0] = textures::get_texture(_image_ids[0]).getTexture().view;
		attachments[1] = textures::get_texture(_image_ids[1]).getTexture().view;
		attachments[2] = textures::get_texture(_image_ids[2]).getTexture().view;
		attachments[3] = textures::get_texture(_image_ids[3]).getTexture().view;

		// Create frame buffer
		VkFramebufferCreateInfo framebuffer{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
		framebuffer.pNext = nullptr;
		framebuffer.renderPass = _renderpasses[0].render_pass;
		framebuffer.attachmentCount = static_cast<u32>(attachments.size());
		framebuffer.pAttachments = attachments.data();
		framebuffer.width = _width;
		framebuffer.height = _height;
		framebuffer.layers = 1;
		VkResult result{ VK_SUCCESS };
		VkCall(result = vkCreateFramebuffer(core::logical_device(), &framebuffer, nullptr, &_framebuffers[0].framebuffer), "Failed to create GBuffer(offscreen) framebuffer...");

		VkImageView attachments2 = textures::get_texture(_image_ids[4]).getTexture().view;
		VkFramebufferCreateInfo framebuffer2{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
		framebuffer2.pNext = nullptr;
		framebuffer2.renderPass = _renderpasses[1].render_pass;
		framebuffer2.attachmentCount = 1;
		framebuffer2.pAttachments = &attachments2;
		framebuffer2.width = _width;
		framebuffer2.height = _height;
		framebuffer2.layers = 1;
		result = VK_SUCCESS ;
		VkCall(result = vkCreateFramebuffer(core::logical_device(), &framebuffer2, nullptr, &_framebuffers[1].framebuffer), "Failed to create GBuffer(offscreen) framebuffer...");

	}

	void vulkan_offscreen::setupDescriptorSets(VkDescriptorPool pool, id::id_type id)
	{
		VkDescriptorSetLayout setLayout;
		utl::vector<VkDescriptorSetLayoutBinding>	bindings;
		bindings.emplace_back(descriptor::descriptorSetLayoutBinding(0, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER));
		
		VkDescriptorSetLayoutCreateInfo setLayoutInfo;
		setLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		setLayoutInfo.pNext = nullptr;
		setLayoutInfo.flags = 0;
		setLayoutInfo.bindingCount = static_cast<u32>(bindings.size());
		setLayoutInfo.pBindings = bindings.data();
		VkResult result{ VK_SUCCESS };
		VkCall(result = vkCreateDescriptorSetLayout(core::logical_device(), &setLayoutInfo, nullptr, &setLayout), "Failed to create GBuffer(offscreen) descriptor set layout...");
		VkDescriptorSetLayout colorSetLayout;
		utl::vector<VkDescriptorSetLayoutBinding>	colorBindings;
		colorBindings.emplace_back(descriptor::descriptorSetLayoutBinding(0, VK_SHADER_STAGE_FRAGMENT_BIT, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER));
		VkDescriptorSetLayoutCreateInfo colorSetLayoutInfo;
		colorSetLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		colorSetLayoutInfo.pNext = nullptr;
		colorSetLayoutInfo.flags = 0;
		colorSetLayoutInfo.bindingCount = static_cast<u32>(colorBindings.size());
		colorSetLayoutInfo.pBindings = colorBindings.data();
		result = VK_SUCCESS;
		VkCall(result = vkCreateDescriptorSetLayout(core::logical_device(), &colorSetLayoutInfo, nullptr, &colorSetLayout), "Failed to create GBuffer(offscreen) descriptor set layout...");
		_descriptorSet_layout_ids.emplace_back(descriptor::add_layout(colorSetLayout));
		std::vector<VkDescriptorSetLayout> setLayouts{ setLayout, colorSetLayout };
		VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(InstanceData)};
		VkPipelineLayout pipelineLayout;
		VkPipelineLayoutCreateInfo pipLayoutInfo;
		pipLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		pipLayoutInfo.pNext = nullptr;
		pipLayoutInfo.flags = 0;
		pipLayoutInfo.setLayoutCount = 2;
		pipLayoutInfo.pSetLayouts = setLayouts.data();
		pipLayoutInfo.pushConstantRangeCount = 1;
		pipLayoutInfo.pPushConstantRanges = &push;
		result = VK_SUCCESS;
		VkCall(result = vkCreatePipelineLayout(core::logical_device(), &pipLayoutInfo, nullptr, &pipelineLayout), "Failed to create GBuffer(offscreen) pipeline layout...");
		//_pipeline_layout_ids.emplace_back(pipeline::add_layout(pipelineLayout));
		_pipeline_layout_ids.emplace_back(create_data(engine_vulkan_data::data_type::vulkan_pipeline_layout, static_cast<void*>(&pipLayoutInfo), 0));

		VkDescriptorSet set;
		VkDescriptorSetAllocateInfo descriptorSetInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
		descriptorSetInfo.pNext = nullptr;
		descriptorSetInfo.descriptorPool = pool;
		descriptorSetInfo.descriptorSetCount = 1;
		descriptorSetInfo.pSetLayouts = &setLayout;
		result = VK_SUCCESS;
		VkCall(result = vkAllocateDescriptorSets(core::logical_device(), &descriptorSetInfo, &set), "Failed to alloc GBuffer(offscreen) descriptor sets...");
		utl::vector<VkWriteDescriptorSet> writes;
		VkDescriptorBufferInfo ubInfo{ _ub.buffer, 0, sizeof(uboGBuffer) };
		writes.emplace_back(descriptor::setWriteDescriptorSet(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, set, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &ubInfo));
		vkUpdateDescriptorSets(core::logical_device(), static_cast<u32>(writes.size()), writes.data(), 0, nullptr);
		_descriptorSet_id = descriptor::add(set);

		VkDescriptorSet set3;
		VkDescriptorSetAllocateInfo descriptorSetInfo3{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
		descriptorSetInfo3.pNext = nullptr;
		descriptorSetInfo3.descriptorPool = pool;
		descriptorSetInfo3.descriptorSetCount = 1;
		descriptorSetInfo3.pSetLayouts = &colorSetLayout;
		result = VK_SUCCESS;
		VkCall(result = vkAllocateDescriptorSets(core::logical_device(), &descriptorSetInfo3, &set3), "Failed to alloc GBuffer(offscreen) descriptor sets...");
		VkWriteDescriptorSet writes3;
		VkDescriptorImageInfo imgInfo4{ textures::get_texture(id).getTexture().sampler, textures::get_texture(id).getTexture().view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
		writes3 = descriptor::setWriteDescriptorSet(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, set3, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &imgInfo4);
		vkUpdateDescriptorSets(core::logical_device(), 1, &writes3, 0, nullptr);
		_descriptorSet_color_id = descriptor::add(set3);

		VkDescriptorSetLayout setLayout2;
		utl::vector<VkDescriptorSetLayoutBinding>	bindings2;
		bindings2.emplace_back(descriptor::descriptorSetLayoutBinding(0, VK_SHADER_STAGE_FRAGMENT_BIT, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER));
		bindings2.emplace_back(descriptor::descriptorSetLayoutBinding(1, VK_SHADER_STAGE_FRAGMENT_BIT, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER));
		bindings2.emplace_back(descriptor::descriptorSetLayoutBinding(2, VK_SHADER_STAGE_FRAGMENT_BIT, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER));
		bindings2.emplace_back(descriptor::descriptorSetLayoutBinding(3, VK_SHADER_STAGE_FRAGMENT_BIT, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER));
		bindings2.emplace_back(descriptor::descriptorSetLayoutBinding(4, VK_SHADER_STAGE_FRAGMENT_BIT, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER));
		VkDescriptorSetLayoutCreateInfo setLayoutInfo2;
		setLayoutInfo2.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		setLayoutInfo2.pNext = nullptr;
		setLayoutInfo2.flags = 0;
		setLayoutInfo2.bindingCount = static_cast<u32>(bindings2.size());
		setLayoutInfo2.pBindings = bindings2.data();
		result = VK_SUCCESS ;
		VkCall(result = vkCreateDescriptorSetLayout(core::logical_device(), &setLayoutInfo2, nullptr, &setLayout2), "Failed to create GBuffer(offscreen) descriptor set layout...");
		_descriptorSet_layout_ids.emplace_back(descriptor::add_layout(setLayout2));
		VkPipelineLayout pipelineLayout2;
		VkPipelineLayoutCreateInfo pipLayoutInfo2;
		pipLayoutInfo2.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		pipLayoutInfo2.pNext = nullptr;
		pipLayoutInfo2.flags = 0;
		pipLayoutInfo2.setLayoutCount = 1;
		pipLayoutInfo2.pSetLayouts = &setLayout2;
		pipLayoutInfo2.pushConstantRangeCount = 0;
		pipLayoutInfo2.pPushConstantRanges = nullptr;
		result = VK_SUCCESS;
		VkCall(result = vkCreatePipelineLayout(core::logical_device(), &pipLayoutInfo2, nullptr, &pipelineLayout2), "Failed to create GBuffer(offscreen) pipeline layout...");
		_pipeline_layout_ids.emplace_back(pipeline::add_layout(pipelineLayout2));

		VkDescriptorSet set2;
		VkDescriptorSetAllocateInfo descriptorSetInfo2{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
		descriptorSetInfo2.pNext = nullptr;
		descriptorSetInfo2.descriptorPool = pool;
		descriptorSetInfo2.descriptorSetCount = 1;
		descriptorSetInfo2.pSetLayouts = &setLayout2;
		result = VK_SUCCESS;
		VkCall(result = vkAllocateDescriptorSets(core::logical_device(), &descriptorSetInfo2, &set2), "Failed to alloc GBuffer(offscreen) descriptor sets...");
		utl::vector<VkWriteDescriptorSet> writes2;
		VkDescriptorImageInfo imgInfo1{ textures::get_texture(_image_ids[0]).getTexture().sampler, textures::get_texture(_image_ids[0]).getTexture().view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
		VkDescriptorImageInfo imgInfo2{ textures::get_texture(_image_ids[1]).getTexture().sampler, textures::get_texture(_image_ids[1]).getTexture().view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
		VkDescriptorImageInfo imgInfo3{ textures::get_texture(_image_ids[5]).getTexture().sampler, textures::get_texture(_image_ids[5]).getTexture().view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
		VkDescriptorBufferInfo ubInfo1{ _ssaoKernel_ub.buffer, 0, sizeof(math::v4) * 64 };
		VkDescriptorBufferInfo ubInfo2{ _ssaoParams_ub.buffer, 0, sizeof(u32) * 3 + sizeof(math::m4x4)};
		writes2.emplace_back(descriptor::setWriteDescriptorSet(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, set2, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &imgInfo1));
		writes2.emplace_back(descriptor::setWriteDescriptorSet(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, set2, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &imgInfo2));
		writes2.emplace_back(descriptor::setWriteDescriptorSet(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, set2, 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &imgInfo3));
		writes2.emplace_back(descriptor::setWriteDescriptorSet(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, set2, 3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &ubInfo1));
		writes2.emplace_back(descriptor::setWriteDescriptorSet(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, set2, 4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &ubInfo2));
		vkUpdateDescriptorSets(core::logical_device(), static_cast<u32>(writes2.size()), writes2.data(), 0, nullptr);
		_ssao_descriptorSet_id = descriptor::add(set2);
	}

	void vulkan_offscreen::setupPipeline()
	{
		PE_vk_pipeline_input_assemble_info inputAssemble;
		VkPipelineRasterizationStateCreateInfo rasterizationState = descriptor::pipelineRasterizationStateCreate(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE);
		std::vector<VkPipelineColorBlendAttachmentState> blendAttachmentState = { descriptor::pipelineColorBlendAttachmentState(0xf, VK_FALSE), descriptor::pipelineColorBlendAttachmentState(0xf, VK_FALSE), descriptor::pipelineColorBlendAttachmentState(0xf, VK_FALSE) };
		VkPipelineColorBlendStateCreateInfo colorBlendState = descriptor::pipelineColorBlendStateCreate(static_cast<u32>(blendAttachmentState.size()), *blendAttachmentState.data());
		VkPipelineDepthStencilStateCreateInfo depthStencilState = descriptor::pipelineDepthStencilStateCreateInfo(VK_TRUE, VK_TRUE, VK_FALSE, VK_FALSE, VK_COMPARE_OP_LESS_OR_EQUAL);
		VkPipelineViewportStateCreateInfo viewportState = descriptor::pipelineViewportStateCreate(1, 1, 0);
		VkPipelineMultisampleStateCreateInfo multisampleState = descriptor::pipelineMultisampleStateCreate();
		std::vector<VkDynamicState> dynamicStateEnables = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
		VkPipelineDynamicStateCreateInfo dynamicState = descriptor::pipelineDynamicStateCreate(dynamicStateEnables);
		std::array<VkPipelineShaderStageCreateInfo, 2> shaderStages;
		shaderStages[0] = shaders::get_shader(shaders::add("C:/Users/zy/Desktop/PrimalMerge/PrimalEngine/Engine/Graphics/Vulkan/Shaders/gbuffer.vert.spv", shader_type::vertex)).getShaderStage();
		shaderStages[1] = shaders::get_shader(shaders::add("C:/Users/zy/Desktop/PrimalMerge/PrimalEngine/Engine/Graphics/Vulkan/Shaders/gbuffer.frag.spv", shader_type::pixel)).getShaderStage();

		auto bind = getVertexInputBindDescriptor();
		auto attr = getVertexInputAttributeDescriptor();

		VkPipelineVertexInputStateCreateInfo vertexInputInfo;
		vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
		vertexInputInfo.pNext = nullptr;
		vertexInputInfo.flags = 0;
		vertexInputInfo.vertexBindingDescriptionCount = static_cast<u32>(bind.size());
		vertexInputInfo.pVertexBindingDescriptions = bind.data();
		vertexInputInfo.vertexAttributeDescriptionCount = static_cast<u32>(attr.size());
		vertexInputInfo.pVertexAttributeDescriptions = attr.data();

		VkGraphicsPipelineCreateInfo pipelineCreateInfo{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
		// pipelineCreateInfo.layout = pipeline::get_layout(_pipeline_layout_ids[0]);
		pipelineCreateInfo.layout = get_data<VkPipelineLayout>(_pipeline_layout_ids[0]);
		pipelineCreateInfo.pNext = nullptr;
		pipelineCreateInfo.renderPass = _renderpasses[0].render_pass;
		pipelineCreateInfo.flags = 0;
		pipelineCreateInfo.pInputAssemblyState = &inputAssemble.info;
		pipelineCreateInfo.pRasterizationState = &rasterizationState;
		pipelineCreateInfo.pColorBlendState = &colorBlendState;
		pipelineCreateInfo.pMultisampleState = &multisampleState;
		pipelineCreateInfo.pViewportState = &viewportState;
		pipelineCreateInfo.pDepthStencilState = &depthStencilState;
		pipelineCreateInfo.pDynamicState = &dynamicState;
		pipelineCreateInfo.stageCount = static_cast<uint32_t>(shaderStages.size());
		pipelineCreateInfo.pStages = shaderStages.data();
		pipelineCreateInfo.pVertexInputState = &vertexInputInfo;
		_pipeline_id = pipeline::add_pipeline(pipelineCreateInfo);

		// SSAO
		VkPipelineColorBlendAttachmentState blendAttachmentState2 = descriptor::pipelineColorBlendAttachmentState(0xf, VK_FALSE);
		VkPipelineColorBlendStateCreateInfo colorBlendState2 = descriptor::pipelineColorBlendStateCreate(1, blendAttachmentState2);
		pipelineCreateInfo.pColorBlendState = &colorBlendState2;
		VkPipelineVertexInputStateCreateInfo emptyVertexInputState{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO , nullptr, 0, 0, nullptr, 0, nullptr};
		pipelineCreateInfo.pVertexInputState = &emptyVertexInputState;
		shaderStages[0] = shaders::get_shader(shaders::add("C:/Users/zy/Desktop/PrimalMerge/PrimalEngine/Engine/Graphics/Vulkan/Shaders/fullscreen.vert.spv", shader_type::vertex)).getShaderStage();
		shaderStages[1] = shaders::get_shader(shaders::add("C:/Users/zy/Desktop/PrimalMerge/PrimalEngine/Engine/Graphics/Vulkan/Shaders/ssao.frag.spv", shader_type::pixel)).getShaderStage();
		pipelineCreateInfo.renderPass = _renderpasses[1].render_pass;
		pipelineCreateInfo.layout = pipeline::get_layout(_pipeline_layout_ids[1]);
		struct SpecializationData {
			uint32_t kernelSize = 64;
			float radius = 0.3f;
		} specializationData;
		std::array< VkSpecializationMapEntry, 2> specializationMapEntries;
		specializationMapEntries[0] = { 0, offsetof(SpecializationData, kernelSize), sizeof(SpecializationData::kernelSize) };
		specializationMapEntries[1] = { 1, offsetof(SpecializationData, radius), sizeof(SpecializationData::radius) };
		VkSpecializationInfo specialzationInfo{ 2, specializationMapEntries.data(), sizeof(SpecializationData), &specializationData };
		shaderStages[1].pSpecializationInfo = &specialzationInfo;

		_ssao_pipeline_id = pipeline::add_pipeline(pipelineCreateInfo);
	}

	void vulkan_offscreen::runRenderpass(vulkan_cmd_buffer cmd_buffer, vulkan_surface* surface)
	{
		// Clear values for all attachments written in the fragment shader
		std::vector<VkClearValue> clearValues(4);
		clearValues[0].color = { { 0.0f, 0.0f, 0.0f, 1.0f } };
		clearValues[1].color = { { 0.0f, 0.0f, 0.0f, 1.0f } };
		clearValues[2].color = { { 0.0f, 0.0f, 0.0f, 1.0f } };
		clearValues[3].depthStencil = { 1.0f, 0 };

		VkRenderPassBeginInfo info{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
		info.pNext = nullptr;
		info.renderPass = _renderpasses[0].render_pass;
		info.framebuffer = _framebuffers[0].framebuffer;
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

		auto set = descriptor::get(_descriptorSet_id);
		auto color_set = descriptor::get(_descriptorSet_color_id);
		auto pipelineLayout = get_data<VkPipelineLayout>(_pipeline_layout_ids[0]);
		//vkCmdPushConstants(cmd_buffer.cmd_buffer, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(InstanceData), 
		vkCmdBindDescriptorSets(cmd_buffer.cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &set, 0, nullptr);
		vkCmdBindDescriptorSets(cmd_buffer.cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 1, 1, &color_set, 0, nullptr);
		auto pipeline = pipeline::get_pipeline(_pipeline_id);
		vkCmdBindPipeline(cmd_buffer.cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
		//for (auto instance : surface->getScene().getInstance())
		//{
		//	auto iPipeline = pipeline::get_pipeline(scene::get_instance(instance).get_pipeline_ids());
		//	vkCmdBindPipeline(cmd_buffer.cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, iPipeline);
		//}

		surface->getScene().drawGBuffer(cmd_buffer);

		vkCmdEndRenderPass(cmd_buffer.cmd_buffer);

		clearValues.clear();
		clearValues.resize(2);
		clearValues[0].color = { {0.0f, 0.0f, 0.0f, 1.0f} };
		clearValues[1].depthStencil = { 1.f, 0 };
		info.pNext = nullptr;
		info.renderPass = _renderpasses[1].render_pass;
		info.framebuffer = _framebuffers[1].framebuffer;
		info.renderArea.extent.width = _width;
		info.renderArea.extent.height = _height;
		info.clearValueCount = 2;
		info.pClearValues = clearValues.data();

		viewport.x = 0.0f;
		viewport.y = 0.f;
		viewport.width = (f32)surface->width();
		viewport.height = (f32)surface->height();
		viewport.minDepth = 0.f;
		viewport.maxDepth = 1.f;
		vkCmdSetViewport(cmd_buffer.cmd_buffer, 0, 1, &viewport);

		scissor.extent.height = _height;
		scissor.extent.width = _width;
		scissor.offset.x = 0;
		scissor.offset.y = 0;
		vkCmdSetScissor(cmd_buffer.cmd_buffer, 0, 1, &scissor);

		vkCmdBeginRenderPass(cmd_buffer.cmd_buffer, &info, VK_SUBPASS_CONTENTS_INLINE);

		auto set2 = descriptor::get(_ssao_descriptorSet_id);
		auto pipelineLayout2 = pipeline::get_layout(_pipeline_layout_ids[1]);
		vkCmdBindDescriptorSets(cmd_buffer.cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout2, 0, 1, &set2, 0, nullptr);
		auto pipeline2 = pipeline::get_pipeline(_ssao_pipeline_id);
		vkCmdBindPipeline(cmd_buffer.cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline2);

		vkCmdDraw(cmd_buffer.cmd_buffer, 3, 1, 0, 0);

		vkCmdEndRenderPass(cmd_buffer.cmd_buffer);
	}
	
	void vulkan_geometry_pass::createUniformBuffer()
	{
		uboGBuffer ubo;
		game_entity::entity entity1{ create_one_game_entity({ 0.f, 5.f, 0.f }, { 1.5f, 0.f, -0.5f })};
		graphics::camera cam{ graphics::create_camera(graphics::perspective_camera_init_info{ entity1.get_id() }) };
		cam.field_of_view(0.75f);
		cam.aspect_ratio(1.0f);
		camera::get(cam.get_id()).update();
		DirectX::XMMATRIX modelMatrix = DirectX::XMMatrixIdentity();
		DirectX::XMStoreFloat4x4(&ubo.model, modelMatrix);
		DirectX::XMMATRIX viewMatrix = DirectX::XMMatrixLookAtLH({ 0.f, 3.f, 0.f }, { 0.f, 0.f, 0.f }, { 1.f, 0.f, 0.f });
		ubo.view = cam.view();
		DirectX::XMMATRIX projectionMatrix = DirectX::XMMatrixPerspectiveFovLH(DirectX::XM_PIDIV4, (f32)_width / (f32)_height, 0.1f, 64.f);
		ubo.projection = cam.projection();
		ubo.nearPlane = 0.1f;
		ubo.farPlane = 64.0f;
		_ub_id = create_data(engine_vulkan_data::vulkan_uniform_buffer, static_cast<void*>(&ubo), sizeof(uboGBuffer));
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

		_renderpass_id = create_data(engine_vulkan_data::vulkan_renderpass, static_cast<void*>(&renderPassInfo), 0);

		utl::vector<VkImageView> attachments;
		attachments.resize(4);
		attachments[0] = textures::get_texture(_image_ids[0]).getTexture().view;
		attachments[1] = textures::get_texture(_image_ids[1]).getTexture().view;
		attachments[2] = textures::get_texture(_image_ids[2]).getTexture().view;
		attachments[3] = textures::get_texture(_image_ids[3]).getTexture().view;

		// Create frame buffer
		VkFramebufferCreateInfo framebuffer{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
		framebuffer.pNext = nullptr;
		framebuffer.renderPass = get_data<VkRenderPass>(_renderpass_id);
		framebuffer.attachmentCount = static_cast<u32>(attachments.size());
		framebuffer.pAttachments = attachments.data();
		framebuffer.width = _width;
		framebuffer.height = _height;
		framebuffer.layers = 1;
		_framebuffer_id = create_data(engine_vulkan_data::vulkan_framebuffer, static_cast<void*>(&framebuffer), 0);
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
		info.renderPass = get_data<VkRenderPass>(_renderpass_id);
		info.framebuffer = get_data<VkFramebuffer>(_framebuffer_id);
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

		/*auto set = descriptor::get(_descriptorSet_id);
		auto color_set = descriptor::get(_descriptorSet_color_id);
		auto pipelineLayout = get_data<VkPipelineLayout>(_pipeline_layout_ids[0]);
		vkCmdBindDescriptorSets(cmd_buffer.cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &set, 0, nullptr);
		vkCmdBindDescriptorSets(cmd_buffer.cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 1, 1, &color_set, 0, nullptr);
		auto pipeline = pipeline::get_pipeline(_pipeline_id);
		vkCmdBindPipeline(cmd_buffer.cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);*/
		//for (auto instance : surface->getScene().getInstance())
		//{
		//	auto iPipeline = pipeline::get_pipeline(scene::get_instance(instance).get_pipeline_ids());
		//	vkCmdBindPipeline(cmd_buffer.cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, iPipeline);
		//}

		surface->getScene().drawGBuffer(cmd_buffer);

		vkCmdEndRenderPass(cmd_buffer.cmd_buffer);
	}

	void vulkan_final_pass::setupDescriptorSets(std::initializer_list<id::id_type> image_id)
	{
		std::vector<VkDescriptorPoolSize> poolSize = {
			descriptor::descriptorPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, static_cast<u32>(image_id.size())
		};

		VkDescriptorPoolCreateInfo poolInfo;
		poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		poolInfo.pNext = VK_NULL_HANDLE;
		poolInfo.flags = 0;
		poolInfo.poolSizeCount = static_cast<u32>(poolSize.size());
		poolInfo.pPoolSizes = poolSize.data();
		poolInfo.maxSets = static_cast<u32>(image_id.size());
		_descriptor_pool_id = create_data(engine_vulkan_data::vulkan_descriptor_pool, static_cast<void*>(&poolInfo), 0);

		std::vector<VkDescriptorSetLayoutBinding> setLayoutBindings;
		
		for (u32 i{ 0 }; i < image_id.size(); ++i)
		{
			setLayoutBindings.emplace_back(descriptor::descriptorSetLayoutBinding(i, VK_SHADER_STAGE_FRAGMENT_BIT, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER));
		}

		VkDescriptorSetLayoutCreateInfo descriptorLayout = descriptor::descriptorSetLayoutCreate(setLayoutBindings);

		_descriptorSet_layout_id = create_data(engine_vulkan_data::vulkan_descriptor_set_layout, static_cast<void*>(&descriptorLayout), 0);

		VkPipelineLayoutCreateInfo pipelineLayoutInfo;
		pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		pipelineLayoutInfo.pNext = nullptr;
		pipelineLayoutInfo.flags = 0;
		pipelineLayoutInfo.setLayoutCount = 1;
		pipelineLayoutInfo.pSetLayouts = &get_data<VkDescriptorSetLayout>(_descriptorSet_layout_id);
		pipelineLayoutInfo.pushConstantRangeCount = 0;
		pipelineLayoutInfo.pPushConstantRanges = VK_NULL_HANDLE;
		_pipeline_layout_id = create_data(engine_vulkan_data::vulkan_pipeline_layout, static_cast<void*>(&pipelineLayoutInfo), 0);

		VkDescriptorSetAllocateInfo allocInfo;

		allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		allocInfo.pNext = VK_NULL_HANDLE;
		allocInfo.descriptorPool = get_data<VkDescriptorPool>(_descriptor_pool_id);
		allocInfo.descriptorSetCount = 1;
		allocInfo.pSetLayouts = &get_data<VkDescriptorSetLayout>(_descriptorSet_layout_id);

		_descriptorSet_id = create_data(engine_vulkan_data::vulkan_descriptor_sets, static_cast<void*>(&allocInfo), 0);

		std::vector<VkWriteDescriptorSet> descriptorWrites;

		/*VkDescriptorImageInfo imageInfo1;
		imageInfo1.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		imageInfo1.imageView = textures::get_texture(_offscreen.getTexture()[0]).getTexture().view;
		imageInfo1.sampler = textures::get_texture(_offscreen.getTexture()[0]).getTexture().sampler;
		descriptorWrites.emplace_back(descriptor::setWriteDescriptorSet(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, get_data<VkDescriptorSet>(_descriptorSet_id), 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &imageInfo1));

		VkDescriptorImageInfo imageInfo2;
		imageInfo2.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		imageInfo2.imageView = textures::get_texture(_offscreen.getTexture()[1]).getTexture().view;
		imageInfo2.sampler = textures::get_texture(_offscreen.getTexture()[1]).getTexture().sampler;
		descriptorWrites.emplace_back(descriptor::setWriteDescriptorSet(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, get_data<VkDescriptorSet>(_descriptorSet_id), 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &imageInfo2));

		VkDescriptorImageInfo imageInfo3;
		imageInfo3.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		imageInfo3.imageView = textures::get_texture(_offscreen.getTexture()[2]).getTexture().view;
		imageInfo3.sampler = textures::get_texture(_offscreen.getTexture()[2]).getTexture().sampler;
		descriptorWrites.emplace_back(descriptor::setWriteDescriptorSet(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, get_data<VkDescriptorSet>(_descriptorSet_id), 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &imageInfo3));

		VkDescriptorImageInfo imageInfo4;
		imageInfo4.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		imageInfo4.imageView = textures::get_texture(_offscreen.getTexture()[4]).getTexture().view;
		imageInfo4.sampler = textures::get_texture(_offscreen.getTexture()[4]).getTexture().sampler;
		descriptorWrites.emplace_back(descriptor::setWriteDescriptorSet(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, get_data<VkDescriptorSet>(_descriptorSet_id), 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &imageInfo4));*/

		u32 count = 0;
		for (auto id : image_id)
		{
			VkDescriptorImageInfo imageInfo4;
			imageInfo4.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			imageInfo4.imageView = textures::get_texture(id).getTexture().view;
			imageInfo4.sampler = textures::get_texture(id).getTexture().sampler;
			descriptorWrites.emplace_back(descriptor::setWriteDescriptorSet(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, get_data<VkDescriptorSet>(_descriptorSet_id), count, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &imageInfo4));
			count++;
		}

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
		shaderStages[0] = shaders::get_shader(shaders::add("C:/Users/zy/Desktop/PrimalMerge/PrimalEngine/Engine/Graphics/Vulkan/Shaders/fullscreen.vert.spv", shader_type::vertex)).getShaderStage();
		shaderStages[1] = shaders::get_shader(shaders::add("C:/Users/zy/Desktop/PrimalMerge/PrimalEngine/Engine/Graphics/Vulkan/Shaders/composition.frag.spv", shader_type::pixel)).getShaderStage();

		VkGraphicsPipelineCreateInfo pipelineCreateInfo{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
		pipelineCreateInfo.layout = get_data<VkPipelineLayout>(_pipeline_layout_id);
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
		_pipeline_id = create_data(engine_vulkan_data::vulkan_pipeline, static_cast<void*>(&pipelineCreateInfo), 0);
	}

	void vulkan_final_pass::render(vulkan_cmd_buffer cmd_buffer)
	{
		auto descriptorSet = get_data<VkDescriptorSet>(_descriptorSet_id);
		auto pipeline = get_data<VkPipeline>(_pipeline_id);
		vkCmdBindDescriptorSets(cmd_buffer.cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, get_data<VkPipelineLayout>(_pipeline_layout_id), 0, 1, &descriptorSet, 0, nullptr);
		vkCmdBindPipeline(cmd_buffer.cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
		vkCmdDraw(cmd_buffer.cmd_buffer, 3, 1, 0, 0);
	}

}