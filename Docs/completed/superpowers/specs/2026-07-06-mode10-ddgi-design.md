# Mode 10: Meshlet + SSGI + SSR + DDGI (Phase A — SDF + Static Bake)

**Date**: 2026-07-06
**Status**: Design — pending implementation plan
**Branch target**: `worktree-dawn-webgpu-backend`

## Goal

Add render Mode 10 to the Dawn backend, building on Mode 9 (`MeshletSSGISSR`) by injecting DDGI indirect lighting. Mode 10 = Mode 9 (meshlet GBuffer + SSGI + SSR, unchanged) + DDGI seeded from a CPU static bake and runtime-updated through GlobalSDF tracing.

This is **Phase A** of DDGI on Dawn. Phase B (SurfaceCache WGSL port, switching finalize to SC radiance lookup) is out of scope and tracked separately.

## Non-Goals

- SurfaceCache WGSL port (7 shaders: CardCapture, Dilate, LightCull, LightEval, IndirectTrace, IndirectResolve, plus DDGICardRadianceAvg / DDGIProbeIrradianceFromCards). Phase B.
- Forward+DDGI (Mode 11). The GIGatherModule port in Phase A makes this straightforward later, but Mode 11 itself is out of scope.
- Merging Trace + Finalize into a single dispatch. Apple Silicon workaround that's unnecessary on WASM, but kept split for code symmetry with Metal. Optimization for later.
- Async bake in background thread. Phase A uses synchronous main-thread bake with ImGui progress bar.

## Architecture

```
Mode 10 = Mode 9 (meshlet GBuffer + SSGI + SSR) + DDGI indirect injection

Startup (once, at Mode 10 entry):
  Load scene geometry → ProbeBakingScene (CPU vertices/indices/light params)
  StaticProbeBaker::Bake(volume, scene) → CPU SH9 + oct depth
  LumenDDGIPass::InitializeProbesFromStatic(volume) → GPU probe buffers
  StaticProbeVolume::SaveToFile("<scene>.spch") → cache for next launch

Per-frame (Mode 10 path):
  GPUCullingPipeline → GPUDrivenDrawPipeline → meshlet GBuffer (4 RT + depth)
  GlobalSDF::DispatchVoxelization → 3 cascade 3D textures
  LumenDDGIPass::AddPass(graph, prev_color, camera, frame_idx):
    [sub-pass 1] SDF Trace     — reads GlobalSDF cascades → hit_distance_buffer
    [sub-pass 2] Finalize       — hit_dist + sky radiance → ray_buffer
    [sub-pass 3] UpdateIrradiance — round-robin 256 probes/frame, SH9 EMA
    [sub-pass 4] UpdateDepth      — round-robin, oct 8x8 mean/var EMA
  GIGatherModule::AddPass(graph, ddgi_pass, gbuffer depth+normal):
    half-res compute: 8 nearest probes → SH9 eval → indirect texture (RGBA16F)
  RenderDawnMeshletDeferredLighting(+ binding 13 indirect texture):
    lit += albedo * indirect * ddgi_weight * ao
  HZB → TAA → SSR → SSGI → SSAO → ToneMap  (unchanged from Mode 9)
```

## Components

### A. Cross-worktree file migration (main repo → dawn worktree)

Three module groups exist in `feat/metal-sdk-upgrade` but not in `worktree-dawn-webgpu-backend`. Migrate via selective cherry-pick (see §"Cherry-pick strategy" below):

1. **StaticProbe module** — `Engine/Graphics/Lumen/StaticProbe/`
   - `StaticProbeVolume.{h,cpp}` — CPU probe storage, save/load (`'SPCH'` magic)
   - `StaticProbeBaker.{h,cpp}` — CPU BVH ray tracer, SH9 + oct depth output
   - `ProbeBakingScene` struct (in `StaticProbeBaker.h`)
   - Dependency: `utl::BVH` (verify path; may need to come along)

2. **GIGatherModule** — `Engine/Graphics/RenderPipeline/Modules/GIGatherModule.{h,cpp}`
   - Backend-agnostic C++. Caller injects shader handle via `Initialize(device, shader, w, h)`.
   - Reads `LumenDDGIPass*` + GBuffer depth/normal; writes half-res `gi_halfres_texture_` (RGBA16F).

3. **Probe confidence utility** (if referenced by StaticProbeBaker) — verify and bring along.

### B. New WGSL shaders (`Engine/Graphics/Dawn/shaders/Lumen/` — directory does not yet exist)

| Shader | Source (Metal) | Notes |
|---|---|---|
| `DDGITraceRays.wgsl` | `Metal/shaders/Lumen/DDGITraceRays.metal` | Contains both `ddgi_trace_sdf` (kernel 1) and `ddgi_trace_finalize` (kernel 2). Keep split — matches Metal, eases cross-backend diff. |
| `DDGIUpdateIrradiance.wgsl` | `Metal/shaders/Lumen/DDGIUpdateIrradiance.metal` | 8x8 cooperative SH9 projection + temporal EMA. |
| `DDGIUpdateDepth.wgsl` | `Metal/shaders/Lumen/DDGIUpdateDepth.metal` | Octahedral 8x8 depth mean/var. |
| `GIGather.wgsl` | (existing GIGatherModule Metal shader — locate via GIGatherModule.cpp) | Half-res probe SH9 sampling + tetrahedral interpolation. |

WGSL porting constraints (WASM + tint):
- Replace `isnan(x)` → `x != x`; `isinf(x)` → `abs(x) > 3.4e38`.
- Metal `float3` in struct = 16-byte align (v3 padded to v4). WGSL mirrors via `@align(16)` or explicit `_pad` fields. C++ side already uses `math::v3` (sizeof=16). Verify SH coefficient struct layout matches between C++ and WGSL.
- Texture3D reads — `GlobalSDF` WGSL already proves this works in Dawn/WASM. Reuse binding pattern.
- Threadgroup barriers — supported in WGSL `workgroupBarrier()`. Use as-is.

### C. Existing file modifications

| File | Change |
|---|---|
| `Engine/Graphics/Lumen/DDGI/LumenDDGIPass.cpp:74,134,138` | `LoadShaderBytecode` — replace hardcoded `.metal` lookup with backend dispatch. Pattern: WGSL-first when Dawn backend active, `.metal` fallback otherwise. Mirror `GlobalSDF.cpp:285,290`. |
| `Engine/Graphics/Dawn/shaders/DeferredLighting_Meshlet.wgsl` | Add `@group(0) @binding(13) var gi_indirect_tex: texture_2d<f32>;`. Reuse existing `iblSampler` (binding 9) for sampling. In lighting compute, add: `if (globalData.enableDDGI > 0u) { lit += albedo * textureSample(gi_indirect_tex, iblSampler, uv).rgb * ddgi_weight * ao; }` |
| `Engine/Graphics/ForwardRenderer.cpp:622-648` (meshlet deferred DSL) | Add `gi_indirect_tex` descriptor slot (binding 13). Add `enableDDGI` bool to `GlobalShaderData` struct (mirror existing `enableIBL`). When LumenDDGIPass is initialized, set `enableDDGI=true`; expose ImGui toggle for debug. |
| `EngineTest/IntegrationTests/TestDawnForwardRenderer.h:144` | Add `MeshletSSGISSRDDGI = 10` to `DawnRenderMode` enum. |
| `EngineTest/IntegrationTests/TestDawnForwardRenderer.cpp` | Add Mode 10 branch in dispatch (around `:1054`). Mode 10 owns `std::unique_ptr<LumenDDGIPass>`, `std::unique_ptr<GIGatherModule>`, `StaticProbeVolume`. At mode entry: bake (with ImGui progress), call `InitializeProbesFromStatic`. Per-frame: invoke `LumenDDGIPass::AddPass` + `GIGatherModule::AddPass` between meshlet GBuffer and deferred lighting, pass half-res indirect texture to `RenderDawnMeshletDeferredLighting`. |

## Cherry-pick strategy

Source commits on `feat/metal-sdk-upgrade`:
- `786e942` — 功能(lumen): 新增静态探针体积并优化表面缓存 (touches 23 files including LumenDDGIPass, ScreenProbes, SurfaceCache, Particle.h, etc.)
- `9bd3ccd` — feat(渲染管线): 新增模块化渲染管线框架 (touches 18+ files including all `RenderPipeline/Modules/*`, GPUDrivenDrawPipeline, RHIGarbageCollector, etc.)

Both commits are too broad to cherry-pick as-is. Procedure for each:

```bash
# In dawn worktree:
git cherry-pick -n -x <commit-sha>     # no commit, record source SHA in message
git restore --staged <unwanted-paths>  # unstage everything we don't want
git checkout -- <unwanted-paths>       # discard unwanted working tree changes
# Review staged set; manually resolve any CMakeLists.txt / LumenTypes.h conflicts
git commit                             # preserve cherry-pick -x trailer
```

Files to keep from `786e942`:
- `Engine/Graphics/Lumen/StaticProbe/*` (4 files)
- `Engine/utl/BVH.*` (if not already in worktree)
- `Engine/Graphics/Lumen/LumenTypes.h` additions (manual merge — also touched on dawn branch)

Files to keep from `9bd3ccd`:
- `Engine/Graphics/RenderPipeline/Modules/GIGatherModule.{h,cpp}` (2 files only)
- Defer the other 5 modules (DeferredLighting, FinalBlit, FusionComposite, ShadowMap, ForwardSceneRenderer) — out of scope for Phase A

If a selective cherry-pick produces conflicts in `LumenDDGIPass.cpp` (likely, since both commits touch it and dawn branch has its own evolution), prefer the dawn worktree version and manually layer in the `InitializeProbesFromStatic` + `LoadShaderBytecode`-friendly struct additions from `786e942`.

## Data flow

### Probe volume configuration

Use the same defaults as Metal Mode 6 (read from `LumenDDGIPass::Initialize` config). Volume follows camera (round-robin relocation already implemented in `UpdateProbeOrigin`). Initial probe grid: **16×8×16 = 2048 probes** (matches Metal test config). Implementation step prints scene AABB on Mode 10 entry and adjusts grid if test scene bounds clearly diverge.

### Buffer layout (WGSL must match)

- **Irradiance**: `totalProbes * 9 * sizeof(vec3<f32>)` — 9 SH3 coeff per probe, stored as float3 (12 bytes used, 16-byte aligned).
- **Depth**: `totalProbes * 128 * sizeof(f32)` — oct 8x8 mean (64) + variance (64).
- **Ray data**: `totalProbes * raysPerProbe * sizeof(vec4<f32>)` — `radiance.xyz` + `hitDist` (neg = miss).
- **Confidence**: `totalProbes * 4 * sizeof(f32)` — hitRatio, stability, _, age.
- **Update list**: triple-buffered, `max_probes_per_frame_ = 256`.

Triple-buffered (3 frames). C++ struct definitions in `LumenDDGIPass.h` already correct (verified against Metal). WGSL must mirror exactly — pay attention to `math::v3` sizeof=16 (see `simd-float3-sizeof-16-padding` memory).

### Static bake parameters

| Param | Value | Source |
|---|---|---|
| Probe grid | 16×8×16 = 2048 probes (default; adjustable if scene AABB diverges) | Metal test config |
| Rays per probe | 64 | StaticProbeBaker default |
| SH bands | 3 (9 coeff, L0+L1+L2) | StaticProbeBaker default |
| Bounces | 4 (MC direct + SH propagate) | StaticProbeBaker::Bake |
| Oct depth | 8x8 mean+var | StaticProbeBaker::BakeVisibility |

Save cache as `<scene-name>.spch` next to scene assets. On subsequent loads, check magic + version, re-bake if mismatch.

## Error handling

| Failure | Behavior |
|---|---|
| Bake fails (empty scene / BVH build error) | Skip `LumenDDGIPass::Initialize`, Mode 10 falls back to Mode 9 visual (no DDGI term), log warning |
| `LumenDDGIPass::Initialize` returns false | Same fallback as above |
| GlobalSDF not initialized at AddPass time | Already handled at `LumenDDGIPass.cpp:805` — TraceRays sub-pass skipped, irradiance stays at static bake values |
| WGSL shader compile fail | `LumenDDGIPass::Initialize` returns false → fallback path |
| Disk cache load fails (missing/magic mismatch/version drift) | Re-bake, overwrite file |
| WASM bake slow (>30s) | ImGui progress bar shows "Baking DDGI probes: X%". Black screen + text until done. Acceptable per design decision. |

## Testing

| Category | Check |
|---|---|
| Visual | Mode 10 vs Mode 9 side-by-side: shadowed areas (under floor, behind walls) show warm indirect from bake |
| Sanity | Mode 10 with empty bake renders pixel-identical to Mode 9 (DDGI term ≈ 0) |
| Sanity | Mode 10 toggle on/off mid-frame doesn't crash, no GPU state leak |
| WASM | Bake completes <60s on test scene, 60fps steady state (256 probes/frame update budget respected) |
| Layout | C++ struct sizes match WGSL (no `float3` alignment surprises). Add static_assert in LumenDDGIPass.h. |
| Performance | Native 1080p ≥60fps. If LumenDDGIPass + GIGather combined >4ms, lower `max_probes_per_frame_` first. |
| Cache | First run bakes + writes `.spch`. Second run loads from cache, no bake. Third run with corrupted cache re-bakes. |

## Open questions for implementation

- Test scene probe grid: 16×8×16 is the default. Implementation prints scene AABB on Mode 10 entry and adjusts if the scene is significantly larger/smaller than the Metal test scene.
- `prev_frame_color` for LumenDDGIPass trace feedback: Mode 9 has TAA history buffer. Reuse TAA's prev-frame color as `prev_frame_color` input. Verify temporal consistency.

## Phase B hooks (out of scope, listed for context)

When SurfaceCache is ported to WGSL:
1. Wire `LumenDDGIPass::SetSurfaceCacheResources(lighting_atlas, card_lookup, card_data)` in Mode 10 frame loop after SC AddPass.
2. Finalize sub-pass will then sample SC atlas instead of sky-only radiance — visible quality jump with no Mode 10 code changes.
3. SC indirection gives the runtime "use surface cache data" path the user requested as the long-term target.
