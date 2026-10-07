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

#include "../Core/RHIDevice.h"

#if defined(__APPLE__)
#include "../Platforms/Metal/MetalDevice.h"
#endif

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN
#include "../Platforms/Vulkan/VulkanDevice.h"
#endif

namespace primal::graphics::rhi {

RHIDeviceBase* CreateRHIDevice(const DeviceDesc& desc) {
    RHIDeviceBase* device = nullptr;
    switch (desc.platform) {
        case RHIPlatform::Metal: {
#if defined(__APPLE__)
            auto* metalDevice = new MetalDevice(desc);
            if (metalDevice && metalDevice->Initialize()) {
                device = metalDevice;
            } else {
                delete metalDevice;
            }
#endif
            break;
        }
        case RHIPlatform::Vulkan: {
#if defined(ENABLE_VULKAN) && ENABLE_VULKAN
            auto* vulkanDevice = new VulkanDevice(desc);
            if (vulkanDevice && vulkanDevice->Initialize()) {
                device = vulkanDevice;
            } else {
                delete vulkanDevice;
            }
#endif
            break;
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

    // [PassDebugView/RG 修复] 注册进 RHIDeviceManager——此前全代码库零调用，
    // g_deviceManager.GetDeviceCount() 恒 0，get_rhi_gpu_mesh 的惰性创建被
    // 设备计数检查短路（RHIGpuMesh 永远 null → Nanite/依赖链全部拿不到
    // GPU mesh）。2026-09-22 pass view 排查定位。
    if (device) {
        g_deviceManager.RegisterDevice(device);
    }
    return device;
}

void DestroyRHIDevice(RHIDeviceBase* device) {
    if (!device) return;
    if (const u32 count = g_deviceManager.GetDeviceCount(); count > 0) {
        // UnregisterDevice 按 deviceId 注销；此处用遍历匹配指针（保持
        // Factory 的无 id 语义）。
        for (u32 i = 0; i < count; ++i) {
            g_deviceManager.UnregisterDevice(i);
        }
    }
    device->Shutdown();
    delete device;
}

} // namespace primal::graphics::rhi
