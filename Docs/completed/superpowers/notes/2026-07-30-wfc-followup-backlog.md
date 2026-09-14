# WFC Follow-up Backlog

**Created:** 2026-07-30
**Branch:** `feat/wfc-pcg`
**Context:** Phase A.1 → B.2 shipped. This file tracks deferred work discovered during completion reviews.

Split into 2D-deferred (parked) and 3D-deferred (active focus). Priority/severity is rough — revisit before planning any follow-up phase.

## 2D-deferred (parked, 2026-07-30)

Out of scope for the next phase. Revisit when 2D actually matters (e.g., tilemap/level-design use case lands).

| ID | Item | Where it leaks | Impact today |
|---|---|---|---|
| 2D-1 | 2D mode reuses 3D meshes (cube/ramp/corner) flat — no true 2D-only flat tiles | `TestWFCRendering` TwoD mode | 2D demo looks like "3D scene viewed from above", not a 2D tilemap |
| 2D-2 | `WFC_FACE_COUNT_2D` not plumbed through `WFCSolver` — always passes `WFC_FACE_COUNT_3D` | `WFCSolver::RunPass` caller | 2D path works only because `grid_size.z=1` makes ±Z neighbors OOB (filtered by bounds check). Works by accident, not by design |
| 2D-3 | `WFCPropagator::face_count` parameter exists but is a runtime value, not a compile-time invariant | `WFCPropagator::RunPass` signature | No type-level guarantee that a "2D" solver actually skips ±Z |
| 2D-4 | 2D mode camera at (2,2,6) looking down at 4×4×1 grid — fixed angle, no orbit | `TestWFCRendering::SnapCameraForCurrentMode` | OK for smoke test, weak for demo |

## 3D-deferred (active focus)

Candidates for the next phase. Ordered rough by "visibility to a viewer watching the demo".

### Visual / Polish (what a viewer sees)

| ID | Item | Where it leaks | Impact today |
|---|---|---|---|
| 3D-V1 | No HUD — mode / restart count / frame / collapse count only go to stdout | `TestWFCRendering::Run` | Demo viewer can't see state; have to read console |
| 3D-V2 | Camera snaps on mode toggle (3D↔2D, 'M' key) — no lerp animation | `TestWFCRendering::SnapCameraForCurrentMode` | Jarring transition; ok for dev, bad for demo |
| 3D-V3 | Entity pop in/out on Restart — destroy + re-spawn is binary, no fade | `PumpSolverFrame` Restart branch | Backtrack visible but abrupt; lacks "undo" feel |
| 3D-V4 | Camera fixed during streaming — doesn't follow grid growth | `UpdateCamera` only responds to mouse/keys | Tile-by-tile appearance has no visual rhythm; viewer doesn't feel the "growth" |
| 3D-V5 | Restart backtrack is all-or-nothing — full destroy + replay, not N-step undo | `WFCSolver` uses `RestartPolicy` (restart-from-scratch) | Faithful to algorithm but looks catastrophic on screen |

### Mesh / Geometry (asset quality)

| ID | Item | Where it leaks | Impact today |
|---|---|---|---|
| 3D-M1 | Corner mesh normals are axial (no cross-product smooth normals) | `create_corner_in_mesh` / `create_corner_out_mesh` in `Engine/Content/ProceduralMesh.h` | Acceptable for flat-shaded catalog; will look faceted under smooth shading |
| 3D-M2 | Ramp mesh normals are axial (same issue as M1) | `create_ramp_mesh` | Same as above |
| 3D-M3 | No cross-variant corner adjacency — each corner variant self-compat only | `WFCTileCatalog::Populate` adjacency rules | Corners of different orientations can't sit next to each other; may produce unnatural boundaries at corner-cluster |

### Solver / Behavior (algorithm)

| ID | Item | Where it leaks | Impact today |
|---|---|---|---|
| 3D-S1 | `WFCSolver::Step` collapses exactly 1 cell/call — budget is ceiling, not target | `WFCSolver::Step` | `TestWFCRendering` 4×4×4 needs 64 frames at 1 cell/frame; `kHeadlessFrameCap=60` exits before completion in CI |
| 3D-S2 | No way to pump multiple Steps/frame from `TestWFCRendering` (would close S1) | `PumpSolverFrame` calls Step once | Manual viewer waits ~64 frames for 4×4×4; no visual progress indicator meanwhile |
| 3D-S3 | Restart generations cap at `max_generations=8`; after that solver returns `GivenUp` | `WFCConfig.max_generations` | Edge case: pathological seeds could exhaust restarts and leave partial grid |

### Infrastructure / Stability (not WFC bugs but affect WFC tests)

| ID | Item | Where it leaks | Impact today |
|---|---|---|---|
| 3D-I1 | `TestWFCRendering` crashes ~1/3 on Initialize with SIGTRAP — Metal pipeline race | Unknown; pre-existing | Flaky CI; retry usually passes. **Unrelated to WFC** |

## Notes on priority

- **3D-V1 (HUD)** is the highest-leverage polish item — small effort, large demo value.
- **3D-S2 (multi-pump in TestWFCRendering)** closes the 60-frame gap for free; TestWFCStreaming already proved 4 pumps/frame works.
- **3D-M1/M2 (smooth normals)** matters only if we ever light the catalog with non-flat shaders; today's flat shading is fine.
- **3D-M3 (cross-variant adjacency)** is a real topology gap but only matters for dense corner clusters; sparse catalogs won't hit it.
- **3D-I1 (Metal race)** is the most annoying but needs separate investigation; not blocking WFC work.
