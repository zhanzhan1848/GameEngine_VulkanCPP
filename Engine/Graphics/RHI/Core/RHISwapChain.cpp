/**
 * @file RHISwapChain.cpp
 * @brief RHI交换链基类实现
 * @author GameEngine VulkanCPP Team
 * @date 2026-01-07
 * @version 0.1.0
 */

#include "RHISwapChain.h"
#include "RHIDevice.h"

namespace primal::graphics::rhi {

RHISwapChain::RHISwapChain(RHIDeviceBase& device, const SwapChainDesc& desc)
    : RHIResource(device, ResourceDesc(ResourceType::SwapChain, ResourceUsage::Present, GPUMemoryUsage::Dynamic, 0, "SwapChain")),
      swapChainDesc_(desc) {
}

void RHISwapChain::Destroy() {
    RHIResource::Destroy();
}

void RHISwapChain::Resize(uint32_t width, uint32_t height) {
    if (swapChainDesc_.width == width && swapChainDesc_.height == height) {
        return;
    }

    swapChainDesc_.width = width;
    swapChainDesc_.height = height;
    
    // 子类需要重写此方法并调用基类方法，或者在此处处理通用逻辑
    // 通常需要销毁旧的交换链并创建新的
}

} // namespace primal::graphics::rhi
