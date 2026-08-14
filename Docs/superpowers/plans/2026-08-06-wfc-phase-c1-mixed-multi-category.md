# WFC Phase C.1 — Mixed Multi-Category Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship multi-source single-domain WFC that mixes Kenney Dungeon + Ruins + new Procedural Room Pack tiles in one solver, with restart-on-failure + conflict-seed memory + degrade-to-single-set fallback. Validates the hardest research risk (mixed convergence) before C.2 adds organic tiles.

**Architecture:** Layered on top of the already-shipped Phase C.1 Ruins work (WFCCategory, 16×4 packing, 8-bit corner socket encoder, 15 ruins tiles). Adds: (a) widened `candidate_mask` from u64 → u64[4] (256 candidates = 64 tile cap), (b) new `AutoSocketClassifier` with 8×8 occupancy grid for doorway-aware tiles, (c) `ProceduralRoomPack` runtime generator (12 tiles), (d) `ConflictSeedMemory` extension to `RestartPolicy` with decay + degrade-to-single-set fallback, (e) `TestWFCSingleDomain` GUI binary (native + WASM).

**Tech Stack:** C++17, CMake, primal `TestFramework` (`TestResult` + `TEST_ASSERT_*`), existing `Engine/Graphics/WFC/*` modules, Möller–Trumbore ray-triangle (build-time precompute, no BVH needed at <1k triangles/tile).

**Spec reference:** `Docs/superpowers/specs/2026-08-06-wfc-pcg-c1-single-domain-mixed-design.md`

**Predecessor:** `Docs/superpowers/plans/2026-07-31-wfc-phase-c1-ruins.md` (shipped — provides WFCCategory, ruins tiles, 8-bit corner signatures)

---

## File Structure

### New files

| Path | Responsibility |
|------|----------------|
| `Engine/Graphics/WFC/AutoSocketClassifier.h` | Public API: `ClassifyFace`, `ClassifyTile`, `MirrorFlipU` |
| `Engine/Graphics/WFC/AutoSocketClassifier.cpp` | 8×8 occupancy grid impl + Möller–Trumbore ray-triangle |
| `Engine/Graphics/WFC/ProceduralRoomPack.h` | `RoomTileDef` table + `GenerateTileMesh` declaration |
| `Engine/Graphics/WFC/ProceduralRoomPack.cpp` | 12-tile runtime mesh generator |
| `EngineTest/UnitTests/Graphics/WFC/TestAutoSocketClassifier.cpp` | Occupancy grid tests (5 cases) |
| `EngineTest/UnitTests/Graphics/WFC/TestProceduralRoomPack.cpp` | Tile generator tests (4 cases) |
| `EngineTest/UnitTests/Graphics/WFC/TestConflictSeedMemory.cpp` | Bias + decay tests (4 cases) |
| `EngineTest/IntegrationTests/Graphics/WFC/TestWFCSingleDomain.h` | GUI test case decl |
| `EngineTest/IntegrationTests/Graphics/WFC/TestWFCSingleDomain.cpp` | GUI binary: panel hooks + smoke assert |
| `EngineTest/IntegrationTests/Graphics/WFC/TestWFCSingleDomainMain.cpp` | WASM C ABI exports |
| `wfc/TestWFCSingleDomain.html` | WASM shell (mirror existing TestKenneyTilePreviewWASM.html) |

### Modified files

| Path | Reason |
|------|--------|
| `Engine/Graphics/WFC/WFCTypes.h` | `WFCCell::candidate_mask` u64 → u64[4]; bump `MaxTiles` 16 → 64 |
| `Engine/Graphics/WFC/WFCTileRegistry.h` | `MaxTiles` 16 → 64; packing helpers updated |
| `Engine/Graphics/WFC/WFCPropagator.cpp` | 2 sites: read/write mask as array ops |
| `Engine/Graphics/WFC/WFCSolver.cpp` | 3 sites: init/pick/set use array ops; entropy loop over 4 elements |
| `Engine/Graphics/WFC/WaveGrid.cpp` (if mask referenced) | Same array migration |
| `Engine/Graphics/WFC/RestartPolicy.h` | Add `ConflictSeedMemory` member + `RecordConflict` + `ApplyBias` + `Decay` |
| `Engine/Graphics/WFC/RestartPolicy.cpp` | Bias weighting + decay halving + degrade trigger |
| `Engine/Graphics/WFC/WFCTileRegistry.h` | Add `RegisterFromCatalog(catalog, category)` overload |
| `Engine/Graphics/WFC/TileAdjacency.h` | Add `BuildFromClassifier(registry, classifier_fn)` entry point |
| `Engine/Graphics/WFC/TileAdjacency.cpp` | `BuildFromClassifier` impl |
| `EngineTest/UnitTests/Graphics/WFC/TestWFCPropagator.cpp` | Update assertions for new mask layout |
| `EngineTest/UnitTests/Graphics/WFC/TestWFCSolver.cpp` | Update assertions for new mask layout |
| `EngineTest/UnitTests/Graphics/WFC/TestWFCTileRegistry.cpp` | Add multi-set register test |
| `EngineTest/UnitTests/Graphics/WFC/TestWFCTileRegistryPacking.cpp` | Update for 64-tile layout |
| `EngineTest/UnitTests/CMakeLists.txt` | Register new unit test binaries |
| `EngineTest/IntegrationTests/CMakeLists.txt` | Register TestWFCSingleDomain + WASM target |
| `CMakeLists.txt` | Add AutoSocketClassifier + ProceduralRoomPack sources |

---

## Milestone M1 — Widen candidate_mask (refactor + regression)

**Goal:** Lift the 16-tile cap to 64 so all three tile sources fit. Touches 5 sites; existing 80 WFC unit tests + 8 integration binaries are the safety net.

### Task 1: Define new mask layout constants

**Files:**
- Modify: `Engine/Graphics/WFC/WFCTypes.h`
- Modify: `Engine/Graphics/WFC/WFCTileRegistry.h`

- [ ] **Step 1: Write the failing test**

Create `EngineTest/UnitTests/Graphics/WFC/TestWFCCandidateLayout.cpp`:

```cpp
#include "../../TestFramework.h"
#include "Engine/Graphics/WFC/WFCTypes.h"
#include "Engine/Graphics/WFC/WFCTileRegistry.h"

using namespace primal::graphics::wfc;
using namespace Engine::Test;

TestResult TestMaskArraySize() {
    WFCCell c{};
    static_assert(sizeof(c.candidate_mask) == sizeof(u64[4]),
                  "candidate_mask must be u64[4] (256 bits)");
    return TestResult::Passed;
}

TestResult TestMaxTilesBumped() {
    TEST_ASSERT_GEQ(WFCTileRegistry::MaxTiles, 64u, "MaxTiles must be >= 64");
    TEST_ASSERT_EQ(WFCTileRegistry::MaxTiles * WFCTileRegistry::MaxVariantsPerTile,
                   256u, "tile*variant must equal 256 (mask capacity)");
    return TestResult::Passed;
}

TestResult TestBitForTileVariantInRange() {
    // Tile 63, variant 3 → bit 255 (top of last u64)
    u32 bit = WFCTileRegistry::BitForTileVariant(wfc_tile_id{63}, 3);
    TEST_ASSERT_EQ(255u, bit, "tile 63 var 3 = bit 255");
    // Word index = 255 / 64 = 3, bit-within-word = 255 % 64 = 63
    return TestResult::Passed;
}

int main() {
    TestSuite suite;
    suite.AddTestCase(TestMaskArraySize,        "MaskArraySize");
    suite.AddTestCase(TestMaxTilesBumped,       "MaxTilesBumped");
    suite.AddTestCase(TestBitForTileVariantInRange, "BitForTileVariantInRange");
    return suite.Run();
}
```

- [ ] **Step 2: Add to CMakeLists**

In `EngineTest/UnitTests/CMakeLists.txt`, add `TestWFCCandidateLayout` to the unit test list following the existing pattern (e.g., copy the `TestWFCCategory` entry).

- [ ] **Step 3: Run test to verify it fails**

```bash
cmake --build build --target TestWFCCandidateLayout -j 4 && ./build/bin/TestWFCCandidateLayout
```

Expected: FAIL — `candidate_mask` is still u64; `MaxTiles` is 16; `BitForTileVariant` returns out-of-range bit.

- [ ] **Step 4: Modify WFCTypes.h**

In `Engine/Graphics/WFC/WFCTypes.h`, locate the `WFCCell` struct (around line 60) and change:

```cpp
// OLD:
u64              candidate_mask;

// NEW:
static constexpr u32 kMaskWords = 4;  // 4 × u64 = 256 candidates (64 tiles × 4 variants)
u64              candidate_mask[kMaskWords];
```

- [ ] **Step 5: Modify WFCTileRegistry.h**

In `Engine/Graphics/WFC/WFCTileRegistry.h`, change:

```cpp
// OLD:
static constexpr u32 MaxTiles = 16;

// NEW:
static constexpr u32 MaxTiles = 64;
```

Add helpers:

```cpp
// Word index for a given global bit
static u32 MaskWordForBit(u32 bit) { return bit / 64u; }
static u32 MaskBitInWord(u32 bit)  { return bit % 64u; }
```

- [ ] **Step 6: Run test to verify it passes**

```bash
cmake --build build --target TestWFCCandidateLayout -j 4 && ./build/bin/TestWFCCandidateLayout
```

Expected: PASS (3 cases).

- [ ] **Step 7: Commit**

```bash
git add Engine/Graphics/WFC/WFCTypes.h Engine/Graphics/WFC/WFCTileRegistry.h \
        EngineTest/UnitTests/Graphics/WFC/TestWFCCandidateLayout.cpp \
        EngineTest/UnitTests/CMakeLists.txt
git commit -m "feat(wfc): widen candidate_mask to u64[4] (64 tile cap)"
```

### Task 2: Migrate WFCPropagator mask reads/writes

**Files:**
- Modify: `Engine/Graphics/WFC/WFCPropagator.cpp:62, 114`

- [ ] **Step 1: Write the failing test**

Append to `EngineTest/UnitTests/Graphics/WFC/TestWFCPropagator.cpp`:

```cpp
TestResult TestPropagatorClearsAcrossWords() {
    // Set a bit in word 3 (tile ≥ 48) and verify propagation clears it correctly.
    WFCGridCoord coord{0, 0, 0};
    grid_.SetCandidateBit(coord, 200);  // bit 200, word 3
    propagator_.PropagateConstraint(coord, /*removed_tile=*/wfc_tile_id{50}, /*variant=*/0);
    TEST_ASSERT_FALSE(grid_.HasCandidateBit(coord, 200), "bit 200 must be cleared");
    return TestResult::Passed;
}
```

Add to the test suite's main().

- [ ] **Step 2: Run to verify fail**

```bash
cmake --build build --target TestWFCPropagator -j 4 && ./build/bin/TestWFCPropagator
```

Expected: FAIL — `candidate_mask` access doesn't compile (u64 vs u64[4]) or `SetCandidateBit` doesn't exist yet.

- [ ] **Step 3: Update WFCPropagator.cpp**

In `Engine/Graphics/WFC/WFCPropagator.cpp` at line ~62 (`u64 old_mask = cell.candidate_mask;`):

```cpp
// OLD:
u64 old_mask = cell.candidate_mask;
...
cell.candidate_mask = new_mask;

// NEW:
u64 old_mask[4];
for (u32 i = 0; i < 4; ++i) old_mask[i] = cell.candidate_mask[i];
// ... compute new_mask[4] by clearing bits per arc-consistency ...
for (u32 i = 0; i < 4; ++i) cell.candidate_mask[i] = new_mask[i];
```

The exact propagation logic depends on the existing implementation — read `WFCPropagator.cpp` lines 50–120 first and translate single-u64 ops to per-word loops. The semantic is: every AND/OR/ANDNOT on the old u64 becomes a loop over 4 elements.

- [ ] **Step 4: Run propagator tests**

```bash
cmake --build build --target TestWFCPropagator -j 4 && ./build/bin/TestWFCPropagator
```

Expected: PASS (all existing cases + new TestPropagatorClearsAcrossWords).

- [ ] **Step 5: Commit**

```bash
git add Engine/Graphics/WFC/WFCPropagator.cpp EngineTest/UnitTests/Graphics/WFC/TestWFCPropagator.cpp
git commit -m "fix(wfc): propagator operates on u64[4] candidate mask"
```

### Task 3: Migrate WFCSolver mask operations

**Files:**
- Modify: `Engine/Graphics/WFC/WFCSolver.cpp:85, 116, 136`

- [ ] **Step 1: Write the failing test**

Append to `EngineTest/UnitTests/Graphics/WFC/TestWFCSolver.cpp`:

```cpp
TestResult TestSolverHandlesTile63() {
    // Register 64 tiles (only possible after mask widening), verify PopulateAllCandidates
    // produces a u64[4] all-ones mask and PickCandidate picks valid bits across all words.
    WFCTileRegistry reg;
    for (u32 i = 0; i < 64; ++i) {
        WFCTile t{}; t.id = wfc_tile_id{i}; t.category = WFCCategory::Primitive;
        reg.Register(t);
    }
    WaveGrid grid;
    grid.Init(/*w=*/2, /*h=*/2, /*d=*/2, reg);
    WFCSolver s; s.Init(grid, reg, WFCConfig{});
    // All 4 words of every cell's mask must be non-zero
    for (u32 cell = 0; cell < grid.CellCount(); ++cell) {
        for (u32 w = 0; w < 4; ++w) {
            TEST_ASSERT_NE(0ull, grid.CellAt(cell).candidate_mask[w], "word must be non-zero");
        }
    }
    return TestResult::Passed;
}
```

- [ ] **Step 2: Run to verify fail**

```bash
cmake --build build --target TestWFCSolver -j 4 && ./build/bin/TestWFCSolver
```

Expected: FAIL — `full_mask` initialization at WFCSolver.cpp:85 still uses single u64; `PickCandidate` at line 116 doesn't handle multi-word.

- [ ] **Step 3: Update WFCSolver.cpp**

In `Engine/Graphics/WFC/WFCSolver.cpp`:

At line 85 (`c.candidate_mask = full_mask;`):
```cpp
// OLD: u64 full_mask = (1ULL << registry.Count() * MaxVariantsPerTile) - 1;
//      c.candidate_mask = full_mask;

// NEW:
u32 total_bits = registry.Count() * WFCTileRegistry::MaxVariantsPerTile;
for (u32 w = 0; w < WFCCell::kMaskWords; ++w) {
    u32 bits_in_word = (total_bits >= (w + 1) * 64) ? 64 : (total_bits - w * 64);
    bits_in_word = std::min(bits_in_word, 64u);
    c.candidate_mask[w] = (bits_in_word == 64) ? ~0ULL : ((1ULL << bits_in_word) - 1);
}
```

At line 116 (`u64 m = c.candidate_mask;` for picking Nth set bit):
```cpp
// OLD: walks bits of single u64
// NEW: walk words, then bits within word
u32 remaining = n;  // Nth set bit requested
for (u32 w = 0; w < WFCCell::kMaskWords; ++w) {
    u64 word = c.candidate_mask[w];
    u32 pop = std::popcount(word);
    if (remaining < pop) {
        // Nth set bit is in word w
        for (u32 b = 0; b < 64; ++b) {
            if (word & (1ULL << b)) {
                if (remaining == 0) {
                    chosen_bit = w * 64 + b;
                    goto found;
                }
                --remaining;
            }
        }
    }
    remaining -= pop;
}
found:;
```

At line 136 (`c.candidate_mask = (1ULL << chosen_bit);`):
```cpp
// OLD: c.candidate_mask = (1ULL << chosen_bit);
// NEW:
for (u32 w = 0; w < WFCCell::kMaskWords; ++w) c.candidate_mask[w] = 0;
u32 w = WFCTileRegistry::MaskWordForBit(chosen_bit);
u32 b = WFCTileRegistry::MaskBitInWord(chosen_bit);
c.candidate_mask[w] = (1ULL << b);
```

Also update the entropy calculation — popcount becomes sum of popcounts across all words:
```cpp
u32 entropy = 0;
for (u32 w = 0; w < WFCCell::kMaskWords; ++w) entropy += std::popcount(c.candidate_mask[w]);
```

- [ ] **Step 4: Run solver tests**

```bash
cmake --build build --target TestWFCSolver -j 4 && ./build/bin/TestWFCSolver
```

Expected: PASS (all cases including new TestSolverHandlesTile63).

- [ ] **Step 5: Run full WFC regression suite**

```bash
cmake --build build --target WFC_UnitTests -j 4 2>/dev/null || \
cmake --build build -j 4 -- -k TestWFC
ctest --test-dir build -R "TestWFC" --output-on-failure
```

Expected: ALL tests pass. If a previously-passing test breaks, the migration missed a site — grep for `candidate_mask` and audit.

- [ ] **Step 6: Commit**

```bash
git add Engine/Graphics/WFC/WFCSolver.cpp EngineTest/UnitTests/Graphics/WFC/TestWFCSolver.cpp
git commit -m "fix(wfc): solver popcount + pick across u64[4] mask"
```

### Task 4: Update WaveGrid mask helpers (if any)

**Files:**
- Modify: `Engine/Graphics/WFC/WaveGrid.h` + `WaveGrid.cpp` (only if mask helpers exist)

- [ ] **Step 1: Audit WaveGrid**

```bash
grep -n "candidate_mask\|HasCandidateBit\|SetCandidateBit\|ClearCandidateBit" Engine/Graphics/WFC/WaveGrid.*
```

If no helpers exist (mask only touched in Propagator/Solver), skip this task — commit nothing. Otherwise continue.

- [ ] **Step 2: Write the failing test**

Add `EngineTest/UnitTests/Graphics/WFC/TestWaveGridMaskBits.cpp`:

```cpp
#include "../../TestFramework.h"
#include "Engine/Graphics/WFC/WaveGrid.h"
#include "Engine/Graphics/WFC/WFCTileRegistry.h"

using namespace primal::graphics::wfc;
using namespace Engine::Test;

TestResult TestHasCandidateBitAcrossWords() {
    WaveGrid grid;
    WFCTileRegistry reg;
    for (u32 i = 0; i < 64; ++i) {
        WFCTile t{}; t.id = wfc_tile_id{i}; t.category = WFCCategory::Primitive;
        reg.Register(t);
    }
    grid.Init(2, 2, 2, reg);
    WFCGridCoord c{0, 0, 0};
    TEST_ASSERT_TRUE(grid.HasCandidateBit(c, 0),   "bit 0 present");
    TEST_ASSERT_TRUE(grid.HasCandidateBit(c, 100), "bit 100 present (word 1)");
    TEST_ASSERT_TRUE(grid.HasCandidateBit(c, 255), "bit 255 present (word 3)");
    return TestResult::Passed;
}

int main() {
    TestSuite suite;
    suite.AddTestCase(TestHasCandidateBitAcrossWords, "HasCandidateBitAcrossWords");
    return suite.Run();
}
```

- [ ] **Step 3: Run to verify fail**

```bash
cmake --build build --target TestWaveGridMaskBits -j 4 && ./build/bin/TestWaveGridMaskBits
```

Expected: FAIL — WaveGrid helpers (if they exist) still operate on single u64.

- [ ] **Step 4: Update WaveGrid helpers**

For each helper that takes or returns a mask:

```cpp
bool HasCandidateBit(WFCGridCoord c, u32 bit) const {
    auto& cell = CellAt(c);
    u32 w = WFCTileRegistry::MaskWordForBit(bit);
    u32 b = WFCTileRegistry::MaskBitInWord(bit);
    return (cell.candidate_mask[w] & (1ULL << b)) != 0;
}
```

- [ ] **Step 5: Run tests**

```bash
cmake --build build --target TestWaveGridMaskBits -j 4 && ./build/bin/TestWaveGridMaskBits
```

Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add Engine/Graphics/WFC/WaveGrid.* EngineTest/UnitTests/Graphics/WFC/TestWaveGridMaskBits.cpp \
        EngineTest/UnitTests/CMakeLists.txt
git commit -m "fix(wfc): WaveGrid helpers operate on u64[4] mask"
```

---

## Milestone M2 — AutoSocketClassifier (8×8 occupancy grid)

**Goal:** Build a doorway-aware socket classifier. Coexists with existing 8-bit corner encoder (TestWFCRuinsRendering keeps using corner; TestWFCSingleDomain will use occupancy grid).

### Task 5: Möller–Trumbore ray-triangle primitive

**Files:**
- Create: `Engine/Graphics/WFC/AutoSocketClassifier.h` (skeleton)
- Create: `Engine/Graphics/WFC/AutoSocketClassifier.cpp` (ray-triangle impl only)

- [ ] **Step 1: Write the failing test**

Create `EngineTest/UnitTests/Graphics/WFC/TestAutoSocketClassifier.cpp`:

```cpp
#include "../../TestFramework.h"
#include "Engine/Graphics/WFC/AutoSocketClassifier.h"
#include "Engine/Graphics/WFC/WFCTypes.h"

using namespace primal::graphics::wfc;
using namespace Engine::Test;

// Triangle in XY plane facing +Z, vertices CCW from +Z
// Test ray-triangle intersection primitive.
TestResult TestRayTriangleHit() {
    math::v3 v0{-1, -1, 0}, v1{1, -1, 0}, v2{0, 1, 0};
    math::v3 ray_origin{0, 0, 1}, ray_dir{0, 0, -1};
    f32 t = -1;
    bool hit = AutoSocketClassifier::RayTriangle(
        ray_origin, ray_dir, v0, v1, v2, /*max_t=*/0.1f, &t);
    TEST_ASSERT_TRUE(hit, "ray should hit triangle");
    TEST_ASSERT_GT(t, 0.0f, "t must be positive");
    TEST_ASSERT_LT(t, 0.1f, "t must be within max_t");
    return TestResult::Passed;
}

TestResult TestRayTriangleMiss() {
    math::v3 v0{-1, -1, 0}, v1{1, -1, 0}, v2{0, 1, 0};
    math::v3 ray_origin{5, 5, 1}, ray_dir{0, 0, -1};  // ray far off triangle
    f32 t = -1;
    bool hit = AutoSocketClassifier::RayTriangle(
        ray_origin, ray_dir, v0, v1, v2, /*max_t=*/0.1f, &t);
    TEST_ASSERT_FALSE(hit, "ray should miss");
    return TestResult::Passed;
}

TestResult TestRayTriangleBackfaceCull() {
    // Reverse winding (CW from +Z) — ray from +Z should miss (backface)
    math::v3 v0{0, 1, 0}, v1{1, -1, 0}, v2{-1, -1, 0};  // CW from +Z
    math::v3 ray_origin{0, 0, 1}, ray_dir{0, 0, -1};
    f32 t = -1;
    bool hit = AutoSocketClassifier::RayTriangle(
        ray_origin, ray_dir, v0, v1, v2, /*max_t=*/0.1f, &t);
    TEST_ASSERT_FALSE(hit, "backface should be culled");
    return TestResult::Passed;
}

int main() {
    TestSuite suite;
    suite.AddTestCase(TestRayTriangleHit,        "RayTriangleHit");
    suite.AddTestCase(TestRayTriangleMiss,       "RayTriangleMiss");
    suite.AddTestCase(TestRayTriangleBackfaceCull, "RayTriangleBackfaceCull");
    return suite.Run();
}
```

- [ ] **Step 2: Add to CMakeLists**

In `Engine/Graphics/CMakeLists.txt` (or wherever WFC sources are listed — check `grep -rn "WFCSocketOps.cpp" Engine/`):

Add `Graphics/WFC/AutoSocketClassifier.cpp` to the engine sources list.

In `EngineTest/UnitTests/CMakeLists.txt`, register `TestAutoSocketClassifier`.

- [ ] **Step 3: Run to verify fail**

```bash
cmake --build build --target TestAutoSocketClassifier -j 4 2>&1 | head -20
```

Expected: FAIL — `AutoSocketClassifier::RayTriangle` doesn't exist.

- [ ] **Step 4: Create AutoSocketClassifier.h**

```cpp
// Engine/Graphics/WFC/AutoSocketClassifier.h
#pragma once

#include "../../Common/CommonHeaders.h"
#include "../../Utilities/MathTypes.h"
#include "WFCTypes.h"

namespace primal::graphics::wfc {

class WFCTileRegistry;

// AutoSocketClassifier — doorway-aware socket signature encoder.
// Uses 8×8 occupancy grid per face: casts 64 rays into the tile mesh from
// outside the face, packs hit/miss into a u64 signature.
//
// Two faces are compatible iff their signatures match (with U-axis mirror
// flip for opposing faces). Doorway openings show as zero bits in the
// signature; solid walls show as one bits.
class AutoSocketClassifier {
public:
    // Möller–Trumbore ray-triangle intersection with backface cull.
    // Returns true on hit; fills *t with hit distance.
    // max_t caps the ray length (face sample rays are 0.05m).
    static bool RayTriangle(const math::v3& origin, const math::v3& dir,
                            const math::v3& v0, const math::v3& v1, const math::v3& v2,
                            f32 max_t, f32* t);

    // (Subsequent tasks add ClassifyFace, ClassifyTile, MirrorFlipU)
};

} // namespace primal::graphics::wfc
```

- [ ] **Step 5: Create AutoSocketClassifier.cpp with RayTriangle only**

```cpp
// Engine/Graphics/WFC/AutoSocketClassifier.cpp
#include "AutoSocketClassifier.h"

namespace primal::graphics::wfc {

bool AutoSocketClassifier::RayTriangle(
    const math::v3& origin, const math::v3& dir,
    const math::v3& v0, const math::v3& v1, const math::v3& v2,
    f32 max_t, f32* t) {
    constexpr f32 kEpsilon = 1e-6f;

    math::v3 edge1 = v1 - v0;
    math::v3 edge2 = v2 - v0;
    math::v3 h = cross(dir, edge2);
    f32 a = dot(edge1, h);

    // Backface cull: only positive a (front-facing triangles)
    if (a < kEpsilon) return false;

    math::v3 s = origin - v0;
    f32 u = dot(s, h);
    if (u < 0.0f || u > a) return false;

    math::v3 q = cross(s, edge1);
    f32 v = dot(dir, q);
    if (v < 0.0f || (u + v) > a) return false;

    f32 inv_a = 1.0f / a;
    f32 tt = dot(edge2, q) * inv_a;
    if (tt < kEpsilon || tt > max_t) return false;

    if (t) *t = tt;
    return true;
}

} // namespace
```

- [ ] **Step 6: Run test to verify pass**

```bash
cmake --build build --target TestAutoSocketClassifier -j 4 && ./build/bin/TestAutoSocketClassifier
```

Expected: PASS (3 cases).

- [ ] **Step 7: Commit**

```bash
git add Engine/Graphics/WFC/AutoSocketClassifier.h Engine/Graphics/WFC/AutoSocketClassifier.cpp \
        EngineTest/UnitTests/Graphics/WFC/TestAutoSocketClassifier.cpp \
        Engine/CMakeLists.txt EngineTest/UnitTests/CMakeLists.txt
git commit -m "feat(wfc): AutoSocketClassifier RayTriangle primitive (Möller–Trumbore)"
```

### Task 6: 8×8 occupancy grid classification

**Files:**
- Modify: `Engine/Graphics/WFC/AutoSocketClassifier.h` + `.cpp`
- Modify: `EngineTest/UnitTests/Graphics/WFC/TestAutoSocketClassifier.cpp`

- [ ] **Step 1: Write the failing test**

Append to `TestAutoSocketClassifier.cpp`:

```cpp
#include "Engine/Graphics/ProceduralMesh.h"  // for create_cube_mesh etc.

TestResult TestClassifyFaceSolidCube() {
    // A solid cube: every face is fully solid (all 64 bits = 1)
    geometry_id cube = primal::content::create_resource(
        primal::content::asset_type::mesh_procedural,
        primal::graphics::procgen::create_cube_mesh(/*size=*/1.0f));
    SocketEncoding sig = AutoSocketClassifier::ClassifyFace(
        cube, /*variant=*/0, WFCFace::PosX, math::mat4::Identity());
    TEST_ASSERT_EQ(0xFFFFFFFFFFFFFFFFULL, sig, "solid cube face must be all ones");
    return TestResult::Passed;
}

TestResult TestClassifyFaceEmptyMesh() {
    // Empty mesh (no triangles): every face is all-zero
    geometry_id empty = primal::content::create_resource(
        primal::content::asset_type::mesh_procedural,
        primal::graphics::procgen::create_empty_mesh());
    SocketEncoding sig = AutoSocketClassifier::ClassifyFace(
        empty, /*variant=*/0, WFCFace::PosX, math::mat4::Identity());
    TEST_ASSERT_EQ(0ULL, sig, "empty mesh face must be all zeros");
    return TestResult::Passed;
}

TestResult TestMirrorFlipU() {
    // Manually construct a signature with known bit pattern, verify mirror.
    SocketEncoding sig = 0;
    // Set bit at (i=0, j=0) — first bit of first row
    sig |= (1ULL << 0);
    // Set bit at (i=7, j=0) — last bit of first row
    sig |= (1ULL << 7);
    SocketEncoding mirrored = AutoSocketClassifier::MirrorFlipU(sig);
    // After mirror, (0,0) → (7,0) and (7,0) → (0,0), so signature should equal original
    TEST_ASSERT_EQ(sig, mirrored, "symmetric pattern mirrors to itself");
    return TestResult::Passed;
}
```

Add to test suite main().

- [ ] **Step 2: Run to verify fail**

```bash
cmake --build build --target TestAutoSocketClassifier -j 4
```

Expected: FAIL — `ClassifyFace` and `MirrorFlipU` not declared.

- [ ] **Step 3: Extend AutoSocketClassifier.h**

```cpp
// Add to AutoSocketClassifier.h public section:

// Compute 64-bit occupancy signature for one face of one mesh.
//   mesh: tile geometry handle (read via content::get_geometry_data)
//   variant: rotation variant index (0..3) — applies variant_transform
//   face: which of the 6 cube faces to classify
//   variant_transform: rotation matrix (variant * 90° around Y typically)
//
// Algorithm: cast 64 rays (8×8 grid) from outside the face inward.
// Each ray hits a mesh triangle → bit=1 (solid); misses → bit=0 (opening).
static SocketEncoding ClassifyFace(
    geometry_id mesh, u32 variant,
    WFCFace face, const math::mat4& variant_transform);

// Mirror-flip the U axis of a signature (used for opposing-face comparison).
// Each 8-bit row has its bits reversed.
static SocketEncoding MirrorFlipU(SocketEncoding sig);
```

- [ ] **Step 4: Implement ClassifyFace + MirrorFlipU in AutoSocketClassifier.cpp**

```cpp
#include "AutoSocketClassifier.h"
#include "../../Content/ContentToEngine.h"  // for get_geometry_data

namespace primal::graphics::wfc {

namespace {

struct FaceBasis {
    math::v3 origin;     // face center on a unit cube (extent 0.5)
    math::v3 normal;     // outward normal
    math::v3 u_axis;     // local U axis on face plane
    math::v3 v_axis;     // local V axis on face plane
};

// Convention matches WFCFaceCorners.h (CCW from outside).
FaceBasis GetFaceBasis(WFCFace face) {
    switch (face) {
        case WFCFace::PosX: return { {+0.5f, 0, 0}, {+1, 0, 0}, {0, 0, -1}, {0, +1, 0} };
        case WFCFace::NegX: return { {-0.5f, 0, 0}, {-1, 0, 0}, {0, 0, +1}, {0, +1, 0} };
        case WFCFace::PosY: return { {0, +0.5f, 0}, {0, +1, 0}, {+1, 0, 0}, {0, 0, +1} };
        case WFCFace::NegY: return { {0, -0.5f, 0}, {0, -1, 0}, {+1, 0, 0}, {0, 0, -1} };
        case WFCFace::PosZ: return { {0, 0, +0.5f}, {0, 0, +1}, {-1, 0, 0}, {0, +1, 0} };
        case WFCFace::NegZ: return { {0, 0, -0.5f}, {0, 0, -1}, {+1, 0, 0}, {0, +1, 0} };
    }
    return {};
}

u8 ReverseBits8(u8 b) {
    u8 r = 0;
    for (u32 i = 0; i < 8; ++i) {
        r = (r << 1) | (b & 1);
        b >>= 1;
    }
    return r;
}

} // namespace

SocketEncoding AutoSocketClassifier::ClassifyFace(
    geometry_id mesh, u32 variant, WFCFace face, const math::mat4& variant_transform) {

    // Fetch mesh triangle data via content API.
    // (Use existing primitive: content::get_geometry_vertices/indices — adjust to actual API)
    const math::v3*   verts = nullptr;
    const u32*        indices = nullptr;
    u32               tri_count = 0;
    primal::content::get_geometry_data(mesh, &verts, &indices, &tri_count);
    if (!verts || tri_count == 0) return 0;

    FaceBasis fb = GetFaceBasis(face);
    constexpr f32 kSampleOffset = 0.005f;   // start outside face
    constexpr f32 kRayLength    = 0.05f;   // short ray into the cube
    constexpr u32 kGridN        = 8;

    SocketEncoding sig = 0;
    for (u32 j = 0; j < kGridN; ++j) {
        for (u32 i = 0; i < kGridN; ++i) {
            // Sample point at center of cell (i, j) in 8×8 grid covering [-0.5, +0.5]²
            f32 u = -0.5f + (i + 0.5f) / kGridN;
            f32 v = -0.5f + (j + 0.5f) / kGridN;

            // Apply variant_transform to face basis (rotates the sample frame)
            math::v3 local_pos = fb.origin + fb.u_axis * u + fb.v_axis * v;
            math::v3 world_pos = variant_transform * local_pos;
            math::v3 world_nrm = transform_direction(variant_transform, fb.normal);

            math::v3 ray_origin = world_pos + world_nrm * kSampleOffset;
            math::v3 ray_dir    = -world_nrm;

            bool solid = false;
            f32 nearest_t = kRayLength;
            for (u32 t = 0; t < tri_count; ++t) {
                math::v3 v0 = variant_transform * verts[indices[t * 3 + 0]];
                math::v3 v1 = variant_transform * verts[indices[t * 3 + 1]];
                math::v3 v2 = variant_transform * verts[indices[t * 3 + 2]];
                f32 hit_t;
                if (RayTriangle(ray_origin, ray_dir, v0, v1, v2, kRayLength, &hit_t)) {
                    if (hit_t < nearest_t) {
                        nearest_t = hit_t;
                        solid = true;
                    }
                }
            }

            u32 bit_index = i + j * kGridN;
            if (solid) sig |= (1ULL << bit_index);
        }
    }
    return sig;
}

SocketEncoding AutoSocketClassifier::MirrorFlipU(SocketEncoding sig) {
    SocketEncoding out = 0;
    for (u32 row = 0; row < 8; ++row) {
        u8 bits = static_cast<u8>((sig >> (row * 8)) & 0xFF);
        u8 reversed = ReverseBits8(bits);
        out |= static_cast<u64>(reversed) << (row * 8);
    }
    return out;
}

} // namespace
```

**Note for implementer**: the exact API to fetch triangle data (`get_geometry_data` above) needs verification against the existing `Engine/Content/ContentToEngine.h`. Look for the existing pattern `WFCTile::mesh_handles[variant]` is consumed — likely via `content::get_geometry_vertices(...)` or similar. Use that path; the signature shape is what matters.

- [ ] **Step 5: Run test to verify pass**

```bash
cmake --build build --target TestAutoSocketClassifier -j 4 && ./build/bin/TestAutoSocketClassifier
```

Expected: PASS (3 original + 3 new = 6 cases).

- [ ] **Step 6: Commit**

```bash
git add Engine/Graphics/WFC/AutoSocketClassifier.h Engine/Graphics/WFC/AutoSocketClassifier.cpp \
        EngineTest/UnitTests/Graphics/WFC/TestAutoSocketClassifier.cpp
git commit -m "feat(wfc): 8×8 occupancy grid ClassifyFace + MirrorFlipU"
```

### Task 7: ClassifyTile convenience + doorway cube test

**Files:**
- Modify: `Engine/Graphics/WFC/AutoSocketClassifier.h` + `.cpp`
- Modify: `EngineTest/UnitTests/Graphics/WFC/TestAutoSocketClassifier.cpp`

- [ ] **Step 1: Write the failing test**

Append:

```cpp
TestResult TestClassifyTileReturnsSixFaces() {
    geometry_id cube = primal::content::create_resource(
        primal::content::asset_type::mesh_procedural,
        primal::graphics::procgen::create_cube_mesh(1.0f));
    AutoSocketClassifier::FaceSignatures sigs =
        AutoSocketClassifier::ClassifyTile(cube, /*variant=*/0, math::mat4::Identity());
    // All 6 faces of a solid cube should be all-ones
    for (u32 f = 0; f < 6; ++f) {
        TEST_ASSERT_EQ(0xFFFFFFFFFFFFFFFFULL, sigs.face[f], "all faces solid");
    }
    return TestResult::Passed;
}

TestResult TestClassifyDoorwayCube() {
    // A cube with the +X face cut out in the center 4×4 of the 8×8 grid:
    // bits in rows 2-5, cols 2-5 should be 0, others should be 1.
    geometry_id doorway = primal::content::create_resource(
        primal::content::asset_type::mesh_procedural,
        primal::graphics::procgen::create_doorway_cube_mesh());
    SocketEncoding sig = AutoSocketClassifier::ClassifyFace(
        doorway, 0, WFCFace::PosX, math::mat4::Identity());

    // Corners (row 0/7, col 0/7) should be solid
    TEST_ASSERT_TRUE(sig & (1ULL << 0),         "(0,0) solid");
    TEST_ASSERT_TRUE(sig & (1ULL << 7),         "(7,0) solid");
    TEST_ASSERT_TRUE(sig & (1ULL << 56),        "(0,7) solid");
    TEST_ASSERT_TRUE(sig & (1ULL << 63),        "(7,7) solid");
    // Center (3..4, 3..4) should be opening (zero)
    TEST_ASSERT_FALSE(sig & (1ULL << (3 + 3*8)), "(3,3) opening");
    TEST_ASSERT_FALSE(sig & (1ULL << (4 + 4*8)), "(4,4) opening");
    return TestResult::Passed;
}
```

- [ ] **Step 2: Run to verify fail**

Expected: FAIL — `ClassifyTile` not declared; `create_doorway_cube_mesh` doesn't exist.

- [ ] **Step 3: Add ClassifyTile to header**

```cpp
// In AutoSocketClassifier.h public:
struct FaceSignatures { SocketEncoding face[6]; };
static FaceSignatures ClassifyTile(geometry_id mesh, u32 variant,
                                   const math::mat4& variant_transform);
```

- [ ] **Step 4: Implement ClassifyTile**

```cpp
// In AutoSocketClassifier.cpp:
AutoSocketClassifier::FaceSignatures
AutoSocketClassifier::ClassifyTile(geometry_id mesh, u32 variant,
                                   const math::mat4& variant_transform) {
    FaceSignatures out;
    for (u32 f = 0; f < 6; ++f) {
        out.face[f] = ClassifyFace(mesh, variant,
                                   static_cast<WFCFace>(f), variant_transform);
    }
    return out;
}
```

- [ ] **Step 5: Add create_doorway_cube_mesh helper**

This is a test-only procedural mesh. Add to `Engine/Graphics/ProceduralMesh.h` + `.cpp` following the existing `create_cube_mesh` pattern. Geometry:
- Floor: 1×0.1×1 quad at y=-0.5
- Ceiling: 1×0.1×1 quad at y=+0.5
- 4 walls around perimeter, each 0.1 thick, with center cut out:
  - +Z wall: full 1×1 except center 0.5×0.5 (rows 2-5, cols 2-5 area removed)
  - -Z wall: same
  - +X wall: same
  - -X wall: same

Or simpler: build by hand as 16 vertices + 24 triangles forming a "picture frame" on each face. Exact vertex layout is up to the implementer; the test asserts the resulting occupancy grid pattern.

- [ ] **Step 6: Run test to verify pass**

```bash
cmake --build build --target TestAutoSocketClassifier -j 4 && ./build/bin/TestAutoSocketClassifier
```

Expected: PASS (8 cases).

- [ ] **Step 7: Commit**

```bash
git add Engine/Graphics/WFC/AutoSocketClassifier.* Engine/Graphics/ProceduralMesh.* \
        EngineTest/UnitTests/Graphics/WFC/TestAutoSocketClassifier.cpp
git commit -m "feat(wfc): ClassifyTile + doorway-cube occupancy test"
```

---

## Milestone M3 — ProceduralRoomPack (12 tiles)

**Goal:** Runtime-generate 12 room tiles (3 sizes × 4 door configs) so we have a third tile source for stress-testing the mixed solver.

### Task 8: ProceduralRoomPack generator API + tile table

**Files:**
- Create: `Engine/Graphics/WFC/ProceduralRoomPack.h`
- Create: `Engine/Graphics/WFC/ProceduralRoomPack.cpp`

- [ ] **Step 1: Write the failing test**

Create `EngineTest/UnitTests/Graphics/WFC/TestProceduralRoomPack.cpp`:

```cpp
#include "../../TestFramework.h"
#include "Engine/Graphics/WFC/ProceduralRoomPack.h"

using namespace primal::graphics::wfc;
using namespace Engine::Test;

TestResult TestTileCountIs12() {
    TEST_ASSERT_EQ(12u, ProceduralRoomPack::kTileCount, "must be 12 tiles");
    return TestResult::Passed;
}

TestResult TestTileDefsCoverMatrix() {
    // 3 sizes × 4 door configs = 12 unique combinations
    bool seen_size[3]   = {false, false, false};
    bool seen_doors[4]  = {false, false, false, false};
    for (u32 i = 0; i < ProceduralRoomPack::kTileCount; ++i) {
        const auto& def = ProceduralRoomPack::TileDefs()[i];
        u32 size_idx = (def.footprint_cells - 3) / 2;  // 3→0, 5→1, 7→2
        TEST_ASSERT_LT(size_idx, 3u, "size must be 3/5/7");
        seen_size[size_idx] = true;
        TEST_ASSERT_LT(def.door_mask, 16u, "door_mask must fit 4 bits");
        // Valid configs: 1=N(0x1), 2=NS(0x3), 3=EW(0xC), 4=4door(0xF)
        // (Exact mapping documented in ProceduralRoomPack.cpp)
        seen_doors[0] |= (def.door_mask == 0x1);
        seen_doors[1] |= (def.door_mask == 0x3);
        seen_doors[2] |= (def.door_mask == 0xC);
        seen_doors[3] |= (def.door_mask == 0xF);
    }
    for (u32 i = 0; i < 3; ++i) TEST_ASSERT_TRUE(seen_size[i],  "all sizes present");
    for (u32 i = 0; i < 4; ++i) TEST_ASSERT_TRUE(seen_doors[i], "all door configs present");
    return TestResult::Passed;
}

TestResult TestTileNamesAreUnique() {
    std::set<std::string> names;
    for (u32 i = 0; i < ProceduralRoomPack::kTileCount; ++i) {
        names.insert(ProceduralRoomPack::TileDefs()[i].name);
    }
    TEST_ASSERT_EQ(12u, names.size(), "all names unique");
    return TestResult::Passed;
}

int main() {
    TestSuite suite;
    suite.AddTestCase(TestTileCountIs12,   "TileCountIs12");
    suite.AddTestCase(TestTileDefsCoverMatrix, "TileDefsCoverMatrix");
    suite.AddTestCase(TestTileNamesAreUnique,  "TileNamesAreUnique");
    return suite.Run();
}
```

- [ ] **Step 2: Run to verify fail**

```bash
cmake --build build --target TestProceduralRoomPack -j 4 2>&1 | head -10
```

Expected: FAIL — header doesn't exist.

- [ ] **Step 3: Create ProceduralRoomPack.h**

```cpp
// Engine/Graphics/WFC/ProceduralRoomPack.h
#pragma once

#include "../../Common/CommonHeaders.h"
#include "../../Content/ContentToEngine.h"
#include "WFCCategory.h"

namespace primal::graphics::wfc {

class ProceduralRoomPack {
public:
    struct RoomTileDef {
        const char*  name;
        u32          footprint_cells;   // 3, 5, or 7
        u32          door_mask;         // bit 0=+X, 1=-X, 2=+Z, 3=-Z
        WFCCategory  category;          // always Primitive
    };

    static constexpr u32 kTileCount = 12;
    static const RoomTileDef (&TileDefs())[kTileCount];

    // Generate mesh for tile N (0..11). Returns geometry_id via content::create_resource.
    // Mesh: floor + ceiling + 4 walls with door openings per door_mask.
    static geometry::geometry_id GenerateTileMesh(u32 tile_index);
};

} // namespace
```

- [ ] **Step 4: Create ProceduralRoomPack.cpp with tile table**

```cpp
// Engine/Graphics/WFC/ProceduralRoomPack.cpp
#include "ProceduralRoomPack.h"
#include "../../Graphics/ProceduralMesh.h"

namespace primal::graphics::wfc {

namespace {
constexpr ProceduralRoomPack::RoomTileDef kTiles[12] = {
    // 3×3 footprint
    { "proc_room_3x3_door_n",  3, 0x4, WFCCategory::Primitive },  // +Z door only
    { "proc_room_3x3_door_ns", 3, 0xC, WFCCategory::Primitive },  // +Z + -Z
    { "proc_room_3x3_door_ew", 3, 0x3, WFCCategory::Primitive },  // +X + -X
    { "proc_room_3x3_door_4",  3, 0xF, WFCCategory::Primitive },  // all 4
    // 5×5 footprint
    { "proc_room_5x5_door_n",  5, 0x4, WFCCategory::Primitive },
    { "proc_room_5x5_door_ns", 5, 0xC, WFCCategory::Primitive },
    { "proc_room_5x5_door_ew", 5, 0x3, WFCCategory::Primitive },
    { "proc_room_5x5_door_4",  5, 0xF, WFCCategory::Primitive },
    // 7×7 footprint
    { "proc_room_7x7_door_n",  7, 0x4, WFCCategory::Primitive },
    { "proc_room_7x7_door_ns", 7, 0xC, WFCCategory::Primitive },
    { "proc_room_7x7_door_ew", 7, 0x3, WFCCategory::Primitive },
    { "proc_room_7x7_door_4",  7, 0xF, WFCCategory::Primitive },
};
} // namespace

const ProceduralRoomPack::RoomTileDef (&ProceduralRoomPack::TileDefs())[12] {
    return kTiles;
}

// GenerateTileMesh implementation comes in Task 9.

} // namespace
```

- [ ] **Step 5: Run test to verify pass**

```bash
cmake --build build --target TestProceduralRoomPack -j 4 && ./build/bin/TestProceduralRoomPack
```

Expected: PASS (3 cases).

- [ ] **Step 6: Commit**

```bash
git add Engine/Graphics/WFC/ProceduralRoomPack.h Engine/Graphics/WFC/ProceduralRoomPack.cpp \
        EngineTest/UnitTests/Graphics/WFC/TestProceduralRoomPack.cpp \
        Engine/CMakeLists.txt EngineTest/UnitTests/CMakeLists.txt
git commit -m "feat(wfc): ProceduralRoomPack tile table (12 tiles)"
```

### Task 9: GenerateTileMesh — floor + ceiling + walls

**Files:**
- Modify: `Engine/Graphics/WFC/ProceduralRoomPack.cpp`

- [ ] **Step 1: Write the failing test**

Append to `TestProceduralRoomPack.cpp`:

```cpp
#include "Engine/Content/ContentToEngine.h"

TestResult TestGenerateTileMeshProducesGeometry() {
    for (u32 i = 0; i < ProceduralRoomPack::kTileCount; ++i) {
        auto mesh = ProceduralRoomPack::GenerateTileMesh(i);
        TEST_ASSERT_NE(geometry::invalid_id, mesh, "mesh must be valid");
        // Verify mesh has vertices and triangles
        u32 vCount = 0, tCount = 0;
        primal::content::get_geometry_counts(mesh, &vCount, &tCount);
        TEST_ASSERT_GT(vCount, 0u, "must have vertices");
        TEST_ASSERT_GT(tCount, 0u, "must have triangles");
    }
    return TestResult::Passed;
}

TestResult TestGenerateTileMeshDoorOnPosZFace() {
    // Tile 0 (proc_room_3x3_door_n) has door_mask=0x4 (+Z).
    // Classify the +Z face via AutoSocketClassifier — center should be opening.
    auto mesh = ProceduralRoomPack::GenerateTileMesh(0);
    SocketEncoding sig = AutoSocketClassifier::ClassifyFace(
        mesh, 0, WFCFace::PosZ, math::mat4::Identity());
    // Center (3..4, 3..4) should be opening
    TEST_ASSERT_FALSE(sig & (1ULL << (3 + 3*8)), "center opening");
    // Corners should be solid
    TEST_ASSERT_TRUE(sig & (1ULL << 0), "corner solid");
    return TestResult::Passed;
}

TestResult TestGenerateTileMeshNoDoorOnNegXFace() {
    // Tile 0 (door_n) has NO door on -X (mask bit 1 not set).
    // -X face should be fully solid.
    auto mesh = ProceduralRoomPack::GenerateTileMesh(0);
    SocketEncoding sig = AutoSocketClassifier::ClassifyFace(
        mesh, 0, WFCFace::NegX, math::mat4::Identity());
    TEST_ASSERT_EQ(0xFFFFFFFFFFFFFFFFULL, sig, "no-door face fully solid");
    return TestResult::Passed;
}
```

- [ ] **Step 2: Run to verify fail**

```bash
cmake --build build --target TestProceduralRoomPack -j 4 && ./build/bin/TestProceduralRoomPack
```

Expected: FAIL — `GenerateTileMesh` returns invalid_id.

- [ ] **Step 3: Implement GenerateTileMesh**

```cpp
// In ProceduralRoomPack.cpp:

geometry::geometry_id ProceduralRoomPack::GenerateTileMesh(u32 tile_index) {
    assert(tile_index < kTileCount);
    const auto& def = kTiles[tile_index];

    const f32 size = static_cast<f32>(def.footprint_cells);
    const f32 half = size * 0.5f;
    const f32 height = 1.0f;  // tile is 1m tall (matches WFC unit cube)
    const f32 half_h = height * 0.5f;

    // Doorway opening: 2m wide × 3m tall (clamped to tile bounds), centered.
    // For tile sizes < 2m, doorway scales down proportionally.
    const f32 door_width  = std::min(2.0f, size * 0.5f);
    const f32 door_height = std::min(0.6f * height, height);  // leave top strip
    const f32 door_half_w = door_width * 0.5f;

    std::vector<math::v3> verts;
    std::vector<u32>      indices;

    auto add_quad = [&](const math::v3& a, const math::v3& b,
                        const math::v3& c, const math::v3& d) {
        u32 i0 = static_cast<u32>(verts.size());
        verts.push_back(a); verts.push_back(b);
        verts.push_back(c); verts.push_back(d);
        // CCW from outside
        indices.insert(indices.end(), { i0, i0 + 1, i0 + 2, i0, i0 + 2, i0 + 3 });
    };

    // Floor (facing -Y from outside = down). y = -half_h.
    add_quad({-half, -half_h, -half}, {+half, -half_h, -half},
             {+half, -half_h, +half}, {-half, -half_h, +half});
    // Ceiling (facing +Y). y = +half_h.
    add_quad({-half, +half_h, +half}, {+half, +half_h, +half},
             {+half, +half_h, -half}, {-half, +half_h, -half});

    // Wall helper: builds a wall along an axis with optional doorway cut out.
    // Doorway is centered on the wall, width door_width, height door_height.
    auto add_wall_with_door = [&](bool is_x_axis, f32 sign) {
        // Wall extends from -half to +half on the wall's main axis,
        // -half_h to +half_h on Y, located at (is_x_axis ? sign*half : 0) etc.
        // For simplicity: build wall as 4 corner pillars + top strip + 2 side panels.
        // If no door: just one full quad.
        bool has_door = (def.door_mask & (is_x_axis ? (sign > 0 ? 0x1 : 0x2)
                                                    : (sign > 0 ? 0x4 : 0x8))) != 0;
        if (!has_door) {
            // Full wall
            if (is_x_axis) {
                add_quad({sign*half, -half_h, -half}, {sign*half, +half_h, -half},
                         {sign*half, +half_h, +half}, {sign*half, -half_h, +half});
            } else {
                add_quad({-half, -half_h, sign*half}, {+half, -half_h, sign*half},
                         {+half, +half_h, sign*half}, {-half, +half_h, sign*half});
            }
            return;
        }
        // Has door: split into top strip + 2 side panels + (door opening left empty)
        // Top strip: y from (door_top) to +half_h, full width
        f32 door_top = -half_h + door_height;
        if (is_x_axis) {
            // Side panels at z = -half..(-door_half_w), z = door_half_w..half
            add_quad({sign*half, -half_h, -half}, {sign*half, +half_h, -half},
                     {sign*half, +half_h, -door_half_w}, {sign*half, -half_h, -door_half_w});
            add_quad({sign*half, -half_h, +door_half_w}, {sign*half, +half_h, +door_half_w},
                     {sign*half, +half_h, +half}, {sign*half, -half_h, +half});
            // Top strip above door
            add_quad({sign*half, door_top, -door_half_w}, {sign*half, +half_h, -door_half_w},
                     {sign*half, +half_h, +door_half_w}, {sign*half, door_top, +door_half_w});
        } else {
            add_quad({-half, -half_h, sign*half}, {-door_half_w, -half_h, sign*half},
                     {-door_half_w, +half_h, sign*half}, {-half, +half_h, sign*half});
            add_quad({+door_half_w, -half_h, sign*half}, {+half, -half_h, sign*half},
                     {+half, +half_h, sign*half}, {+door_half_w, +half_h, sign*half});
            add_quad({-door_half_w, door_top, sign*half}, {-door_half_w, +half_h, sign*half},
                     {+door_half_w, +half_h, sign*half}, {+door_half_w, door_top, sign*half});
        }
    };

    add_wall_with_door(/*is_x=*/true,  /*sign=*/+1);  // +X wall
    add_wall_with_door(/*is_x=*/true,  /*sign=*/-1);  // -X wall
    add_wall_with_door(/*is_x=*/false, /*sign=*/+1);  // +Z wall
    add_wall_with_door(/*is_x=*/false, /*sign=*/-1);  // -Z wall

    // Convert to engine mesh via content API
    return primal::content::create_resource(
        verts.data(), static_cast<u32>(verts.size()),
        indices.data(), static_cast<u32>(indices.size()),
        primal::content::asset_type::mesh);
}
```

**Note**: adjust the create_resource signature to match the existing API. Check `Engine/Content/ContentToEngine.h` for the right overload.

- [ ] **Step 4: Run test to verify pass**

```bash
cmake --build build --target TestProceduralRoomPack -j 4 && ./build/bin/TestProceduralRoomPack
```

Expected: PASS (6 cases total).

- [ ] **Step 5: Commit**

```bash
git add Engine/Graphics/WFC/ProceduralRoomPack.cpp \
        EngineTest/UnitTests/Graphics/WFC/TestProceduralRoomPack.cpp
git commit -m "feat(wfc): ProceduralRoomPack mesh generator (floor/ceiling/walls/doors)"
```

---

## Milestone M4 — Multi-set registry composition

**Goal:** Compose Kenney + Ruins + Procedural tiles into one WFCTileRegistry. Verify the solver runs end-to-end.

### Task 10: RegisterFromCatalog overload

**Files:**
- Modify: `Engine/Graphics/WFC/WFCTileRegistry.h` + `.cpp`
- Modify: `EngineTest/UnitTests/Graphics/WFC/TestWFCTileRegistry.cpp`

- [ ] **Step 1: Write the failing test**

Append to `TestWFCTileRegistry.cpp`:

```cpp
TestResult TestRegisterFromCatalogSetsCategory() {
    WFCTileRegistry reg;
    // Build a minimal fake "catalog" with 2 tiles
    std::vector<WFCTile> fake;
    fake.resize(2);
    fake[0].name = "test_a"; fake[0].category = WFCCategory::Dungeon;
    fake[1].name = "test_b"; fake[1].category = WFCCategory::Dungeon;

    reg.RegisterFromCatalog(fake, WFCCategory::Dungeon);
    TEST_ASSERT_EQ(2u, reg.Count(), "registered 2");
    TEST_ASSERT_EQ(WFCCategory::Dungeon, reg.Get(wfc_tile_id{0}).category, "cat tagged");
    return TestResult::Passed;
}

TestResult TestMultiCatalogAppend() {
    WFCTileRegistry reg;
    std::vector<WFCTile> a, b;
    a.resize(1); a[0].name = "a0"; a[0].category = WFCCategory::Dungeon;
    b.resize(1); b[0].name = "b0"; b[0].category = WFCCategory::Ruins;

    reg.RegisterFromCatalog(a, WFCCategory::Dungeon);
    reg.RegisterFromCatalog(b, WFCCategory::Ruins);
    TEST_ASSERT_EQ(2u, reg.Count(), "both registered");
    TEST_ASSERT_EQ(WFCCategory::Dungeon, reg.Get(wfc_tile_id{0}).category, "first is Dungeon");
    TEST_ASSERT_EQ(WFCCategory::Ruins,   reg.Get(wfc_tile_id{1}).category, "second is Ruins");
    return TestResult::Passed;
}
```

- [ ] **Step 2: Run to verify fail**

```bash
cmake --build build --target TestWFCTileRegistry -j 4
```

Expected: FAIL — `RegisterFromCatalog` not declared.

- [ ] **Step 3: Add to WFCTileRegistry.h**

```cpp
// In WFCTileRegistry.h public section, after existing Register:

// Append every tile from a catalog vector, tagging with `category`.
// Each tile's own .category field is overwritten with `category` so
// callers don't need to set it per-tile. Used by TestWFCSingleDomain to
// compose Kenney + Ruins + Procedural sources.
void RegisterFromCatalog(const std::vector<WFCTile>& tiles, WFCCategory category);
```

Add `#include <vector>` to the header if not already present.

- [ ] **Step 4: Implement RegisterFromCatalog**

```cpp
// In WFCTileRegistry.cpp:
void WFCTileRegistry::RegisterFromCatalog(const std::vector<WFCTile>& tiles,
                                          WFCCategory category) {
    for (const auto& t : tiles) {
        WFCTile copy = t;
        copy.category = category;
        Register(copy);
    }
}
```

- [ ] **Step 5: Run test to verify pass**

```bash
cmake --build build --target TestWFCTileRegistry -j 4 && ./build/bin/TestWFCTileRegistry
```

Expected: PASS (existing + 2 new).

- [ ] **Step 6: Commit**

```bash
git add Engine/Graphics/WFC/WFCTileRegistry.h Engine/Graphics/WFC/WFCTileRegistry.cpp \
        EngineTest/UnitTests/Graphics/WFC/TestWFCTileRegistry.cpp
git commit -m "feat(wfc): WFCTileRegistry::RegisterFromCatalog with category tag"
```

### Task 11: TileAdjacencyTable::BuildFromClassifier

**Files:**
- Modify: `Engine/Graphics/WFC/TileAdjacency.h` + `.cpp`

- [ ] **Step 1: Write the failing test**

Create `EngineTest/UnitTests/Graphics/WFC/TestTileAdjacencyBuild.cpp`:

```cpp
#include "../../TestFramework.h"
#include "Engine/Graphics/WFC/TileAdjacency.h"
#include "Engine/Graphics/WFC/WFCTileRegistry.h"
#include "Engine/Graphics/WFC/AutoSocketClassifier.h"

using namespace primal::graphics::wfc;
using namespace Engine::Test;

TestResult TestBuildFromClassifierProducesSelfAdjacency() {
    // Two identical cubes: classifier should make them mutually compatible on every face.
    WFCTileRegistry reg;
    // Register 2 tiles with cube meshes (need fixture providing geometry_id)
    // ... use create_cube_mesh and set WFCTile.mesh_handles[0] ...
    TileAdjacencyTable table;
    u32 added = table.BuildFromClassifier(reg);
    TEST_ASSERT_GT(added, 0u, "should produce adjacencies");
    // Verify Compatible returns true for tile_a +X vs tile_b -X
    // (need known geometry_ids; left as exercise following existing test patterns)
    return TestResult::Passed;
}

int main() {
    TestSuite suite;
    suite.AddTestCase(TestBuildFromClassifierProducesSelfAdjacency, "BuildFromClassifierProducesSelfAdjacency");
    return suite.Run();
}
```

- [ ] **Step 2: Run to verify fail**

Expected: FAIL — `BuildFromClassifier` not declared.

- [ ] **Step 3: Add to TileAdjacency.h**

```cpp
// In TileAdjacency.h public section, after AddAutoFromSockets:

// Phase C.1 Mixed: walks every (tile_a, variant_a, face_a) × (tile_b, variant_b)
// pair in `reg`, calls the AutoSocketClassifier on each face, and records
// compatibility via AddCompatibility when the 8×8 occupancy grids are equal
// (after MirrorFlipU on the opposing face).
//
// Returns count of newly-added entries.
u32 BuildFromClassifier(const WFCTileRegistry& reg);
```

- [ ] **Step 4: Implement BuildFromClassifier**

```cpp
// In TileAdjacency.cpp:
#include "AutoSocketClassifier.h"
#include "WFCTileRegistry.h"

u32 TileAdjacencyTable::BuildFromClassifier(const WFCTileRegistry& reg) {
    u32 added = 0;
    for (u32 a = 0; a < reg.Count(); ++a) {
        for (u32 va = 0; va < WFCTile::MaxVariants; ++va) {
            const auto& tile_a = reg.Get(wfc_tile_id{a});
            if (va >= tile_a.variant_count) continue;
            math::mat4 var_a = ComputeVariantTransform(va);  // existing helper
            for (u32 f = 0; f < 6; ++f) {
                WFCFace face = static_cast<WFCFace>(f);
                SocketEncoding sig_a = AutoSocketClassifier::ClassifyFace(
                    tile_a.mesh_handles[va], va, face, var_a);
                for (u32 b = a; b < reg.Count(); ++b) {
                    for (u32 vb = 0; vb < WFCTile::MaxVariants; ++vb) {
                        const auto& tile_b = reg.Get(wfc_tile_id{b});
                        if (vb >= tile_b.variant_count) continue;
                        math::mat4 var_b = ComputeVariantTransform(vb);
                        WFCFace opp = OppositeFace(face);
                        SocketEncoding sig_b = AutoSocketClassifier::ClassifyFace(
                            tile_b.mesh_handles[vb], vb, opp, var_b);
                        SocketEncoding sig_b_mirrored = AutoSocketClassifier::MirrorFlipU(sig_b);
                        if (sig_a == sig_b_mirrored) {
                            AddCompatibility(wfc_tile_id{a}, va, face,
                                             wfc_tile_id{b}, vb);
                            ++added;
                        }
                    }
                }
            }
        }
    }
    return added;
}
```

**Note**: `ComputeVariantTransform` and `OppositeFace` may already exist in the WFC module. Grep for them; if not present, add as local static helpers in TileAdjacency.cpp.

- [ ] **Step 5: Run test to verify pass**

```bash
cmake --build build --target TestTileAdjacencyBuild -j 4 && ./build/bin/TestTileAdjacencyBuild
```

Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add Engine/Graphics/WFC/TileAdjacency.h Engine/Graphics/WFC/TileAdjacency.cpp \
        EngineTest/UnitTests/Graphics/WFC/TestTileAdjacencyBuild.cpp \
        EngineTest/UnitTests/CMakeLists.txt
git commit -m "feat(wfc): TileAdjacencyTable::BuildFromClassifier (8×8 occupancy)"
```

---

## Milestone M5 — ConflictSeedMemory + decay

**Goal:** Bias observer away from previously-failing (cell, tile) pairs; decay bias each restart so transient failures don't permanently block cells.

### Task 12: ConflictRecord struct + RecordConflict

**Files:**
- Modify: `Engine/Graphics/WFC/RestartPolicy.h` + `.cpp`
- Create: `EngineTest/UnitTests/Graphics/WFC/TestConflictSeedMemory.cpp`

- [ ] **Step 1: Write the failing test**

```cpp
// EngineTest/UnitTests/Graphics/WFC/TestConflictSeedMemory.cpp
#include "../../TestFramework.h"
#include "Engine/Graphics/WFC/RestartPolicy.h"

using namespace primal::graphics::wfc;
using namespace Engine::Test;

TestResult TestRecordConflictIncrementsOccurrence() {
    RestartPolicy rp(8);
    rp.OnContradiction(WFCGridCoord{1,2,3}, wfc_tile_id{5}, /*gen=*/0);
    rp.OnContradiction(WFCGridCoord{1,2,3}, wfc_tile_id{5}, /*gen=*/0);
    auto records = rp.ConflictRecords();
    TEST_ASSERT_EQ(1u, records.size(), "one record deduped");
    TEST_ASSERT_EQ(2u, records[0].occurrence_count, "two occurrences");
    return TestResult::Passed;
}

TestResult TestConflictRecordsTrackTile() {
    RestartPolicy rp(8);
    rp.OnContradiction(WFCGridCoord{0,0,0}, wfc_tile_id{2}, 0);
    auto records = rp.ConflictRecords();
    TEST_ASSERT_EQ(2u, static_cast<u32>(records[0].tile), "tile tracked");
    return TestResult::Passed;
}

int main() {
    TestSuite suite;
    suite.AddTestCase(TestRecordConflictIncrementsOccurrence, "RecordConflictIncrementsOccurrence");
    suite.AddTestCase(TestConflictRecordsTrackTile,            "ConflictRecordsTrackTile");
    return suite.Run();
}
```

- [ ] **Step 2: Run to verify fail**

Expected: FAIL — existing RestartPolicy only stores `WFCGridCoord`, not the full record.

- [ ] **Step 3: Update RestartPolicy.h**

```cpp
// Replace the conflicts_ vector type and add ConflictRecord struct:

struct ConflictRecord {
    WFCGridCoord coord;
    wfc_tile_id  tile;
    u32          occurrence_count;
};

class RestartPolicy {
public:
    // ... existing API ...
    const utl::vector<ConflictRecord>& ConflictRecords() const { return conflicts_; }

private:
    utl::vector<ConflictRecord> conflicts_;
    u32                         max_generations_;
};
```

- [ ] **Step 4: Update OnContradiction to dedupe + count**

```cpp
// In RestartPolicy.cpp:
Decision RestartPolicy::OnContradiction(WFCGridCoord coord, wfc_tile_id attempted_tile,
                                         u32 current_generation) {
    // Find existing record matching (coord, tile)
    for (auto& r : conflicts_) {
        if (r.coord.x == coord.x && r.coord.y == coord.y && r.coord.z == coord.z
            && r.tile == attempted_tile) {
            ++r.occurrence_count;
            goto decided;
        }
    }
    conflicts_.push_back({coord, attempted_tile, /*count=*/1});
decided:
    if (current_generation + 1 < max_generations_) return Decision::Restart;
    return Decision::GiveUp;
}
```

- [ ] **Step 5: Update ConflictCoords() callers**

The existing API has `ConflictCoords()` returning `utl::vector<WFCGridCoord>`. Either:
- (a) Remove it and update callers to walk ConflictRecords extracting coord
- (b) Keep both APIs

Choose (b) for backwards compat:

```cpp
utl::vector<WFCGridCoord> ConflictCoords() const {
    utl::vector<WFCGridCoord> out;
    out.reserve(conflicts_.size());
    for (auto& r : conflicts_) out.push_back(r.coord);
    return out;
}
```

- [ ] **Step 6: Run test to verify pass + run existing TestWFCRuinsRestart**

```bash
cmake --build build --target TestConflictSeedMemory -j 4 && ./build/bin/TestConflictSeedMemory
cmake --build build --target TestWFCRuinsRestart -j 4 && ./build/bin/TestWFCRuinsRestart
```

Expected: both PASS.

- [ ] **Step 7: Commit**

```bash
git add Engine/Graphics/WFC/RestartPolicy.h Engine/Graphics/WFC/RestartPolicy.cpp \
        EngineTest/UnitTests/Graphics/WFC/TestConflictSeedMemory.cpp \
        EngineTest/UnitTests/CMakeLists.txt
git commit -m "feat(wfc): RestartPolicy tracks full ConflictRecord (coord+tile+count)"
```

### Task 13: ApplyBias + Decay

**Files:**
- Modify: `Engine/Graphics/WFC/RestartPolicy.h` + `.cpp`
- Modify: `EngineTest/UnitTests/Graphics/WFC/TestConflictSeedMemory.cpp`

- [ ] **Step 1: Write the failing test**

```cpp
TestResult TestApplyBiasReturnsPenalty() {
    RestartPolicy rp(8);
    rp.OnContradiction(WFCGridCoord{1,2,3}, wfc_tile_id{5}, 0);
    rp.OnContradiction(WFCGridCoord{1,2,3}, wfc_tile_id{5}, 0);
    // Penalty for (1,2,3) should be > 0
    f32 penalty = rp.BiasForCell(WFCGridCoord{1,2,3});
    TEST_ASSERT_GT(penalty, 0.0f, "bias positive");
    // Penalty for an unrelated cell should be 0
    TEST_ASSERT_EQ(0.0f, rp.BiasForCell(WFCGridCoord{9,9,9}), "no bias for untracked");
    return TestResult::Passed;
}

TestResult TestDecayHalvesOccurrence() {
    RestartPolicy rp(8);
    rp.OnContradiction(WFCGridCoord{1,2,3}, wfc_tile_id{5}, 0);
    rp.OnContradiction(WFCGridCoord{1,2,3}, wfc_tile_id{5}, 0);
    // count = 2
    rp.DecayAll();
    auto records = rp.ConflictRecords();
    TEST_ASSERT_EQ(1u, records[0].occurrence_count, "halved to 1");
    return TestResult::Passed;
}

TestResult TestBiasForTileInCell() {
    RestartPolicy rp(8);
    rp.OnContradiction(WFCGridCoord{1,2,3}, wfc_tile_id{5}, 0);
    f32 tile_bias = rp.BiasForTileInCell(WFCGridCoord{1,2,3}, wfc_tile_id{5});
    TEST_ASSERT_GT(tile_bias, 0.0f, "tile bias positive");
    f32 other_bias = rp.BiasForTileInCell(WFCGridCoord{1,2,3}, wfc_tile_id{6});
    TEST_ASSERT_EQ(0.0f, other_bias, "other tile no bias");
    return TestResult::Passed;
}
```

- [ ] **Step 2: Run to verify fail**

Expected: FAIL — `BiasForCell`, `DecayAll`, `BiasForTileInCell` not declared.

- [ ] **Step 3: Add API to header**

```cpp
// In RestartPolicy.h public:
// Bias weight (>= 0) for a cell — proportional to its conflict occurrence count.
// Used by observer to add to base entropy so conflict-prone cells get picked later.
f32 BiasForCell(WFCGridCoord coord) const;

// Bias weight for a specific (cell, tile) pair — used during candidate tile
// selection to subtract probability mass from historically-failing tiles.
f32 BiasForTileInCell(WFCGridCoord coord, wfc_tile_id tile) const;

// Halve every ConflictRecord's occurrence_count (integer division).
// Called at the start of each restart so old conflicts fade.
void DecayAll();
```

- [ ] **Step 4: Implement**

```cpp
// In RestartPolicy.cpp:
f32 RestartPolicy::BiasForCell(WFCGridCoord coord) const {
    f32 sum = 0;
    for (auto& r : conflicts_) {
        if (r.coord.x == coord.x && r.coord.y == coord.y && r.coord.z == coord.z) {
            sum += static_cast<f32>(r.occurrence_count);
        }
    }
    return sum;
}

f32 RestartPolicy::BiasForTileInCell(WFCGridCoord coord, wfc_tile_id tile) const {
    for (auto& r : conflicts_) {
        if (r.coord.x == coord.x && r.coord.y == coord.y && r.coord.z == coord.z
            && r.tile == tile) {
            return static_cast<f32>(r.occurrence_count);
        }
    }
    return 0.0f;
}

void RestartPolicy::DecayAll() {
    for (auto& r : conflicts_) {
        r.occurrence_count /= 2;
    }
    // Drop records that decayed to 0
    conflicts_.erase(
        std::remove_if(conflicts_.begin(), conflicts_.end(),
                       [](const ConflictRecord& r) { return r.occurrence_count == 0; }),
        conflicts_.end());
}
```

- [ ] **Step 5: Run tests**

```bash
cmake --build build --target TestConflictSeedMemory -j 4 && ./build/bin/TestConflictSeedMemory
```

Expected: PASS (5 cases).

- [ ] **Step 6: Commit**

```bash
git add Engine/Graphics/WFC/RestartPolicy.h Engine/Graphics/WFC/RestartPolicy.cpp \
        EngineTest/UnitTests/Graphics/WFC/TestConflictSeedMemory.cpp
git commit -m "feat(wfc): RestartPolicy ApplyBias + DecayAll for conflict seed memory"
```

---

## Milestone M6 — TestWFCSingleDomain GUI binary

**Goal:** Build the integration test binary that exercises the full mixed pipeline.

### Task 14: Native test scaffold (no panel yet)

**Files:**
- Create: `EngineTest/IntegrationTests/Graphics/WFC/TestWFCSingleDomain.h`
- Create: `EngineTest/IntegrationTests/Graphics/WFC/TestWFCSingleDomain.cpp`

- [ ] **Step 1: Create header**

Mirror `TestKenneyTilePreview.h` structure. Key members:

```cpp
// TestWFCSingleDomain.h
#pragma once

#include "RenderTestFramework.h"
#include "Engine/Graphics/RenderPipeline/StandardRenderPipeline.h"
#include "Engine/Graphics/RHI/Core/RHIDevice.h"
#include "Engine/Graphics/RenderView.h"
#include "Engine/Graphics/RenderScene.h"
#include "Engine/Platform/Platform.h"
#include "Engine/Graphics/RHI/Systems/RenderSystem.h"
#include "Engine/Graphics/WFC/WFCTileRegistry.h"
#include "Engine/Graphics/WFC/TileAdjacency.h"
#include "Engine/Graphics/WFC/WaveGrid.h"
#include "Engine/Graphics/WFC/WFCSolver.h"
#include "Engine/Graphics/WFC/WFCStepBuffer.h"
#include "Engine/Graphics/WFC/WFCSolveBudget.h"
#include "Engine/Graphics/WFC/WFCObserver.h"
#include "Engine/Graphics/WFC/RestartPolicy.h"
#include "KenneyTileCatalog.h"
#include <memory>
#include <vector>

class TestWFCSingleDomainCase : public primal::test::RenderTestCase {
public:
    TestWFCSingleDomainCase();
    bool Initialize() override;
    void Run() override;
    void Shutdown() override;

    static TestWFCSingleDomainCase* Instance() { return g_instance_; }

    // Panel hooks (drained in HandlePanelEdits)
    void SetCategoryMask(u32 mask)   { pending_mask_ = mask; has_pending_mask_ = true; }
    void SetClassifierAuto(bool yes) { pending_auto_ = yes; has_pending_auto_ = true; }
    void SetRestartBudget(u32 n)     { pending_budget_ = n; has_pending_budget_ = true; }
    void SetGridW(u32 w)             { pending_w_ = w; has_pending_w_ = true; }
    void SetGridH(u32 h)             { pending_h_ = h; has_pending_h_ = true; }
    void SetGridD(u32 d)             { pending_d_ = d; has_pending_d_ = true; }
    void ReseedSame()                { pending_reseed_same_ = true; }
    void ReseedNew()                 { pending_reseed_new_ = true; }

private:
    bool InitWFC();
    void ReseedSolver();
    void PumpSolverFrame();
    void HandlePanelEdits();
    void UpdateCamera();
    void UpdateCameraFromInput();
    void CheckSmokeAsserts();

    std::unique_ptr<primal::graphics::rhi::RHIDeviceBase> device;
    primal::platform::window window;
    primal::graphics::RenderSystem renderSystem;
    primal::graphics::StandardRenderPipeline* pipeline = nullptr;
    primal::graphics::RenderScene* scene = nullptr;
    primal::graphics::RenderView* view = nullptr;

    std::unique_ptr<primal::graphics::wfc::WFCTileRegistry>   registry_;
    std::unique_ptr<primal::graphics::wfc::TileAdjacencyTable> adjacency_;
    std::unique_ptr<primal::graphics::wfc::WaveGrid>          grid_;
    std::unique_ptr<primal::graphics::wfc::WFCStepBuffer>     buf_;
    std::unique_ptr<primal::graphics::wfc::WFCSolver>         solver_;
    std::unique_ptr<primal::graphics::wfc::WFCSolveBudget>    budget_;
    std::unique_ptr<primal::graphics::wfc::RestartPolicy>     restart_;

    std::vector<primal::id::id_type> entity_ids_;
    std::vector<u32>                 mesh_slots_;

    primal::graphics::wfc::WFCSolver::StepResult solver_state_{};
    bool solver_done_{false};
    u32  total_collapses_{0};
    u32  total_restarts_{0};
    u32  rng_seed_{1337};

    u32 grid_w_{16}, grid_h_{4}, grid_d_{16};
    u32 category_mask_{0xFFFF};   // all categories
    bool classifier_auto_{true};
    u32 restart_budget_{8};

    u32 pending_mask_{0}, pending_budget_{0};
    u32 pending_w_{0}, pending_h_{0}, pending_d_{0};
    bool pending_auto_{false};
    bool has_pending_mask_{false}, has_pending_auto_{false}, has_pending_budget_{false};
    bool has_pending_w_{false}, has_pending_h_{false}, has_pending_d_{false};
    bool pending_reseed_same_{false}, pending_reseed_new_{false};

    primal::math::v3 camera_pos_{36.0f, 28.0f, 52.0f};
    float camera_yaw_{-0.7853982f};
    float camera_pitch_{-0.6108652f};
    float camera_speed_{14.0f};

    static TestWFCSingleDomainCase* g_instance_;
};
```

- [ ] **Step 2: Create cpp skeleton with Initialize + camera + smoke assert**

Mirror `TestKenneyTilePreview.cpp` for the Initialize/Run/Shutdown structure, camera, and panel hooks. The InitWFC method is the new piece:

```cpp
// In TestWFCSingleDomain.cpp InitWFC():
registry_ = std::make_unique<WFCTileRegistry>();

// Source 1: Kenney Dungeon tiles (reuse existing KenneyTileCatalog)
KenneyTileCatalog kenney;
kenney.Load("EngineTest/assets/Processed/kenney_dungeon_tiles/");
registry_->RegisterFromCatalog(kenney.Tiles(), WFCCategory::Dungeon);

// Source 2: Ruins tiles (existing WFCTileCatalog)
auto ruins = WFCTileCatalog::BuildDefault();
registry_->RegisterFromCatalog(ruins, WFCCategory::Ruins);

// Source 3: ProceduralRoomPack (12 tiles)
for (u32 i = 0; i < ProceduralRoomPack::kTileCount; ++i) {
    auto mesh = ProceduralRoomPack::GenerateTileMesh(i);
    WFCTile t{};
    t.name = ProceduralRoomPack::TileDefs()[i].name;
    t.mesh_handles[0] = mesh;
    t.variant_count = 1;
    t.category = WFCCategory::Primitive;
    registry_->Register(t);
}

// Build adjacency table from classifier
adjacency_ = std::make_unique<TileAdjacencyTable>();
if (classifier_auto_) {
    adjacency_->BuildFromClassifier(*registry_);
} else {
    adjacency_->AddAutoFromSockets(*registry_);  // existing 8-bit corner path
}

// Init grid + solver + restart policy
grid_ = std::make_unique<WaveGrid>();
grid_->Init(grid_w_, grid_h_, grid_d_, *registry_);

buf_ = std::make_unique<WFCStepBuffer>();
restart_ = std::make_unique<RestartPolicy>(restart_budget_);

WFCConfig config;
config.active_category_mask = category_mask_;
config.rng_seed = rng_seed_;

solver_ = std::make_unique<WFCSolver>();
solver_->Init(*grid_, *registry_, config);
```

- [ ] **Step 3: Add to integration test CMakeLists**

In `EngineTest/IntegrationTests/CMakeLists.txt`, add `TestWFCSingleDomain` target following the TestKenneyTilePreview pattern.

- [ ] **Step 4: Build native target**

```bash
cmake --build build --target TestWFCSingleDomain -j 4
```

Expected: builds successfully (may need to fix compile errors as they arise).

- [ ] **Step 5: Run native binary**

```bash
./build/bin/TestWFCSingleDomain
```

Expected: window opens, tiles render, no crashes. Console logs the smoke assert result on solver completion.

- [ ] **Step 6: Commit**

```bash
git add EngineTest/IntegrationTests/Graphics/WFC/TestWFCSingleDomain.h \
        EngineTest/IntegrationTests/Graphics/WFC/TestWFCSingleDomain.cpp \
        EngineTest/IntegrationTests/CMakeLists.txt
git commit -m "feat(wfc): TestWFCSingleDomain native binary scaffold + smoke assert"
```

### Task 15: Smoke assert + degrade-to-single-set fallback

**Files:**
- Modify: `EngineTest/IntegrationTests/Graphics/WFC/TestWFCSingleDomain.cpp`

- [ ] **Step 1: Add CheckSmokeAsserts method**

```cpp
void TestWFCSingleDomainCase::CheckSmokeAsserts() {
    if (total_collapses_ < 100) {
        fprintf(stderr, "[TestWFCSingleDomain] SMOKE FAIL: only %u cells collapsed\n",
                total_collapses_);
        return;
    }
    // Count distinct tile ids
    std::set<u32> tile_ids;
    for (u32 i = 0; i < grid_->CellCount(); ++i) {
        auto& cell = grid_->CellAt(i);
        if (cell.collapsed) {
            tile_ids.insert(static_cast<u32>(cell.collapsed_tile));
        }
    }
    if (tile_ids.size() < 3) {
        fprintf(stderr, "[TestWFCSingleDomain] SMOKE FAIL: only %zu distinct tiles\n",
                tile_ids.size());
        return;
    }
    // Count distinct categories
    std::set<WFCCategory> cats;
    for (auto id : tile_ids) {
        cats.insert(registry_->Get(wfc_tile_id{id}).category);
    }
    if (cats.size() < 2) {
        fprintf(stderr, "[TestWFCSingleDomain] SMOKE FAIL: only %zu categories\n",
                cats.size());
        return;
    }
    fprintf(stdout, "[TestWFCSingleDomain] SMOKE PASS: %u cells, %zu tiles, %zu categories\n",
            total_collapses_, tile_ids.size(), cats.size());
}
```

- [ ] **Step 2: Add DegradeToSingleSet method**

```cpp
void TestWFCSingleDomainCase::DegradeToSingleSet() {
    fprintf(stdout, "[TestWFCSingleDomain] degrading to Dungeon-only after %u restarts\n",
            total_restarts_);
    // Drop non-Dungeon tiles from the registry by setting category_mask to Dungeon only
    category_mask_ = CategoryMaskFor(WFCCategory::Dungeon);
    // Re-init solver with new mask
    WFCConfig config;
    config.active_category_mask = category_mask_;
    config.rng_seed = ++rng_seed_;
    grid_->Reset();
    solver_->Init(*grid_, *registry_, config);
    total_restarts_ = 0;
}
```

- [ ] **Step 3: Wire degrade into PumpSolverFrame**

```cpp
void TestWFCSingleDomainCase::PumpSolverFrame() {
    if (solver_done_) return;
    // ... existing budget collapse logic ...

    if (solver_state_.state == WFCSolver::StepResult::Contradiction) {
        ++total_restarts_;
        if (total_restarts_ >= restart_budget_) {
            DegradeToSingleSet();
            return;
        }
        restart_->DecayAll();
        ReseedSolver();
    } else if (solver_state_.state == WFCSolver::StepResult::Done) {
        solver_done_ = true;
        CheckSmokeAsserts();
    }
}
```

- [ ] **Step 4: Run native binary**

```bash
./build/bin/TestWFCSingleDomain
```

Expected: completes with `SMOKE PASS` log line. If it hits `SMOKE FAIL`, debug the classifier / registry composition.

- [ ] **Step 5: Commit**

```bash
git add EngineTest/IntegrationTests/Graphics/WFC/TestWFCSingleDomain.cpp
git commit -m "feat(wfc): TestWFCSingleDomain smoke assert + degrade-to-single-set fallback"
```

### Task 16: WASM C ABI exports + HTML shell

**Files:**
- Create: `EngineTest/IntegrationTests/Graphics/WFC/TestWFCSingleDomainMain.cpp`
- Create: `wfc/TestWFCSingleDomain.html`

- [ ] **Step 1: Create TestWFCSingleDomainMain.cpp**

Mirror `TestKenneyMain.cpp`. C ABI exports:

```cpp
// TestWFCSingleDomainMain.cpp
#include "TestWFCSingleDomain.h"
#include <emscripten/emscripten.h>

#ifdef __EMSCRIPTEN__

extern "C" {

EMSCRIPTEN_KEEPALIVE void wfc_set_category_mask(u32 mask) {
    if (auto* p = TestWFCSingleDomainCase::Instance()) p->SetCategoryMask(mask);
}
EMSCRIPTEN_KEEPALIVE void wfc_set_classifier_auto(u32 yes) {
    if (auto* p = TestWFCSingleDomainCase::Instance()) p->SetClassifierAuto(yes != 0);
}
EMSCRIPTEN_KEEPALIVE void wfc_set_restart_budget(u32 n) {
    if (auto* p = TestWFCSingleDomainCase::Instance()) p->SetRestartBudget(n);
}
EMSCRIPTEN_KEEPALIVE void wfc_set_grid_w(u32 w) {
    if (auto* p = TestWFCSingleDomainCase::Instance()) p->SetGridW(w);
}
EMSCRIPTEN_KEEPALIVE void wfc_set_grid_h(u32 h) {
    if (auto* p = TestWFCSingleDomainCase::Instance()) p->SetGridH(h);
}
EMSCRIPTEN_KEEPALIVE void wfc_set_grid_d(u32 d) {
    if (auto* p = TestWFCSingleDomainCase::Instance()) p->SetGridD(d);
}
EMSCRIPTEN_KEEPALIVE void wfc_reseed_same() {
    if (auto* p = TestWFCSingleDomainCase::Instance()) p->ReseedSame();
}
EMSCRIPTEN_KEEPALIVE void wfc_reseed_new() {
    if (auto* p = TestWFCSingleDomainCase::Instance()) p->ReseedNew();
}

}  // extern "C"

#endif
```

- [ ] **Step 2: Add WASM CMake target**

In `EngineTest/IntegrationTests/CMakeLists.txt`, add `TestWFCSingleDomainWASM` following the `TestKenneyTilePreviewWASM` pattern. Export the new C ABI symbols in `EXPORTED_FUNCTIONS`.

- [ ] **Step 3: Create HTML shell**

Copy `wfc/TestKenneyTilePreviewWASM.html` → `wfc/TestWFCSingleDomain.html`. Update title + canvas ID + panel controls:

```html
<!-- Panel controls in TestWFCSingleDomain.html -->
<label><input type="checkbox" id="cat-dungeon" checked> Dungeon</label>
<label><input type="checkbox" id="cat-ruins" checked> Ruins</label>
<label><input type="checkbox" id="cat-procedural" checked> Procedural</label>
<br>
<label>Classifier: <select id="classifier-mode">
  <option value="auto" selected>Auto (8×8 grid)</option>
  <option value="manual">Manual (8-bit corner)</option>
</select></label>
<br>
<label>Restart budget: <input type="range" id="restart-budget" min="1" max="16" value="8"></label>
<br>
<grid sliders for W/H/D — same as TestKenneyTilePreviewWASM.html>

<script>
// Wire panel → C ABI
function updateCategoryMask() {
    var m = 0;
    if (document.getElementById('cat-dungeon').checked)     m |= (1 << 2);  // Dungeon
    if (document.getElementById('cat-ruins').checked)        m |= (1 << 1);  // Ruins
    if (document.getElementById('cat-procedural').checked)   m |= (1 << 0);  // Primitive
    Module._wfc_set_category_mask(m);
}
// ... similar wiring for classifier, restart budget, grid sliders ...
</script>
```

- [ ] **Step 4: Build WASM target**

```bash
cmake --build build-wasm --target TestWFCSingleDomainWASM -j 4
```

Expected: builds + produces `TestWFCSingleDomainWASM.js/.wasm` + `TestWFCSingleDomain.html`.

- [ ] **Step 5: Smoke test in browser**

Open `wfc/TestWFCSingleDomain.html` in Chrome. Verify:
- Window opens, scene renders
- No console errors
- Panel controls update solver (toggle category, reseed)
- WASD works (focus canvas first)

- [ ] **Step 6: Commit**

```bash
git add EngineTest/IntegrationTests/Graphics/WFC/TestWFCSingleDomainMain.cpp \
        EngineTest/IntegrationTests/CMakeLists.txt \
        wfc/TestWFCSingleDomain.html
git commit -m "feat(wfc): TestWFCSingleDomain WASM target + HTML shell"
```

---

## Milestone M7 — Final integration + regression sweep

### Task 17: Full WFC regression sweep

**Files:** none modified

- [ ] **Step 1: Run every WFC test target**

```bash
cmake --build build -j 4 -- -k TestWFC
ctest --test-dir build -R "TestWFC" --output-on-failure
cmake --build build-wasm --target TestWFCSingleDomainWASM -j 4
```

Expected: all targets build; all tests pass; no console errors in browser.

- [ ] **Step 2: Verify spec exit criteria**

Walk through `Docs/superpowers/specs/2026-08-06-wfc-pcg-c1-single-domain-mixed-design.md` §11 (Phase Exit Criteria):

- [ ] AutoSocketClassifier ships with 8×8 occupancy grid algorithm
- [ ] ProceduralRoomPack generates 12 valid tiles
- [ ] ConflictSeedMemory integrated into RestartPolicy
- [ ] WFCTileRegistry extended for multi-set composition
- [ ] TileAdjacencyTable::BuildFromClassifier populated
- [ ] TestWFCSingleDomain GUI binary builds native + WASM
- [ ] Smoke test passes on default settings
- [ ] (Benchmark mode — defer if not implemented; document in completion note)
- [ ] Degrade-to-single-set fallback never leaves grid unsolved

- [ ] **Step 3: Run native TestWFCSingleDomain for 60s**

```bash
./build/bin/TestWFCSingleDomain &
sleep 60 && kill %1
```

Watch for: crashes, validation errors, assertion failures in stderr.

- [ ] **Step 4: Commit completion note**

Create `Docs/superpowers/notes/2026-08-XX-wfc-phase-c1-mixed-completion.md` summarizing:
- What shipped (count of tiles, classifiers, tests)
- Convergence benchmark numbers (run benchmark mode if implemented; otherwise note "deferred")
- Known issues / follow-ups for C.2

```bash
git add Docs/superpowers/notes/2026-08-XX-wfc-phase-c1-mixed-completion.md
git commit -m "docs(wfc): Phase C.1 Mixed completion note"
```

---

## Self-Review

### Spec coverage check

| Spec section | Covered by |
|---|---|
| §3 Architecture | All milestones combined |
| §4.1 AutoSocketClassifier | M2 (Tasks 5–7) |
| §4.2 ProceduralRoomPack | M3 (Tasks 8–9) |
| §4.3 ConflictSeedMemory | M5 (Tasks 12–13) |
| §4.4 Multi-set registry | M4 (Task 10) |
| §4.5 TileAdjacencyTable::BuildFromClassifier | M4 (Task 11) |
| §4.6 TestWFCSingleDomain | M6 (Tasks 14–16) |
| §6 Occupancy grid algorithm | M2 (Task 6 step 4 — full algorithm) |
| §7.1 Restart + seed memory | M5 + M6 |
| §7.2 Degrade fallback | M6 (Task 15 step 2) |
| §7.3 Conflict decay | M5 (Task 13) |
| §8.1 Smoke test | M6 Task 15 |
| §8.2 Benchmark mode | DEFERRED — Task 17 step 2 notes as optional |
| §8.3 Classifier unit tests | M2 |
| §8.4 Procedural tile unit tests | M3 |

Gap: §8.2 benchmark mode (32-seed convergence reporter) is not implemented as a separate task. If user wants it, add Task 18 between M6 and M7. Default: defer to C.3 perf work.

### Placeholder scan

No "TBD" / "TODO" / "fill in later" anywhere. The single "exercise following existing test patterns" in Task 11 is intentional — the test fixture for `geometry_id` setup follows the existing `TestWFCSocketOps.cpp` pattern, which the implementer can read directly.

### Type consistency

- `SocketEncoding` (u64) — used consistently across Tasks 5–7, 11
- `WFCFace` enum — used in Tasks 5, 6, 7, 11
- `WFCCategory` — Tasks 8, 10, 14
- `wfc_tile_id` — Tasks 1, 10, 12, 13, 14
- `geometry_id` / `geometry::geometry_id` — Tasks 6, 7, 9. The two names refer to the same type — `geometry::geometry_id` is the qualified name. The implementer should use whatever the existing codebase uses.
- `MaskWordForBit` / `MaskBitInWord` — Task 1 step 5 adds; Task 3 step 3 uses. Names match.

### Risk watch-items for implementer

1. **`get_geometry_data` API name**: Task 6 step 4 uses `primal::content::get_geometry_data(mesh, &verts, &indices, &tri_count)`. The exact signature must be verified against `Engine/Content/ContentToEngine.h`. Likely the existing name is `get_geometry_vertices` / `get_geometry_indices` separately — adjust accordingly.

2. **`create_cube_mesh` / `create_empty_mesh` / `create_doorway_cube_mesh`**: Task 5/6 tests reference these. The first two may already exist in `Engine/Graphics/ProceduralMesh.h`. `create_doorway_cube_mesh` is new (test-only) — Task 7 step 5 adds it.

3. **`ComputeVariantTransform`**: Task 11 step 4 uses this. If it doesn't exist as a public helper, add it locally in TileAdjacency.cpp as `static math::mat4 ComputeVariantTransform(u32 variant)` returning Y-rotation by `variant * 90°`.

4. **Existing `WFCRuinsRestart` integration test**: Task 12 step 6 verifies backwards compat. If it breaks, the ConflictRecord refactor missed a caller of the old `ConflictCoords()` API.

5. **`std::popcount` requires C++20**: Task 3 step 3 uses `std::popcount`. If the project is C++17, use `__builtin_popcountll` (gcc/clang) or write a manual popcount helper.

---

## Execution Handoff

Plan complete and saved to `Docs/superpowers/plans/2026-08-06-wfc-phase-c1-mixed-multi-category.md`. Two execution options:

**1. Subagent-Driven (recommended)** — I dispatch a fresh subagent per task, review between tasks, fast iteration

**2. Inline Execution** — Execute tasks in this session using executing-plans, batch execution with checkpoints

Which approach?
