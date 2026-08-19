/**
 * @file TestMetalForwardRendererIntegration.cpp
 * @brief P4c §4 性能门槛 — ForwardRenderer 集成帧耗时(Metal 侧)
 * @details TestVulkanForwardRendererIntegration::FrameTiming 的 Metal 孪生:
 *          同 workload(64x64 RGBA16F + D32、1 平行光空场景、Reset/Begin/
 *          Render/End/Submit/Wait 逐帧)300 帧平均耗时,输出
 *          [P4C_PERF_METAL=...] 供 ±5% 对比。
 */

#ifdef __APPLE__

#include "../../TestFramework.h"

// 包含顺序对齐 TestMetalSimplePBR(Metal 平台头先于 graphics 层头,
// 规避 MacTypes ::Rect 与 rhi::Rect 的二义)
#include "Engine/Graphics/RHI/Core/RHITypes.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalDevice.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalCommandBuffer.h"
#include "Engine/Graphics/RHI/Core/RHIDeviceFactory.h"
#include "Engine/Graphics/RHI/Core/RHICommand.h"
#include "Engine/Graphics/ForwardRenderer.h"
#include "Engine/Graphics/RenderScene.h"
#include "Engine/Graphics/RenderView.h"

#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <unordered_map>

using namespace primal::graphics::rhi;
using namespace primal::math;
using primal::graphics::ForwardRenderer;
using primal::graphics::RenderScene;
using primal::graphics::RenderLight;
using primal::graphics::RenderView;
using primal::graphics::LightType;
using primal::graphics::MaterialInstance;
using namespace Engine::Test;

namespace {

m4x4 make_identity_m4x4_m() {
    m4x4 r{};
    std::memset(&r, 0, sizeof(r));
    r.columns[0][0] = 1.0f;
    r.columns[1][1] = 1.0f;
    r.columns[2][2] = 1.0f;
    r.columns[3][3] = 1.0f;
    return r;
}

} // anonymous namespace

TestResult TestForwardRendererIntegration_FrameTiming_Metal() {
    DeviceDesc dd{};
    dd.platform = RHIPlatform::Metal;
    dd.enableDebug = true;
    MetalDevice device(dd);
    if (!device.Initialize()) {
        std::cerr << "[MetalFRTiming] MetalDevice init failed" << std::endl;
        return TestResult::Failed;
    }

    constexpr u32 W = 64, H = 64;
    TextureDesc rtDesc{
        {W, H, 1}, 1, 1, DataFormat::RGBA16_Float, TextureType::Texture2D,
        TextureUsage::RenderTarget | TextureUsage::CopySource | TextureUsage::ShaderResource,
        GPUMemoryUsage::Static, "TimingRT"};
    ResourceHandle renderTarget = device.CreateTexture(rtDesc);
    TEST_ASSERT(renderTarget != handles::INVALID_RESOURCE, "CreateTexture renderTarget");
    TextureDesc depthDesc{
        {W, H, 1}, 1, 1, DataFormat::D32_Float, TextureType::Texture2D,
        TextureUsage::DepthStencil | TextureUsage::CopySource | TextureUsage::ShaderResource,
        GPUMemoryUsage::Static, "TimingDepth"};
    ResourceHandle depth = device.CreateTexture(depthDesc);
    TEST_ASSERT(depth != handles::INVALID_RESOURCE, "CreateTexture depth");

    ForwardRenderer renderer;
    TEST_ASSERT(renderer.Initialize(&device), "ForwardRenderer::Initialize on Metal");

    RenderScene scene;
    RenderLight light;
    light.type = LightType::Directional;
    light.direction = v3{0.0f, -1.0f, 0.0f};
    light.color = v3{1.0f, 1.0f, 1.0f};
    light.intensity = 1.0f;
    scene.AddLight(light);

    RenderView view;
    m4x4 viewMat = make_identity_m4x4_m();
    viewMat.columns[3][2] = 5.0f;
    view.SetViewMatrix(viewMat);
    constexpr float pi = 3.14159265358979323846f;
    float fov = 60.0f * (pi / 180.0f);
    float aspect = float(W) / float(H);
    float f = 1.0f / std::tan(fov * 0.5f);
    m4x4 proj{};
    std::memset(&proj, 0, sizeof(proj));
    proj.columns[0][0] = f / aspect;
    proj.columns[1][1] = f;
    proj.columns[2][2] = 50.0f / (0.1f - 100.0f);
    proj.columns[2][3] = 1.0f;
    proj.columns[3][2] = -(0.1f * 100.0f) / (0.1f - 100.0f);
    view.SetProjectionMatrix(proj);
    view.Cull(scene);

    CommandBufferHandle cmd = device.CreateCommandBuffer(CommandQueueType::Graphics);
    MetalCommandBuffer* mcmd = device.GetCommandBuffer(cmd);
    TEST_ASSERT(mcmd != nullptr, "GetCommandBuffer");

    std::unordered_map<primal::id::id_type, std::shared_ptr<MaterialInstance>> emptyMaterials;
    auto renderOneFrame = [&]() {
        TEST_ASSERT(mcmd->Reset() && mcmd->Begin(), "Begin");
        renderer.Render(mcmd, scene, view, renderTarget,
                        handles::INVALID_RESOURCE, depth, emptyMaterials, 0, W, H);
        TEST_ASSERT(mcmd->End() && mcmd->Submit(0) && mcmd->WaitForCompletion(), "Submit");
    };

    constexpr u32 kWarmup = 30;
    constexpr u32 kFrames = 300;
    for (u32 i = 0; i < kWarmup; ++i) renderOneFrame();

    auto t0 = std::chrono::high_resolution_clock::now();
    for (u32 i = 0; i < kFrames; ++i) renderOneFrame();
    auto t1 = std::chrono::high_resolution_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // 空帧基线(Begin/End/Submit/Wait,无 Render)— 分离框架同步开销
    auto emptyFrame = [&]() {
        TEST_ASSERT(mcmd->Reset() && mcmd->Begin(), "Begin(empty)");
        TEST_ASSERT(mcmd->End() && mcmd->Submit(0) && mcmd->WaitForCompletion(), "Submit(empty)");
    };
    for (u32 i = 0; i < kWarmup; ++i) emptyFrame();
    auto e0 = std::chrono::high_resolution_clock::now();
    for (u32 i = 0; i < kFrames; ++i) emptyFrame();
    auto e1 = std::chrono::high_resolution_clock::now();
    const double ems = std::chrono::duration<double, std::milli>(e1 - e0).count();

    std::cout << "[ForwardRendererTiming] Metal FRAME_TIME_AVG = "
              << (ms / kFrames) << " ms/frame (" << kFrames << " frames)"
              << " [P4C_PERF_METAL=" << (ms / kFrames) << "]"
              << " empty-frame=" << (ems / kFrames) << std::endl;

    device.DestroyCommandBuffer(cmd);
    return TestResult::Passed;
}

int main() {
    auto suite = std::make_shared<TestSuite>("MetalForwardRendererIntegrationTests");
    suite->AddTestCase(TestCase("FrameTiming", TestForwardRendererIntegration_FrameTiming_Metal));
    TestRunner::RegisterTestSuite(suite);
    TestRunner::RunAllSuites();
    return 0;
}

#else // !__APPLE__

#include <iostream>

int main() {
    std::cout << "[TestMetalForwardRendererIntegration] Not Apple platform — no-op." << std::endl;
    return 0;
}

#endif
