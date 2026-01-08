#ifdef __APPLE__
#include "Platform.h"
#include "PlatformTypes.h"
#define NS_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#include <OSAPI/MAC/AppKit/AppKit.hpp>
#include <CoreGraphics/CGGeometry.h>

namespace primal::platform
{
	namespace 
	{
		
		struct window_info
		{
			void*		    hwnd{ nullptr };
			CGRect	        client_area{ { 0, 0 }, { 1920, 1080 } };
			CGRect			fullscreen_area{};
			CGPoint		    top_left{ 0, 0 };
			bool			is_fullscreen{ false };
			bool			is_closed{ false };
		};

		utl::free_list<window_info> windows;

		window_info& get_from_id(window_id id)
		{
			assert(windows[id].hwnd);
			return windows[id];
		}

		bool resized{ false };

        void resize_window(window_id id, u32 width, u32 height)
        {
            window_info& info{ get_from_id(id) };
            NS::Window* window = static_cast<NS::Window*>(info.hwnd);

            CGRect frame{ { info.client_area.origin.x, info.client_area.origin.y }, 
            { static_cast<CGFloat>(width), static_cast<CGFloat>(height) } };

            window->setFrame(frame, true);

            info.client_area.size.width = static_cast<CGFloat>(width);
            info.client_area.size.height = static_cast<CGFloat>(height);
        }

		void set_window_fullscreen(window_id id, bool is_fullscreen)
		{
			window_info& info{ get_from_id(id) };
			if (info.is_fullscreen != is_fullscreen)
			{
				info.is_fullscreen = is_fullscreen;
                NS::Window* window = static_cast<NS::Window*>(info.hwnd);
				if (is_fullscreen)
				{
					CGRect rect{ window->frame() };
					info.top_left.x = rect.origin.x;
					info.top_left.y = rect.origin.y;
				}
				else
				{
					window->toggleFullScreen(nullptr);
				}
			}
		}

		bool is_window_fullscreen(window_id id)
		{
			return get_from_id(id).is_fullscreen;
		}

		window_handle get_window_handle(window_id id)
		{
			return get_from_id(id).hwnd;
		}

		void set_window_caption(window_id id, const char* caption)
		{
			window_info& info{ get_from_id(id) };
            NS::Window* window = static_cast<NS::Window*>(info.hwnd);
			window->setTitle(caption);
		}

		math::u32v4 get_window_size(window_id id)
		{
			window_info& info{ get_from_id(id) };

			return { (u32)info.client_area.origin.x, (u32)info.client_area.origin.x, (u32)(info.client_area.size.width + info.client_area.origin.x), (u32)(info.client_area.size.height + info.client_area.origin.y) };
		}

		bool is_window_closed(window_id id)
		{
			return get_from_id(id).is_closed;
		}
	}// anonymous namespace

	window create_window(const window_init_info* init_info /* = nullptr */)
	{
		window_proc callback{ init_info ? init_info->callback : nullptr };
		window_handle parent{ init_info ? init_info->parent : nullptr };

		window_info info{};
		info.client_area = { { static_cast<CGFloat>(init_info->left), static_cast<CGFloat>(init_info->top) }, { static_cast<CGFloat>(init_info->width), static_cast<CGFloat>(init_info->height) } };
		info.top_left = { static_cast<CGFloat>(init_info->left), static_cast<CGFloat>(init_info->top) };

		// Create an instance of the window class
		info.hwnd = NS::Window::alloc()->init(
			info.client_area,
			NS::WindowStyleMaskClosable|NS::WindowStyleMaskTitled,
			NS::BackingStoreBuffered,
			false
		);

		if (info.hwnd)
		{
			NS::Window* nsWindow = static_cast<NS::Window*>(info.hwnd);
			// if (callback) SetWindowLongPtr(info.hwnd, 0, (LONG_PTR)callback);
			nsWindow->setTitle(init_info->caption);
			nsWindow->makeKeyAndOrderFront(nullptr);

			window_id id{ (id::id_type)windows.add(info) };

			return window{ id };
		}

		return {};
	}

	void remove_window(window_id id)
	{
		window_info& info{ get_from_id(id) };
		info.hwnd = nullptr;
		windows.remove(id);
	}
}

#include "IncludeWindowCPP.h"
#endif // MacOS