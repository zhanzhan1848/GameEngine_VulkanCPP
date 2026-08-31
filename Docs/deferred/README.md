# Avalonia 分支合并搁置内容整合（重放指南）

> 背景：`rehearsal/avalonia-into-dev`（910ffdfe）是 feat/avalonia-migration → dev
> 的完整演练合并。解决 18 处冲突时，少数 avalonia 侧内容被搁置（"取 dev"）。
> 本文 consolidates 全部搁置项：哪些需要重放、哪些已判定废弃、以及精确的
> 源码位置与重放步骤。合并正式落库后本文应随 Docs/ 进入 dev。

## 一、待重放（P1）：DeferredLighting.metal 多方向光 + 点光/聚光循环

**来源**：feat/avalonia-migration 分支的编辑器多光源环境功能。
**现状**：合并取了 dev 现行单方向光语义（T8.10h 调优：DIRECT_INTENSITY=3.125、
VSM 阴影、相机光经 sceneData 驱动探针）。

**参考文件**（本文同目录）：
- `avalonia-DeferredLighting.metal.reference` — avalonia 分支完整原文件
- `avalonia-multilight.patch` — 合并结果 vs avalonia 的全文件 diff（**不能直接
  apply**，两侧文件都演进过，仅供对照）

**依赖已就绪（合并中已保留/验证）**：
1. CPU 侧填充：`ForwardSceneRenderer::CreatePersistentResources`（合并结果
   ~2841 行）每帧把 `render_scene_->GetLights()` 全量填入 `light_cb_[idx]`
   （`rhi::ForwardLightBuffer`，方向光≤4 + 点/聚光≤128，含 [LightBuf] 调试输出）。
2. 绑定：`fragmentLighting_v3` 已声明 `lightBuffer [[buffer(13)]]`（dev 侧本就绑定）。
3. 结构体字节匹配：两侧 `RHIShaderCommon.metal` 的 `ForwardLightBuffer` 已
   diff 验证一致，与 C++ `rhi::ForwardLightBuffer` 字节匹配。

**重放步骤**：
1. 用 reference 文件中 264–331 行的三段式替换合并版 `fragmentLighting_v3`
   的单光源直接光块：
   - 方向光循环（`di < lightBuffer.directionalLightCount && di < 4u`），
     `shadow = (di == 0u) ? sunShadow : 1.0`（仅太阳投射 VSM 阴影）；
   - 点光/聚光循环（`li < punctualLightCount && li < 128u`，距离衰减 +
     smoothstep rangeFade + 聚光锥插值）；
   - 零光源回退：`directionalLightCount == 0 && punctualLightCount == 0` 时
     走 dev 现行单光源项（headless/只填 SceneData 的路径仍工作）。
2. **需要决策**：dev 的 `DIRECT_INTENSITY = 3.125f`（T8.10h，与 Vulkan 侧
   DeferredLighting.frag 对齐的调优值）是否应用于方向光循环。建议：循环内
   方向光项乘以该系数以保持 dev 现行画面亮度基准，重放后跑 GI 对比确认。
3. 同步检查 `sceneData.lightColor` 驱动 DDGI 探针的路径（dev：相机
   light_color 驱动 probe rays）与多光源语义的一致性——探针只吃太阳还是
   吃 lightBuffer[0]，需在重放时明确。

**验证**：
- `TestModularPipeline`：单太阳场景画面与合并前一致（回退段或 di==0 路径）；
  另构造 ≥2 方向光 + 1 点光 + 1 聚光场景目检 + 无渲染验证错误。
- `TestMetalGIFusion`（GI_VERIFY_FRAME=90）确认 GI 数值不被多光源重构带偏。
- 编辑器：EnvironmentPanel 多光源编辑 → 画面即时反馈。

## 二、已判定废弃（不需要重放）

| 搁置内容 | 位置（合并前） | 废弃理由 |
|---|---|---|
| BlurPass 硬编码路径回退（`/Users/zhanyuanwei/...` + worktree 路径） | BlurPass.cpp 冲突块 | dev 的 `utils::ShaderRegistry::GetShaderPath/ResolveShaderPath` 是同类问题的正规解 |
| RenderGraphDebug worktree 回退 | RenderGraphDebug.cpp 冲突块 | 同上 |
| LineBatchRenderer filesystem 路径查找 | LineBatchRenderer.cpp 4 处冲突 | 同上；dev 的双平台（SPIR-V/.metal）加载保留 |
| ForwardSceneRenderer worktree-first 着色器搜索（SHADER_DIR_WT 等） | ForwardSceneRenderer.cpp 冲突 0–2 | 同上 |
| RHITypes.h ASTC 注释差异 | RHITypes.h 冲突 0–2 | 两侧枚举数值逐值相同，dev 注释更全 |
| FreeList.h 0xcc 毒化注释差异 | FreeList.h | 同一修复，仅注释措辞不同 |

## 三、已在合并中主动补回（无需再处理）

- `IBLPrecomputer` 的 `equirectToCubeShader` 创建：以 dev 平台感知形式补回
  （Vulkan=cs_main / Metal=CS_EquirectangularToCube），环境贴图功能完整。
- `LineBatchRenderer::create_pipeline(DataFormat)` 定义：avalia 的运行期
  按渲染目标格式重建 PSO（编辑器 RGBA16F→BGRA8 blit 场景），已按合并后
  成员名补回定义，Draw 路径的惰性重建调用完好。

## 四、残留审计（自动合并成功的双改文件）

git 自动合并但**两侧都改过**的 25 个文件（MetalDevice/CommandBuffer/
Texture、MetalCore、Renderer、StandardRenderPipeline、ForwardSceneRenderer.h、
ContentToEngine.h、Transform、ContentTools/Geometry.cpp 等）已通过：
全量构建 + TestModularPipeline 渲染对比（阶段一致）。**未覆盖**：编辑器侧
材质编辑器 / 环境面板与引擎新导出（MaterialGraphAPI 等）的端到端联调，
建议作为重放 P1 的同一批次验证。

## 五、源码提取命令

```bash
# 搁置前的 avalonia 原始内容
git show feat/avalonia-migration:Engine/Graphics/Metal/shaders/Forward/DeferredLighting.metal

# 演练合并的解决结果
git show rehearsal/avalonia-into-dev:Engine/Graphics/Metal/shaders/Forward/DeferredLighting.metal

# 两者全文件差异（= 本文目录下的 patch）
git diff rehearsal/avalonia-into-dev feat/avalonia-migration -- \
  Engine/Graphics/Metal/shaders/Forward/DeferredLighting.metal
```
