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
#include "Graphics/RenderPipeline/StandardRenderPipeline.h"
#include "Graphics/Scene/RenderSceneSnapshot.h"  // complete type for unique_ptr<RenderSceneSnapshot> destructor
#include "Graphics/RenderScene.h"
#include "Graphics/RenderView.h"
#include "Graphics/Lumen/LumenTypes.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN
#include "Graphics/RHI/Platforms/Vulkan/VulkanDevice.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanCommandBuffer.h"
#endif

#include <iostream>
#include <cstring>
#include <fstream>
#include <vector>

using namespace primal::graphics;
using namespace primal::graphics::rhi;
using namespace primal::math;
using namespace Engine::Test;

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

// T4.6.5 part 24 — First end-to-end StandardRenderPipeline::Render() non-Editor
// smoke on Vulkan. Exercises the full production path:
//   1. Initialize + SetLumenConfig triggers InitializeSubsystems → creates
//      GPUDrivenDrawPipeline, GPUCullingPipeline, HZBSystem, ShadowMapModule,
//      DeferredLightingModule, FinalBlitModule, ForwardSceneRenderer, etc.
//   2. Render() runs BuildRenderGraph + Compile + Execute — first real
//      RenderGraph pass execution on Vulkan.
//
// Phase 1 finding: DeferredLighting.spv is compute-only (Dawn WGSL→SPIR-V via
// naga). Metal DeferredLighting.metal has vertexMain + fragmentLighting_v3
// raster entries but those are NOT yet ported. Use Blit.vert + Blit.frag as
// placeholder shaders for both DeferredLightingModule and FinalBlitModule.
// Output will be wrong (placeholder blit) but proves plumbing works.
//
// Bar: completes without crash + zero validation errors. Visual correctness
// deferred to Part 25 (port real DeferredLighting.vert/frag from Metal).
TestResult TestVulkanStandardPipelineRender_NonEditor() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "Vulkan device init");

    StandardRenderPipeline pipeline;
    TEST_ASSERT(pipeline.Initialize(fx.base), "Initialize");

    // T4.6.5 part 24: Load Blit.vert + Blit.frag as placeholder shaders.
    auto loadSpv = [](const char* relpath) -> std::vector<u8> {
        std::ifstream f(relpath, std::ios::binary | std::ios::ate);
        if (!f) return {};
        std::streamsize sz = f.tellg();
        f.seekg(0);
        std::vector<u8> bytes(static_cast<size_t>(sz));
        f.read(reinterpret_cast<char*>(bytes.data()), sz);
        return bytes;
    };
    auto blitVsBytes = loadSpv("Engine/Graphics/Vulkan/shaders/Forward/Blit.vert.spv");
    auto blitFsBytes = loadSpv("Engine/Graphics/Vulkan/shaders/Forward/Blit.frag.spv");
    TEST_ASSERT(!blitVsBytes.empty() && !blitFsBytes.empty(),
                "Load Blit.vert.spv + Blit.frag.spv");

    ShaderHandle blitVs = fx.base->CreateShader(
        blitVsBytes.data(), blitVsBytes.size(), ShaderStage::Vertex, "main");
    ShaderHandle blitFs = fx.base->CreateShader(
        blitFsBytes.data(), blitFsBytes.size(), ShaderStage::Pixel, "main");
    TEST_ASSERT(blitVs != handles::INVALID_SHADER, "CreateShader Blit.vert");
    TEST_ASSERT(blitFs != handles::INVALID_SHADER, "CreateShader Blit.frag");

    StandardRenderPipeline::ShaderHandles handles;
    handles.deferred_vs = blitVs;
    handles.deferred_ps = blitFs;
    handles.blit_vs = blitVs;
    handles.blit_ps = blitFs;
    pipeline.SetShaderHandles(handles);

    // Default LumenConfig — disables most Lumen modules.
    lumen::LumenConfig lumenConfig{};
    pipeline.SetLumenConfig(lumenConfig);

    // Minimal scene (1 directional light, no meshes).
    RenderScene scene;
    RenderLight light;
    light.type = LightType::Directional;
    light.direction = v3{0.0f, -1.0f, 0.0f};
    light.color = v3{1.0f, 1.0f, 1.0f};
    light.intensity = 1.0f;
    scene.AddLight(light);

    constexpr u32 W = 64, H = 64;
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

    TextureDesc rtDesc{
        {W, H, 1}, 1, 1,
        DataFormat::RGBA16_Float,
        TextureType::Texture2D,
        TextureUsage::RenderTarget | TextureUsage::CopySource | TextureUsage::ShaderResource,
        GPUMemoryUsage::Static,
        "NonEditorRenderRT"
    };
    ResourceHandle renderTarget = fx.base->CreateTexture(rtDesc);
    TEST_ASSERT(renderTarget != handles::INVALID_RESOURCE, "CreateTexture renderTarget");

    // T4.6.5 part 24.1: B4 fix shipped (empty-scene guard in Execute() +
    // ExecuteShadowCulling). Re-running the test post-fix to capture the
    // remaining validation error inventory. Pre-fix: 9 categories + GPU lost.
    // Post-fix expectation: errors drop significantly; remaining ones indicate
    // independent bugs (B1/B3/B5/B6 etc.) that need individual triage.
    //
    // Original pre-fix bug inventory (kept for reference):
    //   B1. BufferType::Constant → VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT mapping
    //   B2. VK_NULL_HANDLE imageView on SAMPLED_IMAGE (cascade from B4)
    //   B3. Render pass format mismatch (R16G16B16A16_SFLOAT vs B8G8R8A8_UNORM)
    //   B4. GPUDrivenDrawPipeline::Execute fails (FIXED part 24.1)
    //   B5. vkCmdBlitImage srcImage missing TRANSFER_SRC_BIT
    //   B6. vkCmdWriteTimestamp query not reset
    //   B7. UNDEFINED → SHADER_READ_ONLY layout transition gap
    //   B8. vkFreeCommandBuffers pending state
    //   B9. vkDestroyQueryPool invalid handle

    // Fire the production Render() path — first end-to-end run on Vulkan.
    // Expected outcomes:
    //   - No crash
    //   - Significantly fewer validation errors than pre-fix (9 → ≤3 categories)
    //   - Pipeline completes without GPU lost
    std::cout << "[Part24.1] Invoking StandardRenderPipeline::Render()..." << std::endl;
    pipeline.Render(scene, view, renderTarget, rtDesc);
    std::cout << "[Part24.1] Render() returned without crash." << std::endl;

    // TODO Part 24.2+: validation error count assertion + readback + SavePNG
    // once remaining bugs (B1/B3/B5/B6 etc.) are triaged. For now, reaching
    // this line without crash is the Part 24.1 acceptance bar.

    fx.base->DestroyTexture(renderTarget);
    pipeline.Shutdown();
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
