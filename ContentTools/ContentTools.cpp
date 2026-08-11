#include "ToolsCommon.h"
#include "Geometry.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>

namespace primal::tools
{
	extern void ShutDownTextureTools();

	extern "C" void ImportFbx(const char* file, scene_data* data, void(*callback)(s32, s32));
	extern "C" void ImportObjAPI(const char* file, scene_data* data, void(*callback)(s32, s32));

	namespace
	{
		using namespace primal::utl;

		void append_json_escaped(utl::vector<char>& out, const std::string& s)
		{
			out.emplace_back('"');
			for (char c : s)
			{
				switch (c)
				{
				case '"':  out.emplace_back('\\'); out.emplace_back('"'); break;
				case '\\': out.emplace_back('\\'); out.emplace_back('\\'); break;
				case '\b': out.emplace_back('\\'); out.emplace_back('b'); break;
				case '\f': out.emplace_back('\\'); out.emplace_back('f'); break;
				case '\n': out.emplace_back('\\'); out.emplace_back('n'); break;
				case '\r': out.emplace_back('\\'); out.emplace_back('r'); break;
				case '\t': out.emplace_back('\\'); out.emplace_back('t'); break;
				default:
					if (static_cast<unsigned char>(c) < 0x20)
					{
						char buf[8];
						int n = std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
						for (int k = 0; k < n; ++k) out.emplace_back(buf[k]);
					}
					else
					{
						out.emplace_back(c);
					}
					break;
				}
			}
			out.emplace_back('"');
		}

		u32 write_model_sidecar(const u8* buffer, u32 buffer_size, const char* sidecar_path,
			const char* source_file, const char* mesh_file)
		{
			if (!buffer || buffer_size == 0 || !sidecar_path) return 0;

			utl::vector<submesh_meta> metas;
			if (!extract_submesh_metadata(buffer, buffer_size, metas)) return 0;

			utl::vector<char> json;
			json.reserve(256 + metas.size() * 96);

			const char* header =
				"{\n"
				"  \"version\": 1,\n"
				"  \"source\": ";
			for (const char* p = header; *p; ++p) json.emplace_back(*p);
			append_json_escaped(json, source_file ? source_file : "");
			json.emplace_back(',');
			json.emplace_back('\n');
			json.emplace_back(' ');
			json.emplace_back(' ');
			const char* mesh_field = "\"mesh\": ";
			for (const char* p = mesh_field; *p; ++p) json.emplace_back(*p);
			append_json_escaped(json, mesh_file ? mesh_file : "");
			json.emplace_back(',');
			json.emplace_back('\n');
			json.emplace_back(' ');
			json.emplace_back(' ');
			const char* sub_field = "\"submeshes\": [";
			for (const char* p = sub_field; *p; ++p) json.emplace_back(*p);
			json.emplace_back('\n');

			for (size_t i = 0; i < metas.size(); ++i)
			{
				const submesh_meta& m = metas[i];
				json.emplace_back(' ');
				json.emplace_back(' ');
				json.emplace_back(' ');
				json.emplace_back(' ');
				const char* item_hdr = "{\"name\": ";
				for (const char* p = item_hdr; *p; ++p) json.emplace_back(*p);
				append_json_escaped(json, m.name);

				const char* rest =
					", \"lod_id\": ";
				for (const char* p = rest; *p; ++p) json.emplace_back(*p);

				char num[32];
				int n;

				n = std::snprintf(num, sizeof(num), "%u", m.lod_id);
				for (int k = 0; k < n; ++k) json.emplace_back(num[k]);

				const char* rest2 = ", \"lod_index\": ";
				for (const char* p = rest2; *p; ++p) json.emplace_back(*p);
				n = std::snprintf(num, sizeof(num), "%u", m.lod_index);
				for (int k = 0; k < n; ++k) json.emplace_back(num[k]);

				const char* rest3 = ", \"mesh_index\": ";
				for (const char* p = rest3; *p; ++p) json.emplace_back(*p);
				n = std::snprintf(num, sizeof(num), "%u", m.mesh_index);
				for (int k = 0; k < n; ++k) json.emplace_back(num[k]);

				const char* rest4 = ", \"material_index\": ";
				for (const char* p = rest4; *p; ++p) json.emplace_back(*p);
				n = std::snprintf(num, sizeof(num), "%u", m.material_idx);
				for (int k = 0; k < n; ++k) json.emplace_back(num[k]);

				const char* rest5 = ", \"vertex_count\": ";
				for (const char* p = rest5; *p; ++p) json.emplace_back(*p);
				n = std::snprintf(num, sizeof(num), "%u", m.num_vertices);
				for (int k = 0; k < n; ++k) json.emplace_back(num[k]);

				const char* rest6 = ", \"index_count\": ";
				for (const char* p = rest6; *p; ++p) json.emplace_back(*p);
				n = std::snprintf(num, sizeof(num), "%u", m.num_indices);
				for (int k = 0; k < n; ++k) json.emplace_back(num[k]);

				json.emplace_back('}');
				if (i + 1 < metas.size())
				{
					json.emplace_back(',');
				}
				json.emplace_back('\n');
			}

			json.emplace_back(' ');
			json.emplace_back(' ');
			const char* footer = "]\n}\n";
			for (const char* p = footer; *p; ++p) json.emplace_back(*p);

			FILE* f = std::fopen(sidecar_path, "wb");
			if (!f) return 0;
			size_t w = std::fwrite(json.data(), 1, json.size(), f);
			std::fclose(f);

			return (w == json.size()) ? 1u : 0u;
		}

		void derive_sidecar_path(const char* mesh_path, char* out, size_t out_size)
		{
			assert(mesh_path && out && out_size > 0);
			const char* dot = std::strrchr(mesh_path, '.');
			size_t base = dot ? static_cast<size_t>(dot - mesh_path) : std::strlen(mesh_path);
			const char* suffix = ".model.json";
			const size_t slen = std::strlen(suffix);
			if (base + slen + 1 > out_size)
			{
				out[0] = '\0';
				return;
			}
			std::memcpy(out, mesh_path, base);
			std::memcpy(out + base, suffix, slen);
			out[base + slen] = '\0';
		}
	}
}

EDITOR_INTERFACE void ShutDownContentTools()
{
	using namespace primal::tools;
	ShutDownTextureTools();
}

EDITOR_INTERFACE u32 CookMeshWithSettings(const char* input, const char* output,
	f32 smoothing_angle,
	u8 calculate_normals, u8 calculate_tangents,
	u8 reverse_handedness, u8 import_embedded_textures,
	u8 import_animations, u8 coalesce_meshes)
{
	using namespace primal::tools;

	if (!input || !output) return 0;

	const char* ext = strrchr(input, '.');
	if (!ext) return 0;

	scene_data data{};
	data.settings.smoothing_angle = smoothing_angle;
	data.settings.calculate_normals = calculate_normals;
	data.settings.calculate_tangents = calculate_tangents;
	data.settings.reverse_handedness = reverse_handedness;
	data.settings.import_embedded_textures = import_embedded_textures;
	data.settings.import_animations = import_animations;
	data.settings.coalesce_meshes = coalesce_meshes;

	if (strcasecmp(ext, ".fbx") == 0)
	{
		ImportFbx(input, &data, nullptr);
	}
	else if (strcasecmp(ext, ".obj") == 0)
	{
		ImportObjAPI(input, &data, nullptr);
	}
	else
	{
		return 0;
	}

	if (!data.buffer || data.buffer_size == 0) return 0;

	FILE* f = std::fopen(output, "wb");
	if (!f)
	{
		std::free(data.buffer);
		return 0;
	}

	size_t written = std::fwrite(data.buffer, 1, data.buffer_size, f);
	std::fclose(f);

	if (written == data.buffer_size)
	{
		char sidecar_path[1024];
		derive_sidecar_path(output, sidecar_path, sizeof(sidecar_path));
		if (sidecar_path[0] != '\0')
		{
			write_model_sidecar(data.buffer, data.buffer_size, sidecar_path, input, output);
		}
	}

	std::free(data.buffer);

	return (written == data.buffer_size) ? 1u : 0u;
}

EDITOR_INTERFACE u32 CookMesh(const char* input, const char* output)
{
	return CookMeshWithSettings(input, output,
		178.0f, 1, 1, 0, 0, 0, 1);
}

EDITOR_INTERFACE u32 WriteModelSidecar(const char* cooked_mesh_path, const char* sidecar_path)
{
	using namespace primal::tools;

	if (!cooked_mesh_path || !sidecar_path) return 0;

	FILE* f = std::fopen(cooked_mesh_path, "rb");
	if (!f) return 0;
	std::fseek(f, 0, SEEK_END);
	long size = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	if (size <= 0)
	{
		std::fclose(f);
		return 0;
	}

	primal::utl::vector<u8> buffer;
	buffer.resize(static_cast<u32>(size));
	size_t r = std::fread(buffer.data(), 1, buffer.size(), f);
	std::fclose(f);
	if (r != buffer.size()) return 0;

	return write_model_sidecar(buffer.data(), buffer.size(), sidecar_path, "", cooked_mesh_path);
}