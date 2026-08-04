# WFC Observer Strategy Refactor — Design

**Date:** 2026-08-04
**Status:** Spec (awaiting user review)
**Owner:** Engine team
**Driving principle:** CLAUDE.md → Core Layer as Capability Provider

## Goal

Refactor `WFCObserver` from a concrete class (hardcoded min-entropy binary heap) into a pluggable strategy interface. Ship two standard implementations as core capabilities — `WFCMinEntropyObserver` (existing behavior, migrated) and `WFCDistanceObserver` (new, Euclidean). The Kenney tile showcase gains hotkeys to switch strategy and distance-observer origin at runtime, alongside the existing grid-size and reseed controls.

**Scope:** This spec covers the engine core refactor + Metal (native macOS) application-layer integration only. The Emscripten/WebGPU port for online demo deployment is a separate follow-up spec — see Out of Scope.

## Motivation

Today `WFCObserver` is a concrete class held by value inside `WFCSolver` (`observer_`). Its only selection strategy is min-entropy. Adding alternative strategies — e.g. distance-from-origin for radial-expansion demos — requires modifying engine source.

Per CLAUDE.md's "Core Layer as Capability Provider" principle, the selection-strategy *mechanism* belongs in core (it is a generic algorithm concern), while specific *parameter choices* (origin coordinates, which strategy for this demo) belong in the application layer.

## Architecture

**Core layer** (`Engine/Graphics/WFC/`) provides:
- `WFCObserver` — abstract base class (strategy interface)
- `WFCMinEntropyObserver` — current heap implementation, migrated verbatim
- `WFCDistanceObserver` — new strategy; picks the uncollapsed cell closest to a configured origin (Euclidean)
- `WFCSolver::SetObserver(std::unique_ptr<WFCObserver>)` — injection point

**Application layer** (`EngineTest/.../TestKenneyTilePreview`) provides:
- A local enum `ObserverKind { MinEntropy, DistanceFromOrigin }` for hotkey cycling
- A local enum `OriginPreset { Center, Corner, BottomCenter }` for cycling distance-observer origin
- Construction logic mapping enum + preset → concrete observer instance
- Hotkeys `O` (cycle strategy) and `P` (cycle origin preset); both trigger `ReseedSolver()`

The enum lives in the application, not the engine. The engine only sees `WFCObserver&`. This keeps the core API stable while letting applications expose whichever control surface they want.

## Components

### `WFCObserver` — abstract base class

```cpp
// Engine/Graphics/WFC/WFCObserver.h
#pragma once
#include "../../Common/CommonHeaders.h"
#include "WFCTypes.h"

namespace primal::graphics::wfc {
class WaveGrid;  // forward decl

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

Method semantics (unchanged from current behavior, just virtualized):
- `Initialize(grid)` — called once per solve at `WFCSolver::Initialize` time and again on each restart. Observer builds whatever state it needs.
- `OnCellChanged(c)` — notification that cell `c`'s candidate set changed. MinEntropy maintains its heap here; stateless strategies no-op.
- `PickNextCollapse(grid)` — returns the next cell to collapse, or `{-1,-1,-1}` when no uncollapsed cell remains.
- `Empty()` — true when the observer has no candidate to propose; used by the solver's "done before next Pick" short-circuit.

### `WFCMinEntropyObserver` — migrated heap implementation

```cpp
// Engine/Graphics/WFC/WFCMinEntropyObserver.h
class WFCMinEntropyObserver : public WFCObserver {
public:
    void Initialize(const WaveGrid& grid) override;
    void OnCellChanged(WFCGridCoord c) override;
    WFCGridCoord PickNextCollapse(const WaveGrid& grid) override;
    bool Empty() const override;
private:
    std::vector<std::pair<u8, WFCGridCoord>> heap_;
};
```

Implementation: byte-for-byte the current `WFCObserver.cpp` body (binary min-heap with lazy deletion). Migration is mechanical.

### `WFCDistanceObserver` — new radial-expansion strategy

```cpp
// Engine/Graphics/WFC/WFCDistanceObserver.h
class WFCDistanceObserver : public WFCObserver {
public:
    explicit WFCDistanceObserver(WFCGridCoord origin);
    void Initialize(const WaveGrid& grid) override;
    void OnCellChanged(WFCGridCoord c) override;        // no-op
    WFCGridCoord PickNextCollapse(const WaveGrid& grid) override;
    bool Empty() const override;
private:
    WFCGridCoord origin_;
    WFCGridCoord grid_size_{0,0,0};
    bool empty_{false};
};
```

`PickNextCollapse` algorithm:
- Linear scan over all cells. Track the uncollapsed, non-contradiction (entropy > 0) cell minimizing Euclidean distance squared `(dx² + dy² + dz²)` to `origin_`. Squared distance avoids `sqrt` without changing the comparison result.
- Tie-breaking: first cell encountered in row-major scan order wins. Deterministic.
- Once no candidate is found, `empty_` is latched to true; subsequent calls return `{-1,-1,-1}` without rescanning. `Initialize` resets the latch.

Complexity: O(N) per `PickNextCollapse`, where N = `grid.x × grid.y × grid.z`. For the demo's 16×4×16 = 1024 cells this is negligible. Documented as a performance regression point for very large grids (future work: priority queue keyed by distance).

### `WFCSolver` — holder changes

```cpp
// Engine/Graphics/WFC/WFCSolver.h
class WFCSolver {
public:
    // ... existing API unchanged (Initialize, Step, Generation, etc.) ...

    // NEW — inject a custom observer. Must be called before Initialize.
    // nullptr is rejected (use ResetToDefaultObserver() to restore default).
    void SetObserver(std::unique_ptr<WFCObserver> observer);

    // NEW — restore the default MinEntropy observer.
    void ResetToDefaultObserver();

private:
    std::unique_ptr<WFCObserver> observer_;  // was: WFCObserver observer_;
    // ... rest unchanged ...
};
```

Constructor: `observer_ = std::make_unique<WFCMinEntropyObserver>()`. This preserves the existing default behavior — any test or fixture relying on `WFCSolver solver; solver.Initialize(...)` keeps working without changes.

`Initialize` and `Step`: dereference `observer_->X()` instead of `observer_.X()`. No other changes.

## Application Layer Integration

### New state on `KenneyTilePreviewTestCase`

```cpp
enum class ObserverKind : u32 { MinEntropy = 0, DistanceFromOrigin = 1 };
enum class OriginPreset : u32 { Center = 0, Corner = 1, BottomCenter = 2 };

ObserverKind  observer_kind_{ObserverKind::MinEntropy};
OriginPreset  origin_preset_{OriginPreset::Center};
```

### Hotkey additions

| Key | Edge-triggered action |
|---|---|
| `O` | Cycle `observer_kind_` (MinEntropy → DistanceFromOrigin → MinEntropy). Triggers `ReseedSolver()`. |
| `P` | Cycle `origin_preset_` (Center → Corner → BottomCenter → Center). Triggers `ReseedSolver()` regardless of current `observer_kind_`; the preset is simply ignored by MinEntropy. |

Rationale for `P` triggering even under MinEntropy: keeps the hotkey behavior uniform ("any change → reseed"). The preset is part of the application state and applies the next time the user switches to DistanceFromOrigin.

Existing hotkeys (`[`/`]`, `,`/`.`, `-`/`+`, `R`, `T`) unchanged.

### `ReseedSolver` integration

```cpp
void KenneyTilePreviewTestCase::ReseedSolver() {
    DestroyAllSpawnedEntities();
    solver_.reset();
    grid_ = std::make_unique<WaveGrid>(WFCGridCoord{(s32)grid_w_, (s32)grid_h_, (s32)grid_d_});
    buf_  = std::make_unique<WFCStepBuffer>();

    std::unique_ptr<WFCObserver> observer;
    switch (observer_kind_) {
        case ObserverKind::MinEntropy:
            observer = std::make_unique<WFCMinEntropyObserver>();
            break;
        case ObserverKind::DistanceFromOrigin:
            observer = std::make_unique<WFCDistanceObserver>(ComputeOrigin());
            break;
    }

    solver_ = std::make_unique<WFCSolver>();
    solver_->SetObserver(std::move(observer));
    solver_->Initialize(config_, *grid_, *registry_, *adjacency_, *buf_);

    solver_done_ = false;
    total_collapses_ = 0;
    total_restarts_ = 0;
}

WFCGridCoord KenneyTilePreviewTestCase::ComputeOrigin() const {
    switch (origin_preset_) {
        case OriginPreset::Center:       return {(s32)grid_w_/2, (s32)grid_h_/2, (s32)grid_d_/2};
        case OriginPreset::Corner:       return {0, 0, 0};
        case OriginPreset::BottomCenter: return {(s32)grid_w_/2, 0, (s32)grid_d_/2};
    }
    return {(s32)grid_w_/2, (s32)grid_h_/2, (s32)grid_d_/2};
}
```

Destruction-order note: `solver_` must be reset before `grid_` / `buf_` because the solver holds raw pointers into them.

## Data Flow

Runtime observer switch:

1. User presses `O` (or `P`).
2. `HandleGridEditKeys` detects the edge-triggered press.
3. The relevant enum cycles.
4. `ReseedSolver()` runs immediately:
   - All currently-spawned WFC entities are destroyed.
   - Old solver / grid / buf are dropped.
   - A fresh grid is created in the fully-unobserved state.
   - A new observer is constructed from the current enums.
   - A new solver is constructed, `SetObserver(...)`, `Initialize(...)`.
5. The next `PumpSolverFrame()` collapses cells from scratch using the new strategy.

## Error Handling

- `WFCSolver::SetObserver(nullptr)` — assert in debug, no-op in release. Caller must pass a valid observer.
- `WFCSolver` constructed without `SetObserver` — uses the default `WFCMinEntropyObserver`. All existing tests/fixtures keep working.
- `WFCDistanceObserver::PickNextCollapse` on an all-collapsed grid — returns `{-1,-1,-1}`, latches `empty_=true`.
- `WFCDistanceObserver::PickNextCollapse` when all remaining cells are contradictions (entropy 0) — same: returns `{-1,-1,-1}`.

## Testing

### Engine unit tests

New tests under `EngineTest/UnitTests/Graphics/WFC/` (file names TBD by implementer):

1. **MinEntropy_PreservesExistingBehavior** — port the existing observer tests to construct `WFCMinEntropyObserver`. Validates the migration is mechanical.
2. **Distance_PicksClosestFirst** — set up a 3×1×3 grid with all tiles mutually compatible; origin = (1,0,1); collapse order should be (1,0,1) → 4-neighbor ring at d=1 → 4-corner ring at d=√2 → done.
3. **Distance_TieBreakRowMajor** — grid with equidistant uncollapsed cells; verify row-major scan order produces the documented tie-break.
4. **Distance_AllCollapsedReturnsInvalid** — call `PickNextCollapse` after full collapse; expect `{-1,-1,-1}` and `Empty()==true`.
5. **Solver_SetObserver_ReplacesDefault** — construct solver, `SetObserver` with a distance observer, Initialize, Step until done; verify the collapse order matches distance-from-origin (not min-entropy).
6. **Solver_DefaultObserverIsMinEntropy** — construct solver, Initialize without `SetObserver`; verify behavior matches `WFCMinEntropyObserver`.

### Application-layer verification

Manual, on `TestKenneyTilePreview`:
- Press `O` mid-generation; the solver resets; the collapse pattern visibly switches between min-entropy (no particular order) and radial-outward.
- Press `P` while in DistanceFromOrigin; the radial center moves (center → corner → bottom-center).
- Press `O` while changing nothing else; the reseed is observable via the spawn-from-scratch progression.

## Performance Notes

| Observer | `PickNextCollapse` cost | Constant factors |
|---|---|---|
| MinEntropy | O(log N) amortized | heap maintenance in `OnCellChanged` |
| Distance | O(N) | pure scan; no `OnCellChanged` work |

For the demo's N = 1024, both are negligible. Distance becomes a regression candidate only for grids ~32×8×32 = 8192 cells or larger — flagged as future work.

## Files Affected

### Engine core — `Engine/Graphics/WFC/`
| Op | File | Notes |
|---|---|---|
| Rewrite | `WFCObserver.h` | Concrete class → abstract base (pure virtuals) |
| Delete | `WFCObserver.cpp` | No standalone impl under the abstract base |
| New | `WFCMinEntropyObserver.h` | Concrete subclass declaration |
| New | `WFCMinEntropyObserver.cpp` | Migrated heap implementation |
| New | `WFCDistanceObserver.h` | Concrete subclass declaration |
| New | `WFCDistanceObserver.cpp` | Linear-scan Euclidean strategy |
| Modify | `WFCSolver.h` | `observer_` → `unique_ptr`; add `SetObserver` / `ResetToDefaultObserver` |
| Modify | `WFCSolver.cpp` | Dereference as `observer_->X()` |

### Build files
| Op | File | Notes |
|---|---|---|
| Modify | `Engine/Graphics/CMakeLists.txt` | Add the 4 new files |
| Modify | Engine Xcode / vcxproj if present | Same |

### Application layer — `EngineTest/IntegrationTests/Graphics/WFC/`
| Op | File | Notes |
|---|---|---|
| Modify | `TestKenneyTilePreview.h` | Add `ObserverKind` / `OriginPreset` enums + new members |
| Modify | `TestKenneyTilePreview.cpp` | Hotkeys `O`/`P`; `ReseedSolver` constructs observer per kind; `ComputeOrigin` helper |

### Tests
| Op | File | Notes |
|---|---|---|
| New | `EngineTest/UnitTests/Graphics/WFC/TestWFCMinEntropyObserver.cpp` | Migration validation |
| New | `EngineTest/UnitTests/Graphics/WFC/TestWFCDistanceObserver.cpp` | Distance strategy behavior |
| Modify or new | `EngineTest/UnitTests/Graphics/WFC/TestWFCSolver.cpp` (or similar existing) | `SetObserver` integration tests |

## Out of Scope / Future Work

- **WASM/Emscripten port of TestKenneyTilePreview** — deferred to a separate follow-up spec (task #126, file TBD). The follow-up covers:
  - Platform input bridging between `primal::input::get` and `EmscriptenInput`'s `EmscriptenGetKeyState`
  - Asset handling: embed or preload the `kenney_dungeon_tiles/` directory + `colormap.png` atlas into MEMFS
  - WASM target wiring in root `CMakeLists.txt` mirroring the `TestDawnWASM` pattern (`add_executable(TestKenneyTilePreviewWASM ...)` under `if(EMSCRIPTEN AND ENABLE_WEBGPU)`)
  - Online-deployment `shell.html` (canvas + HUD showing current observer strategy + origin preset)
  - Deployment to the production web environment

  Dependency: this spec must land first — the WASM demo needs the distance observer to be a meaningful showcase of the strategy-switching capability.

- **Other strategies** (BFS, random, weighted, learned priors): not needed for current demos. Adding them is purely additive — new subclasses, no solver changes (open-closed).
- **Distance observer perf optimization** (priority queue keyed by distance, pre-sorted order): deferred until grids grow large enough to matter.
- **Visual debug overlay** showing current observer + origin in the running window: separate UX task; not blocking.
- **Serialization of observer state**: not required; observers are reconstructed on each solve.
- **`OriginPreset` as engine-provided enum**: stays in the application layer. Engine just takes a `WFCGridCoord`.

## Open Questions

None. All three core design choices are resolved:
- Abstraction form: abstract base class + concrete subclasses (chosen over enum-switch and std::function for openness to extension)
- Distance metric: Euclidean (squared, no sqrt in the hot path)
- Switch semantics: immediate solver reset (chosen over "continue with new strategy")

## References

- `CLAUDE.md` → Core Layer as Capability Provider
- `Engine/Graphics/WFC/WFCObserver.h` (current concrete class, to be rewritten)
- `Engine/Graphics/WFC/WFCObserver.cpp` (current heap implementation, to be migrated into `WFCMinEntropyObserver.cpp`)
- `Engine/Graphics/WFC/WFCSolver.h:99` (current `observer_` member, to become `unique_ptr`)
- `Engine/Graphics/WFC/WFCSolver.cpp:55,174,198,213` (call sites to update)
- `EngineTest/IntegrationTests/Graphics/WFC/TestWFCRuinsRendering.cpp` (reference for existing observer behavior under `WFCMinEntropyObserver`)
- `EngineTest/IntegrationTests/Graphics/WFC/TestKenneyTilePreview.cpp` (application layer to extend)
