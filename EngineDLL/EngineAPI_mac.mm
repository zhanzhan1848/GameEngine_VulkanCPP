#if defined(__clang__) && defined(__APPLE__)

#include "Common.h"
#include "CommonHeaders.h"
#include <objc/runtime.h>
#include <objc/message.h>
#import <CoreGraphics/CoreGraphics.h>

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

#endif
