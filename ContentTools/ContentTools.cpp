#include "ToolsCommon.h"
#include "Geometry.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace primal::tools
{
	extern void ShutDownTextureTools();

	extern "C" void ImportFbx(const char* file, scene_data* data, void(*callback)(s32, s32));
	extern "C" void ImportObjAPI(const char* file, scene_data* data, void(*callback)(s32, s32));
}

EDITOR_INTERFACE void ShutDownContentTools()
{
	using namespace primal::tools;
	ShutDownTextureTools();
}

EDITOR_INTERFACE u32 CookMesh(const char* input, const char* output)
{
	using namespace primal::tools;

	if (!input || !output) return 0;

	const char* ext = strrchr(input, '.');
	if (!ext) return 0;

	scene_data data{};
	data.settings.smoothing_angle = 178.0f;
	data.settings.calculate_normals = 1;
	data.settings.calculate_tangents = 1;
	data.settings.reverse_handedness = 0;
	data.settings.import_embedded_textures = 0;
	data.settings.import_animations = 0;
	data.settings.coalesce_meshes = 1;

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
	std::free(data.buffer);

	return (written == data.buffer_size) ? 1u : 0u;
}