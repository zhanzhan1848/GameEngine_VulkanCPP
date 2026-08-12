#pragma once

// TestKenneyTilePreview.h — WFC tile-source composition showcase.
//
// Application-layer binary (NOT engine core). Composes one or more
// IWFCTileStyle implementations (registered in
// graphics::wfc::WFCTileStyleRegistry) into a single WFCTileRegistry, then
// streams a solve across frames. Each frame collapses a few cells, drains
// the post-restart Collapse steps from the buffer, spawns ECS entities via
// PCGEntityFactory, and re-publishes the cumulative list to
// StandardRenderPipeline.
//
// Architecture: this binary proves the "Core Layer as Capability Provider"
// principle — the engine exposes IWFCTileStyle + WFCTileStyleRegistry as
// composable building blocks; this binary (EngineTest layer) registers its
// own KenneyDungeonStyle (Kenney FBX = demo fixture) alongside the
// engine-native RuinsStyle + ProceduralRoomPackStyle and lets the registry's
// Compose() build a unified registry/adjacency set. Adding a new style = one
// RegisterStyle call; no other site needs editing.
//
// Native (Metal/Vulkan) + WASM (Dawn). The WASM panel dropdown auto-
// populates from the registry via the wfc_get_style_* C ABI; an "Advanced"
// multi-select path calls wfc_compose(indices, count).
//
// Smoke assertion: ≥ 100 cells collapsed AND ≥ 1 tile variety (≥ 3 distinct
// tile ids in the final grid). Multi-style compositions additionally enforce
// ≥ 2 distinct categories (catches the silent failure mode where the
// classifier prunes one source out of every cell). Visual quality
// (orientation, layout, materials) is human-reviewed.

#include "RenderTestFramework.h"
#include "Engine/Graphics/RenderPipeline/StandardRenderPipeline.h"
#include "Engine/Graphics/RHI/Core/RHIDevice.h"
#include "Engine/Graphics/RHI/Core/RHIMeshAsset.h"
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
#include "Engine/Graphics/WFC/WFCTileStyleRegistry.h"
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
    // Compose a subset of registered styles. style_indices reference the
    // WFCTileStyleRegistry's stable index (registration order). Empty vector
    // is a no-op; the next HandleGridEditKeys keeps the current composition.
    void RequestCompose(std::vector<u32> style_indices) {
        pending_compose_ = std::move(style_indices);
        has_pending_compose_ = true;
    }

    ObserverKind GetObserverKind() const { return observer_kind_; }
    OriginPreset GetOriginPreset() const { return origin_preset_; }
    const std::vector<u32>& GetActiveStyles() const { return active_styles_; }
    u32 GetGridW() const { return grid_w_; }
    u32 GetGridH() const { return grid_h_; }
    u32 GetGridD() const { return grid_d_; }
    u32 GetSeed()   const { return rng_seed_; }

private:
    // Register the 3 built-in styles (KenneyDungeon, Ruins,
    // ProceduralRoomPack) into WFCTileStyleRegistry on first call. Idempotent
    // — subsequent calls are no-ops. The registry owns the Style objects for
    // program lifetime; we hold raw pointers / indices, never the unique_ptrs.
    void RegisterStylesIfNeeded();

    // Build registry_+adjacency_ from active_styles_ via Compose, then wire
    // per-tile mesh_handles to render slots. Dungeon-category tiles self-
    // register rendering inside their Style (LoadFromDirectory already called
    // RegisterMeshResource); Ruins/Primitive tiles are registered here via
    // content::RegisterProceduralMesh + pipeline->RegisterMeshEntity.
    bool InitFromActiveStyles();
    void ReseedSolver();
    void PumpSolverFrame();
    void DestroyAllSpawnedEntities();
    void HandleGridEditKeys();   // edge-triggered: re-reads grid_w_/h_/d_/seed
    void UpdateCamera();
    void UpdateCameraFromInput();
    void PrintControls();
    void PrintGridState(const char* why);
    // End-of-solve smoke check. Logs PASS/FAIL but never crashes (GUI binary
    // keeps the window open on failure for inspection). Criteria:
    // ≥ kMinCellsCollapsed cells, ≥ kMinDistinctTiles tile ids, and (when
    // active_styles_ spans multiple categories) ≥ kMinDistinctCategories.
    void CheckSmokeAsserts();
    // When max_generations is exhausted without a solve, narrow
    // active_category_mask to the first category in active_styles_ and
    // reseed. Logs the degradation so the operator knows the solver gave up
    // on the mixed set.
    void DegradeToSingleSet();

    std::unique_ptr<primal::graphics::rhi::RHIDeviceBase> device;
    primal::platform::window window;
    primal::graphics::RenderSystem renderSystem;
    primal::graphics::StandardRenderPipeline* pipeline = nullptr;
    primal::graphics::RenderScene* scene = nullptr;
    primal::graphics::RenderView* view = nullptr;

    // Kenney asset directory (resolved at Initialize). Forwarded to
    // KenneyDungeonStyle on first registration.
    std::string kenney_dir_;
    std::string kenney_colormap_;

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

    // Active style composition (indices into WFCTileStyleRegistry). Default
    // = {0} (first registered style = KenneyDungeon). M cycles through the
    // 4 presets below; WASM panel dropdown / Compose panel write via
    // RequestCompose. Switching the composition re-runs InitFromActiveStyles
    // (rebuilds registry_/adjacency_ from scratch), not just ReseedSolver.
    //
    // M preset cycle (registration order: 0=Kenney, 1=Ruins, 2=Pack):
    //   {0}     → Kenney only
    //   {1}     → Ruins only
    //   {2}     → Pack only
    //   {1,2}   → Ruins + Pack (multi-category mix)
    std::vector<u32> active_styles_{0};

    // OR of CategoryMaskFor(style->GetCategory()) over active_styles_.
    // Populated by InitFromActiveStyles (via Compose's out_category_mask) and
    // consumed by ReseedSolver when not degraded.
    u64 active_category_mask_{0};

    // Set true once RegisterStylesIfNeeded has run; prevents re-running on
    // every composition change.
    bool styles_registered_{false};

    // Pending actions set by the WASM panel bridge. Drained in
    // HandleGridEditKeys() — JS writes happen between frames so we just
    // latch the latest value. has_pending_* collapses multiple writes into
    // one drain.
    u32  pending_observer_{0};
    u32  pending_origin_{0};
    u32  pending_grid_w_{0};
    u32  pending_grid_h_{0};
    u32  pending_grid_d_{0};
    std::vector<u32> pending_compose_;
    bool has_pending_observer_{false};
    bool has_pending_origin_{false};
    bool has_pending_grid_w_{false};
    bool has_pending_grid_h_{false};
    bool has_pending_grid_d_{false};
    bool has_pending_compose_{false};
    bool pending_reseed_same_{false};
    bool pending_reseed_new_{false};

    // When the solver exhausts max_generations on the active composition,
    // the next PumpSolverFrame fires DegradeToSingleSet to fall back to a
    // single-category solve (the first style's category). Cleared on every
    // ReseedSolver.
    bool degraded_to_single_set_{false};

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
    // Multi-style compositions (active_styles_.size() > 1) must show actual
    // cross-category mixing (≥ 2 distinct WFCCategory values among collapsed
    // cells). The threshold catches the silent failure mode where
    // BuildFromClassifier prunes one category's tiles out of every cell,
    // leaving a single-set solve dressed up as a multi-set one. Single-style
    // compositions are exempt (only 1 category exists in the registry).
    static constexpr u32 kMinDistinctCategoriesMulti = 2;

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
