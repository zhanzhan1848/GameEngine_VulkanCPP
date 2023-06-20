#include "ObjImporter.h"

#include "Geometry.h"
#include <fstream>
#include <iostream>

namespace primal::tools
{
	namespace
	{
		std::mutex		obj_mutex{};
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
	}
}