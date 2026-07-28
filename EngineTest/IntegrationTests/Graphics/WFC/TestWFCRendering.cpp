// TestWFCRendering.cpp — WFC Phase A.4 visual demo
//
// Task 2: open a Metal-backed window, set up StandardRenderPipeline + camera +
// empty RenderScene, run a per-frame render loop. The test exits cleanly after
// kHeadlessFrameCap frames so it can run headlessly in CI.
//
// Task 3 (this file): RegisterWFCCatalogMeshes() registers 5 procedural meshes
// (cube/ramp/corner_in/corner_out/pillar) and overrides the WFC catalog's
// placeholder mesh_handles (1000-1004) with real slot indices. RunSolverAndEmit()
// drives a 4x4x4 collapse and drains Collapse steps into wfc_point_set via
// WFCOutput::ConsumeSteps. No entities are spawned — that's Task 4.

#include "TestWFCRendering.h"
#include "Engine/Common/CommonHeaders.h"
#include "Engine/Content/ContentToEngine.h"
#include "Engine/Content/ProceduralMesh.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalDevice.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalMath.h"
#include "Engine/Graphics/RHI/Core/RHITypes.h"
#include "Engine/Graphics/Lumen/LumenTypes.h"
#include "Engine/Graphics/WFC/WFCTileCatalog.h"
#include "Engine/Graphics/WFC/WFCTileRegistry.h"
#include "Engine/Graphics/WFC/TileAdjacency.h"
#include "Engine/Graphics/WFC/WaveGrid.h"
#include "Engine/Graphics/WFC/WFCSolver.h"
#include "Engine/Graphics/WFC/WFCStepBuffer.h"
#include "Engine/Graphics/WFC/WFCConfig.h"
#include "Engine/Graphics/WFC/WFCSolveBudget.h"
#include "Engine/Graphics/WFC/WFCOutput.h"
#include "Engine/Graphics/PCG/PCGTypes.h"

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

    // 4. Pipeline. SetLumenConfig triggers InitializeSubsystems which creates
    //    the ForwardSceneRenderer — without it, GetForwardRenderer() returns
    //    null and RegisterMeshEntity can't slot the procedural meshes.
    //    SetEditorMode(true) routes rendering through the forward path so the
    //    Task 4 entities are actually drawn (matches TestPCGScatter setup).
    pipeline = new StandardRenderPipeline();
    if (!pipeline->Initialize(device.get())) return false;
    pipeline->SetOutputResource(handles::INVALID_RESOURCE, {});
    pipeline->SetViewportSize(window_width_, window_height_);

    lumen::LumenConfig lumenConfig;
    lumenConfig.quality = lumen::LumenQualityPreset::Low;
    pipeline->SetLumenConfig(lumenConfig);
    pipeline->SetEditorMode(true);

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

    // 7. Task 3: register catalog meshes + run the WFC solver. Emits a
    //    PCGPointSet that Task 4 will turn into renderable entities. Done at
    //    the end of Initialize so the pipeline + camera are ready in case
    //    Task 4 needs them.
    RegisterWFCCatalogMeshes();
    RunSolverAndEmit();

    std::cout << "[TestWFCRendering] Pipeline + scene ready" << std::endl;
    return true;
}

// ============================================================================
// WFCRenderingTestCase::RegisterWFCCatalogMeshes
// ============================================================================
//
// Registers 5 procedural meshes (cube/ramp/corner_in/corner_out/pillar) with
// the StandardRenderPipeline and captures their ForwardSceneRenderer slot
// indices. The ramp uses create_ramp_mesh with slope_height=0 (full slope);
// corner_in/corner_out are Phase A.4 simplifications that reuse the cube mesh
// (the catalog still emits them as separate tile IDs so Task 4 can swap in
// dedicated geometry later without touching the solver wiring). Pillar is a
// tall thin box.
//
// Slot indices are captured via GetMeshInfoCount() before/after the 5
// RegisterMeshEntity calls — same pattern TestPCGScatter uses for its
// procedural meshes.

void WFCRenderingTestCase::RegisterWFCCatalogMeshes() {
    using namespace primal::content;

    // Capture the slot base BEFORE registering so we can offset the 5 new
    // meshes regardless of how many meshes were already registered.
    auto* fwd = pipeline ? pipeline->GetForwardRenderer() : nullptr;
    const u32 slot_base = fwd ? fwd->GetMeshInfoCount() : 0;

    // No-texture fallback (same as TestPCGScatter's procedural registration).
    primal::id::id_type noTex[3] = {
        primal::id::invalid_id, primal::id::invalid_id, primal::id::invalid_id
    };

    // Order MUST match the WFCTileCatalog tile_id assignment (0..4):
    //   0=cube, 1=ramp, 2=corner_in, 3=corner_out, 4=pillar
    auto cube_geo       = create_box_mesh(1.0f, 1.0f, 1.0f);
    auto ramp_geo       = create_ramp_mesh(1.0f, 1.0f, 1.0f, 0.0f);  // full ramp
    auto corner_in_geo  = create_box_mesh(1.0f, 1.0f, 1.0f);          // Phase A.4 simplification
    auto corner_out_geo = create_box_mesh(1.0f, 1.0f, 1.0f);          // Phase A.4 simplification
    auto pillar_geo     = create_box_mesh(0.25f, 2.0f, 0.25f);        // tall thin

    pipeline->RegisterMeshEntity(cube_geo,       noTex, 3);
    pipeline->RegisterMeshEntity(ramp_geo,       noTex, 3);
    pipeline->RegisterMeshEntity(corner_in_geo,  noTex, 3);
    pipeline->RegisterMeshEntity(corner_out_geo, noTex, 3);
    pipeline->RegisterMeshEntity(pillar_geo,     noTex, 3);

    // Read back slot indices. Registrations are appended in order.
    if (fwd) {
        const u32 after = fwd->GetMeshInfoCount();
        if (after >= slot_base + 5) {
            slot_cube       = slot_base + 0;
            slot_ramp       = slot_base + 1;
            slot_corner_in  = slot_base + 2;
            slot_corner_out = slot_base + 3;
            slot_pillar     = slot_base + 4;
        }
    }
    std::cout << "[TestWFCRendering] Registered 5 catalog meshes: cube="
              << slot_cube << " ramp=" << slot_ramp
              << " corner_in=" << slot_corner_in
              << " corner_out=" << slot_corner_out
              << " pillar=" << slot_pillar << std::endl;
}

// ============================================================================
// WFCRenderingTestCase::RunSolverAndEmit
// ============================================================================
//
// Builds the WFC catalog (placeholder mesh_handles 1000-1004), overrides those
// placeholders with the real slot indices captured in RegisterWFCCatalogMeshes,
// then runs the solver on a 4x4x4 grid. Collapse steps are drained into
// wfc_point_set via WFCOutput::ConsumeSteps for Task 4 to consume.
//
// Pattern mirrors TestWFC3DParametric (Phase A.3 reference). The only
// additions are the mesh_handle override and storing the emitted point set on
// the test case instead of discarding it.

void WFCRenderingTestCase::RunSolverAndEmit() {
    using namespace primal::graphics::wfc;

    // 1. Build catalog -> registry + adjacency (placeholders 1000-1004).
    WFCTileRegistry reg;
    TileAdjacencyTable adj;
    WFCTileCatalog::Populate(reg, adj);

    // 2. Override placeholder mesh_handles with real slot indices. The
    //    catalog's mesh_handle is a geometry::geometry_id (u32-backed), so a
    //    plain static_cast from the slot is enough — no content system lookup
    //    needed since ForwardSceneRenderer resolves meshes by slot index.
    reg.GetMutable(wfc_tile_id{0}).mesh_handle =
        primal::geometry::geometry_id{slot_cube};
    reg.GetMutable(wfc_tile_id{1}).mesh_handle =
        primal::geometry::geometry_id{slot_ramp};
    reg.GetMutable(wfc_tile_id{2}).mesh_handle =
        primal::geometry::geometry_id{slot_corner_in};
    reg.GetMutable(wfc_tile_id{3}).mesh_handle =
        primal::geometry::geometry_id{slot_corner_out};
    reg.GetMutable(wfc_tile_id{4}).mesh_handle =
        primal::geometry::geometry_id{slot_pillar};

    // 3. Configure solver: 4x4x4 = 64 cells max.
    WFCConfig config;
    config.grid_size = {4, 4, 4};
    config.max_cells_per_frame = 256;
    config.max_ms_per_frame = 1000;
    config.seed = 7;
    config.max_generations = 8;

    WaveGrid grid;
    grid.Initialize(config.grid_size, 8);

    WFCStepBuffer buf;
    WFCSolver solver;
    solver.Initialize(config, grid, reg, adj, buf);

    // 4. Run solver to completion (or GivenUp — Phase A.3 known limitation).
    WFCSolveBudget budget(config.max_cells_per_frame, config.max_ms_per_frame);
    budget.Reset();
    WFCSolver::StepResult result = WFCSolver::StepResult::InProgress;
    u32 steps = 0;
    while (result == WFCSolver::StepResult::InProgress && steps < 1000) {
        result = solver.Step(budget);
        ++steps;
    }

    // 5. Drain Collapse steps into the point set. Task 4 will spawn entities.
    wfc_point_set = WFCOutput::ConsumeSteps(buf, reg, 1.0f);
    std::cout << "[TestWFCRendering] Solver emitted "
              << wfc_point_set.count << " tile instances"
              << " (result=" << static_cast<u32>(result)
              << " steps=" << steps << ")" << std::endl;
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
