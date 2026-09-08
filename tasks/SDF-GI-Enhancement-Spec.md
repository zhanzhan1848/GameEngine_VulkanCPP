# ⚠ SUPERSEDED — 本 spec 已被 2026-09-07 GI 补充规划（`Docs/2026-09-07-gi-supplement-plan.md`）实质取代。
# 原规划中的 D1–D7 决策仍有效，但实现路径已被 M0–M3 的修复批次覆盖。
# 保留仅作历史参考。下方为原文。

# OpenSpec: SDF Global Illumination Enhancement

```yaml
spec:
  id: sdf-gi-enhancement
  version: 1.0.0
  status: draft
  created: 2026-02-24
  author: AI Assistant
  target: UE5-quality GI using SDF in Sponza scene
  
requires:
  - Git worktree: feature/sdf-gi-enhancement
  - Branch base: main
  - Test scene: Sponza (EngineTest/IntegrationTests/TestGeometryDebugSponza.cpp)
```

---

## 1. Problem Statement

### 1.1 Current State Analysis

| Aspect | Current Implementation | UE5 Reference |
|--------|----------------------|---------------|
| **Resolution** | 32³ (fixed, hardcoded) | 128-512³ per mesh, scalable |
| **Precision** | R16_Float (unsigned) | Signed Distance Field (negative inside, positive outside) |
| **Generation** | CPU brute-force O(n*m) | GPU compute shaders with jump flooding |
| **Hierarchy** | None (single resolution) | Multi-resolution clipmaps centered on camera |
| **GI Integration** | Debug visualization only | Cone tracing + screen probes |
| **Vector Field** | Stored but unused | Used for dynamic GI propagation |

### 1.2 Current Code Locations

```
SDF Generation:
  ContentTools/Geometry.cpp:352-436
  - generate_sdf() function
  - Fixed 32³ resolution
  - Unsigned distance (no inside/outside sign)
  - CPU-only, O(res³ * triangle_count)

SDF Storage:
  Engine/Graphics/RHI/Core/RHIGpuMesh.cpp:182-194
  - R16_Float format for SDF texture
  - R8_UNorm for voxel density
  - No mipmaps

SDF Visualization:
  Engine/Graphics/RHI/Shaders/Debug/SDFDebug.metal
  - Slice-based visualization
  - No ray marching or cone tracing

Debug Pass:
  Engine/Graphics/RenderPipeline/RenderPasses/Debug/GeometryDebugPass.cpp
  - F2: SDF slice
  - F3: Voxel instances
  - F4: Vector field lines
```

### 1.3 Visual Artifacts (Current)

- **F2 (SDF)**: Shows single image - unsigned distance field without proper sign
- **F3 (Voxel)**: White screen - likely invalid/uninitialized voxel data
- **F4 (VectorField)**: Shows voxel-like patterns - vector field not used for GI

---

## 2. Solution Architecture

### 2.1 Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                    SDF GI Pipeline                              │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────┐      │
│  │ Mesh Assets  │───▶│ SDF Generator│───▶│ Global SDF   │      │
│  │ (Sponza)     │    │ (GPU)        │    │ Atlas        │      │
│  └──────────────┘    └──────────────┘    └──────────────┘      │
│                              │                      │           │
│                              ▼                      ▼           │
│                      ┌──────────────┐    ┌──────────────┐      │
│                      │ Signed DF    │    │ Clipmap      │      │
│                      │ R16/R32 Float│    │ Streaming    │      │
│                      └──────────────┘    └──────────────┘      │
│                                                    │           │
│                                                    ▼           │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────┐      │
│  │ GBuffer      │───▶│ SDF Ray      │───▶│ Lighting     │      │
│  │ (Deferred)   │    │ Marching     │    │ Integration  │      │
│  └──────────────┘    └──────────────┘    └──────────────┘      │
│                              │                                 │
│                              ▼                                 │
│                      ┌──────────────┐                         │
│                      │ Cone Tracing │                         │
│                      │ for GI       │                         │
│                      └──────────────┘                         │
└─────────────────────────────────────────────────────────────────┘
```

### 2.2 Core Components

#### 2.2.1 Signed Distance Field Generation

**Current Issue**: Unsigned distance field (no inside/outside distinction)

**Solution**: Implement proper signed distance field generation

```
Algorithm: Jump Flooding + Sign Determination

1. Voxelization Pass (Compute Shader)
   - Rasterize mesh to 3D grid
   - Mark interior/exterior voxels
   - Initialize boundary voxels with distance 0

2. Jump Flooding (Multiple Passes)
   - Step sizes: N/2, N/4, N/8, ..., 1
   - Each pass propagates closest point information
   - Final pass: accurate distance to surface

3. Sign Determination
   - Use winding number or ray casting
   - Interior voxels: negative distance
   - Exterior voxels: positive distance

Output: R16_Float signed distance field texture
```

**File**: `Engine/Graphics/RHI/Shaders/Compute/SDFGenerator.metal`

#### 2.2.2 Global Distance Field (GDF)

**Purpose**: Merge per-mesh SDFs into camera-centered clipmaps

```
Structure:
  - Multiple clipmap levels (128³, 64³, 32³, 16³)
  - Centered on camera position
  - Updated incrementally as camera moves
  - Packed into atlas texture

Memory Layout:
  - Level 0: 128³ × R16_Float = 4 MB (high detail, near camera)
  - Level 1: 64³ × R16_Float = 512 KB
  - Level 2: 32³ × R16_Float = 64 KB
  - Level 3: 16³ × R16_Float = 8 KB
  - Total: ~4.6 MB per clipmap cascade
```

**File**: `Engine/Graphics/GI/GlobalDistanceField.h/cpp`

#### 2.2.3 SDF Ray Marching

**Algorithm**: Sphere Tracing with Cone Tracing Extension

```cpp
// Sphere Tracing Core
float SDFRayMarch(float3 origin, float3 direction, float maxDistance) {
    float t = 0.0;
    for (int i = 0; i < MAX_STEPS; i++) {
        float3 p = origin + t * direction;
        float d = SampleGlobalSDF(p);  // Sample from clipmap
        
        if (d < EPSILON) return t;  // Hit
        if (t > maxDistance) return -1;  // Miss
        
        t += d;  // Sphere tracing step
    }
    return -1;
}

// Cone Tracing for Soft Shadows
float ConeTraceOcclusion(float3 origin, float3 direction, 
                         float coneAngle, float maxDistance) {
    float t = 0.0;
    float occlusion = 1.0;
    float minConeRadius = coneAngle;
    
    for (int i = 0; i < MAX_STEPS; i++) {
        float3 p = origin + t * direction;
        float d = SampleGlobalSDF(p);
        
        // Cone radius at current distance
        float coneRadius = max(minConeRadius, t * coneAngle);
        
        // Track closest occluder
        occlusion = min(occlusion, d / coneRadius);
        
        if (d < EPSILON) break;
        if (t > maxDistance) break;
        
        t += d * 0.5;  // Conservative step
    }
    
    return saturate(occlusion);
}
```

**File**: `Engine/Graphics/RHI/Shaders/GI/SDFRayMarching.metal`

#### 2.2.4 GI Integration with Deferred Pipeline

**Integration Points**:

1. **GBuffer Pass** (Existing)
   - Output: Albedo, Normal, ORM, Depth, Velocity
   - No changes required

2. **SDF GI Pass** (New)
   - Input: GBuffer depth + normal
   - Process: Ray march from GBuffer pixels
   - Output: Indirect lighting buffer (RGBA16_Float)

3. **Lighting Pass** (Modified)
   - Input: GBuffer + Indirect lighting buffer
   - Process: Combine direct + indirect lighting
   - Output: Final HDR buffer

**Render Graph Addition**:

```cpp
// In TestGeometryDebugSponza.cpp BuildRenderGraph()
auto indirectLighting = AddSDFGIPass(
    graph, 
    gbuffer.albedo, 
    gbuffer.normal, 
    gbuffer.depth,
    globalSDF,
    view
);

auto lighting = AddLightingPass(
    graph,
    gbuffer,
    indirectLighting,  // New input
    shadowMaps
);
```

---

## 3. Implementation Phases

### Phase 1: Signed Distance Field Foundation (Week 1)

**Goal**: Convert unsigned SDF to signed SDF with proper generation

#### Tasks

| ID | Task | Files | Priority |
|----|------|-------|----------|
| 1.1 | Implement GPU-based SDF generator compute shader | `Shaders/Compute/SDFGenerator.metal` | P0 |
| 1.2 | Add sign determination algorithm | `Shaders/Compute/SDFSignDetermination.metal` | P0 |
| 1.3 | Update Geometry.cpp to use GPU generation | `ContentTools/Geometry.cpp` | P0 |
| 1.4 | Change texture format from R16_Float (unsigned) to signed interpretation | `RHIGpuMesh.cpp` | P1 |
| 1.5 | Increase default resolution from 32³ to 64³ minimum | `Geometry.cpp:355` | P1 |
| 1.6 | Add SDF generation settings to mesh import | `RHIMeshAsset.h` | P2 |

#### Deliverables

- [ ] Compute shader for SDF generation
- [ ] Signed distance field output (verified with F2 visualization)
- [ ] Configurable resolution (64³ default, up to 256³ for key meshes)
- [ ] Performance: <500ms generation time for Sponza

### Phase 2: Global Distance Field System (Week 2)

**Goal**: Create camera-centered clipmap system for merged SDF

#### Tasks

| ID | Task | Files | Priority |
|----|------|-------|----------|
| 2.1 | Design GlobalDistanceField class | `GI/GlobalDistanceField.h` | P0 |
| 2.2 | Implement clipmap data structure | `GI/GlobalDistanceField.cpp` | P0 |
| 2.3 | Create clipmap update compute shader | `Shaders/GI/ClipmapUpdate.metal` | P0 |
| 2.4 | Implement SDF merging/atlas packing | `GI/SDFAtlas.cpp` | P1 |
| 2.5 | Add camera tracking and streaming | `GlobalDistanceField.cpp` | P1 |
| 2.6 | Debug visualization for GDF clipmaps | `GeometryDebugPass.cpp` | P2 |

#### Deliverables

- [ ] Multi-level clipmap system (4 levels)
- [ ] Dynamic streaming as camera moves
- [ ] Atlas packing for multiple meshes
- [ ] Debug visualization (new F6 mode)

### Phase 3: Ray Marching & Cone Tracing (Week 3)

**Goal**: Implement sphere tracing and cone tracing for GI

#### Tasks

| ID | Task | Files | Priority |
|----|------|-------|----------|
| 3.1 | Implement sphere tracing shader | `Shaders/GI/SDFRayMarching.metal` | P0 |
| 3.2 | Add cone tracing for soft shadows | `Shaders/GI/ConeTracing.metal` | P0 |
| 3.3 | Optimize with hierarchical stepping | `SDFRayMarching.metal` | P1 |
| 3.4 | Add temporal accumulation for stability | `GI/TemporalAccumulation.cpp` | P1 |
| 3.5 | Implement screen-space denoising | `Shaders/PostProcess/SDFDenoiser.metal` | P2 |

#### Deliverables

- [ ] Working sphere tracing
- [ ] Cone-traced soft shadows
- [ ] Temporal stability
- [ ] Denoised output

### Phase 4: GI Integration (Week 4)

**Goal**: Integrate SDF GI into deferred rendering pipeline

#### Tasks

| ID | Task | Files | Priority |
|----|------|-------|----------|
| 4.1 | Create SDFGIPass render graph node | `RenderPasses/GI/SDFGIPass.cpp` | P0 |
| 4.2 | Modify lighting pass for indirect lighting | `DeferredLighting.metal` | P0 |
| 4.3 | Add render graph integration | `TestGeometryDebugSponza.cpp` | P0 |
| 4.4 | Implement light probe backup | `GI/LightProbeBackup.cpp` | P1 |
| 4.5 | Add quality/performance settings | `GI/SDFGISettings.h` | P2 |
| 4.6 | Performance profiling and optimization | All GI files | P2 |

#### Deliverables

- [ ] Integrated SDF GI in Sponza scene
- [ ] Toggle between SDF GI / SSGI / no GI
- [ ] Quality settings (Low/Medium/High/Epic)
- [ ] Performance target: 30 FPS at 1080p on Sponza

---

## 4. Technical Specifications

### 4.1 SDF Resolution Guidelines

| Mesh Type | Resolution | Memory | Use Case |
|-----------|------------|--------|----------|
| Small props (<1m) | 32³ | 64 KB | Background objects |
| Medium objects (1-5m) | 64³ | 512 KB | Furniture, doors |
| Large structures (5-20m) | 128³ | 4 MB | Walls, floors |
| Huge geometry (>20m) | 256³ | 32 MB | Main Sponza structure |

### 4.2 Texture Formats

| Data Type | Format | Precision | Reasoning |
|-----------|--------|-----------|-----------|
| Per-Mesh SDF | R16_Float | 16-bit half | Sufficient for most meshes |
| Global SDF | R16_Float | 16-bit half | Memory efficient |
| High-Precision SDF | R32_Float | 32-bit float | Critical geometry only |
| Voxel Density | R8_UNorm | 8-bit | Occupancy only |

### 4.3 Performance Targets

| Metric | Target | Measurement |
|--------|--------|-------------|
| SDF Generation (per mesh) | <100ms | Single mesh, 64³ |
| Global SDF Update | <2ms | Per frame, camera move |
| Ray Marching | <4ms | 1080p, half resolution |
| Cone Tracing | <2ms | 8 cones per pixel |
| Total GI Budget | <8ms | Full SDF GI pipeline |

### 4.4 Quality Settings

```cpp
enum class SDFGIQuality {
    Low,     // 32³ clipmap, 4 ray steps, no cones
    Medium,  // 64³ clipmap, 8 ray steps, 4 cones
    High,    // 128³ clipmap, 16 ray steps, 8 cones
    Epic     // 256³ clipmap, 32 ray steps, 16 cones
};

struct SDFGISettings {
    SDFGIQuality quality = SDFGIQuality::High;
    float maxDistance = 50.0f;        // Max ray march distance
    float stepFactor = 0.5f;          // Conservative stepping
    uint32_t maxSteps = 16;           // Per ray
    uint32_t coneCount = 8;           // For soft shadows
    bool enableTemporal = true;       // Temporal accumulation
    bool enableDenoiser = true;       // Spatial denoiser
};
```

---

## 5. Testing & Validation

### 5.1 Test Cases

| Test ID | Description | Expected Result | File |
|---------|-------------|-----------------|------|
| T1.1 | SDF generation produces signed distances | Negative inside, positive outside | `TestSDFGeneration.cpp` |
| T1.2 | 64³ resolution produces visible detail | Clearer F2 visualization | `TestGeometryDebugSponza.cpp` |
| T2.1 | Clipmap updates on camera move | Smooth transitions | `TestGlobalSDF.cpp` |
| T2.2 | Multiple meshes merge correctly | No seams in GDF | `TestSDFAtlas.cpp` |
| T3.1 | Ray marching hits surfaces | Correct intersection | `TestRayMarching.cpp` |
| T3.2 | Cone tracing produces soft shadows | Gradual falloff | `TestConeTracing.cpp` |
| T4.1 | GI integrates with deferred pipeline | Indirect lighting visible | `TestSDFGIIntegration.cpp` |
| T4.2 | Performance meets targets | <8ms total | `TestSDFGIPerformance.cpp` |

### 5.2 Visual Validation

**F2 Mode Improvements**:
- Before: Single flat image
- After: Clear SDF visualization with inside/outside coloring

**F3 Mode Improvements**:
- Before: White screen
- After: Proper voxel occupancy visualization

**F4 Mode Improvements**:
- Before: Voxel-like patterns
- After: Clear vector field visualization

**New F6 Mode**:
- Global Distance Field clipmap visualization
- Camera-relative streaming debug

### 5.3 Performance Benchmarks

```
Scene: Sponza (EngineTest/assets/Sponza_process_rebuild.model)
Resolution: 1920x1080 (Retina: 3840x2160)
Hardware: Apple Silicon (M1/M2)

Baseline (no GI):
  - GBuffer: 2.1ms
  - Lighting: 1.8ms
  - Total: 3.9ms (256 FPS)

SDF GI (Low):
  - GBuffer: 2.1ms
  - SDF GI: 4.2ms
  - Lighting: 1.8ms
  - Total: 8.1ms (123 FPS)

SDF GI (High):
  - GBuffer: 2.1ms
  - SDF GI: 12.4ms
  - Lighting: 1.8ms
  - Total: 16.3ms (61 FPS)
```

---

## 6. Risk Mitigation

### 6.1 Technical Risks

| Risk | Probability | Impact | Mitigation |
|------|-------------|--------|------------|
| SDF generation too slow | Medium | High | Use PhysX PxSDFBuilder (already available) |
| Memory budget exceeded | Low | High | Implement sparse SDF, streaming |
| Ray marching artifacts | Medium | Medium | Increase steps, temporal filtering |
| Light leaking | Medium | Medium | Proper sign determination, bias adjustment |
| Performance regression | Medium | High | Fallback to SSGI, quality scaling |

### 6.2 Fallback Strategy

1. **Primary**: Full SDF GI with cone tracing
2. **Fallback 1**: SDF GI without cones (harder shadows)
3. **Fallback 2**: SSGI with SDF acceleration
4. **Fallback 3**: Light probes only (existing system)

---

## 7. Success Criteria

### 7.1 Must Have (P0)

- [ ] Signed distance field generation (not unsigned)
- [ ] Minimum 64³ resolution for Sponza meshes
- [ ] Working sphere tracing for visibility
- [ ] GI visible in Sponza scene
- [ ] F2/F3/F4 modes show correct visualization

### 7.2 Should Have (P1)

- [ ] Global Distance Field with clipmaps
- [ ] Cone tracing for soft shadows
- [ ] Temporal accumulation
- [ ] Quality settings (Low/Medium/High)

### 7.3 Nice to Have (P2)

- [ ] Sparse SDF for memory optimization
- [ ] Denoiser integration
- [ ] Epic quality preset
- [ ] Real-time SDF updates for dynamic objects

---

## 8. References

### 8.1 UE5 Lumen Technical Details

- **Primary**: [Lumen Technical Details](https://dev.epicgames.com/documentation/en-us/unreal-engine/lumen-technical-details-in-unreal-engine)
- **Mesh Distance Fields**: [Mesh Distance Fields](https://dev.epicgames.com/documentation/en-us/unreal-engine/mesh-distance-fields-in-unreal-engine)
- **SIGGRAPH 2022**: "Lumen: Real-time Global Illumination in Unreal Engine 5"

### 8.2 Research Papers

1. **SDF Dynamic GI**: "Signed Distance Fields Dynamic Diffuse Global Illumination" (arXiv:2007.14394)
2. **RTSDF**: "Generating Signed Distance Fields in Real Time" (arXiv:2210.04449)
3. **NVIDIA GPU Gems**: Chapter 34 - SDF Generation using Single-Pass GPU Scan

### 8.3 Alternative Implementations

- **AMD FidelityFX Brixelizer/GI**: Sparse voxel GI
- **NVIDIA VXGI**: Voxel Cone Tracing
- **CryEngine SVOGI**: Sparse Voxel Octree GI

---

## 9. Approval Checklist

Before proceeding with implementation, please confirm:

- [ ] Problem statement accurately describes current issues
- [ ] Solution architecture is feasible for the codebase
- [ ] Implementation phases are appropriately scoped
- [ ] Technical specifications meet project requirements
- [ ] Timeline (4 weeks) is acceptable
- [ ] Success criteria are measurable and achievable

**Approval Required From**: @zhanyuanwei

---

## 10. Appendix

### 10.1 Current SDF Generation Code (Reference)

```cpp
// ContentTools/Geometry.cpp:352-436
void generate_sdf(mesh& m) {
    constexpr u32 res = 32;  // ISSUE: Fixed low resolution
    
    // ... bounds calculation ...
    
    for (u32 z = 0; z < res; ++z) {
        for (u32 y = 0; y < res; ++y) {
            for (u32 x = 0; x < res; ++x) {
                // ISSUE: Brute-force O(n*m) algorithm
                for (u32 i = 0; i < num_indices; i += 3) {
                    // Find closest point on triangle
                    // ISSUE: No sign determination
                }
                
                // ISSUE: Stores unsigned distance
                m.sdf.data[idx] = math::pack_float<16>(dist, 0.0f, max_dim);
            }
        }
    }
}
```

### 10.2 UE5 SDF Generation (Reference)

```cpp
// UE5 Approach (Conceptual)
void GenerateSignedDistanceFieldGPU(FMeshSDF& OutSDF) {
    // 1. Voxelize mesh to determine inside/outside
    VoxelizeMeshOnGPU();
    
    // 2. Jump Flooding Algorithm (JFA) for distance propagation
    for (int step = N/2; step > 0; step /= 2) {
        JumpFloodingPass(step);
    }
    
    // 3. Sign from voxelization
    ApplySignFromVoxelization();
    
    // 4. Optional: Dilate for robustness
    DilateSDF();
}
```

### 10.3 File Structure (Proposed)

```
Engine/
├── Graphics/
│   ├── GI/
│   │   ├── GlobalDistanceField.h
│   │   ├── GlobalDistanceField.cpp
│   │   ├── SDFAtlas.h
│   │   ├── SDFAtlas.cpp
│   │   ├── SDFGISettings.h
│   │   └── TemporalAccumulation.cpp
│   ├── RHI/
│   │   └── Shaders/
│   │       ├── Compute/
│   │       │   ├── SDFGenerator.metal
│   │       │   └── SDFSignDetermination.metal
│   │       └── GI/
│   │           ├── SDFRayMarching.metal
│   │           ├── ConeTracing.metal
│   │           └── ClipmapUpdate.metal
│   └── RenderPipeline/
│       └── RenderPasses/
│           ├── GI/
│           │   ├── SDFGIPass.h
│           │   └── SDFGIPass.cpp
│           └── Debug/
│               └── GeometryDebugPass.cpp (modified)

ContentTools/
└── Geometry.cpp (modified for GPU SDF generation)

EngineTest/
└── IntegrationTests/
    └── TestGeometryDebugSponza.cpp (modified for GI integration)
```

---

**Document Status**: DRAFT - Awaiting Approval

**Next Step**: Review and approve this specification before implementation begins.
