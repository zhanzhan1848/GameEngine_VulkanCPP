// Copyright (c) Contributors of Primal+
// Distributed under the MIT license. See the LICENSE file in the project root for more information.
#include "GraphicsPlatformInterface.h"
#include "Direct3D12/D3D12Interface.h"
#include "Vulkan/VulkanInterface.h"
#include "Metal/MetalInterface.h"

// === Phase 1 Sub-step 1.2.6': 本文件是 platform_interface 旧实现 ===
// 整个 GraphicsPlatform.cpp 存在的唯一目的就是给 platform_interface 选后端填充器，
// 函数签名和函数体都会触发 platform_interface 结构体的 deprecated 警告。
// 抑制整个 TU 的 -Wdeprecated-declarations，让旧路径继续编译，新代码碰这个头才会警告。
#ifdef __clang__
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

namespace primal::graphics {
#include "GraphicsPlatform.h"

	[[deprecated(
		"set_platform_interface is deprecated. The RHI device path doesn't use "
		"a function-pointer table; platform selection happens in CreateRHIDevice "
		"(RHIDeviceFactory) driven by DeviceDesc.platform. See Phase 1 Sub-step 1.2.6'."
	)]]
	bool
	set_platform_interface(graphics_platform platform, platform_interface& pi)
	{
    switch (platform)
    {
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
    default:
        return false;
    }

    assert(pi.platform == platform);
    return true;
}
}