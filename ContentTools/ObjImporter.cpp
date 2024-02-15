#include "ObjImporter.h"

#define TINYOBJLOADER_IMPLEMENTATION
#include "Content/tiny_obj_loader.h"
#include <unordered_map>
#include <direct.h>
#include <io.h>

#include <stdio.h>
#include <fstream>
#include <iostream>
#include <map>
#include "../Engine/Utilities/IOStream.h"
#include "TemplateShader/PBR_Template_Shader_v1.h"

namespace primal::tools
{
	namespace
	{
		std::mutex		obj_mutex{};

		std::string base_path{ "C:\\Users\\zy\\Desktop\\PrimalMerge\\PrimalEngine" };

		std::string out_path = base_path + std::string{ "\\EngineTest\\assets\\kmt\\" };

		std::string kmt_ext{ ".kmt" };

		std::string out_model_path = base_path + std::string{ "\\EngineTest\\assets\\kms\\" };

		std::string out_shader_path = base_path + std::string{ "\\Engine\\Graphics\\Vulkan\\Shaders\\" };

		std::string kms_ext{ ".kms" };

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

		bool write_kms_file(const char* file_package, const char* filename, u32 count, utl::vector<geometry_config>& out_geometry_darray)
		{
			std::fstream outfile;

			std::string file_package_path{ file_package };

			//if(_access(file_package_path.c_str(), 0) == -1)
			//	OutputDebugStringA(std::to_string(_mkdir(file_package_path.c_str())).c_str());

			std::string fullpath = file_package_path.append("\\").append(filename).append(kms_ext);

			outfile.open(fullpath.c_str(), std::ios::out | std::ios::app | std::ios::binary);

			if (!outfile.is_open()) return false;

			// Geometry count
			outfile.write(reinterpret_cast<char*>(&count), sizeof(u32));

			// Each geometry
			for (u32 i{ 0 }; i < count; ++i)
			{
				geometry_config* g = &out_geometry_darray[i];

				// Vertices (size/count/array)
				outfile.write(reinterpret_cast<char*>(&g->vertex_size), sizeof(u32));
				outfile.write(reinterpret_cast<char*>(&g->vertex_count), sizeof(u32));
				for (u32 x{ 0 }; x < g->vertex_count; ++x)
				{
					outfile.write(reinterpret_cast<char*>(&g->vertices[x]), sizeof(Vertex));
				}

				// Indices (size/count/array)
				outfile.write(reinterpret_cast<char*>(&g->index_size), sizeof(u32));
				outfile.write(reinterpret_cast<char*>(&g->index_count), sizeof(u32));
				for (u32 y{ 0 }; y < g->index_count; ++y)
				{
					outfile.write(reinterpret_cast<char*>(&g->indices[y]), sizeof(u32));
				}

				// Name  
				u32 g_name_length = (u32)strlen(g->name) + 1;
				outfile.write(reinterpret_cast<char*>(&g_name_length), sizeof(u32));
				outfile.write(g->name, g_name_length * sizeof(char));

				// Material Name
				u32 m_name_length = (u32)strlen(g->material_name) + 1;
				outfile.write(reinterpret_cast<char*>(&m_name_length), sizeof(u32));
				outfile.write(g->material_name, m_name_length * sizeof(char));

				// Ambient map Name
				u32 ambient_map_length = (u32)g->ambient_map.length();
				outfile.write(reinterpret_cast<char*>(&ambient_map_length), sizeof(u32));
				outfile.write(g->ambient_map.c_str(), ambient_map_length * sizeof(char));

				// Diffuse map Name
				u32 diffuse_map_length = (u32)g->diffuse_map.length();
				outfile.write(reinterpret_cast<char*>(&diffuse_map_length), sizeof(u32));
				outfile.write(g->diffuse_map.c_str(), diffuse_map_length * sizeof(char));

				// Specular map Name
				u32 specular_map_length = (u32)g->specular_map.length();
				outfile.write(reinterpret_cast<char*>(&specular_map_length), sizeof(u32));
				outfile.write(g->specular_map.c_str(), specular_map_length * sizeof(char));

				// Alpha map Name
				u32 alpha_map_length = (u32)g->alpha_map.length();
				outfile.write(reinterpret_cast<char*>(&alpha_map_length), sizeof(u32));
				outfile.write(g->alpha_map.c_str(), alpha_map_length * sizeof(char));

				// Normal map Name
				u32 normal_map_length = (u32)g->normal_map.length();
				outfile.write(reinterpret_cast<char*>(&normal_map_length), sizeof(u32));
				outfile.write(g->normal_map.c_str(), normal_map_length * sizeof(char));

				// Center
				outfile.write(reinterpret_cast<char*>(&g->center), sizeof(math::v3));

				// Extents (min/max)
				outfile.write(reinterpret_cast<char*>(&g->min_extents), sizeof(math::v3));
				outfile.write(reinterpret_cast<char*>(&g->max_extents), sizeof(math::v3));
			}

			outfile.close();

			return true;
		}

		bool write_kms_file(const char* file_package, const char* filename, geometry_config& out_geometry_darray)
		{
			std::fstream outfile;

			std::string file_package_path{ file_package };

			//if(_access(file_package_path.c_str(), 0) == -1)
			//	OutputDebugStringA(std::to_string(_mkdir(file_package_path.c_str())).c_str());

			std::string fullpath = file_package_path.append("\\").append(filename).append(kms_ext);

			outfile.open(fullpath.c_str(), std::ios::out | std::ios::app | std::ios::binary);

			if (!outfile.is_open()) return false;

			// Geometry count
			u32 count{ 1 };
			outfile.write(reinterpret_cast<char*>(&count), sizeof(u32));

			// Each geometry
			for (u32 i{ 0 }; i < 1; ++i)
			{
				geometry_config* g = &out_geometry_darray;

				// Vertices (size/count/array)
				outfile.write(reinterpret_cast<char*>(&g->vertex_size), sizeof(u32));
				outfile.write(reinterpret_cast<char*>(&g->vertex_count), sizeof(u32));
				for (u32 x{ 0 }; x < g->vertex_count; ++x)
				{
					outfile.write(reinterpret_cast<char*>(&g->vertices[x]), sizeof(Vertex));
				}

				// Indices (size/count/array)
				outfile.write(reinterpret_cast<char*>(&g->index_size), sizeof(u32));
				outfile.write(reinterpret_cast<char*>(&g->index_count), sizeof(u32));
				for (u32 y{ 0 }; y < g->index_count; ++y)
				{
					outfile.write(reinterpret_cast<char*>(&g->indices[y]), sizeof(u32));
				}

				// Name  
				u32 g_name_length = (u32)strlen(g->name) + 1;
				outfile.write(reinterpret_cast<char*>(&g_name_length), sizeof(u32));
				outfile.write(g->name, g_name_length * sizeof(char));

				// Material Name
				u32 m_name_length = (u32)strlen(g->material_name) + 1;
				outfile.write(reinterpret_cast<char*>(&m_name_length), sizeof(u32));
				outfile.write(g->material_name, m_name_length * sizeof(char));

				// Ambient map Name
				u32 ambient_map_length = (u32)g->ambient_map.length();
				outfile.write(reinterpret_cast<char*>(&ambient_map_length), sizeof(u32));
				outfile.write(g->ambient_map.c_str(), ambient_map_length * sizeof(char));

				// Diffuse map Name
				u32 diffuse_map_length = (u32)g->diffuse_map.length();
				outfile.write(reinterpret_cast<char*>(&diffuse_map_length), sizeof(u32));
				outfile.write(g->diffuse_map.c_str(), diffuse_map_length * sizeof(char));

				// Specular map Name
				u32 specular_map_length = (u32)g->specular_map.length();
				outfile.write(reinterpret_cast<char*>(&specular_map_length), sizeof(u32));
				outfile.write(g->specular_map.c_str(), specular_map_length * sizeof(char));

				// Alpha map Name
				u32 alpha_map_length = (u32)g->alpha_map.length();
				outfile.write(reinterpret_cast<char*>(&alpha_map_length), sizeof(u32));
				outfile.write(g->alpha_map.c_str(), alpha_map_length * sizeof(char));

				// Normal map Name
				u32 normal_map_length = (u32)g->normal_map.length();
				outfile.write(reinterpret_cast<char*>(&normal_map_length), sizeof(u32));
				outfile.write(g->normal_map.c_str(), normal_map_length * sizeof(char));

				// Center
				outfile.write(reinterpret_cast<char*>(&g->center), sizeof(math::v3));

				// Extents (min/max)
				outfile.write(reinterpret_cast<char*>(&g->min_extents), sizeof(math::v3));
				outfile.write(reinterpret_cast<char*>(&g->max_extents), sizeof(math::v3));
			}

			outfile.close();

			return true;
		}

		void generate_bounding_box_and_center(geometry_config* geo)
		{
			geo->min_extents = math::v3{ 0, 0, 0 };
			geo->max_extents = math::v3{ 0, 0, 0 };

			for (u32 i{ 0 }; i < geo->vertex_count; ++i)
			{
				if (geo->vertices[i].pos.x < geo->min_extents.x)
				{
					geo->min_extents.x = geo->vertices[i].pos.x;
				}
				if (geo->vertices[i].pos.y < geo->min_extents.y)
				{
					geo->min_extents.y = geo->vertices[i].pos.y;
				}
				if (geo->vertices[i].pos.z < geo->min_extents.z)
				{
					geo->min_extents.z = geo->vertices[i].pos.z;
				}

				if (geo->vertices[i].pos.x > geo->max_extents.x)
				{
					geo->max_extents.x = geo->vertices[i].pos.x;
				}
				if (geo->vertices[i].pos.y > geo->max_extents.y)
				{
					geo->max_extents.y = geo->vertices[i].pos.y;
				}
				if (geo->vertices[i].pos.z > geo->max_extents.z)
				{
					geo->max_extents.z = geo->vertices[i].pos.z;
				}
			}
			geo->center.x = (geo->min_extents.x + geo->max_extents.x) / 2.f;
			geo->center.y = (geo->min_extents.y + geo->max_extents.y) / 2.f;
			geo->center.z = (geo->min_extents.z + geo->max_extents.z) / 2.f;
		}

		void generate_tangents(geometry_config* geos)
		{
			assert(geos->index_count % 3 == 0);
			using namespace DirectX;
			for (u32 i{ 0 }; i < geos->index_count; i += 3)
			{
				u32 i0 = geos->indices[i];
				u32 i1 = geos->indices[i + 1];
				u32 i2 = geos->indices[i + 2];

				math::v3 edge1{ geos->vertices[i1].pos.x - geos->vertices[i0].pos.x,
					geos->vertices[i1].pos.y - geos->vertices[i0].pos.y,
					geos->vertices[i1].pos.z - geos->vertices[i0].pos.z };
				math::v3 edge2{ geos->vertices[i2].pos.x - geos->vertices[i0].pos.x,
					geos->vertices[i2].pos.y - geos->vertices[i0].pos.y,
					geos->vertices[i2].pos.z - geos->vertices[i0].pos.z };

				f32 deltaU1 = geos->vertices[i1].texCoord.x - geos->vertices[i0].texCoord.x;
				f32 deltaV1 = geos->vertices[i1].texCoord.y - geos->vertices[i0].texCoord.y;

				f32 deltaU2 = geos->vertices[i2].texCoord.x - geos->vertices[i0].texCoord.x;
				f32 deltaV2 = geos->vertices[i2].texCoord.y - geos->vertices[i0].texCoord.y;

				f32 dividend = (deltaU1 * deltaV2 - deltaU2 * deltaV1);
				f32 fc = 1.f / dividend;

				math::v3 tangent{ fc * (deltaV2 * edge1.x - deltaV1 * edge2.x),
					fc * (deltaV2 * edge1.y - deltaV1 * edge2.y),
					fc * (deltaV2 * edge1.z - deltaV1 * edge2.z)};

				XMVECTOR tang = XMLoadFloat3(&tangent);
				tang = XMVector3Normalize(tang);
				XMStoreFloat3(&tangent, tang);

				f32 sx = deltaU1, sy = deltaU2;
				f32 tx = deltaV1, ty = deltaV2;
				f32 handedness = ((tx * sy - ty * sx) < 0.f) ? -1.f : 1.f;
				math::v3 t4{ tangent.x * handedness, tangent.y * handedness, tangent.z * handedness };
				geos->vertices[i0].tangent = t4;
				geos->vertices[i1].tangent = t4;
				geos->vertices[i2].tangent = t4;
			}
		}

		bool generate_shader(const void* const data, const char* file_package)
		{
			tinyobj::material_t material_data{ *(tinyobj::material_t*)data };

			{
				std::string out_vertex_shader_name{ file_package };
				out_vertex_shader_name.append("//").append("shaders");
				if (_access(out_vertex_shader_name.c_str(), 0) == -1)
					OutputDebugStringA(std::to_string(_mkdir(out_vertex_shader_name.c_str())).c_str());
				out_vertex_shader_name.append("\\").append(material_data.name.c_str()).append(".vert");
				std::ofstream vert_shader{ out_vertex_shader_name };
				if (!vert_shader.is_open())
				{
					OutputDebugStringA("Failed to open vertex shader to write!");
					return false;
				}
				vert_shader << PBR_Template_Vertex_Shader;
				vert_shader.close();
			}

			{
				std::string out_fragment_shader_name{ file_package };
				out_fragment_shader_name.append("//").append("shaders");
				if (_access(out_fragment_shader_name.c_str(), 0) == -1)
					OutputDebugStringA(std::to_string(_mkdir(out_fragment_shader_name.c_str())).c_str());
				out_fragment_shader_name.append("\\").append(material_data.name.c_str()).append(".frag");
				std::ofstream fragment_shader{ out_fragment_shader_name };
				if (!fragment_shader.is_open())
				{
					OutputDebugStringA("Failed to open fragment shader to write!");
					return false;
				}

				std::string fragment_template_string{ PBR_Template_Fragment_Shader };

				size_t ns_pos = fragment_template_string.find("{{Ns}}");
				if (ns_pos != std::string::npos)
				{
					std::string ns{ std::to_string(material_data.shininess) };
					fragment_template_string.replace(ns_pos, 6, ns);
				}

				size_t ni_pos = fragment_template_string.find("{{Ni}}");
				if (ni_pos != std::string::npos)
				{
					std::string ni{ std::to_string(material_data.ior) };
					fragment_template_string.replace(ni_pos, 6, ni);
				}

				size_t d_pos = fragment_template_string.find("{{d}}");
				if (d_pos != std::string::npos)
				{
					std::string d{ std::to_string(material_data.dissolve) };
					fragment_template_string.replace(d_pos, 5, d);
				}

				size_t tr_pos = fragment_template_string.find("{{Tr}}");
				if (tr_pos != std::string::npos)
				{
					std::string tr{ std::to_string(1.f - material_data.dissolve) };
					fragment_template_string.replace(tr_pos, 6, tr);
				}

				size_t tf_pos = fragment_template_string.find("{{Tf}}");
				if (tf_pos != std::string::npos)
				{
					std::string tf;
					tf.append("vec3(").append(std::to_string(material_data.transmittance[0])).append(",").append(std::to_string(material_data.transmittance[1])).append(",")
						.append(std::to_string(material_data.transmittance[2])).append(")");
					fragment_template_string.replace(tf_pos, 6, tf);
				}

				size_t ka_pos = fragment_template_string.find("{{Ka}}");
				if (ka_pos != std::string::npos)
				{
					std::string ka;
					ka.append("vec3(").append(std::to_string(material_data.ambient[0])).append(",").append(std::to_string(material_data.ambient[1])).append(",")
						.append(std::to_string(material_data.ambient[2])).append(")");
					fragment_template_string.replace(ka_pos, 6, ka);
				}

				size_t kd_pos = fragment_template_string.find("{{Kd}}");
				if (kd_pos != std::string::npos)
				{
					std::string kd;
					kd.append("vec3(").append(std::to_string(material_data.diffuse[0])).append(",").append(std::to_string(material_data.diffuse[1])).append(",")
						.append(std::to_string(material_data.diffuse[2])).append(")");
					fragment_template_string.replace(kd_pos, 6, kd);
				}

				size_t ks_pos = fragment_template_string.find("{{Ks}}");
				if (ks_pos != std::string::npos)
				{
					std::string ks;
					ks.append("vec3(").append(std::to_string(material_data.specular[0])).append(",").append(std::to_string(material_data.specular[1])).append(",")
						.append(std::to_string(material_data.specular[2])).append(")");
					fragment_template_string.replace(ks_pos, 6, ks);
				}

				size_t ke_pos = fragment_template_string.find("{{Ke}}");
				if (ks_pos != std::string::npos)
				{
					std::string ke;
					ke.append("vec3(").append(std::to_string(material_data.emission[0])).append(",").append(std::to_string(material_data.emission[1])).append(",")
						.append(std::to_string(material_data.emission[2])).append(")");
					fragment_template_string.replace(ke_pos, 6, ke);
				}

				size_t dc_pos = fragment_template_string.find("{{diffuse_color}}");
				if (dc_pos != std::string::npos)
				{
					std::string dc;
					dc.append("vec3(").append(std::to_string(material_data.diffuse[0])).append(",").append(std::to_string(material_data.diffuse[1])).append(",")
						.append(std::to_string(material_data.diffuse[2])).append(")");
					fragment_template_string.replace(dc_pos, 17, dc);
				}

				size_t image_pos = fragment_template_string.find("{{images}}");
				if (image_pos != std::string::npos)
				{
					std::string image;
					u32 count{ 0 };
					if (!material_data.diffuse_texname.empty())
					{
						//image.append("layout(set = 0, binding = ").append(std::to_string(count).c_str()).append(") uniform sampler2D diffuseMap;\n");
						count++;
					}
					else
					{
						std::string re{ "texture(samplers[SAMP_DIFFUSE], in_dto.tex_coord)" };
						size_t re_pos;
						while ((re_pos = fragment_template_string.find(re)) != std::string::npos)
						{
							fragment_template_string.replace(fragment_template_string.find(re), re.length(), "vec4(1.0)");
						}
					}
					if (!material_data.specular_texname.empty())
					{
						//image.append("layout(set = 0, binding = ").append(std::to_string(count).c_str()).append(") uniform sampler2D specularMap;\n");
						count++;
					}
					else
					{
						std::string re{ "texture(samplers[SAMP_SPECULAR], in_dto.tex_coord).rgb" };
						size_t re_pos;
						while ((re_pos = fragment_template_string.find(re)) != std::string::npos)
						{
							fragment_template_string.replace(fragment_template_string.find(re), re.length(), "vec3(1.0)");
						}
					}
					if (!material_data.bump_texname.empty())
					{
						//image.append("layout(set = 0, binding = ").append(std::to_string(count).c_str()).append(") uniform sampler2D normalMap;\n");
						count++;
					}
					else
					{
						std::string re{ "texture(samplers[SAMP_NORMAL], in_dto.tex_coord).rgb" };
						size_t re_pos;
						while ((re_pos = fragment_template_string.find(re)) != std::string::npos)
						{
							fragment_template_string.replace(fragment_template_string.find(re), re.length(), "vec3(1.0)");
						}
					}

					if (count == 0)
					{
						fragment_template_string.replace(image_pos, 10, "");
					}
					else
					{
						image.append("layout(set = 0, binding = 2) uniform sampler2D samplers[").append(std::to_string(count)).append("];");
						fragment_template_string.replace(image_pos, 10, image);
					}
				}

				fragment_shader << fragment_template_string;
				fragment_shader.close();
			}
			return true;
		}

		void copy_shader_header(const char* file_package)
		{
			std::string common_src{ "C:/Users/zy/Desktop/PrimalMerge/PrimalEngine/Engine/Graphics/Vulkan/Shaders/Common.h" };
			std::string common_dst{ file_package };
			common_dst.append("\\").append("shaders\\").append("Common.h");

			std::ifstream src(common_src, std::ios::binary);
			std::ofstream dst(common_dst, std::ios::binary);

			dst << src.rdbuf();

			std::string shadertype_src{ "C:/Users/zy/Desktop/PrimalMerge/PrimalEngine/Engine/Graphics/Vulkan/Shaders/CommonTypes.glsli" };
			std::string shadertype_dst{ file_package };
			shadertype_dst.append("\\").append("shaders\\").append("CommonTypes.glsli");

			std::ifstream shadertypesrc(shadertype_src, std::ios::binary);
			std::ofstream shadertypedst(shadertype_dst, std::ios::binary);

			shadertypedst << shadertypesrc.rdbuf();
		}
	} // anonymous namespace

	bool load_obj_model(std::string path, const char* out_ksm_file_package)
	{
		size_t pos = path.rfind("/", path.length());
		std::string base_file_path{ path.substr(0, pos) };
		// size_t last{ path.find_last_of("/\\") };
		// size_t ext_string{ path.find_last_of(".") };
		//std::string filename;
		//if (last != std::string::npos)
		//{
		//	filename = path.substr(last + 1);
		//	size_t ext_string{ filename.find_last_of(".") };
		//	filename = filename.substr(0, ext_string);
		//}

		std::string file_package_path{ out_model_path + std::string{ out_ksm_file_package } };
		if (_access(file_package_path.c_str(), 0) == -1)
			OutputDebugStringA(std::to_string(_mkdir(file_package_path.c_str())).c_str());

		tinyobj::attrib_t attrib;
		std::vector<tinyobj::shape_t> shapes;
		std::vector<tinyobj::material_t> materials;
		std::string warn, err;

		utl::vector<Vertex>	globalvertices;
		utl::vector<geometry_config> out_geometry_darray;

		if (!tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, path.c_str(), base_file_path.c_str()))
		{
			throw std::runtime_error(warn + err);
		}

		for (auto material : materials)
		{
			if (!generate_shader(static_cast<void*>(&material), file_package_path.c_str()))
			{
				OutputDebugStringA("Failed to write shader");
			}
		}

		copy_shader_header(file_package_path.c_str());

		std::unordered_map<tinyobj::index_t, size_t, hash_idx, equal_idx> uniqueVertices;
		std::map<u32, geometry_config>						geo_per_material_id;
		std::map<u32, utl::vector<Vertex>>					vertices;
		std::map<u32, utl::vector<u32>>						indices;

		globalvertices.clear();
		vertices.clear();
		indices.clear();

		for (const auto& shape : shapes)
		{
			out_geometry_darray.clear();
			u32 material_id = shape.mesh.material_ids[0];
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
					//uniqueVertices[index] = (u32)globalvertices.size();
					//globalvertices.emplace_back(vertex);
					uniqueVertices[index] = (u32)vertices[material_id].size();
					vertices[material_id].emplace_back(vertex);
				}

				indices[material_id].emplace_back((u32)uniqueVertices[index]);
			}

			geometry_config g;
			g.vertex_size = sizeof(Vertex);
			g.vertex_count = (u32)vertices[material_id].size();
			//g.vertices.swap(vertices[material_id]);
			g.index_size = sizeof(u32);
			g.index_count = (u32)indices[material_id].size();
			//g.indices.swap(indices[material_id]);
			memcpy_s(g.name, 256, shape.name.c_str(), 256);
			memcpy_s(g.material_name, 256, materials[material_id].name.c_str(), 256);
			if (!materials[material_id].ambient_texname.empty())
			{
				g.ambient_map = materials[material_id].ambient_texname;
				g.ambient_map.replace(g.ambient_map.find("textures"), 8, "images");
			}
			if (!materials[material_id].diffuse_texname.empty())
			{
				g.diffuse_map = materials[material_id].diffuse_texname;
				g.diffuse_map.replace(g.diffuse_map.find("textures"), 8, "images");
			}
			if (!materials[material_id].specular_texname.empty())
			{
				g.specular_map = materials[material_id].specular_texname;
				g.specular_map.replace(g.specular_map.find("textures"), 8, "images");
			}
			if (!materials[material_id].alpha_texname.empty())
			{
				g.alpha_map = materials[material_id].alpha_texname;
				g.alpha_map.replace(g.alpha_map.find("textures"), 8, "images");
			}
			if (!materials[material_id].bump_texname.empty())
			{
				g.normal_map = materials[material_id].bump_texname;
				g.normal_map.replace(g.normal_map.find("textures"), 8, "images");
			}
			//generate_bounding_box_and_center(&g);
			//generate_tangents(&g);
			
			geo_per_material_id[material_id] = g;
			//out_geometry_darray.emplace_back(g);
			//write_kms_file(file_package_path.c_str(), g.name, (u32)out_geometry_darray.size(), out_geometry_darray);
			//g.clear();
		}

		for (std::map<u32, geometry_config>::iterator iter = geo_per_material_id.begin(); iter != geo_per_material_id.end(); ++iter)
		{
			std::string filename{ out_ksm_file_package };
			filename.append("_").append(std::to_string(iter->first));

			u32 material_id = iter->first;

			geometry_config g{geo_per_material_id[material_id]};
			g.vertices = vertices[material_id];
			g.indices = indices[material_id];

			generate_bounding_box_and_center(&g);
			generate_tangents(&g);

			write_kms_file(file_package_path.c_str(), filename.c_str(), g);
		}

		return true;
	}

	EDITOR_INTERFACE void ImportObj(const char* file, const char* kms_name)
	{
		assert(file);
		load_obj_model(file, kms_name);
	}
}