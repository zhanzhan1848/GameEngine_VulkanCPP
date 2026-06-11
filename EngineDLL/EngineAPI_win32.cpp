#if defined(_MSC_VER)

#include "Common.h"
#include <Windows.h>

#pragma comment(lib, "Engine.lib")

namespace {
    HWND g_childWindow = nullptr;
}

EDITOR_INTERFACE void* CreateEmbeddedRenderView(void* parent_hwnd, s32 width, s32 height)
{
    HWND parent = (HWND)parent_hwnd;
    if (!parent) return nullptr;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PrimalEditorRenderChild";
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassExW(&wc);

    RECT rc;
    GetClientRect(parent, &rc);

    g_childWindow = CreateWindowExW(
        0, L"PrimalEditorRenderChild", L"Render",
        WS_CHILD | WS_VISIBLE,
        0, 0, rc.right, rc.bottom,
        parent, nullptr, GetModuleHandleW(nullptr), nullptr);

    return (void*)g_childWindow;
}

EDITOR_INTERFACE void DestroyEmbeddedRenderView(void* view_handle)
{
    HWND hwnd = (HWND)view_handle;
    if (hwnd && IsWindow(hwnd)) {
        DestroyWindow(hwnd);
    }
    g_childWindow = nullptr;
}

EDITOR_INTERFACE void ResizeEmbeddedRenderView(void* view_handle, s32 width, s32 height)
{
    HWND hwnd = (HWND)view_handle;
    if (hwnd && IsWindow(hwnd)) {
        SetWindowPos(hwnd, nullptr, 0, 0, width, height, SWP_NOZORDER | SWP_NOMOVE);
    }
}

#endif
