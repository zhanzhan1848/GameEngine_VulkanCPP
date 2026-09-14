# WFC Phase C.1 Mixed Multi-Category — Completion Note

**Date:** 2026-08-06
**Plan:** `Docs/superpowers/plans/2026-08-06-wfc-phase-c1-mixed-multi-category.md`
**Branch:** `feat/wfc-pcg`

## Outcome

Phase C.1 ships a second tile-source composition for the WFC streaming showcase
that mixes two asset packs via automatic socket classification. Operators can
toggle between:

- **KenneyOnly** (8 hand-authored Kenney dungeon tiles, Dungeon category) — original path.
- **MixedMulti** (15 Ruins + 12 ProceduralRoomPack = 27 tiles spanning Ruins +
  Primitive categories, adjacency rebuilt via `AutoSocketClassifier`).

Both modes run inside the existing `TestKenneyTilePreview` binary (native + WASM),
selected via the `M` hotkey (native) or the **Tile Source** dropdown (WASM).

## Milestones shipped

| M | Title | Status |
|---|---|---|
| M1 | Widen candidate_mask `u64` → `u64[4]` (256 candidates) | ✅ |
| M2 | `AutoSocketClassifier` (8×8 occupancy grid, Möller-Trumbore) | ✅ |
| M3 | `ProceduralRoomPack` (12 tiles, primitive category) | ✅ |
| M4 | Multi-set registry + `BuildFromClassifier` | ✅ |
| M5 | `ConflictSeedMemory` + decay (bias-weighted observer) | ✅ |
| M6 | Native GUI showcase with mode toggle | ✅ |
| M6.5 | WASM C ABI + HTML UI for mode toggle | ✅ |
| M7 | Regression sweep + completion note | ✅ |

## Key contracts locked in

1. **Candidate mask is `u64[kMaskWords]`** with `kMaskWords = 4`. Supports up to
   256 tiles per registry. `WaveGrid`/`WFCPropagator`/`WFCSolver` all migrated.
2. **`SocketEncoding = u64`** with `MirrorFlipU` for opposite-face comparison.
3. **`AutoSocketClassifier::ClassifyFace`** ray-traces mesh triangles against an
   8×8 occupancy grid; `BuildFromClassifier` rebuilds a `TileAdjacencyTable` from
   any registry using the resulting signatures.
4. **`RestartPolicy::ConflictRecord`** tracks full `(coord, tile, occurrence_count)`
   tuples. `BiasForCell` and `BiasForTileInCell` feed observer weighting;
   `DecayAll` halves counts each restart so transient failures fade.
5. **`WFCCategory` enum**: Primitive / Ruins / Dungeon / Cyber / Organic. The
   showcase uses `CategoryMaskFor(Ruins) | CategoryMaskFor(Primitive)` for
   MixedMulti mode.

## Demo wiring

| Surface | KenneyOnly → MixedMulti trigger |
|---|---|
| Native (`TestKenneyTilePreview`) | `M` hotkey |
| WASM (`TestKenneyTilePreviewWASM`) | Tile Source dropdown → `wfc_set_solve_mode` |

Mode switch rebuilds registry+adjacency from scratch via `InitWFC` (heavier than
a reseed because each mode owns a different adjacency set).

## Smoke asserts (MixedMulti mode)

- `kMinCellsCollapsed = 100` cells collapsed.
- `kMinDistinctTiles = 3` distinct tile ids in the final grid.
- `kMinDistinctCategories = 2` distinct `WFCCategory` values — catches the silent
  failure mode where the classifier prunes one category's tiles out of every cell.

If the solver exhausts `max_generations` on MixedMulti without meeting the
category threshold, `DegradeToSingleSet()` narrows `active_category_mask` to
Ruins-only and reseeds. The operator sees a log line announcing the degradation.

## Regression sweep — 2026-08-06

All 26 WFC binaries green (`✅ 所有测试通过!`). Highlights:

| Test | Count |
|---|---|
| `TestAutoSocketClassifier` (M2) | 8/8 |
| `TestProceduralRoomPack` (M3) | 6/6 |
| `TestTileAdjacencyBuild` (M4) | 4/4 |
| `TestConflictSeedMemory` (M5) | 9/9 |
| `TestRestartPolicy` | 3/3 |
| `TestWFCRuinsMixedCategories` | integration, full pass |
| `TestWFCRuinsAdjacencyConsistency` | integration, full pass |
| `TestWFCCategorySolve` | integration, full pass |
| `TestWFCSolver`, `TestWFCPropagator`, `TestWaveGrid` (via M1) | all green |

## Build verification

- Native (`cmake --build build --target TestKenneyTilePreview -j 4`) — links cleanly.
- WASM (`cmake --build build-wasm --target TestKenneyTilePreviewWASM -j 4`) — links
  cleanly; `_wfc_set_solve_mode` and `_wfc_get_solve_mode` exported.

## Known limitations / follow-ups

1. **Ruins procedural-mesh simplification.** All 15 Ruins tiles currently use
   `create_weathered_cube_mesh` (per-tile seed/amplitude variation). The richer
   factory functions (`create_ramp_mesh`, `create_corner_in_mesh`, etc.) are
   registration-only and don't expose `RHIMeshAsset` outputs. A future task could
   add `RHIMeshAsset` overloads to make those available to the classifier.
2. **Visual verification deferred.** Smoke asserts verify solver behavior but
   orientation/material quality of the MixedMulti render is human-reviewed. Run
   the showcase binary, press `M`, and eyeball the result.
3. **WASM HUD sync.** The Tile Source dropdown stays in sync because it's the
   sole trigger on WASM; `setWfcHud` doesn't emit the mode. If a future
   server-driven mode change is added, extend `SyncWfcHudIfWasm`.
