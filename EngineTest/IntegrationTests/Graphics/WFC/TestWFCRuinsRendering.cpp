// TestWFCRuinsRendering.cpp — WFC Phase C.1 T27 visual smoke binary.
//
// Boots Metal-backed window + StandardRenderPipeline + 15-tile catalog
// (5 primitive + 10 ruins placeholder meshes), runs an 8×4×8 Ruins-only
// solve to completion, spawns collapsed cells as ECS entities, renders
// kHeadlessFrameCap frames, then exits.
//
// Smoke assertion: ≥ 50 cells collapsed. Full collapse (256 cells) is
// expected with T25's ruins-vertical-wildcard fix in place; anything
// less than 50 indicates a real catalog bug.
//
// Cloned from TestWFCRendering.cpp (Phase A.4 + B.1 + B.2). Differences:
//   * Single mode (3D, 8×4×8) — no 2D toggle.
//   * Solver runs to completion in Initialize; Run() only renders.
//   * No 'R'/'M'/'Space' interactivity.
//   * 15 mesh slots vs 5 (registers 10 ruins placeholder meshes).

#include "TestWFCRuinsRendering.h"
#include "Engine/Common/CommonHeaders.h"
#include "Engine/Content/ContentToEngine.h"
#include "Engine/Content/ProceduralMesh.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalDevice.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalMath.h"
#include "Engine/Graphics/RHI/Core/RHITypes.h"
#include "Engine/Graphics/Lumen/LumenTypes.h"
#include "Engine/Graphics/WFC/WFCTileCatalog.h"
#include "Engine/Graphics/WFC/WaveGrid.h"
#include "Engine/Graphics/WFC/WFCSolver.h"
#include "Engine/Graphics/WFC/WFCStepBuffer.h"
#include "Engine/Graphics/WFC/WFCConfig.h"
#include "Engine/Graphics/WFC/WFCSolveBudget.h"
#include "Engine/Graphics/WFC/WFCOutput.h"
#include "Engine/Graphics/WFC/WFCCategory.h"
#include "Engine/Graphics/PCG/PCGTypes.h"
#include "Engine/Graphics/PCG/PCGEntityFactory.h"

#ifdef __APPLE__
#include <AppKit/AppKit.hpp>
#endif

#include <iostream>
#include <cmath>
#include <cassert>

using namespace primal::graphics;
using namespace primal::graphics::rhi;
using namespace primal::math;

// ============================================================================
// Engine_Test
// ============================================================================

Engine_Test::Engine_Test()
    : primal::test::RenderTestRunner(std::make_unique<WFCRuinsRenderingTestCase>())
{}

// ============================================================================
// WFCRuinsRenderingTestCase ctor
// ============================================================================

WFCRuinsRenderingTestCase::WFCRuinsRenderingTestCase()
{}

// ============================================================================
// Initialize
// ============================================================================

bool WFCRuinsRenderingTestCase::Initialize() {
    std::cout << "[TestWFCRuinsRendering] Initializing..." << std::endl;

    // 1. Window
    primal::platform::window_init_info winInfo{};
    winInfo.caption = "TestWFCRuinsRendering - WFC Phase C.1";
    winInfo.width = window_width_;
    winInfo.height = window_height_;
    window = primal::platform::create_window(&winInfo);
    if (!window.is_valid()) return false;

    // 2. Metal device + register with global device manager.
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

    // 4. Pipeline.
    pipeline = new StandardRenderPipeline();
    if (!pipeline->Initialize(device.get())) return false;
    pipeline->SetOutputResource(handles::INVALID_RESOURCE, {});
    pipeline->SetViewportSize(window_width_, window_height_);

    lumen::LumenConfig lumenConfig;
    lumenConfig.quality = lumen::LumenQualityPreset::Low;
    pipeline->SetLumenConfig(lumenConfig);
    pipeline->SetEditorMode(true);

    // 5. Empty scene + camera/view.
    scene = new RenderScene();
    view = new RenderView();
    ViewportDesc viewport;
    viewport.size.x = static_cast<float>(window_width_);
    viewport.size.y = static_cast<float>(window_height_);
    view->SetViewport(viewport);

    // 6. Register all 15 catalog meshes + build the 15-tile catalog with
    //    placeholder overrides pointing at the registered slot indices.
    RegisterWFCCatalogMeshes();
    SetupWFCCatalog();

    // 7. Run the ruins-only solver to completion + spawn ECS entities.
    RunSolverAndSpawn();

    // 8. Camera framing for an 8×4×8 grid centered near origin.
    UpdateCamera();

    std::cout << "[TestWFCRuinsRendering] Pipeline + scene + solver ready" << std::endl;
    return true;
}

// ============================================================================
// RegisterWFCCatalogMeshes
// ============================================================================
//
// Registers 15 procedural meshes (5 primitive + 10 ruins placeholders) with
// the StandardRenderPipeline and captures each slot index in slot_tiles[].
//
// Ruins placeholders are simple box variants — they don't visually represent
// "broken_cube" or "vine_cube" faithfully, but they give the renderer real
// geometry to draw at each collapsed cell. Visual fidelity is human-reviewed
// and can be improved later by swapping these for dedicated factories.

void WFCRuinsRenderingTestCase::RegisterWFCCatalogMeshes() {
    using namespace primal::content;
    // NOTE: do NOT add `using namespace primal;` here — global 'id' from
    // objc/runtime.h collides with primal::id and creates ambiguity. Use
    // explicit primal::graphics:: / primal::id:: qualifications below.

    auto* fwd = pipeline ? pipeline->GetForwardRenderer() : nullptr;
    const u32 slot_base = fwd ? fwd->GetMeshInfoCount() : 0;

    primal::id::id_type noTex[3] = {
        primal::id::invalid_id, primal::id::invalid_id, primal::id::invalid_id
    };

    // Tile id 0..4 = primitive (cube/ramp/corner_in/corner_out/pillar).
    auto cube_geo       = create_box_mesh(1.0f, 1.0f, 1.0f);
    auto ramp_geo       = create_ramp_mesh(1.0f, 1.0f, 1.0f, 0.0f);
    auto corner_in_geo  = create_corner_in_mesh(1.0f, 1.0f, 1.0f);
    auto corner_out_geo = create_corner_out_mesh(1.0f, 1.0f, 1.0f);
    auto pillar_geo     = create_box_mesh(0.25f, 2.0f, 0.25f);

    pipeline->RegisterMeshEntity(cube_geo,       noTex, 3);
    pipeline->RegisterMeshEntity(ramp_geo,       noTex, 3);
    pipeline->RegisterMeshEntity(corner_in_geo,  noTex, 3);
    pipeline->RegisterMeshEntity(corner_out_geo, noTex, 3);
    pipeline->RegisterMeshEntity(pillar_geo,     noTex, 3);

    // Tile id 5..14 = ruins. Real factory calls — each writes into an
    // RHIMeshAsset and the caller registers via RegisterProceduralMesh.
    // Variants within a multi-variant tile share the same mesh (tile-level
    // visual distinction is the smoke goal; variant-level distinction
    // would require 4 meshes per multi-variant tile = 30+ total, deferred).
    //   5: broken_cube        — winding-flipped cube, PosXYZ corner broken
    //   6: mossy_cube         — weathered cube, low-amplitude jitter (moss-like)
    //   7: collapsed_pillar   — pillar box tilted 30° around +X (toppled)
    //   8: rubble_pile        — 4-6 small boxes scattered near floor
    //   9: cracked_wall       — plain cube (crack encoding needs UV2, TODO)
    //  10: vine_cube          — weathered cube, medium amplitude + different seed
    //  11: weathered_stone    — weathered cube, larger amplitude (rough surface)
    //  12: broken_corner_in   — broken cube (corner_in L-shape substituted by cube)
    //  13: broken_corner_out  — broken cube (corner_out octant substituted by cube)
    //  14: debris_small       — 2-3 tiny boxes scattered near floor
    auto register_ruins = [&](primal::graphics::rhi::RHIMeshAsset& asset) -> primal::id::id_type {
        return RegisterProceduralMesh(asset);
    };

    primal::graphics::rhi::RHIMeshAsset a_broken_cube;
    create_broken_cube_mesh(a_broken_cube, 1.0f, 1.0f, 1.0f, BrokenCorner::PosXYZ);

    primal::graphics::rhi::RHIMeshAsset a_mossy_cube;
    create_weathered_cube_mesh(a_mossy_cube, 1.0f, 1.0f, 1.0f, /*seed=*/11u, /*amp=*/0.03f);

    primal::graphics::rhi::RHIMeshAsset a_collapsed_pillar;
    create_collapsed_pillar_mesh(a_collapsed_pillar, /*radius=*/0.5f, /*height=*/2.0f,
                                 TiltAxis::PlusX, /*angle_rad=*/0.52f);  // ~30°

    primal::graphics::rhi::RHIMeshAsset a_rubble_pile;
    create_rubble_pile_mesh(a_rubble_pile, /*seed=*/7u, /*radius=*/0.9f);

    primal::graphics::rhi::RHIMeshAsset a_cracked_wall;
    create_cracked_wall_mesh(a_cracked_wall, 1.0f, 1.0f, 1.0f, /*seed=*/3u);

    primal::graphics::rhi::RHIMeshAsset a_vine_cube;
    create_weathered_cube_mesh(a_vine_cube, 1.0f, 1.0f, 1.0f, /*seed=*/23u, /*amp=*/0.08f);

    primal::graphics::rhi::RHIMeshAsset a_weathered_stone;
    create_weathered_cube_mesh(a_weathered_stone, 0.92f, 0.92f, 0.92f, /*seed=*/41u, /*amp=*/0.15f);

    primal::graphics::rhi::RHIMeshAsset a_broken_corner_in;
    create_broken_corner_in_mesh(a_broken_corner_in, 1.0f, 1.0f, 1.0f, BrokenCorner::PosXYZ);

    primal::graphics::rhi::RHIMeshAsset a_broken_corner_out;
    create_broken_corner_out_mesh(a_broken_corner_out, 1.0f, 1.0f, 1.0f, BrokenCorner::NegXPosZ);

    primal::graphics::rhi::RHIMeshAsset a_debris_small;
    create_debris_small_mesh(a_debris_small, /*seed=*/5u, /*radius=*/0.4f);

    pipeline->RegisterMeshEntity(register_ruins(a_broken_cube),        noTex, 3);
    pipeline->RegisterMeshEntity(register_ruins(a_mossy_cube),         noTex, 3);
    pipeline->RegisterMeshEntity(register_ruins(a_collapsed_pillar),   noTex, 3);
    pipeline->RegisterMeshEntity(register_ruins(a_rubble_pile),        noTex, 3);
    pipeline->RegisterMeshEntity(register_ruins(a_cracked_wall),       noTex, 3);
    pipeline->RegisterMeshEntity(register_ruins(a_vine_cube),          noTex, 3);
    pipeline->RegisterMeshEntity(register_ruins(a_weathered_stone),    noTex, 3);
    pipeline->RegisterMeshEntity(register_ruins(a_broken_corner_in),   noTex, 3);
    pipeline->RegisterMeshEntity(register_ruins(a_broken_corner_out),  noTex, 3);
    pipeline->RegisterMeshEntity(register_ruins(a_debris_small),       noTex, 3);

    if (fwd) {
        const u32 after = fwd->GetMeshInfoCount();
        if (after >= slot_base + 15) {
            for (u32 i = 0; i < 15u; ++i) {
                slot_tiles[i] = slot_base + i;
            }
        }
    }
    std::cout << "[TestWFCRuinsRendering] Registered 15 catalog meshes (slots "
              << slot_base << ".." << (slot_base + 14) << ")" << std::endl;
}

// ============================================================================
// SetupWFCCatalog
// ============================================================================
//
// Builds the WFC catalog (15 tiles via WFCTileCatalog::Populate) and overrides
// every tile's mesh_handles[0..MaxVariants-1] with the real ForwardSceneRenderer
// slot indices captured in RegisterWFCCatalogMeshes. For multi-variant tiles,
// all 4 variants point at the same mesh — the variant index still rotates the
// socket signature for adjacency purposes, but the rendered geometry is shared.

void WFCRuinsRenderingTestCase::SetupWFCCatalog() {
    using namespace primal::graphics::wfc;
    registry_ = std::make_unique<WFCTileRegistry>();
    adjacency_ = std::make_unique<TileAdjacencyTable>();
    WFCTileCatalog::Populate(*registry_, *adjacency_);

    // Override every tile's mesh_handles[v] for v in [0, variant_count).
    // Multi-variant tiles (ramp, corner_in/out, broken_cube, vine_cube,
    // collapsed_pillar, broken_corner_in/out) all point at one mesh —
    // variant rotation is a socket-signature concern, not a render concern
    // for this smoke.
    for (u32 tid = 0; tid < 15u; ++tid) {
        WFCTile& t = registry_->GetMutable(wfc_tile_id{tid});
        const u32 vc = t.variant_count;
        for (u32 v = 0; v < vc; ++v) {
            t.mesh_handles[v] = primal::geometry::geometry_id{slot_tiles[tid]};
        }
        // MaxVariants may exceed variant_count; clear unused slots to be safe.
        for (u32 v = vc; v < WFCTile::MaxVariants; ++v) {
            t.mesh_handles[v] = primal::geometry::geometry_id{slot_tiles[tid]};
        }
    }

    std::cout << "[TestWFCRuinsRendering] Catalog ready (15 tiles, "
              << registry_->Count() << " registered)" << std::endl;
}

// ============================================================================
// RunSolverAndSpawn
// ============================================================================
//
// Runs the WFC solver to completion on an 8×4×8 Ruins-only grid (seed=42,
// max_generations=32), drains the step buffer into a PCGPointSet, spawns
// ECS entities for each collapsed cell, and publishes them to the pipeline.
//
// Asserts ≥ 50 cells collapsed (smoke criterion). Full collapse (256 cells)
// is the expected outcome with the T25 ruins-vertical-wildcard fix.

void WFCRuinsRenderingTestCase::RunSolverAndSpawn() {
    using namespace primal::graphics::wfc;
    using namespace primal::graphics::pcg;

    WaveGrid grid;
    WFCStepBuffer buf;
    WFCSolver solver;

    WFCConfig cfg;
    cfg.grid_size = WFCGridCoord{8, 4, 8};
    cfg.seed = 42;
    cfg.max_generations = 32;
    cfg.max_cells_per_frame = 256;
    cfg.max_ms_per_frame = 1000;
    cfg.active_category_mask = CategoryMaskFor(WFCCategory::Ruins);

    grid.Initialize(cfg.grid_size, 8);
    solver.Initialize(cfg, grid, *registry_, *adjacency_, buf);

    WFCSolveBudget budget(cfg.max_cells_per_frame, cfg.max_ms_per_frame);
    budget.Reset();

    WFCSolver::StepResult result = WFCSolver::StepResult::InProgress;
    u32 steps = 0;
    while ((result == WFCSolver::StepResult::InProgress ||
            result == WFCSolver::StepResult::Restarted) &&
           steps < 2000) {
        result = solver.Step(budget);
        ++steps;
        if (result == WFCSolver::StepResult::Restarted) {
            budget.Reset();
        }
    }

    if (result != WFCSolver::StepResult::Done &&
        result != WFCSolver::StepResult::GivenUp) {
        std::cerr << "[TestWFCRuinsRendering] solver did not terminate (last="
                  << static_cast<u32>(result) << " steps=" << steps << ")"
                  << std::endl;
    }

    PCGPointSet instances = WFCOutput::ConsumeSteps(buf, *registry_, 1.0f);

    std::cout << "[TestWFCRuinsRendering] solver result="
              << static_cast<u32>(result)
              << " steps=" << steps
              << " instances=" << instances.count
              << std::endl;

    if (instances.count < 50u) {
        std::cerr << "[TestWFCRuinsRendering] SMOKE FAIL: only "
                  << instances.count << " cells collapsed (expected ≥ 50)"
                  << std::endl;
    }

    if (instances.count > 0) {
        auto spawn = PCGEntityFactory::CreateEntities(instances);
        wfc_entity_ids = std::move(spawn.entity_ids);
        wfc_mesh_slots = std::move(spawn.mesh_slot_indices);
        pipeline->SetPCGEntities(wfc_entity_ids, wfc_mesh_slots);
    }

    // Soft assertion: log a warning but don't crash. The renderer still needs
    // to render whatever collapsed so a human can inspect the partial frame.
    assert(instances.count >= 50u && "smoke criterion: ≥ 50 cells collapsed");
}

// ============================================================================
// UpdateCamera
// ============================================================================

void WFCRuinsRenderingTestCase::UpdateCamera() {
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

// ============================================================================
// Run
// ============================================================================

void WFCRuinsRenderingTestCase::Run() {
    if (!pipeline || !scene || !view) return;

    view->UpdateFrustum();
    view->Cull(*scene);

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
        std::cout << "[TestWFCRuinsRendering] Rendered " << frame_count_
                  << " frames" << std::endl;
#ifdef __APPLE__
        // Tear down engine state BEFORE AppKit starts closing windows.
        Shutdown();
        NS::Application::sharedApplication()->terminate(nullptr);
#endif
    }
}

// ============================================================================
// Shutdown
// ============================================================================

void WFCRuinsRenderingTestCase::Shutdown() {
    if (!pipeline && !scene) return;  // idempotent
    std::cout << "[TestWFCRuinsRendering] Shutting down..." << std::endl;

    if (!wfc_entity_ids.empty()) {
        primal::graphics::pcg::PCGEntityFactory::DestroyEntities(wfc_entity_ids);
    }
    wfc_entity_ids.clear();
    wfc_mesh_slots.clear();
    if (pipeline) pipeline->ClearPCGEntities();

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
    primal::content::shutdown();
}
