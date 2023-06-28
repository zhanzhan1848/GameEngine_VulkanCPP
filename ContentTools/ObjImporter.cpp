#include "ObjImporter.h"

#include "Geometry.h"
#include <fstream>
#include <iostream>

namespace primal::tools
{
	namespace
	{
		std::mutex		obj_mutex{};

		const char* out_path = "C:/Users/27042/Desktop/DX_Test/PrimalMerge/EngineTest/assets/kmt/";

		const char* kmt_ext = ".kmt";

		const char* out_model_path = "C:/Users/27042/Desktop/DX_Test/PrimalMerge/EngineTest/assets/kms/";

		const char* kms_ext = ".kms";

		bool write_kms_file(const char* file, const char* name, u32 count, scene* scene)
		{
			std::fstream outfile;

			char fullpath[256];
			strncat(fullpath, out_model_path, strlen(out_model_path) + 1);
			strncat(fullpath, name, strlen(name) + 1);
			strncat(fullpath, kms_ext, strlen(kms_ext) + 1);

			outfile.open(fullpath, std::ios::out | std::ios::app);

			if (!outfile.is_open()) return false;

			// Version
			outfile << 0x0001U << "\n";
			// Name length
			outfile << strlen(name) + 1 << "\n";
			// Name + terminator
			outfile << name << "\n";

			// Geometry count
			outfile << count << "\n";

			// Each geometry
			for (auto lod : scene->lod_groups)
			{
				for (u32 i{ 0 }; i < count; ++i)
				{
					mesh* m = &lod.meshes[i];

					// Vertices (size / count / array)
					outfile << sizeof(m->positions) << "\n";
					outfile << m->vertices.size() << "\n";
					outfile << &m->vertices << "\n";

					// Indices (size / count / array)
					outfile << sizeof(m->indices) << "\n";
					outfile << m->indices.size() << "\n";
					outfile << &m->indices << "\n";

					// Name
					outfile << strlen(m->name.c_str()) << "\n";
					outfile << m->name.c_str() << "\n";

					//  Normal
					outfile << sizeof(m->normals) << "\n";
					outfile << &m->normals << "\n";

					// UV
					outfile << sizeof(m->uv_sets) << "\n";
					outfile << &m->uv_sets << "\n";
				}
			}

			outfile.close();

			return true;
		}

		bool write_kmt_file(const char* file, mtl_config* config)
		{
			std::fstream outfile;

			char fullpath[256];
			strncat(fullpath, out_path, strlen(out_path) + 1);
			strncat(fullpath, config->name, strlen(config->name) + 1);
			strncat(fullpath, kmt_ext, strlen(kmt_ext) + 1);

			outfile.open(fullpath, std::ios::out | std::ios::app);
			if (!outfile.is_open()) return false;

			outfile << "#material file" << "\n";
			outfile << "" << "\n";
			outfile << "vertsion=0.1" << "\n";
			outfile << "name=" << config->name << "\n";
			outfile << "diffuse_color=" << config->diffuse_color.x << " " << config->diffuse_color.y << " " << config->diffuse_color.z << " " << config->diffuse_color.w << "\n";
			outfile << "shiniess=" << config->shininess << "\n";
			if (config->diffuse_map_name)
			{
				outfile << "diffuse_map_name=" << config->diffuse_map_name << "\n";
			}
			if (config->specular_map_name)
			{
				outfile << "specular_map_name=" << config->specular_map_name << "\n";
			}
			if (config->normal_map_name)
			{
				outfile << "normal_map_name=" << config->normal_map_name << "\n";
			}
			outfile << "shader=" << config->shader_name << "\n";

			outfile.close();

			return true;
		}
	} // anonymous namespace

	void obj_context::load_obj_file(const char* file)
	{
		utl::vector<math::v3>			position;
		utl::vector<math::v3>			normal;
		utl::vector<math::v2>			uv;
		char*							material_file_name;

		std::ifstream inFile(file, std::ios::in);

		if (!inFile) return;

		char* lines{};
		while (inFile.getline(lines, 511))
		{
			// 1、｀o¨    ！！> scene name
			// 2 、'g'  ！！> lod_group name
			// 3、 's'  ！！> mesh name
			char first = lines[0];
			mesh m;
			lod_group lod;
			char prev_first_chars[2]{ 0, 0 };
			switch (first)
			{
			case '#':	continue;
			case 'm':
			{
				// Material library file
				char substr[7];

				sscanf(lines, "%s %s", substr, material_file_name);
				
			}	break;
			case 'o':
			{
				char t[2];
				if (prev_first_chars[0] == 'f')
				{
					_scene->lod_groups.emplace_back(lod);
					lod.name = nullptr;
					lod.meshes.clear();
					position.clear();
					normal.clear();
					uv.clear();
				}
				sscanf(lines, "%s %s", t, &lod.name);
			}	break;
			case 'v':
			{
				char second = lines[1];
				switch (second)
				{
				case ' ':
				{
					// Vertex position
					math::v3 pos{};
					char t[2];
					sscanf(lines, "%s %f %f %f", t, &pos.x, &pos.y, &pos.z);
					position.emplace_back(pos);
				}	break;
				case 'n':
				{
					// Vertex normal
					math::v3 norm{};
					char t[2];
					sscanf(lines, "%s %f %f %f", t, &norm.x, &norm.y, &norm.z);
					normal.emplace_back(norm);
				}	break;
				case 't':
				{
					// Vertex texture coord
					math::v2 tex{};
					char t[2];
					sscanf(lines, "%s %f %f", t, &tex.x, &tex.y);
					uv.emplace_back(tex);
				}	break;
				default:
					break;
				}
			}	break;
			case 'g':
			{
				// !  rule: o_o_usemtl
				char t[2];
				sscanf(lines, "%s %s", t, &m.name);
			}	break;
			case 's':	break;
			case 'f':
			{
				// face
				// f 1/1/1 2/2/2 3/3/3 = pos/tex/norm pos/tex/norm pos/tex/norm
				char t[2];
				if (normal.size() == 0 || uv.size() == 0)
				{
					u32	i, j, k;
					sscanf(lines, "%s %d %d %d", t, &i, &j, &k);
					m.positions.emplace_back(position[i]);
					m.positions.emplace_back(position[j]);
					m.positions.emplace_back(position[k]);
					m.elements_type = elements::elements_type::static_color;
					m.raw_indices.emplace_back(i);
					m.raw_indices.emplace_back(j);
					m.raw_indices.emplace_back(k);
					
					m.vertices.emplace_back([position](u32 x) {
						vertex v;
					v.position = position[x];
					return v;
					}(i));
					m.vertices.emplace_back([position](u32 x) {
						vertex v;
					v.position = position[x];
					return v;
					}(j));
					m.vertices.emplace_back([position](u32 x) {
						vertex v;
					v.position = position[x];
					return v;
					}(k));
					m.indices.emplace_back(i);
					m.indices.emplace_back(j);
					m.indices.emplace_back(k);
				}
				else
				{
					math::v3 i, j, k;
					sscanf(lines, "%s %d/%d/%d %d/%d/%d %d/%d/%d", t, &i.x, &j.x, &k.x, &i.y, &j.y, &k.y, &i.z, &j.z, &k.z);
					m.positions.emplace_back(position[i.x]);
					m.positions.emplace_back(position[j.x]);
					m.positions.emplace_back(position[k.x]);

					m.uv_sets.emplace_back(uv[i.y]);
					m.uv_sets.emplace_back(uv[j.y]);
					m.uv_sets.emplace_back(uv[k.y]);

					m.normals.emplace_back(normal[i.z]);
					m.normals.emplace_back(normal[j.z]);
					m.normals.emplace_back(normal[k.z]);

					m.raw_indices.emplace_back(i.x);
					m.raw_indices.emplace_back(i.y);
					m.raw_indices.emplace_back(i.z);

					m.elements_type = elements::elements_type::static_normal_texture;

					m.vertices.emplace_back([position, uv, normal](math::v3 x) {
						vertex v;
					v.position = position[x.x];
					v.uv = uv[x.y];
					v.normal = normal[x.z];
					return v;
					}(i));
					m.vertices.emplace_back([position, uv, normal](math::v3 x) {
						vertex v;
					v.position = position[x.x];
					v.uv = uv[x.y];
					v.normal = normal[x.z];
					return v;
					}(j));
					m.vertices.emplace_back([position, uv, normal](math::v3 x) {
						vertex v;
					v.position = position[x.x];
					v.uv = uv[x.y];
					v.normal = normal[x.z];
					return v;
					}(k));
					m.indices.emplace_back(i.x);
					m.indices.emplace_back(j.x);
					m.indices.emplace_back(k.x);

					m.lod_id = 0;
					m.lod_threshold = 0;
				}
				lod.meshes.emplace_back(m);
				m.positions.clear();
				m.normals.clear();
				m.uv_sets.clear();
				m.indices.clear();
				m.vertices.clear();
			}	break;
			case ' ':
			{
				if (prev_first_chars[0] == 'f')
				{
					_scene->lod_groups.emplace_back(lod);
					lod.name = nullptr;
					lod.meshes.clear();
					position.clear();
					normal.clear();
					uv.clear();
				}
			}	break;
			default:
				break;
			}

			prev_first_chars[1] = prev_first_chars[0];
			prev_first_chars[0] = first;
		}

		inFile.close();
	}

	void obj_context::load_mtl_file(const char * mtl_file_path)
	{
		std::ifstream inFile(mtl_file_path, std::ios::in);

		if (!inFile) return;

		u8 hit_name{ false };
		char* lines{};
		mtl_config mtl;
		while (inFile.getline(lines, 511))
		{
			char first = lines[0];
			char prev_first_chars[2]{ 0, 0 };
			switch (first)
			{
			case '#':	continue;
			case 'K':
			{
				char second = lines[1];
				switch (second)
				{
				case 'a':
				{
					char t[2];
					sscanf(lines, "%s %f %f %f", t, &mtl.ambient_color.x, &mtl.ambient_color.y, &mtl.ambient_color.z);


					mtl.ambient_color.w = 1.0f;
				}	break;
				case 'd':
				{
					// Ambient/Diffuse color are treated the same at this level.
					// ambient color is determined by the level.
					char t[2];
					sscanf(lines, "%s %f %f %f", t, &mtl.diffuse_color.x, &mtl.diffuse_color.y, &mtl.diffuse_color.z);

					// NOTE: This is only used by the color shader, and will set to max_norm by default.
					// Transparency could be added as a material property all its own at a later time.
					mtl.diffuse_color.w = 1.0f;
				}	break;
				case 's':
				{
					// Specular Color
					char t[2];

					sscanf(lines, "%s %f %f %f", t, &mtl.specular_color.x, &mtl.specular_color.y, &mtl.specular_color.z);
					mtl.specular_color.w = 1.0f;
				}	break;
				default:
					break;
				}
			}	break;
			case 'N':
			{
				char second = lines[1];
				switch (second)
				{
				case 's':
				{
					// Specular exponent
					char t[2];

					sscanf(lines, "%s %f", t, &mtl.shininess);
				}	break;
				case 'i':
				{
					// IOS
					char t[2];

					sscanf(lines, "%s %f", t, &mtl.IOR);
				}	break;
				default:
					break;
				}
			}	break;
			case 'T':
			{
				char second = lines[1];
				switch (second)
				{
				case 'f':
				{
					char t[2];

					sscanf(lines, "%s %f %f %f", t, &mtl.transmission.x, &mtl.transmission.y, &mtl.transmission.z);

					mtl.transmission.w = 1.0f;
				}	break;
				default:
					break;
				}
			}
			case 'm':
			{
				// map
				char substr[10];
				char texture_file_name[256];

				sscanf(lines, "%s %s", substr, texture_file_name);

				if (strstr(substr, "map_Kd") != NULL)
				{
					strcpy(mtl.diffuse_map_name, texture_file_name);
				}
				else if (strstr(substr, "map_Ks") != NULL)
				{
					strcpy(mtl.specular_map_name, texture_file_name);
				}
				else if (strstr(substr, "map_bump") != NULL)
				{
					strcpy(mtl.normal_map_name, texture_file_name);
				}
				else
				{
					break;
				}
			}	break;
			case 'b':
			{
				char substr[10];
				char texture_file_name[256];

				sscanf(lines, "%s %s", substr, texture_file_name);

				if (strstr(substr, "bump") != NULL)
				{
					strcpy(mtl.normal_map_name, texture_file_name);
				}
				else break;
			}	break;
			case 'n':
			{
				char substr[10];
				char material_name[256];

				sscanf(lines, "%s %s", substr, material_name);
				if (strstr(substr, "newmtl") != NULL)
				{
					if (mtl.shininess == 0.f) mtl.shininess = 8.0f;

					if (hit_name)
					{

					}

					hit_name = true;
					strcpy(mtl.name, material_name);
				}
			}	break;
			default:
				break;
			}
		}


		if (mtl.shininess == 0.f) mtl.shininess = 8.f;

		inFile.close();

	}
}