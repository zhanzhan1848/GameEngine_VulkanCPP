# Render Graph Pass Management & Post-Processing Design

## 1. 概述
为了提升渲染画质，我们将基于 Render Graph 架构实现一系列 Post-Processing Pass。
主要包含：
- **SSAO (Screen Space Ambient Occlusion)**: 增强几何细节的遮蔽感。
- **SSGI (Screen Space Global Illumination)**: 屏幕空间全局光照。
- **Bloom**: 泛光效果，增加高光区域的溢出感。
- **ToneMapping**: 色调映射 (HDR -> SDR)。
- **Visibility / PrePass**: 优化 Overdraw 的前置 Pass。

## 2. Pass 分类 (RGPassCategory)
为了更好管理 Pass 执行顺序和资源依赖，引入 `RGPassCategory`：

```cpp
enum class RGPassCategory {
    None,
    Visibility,     // Z-Prepass / Visibility Buffer
    Depth,          // Shadow Maps
    Main,           // GBuffer / Forward Lighting
    Lighting,       // Deferred Lighting / Global Illumination (SSAO, SSGI)
    PostProcess,    // Bloom, ToneMapping, Color Grading
    UI,             // User Interface
    Present         // Final Blit
};
```

## 3. Post-Processing Pipeline 规划

### 3.1 SSAO Pass (Compute)
- **Shader**: `SSAOShader.metal`
- **Inputs**: 
  - `NormalDepth` (RHI_TEXTURE_USAGE_SHADER_RESOURCE)
  - `Albedo` (RHI_TEXTURE_USAGE_SHADER_RESOURCE)
- **Outputs**:
  - `SSAOTexture` (RHI_TEXTURE_USAGE_UNORDERED_ACCESS)
- **Logic**:
  - 计算屏幕空间遮蔽。
  - 随后通常需要一个 Blur Pass 来降噪。

### 3.2 SSGI Pass (Compute)
- **Shader**: `SSGIShader.metal`
- **Inputs**:
  - `NormalDepth`
  - `Albedo`
  - `PreviousFrameResult` (Optional for Temporal)
- **Outputs**:
  - `SSGIOutput`
- **Logic**:
  - Ray Marching in Screen Space。

### 3.3 Bloom Pass (Compute/Graphics)
- **Stages**:
  1. **Bright Pass**: 提取高亮区域 (阈值截断)。
  2. **Blur Pass**: 对高亮区域进行多次高斯模糊 (Downsample -> Blur -> Upsample)。
  3. **Composite**: 将模糊后的高亮叠加回原图。
- **Simplified Version**:
  - 先实现单次高斯模糊 `BlurPass.metal`。

### 3.4 ToneMapping Pass (Graphics)
- **Shader**: `PostProcess.metal` (需修改或新建)
- **Inputs**:
  - `SceneColor` (HDR)
  - `BloomTexture` (Optional)
- **Outputs**:
  - `BackBuffer` (SDR)
- **Logic**:
  - Apply ToneMapping (Reinhard / ACES).
  - Apply Gamma Correction.

## 4. 目录结构
```
Engine/Graphics/RenderPipeline/RenderPasses/
├── ForwardPass.h/cpp
├── ShadowPass.h/cpp
├── PostProcess/
│   ├── SSAOPass.h/cpp
│   ├── SSGIPass.h/cpp
│   ├── BloomPass.h/cpp
│   ├── ToneMappingPass.h/cpp
│   └── BlurPass.h/cpp
└── Visibility/
    └── VisibilityPass.h/cpp
```

## 5. 关键代码变更
- `RenderGraphDefinitions.h`: 添加 `RGPassCategory`。
- `RenderGraphPass.h`: 增加 `SetCategory/GetCategory`。
- `RenderGraph.h`: `AddPass` 支持 Category 参数。
- `RenderGraphBuilder.h`: 增强 `Write` 支持 `UnorderedAccess`。

## 6. Shader 资源
- `SSAOShader.metal`: 已存在。
- `SSGIShader.metal`: 已存在。
- `BlurPass.metal`: 已存在。
- `PostProcess.metal`: 已存在，需检查是否包含 ToneMapping。
