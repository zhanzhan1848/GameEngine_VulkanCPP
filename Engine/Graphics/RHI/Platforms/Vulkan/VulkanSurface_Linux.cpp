/**
 * @file VulkanSurface_Linux.cpp
 * @brief Linux VkSurfaceKHR 创建 — 通过 vkCreateXcbSurfaceKHR
 * @details window 句柄约定为 X11 Window*(platform::window_handle 在 Linux 是 Window*)。
 */

#include "VulkanSurface.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN && defined(__linux__) && !defined(__APPLE__)

#include <xcb/xcb.h>
#include <iostream>

namespace primal::graphics::rhi {

bool CreateVulkanSurface(VkInstance instance, void* window, VkSurfaceKHR* outSurface) {
    if (!instance || !window || !outSurface) return false;
    *outSurface = VK_NULL_HANDLE;

    // platform::window_handle 在 Linux 是 Window*(X11 xcb_window_t 的指针包装)
    auto* xcbWindow = reinterpret_cast<xcb_window_t*>(window);
    if (!xcbWindow) return false;

    auto vkCreateXcbSurfaceKHR = reinterpret_cast<PFN_vkCreateXcbSurfaceKHR>(
        vkGetInstanceProcAddr(instance, "vkCreateXcbSurfaceKHR"));
    if (!vkCreateXcbSurfaceKHR) {
        std::cerr << "[VulkanSurface_Linux] vkCreateXcbSurfaceKHR not loaded" << std::endl;
        return false;
    }

    VkXcbSurfaceCreateInfoKHR ci{};
    ci.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR;
    ci.pNext = nullptr;
    ci.flags = 0;
    ci.connection = nullptr;  // TODO: caller 提供
    ci.window = *xcbWindow;

    VkResult res = vkCreateXcbSurfaceKHR(instance, &ci, nullptr, outSurface);
    if (res != VK_SUCCESS) {
        std::cerr << "[VulkanSurface_Linux] vkCreateXcbSurfaceKHR failed: " << res << std::endl;
        return false;
    }
    return true;
}

} // namespace primal::graphics::rhi

#endif // ENABLE_VULKAN && __linux__
