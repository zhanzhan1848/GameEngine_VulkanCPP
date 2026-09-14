# WFC Phase A.4 Completion Notes

**Date:** 2026-07-28
**Status:** Phase A.4 complete. Ready for Phase B (Layered + Organic).
**Branch:** `feat/wfc-pcg`
**Phase A.4 commits:** 5 (681b8e5 create_ramp_mesh → 7fa0de0 spawn entities) + this docs commit

## Delivered

### New source files
- **`EngineTest/IntegrationTests/Graphics/WFC/TestWFCRendering.cpp`** — Standalone Metal integration test. `WFCRenderingTestCase` derives from `primal::test::RenderTestCase` (mirrors TestPCGScatter pattern). 4 helper methods: `Initialize` (window+device+pipeline+scene+camera+catalog meshes+solver+spawn), `Run` (60-frame headless render loop), `Shutdown` (idempotent, clears entities before pipeline teardown), plus `RegisterWFCCatalogMeshes`, `RunSolverAndEmit`, `SpawnWFCEntities` called from Initialize. Test binary at `Darwin/Debug/TestWFCRendering`, registers via Main.cpp's `#elif TEST_WFC_RENDERING` hook.
- **`EngineTest/IntegrationTests/Graphics/WFC/TestWFCRendering.h`** — Header for the test case class.

### Modified source files
- **`Engine/Content/ProceduralMesh.h`** — Added `create_ramp_mesh(sx, sy, sz, slope_height)` generator. 8 vertices + 30 indices, 5 faces + slope top. Initial implementation had CW windings (plan bug — index triples in the plan produced inward normals); fix commit `66727a4` swapped 2nd/3rd index of every triangle to match engine CCW convention.
- **`Engine/Graphics/WFC/WFCOutput.cpp`** — Ramp variant → RotationY mapping. Variant 0/1/2/3 → 0/π/2/π/3π/2 radians. Other tiles unaffected (variant=0 → RotationY=0). Cube tests in TestWFCOutput still pass.
- **`Engine/Graphics/WFC/WFCOutput.h`** — Doc-only update for the RotationY behavior.
- **`EngineTest/IntegrationTests/CMakeLists.txt`** — Registered `TestWFCRendering` target. Used `setup_test_target` helper (pulls Cocoa/Metal frameworks + shader POST_BUILD copies, required for live Metal pipeline). Added to `IntegrationTests` pseudo-target DEPENDS list.
- **`EngineTest/IntegrationTests/Main.cpp`** — 1-line `#elif TEST_WFC_RENDERING` branch to hook the test into the runner.

### Visual demo summary
End-to-end pipeline:
1. `WFCTileCatalog::Populate` → registry with 5 tiles, adjacency rules
2. Override placeholder `mesh_handles` (1000-1004) with real `ForwardSceneRenderer` slots (0-4)
3. `WFCSolver` runs 4×4×4 = 64 cells, all collapse (result=Done, 64 steps)
4. `WFCOutput::ConsumeSteps` drains 64 Collapse records into `PCGPointSet`
5. `PCGEntityFactory::CreateEntities` → 64 ECS entities with `Transform + MeshIndex`
6. `pipeline->SetPCGEntities` syncs RenderProxies
7. 60-frame render loop draws all 64 entities (5 mesh_info slots, 69 proxies reported)

## Cumulative WFC test tally

| Binary | Phase A.1 | Phase A.2 | Phase A.3 | Phase A.4 | Total |
|---|---:|---:|---:|---:|---:|
| TestWaveGrid | 7 | — | — | — | 7 |
| TestTileAdjacency | 5 | — | — | — | 5 |
| TestWFCConfig | 3 | — | — | — | 3 |
| TestWFCSolveBudget | 4 | — | — | — | 4 |
| TestWFCStepBuffer | 4 | — | — | — | 4 |
| TestWFCRandom | — | 3 | — | — | 3 |
| TestWFCObserver | — | 3 | — | — | 3 |
| TestWFCPropagator | — | 6 | +1 | — | 7 |
| TestRestartPolicy | — | 3 | — | — | 3 |
| TestWFCTileRegistry | — | 3 | — | — | 3 |
| TestWFCSolver | — | 6 | +2 | — | 8 |
| TestWFCTileRegistryPacking | — | — | 4 | — | 4 |
| TestWFCTileCatalog | — | — | 6 | — | 6 |
| TestParametricTileNode | — | — | 2 | — | 2 |
| TestWFCOutput | — | — | 4 | — | 4 |
| TestWFC3DParametric (integration) | — | — | 1 | — | 1 |
| **TestWFCRendering (integration)** | — | — | — | smoke (60 frames) | smoke |
| **Total** | **23** | **24** | **20** | smoke | **67 + smoke** |

All 15 unit binaries + 2 integration binaries pass. No regressions on Phase A.1/A.2/A.3.

## Engine integration verified
- `create_ramp_mesh` follows the `create_box_mesh` idiom (inline, namespace `primal::content`, `RegisterProceduralMesh` return).
- `WFCOutput::ConsumeSteps` single-pass drain pattern preserved; only the per-step `RotationY` attr write changed.
- TestWFCRendering follows the TestPCGScatter pattern (`RenderTestCase` derivation, `NS::Application` event loop, `MacKeyboard.mm`).
- `ForwardSceneRenderer` slot capture uses defensive `slot_base + N` arithmetic with `after >= slot_base + 5` sanity check (better than TestPCGScatter's `meshCount - N` pattern — could be back-ported).
- `PCGEntityFactory::CreateEntities` + `SetPCGEntities` round-trip verified end-to-end.

## Deviations from plan (all reviewed and approved)

- **Task 1 winding bug (plan-corrected):** Plan's index triples produced CW windings (inward normals). Discovered in code quality review. Fix commit `66727a4` swapped 2nd/3rd index of every triangle. Verified via cross-product math against `create_box_mesh`'s CCW convention.
- **Task 2 added .h + Main.cpp hook:** Plan template assumed a standalone `int main()`. Actual macOS Metal path requires `NS::Application::run()` event loop driven by `RenderTestRunner`. Mirroring TestPCGScatter's `.h + .cpp + Main.cpp hook` pattern was the correct call. Implementer's idempotent `Shutdown()` before `terminate()` fixes a real AppKit/engine-free_list teardown race.
- **Task 2 API drift:** Plan assumed `Scene`/`Camera`/`process_window_messages`. Actual engine has `RenderScene`/`RenderView` and uses `NS::Application` event loop. Adapted all calls to real APIs.
- **Task 3 SetLumenConfig + SetEditorMode:** Plan didn't mention these. Required because `StandardRenderPipeline::forward_renderer_` is only constructed inside `InitializeSubsystems()`, which is gated by `SetLumenConfig()`. Without these calls, `RegisterMeshEntity` silently returns `invalid_id`. TestPCGScatter has the same idiom — engine footgun documented in `TestWFCRendering.cpp` comment.
- **Task 3 corner_in/corner_out reuse cube mesh:** Phase A.4 simplification. Documented inline. Each gets a separate `ForwardSceneRenderer` slot so Phase B can swap in distinct L-shaped geometry without rewiring the solver.
- **Task 4 no slot offset:** Plan didn't mention slot offset. Unlike TestPCGScatter (which emits abstract tags and offsets at runtime), WFC embeds real slots in the catalog at registration time. `WFCOutput::ConsumeSteps` writes `tile.mesh_handle` as-is, so `CreateEntities`' output is already correct. Documented inline.
- **Task 4 Shutdown order:** Plan didn't specify. Implementer established: `ClearPCGEntities` (stop `Render()` referencing ids) → `DestroyEntities` (ECS pool reclaims) → `pipeline->Shutdown`. More rigorous than TestPCGScatter's plain `delete pipeline`.

## Known limitations to address in Phase B

- **Corner tiles render as cubes.** Phase A.4 simplification. Phase B: distinct L-shaped geometry via `create_corner_in_mesh` / `create_corner_out_mesh` generators.
- **No textures (white fallback material).** Phase B: per-tile material metadata, texture array integration.
- **Bake-on-load only.** Solver runs once at scene start. User-confirmed scope decision — progressive per-frame collapse deferred to Phase B.
- **Metal-only.** Phase B: Dawn/WASM port.
- **WFCOutput drains step buffer destructively.** Each call to `ConsumeSteps` empties the buffer. Phase B may need non-destructive view if multiple consumers (e.g. shadow + main passes).
- **Camera is fixed.** No WASD/mouse controls. Phase B can add (TestPCGScatter has the pattern).
- **8 tiles × 8 variants hard limit.** Same as Phase A.3 (single `u64` candidate_mask). Catalog is at 5 tiles × ≤4 variants = 14 candidates; room for ~50 more before Phase B refactor.
- **Interactive visual verification not yet performed.** Headless smoke test confirms 64 entities + 60 frames render + exit 0. Visual confirmation (ramps visibly rotated, cubes visible, etc.) requires manual run on a display.
- **Ramp slope normals are axial approximations.** Slope top face uses (0, +Y, +Z) normal direction (tilted up-and-back), which is geometrically correct for the slope direction. Phase B may compute exact slant normals via cross product.
- **`WFCSolver::Step` doesn't notify observer after `RunPropagationCascade` shrinks candidate sets** (Phase A.2 known limitation, still open).
- **Pre-existing engine warnings:** `MetalShader Failed to create library from binary: Invalid library file` appears ~62 times on startup. Pre-existing, unrelated to WFC, not blocking.

## Phase A.4 commits (chronological)

```
681b8e5 feat(content): add create_ramp_mesh procedural generator for WFC ramps
66727a4 fix(content): correct create_ramp_mesh winding to CCW for outward normals
b0adcc4 test(wfc): TestWFCRendering scaffolding — Metal window + empty render loop
1736c9f feat(wfc): register catalog meshes, override placeholders, emit 4x4x4 point set
7fa0de0 feat(wfc): TestWFCRendering spawns entities + visible 4x4x4 grid
```

## Next: Phase B — Layered + Organic

Scope (proposed):
- Proper socket classification (`AutoSocketClassifier`) replacing hand-authored adjacency
- Larger tile sets beyond 8×8 candidate space
- Distinct geometry for corner_in/corner_out
- Per-tile materials + textures
- 2D mode
- Dawn/WASM port
- Progressive per-frame collapse (if real-time demo value emerges)
- Non-destructive WFCOutput view

To be planned as a separate implementation plan.
