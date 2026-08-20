/**
 * @file VulkanSurface.h
 * @brief 平台无关的 VkSurfaceKHR 创建接口
 * @details 平台特定实现(_macOS.cpp / _Linux.cpp / _Win32.cpp)在各自 .cpp 里。
 *          macOS 通过 objc_msgSend 提取 CAMetalLayer(避免 .mm CMake glob 改动)。
 * @author GameEngine VulkanCPP Team
 * @date 2026-07-27
 */

#pragma once

#include "VulkanCommon.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

namespace primal::graphics::rhi {

/// 创建平台 surface。window 是 platform::window_handle(macOS=NSWindow*/NSView*,
/// Linux=X11 Window*,Windows=HWND)。
/// 成功返回 true 并写入 *outSurface;失败返回 false。
bool CreateVulkanSurface(VkInstance instance, void* window, VkSurfaceKHR* outSurface);

} // namespace primal::graphics::rhi

#endif // ENABLE_VULKAN
