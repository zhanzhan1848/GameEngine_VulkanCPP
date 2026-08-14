/**
 * @file DawnSampler.h
 * @brief Dawn/WebGPU sampler (WGPUSampler wrapper)
 * @author GameEngine VulkanCPP Team
 * @date 2026-05-14
 * @version 0.1.0
 */

#pragma once

#include "DawnCommon.h"

#if defined(ENABLE_WEBGPU) && ENABLE_WEBGPU

#include "../../Core/RHITypes.h"

namespace primal::graphics::rhi {

class DawnDevice;

class DawnSampler {
    friend class DawnDevice;
public:
    ~DawnSampler();
    WGPUSampler GetNativeSampler() const { return wgpuSampler_; }

public:
    DawnSampler(DawnDevice& device);

private:
    bool Initialize(const SamplerDesc& desc);
    void Destroy();

    DawnDevice& device_;
    WGPUSampler wgpuSampler_ = nullptr;
};

} // namespace primal::graphics::rhi

#endif // ENABLE_WEBGPU
