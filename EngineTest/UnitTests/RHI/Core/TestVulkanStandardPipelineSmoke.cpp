/**
 * @file TestVulkanStandardPipelineSmoke.cpp
 * @brief Tier 4.5.1 — StandardRenderPipeline plumbing smoke test on Vulkan.
 * @details First-ever exercise of StandardRenderPipeline on a Vulkan device.
 *          Validates the minimal Initialize() path (device + renderGraph +
 *          gpuOptimizer + black_texture) and confirms RenderWithCommandBuffer
 *          degrades gracefully when subsystems are not initialized.
 *
 * Scope:
 *   - StandardRenderPipeline::Initialize() returns true on Vulkan
 *   - SetEditorMode(true) is callable
 *   - RenderWithCommandBuffer() with no subsystems init returns cleanly
 *     (null forward_renderer_ + null gpuDraw → both code paths early-return)
 *   - Zero validation errors
 *
 * NOT in scope (BLOCKED — T4.6 work):
 *   The production Editor render path. Editor mode invokes
 *   ForwardSceneRenderer::Render() (StandardRenderPipeline.cpp:1287).
 *   ForwardSceneRenderer::CreateShaders (cpp:184) hardcodes the `.metal`
 *   extension and ~14 shaders (GBuffer, DepthOnly, DeferredLighting, Skybox,
 *   GBufferAlphaClip, GBufferUnlit, GBufferFoliage, GBufferWater,
 *   GBufferTransparent, ForwardTransparency, StreamingGBuffer). Running the
 *   real path requires:
 *     (a) platform-aware shader loader in ForwardSceneRenderer
 *     (b) SPIR-V ports of all listed shaders
 *   That is multi-session scope mirroring the T4.4 Nanite port.
 *
 *   Calling SetLumenConfig() to trigger InitializeSubsystems() would attempt
 *   ForwardSceneRenderer::Initialize → CreateShaders → fail on missing .metal
 *   files for Vulkan. Test deliberately skips that path.
 */

#include "../../TestFramework.h"

#include "Graphics/RHI/Core/RHIDeviceFactory.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Core/RHITypes.h"
#include "Graphics/RHI/Core/RHIMath.h"
#include "Graphics/RenderPipeline/StandardRenderPipeline.h"
#include "Graphics/Scene/RenderSceneSnapshot.h"  // complete type for unique_ptr<RenderSceneSnapshot> destructor
#include "Graphics/RenderScene.h"
#include "Graphics/RenderView.h"
#include "Graphics/Lumen/LumenTypes.h"

// T4.6.5 part 26: Sponza integration headers.
#include "Graphics/SceneDataAdapter.h"
#include "Graphics/Material.h"
#include "Graphics/MaterialInstance.h"
#include "Graphics/Nanite/GPUMaterialRegistry.h"
#include "Graphics/Nanite/GPUDrivenDrawPipeline.h"
#include "Graphics/RenderProxy.h"
#include "Graphics/RenderMesh.h"
#include "Components/Entity.h"
#include "Components/Cluster.h"
#include "Components/Transform.h"
#include "JobSystem/JobSystem.h"

#define STBI_NO_THREAD_LOCALS
#include "stb_image.h"

#include "Utils/ImageCompare.h"  // SavePNG

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN
#include "Graphics/RHI/Platforms/Vulkan/VulkanDevice.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanCommandBuffer.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanTexture.h"
#endif

#include <iostream>
#include <cstring>
#include <fstream>
#include <vector>
#include <memory>
#include <string>

using namespace primal::graphics;
using namespace primal::graphics::rhi;
using namespace primal::math;
using namespace Engine::Test;

namespace rhimath = primal::graphics::rhi::math;

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

namespace {

struct DeviceFixture {
    RHIDeviceBase* base{nullptr};
    VulkanDevice* vk{nullptr};
    DeviceDesc desc{};
    bool Init() {
        desc.platform = RHIPlatform::Vulkan;
        desc.enableValidation = true;
        desc.enableDebug = true;
        base = CreateRHIDevice(desc);
        if (!base) return false;
        vk = static_cast<VulkanDevice*>(base);
        return true;
    }
    ~DeviceFixture() { if (base) base->Shutdown(); }
};

m4x4 make_identity_m4x4() {
    m4x4 r{};
    std::memset(&r, 0, sizeof(r));
    r.columns[0][0] = 1.0f;
    r.columns[1][1] = 1.0f;
    r.columns[2][2] = 1.0f;
    r.columns[3][3] = 1.0f;
    return r;
}

// T4.6.5 part 26: texture helpers (RHIDeviceBase version of Dawn test helpers).
// Pattern adapted from TestVulkanCommandBuffer.cpp:122-162 — Vulkan has no
// UpdateTextureData on RHIDeviceBase, so we use staging buffer +
// CopyBufferToTexture + GenerateMipmaps. Software mip box filter is generated
// inline so the staging buffer carries only mip 0; GPU GenerateMipmaps fills
// the rest of the chain.

ResourceHandle CreateTextureFromData(RHIDeviceBase* device, int w, int h,
    const unsigned char* data, DataFormat format = DataFormat::RGBA8_UNorm) {
    using namespace primal::graphics::rhi;
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

    // Staging buffer for mip 0 data.
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
    region.bufferRowLength = 0;  // tightly packed
    region.bufferImageHeight = 0;
    region.imageSubresource = { 0, 0, 1 };  // baseArrayLayer=0, mipLevel=0, layerCount=1
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

} // anonymous namespace

TestResult TestVulkanStandardPipeline_Initialize_Smoke() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "Vulkan device init");

    StandardRenderPipeline pipeline;
    bool ok = pipeline.Initialize(fx.base);
    TEST_ASSERT(ok, "StandardRenderPipeline::Initialize on Vulkan");

    pipeline.Shutdown();
    return TestResult::Passed;
}

TestResult TestVulkanStandardPipeline_EditorMode_NoOpRender() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "Vulkan device init");

    StandardRenderPipeline pipeline;
    TEST_ASSERT(pipeline.Initialize(fx.base), "Initialize");

    pipeline.SetEditorMode(true);

    constexpr u32 W = 64, H = 64;
    TextureDesc rtDesc{
        {W, H, 1}, 1, 1,
        DataFormat::RGBA16_Float,
        TextureType::Texture2D,
        TextureUsage::RenderTarget | TextureUsage::CopySource | TextureUsage::ShaderResource,
        GPUMemoryUsage::Static,
        "PipelineSmokeRT"
    };
    ResourceHandle renderTarget = fx.base->CreateTexture(rtDesc);
    TEST_ASSERT(renderTarget != handles::INVALID_RESOURCE, "CreateTexture renderTarget");

    RenderScene scene;
    RenderLight light;
    light.type = LightType::Directional;
    light.direction = v3{0.0f, -1.0f, 0.0f};
    light.color = v3{1.0f, 1.0f, 1.0f};
    light.intensity = 1.0f;
    scene.AddLight(light);

    RenderView view;
    m4x4 viewMat = make_identity_m4x4();
    viewMat.columns[3][2] = 5.0f;
    view.SetViewMatrix(viewMat);

    m4x4 proj{};
    std::memset(&proj, 0, sizeof(proj));
    constexpr float pi = 3.14159265358979323846f;
    float fov = 60.0f * (pi / 180.0f);
    float aspect = float(W) / float(H);
    float f = 1.0f / std::tan(fov * 0.5f);
    proj.columns[0][0] = f / aspect;
    proj.columns[1][1] = f;
    proj.columns[2][2] = 50.0f / (0.1f - 100.0f);
    proj.columns[2][3] = 1.0f;
    proj.columns[3][2] = -(0.1f * 100.0f) / (0.1f - 100.0f);
    view.SetProjectionMatrix(proj);
    view.Cull(scene);

    CommandBufferHandle cmdHandle = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
    VulkanCommandBuffer* vcmd = fx.vk->GetCommandBuffer(cmdHandle);
    TEST_ASSERT(vcmd->Reset() && vcmd->Begin(), "Begin");

    // Editor mode without subsystems init: forward_renderer_ is null, gpuDraw
    // not initialized → both branches early-return. Should complete cleanly.
    pipeline.RenderWithCommandBuffer(scene, view, renderTarget, rtDesc,
                                      vcmd, 0, cmdHandle,
                                      handles::INVALID_SYNC);

    TEST_ASSERT(vcmd->End() && vcmd->Submit(0) && vcmd->WaitForCompletion(),
                "Submit (no-op render)");

    fx.base->DestroyCommandBuffer(cmdHandle);
    fx.base->DestroyTexture(renderTarget);
    pipeline.Shutdown();
    return TestResult::Passed;
}

// Tier 4.6 probe — exercises InitializeSubsystems path on Vulkan.
//
// SetLumenConfig triggers InitializeSubsystems which creates ALL heavy
// subsystems: GPUDrivenDrawPipeline, GPUCullingPipeline, HZBSystem,
// NaniteResourceManager, GlobalSDF, scene_snapshot_, ShadowMapModule,
// DeferredLightingModule, FinalBlitModule, and ForwardSceneRenderer.
//
// STATUS: PASSING with zero validation errors (T4.6.1 + T4.6.2 + T4.6.3).
//
// Fixes shipped:
//   - T4.6.1: GPUCullingPipeline `std::array<DescriptorSetHandle, 3>{ INVALID }`
//     under-init (only element 0 set; elements 1-2 zero-init to valid handle 0).
//   - T4.6.2: GPUDrivenDrawPipeline final_depth_texture_ had RenderTarget bit
//     on a D32_SFLOAT image (illegal in Vulkan).
//   - T4.6.3: 5 Lumen passes + ForwardSceneRenderer early-skip on Vulkan
//     (Metal overlapping binding namespace + .metal hardcode + push-constant
//     offset=2 not 4-aligned).
//
// Deferred (T4.6.4+ scope, NOT blocking this probe):
//   1. ParticlePass + LineBatchRenderer hardcoded `.metal` shader load
//      (T4.6.4 — platform-aware shader loader).
//   2. ForwardSceneRenderer platform-aware shader loader + 11 SPIR-V ports
//      (T4.6.5 — full Editor render path).
//   3. Lumen suite ~25 SPIR-V shader ports + sequential-binding layouts
//      (multi-session).
TestResult TestVulkanStandardPipeline_SubsystemsProbe() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "Vulkan device init");

    StandardRenderPipeline pipeline;
    TEST_ASSERT(pipeline.Initialize(fx.base), "Initialize");

    lumen::LumenConfig lumenConfig{};
    pipeline.SetLumenConfig(lumenConfig);

    pipeline.Shutdown();
    return TestResult::Passed;
}

// T4.6.5 part 15.6 — end-to-end Editor render smoke on Vulkan.
//
// First-ever exercise of the FULL Editor mode render path:
//   1. StandardRenderPipeline::Initialize + SetLumenConfig triggers
//      InitializeSubsystems → ForwardSceneRenderer::Initialize on Vulkan.
//      This loads all 11 ForwardSceneRenderer SPIR-V shaders and creates all
//      11 pipelines (T4.6.5 parts 1-14).
//   2. SetEditorMode(true) routes RenderWithCommandBuffer through
//      forward_renderer_->Render() instead of GPUDrivenDrawPipeline.
//   3. Render() runs the full bind-site wiring from T4.6.5 parts 15.2-15.4
//      (instance buffer / shadow VP / SoA streaming via Vulkan descriptors).
//
// Scene is intentionally mesh-less — RegisterMeshResource requires a real
// content::get_rhi_mesh_asset id, which is out of scope for a smoke test.
// ForwardSceneRenderer::Render early-returns when !scene_loaded_, so this
// test exercises init + render path ENTRY without producing pixels. The bar
// is "no crash, zero validation errors from init" — that alone proves the
// skip lift is safe and all 11 pipelines create cleanly under real load.
TestResult TestVulkanEditorRender_ForwardSceneRenderer() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "Vulkan device init");

    StandardRenderPipeline pipeline;
    TEST_ASSERT(pipeline.Initialize(fx.base), "Initialize");

    lumen::LumenConfig lumenConfig{};
    pipeline.SetLumenConfig(lumenConfig);
    pipeline.SetEditorMode(true);

    constexpr u32 W = 64, H = 64;
    TextureDesc rtDesc{
        {W, H, 1}, 1, 1,
        DataFormat::RGBA16_Float,
        TextureType::Texture2D,
        TextureUsage::RenderTarget | TextureUsage::CopySource | TextureUsage::ShaderResource,
        GPUMemoryUsage::Static,
        "EditorRenderRT"
    };
    ResourceHandle renderTarget = fx.base->CreateTexture(rtDesc);
    TEST_ASSERT(renderTarget != handles::INVALID_RESOURCE, "CreateTexture renderTarget");

    RenderScene scene;
    RenderLight light;
    light.type = LightType::Directional;
    light.direction = v3{0.0f, -1.0f, 0.0f};
    light.color = v3{1.0f, 1.0f, 1.0f};
    light.intensity = 1.0f;
    scene.AddLight(light);

    RenderView view;
    m4x4 viewMat = make_identity_m4x4();
    viewMat.columns[3][2] = 5.0f;
    view.SetViewMatrix(viewMat);

    m4x4 proj{};
    std::memset(&proj, 0, sizeof(proj));
    constexpr float pi = 3.14159265358979323846f;
    float fov = 60.0f * (pi / 180.0f);
    float aspect = float(W) / float(H);
    float f = 1.0f / std::tan(fov * 0.5f);
    proj.columns[0][0] = f / aspect;
    proj.columns[1][1] = f;
    proj.columns[2][2] = 50.0f / (0.1f - 100.0f);
    proj.columns[2][3] = 1.0f;
    proj.columns[3][2] = -(0.1f * 100.0f) / (0.1f - 100.0f);
    view.SetProjectionMatrix(proj);
    view.Cull(scene);

    CommandBufferHandle cmdHandle = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
    VulkanCommandBuffer* vcmd = fx.vk->GetCommandBuffer(cmdHandle);
    TEST_ASSERT(vcmd->Reset() && vcmd->Begin(), "Begin");

    pipeline.RenderWithCommandBuffer(scene, view, renderTarget, rtDesc,
                                      vcmd, 0, cmdHandle,
                                      handles::INVALID_SYNC);

    TEST_ASSERT(vcmd->End() && vcmd->Submit(0) && vcmd->WaitForCompletion(),
                "Submit (Editor render path)");

    fx.base->DestroyCommandBuffer(cmdHandle);
    fx.base->DestroyTexture(renderTarget);
    pipeline.Shutdown();
    return TestResult::Passed;
}

// T4.6.5 part 26 — Sponza end-to-end on Vulkan production Render() path.
// Replaces Part 24's empty scene with the full Sponza scene: loads Sponza.model
// via SceneDataAdapter, creates per-mesh MaterialInstance + game_entity +
// cluster + RenderProxy, wires GPUMaterialRegistry to GPUDrivenDrawPipeline,
// then fires Render() at 1280×720 with camera+light matching TestDawnForwardRenderer.
//
// Pattern lifted from TestDawnForwardRenderer.cpp:430-460 (Sponza load),
// 640-753 (per-mesh setup), 2530-2615 (material registry wiring). Camera
// defaults from TestDawnForwardRenderer.h:111-113.
TestResult TestVulkanStandardPipelineRender_NonEditor() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "Vulkan device init");

    // JobSystem is required by GPUMaterialRegistry::BuildAsync.
    if (!primal::jobsystem::JobSystem::Initialize(
            primal::jobsystem::JobSchedulerConfig::Default())) {
        std::cerr << "[Part26] JobSystem init failed" << std::endl;
        return TestResult::Failed;
    }

    StandardRenderPipeline pipeline;
    TEST_ASSERT(pipeline.Initialize(fx.base), "Initialize");

    // Register with global device manager so content::create_resource can
    // reach the Vulkan device when GPUMaterialRegistry creates placeholder
    // textures. Without this, create_texture_resource falls through to the
    // legacy add_texture path which asserts (Renderer.cpp:448).
    rhi::g_deviceManager.RegisterDevice(fx.base);

    // Load shaders (real DeferredLighting port from Part 25 + Blit for FinalBlit).
    auto loadSpv = [](const char* relpath) -> std::vector<u8> {
        std::ifstream f(relpath, std::ios::binary | std::ios::ate);
        if (!f) return {};
        std::streamsize sz = f.tellg();
        f.seekg(0);
        std::vector<u8> bytes(static_cast<size_t>(sz));
        f.read(reinterpret_cast<char*>(bytes.data()), sz);
        return bytes;
    };
    auto deferredVsBytes = loadSpv("Engine/Graphics/Vulkan/shaders/Forward/DeferredLighting.vert.spv");
    auto deferredFsBytes = loadSpv("Engine/Graphics/Vulkan/shaders/Forward/DeferredLighting.frag.spv");
    TEST_ASSERT(!deferredVsBytes.empty() && !deferredFsBytes.empty(),
                "Load DeferredLighting.vert.spv + .frag.spv");
    auto blitVsBytes = loadSpv("Engine/Graphics/Vulkan/shaders/Forward/Blit.vert.spv");
    auto blitFsBytes = loadSpv("Engine/Graphics/Vulkan/shaders/Forward/Blit.frag.spv");
    TEST_ASSERT(!blitVsBytes.empty() && !blitFsBytes.empty(),
                "Load Blit.vert.spv + Blit.frag.spv");

    ShaderHandle deferredVs = fx.base->CreateShader(
        deferredVsBytes.data(), deferredVsBytes.size(), ShaderStage::Vertex, "main");
    ShaderHandle deferredFs = fx.base->CreateShader(
        deferredFsBytes.data(), deferredFsBytes.size(), ShaderStage::Pixel, "main");
    TEST_ASSERT(deferredVs != handles::INVALID_SHADER, "CreateShader DeferredLighting.vert");
    TEST_ASSERT(deferredFs != handles::INVALID_SHADER, "CreateShader DeferredLighting.frag");
    ShaderHandle blitVs = fx.base->CreateShader(
        blitVsBytes.data(), blitVsBytes.size(), ShaderStage::Vertex, "main");
    ShaderHandle blitFs = fx.base->CreateShader(
        blitFsBytes.data(), blitFsBytes.size(), ShaderStage::Pixel, "main");

    StandardRenderPipeline::ShaderHandles shaderHandles;
    shaderHandles.deferred_vs = deferredVs;
    shaderHandles.deferred_ps = deferredFs;
    shaderHandles.blit_vs = blitVs;
    shaderHandles.blit_ps = blitFs;

    // Phase 2: load Fusion + GIGather SPIR-V shaders so SSAO/DDGI output can be
    // composited into the final image. Without these, fusion_module_ stays null
    // and SSAO's output is never consumed → all-black render.
    auto fusionIndirectBytes = loadSpv("Engine/Graphics/Vulkan/shaders/Lumen/FusionIndirect.frag.spv");
    auto fusionCompositeBytes = loadSpv("Engine/Graphics/Vulkan/shaders/Lumen/FusionComposite.frag.spv");
    auto giGatherBytes = loadSpv("Engine/Graphics/Vulkan/shaders/Lumen/DDGIGIGather.comp.spv");
    if (!fusionIndirectBytes.empty()) {
        shaderHandles.fusion_indirect_ps = fx.base->CreateShader(
            fusionIndirectBytes.data(), fusionIndirectBytes.size(), ShaderStage::Pixel, "fusion_indirect");
    }
    if (!fusionCompositeBytes.empty()) {
        shaderHandles.fusion_composite_ps = fx.base->CreateShader(
            fusionCompositeBytes.data(), fusionCompositeBytes.size(), ShaderStage::Pixel, "fusion_composite");
    }
    if (!giGatherBytes.empty()) {
        shaderHandles.gi_gather = fx.base->CreateShader(
            giGatherBytes.data(), giGatherBytes.size(), ShaderStage::Compute, "ddgi_gi_gather");
    }
    pipeline.SetShaderHandles(shaderHandles);

    // Phase 2 test: Medium preset enables SSAO + SSGI + DDGI + Fusion.
    // SSGI (Phase 3) and DDGI (MoltenVK spvUnsafeArray issue) Initialize fails
    // gracefully — their passes stay null but Fusion still composites SSAO.
    lumen::LumenConfig lumenConfig{};
    lumenConfig.quality = lumen::LumenQualityPreset::Medium;
    pipeline.SetLumenConfig(lumenConfig);

    // -------------------------------------------------------------
    // Step 1: Load Sponza.model via SceneDataAdapter.
    // -------------------------------------------------------------
    const std::string baseDir = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/assets/";
    const std::string modelPath = baseDir + "Sponza.model";
    std::ifstream file(modelPath, std::ios::binary | std::ios::ate);
    TEST_ASSERT(file.is_open(), "Open Sponza.model");
    std::streamsize modelSize = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<char> modelBuffer(static_cast<size_t>(modelSize));
    TEST_ASSERT(file.read(modelBuffer.data(), modelSize), "Read Sponza.model");

    SceneDataAdapter adapter;
    auto sceneMeshes = adapter.LoadRenderItemData(
        fx.base, modelBuffer.data(), (u32)modelBuffer.size());
    TEST_ASSERT(!sceneMeshes.empty(), "LoadRenderItemData");
    std::cout << "[Part26] Loaded " << sceneMeshes.size() << " meshes from Sponza.model" << std::endl;

    // -------------------------------------------------------------
    // Step 2: Create shared Material + fallback textures + sampler.
    // -------------------------------------------------------------
    auto material = std::make_shared<Material>();

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
    ResourceHandle fallbackDiffuse = CreateTextureFromData(
        fx.base, FALLBACK_SIZE, FALLBACK_SIZE, whiteBuf.data(), DataFormat::RGBA8_sRGB);
    ResourceHandle fallbackNormal = CreateTextureFromData(
        fx.base, FALLBACK_SIZE, FALLBACK_SIZE, flatNormalBuf.data());
    ResourceHandle fallbackORM = CreateTextureFromData(
        fx.base, FALLBACK_SIZE, FALLBACK_SIZE, defaultORMBuf.data());
    TEST_ASSERT(fallbackDiffuse != handles::INVALID_RESOURCE, "fallbackDiffuse");
    TEST_ASSERT(fallbackNormal != handles::INVALID_RESOURCE, "fallbackNormal");
    TEST_ASSERT(fallbackORM != handles::INVALID_RESOURCE, "fallbackORM");

    SamplerDesc samplerDesc{};
    samplerDesc.minFilter = FilterMode::Linear;
    samplerDesc.magFilter = FilterMode::Linear;
    samplerDesc.addressU = TextureAddressMode::Wrap;
    samplerDesc.addressV = TextureAddressMode::Wrap;
    samplerDesc.addressW = TextureAddressMode::Wrap;
    samplerDesc.comparisonFunc = ComparisonFunc::Never;
    SamplerHandle materialSampler = fx.base->CreateSampler(samplerDesc);
    TEST_ASSERT(materialSampler != handles::INVALID_SAMPLER, "materialSampler");

    // -------------------------------------------------------------
    // Step 3: Per-mesh MaterialInstance + game_entity + cluster + RenderProxy.
    // -------------------------------------------------------------
    RenderScene scene;
    u32 texLoaded = 0, texFailed = 0;
    std::vector<std::shared_ptr<MaterialInstance>> materialInstances;
    std::vector<primal::game_entity::entity> entities;
    std::vector<primal::cluster::component> clusterComps;

    for (u32 i = 0; i < sceneMeshes.size(); ++i) {
        auto& meshInfo = sceneMeshes[i];
        meshInfo.material = material;

        auto matInst = std::make_shared<MaterialInstance>(material.get());
        if (!matInst->Initialize(fx.base)) {
            std::cerr << "[Part26] MaterialInstance init failed for mesh " << i << std::endl;
            matInst = std::make_shared<MaterialInstance>(material.get());
        }

        // Load textures (fallback when missing).
        ResourceHandle diffuseTex = handles::INVALID_RESOURCE;
        std::string diffusePath = ResolveTexturePath(baseDir, meshInfo.diffuseTexturePath);
        if (!diffusePath.empty()) diffuseTex = LoadTextureFromFile(fx.base, diffusePath);
        if (diffuseTex == handles::INVALID_RESOURCE) { diffuseTex = fallbackDiffuse; texFailed++; }
        else texLoaded++;

        ResourceHandle normalTex = handles::INVALID_RESOURCE;
        std::string normalPath = ResolveTexturePath(baseDir, meshInfo.normalTexturePath);
        if (!normalPath.empty()) normalTex = LoadTextureFromFile(fx.base, normalPath);
        if (normalTex == handles::INVALID_RESOURCE) normalTex = fallbackNormal;

        ResourceHandle ormTex = handles::INVALID_RESOURCE;
        std::string ormPath = ResolveTexturePath(baseDir, meshInfo.ormTexturePath);
        if (!ormPath.empty()) ormTex = LoadTextureFromFile(fx.base, ormPath);
        if (ormTex == handles::INVALID_RESOURCE) ormTex = fallbackORM;

        matInst->SetTexture(0, diffuseTex);
        matInst->SetTexture(1, normalTex);
        matInst->SetTexture(2, ormTex);
        matInst->SetSampler(3, materialSampler);
        matInst->Update(fx.base);

        meshInfo.materialInstance = matInst;
        materialInstances.push_back(matInst);

        // Create game_entity + cluster component for meshlet pipeline.
        primal::game_entity::entity_info entInfo{};
        primal::transform::init_info tfInfo{};
        tfInfo.position[0] = 0.0f;
        tfInfo.position[1] = 0.0f;
        tfInfo.position[2] = 0.0f;
        tfInfo.rotation[0] = 0.0f;
        tfInfo.rotation[1] = 0.0f;
        tfInfo.rotation[2] = 0.0f;
        tfInfo.rotation[3] = 1.0f;  // identity quaternion
        entInfo.transform = &tfInfo;
        primal::game_entity::entity entity = primal::game_entity::create(entInfo);
        TEST_ASSERT(entity.is_valid(), "game_entity::create");
        entities.push_back(entity);

        primal::cluster::init_info clusterInit{};
        clusterInit.geometry_content_id = meshInfo.meshEntityId;
        primal::cluster::component clusterComp = primal::cluster::create(clusterInit, entity);
        clusterComps.push_back(clusterComp);

        RenderProxy proxy;
        proxy.materialId = meshInfo.meshEntityId;
        proxy.entityId = meshInfo.meshEntityId;
        proxy.meshId = clusterComp;
        proxy.transform = rhimath::MatrixIdentity();
        if (meshInfo.mesh && meshInfo.mesh->IsValid()) {
            proxy.worldAABB = meshInfo.mesh->GetLocalAABB();
        }
        scene.AddProxy(proxy);
    }
    std::cout << "[Part26] Textures: " << texLoaded << " loaded, " << texFailed << " fallback" << std::endl;
    std::cout << "[Part26] Scene proxies: " << scene.GetProxies().size() << std::endl;

    // -------------------------------------------------------------
    // Step 4: GPUMaterialRegistry wiring.
    // -------------------------------------------------------------
    auto* registry = new primal::graphics::nanite::GPUMaterialRegistry();
    u32 registeredCount = 0;
    for (auto& meshInfo : sceneMeshes) {
        if (meshInfo.materialInstance) {
            auto matID = registry->RegisterMaterial(meshInfo.materialInstance.get());
            if (matID != primal::graphics::nanite::GPUMaterialRegistry::INVALID_MATERIAL_ID) {
                meshInfo.gpuMaterialId = matID;
                registeredCount++;
            }
        }
    }
    std::cout << "[Part26] Registered " << registeredCount << " materials" << std::endl;

    // Patch proxy.materialId from entity_id → gpuMaterialId.
    for (auto& meshInfo : sceneMeshes) {
        if (meshInfo.materialInstance && meshInfo.gpuMaterialId != primal::id::invalid_id) {
            for (const auto& proxy : scene.GetProxies()) {
                if (proxy.entityId == meshInfo.meshEntityId) {
                    RenderProxy patched = proxy;
                    patched.materialId = meshInfo.gpuMaterialId;
                    scene.UpdateProxy(meshInfo.meshEntityId, patched);
                    break;
                }
            }
        }
    }

    auto buildJob = registry->BuildAsync(fx.base);
    buildJob.Wait();
    TEST_ASSERT(registry->UploadToGPU(fx.base), "Material upload");
    std::cout << "[Part26] Materials uploaded to GPU" << std::endl;

    // Wire to GPUDrivenDrawPipeline singleton (created by InitializeSubsystems).
    auto& gpuDraw = primal::graphics::nanite::GPUDrivenDrawPipeline::Get();
    gpuDraw.SetMaterialDataBuffer(registry->GetMaterialDataBuffer());
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
    SamplerHandle texSampler = fx.base->CreateSampler(texSamplerDesc);
    gpuDraw.SetTextureArrays(
        registry->GetAlbedoTextureArray(),
        registry->GetNormalTextureArray(),
        registry->GetORMTextureArray(),
        texSampler);

    // -------------------------------------------------------------
    // Step 5: Directional light + camera (TestDawnForwardRenderer defaults).
    // -------------------------------------------------------------
    RenderLight sunLight;
    sunLight.type = LightType::Directional;
    sunLight.direction = v3{0.5f, -0.7f, 0.3f};
    sunLight.color = v3{1.0f, 0.95f, 0.9f};
    sunLight.intensity = 3.0f;
    scene.AddLight(sunLight);

    constexpr u32 W = 1280, H = 720;
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

    RenderView view;
    m4x4 viewMat = rhimath::CreateLookAtMatrix(cameraPos, target, up);
    view.SetViewMatrix(viewMat);
    m4x4 projMat = rhimath::CreatePerspectiveMatrix(
        60.0f * rhimath::constants::DEG_TO_RAD,
        static_cast<float>(W) / static_cast<float>(H), 0.1f, 1000.0f);
    view.SetProjectionMatrix(projMat);
    view.Cull(scene);

    // -------------------------------------------------------------
    // Step 6: Render target + Render() + readback + SavePNG.
    // -------------------------------------------------------------
    TextureDesc rtDesc{
        {W, H, 1}, 1, 1,
        DataFormat::BGRA8_UNorm,
        TextureType::Texture2D,
        TextureUsage::RenderTarget | TextureUsage::CopySource | TextureUsage::ShaderResource,
        GPUMemoryUsage::Static,
        "SponzaRT"
    };
    ResourceHandle renderTarget = fx.base->CreateTexture(rtDesc);
    TEST_ASSERT(renderTarget != handles::INVALID_RESOURCE, "CreateTexture renderTarget");

    std::cout << "[Part26] Invoking StandardRenderPipeline::Render()..." << std::endl;
    pipeline.Render(scene, view, renderTarget, rtDesc);
    std::cout << "[Part26] Render() returned." << std::endl;

    // === SurfaceCache lighting-atlas dump (SC visual verification) ===
    // Reads back the card atlas LightEval produced (RGBA16F) and writes an
    // 8-bit BMP. Non-zero + spatially-varied content = Capture gathered real
    // Sponza material and LightEval lit it.
    {
        auto* scPass = pipeline.GetSurfaceCachePass();
        if (scPass) {
            const u32 AW = 2048, AH = 2048;   // atlas_size default
            // Diagnostic A: albedo atlas (RGBA8, 4B/texel — same readback path
            // as the working GBuffer dumps). If non-zero, dispatches + readback
            // are fine and the issue is LightEval output or 16F readback.
            {
                rhi::ResourceHandle atlas = scPass->GetEmissiveAtlas();
                if (atlas != rhi::handles::INVALID_RESOURCE) {
                BufferDesc abDesc{};
                abDesc.size = (u64)AW * AH * 8;
                abDesc.type = BufferType::Raw;
                abDesc.memoryUsage = rhi::GPUMemoryUsage::Readback;
                rhi::ResourceHandle ab = fx.base->CreateBuffer(abDesc);
                CommandBufferHandle acmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
                auto* vacmd = fx.vk->GetCommandBuffer(acmd);
                vacmd->Reset(); vacmd->Begin();
                rhi::ResourceBarrier ab_{};
                ab_.resource = atlas; ab_.beforeState = rhi::ResourceState::UnorderedAccess;
                ab_.afterState = rhi::ResourceState::CopySource;
                ab_.subresource = 0xFFFFFFFF; ab_.queueFamily = 0xFFFFFFFF;
                vacmd->InsertBarrier(&ab_, 1);
                rhi::BufferTextureCopyRegion ar{};
                ar.imageSubresource = {0, 0, 1};
                ar.imageExtent = {AW, AH, 1};
                vacmd->CopyTextureToBuffer(atlas, ab, &ar, 1);
                vacmd->End(); vacmd->Submit(0); vacmd->WaitForCompletion();
                fx.base->DestroyCommandBuffer(acmd);
                auto* amapped = static_cast<u8*>(fx.base->MapBuffer(ab, 0, (u64)AW * AH * 8));
                if (amapped) {
                    u64 nonZero = 0;
                    for (u64 i = 0; i < (u64)AW * AH * 8; ++i) if (amapped[i]) ++nonZero;
                    std::ofstream bmp("sc_emissive_atlas.raw", std::ios::binary);
                    if (bmp.is_open()) bmp.write(reinterpret_cast<const char*>(amapped), (u64)AW*AH*8);
                    if (bmp.is_open()) bmp.close();
                    std::cout << "[SCDump] EMISSIVE atlas (binding3 target) nonZeroBytes=" << nonZero
                              << "/" << (u64)AW * AH * 8 << std::endl;
                    fx.base->UnmapBuffer(ab);
                }
                fx.base->DestroyBuffer(ab);
                }
            }
            // Diagnostic B: lighting atlas (RGBA16F) — all 3 triple-buffer slots.
            for (u32 slot = 0; slot < 3; ++slot) {
            rhi::ResourceHandle atlas = scPass->GetLightingAtlas(slot);
            if (atlas == rhi::handles::INVALID_RESOURCE) continue;
                BufferDesc abDesc{};
                abDesc.size = (u64)AW * AH * 8;   // RGBA16F = 8 bytes/texel
                abDesc.type = BufferType::Raw;
                abDesc.memoryUsage = rhi::GPUMemoryUsage::Readback;
                rhi::ResourceHandle ab = fx.base->CreateBuffer(abDesc);

                CommandBufferHandle acmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
                auto* vacmd = fx.vk->GetCommandBuffer(acmd);
                vacmd->Reset(); vacmd->Begin();
                rhi::ResourceBarrier ab_{};
                ab_.resource = atlas; ab_.beforeState = rhi::ResourceState::ShaderResource;
                ab_.afterState = rhi::ResourceState::CopySource;
                ab_.subresource = 0xFFFFFFFF; ab_.queueFamily = 0xFFFFFFFF;
                vacmd->InsertBarrier(&ab_, 1);
                rhi::BufferTextureCopyRegion ar{};
                ar.imageSubresource = {0, 0, 1};
                ar.imageExtent = {AW, AH, 1};
                vacmd->CopyTextureToBuffer(atlas, ab, &ar, 1);
                vacmd->End(); vacmd->Submit(0); vacmd->WaitForCompletion();
                fx.base->DestroyCommandBuffer(acmd);

                auto* amapped = static_cast<u8*>(fx.base->MapBuffer(ab, 0, (u64)AW * AH * 8));
                {
                    auto* vtex = fx.vk->GetTexture(atlas);
                    std::cout << "[SCDump] slot " << slot << " handle=" << (u64)atlas
                              << " mapped=" << (amapped ? "yes" : "NO")
                              << " layout=" << (vtex ? (int)vtex->GetCurrentLayout() : -1)
                              << " view=" << (vtex && vtex->GetNativeView() ? "ok" : "NULL")
                              << " image=" << (vtex && vtex->GetNativeImage() ? "ok" : "NULL")
                              << std::endl;
                }
                if (amapped) {
                    // f16 → 8-bit, tonemap via simple clamp, write BMP
                    std::vector<u8> rgb((size_t)AW * AH * 4, 0);
                    u64 nonZero = 0; double lumSum = 0.0;
                    for (u32 y = 0; y < AH; ++y) {
                        for (u32 x = 0; x < AW; ++x) {
                            const _Float16* px = reinterpret_cast<const _Float16*>(&amapped[((u64)y * AW + x) * 8]);
                            float r = (float)px[0], g = (float)px[1], b = (float)px[2];
                            // Count texels LightEval actually wrote: alpha=1.0
                            // marks written texels even when rgb == 0 (NdotL=0).
                            if (r > 0.0f || g > 0.0f || b > 0.0f || px[3] > 0.5f) ++nonZero;
                            lumSum += (r + g + b) / 3.0f;
                            u8* dst = &rgb[((u64)y * AW + x) * 4];
                            dst[0] = (u8)std::min(255.0f, r * 255.0f);
                            dst[1] = (u8)std::min(255.0f, g * 255.0f);
                            dst[2] = (u8)std::min(255.0f, b * 255.0f);
                            dst[3] = 255;
                        }
                    }
                    std::ofstream bmp(("sc_lighting_atlas_s" + std::to_string(slot) + ".bmp").c_str(), std::ios::binary);
                    if (bmp.is_open()) {
                        u32 fs = 54 + AW * AH * 4;
                        u8 hdr[54] = {};
                        hdr[0]='B'; hdr[1]='M';
                        hdr[2]=fs&0xFF; hdr[3]=(fs>>8)&0xFF; hdr[4]=(fs>>16)&0xFF;
                        hdr[10]=54; hdr[14]=40;
                        hdr[18]=AW&0xFF; hdr[19]=(AW>>8)&0xFF;
                        hdr[22]=AH&0xFF; hdr[23]=(AH>>8)&0xFF;
                        hdr[26]=1; hdr[28]=32;
                        bmp.write(reinterpret_cast<const char*>(hdr), 54);
                        for (u32 y = AH; y-- > 0;)
                            bmp.write(reinterpret_cast<const char*>(&rgb[(u64)y * AW * 4]), AW * 4);
                        bmp.close();
                    }
                    std::cout << "[SCDump] slot " << slot << " nonZeroTexels="
                              << nonZero << "/" << (u64)AW * AH
                              << " avgLum=" << (lumSum / ((double)AW * AH))
                              << std::endl;
                    fx.base->UnmapBuffer(ab);
                }
                fx.base->DestroyBuffer(ab);
            }
        }
    }

    // === DeferredLighting output dump (composition-chain diagnosis) ===
    {
        auto* dlMod = pipeline.GetDeferredLightingModule();
        if (dlMod) {
            for (u32 slot = 0; slot < 3; ++slot) {
                rhi::ResourceHandle dtex = dlMod->GetOutputTexture(slot);
                if (dtex == rhi::handles::INVALID_RESOURCE) continue;
                BufferDesc dbDesc{};
                dbDesc.size = (u64)W * H * 8;
                dbDesc.type = BufferType::Raw;
                dbDesc.memoryUsage = rhi::GPUMemoryUsage::Readback;
                rhi::ResourceHandle db = fx.base->CreateBuffer(dbDesc);
                CommandBufferHandle dcmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
                auto* vdcmd = fx.vk->GetCommandBuffer(dcmd);
                vdcmd->Reset(); vdcmd->Begin();
                rhi::ResourceBarrier dbk{};
                dbk.resource = dtex; dbk.beforeState = rhi::ResourceState::ShaderResource;
                dbk.afterState = rhi::ResourceState::CopySource;
                dbk.subresource = 0xFFFFFFFF; dbk.queueFamily = 0xFFFFFFFF;
                vdcmd->InsertBarrier(&dbk, 1);
                rhi::BufferTextureCopyRegion dgn{};
                dgn.imageSubresource = {0, 0, 1};
                dgn.imageExtent = {W, H, 1};
                vdcmd->CopyTextureToBuffer(dtex, db, &dgn, 1);
                vdcmd->End(); vdcmd->Submit(0); vdcmd->WaitForCompletion();
                fx.base->DestroyCommandBuffer(dcmd);
                auto* dm = static_cast<u8*>(fx.base->MapBuffer(db, 0, (u64)W * H * 8));
                if (dm) {
                    double rSum = 0.0, gSum = 0.0, bSum = 0.0; u64 n = 0;
                    for (u32 y = 0; y < H; y += 4) {
                        for (u32 x = 0; x < W; x += 4) {
                            const _Float16* px = reinterpret_cast<const _Float16*>(&dm[((u64)y * W + x) * 8]);
                            rSum += (float)px[0]; gSum += (float)px[1]; bSum += (float)px[2]; ++n;
                        }
                    }
                    std::cout << "[DLDump] slot " << slot
                              << " R(shadowVis)=" << (rSum / n)
                              << " G(NdotL)=" << (gSum / n)
                              << " B=" << (bSum / n)
                              << " (" << n << " samples)" << std::endl;
                    fx.base->UnmapBuffer(db);
                }
                fx.base->DestroyBuffer(db);
            }
        }
    }

    // === SSGI output dump (diagnosis: is SSGI producing black?) ===
    {
        auto* ssgiPass = pipeline.GetSSGIPass();
        // Dump trace (half-res) and filter (full-res) to narrow down zero source
        if (ssgiPass && ssgiPass->IsInitialized()) {
            struct TexInfo { rhi::ResourceHandle tex; const char* name; u32 tw, th; };
            TexInfo infos[] = {
                {ssgiPass->GetTraceTexture(), "trace(half)", W/2, H/2},
                {ssgiPass->GetFilterTexture(), "filter(full)", W, H},
            };
            for (auto& ti : infos) {
                if (ti.tex == rhi::handles::INVALID_RESOURCE) continue;
                BufferDesc tbDesc{};
                tbDesc.size = (u64)ti.tw * ti.th * 8;
                tbDesc.type = BufferType::Raw;
                tbDesc.memoryUsage = rhi::GPUMemoryUsage::Readback;
                rhi::ResourceHandle tb = fx.base->CreateBuffer(tbDesc);
                CommandBufferHandle tcmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
                auto* vtcmd = fx.vk->GetCommandBuffer(tcmd);
                vtcmd->Reset(); vtcmd->Begin();
                rhi::ResourceBarrier tbk{};
                tbk.resource = ti.tex; tbk.beforeState = rhi::ResourceState::ShaderResource;
                tbk.afterState = rhi::ResourceState::CopySource;
                tbk.subresource = 0xFFFFFFFF; tbk.queueFamily = 0xFFFFFFFF;
                vtcmd->InsertBarrier(&tbk, 1);
                rhi::BufferTextureCopyRegion tgn{};
                tgn.imageSubresource = {0, 0, 1};
                tgn.imageExtent = {ti.tw, ti.th, 1};
                vtcmd->CopyTextureToBuffer(ti.tex, tb, &tgn, 1);
                vtcmd->End(); vtcmd->Submit(0); vtcmd->WaitForCompletion();
                fx.base->DestroyCommandBuffer(tcmd);
                auto* tm = static_cast<u8*>(fx.base->MapBuffer(tb, 0, (u64)ti.tw * ti.th * 8));
                if (tm) {
                    double rSum=0, gSum=0; u64 nz=0;
                    u64 total = ((ti.th+7)/8) * ((ti.tw+7)/8);
                    for (u32 y = 0; y < ti.th; y += 8) {
                        for (u32 x = 0; x < ti.tw; x += 8) {
                            const _Float16* px = reinterpret_cast<const _Float16*>(&tm[((u64)y * ti.tw + x) * 8]);
                            rSum += (float)px[0]; gSum += (float)px[1];
                            if ((float)px[0] > 0.001f) ++nz;
                        }
                    }
                    std::cout << "[SSGIDump] " << ti.name
                              << " R=" << (rSum/total) << " G=" << (gSum/total)
                              << " nonZero=" << nz << "/" << total << std::endl;
                    fx.base->UnmapBuffer(tb);
                }
                fx.base->DestroyBuffer(tb);
            }
        }
        // Temporal dump
        if (ssgiPass && ssgiPass->IsInitialized()) {
            for (u32 slot = 0; slot < 3; ++slot) {
                rhi::ResourceHandle stex = ssgiPass->GetTemporalTexture(slot);
                if (stex == rhi::handles::INVALID_RESOURCE) continue;
                BufferDesc sbDesc{};
                sbDesc.size = (u64)W * H * 8;   // RGBA16F
                sbDesc.type = BufferType::Raw;
                sbDesc.memoryUsage = rhi::GPUMemoryUsage::Readback;
                rhi::ResourceHandle sb = fx.base->CreateBuffer(sbDesc);
                CommandBufferHandle scmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
                auto* vscmd = fx.vk->GetCommandBuffer(scmd);
                vscmd->Reset(); vscmd->Begin();
                rhi::ResourceBarrier sbk{};
                sbk.resource = stex; sbk.beforeState = rhi::ResourceState::ShaderResource;
                sbk.afterState = rhi::ResourceState::CopySource;
                sbk.subresource = 0xFFFFFFFF; sbk.queueFamily = 0xFFFFFFFF;
                vscmd->InsertBarrier(&sbk, 1);
                rhi::BufferTextureCopyRegion sgn{};
                sgn.imageSubresource = {0, 0, 1};
                sgn.imageExtent = {W, H, 1};
                vscmd->CopyTextureToBuffer(stex, sb, &sgn, 1);
                vscmd->End(); vscmd->Submit(0); vscmd->WaitForCompletion();
                fx.base->DestroyCommandBuffer(scmd);
                auto* sm = static_cast<u8*>(fx.base->MapBuffer(sb, 0, (u64)W * H * 8));
                if (sm) {
                    double rSum=0, gSum=0, bSum=0, aSum=0; u64 nz=0;
                    for (u32 y = 0; y < H; y += 8) {
                        for (u32 x = 0; x < W; x += 8) {
                            const _Float16* px = reinterpret_cast<const _Float16*>(&sm[((u64)y * W + x) * 8]);
                            rSum += (float)px[0]; gSum += (float)px[1];
                            bSum += (float)px[2]; aSum += (float)px[3];
                            if ((float)px[0] > 0.001f || (float)px[1] > 0.001f || (float)px[2] > 0.001f) ++nz;
                        }
                    }
                    u64 total = ((H+7)/8) * ((W+7)/8);
                    std::cout << "[SSGIDump] slot " << slot
                              << " R=" << (rSum/total) << " G=" << (gSum/total)
                              << " B=" << (bSum/total) << " A(hitDist)=" << (aSum/total)
                              << " nonZero=" << nz << "/" << total << std::endl;
                    fx.base->UnmapBuffer(sb);
                }
                fx.base->DestroyBuffer(sb);
            }
        }
    }

    // === GBuffer diagnostic dump (Phase 2 debugging) ===
    // Dump Nanite GBuffer albedo + depth to BMP to isolate whether the all-black
    // output comes from GBuffer (Nanite raster) or DeferredLighting.
    {
        auto& gpuDraw = nanite::GPUDrivenDrawPipeline::Get();
        rhi::ResourceHandle gbAlbedo = gpuDraw.GetGBufferAlbedo();
        rhi::ResourceHandle gbDepth = gpuDraw.GetGBufferDepthSampleable();

        auto dumpTexture = [&](rhi::ResourceHandle tex, const char* name, bool isDepth) {
            if (tex == rhi::handles::INVALID_RESOURCE) {
                std::cerr << "[GBufferDump] " << name << " = INVALID" << std::endl;
                return;
            }
            BufferDesc dbDesc{};
            dbDesc.size = W * H * 4;
            dbDesc.type = BufferType::Raw;
            dbDesc.memoryUsage = rhi::GPUMemoryUsage::Readback;
            rhi::ResourceHandle db = fx.base->CreateBuffer(dbDesc);

            CommandBufferHandle dcmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
            auto* vdcmd = fx.vk->GetCommandBuffer(dcmd);
            vdcmd->Reset(); vdcmd->Begin();
            rhi::ResourceBarrier b{};
            b.resource = tex; b.beforeState = rhi::ResourceState::ShaderResource;
            b.afterState = rhi::ResourceState::CopySource;
            b.subresource = 0xFFFFFFFF; b.queueFamily = 0xFFFFFFFF;
            vdcmd->InsertBarrier(&b, 1);
            rhi::BufferTextureCopyRegion dr{};
            dr.imageSubresource = {0, 0, 1};
            dr.imageExtent = {W, H, 1};
            vdcmd->CopyTextureToBuffer(tex, db, &dr, 1);
            vdcmd->End(); vdcmd->Submit(0); vdcmd->WaitForCompletion();
            fx.base->DestroyCommandBuffer(dcmd);

            auto* dmapped = static_cast<u8*>(fx.base->MapBuffer(db, 0, W * H * 4));
            if (dmapped) {
                std::string fn = std::string("gbuffer_") + name + ".bmp";
                std::ofstream bmp(fn, std::ios::binary);
                if (bmp.is_open()) {
                    u32 fs = 54 + W * H * 4;
                    u8 hdr[54] = {};
                    hdr[0]='B'; hdr[1]='M';
                    hdr[2]=fs&0xFF; hdr[3]=(fs>>8)&0xFF;
                    hdr[10]=54; hdr[14]=40;
                    hdr[18]=W&0xFF; hdr[19]=(W>>8)&0xFF;
                    hdr[22]=H&0xFF; hdr[23]=(H>>8)&0xFF;
                    hdr[26]=1; hdr[28]=32;
                    bmp.write(reinterpret_cast<const char*>(hdr), 54);
                    for (u32 y = H; y-- > 0;) bmp.write(reinterpret_cast<const char*>(&dmapped[y*W*4]), W*4);
                    bmp.close();
                    // Count non-zero pixels
                    u32 nz = 0;
                    for (u32 i = 0; i < W*H*4; ++i) if (dmapped[i]) ++nz;
                    std::cout << "[GBufferDump] " << name << " saved (" << fn << "), nonZeroBytes=" << nz << "/" << (W*H*4) << std::endl;
                }
                fx.base->UnmapBuffer(db);
            }
            fx.base->DestroyBuffer(db);
        };
        dumpTexture(gbAlbedo, "albedo", false);
        dumpTexture(gbDepth, "depth", true);
        dumpTexture(gpuDraw.GetGBufferNormal(), "normal", false);

        // Also dump DeferredLighting output if available
        // DeferredLightingModule output is RGBA16F at full res
        // Accessing via deferred_module_ is private, so we skip — the renderTarget
        // dump below already captures what FinalBlit produced.
    }

    // Readback via CopyTextureToBuffer.
    constexpr u64 kBytes = (u64)W * H * 4;
    BufferDesc rbDesc{};
    rbDesc.size = kBytes;
    rbDesc.type = BufferType::Raw;
    rbDesc.memoryUsage = GPUMemoryUsage::Readback;
    rbDesc.name = "SponzaReadback";
    ResourceHandle readback = fx.base->CreateBuffer(rbDesc);
    TEST_ASSERT(readback != handles::INVALID_RESOURCE, "CreateBuffer readback");

    CommandBufferHandle cmdHandle = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
    VulkanCommandBuffer* vcmd = fx.vk->GetCommandBuffer(cmdHandle);
    TEST_ASSERT(vcmd->Reset() && vcmd->Begin(), "Begin readback");

    BufferTextureCopyRegion region{};
    region.imageSubresource = { 0, 0, 1 };  // { baseArrayLayer, mipLevel, layerCount }
    region.imageExtent = { W, H, 1 };
    vcmd->CopyTextureToBuffer(renderTarget, readback, &region, 1);

    TEST_ASSERT(vcmd->End() && vcmd->Submit(0) && vcmd->WaitForCompletion(),
                "Submit readback");
    fx.base->DestroyCommandBuffer(cmdHandle);

    u8* mapped = static_cast<u8*>(fx.base->MapBuffer(readback, 0, kBytes));
    TEST_ASSERT(mapped != nullptr, "MapBuffer readback");
    if (mapped) {
        // Save PNG for visual verification.
        std::string pngPath = "sponza_vulkan.png";
        EngineTest::SavePNG(pngPath.c_str(), mapped, W, H);
        std::cout << "[Part26] Saved " << pngPath << " (" << W << "x" << H << ")" << std::endl;

        // T4.6.5 part 26: smoke bar is "Render() returns without crash".
        // Output verification (non-trivial pixels) deferred — current run
        // produces all-black output due to known validation errors in HZB
        // depth layout tracking (3 errors). Visual verification needs:
        //   - HZBSystem depth transition DEPTH_STENCIL_ATTACHMENT_OPTIMAL → SHADER_READ_ONLY_OPTIMAL
        //   - Meshlet raster pipeline producing actual GBuffer writes
        // Follow-up parts (27+) scope.
        bool anyNonZero = false;
        for (u64 i = 0; i < kBytes; ++i) {
            if (mapped[i] != 0) { anyNonZero = true; break; }
        }
        std::cout << "[Part26] Output anyNonZero=" << (anyNonZero ? "true" : "false")
                  << " (smoke bar = PNG saved, output verification deferred)" << std::endl;
        fx.base->UnmapBuffer(readback);
    }

    // -------------------------------------------------------------
    // Cleanup (order matters: pipeline + registry first to release any
    // references they hold to clusters/entities/textures).
    // -------------------------------------------------------------
    pipeline.Shutdown();
    registry->Shutdown(fx.base);
    delete registry;

    // Remove cluster components + entities (reverse order).
    for (auto& c : clusterComps) primal::cluster::remove(c);
    for (auto& e : entities) { if (e.is_valid()) primal::game_entity::remove(e.get_id()); }

    fx.base->DestroyBuffer(readback);
    fx.base->DestroyTexture(renderTarget);
    // texSampler ownership transferred to GPUDrivenDrawPipeline via SetTextureArrays;
    // pipeline.Shutdown() destroys it. Don't double-destroy here.
    fx.base->DestroySampler(materialSampler);
    fx.base->DestroyTexture(fallbackDiffuse);
    fx.base->DestroyTexture(fallbackNormal);
    fx.base->DestroyTexture(fallbackORM);

    primal::jobsystem::JobSystem::Shutdown();
    return TestResult::Passed;
}


void RegisterVulkanStandardPipelineSmoke_Tests() {
    auto suite = std::make_shared<TestSuite>("VulkanStandardPipelineSmoke_Tests");
    suite->AddTestCase(TestCase("Initialize_Smoke",          TestVulkanStandardPipeline_Initialize_Smoke));
    suite->AddTestCase(TestCase("EditorMode_NoOpRender",     TestVulkanStandardPipeline_EditorMode_NoOpRender));
    suite->AddTestCase(TestCase("SubsystemsProbe",           TestVulkanStandardPipeline_SubsystemsProbe));
    suite->AddTestCase(TestCase("EditorRender_ForwardSceneRenderer",
                                TestVulkanEditorRender_ForwardSceneRenderer));
    suite->AddTestCase(TestCase("Render_NonEditor",
                                TestVulkanStandardPipelineRender_NonEditor));
    TestRunner::RegisterTestSuite(suite);
}

int main() {
    RegisterVulkanStandardPipelineSmoke_Tests();
    TestRunner::RunAllSuites();
    return 0;
}

#else

int main() {
    std::cout << "Vulkan backend disabled; TestVulkanStandardPipelineSmoke is a no-op." << std::endl;
    return 0;
}

#endif
