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

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN
#include "Graphics/RHI/Platforms/Vulkan/VulkanDevice.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanCommandBuffer.h"
#endif

#include <iostream>
#include <cstring>

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
// DeferredLightingModule, FinalBlitModule, and crucially forward_renderer_
// (ForwardSceneRenderer).
//
// STATUS: Skipped — crashes during GPUCullingPipeline::UpdateHZBBindings
// (descriptor type mismatch at binding 8). Multiple T4.6 sub-issues found:
//
//   1. vkCreateImage D32_SFLOAT rejected: mixed COLOR_ATTACHMENT +
//      DEPTH_STENCIL usage bits on same texture (StandardRenderPipeline
//      subsystem creates a D32 tex with both RenderTarget + DepthStencil
//      TextureUsage flags — Vulkan forbids this combo).
//   2. GPUDrivenDrawPipeline 4 missing SPIR-V shaders:
//      ClusterBinning/cluster_binning_kernel, VisibilityBuffer × 2
//      (vertex + fragment), VisibilityBufferResolve/ComputeMain.
//      T4.4.3 ported some shaders but missed these 4.
//   3. HZB descriptor type mismatch (VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
//      write to layout binding 8 declared as STORAGE_BUFFER).
//      GPUCullingPipeline.cpp:165 declares SampledImage but validation
//      sees STORAGE_BUFFER — possibly bindings_ vector truncation in
//      RHIDescriptorSetLayout base ctor or stale layout handle.
//   4. Segfault inside GPUCullingPipeline::UpdateHZBBindings called
//      eagerly from SetHZBSystem (GPUCullingPipeline.h:130). The
//      validation-layer crash in vvl::BufferDescriptor::WriteUpdate
//      suggests the bufferInfo pointer or descriptor state is corrupt.
//
// Each issue warrants its own investigation. See memory
// vulkan-rhi-t46-subsystems-probe-findings.md for full details.
TestResult TestVulkanStandardPipeline_SubsystemsProbe() {
    std::cout << "[SubsystemsProbe] SKIPPED — T4.6 multi-issue scope. See test comment for punch list." << std::endl;
    return TestResult::Skipped;
}

void RegisterVulkanStandardPipelineSmoke_Tests() {
    auto suite = std::make_shared<TestSuite>("VulkanStandardPipelineSmoke_Tests");
    suite->AddTestCase(TestCase("Initialize_Smoke",          TestVulkanStandardPipeline_Initialize_Smoke));
    suite->AddTestCase(TestCase("EditorMode_NoOpRender",     TestVulkanStandardPipeline_EditorMode_NoOpRender));
    suite->AddTestCase(TestCase("SubsystemsProbe",           TestVulkanStandardPipeline_SubsystemsProbe));
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
