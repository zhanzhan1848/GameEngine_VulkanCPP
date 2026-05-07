# Surface Cache Integration Design

**Date**: 2026-05-07
**Status**: Approved
**Scope**: Phase A (Surface Cache Lite) → Phase B (Full Voxel Radiance)

## Motivation

Current GI system has three unresolved pain points:
1. DDGI probe leaking in complex geometry
2. SSGI limited to screen-space information
3. Poor multi-bounce GI quality

Surface Cache provides per-surface texel lighting cache that eliminates probe leaking, works in world space, and naturally supports multi-bounce propagation.

## Integration Strategy

Progressive replacement: Surface Cache activates at `Quality::High` and above. DDGI retained for `Quality::Medium` and below.

## Section 1: Data Structures

### Card (SurfaceCacheCard)

Per-mesh axis-aligned orthogonal projections capturing surface material data.

```cpp
struct SurfaceCacheCard {
    float3 center;        // Card center in mesh local space
    float3 extent;        // Half-extents (clipped from mesh AABB)
    uint8_t axis;         // Projection axis (0=X, 1=Y, 2=Z)
    uint8_t direction;    // +1 or -1
    uint16_t resolution;  // Texel resolution in atlas
    uint2 atlasOffset;    // Starting position in atlas
};
```

Runtime generation: 6 axis-aligned cards per mesh, skip directions with projection area below threshold.

### Atlas Layout

- Size: **2048x2048** (configurable, can drop to 1024/512; matches `LumenConfig::surface_cache_atlas_size`)
- Page size: **32x32** (4096 pages total; matches `LumenConfig::surface_cache_page_size`)
- Max cards: **4096** (matches `LumenConfig::surface_cache_max_cards`)
- All atlas dimensions are `static constexpr` configurable values, sourced from `LumenConfig`

### Per-Texel Channels

| Channel | Format | Contents |
|---------|--------|----------|
| Albedo | RGBA8 | Base color |
| Normal | RG16F | World-space normal (octant encoded) |
| Depth | R32F | Distance from card projection plane |
| Emissive | RGB11F | Emissive light |
| Lighting | RGBA16F | Direct + indirect lighting result |

### Main Class

```cpp
LumenSurfaceCachePass {
    // Atlas textures
    albedo_atlas_    (2048x2048 RGBA8)
    normal_atlas_    (2048x2048 RG16F)
    depth_atlas_     (2048x2048 R32F)
    emissive_atlas_  (2048x2048 RGB11F)
    lighting_atlas_  (2048x2048 RGBA16F)

    // Double-buffered lighting for temporal
    prev_lighting_atlas_ (2048x2048 RGBA16F)

    // Card registry
    card_data_buffer_  (StructuredBuffer<SurfaceCacheCard>)

    // Page allocation
    page_free_list_    (Available page index queue)
    page_lru_          (LRU priority sorting)
}
```

Memory budget: ~100MB total (albedo 16M + normal 16M + depth 16M + emissive 24M + lighting 32M + prev_lighting 32M = ~136M). Acceptable on Apple Silicon unified memory.

## Section 2: Card Capture Pipeline

### Per-Frame Flow

1. **UpdatePriorities** (compute, lightweight) — mark cards needing update
2. **SelectCards** — pick highest-priority cards until budget exhausted
3. **CardCapturePass** x N directions (fragment) — render meshes to atlas
4. **DepthDilate** (compute) — dilate depth to prevent transparent texel artifacts

### CardCapturePass (Fragment)

One render pass per card direction. Orthographic projection.

**Vertex shader**: Transform Nanite meshlet vertices (`global_vertex_buffer_`, `global_element_buffer_`, `global_instance_data_buffer_`) to card orthographic projection space.

**Fragment shader**: Sample material textures (albedo/normal/ORM texture arrays). Material sampling **must** be encapsulated in a separate function to avoid Apple Silicon inline texture sampling bug. Output to 4 render targets: albedo, normal (octant encoded), depth, emissive.

Reuses existing `GPUDrivenDrawPipeline` meshlet data and `GPUMaterialRegistry` material texture arrays.

### Priority System

```
Priority = (frameCount - lastUpdatedFrame) * screenSpaceArea
```

Newly visible cards get highest priority. Large screen projection area = higher priority.

### Budget

```cpp
static constexpr uint32_t kCaptureBudgetTexels = 512 * 512;  // configurable
```

## Section 3: Direct Lighting on Surface Cache

### Two-Pass Design (controls per-pass read count)

**Pass 1: LightCull** (compute)
- Divide atlas into 8x8 tiles
- Read depth range (min/max) and average normal per tile
- Cone-test cull lights per tile, max 4 lights per tile (configurable)
- Output: `light_assignment_buffer` (uint4 per tile)

Reads: depth_atlas(1) + normal_atlas(1) + card_data_buffer(1) = **3 reads**

**Pass 2: LightEvaluate** (compute)
- Per-texel threads
- Read card_data to reconstruct world position from atlas UV + depth
- Read tile's light assignment, compute per-light contribution
- Lambertian diffuse with albedo
- Add emissive
- Shadow: reuse existing cascade shadow maps (Phase A), add per-texel SDF shadow later (Phase B)

Reads: card_data_buffer(1) + light_assignment_buffer(1) + albedo_atlas(1) + normal_atlas(1) + emissive_atlas(1) + shadow_map(1) = **6 reads**

> **Note**: 6 reads is within the 16-read stable limit but higher than the 4-5 target.
> If this causes issues, split further: pre-compute world positions in a separate pass
> into a `world_position_buffer`, then LightEvaluate reads that buffer instead of
> card_data + depth_atlas (reduces to 5 reads).

### Update Frequency

- Static lights: only update texels where card capture changed
- Dynamic lights: mark dirty tiles per light influence region

## Section 4: Indirect Lighting Propagation

### Two-Pass Design

**Pass 1: IndirectTrace** (compute)
- Place one probe per 8x8 tile (not per-texel)
- Trace 8 cosine-weighted hemisphere rays per probe
- Near hit (< 2m): record precise world position
- Far hit / miss: record GlobalSDF hit or sky marker

Reads: depth_atlas(1) + card_data_buffer(1) + normal_atlas(1) + GlobalSDF(1) = **4 reads**

**Pass 2: IndirectResolve** (compute)
- Read card_lookup + card_data to convert near-hit world positions to atlas UV
- Sample prev_lighting_atlas at near-hit positions
- Far hits: sample DDGI (Phase A) or Voxel Radiance (Phase B)
- Sky hits: sample sky cubemap
- Cosine-weighted average of 8 rays → indirect lighting
- Multiply by albedo
- Temporal filter: exponential blend with previous frame (weight 0.1-0.2)

Reads: hit_position_buffer(1) + card_lookup(1) + card_data_buffer(1) + prev_lighting_atlas(1) + albedo_atlas(1) + DDGI_or_sky(1) = **6 reads**

> **Note**: Same mitigation as LightEvaluate — if 6 reads causes issues, pre-compute
> atlas UVs in a separate pass into `atlas_uv_buffer`, reducing IndirectResolve to 5 reads.

### Multi-Bounce Convergence

Self-updating loop:
```
Frame N indirect = sample(Frame N-1 FinalLighting) at hit points
Frame N FinalLighting = DirectLighting + IndirectLighting
```

Converges in ~3-5 frames (controlled by temporal filter weight).

### Budget

```cpp
static constexpr uint32_t kIndirectBudgetTexels = 256 * 256;  // configurable
```

## Section 5: Screen Probe Integration

### Modified ScreenProbeTraceRays Flow

```
trace ray → GlobalSDF hit → distance check:
  near hit (< 2m):
    world position → CardLookup → atlas UV → sample lighting_atlas
  far hit (> 2m):
    sample DDGI (Phase A) or Voxel Radiance (Phase B)
  sky miss:
    sample sky_cubemap (unchanged)
```

### Card Lookup Structure

```cpp
struct CardLookupEntry {
    float3 aabb_min;        // Mesh world-space AABB
    float3 aabb_max;
    uint32_t card_start;    // Start index in card_data_buffer
    uint32_t card_count;    // Number of cards for this mesh
};
```

Linear scan over mesh entries (mesh count typically small). BVH optimization deferred unless bottleneck.

### Modified Files

- `ScreenProbeTraceRays.metal`: add near/far branch and surface cache sampling path
- `ScreenProbeGIPass.cpp/.h`: bind lighting_atlas and card_data_buffer

New reads in trace pass: card_lookup(1) + card_data(1) + lighting_atlas(1) = **+3 reads** (total ~5 with existing)

### Quality Presets

```
Medium:  SSGI + DDGI (unchanged)
High:    Screen Probes + Surface Cache (near) + DDGI (far)
Ultra:   Screen Probes + Surface Cache (near) + Voxel Radiance (far)  [Phase B]
```

## Section 6: Phase B Extension — Voxel Radiance Cache

### Voxel Clipmap

4 levels of 32^3 voxels each (smaller than UE5's 64^3 for Apple Silicon):

| Level | Voxel Size | Coverage | Update Frequency |
|-------|-----------|----------|-----------------|
| 0 | 0.5m | ~16m^3 | Every 2 frames |
| 1 | 1.0m | ~32m^3 | Every 4 frames |
| 2 | 2.0m | ~64m^3 | Every 8 frames |
| 3 | 4.0m | ~128m^3 | Every 8 frames |

Per-voxel: 6-face directional radiance (RGBA16F). Total memory: 4 x 32^3 x 6 x 8B = ~6MB.

### VoxelInjectPass (compute)

- Iterate clipmap voxels
- Per face: short ray cast against GlobalSDF for nearest surface direction
- Inject surface cache FinalLighting into corresponding voxel face

Reads: lighting_atlas(1) + card_data(1) + GlobalSDF(1) = **3 reads**

### Phase B Transition

- Remove DDGI passes (DDGITraceRays, UpdateIrradiance, UpdateDepth) from High/Ultra quality paths
- Screen Probe far-hit sampling switches from DDGI to Voxel Clipmap
- Indirect propagation far-hit sampling switches from DDGI to Voxel
- DDGI code retained for Medium quality backward compatibility

## Section 7: Complete Frame Pipeline

```
Per-frame execution order:

1.  Nanite GPU Culling + Visibility Buffer + GBuffer          [existing]
2.  Shadow Map rendering                                       [existing]
3.  GlobalSDF update                                           [existing]

--- Surface Cache ---
4.  UpdateCardPriorities (compute, lightweight)                [new]
5.  CardCapturePass x N directions (fragment, budget-gated)   [new]
6.  DepthDilate (compute)                                      [new]
7.  LightCull (compute)                                        [new]
8.  LightEvaluate (compute)                                    [new]
9.  IndirectTrace (compute, budget-gated)                      [new]
10. IndirectResolve (compute)                                  [new]

--- Phase B only ---
11. VoxelInjectPass (compute, per-level frequency)             [Phase B]

--- Existing Lumen ---
12. SSAO Trace + Filter                                        [existing, unchanged]
13. Screen Probe Place + Trace + Gather                        [existing, modified trace path]
14. Deferred Lighting (combine direct + GI + AO)               [existing, unchanged]
```

### Performance Estimate (1080p, Apple Silicon)

| Pass | Estimated Time |
|------|---------------|
| UpdatePriorities | ~0.1ms |
| CardCapture x6 | ~0.8ms |
| DepthDilate | ~0.1ms |
| LightCull + LightEvaluate | ~0.5ms |
| IndirectTrace + Resolve | ~0.6ms |
| **Total new** | **~2.1ms** |

Existing Lumen ~3ms + new ~2.1ms = **~5.1ms total GI cost**.

## Apple Silicon Hardware Constraints

Key mitigation strategy: **split large passes into multiple small passes**, each with <= 5-6 texture/buffer reads. Passes at 6 reads can be further split if needed (pre-compute intermediate buffers).

| Concern | Mitigation |
|---------|-----------|
| Buffer read limit (~16 stable, >32 flickers) | Every compute pass capped at 6 reads; 4-5 target, 6 max with pre-compute split option |
| texture3D ~4 read limit | Using 2D atlas textures, not texture3D |
| Inline texture sampling bug (fragment) | Card capture material sampling in separate function |
| Memory budget | Atlas size configurable (2048/1024/512), can downsize |

## Degradation Path

If performance or stability issues arise:
1. Reduce atlas size from 2048 → 1024 → 512
2. Reduce capture budget from 512^2 → 256^2
3. Reduce indirect budget from 256^2 → 128^2
4. Reduce card count from 6 → 4 → 2 directions per mesh
5. Reduce indirect trace rays from 8 → 4 per probe
6. Skip Phase B entirely, keep DDGI for far-field
