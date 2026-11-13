#pragma once

#include "CommonHeaders.h"
#include "Graphics/Renderer.h"
#include "Platform/Window.h"

// R13-B（Windows 十二测 C1083）：本头此前无条件 include <Metal/Metal.hpp>——
// Metal 框架仅 Apple 存在，任何非 Apple TU 经 include 链到达本头即 C1083
// （实测进入链：EngineDLL/ResourceAPI.cpp 无条件 include MetalContent.h）。
// 本头全部载荷（框架头/帧缓冲常量/调试宏/MetalHelper/MetalResource）都是
// Metal 后端专属，整段 APPLE 门控；非 Apple 平台本头退化为引擎通用头转发，
// 零 Metal 引用。消费者清单（全部 Metal 后端文件，仅 APPLE 编译域经
// Engine/CMakeLists.txt glob）见 runbook §22B。
#if defined(__APPLE__)

#include <Metal/Metal.hpp>
#include <MetalKit/MetalKit.hpp>
#include <QuartzCore/QuartzCore.hpp>


namespace primal::graphics::metal
{
    constexpr u32 frame_buffer_count{ 2 };
}

#if __APPLE__
#ifdef _DEBUG
#include <iostream>
// Debug 信息宏
#define NAME_METAL_OBJECT(obj, name) \
    if (obj) { \
        std::cout << "::Metal Object Created: " << name << std::endl; \
    }

#define NAME_METAL_OBJECT_INDEXED(obj, n, name) \
{ \
    if (obj) { \
        std::string fullName = std::string(name) + "[" + std::to_string((u64)n) + "]"; \
        std::cout << "::Metal Object Created: " << fullName << std::endl; \
    } \
}

// 错误处理宏
#define MTL_CHECK_ERROR(error) \
    if (error) { \
        __builtin_printf("%s", error->localizedDescription()->utf8String()); \
        assert(false); \
    }
// 用于函数调用的错误处理宏
#define MTL_CALL_WITH_ERROR(call, error_ptr) \
    { \
        auto result = call; \
        if (!result) { \
            if (error_ptr) { \
                __builtin_printf("%s", error_ptr->localizedDescription()->utf8String()); \
            } \
            assert(false); \
        } \
    }

#define MTLCALL(x) x;
#else
#define NAME_METAL_OBJECT(obj, name)
#define NAME_METAL_OBJECT_INDEXED(obj, n, name)
#define MTL_CHECK_ERROR(error)
#define MTL_CALL_WITH_ERROR(call, error_ptr) call;
#define MTLCALL(x) x;
#endif // _DEBUG
#endif // __APPLE__

#include "MetalHelper.h"
#include "MetalResource.h"

#endif // defined(__APPLE__) — R13-B Metal 载荷门控结束