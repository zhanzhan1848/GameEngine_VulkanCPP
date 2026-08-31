# Vulkan 后端补齐 TAA / VSM+Blur / Mirror / GeometryDebug / Toon 实施计划

**Worktree**: `.worktrees/vulkan-rhi`(分支 `feat/vulkan-rhi-backend`,基于 `345eaba`)
**已确认方向**:VSM 走 ShadowMapModule 升级(双平台统一);Mirror 做进 StandardRenderPipeline;范围 = TAA、VSM+Blur、Mirror、GeometryDebug、Toon。

## Shader 编写规范(硬性约定)

- **所有新增 Vulkan shader 一律手写 GLSL**(`#version 450`,与 Metal 参考源码 1:1 对应移植),放入 `Engine/Graphics/Vulkan/shaders/` 对应子目录。
- 编译一律走 `build_spv.sh` 的 **glslangValidator 路径**:vert/frag 对加入 `GLSL_NAMES` 列表,compute 加入 `LUMEN_GLSL_NAMES` 或新建 `POSTPROCESS_GLSL_NAMES` 列表;共享函数通过 `#include "RHIShaderFunctions.glsl"` + awk inline 展开复用。
- **禁止新增 WGSL→naga 转换**;存量 naga 产物(`GPUDrivenDraw.spv`、`Nanite/*.spv` 等)保持不动、不重编。TAA 需要的 jitter 已在存量 GPUDrivenDraw.spv 里,无需改 WGSL。
- 产物 `.spv` 二进制 commit 进仓库(沿用 `81663d2` 流程);spv 运行时加载,无需 CMake 登记(Sponza 测试 POST_BUILD 已整目录拷贝)。
- 每个 workstream 独立提交;完成后跑全量 Vulkan 回归。

---

## Workstream 1 — TAA 启用(Vulkan,~1 session)

基础设施已就绪(`GPUDrivenDrawPipeline` jitter/prev_jitter 字段、spv 侧 jitter 应用、velocity RG16F、`Lumen/TAA.comp.spv` 手写 GLSL 已存在),缺的是开闸与修复。

1. **修复 `RenderPasses/PostProcess/TAAPass.cpp`**:
   - `EnsurePipeline` 只建一次 → 分辨率变化时重建 `s_HistoryTex[3]` 三槽历史纹理;
   - `StandardRenderPipeline` 从不调 `ResetTAAHistory()` → 相机跳变/场景重载路径接入(Sponza 测试相机 reset 处);
   - 检查 RG Compute pass 内 `BlitTexture(out→history)` 与 RG 状态跟踪交错,Vulkan 侧补必要 barrier/layout 转换;清理 :190-191 重复注释。
2. **开启 jitter**:`StandardRenderPipeline.cpp:1488` 与 `:2087` 两处取消注释 `gpuDraw.SetJitterEnabled(true)`。
   - 该开关不分平台,而 Metal 端 `ShaderRegistry` 指向的 `Metal/shaders/TAA.metal` 不存在(Metal TAA 现为坏路径)。**补 `Engine/Graphics/Metal/shaders/TAA.metal`**(compute 版,从 `TAA.comp` 反向移植,绑定 0-4 同构),使两端 TAA 都能跑、jitter 无条件开启;若受阻则临时按平台 gate。
3. **测试**:新增 `EngineTest/UnitTests/RHI/Core/TestVulkanTAA.cpp`(照 TestVulkanToneMap 骨架:静态图 + velocity=0 → 输出≈输入;两帧合成 → 历史混合正确;零 validation 错误),注册进 `EngineTest/UnitTests/CMakeLists.txt` 的 `if(ENABLE_VULKAN)` 块;Sponza 窗口验证无 shimmer。

## Workstream 2 — GeometryDebugPass Vulkan 化(~1 session)

1. **手写 GLSL 移植 4 个 debug shader**:`RHI/Shaders/Debug/{MeshletDebug,SDFDebug,VectorFieldDebug,VoxelDebug}.metal`(420 行,顶点拉取+片元)→ `Vulkan/shaders/Debug/*.vert/frag.glsl`,入口名与 cpp 现有期望一致(`meshlet_debug_vs` 等),`DebugUniforms` 160B std140 对齐;加入 `build_spv.sh` GLSL_NAMES。
2. **修加载器**:`GeometryDebugPass.cpp:80-90` 硬编码绝对路径(现指向主 worktree)改平台分支加载(Vulkan→spv 二进制,照 `LineBatchRenderer.cpp:54-112` 模式;Metal→相对路径解析)。
3. **修 RT 格式不一致**:Initialize 写死 RGBA16_Float vs immediate 传 BGRA8,统一参数化/从 RG 派生。
4. **接线**:拆开 `ForwardRenderer.cpp:2570` 的 skipShadows gate;`StandardRenderPipeline` 加可选 GeometryDebug 步骤(settings 成员 + `AddGeometryDebugPass` RG 版插 FinalBlit 前),Sponza 测试加热键切换(参照 TestGeometryDebugSponza F2-F5)。

## Workstream 3 — Toon 后处理(~0.5-1 session)

`ToonShader.metal` 是 compute 后处理(颜色量化 + depth Sobel 边缘),`RenderPasses/PostProcess/ToonPass.cpp` 骨架已搭好但 pipeline/dispatch 全被注释、无调用者——直接填空。

1. **手写 `Vulkan/shaders/PostProcess/Toon.comp`**(1:1 移植 ToonShader.metal:bindings 0-3 = color/depth/normal 采样 + StorageImage 输出,`ToonConstants{edgeThreshold,colorLevels}` 走 UBO,entry main),加进 build_spv.sh compute 列表。
2. **填空 `ToonPass.cpp`**:补 pipeline 创建与 dispatch,shader 加载平台分支(Vulkan spv / Metal `Metal/shaders/ToonShader.metal` 入口 `toon_main`)。
3. **接线**:`StandardRenderPipeline` PostProcess 链 ToneMapping 之后、FinalBlit 之前调 `AddToonPass`(输入 `gpuDraw.GetGBufferDepthSampleable()`/`GetGBufferNormal()`,RG 导入照 `"GBufferVelocity_TAA"` 模式),settings 开关默认关 + Sponza 热键。
4. **测试**:`TestVulkanToon.cpp` — 平面色 + 深度梯度 → 断言量化颜色与边缘像素(CPU 参考确定性)。

## Workstream 4 — VSM + Blur:ShadowMapModule 升级(双平台,~2-3 sessions)

保留模块 RenderGraph 骨架与 cascade VP 缓存,替换滤波链:D32→R32 blit→ShadowFilter(R8) 改为 **RG32 moments → BlurPass → Chebyshev 采样**。共享函数已就绪(`RHIShaderFunctions.glsl/.metal` 手写版 `ChebyshevUpperBound`/`SampleShadowVSM`)。

1. **Moments 光栅 shader 新三件**(不动 ShadowDepth 现有 depth-only 路径):
   - `EngineTest/shaders/ShadowMoments.metal`(Metal 端源,参考 `TestShader_CSM.metal:228-268` `fragmentShadowVSM`:输出 `vec2(depth01, depth01²)`,vertex-pulling 绑定同 ShadowDepth.metal);
   - `Vulkan/shaders/Nanite/ShadowMoments.vert/.frag.glsl`(**手写 GLSL**,glslangValidator 编译)。
2. **GPUDrivenDrawPipeline 增 `ExecuteShadowMomentsRaster`**(照 `ExecuteShadowRaster` .cpp:3320-3420 模式):CPU 剔除复用 `ExtractFrustumPlanes`,RT 改 **RG32_Float 2DArray 2048×cascade 数**,RenderPassDesc colorAttachment `arrayLayer=cascade`(先例:ForwardRenderer ShadowPass / TestVulkanShadowPass)。
3. **ShadowMapModule.AddPasses 改造**:每 cascade ShadowCulling_C(保留)→ ShadowMoments_C(RG32 layer);末尾用现有 `Passes/BlurPass.cpp`(平台无关、支持层数、BlurPass.spv/BlurPass.metal 已在)做 H+V ping-pong(radius 3 / sigma 1.0,与 Metal VSM 一致),替换 ShadowFilter 位置;ShadowFilter(R8) 保留仅当 Froxel 等 visibility 消费者激活时执行(默认跳过)。
4. **采样端切 VSM**:
   - Vulkan `Vulkan/shaders/Forward/DeferredLighting.frag`:新增 moments 2DArray + sampler 绑定(binding 7/8 分立式),`#include RHIShaderFunctions.glsl`,Chebyshev 替换 binding 6 R8 单 tap;
   - Metal `Metal/shaders/Forward/DeferredLighting.metal:206` 同步(texture(6)→moments + `RHIShaderFunctions.metal`);
   - cascade 数本轮维持 2(directional),4+spot+point 留后续。
5. **开关**:ShadowSettings 增 `vsmEnabled`(默认 true,双平台),PCSS R8 路径保留 fallback。
6. **验证**:Sponza 出现软阴影边缘;`TestVulkanShadowPass` 不受影响照过;全量回归;可选单测:blur CPU 参考(T3.3 先例,max-abs-diff ≤2/255)。

## Workstream 5 — Mirror 平面反射模块(StandardRenderPipeline,双平台,~2-3 sessions)

新 `RenderPipeline/Modules/PlanarReflectionModule`,模仿 ShadowMapModule 持有 `GPUDrivenDrawPipeline*`。

1. **反射 VP**:把 `ForwardRenderer.cpp:1121-1151` Householder 反射矩阵提取为共享 util(`Engine/Graphics/Utils/`),模块内算 reflectedView × 主 proj。
2. **反射渲染 pass**:不能复用 `gpuDraw.Execute`(RT 固定)。增 `GPUDrivenDrawPipeline::ExecuteReflectionPass(cmd, snapshot, reflectedView, proj, colorRT, depthRT, ...)`:照 Shadow 模式自建 RenderPassDesc + CPU `ExtractFrustumPlanes` 剔除 + **vertex-pulling 前向光照 shader** 渲到 per-plane 1024² RGBA16F + D32;新 shader `MirrorReflection.vert/frag.glsl`(**手写 GLSL**,前向 PBR 简化版,绑定对齐 meshlet 全局 buffer)+ Metal 端 .metal;GPU culling 单例槽位不被触碰(CPU 剔除路径)。
3. **平面定义**:`RenderSceneSnapshot` 或模块 setter 提供 `ReflectionPlane{position,normal,size}`(参照 `RenderScene.h:117-128`),Sponza 测试注入。
4. **合成 pass**:HDR 域、FusionComposite 之后 TAA 之前插 graphics pass(loadOp=Load 叠加,参照粒子 Pass 4c / FusionComposite 模式):镜子四边形按当前相机投影画到 `postProcessInput`,frag 平面投影 UV 采样反射纹理(Vulkan 注意 Y-flip,Mirror.metal:61 先例);合成 shader 同样手写 GLSL + Metal。`BuildRenderGraph` 与 `RenderWithCommandBuffer` 非 editor 尾部两处接线。
5. **模块接入**:`StandardRenderPipeline.h` Module 列表 + ShaderHandles 槽 + `InitializeSubsystems` 创建 + settings 开关;`Metal/shaders/Mirror.metal` 作合成端参考。
6. **验证**:Sponza 加镜子平面 + 热键开关;反射随主相机正确变化;零 validation 错误;全量回归。

---

## 提交顺序与总节奏

1 → 2 → 3 → 4 → 5(先小后大)。每步:改/增 GLSL → `build_spv.sh` 重编并 commit spv → cmake build → 全量 `TestVulkan*` 零失败零 validation → Sponza 窗口目检 → 独立 commit。

## 风险与对策

| 风险 | 对策 |
|---|---|
| 开 jitter 后 Metal TAA.metal 缺失导致 Metal shimmer | 补 Metal compute 版 TAA.metal;不行则 jitter 先按平台 gate |
| GLSL std140 与 Metal packed 布局偏差 | 沿用 Phase 0 审计经验:float3 16B 槽位用 vec4;`static_assert` 对拍 C++ 结构 |
| VSM 光渗漏/参数不合 | spread/VSM_MIN_VARIANCE 沿用 Metal 已调参数(`RHIShaderConstants`);PCSS fallback 开关保底 |
| Blur ping-pong 在 Vulkan 的 layout/barrier | 复用已验证的 BlurPass.cpp 执行器(T3.3 CPU 对拍先例) |
| GPUDrivenDraw 单例状态被反射 pass 污染 | 反射走 CPU 剔除 + 独立 CB/DS 资源(Shadow 先例) |
| GeometryDebug 绝对路径指向主 worktree | 一并改相对解析 |

## 验收

- 5 个功能在 Vulkan Sponza 窗口可见且热键可切换;
- `build/Tests/UnitTests` 全部 `TestVulkan*` 通过、零 `[Vulkan validation]` 错误;
- 新增 TestVulkanTAA / TestVulkanToon(及可选 VSM blur 对拍)单测;
- Metal 端无回归(共享路径改动在 Metal Sponza 上目检)。