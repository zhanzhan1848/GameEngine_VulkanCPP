#include "HZBPass.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/Utils/ShaderRegistry.h"
#include "Graphics/Dawn/ShaderLoader.h"
#include <fstream>
#include <sstream>
#include <cmath>
#include <iostream>

namespace primal::graphics::PostProcess {

using namespace rhi;

static constexpr u32 MAX_FRAMES = 3;
static constexpr u32 MAX_SETS_PER_FRAME = 4;

static PipelineHandle s_CopyPipeline = handles::INVALID_PIPELINE;
static PipelineHandle s_MipPipeline = handles::INVALID_PIPELINE;
static PipelineLayoutHandle s_CopyLayout = handles::INVALID_PIPELINE_LAYOUT;
static PipelineLayoutHandle s_MipLayout = handles::INVALID_PIPELINE_LAYOUT;
static DescriptorSetLayoutHandle s_CopyDSL = handles::INVALID_RESOURCE;
static DescriptorSetLayoutHandle s_MipDSL = handles::INVALID_RESOURCE;
static DescriptorSetHandle s_CopySetPool[MAX_FRAMES][MAX_SETS_PER_FRAME] = {};
static DescriptorSetHandle s_MipSetPool[MAX_FRAMES][MAX_SETS_PER_FRAME] = {};
static u32 s_CopySetIndex[MAX_FRAMES] = {};
static u32 s_MipSetIndex[MAX_FRAMES] = {};
static ResourceHandle s_CopyParamsBuf[MAX_FRAMES] = {};
static void* s_CopyParamsMapped[MAX_FRAMES] = {};
static ResourceHandle s_MipParamsBuf[MAX_FRAMES] = {};
static void* s_MipParamsMapped[MAX_FRAMES] = {};

// Persistent HZB texture + per-mip views
static ResourceHandle s_HZBTexture = handles::INVALID_RESOURCE;
static utl::vector<ResourceHandle> s_MipWriteViews;   // storage views per mip level
static utl::vector<ResourceHandle> s_MipReadViews;    // sampled views per mip level

static std::string LoadShaderSource(const std::string& path) {
#ifdef __EMSCRIPTEN__
    auto lastSlash = path.find_last_of('/');
    auto lastDot = path.find_last_of('.');
    if (lastSlash != std::string::npos && lastDot != std::string::npos && lastDot > lastSlash) {
        std::string name = path.substr(lastSlash + 1, lastDot - lastSlash - 1);
        return dawn::LoadWGSL(name);
    }
    return "";
#else
    std::ifstream file(path);
    if (!file.is_open()) return "";
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
#endif
}

static bool EnsurePipelines(RHIDeviceBase& device) {
    if (s_CopyPipeline != handles::INVALID_PIPELINE) return true;

    auto platform = device.GetPlatform();
    std::string shaderPath = utils::ShaderRegistry::GetShaderPath(platform, "HZBGeneration");
    std::string source = LoadShaderSource(shaderPath);
    if (source.empty()) {
        std::cerr << "[HZB] Shader source empty: " << shaderPath << std::endl;
        return false;
    }

    ShaderHandle copyCS = device.CreateShader(source.data(), source.size(), ShaderStage::Compute, "copy_depth_to_hzb_mip0");
    ShaderHandle mipCS = device.CreateShader(source.data(), source.size(), ShaderStage::Compute, "generate_hzb_mip_level");

    if (copyCS == handles::INVALID_SHADER || mipCS == handles::INVALID_SHADER) {
        std::cerr << "[HZB] Shader creation failed" << std::endl;
        return false;
    }

    // Copy DSL: depth + hzb output + uniform
    {
        DescriptorSetLayoutBinding bindings[3]{};
        bindings[0] = {0, DescriptorType::SampledDepthImage, 1, ShaderStage::Compute};
        bindings[1] = {1, DescriptorType::StorageImage, 1, ShaderStage::Compute, nullptr, DescriptorBindingFlags::None, DataFormat::RGBA16_Float};
        bindings[2] = {2, DescriptorType::UniformBuffer, 1, ShaderStage::Compute};

        DescriptorSetLayoutDesc dslDesc;
        dslDesc.bindingCount = 3;
        dslDesc.bindings = bindings;
        s_CopyDSL = device.CreateDescriptorSetLayout(dslDesc);

        PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &s_CopyDSL;
        s_CopyLayout = device.CreatePipelineLayout(plDesc);

        ComputePipelineDesc pipeDesc;
        pipeDesc.computeShader = copyCS;
        pipeDesc.layout = s_CopyLayout;
        pipeDesc.threadGroupSize = {8, 8, 1};
        s_CopyPipeline = device.CreateComputePipeline(pipeDesc);
    }

    // Mip DSL: source texture + dest storage + uniform
    {
        DescriptorSetLayoutBinding bindings[3]{};
        bindings[0] = {0, DescriptorType::SampledImage, 1, ShaderStage::Compute};
        bindings[1].binding = 1; bindings[1].descriptorType = DescriptorType::StorageImage;
        bindings[1].descriptorCount = 1; bindings[1].stageFlags = ShaderStage::Compute;
        bindings[1].format = DataFormat::RGBA16_Float;
        bindings[2] = {2, DescriptorType::UniformBuffer, 1, ShaderStage::Compute};

        DescriptorSetLayoutDesc dslDesc;
        dslDesc.bindingCount = 3;
        dslDesc.bindings = bindings;
        s_MipDSL = device.CreateDescriptorSetLayout(dslDesc);

        PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &s_MipDSL;
        s_MipLayout = device.CreatePipelineLayout(plDesc);

        ComputePipelineDesc pipeDesc;
        pipeDesc.computeShader = mipCS;
        pipeDesc.layout = s_MipLayout;
        pipeDesc.threadGroupSize = {8, 8, 1};
        s_MipPipeline = device.CreateComputePipeline(pipeDesc);
    }

    // Create constant buffers and descriptor set pools
    for (u32 i = 0; i < MAX_FRAMES; ++i) {
        BufferDesc bufDesc{};
        bufDesc.size = 16; // HZBCopyParams / HZBMipParams (16 bytes)
        bufDesc.type = BufferType::Constant;
        bufDesc.usage = GPUMemoryUsage::Dynamic;
        bufDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        s_CopyParamsBuf[i] = device.CreateBuffer(bufDesc);
        s_CopyParamsMapped[i] = device.MapBuffer(s_CopyParamsBuf[i], 0, 16);

        s_MipParamsBuf[i] = device.CreateBuffer(bufDesc);
        s_MipParamsMapped[i] = device.MapBuffer(s_MipParamsBuf[i], 0, 16);

        for (u32 j = 0; j < MAX_SETS_PER_FRAME; ++j) {
            {
                DescriptorSetDesc dsDesc;
                dsDesc.layout = s_CopyDSL;
                s_CopySetPool[i][j] = device.CreateDescriptorSet(dsDesc);
            }
            {
                DescriptorSetDesc dsDesc;
                dsDesc.layout = s_MipDSL;
                s_MipSetPool[i][j] = device.CreateDescriptorSet(dsDesc);
            }
        }
    }

    return s_CopyPipeline != handles::INVALID_PIPELINE && s_MipPipeline != handles::INVALID_PIPELINE;
}

static u32 ComputeMipLevels(u32 width, u32 height) {
    return u32(std::ceil(std::log2(std::max(width, height))));
}

static void CreateHZBTexture(RHIDeviceBase& device, u32 width, u32 height) {
    u32 mipLevels = ComputeMipLevels(width, height);

    if (s_HZBTexture != handles::INVALID_RESOURCE) {
        // Check if dimensions match
        // For now, always recreate
    }

    // Destroy old views
    for (auto h : s_MipWriteViews) device.DestroyTexture(h);
    for (auto h : s_MipReadViews) device.DestroyTexture(h);
    if (s_HZBTexture != handles::INVALID_RESOURCE) device.DestroyTexture(s_HZBTexture);

    s_MipWriteViews.clear();
    s_MipReadViews.clear();

    // Create HZB texture
    TextureDesc hzbDesc;
    hzbDesc.size = {width, height, 1};
    hzbDesc.format = DataFormat::RGBA16_Float;
    hzbDesc.type = TextureType::Texture2D;
    hzbDesc.mipLevels = mipLevels;
    hzbDesc.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
    s_HZBTexture = device.CreateTexture(hzbDesc);

    // Create per-mip views
    s_MipWriteViews.resize(mipLevels);
    s_MipReadViews.resize(mipLevels);
    for (u32 i = 0; i < mipLevels; ++i) {
        // Write view (storage, single mip)
        TextureViewDesc writeViewDesc;
        writeViewDesc.texture = s_HZBTexture;
        writeViewDesc.viewType = TextureType::Texture2D;
        writeViewDesc.format = DataFormat::RGBA16_Float;
        writeViewDesc.mostDetailedMip = i;
        writeViewDesc.mipCount = 1;
        s_MipWriteViews[i] = device.CreateTextureView(writeViewDesc);

        // Read view (sampled, single mip)
        TextureViewDesc readViewDesc;
        readViewDesc.texture = s_HZBTexture;
        readViewDesc.viewType = TextureType::Texture2D;
        readViewDesc.format = DataFormat::RGBA16_Float;
        readViewDesc.mostDetailedMip = i;
        readViewDesc.mipCount = 1;
        s_MipReadViews[i] = device.CreateTextureView(readViewDesc);
    }
}

const HZBPassData& AddHZBPass(RenderGraph& graph, RGResourceHandle depthTexture,
                                u32 width, u32 height) {
    auto& device = graph.GetDevice();
    u32 mipLevels = ComputeMipLevels(width, height);

    EnsurePipelines(device);
    CreateHZBTexture(device, width, height);

    return graph.AddPass<HZBPassData>("HZBPass", RGPassType::Compute, RGPassCategory::PostProcess,
        [&](HZBPassData& data, RenderGraphBuilder& builder) {
            builder.Read(depthTexture, ResourceState::ShaderResource);

            // Import HZB texture into render graph and declare write dependency
            // so the pass is not pruned and downstream passes (SSGI) see the dependency.
            data.hzbTexture = graph.ImportResource("HZB_Texture", s_HZBTexture);
            data.hzbTexture = builder.Write(data.hzbTexture, ResourceState::UnorderedAccess);
            data.mipLevels = mipLevels;
        },
        [depthTexture, width, height, mipLevels](const HZBPassData& data, RenderGraphContext& context) {
            if (s_CopyPipeline == handles::INVALID_PIPELINE) return;
            auto& device = context.graph->GetDevice();
            auto* cmd = context.cmdBuffer;

            // Resolve depth texture handle
            auto* depthRes = context.graph->GetResource(depthTexture);
            ResourceHandle depthHandle = depthRes ? depthRes->GetPhysicalHandle() : handles::INVALID_RESOURCE;
            if (depthHandle == handles::INVALID_RESOURCE) return;

            constexpr u32 TG = 8;

            // === Pass 1: Copy depth to mip 0 ===
            {
                u32 fi = 0; // Frame index not used for HZB (single set per dispatch)
                u32 idx = s_CopySetIndex[fi]++;
                if (idx >= MAX_SETS_PER_FRAME) { idx = 0; s_CopySetIndex[fi] = 1; }
                DescriptorSetHandle ds = s_CopySetPool[fi][idx];

                // Update params
                if (s_CopyParamsMapped[fi]) {
                    u32* p = static_cast<u32*>(s_CopyParamsMapped[fi]);
                    p[0] = width;
                    p[1] = height;
                    p[2] = 0;
                    p[3] = 0;
                    device.SetBufferDirtySize(s_CopyParamsBuf[fi], 16);
                }

                DescriptorImageInfo depthInfo; depthInfo.imageView = depthHandle;
                DescriptorImageInfo hzbInfo; hzbInfo.imageView = s_MipWriteViews[0];
                DescriptorBufferInfo bufInfo; bufInfo.buffer = s_CopyParamsBuf[0]; bufInfo.offset = 0; bufInfo.range = 16;

                WriteDescriptorSet writes[3];
                writes[0].dstSet = ds; writes[0].dstBinding = 0; writes[0].descriptorCount = 1;
                writes[0].descriptorType = DescriptorType::SampledDepthImage; writes[0].imageInfo = &depthInfo;
                writes[1].dstSet = ds; writes[1].dstBinding = 1; writes[1].descriptorCount = 1;
                writes[1].descriptorType = DescriptorType::StorageImage; writes[1].imageInfo = &hzbInfo;
                writes[2].dstSet = ds; writes[2].dstBinding = 2; writes[2].descriptorCount = 1;
                writes[2].descriptorType = DescriptorType::UniformBuffer; writes[2].bufferInfo = &bufInfo;

                device.UpdateDescriptorSets(3, writes);

                cmd->BindComputePipeline(s_CopyPipeline);
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, s_CopyLayout, 0, 1, &ds, 0, nullptr);
                cmd->Dispatch((width + TG - 1) / TG, (height + TG - 1) / TG, 1);
            }

            // Barrier: mip 0 UAV -> SRV
            {
                ResourceBarrier barrier{};
                barrier.resource = s_MipWriteViews[0]; // Use the view handle
                barrier.beforeState = ResourceState::UnorderedAccess;
                barrier.afterState = ResourceState::ShaderResource;
                barrier.subresource = 0xFFFFFFFF;
                cmd->InsertBarrier(&barrier, 1);
            }

            // === Pass 2: Generate mip chain ===
            for (u32 mip = 1; mip < mipLevels; ++mip) {
                u32 srcW = std::max(1u, width >> (mip - 1));
                u32 srcH = std::max(1u, height >> (mip - 1));
                u32 dstW = std::max(1u, width >> mip);
                u32 dstH = std::max(1u, height >> mip);

                u32 idx = s_MipSetIndex[0]++;
                if (idx >= MAX_SETS_PER_FRAME) { idx = 0; s_MipSetIndex[0] = 1; }
                DescriptorSetHandle ds = s_MipSetPool[0][idx];

                // Update params
                if (s_MipParamsMapped[0]) {
                    u32* p = static_cast<u32*>(s_MipParamsMapped[0]);
                    p[0] = srcW;
                    p[1] = srcH;
                    p[2] = dstW;
                    p[3] = dstH;
                    device.SetBufferDirtySize(s_MipParamsBuf[0], 16);
                }

                DescriptorImageInfo srcInfo; srcInfo.imageView = s_MipReadViews[mip - 1];
                DescriptorImageInfo dstInfo; dstInfo.imageView = s_MipWriteViews[mip];
                DescriptorBufferInfo bufInfo; bufInfo.buffer = s_MipParamsBuf[0]; bufInfo.offset = 0; bufInfo.range = 16;

                WriteDescriptorSet writes[3];
                writes[0].dstSet = ds; writes[0].dstBinding = 0; writes[0].descriptorCount = 1;
                writes[0].descriptorType = DescriptorType::SampledImage; writes[0].imageInfo = &srcInfo;
                writes[1].dstSet = ds; writes[1].dstBinding = 1; writes[1].descriptorCount = 1;
                writes[1].descriptorType = DescriptorType::StorageImage; writes[1].imageInfo = &dstInfo;
                writes[2].dstSet = ds; writes[2].dstBinding = 2; writes[2].descriptorCount = 1;
                writes[2].descriptorType = DescriptorType::UniformBuffer; writes[2].bufferInfo = &bufInfo;

                device.UpdateDescriptorSets(3, writes);

                cmd->BindComputePipeline(s_MipPipeline);
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, s_MipLayout, 0, 1, &ds, 0, nullptr);
                cmd->Dispatch((dstW + TG - 1) / TG, (dstH + TG - 1) / TG, 1);

                // Barrier: current mip UAV -> SRV
                ResourceBarrier barrier{};
                barrier.resource = s_MipWriteViews[mip];
                barrier.beforeState = ResourceState::UnorderedAccess;
                barrier.afterState = ResourceState::ShaderResource;
                barrier.subresource = 0xFFFFFFFF;
                cmd->InsertBarrier(&barrier, 1);
            }
        }
    );
}

} // namespace primal::graphics::PostProcess
