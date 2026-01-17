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
                 uint32_t width, uint32_t height, uint32_t layers,
                 uint32_t frameIndex,
                 int radius = 5, float sigma = 2.0f);

private:
    struct BlurParams {
        uint32_t textureWidth;
        uint32_t textureHeight;
        int32_t blurRadius;
        float sigma;
        uint32_t arrayLayer;
        uint32_t direction; // 0: Horizontal, 1: Vertical
        uint32_t padding[2];
    };

    rhi::RHIDeviceBase* device_ = nullptr;
    rhi::ShaderHandle computeShader_ = rhi::handles::INVALID_SHADER;
    rhi::PipelineLayoutHandle pipelineLayout_ = rhi::handles::INVALID_PIPELINE_LAYOUT;
    rhi::PipelineHandle pipeline_ = rhi::handles::INVALID_PIPELINE;
    rhi::DescriptorSetLayoutHandle descriptorSetLayout_ = rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT;
    
    // Resource Management
    // We use a ring buffer of descriptor sets to handle dynamic texture bindings
    static constexpr uint32_t MAX_SETS_PER_FRAME = 128; // Enough for many layers
    rhi::DescriptorSetHandle setPool_[rhi::MAX_FRAMES_IN_FLIGHT][MAX_SETS_PER_FRAME];
    uint32_t currentSetIndex_[rhi::MAX_FRAMES_IN_FLIGHT] = {0};
    
    // Dynamic Parameter Buffer
    rhi::ResourceHandle paramBuffer_[rhi::MAX_FRAMES_IN_FLIGHT];
    void* paramBufferMapped_[rhi::MAX_FRAMES_IN_FLIGHT];
    uint32_t paramBufferOffset_[rhi::MAX_FRAMES_IN_FLIGHT] = {0};
    static constexpr uint32_t MAX_PARAM_BUFFER_SIZE = 1024 * 1024; // 1MB
    
    rhi::DescriptorSetHandle GetDescriptorSet(uint32_t frameIndex, 
                                            rhi::ResourceHandle input, 
                                            rhi::ResourceHandle output, 
                                            uint32_t paramOffset);
};

} // namespace primal::graphics
