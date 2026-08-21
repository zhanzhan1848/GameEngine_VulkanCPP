// Copyright (c) Contributors of Primal+
// Distributed under the MIT license. See the LICENSE file in the project root for more information.
#pragma once
#include "CommonHeaders.h"

#ifdef _WIN64
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace primal::platform {

using window_proc = LRESULT(*)(HWND, UINT, WPARAM, LPARAM);
using window_handle = HWND;

struct window_init_info
{
    window_proc     callback{ nullptr };
    window_handle   parent{ nullptr };
    const wchar_t*  caption{ nullptr };
    s32             left{ 0 };
    s32             top{ 0 };
    s32             width{ 1920 };
    s32             height{ 1080 };
};
}
#endif // _WIN64

#ifdef __EMSCRIPTEN__

namespace primal::platform {

using window_proc = void*;
using window_handle = void*;

struct window_init_info
{
    window_proc     callback{ nullptr };
    window_handle   parent{ nullptr };
    const char*     caption{ nullptr };
    s32             left{ 0 };
    s32             top{ 0 };
    s32             width{ 1280 };
    s32             height{ 720 };
};
}
#elif defined(__linux__)
#include <X11/Xlib.h>
#include <stdlib.h>
// X11 的 X.h 把 None/Always/Bool/True/False/Status 定义为宏（如 None=0L），
// 与 RHI 枚举成员名（QueryResultFlags::None、ComparisonFunc::Always 等）冲突
// （'expected identifier before numeric constant'）。包含后立即撤销。
#undef None
#undef Always
#undef Bool
#undef True
#undef False
#undef Status

namespace primal::platform {

using window_handle = Window*;

struct window_init_info
{
    void*			callback{ nullptr };
    window_handle	parent{ nullptr };
    const wchar_t*	caption{ nullptr };
    s32				left{ 0 };
    s32				top{ 0 };
    s32				width{ 1920 };
    s32				height{ 1080 };
};
}
#endif // __linux__

#ifdef __APPLE__

namespace primal::platform {

using window_proc = void*;
using window_handle = void*;

struct window_init_info
{
    [[maybe_unused]] window_proc     callback{ nullptr };
    [[maybe_unused]] window_handle   parent{ nullptr };
    const char*     caption{ nullptr };    // macOS 使用 UTF-8 字符串
    s32             left{ 0 };
    s32             top{ 0 };
    s32             width{ 1920 };
    s32             height{ 1080 };
};
}
#endif // __APPLE__