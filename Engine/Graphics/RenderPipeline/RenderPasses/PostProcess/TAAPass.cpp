#include "TAAPass.h"
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

static SamplerHandle s_LinearSampler = handles::INVALID_SAMPLER;

// Triple-buffered history textures — ping-pong: read from frameIndex, write to next frame's slot
static ResourceHandle s_HistoryTex[MAX_FRAMES] = {};
static bool s_HistoryInit[MAX_FRAMES] = {}; // false until first write to this slot

// Persistent params buffer per frame slot
struct TAAGlobalsCPU {
    math::v4 screenSize;     // xy = (w, h), zw = (1/w, 1/h)
    f32   invHistoryValid;   // 1.0 if history slot is uninitialized
    u32   _pad0, _pad1, _pad2;
};
static ResourceHandle s_ParamsBuf[MAX_FRAMES] = {};
static void* s_ParamsMapped[MAX_FRAMES] = {};

static u32 s_LastFrameIndex = ~0u; // For detecting first-ever frame (force invHistoryValid=1)

static std::string LoadShaderSource(const std::string& path) {
#ifdef __EMSCRIPTEN__
    auto lastSlash = path.find_last_of('/');
    auto lastDot = path.find_last_of('.');
    if (lastSlash != std::string::npos && lastDot != std::string::npos && lastDot > lastSlash) {
        return dawn::LoadWGSL(path.substr(lastSlash + 1, lastDot - lastSlash - 1).c_str());
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

static bool EnsurePipeline(RHIDeviceBase& device, u32 width, u32 height) {
    if (s_Pipeline != handles::INVALID_PIPELINE) return true;

    auto platform = device.GetPlatform();
    std::string shaderPath = utils::ShaderRegistry::GetShaderPath(platform, "TAA");
    std::string source = LoadShaderSource(shaderPath);
    if (source.empty()) { std::cerr << "[TAA] Shader source empty: " << shaderPath << std::endl; return false; }

    ShaderHandle cs = device.CreateShader(source.data(), source.size(), ShaderStage::Compute, "taa_main");
    if (cs == handles::INVALID_SHADER) { std::cerr << "[TAA] Shader creation failed" << std::endl; return false; }

    // 5 bindings: 0..2 = sampled images (curr, hist, vel), 3 = storage image (output),
    //             4 = uniform buffer (globals).
    // No sampler — sampleBilinear in WGSL uses textureLoad only. Dawn WASM backend
    // has issues with sampler bindings in compute shaders (manifests as OOB crash
    // in EventManager::ProcessEvents at end-of-frame).
    DescriptorSetLayoutBinding bindings[5]{};
    bindings[0] = {0, DescriptorType::SampledImage,    1, ShaderStage::Compute};
    bindings[1] = {1, DescriptorType::SampledImage,    1, ShaderStage::Compute};
    bindings[2] = {2, DescriptorType::SampledImage,    1, ShaderStage::Compute};
    bindings[3] = {3, DescriptorType::StorageImage,    1, ShaderStage::Compute};
    bindings[4] = {4, DescriptorType::UniformBuffer,   1, ShaderStage::Compute};

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

    // Linear sampler no longer needed — sampleBilinear uses textureLoad only.
    // Keeping the field for ABI but not creating/binding the sampler.
    s_LinearSampler = handles::INVALID_SAMPLER;

    // Allocate persistent history textures + params buffers per frame slot
    TextureDesc histDesc;
    histDesc.size = {width, height, 1};
    histDesc.format = DataFormat::RGBA16_Float;
    histDesc.type = TextureType::Texture2D;
    histDesc.mipLevels = 1;
    histDesc.usage = TextureUsage::ShaderResource | TextureUsage::CopyDest;

    for (u32 i = 0; i < MAX_FRAMES; ++i) {
        s_HistoryTex[i] = device.CreateTexture(histDesc);
        s_HistoryInit[i] = false;

        BufferDesc bufDesc{};
        bufDesc.size = sizeof(TAAGlobalsCPU);
        bufDesc.type = BufferType::Constant;
        bufDesc.usage = GPUMemoryUsage::Dynamic;
        bufDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        s_ParamsBuf[i] = device.CreateBuffer(bufDesc);
        s_ParamsMapped[i] = device.MapBuffer(s_ParamsBuf[i], 0, sizeof(TAAGlobalsCPU));

        for (u32 j = 0; j < MAX_SETS_PER_FRAME; ++j) {
            DescriptorSetDesc dsDesc;
            dsDesc.layout = s_DSL;
            s_SetPool[i][j] = device.CreateDescriptorSet(dsDesc);
        }
    }

    return s_Pipeline != handles::INVALID_PIPELINE;
}

const TAAPassData& AddTAAPass(RenderGraph& graph, RGResourceHandle inputHDR,
                              RGResourceHandle velocityTexture,
                              u32 width, u32 height, u32 frameIndex) {
    u32 fi = frameIndex % MAX_FRAMES;
    s_SetIndex[fi] = 0;

    EnsurePipeline(graph.GetDevice(), width, height);

    // First-ever frame: force history invalid for ALL slots to avoid reading garbage
    bool firstEverFrame = (s_LastFrameIndex == ~0u);
    s_LastFrameIndex = frameIndex;

    // Triple-buffered ping-pong matching SSR's pattern: read previous frame's
    // output, write current frame's output. Distinct read/write slots are
    // mandatory — reusing the same slot made the compute pass read the texture
    // as SampledImage while the subsequent BlitTexture wrote it as CopyDest on
    // the same command buffer. That write-after-read within one encoder trips
    // Dawn's WASM asyncify stack and crashes in EventManager::ProcessEvents.
    u32 histReadIdx = (fi + MAX_FRAMES - 1) % MAX_FRAMES;
    bool histValid = s_HistoryInit[histReadIdx] && !firstEverFrame;

    // Update params uniform for this frame slot
    // Update params uniform for this frame slot
    if (s_ParamsMapped[fi]) {
        auto* params = static_cast<TAAGlobalsCPU*>(s_ParamsMapped[fi]);
        params->screenSize = {(f32)width, (f32)height, 1.0f / width, 1.0f / height};
        params->invHistoryValid = histValid ? 0.0f : 1.0f;
        params->_pad0 = 0; params->_pad1 = 0; params->_pad2 = 0;
        graph.GetDevice().SetBufferDirtySize(s_ParamsBuf[fi], sizeof(TAAGlobalsCPU));
    }

    return graph.AddPass<TAAPassData>("TAAPass", RGPassType::Compute, RGPassCategory::PostProcess,
        [&](TAAPassData& data, RenderGraphBuilder& builder) {
            builder.Read(inputHDR, ResourceState::ShaderResource);
            builder.Read(velocityTexture, ResourceState::ShaderResource);

            TextureDesc outDesc;
            outDesc.size = {width, height, 1};
            outDesc.format = DataFormat::RGBA16_Float;
            outDesc.type = TextureType::Texture2D;
            outDesc.mipLevels = 1;
            outDesc.usage = TextureUsage::UnorderedAccess | TextureUsage::ShaderResource | TextureUsage::CopySource;
            data.output = builder.CreateTexture("TAA_Output", outDesc, ResourceState::UnorderedAccess);
        },
        [inputHDR, velocityTexture, width, height, fi, histReadIdx]
         (const TAAPassData& data, RenderGraphContext& context) {
            if (s_Pipeline == handles::INVALID_PIPELINE) {
                std::cerr << "[TAA] Execute abort: pipeline INVALID" << std::endl;
                return;
            }

            auto& device = context.graph->GetDevice();
            auto* cmd = context.cmdBuffer;

            auto* inRes = context.graph->GetResource(inputHDR);
            ResourceHandle inHandle = inRes ? inRes->GetPhysicalHandle() : handles::INVALID_RESOURCE;
            auto* velRes = context.graph->GetResource(velocityTexture);
            ResourceHandle velHandle = velRes ? velRes->GetPhysicalHandle() : handles::INVALID_RESOURCE;
            auto* outRes = context.graph->GetResource(data.output);
            ResourceHandle outHandle = outRes ? outRes->GetPhysicalHandle() : handles::INVALID_RESOURCE;

            ResourceHandle histHandle = s_HistoryTex[histReadIdx];
            // Write target is the current frame's slot — distinct from histHandle
            // (read slot) to avoid write-after-read on the same texture within
            // one command buffer.
            ResourceHandle histWriteHandle = s_HistoryTex[fi];

            if (inHandle == handles::INVALID_RESOURCE ||
                velHandle == handles::INVALID_RESOURCE ||
                outHandle == handles::INVALID_RESOURCE) {
                std::cerr << "[TAA] Execute abort: invalid input resource" << std::endl;
                return;
            }

            if (histHandle == handles::INVALID_RESOURCE) {
                std::cerr << "[TAA] Execute abort: invalid history texture" << std::endl;
                return;
            }
            if (histWriteHandle == handles::INVALID_RESOURCE) {
                std::cerr << "[TAA] Execute abort: invalid history write texture" << std::endl;
                return;
            }

            u32 idx = s_SetIndex[fi]++;
            if (idx >= MAX_SETS_PER_FRAME) { idx = 0; s_SetIndex[fi] = 1; }
            DescriptorSetHandle ds = s_SetPool[fi][idx];

            DescriptorImageInfo currInfo; currInfo.imageView = inHandle;
            DescriptorImageInfo histInfo; histInfo.imageView = histHandle;
            DescriptorImageInfo velInfo;  velInfo.imageView = velHandle;
            DescriptorImageInfo outInfo;  outInfo.imageView = outHandle;
            DescriptorBufferInfo bufInfo; bufInfo.buffer = s_ParamsBuf[fi]; bufInfo.offset = 0;
            bufInfo.range = sizeof(TAAGlobalsCPU);

            WriteDescriptorSet writes[5];
            writes[0].dstSet = ds; writes[0].dstBinding = 0; writes[0].descriptorCount = 1;
            writes[0].descriptorType = DescriptorType::SampledImage; writes[0].imageInfo = &currInfo;
            writes[1].dstSet = ds; writes[1].dstBinding = 1; writes[1].descriptorCount = 1;
            writes[1].descriptorType = DescriptorType::SampledImage; writes[1].imageInfo = &histInfo;
            writes[2].dstSet = ds; writes[2].dstBinding = 2; writes[2].descriptorCount = 1;
            writes[2].descriptorType = DescriptorType::SampledImage; writes[2].imageInfo = &velInfo;
            writes[3].dstSet = ds; writes[3].dstBinding = 3; writes[3].descriptorCount = 1;
            writes[3].descriptorType = DescriptorType::StorageImage; writes[3].imageInfo = &outInfo;
            writes[4].dstSet = ds; writes[4].dstBinding = 4; writes[4].descriptorCount = 1;
            writes[4].descriptorType = DescriptorType::UniformBuffer; writes[4].bufferInfo = &bufInfo;

            device.UpdateDescriptorSets(5, writes);

            cmd->BindComputePipeline(s_Pipeline);
            cmd->BindDescriptorSets(PipelineBindPoint::Compute, s_Layout, 0, 1, &ds, 0, nullptr);
            cmd->Dispatch((width + 7) / 8, (height + 7) / 8, 1);

            // Copy output → current frame's history slot so next frame reads resolved color.
            // BlitTexture is a full-screen copy with no scaling. Destination is
            // the current frame's slot (fi), source is the RG-managed output.
            TextureBlitRegion blitRegion{};
            blitRegion.srcSubresource = {0, 0, 1};
            blitRegion.srcOffsets[0] = {0, 0, 0};
            blitRegion.srcOffsets[1] = {(s32)width, (s32)height, 1};
            blitRegion.dstSubresource = {0, 0, 1};
            blitRegion.dstOffsets[0] = {0, 0, 0};
            blitRegion.dstOffsets[1] = {(s32)width, (s32)height, 1};
            cmd->BlitTexture(outHandle, histWriteHandle, &blitRegion, 1, FilterMode::Nearest);

            s_HistoryInit[fi] = true;
        }
    );
}

void ResetTAAHistory() {
    // Forcing re-init causes next TAA pass to skip blending with the stale
    // history texture and overwrite it with the current frame's HDR.
    for (u32 i = 0u; i < MAX_FRAMES; ++i) {
        s_HistoryInit[i] = false;
    }
    s_LastFrameIndex = ~0u;
}

} // namespace primal::graphics::PostProcess
