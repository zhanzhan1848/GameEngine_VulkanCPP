# VSM (Variance Shadow Maps) & Poisson Disk Sampling 实现方案

## 1. 概述
本文档详细描述了在 RHI 层实现 Poisson Disk Sampling 和 Variance Shadow Maps (VSM) 的技术方案。
目标是解决传统 Shadow Map 的锯齿问题（通过 Poisson Sampling）和 Light Bleeding 问题（通过 VSM + Chebyshev 不等式），并提供高质量的软阴影。

## 2. 模块设计

### 2.1 采样算法 (Poisson Disk Sampling)

#### 2.1.1 资源生成 (`SamplingUtils`)
新建工具类 `Engine/Graphics/RHI/Utils/SamplingUtils.h/cpp`，负责：
1.  **生成 Poisson Disk 样本**:
    *   在单位圆内生成均匀分布的随机点。
    *   支持不同采样数量（如 16, 32, 64）。
    *   返回 `std::vector<math::v2>`。
2.  **生成 Noise Texture 数据**:
    *   生成包含随机旋转角度（或旋转向量）的纹理数据。
    *   格式: `RG16_SNorm` (cos, sin) 或 `R32_Float` (angle)。
    *   大小: 通常 4x4, 16x16 或 64x64，平铺使用。

#### 2.1.2 Shader 实现
扩展 `ShadersCommonHeaders.h`：
*   添加 `PoissonDiskSamples` 常量数组（Uniform Buffer 或 Shader 常量）。
*   添加 `SamplePoissonShadow` 函数：
    *   输入: `sampler2D shadowMap`, `vec2 uv`, `float currentDepth`, `sampler2D noiseTexture`, `float searchRadius`。
    *   逻辑:
        1.  采样 Noise Texture 获取旋转矩阵。
        2.  遍历 Poisson Samples，应用旋转。
        3.  采样 Shadow Map 并比较深度。
        4.  返回平均遮挡值。

### 2.2 Variance Shadow Maps (VSM)

#### 2.2.1 Shadow Pass 改造
修改 `ForwardRenderer::ShadowPass` 及相关逻辑：
1.  **RenderTarget 格式**:
    *   原: Depth Only (`D32_Float` 或 `D16_UNorm`).
    *   新: Color (`RG32_Float`) + Depth (`D32_Float`).
    *   Color R 通道存储深度 $d$。
    *   Color G 通道存储深度平方 $d^2$。
2.  **Shader 修改 (`shadowmapping.frag`)**:
    *   输出 `outColor = vec2(depth, depth * depth);`
    *   利用偏导数 `dFdx`, `dFdy` 优化 moments 计算（可选，减少锯齿）。

#### 2.2.2 Blur Pass 架构
新建 `ShadowBlurPass` 类：
1.  **算法**: Two-Pass Gaussian Blur (Horizontal + Vertical)。
2.  **实现方式**: Compute Shader。
3.  **资源管理**:
    *   需要 Ping-Pong Buffer（两个 `RG32_Float` 纹理）。
    *   或者使用 Shared Memory 优化的单 Pass Compute Shader（如果可能）。
4.  **Shader**:
    *   `blur.comp`: 接受输入纹理，输出模糊后的纹理。
    *   Kernel Size 和 Sigma 可配置。

#### 2.2.3 光照计算 (Lighting Pass)
修改光照 Shader：
1.  **Chebyshev 上界计算**:
    *   采样模糊后的 VSM 纹理获取 $(E(d), E(d^2))$。
    *   计算方差 $\sigma^2 = E(d^2) - E(d)^2$。
    *   应用 Chebyshev 不等式计算阴影强度。
    *   应用 `MinVariance` 阈值解决数值不稳定性。
    *   应用 `LightBleedingReduction` 算法。

## 3. 详细实施计划

### Phase 1: 基础设施与采样工具
1.  实现 `SamplingUtils`。
2.  在 `ShadersCommonHeaders.h` 中添加数学帮助函数。
3.  **验证**: 单元测试生成的数据分布。

### Phase 2: Shadow Pass 升级
1.  修改 `RenderView` 或 `ShadowPass` 支持 Color Attachment。
2.  修改 `shadowmapping` shader 输出 moments。
3.  **验证**: 渲染场景，RenderDoc 检查 Shadow Map 内容是否为红绿渐变图（深度和深度平方）。

### Phase 3: Blur Pass
1.  编写 `Gaussian Blur` Compute Shader。
2.  实现 C++ 端的 Dispatch 逻辑。
3.  **验证**: 对 Phase 2 生成的 Shadow Map 进行模糊，检查结果。

### Phase 4: 整合与 Shader 采样
1.  在光照 Shader 中实现 VSM 采样逻辑。
2.  整合 Poisson Sampling 作为可选的高质量过滤（用于 VSM 之前的 PCF 或 VSM 之后的过滤）。
    *   *注*: VSM 主要依赖预模糊，Poisson Sampling 主要用于 PCF。两者结合可以是 "Poisson Sampled VSM" 或者二选一。通常 VSM 不需要复杂的 PCF，但可以结合 Poisson Sampling 进一步提升软阴影质量。
3.  **验证**: 最终场景渲染测试。

## 4. 数据结构变更预览

### 4.1 C++ 端
```cpp
// Engine/Graphics/RHI/Utils/SamplingUtils.h
namespace primal::graphics::utils {
    struct PoissonDiskGenerator {
        static std::vector<math::v2> Generate(uint32_t count);
        static std::vector<uint8_t> GenerateNoiseTexture(uint32_t size); // Returns RG8 or RG16 data
    };
}
```

### 4.2 Shader 端
```glsl
// ShadersCommonHeaders.h
float VSM_Chebyshev(vec2 moments, float currentDepth, float minVariance) {
    // ...
}

float SampleShadow_Poisson(sampler2D map, vec2 uv, float z, sampler2D noise, ...) {
    // ...
}
```

## 5. 待确认细节
1.  **现有 Descriptor Set**: `GlobalShaderData` 和 `ForwardLightBuffer` 是否有足够的槽位绑定新的 Noise Texture？
    *   *方案*: 可能需要新增一个 `Sampler` 绑定点或者使用 Bindless 架构（如果支持）。目前看来可以在 `GlobalDescriptorSet` 中增加绑定。
2.  **兼容性**: 是否保留旧的 PCF Shadow Map？
    *   *建议*: 通过宏定义或材质选项切换。

