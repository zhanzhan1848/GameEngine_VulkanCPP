// TestWFCRendering.cpp — WFC Phase A.4 visual demo
//
// Task 2: open a Metal-backed window, set up StandardRenderPipeline + camera +
// empty RenderScene, run a per-frame render loop. The test exits cleanly after
// kHeadlessFrameCap frames so it can run headlessly in CI.
//
// Task 3 will insert RegisterWFCCatalogMeshes() before Run() to populate the
// pipeline's procedural mesh slots from WFCTileCatalog. Task 4 will insert
// RunSolverAndEmit() + SpawnWFCEntities() to drive a 4x4x4 collapse and turn
// the emitted PCGPointSet into renderable entities.

#include "TestWFCRendering.h"
#include "Engine/Common/CommonHeaders.h"
#include "Engine/Content/ContentToEngine.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalDevice.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalMath.h"
#include "Engine/Graphics/RHI/Core/RHITypes.h"

#ifdef __APPLE__
#include <AppKit/AppKit.hpp>
#endif

#include <iostream>
#include <cmath>

using namespace primal::graphics;
using namespace primal::graphics::rhi;
using namespace primal::math;

// ============================================================================
// Engine_Test
// ============================================================================

Engine_Test::Engine_Test()
    : primal::test::RenderTestRunner(std::make_unique<WFCRenderingTestCase>())
{}

// ============================================================================
// WFCRenderingTestCase::Initialize
// ============================================================================

bool WFCRenderingTestCase::Initialize() {
    std::cout << "[TestWFCRendering] Initializing..." << std::endl;

    // 1. Window
    primal::platform::window_init_info winInfo{};
    winInfo.caption = "TestWFCRendering - WFC Phase A.4";
    winInfo.width = window_width_;
    winInfo.height = window_height_;
    window = primal::platform::create_window(&winInfo);
    if (!window.is_valid()) return false;

    // 2. Metal device + register with global device manager (pipeline lookups
    //    go through rhi::g_deviceManager).
    DeviceDesc desc;
    desc.platform = RHIPlatform::Metal;
    desc.enableDebug = true;
    auto* metalDevice = new MetalDevice(desc);
    if (!metalDevice || !metalDevice->Initialize()) {
        delete metalDevice;
        return false;
    }
    device.reset(metalDevice);
    g_deviceManager.RegisterDevice(device.get());

    // 3. RenderSystem (owning swap chain + per-frame command buffers)
    RenderSystemInitInfo sysInfo;
    sysInfo.device = device.get();
    sysInfo.window = window.handle();
    sysInfo.width = window_width_;
    sysInfo.height = window_height_;
    if (!renderSystem.Initialize(sysInfo)) return false;

    // 4. Pipeline
    pipeline = new StandardRenderPipeline();
    if (!pipeline->Initialize(device.get())) return false;
    pipeline->SetOutputResource(handles::INVALID_RESOURCE, {});
    pipeline->SetViewportSize(window_width_, window_height_);

    // 5. Empty scene. Task 3 will register catalog meshes via the pipeline and
    //    Task 4 will spawn WFC entities; for now the renderer just clears the
    //    back buffer so we can verify the Metal window + pipeline wire-up.
    scene = new RenderScene();

    // 6. Camera/view
    view = new RenderView();
    ViewportDesc viewport;
    viewport.size.x = static_cast<float>(window_width_);
    viewport.size.y = static_cast<float>(window_height_);
    view->SetViewport(viewport);
    UpdateCamera();

    std::cout << "[TestWFCRendering] Pipeline + scene ready" << std::endl;
    return true;
}

// ============================================================================
// WFCRenderingTestCase::Run
// ============================================================================

void WFCRenderingTestCase::Run() {
    if (!pipeline || !scene || !view) return;

    UpdateCamera();
    view->UpdateFrustum();
    view->Cull(*scene);

    // BeginFrame yields the per-frame back buffer + the fence the pipeline
    // must signal on submit. Failure means the swap chain isn't ready —
    // skip this frame without incrementing the counter.
    ResourceHandle backBuffer;
    SyncHandle signalFence;
    if (!renderSystem.BeginFrame(backBuffer, signalFence)) return;

    auto* cmd = renderSystem.GetCurrentCommandBuffer();
    auto cmdHandle = renderSystem.GetCurrentCommandBufferHandle();
    u32 bufferIndex = renderSystem.GetCurrentFrameIndex();

    cmd->Reset();
    cmd->Begin();

    pipeline->RenderWithCommandBuffer(
        *scene, *view, backBuffer, renderSystem.GetBackBufferDesc(),
        cmd, bufferIndex, cmdHandle, signalFence);

    cmd->End();

    QueueSubmitInfo submitInfo{};
    submitInfo.cmdBuffer = cmdHandle;
    submitInfo.signalFence = signalFence;
    device->Submit(submitInfo);

    renderSystem.EndFrame();

    ++frame_count_;
    if (frame_count_ >= kHeadlessFrameCap) {
        std::cout << "[TestWFCRendering] Rendered " << frame_count_
                  << " frames" << std::endl;
#ifdef __APPLE__
        // Tear down engine state BEFORE AppKit starts closing windows.
        // NS::Application::terminate() drives AppKit's window teardown which
        // destroys the CAMetalLayer; if engine globals (metal::detail's
        // surface_collection free_list, etc.) still hold entries when that
        // runs, ~free_list asserts. Calling Shutdown() here lets us release
        // the pipeline + scene + device while the window is still alive,
        // so by the time AppKit closes the window the free_lists are empty.
        Shutdown();
        NS::Application::sharedApplication()->terminate(nullptr);
#endif
    }
}

// ============================================================================
// WFCRenderingTestCase::Shutdown
// ============================================================================

void WFCRenderingTestCase::Shutdown() {
    if (!pipeline && !scene) return;  // already shut down — idempotent
    std::cout << "[TestWFCRendering] Shutting down..." << std::endl;

    // Task 4 will own wfc_entity_ids — for now the vector stays empty.
    if (pipeline) {
        pipeline->ClearPCGEntities();
        pipeline->Shutdown();
        delete pipeline;
        pipeline = nullptr;
    }
    renderSystem.Shutdown();
    if (scene) { delete scene; scene = nullptr; }
    if (view)  { delete view;  view = nullptr; }
    if (window.is_valid()) {
        primal::platform::remove_window(window.get_id());
    }
    device.reset();
    // Mirror TestPCGScatter teardown: content subsystem holds free_lists that
    // assert on destruction if entries remain.
    primal::content::shutdown();
}

// ============================================================================
// WFCRenderingTestCase::UpdateCamera
// ============================================================================

void WFCRenderingTestCase::UpdateCamera() {
    // Static angled view of world origin so Task 4's grid (centered at origin)
    // is immediately visible. Mirrors TestPCGScatter::UpdateCamera math.
    float cy = std::cos(camera_yaw_);
    float sy = std::sin(camera_yaw_);
    float cp = std::cos(camera_pitch_);
    float sp = std::sin(camera_pitch_);
    v3 forward{sy * cp, sp, -cy * cp};
    v3 up{0, 1, 0};
    v3 target = camera_pos_ + simd_normalize(forward);
    m4x4 viewMat = metal::CreateLookAtMatrix(camera_pos_, target, up);
    constexpr float fov = 60.0f * (pi / 180.0f);
    const float aspect = static_cast<float>(window_width_) /
                         static_cast<float>(window_height_);
    m4x4 projMat = metal::CreatePerspectiveMatrix(fov, aspect, 0.1f, 1000.0f);
    if (view) {
        view->SetViewMatrix(viewMat);
        view->SetProjectionMatrix(projMat);
    }
}
