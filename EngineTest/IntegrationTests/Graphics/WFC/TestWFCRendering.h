#pragma once

// TestWFCRendering.h — WFC visual demo (Phase A.4 + B.1 + B.2)
//
// Phase B.2: streaming generation. Solver owns long-lived state across
// frames; Run() calls Step(budget) once per frame, drains new Collapse
// steps via WFCOutput::DrainStream, spawns entities, and handles Restart
// by destroying all prior entities before appending survivors.

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
#include "Engine/Graphics/WFC/WaveGrid.h"
#include "Engine/Graphics/WFC/WFCSolver.h"
#include "Engine/Graphics/WFC/WFCStepBuffer.h"
#include "Engine/Graphics/WFC/WFCSolveBudget.h"
#include "Engine/Graphics/WFC/WFCConfig.h"
#include "Engine/Graphics/WFC/TileAdjacency.h"
#include <memory>

class WFCRenderingTestCase : public primal::test::RenderTestCase {
public:
    WFCRenderingTestCase();  // Phase B.2: constructs budget_ (no default ctor).
    bool Initialize() override;
    void Run() override;
    void Shutdown() override;

private:
    void UpdateCamera();

    // --- Mode + interactive toggle (Phase B.1) ---
    enum class RenderMode : u8 { ThreeD, TwoD };
    RenderMode mode_{RenderMode::ThreeD};
#ifdef WFC_MODE_2D_SMOKE
    static_assert(WFC_MODE_2D_SMOKE == 1, "WFC_MODE_2D_SMOKE must be 1 if defined");
#endif
    bool key_m_pressed_{false};
    // Phase B.2: re-seed + pause
    bool key_r_pressed_{false};
    bool key_space_pressed_{false};
    bool paused_{false};

    // --- Catalog setup (Phase B.1) ---
    void RegisterWFCCatalogMeshes();
    void SetupWFCCatalog();

    // --- Phase B.2: streaming solver lifecycle ---
    // ReseedSolver: destroy entities, recreate grid/buffer/solver with
    // rng_seed_++ and grid_size_for_mode(). Called from Initialize and
    // on 'R' / 'M' key.
    void ReseedSolver();
    // DestroyAllSpawnedEntities: calls PCGEntityFactory::DestroyEntities on
    // wfc_entity_ids, clears the cumulative vectors, calls
    // pipeline->ClearPCGEntities.
    void DestroyAllSpawnedEntities();
    // CellSizeForCurrentMode / GridSizeForCurrentMode: per-mode constants.
    f32  CellSizeForCurrentMode() const;
    primal::graphics::wfc::WFCGridCoord GridSizeForCurrentMode() const;
    // Per-frame streaming step. Returns the DrainStream result.
    void PumpSolverFrame();

    // Phase B.1 helpers, unchanged behavior.
    void CycleMode();
    void SnapCameraForCurrentMode();

    std::unique_ptr<primal::graphics::rhi::RHIDeviceBase> device;
    primal::platform::window window;
    primal::graphics::RenderSystem renderSystem;
    primal::graphics::StandardRenderPipeline* pipeline = nullptr;
    primal::graphics::RenderScene* scene = nullptr;
    primal::graphics::RenderView* view = nullptr;

    // Phase B.1: catalog owned by test case (rebuilt only on Initialize).
    std::unique_ptr<primal::graphics::wfc::WFCTileRegistry> registry_;
    std::unique_ptr<primal::graphics::wfc::TileAdjacencyTable> adjacency_;

    // Phase B.2: streaming solver state. Recreated on ReseedSolver.
    std::unique_ptr<primal::graphics::wfc::WaveGrid>      grid_;
    std::unique_ptr<primal::graphics::wfc::WFCStepBuffer> buf_;
    std::unique_ptr<primal::graphics::wfc::WFCSolver>     solver_;
    primal::graphics::wfc::WFCSolveBudget                 budget_;

    // Phase B.2: solver run state. StepResult is a nested enum on WFCSolver.
    primal::graphics::wfc::WFCSolver::StepResult solver_state_{
        primal::graphics::wfc::WFCSolver::StepResult::InProgress};
    bool solver_done_{false};
    u32  total_collapses_{0};
    u32  total_restarts_{0};
    u32  rng_seed_{7};  // Phase B.1 used fixed seed 7; B.2 increments on each re-seed

    // Camera state.
    primal::math::v3 camera_pos_{8.0f, 8.0f, 8.0f};
    float camera_yaw_{0.0f};
    float camera_pitch_{-0.4f};

    u32 window_width_{1280};
    u32 window_height_{720};
    u64 frame_count_{0};
    static constexpr u64 kHeadlessFrameCap = 60;

    // Phase B.2: cumulative spawned entities. Cleared on Restart / re-seed.
    std::vector<primal::id::id_type> wfc_entity_ids;
    std::vector<u32>                 wfc_mesh_slots;

    // Slot indices for the 5 catalog tile types (Phase B.1).
    u32 slot_cube{0};
    u32 slot_ramp{0};
    u32 slot_corner_in{0};
    u32 slot_corner_out{0};
    u32 slot_pillar{0};
};

class Engine_Test : public primal::test::RenderTestRunner {
public:
    Engine_Test();
};
