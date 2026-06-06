#pragma once

#include "Engine/Common/CommonHeaders.h"

#ifdef __APPLE__

// 仅在 .cpp 文件中开启实现宏，头文件只声明类型
#if defined(RHI_METAL_IMPLEMENTATION)
    #define NS_PRIVATE_IMPLEMENTATION
    #define MTL_PRIVATE_IMPLEMENTATION
    #define MTK_PRIVATE_IMPLEMENTATION
    #define CA_PRIVATE_IMPLEMENTATION
#endif

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <MetalKit/MetalKit.hpp>
#include <QuartzCore/QuartzCore.hpp>

#endif // __APPLE__
