#include "VulkanContent.h"
#include "VulkanHelpers.h"
#include "VulkanResources.h"
#include "VulkanCore.h"
#include "VulkanTexture.h"
#include "VulkanPipeline.h"
#include "VulkanDescriptor.h"
#include "Components/Transform.h"
#define STB_IMAGE_IMPLEMENTATION
#include "Content/stb_image.h"
#include "Utilities/FreeList.h"
#include <iostream>
#define TINYOBJLOADER_IMPLEMENTATION
#include "Content/tiny_obj_loader.h"
#include <unordered_map>
#include <array>

namespace primal::graphics::vulkan
{
	namespace
	{

	} // anonymous namespace

	namespace textures
	{
		namespace
		{
			// ！此freelist存储并生成图片的原始ID
			utl::free_list<textures::vulkan_texture_2d>			textures;

			std::mutex											texture_mutex;
		} // anonymous namespace

		vulkan_texture_2d::vulkan_texture_2d(std::string path)
		{
			loadTexture(path);
		}

		vulkan_texture_2d::vulkan_texture_2d(vulkan_texture tex)
		{
			_texture = tex;
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

		/// <summary>
		// ! 生成图片原始ID
		/// </summary>
		/// <param name="path"></param>
		/// <returns></returns>
		id::id_type add(std::string path)
		{
			return textures.add(path);
		}

		id::id_type add(vulkan_texture tex)
		{
			return textures.add(tex);
		}

		/// <summary>
		// ！ 删除图片原始ID
		///	1、 执行关联的所有material里的remove_texture 操作
		///   2、 执行删除图片原始ID的操作
		/// </summary>
		/// <param name="id"></param>
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
			// ！此freelist存储并生成shader的原始ID
			utl::free_list<shaders::vulkan_shader>				shaders;

			std::mutex											shader_mutex;
		} // anonymous namesapce

		/// <summary>
		// ! 生成shader原始ID
		/// </summary>
		/// <param name="path"></param>
		/// <returns></returns>
		id::id_type add(std::string path, shader_type::type type)
		{
			vulkan_shader shader;
			shader.loadFile(path, type);
			std::lock_guard lock{ shader_mutex };
			return shaders.add(shader);
		}

		/// <summary>
		// ！ 删除shader原始ID
		///	1、 执行关联的所有material里的remove_shader 操作
		/// 2、 执行删除shader原始ID的操作
		/// </summary>
		/// <param name="id"></param>
		void remove(id::id_type id)
		{
			std::lock_guard lock{ shader_mutex };
			assert(id::is_valid(id));
			shaders.remove(id);
		}

		vulkan_shader& get_shader(id::id_type id)
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

			utl::free_list<textures::vulkan_texture_2d>			material_textures;
			utl::free_list<shaders::vulkan_shader>				material_shaders;
		} // anonymous namespace

		vulkan_material::~vulkan_material()
		{
			this->_texture_count = 0;
			/*free(this->_texture_ids);
			this->_texture_ids = nullptr;*/
			this->_texture_ids.clear();
			this->_shader_ids.clear();
		}

		/// <summary>
		///  在一个material内的图片的ID由2部分组成，第一部分是图片的原始ID，第二部分是在material内的copy_id
		///   material内的ID由原始ID作为头16位，copy_id作为后16位
		/// </summary>
		/// <param name="path"></param>
		void vulkan_material::add_texture(std::string path)
		{
			auto texture_id = textures::add(path);
			auto texture = textures::get_texture(texture_id);
			auto copy_id = material_textures.add(texture);
			_texture_ids.emplace_back(copy_id); // (texture_id << 16) | 
			_texture_count++;
		}

		void vulkan_material::add_texture(id::id_type texture_id)
		{
			auto copy_id = material_textures.add(textures::get_texture(texture_id));
			_texture_ids.emplace_back(texture_id); // (texture_id << 16) |
			_texture_count++;
		}

		/// <summary>
		/// 
		/// </summary>
		/// <param name="id">此ID应为material内的组合后的ID</param>
		void vulkan_material::remove_texture(id::id_type id)
		{
			if (_texture_ids.size() < 2) _texture_ids.clear();
			else _texture_ids.erase(id);
			material_textures.remove((id & 0xFFFF));
			_texture_count--;
		}

		void vulkan_material::add_shader(std::string path, shader_type::type type)
		{
			assert(type < shader_type::count);
			if (type > shader_type::count)
			{
				std::cerr << "Type is error" << std::endl;
				return;
			}

			auto shader_id = shaders::add(path, type);
			auto shader = shaders::get_shader(shader_id);
			auto copy_id = material_shaders.add(shader);
			_shader_ids.at(type) = (shader_id << 16) | copy_id;
		}

		void vulkan_material::remove_shader(id::id_type id, shader_type::type type)
		{
			if (id != _shader_ids.at(type)) std::cerr << "Shader id  error!" << std::endl;
			material_shaders.remove(id & 0xFFFF);
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

		vulkan_material& get_material(id::id_type id)
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
						1.0f - attrib.texcoords[2 * index.texcoord_index + 1], // 1.0f - attrib.texcoords[2 * index.texcoord_index + 1]
						0.0f
					};

					vertex.normal = math::v3{
						attrib.normals[3 * index.normal_index + 0],
						attrib.normals[3 * index.normal_index + 1],
						attrib.normals[3 * index.normal_index + 2]
					};

					if (uniqueVertices.count(index) == 0)
					{
						uniqueVertices[index] = (u32)_vertices.size();
						_vertices.emplace_back(vertex);
					}

					_indices.emplace_back((u16)uniqueVertices[index]);
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
			std::vector<InstanceData> datas;
			//datas.resize(_model.getIndicesCount());
			for(u32 i{0}; i < _model.getIndicesCount(); ++i)
			{
				datas.emplace_back(_instanceData);
			}
			size_t bufferSize = sizeof(InstanceData) * datas.size();

			VkBuffer stagingBuffer;
			VkDeviceMemory stagingBufferMemory;
			createBuffer(core::logical_device(), bufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, stagingBuffer, stagingBufferMemory);

			void* data;
			vkMapMemory(core::logical_device(), stagingBufferMemory, 0, bufferSize, 0, &data);
			memcpy(data, datas.data(), bufferSize);
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

		vulkan_instance_model::vulkan_instance_model(game_entity::entity entity, id::id_type model_id) : _model{ get_model(model_id) }, _id{ entity.get_id() }
		{
			_model.create_model_buffer();
			_instanceData.transform = entity.position();
			_instanceData.rotate = { entity.rotation().x, entity.rotation().y, entity.rotation().z };
			_instanceData.scale = entity.scale();
			create_instance_buffer();
		}

		vulkan_instance_model::~vulkan_instance_model()
		{
			vkDeviceWaitIdle(core::logical_device());

			pipeline::remove_pipeline(_pipeline_ids.at(render_type::forward).font());

			for (auto texture_id : materials::get_material(_material_id).getTextureIDS())
			{
				//textures::get_texture(texture_id).~vulkan_texture_2d();
				//textures::remove(texture_id);
				materials::get_material(_material_id).remove_texture(texture_id);
			}

			vkDestroyBuffer(core::logical_device(), _instanceBuffer.buffer, nullptr);
			vkFreeMemory(core::logical_device(), _instanceBuffer.memory, nullptr);

			_model.~vulkan_model();
		}

		void vulkan_instance_model::createDescriptorSet(VkDescriptorPool pool, VkDescriptorSetLayout layout)
		{
			VkDescriptorSetAllocateInfo allocInfo;

			allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
			allocInfo.pNext = VK_NULL_HANDLE;
			allocInfo.descriptorPool = pool;
			allocInfo.descriptorSetCount = 1;
			allocInfo.pSetLayouts = &layout;

			_descriptorSet_ids[render_type::forward].emplace_back(descriptor::add(allocInfo));
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
			vkCmdDrawIndexed(cmd_buffer.cmd_buffer, (u32)_model.getIndicesCount(), 1, 0, 0, 0);
		}

		void vulkan_instance_model::createPipeline(VkPipelineLayout pipelineLayou, vulkan_renderpass render_pass)
		{
			VkPipelineInputAssemblyStateCreateInfo inputAssemblyState = descriptor::pipelineInputAssemblyStateCreate(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, VK_FALSE);
			VkPipelineViewportStateCreateInfo viewportState = descriptor::pipelineViewportStateCreate(1, 1);
			VkPipelineRasterizationStateCreateInfo rasterizationState = descriptor::pipelineRasterizationStateCreate(VK_POLYGON_MODE_FILL, VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_COUNTER_CLOCKWISE);
			VkPipelineMultisampleStateCreateInfo multisampleState = descriptor::pipelineMultisampleStateCreate(VK_SAMPLE_COUNT_1_BIT);
			VkPipelineColorBlendAttachmentState blendAttachmentState = descriptor::pipelineColorBlendAttachmentState(VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT, VK_FALSE);
			VkPipelineColorBlendStateCreateInfo colorBlendState = descriptor::pipelineColorBlendStateCreate(1, blendAttachmentState);

			std::vector<VkDynamicState> dynamicStateEnables{ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
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

			_pipeline_ids[render_type::forward].emplace_back(pipeline::add_pipeline(pipelineCI));

		}

		id::id_type add(std::string path)
		{
			return _models.add(path);
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
			utl::free_list<submesh::vulkan_instance_model>		instance_models;
		}

		vulkan_scene::~vulkan_scene()
		{
			for (auto instance : _instance_ids)
			{
				remove_model_instance(instance);
			}
			_shadowmap.~vulkan_shadowmapping();
		}

		id::id_type vulkan_scene::add_model_instance(game_entity::entity entity, id::id_type model_id)
		{
			auto instance_id = instance_models.add(entity, model_id);
			_instance_ids.emplace_back(instance_id);
			return instance_id;
		}

		void vulkan_scene::remove_model_instance(id::id_type id)
		{
			//_instance_models.erase(id);
			_instance_ids.erase(id);
			instance_models[id].~vulkan_instance_model();
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
			instance_models[model_id].add_material(material_id);
		}

		void vulkan_scene::remove_material(id::id_type model_id)
		{
			instance_models[model_id].remove_material();
		}

		void vulkan_scene::createUniformBuffer() {
			VkDeviceSize bufferSize = sizeof(UniformBufferObjectPlus);

			createBuffer(core::logical_device(), bufferSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, _uniformBuffer.buffer, _uniformBuffer.memory);

			vkMapMemory(core::logical_device(), _uniformBuffer.memory, 0, bufferSize, 0, &_uniformBuffer.mapped);
		}

		void vulkan_scene::createDescriptorSets(VkDescriptorPool pool, VkDescriptorSetLayout layout)
		{
			for (auto& instance : _instance_ids)
			{
				instance_models[instance].createDescriptorSet(pool, layout);
				auto descriptorSet = descriptor::get(instance_models[instance].get_descriptor_set_ids(render_type::forward).font());
				std::vector<VkWriteDescriptorSet> descriptorWrites;

				VkDescriptorBufferInfo bufferInfo;
				bufferInfo.buffer = _uniformBuffer.buffer;
				bufferInfo.offset = 0;
				bufferInfo.range = sizeof(UniformBufferObjectPlus);
				descriptorWrites.emplace_back(descriptor::setWriteDescriptorSet(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, descriptorSet, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &bufferInfo));

				if (!materials::get_material(instance_models[instance].getMaterialID()).getTextureIDS().empty())
				{
					u32 count{ 0 };
					for (u32 i{ 0 }; i < materials::get_material(instance_models[instance].getMaterialID()).getTextureIDS().size(); ++i)
					{
						VkDescriptorImageInfo imageInfo;
						imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
						auto texture_id = materials::get_material(instance_models[instance].getMaterialID()).getTextureIDS()[i];
						imageInfo.imageView = textures::get_texture(texture_id).getTexture().view;
						imageInfo.sampler = textures::get_texture(texture_id).getTexture().sampler;
						descriptorWrites.emplace_back(descriptor::setWriteDescriptorSet(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, descriptorSet, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &imageInfo));
						count++;
					}
				}

				VkDescriptorImageInfo shadowInfo;
				shadowInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL;
				shadowInfo.imageView = _shadowmap.getTexture().view;
				shadowInfo.sampler = _shadowmap.getTexture().sampler;
				descriptorWrites.emplace_back(descriptor::setWriteDescriptorSet(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, descriptorSet, 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &shadowInfo));

				vkUpdateDescriptorSets(core::logical_device(), static_cast<u32>(descriptorWrites.size()), descriptorWrites.data(), 0, nullptr);
			}
		}

		void vulkan_scene::createDeferDescriptorSets(VkDescriptorPool pool, VkDescriptorSetLayout layout)
		{
			VkDescriptorSetAllocateInfo allocInfo;

			allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
			allocInfo.pNext = VK_NULL_HANDLE;
			allocInfo.descriptorPool = pool;
			allocInfo.descriptorSetCount = 1;
			allocInfo.pSetLayouts = &layout;

			_descriptor_set_id = descriptor::add(allocInfo);

			auto descriptorSet = descriptor::get(_descriptor_set_id);
			std::vector<VkWriteDescriptorSet> descriptorWrites;

			VkDescriptorImageInfo imageInfo1;
			imageInfo1.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			imageInfo1.imageView = textures::get_texture(_offscreen.getTexture()[0]).getTexture().view;
			imageInfo1.sampler = textures::get_texture(_offscreen.getTexture()[0]).getTexture().sampler;
			descriptorWrites.emplace_back(descriptor::setWriteDescriptorSet(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, descriptorSet, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &imageInfo1));

			VkDescriptorImageInfo imageInfo2;
			imageInfo2.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			imageInfo2.imageView = textures::get_texture(_offscreen.getTexture()[1]).getTexture().view;
			imageInfo2.sampler = textures::get_texture(_offscreen.getTexture()[1]).getTexture().sampler;
			descriptorWrites.emplace_back(descriptor::setWriteDescriptorSet(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, descriptorSet, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &imageInfo2));

			VkDescriptorImageInfo imageInfo3;
			imageInfo3.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			imageInfo3.imageView = textures::get_texture(_offscreen.getTexture()[2]).getTexture().view;
			imageInfo3.sampler = textures::get_texture(_offscreen.getTexture()[2]).getTexture().sampler;
			descriptorWrites.emplace_back(descriptor::setWriteDescriptorSet(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, descriptorSet, 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &imageInfo3));

			VkDescriptorImageInfo imageInfo4;
			imageInfo4.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			imageInfo4.imageView = textures::get_texture(_offscreen.getTexture()[4]).getTexture().view;
			imageInfo4.sampler = textures::get_texture(_offscreen.getTexture()[4]).getTexture().sampler;
			descriptorWrites.emplace_back(descriptor::setWriteDescriptorSet(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, descriptorSet, 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &imageInfo4));

			vkUpdateDescriptorSets(core::logical_device(), static_cast<u32>(descriptorWrites.size()), descriptorWrites.data(), 0, nullptr);
		}

		void vulkan_scene::createPipeline(vulkan_renderpass render_pass, VkPipelineLayout layout)
		{
			for (auto& instance : _instance_ids)
			{
				instance_models[instance].createPipeline(layout, render_pass);
			}
		}

		void vulkan_scene::createDeferPipeline(vulkan_renderpass render_pass, VkPipelineLayout layout)
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
			pipelineCreateInfo.layout = layout;
			pipelineCreateInfo.pNext = nullptr;
			pipelineCreateInfo.renderPass = render_pass.render_pass;
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
			_pipeline_id = pipeline::add_pipeline(pipelineCreateInfo);
		}

		void vulkan_scene::updateView(frame_info info)
		{
			_camera_ids.emplace_back(info.camera_id);
			graphics::vulkan::camera::get(info.camera_id).update();
			DirectX::XMMATRIX modelMatrix = DirectX::XMMatrixIdentity();
			DirectX::XMStoreFloat4x4(&_ubo.model, modelMatrix);
			DirectX::XMStoreFloat4x4(&_ubo.view, graphics::vulkan::camera::get(info.camera_id).view());
			DirectX::XMStoreFloat4x4(&_ubo.projection, graphics::vulkan::camera::get(info.camera_id).projection());
			_ubo.lightModel = _shadowmap.getDepthMVP().model;
			_ubo.lightView = _shadowmap.getDepthMVP().view;
			_ubo.lightProjection = _shadowmap.getDepthMVP().projection;
			_ubo.lightPos = _shadowmap.lightPos();
			_ubo.lightNear = _shadowmap.lightNear();
			_ubo.lightFar = _shadowmap.lightFar();
			uboGBuffer ubo1;
			DirectX::XMStoreFloat4x4(&ubo1.model, modelMatrix);
			DirectX::XMStoreFloat4x4(&ubo1.view, graphics::vulkan::camera::get(info.camera_id).view());
			DirectX::XMStoreFloat4x4(&ubo1.projection, graphics::vulkan::camera::get(info.camera_id).projection());
			ubo1.nearPlane = graphics::vulkan::camera::get(info.camera_id).near_z();
			ubo1.farPlane = graphics::vulkan::camera::get(info.camera_id).far_z();
			memcpy(_uniformBuffer.mapped, &_ubo, sizeof(UniformBufferObjectPlus));
			memcpy(_offscreen.get_uniform_buffer().mapped, &ubo1, sizeof(uboGBuffer));
			uboSSAOParam ubo2;
			ubo2.ssao = 1;
			ubo2.ssaoOnly = 0;
			ubo2.ssaoblur = 0;
			DirectX::XMStoreFloat4x4(&ubo2.projection, graphics::vulkan::camera::get(info.camera_id).projection());
			memcpy(_offscreen.get_ssao_param().mapped, &ubo2, sizeof(uboSSAOParam));
		}

		void vulkan_scene::flushBuffer(vulkan_cmd_buffer cmd_buffer, VkPipelineLayout layout)
		{
			
			for (auto& instance : _instance_ids)
			{
				auto descriptorSet = descriptor::get(instance_models[instance].get_descriptor_set_ids(render_type::forward).font());
				auto pipeline = pipeline::get_pipeline(instance_models[instance].get_pipeline_ids(render_type::forward).font());
				vkCmdBindDescriptorSets(cmd_buffer.cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &descriptorSet, 0, nullptr);
				vkCmdBindPipeline(cmd_buffer.cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
				instance_models[instance].flushBuffer(cmd_buffer);
				instance_models[instance].draw(cmd_buffer);
			}
		}

		void vulkan_scene::drawGBuffer(vulkan_cmd_buffer cmd_buffer)
		{
			for (auto& instance : _instance_ids)
			{
				instance_models[instance].flushBuffer(cmd_buffer);
				instance_models[instance].draw(cmd_buffer);
			}
		}

		void vulkan_scene::drawDefer(vulkan_cmd_buffer cmd_buffer, VkPipelineLayout layout)
		{
			auto descriptorSet = descriptor::get(_descriptor_set_id);
			auto pipeline = pipeline::get_pipeline(_pipeline_id);
			vkCmdBindDescriptorSets(cmd_buffer.cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &descriptorSet, 0, nullptr);
			vkCmdBindPipeline(cmd_buffer.cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
			vkCmdDraw(cmd_buffer.cmd_buffer, 3, 1, 0, 0);
		}
		
	} // primai::graphics::vulkan::scene
}