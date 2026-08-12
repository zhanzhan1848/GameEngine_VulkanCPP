// TestWFCRuinsRendering.cpp — WFC Phase C.1 T27 visual smoke binary.
//
// Boots Metal-backed window + StandardRenderPipeline + 15-tile catalog
// (5 primitive + 10 ruins real-factory meshes), streams an 8×4×8 Ruins-only
// solve across frames (1 collapse per frame), spawns each new cell as an
// ECS entity, renders kHeadlessFrameCap=300 frames, then exits.
//
// Smoke assertion: ≥ 50 cells collapsed by frame cap. With streaming
// working and the T25 ruins-vertical-wildcard fix in place, the solver
// reaches Done at frame ~256 with 256 collapses + 0 restarts.
//
// Cloned from TestWFCRendering.cpp (Phase A.4 + B.1 + B.2). Differences:
//   * Single mode (3D, 8×4×8) — no 2D toggle.
//   * Ruins-only category mask (10 ruins tiles, primitives filtered out).
//   * No 'R'/'M'/'Space' interactivity.
//   * 15 mesh slots vs 5 (registers 10 ruins real-factory meshes).

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
    : budget_(4u, 16u) {}  // 4 cells/frame → ~64 frames for full 256-cell collapse

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
    //    mesh_handles overrides pointing at the registered slot indices.
    RegisterWFCCatalogMeshes();
    SetupWFCCatalog();

    // 7. Reseed streaming solver state (grid/buffer/solver). Streaming
    //    happens in Run() via PumpSolverFrame — viewer watches cells
    //    collapse one-by-one across frames.
    ReseedSolver();

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
// ReseedSolver
// ============================================================================
//
// Destroys current entities (if any), then creates fresh grid_/buf_/solver_
// with the ruins-only 8×4×8 config and rng_seed_. Catalog (registry_ +
// adjacency_) is preserved — built once in SetupWFCCatalog. Catalog slot
// overrides are stable across reseeds.

void WFCRuinsRenderingTestCase::ReseedSolver() {
    using namespace primal::graphics::wfc;

    DestroyAllSpawnedEntities();

    grid_ = std::make_unique<WaveGrid>();
    buf_  = std::make_unique<WFCStepBuffer>();

    WFCConfig config;
    config.grid_size  = WFCGridCoord{8, 4, 8};
    config.seed       = rng_seed_;
    config.max_generations = 32;
    config.active_category_mask = CategoryMaskFor(WFCCategory::Ruins);

    solver_ = std::make_unique<WFCSolver>();
    solver_->Initialize(config, *grid_, *registry_, *adjacency_, *buf_);

    solver_state_    = WFCSolver::StepResult::InProgress;
    solver_done_     = false;
    total_collapses_ = 0;
    total_restarts_  = 0;

    std::cout << "[TestWFCRuinsRendering] solver reseeded: grid=8x4x8 seed="
              << rng_seed_ << std::endl;
}

// ============================================================================
// DestroyAllSpawnedEntities
// ============================================================================

void WFCRuinsRenderingTestCase::DestroyAllSpawnedEntities() {
    using namespace primal::graphics::pcg;
    // Drain RenderScene proxies BEFORE clearing the list — same rationale as
    // TestKenneyTilePreview: SyncEntitiesToRenderScene only RemoveProxy's for
    // entities still in pcg_entity_ids_, so ClearPCGEntities first would
    // leave stale proxies in RenderScene::proxies_.
    if (scene) {
        for (auto eid : wfc_entity_ids) scene->RemoveProxy(eid);
    }
    if (!wfc_entity_ids.empty()) {
        PCGEntityFactory::DestroyEntities(wfc_entity_ids);
    }
    wfc_entity_ids.clear();
    wfc_mesh_slots.clear();
    if (pipeline) pipeline->ClearPCGEntities();
}

// ============================================================================
// PumpSolverFrame
// ============================================================================
//
// One streaming step per frame. Mirrors TestWFCRendering::PumpSolverFrame:
//   1. Reset per-frame budget + advance solver by one Step.
//   2. Drain whatever steps landed in the buffer via WFCOutput::DrainStream
//      (keeps only post-last-Restart Collapse points).
//   3. On restart_seen: destroy prior entities before appending survivors.
//   4. Spawn entities for new Collapse points (if any). Append to the
//      cumulative vectors and re-publish via SetPCGEntities.
//   5. Detect completion (Done / GivenUp) + assert smoke criterion.

void WFCRuinsRenderingTestCase::PumpSolverFrame() {
    using namespace primal::graphics::wfc;
    using namespace primal::graphics::pcg;

    if (solver_done_ || !solver_) return;

    budget_.Reset();
    solver_state_ = solver_->Step(budget_);

    WFCStreamDrainResult drain = WFCOutput::DrainStream(
        *buf_, *registry_, /*cell_size=*/1.0f);

    if (drain.restart_seen) {
        total_restarts_ += drain.restart_count;
        DestroyAllSpawnedEntities();
        std::cout << "[TestWFCRuinsRendering] restart #" << total_restarts_
                  << " — replaying " << drain.new_points.count
                  << " new collapses" << std::endl;
    }

    if (drain.new_points.count > 0) {
        total_collapses_ += drain.new_points.count;
        auto spawn = PCGEntityFactory::CreateEntities(drain.new_points);
        wfc_entity_ids.insert(wfc_entity_ids.end(),
                              spawn.entity_ids.begin(), spawn.entity_ids.end());
        wfc_mesh_slots.insert(wfc_mesh_slots.end(),
                              spawn.mesh_slot_indices.begin(),
                              spawn.mesh_slot_indices.end());
        assert(wfc_entity_ids.size() == wfc_mesh_slots.size());
        pipeline->SetPCGEntities(wfc_entity_ids, wfc_mesh_slots);
    }

    if (solver_state_ == WFCSolver::StepResult::Done ||
        solver_state_ == WFCSolver::StepResult::GivenUp) {
        solver_done_ = true;
        // Print per-tile distribution so a reader can confirm the ruins-only
        // solve actually used multiple tile kinds (vs. collapsing to a single
        // tile kind because of an adjacency bug).
        const auto& cells = grid_->Cells();
        u32 per_tile[15]{};
        for (u32 i = 0; i < cells.size(); ++i) {
            if (!cells[i].collapsed) continue;
            const u32 tid = static_cast<u32>(cells[i].collapsed_tile);
            if (tid < 15) ++per_tile[tid];
        }
        std::cout << "[TestWFCRuinsRendering] solver done: collapses="
                  << total_collapses_ << " restarts=" << total_restarts_
                  << " state=" << static_cast<u32>(solver_state_) << std::endl;
        std::cout << "[TestWFCRuinsRendering] tile distribution:" << std::endl;
        static const char* kTileNames[15] = {
            "cube", "ramp", "corner_in", "corner_out", "pillar",
            "broken_cube", "mossy_cube", "collapsed_pillar", "rubble_pile",
            "cracked_wall", "vine_cube", "weathered_stone",
            "broken_corner_in", "broken_corner_out", "debris_small"
        };
        for (u32 t = 0; t < 15; ++t) {
            if (per_tile[t] == 0) continue;
            std::cout << "  [" << t << "] " << kTileNames[t]
                      << ": " << per_tile[t] << std::endl;
        }
    }
}

// ============================================================================
// UpdateCamera
// ============================================================================

void WFCRuinsRenderingTestCase::UpdateCamera() {
    // Aim camera at grid center (8×4×8 grid → world center ≈ (4, 2, 4)).
    // The yaw/pitch params still drive camera_pos_ for orbit, but the look-at
    // target is the grid center, not an arbitrary point along forward.
    v3 up{0, 1, 0};
    v3 target{4.0f, 2.0f, 4.0f};
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

    // Streaming pump: collapses a few more cells this frame, spawns entities.
    PumpSolverFrame();

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
                  << " frames, collapses=" << total_collapses_
                  << " restarts=" << total_restarts_
                  << " entities=" << wfc_entity_ids.size()
                  << " final_state=" << static_cast<u32>(solver_state_)
                  << std::endl;
        // Smoke criterion: ≥ 50 cells collapsed. With streaming working,
        // 300 frames at 1 cell-per-frame should yield ≥ 256 collapses (full
        // 8×4×8 grid). Loose lower bound catches catastrophic regressions
        // (catalog mismatch, budget exhaustion, solver hang).
        if (total_collapses_ < 50u) {
            std::cerr << "[TestWFCRuinsRendering] SMOKE FAIL: only "
                      << total_collapses_ << " cells collapsed (expected ≥ 50)"
                      << std::endl;
        }
        assert(total_collapses_ >= 50u && "smoke criterion: >= 50 cells collapsed");
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

    // Drop streaming state before the catalog (solver holds raw pointers
    // into grid_/buf_/registry_/adjacency_).
    solver_.reset();
    buf_.reset();
    grid_.reset();

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
