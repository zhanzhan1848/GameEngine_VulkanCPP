# WFC Phase B.2 Completion Notes

**Date:** 2026-07-29
**Branch:** `feat/wfc-pcg`
**Spec:** `Docs/superpowers/specs/2026-07-29-wfc-phase-b2-design.md` (commit `e45f2b1`)
**Plan:** `Docs/superpowers/plans/2026-07-29-wfc-phase-b2.md` (commit `e598a48`)

## What landed

Phase B.2 converts WFC generation from "emit complete scene in one frame" to a streaming pipeline. Each frame pumps the solver once, drains the new Collapse steps since the last Restart, spawns entities for them, and — when a Restart is seen — destroys all previously-spawned entities first to visualize the backtrack.

1. **`WFCOutput::DrainStream` API** — New streaming drain in `Engine/Graphics/WFC/WFCOutput.{h,cpp}` returns a `WFCStreamDrainResult` (new_points + restart_seen + restart_count). Internally, `DrainStream` walks the buffer once, tracks `emit_start = index_after_last_Restart`, and only Collapse steps at `i >= emit_start` survive into `new_points`. Helpers `WritePointToSet` and `DrainAll` were extracted from the legacy `ConsumeSteps` to share code without duplicating the per-step emit logic.

2. **TestWFCRendering streaming Run loop** — `EngineTest/IntegrationTests/Graphics/WFC/TestWFCRendering.{h,cpp}` now owns long-lived solver state (`grid_`, `buf_`, `solver_`, `budget_`) across frames instead of solving to completion in `Initialize`. `PumpSolverFrame` does: `budget_.Reset()` → `solver_->Step(budget_)` → `DrainStream` → destroy prior entities if `restart_seen` → `PCGEntityFactory::CreateEntities` + append + `pipeline->SetPCGEntities`. Completion is detected by `solver_state_ == Done` (or `GivenUp`). `ReseedSolver` tears down streaming state and recreates with `rng_seed_++` for a fresh solve on demand.

3. **Interactive 'R' + 'Space' keys** — Rising-edge handlers in `Run()`: 'R' calls `ReseedSolver()` (visible restart at the same camera), 'Space' toggles `paused_` (solver frozen, render loop continues). Both latch on the rising edge (`input::get` + per-key `*_pressed_` flag) so auto-repeat doesn't fire them repeatedly.

4. **TestWFCStreaming integration binary** — New target #32 (`EngineTest/IntegrationTests/Graphics/WFC/TestWFCStreaming.{h,cpp}`). Quantitative counterpart to TestWFCRendering: pumps **4** solver Steps per frame (vs 1) so a 4×4×4 grid completes in ~16 frames, then asserts `solver_state_ == Done` and `total_collapses_ == 64`. Catalog setup is copied verbatim from TestWFCRendering (engine convention: helpers are not shared across test cases).

5. **TestWFCStreamDrain unit binary** — New target in `EngineTest/UnitTests/Graphics/WFC/TestWFCStreamDrain.cpp`. 5 cases: `EmptyBuffer_ReturnsEmpty`, `SingleCollapse_ProducesOnePoint`, `MultipleCollapses_ProducesAllPointsInOrder`, `RestartClearsNewPoints_AndSetsFlag`, `MultipleRestarts_IncrementsCountAndKeepsLastBatch`. Covers the restart-clears-prior-batch contract directly.

## Test totals (verified 2026-07-29)

**Unit tests:** 79 cases across 17 binaries, 0 failures.

| Binary | Cases |
|---|---|
| TestWaveGrid | 7 |
| TestWFCConfig | 3 |
| TestWFCObserver | 3 |
| TestWFCOutput | 5 |
| TestWFCPropagator | 9 |
| TestWFCRandom | 3 |
| TestWFCSolveBudget | 4 |
| TestWFCSolver | 8 |
| TestWFCStepBuffer | 4 |
| TestWFCTileCatalog | 7 |
| TestWFCTileRegistry | 3 |
| TestWFCTileRegistryPacking | 4 |
| TestParametricTileNode | 2 |
| TestRestartPolicy | 3 |
| TestTileAdjacency | 5 |
| TestWFCCornerMeshes | 4 |
| **TestWFCStreamDrain** | **5 (new)** |
| **Total** | **79** |

**Phase B.2 unit-test additions:** +5 cases in new TestWFCStreamDrain binary.

**Integration tests:** all 4 binaries exit 0.
- `TestWFC3DParametric`: unchanged.
- `TestWFCRendering` (3D default): streaming Run, pumps 1 Step/frame, renders 60 frames (headless cap), exit 0.
- `TestWFCRendering2DSmoke`: same source, `WFC_MODE_2D_SMOKE=1`, exit 0.
- **`TestWFCStreaming` (new)**: pumps 4 Steps/frame, completes in 16 frames, asserts `state=1` (Done) + `collapses=64` + `restarts=0`. stdout: `[TestWFCStreaming] frames=16 collapses=64 restarts=0 state=1`.

## Spec simplification discovered during planning

The original spec called for 3 new APIs. Codebase verification during planning revealed `PCGEntityFactory::CreateEntities` and `StandardRenderPipeline::SetPCGEntities` already exist. Only **1** new API (`WFCOutput::DrainStream`) was actually needed — the plan shipped with 4 tasks instead of the originally sketched 5+, halving the implementation surface.

## Key insight: WFCSolver::Step collapses ONE cell per call

The plan's budget suggested multiple cells might collapse per Step. Code review during Task 2 revealed `WFCSolver::Step` collapses exactly **1** cell per call (the budget is a ceiling, not a target). Consequences:
- TestWFCRendering pumps 1 Step/frame → visible streaming at 1 tile per frame.
- TestWFCStreaming pumps 4 Steps/frame → completes 4×4×4 in ~16 frames instead of 64.

This is documented in the TestWFCStreaming header comment.

## Known limitations carried forward

- **Pre-existing TestWFCRendering flakiness**: crashes during Initialize with SIGTRAP ~1/3 runs (Metal pipeline race). Retry passes. Unrelated to WFC; tracked separately.
- **No animation/lerp on Restart**: when entities are destroyed and re-spawned after a Restart, they pop in/out instantly. A fade or slide animation would soften the visual backtrack but is deferred.
- **No HUD**: stdout logs only (`[TestWFCStreaming] frames=...`, etc.). Mode/restart/frame counters are not rendered to screen.
- **2D mode plumbing unchanged**: `WFCSolver` still passes `WFC_FACE_COUNT_3D` to `WFCPropagator::RunPass`; 2D mode works because `grid_size.z=1` makes ±Z neighbors OOB. Threading `WFC_FACE_COUNT_2D` through the solver remains a Phase B.1-carried follow-up.
- **Catalog setup duplication**: TestWFCStreaming copies RegisterWFCCatalogMeshes + SetupWFCCatalog verbatim from TestWFCRendering. The engine's test-case convention treats helpers as per-case, so this is intentional rather than a shared utility.
- **Camera doesn't follow grid growth**: the streaming visualization shows tiles appearing one-by-one at fixed camera. An automatic "grow + reframe" mode would help but is out of scope.

## Commit history (this phase)

| SHA | Title |
|---|---|
| `abeb59a` | feat(wfc): DrainStream API + WritePointToSet helper for streaming generation |
| `925b162` | fix(wfc): remove unused WFCSolveBudget.h include from WFCOutput.cpp |
| `9ad0a78` | feat(wfc): TestWFCRendering streaming Run loop + Restart backtrack |
| `6d38bad` | fix(wfc): TestWFCRendering Task 2 cleanup — unused fields, redundant init, duplicate include |
| `6290b07` | feat(wfc): TestWFCRendering 'R' re-seed + 'Space' pause keys |
| `cf7ef7f` | test(wfc): TestWFCStreaming integration binary — streaming invariants |
| (this commit) | docs(wfc): Phase B.2 completion checkpoint |

The three fixup commits (`925b162`, `6d38bad`, and the spec-compliance pass that preceded `cf7ef7f`) address unused-include, YAGNI cleanup, and ctor-signature issues caught during the two-stage review (spec compliance → code quality) per task.

## Subagent-driven development summary

Executed via `superpowers:subagent-driven-development`. Each of the 4 implementation tasks went through:
1. Implementer subagent (fresh context, full task text + scene-setting context provided).
2. Spec compliance reviewer subagent (verified code matches plan, no more/no less).
3. Code quality reviewer subagent (strengths + issues, re-review until approved).

Notable review findings:
- **Task 1**: Removed unused `WFCSolveBudget.h` include after clangd flagged it.
- **Task 2**: Discovered redundant `grid_->Initialize` call (solver does it internally), duplicate `TileAdjacency.h` include, and unused `RenderProxy.h`/`PCGTypes.h` includes. Also removed `key_r_pressed_`/`key_space_pressed_` as YAGNI (re-added in Task 3 when handlers landed).
- **Task 4**: Implementer correctly used `WFCSolver::StepResult` (nested enum, not namespace-level), added explicit `WFCSolveBudget(8u, 16u)` ctor init (no default ctor), mirrored TestWFCRendering's Shutdown (FreeList contract), and added `TestWFCStreaming.h` + `Main.cpp` branch (matches every other test in the directory).

All 4 deviations from the plan's verbatim code were judged justified by the spec reviewer.
