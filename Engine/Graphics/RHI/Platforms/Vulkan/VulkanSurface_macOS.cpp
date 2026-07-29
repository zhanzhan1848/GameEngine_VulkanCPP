/**
 * @file VulkanSurface_macOS.cpp
 * @brief macOS VkSurfaceKHR 创建 — 通过 vkCreateMetalSurfaceEXT + CAMetalLayer
 * @details 使用 objc_msgSend 而非 .mm,避免 CMake glob 改动。
 *          window 句柄约定为 NSWindow*(与 MetalSwapChain 一致)。
 */

#include "VulkanSurface.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN && defined(__APPLE__)

#include <objc/message.h>
#include <objc/runtime.h>
#include <cstdio>

namespace primal::graphics::rhi {

namespace {

// objc_msgSend 强制 cast helper(每签名一个,符合 ABI 规范)
using IdSend      = ::id (*)(::id, ::SEL);
using IdSendStr   = ::id (*)(::id, ::SEL);
using VoidSend    = void (*)(::id, ::SEL);
using VoidSendBool = void (*)(::id, ::SEL, BOOL);
using IdSendId    = ::id (*)(::id, ::SEL, ::id);

::id GetContentView(::id window) {
    auto msg = reinterpret_cast<IdSend>(objc_msgSend);
    ::SEL sel = sel_registerName("contentView");
    return msg(window, sel);
}

::id GetLayer(::id view) {
    auto msg = reinterpret_cast<IdSend>(objc_msgSend);
    ::SEL sel = sel_registerName("layer");
    return msg(view, sel);
}

void SetWantsLayer(::id view, BOOL b) {
    auto msg = reinterpret_cast<VoidSendBool>(objc_msgSend);
    ::SEL sel = sel_registerName("setWantsLayer:");
    msg(view, sel, b);
}

void SetLayer(::id view, ::id layer) {
    auto msg = reinterpret_cast<IdSendId>(objc_msgSend);  // 也作 void 返回,但 cast 兼容
    ::SEL sel = sel_registerName("setLayer:");
    msg(view, sel, layer);
}

::id CreateCAMetalLayer() {
    auto msg = reinterpret_cast<IdSend>(objc_msgSend);
    ::Class cls = objc_getClass("CAMetalLayer");
    if (!cls) return nil;
    ::SEL sel = sel_registerName("layer");
    return msg(reinterpret_cast<::id>(cls), sel);  // [CAMetalLayer layer]
}

bool IsCAMetalLayer(::id layer) {
    if (!layer) return false;
    ::Class metalCls = objc_getClass("CAMetalLayer");
    if (!metalCls) return false;
    // Walk the class chain; CAMetalLayer must be in the hierarchy.
    for (::Class cls = object_getClass(layer); cls != nil; cls = class_getSuperclass(cls)) {
        if (cls == metalCls) return true;
    }
    return false;
}

} // anonymous namespace

bool CreateVulkanSurface(VkInstance instance, void* window, VkSurfaceKHR* outSurface) {
    if (!instance || !window || !outSurface) return false;
    *outSurface = VK_NULL_HANDLE;

    ::id windowId = reinterpret_cast<::id>(window);
    ::id contentView = GetContentView(windowId);
    if (!contentView) {
        // 也许 caller 直接传了 NSView 而非 NSWindow
        contentView = windowId;
    }

    // 确保 view 有 layer-backed(若没有则创建 CAMetalLayer)
    SetWantsLayer(contentView, YES);
    ::id layer = GetLayer(contentView);
    if (!layer || !IsCAMetalLayer(layer)) {
        layer = CreateCAMetalLayer();
        if (!layer) return false;
        SetLayer(contentView, layer);
    }

    // vkCreateMetalSurfaceEXT (来自 VK_EXT_metal_surface,instance 已在 VulkanDevice 启用)
    auto vkCreateMetalSurfaceEXT = reinterpret_cast<PFN_vkCreateMetalSurfaceEXT>(
        vkGetInstanceProcAddr(instance, "vkCreateMetalSurfaceEXT"));
    if (!vkCreateMetalSurfaceEXT) {
        std::fprintf(stderr, "[VulkanSurface_macOS] vkCreateMetalSurfaceEXT not loaded\n");
        return false;
    }

    VkMetalSurfaceCreateInfoEXT ci{};
    ci.sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT;
    ci.pNext = nullptr;
    ci.flags = 0;
    ci.pLayer = const_cast<CAMetalLayer*>(  reinterpret_cast<CAMetalLayer*>(layer));
    VkResult res = vkCreateMetalSurfaceEXT(instance, &ci, nullptr, outSurface);
    if (res != VK_SUCCESS) {
        std::fprintf(stderr, "[VulkanSurface_macOS] vkCreateMetalSurfaceEXT failed: %d\n", res);
        return false;
    }
    return true;
}

} // namespace primal::graphics::rhi

#endif // ENABLE_VULKAN && __APPLE__
