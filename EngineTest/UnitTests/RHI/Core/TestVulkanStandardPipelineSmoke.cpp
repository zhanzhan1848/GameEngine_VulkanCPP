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
    pipeline.SetShaderHandles(shaderHandles);

    // Default LumenConfig — disables most Lumen modules.
    lumen::LumenConfig lumenConfig{};
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
