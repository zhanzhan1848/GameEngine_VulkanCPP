# Phase 9.3b — GPU-Resident Streaming Terrain from GlobalSDF

**Status:** Design (awaiting implementation plan)
**Date:** 2026-06-22
**Branch:** `features/nanite_lumen`
**Prerequisite:** Phase 9.3a (GPU SurfaceNets on PCGField) — completed 2026-06-22 with postmortem

---

## 1. Context

Phase 9.3a shipped a working GPU SurfaceNets pipeline (`GPUMesher` + 4-pass compute + blocking readback) that matches the CPU reference output. Two bottlenecks remain:

1. **Pass 0 CPU sample loop** (~3ms @ 64³) — `PCGField::SampleFloat` runs on CPU to fill `scalar_volume`, which is then uploaded. This is the dominant cost when the field is `PCGReferenceField` over Nanite GlobalSDF (the common case).
2. **Blocking readback** (~1ms @ 64³) — full vertex/element/index buffers round-trip GPU→CPU so `content::register_mesh_asset` can register them as a CPU-side asset.

Both costs exist because Nanite has no GPU-resident mesh ingestion path. Phase 9.3b closes both gaps for the streaming-terrain use case (meshes that regenerate as GlobalSDF cascades reposition).

The broader goal remains use case 1 from the 9.3a spec ("real-time terrain"). 9.3a built the GPU meshing infrastructure; 9.3b consumes it in a real-time loop without the CPU round-trips.

---

## 2. Goals (Phase 9.3b scope)

**Functional:**
- New `GlobalSDFMeshNode` PCG node — no field input, binds GlobalSDF cascades directly
- New `GPUMesher::GenerateSurfaceNetsFromGlobalSDF` entry point — skips Pass 0 by sampling texture3d cascades in `classify_cells`
- New `StreamingMesh` entity type — persistent GPU buffers, per-frame metadata updates via `PipelineUpdateStreamingMeshEntity`
- Nanite `GPUDrivenDrawPipeline` draws StreamingMesh via `draw_primitives_indirect` (passthrough, no cluster build)

**Performance targets:**

| Resolution | 9.3a total | 9.3b target | Savings |
|---|---|---|---|
| 64³ | ~4.3ms | <3ms | Pass 0 eliminated (~3ms) + readback reduced to 8-byte counter copy (~0.1ms) |
| 128³ | ~32ms | <10ms | Same — savings scale with resolution |

**What 9.3b delivers:**
- Real-time (>30fps) terrain at 64³ — Pass 0 was the blocker
- Persistent GPU buffers — no per-frame allocation, no FreeList churn
- Streaming lifecycle — mesh follows camera as cascades reposition
- Foundation for 9.5 (adaptive LOD): cluster build can be layered on later if needed

**What 9.3b does NOT deliver (explicitly):**
- Cluster-level Nanite culling for streaming meshes (deferred to 9.5)
- Adaptive LOD / octree voxel structure (9.5)
- Dual Contouring sharp features (9.4)
- Multi-tile streaming (single GlobalSDFMeshNode covers one cascade-0-aligned volume)
- Classic Marching Cubes variant (dropped)

---

## 3. Out of scope (defer to future specs)

- **GPU-resident mesh for non-streaming use cases** (one-shot authoring still goes through `content::register_mesh_asset`)
- **Cluster build integration** — `GPUDrivenDrawPipeline` emits `draw_primitives_indirect` directly; no mesh-shader clusterization for streaming meshes
- **Material permutations beyond the default** — streaming mesh has one material slot; multi-material requires multiple nodes
- **Collision / physics integration** — GPU-resident mesh is render-only; CPU-side collision proxy is a separate concern
- **True GPU timer queries** (`MTLCounterSampleBuffer`) — still measured via wall-clock; 9.3c or later
- **`PCGField` GPU-backed subclass** (`PCGGlobalSDFField`) — considered and rejected; see §6.2

---

## 4. Architecture

### 4.1 Data flow (per frame)

```
GlobalSDF cascades (3 texture3d, camera-snapped)
    │
    ├─[Compute Pass 1] classify_cells_sdf (dispatch res³)
    │     ├─ reads sampleSDF(world_pos) from cascade textures
    │     ├─ writes dual_id[cell] = vertex_id  (atomic append on counters[0])
    │     └─ writes scalar[corner] = sampled SDF  (shared buffer for passes 2-3)
    │
    ├─[Compute Pass 2] emit_vertices_sdf (dispatch res³)
    │     ├─ reads scalar buffer (8 corners per cell, central-diff normal)
    │     └─ writes positions[vid] / elements[vid]
    │
    ├─[Compute Pass 3] emit_faces_{x,y,z} (3 dispatches, n³ each)
    │     ├─ reads scalar endpoints (unchanged from 9.3a)
    │     └─ writes indices / increments counters[1]
    │
    └─[Compute Pass 4] write_indirect_args (dispatch 1)
          └─ writes MTLDrawPrimitivesIndirectCommand from counters
          │
          ▼
StreamingMesh.gpu_buffers  (persists across frames — zero allocation)
          │
          ├─ 8-byte counter metadata copy (host) → PipelineUpdateStreamingMeshEntity
          │
          ▼
GPUDrivenDrawPipeline.Render:
   for each StreamingMeshRecord in render_scene:
       cmd->draw_primitives_indirect(triangle_list, indirect_args_buffer)
```

### 4.2 Why Pass 1 still writes the scalar buffer

Passes 2-3 in 9.3a read corner samples from a scalar array. Naively, the 9.3b shaders could re-sample cascade textures directly, but that creates:
- 8x redundant texture reads (each corner shared by 8 cells)
- Pass 2/3 shaders diverge from 9.3a versions → more code paths to maintain
- Cache thrashing — re-sampling means hitting the texture LSC 8x more

**Decision:** Pass 1 (`classify_cells_sdf`) writes the scalar buffer in addition to dual_id. Passes 2-3 are byte-identical to 9.3a. Memory cost is `(res+1)³ × 4` bytes (274KB @ 64³, 2.2MB @ 128³) — acceptable.

### 4.3 Why no PCGField subclass

Considered `PCGGlobalSDFField : PCGField` as a marker type for the GPU-backed path. Rejected because:
- `PCGField::SampleFloat(math::v3)` is a pure CPU virtual. A GPU-backed subclass would either fail CPU sampling (breaking any CPU-path consumer) or fall back to expensive readback (re-introducing the bottleneck 9.3b removes).
- GlobalSDF cascades are not a "field" in the PCG sense — they're GPU resources with per-frame repositioning. Treating them as a PCGField muddles the abstraction.
- `GlobalSDFMeshNode` with no field input is more honest about the data source.

Alternative: keep `MarchingCubesNode` and add `algorithm=3` for GlobalSDF direct. Rejected — mixes streaming (per-frame) with one-shot (per-execute) semantics in one node type.

---

## 5. Components

### 5.1 StreamingMesh (new struct)

**File:** `Engine/Graphics/RenderPipeline/StreamingMesh.h`

```cpp
namespace primal::graphics {

struct StreamingMesh {
    rhi::ResourceHandle positions{rhi::handles::INVALID_RESOURCE};    // f32 * 3 * max_verts
    rhi::ResourceHandle elements{rhi::handles::INVALID_RESOURCE};     // 20B * max_verts
    rhi::ResourceHandle indices{rhi::handles::INVALID_RESOURCE};      // u32 * max_indices
    rhi::ResourceHandle counters{rhi::handles::INVALID_RESOURCE};     // u32[2] {vert_count, idx_count}
    rhi::ResourceHandle indirect_args{rhi::handles::INVALID_RESOURCE};// MTLDrawPrimitivesIndirectCommand
    u32  max_verts{};
    u32  max_indices{};
    math::v3 bounds_min{}, bounds_max{};
    u64  generation{0};   // bumped each Execute; Nanite skips stale
    id::id_type entity_id{id::invalid_id};
};

// Allocates GPU buffers sized for worst-case (res+1)³ verts + 18×res³ indices.
// Returns empty StreamingMesh (all INVALID_RESOURCE) on failure.
StreamingMesh CreateStreamingMesh(
    rhi::RHIDeviceBase* device,
    u32 resolution,
    const math::v3& bounds_min,
    const math::v3& bounds_max);

void DestroyStreamingMesh(rhi::RHIDeviceBase* device, StreamingMesh& sm);

} // namespace primal::graphics
```

**Memory budget (worst case):**

| Resolution | positions | elements | indices | counters | indirect | Total |
|---|---|---|---|---|---|---|
| 64³ | 1.0MB | 2.1MB | 18.9MB | 8B | 16B | ~22MB |
| 128³ | 8.0MB | 16.8MB | 151.2MB | 8B | 16B | ~176MB |

Index buffer dominates at high res. Pass 3 worst case (6 tris × 3 idx = 18 idx per grid vertex) is rarely hit; typical is 30-50% of worst case. We allocate worst case for safety; a future optimization can compact via prefix-sum before draw.

### 5.2 GlobalSDFMeshNode (new PCGNode)

**File:** `Engine/Graphics/PCG/Nodes/GlobalSDFMeshNode.h`

```cpp
class GlobalSDFMeshNode : public PCGNode {
public:
    math::v3 bounds_min{-32.f, -32.f, -32.f};  // default = cascade 0 extent
    math::v3 bounds_max{ 32.f,  32.f,  32.f};
    u32      resolution{64};
    f32      iso_value{0.0f};

    GlobalSDFMeshNode();
    ~GlobalSDFMeshNode() override;  // unregister entity + destroy buffers

    const char* TypeName() const override { return "GlobalSDFMesh"; }
    void Execute() override;

    // ... reflection boilerplate matching MarchingCubesNode pattern
private:
    StreamingMesh streaming_mesh_{};
    bool          registered_{false};
};
```

**Execute() flow:**
1. If first execute: `streaming_mesh_ = CreateStreamingMesh(device, resolution, bounds_min, bounds_max)`. Register entity via `PipelineRegisterStreamingMeshEntity` (returns entity_id stored in `streaming_mesh_.entity_id`).
2. Each execute:
   - Zero `counters` buffer (via `UpdateBufferData` or a tiny compute clear).
   - Call `GPUMesher::Get().GenerateSurfaceNetsFromGlobalSDF(GlobalSDF::Get(), bounds_min, bounds_max, resolution, iso_value, streaming_mesh_)`.
   - 8-byte counter copy to host.
   - Call `PipelineUpdateStreamingMeshEntity(entity_id, counters, indirect_args)`.
   - Bump `streaming_mesh_.generation`.

**Output pin:** `PCGGeometryData{content_id = id::invalid_id}` as a sentinel — the node produces a renderable mesh but does not produce a content_id. Downstream nodes that need geometry content_id (e.g., `TransformGeometryNode`) cannot consume GlobalSDFMeshNode output; this is by design (streaming mesh is render-only).

### 5.3 GPUMesher extension

**File:** `Engine/Graphics/PCG/GPU/GPUMesher.h` (additions)

```cpp
class GPUMesher {
public:
    // ... existing 9.3a API unchanged ...

    // 9.3b: Direct GlobalSDF consumption, no CPU Pass 0.
    // Writes into caller-owned `target` buffers (persistent).
    // Returns true if dispatches submitted successfully.
    bool GenerateSurfaceNetsFromGlobalSDF(
        const primal::graphics::GlobalSDF& sdf,
        const math::v3& bounds_min,
        const math::v3& bounds_max,
        u32 resolution,
        f32 iso_value,
        StreamingMesh& target);

private:
    // Separate shader handles + pipelines for the SDF variant.
    // Reuses 9.3a descriptor set layouts (emit_vertices/faces/write_indirect
    // shaders are byte-identical; classify has SDF-specific variant).
    rhi::ShaderHandle classify_sdf_shader_{rhi::handles::INVALID_SHADER};
    rhi::PipelineHandle classify_sdf_pipeline_{rhi::handles::INVALID_PIPELINE};

    // SDF variant of classify descriptor set — adds 3 texture bindings.
    rhi::DescriptorSetLayoutHandle classify_sdf_set_layout_{rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    rhi::PipelineLayoutHandle      classify_sdf_layout_{rhi::handles::INVALID_PIPELINE_LAYOUT};

    bool sdf_pipelines_created_{false};
    void CreateSDFPipelines();
    void DestroySDFPipelines();
};
```

**Reuse strategy:**
- Same 4-pass structure
- Same descriptor set layouts for emit_vertices/emit_faces/write_indirect
- Only `classify` diverges (new shader + layout due to 3 extra texture bindings)

### 5.4 Nanite integration

**File:** `Engine/Graphics/Nanite/GPUDrivenDrawPipeline.cpp` (additions)

```cpp
// New traversal in Render(), before ScanMeshEntitiesAndBuildClusters:
void GPUDrivenDrawPipeline::DrawStreamingMeshes(RHICommandBuffer* cmd) {
    for (auto& sm : render_scene_->streaming_meshes_) {
        if (!sm.visible || sm.mesh == nullptr) continue;
        if (sm.tombstoned) continue;  // pending removal at frame boundary

        // Bind vertex buffers (positions, elements).
        cmd->BindVertexBuffer(0, sm.mesh->positions, 0);
        cmd->BindVertexBuffer(1, sm.mesh->elements, 0);
        cmd->BindIndexBuffer(sm.mesh->indices, 0, IndexFormat::U32);

        // v1: bind default material. Multi-material deferred (§10).
        BindDefaultMaterial();

        // Indirect draw — vertex count comes from indirect_args buffer,
        // written by Pass 4 of the GPU meshing.
        cmd->DrawIndexedIndirect(sm.mesh->indirect_args, 0, 1, 0);
    }
}
```

**RenderScene additions:**

```cpp
struct StreamingMeshRecord {
    id::id_type entity_id{id::invalid_id};
    StreamingMesh* mesh{nullptr};
    math::v3 bounds_min{}, bounds_max{};
    bool visible{true};
    bool tombstoned{false};      // marked for removal at frame end
    u64  last_drawn_generation{0};
};

utl::vector<StreamingMeshRecord> streaming_meshes_;
```

**Lifecycle invariants:**
- Register: append record, entity_id assigned.
- Update: bump `mesh->generation`, refresh bounds if changed.
- Unregister: set `tombstoned=true`. `GPUDrivenDrawPipeline` skips tombstoned records. Cleared at frame end (after GPU work completes).
- Destruction order: PCG node destructor → `PipelineUnregisterStreamingMeshEntity` → next frame end clears tombstone → safe to free GPU buffers.

### 5.5 C ABI

**File:** `EngineDLL/RenderPipelineAPI.cpp` (additions)

```c
// Returns entity_id (u64). 0 on failure.
u64 PipelineRegisterStreamingMeshEntity(
    u64 positions_handle,
    u64 elements_handle,
    u64 indices_handle,
    u64 indirect_args_handle,
    u32  max_verts,
    u32  max_indices,
    const f32* bounds_min,   // ptr to 3 floats
    const f32* bounds_max);  // ptr to 3 floats

// Updates per-frame metadata. Call after GPU meshing completes.
void PipelineUpdateStreamingMeshEntity(
    u64 entity_id,
    u64 generation,
    const u32* counters,        // ptr to 2 u32s (vert_count, idx_count)
    u64  indirect_args_handle); // may differ if buffer was reallocated

void PipelineUnregisterStreamingMeshEntity(u64 entity_id);
```

---

## 6. GPU shader details

### 6.1 Shader file layout

**New file:** `Engine/Graphics/Metal/shaders/PCG/SurfaceNetsGPUSDF.metal`

Mirrors `SurfaceNetsGPU.metal` structure. Contains:
- `classify_cells_sdf` — samples cascades, writes dual_id + scalar buffer
- `emit_vertices` — byte-identical to 9.3a (shared shader via `#include` or duplication; see §6.3)
- `emit_faces_{x,y,z}` — byte-identical to 9.3a
- `write_indirect_args` — byte-identical to 9.3a

### 6.2 Extended uniforms

```metal
struct SurfaceNetsSDFUniforms {
    // Base (offsets 0..60, identical to SurfaceNetsUniforms)
    uint resolution, n, n2;
    float voxel_x, voxel_y, voxel_z;
    float origin_x, origin_y, origin_z;
    float extent_x, extent_y, extent_z;
    float iso_value;

    // GlobalSDF cascades (offset 64+, mirrors DDGIVolumeData.metal:69-74)
    float3 SdfOrigins[3];
    float  SdfVoxelSizes[3];
    float  SdfExtents[3];
    uint   SdfResolutions[3];
};
```

### 6.3 Shader sharing strategy

Two options:
- **Option A (shared):** `SurfaceNetsGPU.metal` hosts the shared kernels (`emit_vertices`, `emit_faces_*`, `write_indirect_args`); `SurfaceNetsGPUSDF.metal` only defines `classify_cells_sdf` and `#include`s the rest.
- **Option B (duplicate):** Each file is self-contained; duplication is the cost of decoupling.

**Recommend Option A.** Single source of truth for the three shared kernels. Shader include model works in Metal via `#include` with proper search paths.

### 6.4 Sample function (cascade fallback)

```metal
// Returns true if point is in cascade AABB.
inline bool in_cascade(float3 p, float3 origin, float extent) {
    return all(p >= origin) && all(p < origin + float3(extent));
}

// Samples SDF at world_pos. Tries cascades from finest (0) to coarsest (2).
// Returns large positive value (solid) if outside all cascades.
inline float sample_global_sdf(
    thread const SurfaceNetsSDFUniforms& u,
    texture3d<float, access::sample> t0,
    texture3d<float, access::sample> t1,
    texture3d<float, access::sample> t2,
    float3 world_pos)
{
    constexpr sampler s(filter::linear, address::clamp_to_edge, coord::normalized);

    if (in_cascade(world_pos, u.SdfOrigins[0], u.SdfExtents[0])) {
        float3 uvw = (world_pos - u.SdfOrigins[0]) / u.SdfExtents[0];
        return t0.sample(s, uvw).r;
    }
    if (in_cascade(world_pos, u.SdfOrigins[1], u.SdfExtents[1])) {
        float3 uvw = (world_pos - u.SdfOrigins[1]) / u.SdfExtents[1];
        return t1.sample(s, uvw).r;
    }
    if (in_cascade(world_pos, u.SdfOrigins[2], u.SdfExtents[2])) {
        float3 uvw = (world_pos - u.SdfOrigins[2]) / u.SdfExtents[2];
        return t2.sample(s, uvw).r;
    }
    return 1e6f;  // outside all cascades → treated as solid
}
```

Mirrors `sampleBestSDF_elseIf` in `Lumen/SDFTraceCommon.metal:60-78` but inlineable.

### 6.5 classify_cells_sdf kernel

```metal
kernel void classify_cells_sdf(
    constant SurfaceNetsSDFUniforms& u  [[buffer(0)]],
    texture3d<float, access::sample> t0 [[texture(0)]],
    texture3d<float, access::sample> t1 [[texture(1)]],
    texture3d<float, access::sample> t2 [[texture(2)]],
    device float*                   scalar   [[buffer(1)]],   // (res+1)³
    device uint*                    dual_id  [[buffer(2)]],
    device atomic_uint*             vcounter [[buffer(3)]],
    uint3                           tid      [[thread_position_in_grid]])
{
    const uint res = u.resolution;
    if (any(tid >= uint3(res))) return;

    // Sample 8 corners, write to scalar buffer (shared with passes 2-3).
    float cv[8];
    uint mask = 0;
    for (uint c = 0; c < 8; ++c) {
        uint3 g = tid + uint3(kCornerOffset[c]);
        float3 world_pos = float3(
            u.origin_x + u.voxel_x * float(g.x),
            u.origin_y + u.voxel_y * float(g.y),
            u.origin_z + u.voxel_z * float(g.z));
        cv[c] = sample_global_sdf(u, t0, t1, t2, world_pos);

        // Persist to scalar buffer for passes 2-3 (indexed as 9.3a).
        uint idx = g.x + u.n * g.y + u.n2 * g.z;
        scalar[idx] = cv[c];
        if (cv[c] > u.iso_value) mask |= (1u << c);
    }

    uint cell_idx = tid.x + res * tid.y + res * res * tid.z;
    if (mask == 0u || mask == 0xFFu) {
        dual_id[cell_idx] = SN_INVALID_ID;
        return;
    }

    uint vid = atomic_fetch_add_explicit(vcounter, 1u, memory_order_relaxed);
    dual_id[cell_idx] = vid;
}
```

---

## 7. Error handling + lifecycle

### 7.1 Fallback matrix

| Condition | Behavior |
|---|---|
| No RHI device (test env) | `GlobalSDFMeshNode` emits invalid geometry; no crash |
| `GlobalSDF` not initialized | `GenerateSurfaceNetsFromGlobalSDF` returns false; node logs error |
| `GPUMesher` not initialized | Same |
| Cascade textures missing | Shader bounds-check returns "solid"; mesh empty in those regions |
| Buffer alloc failure | Node destroys partial allocs, emits invalid geometry, logs error |
| Node destroyed mid-frame | `~GlobalSDFMeshNode` → `PipelineUnregisterStreamingMeshEntity` → tombstone; cleared at frame end |

### 7.2 Use-after-free prevention

GPU buffers are owned by the PCG node but referenced by Nanite. To prevent UAF:

1. `~GlobalSDFMeshNode()` calls `PipelineUnregisterStreamingMeshEntity(entity_id)`.
2. `RenderScene::streaming_meshes_` record is marked `tombstoned=true` (not erased).
3. `GPUDrivenDrawPipeline::DrawStreamingMeshes` skips tombstoned records.
4. At frame end (after `cmd->Commit` and `WaitForCompletion`), tombstoned records are erased from the vector.
5. Only then does the node's destructor free GPU buffers.

**Critical:** step 4-5 ordering requires the PCG node destructor to either (a) run at a safe point (graph teardown between frames), or (b) defer buffer free until next frame boundary.

**Decision:** `~GlobalSDFMeshNode` pushes buffer handles onto a deferred-destruction queue owned by `GPUMesher`. The queue is drained at frame boundary (after `WaitForCompletion`). This decouples node lifetime from GPU-work lifetime and removes the constraint that node destructors only run between frames.

**Deferred-destruction queue shape (added to GPUMesher):**
```cpp
// In GPUMesher.h, public:
void EnqueueDeferredDestroy(rhi::ResourceHandle h);
void DrainDeferredDestroys();  // Called by StandardRenderPipeline at frame end.

// In GPUMesher.cpp:
utl::vector<rhi::ResourceHandle> deferred_destroy_queue_;
```

### 7.3 Buffer sizing

Allocated once at `CreateStreamingMesh`. Worst-case sizes:
- `positions`: `(res+1)³ × 12 bytes`
- `elements`: `(res+1)³ × 20 bytes`
- `indices`: `res³ × 18 × 4 bytes`
- `counters`: 8 bytes
- `indirect_args`: 16 bytes (MTLDrawPrimitivesIndirectCommand)

No compaction. Pass 3 worst case (6 tris/grid-vertex) is rare; typical fill is 30-50%.

---

## 8. Testing strategy

### 8.1 Unit / integration tests (TestGPUMesherIntegration additions)

| Test | What it verifies |
|---|---|
| `TestGPUSurfaceNetsFromGlobalSDF` | Initialize GlobalSDF, run kernel, verify vert_count > 0 and idx_count % 3 == 0 |
| `TestStreamingMeshBufferPersistence` | Call twice, verify buffer handles unchanged (no realloc) |
| `TestStreamingMeshGeneration` | Bump generation, verify Nanote picks up latest |
| `TestGlobalSDFMeshNodeFallback` | Trigger with GlobalSDF uninitialized, verify graceful skip |
| `TestStreamingMeshUnregisterTombstone` | Unregister mid-frame, verify no UAF / crash |

### 8.2 Render tests (TestGPUMesherIntegration render sub-tests)

| Test | What it verifies |
|---|---|
| `TestGlobalSDFMeshNodeVisual` | Wire GlobalSDFMeshNode → RenderScene; verify capture frame shows terrain draw calls |
| `TestStreamingMeshMaterialBind` | Assign material, verify vertex format / UVs match static mesh path |

### 8.3 End-to-end (TestPCGScatter)

- Add N key: toggle GlobalSDFMeshNode visibility
- Visual: terrain regenerates as camera moves alongside GlobalSDF cascade visualization

### 8.4 Performance budgets (TestGPUMesherIntegration perf sub-test)

| Resolution | 9.3b target (GPU-only, no readback) |
|---|---|
| 64³ | < 3ms |
| 128³ | < 10ms |

8-byte counter readback adds < 0.1ms. Measured via wall-clock around the blocking call until true GPU timer query lands.

### 8.5 Lifecycle leak test

- Register/unregister 100× → verify GPU memory returns to baseline
- Destroy node mid-execute → verify no UAF / crash

---

## 9. Roadmap position

| Phase | Status | Scope |
|---|---|---|
| 9.1 CPU SurfaceNets | ✅ Complete | MVP CPU kernel |
| 9.3a GPU SurfaceNets on PCGField | ✅ Complete (2026-06-22) | GPU compute infrastructure, CPU readback |
| **9.3b (this spec)** | 🚧 Design | GPU-resident streaming terrain, GlobalSDF direct bind |
| 9.4 Dual Contouring | Future | Sharp features via `PCGField::SampleGradient` |
| 9.5 Adaptive LOD | Future | Octree voxel structure + mesh simplification |

---

## 10. Open questions

- **Buffer reallocation on resolution change.** If `GlobalSDFMeshNode.resolution` is edited at runtime, current buffers may be undersized. Decision: detect on Execute; if new worst-case > current capacity, destroy + realloc. Allocate-on-first-Execute + realloc-on-param-change.
- **Frame-delayed readback.** 9.3a deferred this to 9.3b. Current 9.3b still uses synchronous counter readback. Full frame-delay would require double-buffered StreamingMesh. Decision: skip for v1; 8-byte copy is cheap enough. Revisit if profiling shows >0.5ms cost.
- **Multiple GlobalSDFMeshNodes in one scene.** Each owns its own StreamingMesh; RenderScene stores a vector. No shared budget enforcement. Decision: accept; if GPU memory pressure emerges, add a global cap.
- **Material binding.** First version hardcodes default material. Multi-material via `material_content_id` param is straightforward but deferred. Decision: add param now, ignore in v1 shader path.
- **Cluster build integration (Nanite mesh shading).** Decision: skip for v1. `draw_primitives_indirect` is sufficient. Cluster build for streaming is a 9.5 concern.
- **Tombstone cleanup timing.** Per-frame-end cleanup assumes one GPU queue. If compute + graphics queues split, need fence on the draw command buffer. Decision: v1 uses single queue (matches existing pattern); revisit if queue splits.
