// PassThroughMTKView.mm
//
// Objective-C++ subclass of MTKView that overrides -hitTest: to return
// nil. This makes the view pass-through for pointer events while
// remaining a normal MTKView for drawing.
//
// Why this is needed: the editor embeds an MTKView on top of the
// Avalonia contentView so the Metal surface is visible. By default an
// NSView's -hitTest: returns the view itself for any point inside its
// bounds, which would make AppKit route all pointer events to the
// MTKView and starve the underlying Avalonia controls (EngineViewport3D,
// etc.). Returning nil tells AppKit "this view doesn't claim the
// event" so the hit-test chain continues to the next subview — which
// is the Avalonia control. Drawing is unaffected.
//
// Why we subclass instead of method swizzling: AppKit's
// CALayer-backed hit-test path does NOT go through objc_msgSend for
// NSView.hitTest:, so a runtime swizzle of -hitTest: is never called
// (verified empirically: the swizzle is applied to the class's method
// table, but the swizzled function is never invoked). Subclassing
// overrides the method at the class level, which IS what the layer
// hit-test path consults.
//
// Layout compatibility: the engine's metal_surface stores its view
// as `MTK::View*` (C++ wrapper from OSAPI/MAC/MetalKit/MTKView.hpp).
// That wrapper is purely a thin C++ facade over the underlying
// Objective-C object — `NS::Object::sendMessage(this, sel, ...)`
// passes `this` directly to `objc_msgSend` as the `id` self. Since
// the C++ wrapper has no data members (see
// OSAPI/MAC/Foundation/NSObject.hpp: `class Object : public
// Referencing<Object, objc_object>` with no fields, only static
// helpers), the wrapper's memory layout is exactly that of `id`.
// Therefore a `PassThroughMTKView*` instance is bitwise layout-
// compatible with `MTK::View*` and `reinterpret_cast<MTK::View*>(
// pass_through_mtk_view)` is sound. The two existing call sites
// (MetalCore.cpp:47 `end_frame` and MetalPostProcess.cpp:195
// `post_process`) call `currentDrawable()` and `draw()` via the
// wrapper, which uses sendMessage → objc_msgSend → resolves to
// MTKView's instance methods inherited by PassThroughMTKView.
// No changes needed at those call sites.

#import <MetalKit/MetalKit.h>
#import <Metal/Metal.h>

@interface PassThroughMTKView : MTKView
@end

@implementation PassThroughMTKView

- (NSView *)hitTest:(NSPoint)point
{
    (void)point;
    return nil;
}

@end

// C entry point: allocate a PassThroughMTKView with the given frame and
// Metal device. Returns the raw Objective-C pointer (id). On failure
// (alloc or init returns nil), logs a detailed error to stderr so
// callers can diagnose without re-running with a debugger.
extern "C" void* PassThroughMTKView_Create(void* device, double x, double y, double w, double h)
{
    if (!device) {
        fprintf(stderr, "[PassThroughMTKView_Create] device is null\n");
        return nullptr;
    }
    CGRect frame = CGRectMake((CGFloat)x, (CGFloat)y, (CGFloat)w, (CGFloat)h);
    PassThroughMTKView* v = [[PassThroughMTKView alloc] initWithFrame:frame
                                                              device:(id<MTLDevice>)device];
    if (!v) {
        fprintf(stderr, "[PassThroughMTKView_Create] alloc/init returned nil (frame=%f,%f %fx%f device=%p)\n",
                x, y, w, h, device);
    }
    return (void*)v;
}
