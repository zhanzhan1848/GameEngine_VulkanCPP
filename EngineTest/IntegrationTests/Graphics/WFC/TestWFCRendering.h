#pragma once

// TestWFCRendering.h — WFC Phase A.4 visual demo
//
// Mirrors the TestPCGScatter scaffolding pattern: a RenderTestCase that owns
// a Metal device + RenderSystem + StandardRenderPipeline + empty RenderScene.
// Task 2 stops here — window opens, 60 frames render headlessly, test exits.
// Task 3 will populate the catalog meshes and run the WFC solver; Task 4 will
// spawn the collapsed tile instances into the scene.

#include "RenderTestFramework.h"
#include "Engine/Graphics/RenderPipeline/StandardRenderPipeline.h"
#include "Engine/Graphics/RHI/Core/RHIDevice.h"
#include "Engine/Graphics/RenderView.h"
#include "Engine/Graphics/RenderScene.h"
#include "Engine/Graphics/RenderProxy.h"
#include "Engine/Platform/Platform.h"
#include "Engine/Graphics/RHI/Systems/RenderSystem.h"

class WFCRenderingTestCase : public primal::test::RenderTestCase {
public:
    bool Initialize() override;
    void Run() override;
    void Shutdown() override;

private:
    void UpdateCamera();

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
};

class Engine_Test : public primal::test::RenderTestRunner {
public:
    Engine_Test();
};
