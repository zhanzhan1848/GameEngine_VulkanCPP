#pragma once
#include "Window.h"
#include "PlatformTypes.h"

namespace primal::platform
{
	struct window_init_info;

	window create_window(const window_init_info* const init_info = nullptr);
	void remove_window(window_id id);
#ifdef __APPLE__
	window register_external_window(window_handle handle, u32 width, u32 height);
#endif
}