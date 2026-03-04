/**
 * @file MetalSampler.h
 * @brief Metal采样器实现
 * @author GameEngine VulkanCPP Team
 * @date 2026-01-07
 * @version 0.1.0
 */

#pragma once

#include "MetalCommon.h"
#include "../../Core/RHITypes.h"

namespace primal::graphics::rhi {

class MetalDevice;

class MetalSampler {
public:
    explicit MetalSampler(MetalDevice& device);
    ~MetalSampler();

    bool Initialize(const SamplerDesc& desc);
    void Destroy();

    MTL::SamplerState* GetSamplerState() const { return samplerState_; }

    void SetHandle(SamplerHandle handle) { handle_ = handle; }
    SamplerHandle GetHandle() const { return handle_; }

private:
    MetalDevice& device_;
    SamplerHandle handle_ = handles::INVALID_SAMPLER;
    MTL::SamplerState* samplerState_ = nullptr;
};

} // namespace primal::graphics::rhi
