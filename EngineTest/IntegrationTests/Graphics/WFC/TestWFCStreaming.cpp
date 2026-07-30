// TestWFCStreaming.cpp — WFC Phase B.2 Task 4: streaming invariant test.
//
// Quantitative counterpart to TestWFCRendering's visual demo. Asserts the
// streaming solver completes a 4x4x4 grid within kFrameCap=240 frames and
// produces exactly 64 collapses. Runs 4 solver Steps per frame (vs
// TestWFCRendering's 1) so the test completes in ~16 frames.
//
// Catalog setup (RegisterWFCCatalogMeshes + SetupWFCCatalog) is copied
// verbatim from TestWFCRendering.cpp so adjacency is consistent. Per the
// engine's test-case convention, helpers are not shared across test cases.

#include "TestWFCStreaming.h"
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
#include "Engine/Graphics/RenderPipeline/StandardRenderPipeline.h"
#include "Engine/Graphics/RHI/Systems/RenderSystem.h"
#include "Engine/Graphics/RenderView.h"
#include "Engine/Graphics/RenderScene.h"
#include "Engine/Platform/Platform.h"

#ifdef __APPLE__
#include <AppKit/AppKit.hpp>
#endif

#include <iostream>
#include <memory>
#include <cassert>

using namespace primal::graphics;
using namespace primal::graphics::rhi;
using namespace primal::math;

// ============================================================================
// WFCStreamingTestCase
// ============================================================================
//
// Mirrors WFCRenderingTestCase's window/device/pipeline/catalog setup but
// drops the interactive keys + camera cycling. Run() pumps 4 solver Steps
// per frame, renders an (empty) frame to keep the Metal pipeline happy, and
// asserts completion + collapse count once the solver finishes.

class WFCStreamingTestCase : public primal::test::RenderTestCase {
public:
    WFCStreamingTestCase();  // WFCSolveBudget has no default ctor.
    bool Initialize() override;
    void Run() override;
    void Shutdown() override;

private:
    void RegisterWFCCatalogMeshes();
    void SetupWFCCatalog();
    void PumpSolverFrame();

    std::unique_ptr<primal::graphics::rhi::RHIDeviceBase> device;
    primal::platform::window window;
    primal::graphics::RenderSystem renderSystem;
    primal::graphics::StandardRenderPipeline* pipeline = nullptr;
    primal::graphics::RenderScene* scene = nullptr;
    primal::graphics::RenderView* view = nullptr;

    std::unique_ptr<primal::graphics::wfc::WFCTileRegistry>     registry_;
    std::unique_ptr<primal::graphics::wfc::TileAdjacencyTable>  adjacency_;
    std::unique_ptr<primal::graphics::wfc::WaveGrid>            grid_;
    std::unique_ptr<primal::graphics::wfc::WFCStepBuffer>       buf_;
    std::unique_ptr<primal::graphics::wfc::WFCSolver>           solver_;
    primal::graphics::wfc::WFCSolveBudget                       budget_;

    primal::graphics::wfc::WFCSolver::StepResult solver_state_{
        primal::graphics::wfc::WFCSolver::StepResult::InProgress};

    u32 total_collapses_{0};
    u32 total_restarts_{0};

    static constexpr u64 kFrameCap = 240;        // generous; 64/4 = 16 frames minimum
    static constexpr u32 kExpectedCollapses = 64; // 4x4x4 grid

    u32 window_width_{1280};
    u32 window_height_{720};
    u64 frame_count_{0};

    // Catalog mesh slots (captured during RegisterWFCCatalogMeshes).
    u32 slot_cube{0}, slot_ramp{0}, slot_corner_in{0}, slot_corner_out{0}, slot_pillar{0};
};

// WFCSolveBudget requires max_cells_per_frame + max_ms_per_frame at construction.
// 8 cells/frame is generous (Run pumps up to 4 Steps, each collapsing 1 cell);
// 16ms is headroom for a headless frame.
WFCStreamingTestCase::WFCStreamingTestCase()
    : budget_(8u, 16u) {}

// ============================================================================
// WFCStreamingTestCase::Initialize
// ============================================================================

bool WFCStreamingTestCase::Initialize() {
    std::cout << "[TestWFCStreaming] Initializing..." << std::endl;

    // 1. Window
    primal::platform::window_init_info winInfo{};
    winInfo.caption = "TestWFCStreaming";
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

    // 3. RenderSystem (owning swap chain + per-frame command buffers).
    RenderSystemInitInfo sysInfo;
    sysInfo.device = device.get();
    sysInfo.window = window.handle();
    sysInfo.width = window_width_;
    sysInfo.height = window_height_;
    if (!renderSystem.Initialize(sysInfo)) return false;

    // 4. Pipeline. SetLumenConfig triggers InitializeSubsystems which creates
    //    the ForwardSceneRenderer — RegisterMeshEntity needs it.
    pipeline = new StandardRenderPipeline();
    if (!pipeline->Initialize(device.get())) return false;
    pipeline->SetOutputResource(handles::INVALID_RESOURCE, {});
    pipeline->SetViewportSize(window_width_, window_height_);

    lumen::LumenConfig lumenConfig;
    lumenConfig.quality = lumen::LumenQualityPreset::Low;
    pipeline->SetLumenConfig(lumenConfig);
    pipeline->SetEditorMode(true);

    // 5. Empty scene (we render only to keep the pipeline live; the test
    //    asserts on solver state, not pixels).
    scene = new RenderScene();
    view = new RenderView();
    ViewportDesc viewport;
    viewport.size.x = static_cast<float>(window_width_);
    viewport.size.y = static_cast<float>(window_height_);
    view->SetViewport(viewport);

    // 6. Catalog setup — copied verbatim from TestWFCRendering so adjacency
    //    is consistent. RegisterWFCCatalogMeshes captures slot indices;
    //    SetupWFCCatalog builds registry_ + adjacency_ and overrides the
    //    placeholder mesh_handles with the real slot indices.
    RegisterWFCCatalogMeshes();
    SetupWFCCatalog();

    // 7. Streaming solver setup. seed=7 matches TestWFCRendering's default.
    grid_ = std::make_unique<primal::graphics::wfc::WaveGrid>();
    buf_  = std::make_unique<primal::graphics::wfc::WFCStepBuffer>();

    primal::graphics::wfc::WFCConfig config;
    config.grid_size = primal::graphics::wfc::WFCGridCoord{4, 4, 4};
    config.seed = 7;
    config.max_generations = 8;

    solver_ = std::make_unique<primal::graphics::wfc::WFCSolver>();
    solver_->Initialize(config, *grid_, *registry_, *adjacency_, *buf_);

    std::cout << "[TestWFCStreaming] solver ready: grid=4x4x4 seed=7" << std::endl;
    return true;
}

// ============================================================================
// WFCStreamingTestCase::RegisterWFCCatalogMeshes
// ============================================================================
//
// Copied verbatim from TestWFCRendering::RegisterWFCCatalogMeshes. Registers
// 5 procedural meshes (cube/ramp/corner_in/corner_out/pillar) and captures
// their ForwardSceneRenderer slot indices.

void WFCStreamingTestCase::RegisterWFCCatalogMeshes() {
    using namespace primal::content;

    auto* fwd = pipeline ? pipeline->GetForwardRenderer() : nullptr;
    const u32 slot_base = fwd ? fwd->GetMeshInfoCount() : 0;

    primal::id::id_type noTex[3] = {
        primal::id::invalid_id, primal::id::invalid_id, primal::id::invalid_id
    };

    // Order MUST match the WFCTileCatalog tile_id assignment (0..4):
    //   0=cube, 1=ramp, 2=corner_in, 3=corner_out, 4=pillar
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
    std::cout << "[TestWFCStreaming] Registered 5 catalog meshes: cube="
              << slot_cube << " ramp=" << slot_ramp
              << " corner_in=" << slot_corner_in
              << " corner_out=" << slot_corner_out
              << " pillar=" << slot_pillar << std::endl;
}

// ============================================================================
// WFCStreamingTestCase::SetupWFCCatalog
// ============================================================================
//
// Copied verbatim from TestWFCRendering::SetupWFCCatalog. Builds registry_ +
// adjacency_ via WFCTileCatalog::Populate, then overrides placeholder
// mesh_handles with real slot indices captured above.

void WFCStreamingTestCase::SetupWFCCatalog() {
    using namespace primal::graphics::wfc;
    registry_ = std::make_unique<WFCTileRegistry>();
    adjacency_ = std::make_unique<TileAdjacencyTable>();
    WFCTileCatalog::Populate(*registry_, *adjacency_);

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

    std::cout << "[TestWFCStreaming] Catalog ready (5 tiles, "
              << registry_->Count() << " registered)" << std::endl;
}

// ============================================================================
// WFCStreamingTestCase::PumpSolverFrame
// ============================================================================
//
// One solver Step + drain. Does NOT spawn entities (the test asserts on
// collapse count, not rendering). Restart accounting mirrors
// TestWFCRendering::PumpSolverFrame.

void WFCStreamingTestCase::PumpSolverFrame() {
    using namespace primal::graphics::wfc;

    budget_.Reset();
    solver_state_ = solver_->Step(budget_);

    WFCStreamDrainResult drain = WFCOutput::DrainStream(*buf_, *registry_, 1.0f);
    if (drain.restart_seen) {
        total_restarts_ += drain.restart_count;
    }
    total_collapses_ += drain.new_points.count;
}

// ============================================================================
// WFCStreamingTestCase::Run
// ============================================================================

void WFCStreamingTestCase::Run() {
    if (!pipeline || !scene || !view) return;

    // Pump up to 4 Steps per frame so the test completes in ~16 frames
    // (64 cells / 4 per frame). Stop early if the solver finishes.
    for (u32 i = 0; i < 4; ++i) {
        if (solver_state_ == primal::graphics::wfc::WFCSolver::StepResult::Done ||
            solver_state_ == primal::graphics::wfc::WFCSolver::StepResult::GivenUp) break;
        PumpSolverFrame();
    }

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
    if (solver_state_ == primal::graphics::wfc::WFCSolver::StepResult::Done ||
        frame_count_ >= kFrameCap) {
        std::cout << "[TestWFCStreaming] frames=" << frame_count_
                  << " collapses=" << total_collapses_
                  << " restarts=" << total_restarts_
                  << " state=" << static_cast<u32>(solver_state_) << std::endl;

        // Assertions: solver must complete within kFrameCap and produce
        // exactly 64 collapses for a 4x4x4 grid.
        assert(solver_state_ == primal::graphics::wfc::WFCSolver::StepResult::Done &&
               "solver must complete within kFrameCap=240 (seed=7, 4x4x4)");
        assert(total_collapses_ == kExpectedCollapses &&
               "4x4x4 grid must produce exactly 64 collapses (seed=7)");

#ifdef __APPLE__
        Shutdown();
        NS::Application::sharedApplication()->terminate(nullptr);
#endif
    }
}

// ============================================================================
// WFCStreamingTestCase::Shutdown
// ============================================================================
//
// Mirrors TestWFCRendering::Shutdown cleanup order: release streaming state
// before the catalog (solver_ holds raw pointers into grid_/buf_/registry_/
// adjacency_), then catalog before pipeline (registry mesh_handles point at
// ForwardSceneRenderer slots).

void WFCStreamingTestCase::Shutdown() {
    if (!pipeline && !scene) return;  // idempotent
    std::cout << "[TestWFCStreaming] Shutting down..." << std::endl;

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

// ============================================================================
// Engine_Test
// ============================================================================

Engine_Test::Engine_Test()
    : primal::test::RenderTestRunner(std::make_unique<WFCStreamingTestCase>())
{}
