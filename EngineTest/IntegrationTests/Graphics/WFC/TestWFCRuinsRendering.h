#pragma once

// TestWFCRuinsRendering.h — WFC Phase C.1 T27 visual smoke binary.
//
// Boots the Metal-backed window + StandardRenderPipeline, registers all 15
// catalog procedural meshes (5 primitive + 10 ruins placeholders using box
// variants), runs the solver on an 8×4×8 Ruins-only grid to completion in
// Initialize, spawns the collapsed cells as ECS entities, and renders
// kHeadlessFrameCap frames before exiting.
//
// Smoke assertion: ≥ 50 cells collapsed (loose; an 8×4×8 = 256-cell grid
// should fully collapse under max_generations=32 with T25's ruins-vertical
// wildcard in place). Visual quality is human-reviewed — open the window
// and inspect that ruins tiles actually appear.
//
// Cloned from TestWFCRendering.h (Phase A.4 + B.1 + B.2) with the
// interactive/streaming logic stripped: T27 is a one-shot solve + render,
// not a streaming demo.

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
    WFCRuinsRenderingTestCase();
    bool Initialize() override;
    void Run() override;
    void Shutdown() override;

private:
    void RegisterWFCCatalogMeshes();
    void SetupWFCCatalog();
    void RunSolverAndSpawn();
    void UpdateCamera();

    std::unique_ptr<primal::graphics::rhi::RHIDeviceBase> device;
    primal::platform::window window;
    primal::graphics::RenderSystem renderSystem;
    primal::graphics::StandardRenderPipeline* pipeline = nullptr;
    primal::graphics::RenderScene* scene = nullptr;
    primal::graphics::RenderView* view = nullptr;

    std::unique_ptr<primal::graphics::wfc::WFCTileRegistry>    registry_;
    std::unique_ptr<primal::graphics::wfc::TileAdjacencyTable> adjacency_;

    // Slot indices for all 15 catalog tiles. Index = wfc_tile_id (0..14).
    // 0..4 = primitive (cube/ramp/corner_in/corner_out/pillar)
    // 5..14 = ruins placeholders (simple box variants)
    u32 slot_tiles[15]{};

    // Spawned ECS entities (for clean Shutdown).
    std::vector<primal::id::id_type> wfc_entity_ids;
    std::vector<u32>                 wfc_mesh_slots;

    u32 window_width_{1280};
    u32 window_height_{720};
    u64 frame_count_{0};
    static constexpr u64 kHeadlessFrameCap = 60;

    // Camera state.
    primal::math::v3 camera_pos_{12.0f, 12.0f, 12.0f};
    float camera_yaw_{0.0f};
    float camera_pitch_{-0.5f};
};

class Engine_Test : public primal::test::RenderTestRunner {
public:
    Engine_Test();
};
