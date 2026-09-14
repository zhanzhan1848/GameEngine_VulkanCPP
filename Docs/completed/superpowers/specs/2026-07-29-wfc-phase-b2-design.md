# WFC Phase B.2: Streaming Generation Pipeline

**Date:** 2026-07-29
**Branch:** `feat/wfc-pcg`
**Predecessors:**
- Spec: `Docs/superpowers/specs/2026-07-29-wfc-phase-b1-design.md` (commit `8170319`)
- Plan: `Docs/superpowers/plans/2026-07-29-wfc-phase-b1.md` (commit `b70af5b`)
- Completion notes: `Docs/superpowers/notes/2026-07-29-wfc-phase-b1-completion.md`

## Goal

Make the WFC solver's progress visible at frame speed. Each Collapse step emits one entity immediately; the viewer watches the world assemble itself tile by tile. On Restart, the viewer sees all prior tiles vanish before generation resumes.

## Motivation

Phase B.1 delivered a 4×4×4 catalog with corner geometry, flag-driven rotation, and a 2D mode toggle. The viewer sees only the final solved state — generation is a black box that takes ~16 ms and pops a complete scene.

Phase B.2 turns generation inside out. The solver keeps producing Collapse/Restart events; the new pipeline drains those events every frame and spawns the corresponding entities live. This exposes solver dynamics (propagation, contradictions, backtracking) that static visualizations hide.

## Non-Goals (deferred)

- **HUD / text overlay** (tile count, restart count, fps) — deferred to Phase B.3. The engine has no first-class text rendering primitive; building one is out of scope for B.2. Mode state is logged to stdout on toggle.
- **Camera lerp animation** on mode toggle — snap remains acceptable.
- **Mesh-level appearance animation** (fade-in, scale-up) per new entity — entities appear at full scale.
- **RenderScene O(n²) `UpdateProxy` optimization** — streaming will make per-frame `UpdateProxy` calls more frequent. B.2 accepts the cost; B.3 may revisit if profiling demands it.
- **Mid-Step budget enforcement** — the solver's `max_cells_per_frame` / `max_ms_per_frame` budget is checked only at `Step()` entry. A single collapse that exceeds the budget still completes. Carried from B.1.
- **Threading `WFC_FACE_COUNT_2D` through the solver** — `WFCSolver` still passes `WFC_FACE_COUNT_3D`. The 2D path works because `grid_size.z=1` filters ±Z via the OOB guard. Carried from B.1.
- **Interactive key testing in CI** — 'M' (mode), 'R' (re-seed), 'Space' (pause) are tested interactively only; no automated hooks.

## Architecture

### Streaming Pipeline (Method B: New Streaming API)

```
┌──────────────┐     ┌──────────────┐     ┌────────────────┐     ┌──────────────────┐     ┌─────────────────────┐
│  WFCSolver   │────▶│ WFCStepBuffer│────▶│ DrainStream    │────▶│ AppendEntities   │────▶│ Pipeline.UpdateProxies│
│ (Collapse +  │     │ (ring buffer │     │ (steps →       │     │ (points → ECS    │     │ (ECS → render scene) │
│  Restart)    │     │  of steps)   │     │  PCGPoints)    │     │  entities)       │     │                     │
└──────────────┘     └──────────────┘     └────────────────┘     └──────────────────┘     └─────────────────────┘
       ▲                                                                  │
       │                                                                  ▼
       └──────────────────── Restart: destroy all entities ◀─────────────
```

The pipeline runs every frame in `TestWFCRendering::Run()`:

1. **Solver pumps N steps** (time-budgeted) into `WFCStepBuffer`.
2. **`DrainStream` consumes the buffer**, producing:
   - A list of new `PCGPoint`s (one per Collapse).
   - A `restart_seen` flag (any Restart step encountered this frame).
3. **On `restart_seen`**: all existing entities are destroyed (ECS + render proxies) before any new points are appended. The viewer sees the world clear, then refill.
4. **`AppendEntities`** spawns ECS entities from the new points.
5. **Pipeline picks them up** via the standard ECS sync path; render proxies appear next frame.

### Why Method B (new streaming API) over Method A (query step buffer inline)

- **Single Responsibility**: `DrainStream` encapsulates step→point translation (tile lookup, variant rotation, cell_size scaling). Inlining this into `TestWFCRendering` duplicates logic that belongs in the WFC layer.
- **Testability**: a pure `DrainStream(step_buffer, registry, cell_size) → points` function is trivial to unit-test without a Metal device. Method A forces integration tests for every step-shape edge case.
- **Reusable for future PCG sources**: once the stream→point→entity pipeline exists, other generators (noise scatters, grammar-based PCG) can plug into the same `AppendEntities` API.
- **Cost**: 3 small new APIs (≈60 lines total). Acceptable for the isolation gained.

## Components

### 1. `WFCStreamDrainResult` + `DrainStream` (new)

**File:** `Engine/Graphics/WFC/WFCOutput.h` (add to existing header), `Engine/Graphics/WFC/WFCOutput.cpp` (add to existing TU).

```cpp
// Engine/Graphics/WFC/WFCOutput.h

struct WFCStreamDrainResult {
    // One PCGPoint per Collapse step drained this frame.
    utl::vector<PCGPoint> new_points;

    // True if any Restart step was encountered. Caller MUST destroy all
    // previously-spawned entities before appending new_points.
    bool restart_seen{false};

    // Number of Restart steps encountered (for diagnostics / stdout log).
    u32  restart_count{0};
};

// Drain all pending steps from the buffer. Each Collapse → one PCGPoint.
// Each Restart → clears new_points (caller still gets restart_seen=true so
// the already-spawned entities are destroyed) and increments restart_count.
//
// cell_size: world-space size of one tile (passed through to PCGPoint::origin).
WFCStreamDrainResult DrainStream(WFCStepBuffer& buf,
                                 const WFCTileRegistry& reg,
                                 f32 cell_size);
```

**Implementation sketch** (in `WFCOutput.cpp`, alongside existing `EmitPointCloud`):

```cpp
WFCStreamDrainResult DrainStream(WFCStepBuffer& buf,
                                 const WFCTileRegistry& reg,
                                 f32 cell_size) {
    WFCStreamDrainResult result;
    std::vector<WFCStep> snapshot;
    buf.Consume(snapshot);  // existing WFCStepBuffer API: drains + clears

    for (const auto& step : snapshot) {
        if (step.kind == WFCStepKind::Collapse) {
            PCGPoint p = MakePoint(step, reg, cell_size);  // existing helper
            result.new_points.push_back(p);
        } else if (step.kind == WFCStepKind::Restart) {
            result.restart_seen = true;
            ++result.restart_count;
            result.new_points.clear();  // discard partials; caller destroys prior spawn
        }
    }
    return result;
}
```

`MakePoint` already exists in `WFCOutput.cpp` (used by `EmitPointCloud`). It maps `(cell_coord, tile_id, variant)` → `PCGPoint{origin, tile_id, variant, rotation_y}`. No new logic needed; `DrainStream` just calls it per-Collapse.

### 2. `PCGEntityFactory::AppendEntities` (new)

**File:** `Engine/Graphics/PCG/PCGEntityFactory.h` (new header), `Engine/Graphics/PCG/PCGEntityFactory.cpp` (new TU).

Rationale for new file: the existing PCG → ECS spawn path is inlined inside `TestWFCRendering::SpawnEntitiesForCurrentMode`. Pulling it out makes it reusable for B.2 streaming and for future PCG sources. The file stays small (single responsibility: points → entities).

```cpp
// Engine/Graphics/PCG/PCGEntityFactory.h

namespace primal::graphics::pcg {

struct AppendResult {
    utl::vector<id::id_type> entity_ids;
    utl::vector<u32>          mesh_slot_indices;  // parallel to entity_ids
    u32                        failed_count{0};    // points with bad tile_id
};

// Spawn one ECS Entity per PCGPoint. Captures slot indices into the per-tile
// mesh_slots map (caller supplies: tile_id → registered mesh slot).
//
// mesh_slots_by_tile_id: tile_id → mesh slot index (e.g. cube=0, ramp=1, ...).
// Length must match the highest tile_id encountered. Missing entries → failed_count++.
AppendResult AppendEntities(utl::span<const PCGPoint> points,
                            utl::span<const u32> mesh_slots_by_tile_id);

} // namespace primal::graphics::pcg
```

**Why a factory function and not a class with state:** stateless transformation. The caller (TestWFCRendering) owns the cumulative `wfc_entity_ids` / `wfc_mesh_slots` vectors. Restart-driven "destroy all" stays a one-liner at the caller side (`for id in wfc_entity_ids: DestroyEntity(id)`).

**Why a parallel `mesh_slot_indices` vector:** matches the existing `wfc_entity_ids` / `wfc_mesh_slots` parallel-array pattern in `TestWFCRendering`. Keeps the render-proxy update path unchanged.

### 3. `StandardRenderPipeline::AppendPCGEntities` (new)

**File:** `Engine/Graphics/RenderPipeline/StandardRenderPipeline.h` (add method), `.cpp` (add implementation).

```cpp
// StandardRenderPipeline.h

class StandardRenderPipeline {
public:
    // ... existing methods ...

    // Append newly-spawned PCG entities to the per-frame ECS sync list.
    // Next UpdateProxies call will create render proxies for these entities.
    // Does not invalidate existing proxies.
    void AppendPCGEntities(utl::vector<id::id_type> entity_ids,
                           utl::vector<u32> mesh_slot_indices);
};
```

**Implementation:** appends to the pipeline's internal `_pending_pcg_entity_ids` / `_pending_pcg_mesh_slots` vectors. These are drained during the next `UpdateProxies` call. If the pipeline does not yet have such an internal list, it's added as a private member.

**Why an explicit `AppendPCGEntities` and not implicit pickup via the ECS sync:** the existing ECS sync path walks *all* entities with RenderProxy components. For streaming, we want per-frame diff delivery (only the new entities) so UpdateProxies can do incremental work, not a full O(N) walk per frame. Explicit append lets the pipeline know "these are new, create proxies" vs "these already exist, just update matrices".

### 4. `WFCSolveBudget` (existing, reused as-is)

No changes. Phase B.1 already has `max_cells_per_frame` and `max_ms_per_frame`. The streaming loop will use both:

```cpp
WFCSolveBudget budget;
budget.max_cells_per_frame = 2;    // ~2 collapses per frame → ~32 seconds for 64 cells at 60 fps
budget.max_ms_per_frame    = 8.0f; // leave 8 ms for rendering on a 60 fps budget
solver_->SetBudget(budget);
```

For 16³ mode (4096 cells), `max_cells_per_frame` is raised to ~16 → ~4 minutes total. Tunable at runtime via the budget struct.

### 5. `TestWFCRendering` new members

```cpp
// TestWFCRendering.h (private members)

// Phase B.2: persistent solver state across frames
std::unique_ptr<primal::graphics::wfc::WFCSolver>     solver_;
std::unique_ptr<primal::graphics::wfc::WaveGrid>      grid_;
std::unique_ptr<primal::graphics::wfc::WFCStepBuffer> buf_;
WFCSolver::StepResult solver_state_{WFCSolver::StepResult::InProgress};
bool solver_done_{false};

// Phase B.2: telemetry for stdout logging
u32 total_collapses_{0};
u32 total_restarts_{0};
```

Existing members `wfc_entity_ids` and `wfc_mesh_slots` stay — they're now the cumulative entity list across frames.

## Data Flow

### Frame loop (TestWFCRendering::Run, Phase B.2 streaming)

```
for each frame until window-close or headless_frame_cap:
    1. Input:
       - if 'M' key rising-edge: CycleMode()  // Phase B.1, unchanged
       - if 'R' key rising-edge: ReseedSolver()  // new: re-seed + clear entities
       - if 'Space' key rising-edge: paused_ = !paused_

    2. If not paused_ and not solver_done_:
       a. solver_->Step(*grid_, *buf_, budget)  → solver_state_
       b. auto drain = wfc::DrainStream(*buf_, *registry_, CellSizeForCurrentMode())  // existing helper
       c. total_collapses_ += drain.new_points.size()
       d. if drain.restart_seen:
          - total_restarts_ += drain.restart_count
          - DestroyAllSpawnedEntities()   // uses wfc_entity_ids
          - wfc_entity_ids.clear()
          - wfc_mesh_slots.clear()
          - log: "[WFC] restart #%u — cleared %u entities, replaying %u new"
       e. if !drain.new_points.empty():
          - auto append = pcg::AppendEntities(drain.new_points, mesh_slots_by_tile_id_)  // member: u32[tile_id] → slot
          - wfc_entity_ids.insert(end, append.entity_ids)
          - wfc_mesh_slots.insert(end, append.mesh_slot_indices)
          - pipeline->AppendPCGEntities(move(append.entity_ids), move(append.mesh_slot_indices))
       f. if solver_state_ == Complete: solver_done_ = true; log "[WFC] complete"

    3. UpdateCamera()  // Phase B.1
    4. pipeline->UpdateProxies(scene, view)  // picks up appends
    5. pipeline->Render(scene, view)
    6. frame_count_++
```

### ReseedSolver (new helper, in TestWFCRendering.cpp)

```cpp
void WFCRenderingTestCase::ReseedSolver() {
    DestroyAllSpawnedEntities();
    wfc_entity_ids.clear();
    wfc_mesh_slots.clear();

    grid_   = std::make_unique<WaveGrid>(grid_size_for_mode());
    buf_    = std::make_unique<WFCStepBuffer>(/* capacity */ 4096);
    solver_ = std::make_unique<WFCSolver>(*grid_, *registry_, *adjacency_,
                                          rng_seed_++);  // increment seed
    solver_->SetBudget(current_budget());
    solver_state_  = WFCSolver::StepResult::InProgress;
    solver_done_   = false;
    total_collapses_ = 0;
    total_restarts_  = 0;
}
```

`rng_seed_` is a new member (`u32 rng_seed_{7}` — same as Phase B.1's fixed seed, but now mutable). Each 'R' press uses the next seed so the viewer sees a different solve.

### CycleMode (modified)

Phase B.1's `CycleMode` rebuilds the catalog. Phase B.2 changes it to:
1. DestroyAllSpawnedEntities()
2. flip `mode_`
3. ReseedSolver()  // uses new grid_size_for_mode()
4. SnapCameraForCurrentMode()

The catalog itself (`registry_`, `adjacency_`, `mesh_slots_by_tile_id_`) is mode-independent — built once in `SetupWFCCatalog()` and never rebuilt.

### DestroyAllSpawnedEntities (new helper)

```cpp
void WFCRenderingTestCase::DestroyAllSpawnedEntities() {
    for (auto id : wfc_entity_ids) {
        pipeline->DestroyPCGEntity(id);  // new pipeline method (see below)
    }
}
```

If `StandardRenderPipeline::DestroyPCGEntity` does not yet exist, it's added alongside `AppendPCGEntities` — marks the entity's render proxy for removal on next `UpdateProxies`, then removes the ECS Entity.

### Restart semantics

- A Restart step in the buffer means the solver hit a contradiction and started over.
- The viewer sees: all spawned tiles disappear (one-frame clear), then generation resumes from scratch.
- If multiple Restart steps happen in one frame (rare but possible), they're folded: `restart_count` is incremented per step, but the clear + replay happens once (since `new_points.clear()` on each Restart leaves only the post-last-Restart points).

## Error Handling

### 1. Bad tile_id in PCGPoint

`AppendEntities` looks up `mesh_slots_by_tile_id[tile_id]`. If `tile_id >= mesh_slots_by_tile_id.size()`:

- Increment `failed_count`.
- Skip the point (no entity spawned).
- Do NOT abort the whole batch.

Rationale: a single bad tile shouldn't tank the entire generation. The viewer sees a gap; diagnostics are in `failed_count`.

### 2. `AppendPCGEntities` called with mismatched vector lengths

This is a programmer error. Assert in Debug builds (`assert(entity_ids.size() == mesh_slot_indices.size())`). In Release, the shorter length wins (truncate the longer).

### 3. `DrainStream` on empty buffer

Returns `result.new_points.empty()`, `restart_seen=false`, `restart_count=0`. Caller's "if not empty" guard handles it.

### 4. Solver returns Error (WFCSolver::StepResult::Error)

Treat as solver_done_. Log `[WFC] solver error — generation halted`. The viewer sees the partial state; no entities are destroyed.

### 5. `solver_->Step` throws (shouldn't, but defensive)

Wrap the per-frame solver step in a try/catch. On exception: log, set `solver_done_ = true`, continue rendering. Never crash the viewer.

### 6. Restart with non-empty `new_points` from a previous Collapse in the same frame

Already handled: the `for` loop in `DrainStream` processes steps in order. If a Collapse precedes a Restart in the same snapshot, the point is added then cleared by the subsequent Restart. Correct behavior — the viewer never sees the aborted Collapse's tile.

### 7. Headless frame cap

Phase B.1's `kHeadlessFrameCap = 60` stays. If the solver isn't done in 60 frames (~1 second at 60 fps), the test exits anyway with the partial scene rendered. CI smoke test (TestWFCRendering2DSmoke) passes as long as no crash + exit 0.

### 8. Render proxy lifetime

If `DestroyPCGEntity` is called on an entity whose proxy is mid-render (GPU still referencing it), the pipeline must defer the actual GPU resource free to the next safe frame (standard deferred-destruction pattern). This is the existing pattern for entity destruction in the engine; `DestroyPCGEntity` reuses it.

## Testing

### New unit-test binaries

#### `TestWFCStreamDrain` (new binary, 5 cases)

**Files:**
- Create: `EngineTest/UnitTests/Graphics/WFC/TestWFCStreamDrain.cpp`
- Modify: `EngineTest/UnitTests/CMakeLists.txt` (add new target)

| Case | Validates |
|---|---|
| `DrainStream_EmptyBuffer_ReturnsEmpty` | No steps → empty new_points, restart_seen=false |
| `DrainStream_SingleCollapse_ProducesOnePoint` | 1 Collapse step → 1 PCGPoint with correct origin/tile_id/rotation |
| `DrainStream_MultipleCollapses_ProducesAllPoints` | N Collapse steps → N points, in order |
| `DrainStream_RestartClearsNewPoints_AndSetsFlag` | Collapse + Restart in same snapshot → new_points empty, restart_seen=true, restart_count=1 |
| `DrainStream_MultipleRestarts_IncrementsCount` | 2 Restart steps → restart_count=2, restart_seen=true |

The test uses a real `WFCStepBuffer` and populates it via `buf.Push(Collapse{...})` / `buf.Push(Restart{...})`. No Metal device needed — pure logic test. Registry is the standard catalog built by `WFCTileCatalog::Populate`.

#### `TestPCGEntityFactory` (new binary, 3 cases)

**Files:**
- Create: `EngineTest/UnitTests/Graphics/PCG/TestPCGEntityFactory.cpp`
- Modify: `EngineTest/UnitTests/CMakeLists.txt` (add new target)

| Case | Validates |
|---|---|
| `AppendEntities_EmptyInput_ReturnsEmpty` | 0 points → 0 entities |
| `AppendEntities_ValidPoints_SpawnsAll` | 3 points → 3 entities, correct slot indices |
| `AppendEntities_BadTileId_IncrementsFailedCount` | point with tile_id=99 → failed_count=1, others succeed |

Note: spawning real ECS entities requires an initialized `EntityComponentSystem`. These tests will use the existing test harness pattern (`CreateEntityComponentSystem` / `DestroyEntityComponentSystem`) used by other ECS unit tests.

### New integration test binary

#### `TestWFCStreaming` (new binary, 3 cases)

**Files:**
- Create: `EngineTest/IntegrationTests/Graphics/WFC/TestWFCStreaming.cpp`
- Modify: `EngineTest/IntegrationTests/CMakeLists.txt` (add new target)

| Case | Validates |
|---|---|
| `Streaming_CompletesWithinBudget_64Cells` | 4×4×4 grid, solver completes in <120 frames, total_collapses==64 |
| `Streaming_RestartDestroysAllEntities` | Force a contradiction (seeded) → Restart fires → entities cleared, replay begins |
| `Streaming_4096Cells_CompletesWithoutCrash` | 16×16×16 grid, completes within 600 frames, no GPU crash |

This binary is a sibling of `TestWFCRendering` — shares the device/pipeline setup, but the Run() loop asserts streaming-specific invariants instead of just rendering frames.

Decision: **new binary** (not merged into `TestWFCRendering`). Rationale: streaming invariants (collapse count, restart semantics) need assertion-heavy tests distinct from the visual smoke test. Keeping them separate avoids bloating `TestWFCRendering` with non-visual checks.

### Modified existing binaries

#### `TestWFCRendering` (modified)

- `Initialize()`: builds catalog via `SetupWFCCatalog()`, then calls `ReseedSolver()` to set up the streaming state.
- `Run()`: per-frame streaming loop (see Data Flow).
- `CycleMode()`: destroy entities → flip mode → ReseedSolver → snap camera.
- `Shutdown()`: destroy entities, release solver/grid/buf_.
- New 'R' key handler: `ReseedSolver()`.
- New 'Space' key handler: `paused_ = !paused_`.

Phase B.1's `WFC_MODE_2D_SMOKE=1` CI mode still works — `Initialize()` calls `ReseedSolver()` which uses `grid_size_for_mode()` (returns `{4,4,1}` in 2D smoke mode).

#### `TestWFCRendering2DSmoke` (unchanged CMake target)

Same source compiled with `WFC_MODE_2D_SMOKE=1`. Still renders 60 frames and exits 0. The streaming path runs in 2D mode and exits before solver completion — acceptable for smoke.

### Test totals (predicted, Phase B.2 end-state)

| Category | Binary | Cases |
|---|---|---|
| Unit (existing) | (all 16 Phase B.1 binaries) | 74 |
| Unit (new) | TestWFCStreamDrain | +5 |
| Unit (new) | TestPCGEntityFactory | +3 |
| Integration (existing) | TestWFC3DParametric | 1 |
| Integration (existing) | TestWFCRendering | 1 (modified) |
| Integration (existing) | TestWFCRendering2DSmoke | 1 (unchanged) |
| Integration (new) | TestWFCStreaming | +3 |

**Unit total: 74 → 82 (8 new).**
**Integration total: 3 cases → 6 cases (across 3 → 4 binaries).**
**Grand total: 77 cases → 88 cases.**

### CI grep patterns (verification)

After Phase B.2 lands, CI logs should contain these strings on a successful run:

- `[WFC] complete` — solver finished within headless frame cap (TestWFCRendering, non-CI run)
- `[WFC] restart #` — at least one restart was seen and handled (if solver contradiction occurs)
- `TestWFCStreamDrain: 5/5 passed`
- `TestPCGEntityFactory: 3/3 passed`
- `TestWFCStreaming: 3/3 passed`

CI failure indicators (must NOT appear):
- `assert.*entity_ids.size.*mesh_slot_indices.size` — AppendPCGEntities length mismatch
- `[WFC] solver error` — solver returned Error state
- `FreeList assert` — entity lifecycle bug (would indicate DestroyPCGEntity misuse)

## Performance Budget

- 4×4×4 = 64 cells. At `max_cells_per_frame=2`, completes in 32 frames (~0.5 seconds at 60 fps). Visible streaming.
- 16×16×16 = 4096 cells. At `max_cells_per_frame=16`, completes in 256 frames (~4 seconds at 60 fps). Acceptable for a "let it run" mode.
- The streaming Run() loop adds <0.5 ms per frame (AppendEntities is O(new_points), typically <32/frame). No measurable fps impact.

## Risks

1. **`StandardRenderPipeline::AppendPCGEntities` API surface** — adding a public method to the pipeline means a non-trivial engine change. If the pipeline's ECS sync path is tightly coupled to the existing "full sync" path, this could cascade. Mitigation: the plan's Task 2 first inspects the existing sync path; if integration is invasive, fall back to "just call `UpdateProxies` per-frame and let the existing O(N) walk pick up new entities". The streaming still works — just less efficient.

2. **`DestroyPCGEntity` correctness** — destroying an entity mid-frame could race with GPU rendering. Mitigation: use the engine's existing deferred-destruction queue (entities destroyed this frame are removed before next frame's render). This is the standard pattern.

3. **`DrainStream`'s `buf.Consume` semantics** — need to verify `WFCStepBuffer::Consume` actually drains (empties the buffer) vs. just peeks. If it only peeks, the same steps will be re-drained next frame → duplicate entities. Mitigation: Task 1 in the plan verifies `Consume` drains; if not, a new `WFCStepBuffer::ConsumeDrain` method is added.

4. **Restart visual clear might be too fast** — if Restart + new Collapses happen in the same frame, the viewer never sees the cleared state. Mitigation: acceptable — the visual goal is "see generation in progress", and a Restart is a generation event, not a viewing event. If the viewer wants to see the clear explicitly, they can press 'Space' to pause.

5. **`MakePoint` may not be reusable as-is** — the existing helper might have side effects or assume single-batch semantics. Mitigation: Task 1's first step is to read `MakePoint` and confirm it's a pure function; if not, refactor before reuse.

## Dependencies

- Phase B.1 completion (✓ merged).
- `WFCStepBuffer::Consume` (existing API, used by `EmitPointCloud`).
- `WFCTileCatalog::Populate` (existing, unchanged).
- `StandardRenderPipeline::RegisterMeshEntity` (existing, used by `SetupWFCCatalog`).
- Entity component system init/destroy helpers (existing in test framework).

## Open Questions (to resolve during planning, not implementation)

None at spec level. All design decisions are captured above. The implementation plan may surface smaller-scoped decisions (e.g. exact `max_cells_per_frame` tuning values) but those are engineering choices, not design forks.

## Success Criteria

1. **All Phase B.1 tests still pass.** (74 unit + 3 integration, 0 regressions.)
2. **All new tests pass.** (8 unit + 3 integration.)
3. **Running `TestWFCRendering` visually shows tiles appearing one-by-one** (manual observation on macOS windowed run).
4. **Pressing 'M' in `TestWFCRendering` clears the scene and restarts generation in the other mode.**
5. **Pressing 'R' in `TestWFCRendering` reseeds and restarts generation.**
6. **A solver-detectable contradiction triggers a Restart and the viewer sees the scene clear + refill** (can be forced with a specific seed if natural contradictions are rare).
7. **`TestWFCRendering2DSmoke` still exits 0 in CI.**
8. **No GPU validation errors in any new code path.**

---

**Spec status:** Ready for implementation planning.
**Next step:** Invoke `superpowers:writing-plans` skill to create the implementation plan.
