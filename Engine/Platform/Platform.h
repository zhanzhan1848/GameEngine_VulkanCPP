#pragma once
#include "Window.h"

namespace primal::platform
{
	struct window_init_info;

	window create_window(const window_init_info* const init_info = nullptr);
	void remove_window(window_id id);

	// T4.6.5 part 29: called by macOS NSWindowWillCloseNotification observer
	// when any window is closed. Sets a global flag checked by window::is_closed().
	void notify_any_window_closed();
}