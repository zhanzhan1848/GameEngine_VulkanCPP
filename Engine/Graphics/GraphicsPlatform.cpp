// Copyright (c) Contributors of Primal+
// Distributed under the MIT license. See the LICENSE file in the project root for more information.
#include "GraphicsPlatformInterface.h"
// legacy Direct3D12 / Graphics/Vulkan 的实现文件只在 Win32/Linux 且 ENABLE_VULKAN=OFF
// 时编入（见 Engine/CMakeLists.txt 的 GRAPHICS_SOURCES glob：ENABLE_VULKAN=ON 时排除
// 这两个目录）。include 与下方 dispatch 守卫必须同条件，否则 LNK2019。
#if !defined(__APPLE__) && !(defined(ENABLE_VULKAN) && ENABLE_VULKAN)
#include "Direct3D12/D3D12Interface.h"
#include "Vulkan/VulkanInterface.h"
#endif
#if defined(__APPLE__)
#include "Metal/MetalInterface.h"
#endif

#if defined(ENABLE_WEBGPU) && ENABLE_WEBGPU
#include "Dawn/DawnInterface.h"
#endif

namespace primal::graphics {
#include "GraphicsPlatform.h"

bool
set_platform_interface(graphics_platform platform, platform_interface& pi)
{
    switch (platform)
    {
    // D3D12 / legacy-Vulkan 只在 Win32+Linux 且 ENABLE_VULKAN=OFF 时编入（CMake glob
    // 与上方 include 同条件）。Apple 无 dispatch；ENABLE_VULKAN 构建中新 RHI Vulkan 走
    // self-contained 路径,不经 legacy platform_interface——InitializeEngine(Vulkan)
    // 跳过 graphics::initialize(legacyPlatform) 整步（与 macOS ENABLE_VULKAN 行为一致）。
#if !defined(__APPLE__) && !(defined(ENABLE_VULKAN) && ENABLE_VULKAN)
    case graphics_platform::direct3d12:
        d3d12::get_platform_interface(pi);
        break;
    case graphics_platform::vulkan_1:
        vulkan::get_platform_interface(pi);
        break;
#endif
#if defined(__APPLE__)
    case graphics_platform::metal:
        metal::get_platform_interface(pi);
        break;
#endif
#if defined(ENABLE_WEBGPU) && ENABLE_WEBGPU
    case graphics_platform::dawn:
        dawn::get_platform_interface(pi);
        break;
#endif
    default:
        return false;
    }

    assert(pi.platform == platform);
    return true;
}
}