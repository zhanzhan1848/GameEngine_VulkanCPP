# GlobalSDF Dawn Port v2 — Real Blocker Fixes for Mode 11

**Date:** 2026-07-13
**Status:** Approved (awaiting implementation)
**Scope:** Fix 4 runtime-verified blockers so Mode 11 produces canonical dynamic DDGI end-to-end
**Predecessor:** `2026-07-10-globalsdf-dawn-port-design.md` (S5 applied — audit was wrong)
**Approach:** Mechanical fixes only — no new architecture, no new files

---

## 1. Problem Statement

The previous spec (`2026-07-10-globalsdf-dawn-port-design.md`) assumed the existing GlobalSDF Dawn port was ~80% complete and the blocker comment was stale. Runtime verification (2026-07-11) disproved this: all 3 cited blockers are real, plus 1 additional layout bug in DDGI trace. S5 was applied (full revert of init + dispatch).

**Current state (post-revert):**
- Mode 11 safety-net early-returns → uses static seed (visually identical to Mode 10)
- `[Mode11] GlobalSDF unavailable — falling back to static seed` fires correctly
- Mode 10 unaffected (0 validation errors)
- 1 pre-existing validation error in Mode 11 from `ddgi_trace_rays` pipeline creation (separate from GlobalSDF)

**4 runtime-verified blockers (all need fixing):**

### B1: WGSL entry point name mismatch

- C++ (`Engine/Graphics/Nanite/GlobalSDF.cpp:363`): `CreateShader(..., "voxelize_sdf")`
- WGSL (`Engine/Graphics/Dawn/shaders/Nanite/GlobalSDFVoxelization.wgsl:113`): `fn voxelize_sdf_main`

Dawn validation: `Entry point "voxelize_sdf" doesn't exist in the shader module`.

### B2: R16Float + StorageBinding rejected by WebGPU spec

WebGPU spec disallows R16Float as storage texture format. Dawn validation:
```
The texture usage (TextureUsage::(CopyDst|TextureBinding|StorageBinding))
includes TextureUsage::StorageBinding, which is incompatible with the
format (TextureFormat::R16Float).
```

- C++ (`Engine/Graphics/Nanite/GlobalSDF.cpp:202`): `desc.format = rhi::DataFormat::R16_Float`
- WGSL (`GlobalSDFVoxelization.wgsl:105`): `var sdf_output: texture_storage_3d<r16float, write>`

### B3: DDGI trace descriptor layout / WGSL binding mismatch

C++ layout (`LumenDDGIPass.cpp:298-311`) uses Metal-style overlapping engine bindings (textures 0-3 + buffers 0-4). DawnDescriptorSetLayout detects conflicts and remaps buffers to wgpu bindings 5-8. Result:
- WGPU binding 4 ← irradiance_history (was engine binding 4, no texture conflict, not remapped)
- WGPU binding 5 ← globalData
- WGPU binding 6 ← volume
- WGPU binding 7 ← ray_buffer
- WGPU binding 8 ← probeUpdateList

But WGSL `DDGITraceRays.wgsl:412-417` declares:
- binding 4 ← globalData (uniform)
- binding 5 ← volume (uniform)
- binding 6 ← ray_buffer (storage, read_write)
- binding 7 ← probeUpdateList (storage, read)
- binding 8 ← irradiance_history (storage, read)

Mismatch on every buffer binding. Validation:
```
The buffer type in the shader (BufferBindingType::Storage) is not compatible
with the type in the layout (BufferBindingType::Uniform).
 - While validating @group(0) @binding(6) matches [BindGroupLayoutInternal]
```

### B4: `IsInitialized()` returns true even with corrupt GPU resources

GlobalSDF::Initialize sets `initialized_ = true` early. If texture or shader creation fails (e.g., due to B1+B2), the bool stays true. Consumers (LumenDDGIPass) check `IsInitialized()` for safety-net — they see `true` and proceed, propagating corrupt handles.

## 2. Goal

Mode 11 produces canonical dynamic DDGI on Dawn:
- 0 Dawn validation errors at runtime (was 6699/min before revert)
- `[Mode11] GlobalSDF available — runtime trace active` fires
- Mode 11 visually differs from Mode 10 in Sponza courtyard after 60+ frames

**Non-goals (deferred):**
- WASM port (separate spec, blocked on meshlet WASM work)
- Performance optimization (26s init from MaterialInstance MapBuffer — orthogonal)
- Audit of other Dawn descriptor set layouts (Path B, deferred)
- Per-cascade temporal throttle (future optimization)

## 3. Architecture

**Change footprint:**
- 3 source files modified (`GlobalSDF.cpp`, `GlobalSDF.h`, `LumenDDGIPass.cpp`)
- 1 shader modified (`GlobalSDFVoxelization.wgsl`)
- 0 new files
- 0 RHI changes

**Approach:**
- F1 (entry point): rename WGSL fn → match C++
- F2 (texture format): R16Float → R32Float (both sides)
- F3 (binding layout): rewrite C++ layout to use unique bindings 0-8 matching WGSL — don't touch WGSL (already self-documenting sequential)
- F4 (sanity check): verify texture + shader handles non-INVALID before declaring initialized

**Why these specific choices:**
- **F1 rename WGSL not C++**: 1-line change in single file. C++ string used in multiple call sites stays the same.
- **F2 R32Float not RGBA8**: R32Float is 4 bytes/voxel (same as RGBA8) with no precision loss. RGBA8snorm would require SDF scale calibration (SDF values typically [-1, 1] meters with sub-cm precision needs). Memory budget: 3 cascades × 60³ × 4B = 5.2MB (well within Apple Silicon limits). R16Float rejected by spec. RG16Float also rejected (not in WebGPU storage format allowlist).
- **F3 change C++ not WGSL**: WGSL bindings are sequential 0-8 and self-documenting. C++ uses Metal-style overlap that DawnDescriptorSetLayout remaps with surprising output. Fixing C++ is 1 place; fixing WGSL would require also touching engine binding numbers in descriptor writes (same effort, less clarity).
- **F4 minimal check**: just verify cascade textures + vox pipeline. Don't validate every internal state — over-engineering.

**Runtime flow (unchanged from previous spec, included for clarity):**

```
RenderMeshletDDGIFrame(cmd)
  ├─ 3a. GlobalSDF voxelization (Mode 11 only)
  │    ├─ guard: IsInitialized() && IsVoxelizationReady()  ← F4 makes this trustworthy
  │    ├─ SetVoxelizationResources(fresh)
  │    ├─ Update(snapshot, frame, cameraPos)
  │    └─ for c in 0..cascade_count-1: DispatchVoxelization(cmd, c)
  ├─ Barrier UAV→SRV on 3 cascades
  └─ 3b. LumenDDGIPass::AddPass
       ├─ DDGITraceRays.wgsl (fixed bindings F3, reads R32Float SDF F2)
       ├─ DDGIUpdateIrradiance.wgsl
       └─ DDGIUpdateDepth.wgsl
```

## 4. Files to Modify

### 4.1 `Engine/Graphics/Dawn/shaders/Nanite/GlobalSDFVoxelization.wgsl`

**F1 (line 113):** Rename entry point.

```wgsl
@compute @workgroup_size(4, 4, 4)
fn voxelize_sdf(@builtin(global_invocation_id) tid: vec3<u32>) {
```

(was: `fn voxelize_sdf_main(...)`)

**F2 (line 105):** Change texture format.

```wgsl
@group(0) @binding(0) var sdf_output: texture_storage_3d<r32float, write>;
```

(was: `texture_storage_3d<r16float, write>`)

### 4.2 `Engine/Graphics/Nanite/GlobalSDF.cpp`

**F2 (line 202):** Change texture format.

```cpp
desc.format = rhi::DataFormat::R32_Float;
```

(was: `rhi::DataFormat::R16_Float`)

**F4 (multiple sites):** Add resource validity tracking.

In `GlobalSDF::Initialize()`, after texture + pipeline creation, before returning true:
```cpp
// Verify GPU resources are valid — initialized_ alone isn't enough;
// partial init (e.g., texture created but shader compile failed) leaves
// consumers with corrupt handles. F4 sanity check (2026-07-13).
init_resources_valid_ =
    (sdf_cascade_textures_[0] != rhi::handles::INVALID_RESOURCE &&
     sdf_cascade_textures_[1] != rhi::handles::INVALID_RESOURCE &&
     sdf_cascade_textures_[2] != rhi::handles::INVALID_RESOURCE &&
     vox_pipeline_ != rhi::handles::INVALID_SHADER);
if (!init_resources_valid_) {
    std::cerr << "[GlobalSDF] Init resource validation failed — textures or pipeline INVALID\n";
    // Still set initialized_ so Shutdown() can clean up partial state.
    // But IsInitialized() will return false (see below).
}
```

Update `IsInitialized()`:
```cpp
bool GlobalSDF::IsInitialized() const {
    return initialized_ && init_resources_valid_;
}
```

**Partial-init handling:** If `init_resources_valid_` is false but `initialized_` is true (partial init), call `Shutdown()` at the end of `Initialize()` to release the partial resources, then set `initialized_ = false`. The dual flag is only meaningful during the init call itself. Pre-implementation step: read `GlobalSDF::Shutdown()` to confirm it handles partial state (textures/pipeline may be INVALID — Shutdown must check each before releasing).

### 4.3 `Engine/Graphics/Nanite/GlobalSDF.h`

**F4:** Add private member.

```cpp
private:
    bool init_resources_valid_ = false;
    // ... existing members
```

### 4.4 `Engine/Graphics/Lumen/DDGI/LumenDDGIPass.cpp`

**F3 (lines ~298-311):** Rewrite trace descriptor set layout bindings.

Replace existing trace layout with explicit sequential bindings matching WGSL:

```cpp
DescriptorSetLayoutBinding traceBindings[] = {
    // Textures (sampled) — sequential bindings 0..3
    {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // sdf_cascade_0
    {1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // sdf_cascade_1
    {2, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // sdf_cascade_2
    {3, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // prev_frame_color
    // Buffers — sequential bindings 4..8 (NO overlap with textures)
    {4, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // globalData
    {5, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // volume
    {6, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // ray_buffer (read_write)
    {7, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probeUpdateList (read)
    {8, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // irradiance_history (read)
};
traceBindings[0].is3D = true;
traceBindings[1].is3D = true;
traceBindings[2].is3D = true;
traceBindings[7].readonly = true;  // var<storage, read>
traceBindings[8].readonly = true;  // var<storage, read>
DescriptorSetLayoutDesc layoutDesc{9, traceBindings};
trace_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
```

**F3 (descriptor writes, lines 922-940):** Update `DescriptorData traceParams[]` to use bindings 0-8 directly (no remap). Current code uses Metal-style overlap (engine bindings 0-3 textures + 0-4 buffers) — change to sequential 0-8.

```cpp
DescriptorData traceParams[] = {
    {0, DescriptorType::SampledImage,  sdfTextures[0]},
    {1, DescriptorType::SampledImage,  sdfTextures[1]},
    {2, DescriptorType::SampledImage,  sdfTextures[2]},
    {3, DescriptorType::SampledImage,  prevColorTex},
    {4, DescriptorType::UniformBuffer, global_cb_[frameIdx]},
    {5, DescriptorType::UniformBuffer, volume_cb_[frameIdx]},
    {6, DescriptorType::StorageBuffer, ray_data_buffer_},
    {7, DescriptorType::StorageBuffer, probe_update_list_buffer_[frameIdx]},
    {8, DescriptorType::StorageBuffer, irradiance_buffers_[histIdx]},
};
UpdateDescriptorSet(device_, trace_ds_[frameIdx], traceParams, 9);
```

**Note on irradiance/depth layouts (verified):** `irradiance_set_layout_` (lines 325-338) and `depth_set_layout_` (lines 344-357) use sequential bindings 0-5 with no textures — no overlap, no remap, no fix needed. Risk 1 is lower than expected.

**Note on Metal backend:** Metal uses separate texture/buffer namespaces, so overlapping bindings work there. Changing to sequential 0-8 should also work on Metal (it just doesn't NEED overlap avoidance). Verify Metal test (`TestNaniteStreamingPipeline`) still works.

**Pre-implementation grep (mandatory):**
```bash
grep -n "traceBindings\|trace_set_layout_\|traceParams\|trace_ds_" Engine/Graphics/Lumen/DDGI/LumenDDGIPass.cpp
```
Confirm all references use the new sequential binding scheme.

## 5. Verification Protocol

### Layer 1: Validation (gating)

**Pass criteria:** Zero validation errors in 50s run.

```bash
cmake --build build_dawn --target TestDawnForwardRenderer -- -j 8
DAWN_FORCE_MODE=11 ./Darwin/Debug/TestDawnForwardRenderer >/tmp/m11.out 2>/tmp/m11.err &
PID=$!; sleep 50; kill $PID; wait $PID 2>/dev/null

L1_COUNT=$(grep -c "Validation\|Device error" /tmp/m11.out)
echo "L1 count: $L1_COUNT"
test "$L1_COUNT" -eq 0 && echo "L1 PASS" || echo "L1 FAIL"
```

**Fail action:** Read first validation error → likely F3 follow-on (UpdateIrradiance/UpdateDepth layout same pattern). Apply S1.

### Layer 2: Safety-net bypass (gating)

```bash
grep "\[Mode11\]" /tmp/m11.err
# Expected: "[Mode11] GlobalSDF available — runtime trace active"
```

**Fail action:** If `unavailable` appears → F4 sanity check rejecting init → recheck F1+F2.

### Layer 3: Visual divergence (manual)

User runs Mode 11 for 60+ frames, compares to Mode 10:
- Mode 11 should subtly differ from Mode 10 after EMA convergence
- Differences most visible in Sponza courtyard / under arches

**Fail action:** Apply S2 (drop L1 SH bands), retry L3.

### Verification matrix

| Outcome | Verdict | Action |
|---|---|---|
| L1 + L2 + L3 all pass | Done | Commit, write memory entry |
| L1 fails | Validation issue | Read error, likely apply S1 (more layouts) |
| L1+L2 pass, L3 fails | Algorithm issue | Apply S2 |
| L1 produces new error class | Unknown issue | Apply S4 (abort), write next follow-up |

## 6. Scope-Cut Menu

Applied in order S1 → S4 → S2 → S3. Each cut gets a comment in the relevant file with date + trigger observation.

| # | Trigger | Cut | Cost |
|---|---|---|---|
| **S1** | L1 fails with `ddgi_update_irradiance` or `ddgi_update_depth` binding error | Apply F3 pattern to those layouts too (rewrite to sequential 0-N matching WGSL) | Scope expansion within spec |
| **S4** | L1 produces unanticipated error class (e.g., texture upload, barrier) | Add defensive `IsInitialized()` guard that always returns false; Mode 11 = Mode 10 | Spec fails, next follow-up |
| **S2** | L1+L2 pass, L3 shows Mode 11 == Mode 10 | Drop L1 SH bands in `samplePrevProbeGrid` (L0 only) | Indirect lighting loses directional detail |
| **S3** | L3 shows flicker / NaN under camera motion | Throttle: voxelization every 2nd frame | Slightly stale SDF |

## 7. Risks

### Risk 1: F3 incomplete — UpdateIrradiance / UpdateDepth have same pattern

`LumenDDGIPass::CreateDescriptorSetLayouts()` (lines 325-357) defines 2 more layouts using the same Metal-style overlap. If they have the same bug, F3 fix on trace layout alone won't clear L1.

**Mitigation:** S1 covers this. Pre-implementation grep to read all 3 layouts upfront.

### Risk 2: Metal regression from F3

Changing C++ layout from overlapping to sequential bindings might break Metal backend if Metal code path assumes specific engine binding numbers.

**Mitigation:** Test `TestNaniteStreamingPipeline` (native Metal path) after F3. If broken, condition the layout on `device_->GetPlatform() == Dawn` (like GlobalSDF.cpp:374 does).

### Risk 3: F2 R32Float memory or read budget

3 cascades × 60³ × 4B = 5.2MB — within budget. DDGITraceRays reads 3 texture_3d in compute — Apple Silicon limit is ~4 reads (memory `apple-silicon-texture3d-limit`). Currently the shader reads from 3 cascades — already at limit. R32Float vs R16Float doesn't change read count.

**Mitigation:** If flicker appears under motion (Risk 3 materializes), apply S2/S3.

### Risk 4: F4 false negative — init succeeds but check rejects

If `INVALID_RESOURCE` macro changes or handles have surprising valid zero values, F4 might reject valid init.

**Mitigation:** Verify `rhi::handles::INVALID_RESOURCE` definition before implementing. Confirm existing textures (e.g., fallback diffuse) use same sentinel.

## 8. Out of Scope

- WASM port (deferred per project convention)
- MaterialInstance init performance (26s — separate optimization spec)
- Audit of other Dawn descriptor set layouts beyond DDGI family (Path B from brainstorming)
- DDGI trace shader correctness (this spec fixes bindings, not algorithm — algorithm is in `2026-07-10` Mode 11 plan)
- Probe classification / relocation (RTXGI features beyond canonical DDGI)

## 9. Implementation Order

Suggested task sequence:
1. F1: WGSL entry point rename — build, verify no compile error
2. F2: Texture format change (C++ + WGSL together) — build, smoke test Mode 11
3. F4: Add init sanity check — build, verify `[GlobalSDF] Init resource validation failed` does NOT appear in log
4. F3: DDGI trace binding layout rewrite (C++ layout + descriptor writes together)
5. Build, run full L1+L2 verification
6. If L1 fails with similar error on UpdateIrradiance/UpdateDepth, apply S1
7. L3 visual verification (user manual)
8. Apply remaining scope cuts as needed
9. Commit, write memory entry

## 10. Success Criteria

Spec is complete when all three hold:
1. `DAWN_FORCE_MODE=11 ./Darwin/Debug/TestDawnForwardRenderer` runs 60+ frames with **0** validation errors
2. Log contains `[Mode11] GlobalSDF available — runtime trace active` exactly once
3. Mode 11 visually differs from Mode 10 in Sponza courtyard after convergence

If S4 is applied, spec is not complete — write next follow-up targeting the new error class.
