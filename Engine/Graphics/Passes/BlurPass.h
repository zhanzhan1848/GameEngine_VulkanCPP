#pragma once

#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Core/RHITypes.h"
#include "Graphics/RHI/Core/RHIDescriptorSetLayout.h"
#include "Graphics/RHI/Core/RHIDescriptorSet.h"
#include "Graphics/RHI/Core/RHIPipelineLayout.h"
#include "Graphics/RHI/Core/RHIMath.h"

#include <vector>

namespace primal::graphics {

class BlurPass {
public:
    BlurPass();
    ~BlurPass();

    bool Initialize(rhi::RHIDeviceBase* device);
    void Shutdown();

    // Execute Blur on Texture2DArray (RG32Float)
    // Performs two passes: Horizontal (Input -> Temp) and Vertical (Temp -> Output)
    void Execute(rhi::RHICommandBuffer* cmdBuffer,
                 rhi::ResourceHandle input,
                 rhi::ResourceHandle output,
                 rhi::ResourceHandle temp,
                 u32 width, u32 height, u32 layers,
                 u32 frameIndex,
                 int radius = 5, float sigma = 2.0f);

private:
    struct BlurParams {
        u32 textureWidth;
        u32 textureHeight;
        s32 blurRadius;
        float sigma;
        u32 arrayLayer;
        u32 direction; // 0: Horizontal, 1: Vertical
        u32 padding[2];
    };

    rhi::RHIDeviceBase* device_ = nullptr;
    rhi::ShaderHandle computeShader_ = rhi::handles::INVALID_SHADER;
    rhi::PipelineLayoutHandle pipelineLayout_ = rhi::handles::INVALID_PIPELINE_LAYOUT;
    rhi::PipelineHandle pipeline_ = rhi::handles::INVALID_PIPELINE;
    rhi::DescriptorSetLayoutHandle descriptorSetLayout_ = rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT;
    
    // Resource Management
    // We use a ring buffer of descriptor sets to handle dynamic texture bindings
    static constexpr u32 MAX_SETS_PER_FRAME = 128; // Enough for many layers
    rhi::DescriptorSetHandle setPool_[rhi::MAX_FRAMES_IN_FLIGHT][MAX_SETS_PER_FRAME];
    u32 currentSetIndex_[rhi::MAX_FRAMES_IN_FLIGHT] = {0};
    
    // Dynamic Parameter Buffer
    rhi::ResourceHandle paramBuffer_[rhi::MAX_FRAMES_IN_FLIGHT];
    void* paramBufferMapped_[rhi::MAX_FRAMES_IN_FLIGHT];
    u32 paramBufferOffset_[rhi::MAX_FRAMES_IN_FLIGHT] = {0};
    static constexpr u32 MAX_PARAM_BUFFER_SIZE = 1024 * 1024; // 1MB
    
    rhi::DescriptorSetHandle GetDescriptorSet(u32 frameIndex, 
                                            rhi::ResourceHandle input, 
                                            rhi::ResourceHandle output, 
                                            u32 paramOffset);
};

} // namespace primal::graphics
