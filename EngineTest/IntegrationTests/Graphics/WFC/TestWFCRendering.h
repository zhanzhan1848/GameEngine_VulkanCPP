#pragma once

// TestWFCRendering.h — WFC visual demo (Phase A.4 + Phase B.1)
//
// Mirrors the TestPCGScatter scaffolding pattern: a RenderTestCase that owns
// a Metal device + RenderSystem + StandardRenderPipeline + RenderScene.
// Phase A.4: register catalog meshes + run solver + emit point set + spawn
// entities into the scene as ECS Entities and hand them to the pipeline.
// Phase B.1: interactive Mode toggle ('M' key) cycles 3D <-> 2D; CI smoke
// variant via -DWFC_MODE_2D_SMOKE=1.

#include "RenderTestFramework.h"
#include "Engine/Graphics/RenderPipeline/StandardRenderPipeline.h"
#include "Engine/Graphics/RHI/Core/RHIDevice.h"
#include "Engine/Graphics/RenderView.h"
#include "Engine/Graphics/RenderScene.h"
#include "Engine/Graphics/RenderProxy.h"
#include "Engine/Platform/Platform.h"
#include "Engine/Graphics/RHI/Systems/RenderSystem.h"
#include "Engine/Graphics/PCG/PCGTypes.h"
#include "Engine/Graphics/WFC/WFCTileRegistry.h"
#include "Engine/Graphics/WFC/TileAdjacency.h"
#include <memory>

class WFCRenderingTestCase : public primal::test::RenderTestCase {
public:
    bool Initialize() override;
    void Run() override;
    void Shutdown() override;

private:
    void UpdateCamera();

    // --- Mode + interactive toggle (Phase B.1) ---
    enum class RenderMode : u8 { ThreeD, TwoD };
    RenderMode mode_{RenderMode::ThreeD};
#ifdef WFC_MODE_2D_SMOKE
    // CI smoke variant: initialize in 2D mode, render 60 frames, exit.
    static_assert(WFC_MODE_2D_SMOKE == 1, "WFC_MODE_2D_SMOKE must be 1 if defined");
#endif
    bool key_m_pressed_{false};

    // --- Catalog setup (Phase B.1: extracted from RunSolverAndEmit) ---
    // Registers 5 procedural meshes (cube/ramp/corner_in/corner_out/pillar)
    // via StandardRenderPipeline::RegisterMeshEntity and captures their slot
    // indices. Overrides the WFC catalog's placeholder mesh_handles with the
    // captured slot indices. Idempotent — safe to call once from Initialize.
    void RegisterWFCCatalogMeshes();
    // Populates registry_ + adjacency_ + overrides mesh_handles. Called once
    // from Initialize so CycleMode doesn't rebuild the catalog each toggle.
    void SetupWFCCatalog();

    // --- Per-mode solver + spawn (Phase B.1) ---
    // Runs the solver on the current mode's grid size, drains Collapse steps
    // into wfc_point_set.
    void RunSolverForCurrentMode();
    // Spawns ECS entities from wfc_point_set and hands them to the pipeline.
    void SpawnEntitiesForCurrentMode();
    // Toggle ThreeD <-> TwoD: destroy entities, flip mode_, re-solve, re-spawn,
    // snap camera. Logs the new mode to stdout.
    void CycleMode();
    // Sets camera_pos_/yaw_/pitch_ based on mode_. Called from Initialize +
    // CycleMode.
    void SnapCameraForCurrentMode();

    std::unique_ptr<primal::graphics::rhi::RHIDeviceBase> device;
    primal::platform::window window;
    primal::graphics::RenderSystem renderSystem;
    primal::graphics::StandardRenderPipeline* pipeline = nullptr;
    primal::graphics::RenderScene* scene = nullptr;
    primal::graphics::RenderView* view = nullptr;

    // Phase B.1: registry + adjacency owned by the test case so CycleMode
    // can re-solve without rebuilding the catalog.
    std::unique_ptr<primal::graphics::wfc::WFCTileRegistry> registry_;
    std::unique_ptr<primal::graphics::wfc::TileAdjacencyTable> adjacency_;

    // Camera state (per-mode positions; snaps on toggle — no lerp).
    primal::math::v3 camera_pos_{8.0f, 8.0f, 8.0f};
    float camera_yaw_{0.0f};
    float camera_pitch_{-0.4f};

    u32 window_width_{1280};
    u32 window_height_{720};
    u64 frame_count_{0};

    // Headless exit: render 60 frames then quit.
    static constexpr u64 kHeadlessFrameCap = 60;

    // Filled in SpawnEntitiesForCurrentMode; cleared in CycleMode + Shutdown.
    std::vector<primal::id::id_type> wfc_entity_ids;
    std::vector<u32> wfc_mesh_slots;

    // Slot indices for the 5 catalog tile types.
    u32 slot_cube{0};
    u32 slot_ramp{0};
    u32 slot_corner_in{0};
    u32 slot_corner_out{0};
    u32 slot_pillar{0};

    primal::graphics::pcg::PCGPointSet wfc_point_set{};
};

class Engine_Test : public primal::test::RenderTestRunner {
public:
    Engine_Test();
};
