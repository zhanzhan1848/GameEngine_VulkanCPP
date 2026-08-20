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
    // T4.6.5 part 30.6 (X4 secondary fix): idempotency guard.
    // VulkanDevice::createSwapChainImpl already called Initialize() before
    // returning the constructed object; RenderSystem::Initialize at line 47
    // calls it again. Without this guard, the second call destroys + recreates
    // the swapchain + backbuffer textures, leaking the VkSwapchainKHR handle
    // and orphaning the previously-allocated ResourceHandle slots.
    if (swapchain_ != VK_NULL_HANDLE) {
        state_ = ResourceState::Ready;
        return true;
    }
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

bool VulkanSwapChain::createSwapchain(VkSwapchainKHR oldSwapchain) {
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
    // P4c-F2: recreate 时手递手旧 swapchain,驱动可复用资源配置,
    // 避免表面闪烁(旧 handle 在新链创建成功后销毁)。
    ci.oldSwapchain = oldSwapchain;

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
// P4c-F2: 失效自动重建
// ============================================================================

bool VulkanSwapChain::recreateSurface() {
    // SURFACE_LOST 恢复路径:销毁并按原始窗口句柄(swapChainDesc_.window,
    // 从未变过)重建 VkSurfaceKHR。必须在 swapchain 重建之前完成 —
    // createSwapchain 的 capability 查询依赖活着的 surface。
    VulkanDevice& vk = static_cast<VulkanDevice&>(device_);
    vkDeviceWaitIdle(vk.GetNativeDevice());
    device_.GetGarbageCollector().Flush();
    destroySurface();
    if (!createSurface()) {
        std::cerr << "[VulkanSwapChain] recreateSurface: createSurface failed" << std::endl;
        return false;
    }
    return true;
}

bool VulkanSwapChain::Recreate() {
    VulkanDevice& vk = static_cast<VulkanDevice&>(device_);
    vkDeviceWaitIdle(vk.GetNativeDevice());

    // backbuffer 包装纹理走 Destroy()(view 经 GC 延迟销毁),随后 Flush
    // 让 view 在旧 swapchain 销毁前真正死亡 — 消除 use-after-free
    // validation error。RenderSystem 侧若仍持句柄,GetTexture() 会得到
    // slot 空态,而下一次 GetBackBuffer() 拿到的是新包装。
    destroyBackBufferTextures();
    device_.GetGarbageCollector().Flush();

    VkSwapchainKHR old = swapchain_;
    swapchain_ = VK_NULL_HANDLE;
    if (!createSwapchain(old)) {
        std::cerr << "[VulkanSwapChain] Recreate: createSwapchain failed" << std::endl;
        if (old != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(vk.GetNativeDevice(), old, nullptr);
        }
        return false;
    }
    // 新链已接管,现在销毁旧链(image 归旧链所有)。
    if (old != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(vk.GetNativeDevice(), old, nullptr);
    }
    if (!createBackBufferTextures()) {
        std::cerr << "[VulkanSwapChain] Recreate: createBackBufferTextures failed" << std::endl;
        return false;
    }
    std::cout << "[VulkanSwapChain] Recreate: "
              << swapChainDesc_.width << "x" << swapChainDesc_.height << std::endl;
    return true;
}

// ============================================================================
// Acquire / Present
// ============================================================================

bool VulkanSwapChain::AcquireNextImage(u32* imageIndex, SyncHandle semaphore,
                                       SyncHandle fence) {
    if (swapchain_ == VK_NULL_HANDLE) return false;
    VulkanDevice& vk = static_cast<VulkanDevice&>(device_);

    // P4c-F2: 上一帧 present 端置位的失效标志在此消费 — 渲染循环无需
    // 任何调用方干预即可恢复(隐式 resize/DPI 变化的自动恢复路径)。
    if (needsRecreate_) {
        needsRecreate_ = false;
        if (!Recreate()) return false;
    }

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

    // P4c-F2: OUT_OF_DATE 时重建并重试(上限 8 次 — 持续失效说明窗口
    // 处于持续 resize 风暴中,放弃本次 acquire 让上层跳帧)。
    constexpr u32 kMaxAcquireRetries = 8;
    for (u32 attempt = 0; attempt < kMaxAcquireRetries; ++attempt) {
        u32 idx = 0;
        VkResult res = vkAcquireNextImageKHR(vk.GetNativeDevice(), swapchain_,
                                             UINT64_MAX, sem, fen, &idx);
        if (res == VK_SUCCESS || res == VK_SUBOPTIMAL_KHR) {
            // SUBOPTIMAL:本次仍可呈现,下一帧入口重建。
            if (res == VK_SUBOPTIMAL_KHR) needsRecreate_ = true;
            currentIndex_ = idx;
            if (imageIndex) *imageIndex = idx;
            return true;
        }
        if (res == VK_ERROR_OUT_OF_DATE_KHR) {
            std::cerr << "[VulkanSwapChain] AcquireNextImage OUT_OF_DATE — recreating (attempt "
                      << attempt + 1 << "/" << kMaxAcquireRetries << ")" << std::endl;
            if (!Recreate()) return false;
            continue;
        }
        if (res == VK_ERROR_SURFACE_LOST_KHR) {
            std::cerr << "[VulkanSwapChain] AcquireNextImage SURFACE_LOST — recreating surface"
                      << std::endl;
            if (!recreateSurface() || !Recreate()) return false;
            continue;
        }
        std::cerr << "[VulkanSwapChain] vkAcquireNextImageKHR failed: " << res << std::endl;
        return false;
    }
    std::cerr << "[VulkanSwapChain] AcquireNextImage: swapchain kept being out-of-date after "
              << kMaxAcquireRetries << " recreates — skipping this frame" << std::endl;
    return false;
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
        // P4c-F2: 置标志,下一帧 Acquire 入口重建 — 不在 present 返回
        // 路径里同步重建(避免打断当前帧的收尾)。
        std::cerr << "[VulkanSwapChain] Present out-of-date/suboptimal — will recreate next frame"
                  << std::endl;
        needsRecreate_ = true;
    } else if (res == VK_ERROR_SURFACE_LOST_KHR) {
        std::cerr << "[VulkanSwapChain] Present SURFACE_LOST — recreating surface" << std::endl;
        needsRecreate_ = true;
        recreateSurface();
    } else if (res != VK_SUCCESS) {
        std::cerr << "[VulkanSwapChain] vkQueuePresentKHR failed: " << res << std::endl;
    }

    currentIndex_ = (currentIndex_ + 1) % static_cast<u32>(backBufferHandles_.size());
}

void VulkanSwapChain::Resize(u32 width, u32 height) {
    if (swapChainDesc_.width == width && swapChainDesc_.height == height) return;
    swapChainDesc_.width = width;
    swapChainDesc_.height = height;

    // P4c-F2: 显式 resize 复用与隐式失效相同的重建路径
    // (GC Flush 保证 view 先于旧 swapchain 销毁)。
    if (!Recreate()) return;
}

} // namespace primal::graphics::rhi

#endif // ENABLE_VULKAN
