# WFC Observer Strategy Refactor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Refactor `WFCObserver` into a pluggable strategy interface, ship two standard implementations as core capabilities (`WFCMinEntropyObserver` migrated from existing heap, `WFCDistanceObserver` new Euclidean radial-expansion strategy), and expose `WFCSolver::SetObserver` as the injection point. The Kenney tile showcase gains hotkeys `O` (cycle strategy) and `P` (cycle origin preset), both triggering immediate solver reset.

**Architecture:** Strategy pattern via abstract base class. Core layer owns the interface + standard implementations; application layer owns enum-driven hotkey switching. All code is RHI-agnostic — same `.cpp/.h` files compile unchanged on native (Metal) and WASM (WebGPU via Dawn).

**Tech Stack:** C++17, CMake, primal engine (`Engine/Graphics/WFC/`), existing WFC unit-test framework (`TestResult` + `TEST_ASSERT*` + `TestSuite`).

**Spec:** `Docs/superpowers/specs/2026-08-04-wfc-observer-strategy-design.md`

---

## File Structure

### Engine core — `Engine/Graphics/WFC/`
| Op | File | Responsibility |
|---|---|---|
| Rewrite | `WFCObserver.h` | Concrete class → abstract base (pure virtuals + virtual dtor) |
| Delete | `WFCObserver.cpp` | No standalone impl under abstract base |
| New | `WFCMinEntropyObserver.h` | Concrete subclass declaration (heap-based) |
| New | `WFCMinEntropyObserver.cpp` | Binary min-heap implementation (migrated byte-for-byte from `WFCObserver.cpp`) |
| New | `WFCDistanceObserver.h` | Concrete subclass declaration (Euclidean, holds origin) |
| New | `WFCDistanceObserver.cpp` | Linear-scan Euclidean strategy |
| Modify | `WFCSolver.h` | `observer_` becomes `unique_ptr<WFCObserver>`; new `SetObserver` / `ResetToDefaultObserver` |
| Modify | `WFCSolver.cpp` | Constructor default-constructs MinEntropy; dereference `observer_->X()`; implement `SetObserver`/`ResetToDefaultObserver` |

### Build files
| Op | File | Responsibility |
|---|---|---|
| Modify | `Engine/Graphics/CMakeLists.txt` | Drop `WFCObserver.cpp`; add the 4 new files |

### Application layer — `EngineTest/IntegrationTests/Graphics/WFC/`
| Op | File | Responsibility |
|---|---|---|
| Modify | `TestKenneyTilePreview.h` | `ObserverKind` / `OriginPreset` enums; new state members |
| Modify | `TestKenneyTilePreview.cpp` | Hotkeys `O`/`P`; `ReseedSolver` constructs observer per kind; `ComputeOrigin` helper |

### Tests — `EngineTest/UnitTests/Graphics/WFC/`
| Op | File | Responsibility |
|---|---|---|
| Modify | `TestWFCObserver.cpp` | Rename test target class to `WFCMinEntropyObserver` (existing tests cover the migrated behavior) |
| New | `TestWFCDistanceObserver.cpp` | Distance strategy behavior |
| Modify | `TestWFCSolver.cpp` | `SetObserver` integration tests |

---

## Tasks

### Task 1: Migrate existing heap implementation into `WFCMinEntropyObserver`

**Goal:** End this task with two parallel concrete classes — the existing `WFCObserver` (untouched) and a new `WFCMinEntropyObserver` (byte-for-byte clone). Engine still compiles, behavior unchanged.

**Files:**
- Create: `Engine/Graphics/WFC/WFCMinEntropyObserver.h`
- Create: `Engine/Graphics/WFC/WFCMinEntropyObserver.cpp`
- Modify: `Engine/Graphics/CMakeLists.txt`

- [ ] **Step 1: Create `WFCMinEntropyObserver.h`**

Copy `Engine/Graphics/WFC/WFCObserver.h` verbatim, rename the class + guard. Result:

```cpp
// Engine/Graphics/WFC/WFCMinEntropyObserver.h
//
// Min-entropy cell selector. Migrated from WFCObserver.cpp when WFCObserver
// became an abstract base. See WFCObserver.h for the strategy interface.
#pragma once

#include "../../Common/CommonHeaders.h"
#include "WFCTypes.h"

#include <vector>
#include <utility>

namespace primal::graphics::wfc {

class WaveGrid;

class WFCMinEntropyObserver {
public:
    void Initialize(const class WaveGrid& grid);
    void OnCellChanged(WFCGridCoord c);
    WFCGridCoord PickNextCollapse(const class WaveGrid& grid);
    bool Empty() const { return heap_.empty(); }

private:
    std::vector<std::pair<u8, WFCGridCoord>> heap_;
};

} // namespace primal::graphics::wfc
```

- [ ] **Step 2: Create `WFCMinEntropyObserver.cpp`**

Copy `Engine/Graphics/WFC/WFCObserver.cpp` verbatim, rename the class. Replace every `WFCObserver::` with `WFCMinEntropyObserver::`. Keep the anonymous namespace + `ComparePairGreater` helper.

- [ ] **Step 3: Add new files to `Engine/Graphics/CMakeLists.txt`**

Find the WFC source list (search for `WFCObserver.cpp` to locate the list) and add `WFCMinEntropyObserver.cpp` next to it. Also add the new `.h` to the header list if the CMake tracks headers separately.

- [ ] **Step 4: Build and verify**

```bash
cmake --build build --target Engine
```

Expected: clean build. Both `WFCObserver` and `WFCMinEntropyObserver` exist as duplicate concrete classes.

- [ ] **Step 5: Commit**

```bash
git add Engine/Graphics/WFC/WFCMinEntropyObserver.h \
        Engine/Graphics/WFC/WFCMinEntropyObserver.cpp \
        Engine/Graphics/CMakeLists.txt
git commit -m "$(cat <<'EOF'
refactor(wfc): clone WFCObserver into WFCMinEntropyObserver

First step of the observer strategy refactor. WFCMinEntropyObserver is a
byte-for-byte clone of WFCObserver; both classes live side-by-side so the
next task can swap WFCObserver to an abstract base without breaking the
build.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: Port `TestWFCObserver.cpp` to test `WFCMinEntropyObserver`

**Goal:** Move all three existing observer tests to the new class name. After this task, `TestWFCObserver.cpp` validates that `WFCMinEntropyObserver` preserves the existing behavior — the migration is observed to be mechanical before we touch `WFCObserver` itself.

**Files:**
- Modify: `EngineTest/UnitTests/Graphics/WFC/TestWFCObserver.cpp`

- [ ] **Step 1: Rename in test file**

In `EngineTest/UnitTests/Graphics/WFC/TestWFCObserver.cpp`:
- Change `#include "Engine/Graphics/WFC/WFCObserver.h"` → `#include "Engine/Graphics/WFC/WFCMinEntropyObserver.h"`
- Replace every `WFCObserver observer;` → `WFCMinEntropyObserver observer;`

The three test functions, asserts, and `main` stay identical.

- [ ] **Step 2: Build the unit test target**

```bash
cmake --build build --target TestWFCObserver
```

Expected: clean build.

- [ ] **Step 3: Run tests**

```bash
./build/EngineTest/UnitTests/Graphics/WFC/TestWFCObserver
```

Expected output: 3/3 tests pass (`Picks_Minimum_Entropy`, `Picks_Invalid_When_All_Collapsed`, `OnCellChanged_Updates_Heap`).

- [ ] **Step 4: Commit**

```bash
git add EngineTest/UnitTests/Graphics/WFC/TestWFCObserver.cpp
git commit -m "$(cat <<'EOF'
test(wfc): port TestWFCObserver to WFCMinEntropyObserver

Mechanical rename. Validates that the migration is byte-for-byte before
WFCObserver becomes an abstract base in the next task.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: Convert `WFCObserver` to abstract base, update `WFCSolver`, delete old `WFCObserver.cpp`

**Goal:** End this task with `WFCObserver` as a pure-virtual interface and `WFCSolver` holding `unique_ptr<WFCObserver>`. Default behavior unchanged (solver auto-creates a `WFCMinEntropyObserver`).

**Files:**
- Modify: `Engine/Graphics/WFC/WFCObserver.h`
- Delete: `Engine/Graphics/WFC/WFCObserver.cpp`
- Modify: `Engine/Graphics/WFC/WFCSolver.h`
- Modify: `Engine/Graphics/WFC/WFCSolver.cpp`
- Modify: `Engine/Graphics/CMakeLists.txt`

- [ ] **Step 1: Rewrite `WFCObserver.h` as abstract base**

```cpp
// Engine/Graphics/WFC/WFCObserver.h
//
// Strategy interface for picking the next cell to collapse. Concrete
// implementations: WFCMinEntropyObserver (default), WFCDistanceObserver.
// Applications can also subclass this directly to inject custom strategies
// into WFCSolver via SetObserver.
#pragma once

#include "../../Common/CommonHeaders.h"
#include "WFCTypes.h"

namespace primal::graphics::wfc {

class WaveGrid;

class WFCObserver {
public:
    virtual ~WFCObserver() = default;
    virtual void Initialize(const WaveGrid& grid) = 0;
    virtual void OnCellChanged(WFCGridCoord c) = 0;
    virtual WFCGridCoord PickNextCollapse(const WaveGrid& grid) = 0;
    virtual bool Empty() const = 0;
};

} // namespace primal::graphics::wfc
```

- [ ] **Step 2: Delete `Engine/Graphics/WFC/WFCObserver.cpp`**

The old concrete implementation is fully superseded by `WFCMinEntropyObserver.cpp`.

```bash
git rm Engine/Graphics/WFC/WFCObserver.cpp
```

- [ ] **Step 3: Modify `WFCSolver.h`**

Three changes to the `WFCSolver` class:

1. Add `#include <memory>` at the top if not already present.
2. Replace the `WFCObserver observer_;` member with:
   ```cpp
   std::unique_ptr<class WFCObserver> observer_;
   ```
3. Add two public methods after `StepResult Step(...)`:
   ```cpp
   // Replace the current observer. Must be called before Initialize.
   // nullptr is rejected in debug builds; use ResetToDefaultObserver() to
   // restore the default MinEntropy observer.
   void SetObserver(std::unique_ptr<WFCObserver> observer);

   // Restore the default MinEntropy observer.
   void ResetToDefaultObserver();
   ```

- [ ] **Step 4: Modify `WFCSolver.cpp`**

Four changes:

1. Replace `#include "WFCObserver.h"` with `#include "WFCMinEntropyObserver.h"` (and keep `WFCObserver.h` for the abstract base type).
2. Constructor (or wherever `observer_` is currently default-constructed): explicitly initialize
   ```cpp
   WFCSolver::WFCSolver()
       : observer_(std::make_unique<WFCMinEntropyObserver>()) {}
   ```
   If `WFCSolver` has no user-defined constructor currently, add one that does only this.
3. Replace every `observer_.X(...)` with `observer_->X(...)`. Sites (from spec references): `Initialize` line 55, `Step` line 174, restart path line 198, `Empty()` check line 213. Search for `observer_\.` to catch all sites.
4. Add the two new methods at the bottom of the public section:
   ```cpp
   void WFCSolver::SetObserver(std::unique_ptr<WFCObserver> observer) {
       assert(observer && "SetObserver(nullptr) is not allowed; use ResetToDefaultObserver()");
       observer_ = std::move(observer);
   }

   void WFCSolver::ResetToDefaultObserver() {
       observer_ = std::make_unique<WFCMinEntropyObserver>();
   }
   ```

- [ ] **Step 5: Update `Engine/Graphics/CMakeLists.txt`**

Remove `WFCObserver.cpp` from the source list (already deleted physically).

- [ ] **Step 6: Build the engine**

```bash
cmake --build build --target Engine
```

Expected: clean build. If errors mention `WFCObserver` being abstract or `unique_ptr` issues, recheck step 4 — every `observer_.X` must be `observer_->X`.

- [ ] **Step 7: Run all WFC unit tests**

```bash
cmake --build build --target TestWFCObserver TestWFCSolver TestWFCPropagator TestWaveGrid
./build/EngineTest/UnitTests/Graphics/WFC/TestWFCObserver
./build/EngineTest/UnitTests/Graphics/WFC/TestWFCSolver
./build/EngineTest/UnitTests/Graphics/WFC/TestWFCPropagator
./build/EngineTest/UnitTests/Graphics/WFC/TestWaveGrid
```

Expected: all pass. Existing solver tests exercise the default MinEntropy path and validate that the `unique_ptr` migration is behavior-preserving.

- [ ] **Step 8: Commit**

```bash
git add Engine/Graphics/WFC/WFCObserver.h \
        Engine/Graphics/WFC/WFCSolver.h \
        Engine/Graphics/WFC/WFCSolver.cpp \
        Engine/Graphics/CMakeLists.txt
git rm Engine/Graphics/WFC/WFCObserver.cpp  # if not already staged
git commit -m "$(cat <<'EOF'
refactor(wfc): WFCObserver -> abstract base, WFCSolver holds unique_ptr

WFCObserver is now a pure-virtual strategy interface. WFCSolver owns a
unique_ptr<WFCObserver> default-constructed to WFCMinEntropyObserver,
preserving existing behavior. Adds SetObserver()/ResetToDefaultObserver()
as the strategy injection point.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

### Task 4: Add `SetObserver` integration tests

**Goal:** Lock in the new injection API with focused tests before building the distance observer.

**Files:**
- Modify: `EngineTest/UnitTests/Graphics/WFC/TestWFCSolver.cpp`

- [ ] **Step 1: Write failing tests**

Append to `TestWFCSolver.cpp` (before `main`). The existing `MakeTrivialSolver` helper is 1x1x1 which can't distinguish strategies, so the new tests inline a 3x1x1 grid setup using the same pattern (`WFCTile` self-compatible + wildcard socket). At top of file, add:

```cpp
#include "Engine/Graphics/WFC/WFCMinEntropyObserver.h"
#include <memory>
```

Test bodies:

```cpp
// 3x1x1 grid, single self-compatible tile, default observer (MinEntropy).
// All three cells have equal entropy -> MinEntropy tie -> first-in-heap = row-major (0,0,0).
// Verifies the unique_ptr migration preserved default behavior.
TestResult TestWFCSolver_DefaultObserver_IsMinEntropy() {
    WFCConfig config;
    config.grid_size = {3, 1, 1};
    config.max_cells_per_frame = 1;
    config.max_ms_per_frame = 100;
    config.seed = 42;
    config.max_generations = 4;

    WFCTile t{};
    t.name = "trivial";
    t.variant_count = 1;
    t.sockets[0] = 0xFFFFFFFFFFFFFFFFULL;  // wildcard socket

    WaveGrid grid;
    WFCTileRegistry reg;
    TileAdjacencyTable adj;
    WFCStepBuffer buf;

    reg.Register(t);
    WFCSolver solver;  // default-constructed observer is MinEntropy
    solver.Initialize(config, grid, reg, adj, buf);

    WFCSolveBudget budget(10, 100);
    budget.Reset();
    solver.Step(budget);

    TEST_ASSERT(grid.CellAt({0, 0, 0}).collapsed,
                "MinEntropy default collapses (0,0,0) first on tie (row-major)");
    TEST_ASSERT(!grid.CellAt({1, 0, 0}).collapsed, "(1,0,0) NOT collapsed first");
    return TestResult::Passed;
}

// Same grid + setup, but call SetObserver(make_unique<MinEntropyObserver>()) explicitly.
// Behavior must match the default. Locks in that SetObserver doesn't break state.
TestResult TestWFCSolver_SetObserver_ReplacesDefault() {
    WFCConfig config;
    config.grid_size = {3, 1, 1};
    config.max_cells_per_frame = 1;
    config.max_ms_per_frame = 100;
    config.seed = 42;
    config.max_generations = 4;

    WFCTile t{};
    t.name = "trivial";
    t.variant_count = 1;
    t.sockets[0] = 0xFFFFFFFFFFFFFFFFULL;

    WaveGrid grid;
    WFCTileRegistry reg;
    TileAdjacencyTable adj;
    WFCStepBuffer buf;

    reg.Register(t);
    WFCSolver solver;
    solver.SetObserver(std::make_unique<WFCMinEntropyObserver>());
    solver.Initialize(config, grid, reg, adj, buf);

    WFCSolveBudget budget(10, 100);
    budget.Reset();
    solver.Step(budget);

    TEST_ASSERT(grid.CellAt({0, 0, 0}).collapsed,
                "SetObserver(MinEntropy) preserves default collapse behavior");
    return TestResult::Passed;
}
```

Register both in `main`:
```cpp
TEST_CASE(suite, "DefaultObserver_IsMinEntropy", TestWFCSolver_DefaultObserver_IsMinEntropy);
TEST_CASE(suite, "SetObserver_ReplacesDefault", TestWFCSolver_SetObserver_ReplacesDefault);
```

- [ ] **Step 3: Run tests**

```bash
cmake --build build --target TestWFCSolver
./build/EngineTest/UnitTests/Graphics/WFC/TestWFCSolver
```

Expected: all tests pass (the default observer is already MinEntropy; SetObserver with MinEntropy is a no-op behaviorally).

- [ ] **Step 4: Commit**

```bash
git add EngineTest/UnitTests/Graphics/WFC/TestWFCSolver.cpp
git commit -m "$(cat <<'EOF'
test(wfc): WFCSolver default observer + SetObserver tests

Locks in that the unique_ptr<WFCObserver> migration preserves default
MinEntropy behavior and that SetObserver correctly swaps strategy.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

### Task 5: Implement `WFCDistanceObserver`

**Goal:** TDD the new radial-expansion strategy. End this task with a passing `TestWFCDistanceObserver` binary.

**Files:**
- Create: `Engine/Graphics/WFC/WFCDistanceObserver.h`
- Create: `Engine/Graphics/WFC/WFCDistanceObserver.cpp`
- Create: `EngineTest/UnitTests/Graphics/WFC/TestWFCDistanceObserver.cpp`
- Modify: `Engine/Graphics/CMakeLists.txt`
- Modify: `EngineTest/UnitTests/Graphics/WFC/CMakeLists.txt` (or wherever unit-test binaries are listed)

- [ ] **Step 1: Write failing tests**

Create `EngineTest/UnitTests/Graphics/WFC/TestWFCDistanceObserver.cpp`:

```cpp
#include "../../TestFramework.h"
#include "Engine/Graphics/WFC/WFCDistanceObserver.h"
#include "Engine/Graphics/WFC/WaveGrid.h"

using namespace primal::graphics::wfc;
using namespace Engine::Test;

namespace {
void SetCellCandidates(WaveGrid& grid, WFCGridCoord c, u64 mask, u32 count) {
    WFCCell& cell = grid.CellAt(c);
    cell.candidate_mask = mask;
    cell.candidate_count = count;
    cell.entropy = static_cast<u8>(count);
    cell.collapsed = false;
}
}

// 3x1x3 grid, all cells uncollapsed, origin = (1,0,1).
// PickNextCollapse should return the origin first, then the 4-neighbor ring
// at distance 1, then the 4 corners at distance sqrt(2).
TestResult TestWFCDistanceObserver_PicksClosestFirst() {
    WaveGrid grid;
    grid.Initialize({3, 1, 3}, 8);
    for (s32 x = 0; x < 3; ++x)
        for (s32 z = 0; z < 3; ++z)
            SetCellCandidates(grid, {x, 0, z}, 0b00001111u, 3);

    WFCDistanceObserver observer({1, 0, 1});
    observer.Initialize(grid);

    WFCGridCoord first = observer.PickNextCollapse(grid);
    TEST_ASSERT_EQ(1, first.x, "First pick X should be origin X");
    TEST_ASSERT_EQ(0, first.y, "First pick Y should be origin Y");
    TEST_ASSERT_EQ(1, first.z, "First pick Z should be origin Z");

    // Collapse origin in the grid, then verify next pick is one of the
    // 4 neighbors at Manhattan distance 1 (Euclidean distance also 1).
    WFCCell& origin = grid.CellAt({1, 0, 1});
    origin.collapsed = true;
    origin.entropy = 0;
    origin.candidate_count = 0;
    observer.OnCellChanged({1, 0, 1});  // distance observer: no-op but must not crash

    WFCGridCoord second = observer.PickNextCollapse(grid);
    const int dx = abs(second.x - 1);
    const int dz = abs(second.z - 1);
    TEST_ASSERT(dx + dz == 1, "Second pick should be a 4-neighbor of origin");

    return TestResult::Passed;
}

// Tie-break: among equidistant cells, row-major (X varies fastest, then Z)
// order wins. In the 3x1x3 grid above, after origin and 4 neighbors are
// collapsed, the 4 corners are all at distance sqrt(2). Row-major picks
// (0,0,0) first.
TestResult TestWFCDistanceObserver_TieBreakRowMajor() {
    WaveGrid grid;
    grid.Initialize({3, 1, 3}, 8);
    for (s32 x = 0; x < 3; ++x)
        for (s32 z = 0; z < 3; ++z)
            SetCellCandidates(grid, {x, 0, z}, 0b00001111u, 3);

    WFCDistanceObserver observer({1, 0, 1});
    observer.Initialize(grid);

    // Collapse origin + 4 neighbors.
    auto collapse = [&](WFCGridCoord c) {
        WFCCell& cell = grid.CellAt(c);
        cell.collapsed = true;
        cell.entropy = 0;
        cell.candidate_count = 0;
    };
    collapse({1, 0, 1});
    collapse({0, 0, 1});
    collapse({2, 0, 1});
    collapse({1, 0, 0});
    collapse({1, 0, 2});

    WFCGridCoord corner = observer.PickNextCollapse(grid);
    TEST_ASSERT_EQ(0, corner.x, "Tie-break should pick X=0 first (row-major)");
    TEST_ASSERT_EQ(0, corner.z, "Tie-break should pick Z=0 first (row-major)");

    return TestResult::Passed;
}

// All cells collapsed -> returns {-1,-1,-1} and Empty()==true.
TestResult TestWFCDistanceObserver_AllCollapsed() {
    WaveGrid grid;
    grid.Initialize({2, 1, 2}, 8);
    for (s32 x = 0; x < 2; ++x)
        for (s32 z = 0; z < 2; ++z) {
            WFCCell& c = grid.CellAt({x, 0, z});
            c.collapsed = true;
            c.entropy = 0;
            c.candidate_count = 0;
        }

    WFCDistanceObserver observer({0, 0, 0});
    observer.Initialize(grid);
    WFCGridCoord picked = observer.PickNextCollapse(grid);
    TEST_ASSERT(picked.x < 0 || picked.y < 0 || picked.z < 0,
                "Should return invalid coord when all collapsed");
    TEST_ASSERT(observer.Empty(), "Empty() should be true after no candidate found");
    return TestResult::Passed;
}

int main() {
    TestSuite suite("WFCDistanceObserver");
    TEST_CASE(suite, "PicksClosestFirst", TestWFCDistanceObserver_PicksClosestFirst);
    TEST_CASE(suite, "TieBreakRowMajor", TestWFCDistanceObserver_TieBreakRowMajor);
    TEST_CASE(suite, "AllCollapsed", TestWFCDistanceObserver_AllCollapsed);
    suite.RunAllTests();
    return 0;
}
```

- [ ] **Step 2: Register the test target in `EngineTest/UnitTests/CMakeLists.txt`**

Unit-test targets live in `EngineTest/UnitTests/CMakeLists.txt` (not a per-subdir file). The `TestWFCObserver` target is registered at line ~1926. Add `TestWFCDistanceObserver` immediately after, mirroring the same block:

```cmake
add_executable(TestWFCDistanceObserver Graphics/WFC/TestWFCDistanceObserver.cpp)
target_include_directories(TestWFCDistanceObserver PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/EngineTest/UnitTests
)
target_link_libraries(TestWFCDistanceObserver PRIVATE Engine)
add_test(NAME TestWFCDistanceObserver COMMAND TestWFCDistanceObserver)
set_target_properties(TestWFCDistanceObserver PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Tests/UnitTests
)
target_compile_definitions(TestWFCDistanceObserver PRIVATE DEBUG _DEBUG)
set_target_properties(TestWFCDistanceObserver PROPERTIES FOLDER "Tests/Graphics/WFC")
```

- [ ] **Step 3: Build the test (expect link failure — `WFCDistanceObserver` doesn't exist yet)**

```bash
cmake --build build --target TestWFCDistanceObserver
```

Expected: failure — `WFCDistanceObserver.h` not found.

- [ ] **Step 4: Create `WFCDistanceObserver.h`**

```cpp
// Engine/Graphics/WFC/WFCDistanceObserver.h
//
// Radial-expansion observer. PickNextCollapse returns the uncollapsed cell
// closest (Euclidean) to a configured origin. Ties broken by row-major
// scan order. OnCellChanged is a no-op (distance doesn't change with
// candidate set). Complexity: O(N) per Pick.
#pragma once

#include "../../Common/CommonHeaders.h"
#include "WFCTypes.h"
#include "WFCObserver.h"

namespace primal::graphics::wfc {

class WFCDistanceObserver : public WFCObserver {
public:
    explicit WFCDistanceObserver(WFCGridCoord origin)
        : origin_(origin) {}

    void Initialize(const WaveGrid& grid) override;
    void OnCellChanged(WFCGridCoord c) override;  // no-op
    WFCGridCoord PickNextCollapse(const WaveGrid& grid) override;
    bool Empty() const override { return empty_; }

private:
    WFCGridCoord origin_;
    WFCGridCoord grid_size_{0, 0, 0};
    bool empty_{false};
};

} // namespace primal::graphics::wfc
```

- [ ] **Step 5: Create `WFCDistanceObserver.cpp`**

```cpp
// Engine/Graphics/WFC/WFCDistanceObserver.cpp
#include "WFCDistanceObserver.h"
#include "WaveGrid.h"

namespace primal::graphics::wfc {

void WFCDistanceObserver::Initialize(const WaveGrid& grid) {
    grid_size_ = grid.Size();
    empty_ = false;
}

void WFCDistanceObserver::OnCellChanged(WFCGridCoord /*c*/) {
    // Distance-from-origin doesn't depend on candidate state. No-op.
}

WFCGridCoord WFCDistanceObserver::PickNextCollapse(const WaveGrid& grid) {
    if (empty_) return {-1, -1, -1};

    const WFCGridCoord size = grid.Size();
    WFCGridCoord best{-1, -1, -1};
    u64 best_dist_sq = static_cast<u64>(-1);  // max u64

    for (s32 z = 0; z < size.z; ++z) {
        for (s32 y = 0; y < size.y; ++y) {
            for (s32 x = 0; x < size.x; ++x) {
                const WFCGridCoord c{x, y, z};
                const WFCCell& cell = grid.CellAt(c);
                if (cell.collapsed) continue;
                if (cell.entropy == 0) continue;  // contradiction
                const s32 dx = x - origin_.x;
                const s32 dy = y - origin_.y;
                const s32 dz = z - origin_.z;
                const u64 dist_sq = u64(dx * dx) + u64(dy * dy) + u64(dz * dz);
                if (dist_sq < best_dist_sq) {
                    best_dist_sq = dist_sq;
                    best = c;
                }
                // Ties: keep the first-found (row-major order). Strict <
                // means later equidistant cells do NOT replace the first.
            }
        }
    }

    if (best.x < 0) {
        empty_ = true;
        return {-1, -1, -1};
    }
    return best;
}

} // namespace primal::graphics::wfc
```

- [ ] **Step 6: Add to `Engine/Graphics/CMakeLists.txt`**

Add `WFCDistanceObserver.cpp` to the WFC source list (and the `.h` if tracked).

- [ ] **Step 7: Build and run**

```bash
cmake --build build --target TestWFCDistanceObserver
./build/EngineTest/UnitTests/Graphics/WFC/TestWFCDistanceObserver
```

Expected: 3/3 tests pass.

- [ ] **Step 8: Commit**

```bash
git add Engine/Graphics/WFC/WFCDistanceObserver.h \
        Engine/Graphics/WFC/WFCDistanceObserver.cpp \
        Engine/Graphics/CMakeLists.txt \
        EngineTest/UnitTests/Graphics/WFC/TestWFCDistanceObserver.cpp \
        EngineTest/UnitTests/CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat(wfc): WFCDistanceObserver — Euclidean radial-expansion strategy

Second standard observer implementation: linear-scan Euclidean distance
to a configured origin. O(N) per Pick but negligible at demo scale
(16x4x16 = 1024 cells). Ties broken by row-major scan order. Adds
focused unit tests covering closest-first, tie-break, and all-collapsed.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

### Task 6: Add `WFCSolver` + `WFCDistanceObserver` integration test

**Goal:** Prove the new strategy actually changes solver collapse order end-to-end.

**Files:**
- Modify: `EngineTest/UnitTests/Graphics/WFC/TestWFCSolver.cpp`

- [ ] **Step 1: Write failing test**

Append to `TestWFCSolver.cpp`. At top of file (with the other includes), add:

```cpp
#include "Engine/Graphics/WFC/WFCDistanceObserver.h"
```

(`<memory>` is already included via `WFCMinEntropyObserver.h` from Task 4.)

Test body — 3x1x3 grid, single self-compatible tile, distance observer at origin (1,0,1):

```cpp
// SetObserver(make_unique<WFCDistanceObserver>(origin)) on a 3x1x3 grid where
// all cells have equal entropy. MinEntropy would pick row-major first: (0,0,0).
// Distance observer with origin (1,0,1) must collapse (1,0,1) first instead.
TestResult TestWFCSolver_SetObserver_DistanceChangesOrder() {
    WFCConfig config;
    config.grid_size = {3, 1, 3};
    config.max_cells_per_frame = 1;
    config.max_ms_per_frame = 100;
    config.seed = 42;
    config.max_generations = 4;

    WFCTile t{};
    t.name = "trivial";
    t.variant_count = 1;
    t.sockets[0] = 0xFFFFFFFFFFFFFFFFULL;  // wildcard

    WaveGrid grid;
    WFCTileRegistry reg;
    TileAdjacencyTable adj;
    WFCStepBuffer buf;

    reg.Register(t);
    WFCSolver solver;
    solver.SetObserver(std::make_unique<WFCDistanceObserver>(WFCGridCoord{1, 0, 1}));
    solver.Initialize(config, grid, reg, adj, buf);

    WFCSolveBudget budget(10, 100);
    budget.Reset();
    solver.Step(budget);

    TEST_ASSERT(grid.CellAt({1, 0, 1}).collapsed,
                "Distance observer collapses origin (1,0,1) first");
    TEST_ASSERT(!grid.CellAt({0, 0, 0}).collapsed,
                "(0,0,0) NOT collapsed — distance sqrt(2) > 0 from origin");
    return TestResult::Passed;
}
```

Register in `main`:
```cpp
TEST_CASE(suite, "SetObserver_DistanceChangesOrder", TestWFCSolver_SetObserver_DistanceChangesOrder);
```

- [ ] **Step 2: Run test**

```bash
cmake --build build --target TestWFCSolver
./build/EngineTest/UnitTests/Graphics/WFC/TestWFCSolver
```

Expected: pass. If it fails, the distance observer isn't being honored — recheck `WFCSolver::SetObserver` actually moves into `observer_` and that `Step` calls `observer_->PickNextCollapse`.

- [ ] **Step 3: Commit**

```bash
git add EngineTest/UnitTests/Graphics/WFC/TestWFCSolver.cpp
git commit -m "$(cat <<'EOF'
test(wfc): WFCSolver + distance observer end-to-end

Verifies SetObserver(WFCDistanceObserver) actually changes the solver's
collapse order to radial-from-origin instead of min-entropy row-major.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

### Task 7: Application-layer integration in `TestKenneyTilePreview`

**Goal:** Wire the new observer strategy + origin preset into the Kenney showcase with `O` / `P` hotkeys.

**Files:**
- Modify: `EngineTest/IntegrationTests/Graphics/WFC/TestKenneyTilePreview.h`
- Modify: `EngineTest/IntegrationTests/Graphics/WFC/TestKenneyTilePreview.cpp`

- [ ] **Step 1: Add enums + members to `TestKenneyTilePreview.h`**

Add to the private section (near `grid_w_`/`grid_h_`/`grid_d_`):

```cpp
enum class ObserverKind : u32 { MinEntropy = 0, DistanceFromOrigin = 1 };
enum class OriginPreset : u32 { Center = 0, Corner = 1, BottomCenter = 2 };

ObserverKind observer_kind_{ObserverKind::MinEntropy};
OriginPreset origin_preset_{OriginPreset::Center};

WFCGridCoord ComputeOrigin() const;
```

Also add `#include "Engine/Graphics/WFC/WFCMinEntropyObserver.h"` and `#include "Engine/Graphics/WFC/WFCDistanceObserver.h"` if not implicitly visible. (Existing includes for `WFCObserver.h` may need updating to point at the new headers.)

- [ ] **Step 2: Add `ComputeOrigin` to `.cpp`**

```cpp
WFCGridCoord KenneyTilePreviewTestCase::ComputeOrigin() const {
    switch (origin_preset_) {
        case OriginPreset::Center:
            return {(s32)grid_w_/2, (s32)grid_h_/2, (s32)grid_d_/2};
        case OriginPreset::Corner:
            return {0, 0, 0};
        case OriginPreset::BottomCenter:
            return {(s32)grid_w_/2, 0, (s32)grid_d_/2};
    }
    return {(s32)grid_w_/2, (s32)grid_h_/2, (s32)grid_d_/2};
}
```

- [ ] **Step 3: Modify `ReseedSolver` to construct the right observer**

Find the existing `ReseedSolver()` body. Where it currently creates `solver_` and calls `Initialize`, insert before `Initialize`:

```cpp
std::unique_ptr<WFCObserver> observer;
switch (observer_kind_) {
    case ObserverKind::MinEntropy:
        observer = std::make_unique<WFCMinEntropyObserver>();
        break;
    case ObserverKind::DistanceFromOrigin:
        observer = std::make_unique<WFCDistanceObserver>(ComputeOrigin());
        break;
}
solver_->SetObserver(std::move(observer));
```

If `ReseedSolver` currently does `solver_ = std::make_unique<WFCSolver>();`, the SetObserver call goes after that line and before `solver_->Initialize(...)`.

- [ ] **Step 4: Add `O` and `P` hotkeys to `HandleGridEditKeys`**

In `HandleGridEditKeys()`, after the existing `T` (new-seed reseed) block, add:

```cpp
if (just_pressed_(static_cast<u32>(ic::key_o), key_now(ic::key_o))) {
    observer_kind_ = (observer_kind_ == ObserverKind::MinEntropy)
                         ? ObserverKind::DistanceFromOrigin
                         : ObserverKind::MinEntropy;
    PrintGridState("observer kind cycled");
    ReseedSolver();
}
if (just_pressed_(static_cast<u32>(ic::key_p), key_now(ic::key_p))) {
    origin_preset_ = static_cast<OriginPreset>(
        (static_cast<u32>(origin_preset_) + 1) % 3);
    PrintGridState("origin preset cycled");
    ReseedSolver();
}
```

- [ ] **Step 5: Update `PrintControls` and `PrintGridState`**

In the controls banner, add:
```
[O] cycle observer strategy (MinEntropy <-> DistanceFromOrigin)
[P] cycle origin preset (Center -> Corner -> BottomCenter)
```

In `PrintGridState`, append the current state:
```cpp
std::cout << "  observer: "
          << (observer_kind_ == ObserverKind::MinEntropy ? "MinEntropy"
               : "DistanceFromOrigin")
          << ", origin preset: ";
switch (origin_preset_) {
    case OriginPreset::Center:       std::cout << "Center"; break;
    case OriginPreset::Corner:       std::cout << "Corner"; break;
    case OriginPreset::BottomCenter: std::cout << "BottomCenter"; break;
}
std::cout << " (origin=" << ComputeOrigin().x << "," << ComputeOrigin().y
          << "," << ComputeOrigin().z << ")" << std::endl;
```

- [ ] **Step 6: Build**

```bash
cmake --build build --target TestKenneyTilePreview
```

Expected: clean build.

- [ ] **Step 7: Manual demo verification**

Launch:
```bash
./build/EngineTest/IntegrationTests/TestKenneyTilePreview
```

Verify each of these in the running window:
1. Default start: MinEntropy strategy, collapses in arbitrary order.
2. Press `O` mid-generation: solver resets; collapses now proceed radially from grid center.
3. Press `P`: radial center moves to corner (0,0,0); observe collapse pattern shifts.
4. Press `P` again: radial center moves to bottom-center.
5. Press `O`: back to MinEntropy.
6. Existing hotkeys (`[`/`]`, `,`/`.`, `-`/`+`, `R`, `T`) still work.
7. Console output prints current observer kind + origin on each switch.

If any step fails, fix before committing.

- [ ] **Step 8: Commit**

```bash
git add EngineTest/IntegrationTests/Graphics/WFC/TestKenneyTilePreview.h \
        EngineTest/IntegrationTests/Graphics/WFC/TestKenneyTilePreview.cpp
git commit -m "$(cat <<'EOF'
feat(wfc): Kenney showcase — O/P hotkeys to cycle observer + origin

Application-layer integration of the new observer strategy API. O cycles
MinEntropy <-> DistanceFromOrigin; P cycles origin preset (Center/Corner/
BottomCenter). Both trigger immediate ReseedSolver so the demo always
reflects the current strategy. All code is RHI-agnostic.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

### Task 8: Regression sweep

**Goal:** Confirm nothing else broke. Existing WFC consumers (`TestWFCRuinsRendering`, `TestWFCStreaming`, etc.) must still work because they construct `WFCSolver` without calling `SetObserver` (default MinEntropy path).

**Files:** None modified — verification only.

- [ ] **Step 1: Run every WFC unit test**

```bash
cmake --build build --target TestWFCObserver TestWFCMinEntropyObserver TestWFCDistanceObserver \
                                        TestWFCSolver TestWFCPropagator TestWaveGrid \
                                        TestWFCTileCatalog TestWFCSocketOps TestWFCTileRegistryPacking \
                                        TestTileAdjacency
# Run each binary; expect all pass.
```

If `TestWFCMinEntropyObserver` doesn't exist as a separate target (we renamed in place in Task 2), skip it — `TestWFCObserver` already covers the migrated behavior.

- [ ] **Step 2: Run the existing WFC integration tests that consume the solver**

```bash
cmake --build build --target TestWFCRuinsRendering TestWFCStreaming TestWFCRuinsRestart \
                                        TestWFCRuinsSolveSmall TestWFCRuinsMixedCategories \
                                        TestWFCCategorySolve TestWFCRuinsAdjacencyConsistency
./build/EngineTest/IntegrationTests/TestWFCRuinsRendering
./build/EngineTest/IntegrationTests/TestWFCStreaming
# (etc., per the project's test runner convention)
```

Expected: all pass. Any failure here means the `unique_ptr` migration broke a non-obvious contract — investigate before proceeding.

- [ ] **Step 3: Run `TestKenneyTilePreview` end-to-end one final time**

```bash
./build/EngineTest/IntegrationTests/TestKenneyTilePreview
```

Smoke check: ≥ 100 cells collapsed AND ≥ 3 distinct tile ids in the final grid (existing smoke assertion). Plus the manual `O`/`P` hotkey verification from Task 7.

- [ ] **Step 4: No commit unless a nit was fixed**

If everything passes, no commit needed — Task 7 already captured the integration work. If a small fix was required (e.g., a missed `observer_.X` → `observer_->X` site surfaced by an integration test), commit it with a focused message.

---

## Self-Review

### Spec coverage check
Walk through each section of the spec (`Docs/superpowers/specs/2026-08-04-wfc-observer-strategy-design.md`) and confirm a task implements it:

| Spec section | Covered by |
|---|---|
| `WFCObserver` abstract base | Task 3 |
| `WFCMinEntropyObserver` migrated | Tasks 1, 2 |
| `WFCDistanceObserver` Euclidean + tie-break + empty latch | Task 5 |
| `WFCSolver::SetObserver` / `ResetToDefaultObserver` | Task 3 |
| `WFCSolver` default MinEntropy behavior preserved | Tasks 3 (impl), 4 (test) |
| Application-layer `ObserverKind` / `OriginPreset` enums | Task 7 |
| Hotkeys `O` / `P` | Task 7 |
| `ReseedSolver` constructs observer per kind | Task 7 |
| `ComputeOrigin` helper | Task 7 |
| 6 engine unit tests | Tasks 2 (3 tests migrated), 4 (2 tests), 5 (3 tests), 6 (1 test) = 9 tests total (≥ spec's 6) |
| Manual demo verification | Task 7 step 7, Task 8 step 3 |
| Files-affected list | File Structure table at top of plan |

No spec gaps.

### Placeholder scan
Search the plan for forbidden phrases:
- "TBD" — appears only for the follow-up spec filename in the spec doc, not in any task.
- "TODO" / "implement later" — none.
- "fill in details" / "Add appropriate error handling" — none.
- "..." in code blocks — appears only as C++ variadic padding inside `PrintGridState` etc. (cosmetic, not a placeholder).

All test bodies include full setup + assertions + expected output. No placeholders that would block an engineer.

### Type consistency check
- `WFCObserver` — abstract base, used identically in Tasks 3, 4, 5, 6, 7. ✓
- `WFCMinEntropyObserver` — concrete class, constructed via `std::make_unique<WFCMinEntropyObserver>()`. ✓
- `WFCDistanceObserver` — concrete class, constructor takes `WFCGridCoord origin`. ✓
- `WFCSolver::SetObserver` — takes `std::unique_ptr<WFCObserver>`. ✓
- `WFCSolver::ResetToDefaultObserver` — no args, returns void. ✓
- `ObserverKind` / `OriginPreset` — u32 enums, used consistently in Task 7. ✓
- `ComputeOrigin` — returns `WFCGridCoord`, no args. ✓

No type mismatches across tasks.

### Open questions for implementer
None. All tasks contain full code; no body is left as an exercise.

---

## Execution Handoff

**Plan complete and saved to `Docs/superpowers/plans/2026-08-04-wfc-observer-strategy.md`. Two execution options:**

**1. Subagent-Driven (recommended)** — dispatch a fresh subagent per task, two-stage review (spec + code quality) between tasks. Best for protecting the main context window and ensuring each task is independently validated.

**2. Inline Execution** — execute tasks in this session via executing-plans skill, batch execution with checkpoints for review. Best if you want to watch each step.

**Which approach?**
