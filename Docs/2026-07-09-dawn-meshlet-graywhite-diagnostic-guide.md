# Dawn Meshlet Gray-White Diagnostic Guide (diagnostic-v1)

## Summary

Three diagnostic hooks have been added to the meshlet rendering path to investigate the long-standing "gray-white screen at specific camera positions" bug affecting modes 7/8/9/10/11.

- **Spec:** `Docs/superpowers/specs/2026-07-09-dawn-meshlet-camera-graywhite-instrumentation-design.md`
- **Plan:** `Docs/superpowers/plans/2026-07-09-dawn-meshlet-camera-graywhite-instrumentation-plan.md`
- **Tag:** `diagnostic-v1`

## Build

```bash
cmake --build build_dawn --target TestDawnForwardRenderer -- -j 8
```

## Hooks

All three hooks are active in native macOS builds only (`#ifdef __APPLE__`). WASM variants deferred.

| Hook | Trigger | Output | Effect |
|------|---------|--------|--------|
| **A1** | Press `P` | stderr `[A1] camera snapshot: pos=(...) yaw=... pitch=... frame=N mode=M` | None (read-only) |
| **A2** | Press `Ctrl+P` | stderr `[A2] force_pass_all = TRUE (stage4 bypass ON)` / `FALSE (stage4 bypass OFF)` | Toggles `GPUCullingPipeline` stage4 bypass on/off |
| **A3** | Auto (every frame, rate-limited 1/sec) | stderr `[A3] NaN/Inf in instance[N] bounds: ...` (only when corrupt) | None (read-only) |

## Repro Procedure

1. **Launch** the test in any meshlet mode:

   ```bash
   DAWN_FORCE_MODE=7 ./Darwin/Debug/TestDawnForwardRenderer
   # or modes 8/9/10/11 — all repro the bug
   ```

2. **Navigate** to a known gray-white region (specific camera position + angle triggers it).

3. **At the trigger position, press `P`** once. Capture the `[A1]` line from stderr. This records the exact camera state at repro.

4. **Press `Ctrl+P`** once. Observe:
   - stderr shows `[A2] force_pass_all = TRUE (stage4 bypass ON)`
   - **Does the gray-white clear?** Note the answer.

5. **Press `Ctrl+P`** again. Verify toggle flips back: `[A2] force_pass_all = FALSE (stage4 bypass OFF)`.

6. **Press `P`** once more to capture the after-state camera position.

7. **Share the log** (the `[A1]`/`[A2]` lines, plus any `[A3]` lines that may have fired).

## Outcome Interpretation

The combination of A2 + A3 outcomes narrows the bug class:

| A3 (NaN scan) | A2 (Ctrl+P) | Conclusion | Next step |
|---------------|-------------|------------|-----------|
| fires | any | Bounds data corruption (H1) | Fix `RenderSceneSnapshot::ComputeLocalBounds` — write follow-up spec |
| clean | clears gray-white | Bug in stage4 visibility/far-plane test (H2/H3) | Inspect stage4 in `GPUCullingPipeline.wgsl`; add per-stage counters (A4) if needed |
| clean | does NOT clear | Bug in stage5 occlusion / stage6 compact / stage7 indirect / post-cull | Add A4 hook with per-stage counters to localize |

In all cases, the `[A1]` lines provide the exact repro coordinates for the follow-up investigation.

## Hypotheses Reference

From the design spec, six root-cause hypotheses for `visible_counter = 0 → DrawIndirect(0 instances) → empty GBuffer → gray-white`:

| # | Hypothesis |
|---|-----------|
| H1 | Instance bounds contain NaN/Inf from `RenderSceneSnapshot::ComputeLocalBounds` |
| H2 | Stage5 occlusion culling false-positive (HZB too coarse at specific view angles) |
| H3 | Stage4 cluster expansion produces zero clusters (lod selection edge case) |
| H4 | `view_matrix` or `proj_matrix` becomes degenerate at specific yaw/pitch |
| H5 | Stage6 atomic race drops visibility counter |
| H6 | Per-geometry `bounds_radius` value corrupts stage1 sphere test |

The hooks are designed to falsify subsets of these in a single repro session.

## Known Limitations

1. **60-frame startup blind spot in A3.** The rate-limit initial value (`0xFFFFFFFFu`) means the bounds scan is skipped for the first ~60 frames due to unsigned wraparound. If bounds corruption exists only during init, it will be missed. Acceptable for now — bad bounds at frame 0 are almost always still bad at frame 60.

2. **A2 is stage4-bypass-only, not full no-cull.** `force_pass_all` is consumed only in stage4 of `GPUCullingPipeline.wgsl`. It bypasses:
   - Stage4's instance-visibility check (everything is treated as visible)
   - Stage4's per-cluster far-plane cull
   
   It does NOT bypass stage1's near/far cull (which is NaN-safe anyway — NaN comparisons return false, so corrupt-bounds instances pass through). It does NOT bypass stage5 occlusion culling.
   
   Practical effect: all clusters from all instances reach stage5. If stage5 is the culprit, A2 alone won't clear the symptom.

3. **WASM variants not implemented.** Bug repro is on macOS native; WASM port deferred to a follow-up if the same bug appears there.

4. **Working tree has uncommitted Mode 11 work.** Not related to this diagnostic plan, but worth noting: the committed hooks build cleanly against the current HEAD; the Mode 11 work sitting in the working tree is independent.
