#include "ToneMappingPass.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/Utils/ShaderRegistry.h"
#include "Graphics/Dawn/ShaderLoader.h"
#include <iostream>

#if defined(ENABLE_WEBGPU) && ENABLE_WEBGPU
#include "Graphics/RHI/Platforms/Dawn/DawnDevice.h"
#endif
#include <fstream>
#include <sstream>
#include <cmath>
#include <chrono>
#include <algorithm>
#include <cstdlib>

namespace primal::graphics::PostProcess {

using namespace rhi;

static PipelineHandle s_ToneMapPipeline = handles::INVALID_PIPELINE;
static PipelineHandle s_ToneMapPassthroughPipeline = handles::INVALID_PIPELINE;

// P0.3 (2026-09-15): 曝光收编为 exposure_ev 单旋钮。
//   exposure = 路径标定基值 × 2^EV；EV 由 PRIMAL_TONE_EV 覆盖（默认 0）。
//   基值与旧编译期常数逐位一致（passthrough 0.26 / ACES 0.10），EV0 时
//   画面零变化。为 P3.1 eye adaptation 预留接口（届时由直方图驱动 EV）。
static float ToneExposureEV() {
    static const float ev = [] {
        const char* e = std::getenv("PRIMAL_TONE_EV");
        return e ? static_cast<float>(std::atof(e)) : 0.0f;
    }();
    return ev;
}
static constexpr float kPassthroughBaseExposure = 0.26f; // 旧 CAL_EXPOSURE
static constexpr float kACESBaseExposure        = 0.10f; // 旧 HDR_ACES_EXPOSURE
struct ToneParamsUBO {
    float exposure;
    float autoAdapt;    // P3.1: >0.5 时由适应态驱动曝光（PRIMAL_TONE_ADAPT=1）
    float key;          // 适应目标（HDR 均值目标；PRIMAL_TONE_KEY，默认 0.45）
    float contrast;     // P3.3: 0.5 枢轴对比（PRIMAL_TONE_CONTRAST，默认 1；P3 修正语义）
    float saturation;   // P3.3: 饱和（PRIMAL_TONE_SAT，默认 1.15）
    float vignette;     // P3.3: 暗角强度（PRIMAL_TONE_VIGNETTE，默认 0.45）
    float bloomIntensity; // P3: Bloom 合成增益（PRIMAL_BLOOM_INTENSITY，默认 1 = 原行为）
    u32   _pad1;
};
static_assert(sizeof(ToneParamsUBO) == 32, "ToneParams UBO must be 32 bytes");
static float EnvFloat(const char* name, float def) {
    const char* e = std::getenv(name);
    return e ? static_cast<float>(std::atof(e)) : def;
}
// P3.3 旋钮。
// [2026-09-22 参考观感复验] 显示域 contrast=1.35 会在 ACES+gamma 后
// 同时截黑暗部和截白高光。锁机位 frame360 A/B 回归 1.0 后，任意通道
// 剪白 4.45%→0，左廊 ROI 4.5→26.0，direct/interior 7.21→3.70，
// 同时通过现有物理关系门禁。GI 供给与直射链零改动。
// 注意间接增益（PRIMAL_GI_INDIRECT_GAIN）不可替代此项：在 SC↔probe 回路
// 平衡点下方存在分叉（0.9→0.85 间画面跳变），且自适应曝光会吸收量级变化。
static const float s_GradeContrast = EnvFloat("PRIMAL_TONE_CONTRAST", 1.0f);
static const float s_GradeSat      = EnvFloat("PRIMAL_TONE_SAT", 1.15f);
// [2026-09-22 参考观感空间复验] 0.45 在不改变 GI 能量的情况下压低画面
// 边缘约 4%~6%，让拱廊与近景地面更接近参考构图；0.8 虽仍通过门禁，
// 但作为全局默认会过度介入非 Sponza 场景，因此保留为显式环境变量上限。
static const float s_GradeVignette = EnvFloat("PRIMAL_TONE_VIGNETTE", 0.45f);
// [2026-09-22] contrast 回归 1.0 后，key 0.55→0.45 把线性均值亮度
// 0.139→0.117，保留左廊细节（26.0→22.7，参考 22.5），剪白仍为 0。
// saturation 1.15 把均值饱和度 0.422→0.472（参考 0.495），不改变 ROI 亮度。
static const float s_AdaptKey      = EnvFloat("PRIMAL_TONE_KEY", 0.45f);
// P3: Bloom 合成增益（接线 BloomConfig.intensity 的运行期入口）。
// 2026-09-16 默认 1.0→4.0（参考图重标定）：参考观感的直射光晕需要
// "亮通 E=1.0 × 合成 4.0"（BloomPass.h 注释的变体 D 实测）；bloom 本体
// 默认只在 Vulkan 参考路径开启，此默认不影响 Metal/legacy 观感。
static const float s_BloomIntensity = EnvFloat("PRIMAL_BLOOM_INTENSITY", 4.0f);
// 与执行期 usePassthrough 判定同口径的基值选择（不含管线有效性——
// 管线 INVALID 时整个 pass 不绘制，无曝光消费方）。
static float ToneBaseExposure(bool upstreamHDR, bool forceACES, bool forcePassthrough) {
    const bool passthrough = !forceACES && !upstreamHDR && !forcePassthrough;
    return passthrough ? kPassthroughBaseExposure : kACESBaseExposure;
}

// P3.1 (2026-09-15): eye adaptation——GPU 亮度统计（EyeAdaptation.comp，
// 单 workgroup 稀疏采样 + EMA）写 1×1 适应态（r32f，log2 平均亮度），
// tonemap 按 key/avgLum 缩放曝光。Vulkan-only v1（管线创建与调度均由
// 平台检查守卫，Metal 无此 pass，翻转默认不影响）。
// [2026-09-20 默认翻转 关→开]（差距分析 P3，差距④主单项）：固定曝光把
// 室内外压进同一显示档；眼适应以亮区为基准压曝光，室内外亮度分层向 UE
// 对齐。PRIMAL_TONE_ADAPT=0 显式回退固定 EV0 基线。
static const bool s_AdaptEnabled = [] {
    const char* e = std::getenv("PRIMAL_TONE_ADAPT");
    return !(e && e[0] == '0');   // 默认开；=0 回退固定曝光
}();
static PipelineHandle s_AdaptPipeline = handles::INVALID_PIPELINE;
static DescriptorSetLayoutHandle s_AdaptDSL = handles::INVALID_DESCRIPTOR_SET_LAYOUT;
static PipelineLayoutHandle s_AdaptLayout = handles::INVALID_PIPELINE_LAYOUT;
static ResourceHandle s_AdaptState[2] = {handles::INVALID_RESOURCE, handles::INVALID_RESOURCE};
static DescriptorSetHandle s_AdaptSets[3] = {
    handles::INVALID_DESCRIPTOR_SET, handles::INVALID_DESCRIPTOR_SET,
    handles::INVALID_DESCRIPTOR_SET};
static u32 s_AdaptPing = 0;
// §4.9: 适应参数 UBO（triple-buffered）——rateUp/rateDown 按墙钟帧间隔与
// 时间常数 τ_up/τ_down 每帧计算；brightness clip 与 EV 上下限给出适应的
// 合理范围。
struct AdaptParamsGPU {
    float rateUp;
    float rateDown;
    float minLogLum;
    float maxLogLum;
    float brightClip;
    float cutSnap;
    float _pad[2];
};
static_assert(sizeof(AdaptParamsGPU) == 32, "AdaptParams GPU layout mismatch");
static ResourceHandle s_AdaptParamsBuf[3] = {
    handles::INVALID_RESOURCE, handles::INVALID_RESOURCE, handles::INVALID_RESOURCE};
static std::chrono::steady_clock::time_point s_AdaptLastTime{};
static bool s_AdaptHasTime = false;
static float s_LastUboExposure = 0.0f;  // sun-audit P3: 最近一次写入 UBO 的曝光
// PRIMAL_TONE_ACES=1：强制走完整 ACES+gamma 展示管线（默认按平台选择）。
static const bool s_ForceACES = std::getenv("PRIMAL_TONE_ACES") != nullptr;
// PRIMAL_TONE_PASSTHROUGH=1：强制回旧的透传行为（A/B 门——透传在 Vulkan 上
// 是 HDR×0.26 硬裁剪直出，仅用于与旧观感对比）。
static const bool s_ForcePassthrough = std::getenv("PRIMAL_TONE_PASSTHROUGH") != nullptr;
// 上游单位制决定 tonemap 形态（EnsurePipeline 按 device 平台填充）：
//   Metal：fusion 上游已做过 exposure+ACES+gamma（LDR 输出）→ 透传。
//   Vulkan/Dawn：FusionComposite/DeferredLighting 输出线性 HDR → ACES+gamma。
// 旧默认（全平台透传）在 Vulkan 上把 HDR ×0.26 后硬裁剪直出：>1/0.26≈3.85
// 全部贴白、无肩部、无 gamma 编码——向阳面纹理全灭、阴影中调线性压死
// （2026-09-11 全链审计：56.6% 像素 <5%、9.3% 贴白正是该变换的签名）。
static bool s_UpstreamIsHDR = false;
static PipelineLayoutHandle s_ToneMapLayout = handles::INVALID_PIPELINE_LAYOUT;

ToneProvenance GetToneProvenance() {
    ToneProvenance p{};
    p.exposure_ev = ToneExposureEV();
    p.base_exposure = ToneBaseExposure(s_UpstreamIsHDR, s_ForceACES, s_ForcePassthrough);
    p.exposure = p.base_exposure * std::pow(2.0f, p.exposure_ev);
    p.adapt_enabled = s_AdaptEnabled;
    p.force_aces = s_ForceACES;
    p.force_passthrough = s_ForcePassthrough;
    p.contrast = s_GradeContrast;
    p.saturation = s_GradeSat;
    p.vignette = s_GradeVignette;
    p.key = s_AdaptKey;
    p.bloom_intensity = s_BloomIntensity;
    return p;
}
static DescriptorSetLayoutHandle s_ToneMapDSL = handles::INVALID_RESOURCE;
static SamplerHandle s_ToneMapSampler = handles::INVALID_SAMPLER;
static ResourceHandle s_DummyTexture = handles::INVALID_RESOURCE;
static ResourceHandle s_DummyTextureView = handles::INVALID_RESOURCE;
static ResourceHandle s_DummyAOTexture = handles::INVALID_RESOURCE;
static ResourceHandle s_DummyAOTextureView = handles::INVALID_RESOURCE;
static ResourceHandle s_DummySSGITexture = handles::INVALID_RESOURCE;
static ResourceHandle s_DummySSGITextureView = handles::INVALID_RESOURCE;
static ResourceHandle s_DummyVelocityTexture = handles::INVALID_RESOURCE;
static ResourceHandle s_DummyVelocityTextureView = handles::INVALID_RESOURCE;

// Pre-allocated descriptor set pool (no per-frame allocation)
static constexpr u32 MAX_FRAMES = 3;
static constexpr u32 MAX_SETS = 2;
static DescriptorSetHandle s_ToneMapSets[MAX_FRAMES][MAX_SETS] = {};
static u32 s_ToneMapSetIdx[MAX_FRAMES] = {};
// P0.3: 曝光参数 UBO（三帧环，EnsurePipeline 创建）
static ResourceHandle s_ToneParamsBuf[MAX_FRAMES] = {
    handles::INVALID_RESOURCE, handles::INVALID_RESOURCE, handles::INVALID_RESOURCE};

// 模块持有的输出纹理（2026-09-02 Vulkan 黑屏二分定位）：输出曾用 RG 瞬态，
// FinalBlit 采样全黑；FinalBlit 直读 TAA 的模块自有 ImportTexture 输出则
// 完全正常（TONE_OFF 对照实验）。瞬态池生命周期/状态追踪与裸 BeginRenderPass
// 的组合在 Vulkan 上产生黑帧。与 TAAPass s_OutputTex 同方案：模块持有 +
// ImportTexture 进图，三帧环随 render 尺寸重建。
static ResourceHandle s_OutputTex[MAX_FRAMES] = {
    handles::INVALID_RESOURCE, handles::INVALID_RESOURCE, handles::INVALID_RESOURCE};
static u32 s_OutTexWidth = 0;
static u32 s_OutTexHeight = 0;

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

static void EnsurePipeline(RHIDeviceBase& device) {
    if (s_ToneMapPipeline != handles::INVALID_PIPELINE) return;

    auto platform = device.GetPlatform();
    s_UpstreamIsHDR = platform != RHIPlatform::Metal;
    // D3D12 多入口单模块拆分：ToneMapping.spv 含 tonemap_vs/tonemap_fs/
    // tonemap_fs_passthrough 三个入口，DXIL 每入口一个 cso（协调约定
    // ToneMapping.<entry>.cso，入口恒 "main"）。Vulkan spv / Metal 源码保持
    // 单文件多入口形态。
    const bool d3d12 = (platform == RHIPlatform::D3D12);
    std::string shaderSource;
    if (!d3d12) {
        std::string shaderPath = utils::ShaderRegistry::GetShaderPath(platform, "ToneMapping");
        // Resolve independently of process CWD（同 T8.13 对 TAA 的修复——从构建
        // 目录启动时裸 CWD 相对路径打不开文件，管线静默 INVALID，ToneMapping 整体旁路）。
        shaderPath = utils::ShaderRegistry::ResolveShaderPath(shaderPath);
        shaderSource = LoadShaderSource(shaderPath);
        if (shaderSource.empty()) {
            std::cerr << "[ToneMapping] Shader source not found: " << shaderPath << std::endl;
            return;
        }
    }
    auto stageShader = [&](ShaderStage stage, const char* entry) -> ShaderHandle {
        if (d3d12) {
            const std::string rel = std::string("EngineTest/shaders/d3d12/ToneMapping.") + entry + ".cso";
            const std::string path = utils::ShaderRegistry::ResolveShaderPath(rel);
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file.is_open()) {
                std::cerr << "[ToneMapping] cso not found: " << path << std::endl;
                return handles::INVALID_SHADER;
            }
            std::streamsize size = file.tellg();
            file.seekg(0, std::ios::beg);
            std::vector<char> bytes(static_cast<size_t>(size));
            if (!file.read(bytes.data(), size)) return handles::INVALID_SHADER;
            return device.CreateShader(bytes.data(), bytes.size(), stage, "main");
        }
        return device.CreateShader(shaderSource.data(), shaderSource.size(), stage, entry);
    };

    ShaderHandle vs = stageShader(ShaderStage::Vertex, "tonemap_vs");
    ShaderHandle fs = stageShader(ShaderStage::Pixel, "tonemap_fs");
    if (vs == handles::INVALID_SHADER || fs == handles::INVALID_SHADER) return;

    // DSL: binding 0 = scene texture, binding 1 = bloom texture, binding 2 = sampler,
    //      binding 3 = AO texture, binding 4 = SSGI texture, binding 5 = velocity texture
    // DSL: binding 0 = scene texture, binding 1 = bloom texture, binding 2 = sampler,
    //      binding 3 = AO texture, binding 4 = SSGI texture, binding 5 = velocity texture,
    //      binding 6 = ToneParams UBO (P0.3 exposure_ev)
    DescriptorSetLayoutBinding bindings[8]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = DescriptorType::SampledImage;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = ShaderStage::Pixel;

    bindings[1].binding = 1;
    bindings[1].descriptorType = DescriptorType::SampledImage;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = ShaderStage::Pixel;

    bindings[2].binding = 2;
    bindings[2].descriptorType = DescriptorType::Sampler;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = ShaderStage::Pixel;

    bindings[3].binding = 3;
    bindings[3].descriptorType = DescriptorType::SampledImage;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags = ShaderStage::Pixel;

    bindings[4].binding = 4;
    bindings[4].descriptorType = DescriptorType::SampledImage;
    bindings[4].descriptorCount = 1;
    bindings[4].stageFlags = ShaderStage::Pixel;

    bindings[5].binding = 5;
    bindings[5].descriptorType = DescriptorType::SampledImage;
    bindings[5].descriptorCount = 1;
    bindings[5].stageFlags = ShaderStage::Pixel;

    // P0.3: binding 6 = ToneParams UBO（exposure_ev 旋钮，三端 shader 同布局）
    bindings[6].binding = 6;
    bindings[6].descriptorType = DescriptorType::UniformBuffer;
    bindings[6].descriptorCount = 1;
    bindings[6].stageFlags = ShaderStage::Pixel;

    // P3.1: binding 7 = adaptation state（1×1 r32f，log2 avgLum；Vulkan only）
    bindings[7].binding = 7;
    bindings[7].descriptorType = DescriptorType::SampledImage;
    bindings[7].descriptorCount = 1;
    bindings[7].stageFlags = ShaderStage::Pixel;

    DescriptorSetLayoutDesc dslDesc;
    dslDesc.bindingCount = 8;
    dslDesc.bindings = bindings;
    s_ToneMapDSL = device.CreateDescriptorSetLayout(dslDesc);

    // Sampler
    SamplerDesc samplerDesc;
    samplerDesc.minFilter = FilterMode::Linear;
    samplerDesc.magFilter = FilterMode::Linear;
    samplerDesc.addressU = TextureAddressMode::Clamp;
    samplerDesc.addressV = TextureAddressMode::Clamp;
    samplerDesc.addressW = TextureAddressMode::Clamp;
    samplerDesc.comparisonFunc = ComparisonFunc::Never;
    s_ToneMapSampler = device.CreateSampler(samplerDesc);

    // Dummy 1x1 black texture for missing bloom
    TextureDesc dummyTexDesc;
    dummyTexDesc.size = {1, 1, 1};
    dummyTexDesc.format = DataFormat::RGBA8_UNorm;
    dummyTexDesc.type = TextureType::Texture2D;
    dummyTexDesc.mipLevels = 1;
    dummyTexDesc.usage = TextureUsage::ShaderResource;
    s_DummyTexture = device.CreateTexture(dummyTexDesc);
    device.PrimeTextureLayout(s_DummyTexture);  // P0-B 首帧 priming（bloom 槽 stand-in，此前漏掉）
    device.PrimeTextureLayout(s_DummyTexture);  // P0-B 首帧 priming
    if (s_DummyTexture != handles::INVALID_RESOURCE) {
        TextureViewDesc viewDesc;
        viewDesc.texture = s_DummyTexture;
        viewDesc.viewType = TextureType::Texture2D;
        viewDesc.format = DataFormat::RGBA8_UNorm;
        s_DummyTextureView = device.CreateTextureView(viewDesc);
    }

    // Pipeline layout
    PipelineLayoutDesc layoutDesc;
    layoutDesc.setLayoutCount = 1;
    layoutDesc.setLayouts = &s_ToneMapDSL;
    s_ToneMapLayout = device.CreatePipelineLayout(layoutDesc);

    // Dummy 1x1 white texture for AO (AO=1.0, no occlusion)
    TextureDesc dummyAODesc;
    dummyAODesc.size = {1, 1, 1};
    dummyAODesc.format = DataFormat::RGBA8_UNorm;
    dummyAODesc.type = TextureType::Texture2D;
    dummyAODesc.mipLevels = 1;
    dummyAODesc.usage = TextureUsage::ShaderResource;
    s_DummyAOTexture = device.CreateTexture(dummyAODesc);
    device.PrimeTextureLayout(s_DummyAOTexture);  // P0-B 首帧 priming
    if (s_DummyAOTexture != handles::INVALID_RESOURCE) {
#if defined(ENABLE_WEBGPU) && ENABLE_WEBGPU
        unsigned char white[4] = {255, 255, 255, 255};
        auto* dawnDev = dynamic_cast<DawnDevice*>(&device);
        if (dawnDev) {
            dawnDev->UpdateTextureData(s_DummyAOTexture, white, 0, 0, 0, 1, 1, 1, 4, 0);
        }
#endif
        TextureViewDesc aoViewDesc;
        aoViewDesc.texture = s_DummyAOTexture;
        aoViewDesc.viewType = TextureType::Texture2D;
        aoViewDesc.format = DataFormat::RGBA8_UNorm;
        s_DummyAOTextureView = device.CreateTextureView(aoViewDesc);
    }

    // Dummy 1x1 black texture for SSGI (no indirect lighting)
    TextureDesc dummySSGIDesc;
    dummySSGIDesc.size = {1, 1, 1};
    dummySSGIDesc.format = DataFormat::RGBA8_UNorm;
    dummySSGIDesc.type = TextureType::Texture2D;
    dummySSGIDesc.mipLevels = 1;
    dummySSGIDesc.usage = TextureUsage::ShaderResource;
    s_DummySSGITexture = device.CreateTexture(dummySSGIDesc);
    device.PrimeTextureLayout(s_DummySSGITexture);  // P0-B 首帧 priming
    if (s_DummySSGITexture != handles::INVALID_RESOURCE) {
        TextureViewDesc ssgiViewDesc;
        ssgiViewDesc.texture = s_DummySSGITexture;
        ssgiViewDesc.viewType = TextureType::Texture2D;
        ssgiViewDesc.format = DataFormat::RGBA8_UNorm;
        s_DummySSGITextureView = device.CreateTextureView(ssgiViewDesc);
    }

    // Dummy 1x1 black texture for velocity (RG16F, zero motion)
    TextureDesc dummyVelDesc;
    dummyVelDesc.size = {1, 1, 1};
    dummyVelDesc.format = DataFormat::RG16_Float;
    dummyVelDesc.type = TextureType::Texture2D;
    dummyVelDesc.mipLevels = 1;
    dummyVelDesc.usage = TextureUsage::ShaderResource;
    s_DummyVelocityTexture = device.CreateTexture(dummyVelDesc);
    device.PrimeTextureLayout(s_DummyVelocityTexture);  // P0-B 首帧 priming
    if (s_DummyVelocityTexture != handles::INVALID_RESOURCE) {
        TextureViewDesc velViewDesc;
        velViewDesc.texture = s_DummyVelocityTexture;
        velViewDesc.viewType = TextureType::Texture2D;
        velViewDesc.format = DataFormat::RG16_Float;
        s_DummyVelocityTextureView = device.CreateTextureView(velViewDesc);
    }

    // Pipeline
    GraphicsPipelineDesc pipelineDesc;
    pipelineDesc.vertexShader = vs;
    pipelineDesc.pixelShader = fs;
    pipelineDesc.layout = s_ToneMapLayout;
    pipelineDesc.topology = PrimitiveTopology::TriangleList;
    pipelineDesc.renderTargetCount = 1;
    pipelineDesc.renderTargetFormats[0] = DataFormat::RGBA8_UNorm;
    pipelineDesc.enableDepthTest = false;
    pipelineDesc.enableDepthWrite = false;
    pipelineDesc.cullMode = CullMode::None;

    s_ToneMapPipeline = device.CreateGraphicsPipeline(pipelineDesc);

    // 透传管线（仅 bloom 合成，全平台）：两平台的 fusion 上游输出都是线性
    // HDR，legacy 基准观感 = 直通显示（无 ACES）。ACES 完整管线保留为
    // PRIMAL_TONE_ACES=1 的可选展示路径（见 exec 的选择逻辑）。
    {
        ShaderHandle fsPT = stageShader(ShaderStage::Pixel, "tonemap_fs_passthrough");
        if (fsPT != handles::INVALID_SHADER) {
            GraphicsPipelineDesc ptDesc = pipelineDesc;
            ptDesc.pixelShader = fsPT;
            s_ToneMapPassthroughPipeline = device.CreateGraphicsPipeline(ptDesc);
        }
        if (s_ToneMapPassthroughPipeline == handles::INVALID_PIPELINE) {
            std::cerr << "[ToneMapping] passthrough pipeline unavailable — using ACES" << std::endl;
        }
    }

    // Pre-allocate descriptor set pool
    for (u32 f = 0; f < MAX_FRAMES; ++f)
        for (u32 s = 0; s < MAX_SETS; ++s) {
            DescriptorSetDesc dsDesc;
            dsDesc.layout = s_ToneMapDSL;
            s_ToneMapSets[f][s] = device.CreateDescriptorSet(dsDesc);
        }

    // P0.3: 曝光参数 UBO（三帧环）。Dynamic 常量缓冲，每帧 execute 期上传。
    for (u32 f = 0; f < MAX_FRAMES; ++f) {
        BufferDesc paramDesc{};
        paramDesc.size = sizeof(ToneParamsUBO);
        paramDesc.type = BufferType::Constant;
        paramDesc.usage = GPUMemoryUsage::Dynamic;
        paramDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        paramDesc.name = "ToneMapping_Params";
        s_ToneParamsBuf[f] = device.CreateBuffer(paramDesc);
    }

    // P3.1: eye adaptation 资源（Vulkan-only v1；PRIMAL_TONE_ADAPT=1 启用）
    // P3.1: 资源始终创建+dispatch 始终跑（开销≈0）——binding 7 是 tonemap
    // shader 的静态引用，必须始终有效；是否采纳适应值由 UBO 的 autoAdapt
    // 门控（PRIMAL_TONE_ADAPT=1）。
    if (device.GetPlatform() == RHIPlatform::Vulkan ||
        device.GetPlatform() == RHIPlatform::D3D12) {
        // spv: Lumen/EyeAdaptation.comp.spv（build_spv.sh Volume 同表）。
        // D3D12: Lumen/EyeAdaptation.comp.cso 镜像（入口恒 "main"，后端忽略
        // entry 名——与 spv 的 eye_adaptation 入口等价消费）。
        const bool d3d12Adapt = device.GetPlatform() == RHIPlatform::D3D12;
        const std::string adaptRel = d3d12Adapt
            ? "EngineTest/shaders/d3d12/Lumen/EyeAdaptation.comp.cso"
            : "Engine/Graphics/Vulkan/shaders/Lumen/EyeAdaptation.comp.spv";
        const std::string adaptPath = utils::ShaderRegistry::ResolveShaderPath(adaptRel);
        std::ifstream adaptFile(adaptPath, std::ios::binary | std::ios::ate);
        if (adaptFile.is_open()) {
            std::streamsize asz = adaptFile.tellg();
            adaptFile.seekg(0);
            std::vector<char> acode(static_cast<size_t>(asz));
            adaptFile.read(acode.data(), asz);
            ShaderHandle adaptCS = device.CreateShader(acode.data(), asz, ShaderStage::Compute,
                                                       d3d12Adapt ? "main" : "eye_adaptation");
            if (adaptCS != handles::INVALID_SHADER) {
                DescriptorSetLayoutBinding abindings[4]{};
                abindings[0] = {0, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr};
                abindings[1] = {1, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr};
                abindings[2] = {2, DescriptorType::StorageImage, 1, ShaderStage::Compute, nullptr};
                abindings[3] = {3, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr};
                DescriptorSetLayoutDesc adesc{4, abindings};
                s_AdaptDSL = device.CreateDescriptorSetLayout(adesc);

                // §4.9: 适应参数 UBO triple-buffered
                BufferDesc adaptParamDesc{};
                adaptParamDesc.size = sizeof(AdaptParamsGPU);
                adaptParamDesc.type = BufferType::Constant;
                adaptParamDesc.usage = GPUMemoryUsage::Dynamic;
                adaptParamDesc.memoryUsage = GPUMemoryUsage::Dynamic;
                for (u32 f = 0; f < MAX_FRAMES; ++f)
                    s_AdaptParamsBuf[f] = device.CreateBuffer(adaptParamDesc);
                PipelineLayoutDesc aplDesc;
                aplDesc.setLayoutCount = 1;
                aplDesc.setLayouts = &s_AdaptDSL;
                s_AdaptLayout = device.CreatePipelineLayout(aplDesc);
                ComputePipelineDesc apipeDesc{};
                apipeDesc.computeShader = adaptCS;
                apipeDesc.layout = s_AdaptLayout;
                apipeDesc.threadGroupSize = {16, 16, 1};
                s_AdaptPipeline = device.CreateComputePipeline(apipeDesc);

                // 1×1 r32f 适应态 ×2（ping-pong；读写双用途）
                for (u32 i = 0; i < 2; ++i) {
                    TextureDesc sd{};
                    sd.size = {1, 1, 1};
                    sd.format = DataFormat::R32_Float;
                    sd.type = TextureType::Texture2D;
                    sd.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
                    sd.name = i == 0 ? "AdaptStateA" : "AdaptStateB";
                    s_AdaptState[i] = device.CreateTexture(sd);
                }
                for (u32 f = 0; f < MAX_FRAMES; ++f) {
                    DescriptorSetDesc asdsDesc{};
                    asdsDesc.layout = s_AdaptDSL;
                    s_AdaptSets[f] = device.CreateDescriptorSet(asdsDesc);
                }
                std::cerr << "[TONE] adaptation resources ready: pipe="
                          << (s_AdaptPipeline != handles::INVALID_PIPELINE)
                          << " dsl=" << (s_AdaptDSL != handles::INVALID_DESCRIPTOR_SET_LAYOUT)
                          << " state0=" << (s_AdaptState[0] != handles::INVALID_RESOURCE)
                          << " sets=" << (s_AdaptSets[0] != handles::INVALID_DESCRIPTOR_SET)
                          << std::endl;
            } else {
                std::cerr << "[TONE] adaptation spv load failed — auto adapt off" << std::endl;
            }
        }
    }
}

static void EnsureOutputTextures(RHIDeviceBase& device, u32 width, u32 height) {
    if (s_OutputTex[0] != handles::INVALID_RESOURCE &&
        s_OutTexWidth == width && s_OutTexHeight == height) {
        return;
    }
    // 尺寸变化或首帧：全部重建（destroyTexture 是 GC 延迟的，上一帧 in-flight 安全）
    for (u32 i = 0; i < MAX_FRAMES; ++i) {
        if (s_OutputTex[i] != handles::INVALID_RESOURCE) {
            device.DestroyTexture(s_OutputTex[i]);
            s_OutputTex[i] = handles::INVALID_RESOURCE;
        }
    }
    s_OutTexWidth = width;
    s_OutTexHeight = height;
}

const ToneMappingPassData& AddToneMappingPass(RenderGraph& graph, RGResourceHandle inputHDR, RGResourceHandle bloomTexture, RGResourceHandle aoTexture, RGResourceHandle ssgiTexture, RGResourceHandle velocityTexture, u32 frameIndex) {
    u32 fi = frameIndex % MAX_FRAMES;
    s_ToneMapSetIdx[fi] = 0;

    return graph.AddPass<ToneMappingPassData>("ToneMappingPass", RGPassType::Graphics, RGPassCategory::PostProcess,
        [&](ToneMappingPassData& data, RenderGraphBuilder& builder) {
            builder.Read(inputHDR, ResourceState::ShaderResource);
            if (bloomTexture != kInvalidRGResourceHandle) {
                builder.Read(bloomTexture, ResourceState::ShaderResource);
            }
            if (aoTexture != kInvalidRGResourceHandle) {
                builder.Read(aoTexture, ResourceState::ShaderResource);
            }
            if (ssgiTexture != kInvalidRGResourceHandle) {
                builder.Read(ssgiTexture, ResourceState::ShaderResource);
            }
            if (velocityTexture != kInvalidRGResourceHandle) {
                builder.Read(velocityTexture, ResourceState::ShaderResource);
            }

            // Create output texture
            TextureDesc outputDesc;
            auto* inputRes = builder.GetGraph().GetResource(inputHDR);
            if (inputRes && inputRes->GetType() == RGResourceType::Texture) {
                const auto& desc = static_cast<RenderGraphTexture*>(inputRes)->GetDesc();
                outputDesc.size.x = desc.size.x;
                outputDesc.size.y = desc.size.y;
                outputDesc.size.z = 1;
                outputDesc.format = DataFormat::RGBA8_UNorm;
                outputDesc.type = TextureType::Texture2D;
                outputDesc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
            } else {
                // desc 解析失败绝不能落回默认空 desc——0 尺寸/pixelFormat 0
                // 会在 Metal validateWithDevice assert（2026-09-02 现场实证）。
                // FusionOutput 曾以 ImportResource（Unknown 类型）注册而触发。
                std::cerr << "[TONE] output desc unresolved (input not Texture-typed) — "
                             "fallback 0-size texture will be skipped" << std::endl;
                outputDesc.size = {1, 1, 1};
                outputDesc.format = DataFormat::RGBA8_UNorm;
                outputDesc.type = TextureType::Texture2D;
                outputDesc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
            }

            // 模块自有输出（见 s_OutputTex 注释）——绕开 RG 瞬态池。
            auto& dev = builder.GetGraph().GetDevice();
            EnsureOutputTextures(dev, outputDesc.size.x, outputDesc.size.y);
            if (s_OutputTex[fi] == handles::INVALID_RESOURCE) {
                outputDesc.mipLevels = 1;
                s_OutputTex[fi] = dev.CreateTexture(outputDesc);
            }
            if (s_OutputTex[fi] == handles::INVALID_RESOURCE) {
                std::cerr << "[TONE] module output texture creation failed — pass will render nothing" << std::endl;
            }

            data.output = builder.Write(
                graph.ImportTexture("ToneMapping_Output", s_OutputTex[fi], outputDesc,
                                    ResourceState::RenderTarget),
                ResourceState::RenderTarget);

            EnsurePipeline(builder.GetGraph().GetDevice());
        },
        [inputHDR, bloomTexture, aoTexture, ssgiTexture, velocityTexture, fi, frameIndex](const ToneMappingPassData& data, RenderGraphContext& context) {
            if (s_ToneMapPipeline == handles::INVALID_PIPELINE) {
                // 管线 INVALID 时 ToneMapping_Output 成为从未被写入的瞬态，
                // FinalBlit 采样池化垃圾（stderr 直写——diag 通道在纯测试进程静默）。
                static bool s_abort = false;
                if (!s_abort) {
                    s_abort = true;
                    std::cerr << "[TONE-EXEC] abort: pipeline INVALID" << std::endl;
                }
                return;
            }

            auto& device = context.graph->GetDevice();
            auto* cmd = context.cmdBuffer;

            // Get output texture
            auto* outputTex = context.graph->GetResource(data.output);
            if (!outputTex || outputTex->GetType() != RGResourceType::Texture) return;
            auto* texture = static_cast<RenderGraphTexture*>(outputTex);

            // Get input HDR texture physical handle
            auto* inputRes = context.graph->GetResource(inputHDR);
            ResourceHandle inputHandle = handles::INVALID_RESOURCE;
            if (inputRes) inputHandle = inputRes->GetPhysicalHandle();

            // Get bloom texture physical handle (or use dummy)
            ResourceHandle bloomHandle = s_DummyTextureView;
            if (bloomTexture != kInvalidRGResourceHandle) {
                auto* bloomRes = context.graph->GetResource(bloomTexture);
                if (bloomRes) bloomHandle = bloomRes->GetPhysicalHandle();
            }
            if (bloomHandle == handles::INVALID_RESOURCE) bloomHandle = s_DummyTextureView;

            // Use physical handles directly — updateDescriptorSetsImpl resolves
            // via DawnTexture::GetDefaultView(), no per-frame WGPU objects created.
            ResourceHandle inputView = inputHandle;

            ResourceHandle bloomViewHandle = bloomHandle;

            ResourceHandle aoViewHandle = s_DummyAOTextureView;
            if (aoTexture != kInvalidRGResourceHandle) {
                auto* aoRes = context.graph->GetResource(aoTexture);
                if (aoRes && aoRes->GetType() == RGResourceType::Texture) {
                    aoViewHandle = aoRes->GetPhysicalHandle();
                }
            }
            if (aoViewHandle == handles::INVALID_RESOURCE) aoViewHandle = s_DummyAOTextureView;

            ResourceHandle ssgiViewHandle = s_DummySSGITextureView;
            if (ssgiTexture != kInvalidRGResourceHandle) {
                auto* ssgiRes = context.graph->GetResource(ssgiTexture);
                if (ssgiRes && ssgiRes->GetType() == RGResourceType::Texture) {
                    ssgiViewHandle = ssgiRes->GetPhysicalHandle();
                }
            }
            if (ssgiViewHandle == handles::INVALID_RESOURCE) ssgiViewHandle = s_DummySSGITextureView;

            ResourceHandle velocityViewHandle = s_DummyVelocityTextureView;
            if (velocityTexture != kInvalidRGResourceHandle) {
                auto* velRes = context.graph->GetResource(velocityTexture);
                if (velRes && velRes->GetType() == RGResourceType::Texture) {
                    velocityViewHandle = velRes->GetPhysicalHandle();
                }
            }
            if (velocityViewHandle == handles::INVALID_RESOURCE) velocityViewHandle = s_DummyVelocityTextureView;

            // P3.1: eye adaptation dispatch——写本帧适应态（下一帧作 prev）。
            // ping 语义：s_AdaptPing = 最近一帧写出的态；tonemap 绑定它。
            if (s_AdaptPipeline != handles::INVALID_PIPELINE &&
                (device.GetPlatform() == RHIPlatform::Vulkan ||
                 device.GetPlatform() == RHIPlatform::D3D12) &&
                inputView != handles::INVALID_RESOURCE &&
                s_AdaptSets[fi] != handles::INVALID_DESCRIPTOR_SET) {
                const u32 prevIdx = s_AdaptPing;
                const u32 nextIdx = (s_AdaptPing + 1u) % 2u;
                // P0-B: on the very first dispatch both 1×1 states are still
                // UNDEFINED; binding prev as SampledImage promises
                // SHADER_READ_ONLY_OPTIMAL and trips VUID-09600. Prime both
                // states once (backend resolves oldLayout from tracked layout).
                // Content garbage is already sanitized in EyeAdaptation.comp.
                static bool s_AdaptPrimed = false;
                if (!s_AdaptPrimed) {
                    ResourceBarrier prime[2]{};
                    prime[0].resource = s_AdaptState[0];
                    prime[1].resource = s_AdaptState[1];
                    for (auto& b : prime) {
                        b.beforeState = ResourceState::ShaderResource;
                        b.afterState  = ResourceState::ShaderResource;
                        b.subresource = 0xFFFFFFFF;
                        b.queueFamily = 0xFFFFFFFF;
                    }
                    cmd->InsertBarrier(prime, 2);
                    s_AdaptPrimed = true;
                }
                DescriptorImageInfo aHdr{ s_ToneMapSampler, inputView, ResourceState::ShaderResource };
                DescriptorImageInfo aPrev{ s_ToneMapSampler, s_AdaptState[prevIdx], ResourceState::ShaderResource };
                DescriptorImageInfo aNext{ s_ToneMapSampler, s_AdaptState[nextIdx], ResourceState::UnorderedAccess };
                // §4.9: 按墙钟帧间隔与时间常数 τ_up=0.3s / τ_down=1.0s 派生
                // 方向化混合率（帧率无关）；单样本亮度裁剪 40（HDR 太阳/高光
                // 离群值）；适应态钳制 [-6,6] log2L；|Δlog2L|>4 视为场景切换
                // 直接 snap。
                float rateUp = 1.0f, rateDown = 1.0f;
                {
                    auto now = std::chrono::steady_clock::now();
                    float dt = 0.016f;
                    if (s_AdaptHasTime) {
                        dt = std::chrono::duration<float>(now - s_AdaptLastTime).count();
                        dt = std::min(std::max(dt, 0.001f), 0.25f);
                    }
                    s_AdaptLastTime = now;
                    s_AdaptHasTime = true;
                    rateUp   = 1.0f - std::exp(-dt / 0.30f);
                    rateDown = 1.0f - std::exp(-dt / 1.00f);
                }
                if (s_AdaptParamsBuf[fi] != handles::INVALID_RESOURCE) {
                    const AdaptParamsGPU ap{ rateUp, rateDown,
                                             -6.0f, 6.0f,
                                             40.0f, 4.0f, {0.0f, 0.0f} };
                    device.UpdateBufferData(s_AdaptParamsBuf[fi], &ap, sizeof(ap), 0);
                }
                DescriptorBufferInfo adaptParamsInfo{};
                adaptParamsInfo.buffer = s_AdaptParamsBuf[fi];
                adaptParamsInfo.offset = 0;
                adaptParamsInfo.range = sizeof(AdaptParamsGPU);

                WriteDescriptorSet aw[4];
                aw[0].dstSet = s_AdaptSets[fi]; aw[0].dstBinding = 0; aw[0].dstArrayElement = 0;
                aw[0].descriptorCount = 1; aw[0].descriptorType = DescriptorType::SampledImage;
                aw[0].imageInfo = &aHdr;
                aw[1].dstSet = s_AdaptSets[fi]; aw[1].dstBinding = 1; aw[1].dstArrayElement = 0;
                aw[1].descriptorCount = 1; aw[1].descriptorType = DescriptorType::SampledImage;
                aw[1].imageInfo = &aPrev;
                aw[2].dstSet = s_AdaptSets[fi]; aw[2].dstBinding = 2; aw[2].dstArrayElement = 0;
                aw[2].descriptorCount = 1; aw[2].descriptorType = DescriptorType::StorageImage;
                aw[2].imageInfo = &aNext;
                aw[3].dstSet = s_AdaptSets[fi]; aw[3].dstBinding = 3; aw[3].dstArrayElement = 0;
                aw[3].descriptorCount = 1; aw[3].descriptorType = DescriptorType::UniformBuffer;
                aw[3].bufferInfo = &adaptParamsInfo;
                device.UpdateDescriptorSets(4, aw);

                // 写前 SR→GENERAL；写后 GENERAL→SR（供 tonemap 采样）
                ResourceBarrier toUA{};
                toUA.resource = s_AdaptState[nextIdx];
                toUA.beforeState = ResourceState::ShaderResource;
                toUA.afterState = ResourceState::UnorderedAccess;
                toUA.subresource = 0xFFFFFFFF;
                toUA.queueFamily = 0xFFFFFFFF;
                cmd->InsertBarrier(&toUA, 1);

                cmd->BindComputePipeline(s_AdaptPipeline);
                const DescriptorSetHandle adaptSets[] = { s_AdaptSets[fi] };
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, s_AdaptLayout,
                                        0, 1, adaptSets, 0, nullptr);
                cmd->Dispatch(1, 1, 1);

                ResourceBarrier toSR{};
                toSR.resource = s_AdaptState[nextIdx];
                toSR.beforeState = ResourceState::UnorderedAccess;
                toSR.afterState = ResourceState::ShaderResource;
                toSR.subresource = 0xFFFFFFFF;
                toSR.queueFamily = 0xFFFFFFFF;
                cmd->InsertBarrier(&toSR, 1);

                s_AdaptPing = nextIdx;

                // sun-audit P3: 自适应曝光 camera-cut 采样（PRIMAL_TONE_ADAPT_LOG=1）。
                // 在主 cmd 内把刚写出的 1×1 r32f 适应态（log2 avgLum）拷入
                // 16B 双缓冲 readback（SR→CopySource→SR，布局由同一 CB 跟踪），
                // 2 帧后再 Map（BeginFrame 的 per-frame fence 已保证完成）。
                // 切镜窗（frame 340-420，对应 SPONZA_CAM_SCRIPT 的 360 切镜）
                // 逐帧采样，其余每 30 帧。有效曝光 =
                // ubo_exposure × key / exp2(log2Lum)（ToneMapping shader 公式）。
                static const bool s_adaptLog = std::getenv("PRIMAL_TONE_ADAPT_LOG") != nullptr;
                if (s_adaptLog) {
                    static ResourceHandle s_logBuf[2] = {handles::INVALID_RESOURCE, handles::INVALID_RESOURCE};
                    static u64 s_copiedFrame[2] = {~0ull, ~0ull};
                    static u64 s_nextLog = 0;
                    const u64 fr = frameIndex;
                    if (fr >= s_nextLog) {
                        s_nextLog = (fr >= 340 && fr <= 420) ? fr + 1 : fr + 30;
                        // ① 报告 2 帧前的拷贝
                        const u32 lag = (u32)((fr + 2) % 2);
                        if (s_copiedFrame[lag] != ~0ull && s_logBuf[lag] != handles::INVALID_RESOURCE) {
                            const float* v = static_cast<const float*>(
                                device.MapBuffer(s_logBuf[lag], 0, 16));
                            if (v) {
                                const float log2L = std::max(v[0], -20.0f);
                                std::cerr << "[ToneAdapt] frame=" << s_copiedFrame[lag]
                                          << " log2Lum=" << log2L
                                          << " avgLum=" << std::exp2(log2L)
                                          << " ubo_exposure=" << s_LastUboExposure
                                          << " key=" << s_AdaptKey
                                          << " effective_exposure="
                                          << s_LastUboExposure * s_AdaptKey / std::exp2(log2L)
                                          << std::endl;
                                device.UnmapBuffer(s_logBuf[lag]);
                            }
                        }
                        // ② 记录本帧拷贝（主 cmd，布局同 CB 跟踪）
                        const u32 slot = (u32)(fr % 2);
                        if (s_logBuf[slot] == handles::INVALID_RESOURCE) {
                            BufferDesc tb{};
                            tb.size = 16;
                            tb.bindFlags = (u32)BufferUsageFlags::TransferDst;
                            tb.memoryUsage = GPUMemoryUsage::Readback;
                            s_logBuf[slot] = device.CreateBuffer(tb);
                        }
                        if (s_logBuf[slot] != handles::INVALID_RESOURCE) {
                            ResourceBarrier b1{};
                            b1.resource = s_AdaptState[s_AdaptPing];
                            b1.beforeState = ResourceState::ShaderResource;
                            b1.afterState = ResourceState::CopySource;
                            b1.subresource = 0xFFFFFFFF;
                            b1.queueFamily = 0xFFFFFFFF;
                            cmd->InsertBarrier(&b1, 1);
                            BufferTextureCopyRegion tr{};
                            tr.imageSubresource.layerCount = 1;
                            tr.imageExtent = {1, 1, 1};
                            cmd->CopyTextureToBuffer(s_AdaptState[s_AdaptPing],
                                                     s_logBuf[slot], &tr, 1);
                            ResourceBarrier b2{};
                            b2.resource = s_AdaptState[s_AdaptPing];
                            b2.beforeState = ResourceState::CopySource;
                            b2.afterState = ResourceState::ShaderResource;
                            b2.subresource = 0xFFFFFFFF;
                            b2.queueFamily = 0xFFFFFFFF;
                            cmd->InsertBarrier(&b2, 1);
                            s_copiedFrame[slot] = fr;
                        }
                    }
                }
            }

            // Use pre-allocated descriptor set from pool
            u32 idx = s_ToneMapSetIdx[fi]++;
            if (idx >= MAX_SETS) { idx = 0; s_ToneMapSetIdx[fi] = 1; }
            DescriptorSetHandle ds = s_ToneMapSets[fi][idx];

            if (ds != handles::INVALID_RESOURCE && inputView != handles::INVALID_RESOURCE) {
                DescriptorImageInfo imageInfos[7];
                imageInfos[0].imageView = inputView;
                imageInfos[0].sampler = s_ToneMapSampler;
                imageInfos[1].imageView = bloomViewHandle;
                imageInfos[1].sampler = s_ToneMapSampler;
                imageInfos[2].imageView = bloomViewHandle;
                imageInfos[2].sampler = s_ToneMapSampler;
                imageInfos[3].imageView = aoViewHandle;
                imageInfos[3].sampler = s_ToneMapSampler;
                imageInfos[4].imageView = ssgiViewHandle;
                imageInfos[4].sampler = s_ToneMapSampler;
                imageInfos[5].imageView = velocityViewHandle;
                imageInfos[5].sampler = s_ToneMapSampler;
                imageInfos[6].imageView = s_AdaptState[s_AdaptPing];  // P3.1 本帧新适应态
                imageInfos[6].sampler = s_ToneMapSampler;

                // P3.1: 8 bindings — 0..5 纹理/采样器 + 6 ToneParams UBO + 7 适应态
                WriteDescriptorSet writes[8] = {};
                writes[0].dstSet = ds; writes[0].dstBinding = 0; writes[0].dstArrayElement = 0;
                writes[0].descriptorCount = 1; writes[0].descriptorType = DescriptorType::SampledImage;
                writes[0].imageInfo = &imageInfos[0];
                writes[1].dstSet = ds; writes[1].dstBinding = 1; writes[1].dstArrayElement = 0;
                writes[1].descriptorCount = 1; writes[1].descriptorType = DescriptorType::SampledImage;
                writes[1].imageInfo = &imageInfos[1];
                writes[2].dstSet = ds; writes[2].dstBinding = 2; writes[2].dstArrayElement = 0;
                writes[2].descriptorCount = 1; writes[2].descriptorType = DescriptorType::Sampler;
                writes[2].imageInfo = &imageInfos[2];
                writes[3].dstSet = ds; writes[3].dstBinding = 3; writes[3].dstArrayElement = 0;
                writes[3].descriptorCount = 1; writes[3].descriptorType = DescriptorType::SampledImage;
                writes[3].imageInfo = &imageInfos[3];
                writes[4].dstSet = ds; writes[4].dstBinding = 4; writes[4].dstArrayElement = 0;
                writes[4].descriptorCount = 1; writes[4].descriptorType = DescriptorType::SampledImage;
                writes[4].imageInfo = &imageInfos[4];
                writes[5].dstSet = ds; writes[5].dstBinding = 5; writes[5].dstArrayElement = 0;
                writes[5].descriptorCount = 1; writes[5].descriptorType = DescriptorType::SampledImage;
                writes[5].imageInfo = &imageInfos[5];
                writes[6].dstSet = ds; writes[6].dstBinding = 7; writes[6].dstArrayElement = 0;
                writes[6].descriptorCount = 1; writes[6].descriptorType = DescriptorType::SampledImage;
                writes[6].imageInfo = &imageInfos[6];

                // P0.3: 曝光上传——基值 × 2^EV，EV0 逐位等于旧编译期常数。
                const float exposure = ToneBaseExposure(s_UpstreamIsHDR, s_ForceACES, s_ForcePassthrough)
                    * std::pow(2.0f, ToneExposureEV());
                s_LastUboExposure = exposure;   // sun-audit P3: [ToneAdapt] 采样用
                const ToneParamsUBO toneParams{ exposure,
                                                s_AdaptEnabled ? 1.0f : 0.0f,
                                                s_AdaptKey,
                                                s_GradeContrast,
                                                s_GradeSat,
                                                s_GradeVignette,
                                                s_BloomIntensity,
                                                0u };
                device.UpdateBufferData(s_ToneParamsBuf[fi], &toneParams, sizeof(toneParams), 0);

                DescriptorBufferInfo toneParamsInfo{};
                toneParamsInfo.buffer = s_ToneParamsBuf[fi];
                toneParamsInfo.offset = 0;
                toneParamsInfo.range = sizeof(ToneParamsUBO);
                writes[7].dstSet = ds; writes[7].dstBinding = 6; writes[7].dstArrayElement = 0;
                writes[7].descriptorCount = 1; writes[7].descriptorType = DescriptorType::UniformBuffer;
                writes[7].bufferInfo = &toneParamsInfo;

                device.UpdateDescriptorSets(8, writes);
            }

            // Begin render pass
            RenderPassDesc passDesc;
            passDesc.colorAttachments.resize(1);
            passDesc.colorAttachments[0].texture = texture->GetPhysicalHandle();
            passDesc.colorAttachments[0].loadOp = LoadAction::Clear;
            passDesc.colorAttachments[0].storeOp = StoreAction::Store;
            // loadOp=Clear 必须带 clearValue——未初始化的 VkClearValue 在
            // Vulkan 动态渲染/AttachmentTracker 上是附件畸形（曾致 validation
            // 崩溃），Metal 上则是未定义的清屏值。
            passDesc.colorAttachments[0].clearValue = ClearValue{ math::v4{ 0.0f, 0.0f, 0.0f, 1.0f } };

            ViewportDesc viewport;
            viewport.size.x = texture->GetDesc().size.x;
            viewport.size.y = texture->GetDesc().size.y;
            viewport.minDepth = 0.0f;
            viewport.maxDepth = 1.0f;

            cmd->BeginRenderPass(passDesc);
            cmd->SetViewport(viewport);
            cmd->SetScissor({ { 0, 0 }, { (u32)viewport.size.x, (u32)viewport.size.y } });
            // 管线选择（2026-09-11 B10.5）：按上游单位制——
            //   Metal（上游 LDR）→ 透传（仅 bloom 合成，legacy 基准观感）；
            //   Vulkan/Dawn（上游线性 HDR）→ ACES+gamma（WGSL tonemap_fs 内
            //   含 HDR_ACES_EXPOSURE 曝光标定）。
            // PRIMAL_TONE_ACES=1 强制 ACES；PRIMAL_TONE_PASSTHROUGH=1 强制透传
            // （Vulkan A/B 旧行为用）。
            const bool usePassthrough = !s_ForceACES && !s_UpstreamIsHDR && !s_ForcePassthrough &&
                s_ToneMapPassthroughPipeline != handles::INVALID_PIPELINE;
            cmd->BindGraphicsPipeline(usePassthrough ? s_ToneMapPassthroughPipeline
                                                     : s_ToneMapPipeline);

            if (ds != handles::INVALID_RESOURCE) {
                cmd->BindDescriptorSets(PipelineBindPoint::Graphics, s_ToneMapLayout, 0, 1, &ds, 0, nullptr);
            }

            cmd->Draw(3, 0, 1, 0);
            cmd->EndRenderPass();
        }
    );
}

} // namespace primal::graphics::PostProcess
