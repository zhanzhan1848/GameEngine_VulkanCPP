#include "SSRPass.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/Utils/ShaderRegistry.h"
#include "Graphics/Dawn/ShaderLoader.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <cmath>

namespace primal::graphics::PostProcess {

using namespace rhi;

static constexpr u32 MAX_FRAMES = 3;
static constexpr u32 MAX_SETS = 4;

// === Trace pipeline ===
static PipelineHandle s_TracePipeline = handles::INVALID_PIPELINE;
static PipelineLayoutHandle s_TraceLayout = handles::INVALID_PIPELINE_LAYOUT;
static DescriptorSetLayoutHandle s_TraceDSL = handles::INVALID_RESOURCE;
static DescriptorSetHandle s_TraceSets[MAX_FRAMES][MAX_SETS] = {};
static u32 s_TraceSetIdx[MAX_FRAMES] = {};
static ResourceHandle s_TraceParamsBuf[MAX_FRAMES] = {};
static void* s_TraceParamsMapped[MAX_FRAMES] = {};

// === Temporal pipeline ===
static PipelineHandle s_TemporalPipeline = handles::INVALID_PIPELINE;
static PipelineLayoutHandle s_TemporalLayout = handles::INVALID_PIPELINE_LAYOUT;
static DescriptorSetLayoutHandle s_TemporalDSL = handles::INVALID_RESOURCE;
static DescriptorSetHandle s_TemporalSets[MAX_FRAMES][MAX_SETS] = {};
static u32 s_TemporalSetIdx[MAX_FRAMES] = {};
static ResourceHandle s_TemporalParamsBuf[MAX_FRAMES] = {};
static void* s_TemporalParamsMapped[MAX_FRAMES] = {};

// === Composite pipeline ===
static PipelineHandle s_CompositePipeline = handles::INVALID_PIPELINE;
static PipelineLayoutHandle s_CompositeLayout = handles::INVALID_PIPELINE_LAYOUT;
static DescriptorSetLayoutHandle s_CompositeDSL = handles::INVALID_RESOURCE;
static DescriptorSetHandle s_CompositeSets[MAX_FRAMES][MAX_SETS] = {};
static u32 s_CompositeSetIdx[MAX_FRAMES] = {};
static ResourceHandle s_CompositeParamsBuf[MAX_FRAMES] = {};
static void* s_CompositeParamsMapped[MAX_FRAMES] = {};

// === Persistent textures ===
static ResourceHandle s_TraceTexture = handles::INVALID_RESOURCE;       // half-res RGBA16F
static ResourceHandle s_TemporalTexture = handles::INVALID_RESOURCE;    // half-res RGBA16F (this frame's output)
static ResourceHandle s_TemporalHistory[MAX_FRAMES] = {                 // half-res triple-buffered history
    handles::INVALID_RESOURCE, handles::INVALID_RESOURCE, handles::INVALID_RESOURCE
};

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

static bool EnsurePipelines(RHIDeviceBase& device) {
    if (s_TracePipeline != handles::INVALID_PIPELINE) return true;

    auto platform = device.GetPlatform();

    auto loadShader = [&](const char* name, const char* entry) -> ShaderHandle {
        std::string path = utils::ShaderRegistry::GetShaderPath(platform, name);
        std::string src = LoadShaderSource(path);
        if (src.empty()) {
            std::cerr << "[SSR] Shader empty: " << name << " (path=" << path << ")" << std::endl;
            return handles::INVALID_SHADER;
        }
        ShaderHandle h = device.CreateShader(src.data(), src.size(), ShaderStage::Compute, entry);
        std::cerr << "[SSR] LoadShader " << name << " -> handle=" << static_cast<u64>(h)
                  << " (src=" << src.size() << " bytes)" << std::endl;
        return h;
    };

    ShaderHandle traceCS = loadShader("SSRTrace", "ssr_trace");
    ShaderHandle temporalCS = loadShader("SSRTemporal", "ssr_temporal");
    ShaderHandle compositeCS = loadShader("SSRComposite", "ssr_composite");

    if (traceCS == handles::INVALID_SHADER || temporalCS == handles::INVALID_SHADER ||
        compositeCS == handles::INVALID_SHADER) {
        std::cerr << "[SSR] Shader compilation failed — aborting pipeline creation" << std::endl;
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

    // Trace DSL: depth + hzb + color + output + uniform
    {
        DescriptorSetLayoutBinding bindings[5]{};
        bindings[0] = {0, DescriptorType::SampledDepthImage, 1, ShaderStage::Compute};
        bindings[1] = {1, DescriptorType::SampledImage, 1, ShaderStage::Compute};
        bindings[2] = {2, DescriptorType::SampledImage, 1, ShaderStage::Compute};
        bindings[3] = {3, DescriptorType::StorageImage, 1, ShaderStage::Compute};
        bindings[4] = {4, DescriptorType::UniformBuffer, 1, ShaderStage::Compute};
        bindings[4].minBindingSize = 192;

        DescriptorSetLayoutDesc dslDesc;
        dslDesc.bindingCount = 5;
        dslDesc.bindings = bindings;
        s_TraceDSL = device.CreateDescriptorSetLayout(dslDesc);
        auto [p, l] = createPipeline(traceCS, s_TraceDSL);
        s_TracePipeline = p; s_TraceLayout = l;
    }

    // Temporal DSL: spatial + history + velocity + output + uniform
    {
        DescriptorSetLayoutBinding bindings[5]{};
        bindings[0] = {0, DescriptorType::SampledImage, 1, ShaderStage::Compute};
        bindings[1] = {1, DescriptorType::SampledImage, 1, ShaderStage::Compute};
        bindings[2] = {2, DescriptorType::SampledImage, 1, ShaderStage::Compute};
        bindings[3] = {3, DescriptorType::StorageImage, 1, ShaderStage::Compute};
        bindings[4] = {4, DescriptorType::UniformBuffer, 1, ShaderStage::Compute};
        bindings[4].minBindingSize = 32;

        DescriptorSetLayoutDesc dslDesc;
        dslDesc.bindingCount = 5;
        dslDesc.bindings = bindings;
        s_TemporalDSL = device.CreateDescriptorSetLayout(dslDesc);
        auto [p, l] = createPipeline(temporalCS, s_TemporalDSL);
        s_TemporalPipeline = p; s_TemporalLayout = l;
    }

    // Composite DSL: hdr + ssr + depth + output + uniform
    {
        DescriptorSetLayoutBinding bindings[5]{};
        bindings[0] = {0, DescriptorType::SampledImage, 1, ShaderStage::Compute};
        bindings[1] = {1, DescriptorType::SampledImage, 1, ShaderStage::Compute};
        bindings[2] = {2, DescriptorType::SampledDepthImage, 1, ShaderStage::Compute};
        bindings[3] = {3, DescriptorType::StorageImage, 1, ShaderStage::Compute};
        bindings[4] = {4, DescriptorType::UniformBuffer, 1, ShaderStage::Compute};
        bindings[4].minBindingSize = sizeof(math::m4x4) + 32;  // header + invProj

        DescriptorSetLayoutDesc dslDesc;
        dslDesc.bindingCount = 5;
        dslDesc.bindings = bindings;
        s_CompositeDSL = device.CreateDescriptorSetLayout(dslDesc);
        auto [p, l] = createPipeline(compositeCS, s_CompositeDSL);
        s_CompositePipeline = p; s_CompositeLayout = l;
    }

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
    createBufs(s_TemporalParamsBuf, s_TemporalParamsMapped, 32);
    createBufs(s_CompositeParamsBuf, s_CompositeParamsMapped, sizeof(math::m4x4) + 32);

    auto createSets = [&](DescriptorSetHandle (&pool)[MAX_FRAMES][MAX_SETS], DescriptorSetLayoutHandle dsl) {
        for (u32 i = 0; i < MAX_FRAMES; ++i)
            for (u32 j = 0; j < MAX_SETS; ++j) {
                DescriptorSetDesc dsDesc;
                dsDesc.layout = dsl;
                pool[i][j] = device.CreateDescriptorSet(dsDesc);
            }
    };

    createSets(s_TraceSets, s_TraceDSL);
    createSets(s_TemporalSets, s_TemporalDSL);
    createSets(s_CompositeSets, s_CompositeDSL);

    std::cerr << "[SSR] Pipelines created — trace=" << static_cast<u64>(s_TracePipeline)
              << " temporal=" << static_cast<u64>(s_TemporalPipeline)
              << " composite=" << static_cast<u64>(s_CompositePipeline) << std::endl;

    return true;
}

static void CreatePersistentTextures(RHIDeviceBase& device, u32 width, u32 height) {
    u32 halfW = width / 2;
    u32 halfH = height / 2;

    auto createHalfTex = [&]() -> ResourceHandle {
        TextureDesc desc{};
        desc.size = {halfW, halfH, 1};
        desc.format = DataFormat::RGBA16_Float;
        desc.type = TextureType::Texture2D;
        desc.mipLevels = 1;
        desc.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess |
                     TextureUsage::CopySource | TextureUsage::CopyDest;
        return device.CreateTexture(desc);
    };

    if (s_TraceTexture == handles::INVALID_RESOURCE) s_TraceTexture = createHalfTex();
    if (s_TemporalTexture == handles::INVALID_RESOURCE) s_TemporalTexture = createHalfTex();
    for (u32 i = 0; i < MAX_FRAMES; ++i)
        if (s_TemporalHistory[i] == handles::INVALID_RESOURCE)
            s_TemporalHistory[i] = createHalfTex();
}

const SSRPassData& AddSSRPass(RenderGraph& graph,
    RGResourceHandle hdrTexture,
    RGResourceHandle depthTexture,
    RGResourceHandle hzbTexture,
    RGResourceHandle velocityTexture,
    u32 width, u32 height, u32 frameIndex,
    const math::m4x4& proj, const math::m4x4& invProj) {
    u32 fi = frameIndex % MAX_FRAMES;
    s_TraceSetIdx[fi] = 0;
    s_TemporalSetIdx[fi] = 0;
    s_CompositeSetIdx[fi] = 0;

    static bool s_firstAddCall = true;
    if (s_firstAddCall) {
        s_firstAddCall = false;
        std::cerr << "[SSR] AddSSRPass first call — frame=" << frameIndex
                  << " w=" << width << " h=" << height
                  << " hdrValid=" << (hdrTexture != kInvalidRGResourceHandle)
                  << " depthValid=" << (depthTexture != kInvalidRGResourceHandle)
                  << " hzbValid=" << (hzbTexture != kInvalidRGResourceHandle)
                  << " velValid=" << (velocityTexture != kInvalidRGResourceHandle) << std::endl;
    }

    auto& device = graph.GetDevice();
    EnsurePipelines(device);
    CreatePersistentTextures(device, width, height);

    u32 hzbMipLevels = u32(std::ceil(std::log2(std::max(width, height))));
    u32 halfW = width / 2;
    u32 halfH = height / 2;

    // Triple-buffered history indexing: read from previous frame's slot.
    u32 histIdx = (fi + MAX_FRAMES - 1) % MAX_FRAMES;

    return graph.AddPass<SSRPassData>("SSRPass", RGPassType::Compute, RGPassCategory::PostProcess,
        [&](SSRPassData& data, RenderGraphBuilder& builder) {
            builder.Read(hdrTexture, ResourceState::ShaderResource);
            builder.Read(depthTexture, ResourceState::ShaderResource);
            builder.Read(hzbTexture, ResourceState::ShaderResource);
            builder.Read(velocityTexture, ResourceState::ShaderResource);

            TextureDesc outDesc;
            outDesc.size = {width, height, 1};
            outDesc.format = DataFormat::RGBA16_Float;
            outDesc.type = TextureType::Texture2D;
            outDesc.mipLevels = 1;
            outDesc.usage = TextureUsage::UnorderedAccess | TextureUsage::ShaderResource;
            data.outputColor = builder.CreateTexture("SSR_OutputHDR", outDesc, ResourceState::UnorderedAccess);
        },
        [hdrTexture, depthTexture, hzbTexture, velocityTexture,
         width, height, halfW, halfH, fi, histIdx,
         hzbMipLevels, proj, invProj, frameIndex]
         (const SSRPassData& data, RenderGraphContext& context) {
            static u32 s_execCount = 0;
            bool logThisFrame = (s_execCount < 3u);
            ++s_execCount;

            if (logThisFrame) {
                std::cerr << "[SSR] Execute frame=" << frameIndex
                          << " tracePipe=" << static_cast<u64>(s_TracePipeline)
                          << " temporalPipe=" << static_cast<u64>(s_TemporalPipeline)
                          << " compositePipe=" << static_cast<u64>(s_CompositePipeline) << std::endl;
            }

            if (s_TracePipeline == handles::INVALID_PIPELINE) {
                if (logThisFrame) std::cerr << "[SSR] Execute abort: trace pipeline INVALID" << std::endl;
                return;
            }

            auto& device = context.graph->GetDevice();
            auto* cmd = context.cmdBuffer;
            constexpr u32 TG = 8;

            auto resolveTex = [&](RGResourceHandle h) -> ResourceHandle {
                auto* r = context.graph->GetResource(h);
                return r ? r->GetPhysicalHandle() : handles::INVALID_RESOURCE;
            };

            ResourceHandle hdrHandle = resolveTex(hdrTexture);
            ResourceHandle depthHandle = resolveTex(depthTexture);
            ResourceHandle hzbHandle = resolveTex(hzbTexture);
            ResourceHandle velocityHandle = resolveTex(velocityTexture);
            auto* outRes = context.graph->GetResource(data.outputColor);
            ResourceHandle outPhys = outRes ? outRes->GetPhysicalHandle() : handles::INVALID_RESOURCE;

            if (logThisFrame) {
                std::cerr << "[SSR] Resolved — hdr=" << static_cast<u64>(hdrHandle)
                          << " depth=" << static_cast<u64>(depthHandle)
                          << " hzb=" << static_cast<u64>(hzbHandle)
                          << " vel=" << static_cast<u64>(velocityHandle)
                          << " out=" << static_cast<u64>(outPhys)
                          << " traceTex=" << static_cast<u64>(s_TraceTexture)
                          << " temporalTex=" << static_cast<u64>(s_TemporalTexture)
                          << " histSlot=" << static_cast<u64>(s_TemporalHistory[histIdx]) << std::endl;
            }

            if (hdrHandle == handles::INVALID_RESOURCE || depthHandle == handles::INVALID_RESOURCE ||
                outPhys == handles::INVALID_RESOURCE) {
                if (logThisFrame) std::cerr << "[SSR] Execute abort: invalid resource handle" << std::endl;
                return;
            }

            // ============================================================
            // Sub-pass 1: Trace (half-res)
            // ============================================================
            {
                struct TraceParamsCPU {
                    math::m4x4 invProj;
                    math::m4x4 proj;
                    math::v4    screenSize;
                    math::v4    halfScreenSize;
                    f32         maxDistance;
                    f32         thickness;
                    f32         nearPlane;
                    f32         farPlane;
                    u32         hzbMipLevels;
                    u32         frameIndex;
                };

                if (s_TraceParamsMapped[fi]) {
                    auto* p = static_cast<TraceParamsCPU*>(s_TraceParamsMapped[fi]);
                    p->invProj = invProj;
                    p->proj = proj;
                    p->screenSize = {(f32)width, (f32)height, 1.0f / width, 1.0f / height};
                    p->halfScreenSize = {(f32)halfW, (f32)halfH, 1.0f / halfW, 1.0f / halfH};
                    p->maxDistance = 50.0f;
                    p->thickness = 2.0f;
                    p->nearPlane = 0.1f;
                    p->farPlane = 1000.0f;
                    p->hzbMipLevels = hzbMipLevels;
                    p->frameIndex = frameIndex;
                    device.SetBufferDirtySize(s_TraceParamsBuf[fi], sizeof(TraceParamsCPU));
                }

                u32 idx = s_TraceSetIdx[fi]++;
                if (idx >= MAX_SETS) { idx = 0; s_TraceSetIdx[fi] = 1; }
                DescriptorSetHandle ds = s_TraceSets[fi][idx];

                DescriptorImageInfo depthInfo; depthInfo.imageView = depthHandle;
                DescriptorImageInfo hzbInfo; hzbInfo.imageView = hzbHandle;
                DescriptorImageInfo hdrInfo; hdrInfo.imageView = hdrHandle;
                DescriptorImageInfo outInfo; outInfo.imageView = s_TraceTexture;
                DescriptorBufferInfo bufInfo; bufInfo.buffer = s_TraceParamsBuf[fi]; bufInfo.offset = 0;
                bufInfo.range = sizeof(TraceParamsCPU);

                WriteDescriptorSet writes[5];
                writes[0].dstSet = ds; writes[0].dstBinding = 0; writes[0].descriptorCount = 1;
                writes[0].descriptorType = DescriptorType::SampledDepthImage; writes[0].imageInfo = &depthInfo;
                writes[1].dstSet = ds; writes[1].dstBinding = 1; writes[1].descriptorCount = 1;
                writes[1].descriptorType = DescriptorType::SampledImage; writes[1].imageInfo = &hzbInfo;
                writes[2].dstSet = ds; writes[2].dstBinding = 2; writes[2].descriptorCount = 1;
                writes[2].descriptorType = DescriptorType::SampledImage; writes[2].imageInfo = &hdrInfo;
                writes[3].dstSet = ds; writes[3].dstBinding = 3; writes[3].descriptorCount = 1;
                writes[3].descriptorType = DescriptorType::StorageImage; writes[3].imageInfo = &outInfo;
                writes[4].dstSet = ds; writes[4].dstBinding = 4; writes[4].descriptorCount = 1;
                writes[4].descriptorType = DescriptorType::UniformBuffer; writes[4].bufferInfo = &bufInfo;

                device.UpdateDescriptorSets(5, writes);

                cmd->BindComputePipeline(s_TracePipeline);
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, s_TraceLayout, 0, 1, &ds, 0, nullptr);
                cmd->Dispatch((halfW + TG - 1) / TG, (halfH + TG - 1) / TG, 1);
            }

            // Barrier: trace → SRV
            {
                ResourceBarrier b{};
                b.resource = s_TraceTexture;
                b.beforeState = ResourceState::UnorderedAccess;
                b.afterState = ResourceState::ShaderResource;
                b.subresource = 0xFFFFFFFF;
                cmd->InsertBarrier(&b, 1);
            }

            // ============================================================
            // Sub-pass 2: Temporal (half-res)
            // ============================================================
            if (s_TemporalPipeline != handles::INVALID_PIPELINE &&
                s_TemporalHistory[histIdx] != handles::INVALID_RESOURCE) {
                struct TemporalParamsCPU {
                    f32 feedback;
                    u32 halfWidth;
                    u32 halfHeight;
                    u32 fullWidth;
                    u32 fullHeight;
                    u32 _pad0;
                    u32 _pad1;
                    u32 _pad2;
                };

                if (s_TemporalParamsMapped[fi]) {
                    auto* p = static_cast<TemporalParamsCPU*>(s_TemporalParamsMapped[fi]);
                    p->feedback = 0.8f;
                    p->halfWidth = halfW;
                    p->halfHeight = halfH;
                    p->fullWidth = width;
                    p->fullHeight = height;
                    p->_pad0 = 0; p->_pad1 = 0; p->_pad2 = 0;
                    device.SetBufferDirtySize(s_TemporalParamsBuf[fi], sizeof(TemporalParamsCPU));
                }

                u32 idx = s_TemporalSetIdx[fi]++;
                if (idx >= MAX_SETS) { idx = 0; s_TemporalSetIdx[fi] = 1; }
                DescriptorSetHandle ds = s_TemporalSets[fi][idx];

                DescriptorImageInfo traceInfo; traceInfo.imageView = s_TraceTexture;
                DescriptorImageInfo histInfo; histInfo.imageView = s_TemporalHistory[histIdx];
                DescriptorImageInfo velInfo; velInfo.imageView = velocityHandle;
                DescriptorImageInfo outInfo; outInfo.imageView = s_TemporalTexture;
                DescriptorBufferInfo bufInfo; bufInfo.buffer = s_TemporalParamsBuf[fi]; bufInfo.offset = 0;
                bufInfo.range = sizeof(TemporalParamsCPU);

                WriteDescriptorSet writes[5];
                writes[0].dstSet = ds; writes[0].dstBinding = 0; writes[0].descriptorCount = 1;
                writes[0].descriptorType = DescriptorType::SampledImage; writes[0].imageInfo = &traceInfo;
                writes[1].dstSet = ds; writes[1].dstBinding = 1; writes[1].descriptorCount = 1;
                writes[1].descriptorType = DescriptorType::SampledImage; writes[1].imageInfo = &histInfo;
                writes[2].dstSet = ds; writes[2].dstBinding = 2; writes[2].descriptorCount = 1;
                writes[2].descriptorType = DescriptorType::SampledImage; writes[2].imageInfo = &velInfo;
                writes[3].dstSet = ds; writes[3].dstBinding = 3; writes[3].descriptorCount = 1;
                writes[3].descriptorType = DescriptorType::StorageImage; writes[3].imageInfo = &outInfo;
                writes[4].dstSet = ds; writes[4].dstBinding = 4; writes[4].descriptorCount = 1;
                writes[4].descriptorType = DescriptorType::UniformBuffer; writes[4].bufferInfo = &bufInfo;

                device.UpdateDescriptorSets(5, writes);

                cmd->BindComputePipeline(s_TemporalPipeline);
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, s_TemporalLayout, 0, 1, &ds, 0, nullptr);
                cmd->Dispatch((halfW + TG - 1) / TG, (halfH + TG - 1) / TG, 1);
            }

            // Barrier: temporal → SRV
            {
                ResourceBarrier b{};
                b.resource = s_TemporalTexture;
                b.beforeState = ResourceState::UnorderedAccess;
                b.afterState = ResourceState::ShaderResource;
                b.subresource = 0xFFFFFFFF;
                cmd->InsertBarrier(&b, 1);
            }

            // ============================================================
            // Sub-pass 3: Composite (full-res) — write into RG output texture
            // ============================================================
            {
                struct CompositeParamsCPU {
                    u32 screenWidth;
                    u32 screenHeight;
                    u32 halfWidth;
                    u32 halfHeight;
                    f32 fresnelPower;
                    f32 reflectionStrength;
                    u32 debugMode;
                    u32 _pad1;
                    math::m4x4 invProj;
                };

                static_assert(sizeof(CompositeParamsCPU) <= 256,
                              "Composite params must fit in setBytes limit");

                if (s_CompositeParamsMapped[fi]) {
                    auto* p = static_cast<CompositeParamsCPU*>(s_CompositeParamsMapped[fi]);
                    p->screenWidth = width;
                    p->screenHeight = height;
                    p->halfWidth = halfW;
                    p->halfHeight = halfH;
                    p->fresnelPower = 3.0f;
                    p->reflectionStrength = 0.7f;
                    p->debugMode = 0u;   // normal composite path
                    p->_pad1 = 0;
                    p->invProj = invProj;
                    device.SetBufferDirtySize(s_CompositeParamsBuf[fi], sizeof(CompositeParamsCPU));
                }

                u32 idx = s_CompositeSetIdx[fi]++;
                if (idx >= MAX_SETS) { idx = 0; s_CompositeSetIdx[fi] = 1; }
                DescriptorSetHandle ds = s_CompositeSets[fi][idx];

                DescriptorImageInfo hdrInfo; hdrInfo.imageView = hdrHandle;
                DescriptorImageInfo ssrInfo; ssrInfo.imageView = s_TemporalTexture;
                DescriptorImageInfo depthInfo; depthInfo.imageView = depthHandle;
                DescriptorImageInfo outInfo; outInfo.imageView = outPhys;
                DescriptorBufferInfo bufInfo; bufInfo.buffer = s_CompositeParamsBuf[fi]; bufInfo.offset = 0;
                bufInfo.range = sizeof(CompositeParamsCPU);

                WriteDescriptorSet writes[5];
                writes[0].dstSet = ds; writes[0].dstBinding = 0; writes[0].descriptorCount = 1;
                writes[0].descriptorType = DescriptorType::SampledImage; writes[0].imageInfo = &hdrInfo;
                writes[1].dstSet = ds; writes[1].dstBinding = 1; writes[1].descriptorCount = 1;
                writes[1].descriptorType = DescriptorType::SampledImage; writes[1].imageInfo = &ssrInfo;
                writes[2].dstSet = ds; writes[2].dstBinding = 2; writes[2].descriptorCount = 1;
                writes[2].descriptorType = DescriptorType::SampledDepthImage; writes[2].imageInfo = &depthInfo;
                writes[3].dstSet = ds; writes[3].dstBinding = 3; writes[3].descriptorCount = 1;
                writes[3].descriptorType = DescriptorType::StorageImage; writes[3].imageInfo = &outInfo;
                writes[4].dstSet = ds; writes[4].dstBinding = 4; writes[4].descriptorCount = 1;
                writes[4].descriptorType = DescriptorType::UniformBuffer; writes[4].bufferInfo = &bufInfo;

                device.UpdateDescriptorSets(5, writes);

                cmd->BindComputePipeline(s_CompositePipeline);
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, s_CompositeLayout, 0, 1, &ds, 0, nullptr);
                cmd->Dispatch((width + TG - 1) / TG, (height + TG - 1) / TG, 1);
            }

            // ============================================================
            // Update history: copy temporal output → history slot for next frame
            // ============================================================
            TextureBlitRegion blitRegion{};
            blitRegion.srcSubresource = {0, 0, 1};
            blitRegion.srcOffsets[0] = {0, 0, 0};
            blitRegion.srcOffsets[1] = {(s32)halfW, (s32)halfH, 1};
            blitRegion.dstSubresource = {0, 0, 1};
            blitRegion.dstOffsets[0] = {0, 0, 0};
            blitRegion.dstOffsets[1] = {(s32)halfW, (s32)halfH, 1};
            cmd->BlitTexture(s_TemporalTexture, s_TemporalHistory[fi], &blitRegion, 1, FilterMode::Nearest);

            // Barrier: SSR output → SRV (for downstream passes)
            {
                ResourceBarrier b{};
                b.resource = outPhys;
                b.beforeState = ResourceState::UnorderedAccess;
                b.afterState = ResourceState::ShaderResource;
                b.subresource = 0xFFFFFFFF;
                cmd->InsertBarrier(&b, 1);
            }

            if (logThisFrame) {
                std::cerr << "[SSR] Execute complete frame=" << frameIndex
                          << " dispatched trace+temporal+composite+blit" << std::endl;
            }
        }
    );
}

} // namespace primal::graphics::PostProcess
