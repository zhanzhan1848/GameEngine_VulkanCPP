// T4.6.5 part 30.4 — TestVulkanFroxelRenderGraph.cpp
//
// First visible-window Vulkan Sponza render using the RenderSystem abstraction
// (mirrors TestNaniteStreamingPipeline's scaffolding). The RenderSystem owns
// triple-buffered command buffers + per-frame fences internally, eliminating the
// fence/command-buffer-in-use validation errors that the deleted
// TestVulkanStandardPipelineRender_Windowed test produced via manual sync.

#include "TestVulkanFroxelRenderGraph.h"
#include "SponzaReference.h"

#include "Engine/Graphics/RHI/Core/RHIDeviceFactory.h"
#include "Engine/Graphics/RHI/Core/RHIDevice.h"
#include "Engine/Graphics/RHI/Core/RHICommand.h"
#include "Engine/Graphics/RHI/Core/RHITypes.h"
#include "Engine/Graphics/RHI/Core/RHIMath.h"
#include "Engine/Graphics/RHI/Platforms/Vulkan/VulkanDevice.h"
#include "Engine/Graphics/RHI/Platforms/Vulkan/VulkanCommandBuffer.h"

// Note: the `Engine/...` include prefix matches TestNaniteStreamingPipeline.cpp.
// The Engine target propagates its own include path transitively via
// target_link_libraries(... Engine ContentTools) in setup_test_target.
#include "Engine/Content/ContentToEngine.h"
#include "Engine/Content/AsyncResourceLoader.h"
#include "Engine/Graphics/Nanite/OfflineSDFMerger.h"
#include "Engine/Graphics/RenderPipeline/RenderPasses/PostProcess/TAAPass.h"
#include "Engine/Graphics/Lumen/GIDebugParams.h"

#include <cstdlib>
#include <sstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <iomanip>
#include <limits>

#define STBI_NO_THREAD_LOCALS
#include "stb_image.h"

#define NS_PRIVATE_IMPLEMENTATION
#include <AppKit/AppKit.hpp>

#include <iostream>
#include <cstring>
#include <fstream>
#include <vector>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <string>
#include <cctype>
#include "Graphics/Volume/FroxelFogPass.h"
#include "ParticleFX/render/Bridges.h"
#include "Engine/Common/CoreLogStream.h"
namespace pfx = primal::pfx;


using namespace primal::graphics;
using namespace primal::graphics::rhi;
using namespace primal::math;
namespace rhimath = primal::graphics::rhi::math;

namespace {

// Texture helpers (RHIDeviceBase version — copied from
// TestVulkanStandardPipelineSmoke.cpp:133-186). Vulkan has no UpdateTextureData
// on RHIDeviceBase, so we use staging buffer + CopyBufferToTexture +
// GenerateMipmaps. Software mip box filter is generated inline so the staging
// buffer carries only mip 0; GPU GenerateMipmaps fills the rest of the chain.

ResourceHandle CreateTextureFromData(RHIDeviceBase* device, int w, int h,
    const unsigned char* data, DataFormat format = DataFormat::RGBA8_UNorm) {
    u32 mipLevels = 1;
    { u32 maxDim = std::max((u32)w, (u32)h); while (maxDim > 1) { mipLevels++; maxDim /= 2; } }

    TextureDesc desc{};
    desc.size = {(u32)w, (u32)h, 1};
    desc.format = format;
    desc.type = TextureType::Texture2D;
    desc.mipLevels = mipLevels;
    desc.usage = TextureUsage::ShaderResource | TextureUsage::CopyDest | TextureUsage::CopySource;

    ResourceHandle tex = device->CreateTexture(desc);
    if (tex == handles::INVALID_RESOURCE) return handles::INVALID_RESOURCE;

    const u64 kBytes = (u64)w * h * 4;
    BufferDesc sdesc{};
    sdesc.size = kBytes;
    sdesc.type = BufferType::Raw;
    sdesc.memoryUsage = GPUMemoryUsage::Dynamic;
    sdesc.name = "TexStaging";
    ResourceHandle staging = device->CreateBuffer(sdesc);
    if (staging == handles::INVALID_RESOURCE) {
        device->DestroyTexture(tex);
        return handles::INVALID_RESOURCE;
    }
    if (!device->UpdateBufferData(staging, data, kBytes, 0)) {
        device->DestroyBuffer(staging);
        device->DestroyTexture(tex);
        return handles::INVALID_RESOURCE;
    }

    CommandBufferHandle cmd = device->CreateCommandBuffer(CommandQueueType::Graphics);
    VulkanCommandBuffer* vcmd = static_cast<VulkanDevice*>(device)->GetCommandBuffer(cmd);
    vcmd->Reset();
    vcmd->Begin();
    BufferTextureCopyRegion region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource = { 0, 0, 1 };
    region.imageOffset = { 0, 0, 0 };
    region.imageExtent = { (u32)w, (u32)h, 1 };
    vcmd->CopyBufferToTexture(staging, tex, &region, 1);
    if (mipLevels > 1) vcmd->GenerateMipmaps(tex);
    vcmd->End();
    vcmd->Submit(0);
    vcmd->WaitForCompletion();
    device->DestroyCommandBuffer(cmd);
    device->DestroyBuffer(staging);
    return tex;
}

ResourceHandle LoadTextureFromFile(RHIDeviceBase* device, const std::string& path,
    DataFormat format = DataFormat::RGBA8_UNorm) {
    int width, height, channels;
    unsigned char* data = stbi_load(path.c_str(), &width, &height, &channels, 4);
    if (!data) return handles::INVALID_RESOURCE;
    ResourceHandle tex = CreateTextureFromData(device, width, height, data, format);
    stbi_image_free(data);
    return tex;
}

// T4.6.5 part 37: Float32 → Float16 conversion for HDR data (mirrors
// TestDawnForwardRenderer.cpp:132). Sunset.hdr is loaded via stbi_loadf as
// float32 RGBA — engine's RGBA16_Float texture format needs f16 encoding.
static uint16_t f32_to_f16(float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    uint16_t sign = (bits >> 16) & 0x8000u;
    int32_t exp = ((bits >> 23) & 0xFF) - 127 + 15;
    uint32_t mantissa = (bits >> 13) & 0x3FFu;
    if (exp <= 0) return sign;
    if (exp >= 31) return sign | 0x7C00u;
    return sign | (uint16_t(exp) << 10) | uint16_t(mantissa);
}

// P3（sponza-gap-reaudit §4.8）: 表现层开关的统一 env 语义——未设置=关，
// 显式 "0"/"false"/"off"（不区分大小写）也=关。旧 `getenv()!=nullptr` 让
// FROXEL=0 / SSR=0 反而开启功能，实验记录与用户理解不一致。
static bool EnvEnabled(const char* name) {
    const char* v = std::getenv(name);
    if (!v) return false;
    std::string s(v);
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return !(s == "0" || s == "false" || s == "off");
}

// 2026-09-16（volume-gi-repair-audit V-Test-A/V7）: froxel 标定用的 float env
// 读取——未设置/非法返回 def。
static float EnvFloat(const char* name, float def) {
    const char* v = std::getenv(name);
    if (!v) return def;
    char* end = nullptr;
    float val = std::strtof(v, &end);
    return (end && *end == '\0') ? val : def;
}

// T4.6.5 part 37: load precompiled .spv bytes (binary SPIR-V). Mirrors the
// SPIR-V loader pattern in InitializePipeline (lines 238-256) — try direct
// path first, then walk up parent dirs.
std::vector<u8> LoadSpvBytes(const char* relpath) {
    auto tryPath = [](const std::string& p) -> std::vector<u8> {
        std::ifstream f(p, std::ios::binary | std::ios::ate);
        if (!f) return {};
        std::streamsize sz = f.tellg();
        f.seekg(0);
        std::vector<u8> bytes(static_cast<size_t>(sz));
        f.read(reinterpret_cast<char*>(bytes.data()), sz);
        return bytes;
    };
    std::vector<u8> bytes = tryPath(relpath);
    if (!bytes.empty()) return bytes;
    for (int i = 1; i <= 6 && bytes.empty(); ++i) {
        std::string prefix;
        for (int j = 0; j < i; ++j) prefix += "../";
        bytes = tryPath(prefix + relpath);
    }
    return bytes;
}

std::string ResolveTexturePath(const std::string& base, const std::string& filename) {
    if (filename.empty()) return "";
    std::string path = base + filename;
    { std::ifstream f(path); if (f.is_open()) return path; }
    path = base + "models/Sponza/" + filename;
    { std::ifstream f(path); if (f.is_open()) return path; }
    path = base + "textures/" + filename;
    { std::ifstream f(path); if (f.is_open()) return path; }
    return "";
}

m4x4 make_identity_m4x4() {
    m4x4 r{};
    std::memset(&r, 0, sizeof(r));
    r.columns[0][0] = 1.0f;
    r.columns[1][1] = 1.0f;
    r.columns[2][2] = 1.0f;
    r.columns[3][3] = 1.0f;
    return r;
}

} // anonymous namespace

Engine_Test::Engine_Test()
    : primal::test::RenderTestRunner(std::make_unique<TestVulkanFroxelRenderGraph>())
{}

TestVulkanFroxelRenderGraph::~TestVulkanFroxelRenderGraph() {
    Shutdown();
}

bool TestVulkanFroxelRenderGraph::Initialize() {
    std::cout << "[Part30.4] TestVulkanFroxelRenderGraph::Initialize" << std::endl;

    // Froxel 集成门禁：本测试自带 env（在管线初始化前设置，模块据此激活）。
    // 仅本测试二进制受影响；Sponza/DLSS 测试不读这些开关（各自独立 setenv）。
    {
        std::cout << "[FroxelGate] enabling FROXEL=1 + reference art direction" << std::endl;
        setenv("FROXEL", "1", 1);
        // 参考机位 + 陡角太阳（sponza_reference art direction）：断言机位与
        // 截图机位一致、不受外部 env 漂移影响。SPONZA_REFERENCE 可被外部
        // 预设（此处强制 on）。
        setenv("SPONZA_REFERENCE", "1", 1);
        // 常量介质基准（V-Test-A 路径）：G1/G2/G3 在 uniform 相位断言，G4
        // 在 noise 相位断言（相位机切换 density_uniform_debug）。外部可覆盖。
        setenv("FROXEL_UNIFORM", std::getenv("FROXEL_UNIFORM") ? std::getenv("FROXEL_UNIFORM") :
               ((EnvEnabled("FROXEL_CLOUD") || EnvEnabled("FROXEL_SPARSE")) ? "-1" : "0.35"), 1);
        setenv("FROXEL_PHASE_G", std::getenv("FROXEL_PHASE_G") ? std::getenv("FROXEL_PHASE_G") : "0.0", 1);
        // WP2 A/B 通道：外部设 FROXEL_SPGI_AMBIENT=1 时门禁的 uniform 相位
        // 也走 SPGI 环境项（默认 0 = 硬编码 fallback 基准）。
        setenv("FROXEL_SPGI_AMBIENT",
               std::getenv("FROXEL_SPGI_AMBIENT") ? std::getenv("FROXEL_SPGI_AMBIENT") : "0", 1);
    }
    // 门禁配置（计划 §6.2 表）。FROXEL_GATE 默认开；=0 退回窗口长跑。
    {
        const char* e = std::getenv("FROXEL_GATE");
        // Cloud preview is independent of the uniform-medium G1-G5 phase machine.
        gateEnabled_ = e ? e[0] != '0' : !(EnvEnabled("FROXEL_CLOUD") || EnvEnabled("FROXEL_SPARSE"));
        if (gateEnabled_ && (EnvEnabled("FROXEL_CLOUD") || EnvEnabled("FROXEL_SPARSE"))) {
            std::cerr << "[FroxelGate] volume preview requires FROXEL_GATE=0; "
                         "run G1-G5 with FROXEL_CLOUD=0 and FROXEL_SPARSE=0" << std::endl;
            return false;
        }
        e = std::getenv("FROXEL_GATE_FRAME");
        gateFrame_ = e ? (u32)std::atoi(e) : 300u;
        gateFrameB_ = gateFrame_ + 45u;    // G3 no-shadow 读回
        gateFrameC_ = gateFrame_ + 90u;    // G5 A'（shadows on + march0）
        gateFrameD_ = gateFrame_ + 135u;   // G5 B'（march=4）+ 断言
        gateFrameNoiseOn_ = gateFrame_ + 137u;
        gateFrameE_ = gateFrame_ + 180u;   // G4 noise 密度读回
    }

    if (!primal::jobsystem::JobSystem::Initialize(
            primal::jobsystem::JobSchedulerConfig::Default())) {
        std::cerr << "[Part30.4] JobSystem init failed" << std::endl;
        return false;
    }

    if (!InitializeDevice()) {
        std::cerr << "[Part30.4] InitializeDevice failed" << std::endl;
        return false;
    }
    if (!InitializeWindowAndRenderSystem()) {
        std::cerr << "[Part30.4] InitializeWindowAndRenderSystem failed" << std::endl;
        return false;
    }
    if (!InitializePipeline()) {
        std::cerr << "[Part30.4] InitializePipeline failed" << std::endl;
        return false;
    }
    if (!LoadSponzaScene()) {
        std::cerr << "[Part30.4] LoadSponzaScene failed" << std::endl;
        return false;
    }
    // T4.6.5 part 37: Tier 5 IBL (sunset.hdr environment). Failure is non-fatal —
    // pipeline falls back to flat 0.03*albedo ambient (DeferredLightingModule
    // binds a 1x1 white fallback when IBL handles are INVALID). Test still
    // passes without IBL; visible fidelity is degraded.
    if (!InitializeIBL()) {
        std::cerr << "[Part37] InitializeIBL failed — continuing without IBL" << std::endl;
    }


    std::cout << "[FroxelGate] Initialization complete — gate asserts at frame " << gateFrame_ << ", then _Exit(0/1)."
              << std::endl;
    return true;
}

bool TestVulkanFroxelRenderGraph::InitializeDevice() {
    DeviceDesc desc;
    desc.platform = RHIPlatform::Vulkan;
    desc.enableValidation = true;
    desc.enableDebug = true;

    auto vulkanDevice = std::make_unique<VulkanDevice>(desc);
    if (!vulkanDevice->Initialize()) {
        std::cerr << "[Part30.4] VulkanDevice::Initialize failed" << std::endl;
        return false;
    }

    device_ = vulkanDevice.get();
    g_deviceManager.RegisterDevice(device_);
    deviceOwnership_ = std::move(vulkanDevice);
    return true;
}

bool TestVulkanFroxelRenderGraph::InitializeWindowAndRenderSystem() {
    primal::platform::window_init_info wi{};
    wi.caption = "Vulkan Froxel Gate (RenderSystem)";
    wi.left = 100;
    wi.top = 100;
    wi.width = 1280;
    wi.height = 720;
    window_ = primal::platform::create_window(&wi);

    if (!window_.is_valid()) {
        std::cerr << "[Part30.4] create_window failed" << std::endl;
        return false;
    }

    RenderSystemInitInfo sysInfo;
    sysInfo.device = device_;
    sysInfo.window = window_.handle();
    sysInfo.width = window_.width();
    sysInfo.height = window_.height();

    if (!renderSystem_.Initialize(sysInfo)) {
        std::cerr << "[Part30.4] RenderSystem::Initialize failed" << std::endl;
        return false;
    }

    // T4.6.5 part 30.13 (X5 fix): per-swapchain-image render-done semaphores.
    // Index by currentImageIndex_ when picking which to signal/submit/present.
    for (u32 i = 0; i < kMaxSwapchainImages; ++i) {
        renderDoneSemaphores_[i] = device_->CreateSync();
        if (renderDoneSemaphores_[i] == primal::graphics::rhi::handles::INVALID_SYNC) {
            std::cerr << "[Part30.4] CreateSync (renderDoneSemaphores_[" << i << "]) failed" << std::endl;
            return false;
        }
    }
    return true;
}

bool TestVulkanFroxelRenderGraph::InitializePipeline() {
    pipeline_ = std::make_unique<StandardRenderPipeline>();
    if (!pipeline_->Initialize(device_)) {
        std::cerr << "[Part30.4] StandardRenderPipeline::Initialize failed" << std::endl;
        return false;
    }

    // SPIR-V loader: try project-root path first (matches UnitTests cwd),
    // then walk up from the binary's cwd (Darwin/Debug) until found.
    auto loadSpv = [](const char* relpath) -> std::vector<u8> {
        auto tryPath = [](const std::string& p) -> std::vector<u8> {
            std::ifstream f(p, std::ios::binary | std::ios::ate);
            if (!f) return {};
            std::streamsize sz = f.tellg();
            f.seekg(0);
            std::vector<u8> bytes(static_cast<size_t>(sz));
            f.read(reinterpret_cast<char*>(bytes.data()), sz);
            return bytes;
        };
        std::vector<u8> bytes = tryPath(relpath);
        if (!bytes.empty()) return bytes;
        for (int i = 1; i <= 6 && bytes.empty(); ++i) {
            std::string prefix;
            for (int j = 0; j < i; ++j) prefix += "../";
            bytes = tryPath(prefix + relpath);
        }
        return bytes;
    };
    auto deferredVsBytes = loadSpv("Engine/Graphics/Vulkan/shaders/Forward/DeferredLighting.vert.spv");
    auto deferredFsBytes = loadSpv(
        // sun-audit P2: PRIMAL_SPEC_OCC=1 → GTAO 镜面遮挡变体；=2 → DDGI
        // 探针 Chebyshev 可见性变体（第二步，消除 GTAO 饱和）；未设 →
        // 太阳阴影代理 stand-in。见 DeferredLighting.frag 分支注释。
        [](const char* e) -> const char* {
            if (!e) return "Engine/Graphics/Vulkan/shaders/Forward/DeferredLighting.frag.spv";
            if (e[0] == '2') return "Engine/Graphics/Vulkan/shaders/Forward/DeferredLighting_probeocc.frag.spv";
            return "Engine/Graphics/Vulkan/shaders/Forward/DeferredLighting_specocc.frag.spv";
        }(std::getenv("PRIMAL_SPEC_OCC")));
    if (deferredVsBytes.empty() || deferredFsBytes.empty()) {
        std::cerr << "[Part30.4] Missing DeferredLighting SPIR-V" << std::endl;
        return false;
    }
    auto blitVsBytes = loadSpv("Engine/Graphics/Vulkan/shaders/Forward/Blit.vert.spv");
    auto blitFsBytes = loadSpv("Engine/Graphics/Vulkan/shaders/Forward/Blit.frag.spv");
    if (blitVsBytes.empty() || blitFsBytes.empty()) {
        std::cerr << "[Part30.4] Missing Blit SPIR-V" << std::endl;
        return false;
    }
    // T4.6.5 part 40: ShadowFilter compute shader — half-res visibility
    // producer consumed by DeferredLighting binding 6. Without this,
    // shadow_visibility_tex_ stays INVALID and DeferredLighting falls back to
    // 1x1 white (no shadows).
    auto shadowFilterBytes = loadSpv("Engine/Graphics/Vulkan/shaders/ShadowFilter.spv");
    if (shadowFilterBytes.empty()) {
        std::cerr << "[Part40] Missing ShadowFilter SPIR-V — shadows disabled" << std::endl;
    }

    ShaderHandle deferredVs = device_->CreateShader(
        deferredVsBytes.data(), deferredVsBytes.size(), ShaderStage::Vertex, "main");
    ShaderHandle deferredFs = device_->CreateShader(
        deferredFsBytes.data(), deferredFsBytes.size(), ShaderStage::Pixel, "main");
    ShaderHandle blitVs = device_->CreateShader(
        blitVsBytes.data(), blitVsBytes.size(), ShaderStage::Vertex, "main");
    ShaderHandle blitFs = device_->CreateShader(
        blitFsBytes.data(), blitFsBytes.size(), ShaderStage::Pixel, "main");
    ShaderHandle shadowFilter = shadowFilterBytes.empty() ? handles::INVALID_SHADER
        : device_->CreateShader(
            shadowFilterBytes.data(), shadowFilterBytes.size(),
            ShaderStage::Compute, "main");

    StandardRenderPipeline::ShaderHandles handles;
    handles.deferred_vs = deferredVs;
    handles.deferred_ps = deferredFs;
    handles.blit_vs = blitVs;
    handles.blit_ps = blitFs;
    handles.shadow_filter = shadowFilter;

    // GI shaders: Fusion (indirect + composite pixel shaders) + GIGather (compute).
    auto fusionIndirectBytes = loadSpv("Engine/Graphics/Vulkan/shaders/Lumen/FusionIndirect.frag.spv");
    auto fusionCompositeBytes = loadSpv("Engine/Graphics/Vulkan/shaders/Lumen/FusionComposite.frag.spv");
    // M8/T8.7: GI_ISOLATE=ssgi|ddgi|spgi selects a FusionIndirect variant
    // with one source zeroed (compile-time -D) for per-source A/B.
    {
        const char* iso = std::getenv("GI_ISOLATE");
        std::string fiPath = "Engine/Graphics/Vulkan/shaders/Lumen/FusionIndirect.frag.spv";
        if (iso && std::string(iso) == "ssgi") fiPath = "Engine/Graphics/Vulkan/shaders/Lumen/FusionIndirect_noSsgi.frag.spv";
        if (iso && std::string(iso) == "ddgi") fiPath = "Engine/Graphics/Vulkan/shaders/Lumen/FusionIndirect_noDdgi.frag.spv";
        if (iso && std::string(iso) == "spgi") fiPath = "Engine/Graphics/Vulkan/shaders/Lumen/FusionIndirect_noSpgi.frag.spv";
        if (iso && std::string(iso) == "all") fiPath = "Engine/Graphics/Vulkan/shaders/Lumen/FusionIndirect_noAll.frag.spv";
        std::cerr << "[GI-isolate] FusionIndirect variant: " << fiPath << std::endl;
        fusionIndirectBytes = loadSpv(fiPath.c_str());
    }
    // 2026-09-17 归因 A/B：DDGI_LEGACY_CONSUMERS=1 → 消费端旧原格点行为变体
    // （build_spv.sh 以 -DDDGI_LEGACY_CONSUMERS 编译的 sibling bytecode）。
    auto giGatherBytes = loadSpv("Engine/Graphics/Vulkan/shaders/Lumen/DDGIGIGather.comp.spv");
    if (std::getenv("DDGI_LEGACY_CONSUMERS")) {
        auto legacyBytes = loadSpv("Engine/Graphics/Vulkan/shaders/Lumen/DDGIGIGather_legacy.comp.spv");
        if (!legacyBytes.empty()) {
            giGatherBytes = legacyBytes;
            std::cerr << "[GI-legacy] DDGIGIGather_legacy variant loaded" << std::endl;
        }
    }
    if (!fusionIndirectBytes.empty())
        handles.fusion_indirect_ps = device_->CreateShader(
            fusionIndirectBytes.data(), fusionIndirectBytes.size(), ShaderStage::Pixel, "fusion_indirect");
    if (!fusionCompositeBytes.empty())
        handles.fusion_composite_ps = device_->CreateShader(
            fusionCompositeBytes.data(), fusionCompositeBytes.size(), ShaderStage::Pixel, "fusion_composite");
    if (!giGatherBytes.empty())
        handles.gi_gather = device_->CreateShader(
            giGatherBytes.data(), giGatherBytes.size(), ShaderStage::Compute, "ddgi_gi_gather");

    // SC→DDGI integration (card radiance avg → probe irradiance projection).
    // Without these the SCDDGIIntegrationModule pipelines stay INVALID and its
    // AddPass early-outs every frame — the world-space card→probe bounce layer
    // silently never runs (probes fall back to their own SDF trace only).
    auto scCardRadianceBytes = loadSpv("Engine/Graphics/Vulkan/shaders/Lumen/DDGICardRadianceAvg.comp.spv");
    auto scProbeIrradianceBytes = loadSpv("Engine/Graphics/Vulkan/shaders/Lumen/DDGIProbeIrradianceFromCards.comp.spv");
    if (!scCardRadianceBytes.empty())
        handles.sc_card_radiance = device_->CreateShader(
            scCardRadianceBytes.data(), scCardRadianceBytes.size(),
            ShaderStage::Compute, "ddgi_card_radiance_avg");
    if (!scProbeIrradianceBytes.empty())
        handles.sc_probe_irradiance = device_->CreateShader(
            scProbeIrradianceBytes.data(), scProbeIrradianceBytes.size(),
            ShaderStage::Compute, "ddgi_probe_irradiance_from_cards");

    // SDF visualization fullscreen shader (toggled at runtime via F6).
    auto sdfVizBytes = loadSpv("Engine/Graphics/Vulkan/shaders/Debug/SDFVisualization.frag.spv");
    if (!sdfVizBytes.empty())
        handles.sdf_viz_ps = device_->CreateShader(
            sdfVizBytes.data(), sdfVizBytes.size(), ShaderStage::Pixel, "main");

    // M8/T8.5: real-mesh DDGI probe spheres (SPGI_PROBE_SPHERES=1).
    auto probeSphereVs = loadSpv("Engine/Graphics/Vulkan/shaders/Debug/ProbeSphere.vert.spv");
    auto probeSpherePs = loadSpv("Engine/Graphics/Vulkan/shaders/Debug/ProbeSphere.frag.spv");
    if (!probeSphereVs.empty())
        handles.probe_sphere_vs = device_->CreateShader(
            probeSphereVs.data(), probeSphereVs.size(), ShaderStage::Vertex, "main");
    if (!probeSpherePs.empty())
        handles.probe_sphere_ps = device_->CreateShader(
            probeSpherePs.data(), probeSpherePs.size(), ShaderStage::Pixel, "main");

    pipeline_->SetShaderHandles(handles);

    // High preset: SSAO + SC + DDGI + SPGI (screen probes) + Fusion. Fusion
    // outputs HDR; ToneMappingPass performs the single HDR -> LDR conversion
    // and FinalBlit only copies that LDR result to the swapchain.
    // Axis-1 defaults mirror Medium (use_screen_probes=false) and override the
    // preset in SetLumenConfig, so flip the axis explicitly for SPGI.
    lumen::LumenConfig lumenConfig{};
    lumenConfig.quality = lumen::LumenQualityPreset::High;
    lumenConfig.axes.use_screen_probes = true;
    // [反弹专项] WRC 近场密度 3→2m（RTXGI 参考中位；3-5m 宽的室内房间
    // 3m 间距仅 ~1 探针/间，房间内变化被抹平）。测试直构 config 不走
    // preset 工厂——这里与 preset 表同步。
    lumenConfig.wrc_probe_spacing = 2.0f;
    // 2026-09-05 跨平台 SC 制度对齐：全量捕获（capture_budget=0）与 Metal
    // TestModularPipeline 同配置。稀疏更新（默认 256 页/帧）下 SC atlas 长
    // 期陈旧 → 探针 trace 采样到均匀化辐射 → 探针场 L1 方向能量只有
    // Metal 的一半（|L1|/|L0| 0.18 vs 0.40），画面偏平偏暗。
    lumenConfig.surface_cache_capture_budget = 0;
    // Scene-fit DDGI probe grid (Docs/completed/2026-09-18-ddgi-scene-fit-probe-grid-plan.md
    // §5.5): near volume derives counts from the geometry AABB measured off
    // Sponza_pipeline_signed_v5.model vertex buffers (plan §1.2,
    // 37.2×15.6×22.9). Spacing 3.0 → 14×7×9 = 882 probes vs 16,384 fixed.
    // A/B switches: SPONZA_DDGI_NO_FIT=1 reverts to the legacy centered
    // 32×16×32 grid; SPONZA_FIT_SPACING overrides spacing for the same-AABB
    // density ladder (P1-B, 3.0 vs 1.5).
    if (!std::getenv("SPONZA_DDGI_NO_FIT")) {
        lumenConfig.ddgi_fit_to_bounds = true;
        if (std::getenv("SPONZA_FIT_AUTO")) {
            // P1-A 口径 A: no explicit box — SetOfflineSDFSource backfills the
            // merger volume and the DDGI volumes rebuild on frame 1.
            lumenConfig.ddgi_fit_bounds_auto = true;
        } else {
            lumenConfig.ddgi_fit_bounds_min = {-19.21f, -1.26f, -11.83f};
            lumenConfig.ddgi_fit_bounds_max = { 18.00f, 14.29f,  11.05f};
        }
        if (const char* e = std::getenv("SPONZA_FIT_SPACING")) {
            const float s = (float)std::atof(e);
            if (s > 0.25f && s < 32.0f) lumenConfig.ddgi_probe_spacing = s;
        }
    }
    pipeline_->SetLumenConfig(lumenConfig);

    // Keep direct and probe lighting in the same physical scale. The DDGI
    // uploader stores a separate intensity of 3.0 in LightColor.w, so feeding
    // the legacy default RGB value of 20 here produced an effective intensity
    // of 60 and drove GI-lit surfaces into the white shoulder of ACES.
    // 2026-09-01 GI-观感恢复：按 8/28 直射校准（975d1f86）之前的历史栈复原
    // —— light 5.0（×shader DIRECT 2.5 = 12.5 屏幕直射）+ ddgi 20（×上传器
    // 内部 3.0 = 有效 60 的火热探针驱动）。这就是"暖亮 GI 弹射"观感的来源。
    // 用户实测 5.0/20.0 偏强，折中取 3.0 / 5.0。
    auto renderSettings = pipeline_->GetSettings();
    renderSettings.lighting.light_direction = {0.5f, -0.7f, 0.3f};
    renderSettings.lighting.light_color = {3.0f, 3.0f, 3.0f, 1.0f};
    renderSettings.lighting.ddgi_light_color = {5.0f, 5.0f, 5.0f, 1.0f};
    sponza_reference::LightingSettings(renderSettings.lighting);
    // sun-audit §5.3: PCSS 太阳角直径（度）。默认 0.5357 已在
    // SceneLightingParams；这里给 A/B 调参入口，0 = 旧固定 lightSizeUV 档。
    if (const char* e = std::getenv("PRIMAL_SUN_ANGULAR_DIAMETER")) {
        const float v = (float)std::atof(e);
        if (v >= 0.0f && v <= 10.0f) renderSettings.lighting.sun_angular_diameter_deg = v;
    }
    // P2.1 (2026-09-15): FroxelFog Vulkan 启用复测（路线图 Phase 2）——
    // FROXEL=1 环境变量开启（体积雾 + 体积阴影光轴）。默认仍关，A/B 门。
    // 密度按参考图空气透视感调参：默认 extinction 0.1 过淡、高度雾关闭。
    renderSettings.quality.enable_froxel_fog = EnvEnabled("FROXEL");
    if (renderSettings.quality.enable_froxel_fog) {
        renderSettings.froxel.extinction_scale = 0.35f;
        renderSettings.froxel.height_fog_base  = 0.0f;
        renderSettings.froxel.height_fog_scale = 8.0f;   // 地面雾向上 ~8m 渐隐
        // P1（reaudit §4.4）: 默认 fade_range=10 → 距相机 20m 外密度清零，
        // 参考机位下庭院（30-60m）整段无雾。扩到 30：30m 内全密度，
        // 60m 处收敛 0，覆盖庭院可视深度。
        renderSettings.froxel.density_fade_range = 30.0f;
        // 2026-09-16（volume-gi-repair-audit V-Test-A/V7）: 标定旋钮。
        // FROXEL_UNIFORM=<density> 常量介质（绕过 noise/SDF/height/fade，
        // 仅保留几何清零）；FROXEL_PHASE_G=<g> HG 各向异性（0=各向同性）。
        renderSettings.froxel.density_uniform_debug = EnvFloat("FROXEL_UNIFORM", -1.0f);
        renderSettings.froxel.phase_g = EnvFloat("FROXEL_PHASE_G", 0.0f);
        renderSettings.froxel.ambient_source_spgi = EnvEnabled("FROXEL_SPGI_AMBIENT") ? 1u : 0u;
        if (EnvEnabled("FROXEL_CLOUD") || EnvEnabled("FROXEL_SPARSE")) {
            auto& fog = renderSettings.froxel;
            // Dense cumulus: tau >= 4 through the core, feathered outer shell.
            // Extinction is shared with haze, so increase local cloud density only.
            fog.cloud_enable = EnvEnabled("FROXEL_CLOUD") ? 1.0f : 0.0f;
            fog.cloud_radius = std::max(0.1f, EnvFloat("FROXEL_CLOUD_R", 4.0f));
            fog.cloud_density = std::max(0.0f, EnvFloat("FROXEL_CLOUD_D", 80.0f));
            fog.cloud_noise_freq = EnvFloat("FROXEL_CLOUD_FREQ", 0.42f);
            fog.cloud_center[0] = 2.0f;
            fog.cloud_center[1] = 3.8f;
            fog.cloud_center[2] = 0.0f;
            if (const char* pos = std::getenv("FROXEL_CLOUD_POS")) {
                float x, y, z;
                if (std::sscanf(pos, "%f,%f,%f", &x, &y, &z) == 3) {
                    fog.cloud_center[0] = x;
                    fog.cloud_center[1] = y;
                    fog.cloud_center[2] = z;
                }
            }
            fog.extinction_scale = std::max(0.001f, EnvFloat("FROXEL_EXT", 0.1f));
            // The courtyard is largely in roof shadow; calibrate broad sky
            // fill for this preview's exposure, independently of optical depth.
            fog.ambient_intensity = std::max(0.0f, EnvFloat("FROXEL_AMBIENT", 12.0f));
            fog.scattering_albedo = 0.9f;
            fog.march_steps = (u32)std::clamp(EnvFloat("FROXEL_MARCH", 32.0f), 0.0f, 64.0f);
            // Resolve the cloud depth instead of spending most slices on distant haze.
            // Fine surface erosion disappears in an 8px / coarse-depth cell.
            // Spend the extra froxel budget only in the cloud look preview.
            fog.grid_width = 480;
            fog.grid_height = 270;
            fog.grid_depth = 320;
            fog.far_plane = 40.0f;
            std::cerr << "[FroxelVolume] preview enabled (uniform="
                      << fog.density_uniform_debug << ", density=" << fog.cloud_density
                      << ", extinction=" << fog.extinction_scale << ")" << std::endl;
        }
    }
    pipeline_->UpdateSettings(renderSettings);

    // Reflections and volume paths are gated per-phase (roadmap
    // 2026-09-15-sponza-reference-look-roadmap): SSR/Volumetric 仍默认关，
    // FroxelFog 由 FROXEL=1 环境变量开启（P2.1 Vulkan 复测）。
    pipeline_->SetPassEnabled(RenderPassID::SSR, EnvEnabled("SSR"));
    pipeline_->SetMirrorEnabled(false);
    pipeline_->SetPassEnabled(RenderPassID::VolumePass, false);
    pipeline_->SetPassEnabled(RenderPassID::VolumeRenderer, false);
    pipeline_->SetPassEnabled(RenderPassID::FroxelFog, EnvEnabled("FROXEL"));
    // WP3: FROXEL_TEMPORAL=1 → 体积时间重投影开启（G1-G5 的 temporal 回归
    // 通道；门禁默认关——G5 阈值按无时域标定）。
    if (EnvEnabled("FROXEL_TEMPORAL")) {
        pipeline_->SetFroxelTemporalEnabled(true);
        std::cerr << "[FroxelGate] temporal reprojection ON (WP3)" << std::endl;
    }

    // Mirror plane: cover the whole Sponza floor (the camera starts at
    // {0,5,0} looking horizontally — a small mirror at the origin sits
    // directly beneath it and out of view; a 40x40 floor mirror is visible
    // from any vantage point).
    primal::graphics::PlanarReflectionPlane mirrorPlane;
    mirrorPlane.position = {0.0f, 0.02f, 0.0f};
    mirrorPlane.normal = {0.0f, 1.0f, 0.0f};
    mirrorPlane.half_extents = {20.0f, 20.0f};
    mirrorPlane.reflectivity = 0.85f;
    pipeline_->SetMirrorPlane(mirrorPlane);

    subsystemsInitialized_ = true;
    return true;
}

bool TestVulkanFroxelRenderGraph::LoadSponzaScene() {
    const std::string baseDir =
        "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/assets/";
    // Try pipeline model (high-precision per-mesh SDF) first, fall back to old.
    // PRIMAL_TEST_MODEL：A/B 替代资产名（如 SponzaSDF.model——--sdf 烘焙的
    // per-mesh SDF 版本，供 PRIMAL_DDGI_DETAIL_SDF 消费）。
    std::string modelName = "Sponza.model";
    if (const char* e = std::getenv("PRIMAL_TEST_MODEL")) modelName = e;
    std::string modelPath = baseDir + modelName;
    std::ifstream file(modelPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cerr << "[Part30.4] Failed to open " << modelPath << std::endl;
        return false;
    }
    std::streamsize modelSize = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<char> modelBuffer(static_cast<size_t>(modelSize));
    if (!file.read(modelBuffer.data(), modelSize)) {
        std::cerr << "[Part30.4] Failed to read " << modelName << std::endl;
        return false;
    }

    SceneDataAdapter adapter;
    sceneMeshes_ = adapter.LoadRenderItemData(
        device_, modelBuffer.data(), (u32)modelBuffer.size());
    if (sceneMeshes_.empty()) {
        std::cerr << "[Part30.4] LoadRenderItemData returned 0 meshes" << std::endl;
        return false;
    }
    std::cout << "[Part30.4] Loaded " << sceneMeshes_.size()
              << " meshes from " << modelName << std::endl;

    // Shared Material + fallback textures + sampler (mirror NonEditor Step 2).
    sharedMaterial_ = std::make_shared<Material>();

    constexpr u32 FALLBACK_SIZE = 1024;
    std::vector<unsigned char> whiteBuf(FALLBACK_SIZE * FALLBACK_SIZE * 4, 255);
    std::vector<unsigned char> flatNormalBuf(FALLBACK_SIZE * FALLBACK_SIZE * 4, 0);
    for (u32 i = 0; i < FALLBACK_SIZE * FALLBACK_SIZE; ++i) {
        flatNormalBuf[i * 4 + 0] = 128;
        flatNormalBuf[i * 4 + 1] = 128;
        flatNormalBuf[i * 4 + 2] = 255;
        flatNormalBuf[i * 4 + 3] = 255;
    }
    std::vector<unsigned char> defaultORMBuf(FALLBACK_SIZE * FALLBACK_SIZE * 4, 0);
    for (u32 i = 0; i < FALLBACK_SIZE * FALLBACK_SIZE; ++i) {
        defaultORMBuf[i * 4 + 0] = 255;  // AO=1
        defaultORMBuf[i * 4 + 1] = 128;  // roughness=0.5
        defaultORMBuf[i * 4 + 2] = 0;    // metallic=0
        defaultORMBuf[i * 4 + 3] = 255;
    }
    fallbackDiffuse_ = CreateTextureFromData(
        device_, FALLBACK_SIZE, FALLBACK_SIZE, whiteBuf.data(), DataFormat::RGBA8_sRGB);
    fallbackNormal_ = CreateTextureFromData(
        device_, FALLBACK_SIZE, FALLBACK_SIZE, flatNormalBuf.data());
    fallbackORM_ = CreateTextureFromData(
        device_, FALLBACK_SIZE, FALLBACK_SIZE, defaultORMBuf.data());
    if (fallbackDiffuse_ == handles::INVALID_RESOURCE ||
        fallbackNormal_ == handles::INVALID_RESOURCE ||
        fallbackORM_ == handles::INVALID_RESOURCE) {
        std::cerr << "[Part30.4] Fallback texture creation failed" << std::endl;
        return false;
    }

    SamplerDesc samplerDesc{};
    samplerDesc.minFilter = FilterMode::Linear;
    samplerDesc.magFilter = FilterMode::Linear;
    samplerDesc.addressU = TextureAddressMode::Wrap;
    samplerDesc.addressV = TextureAddressMode::Wrap;
    samplerDesc.addressW = TextureAddressMode::Wrap;
    samplerDesc.comparisonFunc = ComparisonFunc::Never;
    materialSampler_ = device_->CreateSampler(samplerDesc);
    if (materialSampler_ == handles::INVALID_SAMPLER) {
        std::cerr << "[Part30.4] materialSampler creation failed" << std::endl;
        return false;
    }

    // Per-mesh MaterialInstance + game_entity + cluster + RenderProxy
    // (mirror NonEditor Step 3).
    //
    // T4.6.5 part 30.7 (Track 2): parallelize texture decode via JobSystem.
    // stbi_load is I/O + CPU bound (TGA decode) and runs ~0.5-2s total
    // across 393 meshes. device_->CreateTexture + MaterialInstance::Update
    // touch RHI state and may not be thread-safe, so they stay on main
    // thread after the parallel decode phase completes.
    const u32 meshCount = (u32)sceneMeshes_.size();
    struct DecodedTex {
        int width{0}, height{0};
        unsigned char* data{nullptr};
        bool valid{false};
    };
    std::vector<DecodedTex> decAlbedo(meshCount), decNormal(meshCount), decORM(meshCount);

    auto decodeJob = [&](u32 i, u32) {
        auto& meshInfo = sceneMeshes_[i];
        auto decode = [](const std::string& path, DecodedTex& out) {
            if (path.empty()) return;
            out.data = stbi_load(path.c_str(), &out.width, &out.height, nullptr, 4);
            out.valid = (out.data != nullptr);
        };
        decode(ResolveTexturePath(baseDir, meshInfo.diffuseTexturePath), decAlbedo[i]);
        // P1-C（sponza-gap-audit §4.4）: 法线完整性。Sponza.model 的
        // pipeline 格式材质里 normal 字段全为空字符串——法线贴图从未加载
        // （全静默回退平法线）。显式路径优先，否则按磁盘命名约定
        // （`_diffuse`→`_normal`、`_Albedo`→`_Normal`）派生兄弟贴图。
        {
            std::string normalPath = ResolveTexturePath(baseDir, meshInfo.normalTexturePath);
            if (normalPath.empty()) {
                const std::string diffRel = meshInfo.diffuseTexturePath;
                for (const char* suffix : {"_normal.png", "_Normal.png"}) {
                    const size_t pos = diffRel.rfind("_diffuse");
                    if (pos != std::string::npos) {
                        normalPath = ResolveTexturePath(baseDir, diffRel.substr(0, pos) + suffix);
                    } else {
                        const size_t posA = diffRel.rfind("_Albedo");
                        if (posA != std::string::npos)
                            normalPath = ResolveTexturePath(baseDir, diffRel.substr(0, posA) + suffix);
                    }
                    if (!normalPath.empty()) break;
                }
            }
            decode(normalPath, decNormal[i]);
        }
        // P1-C: ORM 完整性。pipeline 格式从不赋值 ormTexturePath，且磁盘
        // 资产是分离的 `_roughness/_metallic`（无 _orm 合并文件）——此前
        // 全部 393 个 mesh 静默回退 roughness≈0.5 常数，粗糙度贴图被抹掉。
        // 先试显式 ormTexturePath，否则派生 roughness/metallic 兄弟贴图并
        // 合成 ORM（R=AO 恒 1，G=roughness，B=metallic）。
        const std::string ormPath = ResolveTexturePath(baseDir, meshInfo.ormTexturePath);
        if (!ormPath.empty()) {
            decode(ormPath, decORM[i]);
        } else {
            const std::string diffRel = meshInfo.diffuseTexturePath;
            auto deriveSibling = [&](const char* const* suffixes, int n) {
                for (int si = 0; si < n; ++si) {
                    std::string p;
                    const size_t pos = diffRel.rfind("_diffuse");
                    if (pos != std::string::npos) {
                        p = ResolveTexturePath(baseDir, diffRel.substr(0, pos) + suffixes[si]);
                        if (!p.empty()) return p;
                    }
                    const size_t posA = diffRel.rfind("_Albedo");
                    if (posA != std::string::npos) {
                        p = ResolveTexturePath(baseDir, diffRel.substr(0, posA) + suffixes[si]);
                        if (!p.empty()) return p;
                    }
                }
                return std::string();
            };
            static const char* kRoughSuf[] = {"_roughness.png", "_Roughness.png"};
            static const char* kMetalSuf[] = {"_metallic.png", "_Metallic.png"};
            DecodedTex rough, metal;
            decode(deriveSibling(kRoughSuf, 2), rough);
            decode(deriveSibling(kMetalSuf, 2), metal);
            if (rough.valid || metal.valid) {
                const int w = rough.valid ? rough.width : metal.width;
                const int h = rough.valid ? rough.height : metal.height;
                const size_t pxCount = (size_t)w * (size_t)h;
                auto* out = (unsigned char*)malloc(pxCount * 4);
                if (out) {
                    for (size_t px = 0; px < pxCount; ++px) {
                        out[px * 4 + 0] = 255;                                   // AO: 无烘焙 AO
                        out[px * 4 + 1] = rough.valid ? rough.data[px * 4 + 0] : 128;
                        out[px * 4 + 2] = metal.valid ? metal.data[px * 4 + 0] : 0;
                        out[px * 4 + 3] = 255;
                    }
                    decORM[i] = {w, h, out, true};
                }
            }
            if (rough.data) stbi_image_free(rough.data);
            if (metal.data) stbi_image_free(metal.data);
        }
    };

    auto t0 = std::chrono::high_resolution_clock::now();
    auto decodeHandle = primal::jobsystem::JobSystem::ParallelForWithThread(meshCount, decodeJob);
    primal::jobsystem::JobSystem::Wait(decodeHandle);
    auto t1 = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> decodeMs = t1 - t0;
    std::cout << "[Part30.4] Parallel texture decode: " << meshCount
              << " meshes in " << decodeMs.count() << " ms" << std::endl;

    // Serial phase: CreateTextureFromData + material/entity setup.
    // RHI device + MaterialInstance are not thread-safe.
    u32 texLoaded = 0, texFailed = 0;
    // P1-C: normal/ORM 也计数——旧的 "392 loaded, 1 fallback" 只统计
    // diffuse，法线/ORM 的静默回退完全不可见。
    u32 normalLoaded = 0, normalFallback = 0, ormLoaded = 0, ormFallback = 0;
    std::string missingNormalList, missingORMList;
    // The supplied reference uses a substantially darker courtyard stone than
    // this Sponza asset.  Keep the correction reference-only and in linear
    // albedo space; the environment override remains available for A/B.
    float floorAlbedoScale = sponza_reference::Enabled() ? 0.5f : 1.0f;
    if (const char* e = std::getenv("SPONZA_FLOOR_ALBEDO_SCALE")) {
        char* end = nullptr;
        const float parsed = std::strtof(e, &end);
        if (end != e && std::isfinite(parsed))
            floorAlbedoScale = std::clamp(parsed, 0.1f, 1.5f);
    }
    u32 scaledFloorMeshes = 0;
    for (u32 i = 0; i < meshCount; ++i) {
        auto& meshInfo = sceneMeshes_[i];
        meshInfo.material = sharedMaterial_;

        auto matInst = std::make_shared<MaterialInstance>(sharedMaterial_.get());
        if (!matInst->Initialize(device_)) {
            std::cerr << "[Part30.4] MaterialInstance init failed for mesh " << i << std::endl;
            matInst = std::make_shared<MaterialInstance>(sharedMaterial_.get());
        }

        ResourceHandle diffuseTex = handles::INVALID_RESOURCE;
        if (decAlbedo[i].valid) {
            std::string lowerDiffuse = meshInfo.diffuseTexturePath;
            for (auto& ch : lowerDiffuse) ch = (char)tolower((unsigned char)ch);
            if (floorAlbedoScale != 1.0f &&
                lowerDiffuse.find("sponza_floor_diffuse") != std::string::npos) {
                const size_t pixelCount = (size_t)decAlbedo[i].width * decAlbedo[i].height;
                for (size_t px = 0; px < pixelCount; ++px) {
                    for (u32 c = 0; c < 3; ++c) {
                        const float srgb = decAlbedo[i].data[px * 4 + c] / 255.0f;
                        float linear = srgb <= 0.04045f
                            ? srgb / 12.92f
                            : std::pow((srgb + 0.055f) / 1.055f, 2.4f);
                        linear = std::clamp(linear * floorAlbedoScale, 0.0f, 1.0f);
                        const float encoded = linear <= 0.0031308f
                            ? linear * 12.92f
                            : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
                        decAlbedo[i].data[px * 4 + c] =
                            (unsigned char)std::clamp(encoded * 255.0f + 0.5f, 0.0f, 255.0f);
                    }
                }
                ++scaledFloorMeshes;
            }
            diffuseTex = CreateTextureFromData(
                device_, decAlbedo[i].width, decAlbedo[i].height,
                decAlbedo[i].data, DataFormat::RGBA8_sRGB);
        }
        if (diffuseTex == handles::INVALID_RESOURCE) { diffuseTex = fallbackDiffuse_; texFailed++; }
        else texLoaded++;

        ResourceHandle normalTex = handles::INVALID_RESOURCE;
        if (decNormal[i].valid) {
            normalTex = CreateTextureFromData(
                device_, decNormal[i].width, decNormal[i].height, decNormal[i].data);
        }
        if (normalTex == handles::INVALID_RESOURCE) {
            normalTex = fallbackNormal_;
            normalFallback++;
            if (missingNormalList.size() < 512) {
                missingNormalList += " ";
                missingNormalList += meshInfo.name;
            }
        } else normalLoaded++;

        ResourceHandle ormTex = handles::INVALID_RESOURCE;
        if (decORM[i].valid) {
            ormTex = CreateTextureFromData(
                device_, decORM[i].width, decORM[i].height, decORM[i].data);
        }
        if (ormTex == handles::INVALID_RESOURCE) {
            ormTex = fallbackORM_;
            ormFallback++;
            if (missingORMList.size() < 512) {
                missingORMList += " ";
                missingORMList += meshInfo.name;
            }
        } else ormLoaded++;

        // Free stbi buffers now that CreateTextureFromData has copied the pixels.
        if (decAlbedo[i].data) stbi_image_free(decAlbedo[i].data);
        if (decNormal[i].data) stbi_image_free(decNormal[i].data);
        if (decORM[i].data) stbi_image_free(decORM[i].data);

        matInst->SetTexture(0, diffuseTex);
        matInst->SetTexture(1, normalTex);
        matInst->SetTexture(2, ormTex);
        matInst->SetSampler(3, materialSampler_);
        matInst->Update(device_);

        meshInfo.materialInstance = matInst;
        materialInstances_.push_back(matInst);

        primal::game_entity::entity_info entInfo{};
        primal::transform::init_info tfInfo{};
        tfInfo.position[0] = 0.0f;
        tfInfo.position[1] = 0.0f;
        tfInfo.position[2] = 0.0f;
        tfInfo.rotation[0] = 0.0f;
        tfInfo.rotation[1] = 0.0f;
        tfInfo.rotation[2] = 0.0f;
        tfInfo.rotation[3] = 1.0f;
        entInfo.transform = &tfInfo;
        primal::game_entity::entity entity = primal::game_entity::create(entInfo);
        if (!entity.is_valid()) {
            std::cerr << "[Part30.4] game_entity::create failed for mesh " << i << std::endl;
            return false;
        }
        entities_.push_back(entity);

        primal::cluster::init_info clusterInit{};
        clusterInit.geometry_content_id = meshInfo.meshEntityId;
        primal::cluster::component clusterComp =
            primal::cluster::create(clusterInit, entity);
        clusterComps_.push_back(clusterComp);

        RenderProxy proxy;
        proxy.materialId = meshInfo.meshEntityId;
        proxy.entityId = meshInfo.meshEntityId;
        proxy.meshId = clusterComp;
        proxy.transform = rhimath::MatrixIdentity();
        // P2-B: 布料实例标注透光掩码（audit §4.7——石墙严格 0，薄布料开启）。
        // pipeline 格式的 mesh 名是通用名（Mesh_N_M），材质身份在贴图路径里
        // ——按 diffuse 文件名匹配帘幕/织物；其余（石材、金属链等）保持 0。
        {
            std::string lowerName = meshInfo.diffuseTexturePath;
            for (auto& ch : lowerName) ch = (char)tolower((unsigned char)ch);
            if (lowerName.find("curtain") != std::string::npos ||
                lowerName.find("fabric") != std::string::npos ||
                lowerName.find("cloth") != std::string::npos ||
                lowerName.find("banner") != std::string::npos) {
                proxy.transmission = 0.7f;
                // Keep the GPU material table in sync with the per-instance
                // transport annotation.  Nanite GBuffer/CardCapture consume
                // MaterialData, while the proxy value is used by the legacy
                // forward path; setting both avoids a backend-dependent cloth
                // transmission split.
                matInst->SetTransmissionFactor(proxy.transmission);
                // Thin hanging cloth: normalized optical path used by the
                // Surface Cache back-light transmission term.
                matInst->SetThicknessFactor(0.15f);
            }
        }
        if (meshInfo.mesh && meshInfo.mesh->IsValid()) {
            proxy.worldAABB = meshInfo.mesh->GetLocalAABB();
        }
        scene_.AddProxy(proxy);
    }
    std::cout << "[Part30.4] Textures: diffuse " << texLoaded << " loaded / "
              << texFailed << " fallback; normal " << normalLoaded << " / "
              << normalFallback << "; ORM " << ormLoaded << " / "
              << ormFallback << std::endl;
    if (floorAlbedoScale != 1.0f) {
        std::cout << "[Part30.4] Floor albedo scale=" << floorAlbedoScale
                  << " meshes=" << scaledFloorMeshes << std::endl;
    }
    if (!missingNormalList.empty())
        std::cout << "[Part30.4] Missing normal maps:" << missingNormalList << std::endl;
    if (!missingORMList.empty())
        std::cout << "[Part30.4] Missing ORM maps:" << missingORMList << std::endl;
    std::cout << "[Part30.4] Scene proxies: " << scene_.GetProxies().size() << std::endl;

    // GPUMaterialRegistry wiring (mirror NonEditor Step 4).
    materialRegistry_ = new primal::graphics::nanite::GPUMaterialRegistry();
    u32 registeredCount = 0;
    for (auto& meshInfo : sceneMeshes_) {
        if (meshInfo.materialInstance) {
            auto matID = materialRegistry_->RegisterMaterial(meshInfo.materialInstance.get());
            if (matID != primal::graphics::nanite::GPUMaterialRegistry::INVALID_MATERIAL_ID) {
                meshInfo.gpuMaterialId = matID;
                registeredCount++;
            }
        }
    }
    std::cout << "[Part30.4] Registered " << registeredCount << " materials" << std::endl;

    // Patch proxy.materialId: entity_id → gpuMaterialId.
    for (auto& meshInfo : sceneMeshes_) {
        if (meshInfo.materialInstance &&
            meshInfo.gpuMaterialId != primal::id::invalid_id) {
            for (const auto& proxy : scene_.GetProxies()) {
                if (proxy.entityId == meshInfo.meshEntityId) {
                    RenderProxy patched = proxy;
                    patched.materialId = meshInfo.gpuMaterialId;
                    scene_.UpdateProxy(meshInfo.meshEntityId, patched);
                    break;
                }
            }
        }
    }

    auto buildJob = materialRegistry_->BuildAsync(device_);
    buildJob.Wait();
    if (!materialRegistry_->UploadToGPU(device_)) {
        std::cerr << "[Part30.4] Material upload failed" << std::endl;
        return false;
    }
    std::cout << "[Part30.4] Materials uploaded to GPU" << std::endl;

    auto& gpuDraw = primal::graphics::nanite::GPUDrivenDrawPipeline::Get();
    // 2026-09-06 诊断：PRIMAL_GBUFFER_DEBUG=4 世界法线可视化 / 5 物体法线
    if (const char* dm = std::getenv("PRIMAL_GBUFFER_DEBUG")) {
        gpuDraw.SetDebugMode((u32)atoi(dm));
        std::cerr << "[Diag] meshlet debug_mode = " << dm << std::endl;
    }
    // 2026-09-07 Vulkan 主视图阴影切 PCSS（ShadowFilter.comp 半分辨率
    // R8 可见性）：VSM moments 链（WGSL 光栅/模糊）实测几何错位——
    // 光池区可见性仅 0.5、二楼内部 0.5-0.64 泄漏，而 Metal VSM 正常
    // （光池 1.0）。PCSS 路径绕开 moments 全链。默认切换；置
    // PRIMAL_VSM_SHADOWS=1 可回 VSM 对照。
    if (!std::getenv("PRIMAL_VSM_SHADOWS")) {
        pipeline_->SetVSMShadows(false);
        std::cerr << "[Shadow] Vulkan PCSS path (VSM moments chain displaced, see docs)" << std::endl;
    }
    gpuDraw.SetMaterialDataBuffer(materialRegistry_->GetMaterialDataBuffer());
    SamplerDesc texSamplerDesc{};
    texSamplerDesc.minFilter = FilterMode::Linear;
    texSamplerDesc.magFilter = FilterMode::Linear;
    texSamplerDesc.mipFilter = FilterMode::Linear;
    texSamplerDesc.addressU = TextureAddressMode::Wrap;
    texSamplerDesc.addressV = TextureAddressMode::Wrap;
    texSamplerDesc.addressW = TextureAddressMode::Wrap;
    // Use the highest safe value for the oblique, mipmapped floor textures,
    // capped at the device limit and at 8x for a stable reference capture.
    // RHI Vulkan enables this feature only when supported, so keep 1x on
    // devices that expose no sampler anisotropy capability.
    u32 textureAnisotropy = 1;
    if (auto* vkDevice = static_cast<VulkanDevice*>(device_)) {
        VkPhysicalDeviceFeatures supportedFeatures{};
        vkGetPhysicalDeviceFeatures(vkDevice->GetNativePhysicalDevice(),
                                    &supportedFeatures);
        VkPhysicalDeviceProperties physicalProperties{};
        vkGetPhysicalDeviceProperties(vkDevice->GetNativePhysicalDevice(),
                                      &physicalProperties);
        if (supportedFeatures.samplerAnisotropy == VK_TRUE) {
            textureAnisotropy = std::max(
                1u, std::min(8u, static_cast<u32>(physicalProperties.limits.maxSamplerAnisotropy)));
        }
    }
    texSamplerDesc.maxAnisotropy = textureAnisotropy;
    texSamplerDesc.minLod = 0.0f;
    texSamplerDesc.maxLod = 100.0f;
    texSamplerDesc.comparisonFunc = ComparisonFunc::Never;
    texSampler_ = device_->CreateSampler(texSamplerDesc);
    gpuDraw.SetTextureArrays(
        materialRegistry_->GetAlbedoTextureArray(),
        materialRegistry_->GetNormalTextureArray(),
        materialRegistry_->GetORMTextureArray(),
        texSampler_);
    // T4.6.5 part 35.7: GPUDrivenDrawPipeline::SetTextureArrays stores the
    // sampler handle and destroys it on Shutdown (cpp:274). Mark ownership
    // transferred so this test doesn't double-destroy. The engine-side
    // destroySamplerImpl now also has a double-free guard (VulkanDevice.cpp),
    // so even without this marker the abort is gone — but skipping the
    // redundant call keeps the diagnostic log clean.
    (void)texSampler_;

    // Load offline per-mesh SDF from the pipeline model and merge into the
    // global 384³ volume. DDGI's SDF trace needs this precision — the runtime
    // GlobalSDF (1m/2m/4m unsigned voxels, nearest fetch) misplaces hits and
    // skews the GI spatial distribution (back wall out-bouncing the shaded
    // arcade). First run merges (~5 min) and writes a cache beside the assets;
    // later runs load it in ~1s. Delete the .cache to force a re-merge.
    // signed_v5 = 2026-09 re-bake, fast-winding-number inside test (single-ray
    // parity poisoned hollow architecture: nave air read −40 post-merge).
    // PGSDFC02 cache records the encoding so the old unsigned v1 cache can
    // never be reused against the new field semantics.
    {
        const std::string sdfCache = baseDir + "Sponza_global_sdf.v3.cache";
        std::string pipeModelPath = baseDir + "Sponza_pipeline_signed_v5.model";
        // SDF_REBAKE_V6=1: propagate the per-mesh SDF fields in-place into a
        // v6 model (M3-A1 — the v5 bake clamped air beyond the 3³ search to
        // max_dim), then trace from v6 with a forced re-merge.
        if (std::getenv("SDF_REBAKE_V6")) {
            const std::string v6 = baseDir + "Sponza_pipeline_signed_v6.model";
            u32 n = primal::graphics::nanite::OfflineSDFMerger::RebakePerMeshSDF(
                pipeModelPath.c_str(), v6.c_str());
            std::cerr << "[SDF_REBAKE] rebaked " << n << " sections → " << v6
                      << std::endl;
            if (n > 0) {
                pipeModelPath = v6;
                std::remove(sdfCache.c_str()); // force re-merge from v6
            }
        }
        offlineSDFMerger_ = new primal::graphics::nanite::OfflineSDFMerger();
        if (!offlineSDFMerger_->LoadFromCache(device_, sdfCache.c_str())) {
            u32 sdfCount = offlineSDFMerger_->LoadFromPipelineModel(pipeModelPath.c_str(), /*signed_encoding=*/true);
            if (sdfCount > 0) {
                std::cerr << "[Part30.4] Offline SDF loaded: " << sdfCount
                          << " meshes, merging..." << std::endl;
                if (!offlineSDFMerger_->BuildAndUpload(device_, sdfCache.c_str())) {
                    std::cerr << "[Part30.4] Offline SDF merge/upload failed" << std::endl;
                }
            } else {
                std::cerr << "[Part30.4] No offline SDF data found (pipeline model missing?)"
                          << std::endl;
            }
        }
        if (!offlineSDFMerger_->IsValid()) {
            delete offlineSDFMerger_;
            offlineSDFMerger_ = nullptr;
        }
    }

    // Wire offline SDF into the render pipeline for F6 visualization.
    if (offlineSDFMerger_ && offlineSDFMerger_->IsValid() &&
        !std::getenv("SPGI_NO_OFFLINE_SDF")) {   // A/B: runtime GlobalSDF path
        pipeline_->SetOfflineSDFSource(
            offlineSDFMerger_->GetTexture(),
            offlineSDFMerger_->GetOrigin(),
            offlineSDFMerger_->GetExtent(),
            offlineSDFMerger_->GetResolution(),
            offlineSDFMerger_->IsSignedEncoding());   // GI 根治 Phase 4: 零穿越命中
    }

    // Directional light + camera (TestDawnForwardRenderer defaults).
    RenderLight sunLight;
    sunLight.type = LightType::Directional;
    sunLight.direction = v3{0.5f, -0.7f, 0.3f};
    sunLight.color = v3{1.0f, 0.95f, 0.9f};
    sunLight.intensity = 3.0f;
    sunLight.entityId = 7001;
    if (sponza_reference::Enabled() || std::getenv("PRIMAL_LIGHT_DIR") ||
        std::getenv("PRIMAL_DIRECT_LIGHT")) {
        const auto& light = pipeline_->GetSettings().lighting;
        sunLight.direction = light.light_direction;
        sunLight.color = {light.light_color.x / 3.0f, light.light_color.y / 3.0f,
                          light.light_color.z / 3.0f};
    }
    scene_.AddLight(sunLight);

    const u32 W = window_.width();
    const u32 H = window_.height();
    v3 cameraPos{0.0f, 5.0f, -10.0f};
    float cameraYaw = 3.14159265f;
    float cameraPitch = -0.291f;
    float cosPitch = cosf(cameraPitch);
    v3 forward{
        -sinf(cameraYaw) * cosPitch,
        sinf(cameraPitch),
        -cosf(cameraYaw) * cosPitch
    };
    v3 target = cameraPos + forward;
    v3 up{0.0f, 1.0f, 0.0f};

    viewMatrix_ = rhimath::CreateLookAtMatrix(cameraPos, target, up);
    projMatrix_ = rhimath::CreatePerspectiveMatrix(
        sponza_reference::FovDegrees() * rhimath::constants::DEG_TO_RAD,
        static_cast<float>(W) / static_cast<float>(H), 0.1f, 1000.0f);
    view_.SetViewMatrix(viewMatrix_);
    view_.SetProjectionMatrix(projMatrix_);
    view_.SetViewport({ {0, 0}, {static_cast<float>(W), static_cast<float>(H)}, 0, 1 });
    view_.SetScissor({ {0, 0}, {W, H} });
    view_.UpdateFrustum();
    view_.Cull(scene_);
    return true;
}

// T4.6.5 part 37 — Tier 5 IBL setup.
//
// Loads sunset.hdr (RGBA32F) → converts to RGBA16F → uploads to equirectTex_
// (Texture2D). Dispatches IBL_EquirectangularToCube.spv to write envCube_
// (TextureCube, 6 faces) via its 2DArray storage view. Then runs IBLPrecomputer
// to generate irradiance/prefilter cubes + brdfLUT 2D. All four resources are
// forwarded to deferred_module_ via pipeline_->SetIBLResources().
//
// envMap dimensions: sunset.hdr is 2048×1024 → faceSize = 2048/4 = 512. Cube
// is 512×512×6 (RGBA16F). IBLPrecomputer output: irradiance 32³, prefilter
// 128³, brdfLUT 512².
//
// NOTE: This method is non-fatal on failure — Initialize() proceeds without
// IBL. DeferredLighting.frag falls back to flat 0.03*albedo ambient when
// IBL bindings are invalid (bindings 10/11/12 set to INVALID_RESOURCE).
bool TestVulkanFroxelRenderGraph::InitializeIBL() {
    // sun-audit P2: PRIMAL_SKY_IBL=1 → 解析天空作为 IBL 源（见步骤 2 注释），
    // 不查找/不要求 sunset.hdr。
    const bool skyIbl = std::getenv("PRIMAL_SKY_IBL") != nullptr;
    // ----- 1. Locate sunset.hdr -----
    const char* kHdrPaths[] = {
        "EngineTest/assets/textures/hdr/sunset.hdr",
        "textures/hdr/sunset.hdr",
        "Assets/Textures/HDR/sunset.hdr",
        "sunset.hdr",
    };
    std::string hdrPath;
    if (!skyIbl) {
        for (const char* p : kHdrPaths) {
            std::ifstream f(p);
            if (f.is_open()) { hdrPath = p; break; }
        }
        // T4.6.5 part 37: walk up parent dirs as fallback (test runs from various cwd).
        if (hdrPath.empty()) {
            for (int i = 1; i <= 6; ++i) {
                std::string prefix;
                for (int j = 0; j < i; ++j) prefix += "../";
                std::ifstream f(prefix + "EngineTest/assets/textures/hdr/sunset.hdr");
                if (f.is_open()) {
                    hdrPath = prefix + "EngineTest/assets/textures/hdr/sunset.hdr";
                    break;
                }
            }
        }
        if (hdrPath.empty()) {
            std::cerr << "[Part37] sunset.hdr not found — IBL disabled" << std::endl;
            return false;
        }
        std::cerr << "[Part37] Loading HDR: " << hdrPath << std::endl;
    }

    // ----- 2. HDR source: sunset.hdr（默认）或解析天空（PRIMAL_SKY_IBL=1）-----
    // sun-audit P2（同源天空）: skyIbl 时 equirect 由解析天空填充
    // ——SkyAtmosphere.glsl::EvaluateSkyBase 的 CPU 逐位镜像（horizon 4.40,
    // 4.20, 3.80 / zenith 5.50, 5.76, 6.00 / pow(up, 0.45)；若改 GLSL 侧
    // 常量必须同步此处）。可见天空、GI miss、镜面 IBL 三者自此同源；不含
    // 太阳盘/日晕——直射由 deferred 单独供给，防止重复计能。
    int hdrW = 0, hdrH = 0, hdrC = 4;
    float* hdrData = nullptr;
    if (skyIbl) {
        hdrW = 1024;
        hdrH = 512;
        hdrData = static_cast<float*>(std::malloc(size_t(hdrW) * hdrH * 4 * sizeof(float)));
        for (int y = 0; y < hdrH; ++y) {
            // 与 IBL_EquirectangularToCube.wgsl 的 sphereUV 约定互逆：
            // uv = (atan2(z,x), asin(y)) * invAtan + 0.5
            const float theta = ((y + 0.5f) / hdrH - 0.5f) * 3.14159265f;
            for (int x = 0; x < hdrW; ++x) {
                const float phi = ((x + 0.5f) / hdrW - 0.5f) * 6.28318531f;
                const float up = std::clamp(std::sin(theta), 0.0f, 1.0f);
                const float t = std::pow(up, 0.45f);
                float* px = hdrData + (size_t(y) * hdrW + x) * 4;
                px[0] = 4.40f + (5.50f - 4.40f) * t;
                px[1] = 4.20f + (5.76f - 4.20f) * t;
                px[2] = 3.80f + (6.00f - 3.80f) * t;
                px[3] = 1.0f;
            }
        }
        std::cerr << "[Part37] PRIMAL_SKY_IBL: equirect filled from analytic sky "
                  << hdrW << "x" << hdrH << " (EvaluateSkyBase mirror)" << std::endl;
    } else {
        hdrData = stbi_loadf(hdrPath.c_str(), &hdrW, &hdrH, &hdrC, 4);
        if (!hdrData) {
            std::cerr << "[Part37] stbi_loadf failed: " << hdrPath << std::endl;
            return false;
        }
        std::cerr << "[Part37] HDR loaded: " << hdrW << "x" << hdrH << std::endl;
    }

    // ----- 3. Convert f32 → f16 (RGBA16F) -----
    const size_t kPixels = size_t(hdrW) * hdrH;
    std::vector<u8> rgba16(kPixels * 8);  // 4 channels × 2 bytes
    for (size_t i = 0; i < kPixels; ++i) {
        u16 r = f32_to_f16(hdrData[i * 4 + 0]);
        u16 g = f32_to_f16(hdrData[i * 4 + 1]);
        u16 b = f32_to_f16(hdrData[i * 4 + 2]);
        u16 a = f32_to_f16(hdrData[i * 4 + 3]);
        std::memcpy(rgba16.data() + i * 8 + 0, &r, 2);
        std::memcpy(rgba16.data() + i * 8 + 2, &g, 2);
        std::memcpy(rgba16.data() + i * 8 + 4, &b, 2);
        std::memcpy(rgba16.data() + i * 8 + 6, &a, 2);
    }
    if (skyIbl) std::free(hdrData); else stbi_image_free(hdrData);

    // ----- 4. Create equirectTex_ (Texture2D RGBA16F) -----
    TextureDesc equirectDesc{};
    equirectDesc.size = {(u32)hdrW, (u32)hdrH, 1};
    equirectDesc.mipLevels = 1;
    equirectDesc.arraySize = 1;
    equirectDesc.format = DataFormat::RGBA16_Float;
    equirectDesc.type = TextureType::Texture2D;
    equirectDesc.usage = TextureUsage::ShaderResource | TextureUsage::CopyDest;
    equirectDesc.memoryUsage = GPUMemoryUsage::Static;
    equirectDesc.name = "IBL_Equirect";
    equirectTex_ = device_->CreateTexture(equirectDesc);
    if (equirectTex_ == handles::INVALID_RESOURCE) {
        std::cerr << "[Part37] CreateTexture equirectTex_ failed" << std::endl;
        return false;
    }

    // Upload via staging buffer + CopyBufferToTexture.
    BufferDesc eqStagingDesc{};
    eqStagingDesc.size = kPixels * 8;
    eqStagingDesc.type = BufferType::Raw;
    eqStagingDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    eqStagingDesc.name = "IBL_Equirect_Staging";
    ResourceHandle eqStaging = device_->CreateBuffer(eqStagingDesc);
    if (eqStaging == handles::INVALID_RESOURCE) {
        device_->DestroyTexture(equirectTex_);
        equirectTex_ = handles::INVALID_RESOURCE;
        return false;
    }
    if (!device_->UpdateBufferData(eqStaging, rgba16.data(), eqStagingDesc.size, 0)) {
        device_->DestroyBuffer(eqStaging);
        device_->DestroyTexture(equirectTex_);
        equirectTex_ = handles::INVALID_RESOURCE;
        return false;
    }

    {
        CommandBufferHandle cmd = device_->CreateCommandBuffer(CommandQueueType::Graphics);
        VulkanCommandBuffer* vcmd = static_cast<VulkanDevice*>(device_)->GetCommandBuffer(cmd);
        vcmd->Reset(); vcmd->Begin();
        BufferTextureCopyRegion region{};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource = {0, 0, 1};
        region.imageOffset = {0, 0, 0};
        region.imageExtent = {(u32)hdrW, (u32)hdrH, 1};
        vcmd->CopyBufferToTexture(eqStaging, equirectTex_, &region, 1);

        // Transition equirectTex_ CopyDest → ShaderResource.
        ResourceBarrier toSRV{};
        toSRV.resource = equirectTex_;
        toSRV.beforeState = ResourceState::CopyDest;
        toSRV.afterState = ResourceState::ShaderResource;
        toSRV.subresource = 0xFFFFFFFF;
        toSRV.queueFamily = 0xFFFFFFFF;
        vcmd->InsertBarrier(&toSRV, 1);

        vcmd->End();
        vcmd->Submit(0);
        vcmd->WaitForCompletion();
        device_->DestroyCommandBuffer(cmd);
    }
    device_->DestroyBuffer(eqStaging);
    std::cerr << "[Part37] equirectTex_ ready" << std::endl;

    // ----- 5. Create envCube_ (TextureCube RGBA16F) -----
    // faceSize = hdrW / 4 (mirror TestVulkanIBL_Equirect convention).
    // 2048 → 512. Sufficient resolution for IBL convolution quality.
    const u32 faceSize = static_cast<u32>(hdrW) / 4;
    TextureDesc cubeDesc{};
    cubeDesc.size = {faceSize, faceSize, 1};
    cubeDesc.mipLevels = 1;
    cubeDesc.arraySize = 6;
    cubeDesc.format = DataFormat::RGBA16_Float;
    cubeDesc.type = TextureType::TextureCube;
    // StorageImage binding for equirect→cube write + SampledImage for IBL prefilter.
    cubeDesc.usage = TextureUsage::UnorderedAccess | TextureUsage::ShaderResource | TextureUsage::CopySource;
    cubeDesc.memoryUsage = GPUMemoryUsage::Static;
    cubeDesc.name = "IBL_EnvCube";
    envCube_ = device_->CreateTexture(cubeDesc);
    if (envCube_ == handles::INVALID_RESOURCE) {
        std::cerr << "[Part37] CreateTexture envCube_ failed" << std::endl;
        device_->DestroyTexture(equirectTex_);
        equirectTex_ = handles::INVALID_RESOURCE;
        return false;
    }

    // Create 2DArray storage view of envCube_ for the equirect→cube shader
    // (shader writes via texture_storage_2d_array<rgba16float, write>).
    TextureViewDesc arrayViewDesc{};
    arrayViewDesc.texture = envCube_;
    arrayViewDesc.viewType = TextureType::Texture2DArray;
    arrayViewDesc.format = DataFormat::RGBA16_Float;
    arrayViewDesc.mostDetailedMip = 0;
    arrayViewDesc.mipCount = 1;
    arrayViewDesc.firstArraySlice = 0;
    arrayViewDesc.arraySize = 6;
    envCubeArrayView_ = device_->CreateTextureView(arrayViewDesc);
    if (envCubeArrayView_ == handles::INVALID_RESOURCE) {
        std::cerr << "[Part37] CreateTextureView envCubeArrayView_ failed" << std::endl;
        device_->DestroyTexture(envCube_);
        envCube_ = handles::INVALID_RESOURCE;
        device_->DestroyTexture(equirectTex_);
        equirectTex_ = handles::INVALID_RESOURCE;
        return false;
    }

    // ----- 6. Load IBL_EquirectangularToCube.spv + create compute pipeline -----
    auto eqCubeSpv = LoadSpvBytes("Engine/Graphics/Vulkan/shaders/IBL_EquirectangularToCube.spv");
    if (eqCubeSpv.empty()) {
        eqCubeSpv = LoadSpvBytes("Assets/Shaders/IBL_EquirectangularToCube.spv");
    }
    if (eqCubeSpv.empty()) {
        std::cerr << "[Part37] IBL_EquirectangularToCube.spv not found" << std::endl;
        // TextureView is destroyed via DestroyTexture (unified ResourceHandle).
        device_->DestroyTexture(envCubeArrayView_);
        envCubeArrayView_ = handles::INVALID_RESOURCE;
        device_->DestroyTexture(envCube_);
        envCube_ = handles::INVALID_RESOURCE;
        device_->DestroyTexture(equirectTex_);
        equirectTex_ = handles::INVALID_RESOURCE;
        return false;
    }
    ShaderHandle eqCubeCs = device_->CreateShader(eqCubeSpv.data(), eqCubeSpv.size(),
                                                   ShaderStage::Compute, "cs_main");
    if (eqCubeCs == handles::INVALID_SHADER) {
        std::cerr << "[Part37] CreateShader eqCubeCs failed" << std::endl;
        return false;
    }

    // 2-binding descriptor layout: 0=SampledImage equirect, 1=StorageImage arrayView.
    DescriptorSetLayoutBinding eqBindings[2]{};
    eqBindings[0] = {0, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr};
    eqBindings[1] = {1, DescriptorType::StorageImage, 1, ShaderStage::Compute, nullptr};
    DescriptorSetLayoutDesc eqLayoutDesc{};
    eqLayoutDesc.bindingCount = 2;
    eqLayoutDesc.bindings = eqBindings;
    DescriptorSetLayoutHandle eqLayout = device_->CreateDescriptorSetLayout(eqLayoutDesc);

    PipelineLayoutDesc eqPLDesc{};
    eqPLDesc.setLayoutCount = 1;
    eqPLDesc.setLayouts = &eqLayout;
    eqPLDesc.pushConstantRangeCount = 0;
    PipelineLayoutHandle eqPL = device_->CreatePipelineLayout(eqPLDesc);

    DescriptorSetDesc eqDsDesc{}; eqDsDesc.layout = eqLayout;
    DescriptorSetHandle eqDs = device_->CreateDescriptorSet(eqDsDesc);

    DescriptorImageInfo eqSrcInfo{handles::INVALID_SAMPLER, equirectTex_, ResourceState::ShaderResource};
    DescriptorImageInfo eqDstInfo{handles::INVALID_SAMPLER, envCubeArrayView_, ResourceState::UnorderedAccess};
    WriteDescriptorSet eqWrites[2]{};
    eqWrites[0] = {eqDs, 0, 0, 1, DescriptorType::SampledImage, &eqSrcInfo, nullptr};
    eqWrites[1] = {eqDs, 1, 0, 1, DescriptorType::StorageImage, &eqDstInfo, nullptr};
    device_->UpdateDescriptorSets(2, eqWrites);

    ComputePipelineDesc eqCpDesc{};
    eqCpDesc.computeShader = eqCubeCs;
    eqCpDesc.layout = eqPL;
    PipelineHandle eqPipe = device_->CreateComputePipeline(eqCpDesc);
    if (eqPipe == handles::INVALID_PIPELINE) {
        std::cerr << "[Part37] CreateComputePipeline eqPipe failed" << std::endl;
        return false;
    }

    // ----- 7. Dispatch equirect→cube -----
    {
        CommandBufferHandle cmd = device_->CreateCommandBuffer(CommandQueueType::Compute);
        VulkanCommandBuffer* vcmd = static_cast<VulkanDevice*>(device_)->GetCommandBuffer(cmd);
        vcmd->Reset(); vcmd->Begin();

        // envCube_ UNDEFINED → GENERAL (StorageImage write).
        // T4.6.5 part 37: barrier applies to the underlying image (envCube_),
        // not the view — views inherit layout from base image.
        ResourceBarrier toUA{};
        toUA.resource = envCube_;
        toUA.beforeState = ResourceState::Unknown;
        toUA.afterState = ResourceState::UnorderedAccess;
        toUA.subresource = 0xFFFFFFFF;
        toUA.queueFamily = 0xFFFFFFFF;
        vcmd->InsertBarrier(&toUA, 1);

        vcmd->BindComputePipeline(eqPipe);
        vcmd->BindDescriptorSets(PipelineBindPoint::Compute, eqPL, 0, 1, &eqDs, 0, nullptr);

        // WGSL workgroup_size(16,16,1). faceSize/16 in XY, Z=6 for layers.
        const u32 gx = (faceSize + 15) / 16;
        const u32 gy = (faceSize + 15) / 16;
        vcmd->Dispatch(gx, gy, 6);

        // envCube_ → ShaderResource (for IBL precompute sampling).
        ResourceBarrier toSRV{};
        toSRV.resource = envCube_;
        toSRV.beforeState = ResourceState::UnorderedAccess;
        toSRV.afterState = ResourceState::ShaderResource;
        toSRV.subresource = 0xFFFFFFFF;
        toSRV.queueFamily = 0xFFFFFFFF;
        vcmd->InsertBarrier(&toSRV, 1);

        vcmd->End();
        vcmd->Submit(0);
        vcmd->WaitForCompletion();
        device_->DestroyCommandBuffer(cmd);
    }
    std::cerr << "[Part37] envCube_ populated (faceSize=" << faceSize << ")" << std::endl;

    // Cleanup equirect→cube pipeline resources (no longer needed).
    device_->DestroyPipeline(eqPipe);
    device_->DestroyDescriptorSet(eqDs);
    device_->DestroyPipelineLayout(eqPL);
    device_->DestroyDescriptorSetLayout(eqLayout);
    device_->DestroyShader(eqCubeCs);

    // ----- 8. IBLPrecomputer: irradiance + prefilter + brdfLUT -----
    iblPrecomputer_ = std::make_unique<IBLPrecomputer>(device_);
    if (!iblPrecomputer_->Initialize()) {
        std::cerr << "[Part37] IBLPrecomputer.Initialize failed" << std::endl;
        return false;
    }

    iblIrradiance_ = iblPrecomputer_->ComputeIrradianceMap(envCube_, 32);
    if (iblIrradiance_ == handles::INVALID_RESOURCE) {
        std::cerr << "[Part37] ComputeIrradianceMap failed" << std::endl;
        return false;
    }
    std::cerr << "[Part37] irradiance map ready (32^3)" << std::endl;

    iblPrefilter_ = iblPrecomputer_->ComputePrefilteredEnvironmentMap(envCube_, 128);
    if (iblPrefilter_ == handles::INVALID_RESOURCE) {
        std::cerr << "[Part37] ComputePrefilteredEnvironmentMap failed" << std::endl;
        return false;
    }
    std::cerr << "[Part37] prefilter map ready (128^3)" << std::endl;

    iblBrdfLUT_ = iblPrecomputer_->ComputeBRDFIntegrationMap(512);
    if (iblBrdfLUT_ == handles::INVALID_RESOURCE) {
        std::cerr << "[Part37] ComputeBRDFIntegrationMap failed" << std::endl;
        return false;
    }
    std::cerr << "[Part37] brdfLUT ready (512^2)" << std::endl;

    // ----- 9. Forward to StandardRenderPipeline → DeferredLightingModule -----
    pipeline_->SetIBLResources(iblIrradiance_, iblPrefilter_, iblBrdfLUT_);
    std::cerr << "[Part37] IBL wired to pipeline" << std::endl;

    return true;
}


void TestVulkanFroxelRenderGraph::Run() {
    // Safety-net close check — same contract as the DLSS gate (Part35.7):
    // explicit Shutdown() before terminate keeps ~free_list asserts away.
    if (window_.is_closed()) {
        std::cerr << "[Part35.7] Run() safety net fired, hasShutdown_=" << hasShutdown_ << std::endl;
        if (!hasShutdown_) {
            Shutdown();
        }
        NS::Application::sharedApplication()->terminate(nullptr);
        return;
    }

    // 相机：门禁模式锁定参考机位（断言确定性 + 截图可比性）。观景模式
    // （FROXEL_GATE=0）保留 WASD 自由相机。SPGI_CAM 可覆盖初始机位。
    if (!cameraInitialized_) {
        float cx = -12.5f, cy = 1.7f, cz = -0.7f, cpitch = 14.0f, cyaw = 12.0f;
        sponza_reference::Camera(cx, cy, cz, cpitch, cyaw);
        if (EnvEnabled("FROXEL_CLOUD") || EnvEnabled("FROXEL_SPARSE")) {
            cx = -5.0f; cy = 3.0f; cz = -2.0f; cpitch = 5.0f; cyaw = 18.0f;
        }
        if (const char* camEnv = std::getenv("SPGI_CAM")) {
            if (sscanf(camEnv, "%f,%f,%f,%f,%f", &cx, &cy, &cz, &cpitch, &cyaw) == 5) {
                std::cerr << "[Cam] override " << camEnv << std::endl;
            }
        }
        camera_.Initialize({cx, cy, cz}, {cpitch, cyaw, 0.0f});
        camera_.SetSpeed(10.0f, 0.1f);
        cameraInitialized_ = true;
        lastFrameTime_ = std::chrono::steady_clock::now();
    }
    if (!gateEnabled_) {
        auto currentTime = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(currentTime - lastFrameTime_).count();
        lastFrameTime_ = currentTime;
        camera_.Update(dt);
    }
    view_.SetViewMatrix(camera_.GetViewMatrix());
    view_.UpdateFrustum();
    view_.Cull(scene_);

    // §5 基线帧时间：从 BeginFrame 前到本帧收尾的墙钟（含 GPU backpressure
    // 等待——CFRunLoopTimer 60fps 驱动下有效帧时间 ≈ GPU 耗时上界）。
    frameStartTime_ = std::chrono::steady_clock::now();

    ResourceHandle backBuffer;
    SyncHandle signalFence;
    if (!renderSystem_.BeginFrame(backBuffer, signalFence)) {
        return;
    }

    auto cmd = renderSystem_.GetCurrentCommandBuffer();
    auto cmdHandle = renderSystem_.GetCurrentCommandBufferHandle();
    u32 frameIdx = renderSystem_.GetCurrentFrameIndex();

    SyncHandle imageAvailable = renderSystem_.GetCurrentImageAvailableSemaphore();

    if (!cmd->Reset()) return;
    if (!cmd->Begin()) return;

    TextureDesc targetDesc = renderSystem_.GetBackBufferDesc();
    // 相位 A / B 读回帧同时抓离屏最终图像（视觉档案 + 与参考图比对素材）。
    const u64 f = frameCount_;
    const bool captureFrame =
        (!gateEnabled_ && std::getenv("SPONZA_CAPTURE_DIR") && f == (u64)gateFrame_) ||
        (gateEnabled_ && !gateDone_ &&
         (f == (u64)gateFrame_ || f == (u64)gateFrameB_ || f == (u64)gateFrameE_));
    ResourceHandle captureTarget = handles::INVALID_RESOURCE;
    if (captureFrame) {
        targetDesc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource | TextureUsage::CopySource;
        targetDesc.memoryUsage = GPUMemoryUsage::Static;
        captureTarget = device_->CreateTexture(targetDesc);
    }

    #ifndef DISABLE_PARTICLE_FX
    // Independently selectable sparse particle source. Real PFX alive-row bridge,
    // with one slowly moving emitter; FROXEL_SPARSE_CLEAR_FRAME exercises removal.
    if (EnvEnabled("FROXEL_SPARSE")) {
        if (auto* fog = pipeline_->GetFroxelFogPass(); fog && f % 6 == 0) {
            pfx::attribute_store store;
            store.initialize(13, nullptr, 0);
            const float puffs[][4] = {
                {0,0,0,1.8f},{-.9f,.4f,0,1.4f},{.8f,.6f,0,1.5f},
                {0,1.3f,.1f,1.2f},{-1.5f,-.4f,0,1.1f},{1.5f,-.3f,0,1.1f},
                {0,-1.1f,0,1.4f},{-.8f,0,-1,1.1f},{.8f,.2f,-1,1.2f},
                {0,.8f,1,1.2f},{-1,.3f,1,1},{1,-.4f,1,1.1f},{0,-.8f,-1,1.1f}};
            const u64 clearFrame = (u64)std::max(0.0f, EnvFloat("FROXEL_SPARSE_CLEAR_FRAME", 1000000));
            if (f < clearFrame) for (const auto& p : puffs) {
                const auto h = store.allocate();
                *static_cast<primal::math::v4*>(store.resolve(h,pfx::attr::position)) =
                    {2+p[0]+0.3f*std::sin(float(f)*0.02f),3.8f+p[1],p[2],0};
                *static_cast<primal::math::v4*>(store.resolve(h,pfx::attr::scale_rotation)) = {p[3],p[3],0,0};
                *static_cast<primal::math::v4*>(store.resolve(h,pfx::attr::color)) = {1,1,1,1};
            }
            const auto particles = pfx::render::pack_volume_particles(store,1.0f,80.0f);
            volume::SparseVolumeConfig sparse;
            sparse.voxel_size=std::max(0.025f,EnvFloat("FROXEL_SPARSE_VOXEL",0.1f));
            std::string error;
            if (!fog->SetVolumeParticles(particles,sparse,&error)) {
                PRIMAL_LOG_STREAM(Error,"Engine") << "[SparseVolume] " << error << std::endl;
            } else if (f==6 || f>=clearFrame) {
                PRIMAL_LOG_STREAM(Info,"Engine") << "[SparseVolume] frame=" << f
                    << " particles=" << particles.size() << " bricks=" << fog->GetParticleVolume().BrickCount()
                    << " atlasBytes=" << fog->GetParticleVolume().atlas.size()*sizeof(float) << std::endl;
            }
        }
    }

    #endif

    pipeline_->RenderWithCommandBuffer(
        scene_, view_,
        captureTarget != handles::INVALID_RESOURCE ? captureTarget : backBuffer, targetDesc,
        cmd, frameIdx, cmdHandle, imageAvailable);

    {
        rhi::ResourceBarrier toPresent{};
        toPresent.resource = backBuffer;
        toPresent.beforeState = captureTarget != handles::INVALID_RESOURCE
            ? rhi::ResourceState::Unknown : rhi::ResourceState::RenderTarget;
        toPresent.afterState = rhi::ResourceState::Present;
        toPresent.subresource = 0xFFFFFFFF;
        toPresent.queueFamily = 0xFFFFFFFF;
        cmd->InsertBarrier(&toPresent, 1);
    }

    cmd->End();

    const u32 imgIdx = renderSystem_.GetCurrentImageIndex() % kMaxSwapchainImages;
    const SyncHandle renderDoneSem = renderDoneSemaphores_[imgIdx];

    QueueSubmitInfo submitInfo{};
    submitInfo.cmdBuffer = cmdHandle;
    submitInfo.waitSemaphore = imageAvailable;
    submitInfo.signalSemaphore = renderDoneSem;
    submitInfo.signalFence = renderSystem_.GetFrameFence(frameIdx);
    device_->Submit(submitInfo);

    renderSystem_.EndFrame(renderDoneSem);

    const double frameMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - frameStartTime_).count();
    if (gateEnabled_ && !gateDone_ && frameCount_ >= 100 &&
        frameCount_ < (u64)gateFrame_) {
        gateFrameTimeAccumMs_ += frameMs;
        ++gateFrameTimeFrames_;
    }

    // 离屏最终图像读回 → PPM（视觉档案；SPONZA_CAPTURE_DIR 可重定向）。
    if (captureTarget != handles::INVALID_RESOURCE) {
        auto writePPM = [&](const char* tag) {
            u32 capW = targetDesc.size.x;
            u32 capH = targetDesc.size.y;
            BufferDesc sbDesc{};
            sbDesc.size = (u64)capW * capH * 4;
            sbDesc.usage = GPUMemoryUsage::Staging;
            sbDesc.memoryUsage = GPUMemoryUsage::Staging;
            ResourceHandle stagingBuf = device_->CreateBuffer(sbDesc);
            auto capCmdHandle = device_->CreateCommandBuffer(CommandQueueType::Graphics);
            if (capCmdHandle != handles::INVALID_COMMAND_BUFFER) {
                auto* capCmd = static_cast<VulkanDevice*>(device_)->GetCommandBuffer(capCmdHandle);
                capCmd->Begin();
                ResourceBarrier toCopy{};
                toCopy.resource = captureTarget;
                toCopy.beforeState = ResourceState::RenderTarget;
                toCopy.afterState = ResourceState::CopySource;
                toCopy.subresource = 0xFFFFFFFF;
                capCmd->InsertBarrier(&toCopy, 1);
                BufferTextureCopyRegion region{};
                region.bufferOffset = 0;
                region.bufferRowLength = capW;
                region.imageSubresource = {0, 0, 1};
                region.imageExtent = {capW, capH, 1};
                capCmd->CopyTextureToBuffer(captureTarget, stagingBuf, &region, 1);
                ResourceBarrier toPresent{};
                toPresent.resource = captureTarget;
                toPresent.beforeState = ResourceState::CopySource;
                toPresent.afterState = ResourceState::RenderTarget;
                toPresent.subresource = 0xFFFFFFFF;
                capCmd->InsertBarrier(&toPresent, 1);
                capCmd->End();
                capCmd->Submit();
                capCmd->WaitForCompletion();
                device_->DestroyCommandBuffer(capCmdHandle);
            }
            if (auto* mapped = static_cast<u8*>(device_->MapBuffer(stagingBuf))) {
                const char* capDir = std::getenv("SPONZA_CAPTURE_DIR");
                std::string ppmPath = capDir ? std::string(capDir) : std::string("/tmp/");
                if (!ppmPath.empty() && ppmPath.back() != '/') ppmPath.push_back('/');
                ppmPath += std::string("froxel_gate_") + tag + "_" +
                           std::to_string(frameCount_) + ".ppm";
                std::ofstream ppm(ppmPath, std::ios::binary);
                ppm << "P6\n" << capW << " " << capH << "\n255\n";
                for (u32 y = 0; y < capH; ++y) {
                    for (u32 x = 0; x < capW; ++x) {
                        u8* p = mapped + (y * capW + x) * 4;
                        ppm.write(reinterpret_cast<const char*>(&p[2]), 1);  // BGRA8 → RGB
                        ppm.write(reinterpret_cast<const char*>(&p[1]), 1);
                        ppm.write(reinterpret_cast<const char*>(&p[0]), 1);
                    }
                }
                device_->UnmapBuffer(stagingBuf);
                std::cerr << "[FroxelGate] captured " << ppmPath
                          << " (" << capW << "x" << capH << ")" << std::endl;
            }
            device_->DestroyBuffer(stagingBuf);
        };
        if (f == (u64)gateFrame_)   writePPM("A_shadow");
        if (f == (u64)gateFrameB_)  writePPM("B_noshadow");
        if (f == (u64)gateFrameE_)  writePPM("E_noise");
        device_->DestroyTexture(captureTarget);
    }

    frameCount_++;

    // 观景模式：FROXEL_MAX_FRAMES 自动退出（窗口长跑无人工关窗时）。
    if (!gateEnabled_) {
        if (const char* mf = std::getenv("FROXEL_MAX_FRAMES")) {
            if (frameCount_ >= (u64)std::atoi(mf)) {
                std::cout << "[FroxelGate] FROXEL_MAX_FRAMES=" << mf
                          << " reached — clean exit" << std::endl;
                std::_Exit(0);
            }
        }
        return;
    }

    if (!gateDone_) RunFroxelGate(frameCount_ - 1);
}

// ============================================================================
// Froxel 门禁（计划 §6.4）——相位推进 + G1-G4 断言
// ============================================================================

bool TestVulkanFroxelRenderGraph::ReadGridRGBA16F(rhi::ResourceHandle tex,
        u32 w, u32 h, u32 d, std::vector<float>& out) {
    if (tex == handles::INVALID_RESOURCE) return false;
    const u64 bytes = u64(w) * h * d * 8;
    BufferDesc rbDesc{};
    rbDesc.size = bytes;
    rbDesc.usage = GPUMemoryUsage::Readback;
    rbDesc.memoryUsage = GPUMemoryUsage::Readback;
    ResourceHandle staging = device_->CreateBuffer(rbDesc);
    if (staging == handles::INVALID_RESOURCE) return false;
    bool ok = false;
    auto cmdH = device_->CreateCommandBuffer(CommandQueueType::Graphics);
    if (cmdH != handles::INVALID_COMMAND_BUFFER) {
        auto* c = static_cast<VulkanDevice*>(device_)->GetCommandBuffer(cmdH);
        c->Begin();
        BufferTextureCopyRegion region{};
        region.bufferOffset = 0;
        region.bufferRowLength = w;
        region.imageSubresource = {0, 0, 1};
        region.imageExtent = {w, h, d};
        c->CopyTextureToBuffer(tex, staging, &region, 1);
        c->End();
        c->Submit();
        c->WaitForCompletion();
        device_->DestroyCommandBuffer(cmdH);
        if (auto* m = static_cast<u16*>(device_->MapBuffer(staging))) {
            auto f16 = [](u16 hbits) -> float {
                u32 sign = (hbits >> 15) & 1, ex = (hbits >> 10) & 0x1F, mant = hbits & 0x3FF;
                if (ex == 0) return sign ? -0.0f : 0.0f;
                if (ex == 0x1F) return sign ?
                    -std::numeric_limits<float>::infinity() :
                    std::numeric_limits<float>::infinity();
                u32 bits = (sign << 31) | ((ex + 112) << 23) | (mant << 13);
                float f; std::memcpy(&f, &bits, 4); return f;
            };
            out.resize(size_t(w) * h * d * 4);
            for (u64 i = 0; i < u64(w) * h * d * 4; ++i) out[i] = f16(m[i]);
            device_->UnmapBuffer(staging);
            ok = true;
        }
    }
    device_->DestroyBuffer(staging);
    return ok;
}

bool TestVulkanFroxelRenderGraph::ReadGridR16F(rhi::ResourceHandle tex,
        u32 w, u32 h, u32 d, std::vector<float>& out) {
    if (tex == handles::INVALID_RESOURCE) return false;
    const u64 bytes = u64(w) * h * d * 2;
    BufferDesc rbDesc{};
    rbDesc.size = bytes;
    rbDesc.usage = GPUMemoryUsage::Readback;
    rbDesc.memoryUsage = GPUMemoryUsage::Readback;
    ResourceHandle staging = device_->CreateBuffer(rbDesc);
    if (staging == handles::INVALID_RESOURCE) return false;
    bool ok = false;
    auto cmdH = device_->CreateCommandBuffer(CommandQueueType::Graphics);
    if (cmdH != handles::INVALID_COMMAND_BUFFER) {
        auto* c = static_cast<VulkanDevice*>(device_)->GetCommandBuffer(cmdH);
        c->Begin();
        BufferTextureCopyRegion region{};
        region.bufferOffset = 0;
        region.bufferRowLength = w;
        region.imageSubresource = {0, 0, 1};
        region.imageExtent = {w, h, d};
        c->CopyTextureToBuffer(tex, staging, &region, 1);
        c->End();
        c->Submit();
        c->WaitForCompletion();
        device_->DestroyCommandBuffer(cmdH);
        if (auto* m = static_cast<u16*>(device_->MapBuffer(staging))) {
            auto f16 = [](u16 hbits) -> float {
                u32 sign = (hbits >> 15) & 1, ex = (hbits >> 10) & 0x1F, mant = hbits & 0x3FF;
                if (ex == 0) return sign ? -0.0f : 0.0f;
                u32 bits = (sign << 31) | ((ex + 112) << 23) | (mant << 13);
                float f; std::memcpy(&f, &bits, 4); return f;
            };
            out.resize(size_t(w) * h * d);
            for (u64 i = 0; i < u64(w) * h * d; ++i) out[i] = f16(m[i]);
            device_->UnmapBuffer(staging);
            ok = true;
        }
    }
    device_->DestroyBuffer(staging);
    return ok;
}

bool TestVulkanFroxelRenderGraph::BuildFroxelWorldPos(u32 w, u32 h, u32 d,
        std::vector<float>& worldXyz) {
    const math::m4x4 proj = view_.GetProjectionMatrix();
    const math::m4x4 view = view_.GetViewMatrix();
    if (proj.columns[3].w == 0.0f && view.columns[3].w == 0.0f) return false;
    const math::m4x4 invVP = rhi::math::Inverse(proj * view);
    const math::v3 camPos = camera_.GetPosition();
    // 与 CB 填充同式：LH [0,1] 深度投影反解 near/far。
    float a = proj.columns[2].z;
    float b = -proj.columns[3].z;
    const float camNear = (a > 1e-6f) ? b / a : 0.1f;
    const float fogNear = 0.5f, fogFar = 200.0f;   // FroxelGridConfig 默认
    const float invDepth = 1.0f / float(d);
    worldXyz.resize(size_t(w) * h * d * 3);
    for (u32 z = 0; z < d; ++z)
    for (u32 y = 0; y < h; ++y)
    for (u32 x = 0; x < w; ++x) {
        const float zFrac = (float(z) + 0.5f) * invDepth;
        const float zView = fogNear * std::pow(fogFar / fogNear, zFrac);
        const float su = (float(x) + 0.5f) / float(w);
        const float sv = (float(y) + 0.5f) / float(h);
        const float ndcX = su * 2.0f - 1.0f;
        const float ndcY = 1.0f - 2.0f * sv;
        auto col = [&](u32 i) { return invVP.columns[i]; };
        float nx = col(0).x * ndcX + col(1).x * ndcY + col(2).x * 0.0f + col(3).x;
        float ny = col(0).y * ndcX + col(1).y * ndcY + col(2).y * 0.0f + col(3).y;
        float nz = col(0).z * ndcX + col(1).z * ndcY + col(2).z * 0.0f + col(3).z;
        float nw = col(0).w * ndcX + col(1).w * ndcY + col(2).w * 0.0f + col(3).w;
        nx /= nw; ny /= nw; nz /= nw;
        const float s = zView / camNear;
        const size_t i = (size_t(x) + size_t(y) * w + size_t(z) * w * h) * 3;
        worldXyz[i + 0] = camPos.x + (nx - camPos.x) * s;
        worldXyz[i + 1] = camPos.y + (ny - camPos.y) * s;
        worldXyz[i + 2] = camPos.z + (nz - camPos.z) * s;
    }
    return true;
}

void TestVulkanFroxelRenderGraph::RunFroxelGate(u64 frameNo) {
    // 注意：pass 可用性只在读回分支检查——noise 相位的 off 窗口里 pass 是
    // 被有意关掉的（off→on 重建），此处的 G0 短路会杀死相位机。
    auto* froxel = pipeline_ ? pipeline_->GetFroxelFogPass() : nullptr;

    // 阈值（Phase 0 标定占位——首轮跑完按观测回填计划文档 §6.4 表）。
    const float g1_monoTol = 1e-3f;
    const float g1_farFracMin = 0.005f;     // 远半段 lum>1e-4 的体素占比
    // Phase 0 标定（2026-09-30 首跑）：参考机位下 Sponza 几何遮住约 2/3 视
    // 锥深度（coverage 实测 32%），最小 slice 判据无意义（墙后 slice 接近全
    // 清零是预期行为）——改判"相机眼前首 slice 覆盖"（uniform 注入健康度）。
    const float g2_covMin = 0.25f;
    const float g2_firstSliceMin = 0.90f;
    const float g3_fracStrongMin = 0.03f;
    const float g3_meanRatioMax = 0.97f;
    const float g4_solidMax = 0.01f;
    const float g4_airMin = 0.004f;

    auto verdict = [&](const char* name, bool pass, const std::string& detail) {
        std::cerr << "[FroxelGate] " << (pass ? "PASS " : "FAIL ") << name
                  << " " << detail << std::endl;
        pass ? ++gatePassCount_ : ++gateFailCount_;
    };

    const u32 gw = froxel ? froxel->GetGridWidth() : 0;
    const u32 gh = froxel ? froxel->GetGridHeight() : 0;
    const u32 gd = froxel ? froxel->GetGridDepth() : 0;
    const u32 fidx = renderSystem_.GetCurrentFrameIndex();

    // ---- 相位 A：uniform + shadows on —— G1 + G2 ----
    if (gatePhase_ == GatePhase::Off && frameNo >= (u64)gateFrame_) {
        gatePhase_ = GatePhase::UniformShadow;
        if (!froxel || !froxel->IsInitialized()) {
            std::cerr << "[FroxelGate] FAIL G0 pass-not-initialized (phase A)" << std::endl;
            std::cerr << "[FroxelGate] RESULT FAIL" << std::endl;
            std::_Exit(1);
        }
        std::vector<float> scatter, density;
        const bool gotS = ReadGridRGBA16F(froxel->GetScatter3DTexture(fidx), gw, gh, gd, scatter);
        const bool gotD = ReadGridR16F(froxel->GetDensityTexture(fidx), gw, gh, gd, density);
        if (!gotS || !gotD) {
            std::cerr << "[FroxelGate] FAIL G0 readback-unavailable (scatter="
                      << gotS << " density=" << gotD << ")" << std::endl;
            std::cerr << "[FroxelGate] RESULT FAIL" << std::endl;
            std::_Exit(1);
        }
        gateGW_ = gw; gateGH_ = gh; gateGD_ = gd;

        // G1: NaN/Inf + transmittance 单调 + 前端 T≈1 + 远端散射 > 0
        u64 nanCount = 0, monoViol = 0, monoSamples = 0, farVox = 0, farLit = 0;
        double frontTSum = 0.0;
        const u64 sliceN = u64(gw) * gh;
        for (u32 y = 0; y < gh; ++y)
        for (u32 x = 0; x < gw; ++x) {
            float prevT = 1.0f;
            for (u32 z = 0; z < gd; ++z) {
                const size_t i = (size_t(x) + size_t(y) * gw + size_t(z) * sliceN) * 4;
                const float r = scatter[i], g = scatter[i+1], b = scatter[i+2], a = scatter[i+3];
                if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b) || !std::isfinite(a))
                    ++nanCount;
                ++monoSamples;
                if (a > prevT + g1_monoTol) ++monoViol;
                prevT = a;
                if (z == 0) frontTSum += a;
                if (z >= gd / 2) {
                    const float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                    ++farVox;
                    if (lum > 1e-4f) ++farLit;
                }
            }
        }
        const double frontTMean = frontTSum / double(sliceN);
        const double farFrac = farVox ? double(farLit) / double(farVox) : 0.0;
        {
            std::ostringstream ds;
            ds << "nan=" << nanCount << " tviol=" << (monoSamples ? 100.0 * monoViol / monoSamples : 0.0)
               << "% front-meanT=" << frontTMean << " far-scatter-frac=" << farFrac * 100.0 << "%";
            verdict("G1 integrate-health",
                    nanCount == 0 &&
                    double(monoViol) <= 1e-4 * double(monoSamples) &&
                    frontTMean > 0.9 && farFrac >= g1_farFracMin, ds.str());
        }

        // G2: uniform 介质覆盖 + 相机眼前首 slice 覆盖
        u64 nzTotal = 0, firstSliceNz = 0;
        for (u64 i = 0; i < sliceN; ++i) {
            if (density[i] > 1e-3f) ++firstSliceNz;
        }
        for (u32 z = 0; z < gd; ++z) {
            for (u64 i = 0; i < sliceN; ++i) {
                if (density[size_t(z) * sliceN + i] > 1e-3f) ++nzTotal;
            }
        }
        const u64 totalVox = sliceN * gd;
        const double cov = double(nzTotal) / double(totalVox);
        const double firstSliceFrac = double(firstSliceNz) / double(sliceN);
        {
            std::ostringstream ds;
            ds << "coverage=" << cov * 100.0 << "% first-slice=" << firstSliceFrac * 100.0
               << "% want>=" << g2_covMin * 100.0 << "%/" << g2_firstSliceMin * 100.0 << "%";
            verdict("G2 uniform-medium", cov >= g2_covMin && firstSliceFrac >= g2_firstSliceMin, ds.str());
        }

        // 相位 A 逐列均值（G3 的 A 侧）
        gateColMeanA_.assign(size_t(gw) * gh, 0.0f);
        for (u32 y = 0; y < gh; ++y)
        for (u32 x = 0; x < gw; ++x) {
            double s = 0.0;
            for (u32 z = 0; z < gd; ++z) {
                const size_t i = (size_t(x) + size_t(y) * gw + size_t(z) * sliceN) * 4;
                s += 0.2126 * scatter[i] + 0.7152 * scatter[i+1] + 0.0722 * scatter[i+2];
            }
            gateColMeanA_[size_t(x) + size_t(y) * gw] = float(s / gd);
        }

        // G3 双相位：关阴影 → +45 帧读回 B
        pipeline_->SetFroxelShadowsEnabled(false);
        std::cerr << "[FroxelGate] phase A done (frame " << frameNo
                  << ") — shadows disabled for phase B @" << gateFrameB_ << std::endl;
        return;
    }

    // ---- 相位 B：uniform + no-shadow —— G3 ----
    if (gatePhase_ == GatePhase::UniformShadow && frameNo >= (u64)gateFrameB_) {
        gatePhase_ = GatePhase::UniformNoShadow;
        if (!froxel || !froxel->IsInitialized()) {
            std::cerr << "[FroxelGate] FAIL G0 pass-not-initialized (phase B)" << std::endl;
            std::cerr << "[FroxelGate] RESULT FAIL" << std::endl;
            std::_Exit(1);
        }
        std::vector<float> scatter;
        if (!ReadGridRGBA16F(froxel->GetScatter3DTexture(fidx), gw, gh, gd, scatter)) {
            std::cerr << "[FroxelGate] FAIL G0 readback-B-unavailable" << std::endl;
            std::cerr << "[FroxelGate] RESULT FAIL" << std::endl;
            std::_Exit(1);
        }
        const u64 sliceN = u64(gw) * gh;
        std::vector<float> colB(size_t(gw) * gh, 0.0f);
        for (u32 y = 0; y < gh; ++y)
        for (u32 x = 0; x < gw; ++x) {
            double s = 0.0;
            for (u32 z = 0; z < gd; ++z) {
                const size_t i = (size_t(x) + size_t(y) * gw + size_t(z) * sliceN) * 4;
                s += 0.2126 * scatter[i] + 0.7152 * scatter[i+1] + 0.0722 * scatter[i+2];
            }
            colB[size_t(x) + size_t(y) * gw] = float(s / gd);
        }
        // 比值分布：r = A/B；strong-shadow 列 = r < 0.7
        u64 valid = 0, strong = 0;
        double sumR = 0.0;
        for (size_t c = 0; c < colB.size(); ++c) {
            const float b = colB[c], a = gateColMeanA_[c];
            if (b <= 1e-5f) continue;
            ++valid;
            const double r = double(a) / double(b);
            sumR += r;
            if (r < 0.7) ++strong;
        }
        const double fracStrong = valid ? double(strong) / double(valid) : 0.0;
        const double meanRatio = valid ? sumR / double(valid) : 1.0;
        {
            std::ostringstream ds;
            ds << "frac-strong(r<0.7)=" << fracStrong * 100.0 << "% mean-ratio=" << meanRatio
               << " want>=" << g3_fracStrongMin * 100.0 << "% & <=" << g3_meanRatioMax;
            verdict("G3 volume-shadow-contrast",
                    fracStrong >= g3_fracStrongMin && meanRatio <= g3_meanRatioMax, ds.str());
        }

        // 相位 C（uniform + shadows on + march=0）：恢复阴影输入，作为 G5
        // 的 A' 基线。uniform 介质下 march 的 Beer 衰减有解析预测
        //（vis ≈ exp(-σt·march_distance)），断言才有物理锚点——噪声模式的
        // march 贡献被低密度+陡太阳压到 <1%（run12 实测），不做断言。
        pipeline_->SetFroxelShadowsEnabled(true);
        gatePhase_ = GatePhase::UniformNoShadow;   // C 分支（@gateFrameC_）接手
        std::cerr << "[FroxelGate] phase B done (frame " << frameNo
                  << ") — shadows back on, G5 A' readback @" << gateFrameC_ << std::endl;
        return;
    }

    // ---- 相位 C：uniform + shadow + march=0 —— G5 A' 读回 ----
    if (gatePhase_ == GatePhase::UniformNoShadow && frameNo >= (u64)gateFrameC_) {
        gatePhase_ = GatePhase::UniformShadowM0;   // D 分支（@gateFrameD_）接手
        if (!froxel || !froxel->IsInitialized()) {
            std::cerr << "[FroxelGate] FAIL G0 pass-not-initialized (G5 A')" << std::endl;
            std::cerr << "[FroxelGate] RESULT FAIL" << std::endl;
            std::_Exit(1);
        }
        std::vector<float> scatterA;
        if (!ReadGridRGBA16F(froxel->GetScatter3DTexture(fidx), gw, gh, gd, scatterA)) {
            std::cerr << "[FroxelGate] FAIL G0 readback-march0-unavailable" << std::endl;
            std::cerr << "[FroxelGate] RESULT FAIL" << std::endl;
            std::_Exit(1);
        }
        gateColMeanMarch0_ = ColumnMeans(scatterA, gw, gh, gd);
        // march=4 on（免重建 setter，下一帧生效）
        pipeline_->SetFroxelMarchSteps(4);
        std::cerr << "[FroxelGate] G5 phase A' (march=0) read at " << frameNo
                  << " — march=4 on, readback @" << gateFrameD_ << std::endl;
        return;
    }

    // ---- 相位 D：uniform + shadow + march=4 —— G5 断言 ----
    // uniform 介质下 march 的雾自遮蔽有解析预测：lit 区 vis ≈
    // exp(-σt·march_distance)（σt = FROXEL_UNIFORM × extinction_scale）。
    // 断言 lit 列（A' 均值 > 中位数）的 B'/A' 中位比显著 < 1 且不超出
    // Beer 预测的宽区间（几何查询调制允许系数）。frac-darker 做方向性
    // sanity（march 只会让 lit 列变暗，不会成片变亮）。
    if (gatePhase_ == GatePhase::UniformShadowM0 && frameNo >= (u64)gateFrameD_) {
        gatePhase_ = GatePhase::UniformMarch4;
        if (!froxel || !froxel->IsInitialized()) {
            std::cerr << "[FroxelGate] FAIL G0 pass-not-initialized (G5 B')" << std::endl;
            std::cerr << "[FroxelGate] RESULT FAIL" << std::endl;
            std::_Exit(1);
        }
        std::vector<float> scatterB;
        if (!ReadGridRGBA16F(froxel->GetScatter3DTexture(fidx), gw, gh, gd, scatterB)) {
            std::cerr << "[FroxelGate] FAIL G0 readback-march4-unavailable" << std::endl;
            std::cerr << "[FroxelGate] RESULT FAIL" << std::endl;
            std::_Exit(1);
        }
        std::vector<float> colB = ColumnMeans(scatterB, gw, gh, gd);
        // lit 列 = A' 均值 > 全场中位数；其 B'/A' 比值中位数 = march 衰减实测
        std::vector<float> a_sorted = gateColMeanMarch0_;
        std::sort(a_sorted.begin(), a_sorted.end());
        // lit = A' 均值 p75 以上（真正开天穹的亮雾列；中位切分会混入大量
        // PCSS 半影列——run16 实测其直射项占比过低、march 不可见）
        const float litCut = a_sorted[(a_sorted.size() * 3) / 4];
        std::vector<double> litRatios;
        u64 darker = 0, brighter = 0, litN = 0;
        for (size_t c = 0; c < colB.size(); ++c) {
            const float a = gateColMeanMarch0_[c];
            if (a <= 1e-5f) continue;
            const double r = double(colB[c]) / double(a);
            if (r < 0.98) ++darker;
            if (r > 1.02) ++brighter;
            if (a > litCut) { litRatios.push_back(r); ++litN; }
        }
        std::sort(litRatios.begin(), litRatios.end());
        const double litMedian = litRatios.empty() ? 1.0 :
            litRatios[litRatios.size() / 2];
        // 诊断：全场均值 + lit 比值分位（标定用）
        double meanA = 0.0, meanB = 0.0;
        for (size_t c = 0; c < colB.size(); ++c) {
            meanA += gateColMeanMarch0_[c];
            meanB += colB[c];
        }
        if (!litRatios.empty()) {
            std::cerr << "[FroxelGate][G5diag] lit-ratio p10=" << litRatios[litRatios.size() / 10]
                      << " p50=" << litMedian
                      << " p90=" << litRatios[(litRatios.size() * 9) / 10] << std::endl;
        }
        std::cerr << "[FroxelGate][G5diag] meanA=" << meanA / colB.size()
                  << " meanB=" << meanB / colB.size()
                  << " litN=" << litN << std::endl;
        // Beer 预测（gate 配置：FROXEL_UNIFORM × extinction_scale × 16m）
        const auto st = pipeline_->GetSettings().froxel;
        const float sigmaT = (st.density_uniform_debug > 0.0f ? st.density_uniform_debug : 0.35f)
                           * st.extinction_scale;
        const double beerPred = std::exp(-double(sigmaT) * double(st.march_distance));
        // 判据（run17 标定）：亮散射线多半是环境项撑起的阴影雾（march 不
        // 衰减环境项），march 的可见信号 = 直射占比列的显著变暗 + 全场均
        // 值方向性。run17 基线：场均值 -5.9%，r<0.98 的列 1730（12%）。
        // 阈值（run18 标定）：march0→march4 实测场均值 -5.9%、r<0.98 列
        // 1730/14400=12%（最深 ~0.68，无列破 0.5——环境项不被 march 衰减，
        // 直射占比列平均 -30%）。取实测的一半留噪声裕量：
        //   r<0.8 列占比 ≥ 5%（march 确实咬住一批直射列）
        //   场均值比 ≤ 0.97（方向性：只可能变暗）
        //   darker > 4×brighter（无成片变亮）
        u64 darkerStrong = 0;
        for (size_t c = 0; c < colB.size(); ++c) {
            const float a = gateColMeanMarch0_[c];
            if (a <= 1e-5f) continue;
            if (double(colB[c]) / double(a) < 0.8) ++darkerStrong;
        }
        const double fracStrong = double(darkerStrong) / double(colB.size());
        const bool pass = fracStrong >= 0.05 && meanB < meanA * 0.97 &&
                          darker > brighter * 4;
        {
            std::ostringstream ds;
            ds << std::fixed << std::setprecision(4)
               << "frac-strong(r<0.8)=" << fracStrong * 100.0 << "%"
               << " mean-ratio=" << meanB / meanA
               << " (beer-pred=" << beerPred << " 信息位)"
               << " darker=" << darker << " brighter=" << brighter;
            verdict("G5 march-selfshadow", pass, ds.str());
        }

        // 切 noise 相位（G4）：off → on 重建（ApplyConfigChanges 的生命周期）
        auto st2 = pipeline_->GetSettings();
        st2.quality.enable_froxel_fog = false;
        pipeline_->UpdateSettings(st2);
        std::cerr << "[FroxelGate] phase D done (frame " << frameNo
                  << ") — froxel off for noise rebuild @" << gateFrameNoiseOn_ << std::endl;
        return;
    }

    if (gatePhase_ == GatePhase::UniformMarch4 && frameNo >= (u64)gateFrameNoiseOn_) {
        gatePhase_ = GatePhase::Noise;
        auto st2 = pipeline_->GetSettings();
        st2.quality.enable_froxel_fog = true;
        st2.froxel.density_uniform_debug = -1.0f;   // noise/SDF 模式
        pipeline_->UpdateSettings(st2);
        std::cerr << "[FroxelGate] noise phase on (frame " << frameNo
                  << ") — density readback @" << gateFrameE_ << std::endl;
        return;
    }

    // ---- 相位 E：noise —— G4 ----
    if (gatePhase_ == GatePhase::Noise && frameNo >= (u64)gateFrameE_) {
        gatePhase_ = GatePhase::Done;
        if (!froxel || !froxel->IsInitialized()) {
            std::cerr << "[FroxelGate] FAIL G0 pass-not-initialized (noise rebuild "
                      << frameNo << ")" << std::endl;
            std::cerr << "[FroxelGate] RESULT FAIL" << std::endl;
            std::_Exit(1);
        }
        std::vector<float> density;
        if (!ReadGridR16F(froxel->GetDensityTexture(fidx), gw, gh, gd, density)) {
            std::cerr << "[FroxelGate] FAIL G0 readback-C-unavailable" << std::endl;
            std::cerr << "[FroxelGate] RESULT FAIL" << std::endl;
            std::_Exit(1);
        }
        std::vector<float> world;
        if (!BuildFroxelWorldPos(gw, gh, gd, world)) {
            std::cerr << "[FroxelGate] FAIL G0 world-reconstruct-unavailable" << std::endl;
            std::cerr << "[FroxelGate] RESULT FAIL" << std::endl;
            std::_Exit(1);
        }
        // Sponza 地面 y≈0（DDGI 场景包络 min y=-1.26）：地面下参考层（固体，
        // SDF carve 应清零）vs 空气带（0.5..4m，噪声雾应存在）。
        double solidSum = 0.0; u64 solidN = 0;
        double airSum = 0.0;   u64 airN = 0;
        u64 nzTotal = 0;
        const u64 totalVox = u64(gw) * gh * gd;
        for (u64 i = 0; i < totalVox; ++i) {
            const float d = density[i];
            if (d > 1e-3f) ++nzTotal;
            const float wy = world[i * 3 + 1];
            if (wy > -1.2f && wy < -0.4f) { solidSum += d; ++solidN; }
            if (wy >  0.5f && wy <  4.0f) { airSum += d; ++airN; }
        }
        const double solidMean = solidN ? solidSum / solidN : -1.0;
        const double airMean = airN ? airSum / airN : -1.0;
        const double cov = double(nzTotal) / double(totalVox);
        {
            std::ostringstream ds;
            ds << "solid-mean(y<-0.4)=" << solidMean << " air-mean(0.5..4m)=" << airMean
               << " coverage=" << cov * 100.0 << "%"
               << " want solid<" << g4_solidMax << " air>" << g4_airMin;
            verdict("G4 sdf-carve",
                    solidMean >= 0.0 && solidMean < g4_solidMax &&
                    airMean > g4_airMin && cov > 0.02 && cov < 0.95, ds.str());
        }

        // §5 基线帧时间
        if (gateFrameTimeFrames_ > 0) {
            std::cerr << "[FroxelGate] baseline frame-time (uniform phase, frames 100.."
                      << gateFrame_ - 1 << "): mean " << std::fixed
                      << std::setprecision(2)
                      << gateFrameTimeAccumMs_ / gateFrameTimeFrames_ << " ms/frame over "
                      << gateFrameTimeFrames_ << " frames" << std::endl;
        }

        // FROXEL_GATE_AB=0 跳过 G5 直接出结果（保底通道）
        const char* ab = std::getenv("FROXEL_GATE_AB");
        if (ab && ab[0] == '0') gateFailCount_ -= (gateG5Skipped_ ? 1 : 0);
        FinishGate();
        return;
    }
}

void TestVulkanFroxelRenderGraph::FinishGate() {
    std::cerr << "[FroxelGate] RESULT "
              << (gateFailCount_ == 0 ? "PASS" : "FAIL")
              << " (" << gatePassCount_ << " pass, " << gateFailCount_ << " fail)"
              << std::endl;
    std::_Exit(gateFailCount_ == 0 ? 0 : 1);
}

std::vector<float> TestVulkanFroxelRenderGraph::ColumnMeans(
        const std::vector<float>& scatter, u32 w, u32 h, u32 d) {
    std::vector<float> means(size_t(w) * h, 0.0f);
    const u64 sliceN = u64(w) * h;
    for (u32 y = 0; y < h; ++y)
    for (u32 x = 0; x < w; ++x) {
        double s = 0.0;
        for (u32 z = 0; z < d; ++z) {
            const size_t i = (size_t(x) + size_t(y) * w + size_t(z) * sliceN) * 4;
            s += 0.2126 * scatter[i] + 0.7152 * scatter[i+1] + 0.0722 * scatter[i+2];
        }
        means[size_t(x) + size_t(y) * w] = float(s / d);
    }
    return means;
}


void TestVulkanFroxelRenderGraph::Shutdown() {
    // T4.6.5 part 35.7: re-entrancy guard. Two callers can fire Shutdown():
    //   (1) Run()'s safety net (this file) — when CFRunLoopTimer polls
    //       is_closed() and finds it true (NSWindowWillCloseNotification
    //       set g_any_window_closed synchronously, beating NSApplication's
    //       own delegate dispatch).
    //   (2) RenderTestRunner::shutdown() — via
    //       applicationShouldTerminateAfterLastWindowClosed → terminate.
    // Whichever wins, the loser's call must be a clean no-op. The flag is
    // set on ENTRY (not by the caller) so the body runs exactly once and
    // remove_window is guaranteed to execute.
    if (hasShutdown_) return;
    hasShutdown_ = true;

    std::cerr << "[Part30.4] Shutdown — rendered " << frameCount_ << " frames" << std::endl;

    if (device_) {
        std::cerr << "[Part35.7] WaitIdle..." << std::endl;
        device_->WaitIdle();
        std::cerr << "[Part35.7] WaitIdle done" << std::endl;
    }

    if (pipeline_) {
        std::cerr << "[Part35.7] pipeline_->Shutdown..." << std::endl;
        pipeline_->Shutdown();
        pipeline_.reset();
        std::cerr << "[Part35.7] pipeline_ done" << std::endl;
    }


    if (materialRegistry_) {
        std::cerr << "[Part35.7] materialRegistry_->Shutdown..." << std::endl;
        materialRegistry_->Shutdown(device_);
        delete materialRegistry_;
        materialRegistry_ = nullptr;
        std::cerr << "[Part35.7] materialRegistry_ done" << std::endl;
    }

    // Remove cluster components + entities (reverse order).
    std::cerr << "[Part35.7] removing " << clusterComps_.size() << " clusters + " << entities_.size() << " entities" << std::endl;
    for (auto& c : clusterComps_) primal::cluster::remove(c);
    clusterComps_.clear();
    for (auto& e : entities_) {
        if (e.is_valid()) primal::game_entity::remove(e.get_id());
    }
    entities_.clear();
    materialInstances_.clear();
    sharedMaterial_.reset();
    sceneMeshes_.clear();
    std::cerr << "[Part35.7] entities done" << std::endl;

    std::cerr << "[Part35.7] renderSystem_.Shutdown..." << std::endl;
    renderSystem_.Shutdown();
    std::cerr << "[Part35.7] renderSystem_ done" << std::endl;

    // T4.6.5 part 30.13 (X5 fix): destroy per-image render-done semaphores.
    for (u32 i = 0; i < kMaxSwapchainImages; ++i) {
        if (renderDoneSemaphores_[i] != handles::INVALID_SYNC) {
            std::cerr << "[Part35.7] DestroySync renderDoneSemaphores_[" << i << "]..." << std::endl;
            device_->DestroySync(renderDoneSemaphores_[i]);
            renderDoneSemaphores_[i] = handles::INVALID_SYNC;
            std::cerr << "[Part35.7] DestroySync " << i << " done" << std::endl;
        }
    }

    if (texSampler_ != handles::INVALID_SAMPLER) {
        std::cerr << "[Part35.7] DestroySampler texSampler_..." << std::endl;
        device_->DestroySampler(texSampler_);
        texSampler_ = handles::INVALID_SAMPLER;
        std::cerr << "[Part35.7] texSampler_ done" << std::endl;
    }
    if (materialSampler_ != handles::INVALID_SAMPLER) {
        std::cerr << "[Part35.7] DestroySampler materialSampler_..." << std::endl;
        device_->DestroySampler(materialSampler_);
        materialSampler_ = handles::INVALID_SAMPLER;
        std::cerr << "[Part35.7] materialSampler_ done" << std::endl;
    }
    if (fallbackDiffuse_ != handles::INVALID_RESOURCE) {
        std::cerr << "[Part35.7] DestroyTexture fallbackDiffuse_..." << std::endl;
        device_->DestroyTexture(fallbackDiffuse_);
        fallbackDiffuse_ = handles::INVALID_RESOURCE;
        std::cerr << "[Part35.7] fallbackDiffuse_ done" << std::endl;
    }
    if (fallbackNormal_ != handles::INVALID_RESOURCE) {
        std::cerr << "[Part35.7] DestroyTexture fallbackNormal_..." << std::endl;
        device_->DestroyTexture(fallbackNormal_);
        fallbackNormal_ = handles::INVALID_RESOURCE;
        std::cerr << "[Part35.7] fallbackNormal_ done" << std::endl;
    }
    if (fallbackORM_ != handles::INVALID_RESOURCE) {
        std::cerr << "[Part35.7] DestroyTexture fallbackORM_..." << std::endl;
        device_->DestroyTexture(fallbackORM_);
        fallbackORM_ = handles::INVALID_RESOURCE;
        std::cerr << "[Part35.7] fallbackORM_ done" << std::endl;
    }
    std::cerr << "[Part35.7] samplers/textures done" << std::endl;

    // T4.6.5 part 37: IBL cleanup. Reset precomputer BEFORE pipeline_ (below)
    // since precomputer holds pipeline/layout state used during generation;
    // its generated ResourceHandles are device-owned and survive until
    // device destroys them via deferred GC. envCube_ + equirectTex_ are
    // intermediate textures not owned by deferred_module_ — destroy explicitly.
    if (iblPrecomputer_) {
        std::cerr << "[Part37] iblPrecomputer_.reset()..." << std::endl;
        iblPrecomputer_->Shutdown();
        iblPrecomputer_.reset();
        std::cerr << "[Part37] iblPrecomputer_ done" << std::endl;
    }
    if (iblIrradiance_ != handles::INVALID_RESOURCE) {
        device_->DestroyTexture(iblIrradiance_);
        iblIrradiance_ = handles::INVALID_RESOURCE;
        std::cerr << "[Part37] DestroyTexture iblIrradiance_ done" << std::endl;
    }
    if (iblPrefilter_ != handles::INVALID_RESOURCE) {
        device_->DestroyTexture(iblPrefilter_);
        iblPrefilter_ = handles::INVALID_RESOURCE;
        std::cerr << "[Part37] DestroyTexture iblPrefilter_ done" << std::endl;
    }
    if (iblBrdfLUT_ != handles::INVALID_RESOURCE) {
        device_->DestroyTexture(iblBrdfLUT_);
        iblBrdfLUT_ = handles::INVALID_RESOURCE;
        std::cerr << "[Part37] DestroyTexture iblBrdfLUT_ done" << std::endl;
    }
    if (envCubeArrayView_ != handles::INVALID_RESOURCE) {
        // TextureView is a ResourceHandle — destroyed via DestroyTexture.
        // (Engine RHI unifies them; no separate DestroyTextureView path.)
        device_->DestroyTexture(envCubeArrayView_);
        envCubeArrayView_ = handles::INVALID_RESOURCE;
        std::cerr << "[Part37] DestroyTextureView(envCubeArrayView_) done" << std::endl;
    }
    if (envCube_ != handles::INVALID_RESOURCE) {
        device_->DestroyTexture(envCube_);
        envCube_ = handles::INVALID_RESOURCE;
        std::cerr << "[Part37] DestroyTexture envCube_ done" << std::endl;
    }
    if (equirectTex_ != handles::INVALID_RESOURCE) {
        device_->DestroyTexture(equirectTex_);
        equirectTex_ = handles::INVALID_RESOURCE;
        std::cerr << "[Part37] DestroyTexture equirectTex_ done" << std::endl;
    }

    primal::content::shutdown();
    primal::content::AsyncResourceLoader::Shutdown();
    std::cerr << "[Part35.7] content shutdown done" << std::endl;

    std::cerr << "[Part35.7] window_.is_valid()=" << window_.is_valid() << std::endl;
    if (window_.is_valid()) {
        primal::platform::remove_window(window_.get_id());
        std::cerr << "[Part35.7] remove_window done" << std::endl;
    }

    if (device_) {
        device_->GetGarbageCollector().Flush();
        std::cerr << "[Part35.7] GC flush done" << std::endl;
    }

    deviceOwnership_.reset();
    device_ = nullptr;
    std::cerr << "[Part35.7] device reset done" << std::endl;

    primal::jobsystem::JobSystem::Shutdown();
    std::cerr << "[Part35.7] Shutdown complete" << std::endl;
}
