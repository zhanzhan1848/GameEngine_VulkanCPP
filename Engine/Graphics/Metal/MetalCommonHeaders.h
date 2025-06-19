#pragma once

#include "CommonHeaders.h"
#include "Graphics/Renderer.h"
#include "Platform/Window.h"


#include <Metal/Metal.hpp>
#include <MetalKit/MetalKit.hpp>


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