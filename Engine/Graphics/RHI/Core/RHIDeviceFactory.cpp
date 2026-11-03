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

// D3D12 后端（M0 F0-1/F0-8）：Windows 专属 COM 层只在 _WIN32 + ENABLE_D3D12
// 编译；其余平台（含 macOS）该分支保持 return nullptr——平台中立核
// （D3D12Abi/Format/StateMapping/StateTracker/RootSignatureBudget）不经过
// 工厂，由测试直接链接。
#if defined(_WIN32) && defined(ENABLE_D3D12) && ENABLE_D3D12
#include "../Platforms/D3D12/D3D12Device.h"
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
#if defined(_WIN32) && defined(ENABLE_D3D12) && ENABLE_D3D12
        {
            auto* d3d12Device = new D3D12Device(desc);
            if (d3d12Device && d3d12Device->Initialize()) {
                device = d3d12Device;
            } else {
                delete d3d12Device;
            }
        }
            break;
#else
            // 未启用 D3D12 后端（非 Windows 构建，或 Windows 上 ENABLE_D3D12=OFF）。
            // 平台中立核存在于 RHI/Platforms/D3D12/，但无设备实现。
            return nullptr;
#endif
        case RHIPlatform::Dawn:
            // 未实现：RHI/Platforms/Dawn/ 无设备实现
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
