#if defined(__clang__) && defined(__APPLE__)

#include "Common.h"
#include "CommonHeaders.h"
#include <objc/runtime.h>
#include <objc/message.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreFoundation/CoreFoundation.h>

#include <vector>
#include <atomic>

namespace
{
    struct editor_render_loop
    {
        u32 id{ 0 };
        // CFRunLoopTimerRef registered with the main run loop. The timer
        // fires on the AppKit main thread, whose autoreleasepool stack is
        // stable across event loop iterations (AppKit's per-callout pool
        // is reset every iteration and managed by the run loop, not by
        // libdispatch).
        //
        // The PREVIOUS implementation used dispatch_source_t on a global
        // concurrent queue (GCD worker thread). That thread's autoreleasepool
        // is libdispatch's last-resort pool, which recycles pool slot memory
        // aggressively when the worker goes idle. Combined with Avalonia's
        // NSOperationQueue free list reusing NSInvocationOperation wrapper
        // memory, this produced a use-after-free in __CFRunLoopPerCalloutARPEnd
        // on the main thread (Thread 0 in the crash dump): the per-callout
        // pool drain read a stale slot pointer pointing into recycled
        // wrapper memory. The crash signature was
        //     EXC_BAD_ACCESS KERN_INVALID_ADDRESS
        //     Thread 0: objc_release → AutoreleasePoolPage::releaseUntil
        //              → objc_autoreleasePoolPop → __CFRunLoopPerCalloutARPEnd
        //
        // Switching to CFRunLoopTimerRef on the main run loop eliminates
        // the GCD-worker-thread autoreleasepool entirely. The timer's
        // callback runs on the same thread as AppKit's per-callout pool,
        // so any autoreleased ObjC refs created inside the callback (engine
        // MetalCommandBuffer, MTKView::currentDrawable, etc.) land in the
        // per-callout pool and are drained by AppKit in the same iteration
        // — AppKit does not recycle pool slot memory mid-iteration. The
        // pattern matches `EngineTest`'s displayLinkCallback (see
        // EngineTest/IntegrationTests/TestRenderer.h:79).
        CFRunLoopTimerRef timer{ nullptr };
        s32 fps{ 60 };
        bool running{ false };
    };

    std::vector<std::unique_ptr<editor_render_loop>> g_render_loops;

    void (*g_frame_cb)(u32, float) = nullptr;

    void fire_timer(CFRunLoopTimerRef /*timer*/, void* info)
    {
        // The first argument is the CFRunLoopTimerRef (unused — we recover
        // the editor_render_loop pointer from the context passed to
        // CFRunLoopTimerCreate). Runs on the AppKit main thread.
        auto* l = static_cast<editor_render_loop*>(info);
        if (!l || !l->running) return;
        if (!g_frame_cb) return;

        const u32 id = static_cast<u32>(l->id);
        const float dt = (l->fps > 0) ? (1.0f / static_cast<float>(l->fps)) : 0.016f;

        // No @autoreleasepool block needed. The C# thunk is pinned via
        // GCHandle.Alloc in NativeEngineBridge.cs (see _thunkHandle), so
        // its delegate wrapper memory is not recycled. Autoreleased refs
        // created during the engine's synchronous render path
        // (PipelineRenderFrame + BlitRenderTargetToSurface) land in the
        // current per-callout autoreleasepool on the AppKit main thread
        // and are drained by __CFRunLoopPerCalloutARPEnd after this
        // callback returns — exactly the same lifecycle as AppKit's own
        // NSView drawing.
        g_frame_cb(id, dt);
    }
}

EDITOR_INTERFACE void* CreateEmbeddedRenderView(void* parent_view_handle, s32 width, s32 height)
{
    if (!parent_view_handle) return nullptr;

    @autoreleasepool {
        Class nsViewClass = objc_getClass("NSView");
        Class caMetalLayerClass = objc_getClass("CAMetalLayer");
        if (!nsViewClass || !caMetalLayerClass) return nullptr;

        id parentView = (id)parent_view_handle;
        CGRect frame = {{0, 0}, {(CGFloat)width, (CGFloat)height}};

        id renderView = ((id(*)(Class, SEL))objc_msgSend)(nsViewClass, sel_registerName("alloc"));
        renderView = ((id(*)(id, SEL, CGRect))objc_msgSend)(renderView, sel_registerName("initWithFrame:"), frame);
        if (!renderView) return nullptr;

        SEL setWantsLayerSel = sel_registerName("setWantsLayer:");
        ((void(*)(id, SEL, BOOL))objc_msgSend)(renderView, setWantsLayerSel, YES);

        SEL setLayerSel = sel_registerName("setLayer:");
        id metalLayer = ((id(*)(Class, SEL))objc_msgSend)(caMetalLayerClass, sel_registerName("layer"));
        ((void(*)(id, SEL, id))objc_msgSend)(renderView, setLayerSel, metalLayer);

        SEL setFrameSel = sel_registerName("setFrame:"), setAutoresizingMaskSel = sel_registerName("setAutoresizingMask:");
        ((void(*)(id, SEL, CGRect))objc_msgSend)(renderView, setFrameSel, frame);
        ((void(*)(id, SEL, NSUInteger))objc_msgSend)(renderView, setAutoresizingMaskSel, (NSUInteger)(2 | 8 | 16));

        SEL addSubviewSel = sel_registerName("addSubview:");
        ((void(*)(id, SEL, id))objc_msgSend)(parentView, addSubviewSel, renderView);

        return (void*)renderView;
    }
    return nullptr;
}

EDITOR_INTERFACE void DestroyEmbeddedRenderView(void* view_handle)
{
    if (!view_handle) return;
    @autoreleasepool {
        id view = (id)view_handle;
        SEL removeFromSuperviewSel = sel_registerName("removeFromSuperview");
        ((void(*)(id, SEL))objc_msgSend)(view, removeFromSuperviewSel);
        ((void(*)(id, SEL))objc_msgSend)(view, sel_registerName("release"));
    }
}

EDITOR_INTERFACE void ResizeEmbeddedRenderView(void* view_handle, s32 width, s32 height)
{
    if (!view_handle) return;
    @autoreleasepool {
        id view = (id)view_handle;
        CGRect frame = {{0, 0}, {(CGFloat)width, (CGFloat)height}};
        ((void(*)(id, SEL, CGRect))objc_msgSend)(view, sel_registerName("setFrame:"), frame);
    }
}

EDITOR_INTERFACE void EditorSetFrameCallback(void (*cb)(u32, float))
{
    g_frame_cb = cb;
}

EDITOR_INTERFACE u32 EditorCreateRenderLoop(s32 fps)
{
    if (fps <= 0) fps = 60;
    auto l = std::make_unique<editor_render_loop>();
    const u32 id = static_cast<u32>(g_render_loops.size()) + 1;
    l->id = id;
    l->fps = fps;
    g_render_loops.push_back(std::move(l));
    return id;
}

EDITOR_INTERFACE void EditorDestroyRenderLoop(u32 loop_id)
{
    if (loop_id == 0 || loop_id > g_render_loops.size()) return;
    auto& slot = g_render_loops[loop_id - 1];
    if (!slot) return;
    if (slot->timer) {
        CFRunLoopTimerInvalidate(slot->timer);
        // Invalidate removes the timer from the run loop (which releases
        // the run loop's retain on it) but does NOT release the caller-
        // owned +1 from CFRunLoopTimerCreate. Without this CFRelease the
        // timer leaks one ref per Destroy. Per Apple's CFRunLoopTimer
        // docs: "If you want to release the timer after invalidation,
        // call CFRelease."
        CFRelease(slot->timer);
        slot->timer = nullptr;
    }
    slot.reset();
}

EDITOR_INTERFACE void EditorStopRenderLoop(u32 loop_id)
{
    if (loop_id == 0 || loop_id > g_render_loops.size()) return;
    auto& slot = g_render_loops[loop_id - 1];
    if (!slot) return;
    slot->running = false;
    if (slot->timer) {
        CFRunLoopTimerInvalidate(slot->timer);
        // Same caller-retain rule as EditorDestroyRenderLoop above.
        CFRelease(slot->timer);
        slot->timer = nullptr;
    }
}

EDITOR_INTERFACE u32 EditorStartRenderLoop(u32 loop_id)
{
    if (loop_id == 0 || loop_id > g_render_loops.size()) return 0;
    auto& slot = g_render_loops[loop_id - 1];
    if (!slot) return 0;
    if (slot->running) {
        EditorStopRenderLoop(loop_id);
    }

    // Fire on the AppKit main run loop (CFRunLoopGetMain()). When called
    // from the editor process, this is the same run loop Avalonia is
    // pumping its AppKit thread on, so the timer's callback runs on the
    // same thread as Avalonia's UI compositor. Autoreleased ObjC refs
    // created inside the callback land in the AppKit per-callout
    // autoreleasepool — drained by AppKit in the same iteration, no
    // memory recycling between iterations.
    CFRunLoopTimerContext context{};
    context.info = slot.get();
    slot->timer = CFRunLoopTimerCreate(
        kCFAllocatorDefault,
        CFAbsoluteTimeGetCurrent(),
        1.0 / static_cast<CFTimeInterval>(slot->fps),
        0,  // flags (no tolerance)
        0,  // order (default priority)
        &fire_timer,
        &context);
    if (!slot->timer) return 0;
    CFRunLoopAddTimer(CFRunLoopGetMain(), slot->timer, kCFRunLoopCommonModes);

    slot->running = true;
    return loop_id;
}

#endif