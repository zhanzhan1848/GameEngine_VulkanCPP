# GlobalSDF Dawn Port Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Enable GlobalSDF on Dawn so Mode 11 (MeshletDynamicDDGI) produces canonical dynamic DDGI instead of degenerating to the static seed.

**Architecture:** Uncomment existing-but-disabled init + dispatch in `TestDawnForwardRenderer.cpp`, add stderr traces in `LumenDDGIPass.cpp`. The port is ~80% complete (WGSL exists, RHI supports R16Float+Texture3D+StorageBinding, runtime Dawn branch exists). Remaining work: enable + verify. No new files, no shader changes, no RHI changes.

**Tech Stack:** C++17, Dawn (WebGPU native), WGSL, CMake.

**Spec reference:** `Docs/superpowers/specs/2026-07-10-globalsdf-dawn-port-design.md`

---

## File Structure

| File | Responsibility | Change |
|------|----------------|--------|
| `Engine/Graphics/Lumen/DDGI/LumenDDGIPass.cpp` | DDGI trace pass; safety-net early-return | Add 2 stderr traces at safety-net boundary (Change 4) |
| `EngineTest/IntegrationTests/TestDawnForwardRenderer.cpp` | Test harness | Add GlobalSDF Shutdown to teardown; replace blocker comments with init (Change 1) and dispatch (Change 2) |

**Singleton access pattern (verified):**
```cpp
auto& gpuDrawPipeline = primal::graphics::nanite::GPUDrivenDrawPipeline::Get();
auto& globalSDF = primal::graphics::nanite::GlobalSDF::Get();
```
Both are runtime singletons (`::Get()`), not class members.

---

## Task 1: Add GlobalSDF::Shutdown to teardown

**Why:** Per memory `global-sdf-singleton-lifecycle`, GlobalSDF singleton destructor races with device teardown. Currently `TestDawnForwardRenderer.cpp` does NOT call `GlobalSDF::Get().Shutdown()` — adding init in Task 3 without this fix would create a UAF on exit.

**Files:**
- Modify: `EngineTest/IntegrationTests/TestDawnForwardRenderer.cpp:2468-2489` (`ShutdownMeshletPipeline`)

- [ ] **Step 1: Read the current shutdown function**

```bash
grep -n "ShutdownMeshletPipeline\|GlobalSDF" EngineTest/IntegrationTests/TestDawnForwardRenderer.cpp
```

Expected: `ShutdownMeshletPipeline()` at line 2468; no `GlobalSDF` references in the function.

- [ ] **Step 2: Add GlobalSDF shutdown call**

In `EngineTest/IntegrationTests/TestDawnForwardRenderer.cpp`, find `ShutdownMeshletPipeline()` (around line 2468). Add GlobalSDF shutdown at the TOP of the function (before gpuDrawPipeline.Shutdown() — GlobalSDF holds GPU resources that the device must still be valid for):

```cpp
void Engine_Test::ShutdownMeshletPipeline() {
    if (!meshletInitialized_) return;

    // Shutdown GlobalSDF first — it holds GPU textures created from device_.
    // Singleton destructor races with device teardown (see memory entry
    // global-sdf-singleton-lifecycle). Explicit Shutdown() prevents UAF.
    primal::graphics::nanite::GlobalSDF::Get().Shutdown();

    auto& gpuDrawPipeline = primal::graphics::nanite::GPUDrivenDrawPipeline::Get();
    auto& cullingPipeline = primal::graphics::nanite::GPUCullingPipeline::Get();
    // ... rest unchanged
```

- [ ] **Step 3: Add header include if missing**

```bash
grep -n "GlobalSDF.h" EngineTest/IntegrationTests/TestDawnForwardRenderer.cpp
```

If empty, add to the include block at top of file:

```cpp
#include "Engine/Graphics/Nanite/GlobalSDF.h"
```

- [ ] **Step 4: Build**

```bash
cmake --build build_dawn --target TestDawnForwardRenderer -- -j 8 2>&1 | tail -20
```

Expected: Build succeeds. If compile error about `GlobalSDF` namespace, fix include path or namespace typo.

- [ ] **Step 5: Smoke test — Mode 10 still works**

```bash
DAWN_FORCE_MODE=10 ./Darwin/Debug/TestDawnForwardRenderer 2>/tmp/t1.log &
sleep 4
kill %1 2>/dev/null
grep -iE "error|crash" /tmp/t1.log | grep -v "Loaded\|Init\|Ready"
```

Expected: empty (no errors). Mode 10 should be unaffected.

- [ ] **Step 6: Commit**

```bash
git add EngineTest/IntegrationTests/TestDawnForwardRenderer.cpp
git commit -m "fix(test): explicit GlobalSDF::Shutdown before device teardown

Pre-empts UAF when Task 3 enables GlobalSDF init. Per memory entry
global-sdf-singleton-lifecycle."
```

---

## Task 2: Add stderr traces in LumenDDGIPass (Change 4)

**Why:** Task 4's verification needs to confirm whether Mode 11 entered the runtime trace path or fell back to static seed. Currently silent.

**Files:**
- Modify: `Engine/Graphics/Lumen/DDGI/LumenDDGIPass.cpp:683-694`

- [ ] **Step 1: Read the current safety-net code**

```bash
sed -n '680,695p' Engine/Graphics/Lumen/DDGI/LumenDDGIPass.cpp
```

Expected output shows the safety-net block at lines 683-694 with `if (dynamic_mode_ && !sdfAvailable) { return; }`.

- [ ] **Step 2: Add stderr traces**

In `Engine/Graphics/Lumen/DDGI/LumenDDGIPass.cpp`, replace lines 692-694:

```cpp
            if (dynamic_mode_ && !sdfAvailable) {
                return;
            }
```

with:

```cpp
            if (dynamic_mode_ && !sdfAvailable) {
                static bool warned = false;
                if (!warned) {
                    std::cerr << "[Mode11] GlobalSDF unavailable — falling back to static seed\n";
                    warned = true;
                }
                return;
            }
            if (dynamic_mode_) {
                static bool traced = false;
                if (!traced) {
                    std::cerr << "[Mode11] GlobalSDF available — runtime trace active\n";
                    traced = true;
                }
            }
```

- [ ] **Step 3: Build**

```bash
cmake --build build_dawn --target TestDawnForwardRenderer -- -j 8 2>&1 | tail -10
```

Expected: Build succeeds.

- [ ] **Step 4: Smoke test — Mode 11 currently shows fallback message**

(GlobalSDF not initialized yet, so safety-net should fire.)

```bash
DAWN_FORCE_MODE=11 ./Darwin/Debug/TestDawnForwardRenderer 2>/tmp/t2.log &
sleep 4
kill %1 2>/dev/null
grep "\[Mode11\]" /tmp/t2.log
```

Expected: `[Mode11] GlobalSDF unavailable — falling back to static seed`

- [ ] **Step 5: Commit**

```bash
git add Engine/Graphics/Lumen/DDGI/LumenDDGIPass.cpp
git commit -m "feat(ddgi): stderr trace for Mode 11 safety-net state

One-shot messages confirm whether runtime trace is active or Mode 11
degenerates to static seed. Required for Task 4 verification."
```

---

## Task 3: Enable GlobalSDF two-phase init (Change 1)

**Why:** Replace the stale blocker comment with real init. After this task, GlobalSDF textures and pipeline exist but no voxelization runs yet — Mode 11 still falls back to static seed in the dispatch path (gated to Task 4).

**Files:**
- Modify: `EngineTest/IntegrationTests/TestDawnForwardRenderer.cpp:2316-2325`

- [ ] **Step 1: Read the current blocker comment**

```bash
sed -n '2310,2330p' EngineTest/IntegrationTests/TestDawnForwardRenderer.cpp
```

Expected: 9-line `// NOTE: GlobalSDF init on Dawn is currently broken:` comment block.

- [ ] **Step 2: Replace blocker comment with two-phase init**

In `EngineTest/IntegrationTests/TestDawnForwardRenderer.cpp`, replace lines 2316-2324 (the entire NOTE block):

```cpp
    // NOTE: GlobalSDF init on Dawn is currently broken:
    //   - R16Float texture format is incompatible with StorageBinding
    //   - WGSL `voxelize_sdf` entry point missing
    //   - Descriptor layout mismatch (Storage vs Uniform)
    // Cascading validation errors corrupt Dawn device state for ALL subsequent
    // pipeline creation — breaking Mode 7/8/9/10/11 (entire screen solid color).
    // LumenDDGIPass has a safety net: `if (dynamic_mode_ && !sdfAvailable) return;`
    // so Mode 11 falls back to the static seed (looks like Mode 10). Proper
    // GlobalSDF Dawn port is a separate task.
```

with:

```cpp
    // 2a. GlobalSDF init — needed for Mode 11 canonical dynamic DDGI.
    // WGSL port + Dawn RHI support (R16Float + StorageBinding) are complete;
    // prior blocker comment was stale (audit 2026-07-10, see spec
    // Docs/superpowers/specs/2026-07-10-globalsdf-dawn-port-design.md).
    {
        auto& globalSDF = primal::graphics::nanite::GlobalSDF::Get();
        primal::graphics::nanite::GlobalSDFConfig sdfConfig;
        sdfConfig.cascade_count = 3;
        sdfConfig.base_resolution = 60;        // matches native test (Apple Silicon budget)
        sdfConfig.cascade_scale_factor = 2;
        sdfConfig.voxel_size_base = 1.0f;

        if (globalSDF.Initialize(device_, sdfConfig)) {
            // Phase 2: bind GPU geometry buffers from gpuDrawPipeline
            auto& gpuDrawPipeline_forSDF = primal::graphics::nanite::GPUDrivenDrawPipeline::Get();
            primal::graphics::nanite::SDFVoxelizationResources vox;
            vox.vertex_buffer            = gpuDrawPipeline_forSDF.GetGlobalVertexBuffer();
            vox.meshlet_buffer           = gpuDrawPipeline_forSDF.GetGlobalMeshletBuffer();
            vox.meshlet_vertices_buffer  = gpuDrawPipeline_forSDF.GetGlobalMeshletVerticesBuffer();
            vox.meshlet_triangles_buffer = gpuDrawPipeline_forSDF.GetGlobalMeshletTrianglesBuffer();
            vox.cluster_map_buffer       = gpuDrawPipeline_forSDF.GetClusterMapBuffer();
            vox.instance_data_buffer     = gpuDrawPipeline_forSDF.GetGlobalInstanceDataBuffer();
            vox.num_instances            = meshletSceneSnapshot_.GetInstanceCount();
            if (!globalSDF.InitVoxelization(vox)) {
                std::cerr << "[GlobalSDF] InitVoxelization failed — Mode 11 falls back to static seed\n";
            }
        } else {
            std::cerr << "[GlobalSDF] Init failed — Mode 11 falls back to static seed\n";
        }
        // Non-fatal: LumenDDGIPass safety-net (Task 2) handles unavailable SDF.
    }
```

- [ ] **Step 3: Build**

```bash
cmake --build build_dawn --target TestDawnForwardRenderer -- -j 8 2>&1 | tail -20
```

Expected: Build succeeds. Common compile errors and fixes:
- `'GlobalSDFConfig' not found` → add `#include "Engine/Graphics/Nanite/GlobalSDF.h"` at top
- `'SDFVoxelizationResources' not found` → same include
- `'GetGlobalVertexBuffer' not a member` → check `Engine/Graphics/Nanite/GPUDrivenDrawPipeline.h:185-192`

- [ ] **Step 4: Smoke test — Mode 11 init succeeds, dispatch still disabled**

```bash
DAWN_FORCE_MODE=11 ./Darwin/Debug/TestDawnForwardRenderer 2>/tmp/t3.log &
sleep 4
kill %1 2>/dev/null
grep -iE "GlobalSDF|Mode11" /tmp/t3.log
```

Expected: `[Mode11] GlobalSDF unavailable — falling back to static seed` (dispatch still disabled in Task 4). May also see `[GlobalSDF]` init messages.

**Red flag:** If `[GlobalSDF] Init failed` appears, the stale blocker comment was partially correct — apply spec scope-cut S5 (do not enable dispatch in Task 4) and write follow-up spec targeting the specific init failure.

- [ ] **Step 5: Regression — Mode 10 still works**

```bash
DAWN_FORCE_MODE=10 ./Darwin/Debug/TestDawnForwardRenderer 2>/tmp/t3m10.log &
sleep 4
kill %1 2>/dev/null
grep -iE "error|crash|Init failed" /tmp/t3m10.log | grep -v "Loaded\|Ready"
```

Expected: empty. Mode 10 must be unaffected.

- [ ] **Step 6: Commit**

```bash
git add EngineTest/IntegrationTests/TestDawnForwardRenderer.cpp
git commit -m "feat(test): enable GlobalSDF two-phase init for Mode 11

Replace stale blocker comment with real Initialize() + InitVoxelization().
Audit 2026-07-10 confirmed all 3 cited blockers (R16Float, WGSL, layout)
are resolved. Dispatch still gated to Task 4."
```

---

## Task 4: Enable per-frame GlobalSDF dispatch (Change 2)

**Why:** Wire voxelization into the per-frame Mode 11 path. After this task, Mode 11 runs canonical dynamic DDGI end-to-end.

**Files:**
- Modify: `EngineTest/IntegrationTests/TestDawnForwardRenderer.cpp:3078-3084`

- [ ] **Step 1: Read the current disabled block**

```bash
sed -n '3076,3086p' EngineTest/IntegrationTests/TestDawnForwardRenderer.cpp
```

Expected: 7-line `// --- 3a. GlobalSDF voxelization — DISABLED on Dawn ---` comment.

- [ ] **Step 2: Replace disabled block with dispatch loop**

In `EngineTest/IntegrationTests/TestDawnForwardRenderer.cpp`, replace lines 3078-3084:

```cpp
    // --- 3a. GlobalSDF voxelization — DISABLED on Dawn ---
    // GlobalSDF Dawn port is incomplete (R16Float+StorageBinding incompatible,
    // missing WGSL entry point, descriptor layout mismatch). Attempting to init
    // corrupts Dawn device state for all subsequent pipelines. LumenDDGIPass
    // safety-net early-returns when SDF is unavailable, so Mode 11 falls back
    // to the static seed (visually identical to Mode 10). Proper GlobalSDF
    // Dawn port is a separate task.
```

with:

```cpp
    // --- 3a. GlobalSDF voxelization (Mode 11 only) ---
    // Mode 10 (static) skips this — LumenDDGIPass early-returns anyway,
    // so voxelization would be wasted GPU work.
    if (renderMode_ == DawnRenderMode::MeshletDynamicDDGI) {
        auto& globalSDF = primal::graphics::nanite::GlobalSDF::Get();
        if (globalSDF.IsInitialized() && globalSDF.IsVoxelizationReady()) {
            // Refresh buffer handles (geometry may have been uploaded after init)
            primal::graphics::nanite::SDFVoxelizationResources fresh;
            fresh.vertex_buffer            = gpuDrawPipeline.GetGlobalVertexBuffer();
            fresh.meshlet_buffer           = gpuDrawPipeline.GetGlobalMeshletBuffer();
            fresh.meshlet_vertices_buffer  = gpuDrawPipeline.GetGlobalMeshletVerticesBuffer();
            fresh.meshlet_triangles_buffer = gpuDrawPipeline.GetGlobalMeshletTrianglesBuffer();
            fresh.cluster_map_buffer       = gpuDrawPipeline.GetClusterMapBuffer();
            fresh.instance_data_buffer     = gpuDrawPipeline.GetGlobalInstanceDataBuffer();
            fresh.num_instances            = meshletSceneSnapshot_.GetInstanceCount();

            if (fresh.num_instances > 0
                && fresh.vertex_buffer != rhi::handles::INVALID_RESOURCE
                && fresh.meshlet_buffer != rhi::handles::INVALID_RESOURCE
                && fresh.instance_data_buffer != rhi::handles::INVALID_RESOURCE) {
                globalSDF.SetVoxelizationResources(fresh);
                // CPU bookkeeping — recenter cascades on camera
                globalSDF.Update(meshletSceneSnapshot_, totalFrames_, cameraPos_);
                // Per-cascade GPU dispatch
                for (u32 c = 0; c < globalSDF.GetConfig().cascade_count; ++c) {
                    globalSDF.DispatchVoxelization(cmd, c);
                }
            }
        }
    }
```

**Note:** `gpuDrawPipeline` is already in scope at this point (defined at line ~2298 of `initialize()` and again as a local in `RenderMeshletDDGIFrame`). Verify by:
```bash
sed -n '3068,3080p' EngineTest/IntegrationTests/TestDawnForwardRenderer.cpp
```
If `gpuDrawPipeline` is not visible at line 3078 (e.g., the function was refactored), access via `primal::graphics::nanite::GPUDrivenDrawPipeline::Get()`.

- [ ] **Step 3: Build**

```bash
cmake --build build_dawn --target TestDawnForwardRenderer -- -j 8 2>&1 | tail -20
```

Expected: Build succeeds.

- [ ] **Step 4: Layer 1 verification — no Dawn validation errors**

```bash
DAWN_FORCE_MODE=11 ./Darwin/Debug/TestDawnForwardRenderer 2>/tmp/t4.log &
sleep 6  # 60+ frames at 60fps
kill %1 2>/dev/null
grep -iE "validation|wgpu.*error|Unknown\s" /tmp/t4.log | grep -v "Loaded\|Ready\|Init\|GlobalSDF\|Mode11" | head -20
```

Expected: empty (no validation errors).

**Red flag — device corruption:** If Mode 7/8/9/10 also start showing solid color after this change, apply spec scope-cut S5 immediately:
```cpp
// Wrap the dispatch in an additional guard:
if (renderMode_ == DawnRenderMode::MeshletDynamicDDGI
    && primal::graphics::nanite::GlobalSDF::Get().IsInitialized()
    /* ... existing conditions ... */) {
```
And bisect: try `cascade_count=1`, `base_resolution=32` to isolate whether the issue is texture size or cascade count.

- [ ] **Step 5: Layer 2 verification — safety-net bypassed**

```bash
grep "\[Mode11\]" /tmp/t4.log
```

Expected: `[Mode11] GlobalSDF available — runtime trace active`

**Red flag:** If `[Mode11] GlobalSDF unavailable` appears instead → GlobalSDF init failed silently. Re-check Task 3 log output.

- [ ] **Step 6: Regression — Mode 10 unaffected**

```bash
DAWN_FORCE_MODE=10 ./Darwin/Debug/TestDawnForwardRenderer 2>/tmp/t4m10.log &
sleep 4
kill %1 2>/dev/null
grep "\[Mode11\]\|\[GlobalSDF\]" /tmp/t4m10.log
```

Expected: empty. Mode 10 should not run voxelization (gated to Mode 11).

- [ ] **Step 7: Commit**

```bash
git add EngineTest/IntegrationTests/TestDawnForwardRenderer.cpp
git commit -m "feat(test): enable per-frame GlobalSDF dispatch for Mode 11

Voxelization runs every frame in Mode 11 only. Three-layer verification:
no validation errors, safety-net bypassed (stderr confirms runtime trace
active), Mode 10 unaffected.

Spec: Docs/superpowers/specs/2026-07-10-globalsdf-dawn-port-design.md"
```

---

## Task 5: Layer 3 visual verification

**Why:** Layers 1-2 prove the pipeline runs without errors. Layer 3 proves it produces canonical dynamic DDGI visually (mode 10 vs mode 11 look different after convergence).

This task is **manual user verification** — no code changes, just observation + documentation.

- [ ] **Step 1: Launch Mode 11 and let it converge**

```bash
DAWN_FORCE_MODE=11 ./Darwin/Debug/TestDawnForwardRenderer 2>/tmp/t5.log &
```

Wait 60 frames (~1 second at 60fps, but allow 5 seconds to be safe). Note overall look (screenshot optional).

- [ ] **Step 2: Tab to Mode 10, compare**

Press `Tab` once to cycle to Mode 10. Observe for ~1 second. Press `Tab` again to return to Mode 11.

Expected differences (per spec Section 6 Layer 3):

| Region | Mode 10 (static) | Mode 11 (dynamic) |
|---|---|---|
| Under arches / courtyard | Fixed indirect from bake | Slightly brighter near camera-facing walls |
| Behind camera | Static bounce from bake-time camera pos | Updated bounce from current camera-adjacent surfaces |
| Sky-facing surfaces | Fixed sky contribution | Slight changes as SDF traces hit different geometry |

- [ ] **Step 3: Move camera through Sponza**

In Mode 11, navigate through courtyard / arches. Indirect lighting should follow camera movement (subtly — this is diffuse GI, not specular).

- [ ] **Step 4: Document observation**

Append findings to `Docs/superpowers/specs/2026-07-10-globalsdf-dawn-port-design.md` Section 6 (Layer 3 verification matrix):

```
## Verification Result (YYYY-MM-DD)

Layer 1 (validation): ✓ no errors
Layer 2 (safety-net): ✓ runtime trace active
Layer 3 (visual): [observed differences / no differences / cut applied]
```

- [ ] **Step 5: If Layer 3 failed, apply scope cuts from spec Section 5**

If Mode 11 == Mode 10 visually (no divergence after 60 frames), apply cuts in order. After each cut, rebuild and re-verify Layer 3:

**S1 (drop L1 SH bands):** Edit `DDGITraceRays.wgsl` `samplePrevProbeGrid` function — replace the `sh: array<vec3<f32>, 4>` (L0+L1) with `sh: array<vec3<f32>, 1>` (L0 only). Skip `shDot4`, use `sh[0]` directly.

**S2 (skip tetra interp):** Edit `DDGITraceRays.wgsl` `samplePrevProbeGrid` — replace tetrahedral blend with nearest-probe sample. Use `let gp_int = vec3<u32>(gp)` and index directly.

**S3 (single cascade):** Edit `LumenDDGIPass.cpp:671-681` — only bind cascade 0. Update `DDGIVolumeData` cascade_count to 1. Add comment noting S3 applied + date.

**S4 (throttle voxelization):** Edit Task 4's dispatch block — add frame counter modulo:
```cpp
if (totalFrames_ % 2 == 0) {  // every 2nd frame
    // ... existing dispatch loop
}
```

**S5 (disable Mode 11 dynamic):** Add `GlobalSDF::IsInitialized()` guard before the dispatch in Task 4. Mode 11 reverts to static seed. Write follow-up spec targeting the device corruption cause.

After applying any cut, add a comment in `LumenDDGIPass.cpp` near line 692:
```cpp
// Scope cut S<N> applied YYYY-MM-DD: <one-line reason>
// See spec Section 5 for full menu.
```

- [ ] **Step 6: Commit verification result**

If no scope cuts applied:
```bash
git add Docs/superpowers/specs/2026-07-10-globalsdf-dawn-port-design.md
git commit -m "docs(spec): GlobalSDF Dawn port — Layer 3 verification passed"
```

If scope cuts applied, also stage the relevant shader/code changes:
```bash
git add Engine/Graphics/Lumen/DDGI/LumenDDGIPass.cpp \
        Engine/Graphics/Dawn/shaders/Lumen/DDGITraceRays.wgsl \
        Docs/superpowers/specs/2026-07-10-globalsdf-dawn-port-design.md
git commit -m "fix(ddgi): scope cut S<N> applied — <reason>

Verification: Layer 1 ✓ Layer 2 ✓ Layer 3 (with cut) ✓"
```

---

## Self-Review

**Spec coverage check:**

| Spec Section | Covered by |
|---|---|
| §3 Architecture | Tasks 2-4 implement the runtime flow |
| §4.1 TestDawnForwardRenderer Change 1 (init) | Task 3 |
| §4.1 TestDawnForwardRenderer Change 2 (dispatch) | Task 4 |
| §4.1 TestDawnForwardRenderer Change 3 (SetDynamicMode verify) | Implicit — no change needed (already correct) |
| §4.1 Pre-flight (Shutdown) | Task 1 |
| §4.2 LumenDDGIPass Change 4 (stderr) | Task 2 |
| §5 Scope-Cut Menu | Task 5 Step 5 (conditional application) |
| §6 Verification Layer 1 (validation) | Task 4 Step 4 |
| §6 Verification Layer 2 (safety-net) | Task 4 Step 5 |
| §6 Verification Layer 3 (visual) | Task 5 |
| §7 Risks | Task 1 mitigates Risk 2 (lifecycle); Task 3 Step 4 mitigates Risk 1 (stale claim); Task 4 Step 4 mitigates Risk 4 (Apple Silicon); spec Section 7 Risk 3/5 are informational |

**Placeholder scan:** No TBD/TODO. Every code step has actual code.

**Type consistency:** `GlobalSDF`, `GlobalSDFConfig`, `SDFVoxelizationResources` types verified in `GlobalSDF.h`. `GPUDrivenDrawPipeline::Get*Buffer()` accessors verified in `GPUDrivenDrawPipeline.h:185-192`. Variable name `gpuDrawPipeline` (local) vs `gpuDrawPipeline_` (not a member) — Task 3 uses `_forSDF` suffix to avoid shadow warning; Task 4 uses the existing local in scope.

---

## Execution Handoff

Plan complete and saved to `Docs/superpowers/plans/2026-07-10-globalsdf-dawn-port-plan.md`. Two execution options:

**1. Subagent-Driven (recommended)** — Dispatch a fresh subagent per task, review between tasks, fast iteration.

**2. Inline Execution** — Execute tasks in this session using executing-plans, batch execution with checkpoints.

Which approach?
