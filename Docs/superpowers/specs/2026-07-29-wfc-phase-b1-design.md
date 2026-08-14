# WFC Phase B.1: Tech Debt Cleanup + Small Features Design

**Date:** 2026-07-29
**Branch:** `feat/wfc-pcg` (continues from Phase A.4)
**Spec status:** Approved (2026-07-29)

## Goal

Three Phase A.4-deferred items bundled into a single plan. All backward-compatible, no breaking API changes:

1. **Distinct corner geometry** — Replace cube-aliased corner_in/corner_out with dedicated procedural meshes (concave L + convex octant), 4 Y-axis rotation variants each.
2. **Flag-driven RotationY** — Replace `WFCOutput.cpp`'s hardcoded `tile_id == 1` ramp check with the existing `WFCTile::is_rotationally_symmetric` flag.
3. **2D mode via Propagator parametrization** — `WFCPropagator::RunPass` gains a `face_count` parameter. Same code path serves 2D (face_count=4) and 3D (face_count=6). No new solver class.

Interactive runtime toggle: press 'M' in TestWFCRendering to cycle between 3D (4×4×4) and 2D (4×4×1) modes.

## Scope

**In scope:**
- Two new procedural mesh generators (`create_corner_in_mesh`, `create_corner_out_mesh`)
- WFCOutput.cpp 3-line refactor (flag-driven RotationY)
- WFCPropagator signature change (add `face_count` parameter)
- WFCTileCatalog update (corner variant_count: 1 → 4)
- TestWFCRendering: Mode enum, CycleMode() method, keyboard input, 2D_SMOKE CI define
- 4 new unit test cases + 1 new test binary

**Out of scope (deferred to Phase B.2):**
- Auto-socket classifier (replaces hand-authored adjacency)
- Larger tile sets beyond 8×8 candidate space
- Per-tile materials + textures
- True 2D-only flat tile shapes (currently reuses 3D meshes)
- Dawn/WASM port
- Progressive per-frame collapse
- Non-destructive WFCOutput view
- Computed slope normals (Phase A.4 axial approximations carry forward)
- HUD showing current mode (stdout log only)
- Camera lerp animation (snap only)

## Architecture

### Single solver path, parameterized

No `WaveGrid2D` or `Propagator2D` class. The 2D path is the 3D path with `grid_size.z=1` and `face_count=4`. Same WaveGrid, same Propagator, same WFCSolver, same WFCOutput. The ±Z sockets on 2D tiles are simply never queried — dead data, not wildcards.

### Single catalog

Same `WFCTileCatalog::Populate` works for both modes. The 5 tile types (cube/ramp/corner_in/corner_out/pillar) are shared. Phase B.2 may introduce 2D-only flat tiles; Phase B.1 reuses what's there.

### Single test binary

`TestWFCRendering` gains a `Mode` enum + `CycleMode()` method. Mode is selected interactively at runtime (press 'M'). CI runs in 3D mode by default + 2D smoke test via `WFC_MODE_2D_SMOKE` compile define.

## Components

### New procedural mesh generators (`Engine/Content/ProceduralMesh.h`)

**`create_corner_in_mesh(sx, sy, sz)`** — Concave L-shape (interior corner, e.g. inside corner of a room). Geometry: a cube with one vertical-edge quadrant removed. ~8 vertices, ~24 indices (4 visible quads = 8 triangles). 4 rotation variants via caller-set `RotationY` (same pattern as ramp). Outward axial normals. CCW winding from day one (avoids Task 1 winding bug).

**`create_corner_out_mesh(sx, sy, sz)`** — Convex octant (exterior corner, e.g. outside corner of a building). Geometry: three mutually-perpendicular faces meeting at one corner. ~6 vertices, ~12 indices (3 quads = 6 triangles). Same 4-variant pattern. Same axial normals. CCW winding.

Both generators follow `create_ramp_mesh` idiom: inline, namespace `primal::content`, return `RegisterProceduralMesh(asset)`.

### WFCOutput.cpp refactor

Replace:
```cpp
f32 rot_y = 0.0f;
const u32 tile_id_u32 = static_cast<u32>(s.tile);
if (tile_id_u32 == 1) {  // ramp
    constexpr f32 kHalfPi = 1.5707963267948966f;
    rot_y = static_cast<f32>(s.variant) * kHalfPi;
}
```

With:
```cpp
f32 rot_y = 0.0f;
if (!tile.is_rotationally_symmetric) {
    constexpr f32 kHalfPi = 1.5707963267948966f;
    rot_y = static_cast<f32>(s.variant) * kHalfPi;
}
```

Works uniformly for ramp (4 variants), corner_in (4 variants), corner_out (4 variants). Cube/pillar (`is_rotationally_symmetric=true`, 1 variant) stay at RotationY=0.

### WFCPropagator parametrization

```cpp
u32 WFCPropagator::RunPass(WaveGrid& grid,
                           const TileAdjacencyTable& adjacency,
                           const WFCTileRegistry& registry,
                           u32 face_count,                    // NEW
                           bool& out_contradiction);
```

The `kFaces[]` static array keeps all 6 entries (3D), but the loop stops at `i < face_count`. Callers:
- 3D mode: pass `face_count=6` (default value)
- 2D mode: pass `face_count=4` (skips ±Z entries)

Existing Phase A.2/A.3 callers pass 6 (default). Backward compatible.

### WFCTileCatalog update

- corner_in `variant_count`: 1 → 4
- corner_out `variant_count`: 1 → 4
- Adjacency rules: each corner variant self-compatible (same pattern as ramp). Cross-variant compatibility deferred — Phase A.3 simplification preserved.

Catalog candidate count: 1 (cube) + 4 (ramp) + 4 (corner_in) + 4 (corner_out) + 1 (pillar) = 14. Well under the 64-bit candidate_mask cap.

### TestWFCRendering additions

**New members:**
```cpp
enum class RenderMode : u8 { ThreeD, TwoD };
RenderMode mode_{RenderMode::ThreeD};
bool key_m_pressed_{false};

WFCTileRegistry* registry_{nullptr};
TileAdjacencyTable* adjacency_{nullptr};
```

**New methods:**
- `CycleMode()` — destroys entities, flips mode, re-runs solver, re-emits, re-spawns. Camera snaps to mode-appropriate position.
- `SetupWFCCatalog()` — extracted from RunSolverAndEmit. Populates registry + adjacency once, captures mesh slots. Called from Initialize.
- `RunSolverForCurrentMode()` — extracted from RunSolverAndEmit. Resizes grid, runs solver, emits point set. Called from Initialize + CycleMode.
- `SpawnEntitiesForCurrentMode()` — extracted from SpawnWFCEntities. Called from Initialize + CycleMode.

**Input handling (in Run):**
```cpp
bool m_val;
get(input_source::keyboard, input_code::key_m, m_val);
if (m_val) {
    if (!key_m_pressed_) { key_m_pressed_ = true; CycleMode(); }
} else { key_m_pressed_ = false; }
```

Mirrors TestPCGScatter's `key_r → ReScatterPCG()` pattern (rising-edge detection).

**Mode parameters:**
| Mode | grid_size | face_count | Camera pos | Camera target |
|---|---|---|---|---|
| ThreeD | (4, 4, 4) | 6 | (8, 8, 8) | (0, 0, 0) |
| TwoD | (4, 4, 1) | 4 | (2, 2, 6) | (0, 0, 0) |

**CI smoke:** Compile define `WFC_MODE_2D_SMOKE=1` → Initialize with mode_=TwoD. Renders 60 frames, exits 0. Catches 2D regressions without interactive input.

**Seed:** Same seed (7) for both modes. Fair A/B comparison — only grid shape differs, not the random sequence.

## Data Flow

### Initialize (one-time setup)
1. Create window + MetalDevice + RenderSystem + StandardRenderPipeline + Scene + RenderView
2. SetLumenConfig(Low) + SetEditorMode(true) (Phase A.4 footgun fix)
3. SetupWFCCatalog() — populate registry + adjacency + register 5 meshes + capture slot indices + override mesh_handles
4. RunSolverForCurrentMode() — resize grid (4×4×4 default) + run solver + emit point set
5. SpawnEntitiesForCurrentMode() — CreateEntities + SetPCGEntities
6. Camera at (8, 8, 8) looking at origin

### Per-frame Run
1. Process window messages + keyboard input
2. Check 'M' key (rising edge) → CycleMode()
3. BeginFrame + Render + EndFrame
4. Increment frame_count, exit at kHeadlessFrameCap

### CycleMode (interactive)
1. Destroy current entities (`PCGEntityFactory::DestroyEntities(wfc_entity_ids)`)
2. Clear pipeline proxies (`pipeline->ClearPCGEntities()`)
3. Flip mode_ (ThreeD ↔ TwoD)
4. Resize grid to new dimensions
5. RunSolverForCurrentMode() (re-init solver + run + emit)
6. SpawnEntitiesForCurrentMode() (CreateEntities + SetPCGEntities)
7. Snap camera to mode-appropriate position
8. Stdout: `[TestWFCRendering] Mode cycled to 2D (4×4×1)` or `... to 3D (4×4×4)`

### Shutdown
1. Destroy entities + clear proxies (existing)
2. Delete registry_ + adjacency_ (NEW — owned by test case)
3. RenderSystem.Shutdown + pipeline->Shutdown (existing)
4. Idempotent guard preserved

## Testing

### New unit tests

| Binary | New Cases | Total | Verifies |
|---|---|---|---|
| **TestWFCCornerMeshes** (new) | 4 | 4 | corner_in/out vertex+index counts, CCW winding (cross-product), registration returns valid id |
| TestWFCOutput | +1 | 5 | Flag-driven: corner_in variant 2 → RotationY=π; pillar → RotationY=0 |
| TestWFCPropagator | +2 | 9 | face_count=4 skips ±Z entries; contradiction detected on ±X/±Y |
| TestWFCTileCatalog | +1 | 7 | corner_in/out variant_count == 4 (was 1) |

### Integration tests

`TestWFCRendering` — 0 new assertions, but adds `WFC_MODE_2D_SMOKE` CI define. Initialize in 2D mode, render 60 frames, exit 0.

### Regression sweep

- All 15 existing WFC unit binaries still pass (67 cases total).
- TestWFC3DParametric still passes (catalog variant_count change may require assertion adjustment — document in plan).
- TestWFCRendering 3D mode still passes (default mode unchanged).

## Edge Cases + Invariants

1. **CycleMode during solver run** — Solver runs synchronously to completion inside CycleMode. No concurrent state.
2. **Catalog candidate overflow** — 14 candidates << 64-bit cap. No `utl::vector<u64>` needed.
3. **2D adjacency rules** — Catalog uses `AddFullCompat` (sets all 6 faces). In 2D mode, ±Z compat is dead data. No catalog change.
4. **Camera in 2D** — Looking down -Z at XY plane. Tiles at z=0. Camera (2,2,6) sees the whole 4×4 grid.
5. **Corner geometry orientation** — Initial variant (0) faces +X. RotationY 0/π/2/π/3π/2 produces 4 orientations. Solver random collapse decides placement.
6. **Winding order** — Both new generators use CCW from day one (cross-product verified before commit, like Task 1's fix).

## Known Limitations (carried to Phase B.2)

- Corner mesh normals axial (no computed cross-product normals — same as ramp)
- 2D mode reuses 3D meshes (cube/ramp/etc arranged flat). True 2D-only flat tiles deferred.
- No HUD showing current mode (stdout log only)
- Camera snaps (no lerp animation)
- Single seed across modes (no per-mode seed config)
- Cross-variant corner adjacency not implemented (only self-compat)
- Pre-existing `MetalShader Failed to create library from binary` warning (~62× on startup, unrelated to WFC)

## Commit Strategy

7 commits expected:

1. `feat(content): create_corner_in_mesh + create_corner_out_mesh procedural generators`
2. `feat(wfc): flag-driven RotationY (replace tile_id==1 hardcoded check)`
3. `feat(wfc): WFCPropagator face_count parameter for 2D/3D mode`
4. `feat(wfc): catalog corner variant_count = 4 + flag metadata`
5. `test(wfc): TestWFCCornerMeshes binary + TestWFCOutput flag-driven cases`
6. `feat(wfc): TestWFCRendering CycleMode + 2D mode + WFC_MODE_2D_SMOKE`
7. `docs(wfc): Phase B.1 completion checkpoint`

## Estimated Effort

- Spec writing: done (this doc)
- Plan writing: ~1 hour
- Implementation: 2-3 days (7 tasks × ~1-2 hours each + review cycles)
- Test verification + completion notes: 0.5 day

Total: ~3-4 days.

## Open Questions

None. All design decisions confirmed during brainstorming:
- Scope: items 1+2+3 (all three)
- Corner variants: 4 each (matches ramp)
- 2D implementation: parametrize Propagator with face_count
- 2D demo: extend TestWFCRendering with interactive Mode toggle
- Mode selection: runtime parameter via keyboard 'M' (TestPCGScatter key_r pattern)
