#include "VulkanContent.h"
#include "VulkanHelpers.h"
#include "VulkanResources.h"
#include "VulkanCore.h"
#include "VulkanTexture.h"
#define STB_IMAGE_IMPLEMENTATION
#include "Content/stb_image.h"
#include "Utilities/FreeList.h"
#include <iostream>
#define TINYOBJLOADER_IMPLEMENTATION
#include "Content/tiny_obj_loader.h"
#include <unordered_map>


namespace primal::graphics::vulkan
{
	namespace
	{

	} // anonymous namespace

	namespace textures
	{
		namespace
		{
			utl::free_list<textures::vulkan_texture_2d>			textures;

			std::mutex											texture_mutex;
		} // anonymous namespace

		vulkan_texture_2d::vulkan_texture_2d(std::string path)
		{
			loadTexture(path);
		}

		vulkan_texture_2d::~vulkan_texture_2d()
		{
			vkDestroySampler(core::logical_device(), _texture.sampler, nullptr);
			destroy_image(core::logical_device(), &_texture);
		}

		void vulkan_texture_2d::loadTexture(std::string path)
		{
			int texWidth, texHeight, texChannels;
			void* pixels = stbi_load(path.c_str(), &texWidth, &texHeight, &texChannels, STBI_rgb_alpha);
			VkDeviceSize imageSize = 0;
			VkFormat imageFormat = VK_FORMAT_UNDEFINED;

			switch (texChannels)
			{
			case 3:
			{
				imageSize = texWidth * texHeight * 4;
				imageFormat = VK_FORMAT_R8G8B8A8_SRGB;
			}break;
			case 4:
			{
				imageSize = texWidth * texHeight * 4;
				imageFormat = VK_FORMAT_R8G8B8A8_SRGB;
			}break;
			default:
				throw std::runtime_error("The texture do not support...");
				break;
			}

			if (!pixels) throw std::runtime_error("Failed to load texture data...");

			baseBuffer staging;
			createBuffer(core::logical_device(), imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging.buffer, staging.memory);
			//vkBindBufferMemory(core::logical_device(), staging.buffer, staging.memory, 0);

			void* data;
			vkMapMemory(core::logical_device(), staging.memory, 0, imageSize, 0, &data);
			memcpy(data, pixels, static_cast<size_t>(imageSize));
			vkUnmapMemory(core::logical_device(), staging.memory);

			stbi_image_free(pixels);

			image_init_info image_info{};
			image_info.image_type = VK_IMAGE_TYPE_2D;
			image_info.width = texWidth;
			image_info.height = texHeight;
			image_info.format = imageFormat;
			image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
			image_info.usage_flags = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
			image_info.memory_flags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
			image_info.create_view = true;
			image_info.view_aspect_flags = VK_IMAGE_ASPECT_COLOR_BIT;

			create_image(core::logical_device(), &image_info, _texture);
			transitionImageLayout(core::logical_device(), core::graphics_family_queue_index(), core::get_current_command_pool(), _texture.image, imageFormat, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
			copyBufferToImage(core::logical_device(), core::graphics_family_queue_index(), core::get_current_command_pool(), staging.buffer, _texture.image, static_cast<u32>(texWidth), static_cast<u32>(texHeight));
			transitionImageLayout(core::logical_device(), core::graphics_family_queue_index(), core::get_current_command_pool(), _texture.image, imageFormat, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

			vkDestroyBuffer(core::logical_device(), staging.buffer, nullptr);
			vkFreeMemory(core::logical_device(), staging.memory, nullptr);

			VkPhysicalDeviceProperties properties;
			vkGetPhysicalDeviceProperties(core::physical_device(), &properties);

			VkSamplerCreateInfo samplerInfo;
			samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
			samplerInfo.pNext = nullptr;
			samplerInfo.flags = /*VK_SAMPLER_CREATE_SUBSAMPLED_BIT_EXT | */VK_SAMPLER_CREATE_SUBSAMPLED_COARSE_RECONSTRUCTION_BIT_EXT;
			samplerInfo.magFilter = VK_FILTER_LINEAR;
			samplerInfo.minFilter = VK_FILTER_LINEAR;
			samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
			samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
			samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
			samplerInfo.mipLodBias = 0.0f;
			samplerInfo.minLod = 0.0f;
			samplerInfo.maxLod = 0.0f;
			samplerInfo.anisotropyEnable = VK_TRUE;
			samplerInfo.maxAnisotropy = properties.limits.maxSamplerAnisotropy;
			samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
			samplerInfo.unnormalizedCoordinates = VK_FALSE;
			samplerInfo.compareEnable = VK_FALSE;
			samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
			samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;

			VkResult result{ VK_SUCCESS };
			VkCall(result = vkCreateSampler(core::logical_device(), &samplerInfo, nullptr, &_texture.sampler), "Failed to create texture sampler...");
		}

		id::id_type add(std::string path)
		{
			return textures.add(path);
		}

		void remove(id::id_type id)
		{
			std::lock_guard lock{ texture_mutex };
			assert(id::is_valid(id));
			textures.remove(id);
		}

		vulkan_texture_2d& get_texture(id::id_type id)
		{
			std::lock_guard lock{ texture_mutex };
			assert(id::is_valid(id));
			return textures[id];
		}
	} // primai::graphics::vulkan::texture

	namespace shaders
	{
		namespace
		{
			utl::free_list<shaders::vulkan_shader>				shaders;

			std::mutex											shader_mutex;
		} // anonymous namesapce

		id::id_type add(std::string path, shader_type::type type)
		{
			vulkan_shader shader;
			shader.loadFile(path, type);
			std::lock_guard lock{ shader_mutex };
			return shaders.add(std::move(shader));
		}

		void remove(id::id_type id)
		{
			std::lock_guard lock{ shader_mutex };
			assert(id::is_valid(id));
			shaders.remove(id);
		}

		vulkan_shader get_shader(id::id_type id)
		{
			std::lock_guard lock{ shader_mutex };
			assert(id::is_valid(id));
			return shaders[id];
		}
	} // primai::graphics::vulkan::shader

	namespace materials
	{
		namespace
		{
			utl::free_list<vulkan_material>		materials;
			std::mutex							material_mutex;

		} // anonymous namespace

		vulkan_material::~vulkan_material()
		{
			this->_texture_count = 0;
			this->_texture_ids.clear();
			this->_shader_ids.clear();
		}

		void vulkan_material::add_texture(std::string path)
		{
			_texture_ids.emplace_back(textures::add(path));
			_texture_count++;
		}

		void vulkan_material::remove_texture(id::id_type id)
		{
			_texture_ids.erase(id);
		}

		void vulkan_material::add_shader(std::string path, shader_type::type type)
		{
			assert(type < shader_type::count);
			if (type > shader_type::count)
			{
				std::cerr << "Type is error" << std::endl;
				return;
			}

			_shader_ids.at(type) = shaders::add(path, type);
		}

		void vulkan_material::remove_shader(id::id_type id, shader_type::type type)
		{
			assert(shaders::get_shader(id));
			if (id != _shader_ids.at(type)) std::cerr << "Shader id  error!" << std::endl;
			shaders::remove(id);
			_shader_ids.at(type) = id::invalid_id;
		}

		id::id_type add(material_init_info info)
		{
			vulkan_material m(info);
			std::lock_guard lock{ material_mutex };
			return materials.add(std::move(m));
		}

		void remove(id::id_type id)
		{
			std::lock_guard lock{ material_mutex };
			assert(id::is_valid(id));
			materials.remove(id);
		}

		vulkan_material get_material(id::id_type id)
		{
			return materials[id];
		}
	} // primai::graphics::vulkan::material

	namespace submesh
	{
		namespace 
		{
			struct equal_idx
			{
				bool operator()(const tinyobj::index_t& a, const tinyobj::index_t& b) const
				{
					return a.vertex_index == b.vertex_index
						&& a.texcoord_index == b.texcoord_index
						&& a.normal_index == b.normal_index;
				}
			};

			struct hash_idx
			{
				size_t operator()(const tinyobj::index_t& a) const
				{
					return ((a.vertex_index ^ a.texcoord_index << 1) >> 1) ^ (a.normal_index << 1);
				}
			};

			utl::free_list<vulkan_model>							_models;
		} // anonymous namespace

		vulkan_model::vulkan_model(std::string path)
		{
			load_model(path);
			create_vertex_buffer();
			create_index_buffer();
		}

		vulkan_model::~vulkan_model()
		{
			vkDestroyBuffer(core::logical_device(), _vertexBuffer.buffer, nullptr);
			vkFreeMemory(core::logical_device(), _vertexBuffer.memory, nullptr);

			vkDestroyBuffer(core::logical_device(), _indexBuffer.buffer, nullptr);
			vkFreeMemory(core::logical_device(), _indexBuffer.memory, nullptr);
			_vertices.clear();
			_indices.clear();
		}

		void vulkan_model::load_model(std::string path)
		{
			tinyobj::attrib_t attrib;
			std::vector<tinyobj::shape_t> shapes;
			std::vector<tinyobj::material_t> materials;
			std::string warn, err;

			if (!tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, path.c_str()))
			{
				throw std::runtime_error(warn + err);
			}

			std::unordered_map<tinyobj::index_t, size_t, hash_idx, equal_idx> uniqueVertices;

			_vertices.clear();
			_indices.clear();
			for (const auto& shape : shapes)
				for (const auto& index : shape.mesh.indices)
				{
					Vertex vertex;
					vertex.pos = math::v3{
						attrib.vertices[3 * index.vertex_index + 0],
						attrib.vertices[3 * index.vertex_index + 1],
						attrib.vertices[3 * index.vertex_index + 2]
					};

					vertex.color = math::v3{ 1.0f, 1.0f, 1.0f };

					vertex.texCoord = math::v3{
						attrib.texcoords[2 * index.texcoord_index + 0],
						1.0f - attrib.texcoords[2 * index.texcoord_index + 1],
						0.0f
					};

					if (uniqueVertices.count(index) == 0)
					{
						uniqueVertices[index] = (u32)_vertices.size();
						_vertices.emplace_back(vertex);
					}

					_indices.emplace_back((u32)uniqueVertices[index]);
				}
		}

		void vulkan_model::create_model_buffer()
		{
			create_vertex_buffer();
			create_index_buffer();
		}

		void vulkan_model::create_vertex_buffer()
		{
			VkDeviceSize bufferSize = sizeof(Vertex) * _vertices.size();

			VkBuffer stagingBuffer;
			VkDeviceMemory stagingBufferMemory;
			createBuffer(core::logical_device(), bufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, stagingBuffer, stagingBufferMemory);

			void* data;
			vkMapMemory(core::logical_device(), stagingBufferMemory, 0, bufferSize, 0, &data);
			memcpy(data, _vertices.data(), (size_t)bufferSize);
			vkUnmapMemory(core::logical_device(), stagingBufferMemory);

			createBuffer(core::logical_device(), bufferSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, _vertexBuffer.buffer, _vertexBuffer.memory);

			copyBuffer(core::logical_device(), core::graphics_family_queue_index(), core::get_current_command_pool(), stagingBuffer, _vertexBuffer.buffer, bufferSize);
		}

		void vulkan_model::create_index_buffer() {
			VkDeviceSize bufferSize = sizeof(u16) * _indices.size();

			VkBuffer stagingBuffer;
			VkDeviceMemory stagingBufferMemory;
			createBuffer(core::logical_device(), bufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, stagingBuffer, stagingBufferMemory);

			void* data;
			vkMapMemory(core::logical_device(), stagingBufferMemory, 0, bufferSize, 0, &data);
			memcpy(data, _indices.data(), (size_t)bufferSize);
			vkUnmapMemory(core::logical_device(), stagingBufferMemory);

			createBuffer(core::logical_device(), bufferSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, _indexBuffer.buffer, _indexBuffer.memory);

			copyBuffer(core::logical_device(), core::graphics_family_queue_index(), core::get_current_command_pool(), stagingBuffer, _indexBuffer.buffer, bufferSize);
		}

		void vulkan_instance_model::create_instance_buffer()
		{
			size_t bufferSize = sizeof(InstanceData);

			VkBuffer stagingBuffer;
			VkDeviceMemory stagingBufferMemory;
			createBuffer(core::logical_device(), bufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, stagingBuffer, stagingBufferMemory);

			void* data;
			vkMapMemory(core::logical_device(), stagingBufferMemory, 0, bufferSize, 0, &data);
			memcpy(data, &_instanceData, sizeof(InstanceData));
			vkUnmapMemory(core::logical_device(), stagingBufferMemory);

			createBuffer(core::logical_device(), bufferSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, _instanceBuffer.buffer, _instanceBuffer.memory);

			copyBuffer(core::logical_device(), core::graphics_family_queue_index(), core::get_current_command_pool(), stagingBuffer, _instanceBuffer.buffer, bufferSize);

			vkDestroyBuffer(core::logical_device(), stagingBuffer, nullptr);
			vkFreeMemory(core::logical_device(), stagingBufferMemory, nullptr);
		}

		vulkan_instance_model::vulkan_instance_model(id::id_type model_id) : _model{ get_model(model_id)}
		{
			_model.create_model_buffer();
			create_instance_buffer();
		}

		vulkan_instance_model::~vulkan_instance_model()
		{
			vkDeviceWaitIdle(core::logical_device());

			vkDestroyPipeline(core::logical_device(), _pipeline, nullptr);

			/*for (auto texture_id : materials::get_material(_material_id).getTextureIDS())
			{
				textures::get_texture(texture_id).~vulkan_texture_2d();
			}*/

			vkDestroyBuffer(core::logical_device(), _instanceBuffer.buffer, nullptr);
			vkFreeMemory(core::logical_device(), _instanceBuffer.memory, nullptr);

			_model.~vulkan_model();
		}

		void vulkan_instance_model::createDescriptorSet(VkDescriptorPool pool, VkDescriptorSetLayout layout)
		{
			VkDescriptorSetAllocateInfo allocInfo;
			utl::vector<VkWriteDescriptorSet> writeDescriptors;

			allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
			allocInfo.pNext = VK_NULL_HANDLE;
			allocInfo.descriptorPool = pool;
			allocInfo.descriptorSetCount = 1;
			allocInfo.pSetLayouts = &layout;

			VkResult result{ VK_SUCCESS };
			VkCall(result = vkAllocateDescriptorSets(core::logical_device(), &allocInfo, &_descriptorSet), "Failed to create descriptor set...");

			// TODO: Unlock this to access instance's private attribute to translate to GPU
			/*VkDescriptorBufferInfo bufferInfo;
			bufferInfo.buffer = _uniformBuffer.buffer;
			bufferInfo.offset = 0;
			bufferInfo.range = sizeof(math::m4x4);
			writeDescriptors.emplace_back(descriptor::setWriteDescriptorSet(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, this->_descriptorSet, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &bufferInfo, nullptr));

			int iter_num = 0;
			for (auto iter : materials::get_material(this->getMaterialID()).getTextureIDS())
			{
				++iter_num;
				VkDescriptorImageInfo imageInfo;
				imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				imageInfo.imageView = textures::get_texture(iter).getTexture().view;
				imageInfo.sampler = textures::get_texture(iter).getTexture().sampler;
				writeDescriptors.emplace_back(descriptor::setWriteDescriptorSet(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, this->_descriptorSet, iter_num, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, &imageInfo));
			}
			vkUpdateDescriptorSets(core::logical_device(), static_cast<u32>(writeDescriptors.size()), writeDescriptors.data(), 0, nullptr);*/
		}

		void vulkan_instance_model::flushBuffer(vulkan_cmd_buffer cmd_buffer)
		{
			VkDeviceSize offset[] = { 0 };
			auto vertexBuffer = _model.getVertexBuffer();
			auto indexBuffer = _model.getIndexBuffer();
			vkCmdBindVertexBuffers(cmd_buffer.cmd_buffer, 0, 1, &vertexBuffer.buffer, offset);
			vkCmdBindVertexBuffers(cmd_buffer.cmd_buffer, 1, 1, &_instanceBuffer.buffer, offset);
			vkCmdBindIndexBuffer(cmd_buffer.cmd_buffer, indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT16);
		}

		void vulkan_instance_model::draw(vulkan_cmd_buffer cmd_buffer)
		{
			vkCmdDrawIndexed(cmd_buffer.cmd_buffer, _model.getIndicesCount(), 1, 0, 0, 0);
		}

		void vulkan_instance_model::createPipeline(VkPipelineLayout pipelineLayou, vulkan_renderpass render_pass)
		{
			VkPipelineInputAssemblyStateCreateInfo inputAssemblyState = descriptor::pipelineInputAssemblyStateCreate(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, VK_FALSE);
			VkPipelineViewportStateCreateInfo viewportState = descriptor::pipelineViewportStateCreate(1, 1);
			VkPipelineRasterizationStateCreateInfo rasterizationState = descriptor::pipelineRasterizationStateCreate(VK_POLYGON_MODE_FILL, VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_CLOCKWISE);
			VkPipelineMultisampleStateCreateInfo multisampleState = descriptor::pipelineMultisampleStateCreate(VK_SAMPLE_COUNT_1_BIT);
			VkPipelineColorBlendAttachmentState blendAttachmentState = descriptor::pipelineColorBlendAttachmentState(VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT, VK_FALSE);
			VkPipelineColorBlendStateCreateInfo colorBlendState = descriptor::pipelineColorBlendStateCreate(1, blendAttachmentState);

			std::vector<VkDynamicState> dynamicStateEnables{ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
			VkPipelineDynamicStateCreateInfo dynamicState = descriptor::pipelineDynamicStateCreate(dynamicStateEnables);
			VkPipelineDepthStencilStateCreateInfo depthStencilState = descriptor::pipelineDepthStencilStateCreateInfo(VK_TRUE, VK_TRUE, VK_FALSE, VK_FALSE, VK_COMPARE_OP_LESS_OR_EQUAL);

			getVertexInputBindDescriptor();
			getVertexInputAttributeDescriptor();

			VkPipelineVertexInputStateCreateInfo vertexInputInfo;
			vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
			vertexInputInfo.pNext = nullptr;
			vertexInputInfo.flags = 0;
			vertexInputInfo.vertexBindingDescriptionCount = static_cast<u32>(_bindingDescription.size());
			vertexInputInfo.pVertexBindingDescriptions = _bindingDescription.data();
			vertexInputInfo.vertexAttributeDescriptionCount = static_cast<u32>(_attributeDescriptions.size());
			vertexInputInfo.pVertexAttributeDescriptions = _attributeDescriptions.data();

			getShaderStage();

			VkGraphicsPipelineCreateInfo pipelineCI;
			pipelineCI.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
			pipelineCI.pNext = nullptr;
			pipelineCI.flags = 0;
			pipelineCI.stageCount = static_cast<u32>(_shaderStages.size());
			pipelineCI.pStages = _shaderStages.data();
			pipelineCI.pVertexInputState = &vertexInputInfo;
			pipelineCI.pInputAssemblyState = &inputAssemblyState;
			pipelineCI.pTessellationState = VK_NULL_HANDLE;
			pipelineCI.pViewportState = &viewportState;
			pipelineCI.pRasterizationState = &rasterizationState;
			pipelineCI.pMultisampleState = &multisampleState;
			pipelineCI.pDepthStencilState = &depthStencilState;
			pipelineCI.pColorBlendState = &colorBlendState;
			pipelineCI.pDynamicState = &dynamicState;
			pipelineCI.layout = pipelineLayou;
			pipelineCI.renderPass = render_pass.render_pass;
			pipelineCI.subpass = 0;
			pipelineCI.basePipelineHandle = VK_NULL_HANDLE;
			pipelineCI.basePipelineIndex = -1;

			VkResult result{ VK_SUCCESS };
			VkCall(result = vkCreateGraphicsPipelines(core::logical_device(), VK_NULL_HANDLE, 1, &pipelineCI, nullptr, &_pipeline), "Failed to create graphics pipelines...");

			for (auto shaderModule : _shaderStages)
			{
				vkDestroyShaderModule(core::logical_device(), shaderModule.module, nullptr);
			}

		}

		id::id_type add(std::string path)
		{
			return _models.add(vulkan_model(path));
		}

		void remove_model(id::id_type id)
		{
			_models.remove(id);
		}

		vulkan_model get_model(id::id_type id)
		{
			return _models[id];
		}
	} // primai::graphics::vulkan::submesh

	namespace scene
	{
		namespace
		{
			
		}

		vulkan_scene::~vulkan_scene()
		{
			for (auto& instance : _instance_models)
			{
				instance.~vulkan_instance_model();
			}
			vkDestroyPipelineLayout(core::logical_device(), _pipelineLayout, nullptr);
			vkDestroyDescriptorPool(core::logical_device(), _descriptorPool, nullptr);
			vkDestroyDescriptorSetLayout(core::logical_device(), _descriptorSetLayout, nullptr);
		}

		void vulkan_scene::add_model_instance(id::id_type model_id)
		{
			// Use model_id in _models to create a instance model in scene
			//submesh::vulkan_instance_model model(model_id);
			_instance_models.emplace_back(model_id);
		}

		void vulkan_scene::remove_modeel_instance(id::id_type id)
		{
			_instance_models.erase(id);
		}

		void vulkan_scene::add_camera(camera_init_info info)
		{
			//camera::vulkan_camera cam(info);
			_camera_ids.emplace_back(graphics::create_camera(info).get_id());
			graphics::vulkan::camera::get(_camera_ids.back()).update();
		}

		void vulkan_scene::remove_camera(id::id_type id)
		{
			_camera_ids.erase(id);
			graphics::vulkan::camera::remove((camera_id)id);
		}

		void vulkan_scene::add_material(id::id_type model_id, id::id_type material_id)
		{
			_instance_models[0].add_material(material_id);
		}

		void vulkan_scene::remove_material(id::id_type model_id)
		{
			_instance_models[model_id].remove_material();
		}

		void vulkan_scene::createUniformBuffer(u32 width, u32 height) {
			VkDeviceSize bufferSize = sizeof(UniformBufferObject);

			createBuffer(core::logical_device(), bufferSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, _uniformBuffer.buffer, _uniformBuffer.memory);

			//vkBindBufferMemory(core::logical_device(), _uniformBuffer.buffer, _uniformBuffer.memory, 0);

			vkMapMemory(core::logical_device(), _uniformBuffer.memory, 0, bufferSize, 0, &_uniformBuffer.mapped);

			//add_camera({ 128, graphics::camera::type::perspective, math::v3{0, 1, 0}, 1280, 720, 0.01, 10000 });

			if (_camera_ids.empty())
			{
				DirectX::XMMATRIX modelMatrix = DirectX::XMMatrixIdentity();
				modelMatrix = DirectX::XMMatrixRotationZ(DirectX::XM_PIDIV2) * modelMatrix;
				DirectX::XMStoreFloat4x4(&_ubo.model, modelMatrix);
				DirectX::XMMATRIX viewMatrix = DirectX::XMMatrixLookAtLH({ 2, 2, 2 }, { 0, 0, 0 }, { 0, 0, 1 });
				DirectX::XMStoreFloat4x4(&_ubo.view, viewMatrix);
				DirectX::XMMATRIX projectionMatrix = DirectX::XMMatrixPerspectiveFovLH(DirectX::XM_PIDIV4, (f32)width / (f32)height, 0.01, 1000.0);
				projectionMatrix.r[1].m128_f32[1] *= -1;
				DirectX::XMStoreFloat4x4(&_ubo.projection, projectionMatrix);
				memcpy(_uniformBuffer.mapped, &_ubo, sizeof(UniformBufferObject));
			}
			else
			{
				DirectX::XMMATRIX modelMatrix = DirectX::XMMatrixIdentity();
				modelMatrix = DirectX::XMMatrixRotationZ(DirectX::XM_PIDIV2) * modelMatrix;
				DirectX::XMStoreFloat4x4(&_ubo.model, modelMatrix);
				DirectX::XMStoreFloat4x4(&_ubo.view, graphics::vulkan::camera::get(_camera_ids.font()).view());
				DirectX::XMStoreFloat4x4(&_ubo.projection, graphics::vulkan::camera::get(_camera_ids.font()).projection());
				memcpy(_uniformBuffer.mapped, &_ubo, sizeof(UniformBufferObject));
			}
		}

		void vulkan_scene::createDescriptorPool()
		{
			std::vector<VkDescriptorPoolSize> poolSize = {
				descriptor::descriptorPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, static_cast<u32>(frame_buffer_count)),
				descriptor::descriptorPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, static_cast<u32>(frame_buffer_count))
			};

			VkDescriptorPoolCreateInfo poolInfo;
			poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
			poolInfo.pNext = VK_NULL_HANDLE;
			poolInfo.flags = 0;
			poolInfo.poolSizeCount = static_cast<u32>(poolSize.size());
			poolInfo.pPoolSizes = poolSize.data();
			poolInfo.maxSets = static_cast<u32>(frame_buffer_count);

			VkResult result{ VK_SUCCESS };
			VkCall(result = vkCreateDescriptorPool(core::logical_device(), &poolInfo, nullptr, &_descriptorPool), "Failed to create descriptor pool...");
		}

		void vulkan_scene::createDescriptorSetLayout()
		{
			std::vector<VkDescriptorSetLayoutBinding> setLayoutBindings{
				descriptor::descriptorSetLayoutBinding(0, VK_SHADER_STAGE_VERTEX_BIT),
				descriptor::descriptorSetLayoutBinding(1, VK_SHADER_STAGE_FRAGMENT_BIT, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
			};

			VkDescriptorSetLayoutCreateInfo descriptorLayout = descriptor::descriptorSetLayoutCreate(setLayoutBindings);

			VkResult result{ VK_SUCCESS };
			VkCall(result = vkCreateDescriptorSetLayout(core::logical_device(), &descriptorLayout, nullptr, &_descriptorSetLayout), "Failed to create descriptor set layout...");

			VkPipelineLayoutCreateInfo pipelineLayoutInfo;
			pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
			pipelineLayoutInfo.pNext = nullptr;
			pipelineLayoutInfo.flags = 0;
			pipelineLayoutInfo.setLayoutCount = 1;
			pipelineLayoutInfo.pSetLayouts = &_descriptorSetLayout;
			pipelineLayoutInfo.pushConstantRangeCount = 0;
			pipelineLayoutInfo.pPushConstantRanges = VK_NULL_HANDLE;
			result = { VK_SUCCESS };
			VkCall(result = vkCreatePipelineLayout(core::logical_device(), &pipelineLayoutInfo, nullptr, &_pipelineLayout), "Failed to create pipeline layout...");
		}

		void vulkan_scene::createDescriptorSets()
		{
			for (auto& instance : _instance_models)
			{
				instance.createDescriptorSet(_descriptorPool, _descriptorSetLayout);
				_descriptorSets.emplace_back(instance.getDescriptorSet());
				auto descriptorSet = instance.getDescriptorSet();

				VkDescriptorBufferInfo bufferInfo;
				bufferInfo.buffer = _uniformBuffer.buffer;
				bufferInfo.offset = 0;
				bufferInfo.range = sizeof(UniformBufferObject);

				//VkDescriptorBufferInfo modelMatrixInfo;
				//bufferInfo.buffer = instance.getUniformBuffer().buffer;
				//bufferInfo.offset = 0;
				//bufferInfo.range = sizeof(math::m4x4);

				VkDescriptorImageInfo imageInfo;
				imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				auto texture_id = materials::get_material(instance.getMaterialID()).getTextureIDS().font();
				imageInfo.imageView = textures::get_texture(texture_id).getTexture().view;
				imageInfo.sampler = textures::get_texture(texture_id).getTexture().sampler;

				std::vector<VkWriteDescriptorSet> descriptorWrites = {
					descriptor::setWriteDescriptorSet(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, descriptorSet, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &bufferInfo, nullptr),
					//descriptor::setWriteDescriptorSet(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, descriptorSet, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &modelMatrixInfo, nullptr),
					descriptor::setWriteDescriptorSet(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, descriptorSet, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, &imageInfo),
				};

				vkUpdateDescriptorSets(core::logical_device(), static_cast<u32>(descriptorWrites.size()), descriptorWrites.data(), 0, nullptr);
			}
		}

		void vulkan_scene::createPipeline(vulkan_renderpass render_pass)
		{
			for (auto& instance : _instance_models)
			{
				instance.createPipeline(_pipelineLayout, render_pass);
				_pipelines.emplace_back(instance.getPipeline());
			}
		}

		void vulkan_scene::updateView(frame_info info)
		{
			_camera_ids.emplace_back(info.camera_id);
			graphics::vulkan::camera::get(info.camera_id).update();
			//DirectX::XMMATRIX modelMatrix = DirectX::XMMatrixIdentity();
			//modelMatrix = DirectX::XMMatrixRotationZ(DirectX::XM_PIDIV2) * modelMatrix;
			//DirectX::XMStoreFloat4x4(&_ubo.model, modelMatrix);
			DirectX::XMStoreFloat4x4(&_ubo.view, graphics::vulkan::camera::get(info.camera_id).view());
			DirectX::XMStoreFloat4x4(&_ubo.projection, graphics::vulkan::camera::get(info.camera_id).projection());
			memcpy(_uniformBuffer.mapped, &_ubo, sizeof(UniformBufferObject));
		}

		void vulkan_scene::flushBuffer(vulkan_cmd_buffer cmd_buffer)
		{
			
			VkDeviceSize offsets[] = { 0 };
			for (auto& instance : _instance_models)
			{
				auto descriptorSet = instance.getDescriptorSet();
				vkCmdBindDescriptorSets(cmd_buffer.cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, _pipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
				vkCmdBindPipeline(cmd_buffer.cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, instance.getPipeline());
				instance.flushBuffer(cmd_buffer);
				instance.draw(cmd_buffer);
			}
		}
		void vulkan_scene::updateUniformBuffer(u32 width, u32 height)
		{
			if (_camera_ids.empty())
			{
				DirectX::XMMATRIX modelMatrix = DirectX::XMMatrixIdentity();
				modelMatrix = DirectX::XMMatrixRotationZ(DirectX::XM_PIDIV2) * modelMatrix;
				DirectX::XMStoreFloat4x4(&_ubo.model, modelMatrix);
				DirectX::XMMATRIX viewMatrix = DirectX::XMMatrixLookAtLH({ 2, 2, 2 }, { 0, 0, 0 }, { 0, 0, 1 });
				DirectX::XMStoreFloat4x4(&_ubo.view, viewMatrix);
				DirectX::XMMATRIX projectionMatrix = DirectX::XMMatrixPerspectiveFovLH(DirectX::XM_PIDIV4, (f32)width / (f32)height, 0.01, 1000.0);
				projectionMatrix.r[1].m128_f32[1] *= -1;
				DirectX::XMStoreFloat4x4(&_ubo.projection, projectionMatrix);
				memcpy(_uniformBuffer.mapped, &_ubo, sizeof(UniformBufferObject));
			}
			else
			{
				DirectX::XMMATRIX modelMatrix = DirectX::XMMatrixIdentity();
				modelMatrix = DirectX::XMMatrixRotationZ(DirectX::XM_PIDIV2) * modelMatrix;
				DirectX::XMStoreFloat4x4(&_ubo.model, modelMatrix);
				DirectX::XMStoreFloat4x4(&_ubo.view, graphics::vulkan::camera::get(_camera_ids.font()).view());
				DirectX::XMStoreFloat4x4(&_ubo.projection, graphics::vulkan::camera::get(_camera_ids.font()).projection());
				memcpy(_uniformBuffer.mapped, &_ubo, sizeof(UniformBufferObject));
			}
		}
		
	} // primai::graphics::vulkan::scene
}