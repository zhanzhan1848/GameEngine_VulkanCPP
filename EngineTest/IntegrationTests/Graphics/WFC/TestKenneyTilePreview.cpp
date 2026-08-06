// TestKenneyTilePreview.cpp — see header for architecture rationale.
//
// Native-only (Metal) today. Boots a Metal-backed window + StandardRender
// Pipeline, loads every .engine_mesh in EngineTest/assets/Processed/
// kenney_dungeon_tiles/ via KenneyTileCatalog, builds a WFC registry from 8
// of those tiles, then streams a 16×1×16 Dungeon-only solve across frames.
// Each frame: solver Step() collapses a few cells, DrainStream() pulls
// post-restart Collapse points, entities are spawned via PCGEntityFactory
// and re-published to the pipeline. WASM/WebGPU port is tracked separately
// as task #126 (follow-up spec).
//
// The viewer watches the dungeon grow tile-by-tile. WASD + arrows fly the
// camera. Window-close / Ctrl-C quits.

#include "TestKenneyTilePreview.h"
#include "Engine/Common/CommonHeaders.h"
#include "Engine/Content/ContentToEngine.h"
#include "Engine/Content/ProceduralMesh.h"
#include "Engine/EngineAPI/Input.h"
#if defined(ENABLE_WEBGPU) && ENABLE_WEBGPU
#include "Engine/Graphics/RHI/Platforms/Dawn/DawnDevice.h"
#elif defined(__APPLE__)
#include "Engine/Graphics/RHI/Platforms/Metal/MetalDevice.h"
#endif
#include "Engine/Graphics/RHI/Core/RHITypes.h"
#include "Engine/Graphics/Lumen/LumenTypes.h"
#include "Engine/Graphics/WFC/WFCConfig.h"
#include "Engine/Graphics/WFC/WFCOutput.h"
#include "Engine/Graphics/WFC/WFCCategory.h"
#include "Engine/Graphics/WFC/WFCDistanceObserver.h"
#include "Engine/Graphics/WFC/WFCTileCatalog.h"
#include "Engine/Graphics/WFC/AutoSocketClassifier.h"
#include "Engine/Graphics/WFC/ProceduralRoomPack.h"
#include "Engine/Graphics/PCG/PCGTypes.h"
#include "Engine/Graphics/PCG/PCGEntityFactory.h"

#ifdef __APPLE__
#include <AppKit/AppKit.hpp>
#endif

#include <iostream>
#include <cmath>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <cstdio>
#include <set>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
// Bridges observer/origin/grid dims/seed to the wfc/shell.html HUD via JS.
// No-op on native builds (the entire function body is ifdef'd out).
static void SyncWfcHudIfWasm(const char* obs, const char* origin,
                              u32 grid_w, u32 grid_h, u32 grid_d, u32 seed) {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "if (window.setWfcHud) window.setWfcHud('%s','%s',%u,%u,%u,%u);",
                  obs, origin, grid_w, grid_h, grid_d, seed);
    emscripten_run_script(buf);
}
// Mouse + keyboard from EmscriptenInput.cpp. We use EmscriptenGetKeyState
// (raw keyCode lookup against a local array updated by emscripten callbacks)
// instead of primal::input — primal::input's keyboard path is unused on WASM
// and routes through a different system than the basic renderer demo. The
// basic renderer uses this exact pattern and WASD works there.
//   W=87 A=65 S=83 D=68 Q=81 E=69 Shift=16
//   Arrows: left=37 up=38 right=39 down=40
extern "C" {
void EmscriptenGetMouseDelta(float* dx, float* dy);
bool EmscriptenGetMouseButton(int button);
bool EmscriptenGetKeyState(int keyCode);
}
#else
static inline void SyncWfcHudIfWasm(const char*, const char*, u32, u32, u32, u32) {}
#endif

using namespace primal::graphics;
using namespace primal::graphics::rhi;
using namespace primal::math;

// Singleton instance pointer — set in Initialize, cleared in Shutdown.
// Lets the WASM C ABI exports route panel actions to the live test case.
KenneyTilePreviewTestCase* KenneyTilePreviewTestCase::g_instance_{nullptr};

// ============================================================================
// Engine_Test
// ============================================================================

Engine_Test::Engine_Test()
    : primal::test::RenderTestRunner(std::make_unique<KenneyTilePreviewTestCase>())
{}

// ============================================================================
// KenneyTilePreviewTestCase ctor
// ============================================================================

KenneyTilePreviewTestCase::KenneyTilePreviewTestCase() {
    // 4 cells per Step × 1 Step per frame → ~64 frames for a 256-cell grid.
    // 16ms budget is generous; on Apple Silicon the solver finishes each
    // Step in well under 1ms.
    budget_ = std::make_unique<primal::graphics::wfc::WFCSolveBudget>(4u, 16u);
}

// ============================================================================
// Initialize
// ============================================================================

bool KenneyTilePreviewTestCase::Initialize() {
    std::cout << "[TestKenneyTilePreview] Initializing..." << std::endl;

    // Register singleton (used by WASM C ABI panel bridge).
    g_instance_ = this;

    // 1. Window
    primal::platform::window_init_info winInfo{};
    winInfo.caption = "TestKenneyTilePreview - Kenney Dungeon WFC";
    winInfo.width = window_width_;
    winInfo.height = window_height_;
    window = primal::platform::create_window(&winInfo);
    if (!window.is_valid()) return false;

    // 2. RHI device + register with global device_manager.
    //    ENABLE_WEBGPU path uses Dawn (works on macOS native + WASM).
    //    __APPLE__ path uses Metal (native macOS only).
    DeviceDesc desc;
    desc.enableDebug = true;
#if defined(ENABLE_WEBGPU) && ENABLE_WEBGPU
    desc.platform = RHIPlatform::Dawn;
    auto* rhiDevice = new DawnDevice(desc);
#elif defined(__APPLE__)
    desc.platform = RHIPlatform::Metal;
    auto* rhiDevice = new MetalDevice(desc);
#else
    #error "TestKenneyTilePreview requires either ENABLE_WEBGPU or __APPLE__"
#endif
    if (!rhiDevice || !rhiDevice->Initialize()) {
        delete rhiDevice;
        return false;
    }
    device.reset(rhiDevice);
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

    // Off preset: skip SSAO/SSGI/DDGI subsystem init entirely.
    // WebGPU rejects LumenSSAOPass's R16Float + StorageBinding textures
    // (R16Float is not in the storage-texture allowed-format list), so any
    // preset with enable_ssao=true crashes on first frame. WFC uses
    // ForwardSceneRenderer via editor mode — no Lumen GI needed.
    lumen::LumenConfig lumenConfig;
    lumenConfig.quality = lumen::LumenQualityPreset::Off;
    pipeline->SetLumenConfig(lumenConfig);
    pipeline->SetEditorMode(true);

    // 5. Empty scene + camera/view.
    scene = new RenderScene();
    view = new RenderView();
    ViewportDesc viewport;
    viewport.size.x = static_cast<float>(window_width_);
    viewport.size.y = static_cast<float>(window_height_);
    view->SetViewport(viewport);

    // 6. Load Kenney tiles + build WFC registry.
    //    Try multiple candidate paths: test binary may be run from project
    //    root, build dir, or Darwin/Release/. Whichever exists first wins.
    namespace fs = std::filesystem;
    struct DirCandidate { std::string mesh_dir; std::string colormap; };
    const std::vector<DirCandidate> candidates = {
        {"EngineTest/assets/Processed/kenney_dungeon_tiles",
         "EngineTest/assets/Raw/kenney_dungeon_tiles/Textures/colormap.png"},
        {"../EngineTest/assets/Processed/kenney_dungeon_tiles",
         "../EngineTest/assets/Raw/kenney_dungeon_tiles/Textures/colormap.png"},
        {"../../EngineTest/assets/Processed/kenney_dungeon_tiles",
         "../../EngineTest/assets/Raw/kenney_dungeon_tiles/Textures/colormap.png"},
        {"../../../EngineTest/assets/Processed/kenney_dungeon_tiles",
         "../../../EngineTest/assets/Raw/kenney_dungeon_tiles/Textures/colormap.png"},
    };
    u32 loaded = 0;
    for (const auto& cand : candidates) {
        if (fs::exists(cand.mesh_dir) && fs::is_directory(cand.mesh_dir)) {
            loaded = catalog_.LoadFromDirectory(pipeline, cand.mesh_dir, cand.colormap);
            if (loaded > 0) break;
        }
    }
    if (loaded == 0) {
        std::cerr << "[TestKenneyTilePreview] SMOKE FAIL: no Kenney tiles loaded"
                  << " (searched " << candidates.size() << " candidate paths)"
                  << std::endl;
        return false;
    }

    if (!InitWFC()) {
        std::cerr << "[TestKenneyTilePreview] InitWFC failed — aborting." << std::endl;
        return false;
    }
    UpdateCamera();
    PrintControls();
    PrintGridState("init");

    // Push initial observer/origin/grid to the WASM HUD (no-op on native).
    SyncWfcHudIfWasm("MinEntropy", "Center", grid_w_, grid_h_, grid_d_, rng_seed_);

    std::cout << "[TestKenneyTilePreview] Pipeline + scene + " << loaded
              << " catalog tiles + WFC registry ready" << std::endl;
    return true;
}

// ============================================================================
// InitWFC
// ============================================================================
//
// Builds the WFC registry (8 hand-picked Kenney tile types) from the catalog,
// then reseeds the streaming solver state (grid / step buffer / solver).
// Catalog slot ordering is stable across reseeds — only the registry /
// adjacency survive a restart inside the solver; mesh_handles point at the
// same render slots.

bool KenneyTilePreviewTestCase::InitWFC() {
    using namespace primal::graphics::wfc;

    registry_  = std::make_unique<WFCTileRegistry>();
    adjacency_ = std::make_unique<TileAdjacencyTable>();

    // Phase C.1 Task 14: dispatch on tile-source composition. MixedMulti
    // builds a brand-new registry/adjacency set (Ruins + ProceduralRoomPack)
    // via InitMixedMultiCategory; KenneyOnly keeps the original catalog
    // path. Each branch fully initializes registry_ + adjacency_.
    if (solve_mode_ == SolveMode::MixedMulti) {
        if (!InitMixedMultiCategory()) {
            std::cerr << "[TestKenneyTilePreview] InitMixedMultiCategory failed"
                      << " — falling back to KenneyOnly" << std::endl;
            solve_mode_ = SolveMode::KenneyOnly;
            registry_  = std::make_unique<WFCTileRegistry>();
            adjacency_ = std::make_unique<TileAdjacencyTable>();
        }
    }

    if (solve_mode_ == SolveMode::KenneyOnly) {
        const u32 registered = catalog_.BuildWFCRegistry(*registry_, *adjacency_);
        if (registered == 0) {
            std::cerr << "[TestKenneyTilePreview] BuildWFCRegistry failed"
                      << " (catalog missing one of the 8 required Kenney tiles)"
                      << std::endl;
            return false;
        }
    }

    ReseedSolver();
    return true;
}

// ============================================================================
// InitMixedMultiCategory (Phase C.1 Task 14)
// ============================================================================
//
// Composes a 27-tile registry from two sources:
//   * Ruins (15 tiles, WFCCategory::Ruins) via WFCTileCatalog::Populate,
//     which also auto-fills the placeholder socket adjacency. We discard
//     that adjacency and rebuild from the AutoSocketClassifier below so
//     both sources share one socket encoding (8×8 occupancy grid).
//   * ProceduralRoomPack (12 tiles, WFCCategory::Primitive) generated
//     procedurally at runtime — no disk assets.
//
// Mesh data flow:
//   1. Each tile's RHIMeshAsset is owned by ruins_meshes_ /
//      procedural_room_meshes_ (value-typed vectors; assets outlive the
//      solver). Pointers are captured in mesh_lookup_ for the classifier.
//   2. Each tile is also registered as a render entity via
//      RegisterProceduralMesh + RegisterMeshEntity so spawned ECS entities
//      render correctly. The render slot index is written into
//      WFCTile::mesh_handles[0..variant_count-1].
//   3. WFCTile::mesh_handles[] store the slot index (cast to geometry_id),
//      matching the convention used by TestWFCRuinsRendering and
//      TestWFCStreaming.
//
// Adjacency:
//   AutoSocketClassifier ray-traces each face against the original
//   RHIMeshAsset, builds a 64-bit occupancy signature, and matches opposing
//   faces after MirrorFlipU. This replaces the corner-byte socket encoding
//   that Populate just wrote, so both categories share one compatibility
//   table.

bool KenneyTilePreviewTestCase::InitMixedMultiCategory() {
    using namespace primal::graphics::wfc;
    using namespace primal::graphics::rhi;
    namespace content = primal::content;

    // -- 1. Ruins: 15 tiles via WFCTileCatalog::Populate --
    WFCTileCatalog::Populate(*registry_, *adjacency_);
    const u32 ruins_count = registry_->Count();   // expected 15
    if (ruins_count == 0) {
        std::cerr << "[TestKenneyTilePreview] Populate yielded 0 tiles"
                  << std::endl;
        return false;
    }

    // Register ruins procedural meshes for rendering + capture RHIMeshAsset*.
    //
    // Each of the 15 ruins tile IDs dispatches to its matching factory so the
    // demo shows distinct topology (cube, ramp, corner_in/out, broken_cube,
    // collapsed_pillar, rubble_pile, debris_small). Tiles 0/6/10/11 stay on
    // weathered_cube_mesh with different seed/amp so cube variants still
    // differ by texture. Tiles 9 (cracked_wall) and 12/13 (broken_corner_in/
    // out) currently fall back to cube/notched-cube geometry — see
    // ProceduralMesh.h comments at create_cracked_wall_mesh (line 699) and
    // create_broken_corner_in_mesh (line 617) for the Phase C.1 stub status.
    primal::id::id_type noTex[3] = {
        primal::id::invalid_id, primal::id::invalid_id, primal::id::invalid_id
    };
    ruins_meshes_.clear();
    ruins_meshes_.resize(ruins_count);
    auto register_mesh = [&](RHIMeshAsset& asset) -> u32 {
        const primal::id::id_type geo = content::RegisterProceduralMesh(asset);
        pipeline->RegisterMeshEntity(geo, noTex, 3);
        auto* fwd = pipeline->GetForwardRenderer();
        return fwd ? (fwd->GetMeshInfoCount() - 1u) : 0u;
    };
    // Per-tile factory dispatch. Params chosen for visual variety within
    // each factory's output range (different seeds, slopes, tilt axes).
    auto build_tile_asset = [&](RHIMeshAsset& out, u32 tid) {
        using namespace primal::content;
        switch (tid) {
            case 0:  // cube
                create_weathered_cube_mesh(out, 1,1,1, 100u, 0.020f);
                break;
            case 1:  // ramp — full slope (slope_height=0 → ramp top to floor at +Z)
                create_ramp_mesh(out, 1.0f, 1.0f, 1.0f, 0.0f);
                break;
            case 2:  // corner_in — concave L-shape
                create_corner_in_mesh(out, 1.0f, 1.0f, 1.0f);
                break;
            case 3:  // corner_out — convex octant frame
                create_corner_out_mesh(out, 1.0f, 1.0f, 1.0f);
                break;
            case 4:  // pillar — collapsed_pillar standing (small tilt)
                create_collapsed_pillar_mesh(out, 0.5f, 1.0f,
                                             TiltAxis::PlusX, 0.05f);
                break;
            case 5:  // broken_cube — notched corner
                create_broken_cube_mesh(out, 1.0f, 1.0f, 1.0f,
                                        BrokenCorner::PosXYZ);
                break;
            case 6:  // mossy_cube — low-amp weathering (dense moss)
                create_weathered_cube_mesh(out, 1,1,1, 600u, 0.005f);
                break;
            case 7:  // collapsed_pillar — heavier tilt, different axis
                create_collapsed_pillar_mesh(out, 0.5f, 1.0f,
                                             TiltAxis::PlusZ, 0.20f);
                break;
            case 8:  // rubble_pile — multi-box cluster
                create_rubble_pile_mesh(out, 800u, 0.5f);
                break;
            case 9:  // cracked_wall — Phase C.1 stub (emits plain cube)
                create_cracked_wall_mesh(out, 1.0f, 1.0f, 1.0f, 900u);
                break;
            case 10: // vine_cube — high-amp weathering (deep vines)
                create_weathered_cube_mesh(out, 1,1,1, 700u, 0.030f);
                break;
            case 11: // weathered_stone — medium-amp weathering
                create_weathered_cube_mesh(out, 1,1,1, 800u, 0.015f);
                break;
            case 12: // broken_corner_in — notched at PosX-NegZ corner
                create_broken_corner_in_mesh(out, 1.0f, 1.0f, 1.0f,
                                             BrokenCorner::PosXNegZ);
                break;
            case 13: // broken_corner_out — notched at NegX-PosZ corner
                create_broken_corner_out_mesh(out, 1.0f, 1.0f, 1.0f,
                                              BrokenCorner::NegXPosZ);
                break;
            case 14: // debris_small — small scattered boxes
                create_debris_small_mesh(out, 900u, 0.35f);
                break;
            default: // unreachable (ruins_count == 15)
                create_weathered_cube_mesh(out, 1,1,1, 999u, 0.020f);
                break;
        }
    };
    for (u32 tid = 0; tid < ruins_count; ++tid) {
        build_tile_asset(ruins_meshes_[tid], tid);
    }

    // Override WFCTile::mesh_handles[v] with the render slot index. All
    // variants within one tile share a mesh (variant rotation is a socket
    // concern). Also build mesh_lookup_ for the classifier callback.
    mesh_lookup_.clear();
    mesh_lookup_.resize(ruins_count, nullptr);
    for (u32 tid = 0; tid < ruins_count; ++tid) {
        WFCTile& t = registry_->GetMutable(wfc_tile_id{tid});
        const u32 slot = register_mesh(ruins_meshes_[tid]);
        for (u32 v = 0; v < WFCTile::MaxVariants; ++v) {
            t.mesh_handles[v] = primal::geometry::geometry_id{slot};
        }
        // Force category = Ruins for the populated set. Populate already
        // sets this for tiles 5..14, but tiles 0..4 inherit Primitive from
        // the WFCTile default. Override so the active_category_mask logic
        // (Ruins | Primitive) draws from both sources as intended.
        t.category = WFCCategory::Ruins;
        mesh_lookup_[tid] = &ruins_meshes_[tid];
    }

    // -- 2. ProceduralRoomPack: 12 tiles, appended after ruins --
    procedural_room_meshes_.clear();
    procedural_room_meshes_.resize(ProceduralRoomPack::kTileCount);
    for (u32 i = 0; i < ProceduralRoomPack::kTileCount; ++i) {
        ProceduralRoomPack::GenerateTileMesh(i, procedural_room_meshes_[i]);

        WFCTile t{};
        t.name = ProceduralRoomPack::TileDefs()[i].name;
        t.category = WFCCategory::Primitive;
        t.variant_count = 1;
        t.bounds_extents = math::v3{1.0f, 1.0f, 1.0f};

        const u32 slot = register_mesh(procedural_room_meshes_[i]);
        for (u32 v = 0; v < WFCTile::MaxVariants; ++v) {
            t.mesh_handles[v] = primal::geometry::geometry_id{slot};
        }
        registry_->Register(t);
        mesh_lookup_.push_back(&procedural_room_meshes_[i]);
    }

    // -- 3. Rebuild adjacency from classifier --
    // WFCTileCatalog::Populate filled adjacency_ with corner-byte entries
    // (placeholder sockets). Those encodings are not the 8×8 grid format
    // we use here — clear and rebuild from the AutoSocketClassifier so
    // both Ruins and Primitive tiles share one compatibility table.
    adjacency_->Clear();
    const u32 added = adjacency_->BuildFromClassifier(
        *registry_,
        [this](u32 tile_index, u32 /*variant*/) -> const RHIMeshAsset* {
            return tile_index < mesh_lookup_.size() ? mesh_lookup_[tile_index]
                                                    : nullptr;
        });

    std::cout << "[TestKenneyTilePreview] MixedMulti registry: "
              << registry_->Count() << " tiles ("
              << ruins_count << " ruins + "
              << ProceduralRoomPack::kTileCount << " procedural), "
              << added << " adjacency entries"
              << std::endl;
    return added > 0;
}

// ============================================================================
// ComputeOrigin
// ============================================================================
//
// Maps the current OriginPreset to a grid coordinate. Called whenever a
// WFCDistanceObserver is constructed (in ReseedSolver). MinEntropy ignores
// the result.

primal::graphics::wfc::WFCGridCoord KenneyTilePreviewTestCase::ComputeOrigin() const {
    using namespace primal::graphics::wfc;
    switch (origin_preset_) {
        case OriginPreset::Center:
            return {static_cast<s32>(grid_w_) / 2,
                    static_cast<s32>(grid_h_) / 2,
                    static_cast<s32>(grid_d_) / 2};
        case OriginPreset::Corner:
            return {0, 0, 0};
        case OriginPreset::BottomCenter:
            return {static_cast<s32>(grid_w_) / 2, 0,
                    static_cast<s32>(grid_d_) / 2};
    }
    return {static_cast<s32>(grid_w_) / 2,
            static_cast<s32>(grid_h_) / 2,
            static_cast<s32>(grid_d_) / 2};
}

// ============================================================================
// ReseedSolver
// ============================================================================
//
// Re-initializes grid_/buf_/solver_ with the current grid_w_/h_/d_/rng_seed_.
// Catalog (registry_ + adjacency_) is preserved — built once in InitWFC.

void KenneyTilePreviewTestCase::ReseedSolver() {
    using namespace primal::graphics::wfc;

    DestroyAllSpawnedEntities();

    grid_ = std::make_unique<WaveGrid>();
    buf_  = std::make_unique<WFCStepBuffer>();

    WFCConfig config;
    config.grid_size  = WFCGridCoord{
        static_cast<s32>(grid_w_),
        static_cast<s32>(grid_h_),
        static_cast<s32>(grid_d_)};
    config.seed       = rng_seed_;
    config.max_generations = 32;

    // Phase C.1 Task 14: category mask depends on mode + degradation state.
    //   KenneyOnly                       → Dungeon
    //   MixedMulti (nominal)             → Ruins | Primitive
    //   MixedMulti (degraded, see T15)   → Ruins only
    if (solve_mode_ == SolveMode::MixedMulti) {
        if (degraded_to_single_set_) {
            config.active_category_mask = CategoryMaskFor(WFCCategory::Ruins);
        } else {
            config.active_category_mask =
                CategoryMaskFor(WFCCategory::Ruins) |
                CategoryMaskFor(WFCCategory::Primitive);
        }
    } else {
        config.active_category_mask = CategoryMaskFor(WFCCategory::Dungeon);
    }

    solver_ = std::make_unique<WFCSolver>();

    // Inject the strategy selected by observer_kind_. Default-constructed
    // WFCSolver already holds a WFCMinEntropyObserver, so we only need to
    // swap when DistanceFromOrigin is requested. SetObserver MUST be called
    // before Initialize (the observer's state is populated during Initialize).
    if (observer_kind_ == ObserverKind::DistanceFromOrigin) {
        solver_->SetObserver(
            std::make_unique<WFCDistanceObserver>(ComputeOrigin()));
    }

    solver_->Initialize(config, *grid_, *registry_, *adjacency_, *buf_);

    solver_state_    = WFCSolver::StepResult::InProgress;
    solver_done_     = false;
    total_collapses_ = 0;
    total_restarts_  = 0;
}

// ============================================================================
// HandleGridEditKeys
// ============================================================================
//
// Edge-triggered hotkeys for dynamic grid dimensions + reseed.
//   [ / ]      grid_w (X) -- / ++     clamped [4, 32]
//   , / .      grid_d (Z) -- / ++     clamped [4, 32]
//   - / +      grid_h (Y) -- / ++     clamped [1, 8]
//   R          reseed with the SAME seed (regenerate identical layout)
//   T          reseed with a NEW random seed (reroll layout)
//
// On any change: tear down current entities + solver, reseed with new params,
// reset the camera to frame the new grid. Print one status line per change.

bool KenneyTilePreviewTestCase::just_pressed_(u32 code, bool now) {
    if (code >= 256) return false;
    const bool edge = now && !prev_key_state_[code];
    prev_key_state_[code] = now;
    return edge;
}

void KenneyTilePreviewTestCase::HandleGridEditKeys() {
#ifdef __EMSCRIPTEN__
    // On WASM we read keyboard state via EmscriptenGetKeyState (raw keyCode
    // lookup against the array maintained by emscripten callbacks). The
    // panel UI also drains here, so WASM has two input paths both ending
    // at the same ReseedSolver call.
    auto key_now = [](int emscripten_key) {
        return EmscriptenGetKeyState(emscripten_key);
    };
    // Emscripten keyCodes used below (kept as int so the lambda call matches).
    constexpr int kBracketOpen = 219, kBracketClose = 221;
    constexpr int kComma = 188, kPeriod = 190, kMinus = 189, kPlus = 187;
    constexpr int kR = 82, kT = 84, kO = 79, kP = 80, kM = 77;
#else
    using ic = primal::input::input_code;
    auto key_now = [](ic::code c) {
        primal::input::input_value v{};
        primal::input::get(primal::input::input_source::keyboard, c, v);
        return v.current.x > 0.5f;
    };
    constexpr ic::code kBracketOpen  = ic::key_bracket_open;
    constexpr ic::code kBracketClose = ic::key_brack_close;
    constexpr ic::code kComma   = ic::key_comma;
    constexpr ic::code kPeriod  = ic::key_period;
    constexpr ic::code kMinus   = ic::key_minus;
    constexpr ic::code kPlus    = ic::key_plus;
    constexpr ic::code kR       = ic::key_r;
    constexpr ic::code kT       = ic::key_t;
    constexpr ic::code kO       = ic::key_o;
    constexpr ic::code kP       = ic::key_p;
    constexpr ic::code kM       = ic::key_m;
#endif

    bool changed      = false;
    bool mode_changed = false;   // mode switch needs InitWFC, not ReseedSolver
    const char* why   = "manual";

    const u32 kSideMin = 4, kSideMax = 64;
    const u32 kLayerMin = 1, kLayerMax = 32;

    // Drain pending UI panel actions first (WASM JS → C ABI). Each latched
    // value is clamped to the same range as the keyboard hotkeys so both
    // paths stay consistent. has_pending_* collapses repeated writes into
    // a single reseed.
    if (has_pending_observer_) {
        has_pending_observer_ = false;
        const u32 k = pending_observer_ % 2u;  // 0=MinEntropy, 1=Distance
        auto new_kind = static_cast<ObserverKind>(k);
        if (new_kind != observer_kind_) {
            observer_kind_ = new_kind;
            changed = true; why = "panel:observer";
        }
    }
    if (has_pending_origin_) {
        has_pending_origin_ = false;
        const u32 p = pending_origin_ % 3u;  // 0=Center, 1=Corner, 2=BottomCenter
        auto new_origin = static_cast<OriginPreset>(p);
        if (new_origin != origin_preset_) {
            origin_preset_ = new_origin;
            changed = true; why = "panel:origin";
        }
    }
    if (has_pending_solve_mode_) {
        has_pending_solve_mode_ = false;
        const u32 m = pending_solve_mode_ % 2u;  // 0=KenneyOnly, 1=MixedMulti
        auto new_mode = static_cast<SolveMode>(m);
        if (new_mode != solve_mode_) {
            solve_mode_ = new_mode;
            changed = true; mode_changed = true;
            degraded_to_single_set_ = false;
            why = "panel:solve-mode";
        }
    }
    if (has_pending_grid_w_) {
        has_pending_grid_w_ = false;
        const u32 new_w = std::clamp(pending_grid_w_, kSideMin, kSideMax);
        if (new_w != grid_w_) { grid_w_ = new_w; changed = true; why = "panel:grid-w"; }
    }
    if (has_pending_grid_h_) {
        has_pending_grid_h_ = false;
        const u32 new_h = std::clamp(pending_grid_h_, kLayerMin, kLayerMax);
        if (new_h != grid_h_) { grid_h_ = new_h; changed = true; why = "panel:grid-h"; }
    }
    if (has_pending_grid_d_) {
        has_pending_grid_d_ = false;
        const u32 new_d = std::clamp(pending_grid_d_, kSideMin, kSideMax);
        if (new_d != grid_d_) { grid_d_ = new_d; changed = true; why = "panel:grid-d"; }
    }
    if (pending_reseed_same_) {
        pending_reseed_same_ = false;
        changed = true; why = "panel:reseed-same";
    }
    if (pending_reseed_new_) {
        pending_reseed_new_ = false;
        const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        rng_seed_ = static_cast<u32>(ticks & 0xFFFFFFFFu);
        changed = true; why = "panel:reseed-new";
    }

    if (just_pressed_(kBracketOpen, key_now(kBracketOpen))) {
        if (grid_w_ > kSideMin) { --grid_w_; changed = true; why = "X--"; }
    }
    if (just_pressed_(kBracketClose, key_now(kBracketClose))) {
        if (grid_w_ < kSideMax) { ++grid_w_; changed = true; why = "X++"; }
    }
    if (just_pressed_(kComma, key_now(kComma))) {
        if (grid_d_ > kSideMin) { --grid_d_; changed = true; why = "Z--"; }
    }
    if (just_pressed_(kPeriod, key_now(kPeriod))) {
        if (grid_d_ < kSideMax) { ++grid_d_; changed = true; why = "Z++"; }
    }
    if (just_pressed_(kMinus, key_now(kMinus))) {
        if (grid_h_ > kLayerMin) { --grid_h_; changed = true; why = "Y--"; }
    }
    if (just_pressed_(kPlus, key_now(kPlus))) {
        if (grid_h_ < kLayerMax) { ++grid_h_; changed = true; why = "Y++"; }
    }
    if (just_pressed_(kR, key_now(kR))) {
        changed = true; why = "reseed-same";
    }
    if (just_pressed_(kT, key_now(kT))) {
        // Reroll seed from wall clock. std::random_device may be deterministic
        // on some platforms; mix in steady_clock for entropy.
        const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        rng_seed_ = static_cast<u32>(ticks & 0xFFFFFFFFu);
        changed = true; why = "reseed-new";
    }
    if (just_pressed_(kO, key_now(kO))) {
        observer_kind_ = (observer_kind_ == ObserverKind::MinEntropy)
                             ? ObserverKind::DistanceFromOrigin
                             : ObserverKind::MinEntropy;
        changed = true; why = "observer cycled";
    }
    if (just_pressed_(kP, key_now(kP))) {
        origin_preset_ = static_cast<OriginPreset>(
            (static_cast<u32>(origin_preset_) + 1) % 3);
        changed = true; why = "origin preset cycled";
    }
    if (just_pressed_(kM, key_now(kM))) {
        // Phase C.1 Task 14: toggle MixedMulti ↔ KenneyOnly. Mode switch
        // requires rebuilding registry_+adjacency_ (not just ReseedSolver)
        // because the two modes use different tile sets + socket encodings.
        solve_mode_ = (solve_mode_ == SolveMode::KenneyOnly)
                          ? SolveMode::MixedMulti
                          : SolveMode::KenneyOnly;
        degraded_to_single_set_ = false;
        changed = true; mode_changed = true;
        why = "solve mode cycled";
    }

    if (!changed) return;

    // Sync HUD before reseed so the WFC iframe shows the new observer/origin
    // and grid dims as the solve restarts. No-op on native builds.
    {
        const char* obs = (observer_kind_ == ObserverKind::MinEntropy)
                          ? "MinEntropy" : "DistanceFromOrigin";
        const char* origin = nullptr;
        switch (origin_preset_) {
            case OriginPreset::Center:       origin = "Center";       break;
            case OriginPreset::Corner:       origin = "Corner";       break;
            case OriginPreset::BottomCenter: origin = "BottomCenter"; break;
        }
        SyncWfcHudIfWasm(obs, origin, grid_w_, grid_h_, grid_d_, rng_seed_);
    }

    if (mode_changed) {
        // Mode switch: rebuild registry_+adjacency_ from scratch, then
        // reseed the solver against the new tile set.
        registry_  = std::make_unique<primal::graphics::wfc::WFCTileRegistry>();
        adjacency_ = std::make_unique<primal::graphics::wfc::TileAdjacencyTable>();
        InitWFC();
    } else {
        ReseedSolver();
    }
    PrintGridState(why);
}

// ============================================================================
// PrintGridState
// ============================================================================

void KenneyTilePreviewTestCase::PrintGridState(const char* why) {
    using namespace primal::graphics::wfc;
    std::cout << "[TestKenneyTilePreview] " << why
              << " → grid=" << grid_w_ << "x" << grid_h_ << "x" << grid_d_
              << " seed=" << rng_seed_
              << " observer="
              << (observer_kind_ == ObserverKind::MinEntropy
                      ? "MinEntropy"
                      : "DistanceFromOrigin");
    if (observer_kind_ == ObserverKind::DistanceFromOrigin) {
        WFCGridCoord o = ComputeOrigin();
        std::cout << " origin=(" << o.x << "," << o.y << "," << o.z << ") preset=";
        switch (origin_preset_) {
            case OriginPreset::Center:       std::cout << "Center"; break;
            case OriginPreset::Corner:       std::cout << "Corner"; break;
            case OriginPreset::BottomCenter: std::cout << "BottomCenter"; break;
        }
    }
    std::cout << std::endl;
}

// ============================================================================
// DestroyAllSpawnedEntities
// ============================================================================

void KenneyTilePreviewTestCase::DestroyAllSpawnedEntities() {
    using namespace primal::graphics::pcg;
    // Drain RenderScene proxies BEFORE clearing the list —
    // SyncEntitiesToRenderScene only RemoveProxy's for entities that are
    // still in pcg_entity_ids_. If we let ClearPCGEntities() empty the list
    // first, the stale proxies stay in RenderScene::proxies_ and keep rendering.
    if (scene) {
        for (auto eid : wfc_entity_ids_) scene->RemoveProxy(eid);
    }
    if (!wfc_entity_ids_.empty()) {
        PCGEntityFactory::DestroyEntities(wfc_entity_ids_);
    }
    wfc_entity_ids_.clear();
    wfc_mesh_slots_.clear();
    if (pipeline) pipeline->ClearPCGEntities();
}

// ============================================================================
// PumpSolverFrame
// ============================================================================
//
// One streaming step per frame. Mirrors TestWFCRuinsRendering::PumpSolverFrame:
//   1. Reset budget + Step the solver once.
//   2. DrainStream() yields only post-last-Restart Collapse points + a flag
//      for whether a restart happened this frame.
//   3. On restart: destroy prior entities before appending survivors.
//   4. Spawn entities for new Collapse points (if any). Append to cumulative
//      vectors and re-publish via SetPCGEntities.
//   5. Detect completion (Done / GivenUp) + log per-tile distribution.

void KenneyTilePreviewTestCase::PumpSolverFrame() {
    using namespace primal::graphics::wfc;
    using namespace primal::graphics::pcg;

    if (solver_done_ || !solver_ || !budget_) return;

    budget_->Reset();
    solver_state_ = solver_->Step(*budget_);

    WFCStreamDrainResult drain = WFCOutput::DrainStream(
        *buf_, *registry_, /*cell_size=*/4.0f);

    if (drain.restart_seen) {
        total_restarts_ += drain.restart_count;
        DestroyAllSpawnedEntities();
        std::cout << "[TestKenneyTilePreview] restart #" << total_restarts_
                  << " — replaying " << drain.new_points.count
                  << " new collapses" << std::endl;
    }

    if (drain.new_points.count > 0) {
        total_collapses_ += drain.new_points.count;
        auto spawn = PCGEntityFactory::CreateEntities(drain.new_points);
        wfc_entity_ids_.insert(wfc_entity_ids_.end(),
                               spawn.entity_ids.begin(), spawn.entity_ids.end());
        wfc_mesh_slots_.insert(wfc_mesh_slots_.end(),
                               spawn.mesh_slot_indices.begin(),
                               spawn.mesh_slot_indices.end());
        assert(wfc_entity_ids_.size() == wfc_mesh_slots_.size());
        pipeline->SetPCGEntities(wfc_entity_ids_, wfc_mesh_slots_);
    }

    if (solver_state_ == WFCSolver::StepResult::Done ||
        solver_state_ == WFCSolver::StepResult::GivenUp) {
        solver_done_ = true;
        const auto& cells = grid_->Cells();
        const u32 tile_count = registry_->Count();
        std::vector<u32> per_tile(tile_count, 0);
        for (u32 i = 0; i < cells.size(); ++i) {
            if (!cells[i].collapsed) continue;
            const u32 tid = static_cast<u32>(cells[i].collapsed_tile);
            if (tid < tile_count) ++per_tile[tid];
        }
        std::cout << "[TestKenneyTilePreview] solver done: collapses="
                  << total_collapses_ << " restarts=" << total_restarts_
                  << " state=" << static_cast<u32>(solver_state_) << std::endl;
        std::cout << "[TestKenneyTilePreview] tile distribution:" << std::endl;
        for (u32 t = 0; t < tile_count; ++t) {
            if (per_tile[t] == 0) continue;
            const WFCTile& tile = registry_->Get(wfc_tile_id{t});
            std::cout << "  [" << t << "] " << (tile.name ? tile.name : "?")
                      << " (vc=" << tile.variant_count << "): "
                      << per_tile[t] << std::endl;
        }

        // Phase C.1 Task 15: degrade-to-single-set fallback. Only fires
        // on GivenUp in MixedMulti mode (KenneyOnly has no fallback path —
        // if it gives up, the user just reseeds manually). After degrade,
        // the solver re-initializes with a Ruins-only mask and PumpSolverFrame
        // returns early; the next frame picks up the new solve.
        if (solver_state_ == WFCSolver::StepResult::GivenUp &&
            solve_mode_ == SolveMode::MixedMulti &&
            !degraded_to_single_set_) {
            DegradeToSingleSet();
            return;
        }

        CheckSmokeAsserts();
    }
}

// ============================================================================
// CheckSmokeAsserts (Phase C.1 Task 15)
// ============================================================================
//
// Logs PASS/FAIL against the kMin* criteria. Never crashes — the GUI binary
// keeps the window open on failure so the operator can inspect the grid.
// In MixedMulti mode the smoke also enforces ≥ kMinDistinctCategories so
// the silent failure mode where BuildFromClassifier prunes one category to
// zero doesn't pass.

void KenneyTilePreviewTestCase::CheckSmokeAsserts() {
    using namespace primal::graphics::wfc;
    if (!grid_ || !registry_) return;

    const auto& cells = grid_->Cells();
    const u32 tile_count = registry_->Count();

    u32 collapsed_count = 0;
    std::set<u32> distinct_tile_ids;
    std::set<WFCCategory> distinct_categories;
    for (u32 i = 0; i < cells.size(); ++i) {
        if (!cells[i].collapsed) continue;
        ++collapsed_count;
        const u32 tid = static_cast<u32>(cells[i].collapsed_tile);
        if (tid < tile_count) {
            distinct_tile_ids.insert(tid);
            distinct_categories.insert(registry_->Get(wfc_tile_id{tid}).category);
        }
    }

    bool pass = true;
    if (collapsed_count < kMinCellsCollapsed) {
        std::cerr << "[TestKenneyTilePreview] SMOKE FAIL: only "
                  << collapsed_count << " cells collapsed (need >="
                  << kMinCellsCollapsed << ")" << std::endl;
        pass = false;
    }
    if (distinct_tile_ids.size() < kMinDistinctTiles) {
        std::cerr << "[TestKenneyTilePreview] SMOKE FAIL: only "
                  << distinct_tile_ids.size() << " distinct tile ids (need >="
                  << kMinDistinctTiles << ")" << std::endl;
        pass = false;
    }
    if (solve_mode_ == SolveMode::MixedMulti &&
        distinct_categories.size() < kMinDistinctCategories) {
        std::cerr << "[TestKenneyTilePreview] SMOKE FAIL (MixedMulti): only "
                  << distinct_categories.size() << " categories (need >="
                  << kMinDistinctCategories
                  << ") — one source was likely pruned by the classifier"
                  << std::endl;
        pass = false;
    }

    if (pass) {
        std::cout << "[TestKenneyTilePreview] SMOKE PASS: "
                  << collapsed_count << " cells, "
                  << distinct_tile_ids.size() << " tiles, "
                  << distinct_categories.size() << " categories"
                  << (degraded_to_single_set_ ? " (after degrade)" : "")
                  << std::endl;
    }
}

// ============================================================================
// DegradeToSingleSet (Phase C.1 Task 15)
// ============================================================================
//
// Last-resort fallback when the mixed solver exhausts max_generations
// without converging. Narrows active_category_mask to Ruins only and
// reseeds — the simpler Ruins-only solve is much more likely to converge
// since every tile shares solid-cube face signatures (classifier returns
// all-ones for every face of a weathered cube). The degraded flag is
// sticky until the next ReseedSolver triggered by a non-degrade path
// (reseed / mode switch / panel edit), so CheckSmokeAsserts can report
// "after degrade" status.

void KenneyTilePreviewTestCase::DegradeToSingleSet() {
    using namespace primal::graphics::wfc;
    std::cout << "[TestKenneyTilePreview] degrading to Ruins-only after "
              << total_restarts_ << " restarts (mixed solve gave up)"
              << std::endl;
    degraded_to_single_set_ = true;
    // Bump the seed so the degraded solve doesn't repeat the same deadlock.
    ++rng_seed_;
    ReseedSolver();
}

// ============================================================================
// UpdateCamera
// ============================================================================
//
// Yaw/pitch convention matches TestForwardRenderer.cpp:
//   yaw=0   → looking toward -Z
//   yaw=90  → looking toward +X
//   pitch>0 → looking up
// Forward = (sin(yaw)*cos(pitch), sin(pitch), -cos(yaw)*cos(pitch))
//
// For a 16×1×16 grid at cell_size=4m the world center is (32, 0, 32). The
// default camera at (20, 22, 28) looks down-and-toward origin, but the grid
// extends to (64, 0, 64) so the user can fly around to inspect the whole
// dungeon.

void KenneyTilePreviewTestCase::UpdateCamera() {
    const float cp = std::cos(camera_pitch_);
    v3 forward{
        std::sin(camera_yaw_) * cp,
        std::sin(camera_pitch_),
        -std::cos(camera_yaw_) * cp,
    };
    v3 up{0, 1, 0};
    v3 target = camera_pos_ + forward;
    m4x4 viewMat = math::CreateLookAtMatrix(camera_pos_, target, up);
    constexpr float fov = 60.0f * (pi / 180.0f);
    const float aspect = static_cast<float>(window_width_) /
                         static_cast<float>(window_height_);
    m4x4 projMat = math::CreatePerspectiveMatrix(fov, aspect, 0.1f, 1000.0f);
    if (view) {
        view->SetViewMatrix(viewMat);
        view->SetProjectionMatrix(projMat);
    }
}

// ============================================================================
// UpdateCameraFromInput
// ============================================================================
//
// Per-frame FPS-style input poll. WASD moves on ground plane (independent of
// pitch), Q/E descend/ascend, arrows look around. Shift sprint. dt from wall
// clock so motion speed is frame-rate independent.

void KenneyTilePreviewTestCase::UpdateCameraFromInput() {
    namespace chr = std::chrono;
    static chr::steady_clock::time_point last_time = chr::steady_clock::now();
    const auto now = chr::steady_clock::now();
    const float dt = chr::duration<float>(now - last_time).count();
    last_time = now;

#ifdef __EMSCRIPTEN__
    // On WASM we bypass primal::input — that path isn't wired on WASM and the
    // basic renderer demo (TestDawnForwardRenderer) uses raw keyCode lookups
    // directly. Emscripten keyCodes: A=65 W=87 S=83 D=68 Q=81 E=69 Shift=16
    // Arrows: left=37 up=38 right=39 down=40.
    auto key_down = [](int emscripten_key) {
        return EmscriptenGetKeyState(emscripten_key);
    };
    constexpr int kW = 87, kA = 65, kS = 83, kD = 68, kQ = 81, kE = 69;
    constexpr int kShift = 16;
    constexpr int kLeft = 37, kUp = 38, kRight = 39, kDown = 40;
#else
    auto key_down = [](primal::input::input_code::code code) {
        primal::input::input_value v{};
        primal::input::get(primal::input::input_source::keyboard, code, v);
        return v.current.x > 0.5f;
    };
    using ic = primal::input::input_code;
    auto kW = ic::key_w, kA = ic::key_a, kS = ic::key_s, kD = ic::key_d;
    auto kQ = ic::key_q, kE = ic::key_e, kShift = ic::key_shift;
    auto kLeft = ic::key_left, kUp = ic::key_up, kRight = ic::key_right, kDown = ic::key_down;
#endif

    float speed = camera_speed_;
    if (key_down(kShift)) speed *= 3.0f;

    // Ground-plane forward (yaw only) — natural FPS movement.
    // Matches TestForwardRenderer convention: forward=(sy,0,-cy), right=(cy,0,sy).
    const float sy = std::sin(camera_yaw_);
    const float cy = std::cos(camera_yaw_);
    v3 fwd_ground{sy, 0.0f, -cy};
    v3 right_ground{cy, 0.0f, sy};

    v3 move{0.0f, 0.0f, 0.0f};
    if (key_down(kW)) move = move + fwd_ground;
    if (key_down(kS)) move = move - fwd_ground;
    if (key_down(kD)) move = move + right_ground;
    if (key_down(kA)) move = move - right_ground;
    if (key_down(kE)) move.y += 1.0f;
    if (key_down(kQ)) move.y -= 1.0f;

    const float len = std::sqrt(move.x * move.x + move.y * move.y + move.z * move.z);
    if (len > 0.001f) {
        move.x /= len; move.y /= len; move.z /= len;
        camera_pos_ = camera_pos_ + move * (speed * dt);
    }

    // Look — arrow keys adjust yaw/pitch.
    constexpr float kLookSensitivity = 1.5f;  // radians per second
    if (key_down(kLeft))  camera_yaw_   += kLookSensitivity * dt;
    if (key_down(kRight)) camera_yaw_   -= kLookSensitivity * dt;
    if (key_down(kUp))    camera_pitch_ += kLookSensitivity * dt;
    if (key_down(kDown))  camera_pitch_ -= kLookSensitivity * dt;

#ifdef __EMSCRIPTEN__
    // Mouse drag — left button held rotates camera. Matches TestDawnForwardRenderer
    // convention: yaw -= dx, pitch -= dy, sensitivity 0.002 rad/px.
    float mdx = 0.0f, mdy = 0.0f;
    EmscriptenGetMouseDelta(&mdx, &mdy);
    if (EmscriptenGetMouseButton(0)) {
        constexpr float kMouseSensitivity = 0.002f;
        camera_yaw_   -= mdx * kMouseSensitivity;
        camera_pitch_ -= mdy * kMouseSensitivity;
    }
#endif

    // Clamp pitch to ±~89° to avoid flip at the poles.
    constexpr float kPitchLimit = 1.55334f;  // ~89°
    if (camera_pitch_ >  kPitchLimit) camera_pitch_ =  kPitchLimit;
    if (camera_pitch_ < -kPitchLimit) camera_pitch_ = -kPitchLimit;
}

// ============================================================================
// PrintControls
// ============================================================================

void KenneyTilePreviewTestCase::PrintControls() {
    std::cout << "\n[TestKenneyTilePreview] controls:"
              << "\n  W/A/S/D    move (ground plane)"
              << "\n  Q / E      descend / ascend"
              << "\n  Arrows     look (yaw / pitch)"
              << "\n  Shift      sprint (3× speed)"
              << "\n  [ / ]      grid_w (X) -- / ++"
              << "\n  , / .      grid_d (Z) -- / ++"
              << "\n  - / +      grid_h (Y, layers) -- / ++"
              << "\n  R          regenerate same seed"
              << "\n  T          regenerate new seed"
              << "\n  O          cycle observer strategy (MinEntropy <-> DistanceFromOrigin)"
              << "\n  P          cycle origin preset (Center -> Corner -> BottomCenter)"
              << "\n  Close window or Ctrl-C to quit"
              << std::endl;
}

// ============================================================================
// Run
// ============================================================================

void KenneyTilePreviewTestCase::Run() {
    if (!pipeline || !scene || !view) return;

    // One-shot smoke check on the first frame: catalog + registry must have
    // produced ≥ 1 entity. We assert once and then keep rendering so the
    // human can eyeball orientation / textures / scale.
    if (frame_count_ == 0) {
        if (!registry_ || registry_->Count() == 0) {
            std::cerr << "[TestKenneyTilePreview] SMOKE FAIL: registry empty"
                      << std::endl;
            assert(false && "smoke criterion: registry populated");
        }
    }

    // Edge-triggered grid-edit keys may reseed the solver from scratch
    // before this frame's pump.
    HandleGridEditKeys();

    PumpSolverFrame();

    UpdateCameraFromInput();
    UpdateCamera();

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
}

// ============================================================================
// Shutdown
// ============================================================================

void KenneyTilePreviewTestCase::Shutdown() {
    if (!pipeline && !scene) return;  // idempotent
    std::cout << "[TestKenneyTilePreview] Shutting down..." << std::endl;

    // Clear singleton before destroying subsystems — JS panel may still hold
    // a pointer if the user closes the tab mid-frame.
    g_instance_ = nullptr;

    DestroyAllSpawnedEntities();

    // Drop streaming state before the catalog (solver holds raw pointers
    // into grid_/buf_/registry_/adjacency_).
    solver_.reset();
    buf_.reset();
    grid_.reset();
    registry_.reset();
    adjacency_.reset();
    budget_.reset();

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
