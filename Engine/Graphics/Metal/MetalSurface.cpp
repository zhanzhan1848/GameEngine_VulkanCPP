#include "MetalSurface.h"

#include <iostream>
#include <CoreGraphics/CoreGraphics.h>
#include "MetalCore.h"

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
            std::cerr << "Failed to create MTKView" << std::endl;
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

        static_cast<NS::Window*>(_window.handle())->setContentView( _mtk_view );
        static_cast<NS::Window*>(_window.handle())->makeKeyAndOrderFront( nullptr );

        return true;
    }

    void metal_surface::release()
    {
        if (_mtk_view) _mtk_view->release();
    }
}