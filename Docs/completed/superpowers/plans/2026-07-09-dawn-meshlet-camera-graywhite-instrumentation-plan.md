# Dawn Meshlet Camera Gray-White Diagnostic Instrumentation — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add 3 independent diagnostic hooks (P-key camera dump, Ctrl+P bypass toggle, bounds NaN auto-scan) so a real user repro of the gray-white screen bug yields data that bisects the root cause.

**Architecture:** All hooks are macOS-native-only for now (bug repro is on macOS native; WASM variants are deferred). Hook A2 flips an existing `CullingConstants.force_pass_all` field that stage4 of `GPUCullingPipeline.wgsl` already consumes. A1/A3 are pure C++ test-harness code. Each hook is independently toggleable and zero-cost when not triggered.

**Tech Stack:** C++17, Dawn/WebGPU RHI, macOS CGEvent API for keyboard input, primal `RenderSceneSnapshot` for instance bounds.

---

## File Structure

| File | Responsibility | Changes |
|------|---------------|---------|
| `Engine/Graphics/Nanite/GPUCullingPipeline.h` | Public API of GPU culling pipeline | Add `SetForcePassAll`/`IsForcePassAll` + private bool member |
| `Engine/Graphics/Nanite/GPUCullingPipeline.cpp` | Per-frame `Execute` fills `CullingConstants` | Wire debug bool into `constants.force_pass_all` (replaces hardcoded `0`) |
| `EngineTest/IntegrationTests/TestDawnForwardRenderer.h` | Test harness header | Add `prevPState_`, `prevCtrlPState_` members |
| `EngineTest/IntegrationTests/TestDawnForwardRenderer.cpp` | Test harness implementation | Add A1/A2/A3 hooks in macOS branch of `UpdateCamera` + `RenderMeshletFrame` |

All other files unchanged.

---

## Task 1: Add `force_pass_all` debug API to `GPUCullingPipeline`

**Files:**
- Modify: `Engine/Graphics/Nanite/GPUCullingPipeline.h` (class body around line 84–141 for public API, line 142+ for private members)

- [ ] **Step 1: Add public setter/getter methods**

Open `Engine/Graphics/Nanite/GPUCullingPipeline.h`. Locate the public `Set*` methods (around lines 112–113):

```cpp
void SetConfig(const CullingConfig& config) { config_ = config; }
void SetLODBias(float bias) { config_.lod_bias = bias; }
```

Insert immediately after `SetLODBias`:

```cpp

// Debug bypass for the gray-white bug investigation. When true, the next
// Execute() call will set CullingConstants.force_pass_all = 1, which stage4
// of GPUCullingPipeline.wgsl consumes to skip the instance-visibility check
// and the far-plane cluster cull. Effect: all clusters from all instances
// survive the culling pipeline and reach DrawIndirect. Use to bisect whether
// the gray-white bug originates in stage4's visibility/far-plane logic or
// downstream (stage5 occlusion, stage6/7 compact+indirect, or post-cull).
void SetForcePassAll(bool enabled) { force_pass_all_debug_ = enabled; }
bool IsForcePassAll() const { return force_pass_all_debug_; }
```

- [ ] **Step 2: Add private member**

In the same file, scroll to the private members block (around line 205):

```cpp
u32 matrix_print_count_{ 0 };
```

Insert immediately after:

```cpp

// Reflects CullingConstants.force_pass_all for the diagnostic toggle.
// Default false so production behavior is unchanged.
bool force_pass_all_debug_{false};
```

- [ ] **Step 3: Verify header compiles standalone**

Run:
```bash
cmake --build build_dawn --target Engine -- -j 8 2>&1 | tail -20
```
Expected: BUILD SUCCEEDED. No errors in `GPUCullingPipeline.h`.

- [ ] **Step 4: Commit**

```bash
git add Engine/Graphics/Nanite/GPUCullingPipeline.h
git commit -m "feat(nanite): expose SetForcePassAll/IsForcePassAll on GPUCullingPipeline

Adds a public debug toggle for the gray-white investigation. Default
false preserves production behavior. Wired into CullingConstants in the
next commit."
```

---

## Task 2: Wire `force_pass_all_debug_` into `CullingConstants`

**Files:**
- Modify: `Engine/Graphics/Nanite/GPUCullingPipeline.cpp:1575`

- [ ] **Step 1: Replace hardcoded zero with debug flag**

Open `Engine/Graphics/Nanite/GPUCullingPipeline.cpp`. Locate line 1575:

```cpp
    constants.force_pass_all = 0;
```

Replace with:

```cpp
    constants.force_pass_all = force_pass_all_debug_ ? 1u : 0u;
```

No other change in this function — the rest of `CullingConstants` is filled with real per-frame values.

- [ ] **Step 2: Build the Engine target**

Run:
```bash
cmake --build build_dawn --target Engine -- -j 8 2>&1 | tail -20
```
Expected: BUILD SUCCEEDED.

- [ ] **Step 3: Commit**

```bash
git add Engine/Graphics/Nanite/GPUCullingPipeline.cpp
git commit -m "feat(nanite): wire SetForcePassAll into CullingConstants.force_pass_all

Replaces the hardcoded zero with the debug flag so the test harness
can flip stage4 behavior via Ctrl+P."
```

---

## Task 3: Add `prevPState_` / `prevCtrlPState_` members to test harness

**Files:**
- Modify: `EngineTest/IntegrationTests/TestDawnForwardRenderer.h:160-170` (member declarations near `prevTabState_`)

- [ ] **Step 1: Add edge-detect state members**

Open `EngineTest/IntegrationTests/TestDawnForwardRenderer.h`. Locate the block around line 162:

```cpp
bool prevTabState_{false};
bool prevVState_{false};
```

Insert immediately after:

```cpp
// Edge-detect state for diagnostic key hooks (P, Ctrl+P) used to
// investigate the long-standing gray-white bug in meshlet modes.
bool prevPState_{false};
bool prevCtrlPState_{false};
```

- [ ] **Step 2: Verify header parses**

This is a header-only change. Syntax is validated by Task 4's build of `TestDawnForwardRenderer`. No standalone build step here — proceed to Task 4 to confirm.

- [ ] **Step 3: Commit**

```bash
git add EngineTest/IntegrationTests/TestDawnForwardRenderer.h
git commit -m "feat(test): add edge-detect state members for P/Ctrl+P diagnostic hooks"
```

---

## Task 4: Implement A1 (P-key camera snapshot) in `UpdateCamera`

**Files:**
- Modify: `EngineTest/IntegrationTests/TestDawnForwardRenderer.cpp:1145-1180` (macOS branch of `UpdateCamera`)

- [ ] **Step 1: Locate the macOS branch of `UpdateCamera`**

Open `EngineTest/IntegrationTests/TestDawnForwardRenderer.cpp`. Find line 1145:

```cpp
void Engine_Test::UpdateCamera(float dt) {
#ifdef __APPLE__
    float speed = cameraSpeed_ * dt;

    auto keyPressed = [](uint16_t keyCode) -> bool {
        return CGEventSourceKeyState(kCGEventSourceStateHIDSystemState, keyCode);
    };
```

The `keyPressed` lambda here uses macOS virtual key codes. P = 35.

- [ ] **Step 2: Add A1 hook after the existing movement block**

Locate the end of the WASD/arrow-key block (around line 1177, just before the ESC handler). Insert:

```cpp

    // ---- Diagnostic A1: P key → camera snapshot to stderr ----
    // Used to capture exact camera state at a gray-white repro position.
    // macOS virtual key code 35 = ANSI 'P'.
    {
        bool pState = keyPressed(35);
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
    }
```

Note: this block sits **inside the `#ifdef __APPLE__` section**. WASM branch is deferred (see Task 7).

- [ ] **Step 3: Add `<cstdio>` include if missing**

Search the includes at the top of the file (first 80 lines). If `<cstdio>` is not present, add it after `<iostream>` (or wherever the standard includes are):

```cpp
#include <cstdio>
```

Run `grep -n "#include <cstdio>" EngineTest/IntegrationTests/TestDawnForwardRenderer.cpp` to confirm. If present, skip this step.

- [ ] **Step 4: Build the test target**

Run:
```bash
cmake --build build_dawn --target TestDawnForwardRenderer -- -j 8 2>&1 | tail -30
```
Expected: BUILD SUCCEEDED. (First build may take a while if Engine target was stale.)

- [ ] **Step 5: Smoke test A1**

Launch the test in any meshlet mode (default `ShadowAndIBL`, press Tab 5 times to reach `MeshletNoIBL`, or use `DAWN_FORCE_MODE=7`):

```bash
./Darwin/Debug/TestDawnForwardRenderer
```

Once the window opens, press `P` once. Check stderr:
```
[A1] camera snapshot: pos=(...) yaw=... pitch=... frame=N mode=M
```
Move the camera with WASD and press `P` again — values should change.

If no output appears, the key code is wrong. Verify with a key logger or check the key code list: macOS ANSI 'P' = 35 (hex 0x23).

- [ ] **Step 6: Commit**

```bash
git add EngineTest/IntegrationTests/TestDawnForwardRenderer.cpp
git commit -m "feat(test): A1 hook — P key dumps camera state to stderr

Used to capture exact camera position/yaw/pitch at a gray-white repro
location. macOS-only; WASM variant deferred."
```

---

## Task 5: Implement A2 (Ctrl+P force_pass_all toggle) in `UpdateCamera`

**Files:**
- Modify: `EngineTest/IntegrationTests/TestDawnForwardRenderer.cpp:1145-1200` (macOS branch of `UpdateCamera`, immediately after A1)

- [ ] **Step 1: Add A2 hook immediately after the A1 block**

In `EngineTest/IntegrationTests/TestDawnForwardRenderer.cpp`, locate the A1 block just added (search for `[A1]`). Immediately after the closing brace of the A1 block, insert:

```cpp

    // ---- Diagnostic A2: Ctrl+P → toggle force_pass_all (stage4 bypass) ----
    // Bisect switch for the gray-white bug. When force_pass_all=1, stage4 of
    // GPUCullingPipeline.wgsl skips the instance-visibility check and the
    // far-plane cluster cull, so all clusters survive to DrawIndirect. If the
    // gray-white clears when this is on, the bug is in stage4 or upstream
    // (stage1 instance data). If it persists, the bug is in stage5/6/7 or
    // post-cull.
    {
        bool ctrlDown = CGEventSourceFlagsState(kCGEventSourceStateHIDSystemState,
                                                kCGEventFlagMaskControl);
        bool pState = keyPressed(35);  // ANSI 'P'
        bool ctrlP = ctrlDown && pState;
        if (ctrlP && !prevCtrlPState_) {
            auto* cullPipeline = primal::graphics::nanite::GPUCullingPipeline::Get();
            bool newVal = !cullPipeline->IsForcePassAll();
            cullPipeline->SetForcePassAll(newVal);
            std::fprintf(stderr, "[A2] force_pass_all = %s\n",
                         newVal ? "TRUE (stage4 bypass ON)"
                                : "FALSE (stage4 bypass OFF)");
            std::fflush(stderr);
        }
        prevCtrlPState_ = ctrlP;
    }
```

- [ ] **Step 2: Build**

Run:
```bash
cmake --build build_dawn --target TestDawnForwardRenderer -- -j 8 2>&1 | tail -30
```
Expected: BUILD SUCCEEDED.

- [ ] **Step 3: Smoke test A2**

Launch the test in Mode 7:
```bash
DAWN_FORCE_MODE=7 ./Darwin/Debug/TestDawnForwardRenderer
```

Press `Ctrl+P` once. Verify stderr shows:
```
[A2] force_pass_all = TRUE (stage4 bypass ON)
```

The scene should look essentially identical (Mode 7 normally has culling on, but Sponza's instance count is low so visual difference may be minimal). Press `Ctrl+P` again:
```
[A2] force_pass_all = FALSE (stage4 bypass OFF)
```

If the toggle does not appear, the most likely cause is `CGEventSourceFlagsState` not being declared — verify `ApplicationServices.h` / `CoreGraphics/CGEventSource.h` is included transitively (it is via the existing `CGEventSourceKeyState` calls, so this should "just work").

- [ ] **Step 4: Commit**

```bash
git add EngineTest/IntegrationTests/TestDawnForwardRenderer.cpp
git commit -m "feat(test): A2 hook — Ctrl+P toggles force_pass_all bypass

Bisect switch for the gray-white investigation. Wired through
GPUCullingPipeline::SetForcePassAll into CullingConstants.force_pass_all,
which stage4 of GPUCullingPipeline.wgsl consumes."
```

---

## Task 6: Implement A3 (bounds NaN auto-scan) in `RenderMeshletFrame`

**Files:**
- Modify: `EngineTest/IntegrationTests/TestDawnForwardRenderer.cpp` — inside `RenderMeshletFrame`, at function entry

- [ ] **Step 1: Locate `RenderMeshletFrame`**

Search for the function definition:
```bash
grep -n "void Engine_Test::RenderMeshletFrame" EngineTest/IntegrationTests/TestDawnForwardRenderer.cpp
```
Expected output: a single line, e.g. `2770: void Engine_Test::RenderMeshletFrame(primal::graphics::rhi::RHICommandBuffer* cmd) {`

- [ ] **Step 2: Add `<cmath>` include if missing**

Run:
```bash
grep -n "#include <cmath>" EngineTest/IntegrationTests/TestDawnForwardRenderer.cpp
```
If empty, add `#include <cmath>` near the other standard includes at the top of the file. `std::isnan` / `std::isinf` come from `<cmath>`.

- [ ] **Step 3: Add A3 scan at function entry**

Open the file at the `RenderMeshletFrame` location from Step 1. Immediately after the opening brace, insert:

```cpp

    // ---- Diagnostic A3: scan instance bounds for NaN/Inf ----
    // If any instance has corrupt bounds, stage1 near/far tests pass (NaN
    // comparisons are false) but stage4 cluster expansion may still produce
    // garbage. Rate-limited to one warning per ~60 frames so a persistent
    // NaN doesn't flood stderr.
    static u32 lastBoundsWarningFrame = 0xFFFFFFFFu;
    if (totalFrames_ - lastBoundsWarningFrame > 60u) {
        const auto& instances = meshletSceneSnapshot_.GetInstanceData();
        bool anyBad = false;
        for (u32 i = 0; i < meshletSceneSnapshot_.GetInstanceCount(); ++i) {
            const auto& b = instances[i];
            const bool centerBad =
                std::isnan(b.bounds_center.x) || std::isinf(b.bounds_center.x) ||
                std::isnan(b.bounds_center.y) || std::isinf(b.bounds_center.y) ||
                std::isnan(b.bounds_center.z) || std::isinf(b.bounds_center.z);
            const bool radiusBad =
                std::isnan(b.bounds_radius) || std::isinf(b.bounds_radius);
            if (centerBad || radiusBad) {
                std::fprintf(stderr,
                    "[A3] NaN/Inf in instance[%u] bounds: "
                    "center=(%f, %f, %f) radius=%f frame=%u\n",
                    i,
                    b.bounds_center.x, b.bounds_center.y, b.bounds_center.z,
                    b.bounds_radius, totalFrames_);
                anyBad = true;
            }
        }
        if (anyBad) {
            lastBoundsWarningFrame = totalFrames_;
            std::fflush(stderr);
        }
    }
```

- [ ] **Step 4: Verify `InstanceData` has the expected field names**

Run:
```bash
grep -n "bounds_center\|bounds_radius" Engine/Graphics/Scene/RenderSceneSnapshot.h
```
Expected output (already verified, but re-check in case of edits):
```
math::v3 bounds_center;               // 12 bytes - offsets 160-171 (16-byte aligned!)
f32 bounds_radius;                    // 4 bytes - offset 172
```

`math::v3` is the primal 3-float vector; `.x`/`.y`/`.z` are the accessor names.

- [ ] **Step 5: Build**

Run:
```bash
cmake --build build_dawn --target TestDawnForwardRenderer -- -j 8 2>&1 | tail -30
```
Expected: BUILD SUCCEEDED.

- [ ] **Step 6: Smoke test A3**

Launch Mode 7:
```bash
DAWN_FORCE_MODE=7 ./Darwin/Debug/TestDawnForwardRenderer
```

Fly around for ~30 seconds. Expected: **no `[A3]` lines in stderr** (clean state). If `[A3]` lines appear, that's the smoking gun for H1 (bounds corruption) and a follow-up spec is needed.

- [ ] **Step 7: Commit**

```bash
git add EngineTest/IntegrationTests/TestDawnForwardRenderer.cpp
git commit -m "feat(test): A3 hook — auto-scan instance bounds for NaN/Inf

Rate-limited warning (1/sec @ 60fps) on corrupt bounds, which would
otherwise pass stage1 NaN-comparison silently but break downstream
stages. < 50 instances currently, so cost is negligible."
```

---

## Task 7: Full smoke test + diagnostic guide

**Files:** None modified — verification only.

- [ ] **Step 1: Clean rebuild**

Run:
```bash
cmake --build build_dawn --target TestDawnForwardRenderer -- -j 8 2>&1 | tail -10
```
Expected: BUILD SUCCEEDED with no warnings related to the new code.

- [ ] **Step 2: Run Mode 7 with all three hooks active**

Launch:
```bash
DAWN_FORCE_MODE=7 ./Darwin/Debug/TestDawnForwardRenderer 2>&1 | tee /tmp/diagnostic-v1.log
```

Then:
1. Press `P` a few times while moving the camera. Verify `[A1]` lines appear with changing coordinates.
2. Press `Ctrl+P` twice. Verify `[A2]` lines toggle between TRUE and FALSE.
3. Let it run for ~30 seconds. Verify no `[A3]` lines.

If all three behaviors check out, the diagnostic build is ready.

- [ ] **Step 3: Tag the build**

```bash
git tag diagnostic-v1
```

- [ ] **Step 4: Hand off to user with repro instructions**

Send this to the user:

> **Diagnostic build ready.** Launch with `DAWN_FORCE_MODE=7 ./Darwin/Debug/TestDawnForwardRenderer` (or any meshlet mode 7-11). When you hit the gray-white screen:
>
> 1. **First, press `P`** at the exact trigger position. Capture the `[A1]` line from stderr.
> 2. **Then press `Ctrl+P`** to flip on `force_pass_all`. Observe whether the gray-white clears.
>    - Cleared → bug is in stage4 visibility check or stage1 instance data → check if `[A3]` also fired
>    - Not cleared → bug is in stage5 occlusion, stage6/7 compact/indirect, or post-cull (DrawIndirect args, GBuffer clear)
> 3. **Press `Ctrl+P` again** to flip back. Capture a second `[A1]` line so we have before/after coordinates.
> 4. Share the log. Next spec targets the actual fix.

- [ ] **Step 5: Final commit (diagnostic guide doc)**

Create `Docs/2026-07-09-dawn-meshlet-graywhite-diagnostic-guide.md` with the same instructions as Step 4:

```markdown
# Dawn Meshlet Gray-White Diagnostic Guide (diagnostic-v1)

## Setup
- Build: `cmake --build build_dawn --target TestDawnForwardRenderer`
- Tag: `diagnostic-v1`
- Modes affected: 7/8/9/10/11

## Hooks
- **P** — dumps `pos/yaw/pitch/frame/mode` to stderr (prefix `[A1]`)
- **Ctrl+P** — toggles `force_pass_all` (prefix `[A2]`)
- **Auto** — warns on NaN/Inf in instance bounds (prefix `[A3]`, rate-limited 1/sec)

## Repro procedure
1. Launch `DAWN_FORCE_MODE=7 ./Darwin/Debug/TestDawnForwardRenderer` (or modes 8/9/10/11).
2. Fly to a known gray-white region.
3. Press `P` → capture `[A1]` line.
4. Press `Ctrl+P` → note whether screen clears.
5. Press `Ctrl+P` again → toggle back.
6. Share log.

## What each outcome means
- `[A3]` ever fires → H1 (bounds corruption). Next spec: fix `RenderSceneSnapshot::ComputeLocalBounds`.
- `Ctrl+P` clears gray-white → bug in stage4 visibility/far-plane test or stage1 instance data.
- `Ctrl+P` does NOT clear → bug in stage5 occlusion, stage6/7, or post-cull (DrawIndirect args, GBuffer clear).
```

Commit:
```bash
git add Docs/2026-07-09-dawn-meshlet-graywhite-diagnostic-guide.md
git commit -m "docs: diagnostic-v1 user guide for gray-white investigation

Step-by-step repro procedure and outcome interpretation table for the
three hooks (A1/A2/A3) added in this branch."
```

---

## Out of Scope

- **Actual fix** for the gray-white bug — this plan is diagnostic-only. Once repro data is collected, write a follow-up spec targeting the specific failure mode (H1–H6 from the design doc).
- **WASM variants of A1/A2** — bug repro is on macOS native. Porting to Emscripten (`EmscriptenGetKeyState(80)` for P, plus a JS-injected Ctrl flag) is a follow-up if the same bug appears in WASM.
- **Per-stage visibility counters (A4)** — defer until A2 confirms culling is the culprit. If `Ctrl+P` does NOT clear the gray-white, A4 won't help anyway; the bug is post-cull.
- **Persistent logging to disk** — stderr is sufficient for native dev. If logs get noisy, redirect `2> /tmp/diagnostic.log`.

## Risks

- **Wrong key code for P**: macOS ANSI 'P' = 35. If wrong, smoke test in Task 4 will fail to print `[A1]`; fix and re-test.
- **`CGEventSourceFlagsState` undeclared**: extremely unlikely (same framework as `CGEventSourceKeyState`), but if it happens, add `#include <ApplicationServices/ApplicationServices.h>` at the top of the `.cpp`.
- **A3 false-positive**: if `RenderSceneSnapshot` legitimately has instances with bounds radius 0 (planes, degenerate meshes), `isnan`/`isinf` won't fire — zeros are valid. Only NaN/Inf are flagged.
- **`GetInstanceData()[i]` invalidation**: returned by const reference to a `utl::vector`. The vector is not modified during the scan (no `Rebind`/`PartialUpdate` in `RenderMeshletFrame`), so iterators stay valid. Verified by reading `RenderSceneSnapshot::Rebind` — it does not run during the per-frame render path.
