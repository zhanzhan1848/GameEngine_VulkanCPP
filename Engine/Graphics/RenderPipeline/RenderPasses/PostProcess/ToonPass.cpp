#include "ToonPass.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/Utils/ShaderRegistry.h"
#include <fstream>
#include <sstream>
#include <iostream>

namespace primal::graphics::renderpass {

using namespace rendergraph;
using namespace rhi;

static constexpr u32 MAX_FRAMES = 3;

static PipelineHandle s_Pipeline = handles::INVALID_PIPELINE;
static PipelineLayoutHandle s_Layout = handles::INVALID_PIPELINE_LAYOUT;
static DescriptorSetLayoutHandle s_DSL = handles::INVALID_RESOURCE;
static DescriptorSetHandle s_SetPool[MAX_FRAMES] = {
    handles::INVALID_DESCRIPTOR_SET, handles::INVALID_DESCRIPTOR_SET,
    handles::INVALID_DESCRIPTOR_SET};

// Per-frame params UBO (persistently mapped)
struct ToonConstantsCPU {
    f32 edgeThreshold;
    f32 colorLevels;
    f32 _pad0;
    f32 _pad1;
};
static ResourceHandle s_ParamsBuf[MAX_FRAMES] = {
    handles::INVALID_RESOURCE, handles::INVALID_RESOURCE, handles::INVALID_RESOURCE};
static void* s_ParamsMapped[MAX_FRAMES] = {};

// Platform-branched shader loader (same convention as TAAPass):
//   Vulkan: PostProcess/Toon.comp.spv (hand-written GLSL), entry "main".
//   Metal:  Metal/shaders/ToonShader.metal source, entry "toon_main".
static std::vector<u8> LoadShaderBytes(RHIPlatform platform) {
    if (platform == RHIPlatform::Vulkan) {
        const std::string relPath = "Engine/Graphics/Vulkan/shaders/PostProcess/Toon.comp.spv";
        const std::vector<std::string> candidates = {
            relPath,
            "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/.worktrees/vulkan-rhi/" + relPath,
        };
        for (const auto& path : candidates) {
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file.is_open()) continue;
            std::streamsize size = file.tellg();
            file.seekg(0, std::ios::beg);
            std::vector<u8> bytecode(static_cast<size_t>(size));
            if (!file.read(reinterpret_cast<char*>(bytecode.data()), size)) continue;
            return bytecode;
        }
        std::cerr << "[Toon] Failed to load SPIR-V: PostProcess/Toon.comp.spv" << std::endl;
        return {};
    }

    std::string path = utils::ShaderRegistry::GetShaderPath(platform, "ToonShader");
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "[Toon] Failed to open Metal source: " << path << std::endl;
        return {};
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string src = buffer.str();
    return std::vector<u8>(src.begin(), src.end());
}

static const char* ShaderEntryPoint(RHIPlatform platform) {
    return (platform == RHIPlatform::Vulkan) ? "main" : "toon_main";
}

static bool EnsurePipeline(RHIDeviceBase& device) {
    if (s_Pipeline != handles::INVALID_PIPELINE) return true;

    auto platform = device.GetPlatform();
    std::vector<u8> shaderBytes = LoadShaderBytes(platform);
    if (shaderBytes.empty()) {
        std::cerr << "[Toon] Shader load failed" << std::endl;
        return false;
    }

    ShaderHandle cs = device.CreateShader(shaderBytes.data(), shaderBytes.size(),
                                          ShaderStage::Compute, ShaderEntryPoint(platform));
    if (cs == handles::INVALID_SHADER) {
        std::cerr << "[Toon] Shader creation failed" << std::endl;
        return false;
    }

    // 5 bindings: 0..2 = sampled (color/depth/normal), 3 = storage (output),
    // 4 = uniform (ToonConstants). Samplerless — texelFetch only, like TAA.
    DescriptorSetLayoutBinding bindings[5]{};
    bindings[0] = {0, DescriptorType::SampledImage,     1, ShaderStage::Compute};
    bindings[1] = {1, DescriptorType::SampledDepthImage, 1, ShaderStage::Compute};
    bindings[2] = {2, DescriptorType::SampledImage,     1, ShaderStage::Compute};
    bindings[3] = {3, DescriptorType::StorageImage,     1, ShaderStage::Compute};
    bindings[4] = {4, DescriptorType::UniformBuffer,    1, ShaderStage::Compute};

    DescriptorSetLayoutDesc dslDesc;
    dslDesc.bindingCount = 5;
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
        if (s_ParamsBuf[i] == handles::INVALID_RESOURCE) {
            BufferDesc bufDesc{};
            bufDesc.size = sizeof(ToonConstantsCPU);
            bufDesc.type = BufferType::Constant;
            bufDesc.usage = GPUMemoryUsage::Dynamic;
            bufDesc.memoryUsage = GPUMemoryUsage::Dynamic;
            s_ParamsBuf[i] = device.CreateBuffer(bufDesc);
            s_ParamsMapped[i] = device.MapBuffer(s_ParamsBuf[i], 0, sizeof(ToonConstantsCPU));
        }
        if (s_SetPool[i] == handles::INVALID_DESCRIPTOR_SET) {
            DescriptorSetDesc dsDesc;
            dsDesc.layout = s_DSL;
            s_SetPool[i] = device.CreateDescriptorSet(dsDesc);
        }
    }

    return s_Pipeline != handles::INVALID_PIPELINE;
}

const ToonPassData& AddToonPass(RenderGraph& graph, RGResourceHandle inputColor,
                                RGResourceHandle depth, RGResourceHandle normal,
                                const ToonParams& params, u32 bufferIndex,
                                u32 width, u32 height) {
    EnsurePipeline(graph.GetDevice());

    u32 fi = bufferIndex % MAX_FRAMES;

    if (s_ParamsMapped[fi]) {
        auto* consts = static_cast<ToonConstantsCPU*>(s_ParamsMapped[fi]);
        consts->edgeThreshold = params.edgeThreshold;
        consts->colorLevels = params.colorLevels;
        consts->_pad0 = 0.0f;
        consts->_pad1 = 0.0f;
        graph.GetDevice().SetBufferDirtySize(s_ParamsBuf[fi], sizeof(ToonConstantsCPU));
    }

    return graph.AddPass<ToonPassData>("ToonPass", RGPassType::Compute, RGPassCategory::PostProcess,
        [&](ToonPassData& data, RenderGraphBuilder& builder) {
            builder.Read(inputColor, ResourceState::ShaderResource);
            builder.Read(depth, ResourceState::ShaderResource);
            builder.Read(normal, ResourceState::ShaderResource);

            TextureDesc outputDesc;
            outputDesc.size = {width, height, 1};
            outputDesc.format = DataFormat::RGBA8_UNorm;
            outputDesc.type = TextureType::Texture2D;
            outputDesc.mipLevels = 1;
            outputDesc.usage = TextureUsage::UnorderedAccess | TextureUsage::ShaderResource |
                               TextureUsage::CopySource;
            data.toonOutput = builder.CreateTexture("Toon_Output", outputDesc, ResourceState::UnorderedAccess);
        },
        [inputColor, depth, normal, width, height, fi]
         (const ToonPassData& data, RenderGraphContext& context) {
            if (s_Pipeline == handles::INVALID_PIPELINE || width == 0 || height == 0) {
                std::cerr << "[Toon] Execute abort: pipeline INVALID or zero size" << std::endl;
                return;
            }

            auto& device = context.graph->GetDevice();
            auto* cmd = context.cmdBuffer;

            auto* inRes = context.graph->GetResource(inputColor);
            ResourceHandle inHandle = inRes ? inRes->GetPhysicalHandle() : handles::INVALID_RESOURCE;
            auto* depthRes = context.graph->GetResource(depth);
            ResourceHandle depthHandle = depthRes ? depthRes->GetPhysicalHandle() : handles::INVALID_RESOURCE;
            auto* normalRes = context.graph->GetResource(normal);
            ResourceHandle normalHandle = normalRes ? normalRes->GetPhysicalHandle() : handles::INVALID_RESOURCE;
            auto* outRes = context.graph->GetResource(data.toonOutput);
            ResourceHandle outHandle = outRes ? outRes->GetPhysicalHandle() : handles::INVALID_RESOURCE;

            if (inHandle == handles::INVALID_RESOURCE || depthHandle == handles::INVALID_RESOURCE ||
                normalHandle == handles::INVALID_RESOURCE || outHandle == handles::INVALID_RESOURCE) {
                std::cerr << "[Toon] Execute abort: invalid input resource" << std::endl;
                return;
            }

            DescriptorSetHandle ds = s_SetPool[fi];

            DescriptorImageInfo colorInfo; colorInfo.imageView = inHandle;
            DescriptorImageInfo depthInfo; depthInfo.imageView = depthHandle;
            DescriptorImageInfo normalInfo; normalInfo.imageView = normalHandle;
            DescriptorImageInfo outInfo;   outInfo.imageView = outHandle;
            DescriptorBufferInfo bufInfo;
            bufInfo.buffer = s_ParamsBuf[fi];
            bufInfo.offset = 0;
            bufInfo.range = sizeof(ToonConstantsCPU);

            WriteDescriptorSet writes[5];
            writes[0].dstSet = ds; writes[0].dstBinding = 0; writes[0].descriptorCount = 1;
            writes[0].descriptorType = DescriptorType::SampledImage; writes[0].imageInfo = &colorInfo;
            writes[1].dstSet = ds; writes[1].dstBinding = 1; writes[1].descriptorCount = 1;
            writes[1].descriptorType = DescriptorType::SampledDepthImage; writes[1].imageInfo = &depthInfo;
            writes[2].dstSet = ds; writes[2].dstBinding = 2; writes[2].descriptorCount = 1;
            writes[2].descriptorType = DescriptorType::SampledImage; writes[2].imageInfo = &normalInfo;
            writes[3].dstSet = ds; writes[3].dstBinding = 3; writes[3].descriptorCount = 1;
            writes[3].descriptorType = DescriptorType::StorageImage; writes[3].imageInfo = &outInfo;
            writes[4].dstSet = ds; writes[4].dstBinding = 4; writes[4].descriptorCount = 1;
            writes[4].descriptorType = DescriptorType::UniformBuffer; writes[4].bufferInfo = &bufInfo;

            device.UpdateDescriptorSets(5, writes);

            cmd->BindComputePipeline(s_Pipeline);
            cmd->BindDescriptorSets(PipelineBindPoint::Compute, s_Layout, 0, 1, &ds, 0, nullptr);
            cmd->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
        }
    );
}

} // namespace primal::graphics::renderpass
