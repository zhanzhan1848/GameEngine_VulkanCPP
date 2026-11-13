#include "Engine/Common/CoreLogStream.h"
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
#include "DiagnosticsAPI.h"
#include "EngineAPIInternal.h"
#include "../Graphics/Renderer.h"
#include "../Graphics/RHI/Core/RHIDevice.h"
#include "../Graphics/Metal/MetalCore.h"
#include "../Platform/PlatformTypes.h"
#include "../Platform/Platform.h"
#include <dlfcn.h>
#include <cstdio>
#include <iostream>
#if defined(__APPLE__)
#include <objc/runtime.h>
#include <objc/message.h>
#endif

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

// === Phase 1 Sub-step 1.2.7': Embedded render surface ===
// Embeds the engine's forward pipeline into a host NSView owned by the
// editor (Avalonia). Unlike CreateRenderSurface which creates a new
// standalone NSWindow, this path attaches an MTKView as a subview of
// host_view so the render target lives inside the host's contentView.
//
// Returns a surface id suitable for RenderFrame(surface_id, ...).
EDITOR_INTERFACE u32 CreateEmbeddedRenderSurface(void* host_view, s32 width, s32 height)
{
	assert(host_view);
	graphics::render_surface rs{};
	// Register the external host view as a platform window FIRST so the
	// render_surface has a valid window_id for later ResizeRenderSurface
	// calls. create_external_surface takes the pre-registered window instead
	// of registering it itself — the previous design had both sites calling
	// platform::register_external_window, which leaked one slot per embed
	// and left rs.window invalid (a default-constructed window_id) so any
	// resize from C# dereferenced out-of-bounds memory and SIGBUS'd.
	rs.window = platform::register_external_window(host_view, (u32)width, (u32)height);
	rs.surface = graphics::create_external_surface(rs.window);
	if (!rs.surface.is_valid()) {
		PRIMAL_LOG_STREAM(Error, "Engine") << "[EngineAPI] CreateEmbeddedRenderSurface failed: host view "
		          << host_view << " rejected by graphics::create_external_surface "
		          << "(likely 0x0 bounds or MTKView alloc failed)" << std::endl;
		// Roll back the window slot too so it doesn't leak.
		platform::remove_window(rs.window.get_id());
		return (u32)-1;
	}
	surfaces.emplace_back(rs);
	return (u32)surfaces.size() - 1;
}

// Returns the engine-side NSWindow* that hosts the MTKView for this
// surface. For embedded surfaces (CreateEmbeddedRenderSurface) this is
// typically a separate child NSWindow the platform layer created. For
// standard surfaces (CreateRenderSurface) it is the NSWindow that owns
// the MTKView directly. May return NULL on platforms that do not use
// NSWindow for surface presentation.
EDITOR_INTERFACE void* GetSurfaceView(u32 id)
{
	assert(id < surfaces.size());
	return graphics::metal::core::external_surface_view(surfaces[id].surface.get_id());
}

// Reposition an embedded surface's MTKView within its host. Used by the
// editor to keep the MTKView aligned with the host UI control's bounds.
// Coordinates are in the host's coordinate space (top-left origin on
// macOS NSView).
EDITOR_INTERFACE void SetEmbeddedSurfaceFrame(u32 id, s32 x, s32 y, u32 w, u32 h)
{
	assert(id < surfaces.size());
	graphics::metal::core::external_surface_view_frame(
		surfaces[id].surface.get_id(), x, y, w, h);
}

// Register a C function pointer as the per-frame render callback for an
// embedded surface's MTKView, and switch the MTKView to AppKit-driven
// automatic draw mode (CVDisplayLink drives drawRect: at the display's
// refresh rate, ~60Hz on macOS). The callback runs INSIDE AppKit's
// per-callout autoreleasepool (the same one that drains MTKView's own
// drawable references), so any autoreleased ObjC refs the editor creates
// (MTLCommandBuffer, currentDrawable, etc.) are drained by THIS pool —
// no cross-thread pool contamination with LaunchServices' _LSExceptions
// shared singleton, which was the root cause of the previous
// GCD-worker-driven fire_timer SIGSEGV at DRAW #N.
//
// The callback signature is void(*)(void* mtk_view, float dt). mtk_view
// is the same pointer that GetSurfaceView returns. dt is a nominal 1/60s
// — the editor's test scene has no animated uniforms, but the value is
// passed for completeness.
//
// Mac only. Other platforms are a no-op (return 0). The previous
// GCD-worker fire_timer path in EngineAPI_mac.mm is now obsolete but
// kept for engine test harnesses that don't use the editor's pipeline.
EDITOR_INTERFACE u32 EnableSurfaceAutoDraw(u32 id, void(*cb)(void*, float))
{
#if defined(__APPLE__)
	assert(id < surfaces.size());
	void* view = graphics::metal::core::external_surface_view(
		surfaces[id].surface.get_id());
	if (!view || !cb) return 0;
	extern void PassThroughMTKView_RegisterRenderCallback(void* view, void(*cb)(void*, float));
	extern void PassThroughMTKView_EnableAutoDraw(void* view);
	PassThroughMTKView_RegisterRenderCallback(view, cb);
	PassThroughMTKView_EnableAutoDraw(view);
	return 1;
#else
	(void)id; (void)cb;
	return 0;
#endif
}

// Disable AppKit-driven automatic draw for an embedded surface's MTKView
// and unregister its render callback. Mac only.
EDITOR_INTERFACE void DisableSurfaceAutoDraw(u32 id)
{
#if defined(__APPLE__)
	assert(id < surfaces.size());
	void* view = graphics::metal::core::external_surface_view(
		surfaces[id].surface.get_id());
	if (!view) return;
	extern void PassThroughMTKView_DisableAutoDraw(void* view);
	extern void PassThroughMTKView_UnregisterRenderCallback(void* view);
	PassThroughMTKView_UnregisterRenderCallback(view);
	PassThroughMTKView_DisableAutoDraw(view);
#else
	(void)id;
#endif
}

// Returns the contentView (NSView*) of the first visible NSWindow, suitable
// for use as the host argument to CreateEmbeddedRenderSurface. Returns NULL
// if no visible window is found. macOS only.
//
// DEPRECATED 2026-07-03 — DO NOT USE. This function uses
// `NS::Application::sharedApplication()->windows()[0]->contentView()`, which
// is unreliable for three reasons documented by the editor maintainer:
//
//   1. `windows()[0]` is not guaranteed to be the editor's MainWindow.
//      Avalonia (and many other frameworks) registers hidden windows at
//      index 0 (splash, transient, accessibility), so the first window is
//      often NOT the window containing the 3D viewport.
//
//   2. The returned contentView is the AvaloniaRootView, not a
//      NativeControlHost's child NSView. Attaching the MTKView to it puts
//      the engine outside Avalonia's managed native subtree, which on
//      macOS 26.5.1 / Apple Silicon triggers
//      com.apple.FramePacing.LayerStateSyncQueue to invoke an
//      MTKView __initCommon deferred block that crashes with a NULL
//      function pointer jump inside objc_release. The 10-frame-then-
//      deadlock pattern in the editor was this exact crash.
//
//   3. Even if `windows()[0]` happens to be the right window, the
//      contentView's bounds may be 0×0 at the time the engine first tries
//      to acquire a drawable (Avalonia's Measure/Arrange/Loaded sequence
//      is not yet complete). `currentDrawable` returns nil, the engine
//      logs a "first draw too early" warning, and the surface never
//      receives the first frame.
//
// The supported path is to obtain the host view from a NativeControlHost
// subclass (see `PrimalEditor_Avalonia/Views/Engine3D/MTKViewHost.cs`).
// That NSView is positioned by Avalonia's layout system, parented inside
// Avalonia's managed native subtree, and has non-zero bounds at the
// time TryInitializeFromLayout runs (gated by LayoutUpdated).
//
// This function is preserved as a deprecation stub that always returns
// nullptr, so any legacy caller fails fast instead of attaching the
// MTKView to the wrong NSView. The symbol remains exported for ABI
// compatibility with already-built editor binaries.
EDITOR_INTERFACE void* GetAvaloniaContentView()
{
#if defined(__APPLE__)
    // Intentionally return nullptr. See deprecation comment above.
    (void)NS::Application::sharedApplication();  // suppress unused-warning
    return nullptr;
#else
    return nullptr;
#endif
}

EDITOR_INTERFACE void ResizeRenderSurface(u32 id, u32 width, u32 height)
{
	assert(id < surfaces.size());
	surfaces[id].window.resize(width, height);
	surfaces[id].surface.resize(width, height);
}

#endif

// === Phase 1 Sub-step 1.2.5': Engine lifecycle entry for Editor ===
// 一站式封装 Editor 启动/关闭引擎的标准序列：
//   initialize_with_device → bind_rhi_device_to_legacy → initialize(<mapped platform>)
// Editor (C#) 通过 P/Invoke 调用，不需要知道三步顺序。
//
// rhiPlatform: RHIPlatform 枚举值（u32）。
// enableDebug: 0=false, 非 0=true。
// 返回 1=成功，0=失败（任何一步失败都会回滚已初始化的部分）。
//
// R17-B（Windows 十六测）：以下五个引擎生命周期导出是平台无关的纯 RHI
// 调用，原先整体位于上方 __clang__ 分支——MSVC 下 EngineDLL.dll 缺这些
// 导出，PrimalPlayer（R16-B 激活 Windows 无窗口路径）链接即 LNK2019
// （InitializeEngine/ShutdownEngine/GetEngineDeviceHandle 三符号）。
// 移出门控全平台编译，函数体零改动（仅弃用警告抑制补 MSVC 形态——
// #pragma GCC 在 MSVC 是未知 pragma）。
// FlushGPU/IsEngineInitialized 同门控同根因（Windows 消费者 PrimalEngineCli
// 此前以 _WIN32 NOT_SUPPORTED 桩绕开，见 runbook §9 #21），一并补齐。
// 门控取 _MSC_VER||__clang__ 而非裸 #else：Common.h 对 GCC 不定义
// EDITOR_INTERFACE，GCC 维持本 TU 不编译生命周期导出的既有现状。
#if defined(_MSC_VER) || defined(__clang__)
#include "../Graphics/RHI/Core/RHIDevice.h"
#include "DiagnosticsAPI.h"

using namespace primal;

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

	// 日志 L0：初始化失败细节经 diag::Emit 发射（ConsoleSink → stderr +
	// RingBuffer 黑匣子 + 宿主回调），fprintf 直出的旧行为由 ConsoleSink 承接。
	using primal::engine_dll::diag::Emit;
	using primal::engine_dll::diag::kLevelError;

	if (!graphics::initialize_with_device(desc)) {
		Emit(kLevelError, "Engine", "[InitializeEngine] initialize_with_device failed");
		return 0;
	}

	if (!graphics::bind_rhi_device_to_legacy()) {
		Emit(kLevelError, "Engine", "[InitializeEngine] bind_rhi_device_to_legacy failed");
		graphics::shutdown_rhi();
		return 0;
	}

	// Phase 4b: 按 RHIPlatform 决定是否走 legacy graphics::initialize(<platform>)。
	// Metal 走 metal::core(set_external_device 注入的 legacy path);
	// Vulkan/Dawn 是 self-contained RHI,不经 legacy platform_interface,跳过这一步。
	if (desc.platform == graphics::rhi::RHIPlatform::Metal) {
		const auto legacyPlatform = MapRHIPlatformToGraphicsPlatform(desc.platform);
#if defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#else
#pragma warning(push)
#pragma warning(disable : 4996) // graphics::initialize/shutdown [[deprecated]]：三步拆装顺序刻意保留（见本函数注释）
#endif
		if (!graphics::initialize(legacyPlatform)) {
#if defined(__clang__)
#pragma GCC diagnostic pop
#else
#pragma warning(pop)
#endif
			Emit(kLevelError, "Engine", "[InitializeEngine] graphics::initialize(metal) failed (often shader blob path issue)");
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
#if defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#else
#pragma warning(push)
#pragma warning(disable : 4996) // 同上：graphics::shutdown() 弃用为刻意的逆序三步拆装
#endif
	graphics::shutdown();
#if defined(__clang__)
#pragma GCC diagnostic pop
#else
#pragma warning(pop)
#endif
	graphics::shutdown_rhi();
}

EDITOR_INTERFACE void FlushGPU()
{
	// Force GPU→CPU sync so subsequent DebugReadRenderTarget / SaveRenderTargetAsPng
	// observes the finished frame. Without this the transfer-queue copy can run
	// before the graphics queue's render-pass writes complete.
	if (auto* device = graphics::get_rhi_device()) {
		device->WaitIdle();
	}
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
#endif // defined(_MSC_VER) || defined(__clang__)