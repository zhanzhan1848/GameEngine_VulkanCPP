// CaptureAPI.cpp - Backbuffer capture from Editor.
//
// PRAGMATIC MVP IMPLEMENTATION (macOS-only).
//
// Background: Metal swap chain drawables are `framebufferOnly` by default,
// meaning MTLBlitCommandEncoder cannot read from them. The engine's current
// MetalPostProcess writes the final composite directly to the drawable, so
// there is no offscreen texture we can hand to RenderTexture::ReadBack without
// invasive renderer surgery.
//
// The proper engine-side readback path requires:
//   1. Adding a non-framebufferOnly "scene color" texture to MetalSurface
//   2. Modifying post_process to write to scene color first, then blit to drawable
//   3. Plumbing a capture-request flag from CaptureAPI into the renderer
// This is scheduled for Phase 1 (Renderer rewrite onto RHI) where Renderer
// owns RenderTexture outputs directly and the readback path is natural.
//
// This MVP uses CoreGraphics screen capture to grab the rendered NSWindow:
//   - Pros: Zero renderer changes, works today, captures exactly what the user sees.
//   - Cons: macOS-only; captures the whole window content (no Editor overlay
//           today since the engine renders into the NSWindow's contentView).
//
// IMPLEMENTATION NOTE: CGWindowListCreateImage was marked `obsoleted=15.0` in
// macOS 15 Sequoia — a *hard* compile error on modern SDKs, not a warning. The
// symbol still ships in libCGCoreGraphics (Apple rarely removes APIs), so we
// resolve it via dlsym and call through a function pointer. This sidesteps the
// compiler availability check without lying about deployment target.
//
// When the engine-side readback lands, swap the body of CaptureBackbuffer to
// use RenderTexture::ReadBack on the scene color target. The C ABI stays the same.

#include "Common.h"
#include "CommonHeaders.h"
#include "EngineAPIInternal.h"
#include "Platform/Window.h"
#include "Graphics/Renderer.h"

#include <cstdlib>
#include <cstdio>

#if defined(__APPLE__)
#include <dlfcn.h>
#include <objc/message.h>
#include <objc/runtime.h>
// CGWindow.h declares CGWindowListCreateImage with `obsoleted=15.0`, which is
// a hard error on modern SDKs. Avoid including it; declare the bits we need.
#include <CoreGraphics/CGGeometry.h>
#include <CoreGraphics/CGImage.h>
#include <CoreGraphics/CGColorSpace.h>
#include <CoreGraphics/CGBitmapContext.h>

// Types & constants from CGWindow.h that we still need. Re-declared locally
// instead of pulling in the deprecated header.
typedef uint32_t CGWindowID;
typedef uint32_t CGWindowListOption;
typedef uint32_t CGWindowImageOption;
#define PRIMAL_KCG_WINDOW_LIST_OPTION_INCLUDING_WINDOW (1u << 3)
#define PRIMAL_KCG_WINDOW_IMAGE_DEFAULT 0

#endif

using namespace primal;

#if defined(__APPLE__)
namespace {

// NS::Window (Metal-cpp bindings) does not wrap -[NSWindow windowNumber].
// Use objc_msgSend directly. Stays in .cpp (no .mm needed).
// Note: `id` and `NSInteger` must be qualified — `using namespace primal`
// pulls in `primal::id`, making bare `id` ambiguous.
long ns_window_window_number(void* ns_window_handle) {
    SEL sel = sel_registerName("windowNumber");
    using ObjcMsgSend = long (*)(::id, SEL);
    auto func = reinterpret_cast<ObjcMsgSend>(objc_msgSend);
    return func(reinterpret_cast<::id>(ns_window_handle), sel);
}

// Resolve CGWindowListCreateImage via dlsym to avoid the obsoleted=15.0 hard
// error. Returns nullptr if the symbol isn't present (very unlikely on any
// shipping macOS).
using PFN_CGWindowListCreateImage = CGImageRef (*)(CGRect,
                                                    CGWindowListOption,
                                                    CGWindowID,
                                                    CGWindowImageOption);
PFN_CGWindowListCreateImage resolve_cg_window_list_create_image() {
    void* sym = dlsym(RTLD_DEFAULT, "CGWindowListCreateImage");
    return reinterpret_cast<PFN_CGWindowListCreateImage>(sym);
}

// Capture a single NSWindow by its windowNumber. Returns RGBA8 buffer
// (top-left origin, 4 bytes/pixel) and dimensions via out-params.
// Caller owns the returned buffer (heap-allocated, free()-able).
// Returns nullptr on failure.
void* capture_window_pixels(void* ns_window_handle, u32* out_w, u32* out_h) {
    if (!ns_window_handle || !out_w || !out_h) return nullptr;

    long window_number = ns_window_window_number(ns_window_handle);
    if (window_number <= 0) return nullptr;

    auto pfn = resolve_cg_window_list_create_image();
    if (!pfn) return nullptr;

    // CGRectNull = capture whole window regardless of where it is on screen.
    CGImageRef image = pfn(
        CGRectNull,
        PRIMAL_KCG_WINDOW_LIST_OPTION_INCLUDING_WINDOW,
        static_cast<CGWindowID>(window_number),
        PRIMAL_KCG_WINDOW_IMAGE_DEFAULT);
    if (!image) return nullptr;

    const size_t width  = CGImageGetWidth(image);
    const size_t height = CGImageGetHeight(image);
    if (width == 0 || height == 0) {
        CGImageRelease(image);
        return nullptr;
    }

    // Draw into a 32-bit RGBA bitmap context.
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    if (!cs) {
        CGImageRelease(image);
        return nullptr;
    }
    const size_t bytes_per_row = width * 4;
    const size_t buf_size = bytes_per_row * height;

    void* pixels = std::malloc(buf_size);
    if (!pixels) {
        CGColorSpaceRelease(cs);
        CGImageRelease(image);
        return nullptr;
    }

    CGContextRef ctx = CGBitmapContextCreate(
        pixels, width, height, 8, bytes_per_row, cs,
        kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    if (!ctx) {
        std::free(pixels);
        CGColorSpaceRelease(cs);
        CGImageRelease(image);
        return nullptr;
    }

    // CGContextDrawImage uses Y-up by default. Flip to top-left origin to
    // match standard image conventions (PNG, RGBA buffer layout for Editor).
    CGContextTranslateCTM(ctx, 0, height);
    CGContextScaleCTM(ctx, 1.0, -1.0);
    CGContextDrawImage(ctx, CGRectMake(0, 0, width, height), image);

    CGContextRelease(ctx);
    CGColorSpaceRelease(cs);
    CGImageRelease(image);

    *out_w = static_cast<u32>(width);
    *out_h = static_cast<u32>(height);
    return pixels;
}

} // anonymous namespace
#endif

EDITOR_INTERFACE u32 CaptureBackbuffer(u32 surface_id, void** out_data, u64* out_size)
{
    if (!out_data || !out_size) {
        std::fprintf(stderr, "[CaptureBackbuffer] null out_data or out_size\n");
        return 0;
    }
    *out_data = nullptr;
    *out_size = 0;

    if (surface_id >= engine_dll::GetSurfaceCount()) {
        std::fprintf(stderr, "[CaptureBackbuffer] invalid surface_id %u\n", surface_id);
        return 0;
    }

    auto* rs = engine_dll::GetSurface(surface_id);
    if (!rs || !rs->window.is_valid()) {
        std::fprintf(stderr, "[CaptureBackbuffer] surface %u has no valid window\n", surface_id);
        return 0;
    }

#if defined(__APPLE__)
    void* ns_window = rs->window.handle();
    u32 w = 0, h = 0;
    void* pixels = capture_window_pixels(ns_window, &w, &h);
    if (!pixels) {
        std::fprintf(stderr, "[CaptureBackbuffer] window capture returned null "
                             "(window may be offscreen/minimized, or symbol missing)\n");
        return 0;
    }
    *out_data = pixels;
    *out_size = static_cast<u64>(w) * static_cast<u64>(h) * 4ull;
    return 1;
#else
    std::fprintf(stderr, "[CaptureBackbuffer] not yet implemented on this platform\n");
    return 0;
#endif
}

EDITOR_INTERFACE void FreeCaptureBuffer(void* buffer)
{
    if (buffer) std::free(buffer);
}
