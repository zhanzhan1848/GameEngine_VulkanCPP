#pragma once

// TestKenneyTilePreview.h — Kenney dungeon tile WFC streaming showcase.
//
// Application-layer binary (NOT engine core). Loads every .engine_mesh in
// EngineTest/assets/Processed/kenney_dungeon_tiles/ via KenneyTileCatalog,
// builds a hand-authored WFC registry from 8 of those tiles (wall + corridor
// variants + room + stairs), then streams a 16×1×16 Dungeon-only solve
// across frames. Each frame collapses a few cells, drains the post-restart
// Collapse steps from the buffer, spawns ECS entities via PCGEntityFactory,
// and re-publishes the cumulative list to StandardRenderPipeline.
//
// Architecture: this binary proves the "Core Layer as Capability Provider"
// principle — a brand-new asset pack (Kenney dungeon tiles, unrelated to the
// ruins tile set baked into WFCTileCatalog) runs the full WFC pipeline with
// zero changes to Engine/Graphics/WFC/ or any other engine-core module. The
// engine's content::create_resource + StandardRenderPipeline::RegisterMeshEntity
// + PCGEntityFactory + WFC subsystems compose into a working showcase.
//
// Native-only (Metal) today. WASM/WebGPU port is tracked separately as
// task #126 (follow-up spec).
//
// Smoke assertion: ≥ 100 cells collapsed AND ≥ 1 tile variety (≥ 3 distinct
// tile ids in the final grid). Visual quality (orientation, layout, materials)
// is human-reviewed — engine team will eyeball the running window.

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
#include "Engine/Graphics/WFC/WFCObserver.h"
#include "KenneyTileCatalog.h"
#include <memory>
#include <vector>

class KenneyTilePreviewTestCase : public primal::test::RenderTestCase {
public:
    // Observer strategy. OriginPreset is honored only when ObserverKind ==
    // DistanceFromOrigin; MinEntropy ignores it.
    enum class ObserverKind : u32 { MinEntropy = 0, DistanceFromOrigin = 1 };
    enum class OriginPreset : u32 { Center = 0, Corner = 1, BottomCenter = 2 };

    KenneyTilePreviewTestCase();
    bool Initialize() override;
    void Run() override;
    void Shutdown() override;

    // Singleton pointer set in Initialize() / cleared in Shutdown().
    // Used by TestKenneyMain.cpp's WASM C ABI exports to route panel
    // actions to the live test case. Native builds don't reference it.
    static KenneyTilePreviewTestCase* Instance() { return g_instance_; }

    // WASM↔JS bridge — called from EMSCRIPTEN_KEEPALIVE exports in
    // TestKenneyMain.cpp. Set the pending flag; HandleGridEditKeys()
    // drains them next frame and reseeds. Safe to call from JS thread
    // (emscripten runs single-threaded so no locking needed).
    void RequestObserver(u32 kind)   { pending_observer_ = kind; has_pending_observer_ = true; }
    void RequestOrigin(u32 preset)   { pending_origin_   = preset; has_pending_origin_   = true; }
    void RequestGridW(u32 w)         { pending_grid_w_   = w; has_pending_grid_w_ = true; }
    void RequestGridH(u32 h)         { pending_grid_h_   = h; has_pending_grid_h_ = true; }
    void RequestGridD(u32 d)         { pending_grid_d_   = d; has_pending_grid_d_ = true; }
    void RequestReseedSame()         { pending_reseed_same_ = true; }
    void RequestReseedNew()          { pending_reseed_new_  = true; }

    ObserverKind GetObserverKind() const { return observer_kind_; }
    OriginPreset GetOriginPreset() const { return origin_preset_; }
    u32 GetGridW() const { return grid_w_; }
    u32 GetGridH() const { return grid_h_; }
    u32 GetGridD() const { return grid_d_; }
    u32 GetSeed()   const { return rng_seed_; }

private:
    bool InitWFC();
    void ReseedSolver();
    void PumpSolverFrame();
    void DestroyAllSpawnedEntities();
    void HandleGridEditKeys();   // edge-triggered: re-reads grid_w_/h_/d_/seed
    void UpdateCamera();
    void UpdateCameraFromInput();
    void PrintControls();
    void PrintGridState(const char* why);

    std::unique_ptr<primal::graphics::rhi::RHIDeviceBase> device;
    primal::platform::window window;
    primal::graphics::RenderSystem renderSystem;
    primal::graphics::StandardRenderPipeline* pipeline = nullptr;
    primal::graphics::RenderScene* scene = nullptr;
    primal::graphics::RenderView* view = nullptr;

    // Asset catalog (owns std::strings backing WFCTile::name pointers).
    primal::test::kenney::KenneyTileCatalog catalog_;

    // WFC streaming state. Order matters on shutdown: solver holds raw
    // pointers into grid_/buf_/registry_/adjacency_, so solver_.reset() first.
    std::unique_ptr<primal::graphics::wfc::WFCTileRegistry>   registry_;
    std::unique_ptr<primal::graphics::wfc::TileAdjacencyTable> adjacency_;
    std::unique_ptr<primal::graphics::wfc::WaveGrid>          grid_;
    std::unique_ptr<primal::graphics::wfc::WFCStepBuffer>     buf_;
    std::unique_ptr<primal::graphics::wfc::WFCSolver>         solver_;
    std::unique_ptr<primal::graphics::wfc::WFCSolveBudget>    budget_;

    std::vector<primal::id::id_type> wfc_entity_ids_;
    std::vector<u32>                 wfc_mesh_slots_;

    primal::graphics::wfc::WFCSolver::StepResult solver_state_{};
    bool solver_done_{false};
    u32  total_collapses_{0};
    u32  total_restarts_{0};
    u32  rng_seed_{1337};

    // Dynamic grid dimensions. Adjusted live via hotkeys; ReseedSolver uses
    // these. Clamps keep the solver / camera framing well-defined.
    u32 grid_w_{16};   // X — clamped [4, 64]
    u32 grid_h_{4};    // Y (layers) — clamped [1, 32]
    u32 grid_d_{16};   // Z — clamped [4, 64]

    // Observer strategy state. O cycles ObserverKind; P cycles OriginPreset.
    // Both hotkeys trigger ReseedSolver() so the demo always reflects the
    // current strategy. OriginPreset is honored only when ObserverKind ==
    // DistanceFromOrigin; MinEntropy ignores it.
    ObserverKind  observer_kind_{ObserverKind::MinEntropy};
    OriginPreset  origin_preset_{OriginPreset::Center};

    // Pending actions set by the WASM panel bridge. Drained in
    // HandleGridEditKeys() — JS writes happen between frames so we just
    // latch the latest value. has_pending_* collapses multiple writes into
    // one drain.
    u32  pending_observer_{0};
    u32  pending_origin_{0};
    u32  pending_grid_w_{0};
    u32  pending_grid_h_{0};
    u32  pending_grid_d_{0};
    bool has_pending_observer_{false};
    bool has_pending_origin_{false};
    bool has_pending_grid_w_{false};
    bool has_pending_grid_h_{false};
    bool has_pending_grid_d_{false};
    bool pending_reseed_same_{false};
    bool pending_reseed_new_{false};

    primal::graphics::wfc::WFCGridCoord ComputeOrigin() const;

    // Edge-triggered key state for grid-edit hotkeys. Index = input_code value.
    // We only track a handful of keys; the array is sized for direct indexing.
    bool prev_key_state_[256]{};
    bool just_pressed_(u32 code, bool now);

    u32 window_width_{1280};
    u32 window_height_{720};
    u64 frame_count_{0};

    // Smoke criteria: enforced in PumpSolverFrame on solver completion. The
    // showcase is a GUI binary, so failure logs to stderr rather than crashing
    // the window (no hard assert).
    static constexpr u32 kMinCellsCollapsed = 100;
    static constexpr u32 kMinDistinctTiles  = 3;

    // FPS-style camera. Yaw spins around +Y, pitch clamps to ±~89° to avoid
    // flip. Forward/Right derived each frame from yaw/pitch for translation.
    // Convention matches TestForwardRenderer.cpp:
    //   forward = (sin(yaw)*cos(pitch), sin(pitch), -cos(yaw)*cos(pitch))
    //   yaw=0 → -Z, yaw=90 → +X.
    // Initial position default-frames a 16×4×16 grid at cell_size=4m; user
    // flies manually once the showcase starts.
    primal::math::v3 camera_pos_{36.0f, 28.0f, 52.0f};
    float camera_yaw_{-0.7853982f};    // ~-45° → looks toward -X/-Z (back at origin)
    float camera_pitch_{-0.6108652f};  // ~-35° → looks down at the grid
    float camera_speed_{14.0f};        // meters/second; Shift = 3× sprint

    static KenneyTilePreviewTestCase* g_instance_;
};

class Engine_Test : public primal::test::RenderTestRunner {
public:
    Engine_Test();
};
