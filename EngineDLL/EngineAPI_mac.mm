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
        // GCD dispatch source timer. GCD timers fire on a dedicated dispatch
        // queue and don't depend on the thread that scheduled them, which is
        // more robust than CFRunLoopTimer when the caller is a P/Invoke
        // from a managed runtime (dotnet) where the relationship between the
        // calling thread and the macOS main run loop is non-obvious.
        dispatch_source_t timer{ nullptr };
        dispatch_queue_t queue{ nullptr };
        s32 fps{ 60 };
        std::atomic<bool> running{ false };
    };

    std::vector<std::unique_ptr<editor_render_loop>> g_render_loops;

    void (*g_frame_cb)(u32, float) = nullptr;

    void fire_timer(void* info)
    {
        // For dispatch_source_set_event_handler_f, the first argument is
        // the user-set context (via dispatch_set_context). It's NOT the
        // dispatch_source_t pointer.
        auto* l = static_cast<editor_render_loop*>(info);
        if (!l || !l->running.load(std::memory_order_acquire)) return;
        if (!g_frame_cb) return;

        // GCD timer fires on a global concurrent queue worker thread, NOT
        // the main run loop thread. NSObject autoreleased objects returned
        // by Objective-C methods (e.g., MTKView::currentDrawable inside
        // blit_surface_and_present) would otherwise be added to a
        // last-resort pool and eventually crash with SIGSEGV in
        // objc_release when the pool boundary misaligns with the actual
        // object lifetime. Provide a per-callback autorelease pool so
        // the autoreleased references drain deterministically.
        @autoreleasepool {
            const u32 id = static_cast<u32>(l->id);
            const float dt = (l->fps > 0) ? (1.0f / static_cast<float>(l->fps)) : 0.016f;
            g_frame_cb(id, dt);
        }
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
    l->fps = fps;
    // Use a high-priority global concurrent queue so the timer fires
    // regardless of which thread scheduled it (P/Invoke from dotnet,
    // CFRunLoopGetMain() may return a different run loop than the one
    // Avalonia is pumping on macOS).
    l->queue = dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
    l->id = static_cast<u32>(g_render_loops.size()) + 1;
    g_render_loops.push_back(std::move(l));
    return g_render_loops.back()->id;
}

EDITOR_INTERFACE void EditorDestroyRenderLoop(u32 loop_id)
{
    if (loop_id == 0 || loop_id > g_render_loops.size()) return;
    auto& slot = g_render_loops[loop_id - 1];
    if (!slot) return;
    if (slot->timer) {
        dispatch_source_cancel(slot->timer);
        dispatch_release(slot->timer);
        slot->timer = nullptr;
    }
    slot.reset();
}

EDITOR_INTERFACE void EditorStopRenderLoop(u32 loop_id)
{
    if (loop_id == 0 || loop_id > g_render_loops.size()) return;
    auto& slot = g_render_loops[loop_id - 1];
    if (!slot) return;
    slot->running.store(false, std::memory_order_release);
    if (slot->timer) {
        dispatch_source_cancel(slot->timer);
        dispatch_release(slot->timer);
        slot->timer = nullptr;
    }
}

EDITOR_INTERFACE void EditorStartRenderLoop(u32 loop_id)
{
    if (loop_id == 0 || loop_id > g_render_loops.size()) return;
    auto& slot = g_render_loops[loop_id - 1];
    if (!slot || !slot->queue) return;
    if (slot->running.load(std::memory_order_acquire)) {
        EditorStopRenderLoop(loop_id);
    }
    slot->timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, slot->queue);
    if (!slot->timer) return;
    // Stash the loop pointer in the source's context so fire_timer can
    // recover it (GCD timers don't pass user data to the event handler).
    dispatch_set_context(slot->timer, slot.get());
    const uint64_t interval_ns = static_cast<uint64_t>(1e9) / static_cast<uint64_t>(slot->fps);
    dispatch_source_set_timer(slot->timer,
        dispatch_time(DISPATCH_TIME_NOW, interval_ns),
        interval_ns,
        interval_ns / 10);  // 10% tolerance
    dispatch_source_set_event_handler_f(slot->timer, &fire_timer);
    slot->running.store(true, std::memory_order_release);
    dispatch_resume(slot->timer);
}

#endif