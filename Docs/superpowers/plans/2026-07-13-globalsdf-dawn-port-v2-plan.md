# GlobalSDF Dawn Port v2 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix 4 runtime-verified blockers (F1–F4) so Mode 11 produces canonical dynamic DDGI on Dawn with 0 validation errors.

**Architecture:** Mechanical fixes to existing code — no new files, no RHI changes. Rename WGSL entry point (F1); swap R16Float → R32Float on both sides (F2); add `init_resources_valid_` flag to make `IsInitialized()` trustworthy under partial init (F4); rewrite LumenDDGIPass trace layout + descriptor writes to use sequential engine bindings 0–8 matching WGSL, eliminating the Dawn remap collision (F3).

**Tech Stack:** C++17, Dawn (WebGPU native), WGSL, CMake.

**Spec reference:** `Docs/superpowers/specs/2026-07-13-globalsdf-dawn-port-v2-design.md`

---

## File Structure

| File | Change |
|------|--------|
| `Engine/Graphics/Dawn/shaders/Nanite/GlobalSDFVoxelization.wgsl` | F1: rename `voxelize_sdf_main` → `voxelize_sdf` (line 113). F2: `r16float` → `r32float` (line 103). |
| `Engine/Graphics/Nanite/GlobalSDF.cpp` | F2: `R16_Float` → `R32_Float` (line 202). F4: add `init_resources_valid_` check after AllocateTexture succeeds, plus dual-flag handling on failure. |
| `Engine/Graphics/Nanite/GlobalSDF.h` | F4: add `bool init_resources_valid_{ false };` to private members, update `IsInitialized()`. |
| `Engine/Graphics/Lumen/DDGI/LumenDDGIPass.cpp` | F3: rewrite traceBindings[] (lines 298–311) and traceParams[] (lines 922–940) to use sequential engine bindings 0–8. |

**No new files. No RHI changes. No irradiance/depth layout changes (already sequential — verified).**

---

## Task 1: F1 — Rename WGSL entry point

**Why:** C++ calls `CreateShader(..., "voxelize_sdf")` (GlobalSDF.cpp:363). WGSL declares `fn voxelize_sdf_main`. Dawn rejects pipeline creation with `Entry point "voxelize_sdf" doesn't exist in the shader module`. Renaming WGSL is 1 line; the C++ string stays (used in 1 place).

**Files:**
- Modify: `Engine/Graphics/Dawn/shaders/Nanite/GlobalSDFVoxelization.wgsl:113`

- [ ] **Step 1: Read the current entry point declaration**

```bash
grep -n "fn voxelize_sdf" Engine/Graphics/Dawn/shaders/Nanite/GlobalSDFVoxelization.wgsl
```

Expected output:
```
113:fn voxelize_sdf_main(@builtin(global_invocation_id) tid: vec3<u32>) {
```

- [ ] **Step 2: Rename the entry point**

In `Engine/Graphics/Dawn/shaders/Nanite/GlobalSDFVoxelization.wgsl`, change line 113:

```wgsl
@compute @workgroup_size(4, 4, 4)
fn voxelize_sdf(@builtin(global_invocation_id) tid: vec3<u32>) {
```

(was: `fn voxelize_sdf_main(...)`)

- [ ] **Step 3: Verify no other references to the old name**

```bash
grep -rn "voxelize_sdf_main" Engine/ EngineTest/
```

Expected: empty. (If non-empty, those callers also need updating — but C++ uses `"voxelize_sdf"` without `_main`, so this should be clean.)

- [ ] **Step 4: Smoke-build (no test yet — F2 + F4 still block runtime)**

```bash
cmake --build build_dawn --target TestDawnForwardRenderer -- -j 8 2>&1 | tail -5
```

Expected: build succeeds (shader compiles at runtime, not at CMake time, so this just confirms the WGSL file still parses as text).

- [ ] **Step 5: Commit**

```bash
git add Engine/Graphics/Dawn/shaders/Nanite/GlobalSDFVoxelization.wgsl
git commit -m "fix(globalSDF/dawn): rename WGSL entry point to match C++ (F1)

C++ CreateShader calls \"voxelize_sdf\"; WGSL declared voxelize_sdf_main.
Dawn rejected pipeline creation. Rename WGSL to match C++ call site."
```

---

## Task 2: F2 — Texture format R16Float → R32Float (WGSL + C++)

**Why:** WebGPU spec disallows R16Float as storage texture format. Dawn validation rejects the texture creation. R32Float is on the allowlist, 4 bytes/voxel (same as RGBA8), no SDF precision recalculation needed. Memory: 3 cascades × 128³ × 4B = 25MB (verified within budget — note: spec said 60³ = 5.2MB, actual `base_resolution` is 128 per GlobalSDFConfig default, so 25MB; still well within Apple Silicon limits).

**Files:**
- Modify: `Engine/Graphics/Dawn/shaders/Nanite/GlobalSDFVoxelization.wgsl:103`
- Modify: `Engine/Graphics/Nanite/GlobalSDF.cpp:202`

- [ ] **Step 1: WGSL — change storage texture format**

In `Engine/Graphics/Dawn/shaders/Nanite/GlobalSDFVoxelization.wgsl`, change line 103:

```wgsl
@group(0) @binding(0) var sdf_output: texture_storage_3d<r32float, write>;
```

(was: `texture_storage_3d<r16float, write>`)

- [ ] **Step 2: C++ — change DataFormat**

In `Engine/Graphics/Nanite/GlobalSDF.cpp`, change line 202 inside `AllocateTexture`:

```cpp
    desc.format = rhi::DataFormat::R32_Float;
```

(was: `desc.format = rhi::DataFormat::R16_Float;`)

- [ ] **Step 3: Verify both sides match**

```bash
grep -n "R32_Float\|R16_Float" Engine/Graphics/Nanite/GlobalSDF.cpp
grep -n "r32float\|r16float" Engine/Graphics/Dawn/shaders/Nanite/GlobalSDFVoxelization.wgsl
```

Expected:
- `GlobalSDF.cpp:202:    desc.format = rhi::DataFormat::R32_Float;`
- `GlobalSDFVoxelization.wgsl:103:... texture_storage_3d<r32float, write>;`

No `R16_Float` or `r16float` remaining in either file.

- [ ] **Step 4: Build**

```bash
cmake --build build_dawn --target TestDawnForwardRenderer -- -j 8 2>&1 | tail -5
```

Expected: build succeeds.

- [ ] **Step 5: Commit**

```bash
git add Engine/Graphics/Dawn/shaders/Nanite/GlobalSDFVoxelization.wgsl Engine/Graphics/Nanite/GlobalSDF.cpp
git commit -m "fix(globalSDF/dawn): R16Float → R32Float for storage texture (F2)

WebGPU spec disallows R16Float as storage texture format. Switch both
C++ DataFormat and WGSL texture_storage_3d to R32Float. 4B/voxel, no
precision recalculation needed."
```

---

## Task 3: F4 — Init resource validity check

**Why:** `GlobalSDF::Initialize` sets `initialized_ = true` at line 49 after texture allocation succeeds. Separately, `InitVoxelization` creates the shader + pipeline and can fail (would have failed under B1+B2). When `InitVoxelization` fails, `initialized_` stays `true`, so consumers (LumenDDGIPass) see `IsInitialized()==true` and proceed with corrupt state. Add a `init_resources_valid_` flag that gates `IsInitialized()`.

**Approach:** Use a dual-flag pattern — `initialized_` tracks "we went through Initialize()" (so Shutdown cleans up), `init_resources_valid_` tracks "all GPU resources are usable". `IsInitialized()` returns both. On partial init, call `Shutdown()` to release partial state and reset both flags.

**Files:**
- Modify: `Engine/Graphics/Nanite/GlobalSDF.h` (add member, update IsInitialized)
- Modify: `Engine/Graphics/Nanite/GlobalSDF.cpp` (validate resources, partial-init handling)

- [ ] **Step 1: Read current GlobalSDF.h to confirm member layout**

```bash
grep -n "IsInitialized\|initialized_\|voxelization_ready_\|private:" Engine/Graphics/Nanite/GlobalSDF.h
```

Expected:
- Line 71: `bool IsInitialized() const { return initialized_; }`
- Line 96: `std::atomic<bool> initialized_{ false };`
- Line 99: `bool voxelization_ready_{ false };`

- [ ] **Step 2: Read current Shutdown() to confirm partial-state handling**

```bash
sed -n '53,73p' Engine/Graphics/Nanite/GlobalSDF.cpp
```

Expected: `Shutdown()` already guards every resource with `!= INVALID_RESOURCE` check (lines 59, 65). Safe to call on partial state.

- [ ] **Step 3: Add `init_resources_valid_` member to GlobalSDF.h**

In `Engine/Graphics/Nanite/GlobalSDF.h`, change line 96:

```cpp
    std::atomic<bool> initialized_{ false };
    bool init_resources_valid_{ false };  // F4: true only if all GPU resources non-INVALID
```

- [ ] **Step 4: Update `IsInitialized()` in GlobalSDF.h**

Change line 71:

```cpp
    bool IsInitialized() const {
        return initialized_ && init_resources_valid_;
    }
```

- [ ] **Step 5: Set `init_resources_valid_` at end of successful Initialize()**

In `Engine/Graphics/Nanite/GlobalSDF.cpp`, change lines 41–50:

```cpp
    if (!CreateCascades()) {
        return false;
    }

    if (!CreateGlobalTexture()) {
        return false;
    }

    // F4: verify all cascade textures and the global texture are non-INVALID.
    // CreateCascades/CreateGlobalTexture already return false on allocation
    // failure, but belt-and-suspenders guards against silent regressions.
    init_resources_valid_ = true;
    for (const auto& cascade : cascades_) {
        if (cascade.sdf_texture == rhi::handles::INVALID_RESOURCE) {
            init_resources_valid_ = false;
            break;
        }
    }
    if (global_sdf_texture_ == rhi::handles::INVALID_RESOURCE) {
        init_resources_valid_ = false;
    }

    initialized_ = true;
    return true;
```

- [ ] **Step 6: Extend `InitVoxelization` failure path to flip `init_resources_valid_`**

In `Engine/Graphics/Nanite/GlobalSDF.cpp`, find `InitVoxelization` (around line 350). At every early-`return false` site (currently lines 359, 366, 424), add a partial-init cleanup right before returning.

For each early-return, insert before the `return false`:

```cpp
        // F4: shader/pipeline creation failed — IsInitialized() must return false
        // so consumers don't proceed with corrupt state. Shutdown() releases the
        // partial texture resources (already created in Initialize()), then we
        // clear both flags so Shutdown() can be called again safely if needed.
        Shutdown();  // safe: guards every release with INVALID_RESOURCE check
        return false;
```

Apply at:
- After line 358 (`Failed to load voxelization shader`)
- After line 365 (`Failed to compile voxelization shader`)
- After line 423 (`Failed to create voxelization pipeline`)

The successful path at line 442 (`voxelization_ready_ = true; return true;`) leaves `init_resources_valid_ = true` (set in Initialize), so `IsInitialized()` returns true.

- [ ] **Step 7: Build**

```bash
cmake --build build_dawn --target TestDawnForwardRenderer -- -j 8 2>&1 | tail -5
```

Expected: build succeeds.

- [ ] **Step 8: Commit**

```bash
git add Engine/Graphics/Nanite/GlobalSDF.h Engine/Graphics/Nanite/GlobalSDF.cpp
git commit -m "fix(globalSDF/dawn): add init_resources_valid_ gate on IsInitialized (F4)

IsInitialized returned true even when InitVoxelization failed (shader or
pipeline compile error). Consumers proceeded with corrupt state, then
cascaded device corruption to all subsequent pipelines. Dual-flag pattern:
initialized_ tracks Initialize() completion; init_resources_valid_ gates
IsInitialized. Partial-init calls Shutdown() to release textures."
```

---

## Task 4: F3 — DDGI trace descriptor layout (C++ side)

**Why:** `LumenDDGIPass` trace layout uses Metal-style overlap (textures 0–3, buffers 0–4). `DawnDescriptorSetLayout::Initialize` detects collisions and silently remaps buffers to WGPU bindings 5–8, with `irradiance_history` staying at WGPU 4. But WGSL declares bindings 4=globalData, 5=volume, 6=ray_buffer, 7=probeUpdateList, 8=irradiance_history — opposite order. Validation fails on binding 6 with type mismatch (shader=Storage, layout=Uniform). Fix by renumbering C++ buffers to 4–8 (no overlap, no remap, WGPU binding = engine binding = WGSL binding).

**Files:**
- Modify: `Engine/Graphics/Lumen/DDGI/LumenDDGIPass.cpp:298-320` (traceBindings)
- Modify: `Engine/Graphics/Lumen/DDGI/LumenDDGIPass.cpp:922-940` (traceParams)

- [ ] **Step 1: Read current traceBindings + traceParams to confirm exact lines**

```bash
grep -n "traceBindings\|traceParams\|trace_set_layout_\|trace_ds_" Engine/Graphics/Lumen/DDGI/LumenDDGIPass.cpp
```

Expected (approximate):
- Line 298: `DescriptorSetLayoutBinding traceBindings[] = {`
- Line 319: `DescriptorSetLayoutDesc layoutDesc{9, traceBindings};`
- Line 922: `DescriptorData traceParams[] = {`
- Line 940: `UpdateDescriptorSet(device_, trace_ds_[frameIdx], traceParams, 9);`

- [ ] **Step 2: Rewrite traceBindings[] to use sequential engine bindings 0–8**

In `Engine/Graphics/Lumen/DDGI/LumenDDGIPass.cpp`, replace lines 298–321 (the entire block including the traceBindings array, the is3D/readonly flags, the layoutDesc, and the CreateDescriptorSetLayout call) with:

```cpp
        DescriptorSetLayoutBinding traceBindings[] = {
            // Textures (sampled) — sequential bindings 0..3 matching WGSL
            {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},   // SDF cascade 0
            {1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},   // SDF cascade 1
            {2, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},   // SDF cascade 2
            {3, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},   // prev frame color (unused in Mode 11 but kept for layout stability)
            // Buffers — sequential bindings 4..8 (NO overlap with textures, so Dawn won't remap)
            {4, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},   // GlobalShaderData
            {5, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},   // DDGIVolumeData
            {6, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},   // ray data (read_write)
            {7, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},   // probe update list (read)
            {8, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},   // irradiance_history (read)
        };
        // SDF cascades are texture_3d<f32> in WGSL — layout must declare 3D view dim.
        traceBindings[0].is3D = true;
        traceBindings[1].is3D = true;
        traceBindings[2].is3D = true;
        // Read-only storage buffers (var<storage, read> in WGSL).
        traceBindings[7].readonly = true;  // probe update list
        traceBindings[8].readonly = true;  // irradiance_history
        DescriptorSetLayoutDesc layoutDesc{9, traceBindings};
        trace_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
```

**Note:** The old block included a header comment explaining Metal-style overlap + Dawn remap. Delete that comment — it no longer applies once we eliminate the overlap. The WGSL bindings are sequential 0–8 (verified at `DDGITraceRays.wgsl:404–417`).

- [ ] **Step 3: Rewrite traceParams[] (descriptor writes) to use sequential engine bindings 0–8**

In `Engine/Graphics/Lumen/DDGI/LumenDDGIPass.cpp`, replace lines 922–940 (the `DescriptorData traceParams[] = { ... }; UpdateDescriptorSet(...)` block) with:

```cpp
                DescriptorData traceParams[] = {
                    // Textures: SDF cascades + prev frame lit scene color
                    {0, DescriptorType::SampledImage,  sdfTextures[0]},
                    {1, DescriptorType::SampledImage,  sdfTextures[1]},
                    {2, DescriptorType::SampledImage,  sdfTextures[2]},
                    {3, DescriptorType::SampledImage,  prevColorTex},
                    // Buffers — sequential bindings 4..8 matching WGSL.
                    // No texture/buffer collision → no Dawn remap → WGPU binding == engine binding.
                    {4, DescriptorType::UniformBuffer, global_cb_[frameIdx]},
                    {5, DescriptorType::UniformBuffer, volume_cb_[frameIdx]},
                    {6, DescriptorType::StorageBuffer, ray_data_buffer_},
                    {7, DescriptorType::StorageBuffer, probe_update_list_buffer_[frameIdx]},
                    // Mode 11: previous-frame irradiance probe grid (L_i_prev source).
                    // histIdx captured by execute lambda; irradiance_buffers_ holds prior-frame
                    // SH coefficients written by UpdateIrradiance last frame. Frame 0 reads the
                    // static-bake seed copied in by InitializeProbesFromStatic.
                    {8, DescriptorType::StorageBuffer, irradiance_buffers_[histIdx]},
                };
                UpdateDescriptorSet(device_, trace_ds_[frameIdx], traceParams, 9);
```

- [ ] **Step 4: Verify the rewrite**

```bash
grep -n "DescriptorType::" Engine/Graphics/Lumen/DDGI/LumenDDGIPass.cpp | grep -E "traceBindings|traceParams" 
sed -n '298,320p' Engine/Graphics/Lumen/DDGI/LumenDDGIPass.cpp
sed -n '920,945p' Engine/Graphics/Lumen/DDGI/LumenDDGIPass.cpp
```

Expected:
- traceBindings: 9 entries, bindings 0–3 are SampledImage, 4–8 are 2×Uniform + 3×Storage, no Metal-style overlap
- traceParams: 9 entries with binding numbers matching traceBindings exactly

- [ ] **Step 5: Build**

```bash
cmake --build build_dawn --target TestDawnForwardRenderer -- -j 8 2>&1 | tail -5
```

Expected: build succeeds.

- [ ] **Step 6: Commit**

```bash
git add Engine/Graphics/Lumen/DDGI/LumenDDGIPass.cpp
git commit -m "fix(ddgi/dawn): trace layout sequential bindings matching WGSL (F3)

Metal-style overlap (textures 0-3 + buffers 0-4) triggered silent Dawn
remap that left WGPU binding 4 = irradiance_history, but WGSL declared
binding 4 = globalData. Validation failed on binding 6 with type mismatch
(shader Storage, layout Uniform). Renumber C++ buffers to 4-8 so WGPU
binding == engine binding == WGSL binding. No shader change needed."
```

---

## Task 5: Layer 1 + Layer 2 verification (validation errors + safety-net bypass)

**Why:** F1–F4 are mechanical fixes; verification must prove they actually cleared the runtime errors and unblocked Mode 11 dispatch. L1 (validation) is gating — 0 errors required. L2 (safety-net bypass) proves GlobalSDF init succeeds and LumenDDGIPass dispatches the trace pipeline.

- [ ] **Step 1: Run TestDawnForwardRenderer under DAWN_FORCE_MODE=11 for 50s**

```bash
cmake --build build_dawn --target TestDawnForwardRenderer -- -j 8
DAWN_FORCE_MODE=11 ./Darwin/Debug/TestDawnForwardRenderer >/tmp/m11.out 2>/tmp/m11.err &
PID=$!; sleep 50; kill $PID; wait $PID 2>/dev/null
```

- [ ] **Step 2: Layer 1 — count validation errors**

```bash
L1_COUNT=$(grep -c "Validation\|Device error" /tmp/m11.out /tmp/m11.err 2>/dev/null | awk -F: '{s+=$2} END {print s}')
echo "L1 count: $L1_COUNT"
test "$L1_COUNT" -eq 0 && echo "L1 PASS" || echo "L1 FAIL"
```

Expected: `L1 count: 0` + `L1 PASS`.

- [ ] **Step 3: Layer 2 — verify safety-net bypassed**

```bash
grep "\[Mode11\]" /tmp/m11.err /tmp/m11.out
```

Expected: includes `[Mode11] GlobalSDF available — runtime trace active` (exact string from LumenDDGIPass safety-net code).

If `[Mode11] GlobalSDF unavailable — falling back to static seed` appears instead, F4 rejected init — go back and recheck F1/F2.

- [ ] **Step 4: If L1 FAILs, read first validation error**

```bash
grep -m1 "Validation\|Device error" /tmp/m11.out /tmp/m11.err
```

Likely failure modes (per spec Section 5):
- `ddgi_update_irradiance` or `ddgi_update_depth` binding error → apply S1 (apply F3 pattern to those layouts)
- Different error class → escalate (S4)

- [ ] **Step 5: Scope-cut S1 (conditional, only if L1 fails on UpdateIrradiance/UpdateDepth)**

The irradiance and depth layouts (lines 325–357 of LumenDDGIPass.cpp) use sequential bindings 0–5 with no textures — verified during spec writing. **S1 should not be needed**, but if L1 fails with those pipelines, the audit was wrong. Apply F3 pattern (renumber any overlapping bindings to sequential integers matching their WGSL) and re-run L1.

- [ ] **Step 6: Metal regression check — TestNaniteStreamingPipeline**

F3 changed the C++ descriptor layout from Metal-style overlap (textures 0–3, buffers 0–4) to sequential (0–8). Metal uses separate texture/buffer namespaces, so sequential bindings also work — but the change might expose latent assumptions in the Metal backend. Verify no regression.

```bash
cmake --build build --target TestNaniteStreamingPipeline -- -j 8 2>&1 | tail -3
./Darwin/Debug/TestNaniteStreamingPipeline >/tmp/metal.out 2>/tmp/metal.err &
PID=$!; sleep 10; kill $PID; wait $PID 2>/dev/null
grep -c "error\|FAIL\|crash" /tmp/metal.out /tmp/metal.err
```

Expected: 0 errors. The test should render Sponza normally (no validation errors on Metal since Metal has no validation layer, but no crash/abort either).

If the Metal backend breaks, apply spec Risk 2 mitigation: condition the layout on `device_->GetPlatform() == rhi::RHIPlatform::Dawn` and keep the Metal-style overlap for the Metal path (see `GlobalSDF.cpp:374` for the pattern — it already does this for the voxelization layout).

- [ ] **Step 7: Record result in verification log**

If L1 + L2 pass, add a result section to the bottom of the spec file:

```bash
cat >> Docs/superpowers/specs/2026-07-13-globalsdf-dawn-port-v2-design.md <<'EOF'

---

## 11. Verification Result (YYYY-MM-DD)

- L1 (validation): PASS — 0 errors in 50s run
- L2 (safety-net): PASS — `[Mode11] GlobalSDF available — runtime trace active` fires
- L3 (visual): pending user run
EOF
```

Replace YYYY-MM-DD with today's date from `date +%Y-%m-%d`.

---

## Task 6: Layer 3 visual verification (manual, user-run)

**Why:** L1+L2 prove correctness plumbing, but Mode 11 must visibly differ from Mode 10 after probe convergence (~60 frames). Only a human can verify this.

**This task is user-driven — no code changes from the implementer unless scope cuts trigger.**

- [ ] **Step 1: User runs Mode 11 for 60+ frames, compares to Mode 10**

User instructions (provided by implementer in handoff message):

```
1. DAWN_FORCE_MODE=10 ./Darwin/Debug/TestDawnForwardRenderer  # baseline
   - Walk to Sponza courtyard, screenshot.
2. DAWN_FORCE_MODE=11 ./Darwin/Debug/TestDawnForwardRenderer  # dynamic
   - Same camera pose after ~60 frames, screenshot.
3. Compare: Mode 11 should subtly differ from Mode 10 in indirect lighting
   under arches / in shadowed corners. Not a huge change — Mode 10 static
   bake is already close to converged.
```

- [ ] **Step 2: If Mode 11 == Mode 10 visually (L3 FAIL), apply S2**

Per spec Section 6, S2 = drop L1 SH bands in `samplePrevProbeGrid` (L0 only). Trade-off: indirect lighting loses directional detail.

In `Engine/Graphics/Dawn/shaders/Lumen/DDGITraceRays.wgsl`, find `samplePrevProbeGrid` (added in Mode 11 work). Reduce SH read loop from 4 bands to 1:

```wgsl
// S2 scope cut: L0 only (was 4 bands). Recovers visual divergence at the
// cost of high-frequency indirect detail.
for (var i: u32 = 0u; i < 1u; i++) {  // was 4u
```

Rebuild and re-run L3.

- [ ] **Step 3: If Mode 11 flickers/NaN under camera motion (L3 FAIL), apply S3**

S3 = throttle voxelization to every 2nd frame. In `RenderMeshletDDGIFrame` or the dispatch site, gate on `frameIdx % 2 == 0`. Trade-off: SDF is one frame stale.

---

## Task 7: Write memory entry + final commit

**Why:** Capture what was actually fixed (vs what the v1 audit claimed) so future GlobalSDF work doesn't re-discover these blockers. Update existing memory or add new one.

- [ ] **Step 1: Update or supersede the existing memory entry**

Check existing memory:

```bash
ls ~/.claude/projects/-Users-zhanyuanwei-Desktop-GameEngine-VulkanCPP/memory/ | grep -i globalsdf
```

If `globalsdf-dawn-port-real-blockers.md` exists (it should, from v1 work), update it to note the v2 fixes landed:

```markdown
---
name: GlobalSDF Dawn Port Real Blockers
description: 4 blockers for GlobalSDF Dawn port (F1-F4 fixes applied YYYY-MM-D); v2 spec landed
type: project
---
[existing content]

## Update (YYYY-MM-DD): v2 fixes landed

All 4 blockers (entry point, R16Float, binding layout, init_resources_valid_)
fixed per `Docs/superpowers/specs/2026-07-13-globalsdf-dawn-port-v2-design.md`.
Mode 11 now produces canonical dynamic DDGI on Dawn.
```

Otherwise, write a new memory entry covering the v2 fix.

- [ ] **Step 2: Commit any remaining doc/memory updates**

```bash
git add Docs/superpowers/specs/2026-07-13-globalsdf-dawn-port-v2-design.md
git status
# Review what's staged before committing
git commit -m "docs(globalSDF/dawn): v2 verification result + memory update"
```

(The memory file is in `~/.claude/...`, not the repo — it doesn't get committed. Only the spec update does.)

- [ ] **Step 3: Final smoke test — native run with no force-mode**

```bash
./Darwin/Debug/TestDawnForwardRenderer  # default mode
```

Expected: defaults to Mode 0 or whatever the test default is — verify it doesn't regress (no validation errors, Sponza renders normally). This catches any accidental fallout from F3 changes to LumenDDGIPass on non-DDGI modes.

---

## Implementation Order

Tasks 1 → 4 are the code changes. Task 5 is automated verification. Task 6 is manual. Task 7 is bookkeeping.

Suggested dispatch sequence for subagent-driven execution:

1. **Task 1** (F1 entry point rename) — trivial, fast model OK
2. **Task 2** (F2 texture format) — trivial, fast model OK
3. **Task 3** (F4 init check) — touches 2 files, multiple edit sites; standard model
4. **Task 4** (F3 binding layout) — touches 2 sites in 1 file, careful renumbering; standard model
5. **Task 5** (L1+L2 verification) — orchestrator-driven, no subagent; if L1 fails, dispatch S1 subagent
6. **Task 6** (L3 visual) — user-driven; dispatch subagent only if S2/S3 trigger
7. **Task 7** (memory + final commit) — orchestrator-driven

**Tasks 1 and 2 can be batched into a single subagent** if dispatched together (both edit the same WGSL file). Otherwise separate.

## Self-Review Checklist

- [ ] All 4 spec fixes (F1, F2, F3, F4) have a task
- [ ] No placeholder code — every step shows exact code
- [ ] Type names match across tasks (`init_resources_valid_`, `cascades_[i].sdf_texture`, `voxelize_sdf`)
- [ ] Verification protocol from spec Section 5 is wired in (L1+L2+L3)
- [ ] Scope-cut menu S1/S2/S3 from spec Section 6 referenced as conditional tasks
- [ ] Spec risks (Metal regression, R32Float memory, F4 false negative) addressed either in task steps or notes
