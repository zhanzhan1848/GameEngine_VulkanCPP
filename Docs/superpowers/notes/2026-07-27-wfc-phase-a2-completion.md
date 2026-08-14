# WFC Phase A.2 Completion Notes

**Date:** 2026-07-27
**Status:** Phase A.2 complete. Ready for Phase A.3 (Tiles + Demos).
**Branch:** `feat/wfc-pcg`
**Phase A.2 commits:** 9 (a8dfe4d WFCRandom → 340a49e 4×4×4 demo)

## Delivered

### Source files (under `Engine/Graphics/WFC/`)
- **`WFCRandom.{h,cpp}`** — Deterministic xorshift32* RNG (Marsaglia shifts `<<13 >>17 <<5` + golden-ratio multiply `0x9E3779B9u`). `NextRange` uses Lemire debiased modulo rejection. 4 methods: `NextU32`, `NextRange`, `NextF32`, `NextBool`. Reproducible across platforms for the same seed.
- **`WFCObserver.{h,cpp}`** — Min-entropy cell selection. `std::vector<std::pair<u8, WFCGridCoord>>` heap with `ComparePairGreater` (`a.first > b.first`) → min-heap on entropy. Lazy staleness filter: pop → validate (collapsed/bounds/entropy==0/stale) → re-push if entropy mismatch. `OnCellChanged` pushes `(0, c)` sentinel to bubble to top.
- **`WFCPropagator.{h,cpp}`** — AC-4-style constraint propagation. `utl::vector<WFCGridCoord> dirty_queue_`. `RunPass` swaps the queue (so `OnCellCollapsed` during this pass queues for next pass), iterates each dirty cell's 6 faces, filters candidate mask via `adjacency.Compatible(...)` for each set bit. Re-queues neighbors on candidate change. Sets `out_contradiction = true` on `candidate_count == 0`.
- **`RestartPolicy.{h,cpp}`** — `Decision` enum (`Continue`/`Restart`/`GiveUp`). `OnContradiction(coord, attempted_tile, generation)` pushes coord into `conflicts_` and returns `Restart` if `gen+1 < max_generations_`, else `GiveUp`. (Implementer dropped the unused `ConflictRecord` struct as YAGNI; just stores coords.)
- **`WFCTileRegistry.{h,cpp}`** — Minimal `Register(tile)` auto-assigns sequential IDs starting at 0, copies and stamps `id` field, tracks `max_variants_` across registered tiles. `Get`/`GetMutable` use `assert` for OOB. `Count`/`MaxVariants` getters.
- **`WFCSolver.{h,cpp}`** — Orchestrates Observer + Propagator + Restart + Budget + StepBuffer + RNG. `StepResult` enum (`InProgress`/`Done`/`Restarted`/`GivenUp`). `PopulateAllCandidates` writes `full_mask` into every cell using `registry.MaxVariants()`. `CollapseCell` picks via `rng_.NextRange(candidate_count)` and walks the set-bit list. `RunPropagationCascade` loops `RunPass` until queue drains or contradiction.

### Test files (under `EngineTest/UnitTests/Graphics/WFC/`)
- `TestWFCRandom.cpp` — 3 cases (seed reproducibility, different seeds, `NextRange` bounding over 1000 draws)
- `TestWFCObserver.cpp` — 3 cases (min-entropy pick, all-collapsed returns invalid, `OnCellChanged` heap refresh)
- `TestWFCPropagator.cpp` — 6 cases (queue dirty neighbors, init clears, isolated cell, AC-4 candidate filter, contradiction detection, skip already-collapsed)
- `TestRestartPolicy.cpp` — 3 cases (first contradiction restarts, exceeds max gives up, conflict memory grows)
- `TestWFCTileRegistry.cpp` — 3 cases (register/get, max variants across tiles, count grows)
- `TestWFCSolver.cpp` — 6 cases (Initialize populates, Step collapses single cell, Step pushes Collapse record, 2×2×2 all-wildcard solve, Budget stops mid-solve, 4×4×4 demo terminates)

**Total: 24 new test cases across 6 binaries, all passing.**

## Cumulative WFC test tally

| Binary | Phase A.1 | Phase A.2 | Total |
|---|---:|---:|---:|
| TestWaveGrid | 7 | — | 7 |
| TestTileAdjacency | 5 | — | 5 |
| TestWFCConfig | 3 | — | 3 |
| TestWFCSolveBudget | 4 | — | 4 |
| TestWFCStepBuffer | 4 | — | 4 |
| TestWFCRandom | — | 3 | 3 |
| TestWFCObserver | — | 3 | 3 |
| TestWFCPropagator | — | 6 | 6 |
| TestRestartPolicy | — | 3 | 3 |
| TestWFCTileRegistry | — | 3 | 3 |
| TestWFCSolver | — | 6 | 6 |
| **Total** | **23** | **24** | **47** |

All 11 binaries pass. No regressions on Phase A.1.

## Engine integration verified
- All Phase A.2 sources picked up by existing WFC glob (`Engine/CMakeLists.txt:121-124` from Phase A.1 Task 1)
- 6 new test binaries registered in `EngineTest/UnitTests/CMakeLists.txt` mirroring the `TestWFCStepBuffer` block
- All sources build cleanly into the Engine library
- `utl::vector`, `DEFINE_TYPED_ID`, `math::v3` (16-byte alignment), `u32`/`u64`/`f32` used throughout
- Engine test framework (`TestSuite`, `TEST_ASSERT`, `TEST_ASSERT_EQ`, `TEST_ASSERT_STR_EQ`, `TEST_CASE`) used correctly

## Deviations from plan (all reviewed and approved)

- **Task 4 Propagator multi-tile assumption**: Plan's literal `wfc_tile_id my_tile{0}` would have pruned all candidates and made the propagation tests fail. Implementer used `wfc_tile_id my_tile{bit}` so that bit index `b` represents `(tile_id=b, variant=b)` — acceptable for Phase A.2 because tests use single-variant tiles where the bit-to-tile mapping is consistent. Phase A.3 will replace with proper `(tile_id, variant)` packing.
- **Task 5 RestartPolicy**: Unused `ConflictRecord` struct dropped as YAGNI. `conflicts_` now stores `utl::vector<WFCGridCoord>` directly.
- **Task 7 WFCSolver bugs caught by implementer** (plan had 4 latent bugs):
  - **Planted `TEST_ASSERT` in `CollapseCell`** — test-only macro expands to `return TestResult::Failed`. Replaced with `assert(candidate_count > 0)` + `#include <cassert>`.
  - **Missing `grid.Initialize(config.grid_size, max_variants)`** — test fixture default-constructs `WaveGrid`, so `CellAt` would assert-fail on empty cells.
  - **`restart_{8}` placeholder** — `RestartPolicy` field had no default ctor, so `WFCSolver` default ctor wouldn't compile.
  - **`Step` returned `InProgress` instead of `Done` on final cell** — added `observer_.Empty()` check after successful propagation.
- **Task 6 plan typo**: Plan used `TEST_ASSERT_EQ_STR`; engine framework uses `TEST_ASSERT_STR_EQ`. Implementer was pre-warned.
- **Task 8 fixture mismatch**: Initial `Solves_2x2x2_AllWildcard` failed because the fixture set `t.sockets[0] = 0xFFFFFFFFFFFFFFFF` expecting wildcard behavior, but Phase A.2 propagator doesn't read `sockets[]` — it only consults `TileAdjacencyTable`. Empty table → "nothing compatible" → contradiction → restart loop. Fix: Added explicit self-compat declarations on PosX/PosY/PosZ for the wildcard tile in both Task 8 tests (commit `69ece2c`).

## Known limitations to address in Phase A.3

- **Single-tile candidate space (two sites, must be relaxed together)**:
  - `WFCSolver::PopulateAllCandidates` (`Engine/Graphics/WFC/WFCSolver.cpp:57`) — assumes `bit index = variant index of tile 0`. Multi-tile requires `(tile_id, variant)` pair packing or per-tile candidate masks.
  - `WFCPropagator::RunPass` (`Engine/Graphics/WFC/WFCPropagator.cpp:96-97`) — **stronger** assumption: `wfc_tile_id my_tile{bit}` and `my_variant = bit`. Bit index is used as BOTH tile_id and variant. This is acceptable for Phase A.2 because tests register single-variant tiles only (so `tile_id==variant` by construction). If Phase A.3 relaxes `WFCSolver` without also touching the propagator, multi-tile propagation will silently misbehave. Update both sites in the same change.
- **Observer heap drift after propagation**: `WFCSolver::Step` never calls `observer_.OnCellChanged` after `RunPropagationCascade` shrinks candidate sets. The lazy filter in `WFCObserver::PickNextCollapse` recovers correctness (re-push on entropy mismatch), but each stale pop costs an extra heap operation. Not a correctness bug — a per-Step performance hazard. Phase B can fix by having the propagator notify the observer (would require an observer pointer on `WFCPropagator`).
- **No entropy biasing from conflict memory**: `RestartPolicy` records conflict coords but doesn't bias the Observer. Phase B will add this.
- **No parallel propagation**: All work on main thread. Phase B adds `ParallelFor`.
- **No real tile sockets**: Phase A.3 introduces `AutoSocketClassifier` and real socket encodings from `PCGTile` data.
- **Restart re-randomizes all cells**: Doesn't preserve any structure across generations. Phase B will add structural carryover.
- **`WFCObserver::OnCellChanged` pushes `(0, c)` sentinel**: Bubbles the cell to the top of the heap, then re-validates on Pop. Works for Phase A.2 workloads; if the heap grows large in later phases, a reverse-index `coord → heap_slot` may be more efficient.
- **`WFCPropagator::RunPass` re-queues via `OnCellCollapsed`**: Naming is misleading (the cell isn't actually collapsed, just changed). Renamed candidate for Phase B: `QueueNeighborsForRecheck`.
- **`WFCRandom::NextRange` rejection loop unbounded**: Worst case never terminates if `NextU32` keeps returning rejected values. In practice the rejection probability is `< 2^-32` so this is fine, but worth noting.
- **`WFCTileRegistry` is not thread-safe**: `Register` mutates `tiles_` and `max_variants_` without synchronization. Single-threaded Phase A.2 is fine; Phase B (parallel propagation) will need a concurrent version or registration-phase-only contract.

## Phase A.2 commits (chronological)

```
a8dfe4d feat(wfc): WFCRandom deterministic xorshift32* RNG
3fe1c4e feat(wfc): WFCObserver min-entropy cell selection
c93cb50 feat(wfc): WFCPropagator dirty-queue scaffolding
fcb98a0 feat(wfc): WFCPropagator AC-4-style constraint propagation
d587aff feat(wfc): RestartPolicy no-backtracking decisions + conflict memory
6d872ac feat(wfc): WFCTileRegistry minimal register/get/maxvariants
c6f69f8 feat(wfc): WFCSolver orchestrator with single-step collapse
69ece2c fix(wfc): wildcard tests need explicit self-compat in adjacency table
340a49e test(wfc): WFCSolver end-to-end 4x4x4 demo (Phase A.2 placeholder)
```

## Next: Phase A.3 — Tiles + Demos

Scope: PCG tile nodes (`ParametricTileNode`, `SDFTileNode`), `AutoSocketClassifier`, hand-authored tile sets, real-time collapse rendering via existing `PCGEntityFactory`. Builds on the solver foundation from Phase A.2. To be planned as a separate implementation plan.
