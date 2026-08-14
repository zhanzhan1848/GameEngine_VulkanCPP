#include "DawnSampler.h"

#if defined(ENABLE_WEBGPU) && ENABLE_WEBGPU

#include "DawnDevice.h"
#include <iostream>

namespace primal::graphics::rhi {

DawnSampler::DawnSampler(DawnDevice& device)
    : device_(device) {}

DawnSampler::~DawnSampler() {
    Destroy();
}

bool DawnSampler::Initialize(const SamplerDesc& desc) {
    WGPUSamplerDescriptor wgpuDesc{};
    wgpuDesc.nextInChain = nullptr;
    wgpuDesc.label = ToWGPUStringView("DawnSampler");
    wgpuDesc.addressModeU = ToWGPUAddressMode(desc.addressU);
    wgpuDesc.addressModeV = ToWGPUAddressMode(desc.addressV);
    wgpuDesc.addressModeW = ToWGPUAddressMode(desc.addressW);
    wgpuDesc.magFilter = ToWGPUFilterMode(desc.magFilter);
    wgpuDesc.minFilter = ToWGPUFilterMode(desc.minFilter);
    wgpuDesc.mipmapFilter = ToWGPUMipmapFilterMode(desc.mipFilter);
    wgpuDesc.lodMinClamp = desc.minLod;
    wgpuDesc.lodMaxClamp = desc.maxLod;

    if (desc.maxAnisotropy > 1) {
        wgpuDesc.maxAnisotropy = desc.maxAnisotropy;
    } else {
        wgpuDesc.maxAnisotropy = 1;
    }

    if (desc.comparisonFunc != ComparisonFunc::Never) {
        wgpuDesc.compare = ToWGPUCompareFunction(desc.comparisonFunc);
    } else {
        wgpuDesc.compare = WGPUCompareFunction_Undefined;
    }

    wgpuSampler_ = wgpuDeviceCreateSampler(device_.GetNativeDevice(), &wgpuDesc);
    if (!wgpuSampler_) {
        std::cerr << "[DawnSampler] Failed to create sampler" << std::endl;
        return false;
    }
    return true;
}

void DawnSampler::Destroy() {
    if (wgpuSampler_) {
        wgpuSamplerRelease(wgpuSampler_);
        wgpuSampler_ = nullptr;
    }
}

} // namespace primal::graphics::rhi

#endif // ENABLE_WEBGPU
