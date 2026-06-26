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
        CFRunLoopTimerRef timer{ nullptr };
        CFRunLoopRef run_loop{ nullptr };
        s32 fps{ 60 };
        bool running{ false };  // single-threaded; main run loop only
    };

    std::vector<std::unique_ptr<editor_render_loop>> g_render_loops;

    void (*g_frame_cb)(u32, float) = nullptr;

    void fire_timer(CFRunLoopTimerRef, void* info)
    {
        auto* l = static_cast<editor_render_loop*>(info);
        if (!l->running) return;
        if (g_frame_cb) {
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
    l->run_loop = CFRunLoopGetMain();
    if (!l->run_loop) return 0;
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
        CFRunLoopTimerInvalidate(slot->timer);
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
        slot->timer = nullptr;
    }
}

EDITOR_INTERFACE void EditorStartRenderLoop(u32 loop_id)
{
    if (loop_id == 0 || loop_id > g_render_loops.size()) return;
    auto& slot = g_render_loops[loop_id - 1];
    if (!slot || !slot->run_loop) return;
    if (slot->running) {
        EditorStopRenderLoop(loop_id);
    }
    CFRunLoopTimerContext ctx{};
    ctx.info = slot.get();
    const CFTimeInterval interval = 1.0 / static_cast<CFTimeInterval>(slot->fps);
    slot->timer = CFRunLoopTimerCreate(
        kCFAllocatorDefault,
        CFAbsoluteTimeGetCurrent() + interval,
        interval,
        0,
        0,
        &fire_timer,
        &ctx);
    if (!slot->timer) return;
    CFRunLoopAddTimer(slot->run_loop, slot->timer, kCFRunLoopCommonModes);
    slot->running = true;
}

#endif