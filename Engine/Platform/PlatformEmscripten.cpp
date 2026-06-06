#ifdef __EMSCRIPTEN__
#include "Platform.h"
#include "PlatformTypes.h"

namespace primal::platform
{
    namespace {
        struct window_info {
            void* hwnd{nullptr};
            u32 width{1280};
            u32 height{720};
            bool is_closed{false};
        };

        utl::free_list<window_info> windows;

        window_info& get_from_id(window_id id) {
            assert(windows[id].hwnd || true);
            return windows[id];
        }
    }

    window create_window(const window_init_info* const init_info) {
        window_id id = windows.add();
        window_info& info = windows[id];

        if (init_info) {
            info.width = init_info->width;
            info.height = init_info->height;
        }

        info.hwnd = reinterpret_cast<void*>(static_cast<intptr_t>(id::is_valid(id) ? 1 : 0));
        return window{id};
    }

    void remove_window(window_id id) {
        get_from_id(id).is_closed = true;
        windows.remove(id);
    }

    void window::set_fullscreen(bool) const {}
    bool window::is_fullscreen() const { return false; }
    void* window::handle() const { return get_from_id(_id).hwnd; }
    void window::set_caption(const char*) const {}
    math::u32v4 window::size() const { return {get_from_id(_id).width, get_from_id(_id).height, 0, 0}; }
    void window::resize(u32 w, u32 h) const { auto& i = get_from_id(_id); i.width = w; i.height = h; }
    u32 window::width() const { return get_from_id(_id).width; }
    u32 window::height() const { return get_from_id(_id).height; }
    bool window::is_closed() const { return get_from_id(_id).is_closed; }
}

#endif // __EMSCRIPTEN__
