/**
 * @file VulkanSwapChain.cpp
 * @brief VulkanSwapChain 实现
 * @details 流程:
 *          Initialize: createSurface → 查询 surface caps → chooseFormat/Mode/Extent
 *                      → vkCreateSwapchainKHR → vkGetSwapchainImagesKHR → wrap 成 VulkanTexture
 *          AcquireNextImage: vkAcquireNextImageKHR(signal 给 caller 提供的 semaphore)
 *          Present: vkQueuePresentKHR(wait 给 caller 提供的 semaphore)
 *          Resize: destroySwapchain → createSwapchain + recreate backbuffers
 * @author GameEngine VulkanCPP Team
 * @date 2026-07-27
 */

#include "VulkanSwapChain.h"
#include "VulkanSurface.h"
#include "VulkanDevice.h"
#include "VulkanSync.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

#include <algorithm>
#include <iostream>
#include <cstring>

namespace primal::graphics::rhi {

VulkanSwapChain::VulkanSwapChain(VulkanDevice& device, const SwapChainDesc& desc)
    : RHISwapChain(device, desc) {
}

VulkanSwapChain::~VulkanSwapChain() {
    Destroy();
}

// ============================================================================
// Initialize / Destroy
// ============================================================================

bool VulkanSwapChain::Initialize() {
    if (!createSurface()) return false;
    if (!createSwapchain()) {
        destroySurface();
        return false;
    }
    if (!createBackBufferTextures()) {
        destroySwapchain();
        destroySurface();
        return false;
    }
    state_ = ResourceState::Ready;
    return true;
}

void VulkanSwapChain::Destroy() {
    destroyBackBufferTextures();
    destroySwapchain();
    destroySurface();
    state_ = ResourceState::Destroyed;
}

// ============================================================================
// Surface (平台分发)
// ============================================================================

bool VulkanSwapChain::createSurface() {
    VulkanDevice& vk = static_cast<VulkanDevice&>(device_);
    return CreateVulkanSurface(vk.GetNativeInstance(), swapChainDesc_.window, &surface_);
}

void VulkanSwapChain::destroySurface() {
    if (surface_ == VK_NULL_HANDLE) return;
    VulkanDevice& vk = static_cast<VulkanDevice&>(device_);
    vkDestroySurfaceKHR(vk.GetNativeInstance(), surface_, nullptr);
    surface_ = VK_NULL_HANDLE;
}

// ============================================================================
// Swapchain 创建
// ============================================================================

VkSurfaceFormatKHR VulkanSwapChain::chooseSurfaceFormat(
    const std::vector<VkSurfaceFormatKHR>& available) const {
    // 偏好 BGRA8_UNorm + sRGB-nonlinear(跨平台兼容性最好)
    for (const auto& f : available) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return f;
        }
    }
    return available.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM,
                                                  VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}
                             : available[0];
}

VkPresentModeKHR VulkanSwapChain::choosePresentMode(
    const std::vector<VkPresentModeKHR>& available) const {
    // 映射 SwapChainDesc::presentMode → VkPresentModeKHR
    VkPresentModeKHR preferred = VK_PRESENT_MODE_FIFO_KHR;  // vsync on 默认
    switch (swapChainDesc_.presentMode) {
    case PresentMode::Immediate:    preferred = VK_PRESENT_MODE_IMMEDIATE_KHR;    break;
    case PresentMode::Mailbox:      preferred = VK_PRESENT_MODE_MAILBOX_KHR;      break;
    case PresentMode::FIFO:         preferred = VK_PRESENT_MODE_FIFO_KHR;         break;
    case PresentMode::FIFO_Relaxed: preferred = VK_PRESENT_MODE_FIFO_RELAXED_KHR; break;
    }
    for (VkPresentModeKHR m : available) {
        if (m == preferred) return m;
    }
    // fallback FIFO(Vulkan spec 保证支持)
    for (VkPresentModeKHR m : available) {
        if (m == VK_PRESENT_MODE_FIFO_KHR) return m;
    }
    return available.empty() ? VK_PRESENT_MODE_FIFO_KHR : available[0];
}

VkExtent2D VulkanSwapChain::chooseSwapExtent(const VkSurfaceCapabilitiesKHR& caps) const {
    if (caps.currentExtent.width != UINT32_MAX) {
        return caps.currentExtent;  // 平台决定(window 已 resize 同步)
    }
    VkExtent2D actual{
        std::max(caps.minImageExtent.width,
                 std::min(caps.maxImageExtent.width, swapChainDesc_.width)),
        std::max(caps.minImageExtent.height,
                 std::min(caps.maxImageExtent.height, swapChainDesc_.height))};
    return actual;
}

bool VulkanSwapChain::createSwapchain() {
    VulkanDevice& vk = static_cast<VulkanDevice&>(device_);
    VkPhysicalDevice pd = vk.GetNativePhysicalDevice();

    // === 查询 surface capabilities ===
    VkSurfaceCapabilitiesKHR caps{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surface_, &caps) != VK_SUCCESS) {
        std::cerr << "[VulkanSwapChain] vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed" << std::endl;
        return false;
    }

    // === 查询 surface formats ===
    u32 formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface_, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    if (formatCount > 0) {
        vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface_, &formatCount, formats.data());
    }

    // === 查询 present modes ===
    u32 modeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surface_, &modeCount, nullptr);
    std::vector<VkPresentModeKHR> modes(modeCount);
    if (modeCount > 0) {
        vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surface_, &modeCount, modes.data());
    }

    VkSurfaceFormatKHR surfaceFormat = chooseSurfaceFormat(formats);
    VkPresentModeKHR   presentMode   = choosePresentMode(modes);
    VkExtent2D         extent        = chooseSwapExtent(caps);

    // === image count:bufferCount 与 caps 范围内 ===
    u32 imageCount = std::max(swapChainDesc_.bufferCount, caps.minImageCount);
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) {
        imageCount = caps.maxImageCount;
    }

    VkSwapchainCreateInfoKHR ci{};
    ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.pNext = nullptr;
    ci.flags = 0;
    ci.surface = surface_;
    ci.minImageCount = imageCount;
    ci.imageFormat = surfaceFormat.format;
    ci.imageColorSpace = surfaceFormat.colorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.queueFamilyIndexCount = 0;
    ci.pQueueFamilyIndices = nullptr;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = presentMode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = VK_NULL_HANDLE;  // Phase 4b MVP:不处理 recreate

    if (vkCreateSwapchainKHR(vk.GetNativeDevice(), &ci, nullptr, &swapchain_) != VK_SUCCESS) {
        std::cerr << "[VulkanSwapChain] vkCreateSwapchainKHR failed" << std::endl;
        return false;
    }

    imageFormat_ = surfaceFormat.format;
    imageColorSpace_ = surfaceFormat.colorSpace;
    extent_ = extent;
    swapChainDesc_.width = extent.width;
    swapChainDesc_.height = extent.height;
    return true;
}

void VulkanSwapChain::destroySwapchain() {
    if (swapchain_ == VK_NULL_HANDLE) return;
    VulkanDevice& vk = static_cast<VulkanDevice&>(device_);
    vkDeviceWaitIdle(vk.GetNativeDevice());
    vkDestroySwapchainKHR(vk.GetNativeDevice(), swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
    nativeImages_.clear();
}

bool VulkanSwapChain::createBackBufferTextures() {
    VulkanDevice& vk = static_cast<VulkanDevice&>(device_);
    u32 count = 0;
    if (vkGetSwapchainImagesKHR(vk.GetNativeDevice(), swapchain_, &count, nullptr) != VK_SUCCESS) {
        return false;
    }
    nativeImages_.resize(count);
    if (vkGetSwapchainImagesKHR(vk.GetNativeDevice(), swapchain_, &count, nativeImages_.data()) != VK_SUCCESS) {
        return false;
    }

    backBufferHandles_.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        TextureDesc td{};
        td.size.x = extent_.width;
        td.size.y = extent_.height;
        td.size.z = 1;
        td.mipLevels = 1;
        td.arraySize = 1;
        td.format = swapChainDesc_.format;
        td.type = TextureType::Texture2D;
        td.usage = TextureUsage::RenderTarget;
        td.memoryUsage = GPUMemoryUsage::SwapChain;
        td.name = "SwapChainBackBuffer";

        // 走 device 的 texture allocator,这样 GetTexture(handle) 才能找到
        // (InsertBarrier / BeginRenderPass 等都需要它)
        u32 id = vk.GetTextureAllocator().Allocate(vk, td, nativeImages_[i]);
        VulkanTexture* wrapper = vk.GetTextureAllocator().Get(id);
        if (!wrapper || !wrapper->Initialize()) {
            std::cerr << "[VulkanSwapChain] backbuffer wrap Initialize failed at index " << i << std::endl;
            if (wrapper) vk.GetTextureAllocator().Free(id);
            return false;
        }
        backBufferHandles_.push_back(static_cast<ResourceHandle>(id));
    }
    return true;
}

void VulkanSwapChain::destroyBackBufferTextures() {
    VulkanDevice& vk = static_cast<VulkanDevice&>(device_);
    for (ResourceHandle h : backBufferHandles_) {
        u32 id = static_cast<u32>(h);
        VulkanTexture* wrapper = vk.GetTextureAllocator().Get(id);
        if (wrapper) {
            wrapper->Destroy();
            vk.GetTextureAllocator().Free(id);
        }
    }
    backBufferHandles_.clear();
}

ResourceHandle VulkanSwapChain::GetBackBuffer(u32 index) const {
    if (index >= backBufferHandles_.size()) return handles::INVALID_RESOURCE;
    return backBufferHandles_[index];
}

// ============================================================================
// Acquire / Present
// ============================================================================

bool VulkanSwapChain::AcquireNextImage(u32* imageIndex, SyncHandle semaphore,
                                       SyncHandle fence) {
    if (swapchain_ == VK_NULL_HANDLE) return false;
    VulkanDevice& vk = static_cast<VulkanDevice&>(device_);

    VkSemaphore sem = VK_NULL_HANDLE;
    VkFence     fen = VK_NULL_HANDLE;
    if (semaphore != handles::INVALID_SYNC) {
        VulkanSync* s = vk.GetSync(semaphore);
        if (s) sem = s->GetSemaphore();
    }
    if (fence != handles::INVALID_SYNC) {
        VulkanSync* s = vk.GetSync(fence);
        if (s) fen = s->GetFence();
    }

    u32 idx = 0;
    VkResult res = vkAcquireNextImageKHR(vk.GetNativeDevice(), swapchain_,
                                         UINT64_MAX, sem, fen, &idx);
    if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR) {
        // Phase 4b MVP:仅 log,不 recreate(留给 Phase 4c)
        std::cerr << "[VulkanSwapChain] AcquireNextImage out-of-date/suboptimal — recreate TODO" << std::endl;
        return false;
    }
    if (res != VK_SUCCESS) {
        std::cerr << "[VulkanSwapChain] vkAcquireNextImageKHR failed: " << res << std::endl;
        return false;
    }
    currentIndex_ = idx;
    if (imageIndex) *imageIndex = idx;
    return true;
}

void VulkanSwapChain::Present(SyncHandle semaphore) {
    if (swapchain_ == VK_NULL_HANDLE) return;
    VulkanDevice& vk = static_cast<VulkanDevice&>(device_);

    VkSemaphore sem = VK_NULL_HANDLE;
    if (semaphore != handles::INVALID_SYNC) {
        VulkanSync* s = vk.GetSync(semaphore);
        if (s) sem = s->GetSemaphore();
    }

    VkPresentInfoKHR pi{};
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.pNext = nullptr;
    pi.waitSemaphoreCount = sem != VK_NULL_HANDLE ? 1 : 0;
    pi.pWaitSemaphores = &sem;
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &currentIndex_;
    pi.pResults = nullptr;

    VkResult res = vkQueuePresentKHR(vk.GetGraphicsQueue(), &pi);
    if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR) {
        std::cerr << "[VulkanSwapChain] Present out-of-date/suboptimal — recreate TODO" << std::endl;
    } else if (res != VK_SUCCESS) {
        std::cerr << "[VulkanSwapChain] vkQueuePresentKHR failed: " << res << std::endl;
    }

    currentIndex_ = (currentIndex_ + 1) % static_cast<u32>(backBufferHandles_.size());
}

void VulkanSwapChain::Resize(u32 width, u32 height) {
    if (swapChainDesc_.width == width && swapChainDesc_.height == height) return;
    swapChainDesc_.width = width;
    swapChainDesc_.height = height;

    VulkanDevice& vk = static_cast<VulkanDevice&>(device_);
    vkDeviceWaitIdle(vk.GetNativeDevice());

    destroyBackBufferTextures();
    destroySwapchain();
    // surface 不重建(window 句柄未变)

    if (!createSwapchain()) {
        std::cerr << "[VulkanSwapChain] Resize: createSwapchain failed" << std::endl;
        return;
    }
    if (!createBackBufferTextures()) {
        std::cerr << "[VulkanSwapChain] Resize: createBackBufferTextures failed" << std::endl;
    }
}

} // namespace primal::graphics::rhi

#endif // ENABLE_VULKAN
