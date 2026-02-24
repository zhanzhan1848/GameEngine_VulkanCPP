#include "IBLPrecomputer.h"
#include "../Core/RHIDevice.h"
#include "../Core/RHICommand.h"
#include "../Core/RHIDescriptorSet.h"
#include "../Core/RHIDescriptorSetLayout.h"
#include "../Core/RHIShaderCommon.h"
#include "../Core/RHIResource.h"
#include <fstream>
#include <iostream>
#include <cmath>
#include <vector>
#include <set>

namespace primal::graphics::rhi {

struct IrradianceParams {
    uint32_t faceIndex;
    float padding[3];
};

struct PrefilterParams {
    uint32_t faceIndex;
    float roughness;
    float padding[2];
};

IBLPrecomputer::IBLPrecomputer(RHIDeviceBase* device) : device_(device) {}

IBLPrecomputer::~IBLPrecomputer() {
    Shutdown();
}

void IBLPrecomputer::Shutdown() {
    DestroyPipelines();
}

bool IBLPrecomputer::Initialize() {
    return CreatePipelines();
}

namespace {
    std::string LoadShaderSourceRecursive(const std::string& filename, std::set<std::string>& includedFiles) {
        // Check for include guards/duplicates
        if (includedFiles.find(filename) != includedFiles.end()) {
            return "";
        }
        includedFiles.insert(filename);

        std::vector<std::string> searchPaths = {
            "shaders/",
            "Engine/Graphics/RHI/Shaders/",
            "../Engine/Graphics/RHI/Shaders/",
            "../../Engine/Graphics/RHI/Shaders/",
            "../../../Engine/Graphics/RHI/Shaders/",
            "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/Engine/Graphics/RHI/Shaders/"
        };

        std::string path;
        std::ifstream file;
        bool found = false;

        for (const auto& prefix : searchPaths) {
            path = prefix + filename;
            file.open(path);
            if (file.is_open()) {
                found = true;
                break;
            }
            file.clear();
        }

        if (!found) {
            std::cerr << "[IBLPrecomputer] Failed to open shader file: " << filename << std::endl;
            return "";
        }
        
        std::stringstream buffer;
        std::string line;
        while (std::getline(file, line)) {
            if (line.find("#include") != std::string::npos && line.find("\"") != std::string::npos) {
                size_t start = line.find("\"");
                size_t end = line.rfind("\"");
                if (start != std::string::npos && end != std::string::npos && end > start) {
                    std::string includeFile = line.substr(start + 1, end - start - 1);
                    std::string includedSource = LoadShaderSourceRecursive(includeFile, includedFiles);
                    if (!includedSource.empty()) {
                        buffer << "\n// Included from " << includeFile << "\n";
                        buffer << includedSource << "\n";
                    }
                    // If empty, it's either already included or failed. 
                    // Failures are logged in the recursive call.
                } else {
                    buffer << line << "\n";
                }
            } else {
                // Filter out #pragma once
                if (line.find("#pragma once") == std::string::npos) {
                    buffer << line << "\n";
                }
            }
        }
        return buffer.str();
    }
}

std::string IBLPrecomputer::LoadShaderSource(const std::string& filename) {
    std::set<std::string> includedFiles;
    return LoadShaderSourceRecursive(filename, includedFiles);
}

ShaderHandle IBLPrecomputer::CreateComputeShader(const std::string& filename, const char* entryPoint) {
    std::string source = LoadShaderSource(filename);
    if (source.empty()) return handles::INVALID_SHADER;
    return device_->CreateShader(source.data(), source.size(), ShaderStage::Compute, entryPoint);
}

bool IBLPrecomputer::CreatePipelines() {
    // 1. Create DescriptorSetLayouts
    
    // Irradiance & Prefilter share similar layout
    std::vector<DescriptorSetLayoutBinding> commonBindings(4);
    
    // Binding 0: Input EnvMap
    commonBindings[0].binding = 0;
    commonBindings[0].descriptorType = DescriptorType::SampledImage;
    commonBindings[0].descriptorCount = 1;
    commonBindings[0].stageFlags = ShaderStage::Compute;
    
    // Binding 1: Output View
    commonBindings[1].binding = 1;
    commonBindings[1].descriptorType = DescriptorType::StorageImage;
    commonBindings[1].descriptorCount = 1;
    commonBindings[1].stageFlags = ShaderStage::Compute;
    
    // Binding 2: Sampler
    commonBindings[2].binding = 2;
    commonBindings[2].descriptorType = DescriptorType::CombinedImageSampler;
    commonBindings[2].descriptorCount = 1;
    commonBindings[2].stageFlags = ShaderStage::Compute;
    
    // Binding 3: Params
    commonBindings[3].binding = 3;
    commonBindings[3].descriptorType = DescriptorType::UniformBuffer;
    commonBindings[3].descriptorCount = 1;
    commonBindings[3].stageFlags = ShaderStage::Compute;

    DescriptorSetLayoutDesc commonDesc;
    commonDesc.bindingCount = static_cast<uint32_t>(commonBindings.size());
    commonDesc.bindings = commonBindings.data();

    irradianceDescLayout_ = device_->CreateDescriptorSetLayout(commonDesc);
    prefilterDescLayout_ = device_->CreateDescriptorSetLayout(commonDesc);
    
    // BRDF Layout
    std::vector<DescriptorSetLayoutBinding> brdfBindings(1);
    brdfBindings[0].binding = 0;
    brdfBindings[0].descriptorType = DescriptorType::StorageImage;
    brdfBindings[0].descriptorCount = 1;
    brdfBindings[0].stageFlags = ShaderStage::Compute;
    
    DescriptorSetLayoutDesc brdfDesc;
    brdfDesc.bindingCount = static_cast<uint32_t>(brdfBindings.size());
    brdfDesc.bindings = brdfBindings.data();
    
    brdfDescLayout_ = device_->CreateDescriptorSetLayout(brdfDesc);
    
    if (irradianceDescLayout_ == handles::INVALID_DESCRIPTOR_SET_LAYOUT ||
        prefilterDescLayout_ == handles::INVALID_DESCRIPTOR_SET_LAYOUT ||
        brdfDescLayout_ == handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
        std::cerr << "[IBLPrecomputer] Failed to create descriptor set layouts" << std::endl;
        return false;
    }

    // 2. Create PipelineLayouts
    std::vector<DescriptorSetLayoutHandle> irradianceSetLayouts = {irradianceDescLayout_};
    PipelineLayoutDesc irradiancePipeLayoutDesc;
    irradiancePipeLayoutDesc.setLayoutCount = static_cast<uint32_t>(irradianceSetLayouts.size());
    irradiancePipeLayoutDesc.setLayouts = irradianceSetLayouts.data();
    irradiancePipelineLayout_ = device_->CreatePipelineLayout(irradiancePipeLayoutDesc);

    std::vector<DescriptorSetLayoutHandle> prefilterSetLayouts = {prefilterDescLayout_};
    PipelineLayoutDesc prefilterPipeLayoutDesc;
    prefilterPipeLayoutDesc.setLayoutCount = static_cast<uint32_t>(prefilterSetLayouts.size());
    prefilterPipeLayoutDesc.setLayouts = prefilterSetLayouts.data();
    prefilterPipelineLayout_ = device_->CreatePipelineLayout(prefilterPipeLayoutDesc);

    std::vector<DescriptorSetLayoutHandle> brdfSetLayouts = {brdfDescLayout_};
    PipelineLayoutDesc brdfPipeLayoutDesc;
    brdfPipeLayoutDesc.setLayoutCount = static_cast<uint32_t>(brdfSetLayouts.size());
    brdfPipeLayoutDesc.setLayouts = brdfSetLayouts.data();
    brdfPipelineLayout_ = device_->CreatePipelineLayout(brdfPipeLayoutDesc);

    // 3. Create Shaders
    ShaderHandle irradianceShader = CreateComputeShader("IBL_IrradianceConvolution.metal", "CS_IrradianceConvolution");
    ShaderHandle prefilterShader = CreateComputeShader("IBL_SpecularPrefilter.metal", "CS_SpecularPrefilter");
    ShaderHandle brdfShader = CreateComputeShader("IBL_BRDFIntegration.metal", "CS_BRDFIntegration");

    if (irradianceShader == handles::INVALID_SHADER ||
        prefilterShader == handles::INVALID_SHADER ||
        brdfShader == handles::INVALID_SHADER) {
        std::cerr << "[IBLPrecomputer] Failed to create shaders" << std::endl;
        return false;
    }

    // 4. Create Pipelines
    ComputePipelineDesc pipeDesc;
    pipeDesc.threadGroupSize = {8, 8, 1}; // Standard group size
    
    pipeDesc.computeShader = irradianceShader;
    pipeDesc.layout = irradiancePipelineLayout_;
    irradiancePipeline_ = device_->CreateComputePipeline(pipeDesc);

    pipeDesc.computeShader = prefilterShader;
    pipeDesc.layout = prefilterPipelineLayout_;
    prefilterPipeline_ = device_->CreateComputePipeline(pipeDesc);

    pipeDesc.computeShader = brdfShader;
    pipeDesc.layout = brdfPipelineLayout_;
    brdfPipeline_ = device_->CreateComputePipeline(pipeDesc);

    return (irradiancePipeline_ != handles::INVALID_PIPELINE &&
            prefilterPipeline_ != handles::INVALID_PIPELINE &&
            brdfPipeline_ != handles::INVALID_PIPELINE);
}

void IBLPrecomputer::DestroyPipelines() {
    // In a real implementation, we should destroy resources.
    // Assuming device shutdown handles it for now as per previous context.
}

ResourceHandle IBLPrecomputer::ComputeIrradianceMap(ResourceHandle envMap, uint32_t outputSize) {
    if (irradiancePipeline_ == handles::INVALID_PIPELINE) return handles::INVALID_RESOURCE;

    // 1. Create Output Cubemap
    TextureDesc desc;
    desc.type = TextureType::TextureCube;
    desc.format = DataFormat::RGBA16_Float; 
    desc.size = {outputSize, outputSize, 1};
    desc.arraySize = 1;
    desc.mipLevels = 1;
    desc.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
    desc.memoryUsage = GPUMemoryUsage::Static;
    desc.name = "IBL_IrradianceMap";
    
    ResourceHandle outputTexture = device_->CreateTexture(desc);
    if (outputTexture == handles::INVALID_RESOURCE) return handles::INVALID_RESOURCE;

    // 2. Create Sampler
    SamplerDesc samplerDesc;
    samplerDesc.minFilter = FilterMode::Linear;
    samplerDesc.magFilter = FilterMode::Linear;
    samplerDesc.addressU = TextureAddressMode::Clamp;
    samplerDesc.addressV = TextureAddressMode::Clamp;
    samplerDesc.addressW = TextureAddressMode::Clamp;
    SamplerHandle sampler = device_->CreateSampler(samplerDesc);

    // 3. Create CommandBuffer
    CommandBufferHandle cmdHandle = device_->CreateCommandBuffer(CommandQueueType::Graphics);
    RHICommandBuffer* cmd = GetCommandBuffer(cmdHandle);
    if (!cmd) return handles::INVALID_RESOURCE;

    cmd->Begin();
    cmd->BindComputePipeline(irradiancePipeline_);

    // Keep track of temporary resources to destroy later
    std::vector<ResourceHandle> tempTextures;
    std::vector<ResourceHandle> tempBuffers;
    std::vector<DescriptorSetHandle> tempSets;

    // 4. Process each face
    for (uint32_t face = 0; face < 6; ++face) {
        // Create Face View
        TextureViewDesc viewDesc;
        viewDesc.texture = outputTexture;
        viewDesc.viewType = TextureType::Texture2D;
        viewDesc.format = desc.format;
        viewDesc.mostDetailedMip = 0;
        viewDesc.mipCount = 1;
        viewDesc.firstArraySlice = face;
        viewDesc.arraySize = 1;
        
        ResourceHandle faceView = device_->CreateTextureView(viewDesc);
        tempTextures.push_back(faceView);
        
        // Create Params Buffer
        BufferDesc bufDesc{
            sizeof(IrradianceParams),
            BufferType::Constant,
            GPUMemoryUsage::Dynamic,
            GPUMemoryUsage::Dynamic,
            static_cast<uint32_t>(BufferUsageFlags::Uniform)
        };
        ResourceHandle paramBuffer = device_->CreateBuffer(bufDesc);
        tempBuffers.push_back(paramBuffer);
        
        IrradianceParams params;
        params.faceIndex = face;
        
        void* mapped = device_->MapBuffer(paramBuffer, 0, sizeof(IrradianceParams));
        if (mapped) {
            memcpy(mapped, &params, sizeof(IrradianceParams));
            device_->UnmapBuffer(paramBuffer);
        }

        // Allocate DescriptorSet
        DescriptorSetDesc dsDesc;
        dsDesc.layout = irradianceDescLayout_;
        DescriptorSetHandle ds = device_->CreateDescriptorSet(dsDesc);
        tempSets.push_back(ds);
        
        // Update DescriptorSet
        DescriptorImageInfo envMapInfo;
        envMapInfo.imageView = envMap;
        envMapInfo.imageLayout = ResourceState::ShaderResource;
        envMapInfo.sampler = handles::INVALID_SAMPLER;
        
        WriteDescriptorSet write0;
        write0.dstSet = ds;
        write0.dstBinding = 0;
        write0.descriptorType = DescriptorType::SampledImage;
        write0.descriptorCount = 1;
        write0.imageInfo = &envMapInfo;
        
        DescriptorImageInfo outputInfo;
        outputInfo.imageView = faceView;
        outputInfo.imageLayout = ResourceState::UnorderedAccess;
        outputInfo.sampler = handles::INVALID_SAMPLER;
        
        WriteDescriptorSet write1;
        write1.dstSet = ds;
        write1.dstBinding = 1;
        write1.descriptorType = DescriptorType::StorageImage;
        write1.descriptorCount = 1;
        write1.imageInfo = &outputInfo;
        
        DescriptorImageInfo samplerInfo;
        samplerInfo.sampler = sampler;
        samplerInfo.imageView = handles::INVALID_RESOURCE;
        samplerInfo.imageLayout = ResourceState::Unknown;
        
        WriteDescriptorSet write2;
        write2.dstSet = ds;
        write2.dstBinding = 2;
        write2.descriptorType = DescriptorType::CombinedImageSampler;
        write2.descriptorCount = 1;
        write2.imageInfo = &samplerInfo;
        
        DescriptorBufferInfo bufInfo;
        bufInfo.buffer = paramBuffer;
        bufInfo.offset = 0;
        bufInfo.range = sizeof(IrradianceParams);
        
        WriteDescriptorSet write3;
        write3.dstSet = ds;
        write3.dstBinding = 3;
        write3.descriptorType = DescriptorType::UniformBuffer;
        write3.descriptorCount = 1;
        write3.bufferInfo = &bufInfo;
        
        std::vector<WriteDescriptorSet> writes = {write0, write1, write2, write3};
        device_->UpdateDescriptorSets(writes.size(), writes.data());
        
        cmd->BindDescriptorSets(PipelineBindPoint::Compute, irradiancePipelineLayout_, 0, 1, &ds, 0, nullptr);
        
        uint32_t groups = (outputSize + 7) / 8;
        cmd->Dispatch(groups, groups, 1);
    }
    
    cmd->End();
    
    QueueSubmitInfo submitInfo;
    submitInfo.cmdBuffer = cmdHandle;
    
    device_->Submit(submitInfo);
    device_->WaitIdle(); // Wait for completion
    
    // Cleanup Temp Resources
    for (auto h : tempSets) device_->DestroyDescriptorSet(h);
    for (auto h : tempTextures) device_->DestroyTexture(h); // TextureView is destroyed via DestroyTexture
    for (auto h : tempBuffers) device_->DestroyBuffer(h);
    device_->DestroySampler(sampler);
    device_->DestroyCommandBuffer(cmdHandle);

    return outputTexture;
}

ResourceHandle IBLPrecomputer::ComputePrefilteredEnvironmentMap(ResourceHandle envMap, uint32_t outputSize) {
    if (prefilterPipeline_ == handles::INVALID_PIPELINE) return handles::INVALID_RESOURCE;

    uint32_t mipLevels = static_cast<uint32_t>(std::floor(std::log2(outputSize))) + 1;

    TextureDesc desc;
    desc.type = TextureType::TextureCube;
    desc.format = DataFormat::RGBA16_Float;
    desc.size = {outputSize, outputSize, 1};
    desc.arraySize = 1;
    desc.mipLevels = mipLevels;
    desc.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
    desc.memoryUsage = GPUMemoryUsage::Static;
    desc.name = "IBL_PrefilterMap";
    
    ResourceHandle outputTexture = device_->CreateTexture(desc);
    if (outputTexture == handles::INVALID_RESOURCE) return handles::INVALID_RESOURCE;

    SamplerDesc samplerDesc;
    samplerDesc.minFilter = FilterMode::Linear;
    samplerDesc.magFilter = FilterMode::Linear;
    samplerDesc.mipFilter = FilterMode::Linear;
    samplerDesc.addressU = TextureAddressMode::Clamp;
    samplerDesc.addressV = TextureAddressMode::Clamp;
    samplerDesc.addressW = TextureAddressMode::Clamp;
    SamplerHandle sampler = device_->CreateSampler(samplerDesc);

    CommandBufferHandle cmdHandle = device_->CreateCommandBuffer(CommandQueueType::Graphics);
    RHICommandBuffer* cmd = GetCommandBuffer(cmdHandle);
    if (!cmd) return handles::INVALID_RESOURCE;

    cmd->Begin();
    cmd->BindComputePipeline(prefilterPipeline_);
    
    std::vector<ResourceHandle> tempTextures;
    std::vector<ResourceHandle> tempBuffers;
    std::vector<DescriptorSetHandle> tempSets;

    for (uint32_t mip = 0; mip < mipLevels; ++mip) {
        uint32_t mipSize = outputSize >> mip;
        if (mipSize == 0) mipSize = 1;
        
        float roughness = (float)mip / (float)(mipLevels - 1);
        
        for (uint32_t face = 0; face < 6; ++face) {
            TextureViewDesc viewDesc;
            viewDesc.texture = outputTexture;
            viewDesc.viewType = TextureType::Texture2D;
            viewDesc.format = desc.format;
            viewDesc.mostDetailedMip = mip;
            viewDesc.mipCount = 1;
            viewDesc.firstArraySlice = face;
            viewDesc.arraySize = 1;
            ResourceHandle faceView = device_->CreateTextureView(viewDesc);
            tempTextures.push_back(faceView);
            
            BufferDesc bufDesc{
                sizeof(PrefilterParams),
                BufferType::Constant,
                GPUMemoryUsage::Dynamic,
                GPUMemoryUsage::Dynamic,
                static_cast<uint32_t>(BufferUsageFlags::Uniform)
            };
            ResourceHandle paramBuffer = device_->CreateBuffer(bufDesc);
            tempBuffers.push_back(paramBuffer);
            
            PrefilterParams params;
            params.faceIndex = face;
            params.roughness = roughness;
            
            void* mapped = device_->MapBuffer(paramBuffer, 0, sizeof(PrefilterParams));
            if (mapped) {
                memcpy(mapped, &params, sizeof(PrefilterParams));
                device_->UnmapBuffer(paramBuffer);
            }

            DescriptorSetDesc dsDesc;
            dsDesc.layout = prefilterDescLayout_;
            DescriptorSetHandle ds = device_->CreateDescriptorSet(dsDesc);
            tempSets.push_back(ds);

            DescriptorImageInfo envMapInfo;
            envMapInfo.imageView = envMap;
            envMapInfo.imageLayout = ResourceState::ShaderResource;
            envMapInfo.sampler = handles::INVALID_SAMPLER;
            
            DescriptorImageInfo outputInfo;
            outputInfo.imageView = faceView;
            outputInfo.imageLayout = ResourceState::UnorderedAccess;
            outputInfo.sampler = handles::INVALID_SAMPLER;
            
            DescriptorImageInfo samplerInfo;
            samplerInfo.sampler = sampler;
            samplerInfo.imageView = handles::INVALID_RESOURCE;
            
            DescriptorBufferInfo bufInfo;
            bufInfo.buffer = paramBuffer;
            bufInfo.offset = 0;
            bufInfo.range = sizeof(PrefilterParams);
            
            WriteDescriptorSet write0;
            write0.dstSet = ds;
            write0.dstBinding = 0;
            write0.descriptorType = DescriptorType::SampledImage;
            write0.descriptorCount = 1;
            write0.imageInfo = &envMapInfo;
            
            WriteDescriptorSet write1;
            write1.dstSet = ds;
            write1.dstBinding = 1;
            write1.descriptorType = DescriptorType::StorageImage;
            write1.descriptorCount = 1;
            write1.imageInfo = &outputInfo;
            
            WriteDescriptorSet write2;
            write2.dstSet = ds;
            write2.dstBinding = 2;
            write2.descriptorType = DescriptorType::CombinedImageSampler;
            write2.descriptorCount = 1;
            write2.imageInfo = &samplerInfo;
            
            WriteDescriptorSet write3;
            write3.dstSet = ds;
            write3.dstBinding = 3;
            write3.descriptorType = DescriptorType::UniformBuffer;
            write3.descriptorCount = 1;
            write3.bufferInfo = &bufInfo;
            
            std::vector<WriteDescriptorSet> writes = {write0, write1, write2, write3};
            device_->UpdateDescriptorSets(writes.size(), writes.data());
            
            cmd->BindDescriptorSets(PipelineBindPoint::Compute, prefilterPipelineLayout_, 0, 1, &ds, 0, nullptr);
            
            uint32_t groups = (mipSize + 7) / 8;
            cmd->Dispatch(groups, groups, 1);
        }
    }

    cmd->End();
    
    QueueSubmitInfo submitInfo;
    submitInfo.cmdBuffer = cmdHandle;
    
    device_->Submit(submitInfo);
    device_->WaitIdle();
    
    // Cleanup temp resources
    for (auto h : tempSets) device_->DestroyDescriptorSet(h);
    for (auto h : tempTextures) device_->DestroyTexture(h);
    for (auto h : tempBuffers) device_->DestroyBuffer(h);
    device_->DestroySampler(sampler);
    device_->DestroyCommandBuffer(cmdHandle);
    
    return outputTexture;
}

ResourceHandle IBLPrecomputer::ComputeBRDFIntegrationMap(uint32_t outputSize) {
    if (brdfPipeline_ == handles::INVALID_PIPELINE) return handles::INVALID_RESOURCE;

    TextureDesc desc;
    desc.type = TextureType::Texture2D;
    desc.format = DataFormat::RG16_Float;
    desc.size = {outputSize, outputSize, 1};
    desc.arraySize = 1;
    desc.mipLevels = 1;
    desc.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
    desc.memoryUsage = GPUMemoryUsage::Static;
    desc.name = "IBL_BRDF_LUT";
    
    ResourceHandle outputTexture = device_->CreateTexture(desc);
    if (outputTexture == handles::INVALID_RESOURCE) return handles::INVALID_RESOURCE;

    CommandBufferHandle cmdHandle = device_->CreateCommandBuffer(CommandQueueType::Graphics);
    RHICommandBuffer* cmd = GetCommandBuffer(cmdHandle);
    if (!cmd) return handles::INVALID_RESOURCE;

    cmd->Begin();
    cmd->BindComputePipeline(brdfPipeline_);

    DescriptorSetDesc dsDesc;
    dsDesc.layout = brdfDescLayout_;
    DescriptorSetHandle ds = device_->CreateDescriptorSet(dsDesc);
    
    DescriptorImageInfo outputInfo;
    outputInfo.imageView = outputTexture;
    outputInfo.imageLayout = ResourceState::UnorderedAccess;
    outputInfo.sampler = handles::INVALID_SAMPLER;
    
    WriteDescriptorSet write0;
    write0.dstSet = ds;
    write0.dstBinding = 0;
    write0.descriptorType = DescriptorType::StorageImage;
    write0.descriptorCount = 1;
    write0.imageInfo = &outputInfo;
    
    device_->UpdateDescriptorSets(1, &write0);
    
    cmd->BindDescriptorSets(PipelineBindPoint::Compute, brdfPipelineLayout_, 0, 1, &ds, 0, nullptr);
    
    uint32_t groups = (outputSize + 7) / 8;
    cmd->Dispatch(groups, groups, 1);
    
    cmd->End();
    
    QueueSubmitInfo submitInfo;
    submitInfo.cmdBuffer = cmdHandle;
    
    device_->Submit(submitInfo);
    device_->WaitIdle();
    
    device_->DestroyDescriptorSet(ds);
    device_->DestroyCommandBuffer(cmdHandle);
    
    return outputTexture;
}

}
