# 实施计划: TAA 接线 + SurfaceCache/ScreenProbe 移植 (全部手写 GLSL)

工作目录: `.worktrees/vulkan-rhi/`

## 第一批: TAA 接线

### T1. 手写 `Engine/Graphics/Vulkan/shaders/Lumen/TAA.comp` (GLSL)
从 TAA.wgsl 翻译为 GLSL 450 compute,workgroup 8×8,entry point `taa_main`。用 `texelFetch` 做手动 bilinear(与 WGSL 一致,避免需要 sampler binding)。5 个 binding:0-2 SampledImage,3 StorageImage(rgba16f),4 UniformBuffer。
### T2. 删除 naga 编译的 `TAA.spv`,改为 `.comp.spv`
build_spv.sh:把 TAA 从 `WGSL_SHADERS` 移到 `LUMEN_GLSL_*` 数组(entry=`taa_main`, stage=`comp`)。编译生成 `Lumen/TAA.comp.spv`。
### T3. TAAPass.cpp 改 shader 加载路径
当前用 `ShaderRegistry::GetShaderPath(platform, "TAA")` 返回 `TAA.spv`(顶级)。改为加载 `Lumen/TAA.comp.spv`(与 SSAO/SSGI/DDGI 一致)。或保持顶级但确保加载 `.comp.spv`。
### T4. GPUDrivenDraw jitter (保持 WGSL)
- WGSL `DrawConstants` 加 `jitter: vec2<f32>` + pad
- WGSL vertex shader line 282 后:`out.position.xy += uniforms.jitter * out.position.w`
- C++ `DrawConstants` 同步加 `math::v2 jitter` + pad
- C++ 填充 `utils::GetJitterOffset(frame_index, w, h)`
- 重编译 `GPUDrivenDraw.spv`
### T5. BuildRenderGraph 接入 AddTAAPass
include TAAPass.h;Step 10.5 改为 TAA → Bloom → ToneMapping;velocity 用 `gpuDraw.GetGBufferVelocity()`。

---

## 第二批: SurfaceCache (6 GLSL shader + C++ 改造)

### SC1. 手写 6 个 GLSL shader
- `SurfaceCacheCapture.vert` + `.frag`(graphics,4 MRT)
- `SurfaceCacheDilate.comp`(8×8,hole-fill)
- `SurfaceCacheLightCull.comp`(8×8,tile light assignment)
- `SurfaceCacheLightEval.comp`(256×1,direct lighting)
- `SurfaceCacheIndirectTrace.comp`(8×8,SDF sphere trace)
- `SurfaceCacheIndirectResolve.comp`(8×8,radiance resolve)

共享 struct/helper(`SurfaceCacheData.glsl`):`cardTexelToWorld`, `worldToCardUV`, `octEncode`, `octDecode`, `hash`,以及 struct 定义。

### SC2. 改造 SurfaceCachePass.cpp
- `LoadShaderBytecode`:加 device 参数 + Vulkan SPIR-V binary loader
- 移除 Vulkan gate(line 184-188)
- `CreateDescriptorSetLayouts`:6 个 layout 加 isVk flat-binding 分支
- `CreatePipelines`:shader loading 走平台分支
- `AddPass` execute lambda:实现缺失的 5 个 sub-pass dispatch(Capture→Dilate→LightCull→IndirectTrace→IndirectResolve,LightEval 已有)

### SC3. build_spv.sh + 编译验证

---

## 第三批: ScreenProbe (7 GLSL shader + C++ 改造)

### SP1. 手写 7 个 GLSL shader
Place, TraceRays(monolithic), Average(threadgroup shared memory reduction), Temporal, SpatialFilter, Gather(2D), Denoise(2D)。

### SP2. 改造 ScreenProbeGIPass.cpp
同 SC2 模式:gate 移除、loader 改造、9 个 layout 的 isVk flat-binding、shader loading 平台分支。

### SP3. build_spv.sh + 编译验证

---

## 第四批: SCDDGIIntegration (2 GLSL shader + 实现 stub)

`DDGICardRadianceAvg.comp` + `DDGIProbeIrradianceFromCards.comp`;实现 `SCDDGIIntegrationModule.cpp` 的空 execute lambda。

---

## 总计
- 新手写 GLSL shader: 1(TAA)+ 6(SC)+ 7(SP)+ 2(SCDDGI)= **16 个**
- C++ 改造: 4 个 pass 文件 + StandardRenderPipeline + GPUDrivenDrawPipeline + build_spv.sh + TAAPass.cpp
- WGSL 改动: GPUDrivenDraw.wgsl(仅加 jitter)

## 验证
每批:① build_spv.sh(0 FAIL)② ninja Engine ③ TestVulkanStandardPipelineSmoke