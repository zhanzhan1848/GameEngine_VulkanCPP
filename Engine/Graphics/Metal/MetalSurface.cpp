#include "MetalSurface.h"

#include <iostream>
#include <CoreGraphics/CoreGraphics.h>
#include <objc/objc.h>
#include <objc/message.h>
#include <OSAPI/MAC/AppKit/NSView.hpp>
#include "MetalCore.h"
#include "Engine/Common/CoreLogStream.h"

// C entry point into PassThroughMTKView.mm. The editor's embedded
// (create_external) path uses this to allocate a PassThroughMTKView
// subclass instance — the subclass overrides -hitTest: to return nil
// so the view passes pointer events through to the Avalonia controls
// underneath. The subclass is layout-compatible with the C++ MTK::View
// wrapper (the wrapper is a pure facade with no data members — see
// OSAPI/MAC/Foundation/NSObject.hpp), so reinterpret_cast<MTK::View*>
// is sound and the existing 2 call sites (MetalCore.cpp:47,
// MetalPostProcess.cpp:195) that use `static_cast<MTK::View*>(
// surface->view())` continue to work without any changes.
extern "C" void* PassThroughMTKView_Create(void* device, double x, double y, double w, double h);

namespace primal::graphics::metal
{
    namespace
    {

    } // anonymous namespace

bool metal_surface::create()
    {
        CGRect frame = (CGRect){ { 0.f, 0.f }, { static_cast<CGFloat>(_window.width()), static_cast<CGFloat>(_window.height()) } };
        _mtk_view = MTK::View::alloc()->init(
            frame,
            core::get_device()
        );

        if(!_mtk_view)
        {
            PRIMAL_LOG_STREAM(Error, "RHI") << "Failed to create MTKView" << std::endl;
            return false;
        }

        _mtk_view->setColorPixelFormat( MTL::PixelFormat::PixelFormatRGBA16Float );
        _mtk_view->setClearColor( MTL::ClearColor::Make( 1.0, 0.0, 0.0, 1.0 ) );
        _mtk_view->setPaused( true );
        _mtk_view->setEnableSetNeedsDisplay( true );

        // 设置颜色空间以确保正确的颜色显示和调试一致性
        // 对于HDR内容使用扩展sRGB颜色空间，确保Xcode调试器和实际窗口显示一致
        // _mtk_view->setColorSpace(CGColorSpaceCreateWithName(kCGColorSpaceLinearSRGB));

        // _view_delegate = new metal_surface_delegate();
        // _mtk_view->setDelegate( _view_delegate );

        NS::Window* window = static_cast<NS::Window*>(_window.handle());
        if (!window) {
            PRIMAL_LOG_STREAM(Error, "RHI") << "[metal_surface::create] window handle is null, skipping contentView attach" << std::endl;
            return false;
        }
        window->setContentView( _mtk_view );
        window->makeKeyAndOrderFront( nullptr );

        return true;
    }

    void metal_surface::release()
    {
        if (_mtk_view)
        {
            // For externally-attached MTKViews (create_external) the
            // engine owns the MTKView lifetime but the host NSView owns
            // its position in the view hierarchy. Detach it explicitly
            // before release() — otherwise the host keeps a pointer to a
            // freed ObjC object, leaving a phantom view on screen (and
            // risking a crash on the next layout pass when AppKit
            // dispatches setFrame:display: to a freed target).
            if (_is_external)
            {
                // NS::Object::sendMessage is protected, so we drop down
                // to the public objc_msgSend C API. removeFromSuperview
                // takes no arguments and returns void. The variadic-cast
                // pattern is the Apple-recommended way to call
                // objc_msgSend from C++. We spell out `objc_object*`
                // rather than `id` because primal::id is a namespace in
                // Engine/Common/Id.h and shadows the Objective-C `id`
                // typedef in this translation unit.
                reinterpret_cast<void (*)(struct objc_object*, SEL)>(objc_msgSend)(
                    reinterpret_cast<struct objc_object*>(_mtk_view),
                    sel_registerName("removeFromSuperview"));
            }
            _mtk_view->release();
        }
        _external_view = nullptr;
    }

    bool metal_surface::create_external()
    {
        // The "hwnd" of an externally-registered window is actually the
        // host NSView (set by platform::register_external_window). We use it
        // as the parent for the MTKView so the engine's forward pipeline
        // renders into the Avalonia-owned contentView.
        _host_view = reinterpret_cast<NS::View*>(_window.handle());
        if (!_host_view) {
            PRIMAL_LOG_STREAM(Error, "RHI") << "[metal_surface::create_external] host view is null" << std::endl;
            return false;
        }
        _is_external = true;

        // Validate the host view has non-zero bounds BEFORE allocating the
        // MTKView. The editor's MTKViewHost is a NativeControlHost whose
        // child NSView is laid out by Avalonia; if TryInitializeFromLayout
        // is called before the host's child has been laid out
        // (parent.Bounds = 0,0), the host's NSView.bounds is {{0,0},{0,0}}.
        // Allocating a MTKView with frame {0,0,0,0} produces a CAMetalLayer
        // whose drawableSize is 0,0; the first call to currentDrawable then
        // returns either nil or a drawable whose backing texture is invalid.
        // Either way, the first presentDrawable: block fires inside the
        // GPU completion queue with a freed / invalid drawable and we
        // crash with the EXC_BAD_ACCESS at
        // -[_MTLCommandBuffer presentDrawable:options:]_block_invoke
        // signature. Bail out cleanly so the editor can retry the next
        // layout pass when the host is actually laid out.
        const CGRect hostBounds = _host_view->bounds();
        if (hostBounds.size.width <= 0.0 || hostBounds.size.height <= 0.0) {
            PRIMAL_LOG_STREAM(Error, "RHI") << "[metal_surface::create_external] host view has zero-size "
                         "bounds (" << hostBounds.size.width << "x"
                      << hostBounds.size.height << "); deferring until layout pass" << std::endl;
            _is_external = false;
            _host_view = nullptr;
            return false;
        }

        const CGFloat width  = static_cast<CGFloat>(_window.width());
        const CGFloat height = static_cast<CGFloat>(_window.height());

        // Allocate a PassThroughMTKView (subclass of MTKView) so the
        // view's -hitTest: returns nil and pointer events fall through
        // to the Avalonia controls underneath.
        void* raw_view = PassThroughMTKView_Create(
            core::get_device(), 0.0, 0.0,
            static_cast<double>(width), static_cast<double>(height));
        if (!raw_view) {
            PRIMAL_LOG_STREAM(Error, "RHI") << "[metal_surface::create_external] PassThroughMTKView alloc failed" << std::endl;
            return false;
        }
        _mtk_view = reinterpret_cast<MTK::View*>(raw_view);

        // Match the legacy path's MTKView settings so the surface is
        // driven manually (RenderFrame calls pView->draw() in end_frame).
        _mtk_view->setColorPixelFormat(MTL::PixelFormat::PixelFormatBGRA8Unorm);
        _mtk_view->setDepthStencilPixelFormat(MTL::PixelFormat::PixelFormatDepth32Float_Stencil8);
        _mtk_view->setClearColor(MTL::ClearColor::Make(0.10f, 0.10f, 0.10f, 1.0f));
        _mtk_view->setPaused(true);
        _mtk_view->setEnableSetNeedsDisplay(true);

        // The editor explicitly sets the MTKView's frame via
        // set_external_frame to align with its UI control's bounds, so
        // we leave autoresizingMask at 0 (no auto-resize).
        _mtk_view->setAutoresizingMask(0u);

        // autoresizingMask = ViewMinYMargin (2) | ViewMaxYMargin (16) keeps
        // the MTKView stretched vertically while letting us set its frame
        // explicitly. Setting it to 0 triggers AppKit to dispatch
        // setFrame:display: on the MTKView, which is not a recognized
        // selector on the runtime subclass and crashes the process.
        _mtk_view->setAutoresizingMask(2u | 16u);

        _host_view->addSubview(_mtk_view);

        _mtk_view->setNeedsDisplay(true);
        return true;
    }

    void metal_surface::set_external_frame(s32 x, s32 y, u32 w, u32 h)
    {
        if (!_is_external || !_mtk_view || !_host_view) return;

        // Convert client (top-left origin) coordinates to NSView
        // (bottom-left origin) coordinates. The host's bounds height
        // comes from the host view itself.
        const CGRect hostBounds = _host_view->bounds();
        const CGFloat ny = hostBounds.size.height - static_cast<CGFloat>(y) - static_cast<CGFloat>(h);

        CGRect frame{ { static_cast<CGFloat>(x), ny },
                     { static_cast<CGFloat>(w), static_cast<CGFloat>(h) } };
        _mtk_view->setFrame(frame);
        _mtk_view->setDrawableSize(CGSizeMake(static_cast<CGFloat>(w), static_cast<CGFloat>(h)));
    }

    void metal_surface::resize(u32 w, u32 h)
    {
        if (w == 0 || h == 0) return;

        // Update the platform window's stored size so metal_surface::width()
        // /height() and downstream consumers (rendering passes, frame_info)
        // see the new dimensions. resize_window is a no-op when w/h match
        // the platform's stored value, so this is cheap on a no-op resize.
        _window.resize(w, h);

        if (!_mtk_view) return;

        // MTKView's frame and drawableSize are independent: frame is the
        // on-screen position+size in points, drawableSize is the backing
        // texture resolution in pixels (multiplied by the screen's scale
        // factor if you go through CAMetalLayer directly, but MTKView
        // handles the scaling for us when we set the same w/h as the
        // frame). Setting only one leaves the GPU drawing into a stale-
        // sized CAMetalDrawable, which is the root cause of the
        // post-resize black surface.
        _mtk_view->setFrame(CGRect{
            { 0.f, 0.f },
            { static_cast<CGFloat>(w), static_cast<CGFloat>(h) }
        });
        _mtk_view->setDrawableSize(CGSizeMake(
            static_cast<CGFloat>(w), static_cast<CGFloat>(h)));
    }
}
