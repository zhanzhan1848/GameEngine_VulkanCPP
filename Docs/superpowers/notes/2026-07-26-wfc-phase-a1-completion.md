# WFC Phase A.1 Completion Notes

**Date:** 2026-07-26
**Status:** Phase A.1 complete. Ready for Phase A.2 (Solver).
**Branch:** `feat/wfc-pcg`
**Commits:** 13 (ee63aac scaffold → 2f8b460 concurrency test)

## Delivered

### Source files (under `Engine/Graphics/WFC/`)
- **`WFCTypes.h`** — Core types: `wfc_tile_id`, `wfc_cell_id`, `wfc_socket_id` (via `DEFINE_TYPED_ID`); `WFCGridCoord`, `WFCTile`, `WFCCell`; `WFCStep` + `WFCStepKind`; `WFCFace` enum + `OppositeFace` constexpr (XOR-with-1) locked by 3 static_asserts.
- **`WaveGrid.{h,cpp}`** — Heap-allocated 3D cell grid (`utl::vector<WFCCell>`). Methods: `Initialize`, `CellAt` (mutable + const), `Resize`, `Reset`, `Size`, `CellCount`, `BytesPerCell`, `MaxTileVariants`, `Cells`, `CellsMutable`, private `CoordToIndex`.
- **`TileAdjacency.{h,cpp}`** — Socket compatibility store. 64-bit packed key (`[tile_a:16][variant_a:8][face:4][pad:4][tile_b:16][variant_b:16]`) in `std::unordered_set<u64>`. Mirror semantics via `OppositeFace`. `GetCompatible` uses self-documenting 28-bit prefix mask.
- **`WFCConfig.{h,cpp}`** — Settings struct with `Mode` enum (Independent2D/3D, Layered, SingleDomain), default member initializers, 7 reflection descriptors via `PCGParamDescriptor`.
- **`WFCSolveBudget.{h,cpp}`** — Frame-time/cell-count primitive. Short-circuit on cheap cell-count check before `steady_clock::now()` syscall. Reset clears both counters and timestamp.
- **`WFCStepBuffer.{h,cpp}`** — Thread-safe producer/consumer queue. `std::deque<WFCStep>` guarded by `std::shared_mutex` (`unique_lock` for Push/Consume, `shared_lock` for Empty).

### Test files (under `EngineTest/UnitTests/Graphics/WFC/`)
- `TestWaveGrid.cpp` — 7 cases (Initialize, CellAt, Reset, Resize grow/shrink/preserve)
- `TestTileAdjacency.cpp` — 5 cases (Add+Check+mirror, Incompatible, Clear, GetCompatible multi/none)
- `TestWFCConfig.cpp` — 3 cases (Default values, descriptor count, offset round-trip)
- `TestWFCSolveBudget.cpp` — 4 cases (Initial state, Reset, cell limit, time limit)
- `TestWFCStepBuffer.cpp` — 4 cases (Push/Consume, max-count cap, empty, 5k-item concurrency stress)

**Total: 23 test cases across 5 binaries, all passing.**

## Engine integration verified
- WFC sources picked up by `Engine/CMakeLists.txt:121-124` glob entries (added in Task 1)
- WFC tests registered in `EngineTest/UnitTests/CMakeLists.txt` (5 targets)
- All engine types used per spec section 10 (no `std::vector`, no raw u32 IDs)
- `math::v3` 16-byte alignment respected in `WFCTile.bounds_extents`
- All test binaries build cleanly with the existing engine library

## Known limitations to address in Phase A.2

- **`TileAdjacencyTable::GetCompatible`** uses linear scan over the unordered_set. Phase A.2 will replace with a lookup table when perf measurements justify it.
- **`WFCConfig`** has 7 descriptors. Phase A.2 will add more (entropy_heap_size, propagator_kind, etc.) once the solver takes shape.
- **`WFCCell::candidate_mask`** is u64 (64 candidates max). Future tiles with >64 variants need `utl::vector<u64>` extension.
- **`WaveGrid::Resize`** does not preserve cell state. This is intentional (solver state invalid for new topology) but documented as a contract.
- **`WFCSolveBudget`** calls `steady_clock::now()` on every `ShouldContinue` poll. Phase A.1 perf is fine; if collapse count grows large in later phases, consider a "dirty flag" optimization.
- **`WFCStepBuffer`** is single-producer/single-consumer (matches the documented threading contract). Multi-producer would need stronger synchronization.

## Phase A.1 commits (chronological)

```
ee63aac build(wfc): scaffold WFC directory and CMake glob entries
f55ee4a feat(wfc): add WFCTypes.h with core types and typed IDs
2af273c fix(wfc): correct WFCTile pad comment and lock WFCFace pairing
67b708e feat(wfc): WaveGrid Initialize/CellAt/Size with TDD tests
e7fe535 feat(wfc): WaveGrid runtime Resize with full cell reset
c1f1256 feat(wfc): WaveGrid Reset clears state, keeps size
ec4be9c feat(wfc): TileAdjacencyTable with Add/Check/Clear and mirror semantics
f90f53e feat(wfc): TileAdjacencyTable GetCompatible coverage + prefix_mask fix
81d7595 feat(wfc): WFCConfig struct with ParamDescriptor reflection
6fa1c7d feat(wfc): WFCSolveBudget frame-time/cell-count primitive
7dca1dc test(wfc): WFCSolveBudget cell-count and time-limit coverage
77c65e8 feat(wfc): WFCStepBuffer thread-safe producer/consumer queue
2f8b460 test(wfc): WFCStepBuffer producer/consumer concurrency coverage
```

## Next: Phase A.2 — WFC Solver

Scope: Observer + Propagator + RestartPolicy + WFCSolver integration. Builds on the foundation types and structures from Phase A.1. To be planned as a separate implementation plan.
