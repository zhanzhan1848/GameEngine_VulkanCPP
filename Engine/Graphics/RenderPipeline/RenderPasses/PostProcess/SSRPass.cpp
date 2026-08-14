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

// === Upsample (was Composite) pipeline ===
static PipelineHandle s_UpsamplePipeline = handles::INVALID_PIPELINE;
static PipelineLayoutHandle s_UpsampleLayout = handles::INVALID_PIPELINE_LAYOUT;
static DescriptorSetLayoutHandle s_UpsampleDSL = handles::INVALID_RESOURCE;
static DescriptorSetHandle s_UpsampleSets[MAX_FRAMES][MAX_SETS] = {};
static u32 s_UpsampleSetIdx[MAX_FRAMES] = {};
static ResourceHandle s_UpsampleParamsBuf[MAX_FRAMES] = {};
static void* s_UpsampleParamsMapped[MAX_FRAMES] = {};

// === Persistent textures ===
static ResourceHandle s_TraceTexture = handles::INVALID_RESOURCE;       // half-res RGBA16F
static ResourceHandle s_TemporalTexture = handles::INVALID_RESOURCE;    // half-res RGBA16F (this frame's output)
static ResourceHandle s_TemporalHistory[MAX_FRAMES] = {                 // half-res triple-buffered history
    handles::INVALID_RESOURCE, handles::INVALID_RESOURCE, handles::INVALID_RESOURCE
};

// ============================================================================
// Shader loading — platform-branched.
//   Vulkan: load precompiled .spv bytecode (Vulkan has no runtime GLSL compile).
//   Metal/Dawn: load source text for runtime compilation.
// ============================================================================

// Load a shader as either binary SPIR-V (Vulkan) or source text (Metal/Dawn).
// For Vulkan, returns the raw bytecode and *outIsBinary=true.
// For Metal/Dawn, returns the source text and *outIsBinary=false.
static std::vector<u8> LoadShaderBytes(RHIPlatform platform, const char* shaderName,
                                       const char* lumenDir, bool* outIsBinary) {
    *outIsBinary = (platform == RHIPlatform::Vulkan);

    if (platform == RHIPlatform::Vulkan) {
        // Lumen SSR shaders live in Engine/Graphics/Vulkan/shaders/Lumen/SSR*.comp.spv
        std::string relPath = std::string(lumenDir) + shaderName + ".comp.spv";
        const std::vector<std::string> candidates = {
            relPath,
            "Engine/Graphics/Vulkan/shaders/Lumen/" + std::string(shaderName) + ".comp.spv",
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
        std::cerr << "[SSR] Failed to load SPIR-V: " << shaderName
                  << " (tried " << relPath << ")" << std::endl;
        return {};
    }

    // Metal / Dawn: source text. Pack into vector<u8> for uniform handling.
    std::string path;
    std::string src;
#ifdef __EMSCRIPTEN__
    // Dawn/WGSL on web: use the Dawn shader loader.
    auto lastSlash = std::string(lumenDir).find_last_of('/');
    path = std::string(shaderName);
    src = dawn::LoadWGSL(shaderName);
#else
    path = utils::ShaderRegistry::GetShaderPath(platform, shaderName);
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "[SSR] Shader empty: " << shaderName << " (path=" << path << ")" << std::endl;
        return {};
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    src = buffer.str();
#endif
    return std::vector<u8>(src.begin(), src.end());
}

static bool EnsurePipelines(RHIDeviceBase& device) {
    if (s_TracePipeline != handles::INVALID_PIPELINE) return true;

    auto platform = device.GetPlatform();
    // Lumen shader subdir for Metal; Vulkan uses explicit Lumen/ path in LoadShaderBytes.
    const char* lumenDir = (platform == RHIPlatform::Metal)
                           ? "Engine/Graphics/Metal/shaders/Lumen/"
                           : "Engine/Graphics/Dawn/shaders/";

    auto loadShader = [&](const char* name, const char* entry) -> ShaderHandle {
        bool isBinary = false;
        auto bytes = LoadShaderBytes(platform, name, lumenDir, &isBinary);
        if (bytes.empty()) {
            std::cerr << "[SSR] Shader empty: " << name << std::endl;
            return handles::INVALID_SHADER;
        }
        ShaderHandle h = device.CreateShader(bytes.data(), bytes.size(),
                                             ShaderStage::Compute, entry);
        std::cerr << "[SSR] LoadShader " << name << " -> handle=" << static_cast<u64>(h)
                  << " (size=" << bytes.size() << " binary=" << isBinary << ")" << std::endl;
        return h;
    };

    // Metal shader names: SSRPass.metal has the trace entry, SSRComposite.metal has upsample.
    // The Metal trace entry is "ssr_trace" in SSRPass.metal; temporal is "ssr_temporal" in
    // SSRTemporal.metal; upsample is "ssr_composite" in SSRComposite.metal.
    // ShaderRegistry resolves "SSRPass" → SSRPass.metal, "SSRTemporal" → SSRTemporal.metal,
    // "SSRComposite" → SSRComposite.metal. For Vulkan/Dawn we use the same names so the
    // Lumen/ subdir lookup works.
    const char* traceName    = (platform == RHIPlatform::Vulkan) ? "SSRTrace"     : "SSRPass";
    const char* temporalName = (platform == RHIPlatform::Vulkan) ? "SSRTemporal"  : "SSRTemporal";
    const char* upsampleName = (platform == RHIPlatform::Vulkan) ? "SSRComposite" : "SSRComposite";

    ShaderHandle traceCS    = loadShader(traceName,    "ssr_trace");
    ShaderHandle temporalCS = loadShader(temporalName, "ssr_temporal");
    ShaderHandle upsampleCS = loadShader(upsampleName, "ssr_composite");

    if (traceCS == handles::INVALID_SHADER || temporalCS == handles::INVALID_SHADER ||
        upsampleCS == handles::INVALID_SHADER) {
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

    // Trace DSL: depth + hzb + color + orm + output + uniform
    //   (matches SSRTrace.comp: 0=depth, 1=hzb, 2=color, 3=orm, 4=output, 5=params)
    {
        DescriptorSetLayoutBinding bindings[6]{};
        bindings[0] = {0, DescriptorType::SampledDepthImage, 1, ShaderStage::Compute};
        bindings[1] = {1, DescriptorType::SampledImage,      1, ShaderStage::Compute};
        bindings[2] = {2, DescriptorType::SampledImage,      1, ShaderStage::Compute};
        bindings[3] = {3, DescriptorType::SampledImage,      1, ShaderStage::Compute};  // ORM
        bindings[4] = {4, DescriptorType::StorageImage,      1, ShaderStage::Compute};
        bindings[5] = {5, DescriptorType::UniformBuffer,     1, ShaderStage::Compute};
        bindings[5].minBindingSize = 192;

        DescriptorSetLayoutDesc dslDesc;
        dslDesc.bindingCount = 6;
        dslDesc.bindings = bindings;
        s_TraceDSL = device.CreateDescriptorSetLayout(dslDesc);
        auto [p, l] = createPipeline(traceCS, s_TraceDSL);
        s_TracePipeline = p; s_TraceLayout = l;
    }

    // Temporal DSL: spatial + history + velocity + output + uniform
    //   (matches SSRTemporal.comp: 0=trace, 1=history, 2=velocity, 3=output, 4=params)
    {
        DescriptorSetLayoutBinding bindings[5]{};
        bindings[0] = {0, DescriptorType::SampledImage,  1, ShaderStage::Compute};
        bindings[1] = {1, DescriptorType::SampledImage,  1, ShaderStage::Compute};
        bindings[2] = {2, DescriptorType::SampledImage,  1, ShaderStage::Compute};
        bindings[3] = {3, DescriptorType::StorageImage,  1, ShaderStage::Compute};
        bindings[4] = {4, DescriptorType::UniformBuffer, 1, ShaderStage::Compute};
        bindings[4].minBindingSize = 32;

        DescriptorSetLayoutDesc dslDesc;
        dslDesc.bindingCount = 5;
        dslDesc.bindings = bindings;
        s_TemporalDSL = device.CreateDescriptorSetLayout(dslDesc);
        auto [p, l] = createPipeline(temporalCS, s_TemporalDSL);
        s_TemporalPipeline = p; s_TemporalLayout = l;
    }

    // Upsample DSL: ssr_halfres + orm + output + uniform
    //   (matches SSRComposite.comp: 0=ssr, 1=orm, 2=output, 3=params)
    {
        DescriptorSetLayoutBinding bindings[4]{};
        bindings[0] = {0, DescriptorType::SampledImage,  1, ShaderStage::Compute};
        bindings[1] = {1, DescriptorType::SampledImage,  1, ShaderStage::Compute};  // ORM
        bindings[2] = {2, DescriptorType::StorageImage,  1, ShaderStage::Compute};
        bindings[3] = {3, DescriptorType::UniformBuffer, 1, ShaderStage::Compute};
        bindings[3].minBindingSize = 32;

        DescriptorSetLayoutDesc dslDesc;
        dslDesc.bindingCount = 4;
        dslDesc.bindings = bindings;
        s_UpsampleDSL = device.CreateDescriptorSetLayout(dslDesc);
        auto [p, l] = createPipeline(upsampleCS, s_UpsampleDSL);
        s_UpsamplePipeline = p; s_UpsampleLayout = l;
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

    createBufs(s_TraceParamsBuf,    s_TraceParamsMapped,    192);
    createBufs(s_TemporalParamsBuf, s_TemporalParamsMapped, 32);
    createBufs(s_UpsampleParamsBuf, s_UpsampleParamsMapped, 32);

    auto createSets = [&](DescriptorSetHandle (&pool)[MAX_FRAMES][MAX_SETS], DescriptorSetLayoutHandle dsl) {
        for (u32 i = 0; i < MAX_FRAMES; ++i)
            for (u32 j = 0; j < MAX_SETS; ++j) {
                DescriptorSetDesc dsDesc;
                dsDesc.layout = dsl;
                pool[i][j] = device.CreateDescriptorSet(dsDesc);
            }
    };

    createSets(s_TraceSets,    s_TraceDSL);
    createSets(s_TemporalSets, s_TemporalDSL);
    createSets(s_UpsampleSets, s_UpsampleDSL);

    std::cerr << "[SSR] Pipelines created — trace=" << static_cast<u64>(s_TracePipeline)
              << " temporal=" << static_cast<u64>(s_TemporalPipeline)
              << " upsample=" << static_cast<u64>(s_UpsamplePipeline) << std::endl;

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
    RGResourceHandle gbufferOrmTexture,
    u32 width, u32 height, u32 frameIndex,
    const math::m4x4& proj, const math::m4x4& invProj,
    const SSRConfig& ssrCfg) {

    u32 fi = frameIndex % MAX_FRAMES;
    s_TraceSetIdx[fi] = 0;
    s_TemporalSetIdx[fi] = 0;
    s_UpsampleSetIdx[fi] = 0;

    static bool s_firstAddCall = true;
    if (s_firstAddCall) {
        s_firstAddCall = false;
        std::cerr << "[SSR] AddSSRPass first call — frame=" << frameIndex
                  << " w=" << width << " h=" << height
                  << " hdrValid=" << (hdrTexture != kInvalidRGResourceHandle)
                  << " depthValid=" << (depthTexture != kInvalidRGResourceHandle)
                  << " hzbValid=" << (hzbTexture != kInvalidRGResourceHandle)
                  << " velValid=" << (velocityTexture != kInvalidRGResourceHandle)
                  << " ormValid=" << (gbufferOrmTexture != kInvalidRGResourceHandle) << std::endl;
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
            builder.Read(gbufferOrmTexture, ResourceState::ShaderResource);

            TextureDesc outDesc;
            outDesc.size = {width, height, 1};
            outDesc.format = DataFormat::RGBA16_Float;
            outDesc.type = TextureType::Texture2D;
            outDesc.mipLevels = 1;
            outDesc.usage = TextureUsage::UnorderedAccess | TextureUsage::ShaderResource;
            data.outputColor = builder.CreateTexture("SSR_Reflection", outDesc, ResourceState::UnorderedAccess);
        },
        [hdrTexture, depthTexture, hzbTexture, velocityTexture, gbufferOrmTexture,
         width, height, halfW, halfH, fi, histIdx,
         hzbMipLevels, proj, invProj, frameIndex, ssrCfg]
         (const SSRPassData& data, RenderGraphContext& context) {
            static u32 s_execCount = 0;
            bool logThisFrame = (s_execCount < 3u);
            ++s_execCount;

            if (logThisFrame) {
                std::cerr << "[SSR] Execute frame=" << frameIndex
                          << " tracePipe=" << static_cast<u64>(s_TracePipeline)
                          << " temporalPipe=" << static_cast<u64>(s_TemporalPipeline)
                          << " upsamplePipe=" << static_cast<u64>(s_UpsamplePipeline) << std::endl;
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

            ResourceHandle hdrHandle      = resolveTex(hdrTexture);
            ResourceHandle depthHandle    = resolveTex(depthTexture);
            ResourceHandle hzbHandle      = resolveTex(hzbTexture);
            ResourceHandle velocityHandle = resolveTex(velocityTexture);
            ResourceHandle ormHandle      = resolveTex(gbufferOrmTexture);
            auto* outRes = context.graph->GetResource(data.outputColor);
            ResourceHandle outPhys = outRes ? outRes->GetPhysicalHandle() : handles::INVALID_RESOURCE;

            if (logThisFrame) {
                std::cerr << "[SSR] Resolved — hdr=" << static_cast<u64>(hdrHandle)
                          << " depth=" << static_cast<u64>(depthHandle)
                          << " hzb=" << static_cast<u64>(hzbHandle)
                          << " vel=" << static_cast<u64>(velocityHandle)
                          << " orm=" << static_cast<u64>(ormHandle)
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
            //   bindings: 0=depth, 1=hzb, 2=color, 3=orm, 4=output, 5=params
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
                    u32         _pad0;
                    u32         _pad1;
                };
                static_assert(sizeof(TraceParamsCPU) == 192,
                              "TraceParamsCPU must be 192 bytes (16-byte aligned for uniform)");

                if (s_TraceParamsMapped[fi]) {
                    auto* p = static_cast<TraceParamsCPU*>(s_TraceParamsMapped[fi]);
                    p->invProj = invProj;
                    p->proj = proj;
                    p->screenSize = {(f32)width, (f32)height, 1.0f / width, 1.0f / height};
                    p->halfScreenSize = {(f32)halfW, (f32)halfH, 1.0f / halfW, 1.0f / halfH};
                    p->maxDistance = ssrCfg.max_trace_distance;
                    p->thickness = ssrCfg.thickness;
                    p->nearPlane = 0.1f;
                    p->farPlane = 1000.0f;
                    p->hzbMipLevels = hzbMipLevels;
                    p->frameIndex = frameIndex;
                    p->_pad0 = 0; p->_pad1 = 0;
                    device.SetBufferDirtySize(s_TraceParamsBuf[fi], sizeof(TraceParamsCPU));
                }

                u32 idx = s_TraceSetIdx[fi]++;
                if (idx >= MAX_SETS) { idx = 0; s_TraceSetIdx[fi] = 1; }
                DescriptorSetHandle ds = s_TraceSets[fi][idx];

                DescriptorImageInfo depthInfo; depthInfo.imageView = depthHandle;
                DescriptorImageInfo hzbInfo; hzbInfo.imageView = hzbHandle;
                DescriptorImageInfo hdrInfo; hdrInfo.imageView = hdrHandle;
                DescriptorImageInfo ormInfo; ormInfo.imageView = ormHandle;
                DescriptorImageInfo outInfo; outInfo.imageView = s_TraceTexture;
                DescriptorBufferInfo bufInfo; bufInfo.buffer = s_TraceParamsBuf[fi]; bufInfo.offset = 0;
                bufInfo.range = sizeof(TraceParamsCPU);

                WriteDescriptorSet writes[6];
                writes[0].dstSet = ds; writes[0].dstBinding = 0; writes[0].descriptorCount = 1;
                writes[0].descriptorType = DescriptorType::SampledDepthImage; writes[0].imageInfo = &depthInfo;
                writes[1].dstSet = ds; writes[1].dstBinding = 1; writes[1].descriptorCount = 1;
                writes[1].descriptorType = DescriptorType::SampledImage; writes[1].imageInfo = &hzbInfo;
                writes[2].dstSet = ds; writes[2].dstBinding = 2; writes[2].descriptorCount = 1;
                writes[2].descriptorType = DescriptorType::SampledImage; writes[2].imageInfo = &hdrInfo;
                writes[3].dstSet = ds; writes[3].dstBinding = 3; writes[3].descriptorCount = 1;
                writes[3].descriptorType = DescriptorType::SampledImage; writes[3].imageInfo = &ormInfo;
                writes[4].dstSet = ds; writes[4].dstBinding = 4; writes[4].descriptorCount = 1;
                writes[4].descriptorType = DescriptorType::StorageImage; writes[4].imageInfo = &outInfo;
                writes[5].dstSet = ds; writes[5].dstBinding = 5; writes[5].descriptorCount = 1;
                writes[5].descriptorType = DescriptorType::UniformBuffer; writes[5].bufferInfo = &bufInfo;

                device.UpdateDescriptorSets(6, writes);

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
            //   bindings: 0=spatial(trace), 1=history, 2=velocity, 3=output, 4=params
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
                    p->feedback = ssrCfg.temporal_feedback;
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
            // Sub-pass 3: Upsample (full-res) — write reflection color into RG output
            //   bindings: 0=ssr_halfres, 1=orm, 2=output, 3=params
            // ============================================================
            {
                struct UpsampleParamsCPU {
                    u32 screenWidth;
                    u32 screenHeight;
                    u32 halfWidth;
                    u32 halfHeight;
                    f32 maxRoughness;
                    f32 reflectionStrength;
                    f32 _pad0;
                    f32 _pad1;
                };

                if (s_UpsampleParamsMapped[fi]) {
                    auto* p = static_cast<UpsampleParamsCPU*>(s_UpsampleParamsMapped[fi]);
                    p->screenWidth = width;
                    p->screenHeight = height;
                    p->halfWidth = halfW;
                    p->halfHeight = halfH;
                    p->maxRoughness = ssrCfg.max_roughness;
                    p->reflectionStrength = ssrCfg.reflection_strength;
                    p->_pad0 = 0; p->_pad1 = 0;
                    device.SetBufferDirtySize(s_UpsampleParamsBuf[fi], sizeof(UpsampleParamsCPU));
                }

                u32 idx = s_UpsampleSetIdx[fi]++;
                if (idx >= MAX_SETS) { idx = 0; s_UpsampleSetIdx[fi] = 1; }
                DescriptorSetHandle ds = s_UpsampleSets[fi][idx];

                DescriptorImageInfo ssrInfo; ssrInfo.imageView = s_TemporalTexture;
                DescriptorImageInfo ormInfo; ormInfo.imageView = ormHandle;
                DescriptorImageInfo outInfo; outInfo.imageView = outPhys;
                DescriptorBufferInfo bufInfo; bufInfo.buffer = s_UpsampleParamsBuf[fi]; bufInfo.offset = 0;
                bufInfo.range = sizeof(UpsampleParamsCPU);

                WriteDescriptorSet writes[4];
                writes[0].dstSet = ds; writes[0].dstBinding = 0; writes[0].descriptorCount = 1;
                writes[0].descriptorType = DescriptorType::SampledImage; writes[0].imageInfo = &ssrInfo;
                writes[1].dstSet = ds; writes[1].dstBinding = 1; writes[1].descriptorCount = 1;
                writes[1].descriptorType = DescriptorType::SampledImage; writes[1].imageInfo = &ormInfo;
                writes[2].dstSet = ds; writes[2].dstBinding = 2; writes[2].descriptorCount = 1;
                writes[2].descriptorType = DescriptorType::StorageImage; writes[2].imageInfo = &outInfo;
                writes[3].dstSet = ds; writes[3].dstBinding = 3; writes[3].descriptorCount = 1;
                writes[3].descriptorType = DescriptorType::UniformBuffer; writes[3].bufferInfo = &bufInfo;

                device.UpdateDescriptorSets(4, writes);

                cmd->BindComputePipeline(s_UpsamplePipeline);
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, s_UpsampleLayout, 0, 1, &ds, 0, nullptr);
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

            // Barrier: SSR output → SRV (for downstream FusionComposite)
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
                          << " dispatched trace+temporal+upsample+blit" << std::endl;
            }
        }
    );
}

} // namespace primal::graphics::PostProcess
