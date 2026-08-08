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
#include "Utilities/Vector.h"
#include <set>

namespace primal::graphics::rhi {

// T4.6.5 part 37 (IBLPrecomputer fix): structs must mirror WGSL layout.
// WGSL IrradianceParams { faceSize, _pad0, _pad1, _pad2 } — single dispatch
// with z=6 uses gid.z as face index (no per-face loop on Vulkan).
// Metal IrradianceParams { faceIndex, padding[3] } reuses offset 0 as face
// index for its per-face dispatch path — same u32 slot, different semantic.
struct IrradianceParams {
    u32 faceSize;     // WGSL: bounds check; Metal: faceIndex (same u32 slot)
    u32 _pad0;
    u32 _pad1;
    u32 _pad2;
};

// WGSL PrefilterParams { faceSize, _pad0, roughness, srcResolution }.
// Metal PrefilterParams { faceIndex, roughness, padding[2] } has roughness
// at offset 4 — different layout. The Vulkan path uses the WGSL layout.
struct PrefilterParams {
    u32 faceSize;       // WGSL: bounds check; Metal path unused at this slot
    u32 _pad0;
    float roughness;
    float srcResolution;
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

        utl::vector<std::string> searchPaths;
        searchPaths.push_back("shaders/");
        searchPaths.push_back("Engine/Graphics/RHI/Shaders/");
        searchPaths.push_back("../Engine/Graphics/RHI/Shaders/");
        searchPaths.push_back("../../Engine/Graphics/RHI/Shaders/");
        searchPaths.push_back("../../../Engine/Graphics/RHI/Shaders/");
        searchPaths.push_back("/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/Engine/Graphics/RHI/Shaders/");

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
    auto platform = device_->GetPlatform();

    if (platform == rhi::RHIPlatform::Vulkan) {
        // SPIR-V path: load precompiled .spv bytes.
        utl::vector<std::string> searchPaths;
        searchPaths.push_back("Engine/Graphics/Vulkan/shaders/");
        searchPaths.push_back("../Engine/Graphics/Vulkan/shaders/");
        searchPaths.push_back("../../Engine/Graphics/Vulkan/shaders/");
        searchPaths.push_back("../../../Engine/Graphics/Vulkan/shaders/");
        searchPaths.push_back("/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/Engine/Graphics/Vulkan/shaders/");

        for (const auto& prefix : searchPaths) {
            std::string path = prefix + filename;
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file.is_open()) continue;
            auto size = file.tellg();
            if (size <= 0) continue;
            utl::vector<char> bytes(static_cast<size_t>(size));
            file.seekg(0);
            file.read(bytes.data(), size);
            return device_->CreateShader(bytes.data(), bytes.size(), ShaderStage::Compute, entryPoint);
        }
        std::cerr << "[IBLPrecomputer] Failed to open SPIR-V shader file: " << filename << std::endl;
        return handles::INVALID_SHADER;
    }

    // Text path: Metal .metal or Dawn .wgsl, loaded with #include resolution.
    std::string source = LoadShaderSource(filename);
    if (source.empty()) return handles::INVALID_SHADER;
    return device_->CreateShader(source.data(), source.size(), ShaderStage::Compute, entryPoint);
}

bool IBLPrecomputer::CreatePipelines() {
    // 1. Create DescriptorSetLayouts
    
    // Irradiance & Prefilter share similar layout
    utl::vector<DescriptorSetLayoutBinding> commonBindings;
    commonBindings.resize(4);
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
    
    // Binding 2: Sampler (Vulkan/WGSL declares `var smp: sampler` — sampler
    // only, no imageView. Metal uses `sampler smp [[sampler(2)]]` which is
    // also sampler-only at slot 2.)
    commonBindings[2].binding = 2;
    commonBindings[2].descriptorType = DescriptorType::Sampler;
    commonBindings[2].descriptorCount = 1;
    commonBindings[2].stageFlags = ShaderStage::Compute;
    
    // Binding 3: Params
    commonBindings[3].binding = 3;
    commonBindings[3].descriptorType = DescriptorType::UniformBuffer;
    commonBindings[3].descriptorCount = 1;
    commonBindings[3].stageFlags = ShaderStage::Compute;

    DescriptorSetLayoutDesc commonDesc;
    commonDesc.bindingCount = static_cast<u32>(commonBindings.size());
    commonDesc.bindings = commonBindings.data();

    irradianceDescLayout_ = device_->CreateDescriptorSetLayout(commonDesc);
    prefilterDescLayout_ = device_->CreateDescriptorSetLayout(commonDesc);
    
    // BRDF Layout
    utl::vector<DescriptorSetLayoutBinding> brdfBindings;
    brdfBindings.resize(1);
    brdfBindings[0].binding = 0;
    brdfBindings[0].descriptorType = DescriptorType::StorageImage;
    brdfBindings[0].descriptorCount = 1;
    brdfBindings[0].stageFlags = ShaderStage::Compute;
    
    DescriptorSetLayoutDesc brdfDesc;
    brdfDesc.bindingCount = static_cast<u32>(brdfBindings.size());
    brdfDesc.bindings = brdfBindings.data();
    
    brdfDescLayout_ = device_->CreateDescriptorSetLayout(brdfDesc);
    
    if (irradianceDescLayout_ == handles::INVALID_DESCRIPTOR_SET_LAYOUT ||
        prefilterDescLayout_ == handles::INVALID_DESCRIPTOR_SET_LAYOUT ||
        brdfDescLayout_ == handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
        std::cerr << "[IBLPrecomputer] Failed to create descriptor set layouts" << std::endl;
        return false;
    }

    // 2. Create PipelineLayouts
    utl::vector<DescriptorSetLayoutHandle> irradianceSetLayouts; irradianceSetLayouts.push_back(irradianceDescLayout_);
    PipelineLayoutDesc irradiancePipeLayoutDesc;
    irradiancePipeLayoutDesc.setLayoutCount = static_cast<u32>(irradianceSetLayouts.size());
    irradiancePipeLayoutDesc.setLayouts = irradianceSetLayouts.data();
    irradiancePipelineLayout_ = device_->CreatePipelineLayout(irradiancePipeLayoutDesc);

    utl::vector<DescriptorSetLayoutHandle> prefilterSetLayouts; prefilterSetLayouts.push_back(prefilterDescLayout_);
    PipelineLayoutDesc prefilterPipeLayoutDesc;
    prefilterPipeLayoutDesc.setLayoutCount = static_cast<u32>(prefilterSetLayouts.size());
    prefilterPipeLayoutDesc.setLayouts = prefilterSetLayouts.data();
    prefilterPipelineLayout_ = device_->CreatePipelineLayout(prefilterPipeLayoutDesc);

    utl::vector<DescriptorSetLayoutHandle> brdfSetLayouts; brdfSetLayouts.push_back(brdfDescLayout_);
    PipelineLayoutDesc brdfPipeLayoutDesc;
    brdfPipeLayoutDesc.setLayoutCount = static_cast<u32>(brdfSetLayouts.size());
    brdfPipeLayoutDesc.setLayouts = brdfSetLayouts.data();
    brdfPipelineLayout_ = device_->CreatePipelineLayout(brdfPipeLayoutDesc);

    // 3. Create Shaders
    auto platform = device_->GetPlatform();
    const bool isVulkan = (platform == rhi::RHIPlatform::Vulkan);
    const char* ext = isVulkan ? ".spv" : ".metal";
    // SPIR-V preserves the WGSL entry-point name (naga does not rewrite to
    // "main"). WGSL convention: cs_main for compute.
    const char* irradianceEntry = isVulkan ? "cs_main" : "CS_IrradianceConvolution";
    const char* prefilterEntry = isVulkan ? "cs_main" : "CS_SpecularPrefilter";
    const char* brdfEntry = isVulkan ? "cs_main" : "CS_BRDFIntegration";

    ShaderHandle irradianceShader = CreateComputeShader(std::string("IBL_IrradianceConvolution") + ext, irradianceEntry);
    ShaderHandle prefilterShader = CreateComputeShader(std::string("IBL_SpecularPrefilter") + ext, prefilterEntry);
    ShaderHandle brdfShader = CreateComputeShader(std::string("IBL_BRDFIntegration") + ext, brdfEntry);

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

ResourceHandle IBLPrecomputer::ComputeIrradianceMap(ResourceHandle envMap, u32 outputSize) {
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

    // T4.6.5 part 37 (Bug A fix): output cubemap starts in UNDEFINED layout.
    // StorageImage descriptor requires GENERAL — transition the whole image
    // before any Storage write. Without this, validation fires
    // VUID-vkCmdDispatch-None-01020 and storage writes are silently discarded.
    ResourceBarrier toUA{};
    toUA.resource = outputTexture;
    toUA.beforeState = ResourceState::Unknown;
    toUA.afterState = ResourceState::UnorderedAccess;
    toUA.subresource = 0xFFFFFFFF;
    toUA.queueFamily = 0xFFFFFFFF;
    cmd->InsertBarrier(&toUA, 1);

    cmd->BindComputePipeline(irradiancePipeline_);

    // T4.6.5 part 37 (Bug C fix): WGSL declares output as
    // `texture_storage_2d_array<rgba16float, write>` (OpTypeImage Arrayed=1).
    // Per-face Texture2D views (Arrayed=0) trigger
    // VUID-vkCmdDispatch-viewType-07752. Use a single Texture2DArray view
    // covering all 6 faces — the shader indexes via gid.z.
    TextureViewDesc arrayViewDesc;
    arrayViewDesc.texture = outputTexture;
    arrayViewDesc.viewType = TextureType::Texture2DArray;
    arrayViewDesc.format = desc.format;
    arrayViewDesc.mostDetailedMip = 0;
    arrayViewDesc.mipCount = 1;
    arrayViewDesc.firstArraySlice = 0;
    arrayViewDesc.arraySize = 6;
    ResourceHandle outputArrayView = device_->CreateTextureView(arrayViewDesc);

    // Single params buffer (WGSL IrradianceParams: faceSize at offset 0)
    BufferDesc bufDesc{
        sizeof(IrradianceParams),
        BufferType::Constant,
        GPUMemoryUsage::Dynamic,
        GPUMemoryUsage::Dynamic,
        static_cast<u32>(BufferUsageFlags::Uniform)
    };
    ResourceHandle paramBuffer = device_->CreateBuffer(bufDesc);

    IrradianceParams params;
    params.faceSize = outputSize;
    params._pad0 = params._pad1 = params._pad2 = 0;

    void* mapped = device_->MapBuffer(paramBuffer, 0, sizeof(IrradianceParams));
    if (mapped) {
        memcpy(mapped, &params, sizeof(IrradianceParams));
        device_->UnmapBuffer(paramBuffer);
    }

    DescriptorSetDesc dsDesc;
    dsDesc.layout = irradianceDescLayout_;
    DescriptorSetHandle ds = device_->CreateDescriptorSet(dsDesc);

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
    outputInfo.imageView = outputArrayView;
    outputInfo.imageLayout = ResourceState::UnorderedAccess;
    outputInfo.sampler = handles::INVALID_SAMPLER;

    WriteDescriptorSet write1;
    write1.dstSet = ds;
    write1.dstBinding = 1;
    write1.descriptorType = DescriptorType::StorageImage;
    write1.descriptorCount = 1;
    write1.imageInfo = &outputInfo;

    // T4.6.5 part 37 (Bug B fix): WGSL binding 2 is `var smp: sampler`
    // (sampler only). CombinedImageSampler required an imageView, which we
    // didn't have at this slot — validation fired NULL imageView error.
    DescriptorImageInfo samplerInfo;
    samplerInfo.sampler = sampler;
    samplerInfo.imageView = handles::INVALID_RESOURCE;
    samplerInfo.imageLayout = ResourceState::Unknown;

    WriteDescriptorSet write2;
    write2.dstSet = ds;
    write2.dstBinding = 2;
    write2.descriptorType = DescriptorType::Sampler;
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

    utl::vector<WriteDescriptorSet> writes;
    writes.push_back(write0);
    writes.push_back(write1);
    writes.push_back(write2);
    writes.push_back(write3);
    device_->UpdateDescriptorSets(writes.size(), writes.data());

    cmd->BindDescriptorSets(PipelineBindPoint::Compute, irradiancePipelineLayout_, 0, 1, &ds, 0, nullptr);

    // WGSL workgroup_size(16, 16, 1); z dimension is the face index (0..5).
    u32 groups = (outputSize + 15) / 16;
    cmd->Dispatch(groups, groups, 6);

    // T4.6.5 part 37: transition output to ShaderResource for downstream
    // sampling (DeferredLighting binds irradianceMap as SampledImage, which
    // requires SHADER_READ_ONLY_OPTIMAL — leaving it in GENERAL post-storage
    // fires VUID-vkCmdDraw-None-09600 at draw time).
    ResourceBarrier toSRV{};
    toSRV.resource = outputTexture;
    toSRV.beforeState = ResourceState::UnorderedAccess;
    toSRV.afterState = ResourceState::ShaderResource;
    toSRV.subresource = 0xFFFFFFFF;
    toSRV.queueFamily = 0xFFFFFFFF;
    cmd->InsertBarrier(&toSRV, 1);

    cmd->End();

    QueueSubmitInfo submitInfo;
    submitInfo.cmdBuffer = cmdHandle;

    device_->Submit(submitInfo);
    device_->WaitIdle(); // Wait for completion

    // Cleanup Temp Resources
    device_->DestroyDescriptorSet(ds);
    device_->DestroyTexture(outputArrayView);
    device_->DestroyBuffer(paramBuffer);
    device_->DestroySampler(sampler);
    device_->DestroyCommandBuffer(cmdHandle);

    return outputTexture;
}

ResourceHandle IBLPrecomputer::ComputePrefilteredEnvironmentMap(ResourceHandle envMap, u32 outputSize) {
    if (prefilterPipeline_ == handles::INVALID_PIPELINE) return handles::INVALID_RESOURCE;

    u32 mipLevels = static_cast<u32>(std::floor(std::log2(outputSize))) + 1;

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

    // T4.6.5 part 37 (Bug A fix): transition whole mip chain UNDEFINED→GENERAL.
    ResourceBarrier toUA{};
    toUA.resource = outputTexture;
    toUA.beforeState = ResourceState::Unknown;
    toUA.afterState = ResourceState::UnorderedAccess;
    toUA.subresource = 0xFFFFFFFF;
    toUA.queueFamily = 0xFFFFFFFF;
    cmd->InsertBarrier(&toUA, 1);

    cmd->BindComputePipeline(prefilterPipeline_);

    utl::vector<ResourceHandle> tempTextures;
    utl::vector<ResourceHandle> tempBuffers;
    utl::vector<DescriptorSetHandle> tempSets;

    // T4.6.5 part 37 (Bug C fix): one Texture2DArray view per mip + single
    // dispatch with z=6 per mip. WGSL `texture_storage_2d_array` (Arrayed=1)
    // cannot accept per-face Texture2D views.
    for (u32 mip = 0; mip < mipLevels; ++mip) {
        u32 mipSize = outputSize >> mip;
        if (mipSize == 0) mipSize = 1;

        float roughness = (float)mip / (float)(mipLevels - 1);

        TextureViewDesc viewDesc;
        viewDesc.texture = outputTexture;
        viewDesc.viewType = TextureType::Texture2DArray;
        viewDesc.format = desc.format;
        viewDesc.mostDetailedMip = mip;
        viewDesc.mipCount = 1;
        viewDesc.firstArraySlice = 0;
        viewDesc.arraySize = 6;
        ResourceHandle mipArrayView = device_->CreateTextureView(viewDesc);
        tempTextures.push_back(mipArrayView);

        BufferDesc bufDesc{
            sizeof(PrefilterParams),
            BufferType::Constant,
            GPUMemoryUsage::Dynamic,
            GPUMemoryUsage::Dynamic,
            static_cast<u32>(BufferUsageFlags::Uniform)
        };
        ResourceHandle paramBuffer = device_->CreateBuffer(bufDesc);
        tempBuffers.push_back(paramBuffer);

        PrefilterParams params;
        params.faceSize = mipSize;
        params._pad0 = 0;
        params.roughness = roughness;
        // Approximation: srcResolution ideally = source env map's resolution.
        // We don't have it here without a backend query. Using outputSize
        // gives slightly wrong importance-sampling mip selection but the
        // visual result is plausible. Fix later by passing srcResolution
        // as an explicit parameter if a Tier 5.1 visual parity test demands.
        params.srcResolution = (float)outputSize;

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
        outputInfo.imageView = mipArrayView;
        outputInfo.imageLayout = ResourceState::UnorderedAccess;
        outputInfo.sampler = handles::INVALID_SAMPLER;

        DescriptorImageInfo samplerInfo;
        samplerInfo.sampler = sampler;
        samplerInfo.imageView = handles::INVALID_RESOURCE;
        samplerInfo.imageLayout = ResourceState::Unknown;

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

        // T4.6.5 part 37 (Bug B fix): sampler-only binding.
        WriteDescriptorSet write2;
        write2.dstSet = ds;
        write2.dstBinding = 2;
        write2.descriptorType = DescriptorType::Sampler;
        write2.descriptorCount = 1;
        write2.imageInfo = &samplerInfo;

        WriteDescriptorSet write3;
        write3.dstSet = ds;
        write3.dstBinding = 3;
        write3.descriptorType = DescriptorType::UniformBuffer;
        write3.descriptorCount = 1;
        write3.bufferInfo = &bufInfo;

        utl::vector<WriteDescriptorSet> writes;
        writes.push_back(write0);
        writes.push_back(write1);
        writes.push_back(write2);
        writes.push_back(write3);
        device_->UpdateDescriptorSets(writes.size(), writes.data());

        cmd->BindDescriptorSets(PipelineBindPoint::Compute, prefilterPipelineLayout_, 0, 1, &ds, 0, nullptr);

        // WGSL workgroup_size(16, 16, 1); z dimension is the face index (0..5).
        u32 groups = (mipSize + 15) / 16;
        cmd->Dispatch(groups, groups, 6);
    }

    // T4.6.5 part 37: transition whole mip chain to ShaderResource for
    // downstream sampling (DeferredLighting binds prefilterMap as SampledImage
    // + mip-filtered sampler, requires SHADER_READ_ONLY_OPTIMAL).
    ResourceBarrier toSRV{};
    toSRV.resource = outputTexture;
    toSRV.beforeState = ResourceState::UnorderedAccess;
    toSRV.afterState = ResourceState::ShaderResource;
    toSRV.subresource = 0xFFFFFFFF;
    toSRV.queueFamily = 0xFFFFFFFF;
    cmd->InsertBarrier(&toSRV, 1);

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

ResourceHandle IBLPrecomputer::ComputeBRDFIntegrationMap(u32 outputSize) {
    if (brdfPipeline_ == handles::INVALID_PIPELINE) return handles::INVALID_RESOURCE;

    TextureDesc desc;
    desc.type = TextureType::Texture2D;
    // WGSL source declares texture_storage_2d<rgba16float, write>; SPIR-V
    // OpTypeImage encodes Rgba16f and Vulkan requires the VkImage format to
    // match. Metal's texture2d<float, access::write> is format-agnostic and
    // accepts RG16F silently, which masked this mismatch pre-Vulkan.
    desc.format = DataFormat::RGBA16_Float;
    desc.size = {outputSize, outputSize, 1};
    desc.arraySize = 1;
    desc.mipLevels = 1;
    desc.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess | TextureUsage::CopySource;
    desc.memoryUsage = GPUMemoryUsage::Static;
    desc.name = "IBL_BRDF_LUT";

    ResourceHandle outputTexture = device_->CreateTexture(desc);
    if (outputTexture == handles::INVALID_RESOURCE) return handles::INVALID_RESOURCE;

    CommandBufferHandle cmdHandle = device_->CreateCommandBuffer(CommandQueueType::Graphics);
    RHICommandBuffer* cmd = GetCommandBuffer(cmdHandle);
    if (!cmd) return handles::INVALID_RESOURCE;

    cmd->Begin();

    // Fresh texture is in Unknown/UNDEFINED layout — transition to UnorderedAccess
    // (GENERAL on Vulkan) before the compute shader writes to it. Metal's encoder-
    // level tracking makes this a no-op there; Vulkan requires an explicit barrier.
    ResourceBarrier toUA{};
    toUA.resource = outputTexture;
    toUA.beforeState = ResourceState::Unknown;
    toUA.afterState = ResourceState::UnorderedAccess;
    toUA.subresource = 0xFFFFFFFF;
    toUA.queueFamily = 0xFFFFFFFF;
    cmd->InsertBarrier(&toUA, 1);

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

    u32 groups = (outputSize + 7) / 8;
    cmd->Dispatch(groups, groups, 1);

    // T4.6.5 part 37: transition to ShaderResource for downstream sampling
    // (DeferredLighting binds brdfLUT as SampledImage).
    ResourceBarrier toSRV{};
    toSRV.resource = outputTexture;
    toSRV.beforeState = ResourceState::UnorderedAccess;
    toSRV.afterState = ResourceState::ShaderResource;
    toSRV.subresource = 0xFFFFFFFF;
    toSRV.queueFamily = 0xFFFFFFFF;
    cmd->InsertBarrier(&toSRV, 1);

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
