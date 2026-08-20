#include "Graphics/Passes/BlurPass.h"
#include "Graphics/Utils/ShaderRegistry.h"
#include <fstream>
#include <iostream>

namespace primal::graphics {

BlurPass::BlurPass() {}

BlurPass::~BlurPass() {
    Shutdown();
}

bool BlurPass::Initialize(rhi::RHIDeviceBase* device) {
    device_ = device;
    if (!device_) return false;

    // 1. Load Shader
    auto platform = device_->GetPlatform();
    std::string shaderPath = utils::ShaderRegistry::GetShaderPath(platform, "BlurPass");
    std::ifstream file(shaderPath, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        shaderPath = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/.worktrees/vulkan-rhi/" + shaderPath;
        file.open(shaderPath, std::ios::ate | std::ios::binary);
    }

    if (!file.is_open()) {
        std::cerr << "BlurPass: Failed to open shader file: " << shaderPath << std::endl;
        return false;
    }
    
    size_t fileSize = (size_t)file.tellg();
    // SPIR-V (Vulkan) is 4-byte-aligned binary — do NOT append a null byte
    // (2344+1=2345 breaks the alignment and the driver rejects the module).
    // Metal/Dawn source text keeps the null terminator for runtime compile.
    const bool isBinarySpirV = (platform == rhi::RHIPlatform::Vulkan);
    size_t codeSize = isBinarySpirV ? fileSize : fileSize + 1;
    utl::vector<char> buffer(codeSize);
    file.seekg(0);
    file.read(buffer.data(), fileSize);
    if (!isBinarySpirV) buffer[fileSize] = '\0';
    file.close();

    computeShader_ = device_->CreateShader(buffer.data(), codeSize, rhi::ShaderStage::Compute, "blurCS");
    if (computeShader_ == rhi::handles::INVALID_SHADER) {
        std::cerr << "BlurPass: Failed to create compute shader" << std::endl;
        return false;
    }
    
    // 2. Create DescriptorSetLayout
    utl::vector<rhi::DescriptorSetLayoutBinding> bindings;
    // Binding 0: Input Texture (Read)
    {
        rhi::DescriptorSetLayoutBinding b;
        b.binding = 0;
        b.descriptorType = rhi::DescriptorType::SampledImage;
        b.descriptorCount = 1;
        b.stageFlags = rhi::ShaderStage::Compute;
        bindings.push_back(b);
    }
    // Binding 1: Output Texture (Write)
    {
        rhi::DescriptorSetLayoutBinding b;
        b.binding = 1;
        b.descriptorType = rhi::DescriptorType::StorageImage;
        b.descriptorCount = 1;
        b.stageFlags = rhi::ShaderStage::Compute;
        bindings.push_back(b);
    }
    // Binding 2: Params (Uniform Buffer)
    {
        rhi::DescriptorSetLayoutBinding b;
        b.binding = 2;
        b.descriptorType = rhi::DescriptorType::UniformBuffer;
        b.descriptorCount = 1;
        b.stageFlags = rhi::ShaderStage::Compute;
        bindings.push_back(b);
    }
    
    rhi::DescriptorSetLayoutDesc dslDesc;
    dslDesc.bindings = bindings.data();
    dslDesc.bindingCount = (u32)bindings.size();
    
    descriptorSetLayout_ = device_->CreateDescriptorSetLayout(dslDesc);

    if (descriptorSetLayout_ == rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
        return false;
    }
    
    // 3. Create PipelineLayout
    rhi::PipelineLayoutDesc plDesc;
    plDesc.setLayouts = &descriptorSetLayout_;
    plDesc.setLayoutCount = 1;
    pipelineLayout_ = device_->CreatePipelineLayout(plDesc);
    
    // 4. Create Pipeline
    rhi::ComputePipelineDesc pDesc;
    pDesc.computeShader = computeShader_;
    pDesc.layout = pipelineLayout_;
    pDesc.threadGroupSize = {8, 8, 1}; // Matches dispatch logic (width+7)/8
    pipeline_ = device_->CreateComputePipeline(pDesc);

    if (pipeline_ == rhi::handles::INVALID_PIPELINE) {
        return false;
    }
    
    // 5. Create Param Buffers
    rhi::BufferDesc bufferDesc{
        MAX_PARAM_BUFFER_SIZE,
        rhi::BufferType::Constant,
        rhi::GPUMemoryUsage::Dynamic,
        rhi::GPUMemoryUsage::Dynamic,
        0
    };
    
    for (u32 i = 0; i < rhi::MAX_FRAMES_IN_FLIGHT; ++i) {
        paramBuffer_[i] = device_->CreateBuffer(bufferDesc);
        paramBufferMapped_[i] = device_->MapBuffer(paramBuffer_[i]);
        
        // Init Descriptor Pool
        for (u32 j = 0; j < MAX_SETS_PER_FRAME; ++j) {
            rhi::DescriptorSetDesc dsDesc;
            dsDesc.layout = descriptorSetLayout_;
            setPool_[i][j] = device_->CreateDescriptorSet(dsDesc);
            // std::cout << "BlurPass: DescriptorSet created [" << i << "][" << j << "]: " << (u64)setPool_[i][j] << std::endl;
        }
    }
    
    return true;
}

void BlurPass::Shutdown() {
    if (!device_) return;
    
    for (u32 i = 0; i < rhi::MAX_FRAMES_IN_FLIGHT; ++i) {
        if (paramBuffer_[i] != rhi::handles::INVALID_RESOURCE) {
            device_->UnmapBuffer(paramBuffer_[i]);
            device_->DestroyBuffer(paramBuffer_[i]);
        }
        for (u32 j = 0; j < MAX_SETS_PER_FRAME; ++j) {
            if (setPool_[i][j] != rhi::handles::INVALID_RESOURCE) {
                device_->DestroyDescriptorSet(setPool_[i][j]);
            }
        }
    }
    
    if (pipeline_ != rhi::handles::INVALID_PIPELINE) {
        device_->DestroyPipeline(pipeline_);
    }
    if (pipelineLayout_ != rhi::handles::INVALID_PIPELINE_LAYOUT) {
        device_->DestroyPipelineLayout(pipelineLayout_);
    }
    if (descriptorSetLayout_ != rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
        device_->DestroyDescriptorSetLayout(descriptorSetLayout_);
    }
    if (computeShader_ != rhi::handles::INVALID_SHADER) {
        device_->DestroyShader(computeShader_);
    }
    
    device_ = nullptr;
}

rhi::DescriptorSetHandle BlurPass::GetDescriptorSet(u32 frameIndex, 
                                                   rhi::ResourceHandle input, 
                                                   rhi::ResourceHandle output, 
                                                   u32 paramOffset) {
    u32 index = currentSetIndex_[frameIndex]++;
    if (index >= MAX_SETS_PER_FRAME) {
        // Simple wrap around strategy
        index = 0;
        currentSetIndex_[frameIndex] = 1;
    }
    
    rhi::DescriptorSetHandle set = setPool_[frameIndex][index];
    
    // Update Descriptor Set
    utl::vector<rhi::WriteDescriptorSet> writes;
    
    // Input
    rhi::DescriptorImageInfo inputInfo;
    inputInfo.imageView = input;
    inputInfo.imageLayout = rhi::ResourceState::ShaderResource;
    
    rhi::WriteDescriptorSet inputWrite;
    inputWrite.dstSet = set;
    inputWrite.dstBinding = 0;
    inputWrite.descriptorCount = 1;
    inputWrite.descriptorType = rhi::DescriptorType::SampledImage;
    inputWrite.imageInfo = &inputInfo;
    writes.push_back(inputWrite);
    
    // Output
    rhi::DescriptorImageInfo outputInfo;
    outputInfo.imageView = output;
    outputInfo.imageLayout = rhi::ResourceState::UnorderedAccess;
    
    rhi::WriteDescriptorSet outputWrite;
    outputWrite.dstSet = set;
    outputWrite.dstBinding = 1;
    outputWrite.descriptorCount = 1;
    outputWrite.descriptorType = rhi::DescriptorType::StorageImage;
    outputWrite.imageInfo = &outputInfo;
    writes.push_back(outputWrite);
    
    // Params
    rhi::DescriptorBufferInfo bufferInfo;
    bufferInfo.buffer = paramBuffer_[frameIndex];
    bufferInfo.offset = paramOffset;
    bufferInfo.range = sizeof(BlurParams);
    
    rhi::WriteDescriptorSet paramWrite;
    paramWrite.dstSet = set;
    paramWrite.dstBinding = 2;
    paramWrite.descriptorCount = 1;
    paramWrite.descriptorType = rhi::DescriptorType::UniformBuffer;
    paramWrite.bufferInfo = &bufferInfo;
    writes.push_back(paramWrite);
    
    device_->UpdateDescriptorSets((u32)writes.size(), writes.data());
    
    return set;
}

void BlurPass::Execute(rhi::RHICommandBuffer* cmdBuffer,
                       rhi::ResourceHandle input,
                       rhi::ResourceHandle output,
                       rhi::ResourceHandle temp,
                       u32 width, u32 height, u32 layers,
                       u32 frameIndex,
                       int radius, float sigma) {
    
    // Guard: if Initialize failed (invalid pipeline / unmapped params), no-op
    // rather than crash writing an unmapped param buffer.
    if (pipeline_ == rhi::handles::INVALID_PIPELINE ||
        !paramBufferMapped_[frameIndex] || !cmdBuffer) return;

    static u32 lastFrameIndex = -1;
    if (lastFrameIndex != frameIndex) {
        paramBufferOffset_[frameIndex] = 0;
        currentSetIndex_[frameIndex] = 0;
        lastFrameIndex = frameIndex;
    }

    cmdBuffer->BindComputePipeline(pipeline_);
    
    // Barrier: Prepare Temp for writing (ShaderResource -> UnorderedAccess)
    // Assuming Temp was ShaderResource from previous frame or Undefined
    rhi::ResourceBarrier tempBarrier;
    tempBarrier.resource = temp;
    tempBarrier.beforeState = rhi::ResourceState::ShaderResource; // Or Undefined if first use
    tempBarrier.afterState = rhi::ResourceState::UnorderedAccess;
    tempBarrier.subresource = rhi::RHI_ALL_SUBRESOURCES;
    cmdBuffer->InsertBarrier(&tempBarrier, 1);
    
    // Pass 1: Horizontal (Input -> Temp)
    {
        u32 offset = paramBufferOffset_[frameIndex];
        offset = (offset + 255) & ~255; // Alignment
        
        if (offset + sizeof(BlurParams) > MAX_PARAM_BUFFER_SIZE) {
            std::cerr << "BlurPass: Param buffer full!" << std::endl;
            return;
        }
        
        BlurParams* params = (BlurParams*)((u8*)paramBufferMapped_[frameIndex] + offset);
        params->textureWidth = width;
        params->textureHeight = height;
        params->blurRadius = radius;
        params->sigma = sigma;
        params->arrayLayer = 0; 
        params->direction = 0; // Horizontal
        
        paramBufferOffset_[frameIndex] = offset + sizeof(BlurParams);
        
        rhi::DescriptorSetHandle set = GetDescriptorSet(frameIndex, input, temp, offset);
        cmdBuffer->BindDescriptorSets(rhi::PipelineBindPoint::Compute, pipelineLayout_, 0, 1, &set, 0, nullptr);
        
        u32 groupX = (width + 8 - 1) / 8;
        u32 groupY = (height + 8 - 1) / 8;
        cmdBuffer->Dispatch(groupX, groupY, layers);
    }
    
    // Barrier: Wait for Temp write to finish AND Prepare Output for writing
    utl::vector<rhi::ResourceBarrier> barriers;
    
    // Temp: UAV -> SRV
    rhi::ResourceBarrier tempReadBarrier;
    tempReadBarrier.resource = temp;
    tempReadBarrier.beforeState = rhi::ResourceState::UnorderedAccess;
    tempReadBarrier.afterState = rhi::ResourceState::ShaderResource;
    tempReadBarrier.subresource = rhi::RHI_ALL_SUBRESOURCES;
    barriers.push_back(tempReadBarrier);

    // Output: SRV -> UAV
    // Note: Output might be same as Input, which was SRV in Pass 1
    rhi::ResourceBarrier outWriteBarrier;
    outWriteBarrier.resource = output;
    outWriteBarrier.beforeState = rhi::ResourceState::ShaderResource;
    outWriteBarrier.afterState = rhi::ResourceState::UnorderedAccess;
    outWriteBarrier.subresource = rhi::RHI_ALL_SUBRESOURCES;
    barriers.push_back(outWriteBarrier);

    cmdBuffer->InsertBarrier(barriers.data(), (u32)barriers.size());
    
    // Pass 2: Vertical (Temp -> Output)
    {
        u32 offset = paramBufferOffset_[frameIndex];
        offset = (offset + 255) & ~255;
        
        if (offset + sizeof(BlurParams) > MAX_PARAM_BUFFER_SIZE) {
            return;
        }
        
        BlurParams* params = (BlurParams*)((u8*)paramBufferMapped_[frameIndex] + offset);
        params->textureWidth = width;
        params->textureHeight = height;
        params->blurRadius = radius;
        params->sigma = sigma;
        params->arrayLayer = 0;
        params->direction = 1; // Vertical
        
        paramBufferOffset_[frameIndex] = offset + sizeof(BlurParams);
        
        rhi::DescriptorSetHandle set = GetDescriptorSet(frameIndex, temp, output, offset);
        cmdBuffer->BindDescriptorSets(rhi::PipelineBindPoint::Compute, pipelineLayout_, 0, 1, &set, 0, nullptr);
        
        u32 groupX = (width + 8 - 1) / 8;
        u32 groupY = (height + 8 - 1) / 8;
        cmdBuffer->Dispatch(groupX, groupY, layers);
    }
    
    // Barrier: Wait for Output write to finish
    rhi::ResourceBarrier outBarrier;
    outBarrier.resource = output;
    outBarrier.beforeState = rhi::ResourceState::UnorderedAccess;
    outBarrier.afterState = rhi::ResourceState::ShaderResource;
    outBarrier.subresource = rhi::RHI_ALL_SUBRESOURCES;
    cmdBuffer->InsertBarrier(&outBarrier, 1);
}

} // namespace primal::graphics
