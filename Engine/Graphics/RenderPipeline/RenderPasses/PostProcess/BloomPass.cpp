#include "BloomPass.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/Passes/BlurPass.h"
#include "Graphics/Utils/ShaderRegistry.h"
#include "Graphics/Dawn/ShaderLoader.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cstring>
#include "Engine/Common/CoreLogStream.h"

namespace primal::graphics::PostProcess {

using namespace rhi;

static PipelineHandle s_BrightPassPipeline = handles::INVALID_PIPELINE;
static PipelineLayoutHandle s_BrightPassLayout = handles::INVALID_PIPELINE_LAYOUT;
static DescriptorSetLayoutHandle s_BrightPassDSL = handles::INVALID_DESCRIPTOR_SET_LAYOUT;
static SamplerHandle s_BrightPassSampler = handles::INVALID_SAMPLER;

static std::unique_ptr<BlurPass> s_BlurPass = nullptr;
static constexpr u32 MAX_FRAMES = 3;

// P3: bright-pass 参数（每帧由 AddBloomPass 调用方给定，默认 = 旧常数）
static BloomParams s_BloomParams{};
// P3: BloomParams UBO（triple-buffered，随 DSL 建立时创建）
static ResourceHandle s_BloomParamsBuf[MAX_FRAMES] = {
    handles::INVALID_RESOURCE, handles::INVALID_RESOURCE, handles::INVALID_RESOURCE};

struct BloomParamsGPU {
    float threshold;
    float knee;
    float energy;
    float _pad;
};
static_assert(sizeof(BloomParamsGPU) == 16, "BloomParams GPU layout mismatch");

// Bloom is consumed by ToneMapping after the blur has finished.  The old path
// used render-graph transient images and had a recorded alias risk in this
// producer/consumer chain.  Own a separate trio per frame slot and import it
// with a typed descriptor instead, so the physical images remain independent
// for the complete command buffer.
static ResourceHandle s_BloomBrightTex[MAX_FRAMES] = {
    handles::INVALID_RESOURCE, handles::INVALID_RESOURCE, handles::INVALID_RESOURCE};
static ResourceHandle s_BloomOutputTex[MAX_FRAMES] = {
    handles::INVALID_RESOURCE, handles::INVALID_RESOURCE, handles::INVALID_RESOURCE};
static ResourceHandle s_BloomTempTex[MAX_FRAMES] = {
    handles::INVALID_RESOURCE, handles::INVALID_RESOURCE, handles::INVALID_RESOURCE};
// 2026-09-16: 渐进宽模糊链的第三块 ping-pong 缓冲（bright→scratch→output
// 交替；temp 只作单次 Execute 的 H/V 中间）。缺它时链式模糊只能往 bright
// 回写（其 usage 是 RT|SRV，不能当 storage image）。
static ResourceHandle s_BloomScratchTex[MAX_FRAMES] = {
    handles::INVALID_RESOURCE, handles::INVALID_RESOURCE, handles::INVALID_RESOURCE};
static RHIDeviceBase* s_BloomDevice = nullptr;
static u32 s_BloomWidth = 0;
static u32 s_BloomHeight = 0;
static DataFormat s_BloomFormat = DataFormat::Unknown;

// Pre-allocated descriptor set pool (matching SSAO/SSGI pattern — no per-frame allocation)
static constexpr u32 MAX_SETS = 2;
static DescriptorSetHandle s_BrightPassSets[MAX_FRAMES][MAX_SETS] = {
    {handles::INVALID_DESCRIPTOR_SET, handles::INVALID_DESCRIPTOR_SET},
    {handles::INVALID_DESCRIPTOR_SET, handles::INVALID_DESCRIPTOR_SET},
    {handles::INVALID_DESCRIPTOR_SET, handles::INVALID_DESCRIPTOR_SET}};
static u32 s_BrightPassSetIdx[MAX_FRAMES] = {};

static void DestroyBloomTextures(RHIDeviceBase& device) {
    for (u32 i = 0; i < MAX_FRAMES; ++i) {
        if (s_BloomBrightTex[i] != handles::INVALID_RESOURCE) {
            device.DestroyTexture(s_BloomBrightTex[i]);
            s_BloomBrightTex[i] = handles::INVALID_RESOURCE;
        }
        if (s_BloomOutputTex[i] != handles::INVALID_RESOURCE) {
            device.DestroyTexture(s_BloomOutputTex[i]);
            s_BloomOutputTex[i] = handles::INVALID_RESOURCE;
        }
        if (s_BloomTempTex[i] != handles::INVALID_RESOURCE) {
            device.DestroyTexture(s_BloomTempTex[i]);
            s_BloomTempTex[i] = handles::INVALID_RESOURCE;
        }
        if (s_BloomScratchTex[i] != handles::INVALID_RESOURCE) {
            device.DestroyTexture(s_BloomScratchTex[i]);
            s_BloomScratchTex[i] = handles::INVALID_RESOURCE;
        }
    }
    s_BloomWidth = 0;
    s_BloomHeight = 0;
    s_BloomFormat = DataFormat::Unknown;
}

static bool EnsureBloomTextures(RHIDeviceBase& device, const TextureDesc& brightDesc,
                                const TextureDesc& blurDesc) {
    if (s_BloomDevice && s_BloomDevice != &device) {
        // Do not dereference a stale backend pointer.  The owner must shut the
        // pass down before replacing its device; keeping the old handles alive
        // is safer than destroying them through the wrong backend.
        PRIMAL_LOG_STREAM(Error, "Render")
            << "[BLOOM] device changed without ShutdownBloomPass" << std::endl;
        return false;
    }
    s_BloomDevice = &device;

    const bool ready = s_BloomWidth == brightDesc.size.x &&
                       s_BloomHeight == brightDesc.size.y &&
                       s_BloomFormat == brightDesc.format &&
                       s_BloomBrightTex[0] != handles::INVALID_RESOURCE &&
                       s_BloomOutputTex[0] != handles::INVALID_RESOURCE &&
                       s_BloomTempTex[0] != handles::INVALID_RESOURCE &&
                       s_BloomScratchTex[0] != handles::INVALID_RESOURCE;
    if (ready) return true;

    DestroyBloomTextures(device);
    s_BloomWidth = brightDesc.size.x;
    s_BloomHeight = brightDesc.size.y;
    s_BloomFormat = brightDesc.format;

    for (u32 i = 0; i < MAX_FRAMES; ++i) {
        // P0-B 首帧 priming：blur P1/P2 在首帧就把 Temp/Output 绑成
        // SampledImage（承诺 SRO），创建后立即转初始 layout。
        TextureDesc namedBright = brightDesc; namedBright.name = "Bloom_Bright";
        TextureDesc namedBlur = blurDesc;     namedBlur.name = "Bloom_Blur";
        s_BloomBrightTex[i] = device.CreateTexture(namedBright);
        s_BloomOutputTex[i] = device.CreateTexture(namedBlur);
        s_BloomTempTex[i] = device.CreateTexture(namedBlur);
        TextureDesc namedScratch = blurDesc;  namedScratch.name = "Bloom_Scratch";
        s_BloomScratchTex[i] = device.CreateTexture(namedScratch);
        device.PrimeTextureLayout(s_BloomBrightTex[i]);
        device.PrimeTextureLayout(s_BloomOutputTex[i]);
        device.PrimeTextureLayout(s_BloomTempTex[i]);
        device.PrimeTextureLayout(s_BloomScratchTex[i]);
        if (s_BloomBrightTex[i] == handles::INVALID_RESOURCE ||
            s_BloomOutputTex[i] == handles::INVALID_RESOURCE ||
            s_BloomTempTex[i] == handles::INVALID_RESOURCE ||
            s_BloomScratchTex[i] == handles::INVALID_RESOURCE) {
            PRIMAL_LOG_STREAM(Error, "Render")
                << "[BLOOM] persistent texture allocation failed at frame slot " << i << std::endl;
            DestroyBloomTextures(device);
            return false;
        }
    }

    // This check is intentionally based on the physical handles, rather than
    // only the descriptors, so an alias regression is visible in the log.
    for (u32 i = 0; i < MAX_FRAMES; ++i) {
        if (s_BloomBrightTex[i] == s_BloomOutputTex[i] ||
            s_BloomBrightTex[i] == s_BloomTempTex[i] ||
            s_BloomOutputTex[i] == s_BloomTempTex[i] ||
            s_BloomScratchTex[i] == s_BloomBrightTex[i] ||
            s_BloomScratchTex[i] == s_BloomOutputTex[i] ||
            s_BloomScratchTex[i] == s_BloomTempTex[i]) {
            PRIMAL_LOG_STREAM(Error, "Render")
                << "[BLOOM] persistent images unexpectedly share a physical handle at slot " << i
                << " (bright=" << s_BloomBrightTex[i]
                << ", output=" << s_BloomOutputTex[i]
                << ", temp=" << s_BloomTempTex[i]
                << ", scratch=" << s_BloomScratchTex[i] << ")" << std::endl;
            DestroyBloomTextures(device);
            return false;
        }
    }
    PRIMAL_LOG_STREAM(Info, "Render")
        << "[BLOOM] persistent images ready: " << brightDesc.size.x << "x" << brightDesc.size.y
        << ", 3 frame slots, transient aliasing disabled" << std::endl;
    return true;
}

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

struct BloomSetupData {
    RGResourceHandle brightTexture;
};

struct BloomBlurData : public BloomPassData {
    RGResourceHandle tempTexture;
    RGResourceHandle scratchTexture;
};

const BloomPassData& AddBloomPass(RenderGraph& graph, RGResourceHandle inputColor, u32 frameIndex,
                                  const BloomParams& params) {
    u32 fi = frameIndex % MAX_FRAMES;
    s_BrightPassSetIdx[fi] = 0;
    s_BloomParams = params;

    // 1. Bright Pass
    auto& setupData = graph.AddPass<BloomSetupData>("BloomBrightPass", RGPassType::Graphics, RGPassCategory::PostProcess,
        [&](BloomSetupData& data, RenderGraphBuilder& builder) {
            builder.Read(inputColor, ResourceState::ShaderResource);

            TextureDesc brightDesc;
            auto* inputRes = builder.GetGraph().GetResource(inputColor);
            if (inputRes && inputRes->GetType() == RGResourceType::Texture) {
                const auto& desc = static_cast<RenderGraphTexture*>(inputRes)->GetDesc();
                brightDesc.size.x = std::max(1u, desc.size.x / 2);
                brightDesc.size.y = std::max(1u, desc.size.y / 2);
                brightDesc.size.z = 1;
                brightDesc.format = desc.format;
                brightDesc.type = TextureType::Texture2D;
                brightDesc.mipLevels = 1;
                brightDesc.arraySize = 1;
                brightDesc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
            } else {
                // 同 ToneMapping：desc 解析失败不允许落回空 desc（0 尺寸创建
                // 在 Metal validateWithDevice assert）。
                std::cerr << "[BLOOM] bright desc unresolved (input not Texture-typed) — 1x1 fallback" << std::endl;
                brightDesc.size = {1, 1, 1};
                brightDesc.format = DataFormat::RGBA16_Float;
                brightDesc.type = TextureType::Texture2D;
                brightDesc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
            }

            TextureDesc blurDesc = brightDesc;
            blurDesc.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
            if (!EnsureBloomTextures(builder.GetGraph().GetDevice(), brightDesc, blurDesc)) {
                data.brightTexture = {};
                return;
            }

            // Import the module-owned texture with its descriptor so later
            // passes can resolve dimensions and physical handles without
            // falling back to an untyped transient resource.
            data.brightTexture = builder.Write(
                builder.GetGraph().ImportTexture("Bloom_Bright", s_BloomBrightTex[fi], brightDesc,
                                                 ResourceState::Unknown),
                ResourceState::RenderTarget);

            if (s_BrightPassPipeline == handles::INVALID_PIPELINE) {
                auto& device = builder.GetGraph().GetDevice();
                auto platform = device.GetPlatform();
                std::string shaderPath = utils::ShaderRegistry::GetShaderPath(platform, "Bloom");
                // Resolve independently of process CWD（同 TAA/ToneMapping 修复）。
                shaderPath = utils::ShaderRegistry::ResolveShaderPath(shaderPath);
                std::string shaderSource = LoadShaderSource(shaderPath);

                if (!shaderSource.empty()) {
                    ShaderHandle vs = device.CreateShader(shaderSource.data(), shaderSource.size(), ShaderStage::Vertex, "bloom_vs");
                    ShaderHandle fs = device.CreateShader(shaderSource.data(), shaderSource.size(), ShaderStage::Pixel, "bright_pass_fs");

                    if (vs != handles::INVALID_SHADER && fs != handles::INVALID_SHADER) {
                        // DSL: binding 0 = input HDR texture, binding 1 = sampler,
                        // binding 2 = BloomParams UBO (P3)
                        DescriptorSetLayoutBinding bindings[3]{};
                        bindings[0].binding = 0;
                        bindings[0].descriptorType = DescriptorType::SampledImage;
                        bindings[0].descriptorCount = 1;
                        bindings[0].stageFlags = ShaderStage::Pixel;

                        bindings[1].binding = 1;
                        bindings[1].descriptorType = DescriptorType::Sampler;
                        bindings[1].descriptorCount = 1;
                        bindings[1].stageFlags = ShaderStage::Pixel;

                        // P3: binding 2 = BloomParams UBO（threshold/knee/energy）
                        bindings[2].binding = 2;
                        bindings[2].descriptorType = DescriptorType::UniformBuffer;
                        bindings[2].descriptorCount = 1;
                        bindings[2].stageFlags = ShaderStage::Pixel;

                        DescriptorSetLayoutDesc dslDesc;
                        dslDesc.bindingCount = 3;
                        dslDesc.bindings = bindings;
                        s_BrightPassDSL = device.CreateDescriptorSetLayout(dslDesc);

                        // P3: UBO triple-buffered 创建
                        BufferDesc paramsBufDesc{};
                        paramsBufDesc.size = sizeof(BloomParamsGPU);
                        paramsBufDesc.type = BufferType::Constant;
                        paramsBufDesc.usage = GPUMemoryUsage::Dynamic;
                        paramsBufDesc.memoryUsage = GPUMemoryUsage::Dynamic;
                        for (u32 f = 0; f < MAX_FRAMES; ++f)
                            s_BloomParamsBuf[f] = device.CreateBuffer(paramsBufDesc);

                        // Sampler
                        SamplerDesc samplerDesc;
                        samplerDesc.minFilter = FilterMode::Linear;
                        samplerDesc.magFilter = FilterMode::Linear;
                        samplerDesc.addressU = TextureAddressMode::Clamp;
                        samplerDesc.addressV = TextureAddressMode::Clamp;
                        samplerDesc.addressW = TextureAddressMode::Clamp;
                        samplerDesc.comparisonFunc = ComparisonFunc::Never;
                        s_BrightPassSampler = device.CreateSampler(samplerDesc);

                        // Pipeline layout
                        PipelineLayoutDesc layoutDesc;
                        layoutDesc.setLayoutCount = 1;
                        layoutDesc.setLayouts = &s_BrightPassDSL;
                        s_BrightPassLayout = device.CreatePipelineLayout(layoutDesc);

                        GraphicsPipelineDesc pipelineDesc;
                        pipelineDesc.vertexShader = vs;
                        pipelineDesc.pixelShader = fs;
                        pipelineDesc.layout = s_BrightPassLayout;
                        pipelineDesc.topology = PrimitiveTopology::TriangleList;
                        pipelineDesc.renderTargetCount = 1;
                        pipelineDesc.renderTargetFormats[0] = brightDesc.format;
                        pipelineDesc.enableDepthTest = false;
                        pipelineDesc.enableDepthWrite = false;
                        pipelineDesc.cullMode = CullMode::None;
                        s_BrightPassPipeline = device.CreateGraphicsPipeline(pipelineDesc);

                        // Pre-allocate descriptor set pool
                        for (u32 f = 0; f < MAX_FRAMES; ++f)
                            for (u32 s = 0; s < MAX_SETS; ++s) {
                                DescriptorSetDesc dsDesc;
                                dsDesc.layout = s_BrightPassDSL;
                                s_BrightPassSets[f][s] = device.CreateDescriptorSet(dsDesc);
                            }
                    }
                }
            }
        },
        [inputColor, fi](const BloomSetupData& data, RenderGraphContext& context) {
            if (s_BrightPassPipeline == handles::INVALID_PIPELINE) {
                // TEMP 判定：Bloom 管线 INVALID 会让 Bloom_Output 成为从未
                // 被写入的瞬态纹理，下游读到池化垃圾（Vulkan 黑屏排查）。
                static bool s_bloomAbort = false;
                if (!s_bloomAbort) {
                    s_bloomAbort = true;
                    std::cerr << "[BLOOM-EXEC] abort: bright pipeline INVALID" << std::endl;
                }
                return;
            }
            {
                static bool s_bloomExec = false;
                if (!s_bloomExec) {
                    s_bloomExec = true;
                    std::cerr << "[BLOOM-EXEC] ok" << std::endl;
                }
            }
            auto& device = context.graph->GetDevice();
            auto* cmd = context.cmdBuffer;

            auto* brightTex = context.graph->GetResource(data.brightTexture);
            if (!brightTex || brightTex->GetType() != RGResourceType::Texture) return;

            auto* texRes = static_cast<RenderGraphTexture*>(brightTex);

            // Resolve input HDR texture
            auto* inputRes = context.graph->GetResource(inputColor);
            ResourceHandle inputHandle = handles::INVALID_RESOURCE;
            if (inputRes && inputRes->GetType() == RGResourceType::Texture) {
                inputHandle = inputRes->GetPhysicalHandle();
            }

            // Use physical handle directly — updateDescriptorSetsImpl resolves via GetDefaultView()
            ResourceHandle inputView = inputHandle;

            // Use pre-allocated descriptor set from pool
            u32 idx = s_BrightPassSetIdx[fi]++;
            if (idx >= MAX_SETS) { idx = 0; s_BrightPassSetIdx[fi] = 1; }
            DescriptorSetHandle ds = s_BrightPassSets[fi][idx];

            if (inputView != handles::INVALID_RESOURCE && ds != handles::INVALID_RESOURCE) {
                DescriptorImageInfo imageInfos[2];
                imageInfos[0].imageView = inputView;
                imageInfos[0].sampler = s_BrightPassSampler;
                imageInfos[1].imageView = inputView;
                imageInfos[1].sampler = s_BrightPassSampler;

                // P3: 上传 bright-pass 参数（阈值/过渡/能量，默认同旧常数）
                if (s_BloomParamsBuf[fi] != handles::INVALID_RESOURCE) {
                    const BloomParamsGPU gpu{
                        s_BloomParams.threshold, s_BloomParams.knee, s_BloomParams.energy, 0.0f};
                    device.UpdateBufferData(s_BloomParamsBuf[fi], &gpu, sizeof(gpu), 0);
                }

                DescriptorBufferInfo paramsBufInfo{};
                paramsBufInfo.buffer = s_BloomParamsBuf[fi];
                paramsBufInfo.offset = 0;
                paramsBufInfo.range = sizeof(BloomParamsGPU);

                WriteDescriptorSet writes[3] = {};
                writes[0].dstSet = ds; writes[0].dstBinding = 0; writes[0].dstArrayElement = 0;
                writes[0].descriptorCount = 1; writes[0].descriptorType = DescriptorType::SampledImage;
                writes[0].imageInfo = &imageInfos[0];
                writes[1].dstSet = ds; writes[1].dstBinding = 1; writes[1].dstArrayElement = 0;
                writes[1].descriptorCount = 1; writes[1].descriptorType = DescriptorType::Sampler;
                writes[1].imageInfo = &imageInfos[1];
                writes[2].dstSet = ds; writes[2].dstBinding = 2; writes[2].dstArrayElement = 0;
                writes[2].descriptorCount = 1; writes[2].descriptorType = DescriptorType::UniformBuffer;
                writes[2].bufferInfo = &paramsBufInfo;

                device.UpdateDescriptorSets(3, writes);
            }

            // Render
            RenderPassDesc passDesc;
            passDesc.colorAttachments.resize(1);
            passDesc.colorAttachments[0].texture = texRes->GetPhysicalHandle();
            passDesc.colorAttachments[0].loadOp = LoadAction::Clear;
            passDesc.colorAttachments[0].storeOp = StoreAction::Store;
            passDesc.colorAttachments[0].clearValue = ClearValue{ math::v4{ 0.0f, 0.0f, 0.0f, 0.0f } };

            ViewportDesc viewport;
            viewport.size.x = (float)texRes->GetDesc().size.x;
            viewport.size.y = (float)texRes->GetDesc().size.y;
            viewport.minDepth = 0.0f; viewport.maxDepth = 1.0f;

            cmd->BeginRenderPass(passDesc);
            cmd->SetViewport(viewport);
            cmd->SetScissor({ { 0, 0 }, { (u32)viewport.size.x, (u32)viewport.size.y } });
            cmd->BindGraphicsPipeline(s_BrightPassPipeline);

            if (ds != handles::INVALID_RESOURCE) {
                cmd->BindDescriptorSets(PipelineBindPoint::Graphics, s_BrightPassLayout, 0, 1, &ds, 0, nullptr);
            }

            cmd->Draw(3, 0, 1, 0);
            cmd->EndRenderPass();
        }
    );

    // 2. Blur Pass
    auto& blurData = graph.AddPass<BloomBlurData>("BloomBlurPass", RGPassType::Compute, RGPassCategory::PostProcess,
        [&, fi](BloomBlurData& data, RenderGraphBuilder& builder) {
            if (!setupData.brightTexture.IsValid()) {
                data.bloomOutput = {};
                data.tempTexture = {};
                data.scratchTexture = {};
                return;
            }
            builder.Read(setupData.brightTexture, ResourceState::ShaderResource);

            TextureDesc desc;
            auto* inputRes = builder.GetGraph().GetResource(setupData.brightTexture);
            if (inputRes && inputRes->GetType() == RGResourceType::Texture) {
                desc = static_cast<RenderGraphTexture*>(inputRes)->GetDesc();
            } else {
                // 同上：desc 解析失败不允许空 desc 创建（Metal assert）。
                std::cerr << "[BLOOM] blur desc unresolved (bright not Texture-typed) — 1x1 fallback" << std::endl;
                desc.size = {1, 1, 1};
                desc.format = DataFormat::RGBA16_Float;
                desc.type = TextureType::Texture2D;
            }

            desc.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;

            data.bloomOutput = builder.Write(
                builder.GetGraph().ImportTexture("Bloom_Output", s_BloomOutputTex[fi], desc,
                                                 ResourceState::Unknown),
                ResourceState::UnorderedAccess);
            data.tempTexture = builder.Write(
                builder.GetGraph().ImportTexture("Bloom_Temp", s_BloomTempTex[fi], desc,
                                                 ResourceState::Unknown),
                ResourceState::UnorderedAccess);
            data.scratchTexture = builder.Write(
                builder.GetGraph().ImportTexture("Bloom_Scratch", s_BloomScratchTex[fi], desc,
                                                 ResourceState::Unknown),
                ResourceState::UnorderedAccess);

            if (!s_BlurPass) {
                s_BlurPass = std::make_unique<BlurPass>();
                s_BlurPass->Initialize(&builder.GetGraph().GetDevice());
            }
        },
        [&setupData, fi](const BloomBlurData& data, RenderGraphContext& context) {
            if (!s_BlurPass) return;
            // BLUR_OFF=1 内层二分开关：跳过 H/V blur compute，bloomOutput
            // 不会被写入（仅诊断用，画面等于无 bloom）。
            if (std::getenv("BLUR_OFF")) return;

            auto* inputRes = context.graph->GetResource(setupData.brightTexture);
            auto* outputRes = context.graph->GetResource(data.bloomOutput);
            auto* tempRes = context.graph->GetResource(data.tempTexture);
            auto* scratchRes = context.graph->GetResource(data.scratchTexture);

            if (inputRes && outputRes && tempRes && scratchRes &&
                inputRes->GetType() == RGResourceType::Texture) {
                auto* texRes = static_cast<RenderGraphTexture*>(inputRes);
                const u32 w = texRes->GetDesc().size.x;
                const u32 h = texRes->GetDesc().size.y;
                auto in = inputRes->GetPhysicalHandle();
                auto out = outputRes->GetPhysicalHandle();
                auto tmp = tempRes->GetPhysicalHandle();
                auto scratch = scratchRes->GetPhysicalHandle();

                // 2026-09-16: 渐进宽模糊链（参考图直射光晕）。单次 σ2 在
                // 1280×720 下只有 ~10px 光晕，参考观感需要 ~100px。链式
                // 高斯（σ 递增）联合 σ ≈ sqrt(2·Σσi²)；每级 H+V 两次。
                // ping-pong: bright→scratch→output→scratch→output…最终落
                // Bloom_Output；temp 只作每级的 H/V 中间。
                const int passes = std::clamp(s_BloomParams.blurPasses, 1, 4);
                const float sigmas[4] = {2.0f, 3.0f, 5.0f, 8.0f};
                if (passes == 1) {
                    s_BlurPass->Execute(context.cmdBuffer, in, out, tmp, w, h, 1, fi,
                                        5, sigmas[0] * s_BloomParams.blurSigmaBase / 2.0f);
                } else {
                    ResourceHandle src = in;
                    ResourceHandle dst = scratch;
                    for (int i = 0; i < passes; ++i) {
                        const bool last = (i == passes - 1);
                        if (last) dst = out; // 最终结果必须落 Bloom_Output
                        const float sigma = sigmas[i] * s_BloomParams.blurSigmaBase / 2.0f;
                        const int radius = std::max(3, int(sigma * 2.0f + 1.0f));
                        s_BlurPass->Execute(context.cmdBuffer, src, dst, tmp, w, h, 1, fi,
                                            radius, sigma);
                        src = dst;
                        dst = (src == scratch) ? out : scratch;
                    }
                }
            }
        }
    );

    return static_cast<const BloomPassData&>(blurData);
}

void ShutdownBloomPass() {
    if (s_BlurPass) {
        s_BlurPass->Shutdown();
        s_BlurPass.reset();
    }

    if (s_BloomDevice) {
        DestroyBloomTextures(*s_BloomDevice);
        for (u32 f = 0; f < MAX_FRAMES; ++f) {
            for (u32 s = 0; s < MAX_SETS; ++s) {
                if (s_BrightPassSets[f][s] != handles::INVALID_DESCRIPTOR_SET) {
                    s_BloomDevice->DestroyDescriptorSet(s_BrightPassSets[f][s]);
                    s_BrightPassSets[f][s] = handles::INVALID_DESCRIPTOR_SET;
                }
            }
        }
        if (s_BrightPassPipeline != handles::INVALID_PIPELINE)
            s_BloomDevice->DestroyPipeline(s_BrightPassPipeline);
        if (s_BrightPassLayout != handles::INVALID_PIPELINE_LAYOUT)
            s_BloomDevice->DestroyPipelineLayout(s_BrightPassLayout);
        if (s_BrightPassDSL != handles::INVALID_DESCRIPTOR_SET_LAYOUT)
            s_BloomDevice->DestroyDescriptorSetLayout(s_BrightPassDSL);
        if (s_BrightPassSampler != handles::INVALID_SAMPLER)
            s_BloomDevice->DestroySampler(s_BrightPassSampler);
        for (u32 f = 0; f < MAX_FRAMES; ++f) {
            if (s_BloomParamsBuf[f] != handles::INVALID_RESOURCE) {
                s_BloomDevice->DestroyBuffer(s_BloomParamsBuf[f]);
                s_BloomParamsBuf[f] = handles::INVALID_RESOURCE;
            }
        }
        s_BloomDevice = nullptr;
    }
    s_BrightPassPipeline = handles::INVALID_PIPELINE;
    s_BrightPassLayout = handles::INVALID_PIPELINE_LAYOUT;
    s_BrightPassDSL = handles::INVALID_DESCRIPTOR_SET_LAYOUT;
    s_BrightPassSampler = handles::INVALID_SAMPLER;

}

} // namespace primal::graphics::PostProcess
