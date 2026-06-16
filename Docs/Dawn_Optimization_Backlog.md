# Dawn WebGPU Backend — Optimization Backlog

Long-term tracking document for known issues, technical debt, and future
optimization work that surfaced during the Dawn WebGPU backend migration.

Entries are intentionally left here when:
- The fix would require significant algorithmic rework that exceeds the
  current phase's scope.
- A workaround is in place that's acceptable for production but not ideal.
- Root cause is understood but the proper fix needs more iteration than the
  immediate milestone allows.

Each entry records enough context that a future implementer (possibly you,
possibly someone else) can pick it up cold without re-deriving the diagnosis.

---

## Render Quality

### SSR grazing-angle vertical stripes

**Status**: Open. Workaround in place (grazing-angle fade); artifact still
visible at moderate NdotV in some scenes.

**Symptom**: Regular vertical white stripes appear on side walls that run
parallel to the camera's view direction (e.g., walls with normals in ±X
when the camera looks along +Z). The stripes are periodic, coherent across
many pixels, and show up only when reflections are enabled (mode 4:
`ShadowAndIBLAndPuncLightAndSSR`).

**File**: `Engine/Graphics/Dawn/shaders/SSRTrace.wgsl`

**Initial diagnosis** (70/20/10 from initial bug report):
- 70%: Fixed view-space step size too large at grazing angles — ray Z
  changes slowly per step, the mip++/mip-- decision oscillates periodically
  across screen pixels, producing spatially coherent stripes.
- 20%: Hi-Z traversal bug — the mip adaptation logic might not match the
  HZB data structure (HZB uses MIN depth reduction, confirmed in
  `HZBIteration.wgsl:51`).
- 10%: Thickness / precision — the `thickness=2.0` tolerance might be too
  loose at grazing angles.

**Approaches tried**:

1. **MAX_STEPS 64 → 128** + `textureDimensions(hzbTex, mip)` + mip cap at 6.
   Outcome: more stripes (smaller view-space steps tightened the oscillation
   period).

2. **Screen-UV march with linear UV and NDC Z interpolation**.
   Outcome: reflections disappeared. Root cause: linearly interpolating NDC
   Z across screen UV is wrong for perspective projection (NDC Z is
   non-linear in screen UV), so hit tests never fired.

3. **Adaptive view-space step** (step = `texelUV / dUVdt`, where `dUVdt`
   estimated by projecting two nearby points).
   Outcome: reflections disappeared. Root cause: at grazing angles, the
   first fine-mip sample catches the source surface itself (the normal
   offset projects to a tiny UV shift), and the back-face check rejects
   every reflection. Adding an explicit source-UV skip (within 2 pixels of
   `pixelUV`) didn't recover reflections because the algorithm then gets
   stuck inching forward at fine mip on grazing rays and exhausts the
   MAX_STEPS budget before finding hits.

4. **Per-pixel phase dithering** (per-pixel hash seeded by `gid.xy`
   offsetting initial `t`).
   Outcome: stripes unchanged. Root cause: the periodicity comes from the
   deterministic mip-adaptation pattern, which is fixed per ray. Per-pixel
   phase shift on `t` doesn't decorrelate the mip++/mip-- decisions across
   pixels.

5. **Aggressive grazing fade** (`smoothstep(0.2, 0.5, NdotV)`).
   Outcome: reduces reflection intensity in low-NdotV regions but stripes
   still visible at moderate NdotV. Either stripes aren't exclusively at
   very low NdotV, or the underlying artifact source isn't fully covered
   by NdotV fade.

**Current state** (committed baseline):
- Fixed view-space step (known-good, produces reflections).
- Source-UV self-hit skip (within 2 pixels, minor benefit).
- Grazing fade `smoothstep(0.2, 0.5, NdotV)` (reduces but doesn't
  eliminate stripes).

**Future directions to investigate**:

- **Proper Hi-Z traversal with non-linear Z handling**: march in screen UV
  for xy, but compute ray Z per-step by projecting `rayOrigin + t * rayDir`
  (not by linear interp of endpoint NDC Z). Add a cap on view-space step
  (`min(adaptive, fixedStep)`) so head-on reflections where `dUVdt ≈ 0`
  don't blow up. This is the textbook fix; attempts #2 and #3 above got
  close but each missed a piece.

- **Distinguish stripe NdotV range empirically**: write a debug shader
  that outputs `vec4(NdotV, hitMask, tAtHit, mipAtHit)` and inspect the
  NdotV range where stripes actually appear. If they're at NdotV 0.4-0.7
  (not extreme grazing), the grazing fade approach is fundamentally
  insufficient and the algorithm itself needs to change.

- **Roughness-based fade**: if material roughness is bound to the SSR
  shader, fade reflection contribution by roughness. Real SSR should be
  barely visible at roughness > 0.5 anyway; this would mask the artifact
  on rough wall materials.

- **Reconstruct hit normal in different basis**: the current back-face
  rejection uses depth-reconstructed normals which are unreliable at
  silhouette edges. If a G-buffer normal is available, switching to that
  might clean up the back-face logic and reduce artifacts.

- **Blue-noise dithering instead of hash**: replace `hash21` with a
  precomputed blue-noise texture lookup. Blue noise is less visually
  objectionable than white noise and TAA averages it cleanly.

- **Investigate stripe source more carefully**: it's possible the stripes
  aren't purely from ray marching. They could be from the depth-aware
  color sample (`sampleColorDepthAware`) failing across periodic depth
  discontinuities, or from the half-res → full-res upsample in
  SSRComposite. A debug visualization of just the ray-march hit mask
  (before color sampling) would isolate the source.

---

## Performance

(no entries yet)

---

## Tooling / Build

(no entries yet)
