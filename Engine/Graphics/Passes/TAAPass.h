#pragma once

#include "Engine/Graphics/RHI/Core/RHIDevice.h"
#include "Engine/Graphics/RHI/Core/RHICommand.h"
#include "Engine/Graphics/RHI/Core/RHITypes.h"
#include "Engine/Graphics/RHI/Core/RHIDescriptorSetLayout.h"
#include "Engine/Graphics/RHI/Core/RHIDescriptorSet.h"
#include "Engine/Graphics/RHI/Core/RHIPipelineLayout.h"
#include "Engine/Graphics/RHI/Core/RHIMath.h"

namespace primal::graphics {

class TAAPass {
public:
    TAAPass();
    ~TAAPass();

    bool Initialize(rhi::RHIDeviceBase* device, u32 width, u32 height, rhi::DataFormat outputFormat = rhi::DataFormat::RGBA8_UNorm);
    void Shutdown();

    // Execute TAA
    // Output should be a valid RenderTarget
    void Execute(rhi::RHICommandBuffer* cmdBuffer,
                 rhi::ResourceHandle colorInput,
                 rhi::ResourceHandle historyInput,
                 rhi::ResourceHandle velocityInput,
                 rhi::ResourceHandle output,
                 u32 width, u32 height,
                 u32 frameIndex,
                 float jitterX, float jitterY,
                 float prevJitterX, float prevJitterY);

private:
    struct TAAUniforms {
        float resolution[2];
        float jitter[2];
        float previousJitter[2];
        float feedback;
        float padding;
    };

    rhi::RHIDeviceBase* device_ = nullptr;
    rhi::ShaderHandle vertexShader_ = rhi::handles::INVALID_SHADER;
    rhi::ShaderHandle fragmentShader_ = rhi::handles::INVALID_SHADER;
    rhi::PipelineLayoutHandle pipelineLayout_ = rhi::handles::INVALID_PIPELINE_LAYOUT;
    rhi::PipelineHandle pipeline_ = rhi::handles::INVALID_PIPELINE;
    rhi::DescriptorSetLayoutHandle descriptorSetLayout_ = rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT;
    
    // Per-frame resources
    rhi::DescriptorSetHandle descriptorSets_[rhi::MAX_FRAMES_IN_FLIGHT];
    rhi::ResourceHandle uniformBuffers_[rhi::MAX_FRAMES_IN_FLIGHT];
    void* uniformBuffersMapped_[rhi::MAX_FRAMES_IN_FLIGHT];
};

} // namespace primal::graphics
