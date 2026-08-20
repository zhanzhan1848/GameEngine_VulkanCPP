#pragma once

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

// 平台 surface 扩展的宏/原型需要先定义 VK_USE_PLATFORM_* 再包含对应头，
// 否则 VK_KHR_WIN32_SURFACE_EXTENSION_NAME 等宏不会被声明（MSVC C2065）
#if defined(_WIN32)
#define VK_USE_PLATFORM_WIN32_KHR 1
#include <vulkan/vulkan_win32.h>
#elif defined(__linux__)
#define VK_USE_PLATFORM_XCB_KHR 1
#include <vulkan/vulkan_xcb.h>
#endif

#include <vulkan/vulkan.h>
#if defined(__APPLE__)
// MoltenVK 表面扩展
#include <vulkan/vulkan_metal.h>
#endif

#include "Engine/Common/PrimitiveTypes.h"

// VMA forward declarations — let headers reference VmaAllocator/VmaAllocation
// without pulling in the entire single-header. Implementation TUs that call
// vma* functions must include <vk_mem_alloc.h> themselves.
struct VmaAllocator_T;
struct VmaAllocation_T;
using VmaAllocator = VmaAllocator_T*;
using VmaAllocation = VmaAllocation_T*;

namespace primal::graphics::rhi::vulkan {

/// Convert RHI debug flag to Vulkan DebugUtilsMessengerSeverity.
inline VkDebugUtilsMessageSeverityFlagsEXT ToVkDebugSeverity(bool enableDebug) {
    if (!enableDebug) return 0;
    return VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
           VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
           VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
           VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
}

} // namespace primal::graphics::rhi::vulkan

#endif // ENABLE_VULKAN
