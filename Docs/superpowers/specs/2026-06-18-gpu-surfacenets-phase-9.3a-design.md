# Phase 9.3a — GPU SurfaceNets on PCGField

**Status:** Design (awaiting implementation plan)
**Date:** 2026-06-18
**Branch:** `features/nanite_lumen`
**Prerequisite:** Phase 9.1 (CPU SurfaceNets MVP) — completed and visually verified

---

## 1. Context

Phase 9.1 delivered a working CPU SurfaceNets kernel (`Engine/Graphics/PCG/MarchingCubes.cpp`) emitting `RHIMeshAsset` via `MarchingCubesNode`. Performance at 64³ is ~5ms on Apple Silicon; 128³ is ~60ms — both too slow for real-time terrain use cases.

The broader goal is a modern PCG meshing pipeline supporting three use cases:
1. **Real-time terrain** — large smooth surfaces, perf-critical, GPU compute required
2. **Hard-surface props** — sharp feature preservation (Dual Contouring), quality-critical
3. **Authoring bake** — offline, quality-critical, CPU acceptable

Use case 3 is already served by Phase 9.1. This spec addresses use case 1 by moving SurfaceNets to GPU compute, establishing the GPU meshing infrastructure that subsequent phases (GlobalSDF direct bind, Dual Contouring, adaptive LOD) build on.

**Roadmap position:**
- ✅ 9.1 CPU SurfaceNets (done)
- **▶ 9.3a GPU SurfaceNets on PCGField (this spec)**
- 9.3b GlobalSDF direct bind + GPU-resident mesh output (eliminate readback round-trip)
- 9.4 Dual Contouring (extends `PCGField` with `SampleGradient`)
- 9.5 Adaptive LOD + mesh simplification

---

## 2. Goals (Phase 9.3a scope)

**Functional:**
- Implement SurfaceNets as 4 GPU compute passes consuming a scalar volume buffer
- Output is `RHIMeshAsset` (same content_id path as CPU)
- `MarchingCubesNode.algorithm` enum: `0=SurfaceNets_CPU, 1=SurfaceNets_GPU, 2=ClassicMC(future)`
- Both CPU and GPU paths coexist; choice per-node via `algorithm` param
- Automatic fallback to CPU when no RHI device is available (test environment)

**Performance targets:**

Phase 9.3a is **infrastructure-first**, not raw-perf-first. The dominant cost in this phase is Pass 0 (CPU-side `PCGField::SampleFloat` loop), which cannot be removed while `PCGField` remains a CPU virtual. Phase 9.3b removes Pass 0 by binding GlobalSDF directly; until then, GPU wins grow with resolution.

| Resolution | CPU baseline | 9.3a total (Pass 0 + GPU + readback) | 9.3a speedup | 9.3a GPU-only (Passes 1-4) |
|---|---|---|---|---|
| 64³ | ~5ms | ~4.3ms | ~1.2x | ~1.1ms |
| 128³ | ~60ms | ~32ms | ~2x | ~6ms |
| 256³ (if cap raised) | ~480ms | ~140ms | ~3.5x | ~50ms |

**What 9.3a delivers:**
- GPU compute infrastructure (`GPUMesher`, 4-pass pipeline, atomic-append pattern) reusable by 9.3b/9.4/9.5
- Modest speedup that scales with resolution (1.2x → 2x → 3.5x)
- Drop-in replacement for CPU path; no rendering pipeline changes
- Foundation that 9.3b will exploit for true real-time terrain

**What 9.3a does NOT deliver (explicitly):**
- Real-time (>30fps) terrain at 64³ — Pass 0 prevents this; it's a 9.3b outcome
- Uniform 3-5x speedup across all resolutions

**Output contract:**
- Identical `RHIMeshAsset` layout as CPU path: position buffer (`f32[3]` interleaved), element buffer (20B `static_normal_texture` packed via `content::WriteVertex` logic), index buffer (`u32`)
- Renderable via `PipelineRegisterMeshEntity` with no changes
- Compatible with Nanite mesh pipeline (which reads CPU-side `RHIMeshAsset` buffers)

---

## 3. Out of scope (deferred to future specs)

- **GlobalSDF direct bind** (Phase 9.3b) — eliminates PCGField CPU sampling + upload + GPU→CPU readback
- **Dual Contouring** (Phase 9.4) — sharp feature preservation via `PCGField::SampleGradient` API
- **Adaptive LOD / octree voxel structure** (Phase 9.5)
- **Classic Marching Cubes** variant (originally Phase 9.2) — SurfaceNets topology proven sufficient, dropped
- **PCGField GPU-side representation** (`PCGGridField` / `PCGGPUField`) — would expand scope; deferred
- **GPU-resident mesh output** (skip CPU round-trip) — requires changes to Nanite mesh pipeline; deferred to 9.3b
- **Frame-delayed readback** — Phase 9.3a accepts synchronous `waitUntilCompleted`; 9.3b can optimize
- **RHI GPU timer query API** (`MTLCounterSampleBuffer` / Vulkan `vkCmdWriteTimestamp` abstraction) — Phase 9.3a measures via wall-clock around the blocking call (within ~5% of true GPU time); 9.3b adds proper per-pass timer for finer profiling

---

## 4. Architecture

### 4.1 Data flow

```
PCGField (CPU virtual)
  │
  ▼ [Host] CPU sample (res+1)³ grid vertices
std::vector<f32> scalar (CPU)
  │
  ▼ [Host] Upload once
GPU buffer A: scalar_volume        (f32[(res+1)³])
  │
  ▼ [Compute Pass 1] Cell classification (dispatch res³)
GPU buffer B: dual_id[cell]        (u32, UINT_MAX if not straddling)
GPU buffer F[0]: vertex_counter    (atomic_uint, append)
  │
  ▼ [Compute Pass 2] Dual vertex generation (dispatch res³)
GPU buffer C: position_buffer      (f32×3 per vertex)
GPU buffer D: element_buffer       (20B static_normal_texture per vertex)
  │
  ▼ [Compute Pass 3] Face emission (3 dispatches, one per axis X/Y/Z)
GPU buffer E: index_buffer         (u32)
GPU buffer F[1]: index_counter     (atomic_uint, append)
  │
  ▼ [Compute Pass 4] Indirect draw setup (1 thread)
GPU buffer G: indirect_draw_args
  │
  ▼ [Host] waitUntilCompleted + readback (2 u32: vert_count, idx_count)
  │
  ▼ [Host] Blit GPU buffers C/D/E → RHIMeshAsset CPU buffers
RHIMeshAsset
  │
  ▼ content::register_mesh_asset
content_id → PCGGeometryData → PipelineRegisterMeshEntity (existing path)
```

### 4.2 Buffer sizes & memory budget

For a resolution `R` (cells per axis), worst-case buffer sizes:

| Buffer | Element | Count | Bytes @ R=64 | Bytes @ R=128 |
|---|---|---|---|---|
| A: scalar_volume | f32 | (R+1)³ | 275K × 4 = 1.1MB | 2.1M × 4 = 8.5MB |
| B: dual_id | u32 | R³ | 262K × 4 = 1.0MB | 2.1M × 4 = 8.4MB |
| C: position_buffer | f32×3 | R³ (worst case) | 262K × 12 = 3.1MB | 2.1M × 12 = 25MB |
| D: element_buffer | 20B | R³ (worst case) | 262K × 20 = 5.2MB | 2.1M × 20 = 42MB |
| E: index_buffer | u32 | R³ × 3 (worst case) | 786K × 4 = 3.1MB | 6.3M × 4 = 25MB |
| F: counters | u32 × 2 | 2 | 8 B | 8 B |
| G: indirect_draw_args | struct | 1 | ~64 B | ~64 B |
| **Total worst-case** | | | **~13.5MB** | **~109MB** |

In practice vertex count is far below R³ (only straddling cells emit; typically 5-15% of cells). Allocation uses worst-case sizes to avoid resize mid-frame. Buffers are allocated per Execute in Phase 9.3a; pooling optimization deferred.

### 4.3 Why 4 passes (not 1 fused kernel)

- Each pass has a distinct per-thread device-read pattern; Apple Silicon requires careful budget management
- Pass 1 and Pass 3 use atomic-append on independent counters; must be separate dispatches
- Per-pass output buffers are debug-dumpable (verification easier)
- Pass 3 fans out to 3 axis-specific dispatches (cell topology differs per axis; merging would create branchy kernel)

### 4.4 Apple Silicon constraints (from memory)

| Constraint | Mitigation in this design |
|---|---|
| `texture3D` ~4 read limit per thread | Scalar volume stored as **RHI buffer**, not 3D texture |
| 16 stable buffer reads per thread | Pass 1: 8 reads, Pass 2: 8 reads, Pass 3: 6 reads — all within budget |
| Compute >32 device reads per thread flicker | Per-thread reads well below threshold |
| `atomic_uint` on buffer required | Pattern already proven in DDGI probe update (`ddgi-probe-update-list-race.md`) |

### 4.5 Device access pattern

`PCGNode::Execute()` has no device parameter and modifying `PCGGraph::Execute` signature affects every node — violates minimal impact. Use a singleton mirroring `GlobalSDF::Get()`:

```cpp
class GPUMesher {
public:
    static GPUMesher& Get();
    void Initialize(rhi::RHIDeviceBase* device);  // Called by RenderSystem::Initialize
    void Shutdown();                                // Called by RenderSystem::Shutdown
    bool IsReady() const { return device_ != nullptr; }

    MarchingCubesResult GenerateSurfaceNets(
        const PCGField& field,
        const math::v3& bounds_min,
        const math::v3& bounds_max,
        u32 resolution,
        f32 iso_value);
private:
    rhi::RHIDeviceBase* device_{nullptr};
    // Cached compute pipelines, samplers, etc. populated on first use.
};
```

Singleton holds only the device pointer and cached pipeline state. No render state. Lifetime bound to `RenderSystem`.

---

## 5. Compute passes

### Pass 0 — CPU sample + upload (host, not compute)

Host-side loop identical to current CPU SurfaceNets (`Engine/Graphics/PCG/MarchingCubes.cpp:91-99`):
```cpp
std::vector<f32> scalar(n * n * n);
for (k...) for (j...) for (i...) {
    math::v3 p = GridIndexToWorld(i, j, k, bounds_min, voxel);
    scalar[i + n*j + n2*k] = field.SampleFloat(p);
}
device->UpdateBufferData(scalar_volume_buffer, scalar.data(), scalar.size() * 4);
```

**This is the dominant cost for 64³** (~3ms of the ~5ms CPU baseline). Cannot be eliminated while `PCGField::SampleFloat` is a CPU virtual. Phase 9.3b removes this by binding GlobalSDF directly.

### Pass 1 — Cell classification

- **Dispatch:** `resolution × resolution × resolution` threads
- **Per-thread reads:** 8 corner samples from `scalar_volume`
- **Per-thread writes:** `dual_id[cell_index]` (vertex ID or `UINT_MAX`), atomic increment of `vertex_counter`
- **Logic:**
  ```
  for c in 0..7: cv[c] = scalar[corner(c)]; mask |= (cv[c] > iso) << c
  if mask == 0 || mask == 0xFF: dual_id = UINT_MAX; return
  vid = atomic_add(vertex_counter, 1)
  dual_id[cell] = vid
  ```
- **Buffer budget:** 8 / 16 ✓

### Pass 2 — Dual vertex generation

- **Dispatch:** `resolution³` threads (same as Pass 1; threads with `dual_id == UINT_MAX` early-out)
- **Per-thread reads:** 8 corner samples (re-read `scalar_volume`, no caching between passes)
- **Position computation:** Average of edge crossings — same as CPU `MarchingCubes.cpp:128-149`
- **Normal computation — cell-center finite differences** (simplified from CPU's dual-position gradient):

  CPU version samples field 6 extra times at `dual_pos ± voxel`. GPU version approximates gradient at cell center using the 8 corner samples already loaded:
  ```
  gx = avg(cv[+X face corners]) - avg(cv[-X face corners])
  gy = avg(cv[+Y face corners]) - avg(cv[-Y face corners])
  gz = avg(cv[+Z face corners]) - avg(cv[-Z face corners])
  normal = normalize(gx, gy, gz)
  ```
  This is 0 extra reads (reuses the 8 corners). Visually smoother than CPU in some cases (no dual-position jitter), slightly less precise at edges.

  **Future (Phase 9.4 prep):** When `PCGField::SampleGradient` lands, both CPU and GPU paths can use analytic gradient.

- **UV:** Y-planar projection (matches CPU): `u = (pos.x - min.x) / extent.x; v = (pos.z - min.z) / extent.z`
- **Element buffer packing:** Identical layout to `content::WriteVertex` — extract the packing logic to a shared helper to avoid duplication
- **Buffer budget:** 8 / 16 ✓

### Pass 3 — Face emission (3 axis-specific dispatches)

Each dispatch iterates interior grid edges along one axis (X, Y, Z). For axis A:
- **Dispatch:** `n × n × n` threads, early-out when `gp[A] + 1 >= n`
- **Per-thread reads:** 2 scalar endpoints (sign change check) + 4 `dual_id` values (surrounding cells) = 6 reads
- **Winding order:** Must match CPU `make_cell` offset sequence `(0,0), (-1,0), (-1,-1), (0,-1)` exactly (CPU `MarchingCubes.cpp:268-273`). Get this wrong → all triangles back-face culled.
- **Triangulation:** Fan triangulation of valid polygon (matches CPU `emit_face` at `MarchingCubes.cpp:198-216`)
- **Atomic append:** For each emitted triangle, `atomic_add(index_counter, 3)` then write 3 indices
- **Buffer budget:** 6 / 16 ✓

### Pass 4 — Indirect draw setup

- **Dispatch:** 1 thread
- Reads `vertex_counter` and `index_counter`, writes a `MTLDrawPrimitivesIndirectCommand` (or engine equivalent) to `indirect_draw_args` buffer
- This pass is unused in Phase 9.3a (we do readback and use `register_mesh_asset`), but the data structure is needed in Phase 9.3b for direct GPU rendering. Writing it now costs nothing.

### Inter-pass synchronization

Each pass uses its own `MTLComputeCommandEncoder`. Encoder end implies memory coherence in Metal. The `vertex_counter` / `index_counter` readback after Pass 4 requires `[command_buffer waitUntilCompleted]` before host can read.

**`GPUMesher::GenerateSurfaceNets` is a synchronous blocking call** for Phase 9.3a. `PCGNode::Execute()` is already synchronous, so this fits.

---

## 6. MarchingCubesNode integration

```cpp
class MarchingCubesNode : public PCGNode {
public:
    // ... existing params ...
    u32 algorithm{0};  // 0=SurfaceNets_CPU, 1=SurfaceNets_GPU, 2=ClassicMC(future)

    void Execute() override {
        DestroyTrackedAsset();
        auto* field = inputs[0].AsField();
        if (!field) return;

        u32 res = resolution;
        if (res < 2) res = 2;
        if (res > 128) res = 128;

        MarchingCubesResult mesh = (algorithm == 1 && GPUMesher::Get().IsReady())
            ? GPUMesher::Get().GenerateSurfaceNets(*field, bounds_min, bounds_max, res, iso_value)
            : GenerateSurfaceNetsCPU(*field, bounds_min, bounds_max, res, iso_value);

        if (mesh.positions.empty() || mesh.indices.empty()) {
            auto* out = CreateOutput<PCGGeometryData>(0);
            out->content_id = id::invalid_id;
            return;
        }

        BuildAndRegisterAsset(mesh);  // Extract existing asset-build code to method
        auto* out = CreateOutput<PCGGeometryData>(0);
        out->content_id = last_created_id_;
    }

    // ...
};
```

**Refactor:** Existing `Execute()` body that builds `RHIMeshAsset` from `MarchingCubesResult` is extracted to a new private method `BuildAndRegisterAsset(const MarchingCubesResult&)`. Both CPU and GPU paths share this — the only divergence is which kernel produces the `MarchingCubesResult`.

**Rename:** Current free function `GenerateSurfaceNets` → `GenerateSurfaceNetsCPU` for symmetry. Update the one call site in `MarchingCubesNode::Execute`.

**Reflection update:** `algorithm` param's enum string changes from `"SurfaceNets,ClassicMC"` to `"SurfaceNets_CPU,SurfaceNets_GPU,ClassicMC"`. Editor UI dropdown grows by one entry.

**C ABI:** No changes. `PCGSetNodeParamFloat(node_id, "algorithm", 1.0f)` selects GPU path. `PCGGetOutputGeometry` reads through transparently.

---

## 7. File layout & CMake additions

```
Engine/Graphics/PCG/
├── MarchingCubes.h/cpp              ← rename function to GenerateSurfaceNetsCPU
├── MarchingCubesGPU/                ← new subdirectory
│   ├── GPUMesher.h/cpp              ← singleton, device binding, top-level dispatch
│   ├── SurfaceNetsGPU.cpp           ← host-side pass orchestration (buffer alloc, dispatch, readback)
│   └── shaders/
│       └── SurfaceNetsGPU.metal     ← 4 compute functions:
│                                      classify_cells, emit_vertices,
│                                      emit_faces_<axis>, write_indirect_args
├── Nodes/
│   └── MarchingCubesNode.h          ← Execute() updated to dispatch CPU/GPU
```

**CMake additions** (`Engine/CMakeLists.txt`):
- `MarchingCubesGPU/GPUMesher.cpp` to SOURCE_FILES
- `MarchingCubesGPU/SurfaceNetsGPU.cpp` to SOURCE_FILES
- `MarchingCubesGPU/shaders/SurfaceNetsGPU.metal` — picked up by existing metallib build glob

**Shared packing helper:** Extract the body of `content::WriteVertex` (from `Engine/Content/ProceduralMesh.h`) that packs `colorTSign | Normal | Tangent | UV` into 20 bytes to a free function `content::PackVertexElement(u8* dst, f32 nx, f32 ny, f32 nz, f32 u, f32 v)` in the same header. `WriteVertex` becomes a thin wrapper that calls `PackVertexElement` after writing position. The Metal shader defines an equivalent `pack_vertex_element` function with the same constants (`INV_INTERVALS = 2.f/65535`) and identical bit layout (documented in `procedural-vertex-format.md` memory).

---

## 8. Testing

### Test environment caveat

`TestMediatedDataFlow` is headless and does **not** initialize the RHI device. This means headless tests can only validate the **fallback path** (algorithm=1 + no device → CPU execution). True GPU path validation requires a device-initializing test — see Section 8.3.

### 8.1 Headless fallback tests (`EngineTest/IntegrationTests/TestMediatedDataFlow.cpp`)

Add three sub-tests to the existing `MarchingCubes API Tests` suite:

1. **`TestGPUSurfaceNetsNoiseField`**
   - Build a NoiseField → MarchingCubes graph with `algorithm=1`
   - Call `PCGExecute`
   - Assert `PCGGetOutputGeometry` returns a valid content_id (≠ `INVALID_CONTENT_ID`)
   - In headless env (no device): exercises CPU fallback. In device-init env: exercises GPU path. Either way the contract (valid content_id) is the same.

2. **`TestGPUMesherFallback`**
   - Do not call `GPUMesher::Initialize` (simulates headless test environment)
   - Build a MarchingCubes node with `algorithm=1`
   - Execute and verify `GPUMesher::Get().IsReady() == false` and output is non-empty
   - This explicitly documents and enforces the fallback contract

3. **`TestGPUMesherAlgorithmSwitch`**
   - Build identical graph twice: `algorithm=0` and `algorithm=1`
   - In headless env both produce identical CPU output (no device → fallback); assert vertex counts match exactly
   - In device env (when run as part of live test infra): assert within ±5% (cell-center gradient vs dual-position gradient causes minor differences)

### 8.2 Live manual visual test (`EngineTest/IntegrationTests/TestPCGScatter.cpp`)

- Add key `4` (currently declared as `key_4_pressed_` but check current usage in .cpp; if in use, pick a free key) to toggle `algorithm` 0↔1 on the MarchingCubes node
- Update title bar to show `[CPU SurfaceNets]` or `[GPU SurfaceNets]`
- Existing `ReExecuteMarchingCubes` re-runs the MC node with the new algorithm
- Visual check: same iso/bounds/resolution, CPU and GPU surfaces should look approximately identical (slight normal smoothing differences acceptable)
- This is human-in-the-loop verification — not a substitute for the automated test in 8.3

### 8.3 GPU rendering integration test (`EngineTest/IntegrationTests/TestGPUMesherIntegration.cpp` — NEW FILE)

**This is the only test that actually exercises the GPU code path automatically.** Pattern after `TestRenderFrameAPI.cpp` which already has the full device + RenderFrame + CaptureBackbuffer lifecycle.

**CMake additions** (`EngineTest/IntegrationTests/CMakeLists.txt`):
- New executable `TestGPUMesherIntegration` with sources `TestGPUMesherIntegration.cpp`, `Main.cpp`, `MacKeyboard.mm`, `ShaderCompilation.cpp`, COMMON_HEADERS
- `setup_test_target(TestGPUMesherIntegration)` + `target_link_libraries(TestGPUMesherIntegration PRIVATE EngineDLL ${CMAKE_DL_LIBS})`
- Compile define: `TEST_GPU_MESHER_INTEGRATION=1`
- Add to `IntegrationTests` pseudo-target DEPENDS list

**Sub-tests (single binary, multiple cases):**

1. **`TestGPUSurfaceNetsRenderBasic`** — proves GPU path produces renderable geometry
   ```
   InitializeEngine() → 拿 device,GPUMesher::Get().IsReady() == true
   Build PCG graph: NoiseField → MarchingCubes(algorithm=1, res=64)
   PCGExecute → content_id
   PipelineRegisterMeshEntity(content_id) → entity_id (≠ 0)
   AddEntityCamera + AddEntityLight (basic forward setup)
   RenderFrame × 3 (warm up pipeline)
   CaptureBackbuffer → RGBA buffer
   Assert: mean_brightness > 0.05 (非全黑)
   Assert: pixel_variance > threshold (非平铺色,证明几何真的渲染了)
   PipelineUnregisterMeshEntity + PCGDestroyGraph + ShutdownEngine
   ```

2. **`TestGPUSurfaceNetsRenderVsCPU`** — proves GPU output ≈ CPU output
   ```
   InitializeEngine
   Run TestGPUSurfaceNetsRenderBasic flow with algorithm=0 → capture buffer A
   Run TestGPUSurfaceNetsRenderBasic flow with algorithm=1 → capture buffer B
   Compute per-channel histogram for A and B
   Assert: histogram correlation > 0.85(法向略不同 + 法线打包量化会导致像素级差异,
                                          但分布应该相似)
   ```
   Note: pixel-exact match is NOT expected (cell-center gradient vs dual-pos gradient).
   Histogram correlation catches gross divergence (winding errors, missing geometry,
   totally wrong normals).

3. **`TestGPUSurfaceNetsPerf`** — guards against perf regression
   ```
   InitializeEngine
   For res in {64, 128}:
     Build NoiseField → MarchingCubes(algorithm=1, res)
     // Wall-clock around blocking GenerateSurfaceNets call.
     // Since waitUntilCompleted blocks, wall clock ≈ GPU time (within ~5%).
     auto t0 = std::chrono::high_resolution_clock::now();
     PCGExecute();
     auto t1 = std::chrono::high_resolution_clock::now();
     auto ms = duration_cast<milliseconds>(t1 - t0).count();
     Assert: ms < budget[res]
       budget[64]  = 10 ms  (expected ~4.3ms, 2.3x headroom for CI variance)
       budget[128] = 80 ms  (expected ~32ms, 2.5x headroom)
   ```
   Budget rationale: 2-3x expected value gives slack for CI machine variance while
   catching 5-10x regressions (which would indicate a real bug — e.g., accidental
   CPU fallback, atomic contention, debug build only).

   If test runs on debug build, budgets must be x5 looser (debug is ~5x slower).
   Detect via `#ifdef NDEBUG` and multiply budgets accordingly.

**Test execution contract:**
- All three sub-tests must pass in Release build with device
- Sub-tests 1 and 2 should pass in any build (fallback path is exercised if no device)
- Sub-test 3 (perf) is Release-only — debug builds skip via `#ifndef NDEBUG` guard

---

## 9. Risks & mitigations

| Risk | Likelihood | Mitigation |
|---|---|---|
| `waitUntilCompleted` stalls main thread | High (accepted) | Phase 9.3a is synchronous by design. Phase 9.3b introduces frame-delayed readback. |
| Pass 0 CPU sampling dominates at 64³ (~3ms / 5ms total) | High (accepted) | Document clearly. Pass 0 elimination is the central goal of Phase 9.3b (GlobalSDF direct bind). |
| Winding order mismatch → back-face culling | Medium | Port CPU `make_cell` offset sequence verbatim. `TestGPUSurfaceNetsRenderBasic` (8.3) catches via variance assertion; `TestGPUSurfaceNetsRenderVsCPU` catches via histogram correlation. |
| `atomic_uint` flaky on Apple Silicon | Low | Pattern already proven in DDGI probe update; same RHI buffer usage. |
| 16-read budget overrun | Low | Audited: Pass 1=8, Pass 2=8, Pass 3=6 — all within 16. |
| Test env has no device → GPU path crashes | Medium | `IsReady() == false` check forces fallback. Sub-test 2 explicitly covers this. |
| Nanite mesh pipeline requires CPU-side `RHIMeshAsset` | Certain (accepted) | Phase 9.3a always reads back to CPU. Nanite compatibility preserved. |
| `static_normal_texture` packing logic drifts between `WriteVertex` and shader | Medium | Extract shared helper; document bit layout in both C++ and Metal; covered by `TestGPUSurfaceNetsRenderVsCPU` (8.3) — drift shows up as histogram mismatch. |
| Readback race (reading counter before GPU writes) | Medium | Use `[command_buffer waitUntilCompleted]` before any host read. Verified pattern. |
| Buffer allocation per Execute is wasteful | Medium | First version: allocate per Execute. Optimization: pool buffers in `GPUMesher` for re-use across calls (sized to last call). Defer to follow-up if profiling reveals cost. |
| Perf test flakes on slow CI machines | High (accepted) | Budget is 2-3x expected time (10ms/80ms vs 4.3ms/32ms). If still flaky, mark test as `LONG_RUNNING` or skip on CI via env var. |
| Perf test reports wrong number due to wall-clock including unrelated work | Low | `GenerateSurfaceNets` blocks on `waitUntilCompleted`, so wall clock ≈ GPU time. Any unrelated work between t0/t1 is the caller's bug. |
| `TestGPUMesherIntegration` fails to initialize engine in CI env (no window) | Medium | Follow `TestRenderFrameAPI`'s headless surface creation pattern (`CreateRenderSurface` with offscreen-only surface). Test must not require a visible window. |
| Histogram correlation threshold (0.85) too tight / too loose | Medium | Start with 0.85 based on expected cell-center vs dual-pos gradient differences; tune after first runs. If consistently flaky, loosen to 0.75 or replace with mean/sigma comparison. |

---

## 10. Performance budget

Reference: Apple Silicon M1 Pro.

| Phase | Cost @ 64³ | Cost @ 128³ |
|---|---|---|
| Pass 0 (CPU sample + upload) | ~3ms | ~25ms |
| Pass 1 (classify) | ~0.2ms | ~1ms |
| Pass 2 (emit vertices) | ~0.4ms | ~2ms |
| Pass 3 (emit faces × 3 axes) | ~0.5ms total | ~2.5ms total |
| Pass 4 (indirect args) | <0.01ms | <0.01ms |
| Readback + blit to RHIMeshAsset | ~0.2ms | ~1.5ms |
| **Total** | **~4.3ms** | **~32ms** |

**vs CPU baseline:** 64³ 5ms → 4.3ms (~1.2x), 128³ 60ms → 32ms (~2x).

> Note: The 64³ speedup is modest because Pass 0 (CPU sample) still dominates. The GPU advantage grows with resolution. The real unlock for "real-time terrain at 64³" is Phase 9.3b (GlobalSDF direct bind) which removes Pass 0 entirely.

For 128³ and larger, GPU becomes the clear win (~2x at 128³, scaling to ~5-10x at 256³ if we ever raise the cap).

---

## 11. Roadmap (future specs)

| Phase | Spec scope | Depends on |
|---|---|---|
| 9.3a (this spec) | GPU SurfaceNets on PCGField, CPU readback | Phase 9.1 |
| 9.3b | GlobalSDF direct bind + GPU-resident mesh output (skip readback) | 9.3a |
| 9.4 | Dual Contouring (extends `PCGField` with `SampleGradient`) | None strong; can parallel 9.3b |
| 9.5 | Adaptive LOD (octree voxel structure) + mesh simplification | 9.3b + 9.4 |

---

## 12. Open questions (none blocking)

- Whether to share `RHIMeshAsset` build logic between CPU and GPU paths via a common `BuildAndRegisterAsset` method (decided: yes, cleaner).
- Whether `algorithm` enum should add a 4th value for "auto" (pick GPU if available). Decided: no, explicit is better. Caller can check `IsReady` if needed.
- Whether to allocate GPU buffers per Execute or pool them. Decided: per Execute for v1, revisit if profiling shows cost.
- Perf test budget thresholds (10ms / 80ms). Decided: start with 2-3x expected; tune based on first CI runs. If persistently flaky, switch to relative threshold (regression vs rolling baseline).
- Histogram correlation threshold (0.85). Decided: start with 0.85; loosen if cell-center gradient causes more visual drift than expected.
