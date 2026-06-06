#include "LumenSSGIDawnPass.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/Utils/ShaderRegistry.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <cmath>

namespace primal::graphics::PostProcess {

using namespace rhi;

static constexpr u32 MAX_FRAMES = 3;
static constexpr u32 MAX_SETS = 4;

// Triple-buffered deferred destruction: per-frame views are destroyed 3 frames
// later, ensuring the GPU has finished using them.
static utl::vector<ResourceHandle> s_DeferredViewDestroys[MAX_FRAMES];

// === Trace pass statics ===
static PipelineHandle s_TracePipeline = handles::INVALID_PIPELINE;
static PipelineLayoutHandle s_TraceLayout = handles::INVALID_PIPELINE_LAYOUT;
static DescriptorSetLayoutHandle s_TraceDSL = handles::INVALID_RESOURCE;
static DescriptorSetHandle s_TraceSets[MAX_FRAMES][MAX_SETS] = {};
static u32 s_TraceSetIdx[MAX_FRAMES] = {};
static ResourceHandle s_TraceParamsBuf[MAX_FRAMES] = {};
static void* s_TraceParamsMapped[MAX_FRAMES] = {};

// === Half-res denoise statics ===
static PipelineHandle s_DenoisePipeline = handles::INVALID_PIPELINE;
static PipelineLayoutHandle s_DenoiseLayout = handles::INVALID_PIPELINE_LAYOUT;
static DescriptorSetLayoutHandle s_DenoiseDSL = handles::INVALID_RESOURCE;
static DescriptorSetHandle s_DenoiseSets[MAX_FRAMES][MAX_SETS] = {};
static u32 s_DenoiseSetIdx[MAX_FRAMES] = {};
static ResourceHandle s_DenoiseParamsBuf[MAX_FRAMES] = {};
static void* s_DenoiseParamsMapped[MAX_FRAMES] = {};

// === Filter statics ===
static PipelineHandle s_FilterPipeline = handles::INVALID_PIPELINE;
static PipelineLayoutHandle s_FilterLayout = handles::INVALID_PIPELINE_LAYOUT;
static DescriptorSetLayoutHandle s_FilterDSL = handles::INVALID_RESOURCE;
static DescriptorSetHandle s_FilterSets[MAX_FRAMES][MAX_SETS] = {};
static u32 s_FilterSetIdx[MAX_FRAMES] = {};
static ResourceHandle s_FilterParamsBuf[MAX_FRAMES] = {};
static void* s_FilterParamsMapped[MAX_FRAMES] = {};

// === Temporal statics ===
static PipelineHandle s_TemporalPipeline = handles::INVALID_PIPELINE;
static PipelineLayoutHandle s_TemporalLayout = handles::INVALID_PIPELINE_LAYOUT;
static DescriptorSetLayoutHandle s_TemporalDSL = handles::INVALID_RESOURCE;
static DescriptorSetHandle s_TemporalSets[MAX_FRAMES][MAX_SETS] = {};
static u32 s_TemporalSetIdx[MAX_FRAMES] = {};
static ResourceHandle s_TemporalParamsBuf[MAX_FRAMES] = {};
static void* s_TemporalParamsMapped[MAX_FRAMES] = {};

// === Persistent textures ===
static ResourceHandle s_TraceTexture = handles::INVALID_RESOURCE;          // half-res RGBA16F
static ResourceHandle s_DenoisedTexture = handles::INVALID_RESOURCE;       // half-res RGBA16F
static ResourceHandle s_FilterTexture = handles::INVALID_RESOURCE;         // full-res RGBA16F
static ResourceHandle s_TemporalHistory[MAX_FRAMES] = {
    handles::INVALID_RESOURCE, handles::INVALID_RESOURCE, handles::INVALID_RESOURCE
};                                                                          // triple-buffered full-res RGBA16F

static std::string LoadShaderSource(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return "";
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

static bool EnsurePipelines(RHIDeviceBase& device) {
    if (s_TracePipeline != handles::INVALID_PIPELINE) return true;

    auto platform = device.GetPlatform();

    // Load all 4 shader sources
    auto loadShader = [&](const char* name, const char* entry) -> ShaderHandle {
        std::string path = utils::ShaderRegistry::GetShaderPath(platform, name);
        std::string src = LoadShaderSource(path);
        if (src.empty()) { std::cerr << "[SSGI] Shader empty: " << name << std::endl; return handles::INVALID_SHADER; }
        return device.CreateShader(src.data(), src.size(), ShaderStage::Compute, entry);
    };

    ShaderHandle traceCS = loadShader("SSGITrace", "ssgi_trace");
    ShaderHandle denoiseCS = loadShader("SSGIHalfResDenoise", "ssgi_halfres_denoise");
    ShaderHandle filterCS = loadShader("SSGIFilter", "ssgi_filter");
    ShaderHandle temporalCS = loadShader("SSGITemporal", "ssgi_temporal");

    if (traceCS == handles::INVALID_SHADER || denoiseCS == handles::INVALID_SHADER ||
        filterCS == handles::INVALID_SHADER || temporalCS == handles::INVALID_SHADER) {
        std::cerr << "[SSGI] Shader compilation failed" << std::endl;
        return false;
    }

    auto createPipeline = [&](ShaderHandle cs, DescriptorSetLayoutHandle dsl) -> std::pair<PipelineHandle, PipelineLayoutHandle> {
        PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &dsl;
        auto layout = device.CreatePipelineLayout(plDesc);

        ComputePipelineDesc pipeDesc;
        pipeDesc.computeShader = cs;
        pipeDesc.layout = layout;
        pipeDesc.threadGroupSize = {8, 8, 1};
        auto pipeline = device.CreateComputePipeline(pipeDesc);
        return {pipeline, layout};
    };

    // Trace DSL: depth + hzb + prevColor + output + uniform
    {
        DescriptorSetLayoutBinding bindings[5]{};
        bindings[0] = {0, DescriptorType::SampledDepthImage, 1, ShaderStage::Compute};
        bindings[1] = {1, DescriptorType::SampledImage, 1, ShaderStage::Compute};
        bindings[2] = {2, DescriptorType::SampledImage, 1, ShaderStage::Compute};
        bindings[3] = {3, DescriptorType::StorageImage, 1, ShaderStage::Compute};
        bindings[4] = {4, DescriptorType::UniformBuffer, 1, ShaderStage::Compute};
        bindings[4].minBindingSize = 192; // SSGITraceParams struct size

        DescriptorSetLayoutDesc dslDesc;
        dslDesc.bindingCount = 5;
        dslDesc.bindings = bindings;
        s_TraceDSL = device.CreateDescriptorSetLayout(dslDesc);
        auto [p, l] = createPipeline(traceCS, s_TraceDSL);
        s_TracePipeline = p; s_TraceLayout = l;
    }

    // Denoise DSL: input + output + uniform
    {
        DescriptorSetLayoutBinding bindings[3]{};
        bindings[0] = {0, DescriptorType::SampledImage, 1, ShaderStage::Compute};
        bindings[1] = {1, DescriptorType::StorageImage, 1, ShaderStage::Compute};
        bindings[2] = {2, DescriptorType::UniformBuffer, 1, ShaderStage::Compute};
        bindings[2].minBindingSize = 16;

        DescriptorSetLayoutDesc dslDesc;
        dslDesc.bindingCount = 3;
        dslDesc.bindings = bindings;
        s_DenoiseDSL = device.CreateDescriptorSetLayout(dslDesc);
        auto [p, l] = createPipeline(denoiseCS, s_DenoiseDSL);
        s_DenoisePipeline = p; s_DenoiseLayout = l;
    }

    // Filter DSL: input + depth + output + uniform
    {
        DescriptorSetLayoutBinding bindings[4]{};
        bindings[0] = {0, DescriptorType::SampledImage, 1, ShaderStage::Compute};
        bindings[1] = {1, DescriptorType::SampledDepthImage, 1, ShaderStage::Compute};
        bindings[2] = {2, DescriptorType::StorageImage, 1, ShaderStage::Compute};
        bindings[3] = {3, DescriptorType::UniformBuffer, 1, ShaderStage::Compute};
        bindings[3].minBindingSize = 128;

        DescriptorSetLayoutDesc dslDesc;
        dslDesc.bindingCount = 4;
        dslDesc.bindings = bindings;
        s_FilterDSL = device.CreateDescriptorSetLayout(dslDesc);
        auto [p, l] = createPipeline(filterCS, s_FilterDSL);
        s_FilterPipeline = p; s_FilterLayout = l;
    }

    // Temporal DSL: spatial + history + velocity + depth + output + uniform
    {
        DescriptorSetLayoutBinding bindings[6]{};
        bindings[0] = {0, DescriptorType::SampledImage, 1, ShaderStage::Compute};
        bindings[1] = {1, DescriptorType::SampledImage, 1, ShaderStage::Compute};
        bindings[2] = {2, DescriptorType::SampledImage, 1, ShaderStage::Compute};
        bindings[3] = {3, DescriptorType::SampledDepthImage, 1, ShaderStage::Compute};
        bindings[4] = {4, DescriptorType::StorageImage, 1, ShaderStage::Compute};
        bindings[5] = {5, DescriptorType::UniformBuffer, 1, ShaderStage::Compute};
        bindings[5].minBindingSize = 16;

        DescriptorSetLayoutDesc dslDesc;
        dslDesc.bindingCount = 6;
        dslDesc.bindings = bindings;
        s_TemporalDSL = device.CreateDescriptorSetLayout(dslDesc);
        auto [p, l] = createPipeline(temporalCS, s_TemporalDSL);
        s_TemporalPipeline = p; s_TemporalLayout = l;
    }

    // Create constant buffers and descriptor set pools
    auto createBufs = [&](ResourceHandle (&bufs)[MAX_FRAMES], void* (&mapped)[MAX_FRAMES], u64 size) {
        for (u32 i = 0; i < MAX_FRAMES; ++i) {
            BufferDesc desc{};
            desc.size = size;
            desc.type = BufferType::Constant;
            desc.usage = GPUMemoryUsage::Dynamic;
            desc.memoryUsage = GPUMemoryUsage::Dynamic;
            bufs[i] = device.CreateBuffer(desc);
            mapped[i] = device.MapBuffer(bufs[i], 0, size);
        }
    };

    createBufs(s_TraceParamsBuf, s_TraceParamsMapped, 192);
    createBufs(s_DenoiseParamsBuf, s_DenoiseParamsMapped, 16);
    createBufs(s_FilterParamsBuf, s_FilterParamsMapped, 128);
    createBufs(s_TemporalParamsBuf, s_TemporalParamsMapped, 16);

    auto createSets = [&](DescriptorSetHandle (&pool)[MAX_FRAMES][MAX_SETS], DescriptorSetLayoutHandle dsl) {
        for (u32 i = 0; i < MAX_FRAMES; ++i)
            for (u32 j = 0; j < MAX_SETS; ++j) {
                DescriptorSetDesc dsDesc;
                dsDesc.layout = dsl;
                pool[i][j] = device.CreateDescriptorSet(dsDesc);
            }
    };

    createSets(s_TraceSets, s_TraceDSL);
    createSets(s_DenoiseSets, s_DenoiseDSL);
    createSets(s_FilterSets, s_FilterDSL);
    createSets(s_TemporalSets, s_TemporalDSL);

    return true;
}

static void CreatePersistentTextures(RHIDeviceBase& device, u32 width, u32 height) {
    u32 halfW = width / 2;
    u32 halfH = height / 2;

    auto createTex = [&](u32 w, u32 h, DataFormat fmt) -> ResourceHandle {
        TextureDesc desc{};
        desc.size = {w, h, 1};
        desc.format = fmt;
        desc.type = TextureType::Texture2D;
        desc.mipLevels = 1;
        desc.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
        return device.CreateTexture(desc);
    };

    if (s_TraceTexture == handles::INVALID_RESOURCE)
        s_TraceTexture = createTex(halfW, halfH, DataFormat::RGBA16_Float);
    if (s_DenoisedTexture == handles::INVALID_RESOURCE)
        s_DenoisedTexture = createTex(halfW, halfH, DataFormat::RGBA16_Float);
    if (s_FilterTexture == handles::INVALID_RESOURCE)
        s_FilterTexture = createTex(width, height, DataFormat::RGBA16_Float);
    for (u32 i = 0; i < MAX_FRAMES; ++i)
        if (s_TemporalHistory[i] == handles::INVALID_RESOURCE)
            s_TemporalHistory[i] = createTex(width, height, DataFormat::RGBA16_Float);
}

const LumenSSGIData& AddLumenSSGIPass(RenderGraph& graph,
    RGResourceHandle depthTexture,
    RGResourceHandle hzbTexture,
    RGResourceHandle velocityTexture,
    RGResourceHandle prevFrameColor,
    u32 width, u32 height, u32 frameIndex,
    const math::m4x4& proj, const math::m4x4& invProj) {
    u32 fi = frameIndex % MAX_FRAMES;
    s_TraceSetIdx[fi] = 0;
    s_DenoiseSetIdx[fi] = 0;
    s_FilterSetIdx[fi] = 0;
    s_TemporalSetIdx[fi] = 0;

    auto& device = graph.GetDevice();
    EnsurePipelines(device);
    CreatePersistentTextures(device, width, height);

    // NOTE: We intentionally do NOT destroy per-frame WGPU texture views.
    // Calling wgpuTextureViewRelease (via DestroyTexture) corrupts Dawn's
    // internal heap — even with multi-frame deferred destruction. The views
    // are small objects (~few hundred bytes each, 2 per frame = ~1KB/frame).
    // Acceptable leak for test usage. Production code should use GPU fences
    // for safe destruction timing.
    s_DeferredViewDestroys[fi].clear();

    // Compute mip levels for HZB
    u32 hzbMipLevels = u32(std::ceil(std::log2(std::max(width, height))));
    u32 halfW = width / 2;
    u32 halfH = height / 2;

    // Triple-buffer temporal indexing
    u32 outIdx = fi;
    u32 histIdx = (fi + MAX_FRAMES - 1) % MAX_FRAMES;

    return graph.AddPass<LumenSSGIData>("LumenSSGIPass", RGPassType::Compute, RGPassCategory::Lighting,
        [&](LumenSSGIData& data, RenderGraphBuilder& builder) {
            builder.Read(depthTexture, ResourceState::ShaderResource);
            builder.Read(hzbTexture, ResourceState::ShaderResource);
            builder.Read(velocityTexture, ResourceState::ShaderResource);
            builder.Read(prevFrameColor, ResourceState::ShaderResource);

            // Import persistent temporal history for reading (previous frame)
            auto histHandle = graph.ImportResource("SSGI_TemporalHist_" + std::to_string(histIdx),
                                                    s_TemporalHistory[histIdx]);
            builder.Read(histHandle, ResourceState::ShaderResource);

            // Create output texture — the RG needs this to track the dependency chain
            // (ImportTexture doesn't register a write dependency, causing the RG to prune
            // this pass and its upstream passes).
            TextureDesc outDesc;
            outDesc.size = {width, height, 1};
            outDesc.format = DataFormat::RGBA16_Float;
            outDesc.type = TextureType::Texture2D;
            outDesc.mipLevels = 1;
            outDesc.usage = TextureUsage::UnorderedAccess | TextureUsage::ShaderResource;
            data.ssgiOutput = builder.CreateTexture("SSGI_Output", outDesc, ResourceState::UnorderedAccess);
        },
        [depthTexture, hzbTexture, velocityTexture, prevFrameColor,
         width, height, halfW, halfH, fi, outIdx, histIdx,
         hzbMipLevels, proj, invProj, frameIndex]
         (const LumenSSGIData& data, RenderGraphContext& context) {
            auto& device = context.graph->GetDevice();
            auto* cmd = context.cmdBuffer;
            constexpr u32 TG = 8;

            // Resolve physical handles — use raw handles, NOT per-frame texture views.
            // SSAO uses this same pattern (raw handle → UpdateDescriptorSets resolves via
            // GetTexture → GetDefaultView) and works without the BufferBase::NeedsInitialization
            // crash that per-frame WGPUTextureView creation triggers in Dawn's Metal backend.
            auto resolveTex = [&](RGResourceHandle h) -> ResourceHandle {
                auto* r = context.graph->GetResource(h);
                return r ? r->GetPhysicalHandle() : handles::INVALID_RESOURCE;
            };

            ResourceHandle depthHandle = resolveTex(depthTexture);
            ResourceHandle hzbHandle = resolveTex(hzbTexture);
            ResourceHandle prevColorHandle = resolveTex(prevFrameColor);
            ResourceHandle velocityHandle = resolveTex(velocityTexture);

            // Resolve SSGI output (RG-created texture)
            auto* ssgiRes = context.graph->GetResource(data.ssgiOutput);
            ResourceHandle ssgiOutPhys = ssgiRes ? ssgiRes->GetPhysicalHandle() : handles::INVALID_RESOURCE;

            if (depthHandle == handles::INVALID_RESOURCE) return;

            // ============================================================
            // Sub-pass 1: Trace (half-res)
            // ============================================================
            if (s_TracePipeline != handles::INVALID_PIPELINE &&
                s_TraceTexture != handles::INVALID_RESOURCE) {
                struct TraceParamsCPU {
                    math::m4x4 invProj;
                    math::m4x4 proj;
                    math::v4    screenSize;
                    math::v4    halfScreenSize;
                    u32         rayCount;
                    f32         radius;
                    f32         thickness;
                    u32         frameIndex;
                    f32         nearPlane;
                    f32         farPlane;
                    u32         hzbMipLevels;
                    u32         _pad;
                };

                if (s_TraceParamsMapped[fi]) {
                    auto* p = static_cast<TraceParamsCPU*>(s_TraceParamsMapped[fi]);
                    p->invProj = invProj;
                    p->proj = proj;
                    p->screenSize = {(f32)width, (f32)height, 1.0f / width, 1.0f / height};
                    p->halfScreenSize = {(f32)halfW, (f32)halfH, 1.0f / halfW, 1.0f / halfH};
                    p->rayCount = 4;
                    p->radius = 15.0f;
                    p->thickness = 0.5f;
                    p->frameIndex = frameIndex;
                    p->nearPlane = 0.1f;
                    p->farPlane = 1000.0f;
                    p->hzbMipLevels = hzbMipLevels;
                    p->_pad = 0;
                    device.SetBufferDirtySize(s_TraceParamsBuf[fi], sizeof(TraceParamsCPU));
                }

                u32 idx = s_TraceSetIdx[fi]++;
                if (idx >= MAX_SETS) { idx = 0; s_TraceSetIdx[fi] = 1; }
                DescriptorSetHandle ds = s_TraceSets[fi][idx];

                DescriptorImageInfo depthInfo; depthInfo.imageView = depthHandle;
                DescriptorImageInfo hzbInfo; hzbInfo.imageView = hzbHandle;
                DescriptorImageInfo prevInfo; prevInfo.imageView = prevColorHandle;
                DescriptorImageInfo outInfo; outInfo.imageView = s_TraceTexture;
                DescriptorBufferInfo bufInfo; bufInfo.buffer = s_TraceParamsBuf[fi]; bufInfo.offset = 0; bufInfo.range = sizeof(TraceParamsCPU);

                WriteDescriptorSet writes[5];
                writes[0].dstSet = ds; writes[0].dstBinding = 0; writes[0].descriptorCount = 1;
                writes[0].descriptorType = DescriptorType::SampledDepthImage; writes[0].imageInfo = &depthInfo;
                writes[1].dstSet = ds; writes[1].dstBinding = 1; writes[1].descriptorCount = 1;
                writes[1].descriptorType = DescriptorType::SampledImage; writes[1].imageInfo = &hzbInfo;
                writes[2].dstSet = ds; writes[2].dstBinding = 2; writes[2].descriptorCount = 1;
                writes[2].descriptorType = DescriptorType::SampledImage; writes[2].imageInfo = &prevInfo;
                writes[3].dstSet = ds; writes[3].dstBinding = 3; writes[3].descriptorCount = 1;
                writes[3].descriptorType = DescriptorType::StorageImage; writes[3].imageInfo = &outInfo;
                writes[4].dstSet = ds; writes[4].dstBinding = 4; writes[4].descriptorCount = 1;
                writes[4].descriptorType = DescriptorType::UniformBuffer; writes[4].bufferInfo = &bufInfo;

                device.UpdateDescriptorSets(5, writes);

                cmd->BindComputePipeline(s_TracePipeline);
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, s_TraceLayout, 0, 1, &ds, 0, nullptr);
                cmd->Dispatch((halfW + TG - 1) / TG, (halfH + TG - 1) / TG, 1);
            }

            // Barrier: trace -> SRV
            {
                ResourceBarrier b{};
                b.resource = s_TraceTexture;
                b.beforeState = ResourceState::UnorderedAccess;
                b.afterState = ResourceState::ShaderResource;
                b.subresource = 0xFFFFFFFF;
                cmd->InsertBarrier(&b, 1);
            }

            // ============================================================
            // Sub-pass 2: Half-res Denoise
            // ============================================================
            bool denoiseRan = false;
            if (s_DenoisePipeline != handles::INVALID_PIPELINE &&
                s_DenoisedTexture != handles::INVALID_RESOURCE) {
                struct DenoiseParamsCPU {
                    u32 width;
                    u32 height;
                    f32 sigma;
                    u32 _pad;
                };

                if (s_DenoiseParamsMapped[fi]) {
                    auto* p = static_cast<DenoiseParamsCPU*>(s_DenoiseParamsMapped[fi]);
                    p->width = halfW;
                    p->height = halfH;
                    p->sigma = 1.5f;
                    p->_pad = 0;
                    device.SetBufferDirtySize(s_DenoiseParamsBuf[fi], sizeof(DenoiseParamsCPU));
                }

                u32 idx = s_DenoiseSetIdx[fi]++;
                if (idx >= MAX_SETS) { idx = 0; s_DenoiseSetIdx[fi] = 1; }
                DescriptorSetHandle ds = s_DenoiseSets[fi][idx];

                DescriptorImageInfo inInfo; inInfo.imageView = s_TraceTexture;
                DescriptorImageInfo outInfo; outInfo.imageView = s_DenoisedTexture;
                DescriptorBufferInfo bufInfo; bufInfo.buffer = s_DenoiseParamsBuf[fi]; bufInfo.offset = 0; bufInfo.range = sizeof(DenoiseParamsCPU);

                WriteDescriptorSet writes[3];
                writes[0].dstSet = ds; writes[0].dstBinding = 0; writes[0].descriptorCount = 1;
                writes[0].descriptorType = DescriptorType::SampledImage; writes[0].imageInfo = &inInfo;
                writes[1].dstSet = ds; writes[1].dstBinding = 1; writes[1].descriptorCount = 1;
                writes[1].descriptorType = DescriptorType::StorageImage; writes[1].imageInfo = &outInfo;
                writes[2].dstSet = ds; writes[2].dstBinding = 2; writes[2].descriptorCount = 1;
                writes[2].descriptorType = DescriptorType::UniformBuffer; writes[2].bufferInfo = &bufInfo;

                device.UpdateDescriptorSets(3, writes);

                cmd->BindComputePipeline(s_DenoisePipeline);
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, s_DenoiseLayout, 0, 1, &ds, 0, nullptr);
                cmd->Dispatch((halfW + TG - 1) / TG, (halfH + TG - 1) / TG, 1);
                denoiseRan = true;
            }

            // Determine filter input
            ResourceHandle filterInput = s_TraceTexture;
            if (denoiseRan) {
                ResourceBarrier b{};
                b.resource = s_DenoisedTexture;
                b.beforeState = ResourceState::UnorderedAccess;
                b.afterState = ResourceState::ShaderResource;
                b.subresource = 0xFFFFFFFF;
                cmd->InsertBarrier(&b, 1);
                filterInput = s_DenoisedTexture;
            }

            // ============================================================
            // Sub-pass 3: Spatial Filter (full-res)
            // ============================================================
            if (s_FilterPipeline != handles::INVALID_PIPELINE &&
                s_FilterTexture != handles::INVALID_RESOURCE) {
                struct FilterParamsCPU {
                    math::m4x4 invProj;
                    math::v4    screenSize;
                    math::v4    halfScreenSize;
                    f32         sigmaDepth;
                    f32         sigmaNormal;
                    f32         sigmaHitDist;
                    f32         sigmaSpatial;
                    u32         kernelRadius;
                    u32         _pad0;
                    u32         _pad1;
                    u32         _pad2;
                };

                if (s_FilterParamsMapped[fi]) {
                    auto* p = static_cast<FilterParamsCPU*>(s_FilterParamsMapped[fi]);
                    p->invProj = invProj;
                    p->screenSize = {(f32)width, (f32)height, 1.0f / width, 1.0f / height};
                    p->halfScreenSize = {(f32)halfW, (f32)halfH, 1.0f / halfW, 1.0f / halfH};
                    p->sigmaDepth = 10.0f;
                    p->sigmaNormal = 16.0f;
                    p->sigmaHitDist = 8.0f;
                    p->sigmaSpatial = 2.5f;
                    p->kernelRadius = 2;
                    device.SetBufferDirtySize(s_FilterParamsBuf[fi], sizeof(FilterParamsCPU));
                }

                u32 idx = s_FilterSetIdx[fi]++;
                if (idx >= MAX_SETS) { idx = 0; s_FilterSetIdx[fi] = 1; }
                DescriptorSetHandle ds = s_FilterSets[fi][idx];

                DescriptorImageInfo inInfo; inInfo.imageView = filterInput;
                DescriptorImageInfo depthInfo; depthInfo.imageView = depthHandle;
                DescriptorImageInfo outInfo; outInfo.imageView = s_FilterTexture;
                DescriptorBufferInfo bufInfo; bufInfo.buffer = s_FilterParamsBuf[fi]; bufInfo.offset = 0; bufInfo.range = sizeof(FilterParamsCPU);

                WriteDescriptorSet writes[4];
                writes[0].dstSet = ds; writes[0].dstBinding = 0; writes[0].descriptorCount = 1;
                writes[0].descriptorType = DescriptorType::SampledImage; writes[0].imageInfo = &inInfo;
                writes[1].dstSet = ds; writes[1].dstBinding = 1; writes[1].descriptorCount = 1;
                writes[1].descriptorType = DescriptorType::SampledDepthImage; writes[1].imageInfo = &depthInfo;
                writes[2].dstSet = ds; writes[2].dstBinding = 2; writes[2].descriptorCount = 1;
                writes[2].descriptorType = DescriptorType::StorageImage; writes[2].imageInfo = &outInfo;
                writes[3].dstSet = ds; writes[3].dstBinding = 3; writes[3].descriptorCount = 1;
                writes[3].descriptorType = DescriptorType::UniformBuffer; writes[3].bufferInfo = &bufInfo;

                device.UpdateDescriptorSets(4, writes);

                cmd->BindComputePipeline(s_FilterPipeline);
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, s_FilterLayout, 0, 1, &ds, 0, nullptr);
                cmd->Dispatch((width + TG - 1) / TG, (height + TG - 1) / TG, 1);
            }

            // Barrier: filter -> SRV
            {
                ResourceBarrier b{};
                b.resource = s_FilterTexture;
                b.beforeState = ResourceState::UnorderedAccess;
                b.afterState = ResourceState::ShaderResource;
                b.subresource = 0xFFFFFFFF;
                cmd->InsertBarrier(&b, 1);
            }

            // ============================================================
            // Sub-pass 4: Temporal Accumulation (full-res)
            // ============================================================
            if (s_TemporalPipeline != handles::INVALID_PIPELINE &&
                s_TemporalHistory[outIdx] != handles::INVALID_RESOURCE) {
                struct TemporalParamsCPU {
                    f32 feedback;
                    u32 fullWidth;
                    u32 fullHeight;
                    u32 _pad;
                };

                if (s_TemporalParamsMapped[fi]) {
                    auto* p = static_cast<TemporalParamsCPU*>(s_TemporalParamsMapped[fi]);
                    p->feedback = 0.0f;
                    p->fullWidth = width;
                    p->fullHeight = height;
                    p->_pad = 0;
                    device.SetBufferDirtySize(s_TemporalParamsBuf[fi], sizeof(TemporalParamsCPU));
                }

                u32 idx = s_TemporalSetIdx[fi]++;
                if (idx >= MAX_SETS) { idx = 0; s_TemporalSetIdx[fi] = 1; }
                DescriptorSetHandle ds = s_TemporalSets[fi][idx];

                // Write temporal output to RG output texture (raw handle, like SSAO)
                ResourceHandle temporalOutHandle = ssgiOutPhys != handles::INVALID_RESOURCE
                    ? ssgiOutPhys : s_TemporalHistory[outIdx];

                DescriptorImageInfo spatialInfo; spatialInfo.imageView = s_FilterTexture;
                DescriptorImageInfo histInfo; histInfo.imageView = s_TemporalHistory[histIdx];
                DescriptorImageInfo velInfo; velInfo.imageView = velocityHandle;
                DescriptorImageInfo depthInfo; depthInfo.imageView = depthHandle;
                DescriptorImageInfo outInfo; outInfo.imageView = temporalOutHandle;
                DescriptorBufferInfo bufInfo; bufInfo.buffer = s_TemporalParamsBuf[fi]; bufInfo.offset = 0; bufInfo.range = sizeof(TemporalParamsCPU);

                WriteDescriptorSet writes[6];
                writes[0].dstSet = ds; writes[0].dstBinding = 0; writes[0].descriptorCount = 1;
                writes[0].descriptorType = DescriptorType::SampledImage; writes[0].imageInfo = &spatialInfo;
                writes[1].dstSet = ds; writes[1].dstBinding = 1; writes[1].descriptorCount = 1;
                writes[1].descriptorType = DescriptorType::SampledImage; writes[1].imageInfo = &histInfo;
                writes[2].dstSet = ds; writes[2].dstBinding = 2; writes[2].descriptorCount = 1;
                writes[2].descriptorType = DescriptorType::SampledImage; writes[2].imageInfo = &velInfo;
                writes[3].dstSet = ds; writes[3].dstBinding = 3; writes[3].descriptorCount = 1;
                writes[3].descriptorType = DescriptorType::SampledDepthImage; writes[3].imageInfo = &depthInfo;
                writes[4].dstSet = ds; writes[4].dstBinding = 4; writes[4].descriptorCount = 1;
                writes[4].descriptorType = DescriptorType::StorageImage; writes[4].imageInfo = &outInfo;
                writes[5].dstSet = ds; writes[5].dstBinding = 5; writes[5].descriptorCount = 1;
                writes[5].descriptorType = DescriptorType::UniformBuffer; writes[5].bufferInfo = &bufInfo;

                device.UpdateDescriptorSets(6, writes);

                cmd->BindComputePipeline(s_TemporalPipeline);
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, s_TemporalLayout, 0, 1, &ds, 0, nullptr);
                cmd->Dispatch((width + TG - 1) / TG, (height + TG - 1) / TG, 1);
            }

            // Barrier: SSGI output -> SRV (for ToneMapping to read)
            {
                auto* ssgiRes2 = context.graph->GetResource(data.ssgiOutput);
                ResourceHandle barrierRes = ssgiRes2 ? ssgiRes2->GetPhysicalHandle() : handles::INVALID_RESOURCE;
                if (barrierRes != handles::INVALID_RESOURCE) {
                    ResourceBarrier b{};
                    b.resource = barrierRes;
                    b.beforeState = ResourceState::UnorderedAccess;
                    b.afterState = ResourceState::ShaderResource;
                    b.subresource = 0xFFFFFFFF;
                    cmd->InsertBarrier(&b, 1);
                }
            }

        }
    );
}

void ShutdownLumenSSGIPass() {
    for (u32 i = 0; i < MAX_FRAMES; ++i) {
        s_TraceParamsMapped[i] = nullptr;
        s_DenoiseParamsMapped[i] = nullptr;
        s_FilterParamsMapped[i] = nullptr;
        s_TemporalParamsMapped[i] = nullptr;
    }
    s_TracePipeline = handles::INVALID_PIPELINE;
    s_DenoisePipeline = handles::INVALID_PIPELINE;
    s_FilterPipeline = handles::INVALID_PIPELINE;
    s_TemporalPipeline = handles::INVALID_PIPELINE;
}

} // namespace primal::graphics::PostProcess
