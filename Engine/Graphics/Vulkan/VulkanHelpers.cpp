// Copyright (c) Contributors of Primal+
// Distributed under the MIT license. See the LICENSE file in the project root for more information.
#include "VulkanHelpers.h"
#include "VulkanCore.h"

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

		VkWriteDescriptorSet setWriteDescriptorSet(VkStructureType type, VkDescriptorSet & set, u32 binding, VkDescriptorType dType, VkDescriptorImageInfo * image)
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

		VkDescriptorPoolCreateInfo descriptorPoolCreate(
			u32 poolSizeCount,
			VkDescriptorPoolSize* pPoolSizes,
			u32 maxSets)
		{
			VkDescriptorPoolCreateInfo descriptorPoolInfo;
			descriptorPoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
			descriptorPoolInfo.pNext = nullptr;
			descriptorPoolInfo.flags = 0;
			descriptorPoolInfo.maxSets = maxSets;
			descriptorPoolInfo.poolSizeCount = poolSizeCount;
			descriptorPoolInfo.pPoolSizes = pPoolSizes;
			return descriptorPoolInfo;
		}

		VkDescriptorPoolCreateInfo descriptorPoolCreate(
			const std::vector<VkDescriptorPoolSize>& poolSizes,
			u32 maxSets)
		{
			VkDescriptorPoolCreateInfo descriptorPoolInfo;
			descriptorPoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
			descriptorPoolInfo.pNext = nullptr;
			descriptorPoolInfo.flags = 0;
			descriptorPoolInfo.maxSets = maxSets;
			descriptorPoolInfo.poolSizeCount = static_cast<u32>(poolSizes.size());
			descriptorPoolInfo.pPoolSizes = poolSizes.data();
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

		//inline VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo(
		//	u32 setLayoutCount = 1,
		//	u32 pushCount = 0,
		//	VkPushConstantRange* constant = nullptr)
		//{
		//	VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo{};
		//	pipelineLayoutCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		//	pipelineLayoutCreateInfo.setLayoutCount = setLayoutCount;
		//	pipelineLayoutCreateInfo.pushConstantRangeCount = pushCount;
		//	pipelineLayoutCreateInfo.pPushConstantRanges = constant;
		//	return pipelineLayoutCreateInfo;
		//}

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

        VkQueue graphicsQueue;
        vkGetDeviceQueue(device, index, 0, &graphicsQueue);
        vkQueueSubmit(graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
        vkQueueWaitIdle(graphicsQueue);

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

        VkQueue graphicsQueue;
        vkGetDeviceQueue(device, index, 0, &graphicsQueue);
        vkQueueSubmit(graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
        vkQueueWaitIdle(graphicsQueue);

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
}