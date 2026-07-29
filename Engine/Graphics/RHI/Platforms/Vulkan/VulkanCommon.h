#pragma once

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

#include <vulkan/vulkan.h>
#if defined(__APPLE__)
// MoltenVK 表面扩展(Linux/Win32 用 VK_KHR_xcb/win32_surface,已在 vulkan.h 内)
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
