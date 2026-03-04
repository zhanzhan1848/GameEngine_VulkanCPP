# Image Based Lighting (IBL) 实现方案

## 1. 概述

Image Based Lighting (IBL) 是现代 PBR 渲染引擎中实现真实感光照的关键技术。它通过使用环境贴图（Environment Map）作为光源，模拟来自周围环境的复杂光照，特别是对于金属材质和粗糙表面的反射表现至关重要。

本方案基于 Epic Games 的 "Real Shading in Unreal Engine 4" (Karis 2013) 中提出的 **Split Sum Approximation** 方法。

## 2. 数学原理

渲染方程（反射部分）：

$$ L_o(p, \omega_o) = \int_{\Omega} f_r(p, \omega_i, \omega_o) L_i(p, \omega_i) (n \cdot \omega_i) d\omega_i $$

为了实时计算，我们使用 Split Sum Approximation 将积分拆分为两个部分：

$$ \int_{\Omega} f_r(p, \omega_i, \omega_o) L_i(p, \omega_i) (n \cdot \omega_i) d\omega_i \approx \left( \int_{\Omega} L_i(p, \omega_i) d\omega_i \right) \left( \int_{\Omega} f_r(p, \omega_i, \omega_o) (n \cdot \omega_i) d\omega_i \right) $$

这实际上并不完全准确，Epic 的方法是针对 Specular项进行的拆分：

$$ \int_{\Omega} L_i(p, \omega_i) f_r(p, \omega_i, \omega_o) (n \cdot \omega_i) d\omega_i \approx \left( \sum_{k=1}^{N} \frac{L_i(l_k) f_r(l_k, v, h) (n \cdot l_k)}{p(l_k, v)} \right) $$

Epic 的 Split Sum Approximation 具体形式为：

$$ L_o(p, \omega_o) \approx \underbrace{\left( \frac{1}{N} \sum_{k=1}^{N} L_i(\omega_k) \right)}_{\text{Prefiltered Environment Map}} \times \underbrace{\left( \int_{\Omega} f_r(p, \omega_i, \omega_o) (n \cdot \omega_i) d\omega_i \right)}_{\text{BRDF Integration}} $$

### 2.1 Diffuse IBL (Irradiance Map)

对于漫反射（Diffuse），$f_r = \frac{c}{\pi}$ 是常数（Lambertian）。
我们需要计算半球上的辐照度（Irradiance）：

$$ E(p) = \int_{\Omega} L_i(p, \omega_i) (n \cdot \omega_i) d\omega_i $$

这可以通过对环境贴图进行卷积（Convolution）预计算得到 **Irradiance Map**。运行时直接根据法线 $N$ 采样。

### 2.2 Specular IBL

对于镜面反射（Specular），由于 $f_r$ 依赖于视角和粗糙度，我们将其拆分为两个预计算部分：

1.  **Prefiltered Environment Map**:
    *   对环境贴图进行预滤波，模拟不同粗糙度下的模糊反射。
    *   存储在 Cubemap 的 Mipmap 链中。Mip 0 是原始环境图（Roughness=0），高 Mip 代表高 Roughness。
    *   计算时假设 $V = N = R$（零入射角假设），以简化依赖。

2.  **BRDF Integration LUT (Look Up Table)**:
    *   预计算 BRDF 积分部分。
    *   由于去除了 $L_i$，剩下的积分项只依赖于 `Roughness` 和 `NdotV`。
    *   我们可以将其预计算为一个 2D LUT（纹理），横轴为 `NdotV`，纵轴为 `Roughness`。
    *   结果包含两个通道 (Red, Green)，分别代表 Scale 和 Bias：$F_0 \cdot \text{Scale} + \text{Bias}$。

## 3. 预计算流程 (Pre-computation)

所有预计算步骤均使用 Compute Shader 进行。

### 3.1 资源准备

*   **输入**: HDR Environment Map (通常为 .hdr 或 .exr 格式，Equirectangular 投影)。
*   **输出**:
    1.  `IrradianceMap` (Cubemap, RGB16F, size 32x32 或 64x64)。
    2.  `PrefilteredMap` (Cubemap, RGB16F, size 512x512, 带 Mipmaps)。
    3.  `BRDF_LUT` (Texture2D, RG16F, size 512x512)。

### 3.2 步骤 1: Equirectangular to Cubemap (可选)

如果源是 Equirectangular，先转为 Cubemap Face。
*   **Shader**: `IBL_EquirectToCube.compute`
*   **Dispatch**: 6次（每个面），或者一次 Dispatch处理所有面（Geometry Shader 或 Layered Rendering）。

### 3.3 步骤 2: Generate Irradiance Map

*   **原理**: 对半球内的光照进行加权平均。
*   **Shader**: `IBL_IrradianceConvolution.compute`
*   **采样**: 使用均匀采样或余弦加权采样。
*   **伪代码**:
    ```glsl
    // 对每个 texel (代表一个法线方向 N)
    vec3 irradiance = vec3(0.0);
    vec3 up = vec3(0.0, 1.0, 0.0);
    vec3 right = cross(up, N);
    up = cross(N, right);

    for(float phi = 0.0; phi < 2.0 * PI; phi += sampleDelta) {
        for(float theta = 0.0; theta < 0.5 * PI; theta += sampleDelta) {
            // 球坐标转笛卡尔坐标 (Tangent Space)
            vec3 tangentSample = vec3(sin(theta) * cos(phi),  sin(theta) * sin(phi), cos(theta));
            // 变换到 World Space
            vec3 sampleVec = tangentSample.x * right + tangentSample.y * up + tangentSample.z * N;

            irradiance += texture(environmentMap, sampleVec).rgb * cos(theta) * sin(theta);
            nrSamples++;
        }
    }
    irradiance = PI * irradiance * (1.0 / float(nrSamples));
    ```

### 3.4 步骤 3: Generate Prefiltered Environment Map

*   **原理**:基于 Roughness 使用 Importance Sampling (GGX) 对环境图进行卷积。
*   **Shader**: `IBL_SpecularPrefilter.compute`
*   **Mipmap**: 对每个 Mip Level 进行一次 Dispatch，传入对应的 Roughness。
    *   `Roughness = (float)mipLevel / (float)(maxMipLevels - 1);`
*   **伪代码**:
    ```glsl
    // Hammersley Sequence for low-discrepancy sampling
    // ImportanceSampleGGX 生成采样向量 H
    // L = 2 * dot(V, H) * H - V
    // Accumulate Color
    ```

### 3.5 步骤 4: Generate BRDF LUT

*   **原理**: 对 BRDF 积分公式进行数值积分。
*   **Shader**: `IBL_BRDFIntegration.compute`
*   **输出**: 2D 纹理 (NdotV, Roughness)。
*   **伪代码**:
    ```glsl
    // 输入: UV (x=NdotV, y=Roughness)
    // 循环采样 (Hammersley)
    // 计算 G * F * V / (N.H * N.V) 等项
    // 累加 A (Scale) 和 B (Bias)
    return vec2(A, B);
    ```

## 4. 运行时应用 (Runtime PBR Shader)

在 PBR Shader (`RHIShaderPBR.metal` 或主 Shader) 中添加 IBL 计算函数。

```metal
// 菲涅尔项 (Schlick with Roughness)
float3 fresnelSchlickRoughness(float cosTheta, float3 F0, float roughness)
{
    return F0 + (max(float3(1.0 - roughness), F0) - F0) * pow(1.0 - cosTheta, 5.0);
}

// 计算 IBL 贡献
float3 CalculateIBL(SurfaceSurface surface, float3 N, float3 V, float3 R)
{
    float NdotV = max(dot(N, V), 0.0);
    
    // 1. Diffuse IBL
    // F0 是基础反射率
    float3 F = fresnelSchlickRoughness(NdotV, surface.F0, surface.roughness);
    
    float3 kS = F;
    float3 kD = 1.0 - kS;
    kD *= 1.0 - surface.metallic; // 金属没有 Diffuse
    
    float3 irradiance = IrradianceMap.sample(sampler, N).rgb;
    float3 diffuse = irradiance * surface.albedo;
    
    // 2. Specular IBL
    // 采样 Prefiltered Map，根据 Roughness 选择 Mip
    const float MAX_REFLECTION_LOD = 4.0;
    float3 prefilteredColor = PrefilteredMap.sample(sampler, R, level(surface.roughness * MAX_REFLECTION_LOD)).rgb;
    
    // 采样 BRDF LUT
    float2 envBRDF = BRDFLut.sample(sampler, float2(NdotV, surface.roughness)).rg;
    
    float3 specular = prefilteredColor * (F * envBRDF.x + envBRDF.y);
    
    return (kD * diffuse + specular) * surface.ao;
}
```

## 5. 开发计划

1.  **基础架构准备**:
    *   确认 RHI 支持 Compute Shader 创建和 Dispatch。
    *   确认 RHI 支持 Texture Cube 和 UAV 写入 (或者通过 Render Target 写入)。
2.  **Shader 实现**:
    *   编写 `IBL_IrradianceConvolution.metal`
    *   编写 `IBL_SpecularPrefilter.metal`
    *   编写 `IBL_BRDFIntegration.metal`
    *   编写通用辅助函数 `Hammersley.metal`
3.  **C++ 预计算管线**:
    *   创建 `IBLPrecomputer` 类。
    *   实现加载 HDR 纹理。
    *   实现资源创建 (Cubemaps, LUT)。
    *   实现 Command Buffer 录制和执行。
4.  **PBR Shader 更新**:
    *   修改现有 PBR Shader，增加 IBL 纹理绑定。
    *   集成 `CalculateIBL` 函数。
5.  **测试**:
    *   加载一个球体和 HDR 环境图。
    *   验证不同 Roughness/Metallic 下的表现。

## 6. 注意事项

*   **Coordinate System**: 确保 Cubemap 的坐标系与引擎一致 (左手/右手系，Y-up/Z-up)。
*   **Gamma Correction**: 确保 IBL 计算在线性空间进行，HDR 纹理通常是线性的。
*   **Performance**: 预计算只在编辑器加载或场景初始化时进行一次。运行时只有采样开销。
