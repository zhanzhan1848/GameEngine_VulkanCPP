#pragma once

namespace primal::graphics
{
    struct platform_interface;

    namespace metal
    {
        // === Phase 1 Sub-step 1.2.6': 旧 platform_interface 填充器，Phase 2 删除 ===
        [[deprecated(
            "metal::get_platform_interface is deprecated. The Metal backend now lives in "
            "Engine/Graphics/RHI/Platforms/Metal/MetalDevice and is selected via "
            "RHIDeviceFactory::CreateRHIDevice(DeviceDesc{platform=Metal}). See Phase 1 Sub-step 1.2.6'."
        )]]
        void get_platform_interface(platform_interface& pi);
    }
}