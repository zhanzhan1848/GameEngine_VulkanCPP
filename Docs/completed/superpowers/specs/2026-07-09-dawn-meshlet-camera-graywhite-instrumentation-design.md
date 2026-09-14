# Dawn Meshlet Camera Gray-White Bug — Diagnostic Instrumentation Design

**Date:** 2026-07-09
**Status:** Approved (awaiting spec review)
**Scope:** Modes 7/8/9/10/11 — meshlet rendering paths in `TestDawnForwardRenderer`
**Approach:** Diagnostic-first (3 independent hooks), gather data before applying real fix

---

## 1. Problem Statement

Long-standing pre-existing bug (not introduced by Mode 11 work):

- **Symptom:** Moving the camera to specific positions in the Sponza scene triggers a full-screen gray-white (`~(0.5, 0.5, 0.5)` albedo clear color from cleared GBuffer).
- **Trigger:** Fixed position + fixed angle. Rotation unlocks it; switching to Mode 0–6 (non-meshlet) also unlocks it.
- **No movement = no recovery.**
- **Frequency:** Medium — occurs in several known Sponza regions.
- **Affects:** All meshlet modes (7/8/9/10/11).

**Why we cannot fix yet:** We don't know which of the 8 GPU culling stages is dropping the visibility, or whether it's bounds data, view matrix, or DrawIndirect args. No diagnostic exists.

## 2. Root Cause Hypotheses

The chain that produces gray-white:

```
visible_counter = 0  →  DrawIndirect(0 instances)  →  empty GBuffer  →  gray-white
```

Hypotheses for why `visible_counter = 0`:

| # | Hypothesis | Where it would fail |
|---|-----------|---------------------|
| H1 | Instance bounds contain NaN/Inf from `RenderSceneSnapshot::ComputeLocalBounds` | stage1 frustum test rejects all |
| H2 | Stage5 occlusion culling false-positive (HZB too coarse at specific view angles) | stage5 drops all visible clusters |
| H3 | Stage4 cluster expansion produces zero clusters (lod selection edge case) | stage4 emits nothing |
| H4 | `view_matrix` or `proj_matrix` becomes degenerate at specific yaw/pitch | stage1 transform breaks |
| H5 | `force_pass_all=0` is correct, but stage6 atomic race drops visibility counter | stage6 race |
| H6 | Per-geometry `bounds_radius` value corrupts cull test | stage1 sphere test |

Currently **all 6 are unfalsifiable** — no probe to disambiguate. This design adds the probes.

## 3. Design Overview

Three independent diagnostic hooks, all **off by default**, **zero-cost when not triggered**, and **non-invasive to render correctness**:

| Hook | Trigger | Action | Cost |
|------|---------|--------|------|
| **A1** Camera snapshot | Press `P` | stderr dump of `cameraPos/yaw/pitch/frame/mode` + visible counter (when available) | One print per keypress |
| **A2** Stage1 bypass toggle | Press `Ctrl+P` | Flip `CullingConstants.force_pass_all`; stage1 marks every instance visible, skipping frustum/sphere reject | Saves GPU work; no per-frame overhead when off |
| **A3** Bounds NaN auto-scan | Every frame (CPU side) | Scan instance bounds for NaN/Inf; emit one-shot warning per occurrence | ~µs per frame; < 50 instances |

These three together let us **bisect the bug class** in one user playthrough:

- If A3 fires → bounds data corruption (H1) → fix snapshot computation
- If A2 (stage1 bypass) clears the symptom → cause is in stage1 frustum/bounds test (H1/H6); a follow-up A4 hook can bisect stage4/5/6 if needed
- If A2 does NOT clear it → cause is in stage4/5/6 (cluster expansion, occlusion, compact) or post-cull (H2/H3/H5) → inspect indirect_commands buffer + per-stage counters
- A1 gives us the exact repro coordinates to diff against working state

## 4. Detailed Design

### Hook A1: Camera Snapshot on `P` Key

**File:** `EngineTest/IntegrationTests/TestDawnForwardRenderer.h`
```cpp
// Add to private members:
bool prevPState_{false};
```

**File:** `EngineTest/IntegrationTests/TestDawnForwardRenderer.cpp` — in `UpdateCamera(float dt)`

```cpp
// Edge-detect P key
bool pState = platform::IsKeyPressed(PlatformKey::P);
if (pState && !prevPState_) {
    std::fprintf(stderr,
        "[A1] camera snapshot: pos=(%.4f, %.4f, %.4f) yaw=%.4f pitch=%.4f "
        "frame=%u mode=%u\n",
        cameraPos_.x, cameraPos_.y, cameraPos_.z,
        cameraYaw_, cameraPitch_,
        totalFrames_, static_cast<u32>(renderMode_));
    std::fflush(stderr);
}
prevPState_ = pState;
```

**Notes:**
- Uses existing `platform::IsKeyPressed` API (same pattern as Tab key for mode switch, see line ~161 in `.cpp`).
- Format is grep-friendly (`[A1]` prefix) so logs can be filtered during analysis.
- No effect on render path.

### Hook A2: Stage1 Bypass Toggle on `Ctrl+P`

This is the **bisect switch**. Flipping it lets the user verify whether the gray-white is caused by stage1 frustum/sphere rejection (H1/H6) or by downstream stages 4/5/6 (H2/H3/H5).

**File:** `Engine/Graphics/Nanite/GPUCullingPipeline.h`
```cpp
// In public API:
void SetForcePassAll(bool enabled) { force_pass_all_debug_ = enabled; }
bool IsForcePassAll() const { return force_pass_all_debug_; }

// In private members:
bool force_pass_all_debug_{false};
```

**File:** `Engine/Graphics/Nanite/GPUCullingPipeline.cpp` — in `Execute(...)` where `CullingConstants` is filled (around line 1565):
```cpp
constants.force_pass_all = force_pass_all_debug_ ? 1u : 0u;
```

(Replaces the current hardcoded `constants.force_pass_all = 0;`)

**File:** `EngineTest/IntegrationTests/TestDawnForwardRenderer.h`
```cpp
bool prevCtrlPState_{false};
```

**File:** `EngineTest/IntegrationTests/TestDawnForwardRenderer.cpp` — in `UpdateCamera(float dt)`:
```cpp
// Edge-detect Ctrl+P combination (Ctrl held + P pressed)
bool ctrlState = platform::IsKeyPressed(PlatformKey::LeftControl) ||
                 platform::IsKeyPressed(PlatformKey::RightControl);
bool pState = platform::IsKeyPressed(PlatformKey::P);
bool ctrlP = ctrlState && pState;
if (ctrlP && !prevCtrlPState_) {
    auto* cullPipeline = primal::graphics::nanite::GPUCullingPipeline::Get();
    bool newVal = !cullPipeline->IsForcePassAll();
    cullPipeline->SetForcePassAll(newVal);
    std::fprintf(stderr, "[A2] force_pass_all = %s\n",
                 newVal ? "TRUE (no culling)" : "FALSE (culling on)");
    std::fflush(stderr);
}
prevCtrlPState_ = ctrlP;
```

**Notes:**
- `force_pass_all` is already consumed in `GPUCullingPipeline.wgsl` (stage1 reads it) — when set, every instance is forced visible.
- This does NOT bypass stage5 occlusion culling alone — `force_pass_all` short-circuits earlier in stage1. To confirm: stage1 should `if (uniforms.force_pass_all != 0u) { mark visible; continue; }` at top of the frustum test. **Verification step in implementation**: read `GPUCullingPipeline.wgsl:stage1_instance_frustum_culling` and confirm `force_pass_all` is checked first, before any other cull test. If not, extend it.

### Hook A3: Bounds NaN Auto-Scan

**File:** `EngineTest/IntegrationTests/TestDawnForwardRenderer.cpp` — at the start of `RenderMeshletFrame(...)`:

```cpp
// One-shot dedup so we don't spam stderr every frame for the same instance
static u32 lastBoundsWarningFrame_{0xFFFFFFFF};

if (totalFrames_ - lastBoundsWarningFrame_ > 60u) {  // rate-limit: 1/sec at 60fps
    const auto& snapshot = meshletSceneSnapshot_;
    bool anyBad = false;
    for (u32 i = 0; i < snapshot.GetInstanceCount(); ++i) {
        const auto& b = snapshot.GetInstanceBounds(i);
        if (std::isnan(b.center.x) || std::isnan(b.center.y) || std::isnan(b.center.z) ||
            std::isnan(b.radius) ||
            std::isinf(b.center.x) || std::isinf(b.center.y) || std::isinf(b.center.z) ||
            std::isinf(b.radius)) {
            std::fprintf(stderr,
                "[A3] NaN/Inf in instance[%u] bounds: center=(%f, %f, %f) radius=%f "
                "frame=%u\n",
                i, b.center.x, b.center.y, b.center.z, b.radius, totalFrames_);
            std::fflush(stderr);
            anyBad = true;
        }
    }
    if (anyBad) lastBoundsWarningFrame_ = totalFrames_;
}
```

**Notes:**
- Actual API names (`GetInstanceCount`, `GetInstanceBounds`) need verification during implementation — `RenderSceneSnapshot` exposes either this or a vector accessor. Check `Engine/Graphics/Scene/RenderSceneSnapshot.h` first.
- Rate-limited so a persistent NaN doesn't flood stderr — one warning per second is enough.
- < 50 instances currently, so the scan is essentially free.

## 5. Files to Modify

| File | Change |
|------|--------|
| `Engine/Graphics/Nanite/GPUCullingPipeline.h` | Add `SetForcePassAll`/`IsForcePassAll` + private bool |
| `Engine/Graphics/Nanite/GPUCullingPipeline.cpp` | Wire debug bool into `CullingConstants.force_pass_all` |
| `EngineTest/IntegrationTests/TestDawnForwardRenderer.h` | Add `prevPState_`, `prevCtrlPState_` |
| `EngineTest/IntegrationTests/TestDawnForwardRenderer.cpp` | Add A1/A2/A3 hooks; verify `force_pass_all` ordering in stage1 |

## 6. Verification

After implementing:

1. **Build (native):**
   ```
   cmake --build build_dawn --target TestDawnForwardRenderer
   ```

2. **A1 smoke test:** Launch any meshlet mode, press `P`. Confirm stderr shows snapshot line with current `cameraPos`. Move camera and press again — values change.

3. **A2 smoke test:** Launch Mode 7, navigate to a known repro position (gray-white). Press `Ctrl+P`. Confirm:
   - stderr shows `[A2] force_pass_all = TRUE`
   - **If screen recovers:** cause is in stage1 frustum/bounds test (H1/H6) — go inspect `RenderSceneSnapshot::ComputeLocalBounds` or stage1 sphere test
   - **If screen does NOT recover:** cause is in stage4/5/6 or post-cull (H2/H3/H5) — follow-up A4 hook needed for per-stage counters
   - Press `Ctrl+P` again → `[A2] force_pass_all = FALSE` → symptom returns

4. **A3 smoke test:** No-op in clean state. If A3 ever fires, that's the root cause.

5. **User reproduces the bug:** Navigate to a known gray-white position with these hooks in place. Capture:
   - The `[A1]` line at the exact trigger position
   - Whether `[A2]` (no-cull) clears it
   - Whether `[A3]` ever fires
   - Share this log → next iteration localizes the bug to one of H1–H6.

## 7. Out of Scope

- The actual fix for the gray-white bug — this design is purely diagnostic. Once we have data from a real repro, a separate spec will propose the fix.
- Per-stage visibility counters (could add a `[A4]` later if needed to bisect between stages 1/4/5/6, but defer until A2 tells us culling is the culprit).
- Persistent logging to disk. stderr-only is fine for native dev. WASM later.

## 8. Risks

- `force_pass_all` ordering in WGSL stage1 must be **before** all other cull tests. If currently mid-function, we'd need to add an early-out at the top. Verified during implementation, not before.
- `Ctrl+P` may collide with OS shortcut (none on macOS for native app window, but verify). If collision, fall back to a different key (e.g., `Backslash`).
- A3 rate-limit logic assumes 60fps; at much higher fps the warning may be more frequent than 1/sec. Acceptable.

## 9. Implementation Order

1. Header changes (small, fast): `.h` for `GPUCullingPipeline` + `TestDawnForwardRenderer`.
2. A1 (smallest, validates key edge-detect pattern).
3. A2 (verify WGSL `force_pass_all` ordering first; this is the highest-value hook).
4. A3 (verify `RenderSceneSnapshot` API; smallest in scope).
5. Build + smoke test each hook before moving on.
6. Tag build as `diagnostic-v1`, hand to user for repro session.

---

**Post-data analysis plan** (not part of this spec): based on A1/A2/A3 output, write a follow-up spec for the actual fix targeting H1–H6.
