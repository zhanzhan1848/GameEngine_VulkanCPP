# GlobalSDF Dawn Port — Enabling Real Mode 11 Dynamic DDGI

**Date:** 2026-07-10
**Status:** Approved (awaiting spec review)
**Scope:** Enable existing GlobalSDF Dawn port + verify Mode 11 produces canonical dynamic DDGI
**Approach:** Minimal enable + three-layer verification + pre-agreed scope-cut menu

---

## 1. Problem Statement

Mode 11 (`MeshletDynamicDDGI`) was implemented per the canonical DDGI spec (per-frame GPU ray loop, EMA-blended probe update). The LumenDDGIPass canonical trace shader `DDGITraceRays.wgsl` requires GlobalSDF cascades as input — it traces through them to compute radiance at hit points.

**Current state:** GlobalSDF init and per-frame voxelization dispatch are disabled in `TestDawnForwardRenderer.cpp:2316-2324` and `:3078-3084` with stale blocker comments:

```
// NOTE: GlobalSDF init on Dawn is currently broken:
//   - R16Float texture format is incompatible with StorageBinding
//   - WGSL `voxelize_sdf` entry point missing
//   - Descriptor layout mismatch (Storage vs Uniform)
// Cascading validation errors corrupt Dawn device state for ALL subsequent
// pipeline creation — breaking Mode 7/8/9/10/11 (entire screen solid color).
```

**Audit findings (2026-07-10):** All three cited blockers are resolved:
- `R16_Float` is mapped to `WGPUTextureFormat_R16Float` at `Engine/Graphics/RHI/Platforms/Dawn/DawnCommon.h:30`
- `Engine/Graphics/Dawn/shaders/Nanite/GlobalSDFVoxelization.wgsl` exists (192 LOC, fully ported including the u8-to-u32 byte-unpack trick for triangle indices)
- `GlobalSDF.cpp:374-403` has runtime Dawn branch producing correct descriptor layout
- `maxStorageTexturesPerShaderStage=8` (DawnDevice.cpp:145), `maxTextureDimension3D=256` (DawnDevice.cpp:147) — both sufficient for 3× 128³ cascades

The "device corruption" claim is partially valid (Dawn validation errors do cascade, per memory `dawn-device-error-cascade`), but the *causes* of those errors appear fixed. The remaining work is to enable + verify.

## 2. Goal

Mode 11 produces canonical dynamic DDGI on Dawn:
- GlobalSDF voxelization runs every frame
- DDGITraceRays reads SDF cascades and traces rays
- Probe SH coefficients EMA-blend toward canonical radiance over ~60 frames
- Visual output differs from Mode 10 (static bake) after convergence

**Non-goals:** WASM port, SDF shadow rays, sky occlusion ray, probe relocation, mesh vertex bounds cleanup (those are separate specs).

## 3. Architecture

**Change footprint:**
- 2 source files modified (`TestDawnForwardRenderer.cpp`, `LumenDDGIPass.cpp`)
- 0 new files
- 0 shader changes (WGSL already correct)
- 0 RHI changes (Dawn already supports R16Float+Texture3D+StorageBinding)

**GlobalSDF API surface (verified `GlobalSDF.h`):**

```cpp
class GlobalSDF {
public:
    static GlobalSDF& Get();
    bool Initialize(RHIDeviceBase*, const GlobalSDFConfig& = {});
    bool InitVoxelization(const SDFVoxelizationResources&);  // 2nd-phase init
    bool IsInitialized() const;
    bool IsVoxelizationReady() const;

    void Update(const RenderSceneSnapshot&, u64 frame, const v3& camera_position);  // CPU bookkeeping
    void SetVoxelizationResources(const SDFVoxelizationResources&);
    void DispatchVoxelization(RHICommandBuffer*, u32 cascade_index);  // one dispatch per cascade
    const GlobalSDFConfig& GetConfig() const;
};
```

**Key facts:**
- Two-phase init: `Initialize()` (creates textures/pipeline) + `InitVoxelization()` (binds GPU geometry buffers)
- Per-frame `Update()` recomputes cascade origins/extents around camera
- Per-cascade dispatch: loop 0..N-1 calling `DispatchVoxelization(cmd, i)`
- Guards `IsInitialized()` and `IsVoxelizationReady()` already exist — S5 scope-cut is just calling these

**Runtime flow (Mode 11, every frame):**

```
RenderMeshletDDGIFrame(cmd)
  ├─ 3a. GlobalSDF voxelization (Mode 11 only)            ← was disabled
  │    ├─ guard: IsInitialized() && IsVoxelizationReady()
  │    ├─ SetVoxelizationResources(fresh)  ← refresh GPU buffer handles
  │    ├─ Update(snapshot, frame, cameraPos)  ← CPU bookkeeping, cascade origins
  │    └─ for c in 0..cascade_count-1:
  │         DispatchVoxelization(cmd, c)  ← voxelize_sdf_main WGSL
  │                                          writes texture_storage_3d<r16float>
  ├─ Barrier UAV→SRV on 3 cascades (handled by GlobalSDF)
  ├─ 3b. LumenDDGIPass::AddPass
  │    ├─ check sdfAvailable                       ← new stderr trace
  │    ├─ DDGITraceRays.wgsl
  │    │   reads 3 cascade SampledImage @ bindings 0/1/2
  │    │   SDF trace → canonical radiance at hit
  │    ├─ DDGIUpdateIrradiance.wgsl (EMA blend)
  │    └─ DDGIUpdateDepth.wgsl
  └─ Inline GIGather (samples current probe grid)
```

**Mode 10 path (unchanged):** `dynamic_mode_=false` makes LumenDDGIPass early-return after init. GlobalSDF voxelization is gated to Mode 11 (`if (renderMode_ == MeshletDynamicDDGI)`), so Mode 10 doesn't pay the perf cost.

**Reference implementation:** `TestNaniteStreamingPipeline.cpp:554-601` (init), `:3056-3127` (per-frame dispatch). Native test uses `base_resolution=60`; we copy that to stay within Apple Silicon budgets.

## 4. Files to Modify

### 4.1 `EngineTest/IntegrationTests/TestDawnForwardRenderer.cpp`

**Pre-flight (mandatory):** Grep for `GlobalSDF::Get().Shutdown` in this file. If absent, add to shutdown path (per memory `global-sdf-singleton-lifecycle`).

**Change 1 (~line 2316, two-phase init):** Replace the blocker comment with real init.

```cpp
// 2a. GlobalSDF init — needed for Mode 11 canonical dynamic DDGI.
// WGSL port + Dawn RHI support (R16Float + StorageBinding) are complete;
// prior blocker comment was stale (audit 2026-07-10).
auto& globalSDF = nanite::GlobalSDF::Get();
nanite::GlobalSDFConfig sdfConfig;
sdfConfig.cascade_count = 3;
sdfConfig.base_resolution = 60;        // matches native test (Apple Silicon budget)
sdfConfig.cascade_scale_factor = 2;
sdfConfig.voxel_size_base = 1.0f;

if (globalSDF.Initialize(device_, sdfConfig)) {
    // Phase 2: bind GPU geometry buffers from gpuDrawPipeline_
    nanite::SDFVoxelizationResources vox;
    vox.vertex_buffer            = gpuDrawPipeline_->GetGlobalVertexBuffer();
    vox.meshlet_buffer           = gpuDrawPipeline_->GetGlobalMeshletBuffer();
    vox.meshlet_vertices_buffer  = gpuDrawPipeline_->GetGlobalMeshletVerticesBuffer();
    vox.meshlet_triangles_buffer = gpuDrawPipeline_->GetGlobalMeshletTrianglesBuffer();
    vox.cluster_map_buffer       = gpuDrawPipeline_->GetClusterMapBuffer();
    vox.instance_data_buffer     = gpuDrawPipeline_->GetGlobalInstanceDataBuffer();
    vox.num_instances            = meshletSceneSnapshot_.GetInstanceCount();
    if (!globalSDF.InitVoxelization(vox)) {
        std::cerr << "[GlobalSDF] InitVoxelization failed — Mode 11 falls back to static seed\n";
    }
} else {
    std::cerr << "[GlobalSDF] Init failed — Mode 11 falls back to static seed\n";
}
// Non-fatal: LumenDDGIPass safety-net checks IsInitialized() at runtime.
```

**Change 2 (~line 3078, per-frame dispatch loop):** Replace disabled block with real dispatch, gated to Mode 11.

```cpp
// --- 3a. GlobalSDF voxelization (Mode 11 only) ---
// Mode 10 (static) skips this — LumenDDGIPass early-returns anyway,
// so voxelization would be wasted GPU work.
if (renderMode_ == DawnRenderMode::MeshletDynamicDDGI) {
    auto& globalSDF = nanite::GlobalSDF::Get();
    if (globalSDF.IsInitialized() && globalSDF.IsVoxelizationReady()) {
        // Refresh buffer handles (geometry may have been uploaded after init)
        nanite::SDFVoxelizationResources fresh;
        fresh.vertex_buffer            = gpuDrawPipeline_->GetGlobalVertexBuffer();
        fresh.meshlet_buffer           = gpuDrawPipeline_->GetGlobalMeshletBuffer();
        fresh.meshlet_vertices_buffer  = gpuDrawPipeline_->GetGlobalMeshletVerticesBuffer();
        fresh.meshlet_triangles_buffer = gpuDrawPipeline_->GetGlobalMeshletTrianglesBuffer();
        fresh.cluster_map_buffer       = gpuDrawPipeline_->GetClusterMapBuffer();
        fresh.instance_data_buffer     = gpuDrawPipeline_->GetGlobalInstanceDataBuffer();
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

**Change 3 (~line 2924):** Verify existing SetDynamicMode call is unchanged:

```cpp
ddgiPass_->SetDynamicMode(renderMode_ == DawnRenderMode::MeshletDynamicDDGI);
```

No edit — already correct per Mode 11 plan task #43.

**Note on `gpuDrawPipeline_`:** This is the existing `GPUDrivenDrawPipeline` instance in TestDawnForwardRenderer (referenced at line 3075). The `Get*Buffer()` methods are public — verify during implementation by grepping `class GPUDrivenDrawPipeline`.

### 4.2 `Engine/Graphics/Lumen/DDGI/LumenDDGIPass.cpp`

**Change 4 (~line 692):** Add one-shot stderr traces at safety-net boundary.

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

**Total delta:** ~70 lines of code (mostly the per-frame dispatch block) + blocker comments removed. No new files, no API changes, no shader changes.

## 5. Scope-Cut Menu (Apple Silicon fallbacks)

Pre-agreed fallbacks if hardware limits surface during validation. Each cut has a clear trigger and tested alternative.

| # | Trigger (observation) | Cut (change) | Visual cost |
|---|---|---|---|
| **S1** | `[A4]`-style flicker or NaN in irradiance after SDF enable | Drop L1 SH bands in trace — keep L0 only (saves ~24 buffer reads/probe) | Indirect lighting loses directional detail, becomes flat ambient |
| **S2** | Validation error or GPU hang on `texture_storage_3d` dispatch | Skip tetra interp — sample nearest probe (saves ~108 buffer reads at hit point) | Visible probe grid seams under close inspection |
| **S3** | DDGI trace exceeds per-thread texture read budget | Bind 1 cascade (largest) instead of 3 — skip cascade blend | Far surfaces lose SDF coverage → trace falls back to miss/sky more often |
| **S4** | Frame time > 33ms (30fps budget) | Throttle voxelization to every 2nd or 3rd frame (trace reads previous frame's SDF) | Slightly stale SDF under fast camera movement — acceptable for diffuse indirect |
| **S5** | Device-corruption regression (other modes show solid color) | Add `GlobalSDF::IsInitialized()` guard before every DispatchVoxelization call; revert Mode 11 to safety-net | Mode 11 = Mode 10 again (current state) |

**Application rule:** Apply cuts in order S5 → S1 → S2 → S3 → S4. Each cut is independent. Stop cutting once Mode 11 runs stable for 60 consecutive frames. No cut is applied preemptively.

**Order rationale:**
- S5 first (correctness — must not regress other modes)
- S1, S2 next (cheapest, fix most likely culprit: per-thread buffer reads)
- S3 (more invasive — changes SDF coverage)
- S4 last (changes algorithm semantics — temporal smoothing)

**Documentation convention:** Each applied cut gets a brief comment in `LumenDDGIPass.cpp` noting the trigger observation + date, so future engineers know it's intentional.

## 6. Verification Protocol

Three layers, applied in order. Stop at first failure.

### Layer 1: Validation (automatable)

**Pass criteria:** Run Mode 11 for 60 frames, no Dawn validation error in stderr.

```bash
DAWN_FORCE_MODE=11 ./Darwin/Debug/TestDawnForwardRenderer 2>/tmp/m11.log &
sleep 5
kill $!
grep -iE "validation|error|wgpu" /tmp/m11.log | grep -v "Loaded\|Init\|Ready"
```

**Fail action:** Read first validation error → fix at source (likely LumenDDGIPass.cpp or GlobalSDF.cpp) → re-run.

### Layer 2: Safety-net bypass (one-shot stderr)

**Pass criteria:** Log contains exactly one of:
- `[Mode11] GlobalSDF available — runtime trace active` (target)
- `[Mode11] GlobalSDF unavailable — falling back to static seed` (S5 fallback path engaged)

**Fail action:** If neither appears → `dynamic_mode_` not set → recheck Mode 11 wiring at TestDawnForwardRenderer.cpp:2929.

### Layer 3: Visual divergence (manual A/B)

**Pass criteria:** At frame 60+, Mode 11 looks subtly different from Mode 10 at the same camera position. Expected differences per canonical DDGI theory:

| Region | Mode 10 (static) | Mode 11 (dynamic) |
|---|---|---|
| Under arches / courtyard | Fixed indirect from bake | Slightly brighter near camera-facing walls (direct light leak from canonical E_direct) |
| Behind camera | Static bounce from bake-time camera pos | Updated bounce from current camera-adjacent surfaces |
| Sky-facing surfaces | Fixed sky contribution | Slight changes as SDF traces hit different geometry |

**Test procedure:**
1. Launch Mode 11, let it run for 60 frames (EMA convergence ramp per `DDGIUpdateIrradiance.wgsl:194-196`).
2. Note overall look (screenshot optional).
3. Press Tab to cycle to Mode 10. Compare.
4. Tab back to Mode 11. Move camera through Sponza courtyard — indirect lighting should follow.

**Fail action:** If Mode 11 == Mode 10 visually → check Layer 2 log appeared → inspect `DDGITraceRays.wgsl` SDF read code path → may indicate traces always miss (SDF voxels empty). Apply scope cut S1, retry Layer 3.

### Verification matrix

| Outcome | Verdict | Action |
|---|---|---|
| All 3 layers pass | Done | Commit, write memory entry |
| Layer 1 fails | Validation issue | Fix and retry |
| Layer 2 fails | Plumbing issue | Recheck Mode 11 wiring |
| Layer 3 fails | Algorithm issue | Apply S1, retry Layer 3 |
| Layer 3 fails after S1 | Apply S2 → S3 → S4 in order | Each cut gets a comment with date + reason |

## 7. Risks & Known Unknowns

### Risk 1: Stale blocker claim may be partially correct

**Claim:** Validation errors corrupt Dawn device state for all subsequent pipelines.

**Status:** Validation errors do cascade (per memory `dawn-device-error-cascade`). The *enumerated* causes (R16Float, missing WGSL, layout mismatch) are resolved. But there may be an *unenumerated* cause still present.

**Mitigation:** Layer 1 will detect within 30 seconds. If a new error class surfaces, apply S5 immediately and write a follow-up spec for that specific issue.

### Risk 2: GlobalSDF singleton lifecycle

**Memory:** `global-sdf-singleton-lifecycle` — GlobalSDF singleton destructor races with device teardown.

**Status:** `StandardRenderPipeline::ShutdownSubsystems` already calls Shutdown. TestDawnForwardRenderer may have a separate teardown path.

**Mitigation:** Pre-flight check before enabling init: grep for `GlobalSDF::Get().Shutdown` in TestDawnForwardRenderer.cpp. If absent, add explicit call before device destruction.

### Risk 3: Dawn buffer readback trap

**Memory:** `dawn-storage-buffer-map-read` and `dawn-gpu-buffer-debug-readback-clobber` — `MapBuffer` on GPU-only buffers returns zeros AND clobbers data.

**Status:** GlobalSDF uses `MapBuffer` on `vox_cascade_cb_` (Dynamic usage — should be safe). No SSBO readback in GlobalSDF or LumenDDGIPass.

**Mitigation:** None needed for current scope. If we add diagnostic dumps later (S2's probe SH visualization), use CPU-side cached values, never `MapBuffer` on GPU irradiance buffer.

### Risk 4: Apple Silicon read budget unknown until tested

**Memory has conflicting numbers:**
- `apple-silicon-buffer-read-limit`: >32 buffer reads/thread flickers
- `apple-silicon-compute-read-limits`: 16 buffer reads stable, +texture3D → irradiance vanishes
- `apple-silicon-texture3d-limit`: ~4 texture3D reads

**DDGITraceRays read budget:** 3 SDF cascades (texture reads) + tetra interp (~24-108 buffer reads) + ray data buffer. May exceed combined budget.

**Mitigation:** Scope-cut menu Section 5. S1+S2 together reduce to ~10-15 reads + 1-3 textures — well within Apple Silicon budget.

### Risk 5: Performance regression

**3 cascades × 128³ voxelization per frame = ~6.3M voxels.** On Apple Silicon this could be 5-15ms per frame.

**Mitigation:** S4 throttle. Note as future work: only re-vox the cascade closest to camera each frame; far cascades update every 4th frame. Out of scope for this spec.

## 8. Out of Scope

- WASM port (deferred per project convention; WASM meshlet still incomplete)
- SDF shadow rays at hit (canonical scope cut from original Mode 11 plan)
- Sky occlusion ray at hit (same)
- Probe classification/relocation (RTXGI features beyond canonical DDGI)
- Mesh vertex bounds cleanup in `RHIGpuMesh` (separate spec, mentioned in `dawn-meshlet-graywhite-zero-bounds` memory entry)
- Per-cascade temporal throttle (future optimization, see Risk 5)

## 9. Implementation Order

1. Pre-flight: grep for GlobalSDF Shutdown in test → add if missing.
2. Change 1: enable GlobalSDF init (~line 2316).
3. Build, smoke test (Layer 1 — should still work since dispatch is gated to Mode 11).
4. Change 2: enable voxelization dispatch (~line 3078).
5. Change 4: add stderr traces in LumenDDGIPass.cpp.
6. Build, run full Layer 1-3 verification.
7. Apply scope cuts as needed.
8. Commit.

## 10. Success Criteria

The implementation is complete when **all three** hold:

1. `DAWN_FORCE_MODE=11 ./Darwin/Debug/TestDawnForwardRenderer` runs 60+ frames with no Dawn validation error
2. Log contains `[Mode11] GlobalSDF available — runtime trace active` exactly once
3. Mode 11 visually differs from Mode 10 in Sponza courtyard after 60 frames of convergence

If S5 is applied (Mode 11 falls back to static seed due to device corruption regression), the spec is not complete — write a follow-up spec targeting the specific corruption cause.

---

## 11. Verification Result (2026-07-11)

**Status:** Layer 1 FAILED — spec audit was wrong, S5 applied.

### Layer 1 outcome

P3+P4 enabled init+dispatch. Ran `DAWN_FORCE_MODE=11 ./Darwin/Debug/TestDawnForwardRenderer` for 50s (well past 60-frame mark after 26s init). Stdout contained **6699 validation errors**:

- `Entry point "voxelize_sdf" doesn't exist in the shader module` — WGSL names it `voxelize_sdf_main` (line 113). Spec audit claimed WGSL was "fully ported" — wrong.
- `TextureUsage::StorageBinding ... incompatible with the format (TextureFormat::R16Float)` — WebGPU spec disallows this combo. Spec audit claimed "R16_Float mapped correctly" — true but irrelevant; the combo is rejected. Spec audit did not verify the combo.
- Cascade: `Compute_voxelize_sdf` pipeline → Invalid → `ddgi_trace_rays` pipeline → Invalid → all subsequent TextureViews → Invalid. Per-frame dispatch repeated the cascade 60×/sec.

Mode 10 (run for comparison): 10 validation errors at init time (cascade from GlobalSDF init corrupts `ddgi_trace_rays` pipeline creation). Mode 10 still renders correctly because it uses static cache, never dispatches the trace pipeline.

### S5 applied

- `d33821d` Revert "feat(test): enable per-frame GlobalSDF dispatch" (reverts P4 / `787b92a`)
- `113cd1f` Revert "feat(test): enable GlobalSDF two-phase init" (reverts P3 / `b3f6d2e`)

P1 (`d5581d4` Shutdown) and P2 (`6089506` stderr traces) retained — both harmless and useful for future debugging.

### Post-revert verification

- Mode 11: 1 validation error (pre-existing `ddgi_trace_rays` pipeline issue, unrelated to GlobalSDF — see task #45 history). Safety net fires: `[Mode11] GlobalSDF unavailable — falling back to static seed`.
- Mode 10: 0 validation errors. Reaches `[TestDawnFR] Ready: 393 meshes, 265 visible`. Unchanged behavior.

### Layer 2 outcome

Pre-revert: `[Mode11] GlobalSDF available — runtime trace active` (safety net bypassed — but only because `IsInitialized()` returns true even with corrupt textures).
Post-revert: `[Mode11] GlobalSDF unavailable — falling back to static seed` (correct).

### Layer 3 outcome

Not attempted — Layer 1 failed.

### Side finding: "mesh stall" was a diagnostic artifact

P3/P4 implementers reported the binary stalled at "Mesh 2/393". Investigation showed the loop completes in 26.3 seconds (~67ms/iter × 393 iter). The print gate `if (i < 3 || i == sceneMeshInfos_.size() - 1)` only fires for iters 0,1,2 then iter 392, creating the appearance of a stall. The 26s cost is dominated by `MaterialInstance::Initialize` calling Dawn `MapBuffer` (spin-wait on `wgpuInstanceProcessEvents`) 2× per iter. Future optimization target, not in scope of this spec.

### Follow-up spec needed

Per Section 10 instruction: "write a follow-up spec targeting the specific corruption cause." Memory entry `globalsdf-dawn-port-real-blockers` documents the 3 enumerated blockers for the next spec. The follow-up should NOT trust this spec's audit — start from runtime-verified findings.
