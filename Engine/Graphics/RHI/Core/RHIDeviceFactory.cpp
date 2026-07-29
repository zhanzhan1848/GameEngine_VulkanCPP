/**
 * @file RHIDeviceFactory.cpp
 * @brief RHI 设备被动分发器实现
 * @details 被动映射：UI 层决策 platform 值 -> 工厂实例化对应子类。
 *          未实现的后端返回 nullptr。
 * @author GameEngine VulkanCPP Team
 * @date 2026-06-16
 * @version 0.1.0
 */

#include "RHIDeviceFactory.h"

#if defined(__APPLE__)
#include "../Platforms/Metal/MetalDevice.h"
#endif

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN
#include "../Platforms/Vulkan/VulkanDevice.h"
#endif

namespace primal::graphics::rhi {

RHIDeviceBase* CreateRHIDevice(const DeviceDesc& desc) {
    switch (desc.platform) {
        case RHIPlatform::Metal: {
#if defined(__APPLE__)
            auto* device = new MetalDevice(desc);
            if (device && device->Initialize()) {
                return device;
            }
            delete device;
#endif
            return nullptr;
        }
        case RHIPlatform::Vulkan: {
#if defined(ENABLE_VULKAN) && ENABLE_VULKAN
            auto* device = new VulkanDevice(desc);
            if (device && device->Initialize()) {
                return device;
            }
            delete device;
#endif
            return nullptr;
        }
        case RHIPlatform::D3D12:
            // 未实现：RHI/Platforms/D3D12/ 为空目录
            return nullptr;
        case RHIPlatform::Dawn:
            // 未实现：RHI/Platforms/Dawn/ 为空目录
            return nullptr;
        case RHIPlatform::Unknown:
        default:
            return nullptr;
    }
}

void DestroyRHIDevice(RHIDeviceBase* device) {
    if (!device) return;
    device->Shutdown();
    delete device;
}

} // namespace primal::graphics::rhi
