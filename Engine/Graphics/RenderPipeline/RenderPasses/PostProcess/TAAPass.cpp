#include "TAAPass.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/Utils/ShaderRegistry.h"
#include "Graphics/Dawn/ShaderLoader.h"
#include "Graphics/RenderGraph/LumenCompareCapture.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <cmath>
#include "Engine/Common/CoreLogStream.h"

namespace primal::graphics::PostProcess {

using namespace rhi;

static constexpr u32 MAX_FRAMES = 3;
static constexpr u32 MAX_SETS_PER_FRAME = 2;

static PipelineHandle s_Pipeline = handles::INVALID_PIPELINE;
static PipelineLayoutHandle s_Layout = handles::INVALID_PIPELINE_LAYOUT;
static DescriptorSetLayoutHandle s_DSL = handles::INVALID_RESOURCE;
// Handles are u64 with INVALID == (u64)-1 — zero-init would read as a valid
// handle 0, so every array below must be explicitly INVALID-initialized.
static DescriptorSetHandle s_SetPool[MAX_FRAMES][MAX_SETS_PER_FRAME] = {
    {handles::INVALID_DESCRIPTOR_SET, handles::INVALID_DESCRIPTOR_SET},
    {handles::INVALID_DESCRIPTOR_SET, handles::INVALID_DESCRIPTOR_SET},
    {handles::INVALID_DESCRIPTOR_SET, handles::INVALID_DESCRIPTOR_SET}};
static u32 s_SetIndex[MAX_FRAMES] = {};

static SamplerHandle s_LinearSampler = handles::INVALID_SAMPLER;

// Triple-buffered history textures — ping-pong: read from frameIndex, write to next frame's slot
static ResourceHandle s_HistoryTex[MAX_FRAMES] = {
    handles::INVALID_RESOURCE, handles::INVALID_RESOURCE, handles::INVALID_RESOURCE};
static bool s_HistoryInit[MAX_FRAMES] = {}; // false until first write to this slot
struct RasterHistory {
    u32 frame{~u32(0)};
    math::v2 jitter{};
    ResourceHandle depthSource{handles::INVALID_RESOURCE};
    bool valid{false};
};
static RasterHistory s_RasterHistory[MAX_FRAMES];
static u64 s_ResetGeneration=0;

static math::m4x4 ClipJitter(math::v2 jitter) {
    // Identity + jitter translation column（原实现：零矩阵→对角置 1→平移列写 jitter）。
    return math::make_m4x4(math::v4{1.f, 0.f, 0.f, 0.f},
                           math::v4{0.f, 1.f, 0.f, 0.f},
                           math::v4{0.f, 0.f, 1.f, 0.f},
                           math::v4{jitter.x, jitter.y, 0.f, 1.f});
}
static u32 s_TexWidth = 0;                  // dimensions the history textures were
static u32 s_TexHeight = 0;                 // created with — recreate on resize

// 模块持有的输出纹理（2026-09-02）：TAA 输出曾用 builder.CreateTexture（RG
// 瞬态池）。RG 的帧内池化会把它与其它同尺寸瞬态（如 fusion 输出）复用同一
// 物理纹理且 Metal 上无别名屏障——实测（TAA 输出减半、fusion 输出减半两个
// 判定实验）屏幕消费的不是 TAA 写入的内容，TAA 形同旁路，全屏保留原始噪点
// 与几何锯齿。改为模块持有 + ImportResource 进图，与 history 同生命周期管理。
static ResourceHandle s_OutputTex[MAX_FRAMES] = {
    handles::INVALID_RESOURCE, handles::INVALID_RESOURCE, handles::INVALID_RESOURCE};

// Persistent params buffer per frame slot
struct TAAGlobalsCPU {
    math::v4 screenSize;     // xy = (w, h), zw = (1/w, 1/h)
    f32   invHistoryValid;   // 1.0 if history slot is uninitialized
    u32   depthValid;        // 1 if depth reprojection inputs are bound
    u32   _pad1, _pad2;
    math::m4x4 currInvViewProj;  // this frame's inverse view-projection
    math::m4x4 prevViewProj;     // previous frame's view-projection
};
static_assert(sizeof(TAAGlobalsCPU)==160,"TAA shader uniform layout must remain unchanged");
static ResourceHandle s_ParamsBuf[MAX_FRAMES] = {
    handles::INVALID_RESOURCE, handles::INVALID_RESOURCE, handles::INVALID_RESOURCE};
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

// Platform-branched shader loader.
//   Vulkan: load precompiled .comp.spv bytecode (hand-written GLSL).
//   Metal/Dawn: load source text (.metal / .wgsl) for runtime compilation.
static std::vector<u8> LoadShaderBytes(RHIPlatform platform, bool* outIsBinary) {
    *outIsBinary = (platform == RHIPlatform::Vulkan);

    if (platform == RHIPlatform::Vulkan) {
        // Hand-written GLSL compiled to Lumen/TAA.comp.spv
        const std::string relPath = "Engine/Graphics/Vulkan/shaders/Lumen/TAA.comp.spv";
        const std::vector<std::string> candidates = {
            relPath,
            utils::ShaderRegistry::ResolveShaderPath(relPath),
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
        PRIMAL_LOG_STREAM(Error, "Render") << "[TAA] Failed to load SPIR-V: Lumen/TAA.comp.spv" << std::endl;
        return {};
    }

    // Metal / Dawn: source text
#ifdef __EMSCRIPTEN__
    std::string src = dawn::LoadWGSL("TAA");
    return std::vector<u8>(src.begin(), src.end());
#else
    std::string path = utils::ShaderRegistry::GetShaderPath(platform, "TAA");
    // T8.13 sync: resolve repo-relative path independently of process CWD —
    // the bare CWD-relative open failed whenever the app launched from the
    // build dir (Darwin/Debug), silently disabling TAA on Metal.
    path = utils::ShaderRegistry::ResolveShaderPath(path);
    std::string src = LoadShaderSource(path);
    return std::vector<u8>(src.begin(), src.end());
#endif
}

static bool EnsurePipeline(RHIDeviceBase& device, u32 width, u32 height) {
    // History textures must follow the render size — the output→history
    // BlitTexture is a 1:1 copy and validation-rejects mismatched extents.
    // destroyTextureImpl is GC-deferred, so destroying while the previous
    // frame's dispatch may still be in flight is safe.
    bool needHistory = (s_HistoryTex[0] == handles::INVALID_RESOURCE) ||
                       (s_TexWidth != width) || (s_TexHeight != height);
    for (u32 i=0;i<MAX_FRAMES;++i)
        needHistory = needHistory || s_HistoryTex[i] == handles::INVALID_RESOURCE ||
            s_OutputTex[i] == handles::INVALID_RESOURCE;
    if (needHistory) {
        ++s_ResetGeneration;
        for (u32 i = 0; i < MAX_FRAMES; ++i) {
            if (s_HistoryTex[i] != handles::INVALID_RESOURCE) {
                device.DestroyTexture(s_HistoryTex[i]);
                s_HistoryTex[i] = handles::INVALID_RESOURCE;
            }
            s_HistoryInit[i] = false;
            s_RasterHistory[i] = {};
        }
        s_TexWidth = width;
        s_TexHeight = height;
    }

    const bool createPipeline = s_Pipeline == handles::INVALID_PIPELINE;

    auto platform = device.GetPlatform();
    if (createPipeline) {
    bool isBinary = false;
    std::vector<u8> shaderBytes = LoadShaderBytes(platform, &isBinary);
    if (shaderBytes.empty()) { PRIMAL_LOG_STREAM(Error, "Render") << "[TAA] Shader load failed" << std::endl; return false; }

    ShaderHandle cs = device.CreateShader(shaderBytes.data(), shaderBytes.size(), ShaderStage::Compute, "taa_main");
    if (cs == handles::INVALID_SHADER) { PRIMAL_LOG_STREAM(Error, "Render") << "[TAA] Shader creation failed" << std::endl; return false; }

    // Bindings 0..2 = sampled images (curr, hist, vel), 3 = storage image (output),
    //             4 = uniform buffer (globals).
    // Vulkan/Metal add 5..6 = sampled images (currDepth, prevDepth) for matrix
    // reprojection + disocclusion. Dawn keeps the velocity-only WGSL shader
    // with the original 5-binding layout.
    const bool useDepthBindings = (platform != RHIPlatform::Dawn);
    const u32 bindingCount = useDepthBindings ? 7 : 5;
    DescriptorSetLayoutBinding bindings[7]{};
    bindings[0] = {0, DescriptorType::SampledImage,    1, ShaderStage::Compute};
    bindings[1] = {1, DescriptorType::SampledImage,    1, ShaderStage::Compute};
    bindings[2] = {2, DescriptorType::SampledImage,    1, ShaderStage::Compute};
    bindings[3] = {3, DescriptorType::StorageImage,    1, ShaderStage::Compute};
    bindings[4] = {4, DescriptorType::UniformBuffer,   1, ShaderStage::Compute};
    if (useDepthBindings) {
        bindings[5] = {5, DescriptorType::SampledImage, 1, ShaderStage::Compute};
        bindings[6] = {6, DescriptorType::SampledImage, 1, ShaderStage::Compute};
    }

    DescriptorSetLayoutDesc dslDesc;
    dslDesc.bindingCount = bindingCount;
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
    }

    // Allocate persistent history textures (size-tracked) + params buffers
    // and descriptor sets (size-independent, created once) per frame slot.
    TextureDesc histDesc;
    histDesc.name = "TAA_History";
    histDesc.size = {width, height, 1};
    histDesc.format = DataFormat::RGBA16_Float;
    histDesc.type = TextureType::Texture2D;
    histDesc.mipLevels = 1;
    histDesc.usage = TextureUsage::ShaderResource | TextureUsage::CopyDest;

    for (u32 i = 0; i < MAX_FRAMES; ++i) {
        if (needHistory) {
            s_HistoryTex[i] = device.CreateTexture(histDesc);
            if (s_HistoryTex[i] != handles::INVALID_RESOURCE)
                device.PrimeTextureLayout(s_HistoryTex[i]);  // P0-B 首帧 priming
            // 输出纹理与 history 同尺寸同生命周期（见 s_OutputTex 注释）。
            TextureDesc outTexDesc = histDesc;
            outTexDesc.name = "TAA_Output";
            outTexDesc.usage = TextureUsage::UnorderedAccess | TextureUsage::ShaderResource | TextureUsage::CopySource;
            if (s_OutputTex[i] != handles::INVALID_RESOURCE)
                device.DestroyTexture(s_OutputTex[i]);
            s_OutputTex[i] = device.CreateTexture(outTexDesc);
            if (s_OutputTex[i] != handles::INVALID_RESOURCE)
                device.PrimeTextureLayout(s_OutputTex[i]);  // P0-B 首帧 priming
        }

        if (s_ParamsBuf[i] == handles::INVALID_RESOURCE) {
            BufferDesc bufDesc{};
            bufDesc.size = sizeof(TAAGlobalsCPU);
            bufDesc.type = BufferType::Constant;
            bufDesc.usage = GPUMemoryUsage::Dynamic;
            bufDesc.memoryUsage = GPUMemoryUsage::Dynamic;
            s_ParamsBuf[i] = device.CreateBuffer(bufDesc);
        }
        if (s_ParamsBuf[i] != handles::INVALID_RESOURCE && !s_ParamsMapped[i])
            s_ParamsMapped[i] = device.MapBuffer(s_ParamsBuf[i], 0, sizeof(TAAGlobalsCPU));

        {
            for (u32 j = 0; j < MAX_SETS_PER_FRAME; ++j) {
                if (s_SetPool[i][j] != handles::INVALID_DESCRIPTOR_SET) continue;
                DescriptorSetDesc dsDesc;
                dsDesc.layout = s_DSL;
                s_SetPool[i][j] = device.CreateDescriptorSet(dsDesc);
            }
        }
    }

    bool ready = s_Pipeline != handles::INVALID_PIPELINE &&
        s_Layout != handles::INVALID_PIPELINE_LAYOUT && s_DSL != handles::INVALID_RESOURCE;
    for (u32 i=0;i<MAX_FRAMES;++i) {
        ready = ready && s_HistoryTex[i] != handles::INVALID_RESOURCE &&
            s_OutputTex[i] != handles::INVALID_RESOURCE &&
            s_ParamsBuf[i] != handles::INVALID_RESOURCE && s_ParamsMapped[i];
        for (u32 j=0;j<MAX_SETS_PER_FRAME;++j)
            ready = ready && s_SetPool[i][j] != handles::INVALID_DESCRIPTOR_SET;
    }
    return ready;
}

const TAAPassData& AddTAAPass(RenderGraph& graph, RGResourceHandle inputHDR,
                              RGResourceHandle velocityTexture,
                              u32 width, u32 height, u32 frameIndex,
                              const TAADepthInputs* depth) {
    u32 fi = frameIndex % MAX_FRAMES;
    s_SetIndex[fi] = 0;

    if (!EnsurePipeline(graph.GetDevice(), width, height)) {
        ResetTAAHistory();
        return graph.AddPass<TAAPassData>("TAAUnavailable",RGPassType::Compute,RGPassCategory::PostProcess,
            [inputHDR](TAAPassData& data,RenderGraphBuilder& builder) {
                builder.Read(inputHDR,ResourceState::ShaderResource);
                data.output=inputHDR;
            },[](const TAAPassData&,RenderGraphContext&) {});
    }

    // Depth reprojection is active when the caller supplied a valid previous
    // depth (DepthHistoryManager copy) AND the pipeline has the depth bindings
    // (Vulkan/Metal). Binding slots 5/6 still receive valid textures when
    // inactive — the shader gates on depthValid and never reads them.
    const bool depthActive = depth != nullptr &&
                             depth->currDepth != handles::INVALID_RESOURCE &&
                             depth->prevDepth != handles::INVALID_RESOURCE &&
                             graph.GetDevice().GetPlatform() != RHIPlatform::Dawn;
    // One-shot diagnostics — mirrors the "[SSGI_EXEC]" / "[HZB]" stderr style.
    static bool s_DiagDepthActive = false;
    static bool s_DiagDepthFallback = false;
    if (depthActive && !s_DiagDepthActive) {
        s_DiagDepthActive = true;
        PRIMAL_LOG_STREAM(Error, "Render") << "[TAA] depth reprojection active (matrix reprojection + "
                     "prev-depth disocclusion)" << std::endl;
    } else if (!depthActive && !s_DiagDepthFallback && s_Pipeline != handles::INVALID_PIPELINE &&
               graph.GetDevice().GetPlatform() != RHIPlatform::Dawn) {
        s_DiagDepthFallback = true;
        PRIMAL_LOG_STREAM(Error, "Render") << "[TAA] depth reprojection inactive — ";
        if (depth == nullptr) PRIMAL_LOG_STREAM(Error, "Render") << "no depth inputs (first frame or Render()-path only)";
        else if (depth->currDepth == handles::INVALID_RESOURCE) PRIMAL_LOG_STREAM(Error, "Render") << "currDepth invalid";
        else PRIMAL_LOG_STREAM(Error, "Render") << "prevDepth invalid";
        PRIMAL_LOG_STREAM(Error, "Render") << "; using velocity fallback" << std::endl;
    }

    // currDepth is RG-external (GPUDrivenDrawPipeline's GBuffer depth): import
    // it so the render graph transitions it to ShaderResource for this pass.
    // prevDepth (DepthHistoryManager) is pipeline-internal and already sits in
    // ShaderResource after the end-of-frame copy — bound directly.
    RGResourceHandle currDepthRG;
    RGResourceHandle prevDepthRG;
    if (depthActive) {
        currDepthRG = graph.GetDevice().GetPlatform() == RHIPlatform::Vulkan && depth->current_depth_rg.IsValid()
            ? depth->current_depth_rg : graph.ImportResource("TAA_CurrDepth", depth->currDepth);
        if (graph.GetDevice().GetPlatform() == RHIPlatform::Vulkan)
            prevDepthRG = graph.ImportResource("TAA_PrevDepth",depth->prevDepth);
    } else if (depth && graph.GetDevice().GetPlatform() == RHIPlatform::Vulkan) {
        currDepthRG = depth->current_depth_rg;
    }

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
    if (s_ParamsMapped[fi]) {
        auto* params = static_cast<TAAGlobalsCPU*>(s_ParamsMapped[fi]);
        params->screenSize = {(f32)width, (f32)height, 1.0f / width, 1.0f / height};
        params->invHistoryValid = histValid ? 0.0f : 1.0f;
        params->depthValid = depthActive ? 1u : 0u;
        params->_pad1 = 0; params->_pad2 = 0;
        if (depthActive) {
            params->currInvViewProj = depth->currInvViewProj;
            params->prevViewProj = depth->prevViewProj;
        }
        graph.GetDevice().SetBufferDirtySize(s_ParamsBuf[fi], sizeof(TAAGlobalsCPU));
    }

    return graph.AddPass<TAAPassData>("TAAPass", RGPassType::Compute, RGPassCategory::PostProcess,
        [&, inputHDR, velocityTexture](TAAPassData& data, RenderGraphBuilder& builder) {
            builder.Read(inputHDR, ResourceState::ShaderResource);
            builder.Read(velocityTexture, ResourceState::ShaderResource);
            if (currDepthRG.IsValid()) {
                builder.Read(currDepthRG, ResourceState::ShaderResource);
            }
            if (prevDepthRG.IsValid()) builder.Read(prevDepthRG,ResourceState::ShaderResource);

            // 模块持有的输出以 ImportTexture 进图（绕开 RG 瞬态池的帧内
            // 复用——见 s_OutputTex 注释）。desc 必须携带：ToneMapping 用
            // inputHDR 的 desc 决定自己的输出尺寸。
            TextureDesc outDesc;
            outDesc.size = {width, height, 1};
            outDesc.format = DataFormat::RGBA16_Float;
            outDesc.type = TextureType::Texture2D;
            outDesc.mipLevels = 1;
            outDesc.usage = TextureUsage::UnorderedAccess | TextureUsage::ShaderResource | TextureUsage::CopySource;
            data.output = builder.Write(
                graph.ImportTexture("TAA_Output", s_OutputTex[fi], outDesc,
                                    // P0-B（01197 定位）: initialState 必须是
                                    // Unknown——上帧末尾 restore barrier 把该槽
                                    // tracked 在 SRO（blit 源回读态）。声明 UA
                                    // 会让 RG 认为"已在 UA"而不发发射转换，
                                    // resolve 的 storage 写就发生在 SRO 上，
                                    // validation 把实际 layout 钉成 GENERAL，
                                    // 随后 outWriteBarrier 的 SRO→XFER_SRC 触发
                                    // VUID-01197 ×18/帧。Unknown → RG 发
                                    // Unknown→UA 初始转换，后端按 tracked 解析
                                    // （GENERAL→GENERAL 时自然 no-op）。
                                    ResourceState::Unknown),
                ResourceState::UnorderedAccess);
        },
        [inputHDR, velocityTexture, width, height, fi, histReadIdx, histValid, depthActive, frameIndex,
         depthInputs = depth ? *depth : TAADepthInputs{},
         currDepthTex = depthActive ? depth->currDepth : handles::INVALID_RESOURCE,
         prevDepthTex = depthActive ? depth->prevDepth : handles::INVALID_RESOURCE,
         useDepthBindings = graph.GetDevice().GetPlatform() != RHIPlatform::Dawn]
         (const TAAPassData& data, RenderGraphContext& context) {
            if (s_Pipeline == handles::INVALID_PIPELINE) {
                std::cerr << "[TAA] Execute abort: pipeline INVALID" << std::endl;
                return;
            }

            auto& device = context.graph->GetDevice();
            auto* cmd = context.cmdBuffer;
            const bool vulkan = device.GetPlatform() == RHIPlatform::Vulkan;
            math::v2 actualJitter{};
            bool actualRasterValid = false;
            bool executionHistoryValid = histValid;
            bool executionDepthActive = depthActive;
            const auto previousRaster = s_RasterHistory[histReadIdx];
            if (vulkan) {
                actualRasterValid = depthInputs.require_raster_jitter && depthInputs.raster_jitter_provider &&
                    depthInputs.raster_jitter_provider(actualJitter) &&
                    std::isfinite(actualJitter.x) && std::isfinite(actualJitter.y);
                const auto& previous = s_RasterHistory[histReadIdx];
                u32 cw=0,ch=0,pw=0,ph=0;
                const bool depthSizesValid = depthActive &&
                    device.GetTextureSize(depthInputs.currDepth,cw,ch) &&
                    device.GetTextureSize(depthInputs.prevDepth,pw,ph) &&
                    cw==width && ch==height && pw==width && ph==height;
                executionHistoryValid = histValid && s_HistoryInit[histReadIdx] &&
                    frameIndex > 0 && previous.frame == frameIndex-1;
                if (depthInputs.require_raster_jitter) {
                    executionHistoryValid = executionHistoryValid && actualRasterValid &&
                        depthSizesValid && previous.valid &&
                        depthInputs.previous_depth_frame == previous.frame &&
                        previous.depthSource == depthInputs.currDepth;
                }
                executionDepthActive = depthActive && (!depthInputs.require_raster_jitter || executionHistoryValid);
                if (!s_ParamsMapped[fi]) {
                    s_HistoryInit[fi] = false;
                    s_RasterHistory[fi] = {};
                    return;
                }
                auto* p = static_cast<TAAGlobalsCPU*>(s_ParamsMapped[fi]);
                p->invHistoryValid = executionHistoryValid ? 0.0f : 1.0f;
                p->depthValid = executionDepthActive ? 1u : 0u;
                if (executionDepthActive && depthInputs.require_raster_jitter) {
                    p->currInvViewProj = depthInputs.currInvViewProj *
                        ClipJitter(math::v2{-actualJitter.x,-actualJitter.y});
                    p->prevViewProj = ClipJitter(previous.jitter) * depthInputs.prevViewProj;
                }
                device.SetBufferDirtySize(s_ParamsBuf[fi],sizeof(TAAGlobalsCPU));
                cmd->MemoryBarrier(PipelineStage::Host,PipelineStage::ComputeShader,
                    AccessFlag::HostWrite,AccessFlag::ShaderRead);
                // Do not leave an old stamp valid if execution aborts below.
                s_RasterHistory[fi] = {};
                s_HistoryInit[fi] = false;
            }

            auto* inRes = context.graph->GetResource(inputHDR);
            ResourceHandle inHandle = inRes ? inRes->GetPhysicalHandle() : handles::INVALID_RESOURCE;
            auto* velRes = context.graph->GetResource(velocityTexture);
            ResourceHandle velHandle = velRes ? velRes->GetPhysicalHandle() : handles::INVALID_RESOURCE;
            auto* outRes = context.graph->GetResource(data.output);
            ResourceHandle outHandle = outRes ? outRes->GetPhysicalHandle() : handles::INVALID_RESOURCE;

            // When history is invalid (first frame / reset / resize) the history
            // slot may still be in VK_IMAGE_LAYOUT_UNDEFINED — binding it as a
            // sampled image is a validation error even though the shader never
            // samples it (invHistoryValid branch). Bind the input texture as a
            // stand-in instead; the result is identical (alpha=1 passthrough).
            ResourceHandle histHandle = executionHistoryValid ? s_HistoryTex[histReadIdx] : inHandle;
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

            // Slots 5/6 always receive a valid texture (the input image when
            // depth reprojection is inactive) — the layout declares them on
            // Vulkan/Metal and the shader gates actual reads on depthValid.
            DescriptorImageInfo currDepthInfo;
            currDepthInfo.imageView = executionDepthActive ? currDepthTex : inHandle;
            currDepthInfo.imageLayout = ResourceState::ShaderResource;
            DescriptorImageInfo prevDepthInfo;
            prevDepthInfo.imageView = executionDepthActive ? prevDepthTex : inHandle;
            prevDepthInfo.imageLayout = ResourceState::ShaderResource;

            WriteDescriptorSet writes[7] = {};
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

            u32 writeCount = 5;
            if (useDepthBindings) {
                writes[5].dstSet = ds; writes[5].dstBinding = 5; writes[5].descriptorCount = 1;
                writes[5].descriptorType = DescriptorType::SampledImage; writes[5].imageInfo = &currDepthInfo;
                writes[6].dstSet = ds; writes[6].dstBinding = 6; writes[6].descriptorCount = 1;
                writes[6].descriptorType = DescriptorType::SampledImage; writes[6].imageInfo = &prevDepthInfo;
                writeCount = 7;
            }

            device.UpdateDescriptorSets(writeCount, writes);

            if (vulkan && lumencompare::enabled && lumencompare::Selected("TAAPass.Inputs")) {
                lumencompare::Output(&device,cmd,"TAAPass.Inputs","Parameters",s_ParamsBuf[fi],true,ResourceState::ShaderResource);
                lumencompare::Output(&device,cmd,"TAAPass.Inputs","CurrentHDR",inHandle,false,ResourceState::ShaderResource);
                lumencompare::Output(&device,cmd,"TAAPass.Inputs","HistoryColor",histHandle,false,ResourceState::ShaderResource);
                lumencompare::Output(&device,cmd,"TAAPass.Inputs","Velocity",velHandle,false,ResourceState::ShaderResource);
                if (executionDepthActive) {
                    lumencompare::Output(&device,cmd,"TAAPass.Inputs","CurrentDepth",currDepthTex,false,ResourceState::ShaderResource);
                    lumencompare::Output(&device,cmd,"TAAPass.Inputs","PreviousDepth",prevDepthTex,false,ResourceState::ShaderResource);
                }
                const auto directory=std::filesystem::path(std::getenv("LUMEN_COMPARE_DIR"))/
                    ("frame-"+std::to_string(lumencompare::ordinal));
                std::filesystem::create_directories(directory);
                std::ofstream metadata(directory/"taa-inputs.json");
                metadata << std::setprecision(9) << "{\"schema\":1,\"engine_frame\":" << frameIndex
                    << ",\"history_write_slot\":" << fi << ",\"history_read_slot\":" << histReadIdx
                    << ",\"history_written_frame\":" << previousRaster.frame
                    << ",\"previous_depth_frame\":" << depthInputs.previous_depth_frame
                    << ",\"reset_generation\":" << s_ResetGeneration
                    << ",\"require_raster_jitter\":" << depthInputs.require_raster_jitter
                    << ",\"current_raster_valid\":" << actualRasterValid
                    << ",\"history_valid\":" << executionHistoryValid
                    << ",\"depth_valid\":" << executionDepthActive
                    << ",\"width\":" << width << ",\"height\":" << height
                    << ",\"current_jitter_ndc\":[" << actualJitter.x << ',' << actualJitter.y
                    << "],\"previous_jitter_ndc\":[" << previousRaster.jitter.x << ',' << previousRaster.jitter.y
                    << "],\"current_hdr_handle\":" << inHandle << ",\"history_color_handle\":" << histHandle
                    << ",\"velocity_handle\":" << velHandle << ",\"current_depth_source\":" << depthInputs.currDepth
                    << ",\"previous_history_depth_source\":" << previousRaster.depthSource
                    << ",\"bound_current_depth\":" << currDepthInfo.imageView
                    << ",\"bound_previous_depth\":" << prevDepthInfo.imageView
                    << ",\"matrix_source\":\"execution_actual_raster_and_recorded_color_depth_history\"}\n";
            }

            cmd->BindComputePipeline(s_Pipeline);
            cmd->BindDescriptorSets(PipelineBindPoint::Compute, s_Layout, 0, 1, &ds, 0, nullptr);
            cmd->Dispatch((width + 7) / 8, (height + 7) / 8, 1);

            // Storage 写（dispatch）→ 同 command buffer 内 blit 读之间必须显式
            // 屏障。此前零屏障：Vulkan 是 sync2 WRITE_AFTER_READ violation；
            // Metal 上只能靠隐式 hazard tracking 兜底（blit encoder 切换时驱动
            // 介入），全链启用时的启动 GPU 楔死（WindowServer 连带崩溃、
            // validation 无报告）与此形态吻合。
            ResourceBarrier outWriteBarrier{};
            outWriteBarrier.resource = outHandle;
            outWriteBarrier.beforeState = ResourceState::UnorderedAccess;
            outWriteBarrier.afterState = ResourceState::CopySource;
            outWriteBarrier.subresource = 0xFFFFFFFF;
            outWriteBarrier.queueFamily = 0xFFFFFFFF;
            cmd->InsertBarrier(&outWriteBarrier, 1);

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

            // BlitTexture leaves the destination in TRANSFER_DST_OPTIMAL; the
            // next frame binds this slot as a sampled image, which requires
            // SHADER_READ_ONLY. InsertBarrier derives oldLayout from the
            // texture's tracked layout, so this is a no-op when already correct.
            ResourceBarrier histBarrier{};
            histBarrier.resource = histWriteHandle;
            histBarrier.beforeState = ResourceState::CopyDest;
            histBarrier.afterState = ResourceState::ShaderResource;
            histBarrier.subresource = 0xFFFFFFFF;
            histBarrier.queueFamily = 0xFFFFFFFF;
            cmd->InsertBarrier(&histBarrier, 1);

            s_HistoryInit[fi] = true;
            if (vulkan) {
                s_RasterHistory[fi] = {frameIndex,actualJitter,depthInputs.currDepth,actualRasterValid};
            }
        }
    );
}

void ResetTAAHistory() {
    ++s_ResetGeneration;
    // Forcing re-init causes next TAA pass to skip blending with the stale
    // history texture and overwrite it with the current frame's HDR.
    for (u32 i = 0u; i < MAX_FRAMES; ++i) {
        s_HistoryInit[i] = false;
        s_RasterHistory[i] = {};
    }
    s_LastFrameIndex = ~0u;
}

} // namespace primal::graphics::PostProcess
