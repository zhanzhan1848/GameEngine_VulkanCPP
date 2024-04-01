// Copyright (c) Contributors of Primal+
// Distributed under the MIT license. See the LICENSE file in the project root for more information.
#include "VulkanHelpers.h"
#include "VulkanCore.h"
#include "VulkanContent.h"

#include <iostream>
#include <fstream>

namespace primal::graphics::vulkan
{

    namespace descriptor
    {
		VkDescriptorPoolSize descriptorPoolSize(
			VkDescriptorType type,
			u32 descriptorCount)
		{
			VkDescriptorPoolSize descriptorPoolSize;
			descriptorPoolSize.type = type;
			descriptorPoolSize.descriptorCount = descriptorCount;
			return descriptorPoolSize;
		}

		VkWriteDescriptorSet setWriteDescriptorSet(VkStructureType type,
			VkDescriptorSet& set,
			u32 binding,
			VkDescriptorType dType,
			VkDescriptorBufferInfo* buffer)
		{
			VkWriteDescriptorSet descriptorWrite;
			descriptorWrite.sType = type;
			descriptorWrite.pNext = VK_NULL_HANDLE;
			descriptorWrite.dstSet = set;
			descriptorWrite.dstBinding = binding;
			descriptorWrite.dstArrayElement = 0;
			descriptorWrite.descriptorCount = 1;
			descriptorWrite.descriptorType = dType;
			descriptorWrite.pBufferInfo = buffer;
			descriptorWrite.pImageInfo = nullptr;
			descriptorWrite.pTexelBufferView = nullptr;
			return descriptorWrite;
		}

		VkWriteDescriptorSet setWriteDescriptorSet(VkStructureType type, VkDescriptorSet & set, u32 binding, VkDescriptorType dType, const VkDescriptorImageInfo* const image)
		{
			VkWriteDescriptorSet descriptorWrite;
			descriptorWrite.sType = type;
			descriptorWrite.pNext = VK_NULL_HANDLE;
			descriptorWrite.dstSet = set;
			descriptorWrite.dstBinding = binding;
			descriptorWrite.dstArrayElement = 0;
			descriptorWrite.descriptorCount = 1;
			descriptorWrite.descriptorType = dType;
			descriptorWrite.pBufferInfo = nullptr;
			descriptorWrite.pImageInfo = image;
			descriptorWrite.pTexelBufferView = nullptr;
			return descriptorWrite;
		}

		VkWriteDescriptorSet setWriteDescriptorSet(VkStructureType type, VkDescriptorSet & set, u32 binding, u32 count, VkDescriptorType dType, const VkDescriptorImageInfo* const image)
		{
			VkWriteDescriptorSet descriptorWrite;
			descriptorWrite.sType = type;
			descriptorWrite.pNext = VK_NULL_HANDLE;
			descriptorWrite.dstSet = set;
			descriptorWrite.dstBinding = binding;
			descriptorWrite.dstArrayElement = 0;
			descriptorWrite.descriptorCount = count;
			descriptorWrite.descriptorType = dType;
			descriptorWrite.pBufferInfo = nullptr;
			descriptorWrite.pImageInfo = image;
			descriptorWrite.pTexelBufferView = nullptr;
			return descriptorWrite;
		}

		VkWriteDescriptorSet setWriteDescriptorSet(VkStructureType type,
			utl::vector<VkDescriptorSet>& sets,
			u32 num,
			u32 binding,
			VkDescriptorType dType,
			VkDescriptorBufferInfo* buffer,
			VkDescriptorImageInfo * image)
		{
			VkWriteDescriptorSet descriptorWrite;
			descriptorWrite.sType = type;
			descriptorWrite.pNext = VK_NULL_HANDLE;
			descriptorWrite.dstSet = sets[num];
			descriptorWrite.dstBinding = binding;
			descriptorWrite.dstArrayElement = 0;
			descriptorWrite.descriptorCount = 1;
			descriptorWrite.descriptorType = dType;
			descriptorWrite.pBufferInfo = buffer;
			descriptorWrite.pImageInfo = image;
			descriptorWrite.pTexelBufferView = nullptr;
			return descriptorWrite;
		}

		VkDescriptorPoolCreateInfo descriptorPoolCreate(std::tuple<utl::vector<VkDescriptorPoolSize>, u32> info)
		{
			VkDescriptorPoolCreateInfo descriptorPoolInfo;
			descriptorPoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
			descriptorPoolInfo.pNext = nullptr;
			descriptorPoolInfo.flags = 0;
			descriptorPoolInfo.maxSets = std::get<1>(info);
			descriptorPoolInfo.poolSizeCount = static_cast<u32>(std::get<0>(info).size());
			descriptorPoolInfo.pPoolSizes = std::get<0>(info).data();
			return descriptorPoolInfo;
		}

		VkDescriptorSetAllocateInfo descriptorSetAllocate(
			VkDescriptorPool descriptorPool,
			const VkDescriptorSetLayout* pSetLayouts,
			u32 descriptorSetCount)
		{
			VkDescriptorSetAllocateInfo descriptorSetAllocateInfo;
			descriptorSetAllocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
			descriptorSetAllocateInfo.pNext = nullptr;
			descriptorSetAllocateInfo.descriptorPool = descriptorPool;
			descriptorSetAllocateInfo.descriptorSetCount = descriptorSetCount;
			descriptorSetAllocateInfo.pSetLayouts = pSetLayouts;
			return descriptorSetAllocateInfo;
		}

		VkWriteDescriptorSet writeDescriptorSets(VkDescriptorSet dstSet,
			VkDescriptorType type,
			u32 binding,
			VkDescriptorBufferInfo* bufferInfo,
			u32 descriptorCount = 1)
		{
			VkWriteDescriptorSet writeDescriptorSet;
			writeDescriptorSet.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			writeDescriptorSet.pNext = nullptr;
			writeDescriptorSet.dstSet = dstSet;
			writeDescriptorSet.dstBinding = binding;
			writeDescriptorSet.descriptorCount = descriptorCount;
			writeDescriptorSet.descriptorType = type;
			writeDescriptorSet.pImageInfo = nullptr;
			writeDescriptorSet.pBufferInfo = bufferInfo;
			writeDescriptorSet.pTexelBufferView = nullptr;
			return writeDescriptorSet;
		}

		VkDescriptorSetLayoutBinding descriptorSetLayoutBinding(
			u32 binding,
			VkShaderStageFlags stageFlags,
			VkDescriptorType type,
			VkSampler* sampler,
			u32 descriptorCount)
		{
			VkDescriptorSetLayoutBinding setLayoutBinding;
			setLayoutBinding.binding = binding;
			setLayoutBinding.descriptorType = type;
			setLayoutBinding.descriptorCount = descriptorCount;
			setLayoutBinding.stageFlags = stageFlags;
			setLayoutBinding.pImmutableSamplers = sampler;
			return setLayoutBinding;
		}

		VkDescriptorSetLayoutCreateInfo descriptorSetLayoutCreate(
			const VkDescriptorSetLayoutBinding* pBindings,
			u32 bindingCount)
		{
			VkDescriptorSetLayoutCreateInfo descriptorSetLayoutCreateInfo{};
			descriptorSetLayoutCreateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
			descriptorSetLayoutCreateInfo.pNext = nullptr;
			descriptorSetLayoutCreateInfo.flags = 0;
			descriptorSetLayoutCreateInfo.bindingCount = bindingCount;
			descriptorSetLayoutCreateInfo.pBindings = pBindings;
			return descriptorSetLayoutCreateInfo;
		}

		VkDescriptorSetLayoutCreateInfo descriptorSetLayoutCreate(
			const std::vector<VkDescriptorSetLayoutBinding>& bindings)
		{
			VkDescriptorSetLayoutCreateInfo descriptorSetLayoutCreateInfo{};
			descriptorSetLayoutCreateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
			descriptorSetLayoutCreateInfo.pNext = nullptr;
			descriptorSetLayoutCreateInfo.flags = 0;
			descriptorSetLayoutCreateInfo.bindingCount = static_cast<u32>(bindings.size());
			descriptorSetLayoutCreateInfo.pBindings = bindings.data();
			return descriptorSetLayoutCreateInfo;
		}

		VkDescriptorSetLayoutCreateInfo descriptorSetLayoutCreate(
			const utl::vector<VkDescriptorSetLayoutBinding>& bindings)
		{
			VkDescriptorSetLayoutCreateInfo descriptorSetLayoutCreateInfo{};
			descriptorSetLayoutCreateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
			descriptorSetLayoutCreateInfo.pNext = nullptr;
			descriptorSetLayoutCreateInfo.flags = 0;
			descriptorSetLayoutCreateInfo.bindingCount = static_cast<u32>(bindings.size());
			descriptorSetLayoutCreateInfo.pBindings = bindings.data();
			return descriptorSetLayoutCreateInfo;
		}

		VkPipelineLayoutCreateInfo pipelineLayoutCreate(
			u32 setLayoutCount = 0,
			const VkDescriptorSetLayout* pSetLayouts = nullptr,
			u32 pushCount = 0,
			VkPushConstantRange* constant = nullptr)
		{
			VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo{};
			pipelineLayoutCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
			pipelineLayoutCreateInfo.pNext = nullptr;
			pipelineLayoutCreateInfo.flags = 0;
			pipelineLayoutCreateInfo.setLayoutCount = setLayoutCount;
			pipelineLayoutCreateInfo.pSetLayouts = pSetLayouts;
			pipelineLayoutCreateInfo.pushConstantRangeCount = pushCount;
			pipelineLayoutCreateInfo.pPushConstantRanges = constant;
			return pipelineLayoutCreateInfo;
		}

		VkPipelineLayoutCreateInfo pipelineLayoutCreate(std::tuple<utl::vector<VkDescriptorSetLayout>, utl::vector<VkPushConstantRange>> info)
		{
			VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo{};
			pipelineLayoutCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
			pipelineLayoutCreateInfo.pNext = nullptr;
			pipelineLayoutCreateInfo.flags = 0;
			pipelineLayoutCreateInfo.setLayoutCount = std::get<0>(info).size();
			pipelineLayoutCreateInfo.pSetLayouts = std::get<0>(info).data();
			pipelineLayoutCreateInfo.pushConstantRangeCount = std::get<1>(info).size();
			pipelineLayoutCreateInfo.pPushConstantRanges = std::get<1>(info).data();
			return pipelineLayoutCreateInfo;
		}

		VkPipelineVertexInputStateCreateInfo pipelineVertexInputStateCreate(
			const std::vector<VkVertexInputBindingDescription> &vertexBindingDescriptions = std::vector<VkVertexInputBindingDescription>(),
			const std::vector<VkVertexInputAttributeDescription> &vertexAttributeDescriptions = std::vector<VkVertexInputAttributeDescription>()
		)
		{
			VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
			vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
			vertexInputInfo.pNext = nullptr;
			vertexInputInfo.flags = 0;
			vertexInputInfo.vertexBindingDescriptionCount = (vertexBindingDescriptions.empty() ? 0 : static_cast<u32>(vertexBindingDescriptions.size()));
			vertexInputInfo.pVertexBindingDescriptions = (vertexBindingDescriptions.empty() ? nullptr : vertexBindingDescriptions.data());
			vertexInputInfo.vertexAttributeDescriptionCount = (vertexAttributeDescriptions.empty() ? 0 : static_cast<u32>(vertexAttributeDescriptions.size()));
			vertexInputInfo.pVertexAttributeDescriptions = (vertexAttributeDescriptions.empty() ? nullptr : vertexAttributeDescriptions.data());
			return vertexInputInfo;
		}

		VkPushConstantRange pushConstantRange(
			VkShaderStageFlags stageFlags,
			u32 size,
			u32 offset)
		{
			VkPushConstantRange pushConstantRange;
			pushConstantRange.stageFlags = stageFlags;
			pushConstantRange.offset = offset;
			pushConstantRange.size = size;
			return pushConstantRange;
		}

		VkPipelineInputAssemblyStateCreateInfo pipelineInputAssemblyStateCreate(
			VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
			VkPipelineInputAssemblyStateCreateFlags flags = 0,
			VkBool32 primitiveRestartEnable = VK_FALSE)
		{
			VkPipelineInputAssemblyStateCreateInfo pipelineInputAssemblyStateCreateInfo;
			pipelineInputAssemblyStateCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
			pipelineInputAssemblyStateCreateInfo.pNext = nullptr;
			pipelineInputAssemblyStateCreateInfo.flags = flags;
			pipelineInputAssemblyStateCreateInfo.topology = topology;
			pipelineInputAssemblyStateCreateInfo.primitiveRestartEnable = primitiveRestartEnable;
			return pipelineInputAssemblyStateCreateInfo;
		}

		VkPipelineRasterizationStateCreateInfo pipelineRasterizationStateCreate(
			VkPolygonMode polygonMode,
			VkCullModeFlags cullMode,
			VkFrontFace frontFace,
			VkPipelineRasterizationStateCreateFlags flags)
		{
			VkPipelineRasterizationStateCreateInfo pipelineRasterizationStateCreateInfo;
			pipelineRasterizationStateCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
			pipelineRasterizationStateCreateInfo.pNext = VK_NULL_HANDLE;
			pipelineRasterizationStateCreateInfo.flags = flags;
			pipelineRasterizationStateCreateInfo.depthClampEnable = VK_FALSE;
			pipelineRasterizationStateCreateInfo.rasterizerDiscardEnable = VK_FALSE;
			pipelineRasterizationStateCreateInfo.polygonMode = polygonMode;
			pipelineRasterizationStateCreateInfo.cullMode = cullMode;
			pipelineRasterizationStateCreateInfo.frontFace = frontFace;
			pipelineRasterizationStateCreateInfo.depthBiasEnable = VK_FALSE;
			pipelineRasterizationStateCreateInfo.depthBiasClamp = 0.0f;
			pipelineRasterizationStateCreateInfo.depthBiasConstantFactor = 0.0f;
			pipelineRasterizationStateCreateInfo.depthBiasSlopeFactor = 0.0f;
			pipelineRasterizationStateCreateInfo.lineWidth = 1.0f;
			return pipelineRasterizationStateCreateInfo;
		}

		VkPipelineColorBlendAttachmentState pipelineColorBlendAttachmentState(
			VkColorComponentFlags colorWriteMask,
			VkBool32 blendEnable)
		{
			VkPipelineColorBlendAttachmentState pipelineColorBlendAttachmentState;
			pipelineColorBlendAttachmentState.blendEnable = blendEnable;
			pipelineColorBlendAttachmentState.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
			pipelineColorBlendAttachmentState.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
			pipelineColorBlendAttachmentState.colorBlendOp = VK_BLEND_OP_ADD;
			pipelineColorBlendAttachmentState.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
			pipelineColorBlendAttachmentState.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
			pipelineColorBlendAttachmentState.alphaBlendOp = VK_BLEND_OP_ADD;
			pipelineColorBlendAttachmentState.colorWriteMask = colorWriteMask;
			return pipelineColorBlendAttachmentState;
		}

		VkPipelineColorBlendStateCreateInfo pipelineColorBlendStateCreate(
			u32 attachmentCount,
			const VkPipelineColorBlendAttachmentState& pAttachments,
			VkBool32 logicOpEnable,
			VkLogicOp logicOp)
		{
			VkPipelineColorBlendStateCreateInfo pipelineColorBlendStateCreateInfo;
			pipelineColorBlendStateCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
			pipelineColorBlendStateCreateInfo.pNext = VK_NULL_HANDLE;
			pipelineColorBlendStateCreateInfo.flags = 0;
			pipelineColorBlendStateCreateInfo.logicOpEnable = logicOpEnable;
			pipelineColorBlendStateCreateInfo.logicOp = logicOp;
			pipelineColorBlendStateCreateInfo.attachmentCount = attachmentCount;
			pipelineColorBlendStateCreateInfo.pAttachments = &pAttachments;
			pipelineColorBlendStateCreateInfo.blendConstants[0] = 0.0f;
			pipelineColorBlendStateCreateInfo.blendConstants[1] = 0.0f;
			pipelineColorBlendStateCreateInfo.blendConstants[2] = 0.0f;
			pipelineColorBlendStateCreateInfo.blendConstants[3] = 0.0f;
			return pipelineColorBlendStateCreateInfo;
		}

		VkPipelineDepthStencilStateCreateInfo pipelineDepthStencilStateCreateInfo(
			VkBool32 depthTestEnable = VK_TRUE,
			VkBool32 depthWriteEnable = VK_TRUE,
			VkBool32 depthBoundsTest = VK_FALSE,
			VkBool32 stencilTest = VK_FALSE,
			VkCompareOp depthCompareOp = VK_COMPARE_OP_LESS)
		{
			VkPipelineDepthStencilStateCreateInfo pipelineDepthStencilStateCreateInfo;
			pipelineDepthStencilStateCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
			pipelineDepthStencilStateCreateInfo.pNext = VK_NULL_HANDLE;
			pipelineDepthStencilStateCreateInfo.flags = 0;
			pipelineDepthStencilStateCreateInfo.depthTestEnable = depthTestEnable;
			pipelineDepthStencilStateCreateInfo.depthWriteEnable = depthWriteEnable;
			pipelineDepthStencilStateCreateInfo.depthCompareOp = depthCompareOp;
			pipelineDepthStencilStateCreateInfo.minDepthBounds = 0.0f;
			pipelineDepthStencilStateCreateInfo.maxDepthBounds = 1.0f;
			pipelineDepthStencilStateCreateInfo.depthBoundsTestEnable = depthBoundsTest;
			pipelineDepthStencilStateCreateInfo.stencilTestEnable = stencilTest;
			pipelineDepthStencilStateCreateInfo.back = {};
			pipelineDepthStencilStateCreateInfo.back.compareOp = VK_COMPARE_OP_ALWAYS;
			pipelineDepthStencilStateCreateInfo.front = {};
			return pipelineDepthStencilStateCreateInfo;
		}

		VkPipelineViewportStateCreateInfo pipelineViewportStateCreate(
			u32 viewportCount,
			u32 scissorCount,
			VkPipelineViewportStateCreateFlags flags)
		{
			VkPipelineViewportStateCreateInfo pipelineViewportStateCreateInfo;
			pipelineViewportStateCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
			pipelineViewportStateCreateInfo.pNext = nullptr;
			pipelineViewportStateCreateInfo.flags = flags;
			pipelineViewportStateCreateInfo.viewportCount = viewportCount;
			pipelineViewportStateCreateInfo.scissorCount = scissorCount;
			return pipelineViewportStateCreateInfo;
		}

		VkPipelineMultisampleStateCreateInfo pipelineMultisampleStateCreate(
			VkSampleCountFlagBits rasterizationSamples,
			VkPipelineMultisampleStateCreateFlags flags)
		{
			VkPipelineMultisampleStateCreateInfo pipelineMultisampleStateCreateInfo;
			pipelineMultisampleStateCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
			pipelineMultisampleStateCreateInfo.pNext = nullptr;
			pipelineMultisampleStateCreateInfo.sampleShadingEnable = VK_FALSE;
			pipelineMultisampleStateCreateInfo.rasterizationSamples = rasterizationSamples;
			pipelineMultisampleStateCreateInfo.minSampleShading = 1.0f;
			pipelineMultisampleStateCreateInfo.alphaToCoverageEnable = VK_FALSE;
			pipelineMultisampleStateCreateInfo.alphaToOneEnable = VK_FALSE;
			pipelineMultisampleStateCreateInfo.flags = flags;
			pipelineMultisampleStateCreateInfo.pSampleMask = nullptr;
			return pipelineMultisampleStateCreateInfo;
		}

		//VkPipelineDynamicStateCreateInfo pipelineDynamicStateCreateInfo(
		//	const VkDynamicState * pDynamicStates,
		//	u32 dynamicStateCount,
		//	VkPipelineDynamicStateCreateFlags flags = 0)
		//{
		//	VkPipelineDynamicStateCreateInfo pipelineDynamicStateCreateInfo{};
		//	pipelineDynamicStateCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
		//	pipelineDynamicStateCreateInfo.pDynamicStates = pDynamicStates;
		//	pipelineDynamicStateCreateInfo.dynamicStateCount = dynamicStateCount;
		//	pipelineDynamicStateCreateInfo.flags = flags;
		//	return pipelineDynamicStateCreateInfo;
		//}

		VkPipelineDynamicStateCreateInfo pipelineDynamicStateCreate(
			const std::vector<VkDynamicState>& pDynamicStates,
			VkPipelineDynamicStateCreateFlags flags)
		{
			VkPipelineDynamicStateCreateInfo pipelineDynamicStateCreateInfo;
			pipelineDynamicStateCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
			pipelineDynamicStateCreateInfo.pNext = nullptr;
			pipelineDynamicStateCreateInfo.flags = flags;
			pipelineDynamicStateCreateInfo.dynamicStateCount = static_cast<u32>(pDynamicStates.size());
			pipelineDynamicStateCreateInfo.pDynamicStates = pDynamicStates.data();
			return pipelineDynamicStateCreateInfo;
		}

		VkPipelineTessellationStateCreateInfo pipelineTessellationStateCreate(u32 patchControlPoints)
		{
			VkPipelineTessellationStateCreateInfo pipelineTessellationStateCreateInfo;
			pipelineTessellationStateCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO;
			pipelineTessellationStateCreateInfo.pNext = nullptr;
			pipelineTessellationStateCreateInfo.flags = 0;
			pipelineTessellationStateCreateInfo.patchControlPoints = patchControlPoints;
			return pipelineTessellationStateCreateInfo;
		}

		VkGraphicsPipelineCreateInfo pipelineCreate(
			VkPipelineLayout layout,
			VkRenderPass renderPass,
			VkPipelineCreateFlags flags)
		{
			VkGraphicsPipelineCreateInfo pipelineCreateInfo;
			pipelineCreateInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
			pipelineCreateInfo.pNext = nullptr;
			pipelineCreateInfo.flags = flags;
			pipelineCreateInfo.layout = layout;
			pipelineCreateInfo.renderPass = renderPass;
			pipelineCreateInfo.subpass = 0;
			pipelineCreateInfo.basePipelineHandle = VK_NULL_HANDLE;
			pipelineCreateInfo.basePipelineIndex = -1;
			return pipelineCreateInfo;
		}

		//inline VkGraphicsPipelineCreateInfo pipelineCreateInfo()
		//{
		//	VkGraphicsPipelineCreateInfo pipelineCreateInfo{};
		//	pipelineCreateInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
		//	pipelineCreateInfo.basePipelineIndex = -1;
		//	pipelineCreateInfo.basePipelineHandle = VK_NULL_HANDLE;
		//	return pipelineCreateInfo;
		//}

		VkShaderModule loadShader(VkDevice device, const char *fileName)
		{
			{
				std::ifstream file(fileName, std::ios::binary | std::ios::in);

				if (file.is_open())
				{
					/*size_t size = (size_t)file.tellg();
					file.seekg(0, std::ios::beg);
					std::vector<char> shaderCode(size);
					file.read(shaderCode.data(), size);*/
					std::vector<char> fileContents((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
					file.close();

					assert(fileContents.size() > 0);

					VkShaderModule shaderModule;
					VkShaderModuleCreateInfo moduleCreateInfo;
					moduleCreateInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
					moduleCreateInfo.pNext = nullptr;
					moduleCreateInfo.flags = 0;
					moduleCreateInfo.codeSize = fileContents.size();
					moduleCreateInfo.pCode = reinterpret_cast<const u32*>(fileContents.data());

					VkResult result{ VK_SUCCESS };
					VkCall(result = vkCreateShaderModule(device, &moduleCreateInfo, NULL, &shaderModule), "Failed to create shader module...");

					return shaderModule;
				}
				else
				{
					std::cerr << "Error: Could not open shader file \"" << fileName << "\"" << "\n";
					return VK_NULL_HANDLE;
				}
			}
		}

		VkPipelineShaderStageCreateInfo loadShader(VkDevice device, std::string filename, VkShaderStageFlagBits stage)
		{
			VkPipelineShaderStageCreateInfo shaderStage;
			shaderStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			shaderStage.pNext = nullptr;
			shaderStage.flags = 0;
			shaderStage.stage = stage;
			shaderStage.module = loadShader(device, filename.c_str());
			shaderStage.pName = "main";
			shaderStage.pSpecializationInfo = nullptr;
			assert(shaderStage.module != VK_NULL_HANDLE);
			return shaderStage;
		}

		VkVertexInputBindingDescription vertexInputBindingDescription(u32 binding, u32 stride, VkVertexInputRate inputRate)
		{
			VkVertexInputBindingDescription bDescription;
			bDescription.binding = binding;
			bDescription.stride = stride;
			bDescription.inputRate = inputRate;
			return bDescription;
		}
    } //  descriptor namespace

	bool vulkan_success(VkResult result)
	{
        return result == VK_SUCCESS;
        //return false;
	}

	void copyBuffer(VkDevice device, u32 index, const VkCommandPool& pool, VkBuffer& srcBuffer, VkBuffer& dstBuffer, VkDeviceSize size) {
        
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.pNext = nullptr;
        allocInfo.commandPool = pool;
        allocInfo.commandBufferCount = 1;

        VkCommandBuffer commandBuffer;
        vkAllocateCommandBuffers(device, &allocInfo, &commandBuffer);

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.pNext = nullptr;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

        vkBeginCommandBuffer(commandBuffer, &beginInfo);

		VkBufferCopy copyRegion{};
		copyRegion.size = size;
		vkCmdCopyBuffer(commandBuffer, srcBuffer, dstBuffer, 1, &copyRegion);

		vkEndCommandBuffer(commandBuffer);

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.pNext = nullptr;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;

        VkQueue transferQueue;
        vkGetDeviceQueue(device, index, core::transfer_family_queue_index(), &transferQueue);
        vkQueueSubmit(transferQueue, 1, &submitInfo, VK_NULL_HANDLE);
        vkQueueWaitIdle(transferQueue);

        vkFreeCommandBuffers(device, pool, 1, &commandBuffer);
	}

    void createBuffer(VkDevice device, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& bufferMemory) {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.pNext = nullptr;
        bufferInfo.size = size;
        bufferInfo.usage = usage;
        bufferInfo.queueFamilyIndexCount = 0;
        bufferInfo.pQueueFamilyIndices = nullptr;
        bufferInfo.flags = 0;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateBuffer(device, &bufferInfo, nullptr, &buffer) != VK_SUCCESS) {
            throw std::runtime_error("failed to create buffer!");
        }

        VkMemoryRequirements memRequirements;
        vkGetBufferMemoryRequirements(device, buffer, &memRequirements);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.pNext = nullptr;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex = core::find_memory_index(memRequirements.memoryTypeBits, properties);

        if (vkAllocateMemory(device, &allocInfo, nullptr, &bufferMemory) != VK_SUCCESS) {
            throw std::runtime_error("failed to allocate buffer memory!");
        }

        vkBindBufferMemory(core::logical_device(), buffer, bufferMemory, 0);
    }

    VkCommandBuffer beginSingleCommand(VkDevice device, const VkCommandPool& pool)
    {
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.pNext = nullptr;
        allocInfo.commandPool = pool;
        allocInfo.commandBufferCount = 1;

        VkCommandBuffer commandBuffer;
        vkAllocateCommandBuffers(device, &allocInfo, &commandBuffer);

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.pNext = nullptr;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

        vkBeginCommandBuffer(commandBuffer, &beginInfo);

        return commandBuffer;
    }

    void endSingleCommand(VkDevice device, u32 index, const VkCommandPool& pool, VkCommandBuffer commandBuffer)
    {
        vkEndCommandBuffer(commandBuffer);

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.pNext = nullptr;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;

		// Create fence to ensure that the command buffer has finished executing
		VkFenceCreateInfo fenceInfo{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, 0 };

        VkQueue queue;
        vkGetDeviceQueue(device, index, 0, &queue);
        vkQueueSubmit(queue, 1, &submitInfo, nullptr);
		vkQueueWaitIdle(queue);

        vkFreeCommandBuffers(device, pool, 1, &commandBuffer);
    }

	void endSingleCommand_1(VkDevice device, u32 index, const VkCommandPool& pool, VkCommandBuffer commandBuffer)
	{
		vkEndCommandBuffer(commandBuffer);

		VkSubmitInfo submitInfo{};
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submitInfo.pNext = nullptr;
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &commandBuffer;

		// Create fence to ensure that the command buffer has finished executing
		VkFenceCreateInfo fenceInfo{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, 0 };
		VkFence fence;
		vkCreateFence(device, &fenceInfo, nullptr, &fence);

		VkQueue queue;
		vkGetDeviceQueue(device, index, 0, &queue);
		vkQueueSubmit(queue, 1, &submitInfo, fence);
		vkWaitForFences(device, 1, &fence, VK_TRUE, 1);
		vkDestroyFence(device, fence, nullptr);

		vkFreeCommandBuffers(device, pool, 1, &commandBuffer);
	}

	u32 formatIsFilterable(VkPhysicalDevice physicalDevice, VkFormat format, VkImageTiling tilling)
	{
		VkFormatProperties formatProps;
		vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &formatProps);

		if (tilling == VK_IMAGE_TILING_OPTIMAL)
			return formatProps.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;

		if (tilling == VK_IMAGE_TILING_LINEAR)
			return formatProps.linearTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;

		return false;
	}

	void setImageLayout(
		VkCommandBuffer cmdbuffer,
		VkImage image,
		VkImageLayout oldImageLayout,
		VkImageLayout newImageLayout,
		VkImageSubresourceRange subresourceRange,
		VkPipelineStageFlags srcStageMask,
		VkPipelineStageFlags dstStageMask)
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

	void createImageFromBufferData(void* data, VkDeviceSize bufferSize,
		VkFormat format, u32 width, u32 height, VkCommandPool pool, VkFilter filter,
		VkImageUsageFlags imageUsageFlags, VkImageLayout imageLayout, OUT id::id_type& tex_id)
	{
		assert(data);

		u32 miplevels = 1;

		VkMemoryAllocateInfo memAllocInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
		VkMemoryRequirements memReqs;

		// Use a separate command buffer for texture loading
		VkCommandBuffer copyCmd = beginSingleCommand(core::logical_device(), pool);

		VkBuffer stagingBuffer;
		VkDeviceMemory stagingMemory;

		createBuffer(core::logical_device(), bufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, stagingBuffer, stagingMemory);

		void* imageData;
		vkMapMemory(core::logical_device(), stagingMemory, 0, bufferSize, 0, (void**)&imageData);
		memcpy(imageData, data, bufferSize);
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

		VkImageCreateInfo imageCreateInfo{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
		imageCreateInfo.imageType = VK_IMAGE_TYPE_2D;
		imageCreateInfo.format = format;
		imageCreateInfo.mipLevels = miplevels;
		imageCreateInfo.arrayLayers = 1;
		imageCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
		imageCreateInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		imageCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		imageCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		imageCreateInfo.extent = { width, height, 1 };
		imageCreateInfo.usage = imageUsageFlags;

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

		VkImageViewCreateInfo viewCreateInfo = {};
		viewCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		viewCreateInfo.pNext = NULL;
		viewCreateInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		viewCreateInfo.format = format;
		viewCreateInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		viewCreateInfo.subresourceRange.levelCount = 1;
		tex_id = textures::add(imageCreateInfo, viewCreateInfo, samplerCreateInfo);

		textures::vulkan_texture_2d texture{ textures::get_texture(tex_id) };

		vkGetImageMemoryRequirements(core::logical_device(), texture.getTexture().image, &memReqs);

		VkDeviceMemory deviceMemory;
		memAllocInfo.allocationSize = memReqs.size;
		memAllocInfo.memoryTypeIndex = core::find_memory_index(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		vkAllocateMemory(core::logical_device(), &memAllocInfo, nullptr, &deviceMemory);
		vkBindImageMemory(core::logical_device(), texture.getTexture().image, deviceMemory, 0);

		VkImageSubresourceRange subresourceRange = {};
		subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		subresourceRange.baseMipLevel = 0;
		subresourceRange.levelCount = miplevels;
		subresourceRange.layerCount = 1;

		// Image barrier for optimal image (target)
		// Optimal image will be used as destination for the copy
		setImageLayout(
			copyCmd,
			texture.getTexture().image,
			VK_IMAGE_LAYOUT_UNDEFINED,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			subresourceRange);

		// Copy mip levels from staging buffer
		vkCmdCopyBufferToImage(
			copyCmd,
			stagingBuffer,
			texture.getTexture().image,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			1,
			&bufferCopyRegion
		);

		// Change texture image layout to shader read after all mip levels have been copied
		//this->imageLayout = imageLayout;
		setImageLayout(
			copyCmd,
			texture.getTexture().image,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			imageLayout,
			subresourceRange);

		endSingleCommand_1(core::logical_device(), 0, pool, copyCmd);

		// Clean up staging resources
		vkFreeMemory(core::logical_device(), stagingMemory, nullptr);
		vkDestroyBuffer(core::logical_device(), stagingBuffer, nullptr);
	}

	utl::vector<VkVertexInputBindingDescription> getVertexInputBindDescriptor()
	{
		utl::vector<VkVertexInputBindingDescription> _bindingDescription;
		_bindingDescription.emplace_back([]() {
			VkVertexInputBindingDescription bBind{ 0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX };
			return bBind;
			}());

		return _bindingDescription;
	}

	// TODO: Make a configuration to bind id
	utl::vector<VkVertexInputAttributeDescription> getVertexInputAttributeDescriptor()
	{
		utl::vector<VkVertexInputAttributeDescription> _attributeDescriptions;
		for (u32 i{ 0 }; i < sizeof(Vertex) / sizeof(math::v3); ++i)
		{
			_attributeDescriptions.emplace_back([i]() {
				VkVertexInputAttributeDescription attributeDescriptions{ i, 0, VK_FORMAT_R32G32B32_SFLOAT, sizeof(math::v3) * i };
				return attributeDescriptions;
			}());
		}
		return _attributeDescriptions;
	}
}