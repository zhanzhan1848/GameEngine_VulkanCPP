#include "VulkanDescriptor.h"
#include "VulkanSurface.h"
#include <fstream>
#include <iostream>
#include <vector>

namespace primal::graphics::vulkan::descriptor
{
	namespace
	{
		const std::string absolutePath = "C:/Users/27042/Desktop/DX_Test/PrimalMerge/Engine/Graphics/Vulkan/Shaders/";

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
			utl::vector<VkDescriptorSet>& sets, 
			u32 num,
			u32 binding,
			VkDescriptorType dType,
			VkDescriptorBufferInfo* buffer,
			VkDescriptorImageInfo* image)
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

		inline VkDescriptorSetAllocateInfo descriptorSetAllocate(
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
			VkDescriptorType type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			VkSampler* sampler = nullptr,
			u32 descriptorCount = 1)
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

		inline VkPipelineLayoutCreateInfo pipelineLayoutCreate(
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
			VkPolygonMode polygonMode = VK_POLYGON_MODE_FILL,
			VkCullModeFlags cullMode = VK_CULL_MODE_BACK_BIT,
			VkFrontFace frontFace = VK_FRONT_FACE_CLOCKWISE,
			VkPipelineRasterizationStateCreateFlags flags = 0)
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
			VkColorComponentFlags colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
			VkBool32 blendEnable = VK_FALSE)
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
			VkBool32 logicOpEnable = VK_FALSE,
			VkLogicOp logicOp = VK_LOGIC_OP_COPY)
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
			pipelineDepthStencilStateCreateInfo.front = {};
			return pipelineDepthStencilStateCreateInfo;
		}

		VkPipelineViewportStateCreateInfo pipelineViewportStateCreate(
			u32 viewportCount,
			u32 scissorCount,
			VkPipelineViewportStateCreateFlags flags = 0)
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
			VkSampleCountFlagBits rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
			VkPipelineMultisampleStateCreateFlags flags = 0)
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
			VkPipelineDynamicStateCreateFlags flags = 0)
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
			VkPipelineCreateFlags flags = 0)
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
	} // anonymous namespace


	void createDescriptorSets(VkDevice device, utl::vector<VkDescriptorSet>& descriptorSets, VkDescriptorSetLayout& descriptorSetLayout, VkDescriptorPool& descriptorPool, uniformBuffer& uniformBuffers, vulkan_texture tex)
	{
		utl::vector<VkDescriptorSetLayout> layouts(frame_buffer_count, descriptorSetLayout);
		VkDescriptorSetAllocateInfo allocInfo;
		allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		allocInfo.pNext = VK_NULL_HANDLE;
		allocInfo.descriptorPool = descriptorPool;
		allocInfo.descriptorSetCount = static_cast<u32>(frame_buffer_count);
		allocInfo.pSetLayouts = layouts.data();

		descriptorSets.resize(frame_buffer_count);
		VkResult result{ VK_SUCCESS };
		VkCall(result = vkAllocateDescriptorSets(device, &allocInfo, descriptorSets.data()), "Failed to allocate descriptor sets...");

		for (size_t i = 0; i < frame_buffer_count; ++i)
		{
			VkDescriptorBufferInfo bufferInfo;
			bufferInfo.buffer = uniformBuffers.buffer;
			bufferInfo.offset = 0;
			bufferInfo.range = sizeof(UniformBufferObject);

			VkDescriptorImageInfo imageInfo;
			imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			imageInfo.imageView = tex.view;
			imageInfo.sampler = tex.sampler;

			/*VkWriteDescriptorSet descriptorWrite;
			descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			descriptorWrite.pNext = VK_NULL_HANDLE;
			descriptorWrite.dstSet = descriptorSets[i];
			descriptorWrite.dstBinding = 0;
			descriptorWrite.dstArrayElement = 0;
			descriptorWrite.descriptorCount = 1;
			descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
			descriptorWrite.pBufferInfo = &bufferInfo;
			descriptorWrite.pImageInfo = nullptr;
			descriptorWrite.pTexelBufferView = nullptr;*/
			std::vector<VkWriteDescriptorSet> descriptorWrites = {
				setWriteDescriptorSet(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, descriptorSets, i, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &bufferInfo, nullptr),
				setWriteDescriptorSet(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, descriptorSets, i, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, &imageInfo),
			};

			//vkUpdateDescriptorSets(device, 1, &descriptorWrite, 0, nullptr);
			vkUpdateDescriptorSets(device, static_cast<u32>(descriptorWrites.size()), descriptorWrites.data(), 0, nullptr);
		}
	}

	void createDescriptorPool(VkDevice device, VkDescriptorPool& descriptorPool)
	{
		/*VkDescriptorPoolSize poolSize;
		poolSize.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		poolSize.descriptorCount = static_cast<u32>(frame_buffer_count);*/

		std::vector<VkDescriptorPoolSize> poolSize = {
			descriptorPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, static_cast<u32>(frame_buffer_count)),
			descriptorPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, static_cast<u32>(frame_buffer_count))
		};

		VkDescriptorPoolCreateInfo poolInfo;
		poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		poolInfo.pNext = VK_NULL_HANDLE;
		poolInfo.flags = 0;
		poolInfo.poolSizeCount = static_cast<u32>(poolSize.size());
		poolInfo.pPoolSizes = poolSize.data();
		poolInfo.maxSets = static_cast<u32>(frame_buffer_count);

		VkResult result{ VK_SUCCESS };
		VkCall(result = vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool), "Failed to create descriptor pool...");
	}

	void createDescriptorSetLayout(VkDevice device, VkDescriptorSetLayout& descriptorSetLayout)
	{
		std::vector<VkDescriptorSetLayoutBinding> setLayoutBindings{
			descriptorSetLayoutBinding(0, VK_SHADER_STAGE_VERTEX_BIT),
			descriptorSetLayoutBinding(1, VK_SHADER_STAGE_FRAGMENT_BIT, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
			// descriptorSetLayoutBinding(1, VK_SHADER_STAGE_FRAGMENT_BIT),
		};

		VkDescriptorSetLayoutCreateInfo descriptorLayout = descriptorSetLayoutCreate(setLayoutBindings);

		VkResult result{ VK_SUCCESS };
		VkCall(result = vkCreateDescriptorSetLayout(device, &descriptorLayout, nullptr, &descriptorSetLayout), "Failed to create descriptor set layout...");
	}

	void preparePipelines(VkDevice device, VkPipelineLayout& pipelineLayout, VkPipeline& pipeline, VkDescriptorSetLayout& descriptorSetLayout, vulkan_renderpass renderpass)
	{
		std::vector<VkPipelineShaderStageCreateInfo> shaderStages{
			loadShader(device, absolutePath + "test01.vert.spv", VK_SHADER_STAGE_VERTEX_BIT),
			loadShader(device, absolutePath + "test01.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT)
		};

		VkVertexInputBindingDescription bindingDescription;
		bindingDescription.binding = 0;
		bindingDescription.stride = sizeof(Vertex);
		bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
		std::vector<VkVertexInputBindingDescription> bindingDescriptions;
		bindingDescriptions.emplace_back(bindingDescription);
		std::vector<VkVertexInputAttributeDescription> attributeDescriptors;
		/*VkVertexInputAttributeDescription attributeDescriptions_0;
		attributeDescriptions_0.binding = 0;
		attributeDescriptions_0.location = 0;
		attributeDescriptions_0.format = VK_FORMAT_R32G32_SFLOAT;
		attributeDescriptions_0.offset = static_cast<u32>(offsetof(Vertex, Vertex::pos));
		attributeDescriptors.emplace_back(attributeDescriptions_0);*/
		/*VkVertexInputAttributeDescription attributeDescriptions_1;
		attributeDescriptions_1.binding = 0;
		attributeDescriptions_1.location = 1;
		attributeDescriptions_1.format = VK_FORMAT_R32G32B32_SFLOAT;
		attributeDescriptions_1.offset = static_cast<u32>(offsetof(Vertex, Vertex::color));
		attributeDescriptors.emplace_back(attributeDescriptions_1);*/
		attributeDescriptors.emplace_back([]() {
		VkVertexInputAttributeDescription attributeDescriptions{ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<u32>(offsetof(Vertex, Vertex::pos)) };
		return attributeDescriptions;
		}());
		attributeDescriptors.emplace_back([]() {
			VkVertexInputAttributeDescription attributeDescriptions{ 1, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<u32>(offsetof(Vertex, Vertex::color)) };
		return attributeDescriptions;
		}());
		attributeDescriptors.emplace_back([]() {
			VkVertexInputAttributeDescription attributeDescriptions{ 2, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<u32>(offsetof(Vertex, Vertex::texCoord)) };
		return attributeDescriptions;
		}());
		

		VkPipelineVertexInputStateCreateInfo vertexInputInfo;
		vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
		vertexInputInfo.pNext = nullptr;
		vertexInputInfo.flags = 0;
		vertexInputInfo.vertexBindingDescriptionCount = static_cast<u32>(bindingDescriptions.size());
		vertexInputInfo.pVertexBindingDescriptions = bindingDescriptions.data();
		vertexInputInfo.vertexAttributeDescriptionCount = static_cast<u32>(attributeDescriptors.size());
		vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptors.data();

		VkPipelineInputAssemblyStateCreateInfo inputAssemblyState = pipelineInputAssemblyStateCreate(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, VK_FALSE);
		VkPipelineViewportStateCreateInfo viewportState = pipelineViewportStateCreate(1, 1);
		VkPipelineRasterizationStateCreateInfo rasterizationState = pipelineRasterizationStateCreate(VK_POLYGON_MODE_FILL, VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_CLOCKWISE);
		VkPipelineMultisampleStateCreateInfo multisampleState = pipelineMultisampleStateCreate(VK_SAMPLE_COUNT_1_BIT);
		VkPipelineColorBlendAttachmentState blendAttachmentState = pipelineColorBlendAttachmentState(VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT, VK_FALSE);
		VkPipelineColorBlendStateCreateInfo colorBlendState = pipelineColorBlendStateCreate(1, blendAttachmentState);

		std::vector<VkDynamicState> dynamicStateEnables{ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
		VkPipelineDynamicStateCreateInfo dynamicState = pipelineDynamicStateCreate(dynamicStateEnables);
		VkPipelineDepthStencilStateCreateInfo depthStencilState = pipelineDepthStencilStateCreateInfo(VK_TRUE, VK_TRUE, VK_FALSE, VK_FALSE, VK_COMPARE_OP_LESS_OR_EQUAL);
		// VkPipelineVertexInputStateCreateInfo emptyInputState = pipelineVertexInputStateCreateInfo(bindingDescriptions, attributeDescriptors);

		VkPipelineLayoutCreateInfo pipelineLayoutInfo;
		pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		pipelineLayoutInfo.pNext = nullptr;
		pipelineLayoutInfo.flags = 0;
		pipelineLayoutInfo.setLayoutCount = 1;
		pipelineLayoutInfo.pSetLayouts = &descriptorSetLayout;
		pipelineLayoutInfo.pushConstantRangeCount = 0;
		pipelineLayoutInfo.pPushConstantRanges = VK_NULL_HANDLE;
		VkResult result{ VK_SUCCESS };
		VkCall(result = vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout), "Failed to create pipeline layout...");

		//VkGraphicsPipelineCreateInfo pipelineCI = pipelineCreate(pipelineLayout, renderpass.render_pass);
		VkGraphicsPipelineCreateInfo pipelineCI;
		pipelineCI.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
		pipelineCI.pNext = nullptr;
		pipelineCI.flags = 0;
		pipelineCI.stageCount = static_cast<u32>(shaderStages.size());
		pipelineCI.pStages = shaderStages.data();
		pipelineCI.pVertexInputState = &vertexInputInfo;
		pipelineCI.pInputAssemblyState = &inputAssemblyState;
		pipelineCI.pTessellationState = VK_NULL_HANDLE;
		pipelineCI.pViewportState = &viewportState;
		pipelineCI.pRasterizationState = &rasterizationState;
		pipelineCI.pMultisampleState = &multisampleState;
		pipelineCI.pDepthStencilState = &depthStencilState;
		pipelineCI.pColorBlendState = &colorBlendState;
		pipelineCI.pDynamicState = &dynamicState;
		pipelineCI.layout = pipelineLayout;
		pipelineCI.renderPass = renderpass.render_pass;
		pipelineCI.subpass = 0;
		pipelineCI.basePipelineHandle = VK_NULL_HANDLE;
		pipelineCI.basePipelineIndex = -1;

		result = { VK_SUCCESS };
		VkCall(result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineCI, nullptr, &pipeline), "Failed to create graphics pipelines...");

		for (auto shaderModule : shaderStages)
		{
			vkDestroyShaderModule(device, shaderModule.module, nullptr);
		}
	}

}