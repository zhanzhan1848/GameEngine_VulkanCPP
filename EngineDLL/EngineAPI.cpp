#include "Common.h"
#include "CommonHeaders.h"
#include "../Graphics/Renderer.h"
#include "../Platform/PlatformTypes.h"
#include "../Platform/Platform.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif // !WIN32_MEAN_AND_LEAN

#include <Windows.h>


using namespace primal;

namespace {
	HMODULE game_code_dll{ nullptr };

	utl::vector<graphics::render_surface> surfaces;
}// anonymous namespace

EDITOR_INTERFACE u32 LoadGameCodeDll(const char* dll_path)
{
	if (game_code_dll) return FALSE;
	game_code_dll = LoadLibraryA(dll_path);
	assert(game_code_dll);

	return game_code_dll ? TRUE : FALSE;
}

EDITOR_INTERFACE u32 UnloadGameCodeDll(const char* dll_path)
{
	if (!game_code_dll) return FALSE;
	assert(game_code_dll);
	int result{ FreeLibrary(game_code_dll) };
	assert(result);
	game_code_dll = nullptr;
	return TRUE;
}

EDITOR_INTERFACE u32 CreateRenderSurface(HWND host, s32 width, s32 height)
{
	assert(host);
	platform::window_init_info info{ nullptr, host, nullptr, 0, 0, width, height };
	graphics::render_surface surface{ platform::create_window(&info), {} };
	assert(surface.window.is_valid());
	surfaces.emplace_back(surface);
	return (s32)surfaces.size() - 1;
}

EDITOR_INTERFACE void RemoveRenderSurface(u32 id)
{
	assert(id < surfaces.size());
	platform::remove_window(surfaces[id].window.get_id());
}

EDITOR_INTERFACE HWND GetWindowHandle(u32 id)
{
	assert(id < surfaces.size());
	return (HWND)surfaces[id].window.handle();
}

EDITOR_INTERFACE void ResizeRenderSurface(u32 id)
{
	assert(id < surfaces.size());
	surfaces[id].window.resize(0, 0);
}