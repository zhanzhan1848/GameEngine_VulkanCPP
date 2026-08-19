// T4.6.5 part 30.4 — TestVulkanSponzaRenderGraph.cpp
//
// First visible-window Vulkan Sponza render using the RenderSystem abstraction
// (mirrors TestNaniteStreamingPipeline's scaffolding). The RenderSystem owns
// triple-buffered command buffers + per-frame fences internally, eliminating the
// fence/command-buffer-in-use validation errors that the deleted
// TestVulkanStandardPipelineRender_Windowed test produced via manual sync.

#include "TestVulkanSponzaRenderGraph.h"

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

#define STBI_NO_THREAD_LOCALS
#include "stb_image.h"

#define NS_PRIVATE_IMPLEMENTATION
#include <AppKit/AppKit.hpp>

#include <iostream>
#include <cstring>
#include <fstream>
#include <vector>
#include <algorithm>

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
    : primal::test::RenderTestRunner(std::make_unique<TestVulkanSponzaRenderGraph>())
{}

TestVulkanSponzaRenderGraph::~TestVulkanSponzaRenderGraph() {
    Shutdown();
}

bool TestVulkanSponzaRenderGraph::Initialize() {
    std::cout << "[Part30.4] TestVulkanSponzaRenderGraph::Initialize" << std::endl;

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

    // T4.6.5 part 38: ParticleSystem emitter. Config-driven (no per-frame
    // emit() call); ParticleSystem::update(dt) advances the simulation, then
    // ForwardSceneRenderer::RenderParticlePass (Pass 4c) drains the frame pool.
    // Failure is non-fatal — test still renders Sponza without particles.
#ifndef DISABLE_PARTICLE_SYSTEM
    if (primal::particles::initialize()) {
        primal::particles::emitter_config cfg;
        cfg.max_particles   = 5000;
        cfg.spawn_rate      = 50.0f;            // particles/sec
        cfg.lifetime_min    = 1.5f;
        cfg.lifetime_max    = 3.0f;
        cfg.velocity_min    = primal::math::v3{-0.5f, 0.5f, -0.5f};
        cfg.velocity_max    = primal::math::v3{ 0.5f, 2.0f,  0.5f};
        cfg.color_start     = primal::math::v4{1.0f, 0.8f, 0.3f, 1.0f};  // warm yellow
        cfg.color_end       = primal::math::v4{0.9f, 0.2f, 0.1f, 0.0f};  // fade to red
        cfg.scale_min       = primal::math::v2{0.3f, 0.3f};
        cfg.scale_max       = primal::math::v2{0.8f, 0.8f};
        cfg.gravity         = primal::math::v3{0.0f, -3.0f, 0.0f};       // gentle fall
        cfg.drag            = 0.2f;
        cfg.blending        = primal::particles::blend_mode::additive;
        cfg.depth_write     = false;
        particleEmitter_    = primal::particles::create_emitter(cfg);
        if (particleEmitter_ != primal::particles::invalid_id) {
            particlesInitialized_ = true;
            std::cout << "[Part38] particle emitter created (spawn_rate=50/s, max=5000)" << std::endl;
        } else {
            std::cerr << "[Part38] create_emitter returned invalid_id" << std::endl;
        }
    } else {
        std::cerr << "[Part38] particles::initialize failed — continuing without particles" << std::endl;
    }
#endif

    std::cout << "[Part30.4] Initialization complete — window open, close it to exit."
              << std::endl;
    return true;
}

bool TestVulkanSponzaRenderGraph::InitializeDevice() {
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

bool TestVulkanSponzaRenderGraph::InitializeWindowAndRenderSystem() {
    primal::platform::window_init_info wi{};
    wi.caption = "Vulkan Sponza (RenderSystem)";
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

bool TestVulkanSponzaRenderGraph::InitializePipeline() {
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
    auto deferredFsBytes = loadSpv("Engine/Graphics/Vulkan/shaders/Forward/DeferredLighting.frag.spv");
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
    auto giGatherBytes = loadSpv("Engine/Graphics/Vulkan/shaders/Lumen/DDGIGIGather.comp.spv");
    if (!fusionIndirectBytes.empty())
        handles.fusion_indirect_ps = device_->CreateShader(
            fusionIndirectBytes.data(), fusionIndirectBytes.size(), ShaderStage::Pixel, "fusion_indirect");
    if (!fusionCompositeBytes.empty())
        handles.fusion_composite_ps = device_->CreateShader(
            fusionCompositeBytes.data(), fusionCompositeBytes.size(), ShaderStage::Pixel, "fusion_composite");
    if (!giGatherBytes.empty())
        handles.gi_gather = device_->CreateShader(
            giGatherBytes.data(), giGatherBytes.size(), ShaderStage::Compute, "ddgi_gi_gather");

    // SDF visualization fullscreen shader (toggled at runtime via F6).
    auto sdfVizBytes = loadSpv("Engine/Graphics/Vulkan/shaders/Debug/SDFVisualization.frag.spv");
    if (!sdfVizBytes.empty())
        handles.sdf_viz_ps = device_->CreateShader(
            sdfVizBytes.data(), sdfVizBytes.size(), ShaderStage::Pixel, "main");

    pipeline_->SetShaderHandles(handles);

    // Medium preset: SSAO + DDGI + Fusion. Fusion now outputs HDR (no tonemap),
    // FinalBlit does the single tonemap pass.
    lumen::LumenConfig lumenConfig{};
    lumenConfig.quality = lumen::LumenQualityPreset::Medium;
    pipeline_->SetLumenConfig(lumenConfig);

    // Enable SSR (Screen-Space Reflections) — traces reflection rays through the
    // HZB, temporally accumulates, upsamples with roughness attenuation, then
    // FusionComposite blends the reflection color additively into the scene.
    pipeline_->SetPassEnabled(RenderPassID::SSR, true);

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

bool TestVulkanSponzaRenderGraph::LoadSponzaScene() {
    const std::string baseDir =
        "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/assets/";
    // Try pipeline model (high-precision per-mesh SDF) first, fall back to old.
    std::string modelPath = baseDir + "Sponza.model";
    std::ifstream file(modelPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cerr << "[Part30.4] Failed to open " << modelPath << std::endl;
        return false;
    }
    std::streamsize modelSize = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<char> modelBuffer(static_cast<size_t>(modelSize));
    if (!file.read(modelBuffer.data(), modelSize)) {
        std::cerr << "[Part30.4] Failed to read Sponza.model" << std::endl;
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
              << " meshes from Sponza.model" << std::endl;

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
        decode(ResolveTexturePath(baseDir, meshInfo.normalTexturePath), decNormal[i]);
        decode(ResolveTexturePath(baseDir, meshInfo.ormTexturePath), decORM[i]);
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
        if (normalTex == handles::INVALID_RESOURCE) normalTex = fallbackNormal_;

        ResourceHandle ormTex = handles::INVALID_RESOURCE;
        if (decORM[i].valid) {
            ormTex = CreateTextureFromData(
                device_, decORM[i].width, decORM[i].height, decORM[i].data);
        }
        if (ormTex == handles::INVALID_RESOURCE) ormTex = fallbackORM_;

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
        if (meshInfo.mesh && meshInfo.mesh->IsValid()) {
            proxy.worldAABB = meshInfo.mesh->GetLocalAABB();
        }
        scene_.AddProxy(proxy);
    }
    std::cout << "[Part30.4] Textures: " << texLoaded << " loaded, "
              << texFailed << " fallback" << std::endl;
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
    gpuDraw.SetMaterialDataBuffer(materialRegistry_->GetMaterialDataBuffer());
    SamplerDesc texSamplerDesc{};
    texSamplerDesc.minFilter = FilterMode::Linear;
    texSamplerDesc.magFilter = FilterMode::Linear;
    texSamplerDesc.mipFilter = FilterMode::Linear;
    texSamplerDesc.addressU = TextureAddressMode::Wrap;
    texSamplerDesc.addressV = TextureAddressMode::Wrap;
    texSamplerDesc.addressW = TextureAddressMode::Wrap;
    texSamplerDesc.maxAnisotropy = 1;
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

    // Load offline per-mesh SDF from pipeline model for SDF visualization.
    // TEMPORARILY DISABLED: SDF merge is very slow (~5 min). SSR/SSGI don't need it.
    if (false) {
        offlineSDFMerger_ = new primal::graphics::nanite::OfflineSDFMerger();
        std::string pipeModelPath = baseDir + "Sponza_pipeline.model";
        u32 sdfCount = offlineSDFMerger_->LoadFromPipelineModel(pipeModelPath.c_str());
        if (sdfCount > 0) {
            offlineSDFMerger_->BuildAndUpload(device_);
            std::cerr << "[Part30.4] Offline SDF loaded: " << sdfCount
                      << " meshes, texture=" << offlineSDFMerger_->IsValid() << std::endl;
        } else {
            std::cerr << "[Part30.4] No offline SDF data found (pipeline model missing?)" << std::endl;
            delete offlineSDFMerger_;
            offlineSDFMerger_ = nullptr;
        }
    }

    // Wire offline SDF into the render pipeline for F6 visualization.
    if (offlineSDFMerger_ && offlineSDFMerger_->IsValid()) {
        pipeline_->SetOfflineSDFSource(
            offlineSDFMerger_->GetTexture(),
            offlineSDFMerger_->GetOrigin(),
            offlineSDFMerger_->GetExtent(),
            offlineSDFMerger_->GetResolution());
    }

    // Directional light + camera (TestDawnForwardRenderer defaults).
    RenderLight sunLight;
    sunLight.type = LightType::Directional;
    sunLight.direction = v3{0.5f, -0.7f, 0.3f};
    sunLight.color = v3{1.0f, 0.95f, 0.9f};
    sunLight.intensity = 3.0f;
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
        60.0f * rhimath::constants::DEG_TO_RAD,
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
bool TestVulkanSponzaRenderGraph::InitializeIBL() {
    // ----- 1. Locate sunset.hdr -----
    const char* kHdrPaths[] = {
        "EngineTest/assets/textures/hdr/sunset.hdr",
        "textures/hdr/sunset.hdr",
        "Assets/Textures/HDR/sunset.hdr",
        "sunset.hdr",
    };
    std::string hdrPath;
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

    // ----- 2. stbi_loadf → RGBA32F -----
    int hdrW, hdrH, hdrC;
    float* hdrData = stbi_loadf(hdrPath.c_str(), &hdrW, &hdrH, &hdrC, 4);
    if (!hdrData) {
        std::cerr << "[Part37] stbi_loadf failed: " << hdrPath << std::endl;
        return false;
    }
    std::cerr << "[Part37] HDR loaded: " << hdrW << "x" << hdrH << std::endl;

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
    stbi_image_free(hdrData);

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

void TestVulkanSponzaRenderGraph::Run() {
    // T4.6.5 part 30.4: safety-net close check. applicationShouldTerminateAfterLastWindowClosed
    // is the primary path, but some edge cases (e.g. window never ordered front) skip it.
    if (window_.is_closed()) {
        // T4.6.5 part 35.7: NSWindowWillCloseNotification sets g_any_window_closed
        // synchronously, but NSApplication only processes applicationShouldTerminate
        // AfterLastWindowClosed on its own event pass — which races with this timer
        // callback. If terminate fires first, exit() runs static destructors without
        // ever calling Shutdown(), leaking the window_info free_list slot and tripping
        // ~free_list's !_size assert. Call Shutdown() explicitly here.
        std::cerr << "[Part35.7] Run() safety net fired, hasShutdown_=" << hasShutdown_ << std::endl;
        // T4.6.5 part 35.7 fixup: do NOT pre-set hasShutdown_ here. Shutdown()
        // manages the flag itself — pre-setting makes Shutdown()'s own guard
        // (line below: `if (hasShutdown_) return;`) skip the entire body,
        // including remove_window. Result: window_info slot leaks, ~free_list
        // asserts at static destruction. Just call Shutdown(); it sets the
        // flag on entry, so re-entry from applicationShouldTerminateAfter-
        // LastWindowClosed's shutdown() delegate is a clean no-op.
        if (!hasShutdown_) {
            Shutdown();
        }
        NS::Application::sharedApplication()->terminate(nullptr);
        return;
    }

    // T4.6.5 part 31: WASD + right-mouse-look camera. Lazy-init on first frame
    // to the orientation LoadSponzaScene originally hard-coded (cameraPos
    // {0,5,-10}, looking +Z and slightly down). RHICamera's forward formula
    // {cos(yaw)*cos(pitch), sin(pitch), sin(yaw)*cos(pitch)} matches the test's
    // {-sin(yaw)*cos(pitch), ..., -cos(yaw)*cos(pitch)} when RHICamera yaw = 90°
    // and Sponza test yaw = 180°. Pitch -16.67° = -0.291 rad. Per-frame dt is
    // 1/60 since RenderTestRunner fires Run() at 60 FPS via CFRunLoopTimer.
    if (!cameraInitialized_) {
        // Match Metal TestSponzaRenderGraph init: position {0,5,0}, rotation {0,0,0}.
        camera_.Initialize({0.0f, 5.0f, 0.0f}, {0.0f, 0.0f, 0.0f});
        camera_.SetSpeed(10.0f, 0.1f);
        cameraInitialized_ = true;
        lastFrameTime_ = std::chrono::steady_clock::now();
    }
    // Measure real dt like Metal TestSponzaRenderGraph (not hardcoded 1/60).
    auto currentTime = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(currentTime - lastFrameTime_).count();
    lastFrameTime_ = currentTime;

    // Arrow key camera orientation (test-local, not in core RHICamera).
    // Left/Right = yaw, Up/Down = pitch. Complements WASD move + right-mouse-look.
    {
        using namespace primal::input;
        input_value arrow;
        const float lookSpeed = 60.0f; // deg/sec
        float yawDelta = 0.0f, pitchDelta = 0.0f;
        get(input_source::keyboard, input_code::key_left, arrow);
        if (arrow.current.x > 0.0f) yawDelta -= lookSpeed * dt;
        get(input_source::keyboard, input_code::key_right, arrow);
        if (arrow.current.x > 0.0f) yawDelta += lookSpeed * dt;
        get(input_source::keyboard, input_code::key_up, arrow);
        if (arrow.current.x > 0.0f) pitchDelta += lookSpeed * dt;
        get(input_source::keyboard, input_code::key_down, arrow);
        if (arrow.current.x > 0.0f) pitchDelta -= lookSpeed * dt;
        if (yawDelta != 0.0f || pitchDelta != 0.0f) {
            auto rot = camera_.GetRotation();
            rot.y += yawDelta;
            rot.x += pitchDelta;
            if (rot.x > 89.0f) rot.x = 89.0f;
            if (rot.x < -89.0f) rot.x = -89.0f;
            camera_.Initialize(camera_.GetPosition(), rot);
        }
    }

    // F6: toggle GlobalSDF fullscreen visualization (ray-march against cascades).
    {
        using namespace primal::input;
        input_value f6;
        get(input_source::keyboard, input_code::key_f6, f6);
        if (f6.current.x > 0.0f && f6.previous.x == 0.0f) {
            bool enabled = !pipeline_->IsSDFVisualizationEnabled();
            pipeline_->SetSDFVisualization(enabled);
            std::cout << "[SDF_VIZ] GlobalSDF visualization: "
                      << (enabled ? "ON" : "OFF") << std::endl;
        }
    }

    // R: reset camera + TAA history. The view jump invalidates TAA's temporal
    // reprojection — without the reset the stale history ghosts for ~10 frames
    // (alpha 0.1 convergence).
    {
        using namespace primal::input;
        input_value r;
        get(input_source::keyboard, input_code::key_r, r);
        if (r.current.x > 0.0f && r.previous.x == 0.0f) {
            camera_.Initialize({0.0f, 5.0f, 0.0f}, {0.0f, 0.0f, 0.0f});
            PostProcess::ResetTAAHistory();
            std::cout << "[TAA] Camera reset + TAA history cleared" << std::endl;
        }
    }

    // F7: cycle GeometryDebug overlay modes (Off → Meshlet → SDF slice → …).
    // Rendered by StandardRenderPipeline Step 10.7 via AddGeometryDebugPass.
    {
        using namespace primal::input;
        input_value f7;
        get(input_source::keyboard, input_code::key_f7, f7);
        if (f7.current.x > 0.0f && f7.previous.x == 0.0f && pipeline_) {
            static u32 debugCycle = 0;
            debugCycle = (debugCycle + 1) % 5;

            primal::graphics::GeometryDebugSettings dbg{};
            switch (debugCycle) {
            case 0: dbg.enable = false; break;                       // Off
            case 1: dbg.enable = true;  dbg.visualize_meshlets = true; dbg.wireframe = true; break;
            case 2: dbg.enable = true;  dbg.visualize_sdf = true; break;
            case 3: dbg.enable = true;  dbg.visualize_voxels = true; break;
            case 4: dbg.enable = true;  dbg.visualize_vector_field = true; break;
            }
            pipeline_->SetGeometryDebugSettings(dbg);
            std::cout << "[GeometryDebug] cycle=" << debugCycle
                      << " enable=" << dbg.enable
                      << " (meshlets=" << dbg.visualize_meshlets
                      << " sdf=" << dbg.visualize_sdf
                      << " voxels=" << dbg.visualize_voxels
                      << " vf=" << dbg.visualize_vector_field << ")" << std::endl;
        }
    }

    // F8: toggle Toon cel-shading (StandardRenderPipeline Step 10.6).
    {
        using namespace primal::input;
        input_value f8;
        get(input_source::keyboard, input_code::key_f8, f8);
        if (f8.current.x > 0.0f && f8.previous.x == 0.0f && pipeline_) {
            bool enabled = !pipeline_->IsToonEnabled();
            pipeline_->SetToonEnabled(enabled);
            std::cout << "[Toon] cel-shading: " << (enabled ? "ON" : "OFF") << std::endl;
        }
    }

    // F9: toggle VSM (moments + Chebyshev) vs PCSS (R8 visibility) shadows.
    {
        using namespace primal::input;
        input_value f9;
        get(input_source::keyboard, input_code::key_f9, f9);
        if (f9.current.x > 0.0f && f9.previous.x == 0.0f && pipeline_) {
            bool enabled = !pipeline_->IsVSMShadows();
            pipeline_->SetVSMShadows(enabled);
            std::cout << "[Shadow] VSM: " << (enabled ? "ON" : "OFF (PCSS)") << std::endl;
        }
    }

    // F10: toggle TAA sub-pixel jitter (A/B the temporal resolve).
    {
        using namespace primal::input;
        input_value f10;
        get(input_source::keyboard, input_code::key_f10, f10);
        if (f10.current.x > 0.0f && f10.previous.x == 0.0f && pipeline_) {
            bool on = !pipeline_->IsTAAJitter();
            pipeline_->SetTAAJitter(on);
            std::cout << "[TAA] jitter: " << (on ? "ON" : "OFF") << std::endl;
        }
    }

    // F11: toggle the planar-reflection mirror (StandardRenderPipeline
    // Step 10.45 — reflection rendered from the mirrored camera, mirror quad
    // composited over HDR before TAA).
    {
        using namespace primal::input;
        input_value f11;
        get(input_source::keyboard, input_code::key_f11, f11);
        if (f11.current.x > 0.0f && f11.previous.x == 0.0f && pipeline_) {
            bool enabled = !pipeline_->IsMirrorEnabled();
            pipeline_->SetMirrorEnabled(enabled);
            std::cout << "[Mirror] planar reflection: " << (enabled ? "ON" : "OFF") << std::endl;
        }
    }

    camera_.Update(dt);
    view_.SetViewMatrix(camera_.GetViewMatrix());
    view_.UpdateFrustum();
    view_.Cull(scene_);

    // T4.6.5 part 38: advance particle simulation each frame. dt=1/60 matches
    // the CFRunLoopTimer cadence. ForwardSceneRenderer::Render → Pass 4c drains
    // the per-frame pool that ParticlePass::execute reads via get_frame_pool.
#ifndef DISABLE_PARTICLE_SYSTEM
    if (particlesInitialized_) {
        primal::particles::update(1.0f / 60.0f);
    }
#endif

    // T4.6.5 part 38: debug line grid + RGB axes around origin. Triggers
    // ForwardSceneRenderer Pass 6 (LineBatchRenderer) via debug_draw::drain_into.
    // Camera {0,5,-10} looking +Z sees the grid edge-on at floor level + axes
    // sticking up — sufficient to verify Pass 6 fires per frame.
    {
        constexpr float kGridExtent = 5.0f;
        constexpr float kGridStep   = 1.0f;
        for (float x = -kGridExtent; x <= kGridExtent; x += kGridStep) {
            primal::graphics::debug_draw::add_line(
                x, 0.0f, -kGridExtent, x, 0.0f, kGridExtent, 0x404040);
        }
        for (float z = -kGridExtent; z <= kGridExtent; z += kGridStep) {
            primal::graphics::debug_draw::add_line(
                -kGridExtent, 0.0f, z, kGridExtent, 0.0f, z, 0x404040);
        }
        // RGB world axes (length 3.0).
        primal::graphics::debug_draw::add_line(0,0,0, 3,0,0, 0xFF0000); // +X red
        primal::graphics::debug_draw::add_line(0,0,0, 0,3,0, 0x00FF00); // +Y green
        primal::graphics::debug_draw::add_line(0,0,0, 0,0,3, 0x0000FF); // +Z blue
    }

    ResourceHandle backBuffer;
    SyncHandle signalFence;
    if (!renderSystem_.BeginFrame(backBuffer, signalFence)) {
        return;
    }

    auto cmd = renderSystem_.GetCurrentCommandBuffer();
    auto cmdHandle = renderSystem_.GetCurrentCommandBufferHandle();
    u32 frameIdx = renderSystem_.GetCurrentFrameIndex();

    // T4.6.5 part 30.14: BeginFrame's 2nd param is the CPU-GPU fence, NOT the
    // GPU-GPU acquire semaphore. The acquire semaphore is exposed separately.
    // Submit MUST wait on it, otherwise vkAcquireNextImageKHR's signal op has
    // no consumer → VUID-vkQueueSubmit-pWaitSemaphores-03238.
    SyncHandle imageAvailable = renderSystem_.GetCurrentImageAvailableSemaphore();

    if (!cmd->Reset()) return;
    if (!cmd->Begin()) return;

    TextureDesc targetDesc = renderSystem_.GetBackBufferDesc();

    pipeline_->RenderWithCommandBuffer(
        scene_, view_,
        backBuffer, targetDesc,
        cmd, frameIdx, cmdHandle, imageAvailable);

    // T4.6.5 part 30.6 (X3 fix): StandardRenderPipeline's FinalBlit pass leaves
    // the backbuffer in COLOR_ATTACHMENT_OPTIMAL. Vulkan spec requires
    // PRESENT_SRC_KHR at Present time (VUID-VkPresentInfoKHR-pImageIndices-01430).
    // Insert explicit transition before Submit.
    {
        rhi::ResourceBarrier toPresent{};
        toPresent.resource = backBuffer;
        toPresent.beforeState = rhi::ResourceState::RenderTarget;
        toPresent.afterState = rhi::ResourceState::Present;
        toPresent.subresource = 0xFFFFFFFF;
        toPresent.queueFamily = 0xFFFFFFFF;
        cmd->InsertBarrier(&toPresent, 1);
    }

    cmd->End();

    // T4.6.5 part 30.13 (X5 fix): pick render-done semaphore by acquired
    // image index. FIFO Present keeps the semaphore in flight until that
    // specific image is re-acquired. Indexing by frameIdx would race when
    // frame N+3 acquires a different image than frame N presented.
    const u32 imgIdx = renderSystem_.GetCurrentImageIndex() % kMaxSwapchainImages;
    const SyncHandle renderDoneSem = renderDoneSemaphores_[imgIdx];

    QueueSubmitInfo submitInfo{};
    submitInfo.cmdBuffer = cmdHandle;
    submitInfo.waitSemaphore = imageAvailable;
    submitInfo.signalSemaphore = renderDoneSem;
    submitInfo.signalFence = renderSystem_.GetFrameFence(frameIdx);
    device_->Submit(submitInfo);

    renderSystem_.EndFrame(renderDoneSem);

    // Backbuffer PPM capture — pixel-exact frames for temporal-stability
    // analysis. Fires at frames 60/61 (baseline, jitter ON) and for two
    // adjacent frames after an F10 jitter toggle (A/B without window-scaling
    // artifacts). Runs AFTER Present via a dedicated one-shot command buffer.
    auto capturePPM = [&](const char* tag) {
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

            // Swapchain image is in PRESENT_SRC after EndFrame. Transition to
            // CopySource so we can read it back.
            ResourceBarrier toCopy{};
            toCopy.resource = backBuffer;
            toCopy.beforeState = ResourceState::Present;
            toCopy.afterState = ResourceState::CopySource;
            toCopy.subresource = 0xFFFFFFFF;
            capCmd->InsertBarrier(&toCopy, 1);

            BufferTextureCopyRegion region{};
            region.bufferOffset = 0;
            region.bufferRowLength = capW;
            region.imageSubresource = {0, 0, 1};
            region.imageExtent = {capW, capH, 1};
            capCmd->CopyTextureToBuffer(backBuffer, stagingBuf, &region, 1);

            ResourceBarrier toPresent{};
            toPresent.resource = backBuffer;
            toPresent.beforeState = ResourceState::CopySource;
            toPresent.afterState = ResourceState::Present;
            toPresent.subresource = 0xFFFFFFFF;
            capCmd->InsertBarrier(&toPresent, 1);

            capCmd->End();
            capCmd->Submit();
            capCmd->WaitForCompletion();
            device_->DestroyCommandBuffer(capCmdHandle);
        }

        auto* mapped = static_cast<u8*>(device_->MapBuffer(stagingBuf));
        if (mapped) {
            std::string ppmPath = "/tmp/sponza_" + std::string(tag) + "_" +
                                  std::to_string(frameCount_) + ".ppm";
            std::ofstream ppm(ppmPath, std::ios::binary);
            ppm << "P6\n" << capW << " " << capH << "\n255\n";
            for (u32 y = 0; y < capH; ++y) {
                for (u32 x = 0; x < capW; ++x) {
                    u8* p = mapped + (y * capW + x) * 4;
                    // BGRA8 -> RGB
                    ppm.write(reinterpret_cast<const char*>(&p[2]), 1);
                    ppm.write(reinterpret_cast<const char*>(&p[1]), 1);
                    ppm.write(reinterpret_cast<const char*>(&p[0]), 1);
                }
            }
            device_->UnmapBuffer(stagingBuf);
            std::cerr << "[DEBUG] Frame " << frameCount_ << " captured to " << ppmPath
                      << " (" << capW << "x" << capH << ")" << std::endl;
        }
        device_->DestroyBuffer(stagingBuf);
    };

    if (frameCount_ == 60 || frameCount_ == 61) {
        capturePPM("jit_on");
    }



    frameCount_++;
}

void TestVulkanSponzaRenderGraph::Shutdown() {
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

    // T4.6.5 part 38: shutdown particle system AFTER pipeline (which consumes
    // it in Pass 4c) but before device teardown. destroy_emitter happens
    // implicitly inside shutdown(). Idempotent — safe to call even if init
    // failed.
#ifndef DISABLE_PARTICLE_SYSTEM
    if (particlesInitialized_) {
        std::cerr << "[Part38] particles::shutdown..." << std::endl;
        primal::particles::shutdown();
        particlesInitialized_ = false;
        particleEmitter_ = primal::particles::invalid_id;
        std::cerr << "[Part38] particles::shutdown done" << std::endl;
    }
#endif

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
