#pragma once

// TestWFCRuinsRendering.h — WFC Phase C.1 T27 visual smoke binary.
//
// Boots the Metal-backed window + StandardRenderPipeline, registers all 15
// catalog procedural meshes (5 primitive + 10 ruins real-factory meshes),
// and streams an 8×4×8 Ruins-only solve across frames:
//
//   Initialize  → build catalog + create grid/buffer/solver (no solve yet)
//   Run         → PumpSolverFrame (Step + DrainStream + spawn new entities)
//                 then render one frame
//
// WFCSolver::Step collapses exactly one cell per call regardless of the
// budget's max_cells_per_frame (the budget is a cap, not a target). So at
// 1 Step per frame, the viewer watches the ruins-style grid fill in over
// ~256 frames (8×4×8 = 256 cells). Cumulative entity tracking survives
// non-restart steps; Restart destroys the prior batch and replays survivors.
//
// Smoke assertion: ≥ 50 cells collapsed by kHeadlessFrameCap. Full collapse
// (256 cells) is expected around frame 256 with the T25 ruins-vertical-
// wildcard fix in place; the cap is 300 to allow ~50 viewing frames after
// completion. Visual quality is human-reviewed.

#include "RenderTestFramework.h"
#include "Engine/Graphics/RenderPipeline/StandardRenderPipeline.h"
#include "Engine/Graphics/RHI/Core/RHIDevice.h"
#include "Engine/Graphics/RenderView.h"
#include "Engine/Graphics/RenderScene.h"
#include "Engine/Platform/Platform.h"
#include "Engine/Graphics/RHI/Systems/RenderSystem.h"
#include "Engine/Graphics/WFC/WFCTileRegistry.h"
#include "Engine/Graphics/WFC/TileAdjacency.h"
#include "Engine/Graphics/WFC/WaveGrid.h"
#include "Engine/Graphics/WFC/WFCSolver.h"
#include "Engine/Graphics/WFC/WFCStepBuffer.h"
#include "Engine/Graphics/WFC/WFCSolveBudget.h"
#include "Engine/Graphics/WFC/WFCConfig.h"
#include <memory>

class WFCRuinsRenderingTestCase : public primal::test::RenderTestCase {
public:
    WFCRuinsRenderingTestCase();  // constructs budget_ (no default ctor)
    bool Initialize() override;
    void Run() override;
    void Shutdown() override;

private:
    void RegisterWFCCatalogMeshes();
    void SetupWFCCatalog();
    void ReseedSolver();
    void DestroyAllSpawnedEntities();
    void PumpSolverFrame();
    void UpdateCamera();

    std::unique_ptr<primal::graphics::rhi::RHIDeviceBase> device;
    primal::platform::window window;
    primal::graphics::RenderSystem renderSystem;
    primal::graphics::StandardRenderPipeline* pipeline = nullptr;
    primal::graphics::RenderScene* scene = nullptr;
    primal::graphics::RenderView* view = nullptr;

    std::unique_ptr<primal::graphics::wfc::WFCTileRegistry>    registry_;
    std::unique_ptr<primal::graphics::wfc::TileAdjacencyTable> adjacency_;

    // Streaming solver state. Created in ReseedSolver (called from Initialize).
    std::unique_ptr<primal::graphics::wfc::WaveGrid>      grid_;
    std::unique_ptr<primal::graphics::wfc::WFCStepBuffer> buf_;
    std::unique_ptr<primal::graphics::wfc::WFCSolver>     solver_;
    primal::graphics::wfc::WFCSolveBudget                 budget_;
    primal::graphics::wfc::WFCSolver::StepResult          solver_state_{
        primal::graphics::wfc::WFCSolver::StepResult::InProgress};
    bool solver_done_{false};
    u32  total_collapses_{0};
    u32  total_restarts_{0};
    u32  rng_seed_{42};

    // Cumulative spawned entities (cleared on Restart).
    std::vector<primal::id::id_type> wfc_entity_ids;
    std::vector<u32>                 wfc_mesh_slots;

    // Slot indices for the 15 catalog tiles. Index = wfc_tile_id (0..14).
    u32 slot_tiles[15]{};

    u32 window_width_{1280};
    u32 window_height_{720};
    u64 frame_count_{0};
    // 256-cell grid (8×4×8) at 1 cell-per-frame Step + ~50 frames viewing
    // the finished state. ~5s @ 60fps.
    static constexpr u64 kHeadlessFrameCap = 300;

    // Camera state.
    primal::math::v3 camera_pos_{12.0f, 12.0f, 12.0f};
    float camera_yaw_{0.0f};
    float camera_pitch_{-0.5f};
};

class Engine_Test : public primal::test::RenderTestRunner {
public:
    Engine_Test();
};
