#include "GIPlatformProfile.h"
#include "../RHI/Core/RHIDevice.h"

namespace primal::graphics::lumen {

GIPlatformProfile GIPlatformProfile::Detect(const rhi::RHIDeviceBase* device) {
    if (!device) return Desktop();
    switch (device->GetPlatform()) {
    case rhi::RHIPlatform::Metal:
        // Apple Silicon unified memory — tight per-thread read budgets and
        // split-dispatch is the norm for texture3D+buffer mixing kernels.
        return AppleSilicon();
    case rhi::RHIPlatform::Dawn:
        return Web();
    case rhi::RHIPlatform::Vulkan:
    default:
        return Desktop();
    }
}

} // namespace primal::graphics::lumen
