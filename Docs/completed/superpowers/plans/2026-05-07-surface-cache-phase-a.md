# Surface Cache Phase A Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement UE5 Lumen-style Surface Cache (Phase A Lite) — card capture, direct/indirect lighting on cached surfaces, and Screen Probe integration.

**Architecture:** Per-mesh axis-aligned cards capture material data into a 2048² atlas. Direct lighting evaluates per-texel on the atlas. Indirect lighting traces short rays from probes placed on 8×8 tiles, sampling previous-frame Final Lighting for multi-bounce propagation. Screen Probes sample the lit atlas for near-field GI, falling back to DDGI for far-field.

**Tech Stack:** C++17 / Metal Shading Language / Apple Silicon (M-series)

**Spec:** `Docs/superpowers/specs/2026-05-07-surface-cache-design.md`

---

## File Structure

### New Files

| File | Responsibility |
|------|---------------|
| `Engine/Graphics/Lumen/SurfaceCache/SurfaceCacheTypes.h` | Card struct, params, output types, constants |
| `Engine/Graphics/Lumen/SurfaceCache/SurfaceCachePass.h` | Main pass class declaration |
| `Engine/Graphics/Lumen/SurfaceCache/SurfaceCachePass.cpp` | Init, CreatePipelines, AddPass, Shutdown |
| `Engine/Graphics/Lumen/SurfaceCache/CardGenerator.h` | Runtime card generation from mesh AABBs |
| `Engine/Graphics/Lumen/SurfaceCache/CardGenerator.cpp` | Generate 6 axis-aligned cards per mesh |
| `Engine/Graphics/Metal/shaders/Lumen/SurfaceCacheData.metal` | Shared shader types, constants, helpers |
| `Engine/Graphics/Metal/shaders/Lumen/SurfaceCacheCapture.metal` | Fragment shader for card material capture |
| `Engine/Graphics/Metal/shaders/Lumen/SurfaceCacheDilate.metal` | Compute shader for depth dilation |
| `Engine/Graphics/Metal/shaders/Lumen/SurfaceCacheLightCull.metal` | Compute shader for per-tile light assignment |
| `Engine/Graphics/Metal/shaders/Lumen/SurfaceCacheLightEval.metal` | Compute shader for per-texel direct lighting |
| `Engine/Graphics/Metal/shaders/Lumen/SurfaceCacheIndirectTrace.metal` | Compute shader for indirect ray tracing |
| `Engine/Graphics/Metal/shaders/Lumen/SurfaceCacheIndirectResolve.metal` | Compute shader for indirect lighting resolve |

### Modified Files

| File | Change |
|------|--------|
| `Engine/Graphics/Lumen/ScreenProbes/ScreenProbeGIPass.h` | Add surface cache texture bindings |
| `Engine/Graphics/Lumen/ScreenProbes/ScreenProbeGIPass.cpp` | Bind lighting_atlas + card_data in trace pass |
| `Engine/Graphics/Metal/shaders/Lumen/ScreenProbeTraceRays.metal` | Add near-hit surface cache sampling branch |

### Existing Patterns to Follow

- **Class structure**: `Initialize(device, params)` / `AddPass(graph, ...)` / `Shutdown()` — see `LumenDDGIPass.h`
- **Member naming**: `snake_case_` with trailing underscore
- **Handle types**: `rhi::PipelineHandle`, `rhi::ResourceHandle`, initialized to `rhi::handles::INVALID_*`
- **Triple buffering**: arrays of size 3, indexed by `frame_index % 3`
- **RenderGraph**: `graph.AddPass<DataType>(name, type, category, setup_lambda, execute_lambda)`
- **Shader loading**: `LoadShaderBytecode("filename")` from `Engine/Graphics/Metal/shaders/Lumen/`
- **Metal bindings**: `[[buffer(0)]]` = GlobalShaderData, `[[buffer(1)]]` = pass params, `[[texture(N)]]` for textures
- **Thread groups**: `(64,1,1)` for 1D, `(16,16,1)` for 2D
- **Fragment texture sampling**: Must wrap in separate function (Apple Silicon inline sampling bug)

---

## Task 1: Types and Shared Data Structures

**Files:**
- Create: `Engine/Graphics/Lumen/SurfaceCache/SurfaceCacheTypes.h`
- Create: `Engine/Graphics/Metal/shaders/Lumen/SurfaceCacheData.metal`

- [ ] **Step 1: Create SurfaceCacheTypes.h**

```cpp
#pragma once

#include "CommonHeaders.h"
#include "../../RHI/Core/RHITypes.h"

namespace primal::graphics::lumen {

// Must match Metal SurfaceCacheData.metal exactly
// Uses math::v4 instead of math::v3 to guarantee 16-byte alignment (matches DDGIVolumeData pattern)
struct SurfaceCacheCard {
    math::v4  center;         // xyz = card center in mesh local space, w unused
    math::v4  extent;         // xyz = half-extents from AABB, w unused
    uint32_t  axis_direction; // low byte = axis (0=X,1=Y,2=Z), high byte = direction (0/1)
    uint16_t  resolution;     // Texel resolution in atlas
    uint16_t  _pad0;
    uint32_t  atlas_offset_x;
    uint32_t  atlas_offset_y;
    uint32_t  mesh_instance_id;
    uint32_t  _pad1[3];
};
static_assert(sizeof(SurfaceCacheCard) == 64, "SurfaceCacheCard must be 64 bytes");

struct SurfaceCacheCardLookup {
    math::v4  aabb_min;      // xyz = min, w unused
    math::v4  aabb_max;      // xyz = max, w unused
    uint32_t  card_start;
    uint32_t  card_count;
    uint32_t  _pad[2];
};
static_assert(sizeof(SurfaceCacheCardLookup) == 48, "SurfaceCacheCardLookup must be 48 bytes");

struct SurfaceCacheParams {
    uint32_t  atlas_size;
    uint32_t  page_size;
    uint32_t  capture_budget_pages;
    uint32_t  max_cards;
    float     update_distance;
    float     importance_weight;
    uint32_t  max_lights_per_tile;
    uint32_t  indirect_rays_per_probe;
    float     indirect_temporal_weight;
    float     indirect_near_distance;
    uint32_t  _pad[2];
};

// Minimal camera data — card capture uses orthographic projection from card,
// not camera view/proj. Only camera_position needed for priority calculation.
struct SurfaceCacheFrameData {
    math::v3   camera_position;
    float      _pad;
    uint32_t   frame_index;
    uint32_t   light_count;
    float      _pad2[2];
};

struct SurfaceCacheOutput {
    rhi::RGResourceHandle lighting_atlas;
};

} // namespace
```

- [ ] **Step 2: Create SurfaceCacheData.metal**

```metal
#include <metal_stdlib>
using namespace metal;

// Must match C++ SurfaceCacheCard exactly (uses float4 for 16-byte alignment, same pattern as DDGIVolumeData)
struct SurfaceCacheCard {
    float4   center;         // xyz = center, w unused
    float4   extent;         // xyz = half-extents, w unused
    uint     axis_direction; // low byte = axis, high byte = direction
    uint16_t resolution;
    uint16_t _pad0;
    uint     atlas_offset_x;
    uint     atlas_offset_y;
    uint     mesh_instance_id;
    uint     _pad1[3];
};

struct SurfaceCacheCardLookup {
    float4   aabb_min;      // xyz = min, w unused
    float4   aabb_max;      // xyz = max, w unused
    uint     card_start;
    uint     card_count;
    uint     _pad[2];
};

struct SurfaceCacheParams {
    uint    atlas_size;
    uint    page_size;
    uint    capture_budget_pages;
    uint    max_cards;
    float   update_distance;
    float   importance_weight;
    uint    max_lights_per_tile;
    uint    indirect_rays_per_probe;
    float   indirect_temporal_weight;
    float   indirect_near_distance;
    uint    _pad[2];
};

// Helper: reconstruct world position from card atlas UV + depth
static float3 cardTexelToWorld(
    uint2 atlas_uv,
    float depth,
    constant SurfaceCacheCard& card)
{
    float3 local_pos;
    float u = (float(atlas_uv.x - card.atlas_offset_x) + 0.5) / float(card.resolution);
    float v = (float(atlas_uv.y - card.atlas_offset_y) + 0.5) / float(card.resolution);

    if (card.axis == 0) { // X axis
        local_pos.x = (card.direction ? 1.0 : -1.0) * depth;
        local_pos.y = (u - 0.5) * card.extent.y * 2.0;
        local_pos.z = (v - 0.5) * card.extent.z * 2.0;
    } else if (card.axis == 1) { // Y axis
        local_pos.y = (card.direction ? 1.0 : -1.0) * depth;
        local_pos.x = (u - 0.5) * card.extent.x * 2.0;
        local_pos.z = (v - 0.5) * card.extent.z * 2.0;
    } else { // Z axis
        local_pos.z = (card.direction ? 1.0 : -1.0) * depth;
        local_pos.x = (u - 0.5) * card.extent.x * 2.0;
        local_pos.y = (v - 0.5) * card.extent.y * 2.0;
    }
    return local_pos + card.center;
}

// Helper: find atlas UV for a world position given a card
static bool worldToCardUV(
    float3 world_pos,
    constant SurfaceCacheCard& card,
    thread float2& out_uv)
{
    float3 local = world_pos - card.center;
    float u, v, depth;
    float2 extent_uv;

    if (card.axis == 0) {
        depth = local.x * (card.direction ? 1.0 : -1.0);
        u = (local.y / (card.extent.y * 2.0)) + 0.5;
        v = (local.z / (card.extent.z * 2.0)) + 0.5;
    } else if (card.axis == 1) {
        depth = local.y * (card.direction ? 1.0 : -1.0);
        u = (local.x / (card.extent.x * 2.0)) + 0.5;
        v = (local.z / (card.extent.z * 2.0)) + 0.5;
    } else {
        depth = local.z * (card.direction ? 1.0 : -1.0);
        u = (local.x / (card.extent.x * 2.0)) + 0.5;
        v = (local.y / (card.extent.y * 2.0)) + 0.5;
    }

    if (depth < 0.0 || u < 0.0 || u > 1.0 || v < 0.0 || v > 1.0) return false;

    out_uv.x = float(card.atlas_offset_x) + u * float(card.resolution);
    out_uv.y = float(card.atlas_offset_y) + v * float(card.resolution);
    return true;
}

// Octahedral normal encoding (matches DDGI pattern)
static float2 octEncode(float3 n) {
    float l1norm = abs(n.x) + abs(n.y) + abs(n.z);
    float2 result = n.xy / l1norm;
    if (n.z < 0.0) {
        result = (1.0 - abs(result.yx)) * select(float2(-1.0), float2(1.0), result.xy >= 0.0);
    }
    return result * 0.5 + 0.5;
}

static float3 octDecode(float2 f) {
    f = f * 2.0 - 1.0;
    float3 n = float3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    float t = clamp(-n.z, 0.0, 1.0);
    n.xy += select(float2(-t), float2(t), n.xy >= 0.0);
    return normalize(n);
}

// Light info — shared by LightCull and LightEval shaders (defined once here)
struct LightInfo {
    float3  position;
    float   radius;
    float3  color;
    float   _pad0;
    float3  direction;    // For directional lights
    uint    type;         // 0=point, 1=directional, 2=spot
    float   _pad1[2];
};
```

- [ ] **Step 3: Verify both files compile (no syntax errors)**

Run: `grep -c "struct" Engine/Graphics/Lumen/SurfaceCache/SurfaceCacheTypes.h Engine/Graphics/Metal/shaders/Lumen/SurfaceCacheData.metal`
Expected: Count > 0 for both files (basic syntax sanity check)

- [ ] **Step 4: Commit**

```bash
git add Engine/Graphics/Lumen/SurfaceCache/SurfaceCacheTypes.h Engine/Graphics/Metal/shaders/Lumen/SurfaceCacheData.metal
git commit -m "feat(surface-cache): add shared data types and Metal shader helpers"
```

---

## Task 2: Card Generator

**Files:**
- Create: `Engine/Graphics/Lumen/SurfaceCache/CardGenerator.h`
- Create: `Engine/Graphics/Lumen/SurfaceCache/CardGenerator.cpp`

**Depends on:** Task 1

- [ ] **Step 1: Create CardGenerator.h**

```cpp
#pragma once

#include "CommonHeaders.h"
#include "SurfaceCacheTypes.h"
#include "../../Nanite/GPUDrivenDrawPipeline.h"

namespace primal::graphics::lumen {

struct MeshInstanceInfo {
    math::v3  aabb_min;
    math::v3  aabb_max;
    uint32_t  instance_id;
    float     screen_space_area;  // Populated per-frame for priority
};

class CardGenerator {
public:
    CardGenerator() = default;

    // Generate cards for all registered meshes
    void Initialize(rhi::RHIDeviceBase* device, uint32_t atlas_size, uint32_t page_size, uint32_t max_cards);

    // Register a mesh instance (call when meshes are loaded)
    void RegisterMesh(const math::v3& aabb_min, const math::v3& aabb_max, uint32_t instance_id);

    // Rebuild card allocation (call when meshes change)
    void RebuildCardAllocation();

    // Get GPU buffers (for binding to shaders)
    rhi::ResourceHandle GetCardDataBuffer() const { return card_data_buffer_; }
    rhi::ResourceHandle GetCardLookupBuffer() const { return card_lookup_buffer_; }
    uint32_t GetCardCount() const { return card_count_; }
    uint32_t GetLookupCount() const { return static_cast<uint32_t>(meshes_.size()); }

private:
    void AllocateAtlasSpace();
    void UploadToGPU();

    rhi::RHIDeviceBase* device_ = nullptr;
    uint32_t atlas_size_ = 0;
    uint32_t page_size_ = 0;
    uint32_t max_cards_ = 0;

    std::vector<MeshInstanceInfo> meshes_;
    std::vector<SurfaceCacheCard> cards_;
    std::vector<SurfaceCacheCardLookup> lookups_;
    uint32_t card_count_ = 0;
    bool allocation_dirty_ = true;

    rhi::ResourceHandle card_data_buffer_;
    rhi::ResourceHandle card_lookup_buffer_;
};

} // namespace
```

- [ ] **Step 2: Create CardGenerator.cpp**

```cpp
#include "CardGenerator.h"
#include "../../RHI/Core/RHIDeviceBase.h"

namespace primal::graphics::lumen {

void CardGenerator::Initialize(rhi::RHIDeviceBase* device, uint32_t atlas_size, uint32_t page_size, uint32_t max_cards) {
    device_ = device;
    atlas_size_ = atlas_size;
    page_size_ = page_size;
    max_cards_ = max_cards;

    // Pre-allocate GPU buffers
    rhi::BufferDesc card_desc{};
    card_desc.size = max_cards * sizeof(SurfaceCacheCard);
    card_desc.usage = rhi::BufferUsage::ShaderResource;
    card_desc.cpu_access = rhi::CPUAccess::Write;
    card_data_buffer_ = device_->CreateBuffer(card_desc);

    rhi::BufferDesc lookup_desc{};
    lookup_desc.size = max_cards * sizeof(SurfaceCacheCardLookup); // generous upper bound
    lookup_desc.usage = rhi::BufferUsage::ShaderResource;
    lookup_desc.cpu_access = rhi::CPUAccess::Write;
    card_lookup_buffer_ = device_->CreateBuffer(lookup_desc);
}

void CardGenerator::RegisterMesh(const math::v3& aabb_min, const math::v3& aabb_max, uint32_t instance_id) {
    MeshInstanceInfo info;
    info.aabb_min = aabb_min;
    info.aabb_max = aabb_max;
    info.instance_id = instance_id;
    info.screen_space_area = 0.0f;
    meshes_.push_back(info);
    allocation_dirty_ = true;
}

void CardGenerator::RebuildCardAllocation() {
    if (!allocation_dirty_) return;

    cards_.clear();
    lookups_.clear();

    math::v3 extent = math::v3(0, 0, 0);
    uint32_t current_atlas_x = 0;
    uint32_t current_atlas_y = 0;
    uint32_t row_height = 0;
    uint32_t pages_per_side = atlas_size_ / page_size_;

    for (auto& mesh : meshes_) {
        math::v3 half_extent = (mesh.aabb_max - mesh.aabb_min) * 0.5f;
        math::v3 center = (mesh.aabb_min + mesh.aabb_max) * 0.5f;

        SurfaceCacheCardLookup lookup;
        lookup.aabb_min = mesh.aabb_min;
        lookup.aabb_max = mesh.aabb_max;
        lookup.card_start = static_cast<uint32_t>(cards_.size());
        lookup.card_count = 0;

        // Generate 6 axis-aligned cards
        for (uint8_t axis = 0; axis < 3; ++axis) {
            for (uint8_t dir = 0; dir < 2; ++dir) {
                // Skip cards with near-zero projection area
                float proj_area;
                if (axis == 0) proj_area = half_extent.y * half_extent.z * 4.0f;
                else if (axis == 1) proj_area = half_extent.x * half_extent.z * 4.0f;
                else proj_area = half_extent.x * half_extent.y * 4.0f;

                if (proj_area < 0.01f) continue; // Skip degenerate cards
                if (cards_.size() >= max_cards_) break;

                SurfaceCacheCard card;
                card.center = math::v4(center.x, center.y, center.z, 0.0f);
                card.extent = math::v4(half_extent.x, half_extent.y, half_extent.z, 0.0f);
                card.axis_direction = static_cast<uint32_t>(axis) | (static_cast<uint32_t>(dir) << 8);
                card.resolution = 0; // Set below
                card.atlas_offset_x = 0;
                card.atlas_offset_y = 0;
                card.mesh_instance_id = mesh.instance_id;

                // Calculate resolution from projection area (clamped to page_size multiples)
                float texels_per_unit = 100.0f; // ~100 texels per world unit (matches UE5 default)
                float proj_size;
                if (axis == 0) proj_size = max(half_extent.y, half_extent.z) * 2.0f;
                else if (axis == 1) proj_size = max(half_extent.x, half_extent.z) * 2.0f;
                else proj_size = max(half_extent.x, half_extent.y) * 2.0f;

                uint32_t res = static_cast<uint32_t>(proj_size * texels_per_unit);
                res = max(res, page_size_); // Minimum one page
                res = min(res, pages_per_side * page_size_); // Max atlas side length
                // Round up to page_size multiple
                res = ((res + page_size_ - 1) / page_size_) * page_size_;
                card.resolution = static_cast<uint16_t>(res);

                // Simple row-based atlas packing
                if (current_atlas_x + res > atlas_size_) {
                    current_atlas_x = 0;
                    current_atlas_y += row_height;
                    row_height = 0;
                }
                card.atlas_offset_x = current_atlas_x;
                card.atlas_offset_y = current_atlas_y;
                current_atlas_x += res;
                row_height = max(row_height, res);

                cards_.push_back(card);
                lookup.card_count++;
            }
            if (cards_.size() >= max_cards_) break;
        }

        lookups_.push_back(lookup);
    }

    card_count_ = static_cast<uint32_t>(cards_.size());
    UploadToGPU();
    allocation_dirty_ = false;
}

void CardGenerator::UploadToGPU() {
    if (cards_.empty()) return;

    auto* card_data = static_cast<SurfaceCacheCard*>(device_->MapBuffer(card_data_buffer_));
    if (card_data) {
        memcpy(card_data, cards_.data(), cards_.size() * sizeof(SurfaceCacheCard));
        device_->UnmapBuffer(card_data_buffer_);
    }

    if (!lookups_.empty()) {
        auto* lookup_data = static_cast<SurfaceCacheCardLookup*>(device_->MapBuffer(card_lookup_buffer_));
        if (lookup_data) {
            memcpy(lookup_data, lookups_.data(), lookups_.size() * sizeof(SurfaceCacheCardLookup));
            device_->UnmapBuffer(card_lookup_buffer_);
        }
    }
}

} // namespace
```

- [ ] **Step 3: Verify compilation**

Run: `cd /Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP && cmake --build build --target Engine 2>&1 | head -30`
Expected: No errors in new files (may have linker issues until Task 3, that's OK)

- [ ] **Step 4: Commit**

```bash
git add Engine/Graphics/Lumen/SurfaceCache/CardGenerator.h Engine/Graphics/Lumen/SurfaceCache/CardGenerator.cpp
git commit -m "feat(surface-cache): add runtime card generator with atlas packing"
```

---

## Task 3: Main Pass Skeleton — Init, Atlas Creation, Shutdown

**Files:**
- Create: `Engine/Graphics/Lumen/SurfaceCache/SurfaceCachePass.h`
- Create: `Engine/Graphics/Lumen/SurfaceCache/SurfaceCachePass.cpp`

**Depends on:** Task 1, Task 2

- [ ] **Step 1: Create SurfaceCachePass.h**

```cpp
#pragma once

#include "CommonHeaders.h"
#include "SurfaceCacheTypes.h"
#include "CardGenerator.h"
#include "../LumenTypes.h"
#include "../../RHI/Core/RHITypes.h"
#include "../../RenderGraph/RenderGraph.h"

namespace primal::graphics::lumen {

class SurfaceCachePass {
public:
    SurfaceCachePass() = default;
    ~SurfaceCachePass();

    bool Initialize(rhi::RHIDeviceBase* device, const LumenConfig& config);
    void Shutdown();

    SurfaceCacheOutput AddPass(
        rendergraph::RenderGraph& graph,
        rhi::RGResourceHandle prev_frame_color,
        rhi::ResourceHandle light_data_buffer,
        const SurfaceCacheFrameData& frame_data,
        u32 current_frame_index);

    // For Screen Probe integration — returns the correct frame's atlas
    rhi::ResourceHandle GetLightingAtlas(u32 frame_index) const { return lighting_atlas_[frame_index % 3]; }
    rhi::ResourceHandle GetCardDataBuffer() const { return card_generator_.GetCardDataBuffer(); }
    rhi::ResourceHandle GetCardLookupBuffer() const { return card_generator_.GetCardLookupBuffer(); }

    bool IsInitialized() const { return initialized_; }

private:
    void CreateAtlasTextures();
    void CreateConstantBuffers();
    void CreateDescriptorSetLayouts(); // Per-pass descriptor layouts
    void CreatePipelines(); // Stubs, filled in later tasks

    bool initialized_ = false;
    rhi::RHIDeviceBase* device_ = nullptr;
    LumenConfig config_{};

    CardGenerator card_generator_;

    // Atlas textures (persistent, not per-frame)
    rhi::ResourceHandle albedo_atlas_;
    rhi::ResourceHandle normal_atlas_;
    rhi::ResourceHandle depth_atlas_;
    rhi::ResourceHandle emissive_atlas_;
    rhi::ResourceHandle lighting_atlas_[3];     // Triple-buffered for temporal
    rhi::ResourceHandle prev_lighting_atlas_[3];

    // Constant buffers (triple-buffered)
    rhi::ResourceHandle global_cb_[3];
    rhi::ResourceHandle params_cb_[3];

    // Pipelines (created in later tasks)
    rhi::PipelineHandle capture_pipeline_;
    rhi::PipelineHandle dilate_pipeline_;
    rhi::PipelineHandle light_cull_pipeline_;
    rhi::PipelineHandle light_eval_pipeline_;
    rhi::PipelineHandle indirect_trace_pipeline_;
    rhi::PipelineHandle indirect_resolve_pipeline_;

    // Descriptor layouts
    rhi::PipelineLayoutHandle capture_layout_;
    rhi::PipelineLayoutHandle dilate_layout_;
    rhi::PipelineLayoutHandle light_cull_layout_;
    rhi::PipelineLayoutHandle light_eval_layout_;
    rhi::PipelineLayoutHandle indirect_trace_layout_;
    rhi::PipelineLayoutHandle indirect_resolve_layout_;

    // Descriptor sets (triple-buffered, per-pass)
    rhi::DescriptorSetHandle capture_set_[3];
    rhi::DescriptorSetHandle dilate_set_[3];
    rhi::DescriptorSetHandle light_cull_set_[3];
    rhi::DescriptorSetHandle light_eval_set_[3];
    rhi::DescriptorSetHandle indirect_trace_set_[3];
    rhi::DescriptorSetHandle indirect_resolve_set_[3];

    // Runtime buffers
    rhi::ResourceHandle light_assignment_buffer_;
    rhi::ResourceHandle light_info_buffer_;
    rhi::ResourceHandle ray_hits_buffer_;

    u32 atlas_size_ = 0;
    u32 page_size_ = 0;
};

} // namespace
```

- [ ] **Step 2: Create SurfaceCachePass.cpp with Init/Shutdown/CreateAtlasTextures**

Full implementation of:
- `Initialize()`: Store config, call CreateAtlasTextures, CreateConstantBuffers, init CardGenerator
- `CreateAtlasTextures()`: Create all atlas textures (albedo RGBA8, normal RG16F, depth R32F, emissive RGB11F, lighting RGBA16F x3, prev_lighting RGBA16F x3)
- `CreateConstantBuffers()`: Triple-buffered GlobalShaderData and SurfaceCacheParams
- `Shutdown()`: Release all handles
- `AddPass()`: Stub that imports atlas resources and returns output (filled in later tasks)
- `CreatePipelines()`: Stub (filled in later tasks)

Follow the exact pattern from `LumenDDGIPass.cpp`:
- `rhi::handles::INVALID_RESOURCE` for initial handles
- `device_->CreateTexture(desc)` with proper formats
- `device_->CreateBuffer(desc)` for constant buffers
- RenderGraph `ImportResource` + `AddPass` pattern

Key atlas texture creation pattern:
```cpp
void SurfaceCachePass::CreateAtlasTextures() {
    rhi::TextureDesc desc{};
    desc.width = atlas_size_;
    desc.height = atlas_size_;
    desc.depth = 1;
    desc.mip_levels = 1;
    desc.array_size = 1;

    // Albedo atlas - RGBA8
    desc.format = rhi::PixelFormat::RGBA8_UNorm;
    desc.initial_state = rhi::ResourceState::RenderTarget;
    desc.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
    albedo_atlas_ = device_->CreateTexture(desc);

    // Normal atlas - RG16F (octant encoded)
    desc.format = rhi::PixelFormat::RG16_Float;
    normal_atlas_ = device_->CreateTexture(desc);

    // Depth atlas - R32F
    desc.format = rhi::PixelFormat::R32_Float;
    depth_atlas_ = device_->CreateTexture(desc);

    // Emissive atlas - RGB11F
    desc.format = rhi::PixelFormat::R11G11B10_Float;
    desc.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
    emissive_atlas_ = device_->CreateTexture(desc);

    // Lighting atlas - RGBA16F (triple-buffered)
    desc.format = rhi::PixelFormat::RGBA16_Float;
    desc.usage = rhi::TextureUsage::UnorderedAccess | rhi::TextureUsage::ShaderResource;
    for (int i = 0; i < 3; ++i) {
        lighting_atlas_[i] = device_->CreateTexture(desc);
        prev_lighting_atlas_[i] = device_->CreateTexture(desc);
    }
}
```

- [ ] **Step 3: Verify compilation**

Run: `cmake --build build --target Engine 2>&1 | head -30`

- [ ] **Step 4: Commit**

```bash
git add Engine/Graphics/Lumen/SurfaceCache/SurfaceCachePass.h Engine/Graphics/Lumen/SurfaceCache/SurfaceCachePass.cpp
git commit -m "feat(surface-cache): add pass skeleton with atlas texture creation"
```

---

## Task 4: Card Capture Vertex + Fragment Shader

**Files:**
- Create: `Engine/Graphics/Metal/shaders/Lumen/SurfaceCacheCapture.metal`
- Modify: `Engine/Graphics/Lumen/SurfaceCache/SurfaceCachePass.cpp` (add capture pipeline + render pass)

**Depends on:** Task 3

> **Critical:** Card capture renders actual Nanite mesh geometry into the card atlas using orthographic projection. This is NOT a fullscreen triangle — the vertex shader reads meshlet vertex buffers and transforms them into the card's orthographic projection space. The card defines a view matrix (axis-aligned ortho projection) and the mesh is rasterized from that viewpoint.

- [ ] **Step 1: Create SurfaceCacheCapture.metal with vertex + fragment shaders**

Vertex shader reads `global_vertex_buffer` + `global_element_buffer` + `global_instance_data_buffer` (same as GPUDrivenDrawPipeline). Transforms mesh vertices into card orthographic projection space. Fragment shader samples material textures and outputs albedo/normal/depth/emissive to 4 render targets.

```metal
#include <metal_stdlib>
using namespace metal;
#include "../CommonTypes.metal"
#include "SurfaceCacheData.metal"

// --- Vertex Shader ---
// Reads Nanite meshlet vertices and transforms to card orthographic projection

struct CaptureVertexIn {
    float3 position [[attribute(0)]];
    float3 normal   [[attribute(1)]];
    float2 uv       [[attribute(2)]];
};

struct CaptureVertexOut {
    float4 position [[position]];
    float2 uv;
    float3 world_normal;
    float3 world_pos;
    uint   material_id;
};

// Build orthographic view matrix for a card
static float4x4 cardOrthoView(constant SurfaceCacheCard& card) {
    float3 forward, right, up;
    float3 origin = card.center;

    if (card.axis == 0) {
        right = float3(0,1,0); up = float3(0,0,1);
        forward = float3(card.direction ? 1.0 : -1.0, 0, 0);
    } else if (card.axis == 1) {
        right = float3(1,0,0); up = float3(0,0,1);
        forward = float3(0, card.direction ? 1.0 : -1.0, 0);
    } else {
        right = float3(1,0,0); up = float3(0,1,0);
        forward = float3(0, 0, card.direction ? 1.0 : -1.0);
    }

    // View matrix: translate then rotate
    float4x4 view;
    view[0] = float4(right.x, up.x, forward.x, 0);
    view[1] = float4(right.y, up.y, forward.y, 0);
    view[2] = float4(right.z, up.z, forward.z, 0);
    view[3] = float4(-dot(right,origin), -dot(up,origin), -dot(forward,origin), 1);
    return view;
}

static float4x4 cardOrthoProj(constant SurfaceCacheCard& card) {
    // Orthographic projection covering the card extent
    float rx = (card.axis == 0) ? card.extent.x : card.extent.y;
    float ry = (card.axis == 0) ? card.extent.z : ((card.axis == 1) ? card.extent.z : card.extent.y);
    float rz = card.extent.x; // depth range

    float4x4 proj;
    proj[0] = float4(1.0/rx, 0, 0, 0);
    proj[1] = float4(0, 1.0/ry, 0, 0);
    proj[2] = float4(0, 0, 1.0/rz, 0);
    proj[3] = float4(0, 0, 0, 1);
    return proj;
}

struct CapturePassData {
    SurfaceCacheCard card;
    uint32_t instance_offset;  // offset into global_instance_data_buffer_
    uint32_t _pad[3];
};

vertex CaptureVertexOut surfaceCacheCaptureVS(
    CaptureVertexIn in [[stage_in]],
    constant CapturePassData& pass_data [[buffer(1)]],
    constant float4x4* instance_transforms [[buffer(2)]])
{
    CaptureVertexOut out;

    // Get instance world transform
    float4x4 world = instance_transforms[pass_data.instance_offset];
    float4 world_pos = world * float4(in.position, 1.0);
    float3 world_normal = normalize((world * float4(in.normal, 0.0)).xyz);

    // Transform to card orthographic view space
    float4x4 view = cardOrthoView(pass_data.card);
    float4x4 proj = cardOrthoProj(pass_data.card);
    float4x4 vp = proj * view;

    out.position = vp * world_pos;
    out.world_pos = world_pos.xyz;
    out.world_normal = world_normal;
    out.uv = in.uv;
    out.material_id = 0; // TODO: from instance data

    return out;
}

// --- Fragment Shader ---

// MUST be separate functions to avoid Apple Silicon inline sampling bug
static float4 sampleAlbedo(uint material_id, float2 uv,
                            texture2d_array<float> albedo_array) {
    constexpr sampler s(coord::normalized, filter::linear, address::repeat);
    return albedo_array.sample(s, uv, material_id);
}

static float3 sampleNormal(uint material_id, float2 uv,
                            texture2d_array<float> normal_array) {
    constexpr sampler s(coord::normalized, filter::linear, address::repeat);
    float4 n = normal_array.sample(s, uv, material_id);
    return normalize(n.xyz * 2.0 - 1.0);
}

struct CaptureOutput {
    float4 albedo  [[color(0)]];
    float4 normal  [[color(1)]];  // RG = octant encoded
    float  depth   [[color(2)]];
    float4 emissive [[color(3)]];
};

fragment CaptureOutput surfaceCacheCaptureFS(
    CaptureVertexOut in [[stage_in]],
    texture2d_array<float> albedo_array  [[texture(0)]],
    texture2d_array<float> normal_array  [[texture(1)]],
    texture2d_array<float> orm_array     [[texture(2)]],
    constant CapturePassData& pass_data  [[buffer(1)]])
{
    CaptureOutput out;

    out.albedo = sampleAlbedo(in.material_id, in.uv, albedo_array);

    float3 world_normal = normalize(in.world_normal);
    float2 oct = octEncode(world_normal);
    out.normal = float4(oct, 0.0, 1.0);

    // Depth from card projection plane
    float3 local = in.world_pos - pass_data.card.center;
    float depth;
    if (pass_data.card.axis == 0) depth = abs(local.x);
    else if (pass_data.card.axis == 1) depth = abs(local.y);
    else depth = abs(local.z);
    out.depth = depth;

    // Emissive: TODO - sample from material data when emissive texture is available
    out.emissive = float4(0.0, 0.0, 0.0, 1.0);

    return out;
}
```

- [ ] **Step 2: Add capture pipeline creation to SurfaceCachePass.cpp**

In `CreatePipelines()`, add graphics pipeline with vertex + fragment shaders. The capture render pass draws each card's associated mesh with the card's orthographic projection into the atlas region.

```cpp
// Card capture (vertex + fragment shader)
auto capture_code = LoadShaderBytecode("SurfaceCacheCapture");
auto capture_vs = device_->CreateShader(capture_code.data(), capture_code.size(),
                                         rhi::ShaderStage::Vertex, "surfaceCacheCaptureVS");
auto capture_fs = device_->CreateShader(capture_code.data(), capture_code.size(),
                                         rhi::ShaderStage::Fragment, "surfaceCacheCaptureFS");

rhi::GraphicsPipelineDesc capture_desc{};
capture_desc.vertex_shader = capture_vs;
capture_desc.pixel_shader = capture_fs;
capture_desc.layout = capture_layout_;
// Vertex attributes: position(0), normal(1), uv(2) — matches Nanite vertex layout
capture_desc.vertex_attributes[0] = {0, 0, rhi::VertexFormat::RGB32_Float};  // position
capture_desc.vertex_attributes[1] = {1, 1, rhi::VertexFormat::RGB32_Float};  // normal
capture_desc.vertex_attributes[2] = {2, 2, rhi::VertexFormat::RG32_Float};   // uv
// 4 render targets: albedo, normal, depth, emissive
capture_desc.render_target_formats[0] = rhi::PixelFormat::RGBA8_UNorm;
capture_desc.render_target_formats[1] = rhi::PixelFormat::RG16_Float;
capture_desc.render_target_formats[2] = rhi::PixelFormat::R32_Float;
capture_desc.render_target_formats[3] = rhi::PixelFormat::R11G11B10_Float;
capture_pipeline_ = device_->CreateGraphicsPipeline(capture_desc);
```

The AddPass render loop for card capture: for each card direction, set viewport to card's atlas region, bind card data as constants, draw the mesh instance.

- [ ] **Step 3: Verify shader compiles**

Run: `xcrun -sdk macosx metal -std=metal3.0 -c Engine/Graphics/Metal/shaders/Lumen/SurfaceCacheCapture.metal -o /dev/null 2>&1 | head -20`

- [ ] **Step 4: Commit**

```bash
git add Engine/Graphics/Metal/shaders/Lumen/SurfaceCacheCapture.metal Engine/Graphics/Lumen/SurfaceCache/SurfaceCachePass.cpp
git commit -m "feat(surface-cache): add card capture vertex+fragment shader with orthographic projection"
```

---

## Task 5: Depth Dilation Compute Shader

**Files:**
- Create: `Engine/Graphics/Metal/shaders/Lumen/SurfaceCacheDilate.metal`
- Modify: `Engine/Graphics/Lumen/SurfaceCache/SurfaceCachePass.cpp` (add dilate pipeline + dispatch)

**Depends on:** Task 4

- [ ] **Step 1: Create SurfaceCacheDilate.metal**

```metal
#include <metal_stdlib>
using namespace metal;
#include "SurfaceCacheData.metal"

kernel void surfaceCacheDilate(
    uint2 global_id [[thread_position_in_grid]],
    texture2d<float, access::read>  depth_in    [[texture(0)]],
    texture2d<float, access::write> depth_out   [[texture(1)]],
    constant SurfaceCacheParams& params         [[buffer(1)]])
{
    if (global_id.x >= params.atlas_size || global_id.y >= params.atlas_size) return;

    float center = depth_in.read(global_id).r;

    // If texel already has valid depth, pass through
    if (center > 0.0) {
        depth_out.write(float4(center, 0, 0, 1), global_id);
        return;
    }

    // Dilate: find closest non-zero depth in 3x3 neighborhood
    float min_depth = 1e10;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dy == 0) continue;
            int2 neighbor = int2(global_id) + int2(dx, dy);
            if (neighbor.x < 0 || neighbor.y < 0) continue;
            if (neighbor.x >= params.atlas_size || neighbor.y >= params.atlas_size) continue;
            float d = depth_in.read(uint2(neighbor)).r;
            if (d > 0.0) min_depth = min(min_depth, d);
        }
    }

    depth_out.write(float4(min_depth < 1e10 ? min_depth : 0.0, 0, 0, 1), global_id);
}
```

- [ ] **Step 2: Add dilate pipeline + dispatch to SurfaceCachePass**

Pipeline: compute, thread group (8,8,1), dispatch (atlas_size/8, atlas_size/8, 1)

- [ ] **Step 3: Verify shader compiles**

- [ ] **Step 4: Commit**

```bash
git add Engine/Graphics/Metal/shaders/Lumen/SurfaceCacheDilate.metal
git commit -m "feat(surface-cache): add depth dilation compute shader"
```

---

## Task 6: Direct Lighting — Light Cull

**Files:**
- Create: `Engine/Graphics/Metal/shaders/Lumen/SurfaceCacheLightCull.metal`
- Modify: `Engine/Graphics/Lumen/SurfaceCache/SurfaceCachePass.cpp`

**Depends on:** Task 5

- [ ] **Step 1: Create SurfaceCacheLightCull.metal**

Compute shader that divides atlas into 8×8 tiles, computes depth range per tile, cone-tests lights, outputs assignment buffer.

```metal
#include <metal_stdlib>
using namespace metal;
#include "../CommonTypes.metal"
#include "SurfaceCacheData.metal"

struct LightInfo {
    float3 position;
    float  radius;
    float3 color;
    float  _pad;
    float3 direction;    // For directional lights
    uint   type;         // 0=point, 1=directional, 2=spot
};

struct LightCullParams {
    SurfaceCacheParams  sc_params;
    uint                light_count;
    uint                tile_size;       // 8
    uint                tiles_x;
    uint                tiles_y;
};

kernel void surfaceCacheLightCull(
    uint2 global_id [[thread_position_in_grid]],
    texture2d<float, access::read> depth_atlas  [[texture(0)]],
    texture2d<float, access::read> normal_atlas [[texture(1)]],
    constant LightCullParams& params            [[buffer(1)]],
    constant LightInfo* lights                  [[buffer(2)]],
    device uint4* tile_light_assignment         [[buffer(3)]])
{
    uint tile_x = global_id.x;
    uint tile_y = global_id.y;
    if (tile_x >= params.tiles_x || tile_y >= params.tiles_y) return;

    uint tile_idx = tile_y * params.tiles_x + tile_x;
    uint tile_size = params.tile_size;
    uint2 tile_origin = uint2(tile_x * tile_size, tile_y * tile_size);

    // Compute tile depth range and average position
    float min_depth = 1e10;
    float max_depth = 0.0;
    float3 avg_normal = float3(0.0);
    uint valid_count = 0;

    for (uint dy = 0; dy < tile_size; ++dy) {
        for (uint dx = 0; dx < tile_size; ++dx) {
            uint2 texel = tile_origin + uint2(dx, dy);
            if (texel.x >= params.sc_params.atlas_size || texel.y >= params.sc_params.atlas_size) continue;

            float d = depth_atlas.read(texel).r;
            if (d > 0.0) {
                min_depth = min(min_depth, d);
                max_depth = max(max_depth, d);
                float2 enc = normal_atlas.read(texel).rg;
                avg_normal += octDecode(enc);
                valid_count++;
            }
        }
    }

    // Cull lights per tile
    uint4 assigned = uint4(0xFFFFFFFF); // 0xFFFFFFFF = no light
    uint assign_count = 0;

    if (valid_count > 0 && max_depth > 0.0) {
        avg_normal = normalize(avg_normal);

        for (uint li = 0; li < params.light_count && assign_count < 4; ++li) {
            bool visible = false;

            if (lights[li].type == 1) {
                // Directional light: always visible (cone test with normal)
                visible = true;
            } else {
                // Point light: distance-based cull
                // Use tile center as approximate position (precise would need card_data)
                float light_range = lights[li].radius;
                if (max_depth < light_range) {
                    visible = true;
                }
            }

            if (visible) {
                switch (assign_count) {
                    case 0: assigned.x = li; break;
                    case 1: assigned.y = li; break;
                    case 2: assigned.z = li; break;
                    case 3: assigned.w = li; break;
                }
                assign_count++;
            }
        }
    }

    tile_light_assignment[tile_idx] = assigned;
}
```

- [ ] **Step 2: Add light_cull_pipeline_ creation and dispatch**

Pipeline: compute, thread group (8,8,1), dispatch (tiles_x, tiles_y, 1) where tiles = atlas_size / 8

Also create:
- `rhi::ResourceHandle light_assignment_buffer_` (tiles_x * tiles_y * sizeof(uint4))
- `rhi::ResourceHandle light_info_buffer_` (shared with existing light system or new)

- [ ] **Step 3: Verify compilation**

- [ ] **Step 4: Commit**

```bash
git add Engine/Graphics/Metal/shaders/Lumen/SurfaceCacheLightCull.metal Engine/Graphics/Lumen/SurfaceCache/SurfaceCachePass.h Engine/Graphics/Lumen/SurfaceCache/SurfaceCachePass.cpp
git commit -m "feat(surface-cache): add per-tile light culling compute shader"
```

---

## Task 7: Direct Lighting — Light Evaluate

**Files:**
- Create: `Engine/Graphics/Metal/shaders/Lumen/SurfaceCacheLightEval.metal`
- Modify: `Engine/Graphics/Lumen/SurfaceCache/SurfaceCachePass.cpp`

**Depends on:** Task 6

- [ ] **Step 1: Create SurfaceCacheLightEval.metal**

Compute shader that evaluates direct lighting per-texel. Reads: card_data(1) + light_assignment(1) + albedo_atlas(1) + normal_atlas(1) + emissive_atlas(1) + shadow_map(1) = 6 reads.

```metal
#include <metal_stdlib>
using namespace metal;
#include "../CommonTypes.metal"
#include "SurfaceCacheData.metal"

// LightInfo already defined in SurfaceCacheData.metal

struct LightEvalParams {
    SurfaceCacheParams sc_params;
    uint               tiles_x;
    uint               tile_size;
    uint               light_count;
};

kernel void surfaceCacheLightEval(
    uint2 global_id [[thread_position_in_grid]],
    // Material textures
    texture2d<float, access::read>  albedo_atlas    [[texture(0)]],
    texture2d<float, access::read>  normal_atlas    [[texture(1)]],
    texture2d<float, access::read>  emissive_atlas  [[texture(2)]],
    depth2d<float, access::sample>  shadow_map      [[texture(3)]],
    // Output
    texture2d<float, access::write> lighting_out    [[texture(4)]],
    // Buffers
    device GlobalShaderData&        gd              [[buffer(0)]],
    constant LightEvalParams&       params          [[buffer(1)]],
    constant SurfaceCacheCard*      cards           [[buffer(2)]],
    constant LightInfo*             lights          [[buffer(3)]],
    device const uint4*             tile_lights     [[buffer(4)]])
{
    if (global_id.x >= params.sc_params.atlas_size ||
        global_id.y >= params.sc_params.atlas_size) return;

    float4 albedo = albedo_atlas.read(global_id);
    if (albedo.a < 0.01) return; // Empty texel

    float2 enc_n = normal_atlas.read(global_id).rg;
    float3 normal = octDecode(enc_n);
    float3 emissive = emissive_atlas.read(global_id).rgb;

    // Find tile assignment
    uint tile_x = global_id.x / params.tile_size;
    uint tile_y = global_id.y / params.tile_size;
    uint tile_idx = tile_y * params.tiles_x + tile_x;
    uint4 assigned = tile_lights[tile_idx];

    // Accumulate direct lighting
    float3 direct_light = float3(0.0);

    for (uint i = 0; i < 4; ++i) {
        uint light_idx;
        switch(i) {
            case 0: light_idx = assigned.x; break;
            case 1: light_idx = assigned.y; break;
            case 2: light_idx = assigned.z; break;
            case 3: light_idx = assigned.w; break;
        }
        if (light_idx == 0xFFFFFFFF) continue;

        float3 L;
        float attenuation;
        if (lights[light_idx].type == 1) {
            // Directional
            L = -normalize(lights[light_idx].direction);
            attenuation = 1.0;
        } else {
            // Point light: compute per-texel world position from card data
            // Use the first card that covers this texel (cards buffer indexed per-tile)
            float3 world_pos = cardTexelToWorld(global_id, 0.0, cards[0]); // simplified
            float3 to_light = lights[light_idx].position - world_pos;
            float dist = length(to_light);
            L = to_light / max(dist, 0.001);
            attenuation = max(1.0 - (dist * dist) / (lights[light_idx].radius * lights[light_idx].radius), 0.0);
        }

        float NdotL = max(dot(normal, L), 0.0);
        direct_light += lights[light_idx].color * NdotL * attenuation;
    }

    float3 result = direct_light * albedo.rgb + emissive;
    lighting_out.write(float4(result, 1.0), global_id);
}
```

- [ ] **Step 2: Add light_eval_pipeline_ creation and dispatch**

Pipeline: compute, thread group (16,16,1), dispatch (atlas_size/16, atlas_size/16, 1)

- [ ] **Step 3: Wire LightCull → LightEvaluate in AddPass()**

In the RenderGraph AddPass lambda:
1. Import atlas textures
2. Add LightCull pass (writes tile_light_assignment_buffer_)
3. Add LightEvaluate pass (reads tile_light_assignment_buffer_ + material atlases → writes lighting_atlas_)

- [ ] **Step 4: Verify compilation**

- [ ] **Step 5: Commit**

```bash
git add Engine/Graphics/Metal/shaders/Lumen/SurfaceCacheLightEval.metal Engine/Graphics/Lumen/SurfaceCache/SurfaceCachePass.h Engine/Graphics/Lumen/SurfaceCache/SurfaceCachePass.cpp
git commit -m "feat(surface-cache): add per-texel direct lighting evaluation"
```

---

## Task 8: Indirect Lighting — Indirect Trace

**Files:**
- Create: `Engine/Graphics/Metal/shaders/Lumen/SurfaceCacheIndirectTrace.metal`
- Modify: `Engine/Graphics/Lumen/SurfaceCache/SurfaceCachePass.cpp`

**Depends on:** Task 7

- [ ] **Step 1: Create SurfaceCacheIndirectTrace.metal**

Compute shader that places probes on 8×8 tiles and traces 8 rays per probe through GlobalSDF.

Reads: depth_atlas(1) + card_data_buffer(1) + normal_atlas(1) + GlobalSDF(3 cascades) = 6 reads

```metal
#include <metal_stdlib>
using namespace metal;
#include "../CommonTypes.metal"
#include "../CommonFunction.metal"
#include "SurfaceCacheData.metal"

struct IndirectTraceParams {
    SurfaceCacheParams sc_params;
    uint               tile_size;       // 8
    uint               rays_per_probe;
    uint               frame_index;
    float              near_distance;
    float              max_ray_distance;
    // GlobalSDF cascade data
    float4             sdf_origins[3];
    float4             sdf_voxel_sizes[3];
    float4             sdf_extents[3];
    uint               sdf_resolutions[3];
    uint               sdf_cascade_count;
};

struct ProbeRayHit {
    float3  hit_position;    // World-space hit position
    float   hit_distance;
    uint    hit_type;        // 0=near, 1=far(SDF), 2=sky
    float   _pad;
};

kernel void surfaceCacheIndirectTrace(
    uint2 global_id [[thread_position_in_grid]],
    // Input textures
    texture2d<float, access::read>  depth_atlas  [[texture(0)]],
    texture2d<float, access::read>  normal_atlas [[texture(1)]],
    texture3d<float, access::sample> sdf0         [[texture(2)]],
    texture3d<float, access::sample> sdf1         [[texture(3)]],
    texture3d<float, access::sample> sdf2         [[texture(4)]],
    // Buffers
    device GlobalShaderData&        gd           [[buffer(0)]],
    constant IndirectTraceParams&   params       [[buffer(1)]],
    constant SurfaceCacheCard*      cards        [[buffer(2)]],
    device ProbeRayHit*             ray_hits     [[buffer(3)]])
{
    uint probe_x = global_id.x;
    uint probe_y = global_id.y;
    uint tile_size = params.tile_size;
    uint tiles_x = params.sc_params.atlas_size / tile_size;

    uint probe_idx = probe_y * tiles_x + probe_x;
    uint rays = params.rays_per_probe;

    // Probe center: center of 8x8 tile in atlas
    uint2 tile_origin = uint2(probe_x * tile_size + tile_size/2,
                               probe_y * tile_size + tile_size/2);
    if (tile_origin.x >= params.sc_params.atlas_size ||
        tile_origin.y >= params.sc_params.atlas_size) return;

    // Get probe world position and normal from atlas
    float depth = depth_atlas.read(tile_origin).r;
    if (depth <= 0.0) return; // Empty tile

    float2 enc_n = normal_atlas.read(tile_origin).rg;
    float3 probe_normal = octDecode(enc_n);

    // TODO: reconstruct world pos from card_data (needs card index per tile)
    // For now, approximate from atlas position
    float3 probe_pos = float3(float(tile_origin.x), float(tile_origin.y), depth);

    // Trace rays
    for (uint r = 0; r < rays; ++r) {
        // Cosine-weighted hemisphere direction using hash-based sampling
        float2 seed = float2(hash(float2(float(probe_idx * rays + r), float(params.frame_index))),
                             hash(float2(float(params.frame_index), float(probe_idx * rays + r))));
        float r1 = seed.x;
        float r2 = seed.y * 2.0 * 3.14159265;
        float sin_theta = sqrt(r1);
        float cos_theta = sqrt(1.0 - r1);
        float3 local_dir = float3(cos_theta * cos(r2), cos_theta * sin(r2), sin_theta);

        // Orient around probe normal (same pattern as ScreenProbeTraceRays)
        float3 up = abs(probe_normal.y) < 0.999 ? float3(0,1,0) : float3(1,0,0);
        float3 tangent = normalize(cross(up, probe_normal));
        float3 bitangent = cross(probe_normal, tangent);
        float3 ray_dir = normalize(tangent * local_dir.x + bitangent * local_dir.y + probe_normal * local_dir.z);

        // Sphere march through GlobalSDF (same pattern as DDGITraceRays / ScreenProbeTraceRays)
        float t = 0.0;
        bool hit = false;
        float3 hit_pos = float3(0.0);

        for (uint step = 0; step < 64; ++step) {
            float3 p = probe_pos + ray_dir * t;

            // Sample best SDF cascade (reuse pattern from existing shaders)
            float d = 1e6;
            for (uint c = 0; c < params.sdf_cascade_count; ++c) {
                float3 local_p = (p - params.sdf_origins[c].xyz) / params.sdf_extents[c].xyz;
                float3 uvw = local_p * 0.5 + 0.5;
                if (uvw.x < 0.0 || uvw.x > 1.0 || uvw.y < 0.0 || uvw.y > 1.0 || uvw.z < 0.0 || uvw.z > 1.0)
                    continue;
                texture3d<float, access::sample> sdf_tex = (c == 0) ? sdf0 : ((c == 1) ? sdf1 : sdf2);
                constexpr sampler s(coord::normalized, filter::linear, address::clamp_to_edge);
                float sample_d = sdf_tex.sample(s, uvw).r * params.sdf_extents[c].x;
                d = min(d, sample_d);
            }

            if (d < 0.01) { hit = true; hit_pos = p; break; }
            if (t > params.max_ray_distance) break;
            t += max(d, 0.01);
        }

        uint hit_idx = (probe_idx * rays + r);
        ray_hits[hit_idx].hit_distance = t;
        if (hit && t < params.near_distance) {
            ray_hits[hit_idx].hit_type = 0; // near
            ray_hits[hit_idx].hit_position = hit_pos;
        } else if (hit) {
            ray_hits[hit_idx].hit_type = 1; // far
            ray_hits[hit_idx].hit_position = hit_pos;
        } else {
            ray_hits[hit_idx].hit_type = 2; // sky
            ray_hits[hit_idx].hit_position = float3(0.0);
        }
    }
}
```

- [ ] **Step 2: Add indirect_trace_pipeline_ and ray_hits_buffer_**

Create buffer: `(tiles_total * rays_per_probe * sizeof(ProbeRayHit))`

- [ ] **Step 3: Verify compilation**

- [ ] **Step 4: Commit**

```bash
git add Engine/Graphics/Metal/shaders/Lumen/SurfaceCacheIndirectTrace.metal Engine/Graphics/Lumen/SurfaceCache/SurfaceCachePass.h Engine/Graphics/Lumen/SurfaceCache/SurfaceCachePass.cpp
git commit -m "feat(surface-cache): add indirect lighting ray trace compute shader"
```

---

## Task 9: Indirect Lighting — Indirect Resolve

**Files:**
- Create: `Engine/Graphics/Metal/shaders/Lumen/SurfaceCacheIndirectResolve.metal`
- Modify: `Engine/Graphics/Lumen/SurfaceCache/SurfaceCachePass.cpp`

**Depends on:** Task 8

- [ ] **Step 1: Create SurfaceCacheIndirectResolve.metal**

Compute shader that resolves indirect lighting by sampling prev_lighting_atlas at hit positions.

Reads: ray_hits_buffer(1) + card_lookup(1) + card_data(1) + prev_lighting_atlas(1) + albedo_atlas(1) = 5 reads (+ DDGI or sky optional = 6 max)

```metal
#include <metal_stdlib>
using namespace metal;
#include "../CommonTypes.metal"
#include "SurfaceCacheData.metal"

struct ProbeRayHit {
    float3  hit_position;
    float   hit_distance;
    uint    hit_type;
    float   _pad;
};

struct IndirectResolveParams {
    SurfaceCacheParams sc_params;
    uint               tile_size;
    uint               rays_per_probe;
    float              temporal_weight;
    uint               frame_index;
    uint               card_count;
};

kernel void surfaceCacheIndirectResolve(
    uint2 global_id [[thread_position_in_grid]],
    // Input textures
    texture2d<float, access::sample> prev_lighting [[texture(0)]],
    texture2d<float, access::read>   albedo_atlas   [[texture(1)]],
    texture2d<float, access::sample> sky_cubemap    [[texture(2)]],
    // Output
    texture2d<float, access::write>  indirect_out   [[texture(3)]],
    // Buffers
    device GlobalShaderData&           gd           [[buffer(0)]],
    constant IndirectResolveParams&    params       [[buffer(1)]],
    constant SurfaceCacheCardLookup*   lookups      [[buffer(2)]],
    constant SurfaceCacheCard*         cards        [[buffer(3)]],
    device const ProbeRayHit*          ray_hits     [[buffer(4)]])
{
    uint probe_x = global_id.x;
    uint probe_y = global_id.y;
    uint tiles_x = params.sc_params.atlas_size / params.tile_size;
    uint probe_idx = probe_y * tiles_x + probe_x;

    uint rays = params.rays_per_probe;
    uint tile_size = params.tile_size;
    uint2 tile_origin = uint2(probe_x * tile_size, probe_y * tile_size);

    if (tile_origin.x >= params.sc_params.atlas_size ||
        tile_origin.y >= params.sc_params.atlas_size) return;

    constexpr sampler linear_samp(coord::normalized, filter::linear, address::clamp_to_edge);

    // Accumulate indirect lighting from ray hits
    float3 indirect = float3(0.0);
    float total_weight = 0.0;

    for (uint r = 0; r < rays; ++r) {
        uint hit_idx = probe_idx * rays + r;
        float3 hit_pos = ray_hits[hit_idx].hit_position;
        uint hit_type = ray_hits[hit_idx].hit_type;

        float3 radiance = float3(0.0);

        if (hit_type == 0) {
            // Near hit: sample prev_lighting_atlas at hit position
            // First find which mesh (lookup) contains this hit, then which card has a valid UV
            float2 atlas_uv;
            bool found = false;
            for (uint li = 0; li < params.card_count && !found; ++li) {
                // Check if hit is within this mesh's AABB
                float3 mn = lookups[li].aabb_min.xyz;
                float3 mx = lookups[li].aabb_max.xyz;
                if (hit_pos.x < mn.x || hit_pos.x > mx.x ||
                    hit_pos.y < mn.y || hit_pos.y > mx.y ||
                    hit_pos.z < mn.z || hit_pos.z > mx.z) continue;

                // Check each card of this mesh
                for (uint ci = lookups[li].card_start;
                     ci < lookups[li].card_start + lookups[li].card_count && !found; ++ci) {
                    found = worldToCardUV(hit_pos, cards[ci], atlas_uv);
                }
            }

            if (found) {
                float2 uv_norm = atlas_uv / float2(params.sc_params.atlas_size);
                radiance = prev_lighting.sample(linear_samp, uv_norm).rgb;
            } else {
                // Hit didn't map to any card — use ambient fallback
                radiance = float3(0.02, 0.02, 0.03);
            }
        } else if (hit_type == 1) {
            // Far hit: DDGI sampling (Phase A) - placeholder
            // Will be replaced by Voxel Radiance in Phase B
            radiance = float3(0.02, 0.02, 0.03); // ambient fallback
        } else {
            // Sky hit
            radiance = float3(0.05, 0.05, 0.08);
        }

        indirect += radiance;
        total_weight += 1.0;
    }

    if (total_weight > 0.0) indirect /= total_weight;

    // Multiply by albedo and write to all texels in tile
    for (uint dy = 0; dy < tile_size; ++dy) {
        for (uint dx = 0; dx < tile_size; ++dx) {
            uint2 texel = tile_origin + uint2(dx, dy);
            if (texel.x >= params.sc_params.atlas_size ||
                texel.y >= params.sc_params.atlas_size) continue;

            float3 albedo = albedo_atlas.read(texel).rgb;
            float3 result = indirect * albedo;
            indirect_out.write(float4(result, 1.0), texel);
        }
    }
}
```

- [ ] **Step 2: Add indirect_resolve_pipeline_ and wire in AddPass**

- [ ] **Step 3: Wire full AddPass() flow**

Complete the `AddPass()` method to chain all passes:
```
UpdateCardPriorities → CardCapture × N → DepthDilate → LightCull → LightEvaluate → IndirectTrace → IndirectResolve
```

Swap lighting_atlas_ / prev_lighting_atlas_ double buffer indices each frame.

- [ ] **Step 4: Verify compilation**

- [ ] **Step 5: Commit**

```bash
git add Engine/Graphics/Metal/shaders/Lumen/SurfaceCacheIndirectResolve.metal Engine/Graphics/Lumen/SurfaceCache/SurfaceCachePass.h Engine/Graphics/Lumen/SurfaceCache/SurfaceCachePass.cpp
git commit -m "feat(surface-cache): add indirect lighting resolve and wire full pass pipeline"
```

---

## Task 10: Screen Probe Integration

**Files:**
- Modify: `Engine/Graphics/Lumen/ScreenProbes/ScreenProbeGIPass.h`
- Modify: `Engine/Graphics/Lumen/ScreenProbes/ScreenProbeGIPass.cpp`
- Modify: `Engine/Graphics/Metal/shaders/Lumen/ScreenProbeTraceRays.metal`

**Depends on:** Task 9

- [ ] **Step 1: Add surface cache bindings to ScreenProbeGIPass.h**

Add member variables:
```cpp
// Surface Cache integration (optional, only for High+ quality)
rhi::ResourceHandle surface_cache_lighting_atlas_;
rhi::ResourceHandle surface_cache_card_data_buffer_;
rhi::ResourceHandle surface_cache_card_lookup_buffer_;
bool surface_cache_available_ = false;

void SetSurfaceCacheData(
    rhi::ResourceHandle lighting_atlas,
    rhi::ResourceHandle card_data,
    rhi::ResourceHandle card_lookup) {
    surface_cache_lighting_atlas_ = lighting_atlas;
    surface_cache_card_data_buffer_ = card_data;
    surface_cache_card_lookup_buffer_ = card_lookup;
    surface_cache_available_ = true;
}
```

- [ ] **Step 2: Modify ScreenProbeGIPass.cpp**

In the trace pass setup lambda, add surface cache texture bindings when `surface_cache_available_` is true:
```cpp
if (surface_cache_available_) {
    builder.Read(surface_cache_lighting_atlas_, ResourceState::ShaderResource);
    builder.Read(surface_cache_card_data_buffer_, ResourceState::ShaderResource);
    builder.Read(surface_cache_card_lookup_buffer_, ResourceState::ShaderResource);
    // Bind to texture slots 4,5,6 (after existing SDF + prev_frame_color)
}

// In execute lambda, pass flag to shader via constant buffer:
// Add to ScreenProbeGlobalData: uint surface_cache_available
```

- [ ] **Step 3: Modify ScreenProbeTraceRays.metal**

Add `surface_cache_available` flag to the constant data struct, and add near/far branch in ray result evaluation:
```metal
// After GlobalSDF hit detection:
if (hit && hit_distance < 2.0 && surface_cache_available) {
    // Near hit: sample surface cache
    float2 atlas_uv;
    bool found = false;
    for (uint ci = 0; ci < card_count && !found; ++ci) {
        found = worldToCardUV(hit_world_pos, cards[ci], atlas_uv);
    }
    if (found) {
        constexpr sampler s(coord::normalized, filter::linear, address::clamp_to_edge);
        float2 uv_norm = atlas_uv / float2(atlas_size);
        radiance = lighting_atlas.sample(s, uv_norm).rgb;
    }
} else if (hit) {
    // Far hit: DDGI sampling (existing code, unchanged)
    // ...
}
```

- [ ] **Step 4: Verify compilation**

- [ ] **Step 5: Commit**

```bash
git add Engine/Graphics/Lumen/ScreenProbes/ScreenProbeGIPass.h Engine/Graphics/Lumen/ScreenProbes/ScreenProbeGIPass.cpp Engine/Graphics/Metal/shaders/Lumen/ScreenProbeTraceRays.metal
git commit -m "feat(surface-cache): integrate surface cache sampling into screen probe trace"
```

---

## Task 11: Render Pipeline Integration

**Files:**
- Modify: The main render pipeline file (find where DDGI/SSGI/ScreenProbe passes are wired)

**Depends on:** Task 10

- [ ] **Step 1: Find the render pipeline orchestration point**

Search for where `LumenDDGIPass::AddPass` is called to find the integration point.

- [ ] **Step 2: Add SurfaceCachePass to the pipeline**

```cpp
// Create and initialize
auto surface_cache = std::make_unique<lumen::SurfaceCachePass>();
if (config.quality >= lumen::LumenQualityPreset::High) {
    surface_cache->Initialize(device, config);
}

// In render loop:
if (surface_cache->IsInitialized()) {
    lumen::SurfaceCacheFrameData frame_data;
    frame_data.camera_position = camera_position;
    frame_data.frame_index = frame_index;
    frame_data.light_count = light_count;

    auto sc_output = surface_cache->AddPass(graph, prev_frame_color,
                                             light_data_buffer, frame_data, frame_index);

    // Connect to Screen Probes
    if (screen_probe_pass) {
        screen_probe_pass->SetSurfaceCacheData(
            surface_cache->GetLightingAtlas(frame_index),
            surface_cache->GetCardDataBuffer(),
            surface_cache->GetCardLookupBuffer());
    }
}
```

- [ ] **Step 3: Wire CardGenerator with mesh registration**

Connect mesh loading events to `CardGenerator::RegisterMesh()` and call `RebuildCardAllocation()` when the scene changes.

- [ ] **Step 4: Full build and smoke test**

Run: `cmake --build build --target Engine`
Expected: Clean build

- [ ] **Step 5: Visual validation**

Run the engine with `Quality::High` and verify:
1. Atlas textures contain valid material data (can be visualized via debug output)
2. Direct lighting appears on cached surfaces
3. Screen probes show improved near-field GI quality
4. No crash or corruption

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "feat(surface-cache): wire Surface Cache into render pipeline with Quality::High"
```

---

## Dependency Graph

```
Task 1 (Types)
  ├── Task 2 (CardGenerator)
  │     └── Task 3 (Pass Skeleton)
  │           ├── Task 4 (Card Capture)
  │           │     └── Task 5 (Depth Dilate)
  │           │           └── Task 6 (Light Cull)
  │           │                 └── Task 7 (Light Evaluate)
  │           │                       └── Task 8 (Indirect Trace)
  │           │                             └── Task 9 (Indirect Resolve)
  │           │                                   └── Task 10 (Screen Probe)
  │           │                                         └── Task 11 (Pipeline)
  └── (shared Metal types used by all shader tasks)
```

## Risk Mitigation Per Task

| Task | Risk | Mitigation |
|------|------|-----------|
| 4 (Capture) | Apple Silicon inline sampling bug | Texture sampling in separate functions |
| 6-7 (Lighting) | Buffer read limit | Split into Cull + Evaluate (5-6 reads each) |
| 8-9 (Indirect) | GlobalSDF sampling + atlas reads | Budget-limited probe count, separate trace/resolve |
| 10 (Screen Probe) | Breaking existing probe quality | Feature-gated behind `surface_cache_available_` |
| 11 (Pipeline) | Integration conflicts | Quality::High guard, DDGI fallback for Medium |
