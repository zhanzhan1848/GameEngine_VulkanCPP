// PassThroughMTKView.mm
//
// Objective-C++ subclass of MTKView that overrides -hitTest: to return
// nil and overrides -drawRect: to drive the editor's render callback.
//
// Hit-test pass-through: by default an NSView's -hitTest: returns the view
// itself for any point inside its bounds, which would make AppKit route
// all pointer events to the MTKView and starve the underlying Avalonia
// controls. Returning nil tells AppKit "this view doesn't claim the
// event" so the hit-test chain continues to the next subview — which is
// the Avalonia control. Drawing is unaffected.
//
// Why we subclass instead of method swizzling: AppKit's CALayer-backed
// hit-test path does NOT go through objc_msgSend for NSView.hitTest: —
// a runtime swizzle of -hitTest: is applied to the class's method table
// but never invoked. Subclassing overrides the method at the class level,
// which IS what the layer hit-test path consults.
//
// Layout compatibility: the engine's metal_surface stores its view as
// `MTK::View*` (C++ wrapper from OSAPI/MAC/MetalKit/MTKView.hpp). That
// wrapper is purely a thin C++ facade over the underlying Objective-C
// object — `NS::Object::sendMessage(this, sel, ...)` passes `this`
// directly to `objc_msgSend` as the `id` self. Since the C++ wrapper
// has no data members (OSAPI/MAC/Foundation/NSObject.hpp: `class Object :
// public Referencing<Object, objc_object>` with no fields, only static
// helpers), the wrapper's memory layout is exactly that of `id`.
// Therefore a `PassThroughMTKView*` instance is bitwise layout-compatible
// with `MTK::View*` and `reinterpret_cast<MTK::View*>(pass_through_mtk_view)`
// is sound.
//
// Why override -drawRect: instead of installing an MTKViewDelegate:
// the previous implementation drove the MTKView manually from a GCD worker
// timer (EngineAPI_mac.mm:50 fire_timer), which produced a cross-thread
// autoreleasepool contamination with LaunchServices' _LSExceptions
// NSString cache. The .ips trace from lldb (thread 10, fire_timer) showed
// objc_release → AutoreleasePoolPage::releaseUntil with a tagged-pointer
// class address from LaunchServices' shared singleton. By overriding
// -drawRect: on the MTKView subclass and driving pView->draw() from the
// main thread's CFRunLoop (via CVDisplayLink or DispatcherTimer), the
// editor's render code runs inside AppKit's per-callout autoreleasepool
// (the same one that drains MTKView's own drawable references). All
// autoreleased ObjC refs created during render (MTLCommandBuffer,
// currentDrawable, etc.) are drained by THIS pool — no cross-pool
// contamination. Same pattern Unity Hub, Godot Editor, Xcode, and
// most CAD/DCC tools use.

#import <MetalKit/MetalKit.h>
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include <unordered_map>

extern "C" void PassThroughMTKView_DrawCallback(void* mtk_view);

@interface PassThroughMTKView : MTKView
@end

@implementation PassThroughMTKView

- (NSView *)hitTest:(NSPoint)point
{
    (void)point;
    return nil;
}

// Override -drawRect: to drive the editor's render code from inside
// AppKit's per-callout autoreleasepool. This is invoked either by
// setNeedsDisplay (manual mode, set by metal_surface::create_external)
// OR by AppKit's CVDisplayLink (automatic mode, set via setPaused:NO +
// setEnableSetNeedsDisplay:NO). Either way, drawRect: is called on the
// main thread with AppKit's autoreleasepool scoping the body, so any
// autoreleased ObjC refs the editor creates inside the callback land
// in the same pool that drains MTKView's own drawable refs.
//
// Note: when MTKView has a delegate installed, AppKit prefers the
// delegate's drawInMTKView: over drawRect:. We do NOT install a delegate
// here so drawRect: is always called.
- (void)drawRect:(NSRect)dirtyRect
{
    (void)dirtyRect;
    PassThroughMTKView_DrawCallback((void*)self);
}

@end

// Per-surface render callback registry. drawRect: above calls into
// PassThroughMTKView_DrawCallback, which looks up the registered callback
// for this view and invokes it. Keyed by the PassThroughMTKView pointer
// itself, which is also the engine's surface_id (1:1 in the editor's
// current path). We use a std::unordered_map guarded by a single
// NSLock rather than NSDictionary because the value is a C function
// pointer, not an Objective-C object (NSDictionary requires its values
// to be NSObject subclasses; boxing via NSValue works but adds an
// allocation per frame, which we don't need for a hot path).
static std::unordered_map<void*, void(*)(void*, float)>* g_render_callbacks = nullptr;
static NSLock* g_callback_lock = nil;

__attribute__((constructor))
static void PassThroughMTKView_Init(void)
{
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        g_render_callbacks = new std::unordered_map<void*, void(*)(void*, float)>();
        g_callback_lock = [[NSLock alloc] init];
    });
}

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

// Called from PassThroughMTKView.drawRect: above. Looks up the registered
// callback for this view and invokes it. Returns silently if no callback
// is registered yet (the editor may attach the MTKView before wiring
// its callback).
extern "C" void PassThroughMTKView_DrawCallback(void* view_handle)
{
    if (!g_render_callbacks || !view_handle) return;
    [g_callback_lock lock];
    auto it = g_render_callbacks->find(view_handle);
    void(*cb)(void*, float) = (it != g_render_callbacks->end()) ? it->second : nullptr;
    [g_callback_lock unlock];
    if (cb) {
        cb(view_handle, 1.0f / 60.0f);
    }
}

// Register a per-surface render callback. Replaces any previous callback
// for the same view. Called from the C ABI in response to the C# editor
// installing EngineMetalView3D's render hook.
extern "C" void PassThroughMTKView_RegisterRenderCallback(void* view_handle,
                                                          void(*cb)(void*, float))
{
    if (!g_render_callbacks || !view_handle) return;
    [g_callback_lock lock];
    if (cb) {
        (*g_render_callbacks)[view_handle] = cb;
    } else {
        g_render_callbacks->erase(view_handle);
    }
    [g_callback_lock unlock];
}

// Unregister a previously-registered callback. Safe to call multiple times.
extern "C" void PassThroughMTKView_UnregisterRenderCallback(void* view_handle)
{
    if (!g_render_callbacks || !view_handle) return;
    [g_callback_lock lock];
    g_render_callbacks->erase(view_handle);
    [g_callback_lock unlock];
}

// STUB: previously switched MTKView to AppKit-driven automatic draw mode
// (setPaused:NO, setEnableSetNeedsDisplay:NO, setPreferredFramesPerSecond:60).
// That mode was supposed to drive drawRect: at CVDisplayLink rate and put
// render code inside AppKit's per-callout autoreleasepool — eliminating
// the cross-thread pool contamination we saw with the GCD-worker-driven
// fire_timer path.
//
// Empirically, however, switching to automatic mode on Apple Silicon
// (macOS 26.5.1) triggers an MTKView __initCommon deferred block
// (visible in .ips traces at [MTKView __initCommon]_block_invoke via
// _dispatch_main_queue_drain) that crashes with the SAME tagged-pointer
// objc_release SIGSEGV — just on the main queue's autoreleasepool
// instead of the GCD worker's. The cross-pool contamination shifts
// layers but does not go away.
//
// The cleanest path is therefore: keep the MTKView in manual mode
// (paused=YES, enableSetNeedsDisplay=YES, as metal_surface::create_external
// sets it), and let the engine's existing fire_timer call pView->draw().
// The cross-pool contamination on the fire_timer side is also real, but
// it lives at a different code path that does not trigger MTKView's
// init block. With the FreeList.h fix (commit a9a80f2) the fire_timer
// path survived for 22+ seconds with 10 DRAWs before a graceful
// exit. The auto-draw path crashed earlier because of the MTKView init
// block race. So manual mode is strictly safer until Apple's
// autoreleasepool handling is fixed for this case.
//
// This function is kept as a no-op ABI stub so the editor can call
// EnableSurfaceAutoDraw without changes on its side; the registration
// of the per-frame render callback still happens, but pView->draw() is
// driven manually by the engine (which is what metal_surface already
// does in end_frame via its existing metal_command wrapper).
extern "C" void PassThroughMTKView_EnableAutoDraw(void* view_handle)
{
    PassThroughMTKView* v = (__bridge PassThroughMTKView*)view_handle;
    if (!v) return;
    // Intentionally do NOT change paused/enableSetNeedsDisplay. The
    // default from metal_surface::create_external is manual mode,
    // which is the only path that does not trigger MTKView's deferred
    // init block and crash on Apple Silicon.
    (void)v;
}

// Disable AppKit-driven automatic draw and revert to manual mode (engine
// calls pView->draw() to advance drawables). Used when the editor wants
// to drive the loop from its own timer instead of AppKit.
extern "C" void PassThroughMTKView_DisableAutoDraw(void* view_handle)
{
    PassThroughMTKView* v = (__bridge PassThroughMTKView*)view_handle;
    if (!v) return;
    [v setPaused:YES];
    [v setEnableSetNeedsDisplay:YES];
}
