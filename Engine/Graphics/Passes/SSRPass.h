#pragma once

#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Core/RHITypes.h"
#include "Graphics/RHI/Core/RHIDescriptorSetLayout.h"
#include "Graphics/RHI/Core/RHIDescriptorSet.h"
#include "Graphics/RHI/Core/RHIPipelineLayout.h"
#include "Graphics/RHI/Core/RHIMath.h"

namespace primal::graphics {

class SSRPass {
public:
    SSRPass();
    ~SSRPass();

    bool Initialize(rhi::RHIDeviceBase* device);
    void Shutdown();

    // Execute SSR (SceneColor + Depth -> Output)
    void Execute(rhi::RHICommandBuffer* cmdBuffer,
                 rhi::ResourceHandle sceneColor,
                 rhi::ResourceHandle sceneDepth,
                 rhi::ResourceHandle output,
                 uint32_t width, uint32_t height,
                 uint32_t frameIndex,
                 const rhi::math::m4x4& viewMatrix,
                 const rhi::math::m4x4& projMatrix);

private:
    struct SSRParams {
        rhi::math::m4x4 viewMatrix;
        rhi::math::m4x4 projMatrix;
        rhi::math::m4x4 invProjMatrix;
        rhi::math::v4 resolution; // xy: size, zw: invSize
        float maxDistance;
        float stride;
        float thickness;
        float jitter;
    };

    rhi::RHIDeviceBase* device_ = nullptr;
    rhi::ShaderHandle computeShader_ = rhi::handles::INVALID_SHADER;
    rhi::PipelineLayoutHandle pipelineLayout_ = rhi::handles::INVALID_PIPELINE_LAYOUT;
    rhi::PipelineHandle pipeline_ = rhi::handles::INVALID_PIPELINE;
    rhi::DescriptorSetLayoutHandle descriptorSetLayout_ = rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT;
    
    rhi::DescriptorSetHandle descriptorSets_[rhi::MAX_FRAMES_IN_FLIGHT];
    rhi::ResourceHandle paramBuffer_[rhi::MAX_FRAMES_IN_FLIGHT];
    void* paramBufferMapped_[rhi::MAX_FRAMES_IN_FLIGHT];
};

} // namespace primal::graphics
