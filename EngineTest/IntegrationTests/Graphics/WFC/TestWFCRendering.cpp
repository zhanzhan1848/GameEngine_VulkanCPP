// TestWFCRendering.cpp — WFC visual demo (Phase A.4 + Phase B.1)
//
// Phase A.4 scaffolding: open a Metal-backed window, set up
// StandardRenderPipeline + camera + RenderScene, register the 5 catalog
// procedural meshes, run the solver, spawn ECS entities from the emitted
// point set, render loop. Exits cleanly after kHeadlessFrameCap frames.
//
// Phase B.1 additions:
// - Dedicated corner geometry (create_corner_in_mesh / create_corner_out_mesh).
// - Interactive Mode toggle: press 'M' to cycle 3D (4x4x4) <-> 2D (4x4x1).
// - CI smoke variant: compile with -DWFC_MODE_2D_SMOKE=1 to start in 2D mode.
// - Catalog built once (SetupWFCCatalog); CycleMode re-solves on the same
//   registry + adjacency.

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
#include "Engine/Graphics/PCG/PCGEntityFactory.h"
#include "Engine/Input/Input.h"

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
    winInfo.caption = "TestWFCRendering - WFC Phase B.1";
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

    // 7. Phase B.1: register catalog meshes once (slots captured on the test
    //    case), build the WFC catalog once (registry_ + adjacency_ owned by
    //    the test case so CycleMode doesn't rebuild), run the solver for the
    //    current mode (3D by default, 2D under WFC_MODE_2D_SMOKE), spawn the
    //    emitted tile instances as ECS entities, then snap the camera to the
    //    mode-appropriate position.
#ifdef WFC_MODE_2D_SMOKE
    mode_ = RenderMode::TwoD;
    std::cout << "[TestWFCRendering] CI smoke mode: starting in 2D" << std::endl;
#endif

    RegisterWFCCatalogMeshes();
    SetupWFCCatalog();
    RunSolverForCurrentMode();
    SpawnEntitiesForCurrentMode();

    // Snap camera to mode-appropriate position.
    SnapCameraForCurrentMode();

    std::cout << "[TestWFCRendering] Pipeline + scene ready" << std::endl;
    return true;
}

// ============================================================================
// WFCRenderingTestCase::SpawnEntitiesForCurrentMode
// ============================================================================
//
// Converts wfc_point_set (populated by RunSolverForCurrentMode) into ECS
// Entities via PCGEntityFactory::CreateEntities, then pushes the entity_ids +
// mesh_slot indices into the pipeline. Pattern mirrors TestPCGScatter's
// integration.
//
// Key difference from TestPCGScatter: we do NOT offset mesh_slot_indices.
// SetupWFCCatalog already overwrote the WFC catalog's placeholder
// mesh_handles (sentinel 1000-1004) with the real ForwardSceneRenderer slot
// indices captured in RegisterWFCCatalogMeshes, and WFCOutput::ConsumeSteps
// writes tile.mesh_handle into the MeshIndex attr verbatim — so the slot
// indices coming out of CreateEntities are already correct. TestPCGScatter
// adds += procedural_slot_base_ because its scatter graph emits abstract
// {0,1,2} tags that need to be offset to its procedural mesh range; the WFC
// path bakes the real slot in at catalog-build time.

void WFCRenderingTestCase::SpawnEntitiesForCurrentMode() {
    using namespace primal::graphics::pcg;

    if (wfc_point_set.count == 0) {
        std::cout << "[TestWFCRendering] SpawnEntitiesForCurrentMode: empty point set, "
                  << "skipping" << std::endl;
        return;
    }

    // CreateEntities mints one Entity per point with a Transform (pos/rot/scale
    // from the point's attrs) and records MeshIndex into mesh_slot_indices.
    auto result = PCGEntityFactory::CreateEntities(wfc_point_set);

    // Keep a copy on the test case for Shutdown cleanup. Then hand the
    // originals to the pipeline by move so we don't hold two live copies
    // through the render loop. (Mirrors ReScatterPCG in TestPCGScatter.)
    wfc_entity_ids = result.entity_ids;
    wfc_mesh_slots = result.mesh_slot_indices;
    pipeline->SetPCGEntities(std::move(result.entity_ids),
                             std::move(result.mesh_slot_indices));

    std::cout << "[TestWFCRendering] Spawned " << wfc_entity_ids.size()
              << " WFC entities (mode="
              << (mode_ == RenderMode::TwoD ? "2D" : "3D") << ")"
              << std::endl;
}

// ============================================================================
// WFCRenderingTestCase::RegisterWFCCatalogMeshes
// ============================================================================
//
// Registers 5 procedural meshes (cube/ramp/corner_in/corner_out/pillar) with
// the StandardRenderPipeline and captures their ForwardSceneRenderer slot
// indices. The ramp uses create_ramp_mesh with slope_height=0 (full slope);
// corner_in/corner_out use dedicated Phase B.1 generators (L-shape and
// octant frame respectively, each with 4 rotation variants via RotationY).
// Pillar is a tall thin box.
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
    auto corner_in_geo  = create_corner_in_mesh(1.0f, 1.0f, 1.0f);   // Phase B.1: dedicated L-shape
    auto corner_out_geo = create_corner_out_mesh(1.0f, 1.0f, 1.0f);  // Phase B.1: dedicated octant frame
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
// WFCRenderingTestCase::SetupWFCCatalog
// ============================================================================
//
// Phase B.1: extracted from the old RunSolverAndEmit. Builds the WFC catalog
// once (registry_ + adjacency_ owned by the test case) and overrides the
// placeholder mesh_handles with the real ForwardSceneRenderer slot indices
// captured in RegisterWFCCatalogMeshes. Called once from Initialize so
// CycleMode can re-solve without rebuilding the catalog each toggle.

void WFCRenderingTestCase::SetupWFCCatalog() {
    using namespace primal::graphics::wfc;
    registry_ = std::make_unique<WFCTileRegistry>();
    adjacency_ = std::make_unique<TileAdjacencyTable>();
    WFCTileCatalog::Populate(*registry_, *adjacency_);

    // Override placeholder mesh_handles with real slot indices captured in
    // RegisterWFCCatalogMeshes. Same logic as the old RunSolverAndEmit lines
    // 269-278, but now runs once and is reused by CycleMode.
    registry_->GetMutable(wfc_tile_id{0}).mesh_handle =
        primal::geometry::geometry_id{slot_cube};
    registry_->GetMutable(wfc_tile_id{1}).mesh_handle =
        primal::geometry::geometry_id{slot_ramp};
    registry_->GetMutable(wfc_tile_id{2}).mesh_handle =
        primal::geometry::geometry_id{slot_corner_in};
    registry_->GetMutable(wfc_tile_id{3}).mesh_handle =
        primal::geometry::geometry_id{slot_corner_out};
    registry_->GetMutable(wfc_tile_id{4}).mesh_handle =
        primal::geometry::geometry_id{slot_pillar};

    std::cout << "[TestWFCRendering] Catalog ready (5 tiles, "
              << registry_->Count() << " registered)" << std::endl;
}

// ============================================================================
// WFCRenderingTestCase::RunSolverForCurrentMode
// ============================================================================
//
// Phase B.1: mode-aware grid size (3D=4x4x4, 2D=4x4x1). Runs the solver on
// registry_ + adjacency_ (built once in SetupWFCCatalog), drains Collapse
// steps into wfc_point_set.
//
// Architectural caveat (plan Step 4 Note): the solver internally always
// passes WFC_FACE_COUNT_3D. The 2D path still works correctly because the 2D
// grid has grid_size.z=1, so ±Z neighbors are always out-of-bounds and get
// filtered by the OOB check in WFCPropagator. Threading face_count through
// the solver is a Phase B.2 follow-up.

void WFCRenderingTestCase::RunSolverForCurrentMode() {
    using namespace primal::graphics::wfc;

    WFCConfig config;
    if (mode_ == RenderMode::TwoD) {
        config.grid_size = {4, 4, 1};
    } else {
        config.grid_size = {4, 4, 4};
    }
    config.max_cells_per_frame = 256;
    config.max_ms_per_frame = 1000;
    config.seed = 7;  // Same seed for both modes (fair A/B comparison)
    config.max_generations = 8;

    WaveGrid grid;
    grid.Initialize(config.grid_size, 8);

    WFCStepBuffer buf;
    WFCSolver solver;
    solver.Initialize(config, grid, *registry_, *adjacency_, buf);

    WFCSolveBudget budget(config.max_cells_per_frame, config.max_ms_per_frame);
    budget.Reset();
    WFCSolver::StepResult result = WFCSolver::StepResult::InProgress;
    u32 steps = 0;
    while (result == WFCSolver::StepResult::InProgress && steps < 1000) {
        result = solver.Step(budget);
        ++steps;
    }

    wfc_point_set = WFCOutput::ConsumeSteps(buf, *registry_, 1.0f);
    std::cout << "[TestWFCRendering] Solver emitted "
              << wfc_point_set.count << " tile instances"
              << " (mode=" << (mode_ == RenderMode::TwoD ? "2D" : "3D")
              << " result=" << static_cast<u32>(result)
              << " steps=" << steps << ")" << std::endl;
}

// ============================================================================
// WFCRenderingTestCase::CycleMode
// ============================================================================
//
// Phase B.1: toggle ThreeD <-> TwoD. Destroys current ECS entities + pipeline
// proxies, flips mode_, re-solves on the new grid size, re-spawns entities,
// and snaps the camera. Registry/adjacency are preserved (built once in
// SetupWFCCatalog) — same seed (7) means only grid shape differs between modes.

void WFCRenderingTestCase::CycleMode() {
    using namespace primal::graphics::pcg;
    using namespace primal::graphics::wfc;

    // 1. Destroy current entities (ECS + pipeline proxies).
    if (pipeline) pipeline->ClearPCGEntities();
    if (!wfc_entity_ids.empty()) {
        PCGEntityFactory::DestroyEntities(wfc_entity_ids);
        wfc_entity_ids.clear();
        wfc_mesh_slots.clear();
    }

    // 2. Flip mode.
    mode_ = (mode_ == RenderMode::ThreeD) ? RenderMode::TwoD : RenderMode::ThreeD;

    // 3. Re-solve + re-spawn. Registry/adjacency are preserved (built once in
    //    SetupWFCCatalog). Same seed (7) -> only grid shape differs.
    RunSolverForCurrentMode();
    SpawnEntitiesForCurrentMode();
    SnapCameraForCurrentMode();

    std::cout << "[TestWFCRendering] Mode cycled to "
              << (mode_ == RenderMode::TwoD ? "2D (4x4x1)" : "3D (4x4x4)")
              << std::endl;
}

// ============================================================================
// WFCRenderingTestCase::Run
// ============================================================================

void WFCRenderingTestCase::Run() {
    if (!pipeline || !scene || !view) return;

    // Phase B.1: key M cycles between 3D (4x4x4) and 2D (4x4x1) modes.
    // Rising-edge detection — one cycle per key press, not per frame.
    {
        using namespace primal::input;
        input_value val{};
        get(input_source::keyboard, input_code::key_m, val);
        if (val.current.x > 0.0f) {
            if (!key_m_pressed_) {
                key_m_pressed_ = true;
                CycleMode();
            }
        } else {
            key_m_pressed_ = false;
        }
    }

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

    // Task 4 entities: drop them from the pipeline first so Render() stops
    // syncing RenderProxies for IDs we're about to invalidate, then destroy
    // the ECS Entities themselves. Order matters — ClearPCGEntities just
    // clears the id list on the pipeline, DestroyEntities actually removes
    // the Entities from the game_entity component pool.
    if (pipeline) {
        pipeline->ClearPCGEntities();
    }
    if (!wfc_entity_ids.empty()) {
        primal::graphics::pcg::PCGEntityFactory::DestroyEntities(wfc_entity_ids);
        wfc_entity_ids.clear();
        wfc_mesh_slots.clear();
    }

    // Phase B.1: release catalog before pipeline (registry mesh_handles point
    // at ForwardSceneRenderer slots that the pipeline owns).
    registry_.reset();
    adjacency_.reset();

    if (pipeline) {
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
// WFCRenderingTestCase::SnapCameraForCurrentMode
// ============================================================================
//
// Phase B.1: sets camera_pos_/yaw_/pitch_ based on mode_. Called from
// Initialize + CycleMode. UpdateCamera applies the state to the RenderView.

void WFCRenderingTestCase::SnapCameraForCurrentMode() {
    if (mode_ == RenderMode::TwoD) {
        // 2D grid on XY plane (z=0). View from +Z toward origin.
        // pos (2,2,6) -> forward = (0,0,-1). yaw=0, pitch=0.
        camera_pos_ = primal::math::v3{2.0f, 2.0f, 6.0f};
        camera_yaw_ = 0.0f;
        camera_pitch_ = 0.0f;
    } else {
        camera_pos_ = primal::math::v3{8.0f, 8.0f, 8.0f};
        camera_yaw_ = 0.0f;
        camera_pitch_ = -0.4f;
    }
    UpdateCamera();
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
