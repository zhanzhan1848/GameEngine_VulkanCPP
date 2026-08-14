# Dawn WebGPU 后端实施路线图

## 概述

基于 Google Dawn 库的 WebGPU RHI 后端。macOS (Apple Silicon) 上以 Metal 为底层运行。
路线图按 **传统光栅 → PBR → GPU Driven → 对齐Metal做GI** 四个阶段推进。

---

## 前置: RHI 核心抽象层 ✅ 完成

14 个 RHI 组件全部实现并通过单元测试：
`Device / SwapChain / CommandBuffer / Buffer / Texture / Shader / Pipeline / PipelineLayout / DescriptorSet / DescriptorSetLayout / Sampler / Sync / Query / RenderPass`

引擎集成：`GraphicsPlatform / DawnInterface / CMake(ENABLE_WEBGPU) / EngineConfig / SceneDataAdapter`

11 个单元测试全部通过 (TestDawnRenderer)，TestDawnSponza 60FPS。

---

## Stage 1: 传统光栅渲染管线 ✅ 基本完成

将引擎的 RenderGraph / RenderPipeline 系统适配到 Dawn 后端，实现与 Metal 相同的传统光栅化流程。

### 1.0 RHI 补全 ✅ 完成
- [x] GenerateMipmaps — per-mip-level fullscreen triangle blit pass
- [x] Blit.wgsl fullscreen triangle shader
- [x] Blit pipeline lazy initialization in DawnDevice
- [x] getQueryPoolResults 日志抑制 (WebGPU 同步查询限制)

### 1.1 RenderGraph Dawn 适配 ✅ 基本验证通过
- [x] RenderGraph 与 DawnDevice 集成 (TestDawnRenderGraph 300帧 @ 60FPS)
- [x] RenderPass 资源屏障/转换 (WebGPU 不需要手动 barrier，已验证)
- [x] RenderGraph 对 Dawn 的 pass 执行调度
- [x] Pass 纹理导入 (import backbuffer)
- [ ] Pass 纹理创建/别名 (transient texture)
- [ ] 已知问题: shutdown 时 FreeList assertion (资源释放时序)

### 1.2 标准 Forward 渲染管线
- [x] Dawn 后端 UniformBufferDynamic 支持 (hasDynamicOffset=true, offset partitioning)
- [x] Dawn 持久映射缓冲区自动刷新 (FlushStaging before submit)
- [x] ForwardPBR.wgsl 生产着色器 (engine 3-set layout, Cook-Torrance PBR)
- [x] ForwardPBR_NoShadow.wgsl Dawn 专用着色器 (2-binding global layout, 无 CombinedImageSampler)
- [x] ForwardPass 委托 ForwardRenderer (HDR pipeline)
- [x] StandardRenderPipeline ForwardRenderer 集成 (HDR → ToneMapping → Present)
- [x] ForwardRenderer Dawn 端到端验证 (TestDawnForwardRenderer: 393 mesh Sponza, 392 纹理, PBR 材质, WASD/箭头键交互式相机)
- [x] DepthPrePass/DepthEqual 跳过 (Dawn 用普通 Less 深度测试)
- [x] Frustum::FromMatrix 行/列混淆修复 (影响全部 6 裁剪平面)
- [x] 近裁面 Metal [0,1] 深度公式修复 (col2 而非 col3+col2)
- [ ] 顶点缓冲区绑定 ( interleaved / separate buffer 支持 )
- [ ] 多子网格绘制 (mesh batch rendering)

### 1.3 后处理管线 ✅ 完成
- [x] FullScreenTriangle pass (WGSL 已写)
- [x] ToneMapping WGSL 着色器翻译完成
- [x] ToneMapping pass 集成 ✅ (DescriptorSet绑定、采样器、格式匹配全部验证通过)
- [x] Bloom pass ✅ (bright_pass WGSL + BlurPass compute + ToneMapping 合成, HDR→Bloom→ToneMap→Present 全管线验证)
- [x] SSAO pass ✅ (SSAO.wgsl trace+blur compute, SampledDepthImage RHI type, ToneMapping AO compositing, DepthStencil|ShaderResource texture usage)
- [x] SSGI pass ✅ (LumenSSGIDawnPass: Trace→HalfResDenoise→Filter→Temporal 4子pass compute管线, HZB ray march, 三缓冲持久纹理/常量缓冲)
- [x] HZB generation ✅ (compute mip chain, HZBGeneration.wgsl)
- [x] Velocity buffer ✅ (compute motion vectors, Velocity.wgsl)

### 1.4 ContentToEngine Dawn 纹理路径
- [x] `create_texture_resource()` Dawn/WebGPU GPU 上传路径 (wgpuQueueWriteTexture)
- [ ] 验证所有 Sponza 纹理通过引擎 content 系统正确加载

---

## Stage 2: PBR 渲染 🔜 进行中

Forward PBR 已有原型 (TestDawnSponza + ForwardPBR.wgsl)，需扩展为完整管线。

### 2.1 Forward PBR 完善 ✅ 基本完成
- [x] Cook-Torrance BRDF (albedo/normal/ORM/metallic/roughness)
- [x] 法线贴图 (tangent space normal reconstruction)
- [x] UV 翻转 + 法线解包字节序修复
- [ ] IBL (Image Based Lighting) - 环境反射/辐照
- [ ] 点光源/聚光灯支持 (当前仅方向光)

### 2.2 Deferred PBR
- [ ] GBuffer pass (WGSL - position/normal/albedo/depth)
- [ ] DeferredLighting.wgsl 完善
- [ ] Deferred vs Forward 切换策略

### 2.3 阴影 ✅ 基本完成 (单级联)
- [x] Shadow map pass ✅ (ShadowDepth.wgsl vertex-only depth pass, 2048x2048 D32_Float, front-face culling)
- [x] PCF 软阴影 ✅ (3x3 PCF in ForwardPBR_NoShadow.wgsl, textureLoad + 手动深度比较, bias=0.003)
- [x] ForwardRenderer Dawn shadow 集成 ✅ (SetDawnShadowResources/SetDawnShadowLightVP, global DS bindings 13/14)
- [ ] CSM (Cascaded Shadow Maps) - WGSL 实现
- [ ] PCSS 软阴影

---

## Stage 3: GPU Driven 📋 规划中

### 3.1 Meshlet 渲染
- [ ] Meshlet 数据结构 WGSL 适配
- [ ] GPU Culling (frustum + occlusion)
- [ ] Indirect draw (WebGPU indirect buffer)

### 3.2 Nanite 风格 LOD
- [ ] Cluster-based LOD 选择
- [ ] Continuous LOD transition
- [ ] DAG edge collapse on GPU

### 3.3 Virtual Geometry
- [ ] Streaming request / residency management
- [ ] Page-in / Page-out on Dawn

---

## Stage 4: 对齐 Metal 做 GI 📋 规划中

将引擎已有的 Lumen/DDGI/SurfaceCache 全局光照系统移植到 Dawn。

### 4.1 DDGI (Dynamic Diffuse Global Illumination)
- [ ] Probe grid WGSL compute shader
- [ ] Probe irradiance/depth update
- [ ] Probe relocation / classification
- [ ] 注意 Apple Silicon buffer read limits (参见 memory)

### 4.2 Surface Cache
- [ ] Surface cache capture pass (WGSL)
- [ ] Light evaluation (WGSL compute)
- [ ] Card system (findOwningCard 限 24 次 read)

### 4.3 Screen Space GI ✅ 完成
- [x] SSGI pass WGSL ✅ (LumenSSGIDawnPass: SSGITrace + SSGIHalfResDenoise + SSGIFilter + SSGITemporal, HZB ray march)
- [x] Denoiser (spatial + temporal) ✅ (5x5 Gaussian pre-smooth + bilateral spatial filter + temporal variance clipping)

### 4.4 Lumen 集成
- [ ] Radiance cache
- [ ] Final gather
- [ ] Temporal accumulation

---

## WGSL 着色器清单

| 着色器 | 用途 | Stage |
|--------|------|-------|
| `triangle.wgsl` | 基础三角形测试 | ✅ |
| `textured.wgsl` | 纹理采样测试 | ✅ |
| `compute.wgsl` | 计算着色器测试 | ✅ |
| `push_constants.wgsl` | Push常量模拟测试 | ✅ |
| `ForwardPBR.wgsl` | Forward PBR (engine 3-set) | ✅ |
| `ForwardPBR_NoShadow.wgsl` | Dawn 专用 Forward PBR (含 shadow map 采样) | ✅ |
| `ShadowDepth.wgsl` | 阴影深度 pass (vertex-only) | ✅ |
| `CommonTypes.wgsl` | 共享类型定义 | ✅ |
| `CommonFunction.wgsl` | 共享函数 | ✅ |
| `FullScreenTriangle.wgsl` | 全屏三角形 | ✅ |
| `Blit.wgsl` | Blit/Mipmap生成 | ✅ |
| `Bloom.wgsl` | Bloom亮度提取 | ✅ |
| `BlurPass.wgsl` | 通用模糊 compute | ✅ |
| `SSAO.wgsl` | SSAO trace compute | ✅ |
| `SSAOBlur.wgsl` | SSAO bilateral blur compute | ✅ |
| `SSGITrace.wgsl` | SSGI HZB ray march (half-res) | ✅ |
| `SSGIHalfResDenoise.wgsl` | SSGI Gaussian pre-smooth (half-res) | ✅ |
| `SSGIFilter.wgsl` | SSGI bilateral spatial filter (full-res) | ✅ |
| `SSGITemporal.wgsl` | SSGI temporal accumulation | ✅ |
| `HZBGeneration.wgsl` | HZB mip chain compute | ✅ |
| `Velocity.wgsl` | 运动向量 compute | ✅ |
| `ToneMapping.wgsl` | ACES色调映射 | ✅ |
| `SSRPass.wgsl` | 屏幕空间反射 compute | ✅ |
| `DeferredLighting.wgsl` | 延迟光照 | 📝 框架 |

---

## 已知问题 & 修复记录

详见 [Dawn_WebGPU_Lessons.md](Dawn_WebGPU_Lessons.md)

- ✅ Normal/Tangent 解包字节序修正 (低16位=x, 高16位=y)
- ✅ UV V-flip 添加 (1.0 - uv.y 匹配 Metal 行为)
- ✅ 元素缓冲区 24 字节 padding 处理 (simd::float2 8 字节对齐)
- ✅ Lambda `[&]` 捕获函数参数导致悬垂引用 (execute lambda 必须 value-capture RGResourceHandle)
- ✅ SamplerDesc.comparisonFunc 默认 Always 导致 WebGPU 比较采样器不兼容
- ✅ TextureView 格式必须匹配源纹理格式 (WebGPU 严格校验 BGRA8 vs RGBA8)
- ✅ LoadAction::DontCare 映射到 WGPULoadOp_Clear (WebGPU 无 Undefined loadOp)
- ✅ Timestamp query feature 未启用时降级为 Occlusion + writeTimestamp 跳过
- ✅ UniformBufferDynamic 未支持 → 添加 hasDynamicOffset=true + dynamic offset partitioning
- ✅ 持久映射 constant buffer GPU 不见更新 → FlushStaging before wgpuQueueSubmit
- ✅ RenderGraph execute lambda 捕获函数参数引用会悬垂 → 通过 PassData struct 传递指针
- ✅ SamplerDesc.comparisonFunc 默认 Always → Dawn 端设为 Never (filtering sampler)
- ✅ DepthPrePass + DepthEqual 导致 Dawn 无几何绘制 → 跳过 DepthPrePass 用普通 Less
- ✅ RenderMesh registry 空 → SceneDataAdapter 传 meshEntityId 而非 invalid_id
- ✅ UniformBufferDynamic 缺失 BuildBindGroup case → 添加 dynamic buffer switch case
- ✅ Frustum::FromMatrix 列加法 vs 行加法 → column-major 矩阵须用 row extraction
- ✅ 近裁面 OpenGL vs Metal 深度范围 → Metal [0,1] 用 col2 而非 col3+col2
- ✅ BloomPass empty pipeline layout → 添加 SampledImage+Sampler DSL, 绑定输入 HDR 纹理
- ✅ BloomPass frameIndex 硬编码 0 → 从 AddBloomPass 参数传入
- ✅ BloomPass 未接入 StandardRenderPipeline → 在 ForwardPass 和 ToneMappingPass 之间插入
- ✅ BlurPass.wgsl rgba32float 不可过滤 → 改为 rgba16float
- ✅ DawnDescriptorSetLayout StorageImage 硬编码 RGBA32Float → 改为 RGBA16Float
- ✅ Material render target format 需匹配 HDR 纹理格式 (RGBA16_Float)
- ✅ DepthTexture 缺少 TextureBinding usage → DepthStencil|ShaderResource (SSAO 采样深度纹理)
- ✅ 新增 DescriptorType::SampledDepthImage — WGSL texture_depth_2d 需要 sampleType=Depth
- ✅ DawnDescriptorSetLayout SampledDepthImage → WGPUTextureSampleType_Depth
- ✅ DawnDevice/DawnDescriptorSet BuildBindGroup SampledDepthImage 走 textureView 路径
- ✅ SSGI minBindingSize=0 导致 Dawn 推断 4GB buffer → DescriptorSetLayoutBinding 新增 minBindingSize 字段
- ✅ PresentPass 每帧泄漏 WGPUTextureView/WGPUBindGroup → 改用物理句柄 + 三缓冲延迟销毁
- ✅ Shadow matrix 顺序 V*P → P*V 匹配引擎 viewProjectionMatrix_ 约定
- ✅ ForwardPBR_NoShadow.wgsl 添加 Group 0 binding 13/14 shadow sampling (3x3 PCF + textureLoad)
- ✅ ForwardRenderer Dawn shadow 集成 (SetDawnShadowResources, SetDawnShadowLightVP, skipShadows)
- ✅ DawnDescriptorSetLayout 使用 binding.minBindingSize 替代硬编码 0

## 构建说明

```bash
cmake -B build -DENABLE_WEBGPU=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target TestDawnForwardRenderer
./Darwin/Debug/TestDawnForwardRenderer
```

## 依赖

- Dawn: `third_party/dawn/` (git submodule)
- Tint: 随 Dawn 一起编译 (WGSL→MSL)
- abseil: Dawn 依赖
