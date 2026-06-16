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

    // 6 bindings: 0..2 = sampled images (curr, hist, vel), 3 = storage image (output),
    //             4 = sampler, 5 = uniform buffer (globals)
    DescriptorSetLayoutBinding bindings[6]{};
    bindings[0] = {0, DescriptorType::SampledImage,    1, ShaderStage::Compute};
    bindings[1] = {1, DescriptorType::SampledImage,    1, ShaderStage::Compute};
    bindings[2] = {2, DescriptorType::SampledImage,    1, ShaderStage::Compute};
    bindings[3] = {3, DescriptorType::StorageImage,    1, ShaderStage::Compute};
    bindings[4] = {4, DescriptorType::Sampler,         1, ShaderStage::Compute};
    bindings[5] = {5, DescriptorType::UniformBuffer,   1, ShaderStage::Compute};

    DescriptorSetLayoutDesc dslDesc;
    dslDesc.bindingCount = 6;
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

    std::cerr << "[TAA] EnsurePipeline — shader=" << static_cast<u64>(cs)
              << " dsl=" << static_cast<u64>(s_DSL)
              << " layout=" << static_cast<u64>(s_Layout)
              << " pipeline=" << static_cast<u64>(s_Pipeline) << std::endl;

    // Linear sampler for history sampling
    SamplerDesc samplerDesc;
    samplerDesc.minFilter = FilterMode::Linear;
    samplerDesc.magFilter = FilterMode::Linear;
    samplerDesc.addressU = TextureAddressMode::Clamp;
    samplerDesc.addressV = TextureAddressMode::Clamp;
    samplerDesc.addressW = TextureAddressMode::Clamp;
    samplerDesc.comparisonFunc = ComparisonFunc::Never;
    s_LinearSampler = device.CreateSampler(samplerDesc);

    std::cerr << "[TAA] EnsurePipeline — sampler=" << static_cast<u64>(s_LinearSampler) << std::endl;

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

        std::cerr << "[TAA] EnsurePipeline frame-slot " << i
                  << " — histTex=" << static_cast<u64>(s_HistoryTex[i])
                  << " paramsBuf=" << static_cast<u64>(s_ParamsBuf[i])
                  << " paramsMapped=" << s_ParamsMapped[i]
                  << " (TAAGlobalsCPU=" << sizeof(TAAGlobalsCPU) << " bytes)" << std::endl;

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

    // The current frame READS history slot [fi-1] (last frame's write) and WRITES to slot [fi].
    // But for triple buffering, we use: read from [fi], write to [fi] (overwrites prev frame's data).
    // This is safe because we're 3 frames behind in flight, so by the time fi is read again, GPU
    // has consumed it.
    u32 histReadIdx = fi;
    bool histValid = s_HistoryInit[histReadIdx] && !firstEverFrame;

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
        [inputHDR, velocityTexture, width, height, fi, histReadIdx, histValid]
         (const TAAPassData& data, RenderGraphContext& context) {
            static u32 s_execCount = 0;
            bool logThisFrame = (s_execCount < 3u);
            ++s_execCount;

            if (logThisFrame) {
                std::cerr << "[TAA] Execute fi=" << fi
                          << " pipeline=" << static_cast<u64>(s_Pipeline)
                          << " histValid=" << histValid
                          << " histReadIdx=" << histReadIdx << std::endl;
            }

            if (s_Pipeline == handles::INVALID_PIPELINE) {
                if (logThisFrame) std::cerr << "[TAA] Execute abort: pipeline INVALID" << std::endl;
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

            if (logThisFrame) {
                std::cerr << "[TAA] Resolved — in=" << static_cast<u64>(inHandle)
                          << " vel=" << static_cast<u64>(velHandle)
                          << " out=" << static_cast<u64>(outHandle)
                          << " hist=" << static_cast<u64>(histHandle)
                          << " sampler=" << static_cast<u64>(s_LinearSampler)
                          << " paramsBuf=" << static_cast<u64>(s_ParamsBuf[fi]) << std::endl;
            }

            if (inHandle == handles::INVALID_RESOURCE ||
                velHandle == handles::INVALID_RESOURCE ||
                outHandle == handles::INVALID_RESOURCE) {
                if (logThisFrame) std::cerr << "[TAA] Execute abort: invalid input resource" << std::endl;
                return;
            }

            if (histHandle == handles::INVALID_RESOURCE) {
                if (logThisFrame) std::cerr << "[TAA] Execute abort: invalid history texture" << std::endl;
                return;
            }

            u32 idx = s_SetIndex[fi]++;
            if (idx >= MAX_SETS_PER_FRAME) { idx = 0; s_SetIndex[fi] = 1; }
            DescriptorSetHandle ds = s_SetPool[fi][idx];

            DescriptorImageInfo currInfo; currInfo.imageView = inHandle;
            DescriptorImageInfo histInfo; histInfo.imageView = histHandle;
            DescriptorImageInfo velInfo;  velInfo.imageView = velHandle;
            DescriptorImageInfo outInfo;  outInfo.imageView = outHandle;
            DescriptorImageInfo smpInfo;  smpInfo.sampler = s_LinearSampler;
            DescriptorBufferInfo bufInfo; bufInfo.buffer = s_ParamsBuf[fi]; bufInfo.offset = 0;
            bufInfo.range = sizeof(TAAGlobalsCPU);

            WriteDescriptorSet writes[6];
            writes[0].dstSet = ds; writes[0].dstBinding = 0; writes[0].descriptorCount = 1;
            writes[0].descriptorType = DescriptorType::SampledImage; writes[0].imageInfo = &currInfo;
            writes[1].dstSet = ds; writes[1].dstBinding = 1; writes[1].descriptorCount = 1;
            writes[1].descriptorType = DescriptorType::SampledImage; writes[1].imageInfo = &histInfo;
            writes[2].dstSet = ds; writes[2].dstBinding = 2; writes[2].descriptorCount = 1;
            writes[2].descriptorType = DescriptorType::SampledImage; writes[2].imageInfo = &velInfo;
            writes[3].dstSet = ds; writes[3].dstBinding = 3; writes[3].descriptorCount = 1;
            writes[3].descriptorType = DescriptorType::StorageImage; writes[3].imageInfo = &outInfo;
            writes[4].dstSet = ds; writes[4].dstBinding = 4; writes[4].descriptorCount = 1;
            writes[4].descriptorType = DescriptorType::Sampler; writes[4].imageInfo = &smpInfo;
            writes[5].dstSet = ds; writes[5].dstBinding = 5; writes[5].descriptorCount = 1;
            writes[5].descriptorType = DescriptorType::UniformBuffer; writes[5].bufferInfo = &bufInfo;

            device.UpdateDescriptorSets(6, writes);

            cmd->BindComputePipeline(s_Pipeline);
            cmd->BindDescriptorSets(PipelineBindPoint::Compute, s_Layout, 0, 1, &ds, 0, nullptr);
            cmd->Dispatch((width + 7) / 8, (height + 7) / 8, 1);

            if (logThisFrame) {
                std::cerr << "[TAA] Dispatched compute — ds=" << static_cast<u64>(ds)
                          << " tg=(" << ((width + 7) / 8) << "," << ((height + 7) / 8) << ",1)" << std::endl;
            }

            // Copy output → history slot so next frame reads resolved color.
            // BlitTexture is a full-screen copy with no scaling.
            TextureBlitRegion blitRegion{};
            blitRegion.srcSubresource = {0, 0, 1};
            blitRegion.srcOffsets[0] = {0, 0, 0};
            blitRegion.srcOffsets[1] = {(s32)width, (s32)height, 1};
            blitRegion.dstSubresource = {0, 0, 1};
            blitRegion.dstOffsets[0] = {0, 0, 0};
            blitRegion.dstOffsets[1] = {(s32)width, (s32)height, 1};
            cmd->BlitTexture(outHandle, histHandle, &blitRegion, 1, FilterMode::Nearest);

            if (logThisFrame) {
                std::cerr << "[TAA] BlitTexture queued — out=" << static_cast<u64>(outHandle)
                          << " hist=" << static_cast<u64>(histHandle)
                          << " size=" << width << "x" << height << std::endl;
            }

            s_HistoryInit[histReadIdx] = true;
            (void)histValid; // suppress unused warning
        }
    );
}

} // namespace primal::graphics::PostProcess
