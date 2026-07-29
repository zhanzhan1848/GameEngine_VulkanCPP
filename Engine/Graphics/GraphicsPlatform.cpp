// Copyright (c) Contributors of Primal+
// Distributed under the MIT license. See the LICENSE file in the project root for more information.
#include "GraphicsPlatformInterface.h"
#include "Direct3D12/D3D12Interface.h"
#include "Vulkan/VulkanInterface.h"
#include "Metal/MetalInterface.h"

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
    // D3D12 / legacy-Vulkan 只在 Win32+Linux 编入(见 CMake glob),Apple 上没 dispatch。
    // Phase 4b: 新 RHI Vulkan 走 self-contained 路径,不经 legacy platform_interface,
    // 所以 macOS 上 InitializeEngine(Vulkan) 会跳过 graphics::initialize(legacyPlatform) 整步。
#ifndef __APPLE__
    case graphics_platform::direct3d12:
        d3d12::get_platform_interface(pi);
        break;
    case graphics_platform::vulkan_1:
        vulkan::get_platform_interface(pi);
        break;
#endif
    case graphics_platform::metal:
        metal::get_platform_interface(pi);
        break;
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