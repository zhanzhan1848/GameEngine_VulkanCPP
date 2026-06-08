#include "VelocityPass.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/Utils/ShaderRegistry.h"
#include "Graphics/Dawn/ShaderLoader.h"
#include <fstream>
#include <sstream>
#include <iostream>

namespace primal::graphics::PostProcess {

using namespace rhi;

static constexpr u32 MAX_FRAMES = 3;
static constexpr u32 MAX_SETS_PER_FRAME = 2;

static PipelineHandle s_Pipeline = handles::INVALID_PIPELINE;
static PipelineLayoutHandle s_Layout = handles::INVALID_PIPELINE_LAYOUT;
static DescriptorSetLayoutHandle s_DSL = handles::INVALID_RESOURCE;
static DescriptorSetHandle s_SetPool[MAX_FRAMES][MAX_SETS_PER_FRAME] = {};
static u32 s_SetIndex[MAX_FRAMES] = {};

struct VelocityParamsCPU {
    math::m4x4 invProj;
    math::m4x4 viewProj;
    math::m4x4 prevViewProj;
    math::v4    screenSize;
};

static ResourceHandle s_ParamsBuf[MAX_FRAMES] = {};
static void* s_ParamsMapped[MAX_FRAMES] = {};

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

static bool EnsurePipeline(RHIDeviceBase& device) {
    if (s_Pipeline != handles::INVALID_PIPELINE) return true;

    auto platform = device.GetPlatform();
    std::string shaderPath = utils::ShaderRegistry::GetShaderPath(platform, "Velocity");
    std::string source = LoadShaderSource(shaderPath);
    if (source.empty()) { std::cerr << "[Velocity] Shader source empty: " << shaderPath << std::endl; return false; }

    ShaderHandle cs = device.CreateShader(source.data(), source.size(), ShaderStage::Compute, "main");
    if (cs == handles::INVALID_SHADER) { std::cerr << "[Velocity] Shader creation failed" << std::endl; return false; }

    DescriptorSetLayoutBinding bindings[3]{};
    bindings[0] = {0, DescriptorType::SampledDepthImage, 1, ShaderStage::Compute};
    bindings[1] = {1, DescriptorType::StorageImage, 1, ShaderStage::Compute};
    bindings[2] = {2, DescriptorType::UniformBuffer, 1, ShaderStage::Compute};

    DescriptorSetLayoutDesc dslDesc;
    dslDesc.bindingCount = 3;
    dslDesc.bindings = bindings;
    s_DSL = device.CreateDescriptorSetLayout(dslDesc);

    PipelineLayoutDesc layoutDesc;
    layoutDesc.setLayoutCount = 1;
    layoutDesc.setLayouts = &s_DSL;
    s_Layout = device.CreatePipelineLayout(layoutDesc);

    ComputePipelineDesc pipeDesc;
    pipeDesc.computeShader = cs;
    pipeDesc.layout = s_Layout;
    pipeDesc.threadGroupSize = {8, 8, 1};
    s_Pipeline = device.CreateComputePipeline(pipeDesc);

    for (u32 i = 0; i < MAX_FRAMES; ++i) {
        BufferDesc bufDesc{};
        bufDesc.size = sizeof(VelocityParamsCPU);
        bufDesc.type = BufferType::Constant;
        bufDesc.usage = GPUMemoryUsage::Dynamic;
        bufDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        s_ParamsBuf[i] = device.CreateBuffer(bufDesc);
        s_ParamsMapped[i] = device.MapBuffer(s_ParamsBuf[i], 0, sizeof(VelocityParamsCPU));

        for (u32 j = 0; j < MAX_SETS_PER_FRAME; ++j) {
            DescriptorSetDesc dsDesc;
            dsDesc.layout = s_DSL;
            s_SetPool[i][j] = device.CreateDescriptorSet(dsDesc);
        }
    }

    return s_Pipeline != handles::INVALID_PIPELINE;
}

const VelocityPassData& AddVelocityPass(RenderGraph& graph, RGResourceHandle depthTexture,
                                         u32 width, u32 height, u32 frameIndex,
                                         const math::m4x4& viewProj,
                                         const math::m4x4& prevViewProj,
                                         const math::m4x4& invProj) {
    u32 fi = frameIndex % MAX_FRAMES;
    s_SetIndex[fi] = 0;

    EnsurePipeline(graph.GetDevice());

    // Update params
    if (s_ParamsMapped[fi]) {
        auto* params = static_cast<VelocityParamsCPU*>(s_ParamsMapped[fi]);
        params->invProj = invProj;
        params->viewProj = viewProj;
        params->prevViewProj = prevViewProj;
        params->screenSize = {(f32)width, (f32)height, 1.0f / width, 1.0f / height};
        graph.GetDevice().SetBufferDirtySize(s_ParamsBuf[fi], sizeof(VelocityParamsCPU));
    }

    return graph.AddPass<VelocityPassData>("VelocityPass", RGPassType::Compute, RGPassCategory::PostProcess,
        [&](VelocityPassData& data, RenderGraphBuilder& builder) {
            builder.Read(depthTexture, ResourceState::ShaderResource);

            TextureDesc velDesc;
            velDesc.size = {width, height, 1};
            velDesc.format = DataFormat::RGBA16_Float;
            velDesc.type = TextureType::Texture2D;
            velDesc.mipLevels = 1;
            velDesc.usage = TextureUsage::UnorderedAccess | TextureUsage::ShaderResource;
            data.velocityTexture = builder.CreateTexture("Velocity_Output", velDesc, ResourceState::UnorderedAccess);
        },
        [depthTexture, width, height, fi](const VelocityPassData& data, RenderGraphContext& context) {
            if (s_Pipeline == handles::INVALID_PIPELINE) return;

            auto& device = context.graph->GetDevice();
            auto* cmd = context.cmdBuffer;

            auto* depthRes = context.graph->GetResource(depthTexture);
            ResourceHandle depthHandle = depthRes ? depthRes->GetPhysicalHandle() : handles::INVALID_RESOURCE;
            auto* velRes = context.graph->GetResource(data.velocityTexture);
            ResourceHandle velHandle = velRes ? velRes->GetPhysicalHandle() : handles::INVALID_RESOURCE;

            if (depthHandle == handles::INVALID_RESOURCE || velHandle == handles::INVALID_RESOURCE) return;

            u32 idx = s_SetIndex[fi]++;
            if (idx >= MAX_SETS_PER_FRAME) { idx = 0; s_SetIndex[fi] = 1; }
            DescriptorSetHandle ds = s_SetPool[fi][idx];

            DescriptorImageInfo depthInfo; depthInfo.imageView = depthHandle;
            DescriptorImageInfo velInfo; velInfo.imageView = velHandle;
            DescriptorBufferInfo bufInfo; bufInfo.buffer = s_ParamsBuf[fi]; bufInfo.offset = 0; bufInfo.range = sizeof(VelocityParamsCPU);

            WriteDescriptorSet writes[3];
            writes[0].dstSet = ds; writes[0].dstBinding = 0; writes[0].descriptorCount = 1;
            writes[0].descriptorType = DescriptorType::SampledDepthImage; writes[0].imageInfo = &depthInfo;
            writes[1].dstSet = ds; writes[1].dstBinding = 1; writes[1].descriptorCount = 1;
            writes[1].descriptorType = DescriptorType::StorageImage; writes[1].imageInfo = &velInfo;
            writes[2].dstSet = ds; writes[2].dstBinding = 2; writes[2].descriptorCount = 1;
            writes[2].descriptorType = DescriptorType::UniformBuffer; writes[2].bufferInfo = &bufInfo;

            device.UpdateDescriptorSets(3, writes);

            cmd->BindComputePipeline(s_Pipeline);
            cmd->BindDescriptorSets(PipelineBindPoint::Compute, s_Layout, 0, 1, &ds, 0, nullptr);
            cmd->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
        }
    );
}

} // namespace primal::graphics::PostProcess
