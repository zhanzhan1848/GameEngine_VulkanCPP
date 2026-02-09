/**
 * @file SSRPass.cpp
 * @brief Implementation of Screen Space Reflection (SSR) pass.
 * 
 * Handles the creation and execution of the SSR compute shader,
 * including resource management (buffers, descriptors) and synchronization.
 */
#include "SSRPass.h"
#include <iostream>
#include <fstream>
#include <vector>

namespace primal::graphics {

SSRPass::SSRPass() {}

SSRPass::~SSRPass() {
    Shutdown();
}

bool SSRPass::Initialize(rhi::RHIDeviceBase* device) {
    device_ = device;

    // 1. Create Descriptor Set Layout
    rhi::DescriptorSetLayoutBinding bindings[4];

    bindings[0].binding = 0;
    bindings[0].descriptorType = rhi::DescriptorType::SampledImage;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = rhi::ShaderStage::Compute;

    bindings[1].binding = 1;
    bindings[1].descriptorType = rhi::DescriptorType::SampledImage;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = rhi::ShaderStage::Compute;

    bindings[2].binding = 2;
    bindings[2].descriptorType = rhi::DescriptorType::StorageImage;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = rhi::ShaderStage::Compute;

    bindings[3].binding = 3;
    bindings[3].descriptorType = rhi::DescriptorType::UniformBuffer;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags = rhi::ShaderStage::Compute;

    rhi::DescriptorSetLayoutDesc layoutDesc;
    layoutDesc.bindings = bindings;
    layoutDesc.bindingCount = 4;
    descriptorSetLayout_ = device_->CreateDescriptorSetLayout(layoutDesc);

    if (descriptorSetLayout_ == rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT) return false;

    // 2. Load Shader
    std::string shaderPath = "Engine/Graphics/Metal/shaders/SSRPass.metal";
    std::ifstream file(shaderPath, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        shaderPath = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/" + shaderPath;
        file.open(shaderPath, std::ios::ate | std::ios::binary);
    }

    if (!file.is_open()) {
        std::cerr << "SSRPass: Failed to open shader file: " << shaderPath << std::endl;
        return false;
    }
    
    size_t fileSize = (size_t)file.tellg();
    std::vector<char> buffer(fileSize + 1);
    file.seekg(0);
    file.read(buffer.data(), fileSize);
    buffer[fileSize] = '\0';
    file.close();

    computeShader_ = device_->CreateShader(buffer.data(), buffer.size(), rhi::ShaderStage::Compute, "kernelMain");
    
    if (computeShader_ == rhi::handles::INVALID_SHADER) {
        std::cerr << "SSRPass: Failed to create compute shader" << std::endl;
        return false;
    }

    // 3. Create Pipeline Layout
    rhi::PipelineLayoutDesc plDesc;
    plDesc.setLayouts = &descriptorSetLayout_;
    plDesc.setLayoutCount = 1;
    pipelineLayout_ = device_->CreatePipelineLayout(plDesc);

    // 4. Create Resources per frame
    for (uint32_t i = 0; i < rhi::MAX_FRAMES_IN_FLIGHT; ++i) {
        // Param Buffer
        rhi::BufferDesc paramDesc{
            sizeof(SSRParams),
            rhi::BufferType::Constant,
            rhi::GPUMemoryUsage::Dynamic,
            rhi::GPUMemoryUsage::Dynamic,
            0
        };
        
        paramBuffer_[i] = device_->CreateBuffer(paramDesc);
        paramBufferMapped_[i] = device_->MapBuffer(paramBuffer_[i]);
        
        // Descriptor Set
        rhi::DescriptorSetDesc setDesc;
        setDesc.layout = descriptorSetLayout_;
        descriptorSets_[i] = device_->CreateDescriptorSet(setDesc);
    }

    return true;
}

void SSRPass::Shutdown() {
    if (device_) {
        if (pipeline_ != rhi::handles::INVALID_PIPELINE) device_->DestroyPipeline(pipeline_);
        if (pipelineLayout_ != rhi::handles::INVALID_PIPELINE_LAYOUT) device_->DestroyPipelineLayout(pipelineLayout_);
        if (descriptorSetLayout_ != rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT) device_->DestroyDescriptorSetLayout(descriptorSetLayout_);
        if (computeShader_ != rhi::handles::INVALID_SHADER) device_->DestroyShader(computeShader_);

        for (uint32_t i = 0; i < rhi::MAX_FRAMES_IN_FLIGHT; ++i) {
            if (paramBuffer_[i] != rhi::handles::INVALID_RESOURCE) {
                device_->UnmapBuffer(paramBuffer_[i]);
                device_->DestroyBuffer(paramBuffer_[i]);
            }
            // Descriptor sets are destroyed with pool usually, but if independent:
            // device_->DestroyDescriptorSet(descriptorSets_[i]);
        }
    }
}

void SSRPass::Execute(rhi::RHICommandBuffer* cmdBuffer,
                      rhi::ResourceHandle sceneColor,
                      rhi::ResourceHandle sceneDepth,
                      rhi::ResourceHandle output,
                      uint32_t width, uint32_t height,
                      uint32_t frameIndex,
                      const rhi::math::m4x4& viewMatrix,
                      const rhi::math::m4x4& projMatrix) {
    
    if (pipeline_ == rhi::handles::INVALID_PIPELINE) {
        // Create Pipeline
        rhi::ComputePipelineDesc desc;
        desc.layout = pipelineLayout_;
        desc.computeShader = computeShader_;
        desc.threadGroupSize = {16, 16, 1};
        pipeline_ = device_->CreateComputePipeline(desc);
    }
    
    // Update Params
    if (paramBufferMapped_[frameIndex]) {
        SSRParams params;
        params.viewMatrix = viewMatrix;
        params.projMatrix = projMatrix;
        
        // Inverse Projection
        params.invProjMatrix = rhi::math::Inverse(projMatrix);
        
        params.resolution = { (float)width, (float)height, 1.0f / width, 1.0f / height };
        params.maxDistance = 100.0f; // Configurable
        params.stride = 1.0f;
        params.thickness = 0.5f;
        params.jitter = 0.0f; // Add time based jitter if needed
        
        memcpy(paramBufferMapped_[frameIndex], &params, sizeof(SSRParams));
    }

    // Update Descriptor Set
    rhi::DescriptorImageInfo colorInfo;
    colorInfo.imageView = sceneColor;
    colorInfo.imageLayout = rhi::ResourceState::ShaderResource;
    
    rhi::WriteDescriptorSet updateColor;
    updateColor.dstSet = descriptorSets_[frameIndex];
    updateColor.dstBinding = 0;
    updateColor.descriptorType = rhi::DescriptorType::SampledImage;
    updateColor.descriptorCount = 1;
    updateColor.imageInfo = &colorInfo;
    
    rhi::DescriptorImageInfo depthInfo;
    depthInfo.imageView = sceneDepth;
    depthInfo.imageLayout = rhi::ResourceState::ShaderResource;
    
    rhi::WriteDescriptorSet updateDepth;
    updateDepth.dstSet = descriptorSets_[frameIndex];
    updateDepth.dstBinding = 1;
    updateDepth.descriptorType = rhi::DescriptorType::SampledImage;
    updateDepth.descriptorCount = 1;
    updateDepth.imageInfo = &depthInfo;
    
    rhi::DescriptorImageInfo outputInfo;
    outputInfo.imageView = output;
    outputInfo.imageLayout = rhi::ResourceState::UnorderedAccess;
    
    rhi::WriteDescriptorSet updateOutput;
    updateOutput.dstSet = descriptorSets_[frameIndex];
    updateOutput.dstBinding = 2;
    updateOutput.descriptorType = rhi::DescriptorType::StorageImage;
    updateOutput.descriptorCount = 1;
    updateOutput.imageInfo = &outputInfo;
    
    rhi::DescriptorBufferInfo paramInfo;
    paramInfo.buffer = paramBuffer_[frameIndex];
    paramInfo.offset = 0;
    paramInfo.range = sizeof(SSRParams);
    
    rhi::WriteDescriptorSet updateParam;
    updateParam.dstSet = descriptorSets_[frameIndex];
    updateParam.dstBinding = 3;
    updateParam.descriptorType = rhi::DescriptorType::UniformBuffer;
    updateParam.descriptorCount = 1;
    updateParam.bufferInfo = &paramInfo;
    
    rhi::WriteDescriptorSet updates[] = {updateColor, updateDepth, updateOutput, updateParam};
    
    device_->UpdateDescriptorSets(4, updates);

    // Dispatch
    cmdBuffer->BindComputePipeline(pipeline_);
    cmdBuffer->BindDescriptorSets(rhi::PipelineBindPoint::Compute, pipelineLayout_, 0, 1, &descriptorSets_[frameIndex], 0, nullptr);
    
    uint32_t groupSizeX = 16;
    uint32_t groupSizeY = 16;
    uint32_t groupCountX = (width + groupSizeX - 1) / groupSizeX;
    uint32_t groupCountY = (height + groupSizeY - 1) / groupSizeY;
    
    cmdBuffer->Dispatch(groupCountX, groupCountY, 1);
}

} // namespace primal::graphics
