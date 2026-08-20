/**
 * @file VulkanSurface_Win32.cpp
 * @brief Windows VkSurfaceKHR 创建 — 通过 vkCreateWin32SurfaceKHR
 * @details window 句柄约定为 HWND(platform::window_handle 在 Windows 是 HWND)。
 */

#include "VulkanSurface.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN && defined(_WIN32)

#define VK_USE_PLATFORM_WIN32_KHR 1
#include <windows.h>
#include <vulkan/vulkan_win32.h>
#include <iostream>

namespace primal::graphics::rhi {

bool CreateVulkanSurface(VkInstance instance, void* window, VkSurfaceKHR* outSurface) {
    if (!instance || !window || !outSurface) return false;
    *outSurface = VK_NULL_HANDLE;

    HWND hwnd = reinterpret_cast<HWND>(window);
    if (!hwnd) return false;

    auto vkCreateWin32SurfaceKHR = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(
        vkGetInstanceProcAddr(instance, "vkCreateWin32SurfaceKHR"));
    if (!vkCreateWin32SurfaceKHR) {
        std::cerr << "[VulkanSurface_Win32] vkCreateWin32SurfaceKHR not loaded" << std::endl;
        return false;
    }

    VkWin32SurfaceCreateInfoKHR ci{};
    ci.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    ci.pNext = nullptr;
    ci.flags = 0;
    ci.hinstance = GetModuleHandle(nullptr);
    ci.hwnd = hwnd;

    VkResult res = vkCreateWin32SurfaceKHR(instance, &ci, nullptr, outSurface);
    if (res != VK_SUCCESS) {
        std::cerr << "[VulkanSurface_Win32] vkCreateWin32SurfaceKHR failed: " << res << std::endl;
        return false;
    }
    return true;
}

} // namespace primal::graphics::rhi

#endif // ENABLE_VULKAN && _WIN32
