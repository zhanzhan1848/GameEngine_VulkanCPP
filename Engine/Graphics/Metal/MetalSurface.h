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

        bool create();
        void release();
        u32 width() const { return _window.width(); }
        u32 height() const { return _window.height(); }
        [[nodiscard]] constexpr MTK::View* view() const { return _mtk_view; }
    private:
        MTK::View* _mtk_view{  };
        platform::window _window{  };
    };
}