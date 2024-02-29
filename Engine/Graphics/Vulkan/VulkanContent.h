#pragma once
#include "VulkanCommonHeaders.h"
#include "VulkanShader.h"
#include "Graphics/Renderer.h"
#include "VulkanCamera.h"
#include "VulkanGBuffer.h"
#include "Components/Entity.h"

#include <map>

namespace primal::graphics::vulkan
{

	namespace textures
	{
		class vulkan_texture_2d
		{
		public:

			vulkan_texture_2d() = default;

			// DISABLE_COPY(vulkan_texture_2d);

			explicit vulkan_texture_2d(std::string path);

			explicit vulkan_texture_2d(vulkan_texture);

			explicit vulkan_texture_2d(VkImageCreateInfo, VkImageViewCreateInfo);

			explicit vulkan_texture_2d(VkImageCreateInfo, VkImageViewCreateInfo, VkSamplerCreateInfo);

			~vulkan_texture_2d();

			void loadTexture(std::string path);

			void create_sampler(VkSamplerCreateInfo);

			[[nodiscard]] vulkan_texture& getTexture() { return _texture; }

			[[nodiscard]] VkDescriptorImageInfo get_descriptor_info() { return { _texture.sampler, _texture.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }; }

		private:
			vulkan_texture								_texture;
		};

		id::id_type add(std::string path);

		id::id_type add(vulkan_texture);

		id::id_type add(VkImageCreateInfo, VkImageViewCreateInfo);

		id::id_type add(VkImageCreateInfo, VkImageViewCreateInfo, VkSamplerCreateInfo);

		void remove(id::id_type id);

		vulkan_texture_2d& get_texture(id::id_type id);
	}

	namespace materials
	{
		class vulkan_material
		{
		public:
			vulkan_material() = default;

			explicit vulkan_material(material_init_info info) : _type{ info.type }, _texture_count{ info.texture_count }
			{ 
				for (u32 i{ 0 }; i < info.texture_count; ++i)
				{
					_texture_ids.emplace_back(*info.texture_ids);
					info.texture_ids++;
				}

				for (u32 i{ 0 }; i < shader_type::count; ++i)
				{
					if (info.shader_ids[i] == id::invalid_id) continue;
					switch (i)
					{
					case 0:					_shader_ids.at(shader_type::vertex)			= info.shader_ids[i]; break;
					case 1:					_shader_ids.at(shader_type::hull)			= info.shader_ids[i]; break;
					case 2:					_shader_ids.at(shader_type::domain)			= info.shader_ids[i]; break;
					case 3:					_shader_ids.at(shader_type::geometry)		= info.shader_ids[i]; break;
					case 4:					_shader_ids.at(shader_type::pixel)			= info.shader_ids[i]; break;
					case 5:					_shader_ids.at(shader_type::compute)		= info.shader_ids[i]; break;
					case 6:					_shader_ids.at(shader_type::amplification)	= info.shader_ids[i]; break;
					case 7:					_shader_ids.at(shader_type::mesh)			= info.shader_ids[i]; break;
					default:				break;
					}
				}
			}

			~vulkan_material();

			void add_texture(std::string path);
			void add_texture(id::id_type id);

			void remove_texture(id::id_type id);
			void add_shader(std::string path, shader_type::type type);
			void remove_shader(id::id_type id, shader_type::type type);

			[[nodiscard]] utl::vector<id::id_type> getTextureIDS() const { return _texture_ids; }
			[[nodiscard]] constexpr u32 getTextureCount() const { return _texture_count; }
			[[nodiscard]] id::id_type getShaderIDS(shader_type::type type) const { return _shader_ids.at(type); }

		protected:

		private:
			material_type::type											_type;
			u32															_texture_count;
			utl::vector<id::id_type>									_texture_ids;
			std::map<shader_type::type, id::id_type>					_shader_ids{ {shader_type::vertex, id::invalid_id}, {shader_type::hull, id::invalid_id},
																					 {shader_type::domain, id::invalid_id}, {shader_type::geometry, id::invalid_id},
																					 {shader_type::pixel, id::invalid_id}, {shader_type::compute, id::invalid_id}, 
																					 {shader_type::amplification, id::invalid_id}, {shader_type::mesh, id::invalid_id}, };
		};

		id::id_type add(material_init_info info);
		void remove(id::id_type id);
		vulkan_material& get_material(id::id_type id);
	}

	namespace submesh
	{
		class vulkan_model
		{
		public:
			vulkan_model() = default;
			explicit vulkan_model(const void* const data);

			~vulkan_model();

			void create_model_buffer();

			// ! Readonly function -> Two Buffers of Model are both readonly after load model
			[[nodiscard]] constexpr id::id_type const getVertexBuffer() const { return _vertexBuffer_id; }
			[[nodiscard]] constexpr id::id_type const getIndexBuffer() const { return _indexBuffer_id; }
			[[nodiscard]] constexpr u64 const getIndicesCount() const { return _indices.size(); }

		private:
			utl::vector<Vertex>			_vertices;
			utl::vector<u32>			_indices;
			id::id_type					_vertexBuffer_id;
			id::id_type					_indexBuffer_id;
			void create_vertex_buffer();
			void create_index_buffer();
		};

		/// <summary>
		///  继承entity类，通过entity_script修改instance的世界坐标，按照entity_id查询component_cache计算世界坐标并更新对应的UBO
		/// </summary>
		class vulkan_instance_model
		{
		public:
			vulkan_instance_model() = default;

			DISABLE_COPY(vulkan_instance_model);

			explicit vulkan_instance_model(id::id_type model_id);

			explicit vulkan_instance_model(game_entity::entity entity, id::id_type model_id);

			~vulkan_instance_model();

			/// <summary>
			//  ! Create descriptor , bind buffer , draw command
			/// </summary>
			void createDescriptorSet(VkDescriptorPool pool, VkDescriptorSetLayout layout);
			void flushBuffer(vulkan_cmd_buffer cmd_buffer);
			void draw(vulkan_cmd_buffer cmd_buffer);

			void getShaderStage() 
			{
				if (_shaderStages.empty())
				{
					for (u32 i{ 0 }; i < shader_type::count; ++i)
					{
						id::id_type id = materials::get_material(this->_material_id).getShaderIDS((shader_type::type)i);
						if (id == id::invalid_id)	continue;
						_shaderStages.emplace_back(shaders::get_shader(id).getShaderStage());
					}
				} 
			}

			void createPipeline(VkPipelineLayout pipelineLayout, VkRenderPass render_pass);

			/// <summary>
			//  ! add a material to a instance model
			/// </summary>
			/// <param name="id">material ID</param>
			void add_material(id::id_type id) { _material_id = id; _modelData.material_id = id; }
			void remove_material() { _material_id = id::invalid_id; }
			void update_model_data(bool isReflect);
			[[nodiscard]] constexpr id::id_type const getMaterialID() const { return _material_id; }
			[[nodiscard]] constexpr id::id_type const getEntityID() const { return _id; }
			[[nodiscard]] constexpr id::id_type const getPipelineID() const { return _pipeline_id; }
			[[nodiscard]] constexpr id::id_type const getDescriptorSet() const { return _descriptorSet_id; }
			[[nodiscard]] constexpr id::id_type const getLightDescriptorSet() const { return _light_descriptorSet_id; }
			[[nodiscard]] constexpr id::id_type const getModelMatrixID() const { return _modelMatrx_id; }

		private:

			struct model_data
			{
				math::m4x4											model_matrix;
				u32													material_id;
				u32													is_reflect;
			};

			game_entity::entity_id									_id;
			vulkan_model											_model;
			id::id_type												_material_id;
			utl::vector<VkPipelineShaderStageCreateInfo>			_shaderStages;
			id::id_type												_pipeline_id;
			id::id_type												_descriptorSet_id;
			id::id_type												_light_descriptorSet_id;
			model_data												_modelData;
			id::id_type												_modelMatrx_id;

			void create_instance_buffer();
		};

		// TODO: complete the parameter
		// ! When load model to engine, call this function to get vulkan model id
		id::id_type add(const void* const data);
		void remove(id::id_type id);
		vulkan_model get_model(id::id_type id);
	}

	namespace scene
	{
		class vulkan_scene
		{
		public:
			vulkan_scene() = default;

			~vulkan_scene();

			id::id_type add_model_instance(game_entity::entity entity, id::id_type model_id);
			void remove_model_instance(id::id_type id);
			void add_camera(camera_init_info info);
			void remove_camera(id::id_type id);
			void add_material(id::id_type model_id, id::id_type material_id, bool isReflect);
			void remove_material(id::id_type model_id);

			void createUniformBuffer();
			void createDescriptorSets(VkDescriptorPool pool, VkDescriptorSetLayout layout);
			void createDeferDescriptorSets(VkDescriptorPool pool, VkDescriptorSetLayout layout);
			void createPipeline(VkRenderPass render_pass, VkPipelineLayout layout);
			void createDeferPipeline(VkRenderPass render_pass, VkPipelineLayout layout);

			void updateView(frame_info info);
			void flushBuffer(vulkan_cmd_buffer cmd_buffer, VkPipelineLayout layout);

			[[nodiscard]] utl::vector<id::id_type> getInstance() { return _instance_ids; }
			[[nodiscard]] constexpr id::id_type const getUboID() const { return _ubo_id;  }

		private:
			utl::vector<id::id_type>							_instance_ids;
			utl::vector<camera_id>								_camera_ids;
			id::id_type											_ubo_id;
			id::id_type											_pipeline_id;
			id::id_type											_descriptor_set_id;
		};

		submesh::vulkan_instance_model& get_instance(id::id_type);
	}
}