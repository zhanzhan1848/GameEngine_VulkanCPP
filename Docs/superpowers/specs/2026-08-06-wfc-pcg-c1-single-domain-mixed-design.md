# WFC Phase C.1 — Single-Domain Mixed WFC (Parametric) Design

**Status:** Brainstormed 2026-08-06, pending implementation plan
**Spec author:** pair (user + assistant)
**Predecessors:** [2026-07-25-wfc-pcg-3d-map-generator-design.md](./2026-07-25-wfc-pcg-3d-map-generator-design.md) (parent spec), Phase A1–B2 completion notes

---

## 1. Goal

Ship a research-grade **multi-set parametric single-domain WFC** that mixes tile sets from three independent parametric sources in one solver, with automatic socket classification and restart-on-failure convergence.

Validates the hardest research risk identified in the parent spec — *single-domain mixed WFC may not converge for some tile combinations* — before C.2 adds organic (SDF-derived) tiles that compound the problem.

**Out of scope (deferred):**
- Organic / SDF-derived tiles → C.2
- Auto-socket classifier v2 (improved accuracy) → C.2
- 32³ performance tuning + JobSystem parallelization → C.3
- Dawn/WebGPU validation → C.3 (parametric tiles need no new compute shaders; existing renderer paths suffice)

---

## 2. Background

### 2.1 What's already in the engine

The WFC module (`Engine/Graphics/WFC/`) already has the bones of a single-domain solver:

| Module | Status | Notes |
|---|---|---|
| `WFCSolver` | ✅ shipping | Streaming-capable; used by `TestKenneyTilePreview` |
| `WaveGrid` | ✅ shipping | Cell bitset + entropy tracking |
| `WFCPropagator` | ✅ shipping | Arc-consistency propagation |
| `WFCObserver` | ✅ shipping | `MinEntropy` + `DistanceFromOrigin` strategies |
| `WFCTileRegistry` | ✅ shipping | Per-tile variant metadata |
| `TileAdjacencyTable` | ✅ shipping | Hand-populated socket compatibility table |
| `WFCCategory` | ✅ shipping | 5 categories (Primitive/Ruins/Dungeon/Cyber/Organic) with u64 mask filtering |
| `RestartPolicy` | ✅ shipping | Basic restart-on-deadlock |
| `WFCSolveBudget` | ✅ shipping | Per-frame collapse quota for streaming |
| `WFCTileCatalog` | ✅ shipping | Ruins tile set baked in code (~15 tiles) |
| `KenneyTileCatalog` | ✅ shipping | Loads Kenney dungeon tiles from disk |

### 2.2 What's missing for C.1

1. **Auto-socket classifier** — currently `TileAdjacencyTable` is hand-authored per tile set; mixing sets requires a unified socket language.
2. **Conflict seed memory** — current `RestartPolicy` re-randomizes; doesn't bias observer away from previously-failed cells/tiles.
3. **Procedural tile generator** — needed to stress-test classifier on tiles not in any fixed asset pack.
4. **Multi-set registry composition** — current demos load one catalog; need clean union of 3 sources.
5. **TestWFCSingleDomain binary** — existing demos solve single-set; need a demo that exercises the mixed path.

### 2.3 Research risk

The parent spec identifies the #1 risk as: *single-domain WFC may not converge for some tile combinations*. Mixing parametric tile sets with different socket conventions (Kenney's hand-authored, ruins' separate hand-authored, procedural generator's fresh conventions) is a meaningful convergence stress test even without organic tiles.

**Mitigation strategy**: restart-on-failure with conflict seed memory + degrade-to-single-set fallback after budget exhaustion (Section 7).

---

## 3. Architecture

```
       ┌─────────────────┐  ┌─────────────────┐  ┌────────────────────┐
       │ KenneyTile      │  │ RuinsTile       │  │ ProceduralRoomPack │
       │ Catalog         │  │ Catalog         │  │ (runtime generator)│
       │ (Dungeon cat.)  │  │ (Ruins cat.)    │  │ (Primitive cat.)   │
       └────────┬────────┘  └────────┬────────┘  └─────────┬──────────┘
                │                    │                     │
                └────────────────────┼─────────────────────┘
                                     ▼
                       ┌──────────────────────────┐
                       │  WFCTileRegistry          │  (extended: multi-set
                       │  (unified, multi-source)  │   union + category tag)
                       └─────────────┬────────────┘
                                     ▼
                       ┌──────────────────────────┐
                       │  AutoSocketClassifier    │  NEW
                       │  (8×8 occupancy grid     │  Reads each tile's mesh,
                       │   per face → u64 socket) │  computes SocketEncoding
                       └─────────────┬────────────┘
                                     ▼
                       ┌──────────────────────────┐
                       │  TileAdjacencyTable      │  (existing data shape,
                       │  (auto-populated)        │   populated by classifier)
                       └─────────────┬────────────┘
                                     ▼
           ┌───────────────────────────────────────────────────┐
           │  WFCSolver + WFCPropagator + WFCObserver          │  (existing)
           │  + ConflictSeedMemory (extends RestartPolicy)     │  NEW (policy ext)
           └────────────────────────┬──────────────────────────┘
                                    ▼
                       ┌──────────────────────────┐
                       │  TestWFCSingleDomain     │  NEW (GUI binary)
                       │  (panel: category mask,  │
                       │   classifier mode, etc.) │
                       └──────────────────────────┘
```

**Data flow**: three tile catalogs → unified registry (one tile per slot, tagged with WFCCategory) → AutoSocketClassifier walks each tile's mesh and computes a `SocketEncoding` per (tile, variant, face) → TileAdjacencyTable is auto-filled from those encodings → WFCSolver runs as in existing demos but with the unified table. TestWFCSingleDomain exercises the full path with interactive controls.

---

## 4. Component Specifications

### 4.1 `AutoSocketClassifier` (NEW)

**File**: `Engine/Graphics/WFC/AutoSocketClassifier.h` + `.cpp`

**Responsibility**: Compute a `SocketEncoding` (u64) per (tile variant, face direction) by sampling the tile's mesh geometry.

**Algorithm — 8×8 occupancy grid**:

```cpp
// For each face direction d in {+X, -X, +Y, -Y, +Z, -Z}:
//   Define an 8×8 sample grid on that face's unit square
//   (face is 1m × 1m, samples at (i+0.5)/8, (j+0.5)/8 for i,j in [0,8))
//   For each of 64 sample points:
//     Cast a short ray (length 0.05m) inward from the face surface
//     Solid = ray hits mesh triangle; Opening = ray misses
//   Pack 64 bits → SocketEncoding (MSB = (0,0), LSB = (7,7))
```

**Symmetry handling**: opposing faces (e.g., tile A's +X face vs tile B's -X face) must mirror-flip the occupancy grid along the face's vertical axis before comparison. Encoded directly into the equality predicate.

**Robustness**:
- Ray-mesh intersection uses existing `Bvh` from `Engine/Graphics/Geometry/` (already used by DDGI bake)
- Sample ray origin is offset 0.005m outside the face to avoid self-intersection
- Mesh with no triangles on a face → all-Opening → "fully open" socket (doorway variant)
- Mesh with all triangles on a face → all-Solid → "fully sealed" socket (wall variant)

**Public API**:
```cpp
namespace primal::graphics::wfc {

class AutoSocketClassifier {
public:
    // Compute socket signature for one face of one tile variant.
    // tile_mesh: read from geometry_id (existing fetch path)
    // face: 0..5 mapping to +X,-X,+Y,-Y,+Z,-Z
    // variant_transform: rotation applied to tile before classification
    //   (handles rotational variants without re-authoring mesh)
    static SocketEncoding ClassifyFace(
        geometry_id mesh, u32 variant_index,
        wfc::WFCFaceDirection face,
        const math::mat4& variant_transform);

    // Convenience: classify all 6 faces for a tile variant.
    struct FaceSignatures { SocketEncoding face[6]; };
    static FaceSignatures ClassifyTile(
        geometry_id mesh, u32 variant_index,
        const math::mat4& variant_transform);
};

} // namespace
```

**Variant handling**: tiles with rotational variants pass the variant's rotation matrix; classifier applies it to mesh before sampling. This means one mesh can produce N variant signatures without re-authoring geometry.

### 4.2 `ProceduralRoomPack` (NEW)

**File**: `Engine/Graphics/WFC/ProceduralRoomPack.h` + `.cpp`

**Responsibility**: Generate 12 room tiles at runtime (no offline asset authoring).

**Tile matrix**:
- 3 sizes: 3×3, 5×5, 7×7 (floor plan in cells, height fixed at 4m for ceiling clearance)
- 4 doorway configurations:
  - `Door_N` (1 door on north wall)
  - `Door_NS` (2 doors: north + south, aligned)
  - `Door_EW` (2 doors: east + west, aligned)
  - `Door_4` (4 doors, one per cardinal direction, all aligned to center)

Total: 3 × 4 = **12 procedural tiles**.

**Mesh generation**:
- Floor: solid quad at y=0 covering the room footprint
- Ceiling: solid quad at y=4 (inverted normal)
- Walls: 4 walls around perimeter, with doorway openings cut where doors are configured
- Doorway opening shape: rectangular, 2m wide × 3m tall, centered on wall
- Material: single material per tile, simple stone-like procedural pattern (reuse ruins material palette)

**Generator API**:
```cpp
namespace primal::graphics::wfc {

class ProceduralRoomPack {
public:
    struct RoomTileDef {
        const char* name;            // e.g., "proc_room_3x3_door_ns"
        u32 footprint_cells;          // 3, 5, or 7
        u32 door_mask;                // bit 0=N, 1=S, 2=E, 3=W
        WFCCategory category;         // always WFCCategory::Primitive
    };

    static constexpr u32 kTileCount = 12;
    static const RoomTileDef (&TileDefs())[kTileCount];

    // Generate mesh for one tile. Returns geometry_id registered via existing
    // content::create_resource path. Caller owns lifetime (standard contract).
    static geometry_id GenerateTileMesh(u32 tile_index);
};

} // namespace
```

**Why runtime generation**: avoids baking .engine_mesh files; lets panel UI regenerate tiles with seed variation if we later want procedural patterns.

### 4.3 `ConflictSeedMemory` (extends `RestartPolicy`)

**File**: extend `Engine/Graphics/WFC/RestartPolicy.h` + `.cpp`

**Responsibility**: track which (cell_coord, tile_id) pairs triggered conflicts in previous restarts; bias observer to delay collapsing those cells / picking those tiles.

**Memory shape**:
```cpp
struct ConflictRecord {
    WFCGridCoord cell;          // location of conflict
    wfc_tile_id  tile;          // tile involved in conflict
    u32          occurrence_count;
};

// Per-solver-owned instance, persists across restarts within one solve.
utl::vector<ConflictRecord> conflict_history_;
```

**Bias application**: when observer picks the next cell to collapse, add a penalty weight to cells in `conflict_history_` proportional to `occurrence_count`. Effective entropy = base_entropy + penalty. Penalty decays by 50% each restart (so old conflicts don't permanently block cells).

**When tiles are picked during collapse**: subtract a small probability mass from tiles that appear frequently in `conflict_history_` for the candidate cell.

**Recording**: propagator adds a record when it detects an arc-consistency violation involving a specific (cell, tile).

### 4.4 Multi-set `WFCTileRegistry` extension

**File**: extend `Engine/Graphics/WFC/WFCTileRegistry.h` + `.cpp`

**Change**: existing registry assumes one catalog. Add a `RegisterFromCatalog(catalog, category_tag)` overload that appends tiles with explicit category. The registry's internal tile list grows; each tile's `category` field (already exists in `WFCTile` per parent spec section 5.1) gets set from the parameter.

**No breaking changes**: existing single-catalog registration path is preserved.

### 4.5 `TileAdjacencyTable` auto-population path

**File**: extend `Engine/Graphics/WFC/TileAdjacency.h` + `.cpp`

**Change**: add a `BuildFromClassifier(registry, AutoSocketClassifier::ClassifyTile)` entry point that walks every tile variant in the registry and fills the compatibility table by comparing face signatures. Existing hand-authored builder is preserved (used when classifier mode = manual in panel).

**Compatibility predicate**:
```cpp
bool SocketsCompatible(SocketEncoding a, SocketEncoding b, WFCFaceDirection face) {
    // For opposing faces: mirror-flip b's grid before compare
    if (IsPositiveAxisFace(face)) b = MirrorFlip(b);
    return a == b;
}
```

### 4.6 `TestWFCSingleDomain` (NEW GUI binary)

**Files**:
- `EngineTest/IntegrationTests/Graphics/WFC/TestWFCSingleDomain.h` + `.cpp`
- WASM C ABI exports in `EngineTest/IntegrationTests/Graphics/WFC/TestWFCSingleDomainMain.cpp` (mirror TestKenneyMain.cpp pattern)

**Panel controls** (WASM via postMessage; native via hotkeys):
- Category mask (checkboxes: Dungeon / Ruins / Primitive) — default all on
- Classifier mode (radio: Auto / Manual-handcrafted) — default Auto
- Observer strategy (dropdown: MinEntropy / DistanceFromOrigin)
- Restart budget (slider: 1–16) — default 8
- Grid size (sliders: W/H/D, range 4–32) — default 16/4/16
- Reseed buttons (same seed / new seed)

**Smoke assert on solver completion**:
- `≥ 100 cells collapsed` AND
- `≥ 3 distinct tile_ids in final grid` AND
- `tile_ids span ≥ 2 distinct WFCCategory values`

Failure logs to stderr (no crash — GUI binary convention from TestKenneyTilePreview).

**Native + WASM builds**: both supported. WASM panel routes via EMSCRIPTEN_KEEPALIVE exports; CMakeLists adds `TestWFCSingleDomainWASM` target following the existing TestKenneyTilePreviewWASM pattern.

---

## 5. Data Flow — Detailed

### 5.1 Registry build (one-time at solver init)

```
TestWFCSingleDomain::InitWFC()
  ├── KenneyTileCatalog::Load(assets/Processed/kenney_dungeon_tiles/)
  ├── registry_->RegisterFromCatalog(kenney_catalog, WFCCategory::Dungeon)
  ├── registry_->RegisterFromCatalog(RuinsTileCatalog, WFCCategory::Ruins)
  ├── for i in [0, ProceduralRoomPack::kTileCount):
  │     mesh = ProceduralRoomPack::GenerateTileMesh(i)
  │     registry_->RegisterTile({
  │         .name = ProceduralRoomPack::TileDefs()[i].name,
  │         .mesh_handle = mesh,
  │         .category = WFCCategory::Primitive,
  │         ...
  │     })
  └── adjacency_->BuildFromClassifier(*registry_, AutoSocketClassifier::ClassifyTile)
```

### 5.2 Per-frame solver pump

Same streaming pattern as TestKenneyTilePreview: budget collapses N cells per frame, drains WFCStepBuffer into ECS entity spawns, registers meshes with StandardRenderPipeline. No new infrastructure.

### 5.3 Restart path

When propagator detects unrecoverable contradiction:
1. `ConflictSeedMemory::RecordConflict(cell, tile)` for each cell involved
2. `grid_->Reset()` (clears all cells to full candidate mask)
3. `solver_->Restart(new_seed)`
4. `ConflictSeedMemory::ApplyBias(observer)` — observer now sees biased entropy
5. Restart counter incremented; if ≥ budget, trigger fallback (Section 7)

---

## 6. AutoSocketClassifier — Implementation Details

### 6.1 Face direction convention

Matches existing engine convention (see `WFCFaceCorners.h`):
```
+WFCFaceDirection: PositiveX=0, NegativeX=1, PositiveY=2, NegativeY=3, PositiveZ=4, NegativeZ=5
```

### 6.2 Sample grid orientation

For each face, define local (u, v) axes on the face plane:
- +X face: u=+Z, v=+Y (looking from +X toward origin)
- -X face: u=-Z, v=+Y (mirror)
- +Y face: u=+X, v=+Z (looking down from above)
- -Y face: u=+X, v=-Z (mirror)
- +Z face: u=-X, v=+Y (looking from +Z)
- -Z face: u=+X, v=+Y (mirror)

This mirror convention ensures that when comparing tile A's +X face to tile B's -X face (which physically mate), the (u, v) coordinates are aligned.

### 6.3 Ray cast

```cpp
// Pseudo-code:
math::v3 ray_origin = face_center + face_normal * 0.005f
                    + u_axis * ((i + 0.5f) / 8.0f - 0.5f)
                    + v_axis * ((j + 0.5f) / 8.0f - 0.5f);
math::v3 ray_dir = -face_normal;  // pointing inward
float t_hit;
bool solid = bvh_->Intersect(ray_origin, ray_dir, /*max_t=*/0.05f, &t_hit);
bit = solid ? 1 : 0;
```

### 6.4 Bit packing

```cpp
SocketEncoding encoding = 0;
for (u32 j = 0; j < 8; ++j)
    for (u32 i = 0; i < 8; ++i)
        encoding |= (u64)sample_bit[i + j*8] << (i + j*8);
```

### 6.5 Mirror flip for opposing faces

When tile A's +X face mates with tile B's -X face, the physical (u, v) axes are mirrored. To compare directly, flip B's encoding:
```cpp
SocketEncoding MirrorFlipU(SocketEncoding e) {
    // Reverse bit order within each 8-bit row
    SocketEncoding out = 0;
    for (u32 row = 0; row < 8; ++row) {
        u8 bits = (e >> (row * 8)) & 0xFF;
        u8 reversed = ReverseBits(bits);  // 0b10110011 → 0b11001101
        out |= (u64)reversed << (row * 8);
    }
    return out;
}
```

---

## 7. Convergence Strategy

### 7.1 Restart-on-failure with conflict seed memory

```
on_propagation_contradiction():
    for each cell C in contradiction_set:
        ConflictSeedMemory::Record(C.coord, C.attempted_tile)
    grid_->Reset()
    solver_->Restart(new_random_seed)
    ConflictSeedMemory::ApplyBias(observer)  // modifies entropy weighting
    ++restart_count_

on_restart_threshold_exceeded():
    DegradeToSingleSet()  // Section 7.2
```

### 7.2 Degrade-to-single-set fallback

When `restart_count_ >= restart_budget_` (default 8):
1. Clear registry, keep only `WFCCategory::Dungeon` tiles (most-curated set, highest convergence odds)
2. Rebuild `TileAdjacencyTable` for the reduced registry (still via classifier)
3. Reset grid + solver, fresh seed
4. Solve to completion with no further restart limit (single-set is virtually guaranteed to converge)

This guarantees the binary never reports failure on default settings — it produces *some* output.

### 7.3 Conflict decay

Each restart, all `ConflictRecord::occurrence_count` values are halved (integer division). This ensures bias doesn't permanently block cells that may have only been problematic due to a transient combination.

---

## 8. Test Plan

### 8.1 Smoke test (TestWFCSingleDomain)

Asserted on solver completion (default settings, all 3 categories on):
- ≥ 100 cells collapsed
- ≥ 3 distinct tile_ids in final grid
- ≥ 2 distinct WFCCategory values among collapsed tiles

### 8.2 Convergence rate benchmark (panel mode)

Panel exposes a "Benchmark" button that runs 32 seeds, logs:
- Convergence rate (how many reached full collapse without degrade)
- Average restart count
- How often degrade-to-single-set triggered
- Category distribution in successful solves

This is a research-grade diagnostic — the user uses it to characterize the mixed solver.

### 8.3 Auto-socket classifier unit tests (in `EngineTest/UnitTests/Graphics/WFC/`)

- Empty mesh → all-Opening socket
- Solid cube mesh → all-Solid socket
- Cube with doorway opening → expected bit pattern
- Mirror-flip correctness: A's +X signature matches B's -X signature after MirrorFlipU
- Variant transform: same tile rotated 90° produces different signatures on different faces

### 8.4 ProceduralRoomPack unit tests

- All 12 tiles generate valid meshes (vertex count > 0, no NaN bounds)
- Each tile's doorway mask matches what occupancy grid detects (e.g., `Door_NS` tile has openings on +Z and -Z faces)

---

## 9. Risks & Mitigations

| Risk | Likelihood | Mitigation |
|---|---|---|
| Auto-socket classifier produces too many distinct sockets (no adjacencies possible) | Med | Coarse-grained fallback: bucket signatures by Hamming distance, treat near-identical signatures as compatible |
| Mixed solve never converges even with seed memory | Med | Degrade-to-single-set fallback (Section 7.2) |
| 32³ stress mode too slow at 16³ default | Low | 16³ default for development; 32³ optional |
| Auto-socket classifier too slow at registry build | Low | One-time cost per solve; ~12 tiles × 6 faces × 64 rays = 4608 ray casts — well under 100ms |
| ProceduralRoomPack generates broken mesh (NaN verts, holes) | Low | Unit test asserts mesh validity; generation is deterministic |
| Conflict seed memory biases observer into corner | Low | Decay halving per restart prevents permanent bias |

---

## 10. Open Questions (to resolve during planning)

1. **BVH reuse**: does the existing `Bvh` from DDGI bake path expose a public Intersect API usable from `AutoSocketClassifier`? Need to check.
2. **ProceduralRoomPack material**: reuse ruins material palette or define a new material? Leaning toward reuse.
3. **Panel UI**: native hotkey parity for category mask toggles (currently panel is WASM-only via postMessage).
4. **Variant expansion**: do procedural tiles have rotational variants? Default no (4 doorway configs already cover rotations).

---

## 11. Phase Exit Criteria

C.1 v1 is **done** when:

- [ ] `AutoSocketClassifier` ships with 8×8 occupancy grid algorithm
- [ ] `ProceduralRoomPack` generates 12 valid tiles
- [ ] `ConflictSeedMemory` integrated into `RestartPolicy`
- [ ] `WFCTileRegistry` extended for multi-set composition
- [ ] `TileAdjacencyTable::BuildFromClassifier` populated without hand-authored sockets
- [ ] `TestWFCSingleDomain` GUI binary builds native + WASM
- [ ] Smoke test passes on default settings (all 3 categories on, 16³ grid, restart budget 8)
- [ ] Benchmark mode reports convergence rate > 50% on default settings
- [ ] Degrade-to-single-set fallback never leaves the grid unsolved

C.2 (organic / SDF tiles) starts only after C.1 v1 ships and the user reviews the convergence benchmark data.

---

## 12. Glossary

- **Single-domain**: one solver, one grid (vs Phase B's layered multi-grid approach)
- **Mixed**: multiple tile categories coexisting in the wave (vs single-set demos)
- **Socket**: a face's signature; two faces can mate iff sockets are compatible
- **Occupancy grid**: 8×8 bit pattern of solid/opening samples on a face
- **Conflict seed memory**: history of (cell, tile) pairs that caused contradictions; biases observer
- **Degrade-to-single-set**: fallback after restart budget exhausted; reduces registry to one category
