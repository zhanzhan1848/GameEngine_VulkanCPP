#pragma once

// TestWFCRendering.h — WFC Phase A.4 visual demo
//
// Mirrors the TestPCGScatter scaffolding pattern: a RenderTestCase that owns
// a Metal device + RenderSystem + StandardRenderPipeline + empty RenderScene.
// Task 2: open window + render 60 headless frames. Task 3: register catalog
// meshes + run solver + emit point set. Task 4: spawn the collapsed tile
// instances into the scene as ECS Entities and hand them to the pipeline.

#include "RenderTestFramework.h"
#include "Engine/Graphics/RenderPipeline/StandardRenderPipeline.h"
#include "Engine/Graphics/RHI/Core/RHIDevice.h"
#include "Engine/Graphics/RenderView.h"
#include "Engine/Graphics/RenderScene.h"
#include "Engine/Graphics/RenderProxy.h"
#include "Engine/Platform/Platform.h"
#include "Engine/Graphics/RHI/Systems/RenderSystem.h"
#include "Engine/Graphics/PCG/PCGTypes.h"

class WFCRenderingTestCase : public primal::test::RenderTestCase {
public:
    bool Initialize() override;
    void Run() override;
    void Shutdown() override;

private:
    void UpdateCamera();

    // --- Task 3 helpers ---
    // Registers 5 procedural meshes (cube/ramp/corner_in/corner_out/pillar)
    // via StandardRenderPipeline::RegisterMeshEntity and captures their slot
    // indices. Overrides the WFC catalog's placeholder mesh_handles with the
    // captured slots, so the emitted PCGPointSet's MeshIndex attr resolves to
    // a real renderable mesh in Task 4.
    void RegisterWFCCatalogMeshes();
    // Builds the WFC catalog, runs the solver on a 4x4x4 grid, drains Collapse
    // steps into wfc_point_set. Leaves Task 4 to spawn entities from the set.
    void RunSolverAndEmit();

    // --- Task 4 helper ---
    // Feeds wfc_point_set into PCGEntityFactory::CreateEntities to mint ECS
    // Entities, then hands the entity_ids + mesh_slot_indices to
    // pipeline->SetPCGEntities so Render() syncs RenderProxies for each tile
    // instance into the RenderScene. Must run after RunSolverAndEmit.
    void SpawnWFCEntities();

    std::unique_ptr<primal::graphics::rhi::RHIDeviceBase> device;
    primal::platform::window window;
    primal::graphics::RenderSystem renderSystem;
    primal::graphics::StandardRenderPipeline* pipeline = nullptr;
    primal::graphics::RenderScene* scene = nullptr;
    primal::graphics::RenderView* view = nullptr;

    // Camera state (mirrors TestPCGScatter's simple yaw/pitch setup).
    primal::math::v3 camera_pos_{8.0f, 8.0f, 8.0f};
    float camera_yaw_{0.0f};
    float camera_pitch_{-0.4f};

    u32 window_width_{1280};
    u32 window_height_{720};
    u64 frame_count_{0};

    // Headless exit: task spec says render 60 frames then quit. The run loop
    // (CFRunLoopTimer in RenderTestRunner) invokes Run() at 60 FPS; we count
    // frames and request NSApplication termination when the cap is hit.
    static constexpr u64 kHeadlessFrameCap = 60;

    // Filled in Task 3 (RegisterWFCCatalogMeshes) and Task 4 (RunSolverAndEmit).
    std::vector<primal::id::id_type> wfc_entity_ids;
    std::vector<u32> wfc_mesh_slots;

    // Slot indices for the 5 catalog tile types (cube, ramp, corner_in,
    // corner_out, pillar). Captured from ForwardSceneRenderer::GetMeshInfoCount
    // before/after RegisterWFCCatalogMeshes runs. The catalog's placeholder
    // mesh_handles (1000-1004) are overwritten with these slots so the emitted
    // PCGPointSet's MeshIndex attr points at real renderable meshes.
    u32 slot_cube{0};
    u32 slot_ramp{0};
    u32 slot_corner_in{0};
    u32 slot_corner_out{0};
    u32 slot_pillar{0};

    // Emitted by RunSolverAndEmit; consumed by SpawnWFCEntities in Task 4.
    primal::graphics::pcg::PCGPointSet wfc_point_set{};
};

class Engine_Test : public primal::test::RenderTestRunner {
public:
    Engine_Test();
};
