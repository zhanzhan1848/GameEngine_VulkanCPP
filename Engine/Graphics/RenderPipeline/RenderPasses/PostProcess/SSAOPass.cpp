#include "SSAOPass.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Core/RHIMath.h"
#include "Graphics/Utils/ShaderRegistry.h"
#include "Graphics/Dawn/ShaderLoader.h"
#include <fstream>
#include <sstream>
#include <cstring>
#include <iostream>

namespace primal::graphics::PostProcess {

using namespace rhi;

static constexpr u32 MAX_SETS_PER_FRAME = 2;

// Matches SSAOParams struct in SSAO.wgsl
struct SSAOParamsCPU {
    math::m4x4 invProj;
    math::m4x4 proj;
    math::v4    screenSize;     // x=width, y=height, z=1/width, w=1/height
    f32         radius;
    f32         power;
    u32         sampleCount;
    u32         frameIndex;
};

// Matches BlurParams struct in SSAOBlur.wgsl
struct BlurParamsCPU {
    math::v4    screenSize;
};

// === Trace pass statics ===
static PipelineHandle s_TracePipeline = handles::INVALID_PIPELINE;
static PipelineLayoutHandle s_TraceLayout = handles::INVALID_PIPELINE_LAYOUT;
static DescriptorSetLayoutHandle s_TraceDSL = handles::INVALID_RESOURCE;
static DescriptorSetHandle s_TraceSetPool[MAX_FRAMES_IN_FLIGHT][MAX_SETS_PER_FRAME] = {};
static u32 s_TraceSetIndex[MAX_FRAMES_IN_FLIGHT] = {};
static ResourceHandle s_ParamsBuffer[MAX_FRAMES_IN_FLIGHT] = {};
static void* s_ParamsMapped[MAX_FRAMES_IN_FLIGHT] = {};

// === Blur pass statics ===
static PipelineHandle s_BlurPipeline = handles::INVALID_PIPELINE;
static PipelineLayoutHandle s_BlurLayout = handles::INVALID_PIPELINE_LAYOUT;
static DescriptorSetLayoutHandle s_BlurDSL = handles::INVALID_RESOURCE;
static DescriptorSetHandle s_BlurSetPool[MAX_FRAMES_IN_FLIGHT][MAX_SETS_PER_FRAME] = {};
static u32 s_BlurSetIndex[MAX_FRAMES_IN_FLIGHT] = {};
static ResourceHandle s_BlurParamsBuffer[MAX_FRAMES_IN_FLIGHT] = {};
static void* s_BlurParamsMapped[MAX_FRAMES_IN_FLIGHT] = {};

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

static bool EnsureTracePipeline(RHIDeviceBase& device) {
    if (s_TracePipeline != handles::INVALID_PIPELINE) return true;

    auto platform = device.GetPlatform();
    std::string shaderPath = utils::ShaderRegistry::GetShaderPath(platform, "SSAO");
    std::string shaderSource = LoadShaderSource(shaderPath);
    if (shaderSource.empty()) { std::cerr << "[SSAO] Trace shader source empty: " << shaderPath << std::endl; return false; }

    ShaderHandle cs = device.CreateShader(shaderSource.data(), shaderSource.size(), ShaderStage::Compute, "ssao_trace");

    if (cs == handles::INVALID_SHADER) { std::cerr << "[SSAO] Trace shader creation failed" << std::endl; return false; }

    DescriptorSetLayoutBinding bindings[3]{};
    bindings[0] = {0, DescriptorType::SampledDepthImage, 1, ShaderStage::Compute};
    bindings[1] = {1, DescriptorType::StorageImage, 1, ShaderStage::Compute};
    bindings[2] = {2, DescriptorType::UniformBuffer, 1, ShaderStage::Compute};

    DescriptorSetLayoutDesc dslDesc;
    dslDesc.bindingCount = 3;
    dslDesc.bindings = bindings;
    s_TraceDSL = device.CreateDescriptorSetLayout(dslDesc);

    PipelineLayoutDesc layoutDesc;
    layoutDesc.setLayoutCount = 1;
    layoutDesc.setLayouts = &s_TraceDSL;
    s_TraceLayout = device.CreatePipelineLayout(layoutDesc);

    ComputePipelineDesc pipelineDesc;
    pipelineDesc.computeShader = cs;
    pipelineDesc.layout = s_TraceLayout;
    pipelineDesc.threadGroupSize = {8, 8, 1};
    s_TracePipeline = device.CreateComputePipeline(pipelineDesc);

    for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        BufferDesc bufDesc{};
        bufDesc.size = sizeof(SSAOParamsCPU);
        bufDesc.type = BufferType::Constant;
        bufDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        bufDesc.usage = GPUMemoryUsage::Dynamic;
        bufDesc.name = "SSAO_Params_" + std::to_string(i);
        s_ParamsBuffer[i] = device.CreateBuffer(bufDesc);
        s_ParamsMapped[i] = device.MapBuffer(s_ParamsBuffer[i], 0, sizeof(SSAOParamsCPU));
    }

    for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        for (u32 j = 0; j < MAX_SETS_PER_FRAME; ++j) {
            DescriptorSetDesc dsDesc;
            dsDesc.layout = s_TraceDSL;
            s_TraceSetPool[i][j] = device.CreateDescriptorSet(dsDesc);
        }
    }

    return s_TracePipeline != handles::INVALID_PIPELINE;
}

static bool EnsureBlurPipeline(RHIDeviceBase& device) {
    if (s_BlurPipeline != handles::INVALID_PIPELINE) return true;

    auto platform = device.GetPlatform();
    std::string shaderPath = utils::ShaderRegistry::GetShaderPath(platform, "SSAOBlur");
    std::string shaderSource = LoadShaderSource(shaderPath);
    if (shaderSource.empty()) { std::cerr << "[SSAO] Blur shader source empty: " << shaderPath << std::endl; return false; }

    ShaderHandle cs = device.CreateShader(shaderSource.data(), shaderSource.size(), ShaderStage::Compute, "main");
    if (cs == handles::INVALID_SHADER) { std::cerr << "[SSAO] Blur shader creation failed" << std::endl; return false; }

    // Binding 0: AO input, 1: depth, 2: blurred output, 3: blur params uniform
    DescriptorSetLayoutBinding bindings[4]{};
    bindings[0] = {0, DescriptorType::SampledImage, 1, ShaderStage::Compute};
    bindings[1] = {1, DescriptorType::SampledDepthImage, 1, ShaderStage::Compute};
    bindings[2] = {2, DescriptorType::StorageImage, 1, ShaderStage::Compute};
    bindings[3] = {3, DescriptorType::UniformBuffer, 1, ShaderStage::Compute};

    DescriptorSetLayoutDesc dslDesc;
    dslDesc.bindingCount = 4;
    dslDesc.bindings = bindings;
    s_BlurDSL = device.CreateDescriptorSetLayout(dslDesc);

    PipelineLayoutDesc layoutDesc;
    layoutDesc.setLayoutCount = 1;
    layoutDesc.setLayouts = &s_BlurDSL;
    s_BlurLayout = device.CreatePipelineLayout(layoutDesc);

    ComputePipelineDesc pipelineDesc;
    pipelineDesc.computeShader = cs;
    pipelineDesc.layout = s_BlurLayout;
    pipelineDesc.threadGroupSize = {8, 8, 1};
    s_BlurPipeline = device.CreateComputePipeline(pipelineDesc);

    for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        BufferDesc bufDesc{};
        bufDesc.size = sizeof(BlurParamsCPU);
        bufDesc.type = BufferType::Constant;
        bufDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        bufDesc.usage = GPUMemoryUsage::Dynamic;
        bufDesc.name = "SSAO_BlurParams_" + std::to_string(i);
        s_BlurParamsBuffer[i] = device.CreateBuffer(bufDesc);
        s_BlurParamsMapped[i] = device.MapBuffer(s_BlurParamsBuffer[i], 0, sizeof(BlurParamsCPU));
    }

    for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        for (u32 j = 0; j < MAX_SETS_PER_FRAME; ++j) {
            DescriptorSetDesc dsDesc;
            dsDesc.layout = s_BlurDSL;
            s_BlurSetPool[i][j] = device.CreateDescriptorSet(dsDesc);
        }
    }

    return s_BlurPipeline != handles::INVALID_PIPELINE;
}

const SSAOPassData& AddSSAOPass(RenderGraph& graph, RGResourceHandle depthTexture,
                                  u32 width, u32 height, u32 frameIndex,
                                  const math::m4x4& projMatrix, const math::m4x4& invProjMatrix) {
    u32 fi = frameIndex % MAX_FRAMES_IN_FLIGHT;
    s_TraceSetIndex[fi] = 0;
    s_BlurSetIndex[fi] = 0;

    // Update trace params
    if (s_ParamsMapped[fi]) {
        auto* params = static_cast<SSAOParamsCPU*>(s_ParamsMapped[fi]);
        params->invProj = invProjMatrix;
        params->proj = projMatrix;
        params->screenSize = {(f32)width, (f32)height, 1.0f / width, 1.0f / height};
        params->radius = 0.3f;
        params->power = 2.5f;
        params->sampleCount = 16;
        params->frameIndex = frameIndex;
        graph.GetDevice().SetBufferDirtySize(s_ParamsBuffer[fi], sizeof(SSAOParamsCPU));
    }

    // Update blur params
    if (s_BlurParamsMapped[fi]) {
        auto* bp = static_cast<BlurParamsCPU*>(s_BlurParamsMapped[fi]);
        bp->screenSize = {(f32)width, (f32)height, 1.0f / width, 1.0f / height};
        graph.GetDevice().SetBufferDirtySize(s_BlurParamsBuffer[fi], sizeof(BlurParamsCPU));
    }

    // === Pass 1: SSAO Trace ===
    RGResourceHandle rawAO;
    graph.AddPass<SSAOPassData>("SSAOTracePass", RGPassType::Compute, RGPassCategory::PostProcess,
        [&](SSAOPassData& data, RenderGraphBuilder& builder) {
            builder.Read(depthTexture, ResourceState::ShaderResource);

            TextureDesc aoDesc;
            aoDesc.size = {width, height, 1};
            aoDesc.format = DataFormat::RGBA16_Float;
            aoDesc.type = TextureType::Texture2D;
            aoDesc.mipLevels = 1;
            aoDesc.usage = TextureUsage::UnorderedAccess | TextureUsage::ShaderResource;
            data.ssaoOutput = builder.CreateTexture("SSAO_Raw", aoDesc, ResourceState::UnorderedAccess);
            rawAO = data.ssaoOutput;

            EnsureTracePipeline(builder.GetGraph().GetDevice());
        },
        [depthTexture, width, height, fi](const SSAOPassData& data, RenderGraphContext& context) {
            if (s_TracePipeline == handles::INVALID_PIPELINE) return;

            auto& device = context.graph->GetDevice();
            auto* cmd = context.cmdBuffer;

            auto* depthRes = context.graph->GetResource(depthTexture);
            ResourceHandle depthHandle = depthRes ? depthRes->GetPhysicalHandle() : handles::INVALID_RESOURCE;
            auto* aoRes = context.graph->GetResource(data.ssaoOutput);
            ResourceHandle aoHandle = aoRes ? aoRes->GetPhysicalHandle() : handles::INVALID_RESOURCE;

            if (depthHandle == handles::INVALID_RESOURCE || aoHandle == handles::INVALID_RESOURCE) return;

            u32 idx = s_TraceSetIndex[fi]++;
            if (idx >= MAX_SETS_PER_FRAME) { idx = 0; s_TraceSetIndex[fi] = 1; }
            DescriptorSetHandle ds = s_TraceSetPool[fi][idx];

            DescriptorImageInfo depthInfo; depthInfo.imageView = depthHandle;
            DescriptorImageInfo aoInfo; aoInfo.imageView = aoHandle;
            DescriptorBufferInfo bufInfo; bufInfo.buffer = s_ParamsBuffer[fi]; bufInfo.offset = 0; bufInfo.range = sizeof(SSAOParamsCPU);

            WriteDescriptorSet writes[3];
            writes[0].dstSet = ds; writes[0].dstBinding = 0; writes[0].descriptorCount = 1;
            writes[0].descriptorType = DescriptorType::SampledDepthImage; writes[0].imageInfo = &depthInfo;
            writes[1].dstSet = ds; writes[1].dstBinding = 1; writes[1].descriptorCount = 1;
            writes[1].descriptorType = DescriptorType::StorageImage; writes[1].imageInfo = &aoInfo;
            writes[2].dstSet = ds; writes[2].dstBinding = 2; writes[2].descriptorCount = 1;
            writes[2].descriptorType = DescriptorType::UniformBuffer; writes[2].bufferInfo = &bufInfo;

            device.UpdateDescriptorSets(3, writes);

            cmd->BindComputePipeline(s_TracePipeline);
            cmd->BindDescriptorSets(PipelineBindPoint::Compute, s_TraceLayout, 0, 1, &ds, 0, nullptr);
            cmd->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
        }
    );

    // === Pass 2: SSAO Blur ===
    return graph.AddPass<SSAOPassData>("SSAOBlurPass", RGPassType::Compute, RGPassCategory::PostProcess,
        [&](SSAOPassData& data, RenderGraphBuilder& builder) {
            builder.Read(rawAO, ResourceState::ShaderResource);
            builder.Read(depthTexture, ResourceState::ShaderResource);

            TextureDesc blurDesc;
            blurDesc.size = {width, height, 1};
            blurDesc.format = DataFormat::RGBA16_Float;
            blurDesc.type = TextureType::Texture2D;
            blurDesc.mipLevels = 1;
            blurDesc.usage = TextureUsage::UnorderedAccess | TextureUsage::ShaderResource;
            data.ssaoOutput = builder.CreateTexture("SSAO_Blurred", blurDesc, ResourceState::UnorderedAccess);

            EnsureBlurPipeline(builder.GetGraph().GetDevice());
        },
        [depthTexture, rawAO, width, height, fi](const SSAOPassData& data, RenderGraphContext& context) {
            if (s_BlurPipeline == handles::INVALID_PIPELINE) return;

            auto& device = context.graph->GetDevice();
            auto* cmd = context.cmdBuffer;

            auto* aoRes = context.graph->GetResource(rawAO);
            ResourceHandle aoHandle = aoRes ? aoRes->GetPhysicalHandle() : handles::INVALID_RESOURCE;
            auto* depthRes = context.graph->GetResource(depthTexture);
            ResourceHandle depthHandle = depthRes ? depthRes->GetPhysicalHandle() : handles::INVALID_RESOURCE;
            auto* blurRes = context.graph->GetResource(data.ssaoOutput);
            ResourceHandle blurHandle = blurRes ? blurRes->GetPhysicalHandle() : handles::INVALID_RESOURCE;

            if (aoHandle == handles::INVALID_RESOURCE || depthHandle == handles::INVALID_RESOURCE || blurHandle == handles::INVALID_RESOURCE) return;

            u32 idx = s_BlurSetIndex[fi]++;
            if (idx >= MAX_SETS_PER_FRAME) { idx = 0; s_BlurSetIndex[fi] = 1; }
            DescriptorSetHandle ds = s_BlurSetPool[fi][idx];

            DescriptorImageInfo aoInfo; aoInfo.imageView = aoHandle;
            DescriptorImageInfo depthInfo; depthInfo.imageView = depthHandle;
            DescriptorImageInfo blurInfo; blurInfo.imageView = blurHandle;
            DescriptorBufferInfo bufInfo; bufInfo.buffer = s_BlurParamsBuffer[fi]; bufInfo.offset = 0; bufInfo.range = sizeof(BlurParamsCPU);

            WriteDescriptorSet writes[4];
            writes[0].dstSet = ds; writes[0].dstBinding = 0; writes[0].descriptorCount = 1;
            writes[0].descriptorType = DescriptorType::SampledImage; writes[0].imageInfo = &aoInfo;
            writes[1].dstSet = ds; writes[1].dstBinding = 1; writes[1].descriptorCount = 1;
            writes[1].descriptorType = DescriptorType::SampledDepthImage; writes[1].imageInfo = &depthInfo;
            writes[2].dstSet = ds; writes[2].dstBinding = 2; writes[2].descriptorCount = 1;
            writes[2].descriptorType = DescriptorType::StorageImage; writes[2].imageInfo = &blurInfo;
            writes[3].dstSet = ds; writes[3].dstBinding = 3; writes[3].descriptorCount = 1;
            writes[3].descriptorType = DescriptorType::UniformBuffer; writes[3].bufferInfo = &bufInfo;

            device.UpdateDescriptorSets(4, writes);

            cmd->BindComputePipeline(s_BlurPipeline);
            cmd->BindDescriptorSets(PipelineBindPoint::Compute, s_BlurLayout, 0, 1, &ds, 0, nullptr);
            cmd->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
        }
    );
}

void ShutdownSSAOPass() {
    // Unmap persistent buffers before device destruction
    for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        s_ParamsMapped[i] = nullptr;
        s_BlurParamsMapped[i] = nullptr;
    }
    // Reset static handles so GC/descriptors don't reference stale resources
    s_TracePipeline = handles::INVALID_PIPELINE;
    s_TraceLayout = handles::INVALID_PIPELINE_LAYOUT;
    s_TraceDSL = handles::INVALID_RESOURCE;
    s_BlurPipeline = handles::INVALID_PIPELINE;
    s_BlurLayout = handles::INVALID_PIPELINE_LAYOUT;
    s_BlurDSL = handles::INVALID_RESOURCE;
}

} // namespace primal::graphics::PostProcess
