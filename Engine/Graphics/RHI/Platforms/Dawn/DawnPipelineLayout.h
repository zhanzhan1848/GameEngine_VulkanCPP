#pragma once

#include "DawnCommon.h"

#if defined(ENABLE_WEBGPU) && ENABLE_WEBGPU

#include "../../Core/RHITypes.h"

namespace primal::graphics::rhi {

class DawnDevice;

class DawnPipelineLayout {
    friend class DawnDevice;
public:
    ~DawnPipelineLayout();
    WGPUPipelineLayout GetNativeLayout() const { return wgpuLayout_; }

    /// Returns the bind group index for push constants (= setLayoutCount).
    u32 GetPushConstantBindGroupIndex() const { return setLayoutCount_; }

    WGPUBindGroupLayout GetPushConstantBindGroupLayout() const { return pushConstantBindGroupLayout_; }

    bool HasPushConstants() const { return pushConstantBindGroupLayout_ != nullptr; }

public:
    DawnPipelineLayout(DawnDevice& device);

private:
    bool Initialize(const PipelineLayoutDesc& desc);
    void Destroy();

    DawnDevice& device_;
    WGPUPipelineLayout wgpuLayout_ = nullptr;
    WGPUBindGroupLayout pushConstantBindGroupLayout_ = nullptr;
    u32 setLayoutCount_{ 0 };
};

} // namespace primal::graphics::rhi

#endif // ENABLE_WEBGPU
