# Nanite + Lumen 实现方案

**版本**: 1.0  
**日期**: 2026-03-03  
**作者**: Engine Team  
**状态**: Draft

---

## 目录

1. [概述](#1-概述)
2. [引擎现状分析](#2-引擎现状分析)
3. [技术架构设计](#3-技术架构设计)
4. [Phase 1: RHI 扩展与 GPU-Driven 基础](#4-phase-1-rhi-扩展与-gpu-driven-基础)
5. [Phase 2: 层级化 Cluster LOD](#5-phase-2-层级化-cluster-lod)
6. [Phase 3: Software Rasterization](#6-phase-3-software-rasterization)
7. [Phase 4: Global SDF](#7-phase-4-global-sdf)
8. [Phase 5: Radiance Probe Grid](#8-phase-5-radiance-probe-grid)
9. [Phase 6: Lumen Integration](#9-phase-6-lumen-integration)
10. [文件结构](#10-文件结构)
11. [测试策略](#11-测试策略)
12. [性能目标](#12-性能目标)
13. [风险评估](#13-风险评估)
14. [时间线](#14-时间线)

---

## 1. 概述

### 1.1 目标

在现有引擎中实现类似 Unreal Engine 5 的 **Nanite** (虚拟化几何体) 和 **Lumen** (动态全局光照) 系统。

### 1.2 核心特性

**Nanite 特性**:
- GPU-Driven 渲染管线
- 层级化 Cluster LOD 自动选择
- Software Rasterization 支持任意三角形密度
- Visibility Buffer 延迟材质
- 持续流式加载

**Lumen 特性**:
- 基于 SDF 的 Software Ray Tracing
- Radiance Probe Grid 动态 GI
- 多次反弹间接光照
- 屏幕空间 GI 补充

### 1.3 平台目标

| 平台 | 支持级别 | 备注 |
|------|---------|------|
| macOS (Apple Silicon) | 完全支持 | Mesh Shaders, Unified Memory |
| macOS (Intel) | 支持 | 无 Mesh Shaders, 使用 Compute |
| iOS | 完全支持 | Tile-Based 优化 |
| Windows (D3D12) | 未来 | 可选 Hardware RT |
| Windows (Vulkan) | 未来 | 可选 Hardware RT |

### 1.4 约束

- **无 Hardware Ray Tracing**: Metal 平台目前不支持 RTX/DXR 级别的硬件光线追踪
- **内存限制**: 需要高效的流式加载系统
- **兼容性**: 必须与现有渲染管线共存

---

## 2. 引擎现状分析

### 2.1 已有功能

| 模块 | 状态 | 可复用性 | 备注 |
|------|------|---------|------|
| Meshlets | ✅ 完整实现 | 高 | 基于 meshoptimizer |
| SDF | ⚠️ 仅调试 | 中 | 有数据结构，需扩展 |
| Light Probes | ✅ CPU Octree | 低 | 需要完全重写 |
| IBL | ✅ 完整实现 | 高 | Irradiance + Prefilter |
| Indirect Draw | ⚠️ 仅粒子 | 中 | 需要扩展到 DrawIndexedIndirect |
| GPU Culling | ⚠️ 仅光源/粒子 | 中 | 架构可复用 |
| Cluster Culling | ❌ 未实现 | - | 需要新建 |
| Ray Tracing | ❌ 未实现 | - | Software RT via SDF |

### 2.2 代码资产

```
可复用:
├── Engine/Graphics/RHI/Core/RHIGpuMesh.h        # Meshlet 数据结构
├── Engine/Graphics/RHI/Shaders/Debug/*.metal    # 调试着色器模式
├── third_party/meshoptimizer/                   # Cluster 生成
├── Engine/Graphics/Lighting/LightProbeManager.h # Probe 概念
└── ContentTools/Geometry.cpp                    # 几何处理

需要扩展:
├── Engine/Graphics/RHI/Core/RHICommand.h        # 添加 Indirect API
├── Engine/Graphics/RHI/Platforms/Metal/         # Metal 实现
└── ContentTools/pack_geometry.py                # Cluster hierarchy 输出
```

### 2.3 性能基线

当前 TestParticleSponza (Sponza 场景):
- 三角形数: ~100K
- Draw Calls: ~50 (CPU-driven)
- 帧率: 60 FPS (Metal, M1)
- 内存: ~200MB GPU

目标 (同样场景):
- 三角形数: ~1M (10x detail)
- Draw Calls: 1-2 (GPU-driven)
- 帧率: 60 FPS
- 内存: ~300MB GPU (with streaming)

---

## 3. 技术架构设计

### 3.1 整体架构

```
┌─────────────────────────────────────────────────────────────────────────┐
│                           Render Pipeline                                │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  ┌──────────────────────────────────────────────────────────────────┐   │
│  │                        Nanite Pipeline                            │   │
│  │  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────────┐   │   │
│  │  │ Cluster     │─→│ Software    │─→│ Material Pass           │   │   │
│  │  │ Culling     │  │ Rasterizer  │  │ (Vertex Pulling)        │   │   │
│  │  └─────────────┘  └─────────────┘  └─────────────────────────┘   │   │
│  │         │                │                       │                │   │
│  │         ▼                ▼                       ▼                │   │
│  │  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────────┐   │   │
│  │  │ Visible     │  │ Visibility  │  │ GBuffer                 │   │   │
│  │  │ Clusters    │  │ Buffer      │  │ (Albedo/Normal/Depth)   │   │   │
│  │  └─────────────┘  └─────────────┘  └─────────────────────────┘   │   │
│  └──────────────────────────────────────────────────────────────────┘   │
│                                    │                                     │
│                                    ▼                                     │
│  ┌──────────────────────────────────────────────────────────────────┐   │
│  │                        Lumen Pipeline                             │   │
│  │                                                                    │   │
│  │  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────────┐   │   │
│  │  │ Global SDF  │  │ Probe Grid  │  │ Indirect Lighting       │   │   │
│  │  │ Update      │  │ Trace       │  │ (SDF Ray Marching)      │   │   │
│  │  └─────────────┘  └─────────────┘  └─────────────────────────┘   │   │
│  │         │                │                       │                │   │
│  │         ▼                ▼                       ▼                │   │
│  │  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────────┐   │   │
│  │  │ Cascaded    │  │ Radiance    │  │ Indirect Color          │   │   │
│  │  │ SDF Textures│  │ Probes      │  │ (2-bounce GI)           │   │   │
│  │  └─────────────┘  └─────────────┘  └─────────────────────────┘   │   │
│  └──────────────────────────────────────────────────────────────────┘   │
│                                    │                                     │
│                                    ▼                                     │
│  ┌──────────────────────────────────────────────────────────────────┐   │
│  │                        Final Composition                         │   │
│  │         Direct Lighting + Indirect GI + Post Process             │   │
│  └──────────────────────────────────────────────────────────────────┘   │
│                                                                          │
└─────────────────────────────────────────────────────────────────────────┘
```

### 3.2 数据流

```
┌─────────────────────────────────────────────────────────────────────────┐
│                           Frame Data Flow                                │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  CPU Side:                                                               │
│  ┌──────────┐    ┌──────────────┐    ┌──────────────────────────┐      │
│  │ Scene    │───→│ Cluster      │───→│ GPU Buffers Upload       │      │
│  │ Graph    │    │ Hierarchy    │    │ (Streaming)              │      │
│  └──────────┘    └──────────────┘    └──────────────────────────┘      │
│        │                                                      │          │
│        │                  Content Pipeline                     │          │
│        │              (Offline/Background)                     │          │
│        ▼                                                      ▼          │
│  ┌──────────────────────────────────────────────────────────────────┐   │
│  │                         GPU Side                                   │   │
│  │                                                                    │   │
│  │  Frame Start                                                       │   │
│  │      │                                                             │   │
│  │      ▼                                                             │   │
│  │  ┌────────────────────────────────────────────────────────────┐   │   │
│  │  │ Pass 1: Global SDF Update (Compute)                        │   │   │
│  │  │         - Update cascades based on camera                  │   │   │
│  │  │         - Rasterize mesh SDFs to global SDF                │   │   │
│  │  └────────────────────────────────────────────────────────────┘   │   │
│  │      │                                                             │   │
│  │      ▼                                                             │   │
│  │  ┌────────────────────────────────────────────────────────────┐   │   │
│  │  │ Pass 2: Probe Grid Trace (Compute)                         │   │   │
│  │  │         - Trace subset of probes this frame                │   │   │
│  │  │         - Update radiance/depth textures                   │   │   │
│  │  └────────────────────────────────────────────────────────────┘   │   │
│  │      │                                                             │   │
│  │      ▼                                                             │   │
│  │  ┌────────────────────────────────────────────────────────────┐   │   │
│  │  │ Pass 3: Cluster Culling (Compute)                          │   │   │
│  │  │         - Frustum culling                                   │   │   │
│  │  │         - Backface culling (normal cone)                   │   │   │
│  │  │         - LOD selection (screen-space error)               │   │   │
│  │  │         - Occlusion culling (Hi-Z)                         │   │   │
│  │  └────────────────────────────────────────────────────────────┘   │   │
│  │      │                                                             │   │
│  │      ▼                                                             │   │
│  │  ┌────────────────────────────────────────────────────────────┐   │   │
│  │  │ Pass 4: Software Rasterization (Compute)                   │   │   │
│  │  │         - Visibility buffer rendering                       │   │   │
│  │  │         - Per-pixel atomic depth test                       │   │   │
│  │  └────────────────────────────────────────────────────────────┘   │   │
│  │      │                                                             │   │
│  │      ▼                                                             │   │
│  │  ┌────────────────────────────────────────────────────────────┐   │   │
│  │  │ Pass 5: Material Pass (Render)                             │   │   │
│  │  │         - Vertex pulling from visibility buffer            │   │   │
│  │  │         - GBuffer output                                    │   │   │
│  │  └────────────────────────────────────────────────────────────┘   │   │
│  │      │                                                             │   │
│  │      ▼                                                             │   │
│  │  ┌────────────────────────────────────────────────────────────┐   │   │
│  │  │ Pass 6: Direct Lighting (Compute/Render)                   │   │   │
│  │  │         - Shadow mapping                                    │   │   │
│  │  │         - PBR lighting                                      │   │   │
│  │  └────────────────────────────────────────────────────────────┘   │   │
│  │      │                                                             │   │
│  │      ▼                                                             │   │
│  │  ┌────────────────────────────────────────────────────────────┐   │   │
│  │  │ Pass 7: Indirect Lighting (Compute)                        │   │   │
│  │  │         - SDF ray marching                                  │   │   │
│  │  │         - Probe sampling                                    │   │   │
│  │  │         - 2-bounce GI                                       │   │   │
│  │  └────────────────────────────────────────────────────────────┘   │   │
│  │      │                                                             │   │
│  │      ▼                                                             │   │
│  │  ┌────────────────────────────────────────────────────────────┐   │   │
│  │  │ Pass 8: Composition (Render)                               │   │   │
│  │  │         - Combine direct + indirect                        │   │   │
│  │  │         - Post process                                      │   │   │
│  │  │         - UI overlay                                        │   │   │
│  │  └────────────────────────────────────────────────────────────┘   │   │
│  │      │                                                             │   │
│  │      ▼                                                             │   │
│  │  Present                                                            │   │
│  │                                                                    │   │
│  └──────────────────────────────────────────────────────────────────┘   │
│                                                                          │
└─────────────────────────────────────────────────────────────────────────┘
```

### 3.3 内存布局

```
┌─────────────────────────────────────────────────────────────────────────┐
│                           GPU Memory Layout                              │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  Geometry Pool (Persistent, Streamed):                                   │
│  ┌────────────────────────────────────────────────────────────────┐     │
│  │ Vertex Buffer      │ 256 MB │ Position (float3), compressed    │     │
│  │ Index Buffer       │ 64 MB  │ uint32 indices                    │     │
│  │ Cluster Buffer     │ 32 MB  │ RHICluster (64 bytes each)       │     │
│  │ Meshlet Buffer     │ 16 MB  │ Meshlet data                      │     │
│  │ Material Buffer    │ 8 MB   │ Material parameters               │     │
│  └────────────────────────────────────────────────────────────────┘     │
│                                                                          │
│  Per-Frame Transient:                                                    │
│  ┌────────────────────────────────────────────────────────────────┐     │
│  │ Visible Clusters   │ 4 MB   │ uint32 cluster indices           │     │
│  │ Indirect Args      │ 1 MB   │ Draw commands                     │     │
│  │ Visibility Buffer  │ 16 MB  │ 1920x1080 x 16 bytes             │     │
│  │ GBuffer            │ 32 MB  │ 4 x 1920x1080 RGBA16F            │     │
│  └────────────────────────────────────────────────────────────────┘     │
│                                                                          │
│  Lumen Resources:                                                        │
│  ┌────────────────────────────────────────────────────────────────┐     │
│  │ Global SDF LOD0    │ 8 MB   │ 512³ R16F                         │     │
│  │ Global SDF LOD1    │ 2 MB   │ 256³ R16F                         │     │
│  │ Global SDF LOD2    │ 0.5 MB │ 128³ R16F                         │     │
│  │ Probe Irradiance   │ 8 MB   │ 32³ x 6 faces RGBA16F            │     │
│  │ Probe Depth        │ 2 MB   │ 32³ x 6 faces RG16F              │     │
│  └────────────────────────────────────────────────────────────────┘     │
│                                                                          │
│  Total Estimate: ~450 MB GPU Memory                                      │
│                                                                          │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## 4. Phase 1: RHI 扩展与 GPU-Driven 基础

**持续时间**: 2 周  
**前置条件**: 无  
**交付物**: RHI 接口扩展, DrawIndexedIndirect, Cluster Culling 原型

### 4.1 RHI 接口扩展

#### 4.1.1 新增类型定义

```cpp
// Engine/Graphics/RHI/Core/RHITypes.h

namespace primal::graphics::rhi {

// === Cluster 数据结构 (64 bytes) ===
struct RHICluster {
    // Bounding volume (24 bytes)
    alignas(16) float bounds_min[3];
    alignas(4)  float _pad0;
    alignas(16) float bounds_max[3];
    alignas(4)  float _pad1;
    
    // Normal cone for backface culling (24 bytes)
    alignas(16) float cone_apex[3];
    alignas(4)  float _pad2;
    alignas(16) float cone_axis[3];
    alignas(4)  float cone_cutoff;
    
    // LOD hierarchy (16 bytes)
    float geometric_error;      // Screen-space error metric
    uint32_t parent_cluster;    // LOD parent (-1 if root)
    uint32_t child_start;       // First child index
    uint32_t child_count;       // Number of children
    
    // Mesh data references (16 bytes)
    uint32_t meshlet_index;     // Index into meshlet buffer
    uint32_t mesh_index;        // Index into mesh array
    uint32_t triangle_count;    // Triangles in this cluster
    uint32_t flags;             // Various flags
};

static_assert(sizeof(RHICluster) == 64, "RHICluster must be 64 bytes");

// === Visibility Buffer 格式 ===
struct VisibilityBufferData {
    uint32_t cluster_id;    // R: Cluster index
    uint32_t triangle_id;   // G: Triangle within cluster
    uint32_t barycentric;   // B: Packed barycentric (f16 x2)
    uint32_t depth;         // A: Depth as float bits
};

// === Indirect Draw Command ===
struct DrawIndexedIndirectCommand {
    uint32_t index_count;
    uint32_t instance_count;
    uint32_t first_index;
    int32_t  vertex_offset;
    uint32_t first_instance;
};

struct DrawIndirectCommand {
    uint32_t vertex_count;
    uint32_t instance_count;
    uint32_t first_vertex;
    uint32_t first_instance;
};

// === Hardware Feature Flags ===
enum class HardwareFeature : uint32_t {
    None            = 0,
    MeshShaders     = 1 << 0,   // Apple Silicon mesh shaders
    RayTracing      = 1 << 1,   // Hardware RT (not available on Metal yet)
    Atomics64       = 1 << 2,   // 64-bit atomics
    TileShading     = 1 << 3,   // Tile-based deferred rendering
    UnifiedMemory   = 1 << 4,   // CPU/GPU shared memory
};

}

// Enable bitwise operations
ENABLE_BITWISE_OPERATORS(HardwareFeature);
```

#### 4.1.2 RHICommand 扩展

```cpp
// Engine/Graphics/RHI/Core/RHICommand.h

class RHICommandBuffer {
public:
    // ... existing methods ...
    
    // === Indirect Drawing ===
    
    /**
     * @brief 非索引间接绘制
     * @param indirectBuffer 包含 DrawIndirectCommand 的缓冲区
     * @param offset 缓冲区偏移量
     * @param drawCount 绘制次数
     * @param stride 命令步长 (默认 sizeof(DrawIndirectCommand))
     */
    virtual void DrawIndirect(
        ResourceHandle indirectBuffer,
        uint64_t offset = 0,
        uint32_t drawCount = 1,
        uint32_t stride = sizeof(DrawIndirectCommand)
    ) = 0;
    
    /**
     * @brief 索引间接绘制
     * @param indirectBuffer 包含 DrawIndexedIndirectCommand 的缓冲区
     * @param offset 缓冲区偏移量
     * @param drawCount 绘制次数
     * @param stride 命令步长 (默认 sizeof(DrawIndexedIndirectCommand))
     */
    virtual void DrawIndexedIndirect(
        ResourceHandle indirectBuffer,
        uint64_t offset = 0,
        uint32_t drawCount = 1,
        uint32_t stride = sizeof(DrawIndexedIndirectCommand)
    ) = 0;
    
    // === Dispatch Indirect ===
    
    /**
     * @brief 间接计算分派
     * @param indirectBuffer 包含 DispatchIndirectCommand 的缓冲区
     * @param offset 缓冲区偏移量
     */
    virtual void DispatchIndirect(
        ResourceHandle indirectBuffer,
        uint64_t offset = 0
    ) = 0;
};
```

#### 4.1.3 Metal 实现

```cpp
// Engine/Graphics/RHI/Platforms/Metal/MetalCommandBuffer.cpp

void MetalCommandBuffer::DrawIndexedIndirect(
    ResourceHandle indirectBuffer,
    uint64_t offset,
    uint32_t drawCount,
    uint32_t stride)
{
    if (currentEncoderType_ != EncoderType::Render) {
        std::cerr << "DrawIndexedIndirect: Not in render pass!" << std::endl;
        return;
    }
    
    MetalDevice& metalDevice = static_cast<MetalDevice&>(device_);
    MetalBuffer* mtlBuffer = metalDevice.GetBuffer(indirectBuffer);
    
    if (!mtlBuffer || !mtlBuffer->GetNativeBuffer()) {
        std::cerr << "DrawIndexedIndirect: Invalid indirect buffer!" << std::endl;
        return;
    }
    
    if (!currentIndexBuffer_) {
        std::cerr << "DrawIndexedIndirect: No index buffer bound!" << std::endl;
        return;
    }
    
    MTL::RenderCommandEncoder* encoder = 
        static_cast<MTL::RenderCommandEncoder*>(currentEncoder_);
    MTL::Buffer* nativeBuffer = mtlBuffer->GetNativeBuffer();
    
    // Metal doesn't have native multi-draw, so we loop
    for (uint32_t i = 0; i < drawCount; ++i) {
        encoder->drawIndexedPrimitives(
            currentPrimitiveType_,
            MTL::Buffer*, offset + i * stride,
            currentIndexType_,
            currentIndexBuffer_,
            0  // index buffer offset
        );
    }
}

void MetalCommandBuffer::DispatchIndirect(
    ResourceHandle indirectBuffer,
    uint64_t offset)
{
    MTL::ComputeCommandEncoder* encoder = getComputeEncoder();
    if (!encoder) return;
    
    MetalDevice& metalDevice = static_cast<MetalDevice&>(device_);
    MetalBuffer* mtlBuffer = metalDevice.GetBuffer(indirectBuffer);
    
    if (!mtlBuffer || !mtlBuffer->GetNativeBuffer()) return;
    
    encoder->dispatchThreadgroups(
        mtlBuffer->GetNativeBuffer(),
        offset,
        MTL::Size(currentThreadGroupSize_, 1, 1)
    );
}
```

### 4.2 硬件特性检测

```cpp
// Engine/Graphics/RHI/Platforms/Metal/MetalDevice.cpp

HardwareFeature MetalDevice::GetSupportedFeatures() const {
    HardwareFeature features = HardwareFeature::None;
    
    // Mesh Shaders: Apple7 family (M1+)
    if ([m_device_ supportsFamily:MTLGPUFamilyApple7]) {
        features |= HardwareFeature::MeshShaders;
    }
    
    // Ray Tracing: Apple9 family (M3+) - but we use software path
    // Note: Enable when we implement hardware RT fallback
    // if ([m_device_ supportsFamily:MTLGPUFamilyApple9]) {
    //     features |= HardwareFeature::RayTracing;
    // }
    
    // 64-bit Atomics: Always supported on Apple Silicon
    if ([m_device_ supportsFamily:MTLGPUFamilyApple1]) {
        features |= HardwareFeature::Atomics64;
    }
    
    // Tile Shading: Apple2+ supports imageblocks
    if ([m_device_ supportsFamily:MTLGPUFamilyApple2]) {
        features |= HardwareFeature::TileShading;
    }
    
    // Unified Memory: Apple Silicon always has this
    if ([m_device_ hasUnifiedMemory]) {
        features |= HardwareFeature::UnifiedMemory;
    }
    
    return features;
}

bool MetalDevice::SupportsMeshShaders() const {
    return (GetSupportedFeatures() & HardwareFeature::MeshShaders) != HardwareFeature::None;
}

bool MetalDevice::SupportsRayTracing() const {
    // We intentionally return false to use software path
    // This can be changed when hardware RT is implemented
    return false;
}
```

### 4.3 Cluster Culling Pass

#### 4.3.1 C++ 接口

```cpp
// Engine/Graphics/Nanite/ClusterCullingPass.h

#pragma once

#include "Graphics/RHI/Core/RHITypes.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include <memory>

namespace primal::graphics::nanite {

class ClusterCullingPass {
public:
    struct Config {
        uint32_t max_clusters_per_frame = 100000;
        uint32_t thread_group_size = 64;
        bool enable_occlusion_culling = true;
        bool enable_backface_culling = true;
        float screen_space_error_threshold = 1.0f;
    };
    
    struct CullingResults {
        BufferHandle visible_clusters_buffer;
        BufferHandle draw_commands_buffer;
        BufferHandle counter_buffer;
        uint32_t visible_cluster_count;
    };
    
    ClusterCullingPass() = default;
    ~ClusterCullingPass();
    
    // Non-copyable, movable
    ClusterCullingPass(const ClusterCullingPass&) = delete;
    ClusterCullingPass& operator=(const ClusterCullingPass&) = delete;
    ClusterCullingPass(ClusterCullingPass&&) noexcept;
    ClusterCullingPass& operator=(ClusterCullingPass&&) noexcept;
    
    /**
     * @brief 初始化 culling pass
     */
    bool Initialize(RHIDeviceBase* device, const Config& config);
    
    /**
     * @brief 执行 cluster culling
     * @param cmd 命令缓冲区
     * @param clusterBuffer 包含所有 cluster 数据的缓冲区
     * @param clusterCount cluster 数量
     * @param view 渲染视图
     * @param hizTexture Hi-Z 纹理 (可选，用于遮挡剔除)
     */
    void Execute(
        RHICommandBuffer* cmd,
        BufferHandle clusterBuffer,
        uint32_t clusterCount,
        const RenderView& view,
        TextureHandle hizTexture = handles::INVALID_RESOURCE
    );
    
    /**
     * @brief 获取 culling 结果
     */
    const CullingResults& GetResults() const { return results_; }
    
    /**
     * @brief 重置计数器 (每帧开始时调用)
     */
    void ResetCounters(RHICommandBuffer* cmd);
    
private:
    RHIDeviceBase* device_ = nullptr;
    Config config_;
    CullingResults results_;
    
    PipelineHandle culling_pipeline_;
    PipelineLayoutHandle pipeline_layout_;
    
    BufferHandle uniform_buffer_;
    DescriptorSetHandle descriptor_set_;
    
    bool InitializePipelines();
    bool InitializeBuffers();
};

}
```

#### 4.3.2 实现

```cpp
// Engine/Graphics/Nanite/ClusterCullingPass.cpp

#include "ClusterCullingPass.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include <cstring>

namespace primal::graphics::nanite {

// Uniform buffer structure for culling shader
struct CullingUniforms {
    math::m4x4 view_projection;
    math::m4x4 view;
    math::v3 camera_position;
    uint32_t total_clusters;
    
    float screen_space_error_threshold;
    float lod_distance_scale;
    uint32_t frame_index;
    uint32_t _pad;
    
    uint32_t enable_occlusion;
    uint32_t enable_backface;
    uint32_t _pad1[2];
};

bool ClusterCullingPass::Initialize(RHIDeviceBase* device, const Config& config) {
    device_ = device;
    config_ = config;
    
    if (!InitializePipelines()) return false;
    if (!InitializeBuffers()) return false;
    
    return true;
}

bool ClusterCullingPass::InitializePipelines() {
    // Load compute shader
    // Note: In production, this would use shader compilation system
    ShaderHandle shader = device_->CreateShader(
        cluster_culling_shader_bytecode,
        sizeof(cluster_culling_shader_bytecode),
        ShaderStage::Compute,
        "cluster_culling"
    );
    
    if (shader == handles::INVALID_SHADER) return false;
    
    // Create descriptor set layout
    DescriptorSetLayoutBinding bindings[] = {
        { 0, DescriptorType::StorageBuffer, 1, ShaderStage::Compute },  // Clusters in
        { 1, DescriptorType::UniformBuffer, 1, ShaderStage::Compute },  // Uniforms
        { 2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute },  // Visible out
        { 3, DescriptorType::StorageBuffer, 1, ShaderStage::Compute },  // Counter
        { 4, DescriptorType::SampledImage, 1, ShaderStage::Compute },   // Hi-Z (optional)
    };
    
    DescriptorSetLayoutDesc layoutDesc = {
        .bindings = bindings,
        .bindingCount = 5
    };
    
    DescriptorSetLayoutHandle setLayout = device_->CreateDescriptorSetLayout(layoutDesc);
    
    // Create pipeline layout
    PipelineLayoutDesc plDesc = {
        .setLayoutCount = 1,
        .setLayouts = &setLayout
    };
    
    pipeline_layout_ = device_->CreatePipelineLayout(plDesc);
    
    // Create compute pipeline
    ComputePipelineDesc pipelineDesc = {
        .layout = pipeline_layout_,
        .computeShader = shader
    };
    
    culling_pipeline_ = device_->CreateComputePipeline(pipelineDesc);
    
    return culling_pipeline_ != handles::INVALID_PIPELINE;
}

bool ClusterCullingPass::InitializeBuffers() {
    // Create uniform buffer
    BufferDesc uniformDesc = {
        .size = sizeof(CullingUniforms),
        .usage = GPUMemoryUsage::Dynamic,
        .bindFlags = (uint32_t)BufferUsageFlags::Uniform,
        .name = "CullingUniforms"
    };
    uniform_buffer_ = device_->CreateBuffer(uniformDesc);
    
    // Create visible clusters buffer
    BufferDesc visibleDesc = {
        .size = config_.max_clusters_per_frame * sizeof(uint32_t),
        .usage = GPUMemoryUsage::Static,
        .bindFlags = (uint32_t)BufferUsageFlags::Storage,
        .name = "VisibleClusters"
    };
    results_.visible_clusters_buffer = device_->CreateBuffer(visibleDesc);
    
    // Create counter buffer (atomic uint + draw command)
    BufferDesc counterDesc = {
        .size = sizeof(uint32_t) * 4,  // counter + 3 padding
        .usage = GPUMemoryUsage::Static,
        .bindFlags = (uint32_t)BufferUsageFlags::Storage | (uint32_t)BufferUsageFlags::Indirect,
        .name = "CullingCounter"
    };
    results_.counter_buffer = device_->CreateBuffer(counterDesc);
    
    return uniform_buffer_ != handles::INVALID_RESOURCE &&
           results_.visible_clusters_buffer != handles::INVALID_RESOURCE &&
           results_.counter_buffer != handles::INVALID_RESOURCE;
}

void ClusterCullingPass::ResetCounters(RHICommandBuffer* cmd) {
    // Reset counter to 0
    uint32_t zero = 0;
    cmd->UpdateBuffer(results_.counter_buffer, 0, sizeof(zero), &zero);
}

void ClusterCullingPass::Execute(
    RHICommandBuffer* cmd,
    BufferHandle clusterBuffer,
    uint32_t clusterCount,
    const RenderView& view,
    TextureHandle hizTexture)
{
    // Update uniforms
    CullingUniforms uniforms;
    uniforms.view_projection = view.viewProjection;
    uniforms.view = view.view;
    uniforms.camera_position = view.cameraPosition;
    uniforms.total_clusters = clusterCount;
    uniforms.screen_space_error_threshold = config_.screen_space_error_threshold;
    uniforms.lod_distance_scale = 1.0f;
    uniforms.frame_index = view.frameIndex;
    uniforms.enable_occlusion = config_.enable_occlusion_culling && hizTexture != handles::INVALID_RESOURCE ? 1 : 0;
    uniforms.enable_backface = config_.enable_backface_culling ? 1 : 0;
    
    void* mapped = device_->MapBuffer(uniform_buffer_);
    if (mapped) {
        memcpy(mapped, &uniforms, sizeof(uniforms));
        device_->UnmapBuffer(uniform_buffer_);
    }
    
    // Bind pipeline
    cmd->BindComputePipeline(culling_pipeline_);
    
    // Bind descriptor set
    // (In production, would use proper descriptor set management)
    cmd->BindDescriptorSets(
        PipelineType::Compute,
        pipeline_layout_,
        0, 1, &descriptor_set_,
        0, nullptr
    );
    
    // Dispatch
    uint32_t threadGroups = (clusterCount + config_.thread_group_size - 1) / config_.thread_group_size;
    cmd->Dispatch(threadGroups, 1, 1);
    
    // Memory barrier to ensure culling is complete before indirect draw
    MemoryBarrier barrier = {
        .srcAccess = AccessFlags::ShaderWrite,
        .dstAccess = AccessFlags::IndirectCommandRead,
        .srcStage = PipelineStage::ComputeShader,
        .dstStage = PipelineStage::DrawIndirect
    };
    cmd->PipelineBarrier(1, &barrier, 0, nullptr, 0, nullptr);
}

}
```

### 4.4 交付清单

| 项目 | 状态 | 说明 |
|------|------|------|
| RHICluster 结构 | 📋 待实现 | 64 字节对齐的 cluster 数据 |
| DrawIndexedIndirect API | 📋 待实现 | RHI + Metal 实现 |
| Hardware Feature Query | 📋 待实现 | 特性检测接口 |
| Cluster Culling Shader | 📋 待实现 | Metal compute shader |
| ClusterCullingPass 类 | 📋 待实现 | C++ 封装类 |
| 单元测试 | 📋 待实现 | API 功能测试 |

---

## 5. Phase 2: 层级化 Cluster LOD

**持续时间**: 3 周  
**前置条件**: Phase 1 完成  
**交付物**: Cluster hierarchy 生成工具, LOD selection shader, 扩展的 .model 格式

### 5.1 Cluster Hierarchy 数据结构

```cpp
// Engine/Graphics/RHI/Core/RHIClusterHierarchy.h

#pragma once

#include "RHITypes.h"
#include <vector>

namespace primal::graphics::rhi {

/**
 * @brief Cluster hierarchy 用于 LOD 选择
 * 
 * 结构说明:
 * - 每个 cluster 有一个 parent (更高 LOD) 和多个 children (更低 LOD)
 * - LOD 0 是最高细节，LOD N 是最低细节
 * - 每个 LOD level 中的 clusters 覆盖相同的表面区域
 */
struct ClusterHierarchy {
    // 所有 cluster 数据
    std::vector<RHICluster> clusters;
    
    // LOD level 信息
    struct LODLevel {
        uint32_t cluster_start;     // 第一个 cluster 的索引
        uint32_t cluster_count;     // 该 LOD 的 cluster 数量
        float geometric_error_scale; // 相对于上一级的几何误差
    };
    std::vector<LODLevel> lod_levels;
    
    // Meshlet 数据
    std::vector<Meshlet> meshlets;
    std::vector<uint32_t> meshlet_vertices;
    std::vector<uint8_t> meshlet_triangles;
    
    // 统计信息
    uint32_t total_triangles;
    uint32_t max_lod_level;
    
    bool IsValid() const {
        return !clusters.empty() && !lod_levels.empty();
    }
    
    uint32_t GetClusterCount() const { return (uint32_t)clusters.size(); }
    uint32_t GetLODCount() const { return (uint32_t)lod_levels.size(); }
};

/**
 * @brief 单个 mesh 的完整 GPU 数据
 */
struct RHIClusterMesh {
    // Geometry
    BufferHandle vertex_buffer;
    BufferHandle index_buffer;
    
    // Clusters
    BufferHandle cluster_buffer;
    BufferHandle meshlet_buffer;
    BufferHandle meshlet_vertex_buffer;
    BufferHandle meshlet_triangle_buffer;
    
    // LOD info
    uint32_t cluster_count;
    uint32_t lod_count;
    uint32_t lod_offsets[8];  // 每个 LOD 的起始 cluster 索引
    
    // Bounds
    math::v3 bounds_min;
    math::v3 bounds_max;
    
    bool IsValid() const {
        return cluster_buffer != handles::INVALID_RESOURCE;
    }
};

}
```

### 5.2 Cluster Hierarchy 生成器

```cpp
// ContentTools/ClusterHierarchyBuilder.h

#pragma once

#include "Geometry.h"
#include <vector>
#include <memory>

namespace primal::content {

struct ClusterHierarchyBuildConfig {
    // Cluster 设置
    uint32_t max_triangles_per_cluster = 128;
    uint32_t max_vertices_per_meshlet = 64;
    
    // LOD 设置
    uint32_t max_lod_levels = 6;
    float lod_simplify_ratio = 0.5f;       // 每个 LOD 的简化比例
    float geometric_error_multiplier = 1.0f;
    
    // 质量
    bool use_normal_cone_culling = true;
    float normal_cone_weight = 1.0f;
};

class ClusterHierarchyBuilder {
public:
    struct BuildResult {
        rhi::ClusterHierarchy hierarchy;
        
        // 中间数据 (用于调试)
        std::vector<std::vector<uint32_t>> lod_cluster_groups;
        std::vector<float> cluster_errors;
    };
    
    /**
     * @brief 从源 mesh 构建层级化 cluster
     */
    static BuildResult Build(
        const Geometry& sourceGeometry,
        const ClusterHierarchyBuildConfig& config
    );
    
private:
    // LOD 生成
    static Geometry SimplifyGeometry(
        const Geometry& source,
        float ratio,
        uint32_t targetTriangleCount
    );
    
    // Cluster 生成
    static std::vector<Meshlet> GenerateClusters(
        const Geometry& geometry,
        uint32_t maxTriangles
    );
    
    // 计算 cluster 属性
    static void ComputeClusterBounds(
        rhi::RHICluster& cluster,
        const Meshlet& meshlet,
        const Geometry& geometry
    );
    
    static void ComputeClusterNormalCone(
        rhi::RHICluster& cluster,
        const Meshlet& meshlet,
        const Geometry& geometry
    );
    
    // 层级构建
    static void BuildParentChildRelationships(
        BuildResult& result,
        const std::vector<Geometry>& lodGeometries,
        const ClusterHierarchyBuildConfig& config
    );
    
    static uint32_t FindParentCluster(
        const rhi::RHICluster& childCluster,
        const std::vector<rhi::RHICluster>& parentClusters,
        const Geometry& parentGeometry
    );
    
    static float ComputeGeometricError(
        const rhi::RHICluster& childCluster,
        const rhi::RHICluster& parentCluster
    );
};

}
```

### 5.3 LOD Selection Shader

```metal
// Engine/Graphics/Nanite/Shaders/lod_selection.metal

#include <metal_stdlib>
using namespace metal;

// Append to cluster_culling.metal

/**
 * @brief 计算屏幕空间误差
 */
float ComputeScreenSpaceError(
    constant RHICluster& cluster,
    float3 cameraPosition,
    float screenSpaceThreshold)
{
    // Cluster 中心到相机的距离
    float3 center = (float3(cluster.bounds_min) + float3(cluster.bounds_max)) * 0.5;
    float distance = length(center - cameraPosition);
    
    // Cluster 的屏幕空间大小
    float3 extent = float3(cluster.bounds_max) - float3(cluster.bounds_min);
    float radius = length(extent) * 0.5;
    
    // 屏幕空间投影大小 (简化: 假设 FOV 90 度)
    float screenSize = radius / max(distance, 0.001);
    
    // 屏幕空间误差 = 屏幕大小 * 几何误差
    float screenError = screenSize * cluster.geometric_error;
    
    return screenError;
}

/**
 * @brief 选择合适的 LOD level
 * @return true 如果应该使用这个 cluster, false 如果应该使用 parent
 */
bool ShouldUseThisLOD(
    constant RHICluster& cluster,
    constant RHICluster* allClusters,
    float3 cameraPosition,
    float screenSpaceThreshold)
{
    // 如果没有几何误差 (LOD 0), 总是使用
    if (cluster.geometric_error <= 0.0) {
        return true;
    }
    
    // 计算屏幕空间误差
    float screenError = ComputeScreenSpaceError(cluster, cameraPosition, screenSpaceThreshold);
    
    // 如果误差太大, 应该使用更详细的 LOD (children)
    // 注意: 这个函数是从 LOD 0 到 LOD N 遍历的
    // 所以如果误差太大, 我们不选择这个 cluster, 让 child 来处理
    
    if (screenError > screenSpaceThreshold) {
        return false;  // 需要更详细的 LOD
    }
    
    // 检查 parent 是否更合适
    if (cluster.parent_cluster != 0xFFFFFFFF) {
        constant RHICluster& parent = allClusters[cluster.parent_cluster];
        float parentScreenError = ComputeScreenSpaceError(parent, cameraPosition, screenSpaceThreshold);
        
        // 如果 parent 的误差也在阈值内, 使用 parent
        if (parentScreenError < screenSpaceThreshold) {
            return false;  // 让 parent 来处理
        }
    }
    
    return true;  // 这个 LOD 正好
}

// 修改后的 culling kernel
kernel void cluster_culling_with_lod(
    constant RHICluster* clusters [[buffer(0)]],
    constant CullingUniforms& uniforms [[buffer(1)]],
    constant uint32_t* lodOffsets [[buffer(2)]],  // 每个 LOD 的起始索引
    constant uint32_t& lodCount [[buffer(3)]],
    
    device uint32_t* visible_clusters [[buffer(4)]],
    device atomic_uint& visible_count [[buffer(5)]],
    
    uint3 gid [[thread_position_in_grid]])
{
    uint32_t clusterIdx = gid.x;
    if (clusterIdx >= uniforms.total_clusters) return;
    
    constant RHICluster& cluster = clusters[clusterIdx];
    
    // === LOD Selection (先于其他 culling, 节省计算) ===
    if (!ShouldUseThisLOD(cluster, clusters, uniforms.camera_position, 
                          uniforms.screen_space_error_threshold)) {
        return;
    }
    
    // === Frustum Culling ===
    float3 center = (float3(cluster.bounds_min) + float3(cluster.bounds_max)) * 0.5;
    float3 halfExtent = float3(cluster.bounds_max) - center;
    float radius = length(halfExtent);
    
    float4 clipPos = uniforms.view_projection * float4(center, 1.0);
    if (!SphereInFrustum(clipPos, radius)) return;
    
    // === Backface Culling ===
    if (uniforms.enable_backface != 0) {
        float3 viewDir = normalize(uniforms.camera_position - center);
        float coneDot = dot(float3(cluster.cone_axis), viewDir);
        if (coneDot > cluster.cone_cutoff) return;
    }
    
    // === Visible ===
    uint32_t drawIdx = atomic_fetch_add_explicit(&visible_count, 1, memory_order_relaxed);
    visible_clusters[drawIdx] = clusterIdx;
}
```

### 5.4 扩展的 .model 文件格式

```
.model 文件格式 v2 (支持 cluster hierarchy):

Header (64 bytes):
┌────────────────────────────────────────────────────────────────┐
│ Magic: "MDLV" (4 bytes)                                        │
│ Version: uint32 (4 bytes) - 当前版本 2                         │
│ Flags: uint32 (4 bytes)                                        │
│ Vertex Count: uint32                                           │
│ Index Count: uint32                                            │
│ Meshlet Count: uint32                                          │
│ Cluster Count: uint32                                          │
│ LOD Count: uint32                                              │
│ Reserved: 32 bytes                                             │
└────────────────────────────────────────────────────────────────┘

LOD Table (8 bytes per LOD):
┌────────────────────────────────────────────────────────────────┐
│ Cluster Start: uint32                                          │
│ Cluster Count: uint32                                          │
└────────────────────────────────────────────────────────────────┘

Data Sections (each preceded by size and offset):
┌────────────────────────────────────────────────────────────────┐
│ Vertex Data: float3 positions (may be compressed)              │
│ Index Data: uint32 indices                                     │
│ Meshlet Data: Meshlet structures                               │
│ Meshlet Vertices: uint32 vertex indices                        │
│ Meshlet Triangles: uint8 triangle indices                      │
│ Cluster Data: RHICluster structures                            │
│ SDF Data: R16_Float 3D texture data (existing)                 │
└────────────────────────────────────────────────────────────────┘
```

### 5.5 交付清单

| 项目 | 状态 | 说明 |
|------|------|------|
| ClusterHierarchy 结构 | 📋 待实现 | 数据结构定义 |
| ClusterHierarchyBuilder | 📋 待实现 | C++ 生成工具 |
| LOD Selection Shader | 📋 待实现 | Metal compute shader |
| .model 格式扩展 | 📋 待实现 | 文件格式更新 |
| ContentTools 集成 | 📋 待实现 | 构建管线集成 |
| 测试场景 | 📋 待实现 | 多 LOD 测试模型 |

---

## 6. Phase 3: Software Rasterization

**持续时间**: 3 周  
**前置条件**: Phase 1, 2 完成  
**交付物**: Visibility Buffer, Software Rasterizer Shader, Material Pass

### 6.1 Visibility Buffer 设计

```cpp
// Engine/Graphics/Nanite/VisibilityBuffer.h

#pragma once

#include "Graphics/RHI/Core/RHITypes.h"

namespace primal::graphics::nanite {

/**
 * @brief Visibility Buffer 格式
 * 
 * 内存布局 (16 bytes per pixel):
 * - R32: Cluster ID
 * - G32: Triangle ID (within cluster)
 * - B32: Barycentric coordinates (packed: X = high 16 bits, Y = low 16 bits)
 * - A32: Depth (as uint32, reinterpret from float)
 */
struct VisibilityBuffer {
    TextureHandle texture;
    
    // 分辨率
    uint32_t width;
    uint32_t height;
    
    // 用于 clear
    void Clear(RHICommandBuffer* cmd);
    
    // 创建
    static VisibilityBuffer Create(RHIDeviceBase* device, uint32_t width, uint32_t height);
};

// 解码函数
inline void DecodeVisibilityBuffer(
    uint32_t packed_r, uint32_t packed_g, uint32_t packed_b, uint32_t packed_a,
    uint32_t& out_cluster_id,
    uint32_t& out_triangle_id,
    float& out_bary_x,
    float& out_bary_y,
    float& out_depth)
{
    out_cluster_id = packed_r;
    out_triangle_id = packed_g;
    out_bary_x = float(packed_b >> 16) / 65535.0f;
    out_bary_y = float(packed_b & 0xFFFF) / 65535.0f;
    out_depth = *reinterpret_cast<const float*>(&packed_a);
}

// 编码函数
inline void EncodeVisibilityBuffer(
    uint32_t cluster_id, uint32_t triangle_id, float bary_x, float bary_y, float depth,
    uint32_t& out_r, uint32_t& out_g, uint32_t& out_b, uint32_t& out_a)
{
    out_r = cluster_id;
    out_g = triangle_id;
    out_b = (uint32_t(bary_x * 65535.0f) << 16) | uint32_t(bary_y * 65535.0f);
    out_a = *reinterpret_cast<const uint32_t*>(&depth);
}

}
```

### 6.2 Software Rasterizer Shader

详见 [Phase 3 详细实现](#phase-3-software-rasterization-1) 中的 Metal shader 代码。

### 6.3 Material Pass

```metal
// Engine/Graphics/Nanite/Shaders/material_pass.metal

#include <metal_stdlib>
using namespace metal;

// Material pass 从 Visibility Buffer 重建像素属性
struct MaterialPassUniforms {
    float4x4 view_projection;
    float4x4 inv_view_projection;
    float3 camera_position;
    uint32_t cluster_count;
    
    uint2 render_target_size;
    uint32_t frame_index;
    uint32_t _pad;
};

// Vertex output for fullscreen quad
struct FullscreenVertexOut {
    float4 position [[position]];
    float2 uv;
};

// Fullscreen quad vertices
vertex FullscreenVertexOut fullscreen_vs(uint vertexID [[vertex_id]]) {
    FullscreenVertexOut out;
    
    // Generate fullscreen quad
    float2 positions[6] = {
        float2(-1, -1), float2(1, -1), float2(-1, 1),
        float2(-1, 1), float2(1, -1), float2(1, 1)
    };
    float2 uvs[6] = {
        float2(0, 1), float2(1, 1), float2(0, 0),
        float2(0, 0), float2(1, 1), float2(1, 0)
    };
    
    out.position = float4(positions[vertexID], 0, 1);
    out.uv = uvs[vertexID];
    
    return out;
}

// Material pass fragment shader
struct GBufferOut {
    float4 albedo [[color(0)]];
    float4 normal [[color(1)]];
    float4 orm [[color(2)]];     // Occlusion, Roughness, Metallic
    float4 depth [[color(3)]];
};

fragment GBufferOut material_pass_fs(
    FullscreenVertexOut in [[stage_in]],
    
    // Visibility buffer
    texture2d<uint> visibilityBuffer [[texture(0)]],
    
    // Geometry data
    constant RHICluster* clusters [[buffer(0)]],
    constant Meshlet* meshlets [[buffer(1)]],
    constant uint32_t* meshlet_vertices [[buffer(2)]],
    constant uint8_t* meshlet_triangles [[buffer(3)]],
    constant packed_float3* positions [[buffer(4)]],
    constant packed_float3* normals [[buffer(5)]],
    constant packed_float2* uvs [[buffer(6)]],
    constant packed_float3* tangents [[buffer(7)]],
    
    // Material data
    constant MaterialData* materials [[buffer(8)]],
    constant uint32_t* cluster_materials [[buffer(9)]],  // Per-cluster material index
    
    // Uniforms
    constant MaterialPassUniforms& uniforms [[buffer(10)]],
    
    // Material textures
    texture2d<float> albedoTextures[[texture(1)]],
    texture2d<float> normalTextures[[texture(2)]],
    texture2d<float> ormTextures[[texture(3)]])
{
    GBufferOut out;
    
    // Initialize to default (background)
    out.albedo = float4(0, 0, 0, 0);
    out.normal = float4(0, 0, 0, 0);
    out.orm = float4(0, 0, 0, 0);
    out.depth = float4(1, 0, 0, 1);
    
    // Sample visibility buffer
    uint2 pixelCoord = uint2(in.uv * float2(uniforms.render_target_size));
    uint4 vis = visibilityBuffer.read(pixelCoord);
    
    uint32_t cluster_id = vis.r;
    uint32_t triangle_id = vis.g;
    uint32_t bary_packed = vis.b;
    float depth = as_type<float>(vis.a);
    
    // Check if this pixel has geometry
    if (cluster_id == 0xFFFFFFFF) {
        discard_fragment();
    }
    
    // Decode barycentric
    float bary_x = float(bary_packed >> 16) / 65535.0f;
    float bary_y = float(bary_packed & 0xFFFF) / 65535.0f;
    float bary_z = 1.0f - bary_x - bary_y;
    float3 bary = float3(bary_x, bary_y, bary_z);
    
    // Get cluster and meshlet
    constant RHICluster& cluster = clusters[cluster_id];
    constant Meshlet& meshlet = meshlets[cluster.meshlet_index];
    
    // Get triangle vertex indices
    uint32_t triOffset = meshlet.triangle_offset + triangle_id * 3;
    
    uint8_t localIdx0 = meshlet_triangles[triOffset + 0];
    uint8_t localIdx1 = meshlet_triangles[triOffset + 1];
    uint8_t localIdx2 = meshlet_triangles[triOffset + 2];
    
    uint32_t vertIdx0 = meshlet_vertices[meshlet.vertex_offset + localIdx0];
    uint32_t vertIdx1 = meshlet_vertices[meshlet.vertex_offset + localIdx1];
    uint32_t vertIdx2 = meshlet_vertices[meshlet.vertex_offset + localIdx2];
    
    // Get vertex attributes
    float3 p0 = positions[vertIdx0];
    float3 p1 = positions[vertIdx1];
    float3 p2 = positions[vertIdx2];
    
    float3 n0 = normals[vertIdx0];
    float3 n1 = normals[vertIdx1];
    float3 n2 = normals[vertIdx2];
    
    float2 uv0 = uvs[vertIdx0];
    float2 uv1 = uvs[vertIdx1];
    float2 uv2 = uvs[vertIdx2];
    
    // Interpolate attributes
    float3 position = p0 * bary.x + p1 * bary.y + p2 * bary.z;
    float3 normal = normalize(n0 * bary.x + n1 * bary.y + n2 * bary.z);
    float2 texCoord = uv0 * bary.x + uv1 * bary.y + uv2 * bary.z;
    
    // Get material
    uint32_t materialIdx = cluster_materials[cluster_id];
    constant MaterialData& material = materials[materialIdx];
    
    // Sample textures
    constexpr sampler texSampler(mag_filter::linear, min_filter::linear, 
                                  address::repeat, coord::normalized);
    
    float4 albedo = albedoTextures.sample(texSampler, texCoord);
    float3 normalTS = normalTextures.sample(texSampler, texCoord).xyz * 2.0 - 1.0;
    float3 orm = ormTextures.sample(texSampler, texCoord).xyz;
    
    // Transform tangent-space normal to world space
    float3 tangent = tangents[vertIdx0];  // Simplified: should interpolate
    float3 bitangent = cross(normal, tangent);
    float3x3 TBN = float3x3(tangent, bitangent, normal);
    float3 worldNormal = normalize(TBN * normalTS);
    
    // Output to GBuffer
    out.albedo = albedo;
    out.normal = float4(worldNormal * 0.5 + 0.5, 1.0);  // Pack to [0,1]
    out.orm = float4(orm, 1.0);
    out.depth = float4(depth, 0, 0, 1.0);
    
    return out;
}
```

### 6.4 交付清单

| 项目 | 状态 | 说明 |
|------|------|------|
| VisibilityBuffer 类 | 📋 待实现 | C++ 封装 |
| Software Rasterizer Shader | 📋 待实现 | Metal compute shader |
| Material Pass Shader | 📋 待实现 | Metal fragment shader |
| Tile-based 优化 | 📋 待实现 | 8x8 tile 处理 |
| 深度原子操作 | 📋 待实现 | Atomic min-max |
| 性能测试 | 📋 待实现 | Triangle throughput |

---

## 7. Phase 4: Global SDF

**持续时间**: 4 周  
**前置条件**: Phase 1 完成 (可与 Phase 2,3 并行)  
**交付物**: GlobalSDF 类, SDF Update Pass, Ray Marching 函数库

### 7.1 Global SDF 结构

```cpp
// Engine/Graphics/Lumen/GlobalSDF.h

#pragma once

#include "Graphics/RHI/Core/RHITypes.h"
#include <array>

namespace primal::graphics::lumen {

/**
 * @brief 全局场景 SDF，用于 software ray tracing
 * 
 * 使用 cascaded 结构，类似阴影级联：
 * - LOD 0: 高细节，相机附近小范围
 * - LOD 1: 中细节，中等范围
 * - LOD 2: 低细节，整个场景
 */
class GlobalSDF {
public:
    static constexpr uint32_t CASCADE_COUNT = 3;
    
    struct CascadeConfig {
        uint32_t resolution;    // 体素分辨率
        float extent;           // 世界空间范围 (半边长)
        float voxel_size;       // 体素大小
    };
    
    struct Config {
        std::array<CascadeConfig, CASCADE_COUNT> cascades = {{
            { 512, 50.0f, 0.195f },   // LOD 0: 50m 半径，高细节
            { 256, 200.0f, 1.56f },   // LOD 1: 200m 半径
            { 128, 800.0f, 12.5f }    // LOD 2: 800m 半径
        }};
        
        uint32_t update_rate = 1;  // 每 N 帧更新一次
        bool use_async_compute = true;
    };
    
    struct CascadeData {
        TextureHandle texture;
        math::v3 origin;          // 当前原点 (世界空间)
        float extent;
        float voxel_size;
        uint32_t resolution;
    };
    
    GlobalSDF() = default;
    ~GlobalSDF();
    
    GlobalSDF(const GlobalSDF&) = delete;
    GlobalSDF& operator=(const GlobalSDF&) = delete;
    GlobalSDF(GlobalSDF&&) noexcept;
    GlobalSDF& operator=(GlobalSDF&&) noexcept;
    
    /**
     * @brief 初始化全局 SDF
     */
    bool Initialize(RHIDeviceBase* device, const Config& config);
    
    /**
     * @brief 更新全局 SDF
     * @param cmd 命令缓冲区
     * @param sceneMeshes 场景中的所有 mesh (带 SDF)
     * @param cameraPosition 相机位置 (用于确定 cascade 原点)
     */
    void Update(
        RHICommandBuffer* cmd,
        const std::vector<RHIClusterMesh>& sceneMeshes,
        const math::v3& cameraPosition);
    
    /**
     * @brief 获取 cascade 数据
     */
    const CascadeData& GetCascade(uint32_t index) const { return cascades_[index]; }
    const std::array<CascadeData, CASCADE_COUNT>& GetCascades() const { return cascades_; }
    
    /**
     * @brief 获取 cascade 原点 (用于 shader uniform)
     */
    std::array<math::v3, CASCADE_COUNT> GetCascadeOrigins() const;
    std::array<float, CASCADE_COUNT> GetCascadeExtents() const;
    
private:
    RHIDeviceBase* device_ = nullptr;
    Config config_;
    std::array<CascadeData, CASCADE_COUNT> cascades_;
    
    // Update pipelines
    PipelineHandle clear_pipeline_;
    PipelineHandle rasterize_pipeline_;
    PipelineHandle composite_pipeline_;
    
    // Intermediate buffers
    BufferHandle mesh_sdf_info_buffer_;
    BufferHandle cascade_uniform_buffer_;
    
    uint32_t frame_counter_ = 0;
    
    bool InitializeTextures();
    bool InitializePipelines();
    
    void UpdateCascadeOrigins(const math::v3& cameraPosition);
    void ClearCascades(RHICommandBuffer* cmd);
    void RasterizeMeshSDFs(RHICommandBuffer* cmd, const std::vector<RHIClusterMesh>& meshes);
};

}
```

### 7.2 SDF Ray Marching 库

```metal
// Engine/Graphics/Lumen/Shaders/sdf_ray_marching.metal

#include <metal_stdlib>
using namespace metal;

// === SDF Sampling ===

/**
 * @brief 采样 cascaded SDF
 * @return 有符号距离 (负值表示在物体内部)
 */
float SampleGlobalSDF(
    float3 worldPos,
    constant float3* cascadeOrigins,
    constant float* cascadeExtents,
    constant float* cascadeVoxelSizes,
    texture3d<float> cascadeTextures[GlobalSDF::CASCADE_COUNT])
{
    for (uint32_t c = 0; c < GlobalSDF::CASCADE_COUNT; ++c) {
        float3 localPos = worldPos - cascadeOrigins[c];
        float halfExtent = cascadeExtents[c];
        
        // Check if position is in this cascade
        if (all(abs(localPos) <= halfExtent)) {
            // Convert to UVW [0, 1]
            float3 uvw = (localPos + halfExtent) / (halfExtent * 2.0);
            
            constexpr sampler s(filter::linear, address::clamp_to_edge);
            return cascadeTextures[c].sample(s, uvw).r;
        }
    }
    
    // Outside all cascades - return large distance
    return 1e10f;
}

/**
 * @brief 计算 SDF 法线 (使用中心差分)
 */
float3 ComputeSDFNormal(
    float3 worldPos,
    constant float3* cascadeOrigins,
    constant float* cascadeExtents,
    constant float* cascadeVoxelSizes,
    texture3d<float> cascadeTextures[GlobalSDF::CASCADE_COUNT])
{
    const float h = 0.01f;
    
    float dx = SampleGlobalSDF(worldPos + float3(h, 0, 0), cascadeOrigins, cascadeExtents, cascadeVoxelSizes, cascadeTextures)
             - SampleGlobalSDF(worldPos - float3(h, 0, 0), cascadeOrigins, cascadeExtents, cascadeVoxelSizes, cascadeTextures);
    
    float dy = SampleGlobalSDF(worldPos + float3(0, h, 0), cascadeOrigins, cascadeExtents, cascadeVoxelSizes, cascadeTextures)
             - SampleGlobalSDF(worldPos - float3(0, h, 0), cascadeOrigins, cascadeExtents, cascadeVoxelSizes, cascadeTextures);
    
    float dz = SampleGlobalSDF(worldPos + float3(0, 0, h), cascadeOrigins, cascadeExtents, cascadeVoxelSizes, cascadeTextures)
             - SampleGlobalSDF(worldPos - float3(0, 0, h), cascadeOrigins, cascadeExtents, cascadeVoxelSizes, cascadeTextures);
    
    return normalize(float3(dx, dy, dz));
}

// === Ray Marching ===

struct RayHit {
    bool hit;
    float t;
    float3 position;
    float3 normal;
    uint32_t cascade_index;
};

/**
 * @brief Sphere tracing ray marcher
 */
RayHit RayMarchSDF(
    float3 origin,
    float3 direction,
    float maxDistance,
    constant float3* cascadeOrigins,
    constant float* cascadeExtents,
    constant float* cascadeVoxelSizes,
    texture3d<float> cascadeTextures[GlobalSDF::CASCADE_COUNT],
    uint32_t maxSteps = 128)
{
    RayHit result;
    result.hit = false;
    result.t = 0.0f;
    result.cascade_index = 0xFFFFFFFF;
    
    const float EPSILON = 0.001f;
    
    for (uint32_t i = 0; i < maxSteps; ++i) {
        float3 pos = origin + direction * result.t;
        
        float dist = SampleGlobalSDF(pos, cascadeOrigins, cascadeExtents, cascadeVoxelSizes, cascadeTextures);
        
        // Hit!
        if (dist < EPSILON) {
            result.hit = true;
            result.position = pos;
            result.normal = ComputeSDFNormal(pos, cascadeOrigins, cascadeExtents, cascadeVoxelSizes, cascadeTextures);
            
            // Determine which cascade we hit
            for (uint32_t c = 0; c < GlobalSDF::CASCADE_COUNT; ++c) {
                if (all(abs(pos - cascadeOrigins[c]) <= cascadeExtents[c])) {
                    result.cascade_index = c;
                    break;
                }
            }
            
            return result;
        }
        
        // Miss (outside all cascades)
        if (dist > 1e9f) {
            break;
        }
        
        // Step forward
        result.t += dist * 0.9f;  // Safety margin
        
        if (result.t > maxDistance) {
            break;
        }
    }
    
    return result;
}

/**
 * @brief Cone tracing for rough surfaces (approximate)
 * @param coneAngle Cone half-angle in radians
 */
float3 ConeTraceSDF(
    float3 origin,
    float3 direction,
    float coneAngle,
    float maxDistance,
    constant float3* cascadeOrigins,
    constant float* cascadeExtents,
    constant float* cascadeVoxelSizes,
    texture3d<float> cascadeTextures[GlobalSDF::CASCADE_COUNT])
{
    float3 result = float3(0.0f);
    float weight = 1.0f;
    
    float t = 0.0f;
    const uint32_t MAX_STEPS = 64;
    
    for (uint32_t i = 0; i < MAX_STEPS; ++i) {
        float3 pos = origin + direction * t;
        
        float dist = SampleGlobalSDF(pos, cascadeOrigins, cascadeExtents, cascadeVoxelSizes, cascadeTextures);
        
        // Hit
        if (dist < 0.01f) {
            float3 normal = ComputeSDFNormal(pos, cascadeOrigins, cascadeExtents, cascadeVoxelSizes, cascadeTextures);
            result = normal * 0.5 + 0.5;  // Visualize normal
            break;
        }
        
        // Miss
        if (dist > 1e9f || t > maxDistance) {
            break;
        }
        
        // Cone expansion
        float coneRadius = t * tan(coneAngle * 0.5f);
        
        // If cone radius > SDF distance, we're inside geometry (partial occlusion)
        if (coneRadius > dist) {
            weight -= 0.1f;  // Approximate soft shadow
        }
        
        t += dist * 0.8f;
    }
    
    return result * weight;
}

```

### 7.3 交付清单

| 项目 | 状态 | 说明 |
|------|------|------|
| GlobalSDF 类 | 📋 待实现 | C++ 封装 |
| Cascade 结构 | 📋 待实现 | 3-level cascade |
| SDF Clear Pass | 📋 待实现 | Compute shader |
| SDF Rasterize Pass | 📋 待实现 | Mesh SDF composite |
| Ray Marching 库 | 📋 待实现 | Metal shader library |
| 性能优化 | 📋 待实现 | Early-out, cascade selection |

---

## 8. Phase 5: Radiance Probe Grid

**持续时间**: 3 周  
**前置条件**: Phase 4 完成  
**交付物**: RadianceProbeGrid 类, Probe Trace/Propagate Shaders

### 8.1 Radiance Probe Grid 结构

```cpp
// Engine/Graphics/Lumen/RadianceProbeGrid.h

#pragma once

#include "Graphics/RHI/Core/RHITypes.h"

namespace primal::graphics::lumen {

/**
 * @brief 动态 Radiance Probe Grid
 * 
 * 实现 DDGI-style 的 probe-based GI:
 * - 3D probe grid 覆盖场景
 * - 每个 probe 存储球谐 (SH) 或 cubemap irradiance
 * - 每帧更新部分 probe (rotating update)
 * - 支持多次反弹
 */
class RadianceProbeGrid {
public:
    struct Config {
        // Grid 设置
        math::v3 grid_origin = math::v3(0.0f);
        math::v3 grid_spacing = math::v3(2.0f);  // 2m 间距
        uint3 grid_resolution = uint3(32, 16, 32);  // 32x16x32 probes
        
        // Ray tracing 设置
        uint32_t rays_per_probe = 32;        // 每帧每个 probe 的射线数
        float max_ray_distance = 100.0f;
        
        // 更新设置
        uint32_t probes_per_frame = 512;     // 每帧更新的 probe 数
        float temporal_weight = 0.9f;         // 时间滤波权重
        float spatial_weight = 0.5f;          // 空间滤波权重
    };
    
    RadianceProbeGrid() = default;
    ~RadianceProbeGrid();
    
    RadianceProbeGrid(const RadianceProbeGrid&) = delete;
    RadianceProbeGrid& operator=(const RadianceProbeGrid&) = delete;
    
    /**
     * @brief 初始化 probe grid
     */
    bool Initialize(RHIDeviceBase* device, const Config& config);
    
    /**
     * @brief 更新 probes
     * @param cmd 命令缓冲区
     * @param globalSDF 全局 SDF (用于 ray tracing)
     * @param lights 场景光源
     * @param skyCubemap 天空盒
     */
    void Update(
        RHICommandBuffer* cmd,
        const GlobalSDF& globalSDF,
        const LightScene& lights,
        TextureHandle skyCubemap);
    
    /**
     * @brief 获取 irradiance 纹理 (用于采样)
     */
    TextureHandle GetIrradianceTexture() const { return irradiance_texture_; }
    
    /**
     * @brief 获取 depth 纹理 (用于采样)
     */
    TextureHandle GetDepthTexture() const { return depth_texture_; }
    
    /**
     * @brief 获取配置
     */
    const Config& GetConfig() const { return config_; }
    
private:
    RHIDeviceBase* device_ = nullptr;
    Config config_;
    
    // Probe data textures
    TextureHandle irradiance_texture_;  // RGBA16F: RGB irradiance + A validity
    TextureHandle depth_texture_;       // RG16F: mean depth + variance
    
    // Ray tracing data
    BufferHandle ray_direction_buffer_;    // 预计算的随机射线方向
    BufferHandle probe_index_buffer_;      // 当前帧要更新的 probe 索引
    BufferHandle ray_result_buffer_;       // 射线追踪结果
    
    // Pipelines
    PipelineHandle trace_pipeline_;
    PipelineHandle propagate_pipeline_;
    PipelineHandle update_texture_pipeline_;
    
    // State
    uint32_t frame_counter_ = 0;
    uint32_t current_probe_offset_ = 0;
    
    bool InitializeTextures();
    bool InitializeBuffers();
    bool InitializePipelines();
    
    void UpdateProbeSelection();
};

}
```

### 8.2 交付清单

| 项目 | 状态 | 说明 |
|------|------|------|
| RadianceProbeGrid 类 | 📋 待实现 | C++ 封装 |
| Probe Trace Shader | 📋 待实现 | SDF ray tracing |
| Probe Propagate Shader | 📋 待实现 | 空间滤波 |
| Probe Update Shader | 📋 待实现 | 纹理更新 |
| 采样函数 | 📋 待实现 | Shader 采样库 |

---

## 9. Phase 6: Lumen Integration

**持续时间**: 3 周  
**前置条件**: Phase 4, 5 完成  
**交付物**: LumenPipeline 类, 完整 GI 渲染管线

### 9.1 Lumen Pipeline

```cpp
// Engine/Graphics/Lumen/LumenPipeline.h

#pragma once

#include "Graphics/Lumen/GlobalSDF.h"
#include "Graphics/Lumen/RadianceProbeGrid.h"
#include "Graphics/RenderGraph/RenderGraphDefinitions.h"

namespace primal::graphics::lumen {

/**
 * @brief Lumen 全局光照管线
 * 
 * 整合所有 Lumen 组件:
 * - Global SDF for ray tracing
 * - Radiance Probe Grid for diffuse GI
 * - Screen-space GI for high-frequency
 */
class LumenPipeline {
public:
    struct Config {
        // SDF 配置
        GlobalSDF::Config sdf_config;
        
        // Probe 配置
        RadianceProbeGrid::Config probe_config;
        
        // GI 配置
        uint32_t screen_ray_samples = 1;    // 每像素屏幕空间射线
        float max_ray_distance = 100.0f;
        uint32_t max_bounces = 2;
        
        // 性能
        bool enable_probe_grid = true;
        bool enable_screen_space_gi = true;
        bool enable_sdf_tracing = true;
    };
    
    struct FrameData {
        // Input
        TextureHandle gbuffer_depth;
        TextureHandle gbuffer_normal;
        TextureHandle gbuffer_albedo;
        TextureHandle gbuffer_orm;
        
        // Scene
        const std::vector<RHIClusterMesh>* scene_meshes;
        const LightScene* lights;
        TextureHandle sky_cubemap;
        
        // View
        const RenderView* view;
    };
    
    LumenPipeline() = default;
    ~LumenPipeline();
    
    LumenPipeline(const LumenPipeline&) = delete;
    LumenPipeline& operator=(const LumenPipeline&) = delete;
    
    /**
     * @brief 初始化 Lumen 管线
     */
    bool Initialize(RHIDeviceBase* device, const Config& config);
    
    /**
     * @brief 执行一帧的 Lumen 更新
     */
    void Execute(RHICommandBuffer* cmd, const FrameData& frameData);
    
    /**
     * @brief 添加到 RenderGraph
     */
    rendergraph::RGResourceHandle AddToRenderGraph(
        rendergraph::RenderGraph& graph,
        const FrameData& frameData,
        rendergraph::RGResourceHandle depthTarget);
    
    /**
     * @brief 获取间接光照纹理
     */
    TextureHandle GetIndirectLightingTexture() const { return indirect_lighting_texture_; }
    
    /**
     * @brief 获取组件
     */
    GlobalSDF& GetGlobalSDF() { return *global_sdf_; }
    const GlobalSDF& GetGlobalSDF() const { return *global_sdf_; }
    RadianceProbeGrid& GetProbeGrid() { return *probe_grid_; }
    const RadianceProbeGrid& GetProbeGrid() const { return *probe_grid_; }
    
private:
    RHIDeviceBase* device_ = nullptr;
    Config config_;
    
    // Components
    std::unique_ptr<GlobalSDF> global_sdf_;
    std::unique_ptr<RadianceProbeGrid> probe_grid_;
    
    // Output
    TextureHandle indirect_lighting_texture_;
    
    // Pipelines
    PipelineHandle indirect_lighting_pipeline_;
    PipelineHandle screen_space_gi_pipeline_;
    
    bool InitializeResources();
};

}
```

### 9.2 交付清单

| 项目 | 状态 | 说明 |
|------|------|------|
| LumenPipeline 类 | 📋 待实现 | 主管线类 |
| Indirect Lighting Shader | 📋 待实现 | 最终合成 shader |
| RenderGraph 集成 | 📋 待实现 | RG pass 定义 |
| 性能分析 | 📋 待实现 | GPU timing |
| 调试可视化 | 📋 待实现 | SDF/Probe 可视化 |

---

## 10. 文件结构

```
Engine/
├── Graphics/
│   ├── Nanite/
│   │   ├── NaniteCore.h                      # 核心类型定义
│   │   ├── NaniteCore.cpp
│   │   ├── ClusterCullingPass.h              # GPU culling
│   │   ├── ClusterCullingPass.cpp
│   │   ├── SoftwareRasterizer.h              # Visibility buffer
│   │   ├── SoftwareRasterizer.cpp
│   │   ├── VisibilityBuffer.h                # VB 格式
│   │   ├── VisibilityBuffer.cpp
│   │   ├── ClusterHierarchy.h                # LOD 数据
│   │   ├── ClusterHierarchy.cpp
│   │   └── Shaders/
│   │       ├── cluster_culling.metal         # Culling compute
│   │       ├── software_rasterizer.metal     # Rasterization compute
│   │       └── material_pass.metal           # GBuffer output
│   │
│   ├── Lumen/
│   │   ├── LumenPipeline.h                   # 主管线
│   │   ├── LumenPipeline.cpp
│   │   ├── GlobalSDF.h                       # SDF 系统
│   │   ├── GlobalSDF.cpp
│   │   ├── RadianceProbeGrid.h               # Probe 系统
│   │   ├── RadianceProbeGrid.cpp
│   │   └── Shaders/
│   │       ├── sdf_clear.metal               # SDF clear
│   │       ├── sdf_rasterize.metal           # SDF update
│   │       ├── sdf_ray_marching.metal        # Ray tracing lib
│   │       ├── probe_trace.metal             # Probe update
│   │       ├── probe_propagate.metal         # Probe filtering
│   │       ├── lumen_indirect.metal          # Final GI
│   │       └── lumen_common.metal            # Shared functions
│   │
│   └── RHI/
│       └── Core/
│           ├── RHICluster.h                  # Cluster 数据结构
│           ├── RHIClusterHierarchy.h         # LOD hierarchy
│           └── RHIVisibilityBuffer.h         # VB 格式
│
ContentTools/
├── ClusterHierarchyBuilder.h                 # LOD 生成工具
├── ClusterHierarchyBuilder.cpp
└── pack_geometry.py                          # 扩展 (cluster 输出)
│
EngineTest/
├── IntegrationTests/
│   ├── TestNanite.cpp                        # Nanite 测试
│   └── TestLumen.cpp                         # Lumen 测试
└── shaders/
    ├── NaniteTest.metal                      # 测试着色器
    └── LumenTest.metal
```

---

## 11. 测试策略

### 11.1 单元测试

| 测试 | 范围 | 验证内容 |
|------|------|---------|
| RHICluster 结构 | 内存布局 | 64 字节对齐 |
| DrawIndirect | API | Metal 命令正确提交 |
| Visibility Buffer | 编码/解码 | 精度验证 |
| SDF Ray Marching | 算法 | 命中/未命中正确 |
| Probe Grid | 更新 | 时间一致性 |

### 11.2 集成测试

| 测试 | 场景 | 验证内容 |
|------|------|---------|
| Nanite 基础 | 简单 cube | 正确渲染 |
| Nanite LOD | 多 LOD mesh | 自动切换 |
| Lumen 基础 | 单光源 | 间接光照可见 |
| Lumen 多次反弹 | 复杂场景 | 颜色渗透 |
| 完整管线 | Sponza | 性能达标 |

### 11.3 性能测试

| 指标 | 目标 | 测试方法 |
|------|------|---------|
| Cluster Culling | < 1ms (100K clusters) | GPU timing |
| Software Rasterization | < 3ms (1M tris) | GPU timing |
| SDF Update | < 2ms | GPU timing |
| Probe Update | < 2ms (512 probes) | GPU timing |
| Indirect Lighting | < 4ms | GPU timing |
| **Total** | **< 12ms** (60+ FPS) | Frame timing |

---

## 12. 性能目标

### 12.1 硬件目标

| 硬件 | 目标帧率 | 目标分辨率 |
|------|---------|-----------|
| M1 Pro | 60 FPS | 1920x1080 |
| M1 Max | 60 FPS | 2560x1440 |
| M2 | 60 FPS | 1920x1080 |
| A15 (iOS) | 30 FPS | 1280x720 |

### 12.2 内存预算

| 资源 | 预算 | 说明 |
|------|------|------|
| Geometry Pool | 256 MB | 流式加载 |
| Per-Frame | 64 MB | Transient |
| Lumen Resources | 24 MB | SDF + Probes |
| **Total** | **~350 MB** | GPU Memory |

### 12.3 场景复杂度

| 指标 | 当前 | 目标 |
|------|------|------|
| 三角形数 | 100K | 1M-10M |
| Draw Calls | 50 | 1-2 |
| Clusters | 0 | 100K+ |
| LOD Levels | 0 | 5-6 |

---

## 13. 风险评估

### 13.1 技术风险

| 风险 | 概率 | 影响 | 缓解措施 |
|------|------|------|---------|
| Software Rasterization 性能不达标 | 中 | 高 | 优化 tile 处理，使用 Mesh Shaders fallback |
| SDF 更新延迟 | 中 | 中 | 异步 compute，增量更新 |
| Probe 闪烁 | 中 | 低 | 时间滤波，空间滤波 |
| 内存超限 | 低 | 高 | 流式加载，LOD bias |
| Metal 兼容性问题 | 低 | 中 | 广泛测试，fallback path |

### 13.2 进度风险

| 风险 | 概率 | 影响 | 缓解措施 |
|------|------|------|---------|
| Phase 延期 | 中 | 中 | 预留 buffer time |
| 集成问题 | 中 | 高 | 持续集成，早期测试 |
| 性能优化超时 | 高 | 中 | 分阶段优化 |

---

## 14. 时间线

### 14.1 甘特图

```
Week:  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15 16 17 18
       │  │  │  │  │  │  │  │  │  │  │  │  │  │  │  │  │  │
Phase 1: RHI + GPU-Driven
       ████████████
       
Phase 2: Cluster LOD
             ████████████████
             
Phase 3: Software Raster
                     ████████████████
                     
Phase 4: Global SDF
             ████████████████████████
             
Phase 5: Probe Grid
                         ████████████████
                         
Phase 6: Integration
                                 ████████████████
                                 
Integration & Polish
                                         ████████████
```

### 14.2 里程碑

| 里程碑 | 日期 | 交付物 |
|--------|------|--------|
| M1: GPU-Driven Foundation | Week 2 | Indirect draw, Cluster culling |
| M2: LOD System | Week 5 | Hierarchical clusters, LOD selection |
| M3: Software Raster | Week 8 | Visibility buffer, Material pass |
| M4: SDF Complete | Week 10 | Global SDF, Ray marching |
| M5: Probes Complete | Week 12 | Radiance grid, Propagation |
| M6: Lumen Alpha | Week 15 | Full GI pipeline |
| M7: Release | Week 18 | Optimized, tested, documented |

### 14.3 人员分配 (假设 2 人)

| Phase | 开发者 A | 开发者 B |
|-------|---------|---------|
| 1 | RHI 扩展 | Culling shader |
| 2 | LOD 生成工具 | LOD selection |
| 3 | Raster shader | Material pass |
| 4 | SDF update | Ray marching |
| 5 | Probe trace | Propagation |
| 6 | Integration | Testing |

---

## 附录 A: 参考资料

1. **Nanite**
   - "A Deep Dive into Nanite Virtualized Geometry" - Karis, SIGGRAPH 2021
   - "Real-Time Meshlet Rendering" - Wihlidal, GDC 2022

2. **Lumen**
   - "Dynamic Diffuse Global Illumination with Ray-Traced Irradiance Probes" - Majercik, 2019
   - "DDGI: Dynamic Diffuse Global Illumination" - NVIDIA

3. **Software Rasterization**
   - "Visibility Buffer: How to render a trillion triangles without blowing out memory" - Schied, 2022
   - "Mesh Shaders in Metal" - Apple Developer

4. **SDF Ray Tracing**
   - "Signed Distance Fields for Real-Time Rendering" - Wright, 2021
   - "Ray Tracing Gems II" - Chapter 15

---

## 附录 B: 术语表

| 术语 | 解释 |
|------|------|
| Cluster | Nanite 的基本渲染单元，包含 128 个三角形 |
| Meshlet | Cluster 的 mesh 数据部分 |
| Visibility Buffer | 存储 per-pixel 的 cluster/triangle/barycentric |
| LOD | Level of Detail，细节级别 |
| SDF | Signed Distance Field，有符号距离场 |
| Probe | 存储局部 irradiance 的采样点 |
| DDGI | Dynamic Diffuse Global Illumination |
| Cascade | 类似阴影级联的空间划分 |

---

**文档结束**

*本文档将在开发过程中持续更新。最后更新: 2026-03-03*
