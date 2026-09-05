#pragma once

// Minimal AppKit window for PrimalPlayer (plan §Run/Stop Phase 1).
// The engine surface attaches to the returned NSView via
// CreateRenderSurface(host, w, h); the event loop pumps manually so the
// render tick stays on the main thread (same constraint the editor's
// DispatcherTimer render loop documents).

#ifdef __cplusplus
extern "C" {
#endif

// Creates the application, a titled window and its content view.
// Returns the content NSView pointer (for CreateRenderSurface's host),
// or null on failure. w/h are the initial content size. resizable=false
// 去掉窗口的 Resizable 样式（--fixed：固定分辨率录制/比对要求显示
// 分辨率与渲染分辨率严格一致）。
void* PlayerWindowCreate(int width, int height, const char* title, int resizable);

// Pumps NSApp events. Returns 0 once the user asked to close (close box),
// non-zero while the window should stay up. (Esc handling 相位 1 暂缺。)
int PlayerWindowPump(void);

// Tears the window down. Call before ShutdownEngine.
void PlayerWindowDestroy(void);

#ifdef __cplusplus
}
#endif
