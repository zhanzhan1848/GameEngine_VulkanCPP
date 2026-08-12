# WFC Phase A.3 Completion Notes

**Date:** 2026-07-28
**Status:** Phase A.3 complete. Ready for Phase A.4 (Real-time Rendering).
**Branch:** `feat/wfc-pcg`
**Phase A.3 commits:** 13 (15aeaee plan → fec0138 integration test)

## Delivered

### New source files (under `Engine/Graphics/WFC/`)
- **`WFCTileCatalog.{h,cpp}`** — Hand-authored catalog of 5 tile types: cube (1 variant), ramp (4 rotations), corner_in, corner_out, pillar = 8 tile-variant pairs fitting the 8×8=64 candidate bit space. Placeholder mesh_handles 1000-1004. `Populate(registry, adjacency)` registers all tiles and adjacency rules. Cube is universally compatible (wildcard structural tile); others self-compatible.
- **`Nodes/ParametricTileNode.{h,cpp}`** — PCGNode subclass emitting one `PCGPointSet` point per catalog tile. Position=(0,0,0); MeshIndex attr carries `tile.mesh_handle`. Bridge between static WFC catalog and PCG scatter pipeline.
- **`WFCOutput.{h,cpp}`** — Standalone class with `ConsumeSteps(WFCStepBuffer&, const WFCTileRegistry&, f32 cell_size)`. Drains Collapse records into a `PCGPointSet` of tile instances. Single-pass drain into local `std::vector<WFCStep>` (NOT the spec's broken two-pass) — counts Collapse precisely, then writes with separate `out_idx` to skip non-Collapse records.

### Modified source files
- **`WFCTypes.h`** — Added `geometry::geometry_id mesh_handle;` field to `WFCTile`. Struct grew 304→320 bytes (added field plus 4 bytes of pad to keep `sockets[32]` 16-byte aligned). `static_assert(sizeof(WFCTile) == 320)` locks the layout.
- **`WFCTileRegistry.h`** — Multi-tile bit-packing helpers: `MaxVariantsPerTile=8`, `MaxTiles=8`, `BitForTileVariant(tile, variant)`, `TileForBit(bit)`, `VariantForBit(bit)`.
- **`WFCSolver.cpp`** — `PopulateAllCandidates` iterates registry tiles and sets bits via `BitForTileVariant`; `CollapseCell` decodes chosen bit via `TileForBit`/`VariantForBit`.
- **`WFCPropagator.{h,cpp}`** — `RunPass` signature gained `const WFCTileRegistry&` parameter; inner bit loop decodes via static helpers instead of stale Phase A.2 `wfc_tile_id{bit}` / `variant = bit`.

### Test files (under `EngineTest/`)
- `UnitTests/Graphics/WFC/TestWFCTileRegistryPacking.cpp` — 4 cases (Bit_For_Tile_Variant, Tile_For_Bit, Variant_For_Bit, Round_Trip)
- `UnitTests/Graphics/WFC/TestWFCTileCatalog.cpp` — 6 cases (Populate count, Ramp name+variants, Mesh handles, Cube self-compat all faces, Ramp self-compat PosZ/NegZ, 4×4×4 catalog solve)
- `UnitTests/Graphics/WFC/TestParametricTileNode.cpp` — 2 cases (Execute emits 5-point set, MeshIndex attrs in [1000,1004])
- `UnitTests/Graphics/WFC/TestWFCOutput.cpp` — 4 cases (Empty buffer, One Collapse, MeshIndex match, Multi-step drains)
- `IntegrationTests/Graphics/WFC/TestWFC3DParametric.cpp` — 1 case (Full pipeline 4×4×4)
- Updated `TestWFCPropagator.cpp` (added RunPass_Multi_Tile_Filter + updated 3 existing RunPass tests with registry fixtures)
- Updated `TestWFCSolver.cpp` (added multi-tile candidate tests, re-tightened Demo_4x4x4_TwoTile loop on Restarted)

**Total: 17 new test cases across 5 new binaries, all passing.**

## Cumulative WFC test tally

| Binary | Phase A.1 | Phase A.2 | Phase A.3 | Total |
|---|---:|---:|---:|---:|
| TestWaveGrid | 7 | — | — | 7 |
| TestTileAdjacency | 5 | — | — | 5 |
| TestWFCConfig | 3 | — | — | 3 |
| TestWFCSolveBudget | 4 | — | — | 4 |
| TestWFCStepBuffer | 4 | — | — | 4 |
| TestWFCRandom | — | 3 | — | 3 |
| TestWFCObserver | — | 3 | — | 3 |
| TestWFCPropagator | — | 6 | +1 | 7 |
| TestRestartPolicy | — | 3 | — | 3 |
| TestWFCTileRegistry | — | 3 | — | 3 |
| TestWFCSolver | — | 6 | +2 (multi-tile + re-tightened demo) | 8 |
| TestWFCTileRegistryPacking | — | — | 4 | 4 |
| TestWFCTileCatalog | — | — | 6 | 6 |
| TestParametricTileNode | — | — | 2 | 2 |
| TestWFCOutput | — | — | 4 | 4 |
| TestWFC3DParametric (integration) | — | — | 1 | 1 |
| **Total** | **23** | **24** | **20** | **67** |

All 15 unit binaries + 1 integration binary pass. No regressions on Phase A.1/A.2.

## Engine integration verified
- All new sources picked up by existing WFC glob (`Engine/CMakeLists.txt:121-124`) — no Engine CMake edits needed beyond test target registration.
- 5 new test binaries registered in `EngineTest/UnitTests/CMakeLists.txt` + 1 in `EngineTest/IntegrationTests/CMakeLists.txt`, all mirroring the `TestWFCTileCatalog` block pattern.
- PCGNode base class pattern followed exactly (matches `MeshAssignNode`): constructor resizes `outputs` + sets `expected_type`, Execute dispatches to private helper.
- `PCGPointSet` SoA layout respected (`positions` is `std::vector<math::v3>`, `attrs` is `std::vector<f32>` with `attr_stride`).
- `geometry::geometry_id` (DEFINE_TYPED_ID) round-trips through `static_cast<u32>` for attr encoding.
- `WFCTile` 16-byte alignment preserved (`math::v3` = simd::float3, `static_assert(sizeof==320)`).

## Deviations from plan (all reviewed and approved)

- **Task 5 propagator test #1 reshape:** Plan's literal `Removes_Incompatible_Candidates` adjacency rule was cross-tile (A=tile0, B=tile1). Under multi-tile packing, the surviving bit was different. Reshaped to single-tile (tile0 var0 -X tile0 var1) so the test logic still holds. Spec reviewer approved.
- **Task 5 Demo_4x4x4_TwoTile loop extension:** Original Phase A.2 test loop exited on any non-InProgress result. With Task 5's multi-tile fix, the solver may now return Restarted mid-solve. Extended loop to also continue on Restarted. Final assertion re-tightened to `Done | GivenUp` only (no more InProgress/Restarted escape hatches).
- **Task 6 header comment stale:** "Phase A.3 placeholder mesh_handles (sentinel IDs 0-4)" was wrong; cpp uses 1000-1004. Fixed to "1000-1004".
- **Task 9 PCG API corrections:** Plan had multiple stale API assumptions. Real names: `PCGPointSet` (not `CGPointSet`), `PCGDataType::PointSet` (not `PCGPinType`), `outputs[0].expected_type` (not `.type`), `positions` is `std::vector<math::v3>` not raw pointer, header at `WFC/Nodes/` depth uses `../../../Common/...`. All corrections verified against `Engine/Graphics/PCG/PCGTypes.h` and `PCGNode.h`.
- **Task 10 single-pass implementation:** Plan's first version of `WFCOutput::ConsumeSteps` was broken (consumed buffer in counting pass, then had nothing to write). Plan acknowledged the bug and offered a single-pass alternative. Used the alternative; added a separate `out_idx` write counter to robustly handle non-Collapse steps even though Phase A.3 tests only push Collapse records.
- **Task 10 `std::vector<WFCStep>` vs `utl::vector`:** Used `std::vector` for the local drain buffer (function-local temporary, no engine allocator integration needed).
- **Task 10 `geometry_id` namespace:** Spec draft's bare `geometry_id{42}` wouldn't compile — `geometry_id` lives in `primal::geometry`. Test file adds `using namespace primal::geometry;`.
- **Task 11 standalone CMake block:** Bypassed the `setup_test_target` helper (which would pull Cocoa/Metal + shader asset POST_BUILD copies that the headless WFC path doesn't need). Used the same standalone pattern as `UnitTests/Graphics/WFC/` test targets, but registered with the `Engine_Integration_Tests_TestWFC3DParametric` naming convention so `run_integration_tests` (which runs `ctest -R Engine_Integration_Tests`) picks it up. Manually appended to the `IntegrationTests` pseudo-target DEPENDS list since the helper was bypassed.

## Known limitations to address in Phase A.4 / B

- **8 tiles × 8 variants hard limit (single `u64` candidate_mask).** Catalog is currently at 5 tiles × ≤4 variants = 14 candidates. Adding more tiles past 8 total, or variants past 8 per tile, requires `utl::vector<u64>` per cell. Phase B concern.
- **Catalog mesh_handles are placeholders (1000-1004).** Phase A.4 will swap these for real `register_mesh_asset` results via `create_box_mesh` and friends.
- **WFCOutput drains step buffer destructively.** Each call to `ConsumeSteps` empties the buffer. If the renderer needs to read the same steps twice (e.g. for shadow + main passes), Phase B will need a non-destructive view or copy-on-drain.
- **Catalog adjacency is intentionally simplified.** Cube is universally compatible (wildcard). Other tiles self-compatible. Real game rules need proper socket classification (Phase B `AutoSocketClassifier`).
- **No 2D mode yet.** Phase A foundation defines `WFC_FACE_COUNT_2D = 4` but no 2D solver/cataldemo. Deferred to Phase A.4 or B.
- **No real-time rendering integration yet.** That's the entire Phase A.4 scope.
- **`WFCSolver::Step` doesn't notify observer after `RunPropagationCascade` shrinks candidate sets** (Phase A.2 known limitation, still open). Lazy heap filter in `WFCObserver::PickNextCollapse` recovers correctness; per-Step perf cost only.
- **Ramp variant → `RotationY` attr mapping deferred.** WFCOutput writes `RotationY=0` regardless of variant. Phase A.4 will derive rotation from variant (0/1/2/3 → 0°/90°/180°/270°) when visual demo lands.

## Phase A.3 commits (chronological)

```
15aeaee docs(plan): WFC Phase A.3 — tiles + PCG integration
8090ed3 feat(wfc): WFCTileRegistry multi-tile bit-packing helpers
f828257 feat(wfc): add mesh_handle field to WFCTile
8418d15 feat(wfc): WFCSolver multi-tile candidate population
c521968 feat(wfc): WFCSolver multi-tile collapse decode
d21ca7b feat(wfc): WFCPropagator multi-tile candidate filter via registry
4d178e0 chore(wfc): comment cleanup after Phase A.3 Task 5
d92a0c5 feat(wfc): WFCTileCatalog scaffolding with 5 tile types
ac01321 docs(wfc): fix WFCTileCatalog header comment — placeholders are 1000-1004 not 0-4
fc0e12f feat(wfc): WFCTileCatalog hand-authored adjacency rules
1fc344d test(wfc): WFCTileCatalog end-to-end 4x4x4 solve
7582d07 feat(wfc): ParametricTileNode PCGNode emitting catalog point set
5937c8e feat(wfc): WFCOutput converts step buffer to PCGPointSet
fec0138 test(wfc): TestWFC3DParametric end-to-end integration test
```

## Next: Phase A.4 — Real-time Collapse Rendering

Scope: register real procedural meshes (cube, ramp variants via rotation, corner_in/out, pillar) and feed the WFCOutput point set into the existing `PCGEntityFactory` for per-cell instance rendering. End of Phase A.4: a live demo where the user can watch the WFC solver collapse a 4×4×4 grid into visible geometry. To be planned as a separate implementation plan.
