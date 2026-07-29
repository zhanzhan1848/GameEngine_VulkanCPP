/**
 * @file VulkanSwapChain.h
 * @brief Vulkan RHI 交换链实现
 * @details Phase 4b:
 *          - VkSurfaceKHR (platform 创建) + VkSwapchainKHR
 *          - N 个 backbuffer image 由 swapchain 拥有,VulkanTexture wrap 模式包装
 *          - 每帧 acquire/renderFinished semaphore 对
 *          - FIFO present mode(vsync on)默认;Mailbox/Immediate 按 SwapChainDesc 选
 * @author GameEngine VulkanCPP Team
 * @date 2026-07-27
 */

#pragma once

#include "VulkanCommon.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

#include "../../Core/RHISwapChain.h"
#include "VulkanTexture.h"

#include <vector>

namespace primal::graphics::rhi {

class VulkanDevice;

class VulkanSwapChain : public RHISwapChain {
    friend class VulkanDevice;
public:
    VulkanSwapChain(VulkanDevice& device, const SwapChainDesc& desc);
    virtual ~VulkanSwapChain();

    bool Initialize() override;
    void Destroy() override;
    void Resize(u32 width, u32 height) override;

    bool AcquireNextImage(u32* imageIndex, SyncHandle semaphore = handles::INVALID_SYNC,
                          SyncHandle fence = handles::INVALID_SYNC) override;
    void Present(SyncHandle semaphore) override;

    u32 GetCurrentBackBufferIndex() const override { return currentIndex_; }
    ResourceHandle GetBackBuffer(u32 index) const override;

protected:
    // RHIResource 抽象纯虚的 no-op 实现(SwapChain 不支持 map/update)
    void* mapImpl(u64, u64) override { return nullptr; }
    void unmapImpl() override {}
    bool updateDataImpl(const void*, u64, u64) override { return false; }

private:
    bool createSurface();
    void destroySurface();

    bool createSwapchain();
    void destroySwapchain();

    bool createBackBufferTextures();
    void destroyBackBufferTextures();

    VkSurfaceFormatKHR chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& available) const;
    VkPresentModeKHR choosePresentMode(const std::vector<VkPresentModeKHR>& available) const;
    VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR& caps) const;

    VkSurfaceKHR  surface_{VK_NULL_HANDLE};
    VkSwapchainKHR swapchain_{VK_NULL_HANDLE};
    VkFormat      imageFormat_{VK_FORMAT_UNDEFINED};
    VkColorSpaceKHR imageColorSpace_{VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    VkExtent2D    extent_{0, 0};

    std::vector<VkImage>         nativeImages_;   // swapchain 拥有
    std::vector<ResourceHandle>  backBufferHandles_;  // wrap 后的 VulkanTexture handles
    u32                          currentIndex_{0};
};

} // namespace primal::graphics::rhi

#endif // ENABLE_VULKAN
