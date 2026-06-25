#pragma once
#include "MetalCommonHeaders.h"

namespace primal::graphics::metal
{

    class metal_surface
    {
    public:
        explicit metal_surface(platform::window window) : _window{ window }
        {
            assert(window.handle());
        }
        DISABLE_COPY_AND_MOVE(metal_surface);
        ~metal_surface() { release(); }

        // Default: creates MTKView and attaches it to the platform::window's NSWindow.
        bool create();
        // External-host mode: creates MTKView only, leaves it detached.
        // Caller is responsible for adding the returned view() to its own NSView tree.
        bool create_external();
        // Repositions the embedded MTKView within its host view. Used by
        // editors that need to keep the render surface aligned with a
        // specific UI control's bounds.
        void set_external_frame(s32 x, s32 y, u32 w, u32 h);
        void release();
        u32 width() const { return _window.width(); }
        u32 height() const { return _window.height(); }
        // Returns the raw Objective-C view pointer (id). May be either a
        // base MTKView (for create()) or a PassThroughMTKView subclass (for
        // create_external()) — both are MTKView instances.
        [[nodiscard]] constexpr void* view() const { return _mtk_view; }
    private:
        // For the standalone case (create()) we use the C++ MTK::View
        // wrapper. For the embedded case (create_external()) we use a
        // PassThroughMTKView subclass that returns nil from hitTest: so the
        // Avalonia controls underneath receive pointer events. We use a
        // void* for the external path so we don't have to template the
        // wrapper on the subclass.
        MTK::View* _mtk_view{  };
        void* _external_view{ nullptr };  // PassThroughMTKView* (id)
        platform::window _window{  };
        NS::View* _host_view{ nullptr };
        bool _is_external{ false };
    };
}
