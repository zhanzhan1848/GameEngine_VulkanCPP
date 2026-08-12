#pragma once

#include "DawnCommon.h"

#if defined(ENABLE_WEBGPU) && ENABLE_WEBGPU

#include "../../Core/RHITypes.h"
#include "../../Core/RHIDevice.h"

namespace primal::graphics::rhi {

class DawnDevice;

class DawnPipeline {
    friend class DawnDevice;
public:
    ~DawnPipeline();
    WGPURenderPipeline GetRenderPipeline() const { return wgpuRenderPipeline_; }
    WGPUComputePipeline GetComputePipeline() const { return wgpuComputePipeline_; }
    bool IsCompute() const { return isCompute_; }
    PipelineLayoutHandle GetLayout() const { return layout_; }

public:
    DawnPipeline(DawnDevice& device);

private:
    bool InitializeGraphics(const GraphicsPipelineDesc& desc);
    bool InitializeCompute(const ComputePipelineDesc& desc);
    void Destroy();

    DawnDevice& device_;
    bool isCompute_ = false;
    WGPURenderPipeline wgpuRenderPipeline_ = nullptr;
    WGPUComputePipeline wgpuComputePipeline_ = nullptr;
    PipelineLayoutHandle layout_ = handles::INVALID_PIPELINE_LAYOUT;
};

} // namespace primal::graphics::rhi

#endif // ENABLE_WEBGPU
