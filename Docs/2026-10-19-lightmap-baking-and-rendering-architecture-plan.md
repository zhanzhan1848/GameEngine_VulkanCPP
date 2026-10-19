# Lightmap 烘焙与运行时渲染全流程架构设计方案

- **日期**：2026-10-19
- **状态**：方案拟定 / 待评审（Planning）
- **涉及子系统**：`PrimalEditor_Avalonia` (UI/CliHost)、`EngineDLL` (C ABI)、`Engine/Graphics` (RHI/RenderPipeline/Shader)、`ContentTools` (xatlas/SceneBlob)

---

## 1. 目标与背景

本方案规划在 Primal 引擎中实现一套完整的 **离线 Lightmap 烘焙与运行时渲染管线**：
1. **编辑器端**：在 Avalonia UI 提供烘焙控制面板与操作按钮，支持配置分辨率、采样数、反弹次数等参数，并通过 CLI 宿主实现命令解耦与可脚本化调度（支持无头 CI 自动化）。
2. **烘焙器核心**：基于场景几何与光追积分（GPU Compute / RT），在 UV 贴图空间计算直接光照与多次漫反射间接光反弹，完成边缘扩张（Edge Dilation）与降噪（OIDN / 滤波），生成 HDR 光照图集。
3. **资产管理与数据流**：利用现有的 A9 地基（`xatlas` 展开、`UVLM` sidecar 格式与 `RHIMeshAsset.lightmap_uv_buffer`），实现场景级多网格图集装箱（Atlas Packing）与 UV Scale/Bias 绑定。
4. **运行时渲染**：通过 Vertex Pulling / 独立顶点流绑定 Lightmap UV，在渲染管线着色阶段（GBuffer / Forward / GIGatherModule）根据材质或实例标志采样 Lightmap，与动态 DDGI 探针平滑融合。

---

## 2. 引擎现状与技术就绪度（Gap Analysis）

| 模块 | 现有基础 (A9 地基段) | 缺失链路 (路线段) |
| :--- | :--- | :--- |
| **UV 展开** | `ContentTools/modules/uvatlas` 集成 xatlas，支持 `--uvatlas-lightmap`（strict pack, gutter 4.0） | 场景级 Multi-Mesh 矩形装箱（Atlas Packing） |
| **资产序列化** | `SceneBlobWriter` 支持 `elements_type::lightmap_uv_flag (0x10)` 及 `UVLM` 数据段 | 场景描述文件中的 LightmapAtlas 贴图引用与 UV Scale/Bias 记录 |
| **引擎资产加载** | `RHIMeshAsset.lightmap_uv_buffer` 可读入 Lightmap UV（CPU 端） | GPU 显存 Buffer 创建、Vertex Layout 或 Vertex Pulling 绑定 |
| **离线烘焙系统** | 存在基础 GPU RT / SDF Tracing 代码 | 缺失 UV 空间 GBuffer 栅格化、光追辐射度积分器、多 Bounce 弹射更新器、降噪器 |
| **着色与管线** | GIGatherModule 支持 DDGI 与静态探针冻结（A10） | 着色器（Vulkan/Metal/Dawn）无 Lightmap 采样与漫反射解码分支 |
| **编辑器与交互** | 具备 Avalonia UI 框架与 CliHost 契约机制 | 缺少 UI 烘焙按钮/面板、`scene.bake_lightmaps` CLI 命令、EngineDLL C ABI 导出 |

---

## 3. 总体架构与数据流图

```
[UI 层: Avalonia Views] ──(点击/配置)──> [CliHost: scene.bake_lightmaps]
                                                  │
                                          [EngineDLL C ABI]
                                                  │
                                 ┌────────────────┴────────────────┐
                                 ▼                                 ▼
                    [1. 场景图集装箱 (Atlas)]          [2. 场景光线追踪数据准备]
                     - 计算 Texel Density               - BVH / 几何实例加速结构
                     - 矩形装箱 (MaxRects)              - 材质 Albedo/Emissive
                                 │                                 │
                                 └────────────────┬────────────────┘
                                                  ▼
                                       [3. 烘焙核心 (Baker)]
                                        - UV G-Buffer 栅格化 (Pos/Norm)
                                        - 半球余弦加权光追积分 (Direct+Indirect)
                                        - 多 Bounce 弹射辐射度更新
                                                  │
                                                  ▼
                                       [4. 后处理与编码 (Post)]
                                        - UV Seam 边缘扩张 (Dilation)
                                        - OIDN 降噪 / 双边滤波
                                        - 压缩与落盘 (*.dds / BC6H)
                                                  │
                                                  ▼
[运行时加载 (Engine)] <───────────────────────────┘
  ├─ Lightmap 纹理加载至显存 (Texture2D Array / Bindless)
  ├─ Lightmap UV 创建 GPU Raw Buffer (Vertex Pulling 模式)
  └─ [RenderGraph / Shader 消费]
       ├─ 静态网格: 采样 Lightmap 替代间接光
       └─ 动态物体: 继续采样 DDGI / Screen Probe 探针
```

---

## 4. 详细分层实现方案

### 4.1 编辑器 UI 与 CLI 调度层 (Editor & Command Surface)

遵循本仓库开发纪律：**命令面单一事实源，文档模型优先，UI 走 UndoRedo / Command**。

1. **CLI 契约命令**（`PrimalEditor_Avalonia/Cli/SceneCommands.cs`）：
   ```csharp
   public record BakeLightmapsArgs(
       int Resolution = 1024,
       int SamplesPerPixel = 256,
       int Bounces = 3,
       float TexelDensity = 16.0f, // 纹素/米
       bool Denoise = true
   );

   [CliCommand("scene.bake_lightmaps", Description = "Bake static scene lighting into lightmaps. Runs path tracing integrator on GPU/CPU.")]
   public static CommandResult BakeLightmaps(BakeLightmapsArgs args) { ... }
   ```
   *该命令同时在 CI 与无头环境下供脚本自动化调度*。
2. **C ABI 门面扩展**（`EngineDLL/`）：
   * 遵循导出只增不改原则：
     ```cpp
     EDITOR_API JobHandle Lightmap_StartBake(const LightmapBakeSettings* settings, BakeProgressCallback callback);
     EDITOR_API void Lightmap_CancelBake(JobHandle handle);
     EDITOR_API bool Lightmap_GetBakeStatus(JobHandle handle, BakeStatus* out_status);
     ```
3. **Avalonia UI**（`PrimalEditor_Avalonia/Views/`）：
   * 在工具栏添加 `Bake Lightmaps` 触发按钮与参数折叠面板。
   * 支持通过后台 Worker 轮询进度，在 UI 底部显示进度条，并将分阶段日志输出至 `BuildLogView`。

### 4.2 场景级 Atlas 整合与元数据 (Atlas Packing)

1. **纹素密度与分辨率分配**：
   * 遍历场景中拥有静态光照标记（`StaticLighting`）的 Mesh 实体。
   * 计算三角形世界表面积 $A$，根据 `TexelDensity` 计算目标分配面积：
     $$N_{\text{texels}} = A \cdot \text{TexelDensity}^2$$
2. **2D 矩形装箱（Skyline / MaxRects 算法）**：
   * 将各网格的展开图块打包装入固定规格的全局 Atlas 贴图（如 $2048 \times 2048$）。
   * 输出每个 Mesh 实例在图集内的缩放与偏移量：
     $$\text{UV}_{\text{atlas}} = \text{UV}_{\text{mesh}} \cdot \mathbf{Scale} + \mathbf{Bias}$$
3. **场景元数据存储**：
   * 场景文件追加记录：实体 `lightmap_atlas_index` 与 `lightmap_scale_bias (vec4)`。

### 4.3 离线烘焙系统核心 (Baking Engine)

部署于 `Engine/Graphics/Baking/`：

1. **UV 空间 G-Buffer 栅格化**：
   * 在贴图空间离屏渲染每个静态网格：
     * Vertex Shader：强制将 Lightmap UV 映射为 NDC 裁剪坐标 $P_{\text{clip}} = (\text{UV} \times 2 - 1)$。
     * Pixel Shader：输出世界坐标 $P_{\text{world}}$、世界法线 $N_{\text{world}}$、表面反照率 $Albedo$ 至 RGBA32F 贴图。
   * **边缘扩张（Seam Dilation）**：沿图块有效边界向外填充 4~8 像素，杜绝双线性插值采样导致的黑边与下采样暗化。
2. **光线追踪积分器（Compute Shader / GPU RT）**：
   * 逐有效纹素执行光线投射：
     * **直接光（Direct Lighting）**：针对 Directional/Point/Spot 光源做可见度阴影射线求交（Shadow Ray）。
     * **间接光漫反射反弹（Indirect Bounces）**：基于半球余弦加权采样发射 $N$ 条射线进行蒙特卡洛积分；利用双缓冲（Ping-Pong Buffer）实现多轮反弹能量累加。
3. **降噪与编码**：
   * 引入 Open Image Denoise (OIDN) 或 GPU 双边滤波对辐射度结果降噪。
   * 产物以 BC6H (HDR) 编码保存为 `.dds`，并存放于 `Content/Bakes/` 目录。

### 4.4 RHI 数据层与顶点流绑定 (Vertex Binding)

为保证既有 20 字节单顶点布局的跨后端兼容性，采用 **Vertex Pulling（数据拉取）**：

1. **显存缓冲上传**：
   * `SceneDataAdapter` 构建 `RenderMesh` 时，若检测到该 Mesh 带有 `lightmap_uv_buffer`，则创建独立的 Raw/Storage Buffer（`rhi::ResourceHandle lightmap_uv_vb`）。
2. **Shader 端拉取**：
   * 保持三端 RHI 的 Vertex Input Layout 不变（避免频繁重建 PSO）。
   * 在顶点着色器中通过 `gl_VertexIndex` 直接拉取对应的 UV：
     ```glsl
     vec2 lightmap_uv = FetchLightmapUV(gl_VertexIndex);
     ```

### 4.5 渲染管线与着色器消费 (Pipeline Integration)

1. **全局 Descriptor 绑定**：
   * 全局 Frame 描述符集绑定场景 Lightmap 贴图数组（`Texture2DArray` 或 Bindless 索引）。
   * 实例常量缓冲（Per-Instance Data）追加 `lightmap_scale_bias` 与 `lightmap_index`（`0xFFFFFFFF` 代表无 Lightmap）。
2. **着色阶段合成（GIGather / Lighting）**：
   ```glsl
   vec3 diffuse_gi = vec3(0.0);
   if (instance.lightmap_index != 0xFFFFFFFF) {
       vec2 final_uv = in_lightmap_uv * instance.lightmap_scale_bias.xy + instance.lightmap_scale_bias.zw;
       vec3 baked_radiance = texture(sampler2DArray(lightmap_textures, lightmap_sampler), 
                                     vec3(final_uv, instance.lightmap_index)).rgb;
       diffuse_gi = baked_radiance * surface.albedo;
   } else {
       // 动态网格/未烘焙网格：回退到 DDGI 探针采样
       diffuse_gi = SampleDDGIIrradiance(surface.world_pos, surface.world_normal);
   }
   ```
3. **动静态混合策略**：
   * 采用 **Indirect-Only 烘焙模式**：Lightmap 仅记录间接光与漫反射反弹，直接光仍由实时 Shadow Map 计算，确保动静态物体的直接光阴影连续一致。

---

## 5. 阶段规划与交付路径 (Roadmap)

| 阶段 | 周期 | 核心交付产物 | 验收标志 |
| :--- | :--- | :--- | :--- |
| **Phase 1: Atlas 与元数据** | 3-4 天 | 1. 静态网格 Texel 分配与矩形装箱<br>2. 场景元数据 Scale/Bias 读写<br>3. 打包率与装箱单测 | 场景多 Mesh 能紧凑无重叠拼合成 Atlas，单测通过 |
| **Phase 2: 烘焙器核心** | 7-10 天 | 1. UV 空间 GBuffer 离屏渲染 Pass<br>2. Compute 光线投射与多 Bounce 积分<br>3. Seam Dilation 与 OIDN 降噪 | 能输出平滑无缝隙的 HDR 离线 Lightmap 贴图 |
| **Phase 3: RHI 与 Shader 消费** | 4-5 天 | 1. Lightmap UV 上传与 Vertex Pulling 绑定<br>2. GBuffer / GIGather 着色器分支与解码<br>3. Vulkan / Metal / Dawn 三端对齐 | 视口内静态物体正确显示烘焙间接光，动静态过渡自然 |
| **Phase 4: UI、CLI 闭环与门禁** | 3-4 天 | 1. Avalonia 烘焙按钮与配置/进度条<br>2. `scene.bake_lightmaps` CLI 暴露与 Schema 快照<br>3. 无头烘焙渲染冒烟脚本 | UI 点击可完整跑通烘焙与热重载；CI 门禁与单测全绿 |

---

## 6. 验证与回归门禁标准

1. **CLI Schema 快照门禁**：
   * 运行 `dotnet test PrimalEditor_Avalonia.Tests -c DebugEditor --filter "FullyQualifiedName~Cli"`。
   * 变更必须附带更新后的 CLI Schema 快照。
2. **无头自动化冒烟验证**：
   * 通过脚本 `./scripts/primal.sh scene.bake_lightmaps --resolution 512` 测试非 UI 环境下的稳定性。
3. **渲染回归基线**：
   * 渲染基准场景输出 PPM/PNG，核验与全动态 DDGI 以及 A10 静态探针冻结的视觉一致性与暗部伪影。
