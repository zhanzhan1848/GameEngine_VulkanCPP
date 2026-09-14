# WFC Phase B.1 Completion Notes

**Date:** 2026-07-29
**Branch:** `feat/wfc-pcg`
**Spec:** `Docs/superpowers/specs/2026-07-29-wfc-phase-b1-design.md` (commit `8170319`)
**Plan:** `Docs/superpowers/plans/2026-07-29-wfc-phase-b1.md` (commit `b70af5b`)

## What landed

1. **Distinct corner geometry** — `create_corner_in_mesh` (concave L, 12 verts, 60 idx) and `create_corner_out_mesh` (convex octant frame, 12 verts, 18 idx) in `Engine/Content/ProceduralMesh.h`. `WFCTileCatalog` updated to `variant_count=4` for both corners. `RegisterWFCCatalogMeshes` swaps the placeholder box mesh for these dedicated generators.
2. **Flag-driven RotationY** — `WFCOutput.cpp` now reads `WFCTile::is_rotationally_symmetric` instead of hardcoding `tile_id == 1`. Ramp, corner_in, corner_out all get `variant * π/2`; cube + pillar stay at 0.
3. **2D mode via parameterization** — `WFCPropagator::RunPass` takes a `face_count` parameter (4 for 2D, 6 for 3D). When `face_count=4`, the `kFaces[]` loop skips the ±Z entries. Existing 3D callers pass `WFC_FACE_COUNT_3D`.
4. **Interactive mode toggle** — `TestWFCRendering` gained a `RenderMode` enum, `CycleMode()` method, and 'M' key handler. Mode cycles between ThreeD (4×4×4, camera at (8,8,8)) and TwoD (4×4×1, camera at (2,2,6)). Same seed (7) for fair A/B comparison.
5. **CI 2D smoke test** — `TestWFCRendering2DSmoke` target compiles `TestWFCRendering.cpp` with `WFC_MODE_2D_SMOKE=1`, forcing Initialize into 2D mode. Renders 60 frames, exits 0.

## Test totals (verified 2026-07-29)

**Unit tests:** 74 cases across 16 binaries, 0 failures.

| Binary | Cases |
|---|---|
| TestWaveGrid | 7 |
| TestWFCConfig | 3 |
| TestWFCObserver | 3 |
| TestWFCOutput | 5 (+1 Phase B.1) |
| TestWFCPropagator | 9 (+2 Phase B.1) |
| TestWFCRandom | 3 |
| TestWFCSolveBudget | 4 |
| TestWFCSolver | 8 |
| TestWFCStepBuffer | 4 |
| TestWFCTileCatalog | 7 (+1 Phase B.1) |
| TestWFCTileRegistry | 3 |
| TestWFCTileRegistryPacking | 4 |
| TestParametricTileNode | 2 |
| TestRestartPolicy | 3 |
| TestTileAdjacency | 5 |
| TestWFCCornerMeshes | 4 (new binary) |

**Phase B.1 unit-test additions:** +8 cases = 4 (new TestWFCCornerMeshes binary) + 1 (TestWFCOutput flag-driven) + 1 (TestWFCTileCatalog corner variants) + 2 (TestWFCPropagator face_count=4 paths).

**Integration tests:** all 3 binaries exit 0.
- `TestWFC3DParametric`: 1 case pass.
- `TestWFCRendering` (3D default): solver emits 64 tile instances (4×4×4), renders 60 frames, exit 0.
- `TestWFCRendering2DSmoke` (CI 2D): solver emits 16 tile instances (4×4×1), renders 60 frames, exit 0.

## Known limitations carried to Phase B.2

- Corner mesh normals are axial (no computed cross-product smooth normals — same as ramp). Acceptable for the flat-shaded catalog.
- 2D mode reuses 3D meshes (cube/ramp/corner_in/corner_out) arranged flat. True 2D-only flat tiles deferred.
- The `WFCPropagator::face_count` parameter exists, but `WFCSolver` always passes `WFC_FACE_COUNT_3D`. The 2D path in `TestWFCRendering` works because `grid_size.z=1` makes ±Z neighbors OOB (filtered by the bounds check at `WFCPropagator.cpp` OOB guard). Threading `WFC_FACE_COUNT_2D` through the solver is a Phase B.2 follow-up.
- Cross-variant corner adjacency not implemented (each corner variant self-compat only; matches ramp).
- No HUD showing current mode (stdout log only — press 'M' and read console).
- Camera snaps on mode toggle (no lerp animation).
- Pre-existing `MetalShader Failed to create library from binary` warning (~62× on startup) is unrelated to WFC.

## Commit history (this phase)

| SHA | Title |
|---|---|
| `13d3ad9` | feat(content): create_corner_in_mesh + create_corner_out_mesh procedural generators |
| `0ceb68b` | fix(content): correct CCW winding for corner_in top + corner_out +Y wall |
| `ef9be7a` | fix(content): correct outward direction comment in create_corner_in_mesh |
| `1d35faa` | feat(wfc): flag-driven RotationY (replace tile_id==1 hardcoded check) |
| `08f2f4d` | feat(wfc): WFCPropagator face_count parameter for 2D/3D mode |
| `a515003` | feat(wfc): catalog corner variant_count = 4 + flag metadata |
| `b223b2a` | docs(wfc): refresh WFCTileCatalog.h header comment for Phase B.1 |
| `130b625` | test(wfc): TestWFCCornerMeshes binary + TestWFCOutput flag-driven cases |
| `5b7f6a8` | feat(wfc): TestWFCRendering CycleMode + 2D mode + WFC_MODE_2D_SMOKE |
| `22ed7b1` | docs(wfc): refresh TestWFCRendering header comments for Phase B.1 |
| (this commit) | docs(wfc): Phase B.1 completion checkpoint |

The two fixup commits (`0ceb68b`, `ef9be7a`) address winding-bug and comment-typo issues caught during Task 1's spec/code-quality review loops. The two docs refresh commits (`b223b2a`, `22ed7b1`) address stale header comments flagged during Task 4 and Task 6 code-quality reviews.
