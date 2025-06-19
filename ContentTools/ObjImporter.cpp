#include "ObjImporter.h"

#include <unordered_map>
#if defined(_MSC_VER)
#include <direct.h>
#include <io.h>
#elif defined(__clang__)
#include <sys/stat.h>
#include <unistd.h>
#endif
#include "Geometry.h"

#include <stdio.h>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include "../Engine/Utilities/IOStream.h"
#include "TemplateShader/PBR_Template_Shader_v1.h"
#include "meshoptimizer/meshoptimizer.h"
#include "../Engine/Utilities/Hash.h"

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

		bool write_kms_file(const char* file_package, const char* filename, u32 count, [[maybe_unused]] utl::vector<geometry_config>& out_geometry_darray)
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
#if defined(_MSC_VER)
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
#elif defined(__clang__)
			geo->min_extents = math::v3{ 0, 0, 0 };
			geo->max_extents = math::v3{ 0, 0, 0 };

			for (u32 i{ 0 }; i < geo->vertex_count; ++i)
			{
				if (geo->vertices[i].pos.x() < geo->min_extents.x())
				{
					geo->min_extents.x() = geo->vertices[i].pos.x();
				}
				if (geo->vertices[i].pos.y() < geo->min_extents.y())
				{
					geo->min_extents.y() = geo->vertices[i].pos.y();
				}
				if (geo->vertices[i].pos.z() < geo->min_extents.z())
				{
					geo->min_extents.z() = geo->vertices[i].pos.z();
				}

				if (geo->vertices[i].pos.x() > geo->max_extents.x())
				{
					geo->max_extents.x() = geo->vertices[i].pos.x();
				}
				if (geo->vertices[i].pos.y() > geo->max_extents.y())
				{
					geo->max_extents.y() = geo->vertices[i].pos.y();
				}
				if (geo->vertices[i].pos.z() > geo->max_extents.z())
				{
					geo->max_extents.z() = geo->vertices[i].pos.z();
				}
			}
			geo->center.x() = (geo->min_extents.x() + geo->max_extents.x()) / 2.f;
			geo->center.y() = (geo->min_extents.y() + geo->max_extents.y()) / 2.f;
			geo->center.z() = (geo->min_extents.z() + geo->max_extents.z()) / 2.f;
#endif
		}

		void generate_tangents(geometry_config* geos)
		{
#if defined(_MSC_VER)
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
#elif defined(__clang__)
			assert(geos->index_count % 3 == 0);
			for (u32 i{ 0 }; i < geos->index_count; i += 3)
			{
				u32 i0 = geos->indices[i];
				u32 i1 = geos->indices[i + 1];
				u32 i2 = geos->indices[i + 2];

				math::v3 edge1{ geos->vertices[i1].pos.x() - geos->vertices[i0].pos.x(),
					geos->vertices[i1].pos.y() - geos->vertices[i0].pos.y(),
					geos->vertices[i1].pos.z() - geos->vertices[i0].pos.z() };
				math::v3 edge2{ geos->vertices[i2].pos.x() - geos->vertices[i0].pos.x(),
					geos->vertices[i2].pos.y() - geos->vertices[i0].pos.y(),
					geos->vertices[i2].pos.z() - geos->vertices[i0].pos.z() };

				f32 deltaU1 = geos->vertices[i1].texCoord.x() - geos->vertices[i0].texCoord.x();
				f32 deltaV1 = geos->vertices[i1].texCoord.y() - geos->vertices[i0].texCoord.y();

				f32 deltaU2 = geos->vertices[i2].texCoord.x() - geos->vertices[i0].texCoord.x();
				f32 deltaV2 = geos->vertices[i2].texCoord.y() - geos->vertices[i0].texCoord.y();

				f32 dividend = (deltaU1 * deltaV2 - deltaU2 * deltaV1);
				f32 fc = 1.f / dividend;

				math::v3 tangent{ fc * (deltaV2 * edge1.x() - deltaV1 * edge2.x()),
					fc * (deltaV2 * edge1.y() - deltaV1 * edge2.y()),
					fc * (deltaV2 * edge1.z() - deltaV1 * edge2.z())};

				tangent.normalize();

				f32 sx = deltaU1, sy = deltaU2;
				f32 tx = deltaV1, ty = deltaV2;
				f32 handedness = ((tx * sy - ty * sx) < 0.f) ? -1.f : 1.f;
				math::v3 t4{ tangent.x() * handedness, tangent.y() * handedness, tangent.z() * handedness };
				geos->vertices[i0].tangent = t4;
				geos->vertices[i1].tangent = t4;
				geos->vertices[i2].tangent = t4;
			}
#endif
		}

		bool generate_shader(const void* const data, const char* file_package)
		{
			tinyobj::material_t material_data{ *(tinyobj::material_t*)data };

			{
				std::string out_vertex_shader_name{ file_package };
				out_vertex_shader_name.append("//").append("shaders");
#if defined(_MSC_VER)
				if (_access(out_vertex_shader_name.c_str(), 0) == -1)
					OutputDebugStringA(std::to_string(_mkdir(out_vertex_shader_name.c_str())).c_str());
					
#elif defined(__clang__)
				if (access(out_vertex_shader_name.c_str(), 0) == -1)
				{
					std::cerr << std::to_string(mkdir(out_vertex_shader_name.c_str(), 0777)).c_str() << std::endl;
				}
#endif
				out_vertex_shader_name.append("\\").append(material_data.name.c_str()).append(".vert");
				std::ofstream vert_shader{ out_vertex_shader_name };
				if (!vert_shader.is_open())
				{
#if defined(_MSC_VER)
					OutputDebugStringA("Failed to open vertex shader to write!");
#elif defined(__clang__)
					std::cerr << "Failed to open vertex shader to write!" << std::endl;
#endif
					return false;
				}
				vert_shader << PBR_Template_Vertex_Shader;
				vert_shader.close();
			}

			{
				std::string out_fragment_shader_name{ file_package };
				out_fragment_shader_name.append("//").append("shaders");
#if defined(_MSC_VER)
				if (_access(out_fragment_shader_name.c_str(), 0) == -1)
					OutputDebugStringA(std::to_string(_mkdir(out_fragment_shader_name.c_str())).c_str());
#elif defined(__clang__)
				if (access(out_fragment_shader_name.c_str(), 0) == -1)
				{
					std::cerr << std::to_string(mkdir(out_fragment_shader_name.c_str(), 0777)).c_str() << std::endl;
				}
#endif
				out_fragment_shader_name.append("\\").append(material_data.name.c_str()).append(".frag");
				std::ofstream fragment_shader{ out_fragment_shader_name };
				if (!fragment_shader.is_open())
				{
#if defined(_MSC_VER)
					OutputDebugStringA("Failed to open fragment shader to write!");
#elif defined(__clang__)
					std::cerr << "Failed to open fragment shader to write!" << std::endl;
#endif
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

			std::string shaderfunction_src{ "C:/Users/zy/Desktop/PrimalMerge/PrimalEngine/Engine/Graphics/Vulkan/Shaders/CommonFunction.glsli" };
			std::string shaderfunction_dst{ file_package };
			shaderfunction_dst.append("\\").append("shaders\\").append("CommonFunction.glsli");

			std::ifstream shaderfunctionsrc(shaderfunction_src, std::ios::binary);
			std::ofstream shaderfunctiondst(shaderfunction_dst, std::ios::binary);

			shaderfunctiondst << shaderfunctionsrc.rdbuf();

			std::string shaderconstant_src{ "C:/Users/zy/Desktop/PrimalMerge/PrimalEngine/Engine/Graphics/Vulkan/Shaders/CommonConstant.glsli" };
			std::string shaderconstant_dst{ file_package };
			shaderconstant_dst.append("\\").append("shaders\\").append("CommonConstant.glsli");

			std::ifstream shaderconstantsrc(shaderconstant_src, std::ios::binary);
			std::ofstream shaderconstantdst(shaderconstant_dst, std::ios::binary);

			shaderconstantdst << shaderconstantsrc.rdbuf();
		}

		/// <summary>
		/// Mesh Optimizing Pipeline (Use meshoptimizer library)
		/// https://github.com/zeux/meshoptimizer
		/// 
		/// Pipeline(the order is important!):
		/// 1. Indexing
		/// 2. (optional) Simplification (Use to generate lod)
		/// 3. Vertex cache optimization
		/// 4. Overdraw optimization
		/// 5. Vertex fetch optimization
		/// 6. Vertex quantization
		/// 7. Shadow indexing
		/// 8. (optional) Vertex/Index buffer compression (maybe do this in graphics core)
		/// 
		/// Meshes input, output meshes after optimize and generate next level lod
		/// </summary>
		/*void Mesh_Optimiziong(mesh& mesh, const f32 lod_precent, lod_group& lod)
		{
			u32 index_count = mesh.raw_indices.size();
			u32 unindexed_vertex_count = mesh.raw_indices.size() / 3;

			utl::vector<u32> remap{ index_count };
			u32 vertex_count = meshopt_generatevertexremap(&remap[0], mesh.raw_indices.data(), index_count, mesh.positions.data(), unindexed_vertex_count, sizeof(math::v3));
		}*/
	} // anonymous namespace

	void obj_context::load_obj_file(const char* file)
	{
		std::string path{ file };
		size_t pos = path.rfind("\\", path.length());
		std::string base_file_path{ path.substr(0, pos) };

		std::string warn, err;

		if (!tinyobj::LoadObj(&_attribute, &_shapes, &_materials, &warn, &err, file, base_file_path.c_str()))
		{
#if defined(_MSC_VER)
			throw std::runtime_error(warn + err);
#elif defined(__clang__)
			std::cerr << "Error loading OBJ file: " <<
			warn << "  --  " << err << std::endl;
#endif
		}
	}

	void obj_context::get_scene()
	{
		if (_scene_data->settings.coalesce_meshes)
		{
			lod_group lod{};
			get_meshes(lod.meshes, 0, -1.f);

			if (lod.meshes.size())
			{
				lod.name = lod.meshes[0].name;
				mesh combined_mesh{};
				if (coalesce_meshes(lod, combined_mesh, _progression))
				{
					lod.meshes.clear();
					lod.meshes.emplace_back(combined_mesh);
				}
				_scene->lod_groups.emplace_back(lod);
			}

			get_lod_group(lod.meshes, 1, 20.f);
		}
		else
		{

			lod_group lod{};
			get_meshes(lod.meshes, 0, -1.f);
			if (lod.meshes.size())
			{
				lod.name = lod.meshes[0].name;
				_scene->lod_groups.emplace_back(lod);
			}

			get_lod_group(lod.meshes, 1, 20.f);
		}
	}

	void obj_context::get_lod_group(const utl::vector<mesh>& meshes, u32 lod_id, f32 lod_threshold)
	{
		// Generate LOD group using meshoptimizer library
		// size_t meshopt_simplify(unsigned int* destination, 
		//						   const unsigned int* indices,	
		//						   size_t index_count, 
		//						   const float* vertex_positions, 
		//						   size_t vertex_count, 
		//						   size_t vertex_positions_stride, 
		//						   size_t target_index_count, 
		//						   float target_error, 
		//						   unsigned int options, 
		//						   float* result_error)
		if (lod_id > 5) return;
		lod_group lod{};
		lod.name = meshes[0].name + "_LODS" + std::to_string(lod_id);
		// lod.name = (lod_id > 0) ? meshes[0].name + "_LODS" + std::to_string(lod_id) : meshes[0].name;
		for (auto& m : meshes)
		{
			mesh submesh{};
			submesh.positions = m.positions;
			submesh.name = m.name;
			submesh.lod_id = lod_id;
			submesh.lod_threshold = lod_threshold;
			submesh.colors = m.colors;
			submesh.normals = m.normals;
			submesh.tangents = m.tangents;
			submesh.uv_sets = m.uv_sets;
			submesh.material_indices = m.material_indices;
			submesh.material_used = m.material_used;
			if (m.raw_indices.size() > 24)
			{
				u32 target_index_count{ static_cast<u32>(std::floor(m.raw_indices.size() * 0.8f)) - static_cast<u32>(std::floor(m.raw_indices.size() * 0.8f)) % 3 };
				utl::vector<u32> dst_indices(m.raw_indices.size());
				f32 error;
				u64 simplify_indices_count = meshopt_simplify(dst_indices.data(),
					m.raw_indices.data(),
					m.raw_indices.size(),
					reinterpret_cast<f32*>(m.positions.data()), // ����math::v3��DirectX::FLOAT3,��һ���ṹ�壬��Աx��y��z���ڴ������ģ����Կ���ͨ�������ʽ����
					m.positions.size(),
					sizeof(math::v3),
					(u64)target_index_count,
					(f32)0.01,
					(u32)0,
					&error);
				dst_indices.resize(simplify_indices_count);
				submesh.raw_indices = dst_indices;
			}
			else
			{
				submesh.raw_indices = m.raw_indices;
			}
			
			lod.meshes.emplace_back(submesh);
		}
		if (lod.meshes.size()) _scene->lod_groups.emplace_back(lod);
		get_lod_group(lod.meshes, lod_id + 1, lod_threshold + 20.f);
	}

	void obj_context::get_meshes(utl::vector<mesh>& meshes, u32 lod_id, f32 lod_threshold)
	{
		assert(lod_id != u32_invalid_id);

		get_mesh(meshes, lod_id, lod_threshold);
	}

	void obj_context::get_mesh(utl::vector<mesh>& meshes, u32 lod_id, f32 lod_threshold)
	{
		assert(lod_id != u32_invalid_id);
		u32 num_shapes{ (u32)_shapes.size() };
		
		for (u32 i{ 0 }; i < num_shapes; ++i)
		{
			mesh m;
			m.lod_id = lod_id;
			m.lod_threshold = lod_threshold;
			m.name = _shapes[i].name.c_str();


			if (get_mesh_data(&_shapes[i], m))
			{
				meshes.emplace_back(m);
				_progression->callback(_progression->value(), _progression->max_value() + 1);
			}
		}
		generate_shader();
	}

	bool obj_context::get_mesh_data(tinyobj::shape_t* shape, mesh& m)
	{
		const s32 num_polys{ (s32)shape->mesh.num_face_vertices.size() };
		if (num_polys <= 0) return false;

		// Get vertices
		utl::vector<math::v4> vertices;
		// const s32 num_indices{ (s32)shape->mesh.indices.size() };
		utl::vector<s32> indices;
		m.uv_sets.resize((u64)1);
		std::unordered_map<tinyobj::index_t, size_t, hash_idx, equal_idx> uniqueVertices;
		for (const auto& index : shape->mesh.indices)
		{
			math::v3 pos = math::v3{
				_attribute.vertices[3 * index.vertex_index + 0],
				_attribute.vertices[3 * index.vertex_index + 1],
				_attribute.vertices[3 * index.vertex_index + 2]
			};

			math::v3 color = math::v3{ 1.0f, 1.0f, 1.0f };

			math::v2 texCoord = math::v2{
				_attribute.texcoords[2 * index.texcoord_index + 0],
				1.0f - _attribute.texcoords[2 * index.texcoord_index + 1] // 1.0f - attrib.texcoords[2 * index.texcoord_index + 1]
			};

			math::v3 normal = math::v3{
				_attribute.normals[3 * index.normal_index + 0],
				_attribute.normals[3 * index.normal_index + 1],
				_attribute.normals[3 * index.normal_index + 2]
			};

			if (uniqueVertices.count(index) == 0)
			{
				uniqueVertices[index] = (u32)m.positions.size();
				m.positions.emplace_back(pos);
			}

			m.colors.emplace_back(color);
			m.normals.emplace_back(normal);
			m.uv_sets[0].emplace_back(texCoord);
			m.raw_indices.emplace_back((u32)uniqueVertices[index]);
		}

		assert(m.raw_indices.size() % 3 == 0);

		// Get material index per polygon
		// Use materials' name to generate hash to connect shader file
		assert(num_polys > 0);
		const s32 mtl_index{ shape->mesh.material_ids[0] };
		u32 mtl_name_hash;
		utl::MurmurHash3_x86_32(static_cast<void*>(&_materials[mtl_index].name), (u32)_materials[mtl_index].name.length(), (u32)123, &mtl_name_hash);
		assert(mtl_index >= 0);
		m.material_indices.emplace_back((u32)mtl_name_hash);
		if (std::find(m.material_used.begin(), m.material_used.end(), (u32)mtl_name_hash) == m.material_used.end())
		{
			m.material_used.emplace_back((u32)mtl_name_hash);
		}

		// Importing normals is ON by default
		const bool import_normals{ !_scene_data->settings.calculate_normals };
		// Importing tangents is OFF by default
		const bool import_tangents{ !_scene_data->settings.calculate_tangents };

		// Import normals
		if (import_normals)
		{
			// Calculate normals using OBJ's built-in method, but only if no normal data is already there.
			if (m.normals.empty())
			{
				for (s32 i{ 0 }; i < indices.size() / 3; ++i)
				{
					s32 index{ shape->mesh.indices[i].normal_index };
					math::v4 n{ _attribute.normals[index * 3],
								_attribute.normals[index * 3 + 1],
								_attribute.normals[index * 3 + 2],
								1.f };
#if defined(_MSC_VER)
					DirectX::XMVECTOR N = DirectX::XMLoadFloat4(&n);
					DirectX::XMStoreFloat4(&n, DirectX::XMVector4Normalize(N));
					m.normals.emplace_back((f32)n.x, (f32)n.y, (f32)n.z);
#elif defined(__clang__)
					n.normalize();
					m.normals.emplace_back((f32)n.x(), (f32)n.y(), (f32)n.z());
#endif
				}
			}
			else
			{
				// something went wrong wtith importing normals from FBX.
				// Fall back to our normal calculation method
				_scene_data->settings.calculate_normals = true;
			}
		}

		// Import tangents
		if (import_tangents)
		{
			// Calculate tangents using function.
			assert(m.raw_indices.size() % 3 == 0);
			for (u32 i{ 0 }; i < m.raw_indices.size(); i += 3)
			{
#if defined(_MSC_VER)
				u32 i0{ m.raw_indices[i] };
				u32 i1{ m.raw_indices[i + 1] };
				u32 i2{ m.raw_indices[i + 2] };

				math::v3 v0{ m.positions[i0] };
				math::v3 v1{ m.positions[i1] };
				math::v3 v2{ m.positions[i2] };

				math::v2 uv0{ m.uv_sets[0][i0] };
				math::v2 uv1{ m.uv_sets[0][i1] };
				math::v2 uv2{ m.uv_sets[0][i2] };

				math::v3 edge1{ v1.x - v0.x,
					v1.y - v0.y,
					v1.z - v0.z };
				math::v3 edge2{ v2.x - v0.x,
					v2.y - v0.y,
					v2.z - v0.z };

				f32 deltaU1 = uv1.x - uv0.x;
				f32 deltaV1 = uv1.y - uv0.y;

				f32 deltaU2 = uv2.x - uv0.x;
				f32 deltaV2 = uv2.y - uv0.y;

				f32 dividend = (deltaU1 * deltaV2 - deltaU2 * deltaV1);
				f32 fc = 1.f / dividend;

				math::v3 tangent{ fc * (deltaV2 * edge1.x - deltaV1 * edge2.x),
					fc * (deltaV2 * edge1.y - deltaV1 * edge2.y),
					fc * (deltaV2 * edge1.z - deltaV1 * edge2.z) };

				DirectX::XMVECTOR tang = XMLoadFloat3(&tangent);
				tang = DirectX::XMVector3Normalize(tang);
				DirectX::XMStoreFloat3(&tangent, tang);

				f32 sx = deltaU1, sy = deltaU2;
				f32 tx = deltaV1, ty = deltaV2;
				f32 handedness = ((tx * sy - ty * sx) < 0.f) ? -1.f : 1.f;
				math::v4 t4{ tangent.x * handedness, tangent.y * handedness, tangent.z * handedness, 0.f };
				m.tangents.emplace_back(t4);
				m.tangents.emplace_back(t4);
				m.tangents.emplace_back(t4);
#elif defined(__clang__)
				u32 i0{ m.raw_indices[i] };
				u32 i1{ m.raw_indices[i + 1] };
				u32 i2{ m.raw_indices[i + 2] };

				math::v3 v0{ m.positions[i0] };
				math::v3 v1{ m.positions[i1] };
				math::v3 v2{ m.positions[i2] };

				math::v2 uv0{ m.uv_sets[0][i0] };
				math::v2 uv1{ m.uv_sets[0][i1] };
				math::v2 uv2{ m.uv_sets[0][i2] };

				math::v3 edge1{ v1.x() - v0.x(),
					v1.y() - v0.y(),
					v1.z() - v0.z() };
				math::v3 edge2{ v2.x() - v0.x(),
					v2.y() - v0.y(),
					v2.z() - v0.z() };

				f32 deltaU1 = uv1.x() - uv0.x();
				f32 deltaV1 = uv1.y() - uv0.y();

				f32 deltaU2 = uv2.x() - uv0.x();
				f32 deltaV2 = uv2.y() - uv0.y();

				f32 dividend = (deltaU1 * deltaV2 - deltaU2 * deltaV1);
				f32 fc = 1.f / dividend;

				math::v3 tangent{ fc * (deltaV2 * edge1.x() - deltaV1 * edge2.x()),
					fc * (deltaV2 * edge1.y() - deltaV1 * edge2.y()),
					fc * (deltaV2 * edge1.z() - deltaV1 * edge2.z()) };

				tangent.normalize();

				f32 sx = deltaU1, sy = deltaU2;
				f32 tx = deltaV1, ty = deltaV2;
				f32 handedness = ((tx * sy - ty * sx) < 0.f) ? -1.f : 1.f;
				math::v4 t4{ tangent.x() * handedness, tangent.y() * handedness, tangent.z() * handedness, 0.f };
				m.tangents.emplace_back(t4);
				m.tangents.emplace_back(t4);
				m.tangents.emplace_back(t4);
#endif
			}
		}

		return true;
	}

	void obj_context::generate_shader()
	{
		assert(_materials.size());
		std::string shader_file_package{ "C:/Users/zy/Desktop/PrimalMerge/PrimalEngine/x64" };
		copy_shader_header(shader_file_package.c_str());
		for (auto m : _materials)
		{
			u32 m_name;
			utl::MurmurHash3_x86_32((void*)m.name.c_str(), (u32)m.name.length(), 0, (void*)&m_name);
			{
				std::string out_vertex_shader_name{ shader_file_package };
				out_vertex_shader_name.append("//").append("shaders");
				if (_access(out_vertex_shader_name.c_str(), 0) == -1)
					OutputDebugStringA(std::to_string(_mkdir(out_vertex_shader_name.c_str())).c_str());
				out_vertex_shader_name.append("\\").append(std::to_string(m_name)).append(".vert");
				std::ofstream vert_shader{ out_vertex_shader_name };
				if (!vert_shader.is_open())
				{
					OutputDebugStringA("Failed to open vertex shader to write!");
					return;
				}
				vert_shader << PBR_Template_Vertex_Shader;
				vert_shader.close();
			}

			{
				std::string out_fragment_shader_name{ shader_file_package };
				out_fragment_shader_name.append("//").append("shaders");
				if (_access(out_fragment_shader_name.c_str(), 0) == -1)
					OutputDebugStringA(std::to_string(_mkdir(out_fragment_shader_name.c_str())).c_str());
				out_fragment_shader_name.append("\\").append(std::to_string(m_name)).append(".frag");
				std::ofstream fragment_shader{ out_fragment_shader_name };
				if (!fragment_shader.is_open())
				{
					OutputDebugStringA("Failed to open fragment shader to write!");
					return;
				}

				std::string fragment_template_string{ PBR_Template_Fragment_Shader };

				size_t ns_pos = fragment_template_string.find("{{Ns}}");
				if (ns_pos != std::string::npos)
				{
					std::string ns{ std::to_string(m.shininess) };
					fragment_template_string.replace(ns_pos, 6, ns);
				}

				size_t ni_pos = fragment_template_string.find("{{Ni}}");
				if (ni_pos != std::string::npos)
				{
					std::string ni{ std::to_string(m.ior) };
					fragment_template_string.replace(ni_pos, 6, ni);
				}

				size_t d_pos = fragment_template_string.find("{{d}}");
				if (d_pos != std::string::npos)
				{
					std::string d{ std::to_string(m.dissolve) };
					fragment_template_string.replace(d_pos, 5, d);
				}

				size_t tr_pos = fragment_template_string.find("{{Tr}}");
				if (tr_pos != std::string::npos)
				{
					std::string tr{ std::to_string(1.f - m.dissolve) };
					fragment_template_string.replace(tr_pos, 6, tr);
				}

				size_t tf_pos = fragment_template_string.find("{{Tf}}");
				if (tf_pos != std::string::npos)
				{
					std::string tf;
					tf.append("vec3(").append(std::to_string(m.transmittance[0])).append(",").append(std::to_string(m.transmittance[1])).append(",")
						.append(std::to_string(m.transmittance[2])).append(")");
					fragment_template_string.replace(tf_pos, 6, tf);
				}

				size_t ka_pos = fragment_template_string.find("{{Ka}}");
				if (ka_pos != std::string::npos)
				{
					std::string ka;
					ka.append("vec3(").append(std::to_string(m.ambient[0])).append(",").append(std::to_string(m.ambient[1])).append(",")
						.append(std::to_string(m.ambient[2])).append(")");
					fragment_template_string.replace(ka_pos, 6, ka);
				}

				size_t kd_pos = fragment_template_string.find("{{Kd}}");
				if (kd_pos != std::string::npos)
				{
					std::string kd;
					kd.append("vec3(").append(std::to_string(m.diffuse[0])).append(",").append(std::to_string(m.diffuse[1])).append(",")
						.append(std::to_string(m.diffuse[2])).append(")");
					fragment_template_string.replace(kd_pos, 6, kd);
				}

				size_t ks_pos = fragment_template_string.find("{{Ks}}");
				if (ks_pos != std::string::npos)
				{
					std::string ks;
					ks.append("vec3(").append(std::to_string(m.specular[0])).append(",").append(std::to_string(m.specular[1])).append(",")
						.append(std::to_string(m.specular[2])).append(")");
					fragment_template_string.replace(ks_pos, 6, ks);
				}

				size_t ke_pos = fragment_template_string.find("{{Ke}}");
				if (ks_pos != std::string::npos)
				{
					std::string ke;
					ke.append("vec3(").append(std::to_string(m.emission[0])).append(",").append(std::to_string(m.emission[1])).append(",")
						.append(std::to_string(m.emission[2])).append(")");
					fragment_template_string.replace(ke_pos, 6, ke);
				}

				size_t dc_pos = fragment_template_string.find("{{diffuse_color}}");
				if (dc_pos != std::string::npos)
				{
					std::string dc;
					dc.append("vec3(").append(std::to_string(m.diffuse[0])).append(",").append(std::to_string(m.diffuse[1])).append(",")
						.append(std::to_string(m.diffuse[2])).append(")");
					fragment_template_string.replace(dc_pos, 17, dc);
				}

				size_t image_pos = fragment_template_string.find("{{images}}");
				if (image_pos != std::string::npos)
				{
					std::string image;
					u32 count{ 0 };
					if (!m.diffuse_texname.empty())
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
					if (!m.specular_texname.empty())
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
					if (!m.bump_texname.empty())
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
		}
	}

	bool load_obj_model(std::string path, const char* out_ksm_file_package)
	{
		size_t pos = path.rfind("/", path.length());
		std::string base_file_path{ path.substr(0, pos) };

		std::string file_package_path{ out_model_path + std::string{ out_ksm_file_package } };
#if defined(_MSC_VER)
		if (_access(file_package_path.c_str(), 0) == -1)
			OutputDebugStringA(std::to_string(_mkdir(file_package_path.c_str())).c_str());
#elif defined(__clang__)
		if (access(file_package_path.c_str(), 0) == -1)
		{
			std::cerr << std::to_string(mkdir(file_package_path.c_str(), 0777)).c_str() << std::endl;
		}
#endif
		tinyobj::attrib_t attrib;
		std::vector<tinyobj::shape_t> shapes;
		std::vector<tinyobj::material_t> materials;
		std::string warn, err;

		utl::vector<Vertex>	globalvertices;
		utl::vector<geometry_config> out_geometry_darray;

		if (!tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, path.c_str(), base_file_path.c_str()))
		{
#if defined(_MSC_VER)
			throw std::runtime_error(warn + err);
#elif defined(__clang__)
			std::cerr << "Error loading OBJ file: " <<
			warn << "  --  " << err << std::endl;
			return false;
#endif
		}

		for (auto material : materials)
		{
			if (!generate_shader(static_cast<void*>(&material), file_package_path.c_str()))
			{
#if defined(_MSC_VER)
				OutputDebugStringA("Failed to write shader");
#elif defined(__clang__)
				std::cerr << "Failed to write shader" << std::endl;
#endif
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
#if defined(_MSC_VER)
			memcpy_s(g.name, 256, shape.name.c_str(), 256);
			memcpy_s(g.material_name, 256, materials[material_id].name.c_str(), 256);
#elif defined(__clang__)
			strncpy(g.name, shape.name.c_str(), 256);
			g.name[255] = '\0';
			strncpy(g.material_name, materials[material_id].name.c_str(), 256);
			g.material_name[255] = '\0';
#endif
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

	bool load_single_obj_model(std::string path, const char* out_ksm_file_package)
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
#if defined(_MSC_VER)
		if (_access(file_package_path.c_str(), 0) == -1)
			OutputDebugStringA(std::to_string(_mkdir(file_package_path.c_str())).c_str());
#elif defined(__clang__)
		if (access(file_package_path.c_str(), 0) == -1)
		{
			std::cerr << std::to_string(mkdir(file_package_path.c_str(), 0777)).c_str() << std::endl;
		}
#endif
		tinyobj::attrib_t attrib;
		std::vector<tinyobj::shape_t> shapes;
		std::vector<tinyobj::material_t> materials;
		std::string warn, err;

		utl::vector<Vertex>	globalvertices;
		utl::vector<geometry_config> out_geometry_darray;

		if (!tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, path.c_str(), base_file_path.c_str()))
		{
#if defined(_MSC_VER)
			throw std::runtime_error(warn + err);
#elif defined(__clang__)
			std::cerr << "Error loading OBJ file: " <<
			warn << "  --  " << err << std::endl;
			return false;
#endif
		}

		for (auto material : materials)
		{
			if (!generate_shader(static_cast<void*>(&material), file_package_path.c_str()))
			{
#if defined(_MSC_VER)
				OutputDebugStringA("Failed to write shader");
#elif defined(__clang__)
				std::cerr << "Failed to write shader" << std::endl;
#endif
			}
		}

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
#if defined(_MSC_VER)
			memcpy_s(g.name, 256, shape.name.c_str(), 256);
#elif defined(__clang__)
			strncpy(g.name, shape.name.c_str(), 256);
			g.name[255] = '\0';
#endif
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

			geometry_config g{ geo_per_material_id[material_id] };
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

	EDITOR_INTERFACE void ImportSimgleObj(const char* file, const char* kms_name)
	{
		assert(file);
		load_single_obj_model(file, kms_name);
	}

	EDITOR_INTERFACE void ImportObjAPI(const char* file, scene_data* data, progression::progress_callback callback)
	{
		assert(file && data);
		scene scene{};
		progression progression{ callback };
		// NOTE: anything that involves using the rapidobj should be single-threaded
		{
			std::lock_guard lock{ obj_mutex };
			obj_context obj_model_context{ file, &scene, data, &progression };
			obj_model_context.get_scene();
		}
		if (scene.lod_groups.empty())
		{
			// TODO: send fasilure log message to editor
			return;
		}

		process_scene(scene, data->settings, &progression);
		pack_data(scene, *data);
	}
}