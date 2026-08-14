---
title: Dawn WebGPU 渲染管线迁移规划
created: 2026-06-11
updated: 2026-06-13
status: Phase 1 complete (macOS + WASM)
---

# Dawn WebGPU 渲染管线迁移规划

## 目标

将 Dawn WebGPU 后端从当前 Forward Only 渲染逐步迁移到与 Metal 后端功能对等，每个阶段作为一个独立的 Render Mode，通过 UI 按钮实时切换。

## Render Mode 枚举定义

```cpp
enum class DawnRenderMode : u32 {
    Forward = 0,           // Phase 1: 当前状态 + 点光/聚光/CSM/IBL
    ForwardSSR = 1,        // Phase 2: + TAA + SSR
    DeferredPBR = 2,       // Phase 3: G-Buffer + Deferred Lighting
    LumenDDGI = 3,         // Phase 4: + DDGI 间接光照
    LumenFull = 4,         // Phase 5: + Surface Cache + Screen Probes (Metal 对等)
};
```

## 管线切换架构

### 切换机制

- 在 `StandardRenderPipeline` 中持有 `DawnRenderMode currentMode_`
- UI 层（Test 入口或 ImGui）通过按钮/快捷键切换 `currentMode_`
- 切换时重建 RenderGraph（不同 mode 对应不同的 pass 链）
- 共享资源（HDR scene color, depth, HZB）跨 mode 复用

### 切换逻辑伪代码

```cpp
// StandardRenderPipeline::Render()
void StandardRenderPipeline::Render(...) {
    switch (currentMode_) {
    case DawnRenderMode::Forward:
        BuildForwardPipeline(graph);       // Phase 1 passes
        break;
    case DawnRenderMode::ForwardSSR:
        BuildForwardSSRPipeline(graph);    // Phase 2 passes
        break;
    case DawnRenderMode::DeferredPBR:
        BuildDeferredPipeline(graph);      // Phase 3 passes
        break;
    case DawnRenderMode::LumenDDGI:
        BuildLumenDDGIPipeline(graph);    // Phase 4 passes
        break;
    case DawnRenderMode::LumenFull:
        BuildLumenFullPipeline(graph);    // Phase 5 passes
        break;
    }
    graph.Compile();
    graph.Execute(device, cmdBuffer);
}
```

---

## Phase 1 — Forward PBR 补全 (Forward Mode)

**目标**: 补全 Forward 渲染的基础光照能力

**状态**: ✅ **已完成** (2026-06-13, macOS + WASM 均通过)

**当前基线**:
- [x] Forward PBR Cook-Torrance
- [x] 4级联 CSM (Cascade Shadow Map) + PCF
- [x] IBL (Image-Based Lighting) — irradiance + prefilter + BRDF LUT
- [x] 点光源 + 聚光灯 PBR（最多 128 盏）
- [x] 渲染模式切换（Tab 键）

### 渲染模式切换 (Phase 1 实现)

通过 **Tab 键** 循环切换 4 种渲染模式，控制台打印当前模式名称：

| Mode | 名称 | 阴影 | IBL | 点光源/聚光灯 | 说明 |
|------|------|------|-----|-------------|------|
| 0 | NoEffects | ✗ | ✗ | ✗ | 仅方向光，无任何效果 |
| 1 | ShadowOnly | ✓ | ✗ | ✗ | 方向光 + 阴影 |
| 2 | ShadowAndIBL | ✓ | ✓ | ✗ | 方向光 + 阴影 + IBL（默认模式） |
| 3 | Full | ✓ | ✓ | ✓ | 方向光 + 阴影 + IBL + 点光源/聚光灯 |

**操作**:
- 按 **Tab** 切换到下一个模式
- 控制台输出 `[Mode] <模式名>` 显示当前模式
- 默认启动模式: **ShadowAndIBL** (Mode 2)

**Full 模式测试灯光** (Sponza 场景):
- 绿色点光源: (-6, 2.5, 0), range=6, intensity=4
- 橙色点光源: (6, 2.5, 0), range=6, intensity=4
- 紫色点光源: (0, 2.5, 3), range=5, intensity=3
- 暖黄聚光灯: (0, 6, -3) 方向向下, range=8, intensity=5

### 实现细节

#### WASM 部署
- **构建**: Emscripten (emcc) + Dawn WebGPU 后端
- **前端**: `wasm/shell.html` 提供 WebGPU canvas + HUD overlay
- **HUD**: 动态创建 (EM_ASM)，Tab 切换时通过 `window.setRenderMode()` 更新
- **IBL**: WASM 无 HDR 文件时使用程序化天空渐变生成 irradiance/prefilter/LUT
- **注意**: `UpdatePunctualLights()` 必须在平台条件编译块外调用，否则 WASM 不执行

#### 1.1 点光源 + 聚光灯 PBR
- **文件**: `ForwardPBR.wgsl`, `ForwardRenderer.cpp`
- **实现**: fragment shader 中 punctual light loop（最多 128 盏），PBR Cook-Torrance 光照
- **注意**: WGSL `PunctualLightParameters` 使用 `vec4<f32>` 替代 `vec3<f32>`（见下方兼容性说明）

#### 1.2 CSM 多级联阴影
- **文件**: `ForwardPBR.wgsl`, `TestDawnForwardRenderer.cpp`
- **实现**: `texture_depth_2d_array` + 4 cascade，PCF 3×3 采样
- **注意**: 阴影 UV Y 轴翻转适配 WebGPU viewport 坐标系

#### 1.3 IBL (Image-Based Lighting)
- **文件**: `IBL_*.wgsl` (5 个), `ForwardPBR.wgsl`, `TestDawnForwardRenderer.cpp`
- **实现**: Equirect→Cube, Irradiance convolution, Specular prefilter, BRDF LUT
- **强度**: IBL 贡献乘以 `ao * iblShadow * 0.6`

### Apple Silicon 兼容性注意事项

**simd::float3 布局问题**: macOS 上 `math::v3 = simd::float3`，实际占 16 字节（而非 12 字节）。WGSL `vec3<f32>` 占 12 字节。这导致 `LightParameters` (C++) = 192 bytes vs `PunctualLightParameters` (WGSL) 如果用 vec3 只有 144 bytes。

**解决方案**: WGSL 中使用 `vec4<f32>` + 显式 padding 匹配 C++ 布局：
```wgsl
struct PunctualLightParameters {
    position: vec4<f32>,       // xyz=pos, w=unused (16 bytes, matches simd::float3)
    intensity: f32,
    _p0: f32, _p1: f32, _p2: f32,  // pad to 32
    direction: vec4<f32>,      // xyz=dir, w=unused
    ...
};
```
读取时使用 `.xyz`：`plight.position.xyz - input.worldPos`

**GlobalShaderData padding**: `_pad0: vec3<u32>` 在 WGSL 中有 16 字节对齐，必须拆分为 3 个独立 `u32` 字段匹配 C++ 的 `u32 _pad0[3]`。

---

## Phase 2 — Forward + TAA + SSR (ForwardSSR Mode)

**目标**: 加入时域抗锯齿和屏幕空间反射

**Phase 1 基础上新增**:

### 2.1 Motion Vector (Velocity Buffer)
- **文件**: `Velocity.wgsl`（当前输出零）
- **改动**: 计算 prev frame clip pos → curr frame clip pos 的差值
- **依赖**: 需要 per-object 的 `prevWorldTransform`，在 `PerObjectData` 中添加
- **验收**: 移动摄像机时 velocity 可视化显示正确的运动方向

### 2.2 TAA (Temporal Anti-Aliasing)
- **新增 shader**: `TAA.wgsl`
- **新增 pass**: `TAAPass.h/.cpp`
- **改动**:
  - Ping-pong 历史帧纹理 (RGBA16F)
  - Velocity-based reprojection
  - Variance clipping (YCoCg 空间)
  - 可选： sharpening filter
- **依赖**: 2.1 Motion Vector
- **验收**: 物体边缘无闪烁，移动时无鬼影

### 2.3 SSR (Screen-Space Reflections)
- **新增 shader**: `SSRTrace.wgsl`, `SSRTemporal.wgsl`
- **新增 pass**: `SSRPass.h/.cpp`（替代当前 placeholder）
- **改动**:
  - Hi-Z ray march（复用 HZB + SSGI 的 ray march 经验）
  - Depth/normal-based hit detection
  - Temporal accumulation（复用 TAA 基础设施）
- **依赖**: 2.2 TAA
- **验收**: 地面反射可见场景物体，金属表面有明显反射效果

**Phase 2 Pass 链**:
```
Shadow (4 cascade) -> Opaque -> Transparent -> HZB -> Velocity -> SSR
-> TAA -> Bloom -> SSAO -> SSGI -> ToneMapping -> Present
```

**UI 按钮**: `[Mode: Forward+SSR]`

---

## Phase 3 — Deferred PBR (DeferredPBR Mode)

**目标**: 从 Forward 切换到 Deferred 渲染架构

### 3.1 G-Buffer Pass
- **新增 shader**: `GBuffer.wgsl`
- **新增 pass**: `GBufferPass.h/.cpp`
- **改动**: 4 个 render target
  - RT0: World Position (RGBA16F)
  - RT1: Normal + Depth (RGBA16F)
  - RT2: Albedo + Metallic (RGBA8)
  - RT3: ORM (Occlusion, Roughness, Metallic) + Emissive (RGBA8)
- **依赖**: 无
- **验收**: G-Buffer 可视化正确

### 3.2 Deferred Lighting Pass
- **修改 shader**: `DeferredLighting.wgsl`（当前已存在框架）
- **改动**:
  - Full-screen compute shader 读取 4 个 G-Buffer
  - 完整 PBR（方向光 + 点光 + 聚光 + CSM + IBL）
- **依赖**: 3.1 G-Buffer
- **验收**: Deferred 渲染结果与 Forward 视觉一致

### 3.3 Deferred + SSAO/SSGI/TAA/SSR 集成
- **改动**: 所有 post-process pass 从读 Forward output 改为读 G-Buffer
- **验收**: 所有 Phase 1-2 的效果在 Deferred 管线下正常工作

**Phase 3 Pass 链**:
```
Shadow (4 cascade) -> G-Buffer -> Deferred Lighting -> HZB -> Velocity -> SSR
-> TAA -> Bloom -> SSAO -> SSGI -> ToneMapping -> Present
```

**UI 按钮**: `[Mode: Deferred PBR]`

---

## Phase 4 — Lumen DDGI (LumenDDGI Mode)

**目标**: 移植 DDGI 动态全局光照

### 4.1 Global SDF
- **移植 shader**: `GlobalSDFVoxelization.metal` → `GlobalSDFVoxelization.wgsl`
- **新增 pass**: `GlobalSDFPass.h/.cpp`
- **改动**: 将场景体素化为 SDF volume（3D texture）
- **验收**: SDF 可视化（切片）正确反映场景几何

### 4.2 DDGI Probe Grid
- **移植 shader** (7 个):
  - `DDGIVolumeData.metal` → `DDGIVolumeData.wgsl`
  - `DDGITraceRays.metal` → `DDGITraceRays.wgsl`（通过 SDF trace）
  - `DDGIUpdateIrradiance.metal` → `DDGIUpdateIrradiance.wgsl`（SH projection）
  - `DDGIUpdateDepth.metal` → `DDGIUpdateDepth.wgsl`
  - `DDGISample.metal` → `DDGISample.wgsl`
  - `DDGICardRadianceAvg.metal` → `DDGICardRadianceAvg.wgsl`
  - `DDGIProbeIrradianceFromCards.metal` → `DDGIProbeIrradianceFromCards.wgsl`
- **新增 pass**: `DDGIPass.h/.cpp`
- **Apple Silicon 注意事项**:
  - Probe data 用 buffer 不用 texture3D（memory 限制）
  - Ray trace 必须限制 device read 次数（≤32）
  - 需三缓冲 probe update list
- **依赖**: 4.1 Global SDF
- **验收**: 彩色间接光照可见，关掉直接光后场景仍有 bounced light

### 4.3 DDGI 与 Deferred 管线集成
- **改动**: Deferred lighting shader 采样 DDGI irradiance 替代 SH9 ambient
- **验收**: DDGI 为场景提供正确的间接光照，颜色 bounce 正确

**Phase 4 Pass 链**:
```
Shadow (4 cascade) -> G-Buffer -> GlobalSDF Update -> DDGI Trace -> DDGI Update
-> Deferred Lighting (PBR + DDGI) -> HZB -> Velocity -> SSR
-> TAA -> Bloom -> SSAO -> SSGI -> ToneMapping -> Present
```

**UI 按钮**: `[Mode: Lumen DDGI]`

---

## Phase 5 — Lumen Full (LumenFull Mode, Metal 对等)

**目标**: 完整 Lumen GI + Surface Cache + Screen Probes

### 5.1 Surface Cache
- **移植 shader** (7 个):
  - `SurfaceCacheCapture.metal` → `SurfaceCacheCapture.wgsl`
  - `SurfaceCacheData.metal` → `SurfaceCacheData.wgsl`
  - `SurfaceCacheDilate.metal` → `SurfaceCacheDilate.wgsl`
  - `SurfaceCacheIndirectResolve.metal` → `SurfaceCacheIndirectResolve.wgsl`
  - `SurfaceCacheIndirectTrace.metal` → `SurfaceCacheIndirectTrace.wgsl`
  - `SurfaceCacheLightCull.metal` → `SurfaceCacheLightCull.wgsl`
  - `SurfaceCacheLightEval.metal` → `SurfaceCacheLightEval.wgsl`
- **新增 pass**: `SurfaceCachePass.h/.cpp`
- **改动**: 2048 atlas, 32px pages, card capture + light eval
- **Apple Silicon 注意事项**:
  - Light cull/eval 必须 skip empty tiles（全 atlas dispatch 超限）
  - texture bandwidth ~4M pixel 上限
- **依赖**: Phase 4 DDGI
- **验收**: Surface cache atlas 可视化正确，card lighting 正确

### 5.2 Screen Probe GI
- **移植 shader** (7 个):
  - `ScreenProbeTraceRays.metal` → `ScreenProbeTraceRays.wgsl`
  - `ScreenProbeGather.metal` → `ScreenProbeGather.wgsl`
  - `ScreenProbePlace.metal` → `ScreenProbePlace.wgsl`
  - `ScreenProbeAverage.metal` → `ScreenProbeAverage.wgsl`
  - `ScreenProbeTemporal.metal` → `ScreenProbeTemporal.wgsl`
  - `ScreenProbeDenoise.metal` → `ScreenProbeDenoise.wgsl`
  - `ScreenProbeSpatialFilter.metal` → `ScreenProbeSpatialFilter.wgsl`
- **新增 pass**: `ScreenProbeGIPass.h/.cpp`
- **依赖**: 5.1 Surface Cache
- **验收**: 最高质量间接光照，接近离线渲染的 bounced light

### 5.3 Lumen SSAO (GTAO)
- **移植 shader**: `SSAOTrace.metal` → `SSAOTrace.wgsl`, `SSAOFilter.metal` → `SSAOFilter.wgsl`
- **改动**: 替换当前基础 SSAO 为 GTAO（方向性 AO）
- **依赖**: 无
- **验收**: 接触阴影更精细，方向性遮挡正确

**Phase 5 Pass 链**:
```
Shadow (4 cascade) -> G-Buffer -> GlobalSDF Update -> Surface Cache Capture
-> Surface Cache Light Eval -> DDGI Trace -> DDGI Update -> Screen Probe Place
-> Screen Probe Trace -> Screen Probe Gather -> Deferred Lighting (Full Lumen)
-> HZB -> Velocity -> SSR -> TAA -> Bloom -> Lumen SSAO (GTAO)
-> SSGI -> ToneMapping -> Present
```

**UI 按钮**: `[Mode: Lumen Full]`

---

## 跨 Phase 共享资源

以下资源在所有 Mode 间共享，切换时无需重建：

| 资源 | 格式 | 用途 |
|------|------|------|
| Scene Color HDR | RGBA16F, Window Size | 主渲染目标 |
| Depth Buffer | D32F, Window Size | 深度 |
| HZB Mip Chain | R32F, Half→1/16 Size | SSGI/SSR ray march |
| Shadow Map Array | D32F, 2048x2048x4 | CSM |
| Velocity Buffer | RG16F, Window Size | TAA/SSG temporal |
| Bloom Extract | RGBA16F, Quarter Size | Bloom |
| SSAO Output | R8, Window Size | AO |
| SSGI Output | RGBA16F, Window Size | GI |

Phase-specific 资源（切换时按需创建/销毁）：
- Phase 3: G-Buffer 4 RT
- Phase 4: Global SDF volume, DDGI probe buffers/textures
- Phase 5: Surface Cache atlas, Screen Probe textures

---

## 预估工作量

| Phase | 新增 Shader | 新增 Pass | 预估时间 | 累计 Metal 对等度 |
|-------|------------|-----------|----------|------------------|
| 1 | 3 (IBL) | 0 | 2-3 周 | ~50% |
| 2 | 3 (TAA + SSR) | 2 | 3-4 周 | ~70% |
| 3 | 2 (G-Buffer + Deferred) | 2 | 3-4 周 | ~75% |
| 4 | 8 (SDF + DDGI) | 2 | 6-8 周 | ~90% |
| 5 | 14 (Surface Cache + Screen Probes + GTAO) | 3 | 8-10 周 | ~100% |

---

## Apple Silicon 已知限制（各 Phase 需注意）

| 限制 | 影响 Phase | 应对策略 |
|------|-----------|---------|
| texture3D 读取 ~4 次上限 | Phase 4 (DDGI) | Probe data 用 buffer |
| Compute buffer read ≤32 次/线程 | Phase 4-5 | 预聚合 pass，循环限 24 次 |
| Texture bandwidth >2M pixel 闪烁 | Phase 5 (Surface Cache) | Skip empty tiles, 分 pass |
| Fragment texture 须 batch 单次调用 | Phase 3 (G-Buffer) | setFragmentTextures 单次 |
| Pool buffer 帧间竞争 | 全 Phase | constant buffer 用 setBytes |

---

## 文件结构规划

```
Engine/Graphics/RenderPipeline/
├── DawnRenderMode.h                    // DawnRenderMode enum + 切换逻辑
├── StandardRenderPipeline.h/.cpp       // 扩展：mode-based pass chain
└── RenderPasses/
    ├── Dawn/                           // Dawn 专用 pass
    │   ├── GBufferPass.h/.cpp
    │   ├── DeferredLightingPass.h/.cpp
    │   ├── TAAPass.h/.cpp
    │   ├── SSRPass.h/.cpp
    │   └── IBLPass.h/.cpp
    ├── Lumen/Dawn/                     // Dawn Lumen passes
    │   ├── DawnDDGIPass.h/.cpp
    │   ├── DawnSurfaceCachePass.h/.cpp
    │   ├── DawnScreenProbeGIPass.h/.cpp
    │   └── DawnGlobalSDFPass.h/.cpp
    └── PostProcess/                    // 已有，共用
        ├── LumenSSGIDawnPass.h/.cpp
        ├── SSAOPass.h/.cpp
        └── ...

Engine/Graphics/Dawn/shaders/           // 新增 WGSL shader
├── IBLPrefilter.wgsl                   // Phase 1
├── IBLIrradiance.wgsl                  // Phase 1
├── IBLBRDF.wgsl                        // Phase 1
├── TAA.wgsl                            // Phase 2
├── SSRTrace.wgsl                       // Phase 2
├── SSRTemporal.wgsl                    // Phase 2
├── GBuffer.wgsl                        // Phase 3
├── DeferredLighting.wgsl              // Phase 3 (upgrade existing)
├── GlobalSDFVoxelization.wgsl         // Phase 4
├── DDGI*.wgsl (7 files)               // Phase 4
├── SurfaceCache*.wgsl (7 files)       // Phase 5
├── ScreenProbe*.wgsl (7 files)        // Phase 5
└── SSAOTrace.wgsl (GTAO)              // Phase 5
```
