# 着色器重构说明文档

## 概述
本次重构旨在统一 Metal 着色器中的通用代码，减少冗余，并提高代码的可维护性。我们将分散在各个着色器文件中的通用结构体、常量和工具函数提取到了一个新的统一头文件中。

## 新增文件
*   **`Engine/Graphics/RHI/Shaders/RHIShaderCommon.metal`**: 包含通用的着色器定义和工具函数。
    *   **通用结构体**: `GlobalShaderData`, `PerObjectData`, `LightParameters`, `DirectionalLightParameters`, `ForwardLightBuffer` 等，与 C++ 层的 `RHIShaderCommon.h` 保持一致。
    *   **阴影采样函数**: `SampleShadowPCF`, `SampleShadowVSM`, `SamplePointShadow`, `SamplePointShadowVSM`, `ChebyshevUpperBound`。
    *   **工具函数**: `GetFullScreenTrianglePosUV` (全屏三角形生成), `InterleavedGradientNoise` (随机噪声), `ToneMapReinhard` (色调映射)。

## 修改文件
1.  **`EngineTest/shaders/TestShader_CSM.metal`**
    *   移除了本地定义的 `GlobalShaderData` 等结构体，改为包含 `RHIShaderCommon.metal`。
    *   移除了本地定义的阴影采样函数 (`SampleShadowPCF`, `SampleShadowVSM` 等)，改用通用头文件中的实现。
    *   代码量显著减少，逻辑更加清晰。

2.  **`EngineTest/shaders/TAA.metal`**
    *   引入 `RHIShaderCommon.metal`。
    *   使用 `GetFullScreenTrianglePosUV` 替换了手动计算全屏三角形顶点位置和 UV 的代码。

3.  **`EngineTest/shaders/MultiView.metal`**
    *   引入 `RHIShaderCommon.metal`。
    *   使用 `ToneMapReinhard` 替换了手动编写的 Reinhard 色调映射代码。

4.  **`EngineTest/shaders/SimpleColor.metal`**
    *   引入 `RHIShaderCommon.metal` 以便未来使用通用类型。

## 验证
*   已检查所有修改后的文件，确保引用的相对路径正确 (`../../Engine/Graphics/RHI/Shaders/RHIShaderCommon.metal`)。
*   确认通用函数的签名与调用处匹配。
*   确认结构体定义与 C++ 层 (`Engine/Graphics/RHI/Core/RHIShaderCommon.h`) 保持一致。
