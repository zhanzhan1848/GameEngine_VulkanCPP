#if defined(_MSC_VER)
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

// 与 __clang__ 分支一致：把匿名命名空间的 surfaces 向量暴露给其它 API 编译
// 单元（FrameAPI/CaptureAPI/RenderPipelineAPI 经 EngineAPIInternal.h 引用）。
// 此前只存在于 clang 分支，MSVC 分支缺失 → EngineDLL.dll LNK2019/LNK1120。
namespace primal::engine_dll {
graphics::render_surface* GetSurface(u32 id) {
    return id < surfaces.size() ? &surfaces[id] : nullptr;
}
u32 GetSurfaceCount() {
    return static_cast<u32>(surfaces.size());
}
} // namespace primal::engine_dll

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

#elif defined(__clang__)
#include "Common.h"
#include "CommonHeaders.h"
#include "EngineAPIInternal.h"
#include "../Graphics/Renderer.h"
#include "../Graphics/RHI/Core/RHIDevice.h"
#include "../Platform/PlatformTypes.h"
#include "../Platform/Platform.h"
#include <dlfcn.h>
#include <cstdio>

using namespace primal;

namespace {
	void* game_code_dll{ nullptr };

	utl::vector<graphics::render_surface> surfaces;
}// anonymous namespace

namespace primal::engine_dll {
// Expose the anonymous-namespace surfaces vector for other API translation units.
graphics::render_surface* GetSurface(u32 id) {
    return id < surfaces.size() ? &surfaces[id] : nullptr;
}
u32 GetSurfaceCount() {
    return static_cast<u32>(surfaces.size());
}
} // namespace primal::engine_dll

EDITOR_INTERFACE u32 LoadGameCodeDll(const char* dll_path)
{
	if (game_code_dll) return 0;
	game_code_dll = dlopen(dll_path, RTLD_NOW);
	assert(game_code_dll);

	return game_code_dll ? 1 : 0;
}

EDITOR_INTERFACE u32 UnloadGameCodeDll([[maybe_unused]] const char* dll_path)
{
	if (!game_code_dll) return 0;
	assert(game_code_dll);
	int result{ dlclose(game_code_dll) };
	assert(result == 0);
	game_code_dll = nullptr;
	return 1;
}

EDITOR_INTERFACE u32 CreateRenderSurface(void* host, s32 width, s32 height)
{
	// Headless tests pass nullptr host (no NSView/NSWindow). Graceful return
	// instead of asserting so callers can WARN + skip render validation.
	if (!host) {
		std::fprintf(stderr, "[CreateRenderSurface] host is null — headless mode, returning 0\n");
		return 0;
	}
	platform::window_init_info info{ nullptr, host, nullptr, 0, 0, width, height };
	graphics::render_surface rs{};
	rs.window = platform::create_window(&info);
	assert(rs.window.is_valid());
	rs.surface = graphics::create_surface(rs.window);
	assert(rs.surface.is_valid());
	surfaces.emplace_back(rs);
	return (s32)surfaces.size() - 1;
}

EDITOR_INTERFACE void RemoveRenderSurface(u32 id)
{
	assert(id < surfaces.size());
	graphics::render_surface temp{ surfaces[id] };
	surfaces[id] = {};
	if (temp.surface.is_valid()) graphics::remove_surface(temp.surface.get_id());
	if (temp.window.is_valid()) platform::remove_window(temp.window.get_id());
}

EDITOR_INTERFACE void* GetWindowHandle(u32 id)
{
	assert(id < surfaces.size());
	return surfaces[id].window.handle();
}

EDITOR_INTERFACE void ResizeRenderSurface(u32 id, u32 width, u32 height)
{
	assert(id < surfaces.size());
	surfaces[id].window.resize(width, height);
	surfaces[id].surface.resize(width, height);
}

// === Phase 1 Sub-step 1.2.5': Engine lifecycle entry for Editor ===
// 一站式封装 Editor 启动/关闭引擎的标准序列：
//   initialize_with_device → bind_rhi_device_to_legacy → initialize(<mapped platform>)
// Editor (C#) 通过 P/Invoke 调用，不需要知道三步顺序。
//
// rhiPlatform: RHIPlatform 枚举值（u32）。
// enableDebug: 0=false, 非 0=true。
// 返回 1=成功，0=失败（任何一步失败都会回滚已初始化的部分）。

namespace {
graphics::graphics_platform MapRHIPlatformToGraphicsPlatform(graphics::rhi::RHIPlatform p)
{
    switch (p) {
    case graphics::rhi::RHIPlatform::D3D12:   return graphics::graphics_platform::direct3d12;
    case graphics::rhi::RHIPlatform::Vulkan:  return graphics::graphics_platform::vulkan_1;
    case graphics::rhi::RHIPlatform::Metal:   return graphics::graphics_platform::metal;
    case graphics::rhi::RHIPlatform::Dawn:    return graphics::graphics_platform::dawn;
    default:                                  return graphics::graphics_platform::metal;
    }
}
} // anonymous namespace

EDITOR_INTERFACE u32 InitializeEngine(u32 rhiPlatform, u32 enableDebug)
{
	graphics::rhi::DeviceDesc desc{};
	desc.platform = static_cast<graphics::rhi::RHIPlatform>(rhiPlatform);
	desc.enableDebug = (enableDebug != 0);

	if (!graphics::initialize_with_device(desc)) {
		std::fprintf(stderr, "[InitializeEngine] initialize_with_device failed\n");
		return 0;
	}

	if (!graphics::bind_rhi_device_to_legacy()) {
		std::fprintf(stderr, "[InitializeEngine] bind_rhi_device_to_legacy failed\n");
		graphics::shutdown_rhi();
		return 0;
	}

	// Phase 4b: 按 RHIPlatform 决定是否走 legacy graphics::initialize(<platform>)。
	// Metal 走 metal::core(set_external_device 注入的 legacy path);
	// Vulkan/Dawn 是 self-contained RHI,不经 legacy platform_interface,跳过这一步。
	if (desc.platform == graphics::rhi::RHIPlatform::Metal) {
		const auto legacyPlatform = MapRHIPlatformToGraphicsPlatform(desc.platform);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
		if (!graphics::initialize(legacyPlatform)) {
#pragma GCC diagnostic pop
			std::fprintf(stderr, "[InitializeEngine] graphics::initialize(metal) failed (often shader blob path issue)\n");
			graphics::shutdown_rhi();
			return 0;
		}
	}

	return 1;
}

EDITOR_INTERFACE void ShutdownEngine()
{
	// 先 graphics::shutdown() 释放 metal::core 对 _external_device 的 retain + 所有 Metal 资源
	// 再 shutdown_rhi() 释放 rhi::MetalDevice 对 mtlDevice_ 的所有权
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
	graphics::shutdown();
#pragma GCC diagnostic pop
	graphics::shutdown_rhi();
}

EDITOR_INTERFACE u32 IsEngineInitialized()
{
	return graphics::is_rhi_initialized() ? 1 : 0;
}

EDITOR_INTERFACE u64 GetEngineDeviceHandle()
{
	// 返回 RHIDeviceBase* 的整数值。Editor 传给 MaterialPreviewAPI 等需要 device 的子模块。
	return reinterpret_cast<u64>(graphics::get_rhi_device());
}
#endif