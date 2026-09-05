# 编辑器内容制作路线 + 四大功能规划（probe 可视化 / 大气雾 / 渲染模式选择器 / 粒子编辑器）

> 状态：规划稿（未实施）。
> 调查出处：2026-09-03 代码勘察（本文 file:line 均为当日 HEAD 5fea1b46 附近）；GI 质量问题另见 `Docs/2026-08-24-gi-optimization-plan.md`（当前冻结根因：SDF trace miss 97%）。
> 阶段目标：**内容制作 + 场景半成品编辑**。剔除蓝图、角色/动画玩法、游戏逻辑脚本三类功能不做。

## 0. 原则

- ABI 纪律：EngineDLL 导出只增不改；POD 镜像 `static_assert(sizeof)` 锁尺寸（沿 `AssetAPI.h` 惯例）。
- 文档模型优先：编辑器修改类操作走 Scene/GameEntity + UndoRedo 栈，场景持久化走 `GameProject/Documents/*Document.cs` 扩展，绝不裸调 EngineDLL 制造第二事实源。
- 若功能需要 agent CLI 面：一个方法 + 一个 `[CliCommand]`，同步重生成 schema 快照。
- 新 C++ 代码用 `PRIMAL_LOG`/`primal::diag::Emit`，日志走 stderr。

---

## 1. 四大功能现状结论（代码事实）

### 1.1 DDGI Probe

| 环节 | 实现 | 位置 |
|---|---|---|
| DDGI pass | 三子 pass：TraceRays → UpdateIrradiance → UpdateDepth，class `LumenDDGIPass` | `Engine/Graphics/Lumen/DDGI/LumenDDGIPass.h:141` |
| 放置模型 | 体积内规则网格**自动放置**，无逐 probe 摆位；参数 `DDGIRuntimeParams`（probe_count_x/y/z、probe_spacing、rays_per_probe） | `LumenDDGIPass.h:38-48` |
| 位置编辑面（唯一） | `LumenVolumeConfig.origin_override` / `use_origin_override`；多体积 `LumenConfig.volume_overrides`（cap 4：0=近场、1=远场粗壳、2..3=用户摆放）；无 rotation、无 per-probe offset | `Engine/Graphics/Lumen/LumenTypes.h:31-41,112-114` |
| probe 数据 | **storage buffer 而非 atlas 纹理**：irradiance 三缓冲、depth（16 float/probe）、confidence、ray_data、hit_distance；accessor 已齐（`GetIrradianceBuffer/GetDepthBuffer/GetConfidenceBuffer/GetRayDataBuffer/GetHitDistanceBuffer/GetProbePositionBuffer/GetVolumeView`） | `LumenDDGIPass.h:172-214` |
| 调试面 | `GIDebug()` 进程级单例：逐 pass 开关、`freeze_probe_updates`、`ProbeVisMode` 枚举（Off/IrradianceAtlas/DepthAtlas/Confidence/ProbePositions） | `Engine/Graphics/Lumen/GIDebugParams.h:24-65` |
| 真 probe 球 overlay | `DebugProbeSphereModule`（每 probe 一个 icosphere，FinalBlit 后 LDR 上叠加）；开关 `StandardRenderPipeline::SetProbeSphereDebug`，需注入 probe_sphere_vs/ps 句柄 | `Engine/Graphics/RenderPipeline/Modules/DebugProbeSphereModule.h`、`StandardRenderPipeline.h:114-115,201-202` |
| volume 编辑 ABI | `GIGetVolumeOverrideCount/Get/Set/Append/Remove/MaterializeDerivedVolumes/GIGetEffectiveVolumeConfig`（:821-857）；`GIBakeStaticProbes`（:864）；GI gizmo（`GIGizmoSetMode/GetMode/GetSelectedVolume/Pick/Translate/Scale/GetWireframe`，:976-1061，kMinProbeCount=4/kMaxProbeCount=128） | `EngineDLL/RenderPipelineAPI.cpp` |
| probe_vis 接线 | **仅 Vulkan GIGather**（binding 6 debugVis，`GIGatherModule.cpp:28-30,77,293-297,580`）；Metal/Dawn 未接 | `GIDebugParams.h:52-53` 注释 |
| **缺口** | ① probe 数据 CPU readback **无 ABI**（底层 getter 已就绪，缺导出）；② Metal/Dawn probe_vis 未接线；③ 编辑器侧无任何 probe 可视化/编辑 UI；④ "probe 位置编辑"只能落在体积级（见 §2 B1 语义界定） | — |

### 1.2 场景雾

| 环节 | 实现 | 位置 |
|---|---|---|
| 解析式体积雾 | `VolumePass`（step_size/extinction/scattering/density_threshold），开关 `enable_volume_pass` 默认 false | `Engine/Graphics/Volume/VolumePass.h`、`PipelineQualityConfig.h:116` |
| Froxel 网格雾 | `FroxelFogPass` 三 pass（density_inject → light_integrate → resolve），`FroxelGridConfig`：extinction_scale、scattering_albedo、phase_g(HG)、noise_scale/speed、**height_fog_base/height_fog_scale**、ambient_intensity；输入含 SDF 与 shadow_map；开关 `enable_froxel_fog` 默认 false | `Engine/Graphics/Volume/FroxelFogPass.h/.cpp`、`FroxelTypes.h:53-69`、`PipelineQualityConfig.h:118` |
| 着色器 | Metal MSL 源**运行时按路径读取**（`FroxelFogPass.cpp:110-118,249`）→ 天然支持"改源重启"；无 .comp 对应物，三端共用 MSL | `Graphics/Metal/shaders/Volume/Froxel*.metal` |
| 场景环境 ABI | `UpdateSceneEnvironment(const f32* params)` **布局已定义 24 float**（0..3 Ambient、4..6 Fog RGB、7..9 Fog Near/Far/Density、10 Fog Enabled、11..14 Sky、15/16 Tonemap/Exposure），**但函数体只消费 Ambient（params[0..3]）——雾段未接线** | `EngineDLL/RenderPipelineAPI.cpp:1063-1082` |
| 编辑器序列化 | `SceneEnvironmentDocument`：FogEnabled、FogR/G/B、FogNear(10)/FogFar(200)/FogDensity(0.01) + Skybox/ToneMapping/Exposure/EnvMapPath | `PrimalEditor_Avalonia/GameProject/Documents/SceneEnvironmentDocument.cs:20-26` |
| 着色器热更 | `ReloadPipelineShader(shader_handle, data, size)` ABI 现成 | `RenderPipelineAPI.cpp:223-229` → `StandardRenderPipeline.cpp:222-224` |
| 简单距离雾 | 前向/延迟着色器内**无逐像素距离雾**（Vulkan glsl 无 fog 命中） | — |
| **缺口** | ① ABI 雾段未消费（接线而非新建）；② 简单距离/高度雾着色器不存在；③ 雾 shader 的编辑器编辑入口不存在 | — |

### 1.3 渲染管线模式

| 环节 | 实现 | 位置 |
|---|---|---|
| 统一管线 | `StandardRenderPipeline`（RenderGraph 驱动，RHI 抽象，Vulkan/Metal/Dawn）；嵌入视口走 `CreateEmbeddedRenderSurface` + `PipelineRenderFrame::ForEditorCamera` | `Engine/Graphics/RenderPipeline/`、`EngineDLL/EngineAPI.cpp:55,178` |
| 延迟路径 | `enable_deferred_lighting` + `DeferredLightingModule` + GBuffer/DeferredLighting.spv | `PipelineQualityConfig.h:114` |
| **pass 单一真源** | `RenderPassID` 枚举（Shadow/DeferredLighting/SSAO/DDGI/SurfaceCache/SSGI/SSR/ScreenProbes/GIGather/VolumePass/FroxelFog/FluidRender/FusionComposite/FinalBlit…）；`IsPassEnabled/SetPassEnabled/IsPassActive` | `PipelineQualityConfig.h:14-32,126-127`、`StandardRenderPipeline.h:77-80` |
| 配置 ABI | `GetRenderPipelineSettings/UpdateRenderPipelineSettings`（整 struct memcpy，Update 置 dirty 下帧重建）；`SetRenderPassEnabled/IsRenderPassEnabled/IsRenderPassActive`；**参数自省 `ParamDescriptor/GetPipelineParamDescriptors`**（group/type/range，可自动生成 UI） | `RenderPipelineAPI.cpp:144-164,170,181,192` |
| AO / Shadow | AO：`Lumen/SSAO/LumenSSAOPass`（GTAO 参数 `LumenConfig.gtao_*`）；Shadow：`ShadowMapModule`（cascade/res/quality `LumenConfig.shadow_*`，VSM 切换 `SetVSMShadows`） | `LumenTypes.h:125-145` |
| GI 档位 | `LumenQualityPreset` Off/Low/Medium/High/Ultra/UltraRT；五轴 `GIFiveAxisConfig` 经 `ApplyAxesConfig` 热重载 | `LumenTypes.h:15-22,52-74` |
| 归属 | **管线是全局单例**（`GetStdPipeline()`），配置全局而非 per-surface；"ForwardRenderer" 是遗留分端轻量路径 + editor-mode 分支（`SetEditorMode`） | `RenderPipelineAPI.cpp:68-72,448` |
| **缺口** | ① 无面向 viewport 的"渲染模式"枚举到 pass 组合的映射；② 编辑器工具栏无 UI。**引擎侧几乎零改动** | — |

### 1.4 粒子系统

| 环节 | 实现 | 位置 |
|---|---|---|
| 数据模型 | `emitter_config` 完整参数组：limits/emission(continuous/burst/ring_buffer、spawn_rate)/lifetime/velocity/color/scale 初值区间/gravity/wind/drag/blend_mode+depth_write/sprite sheet 动画/emit_in_local_space | `Engine/Particles/ParticleTypes.h:126-183` |
| over-life 曲线 | 6 条：scale/alpha/color_gradient/velocity/force(v3)/rotation，各带 use_* 开关；`float_curve/color_gradient/vector_curve`：keyframe(time,value,in/out tangent) + constant/linear/smooth/bezier 插值 | `ParticleTypes.h:52-99`、`ParticleCurve.h` |
| 预设 | `particle_preset`（metadata + config + 曲线快照）；**JSON 序列化完备**（`preset_json::to_json/from_json/save_to_file/load_from_file`，自含 parser）；`preset_manager` 整目录加载；`default_presets` 8 个（fire/smoke/explosion/magic_sparkle/rain/snow/dust/bubbles），代码注册 + 文件双轨 | `ParticlePreset.h:19-72,133-147`、`ParticlePreset.cpp` |
| 组件 | `primal::particle::component`（particle_id）+ `init_info{emitter_config, material_id, auto_activate}`——**引用 config 实例而非 preset 名** | `Engine/Components/Particle.h:25-29` |
| 已有 ABI | **只有材质面** `ParticleMaterialAPI.cpp`（blend/base texture/texture sheet/depth write/soft particles）；**无 emitter/preset/component 导出** | `EngineDLL/ParticleMaterialAPI.cpp` |
| 引擎内 ImGui 编辑器 | `PresetBrowser.cpp`/`CurveEditor.cpp` 存在但是**死代码**（引擎构建无 ImGui，CurveEditor.cpp:5-8 明示全 stub） | `Engine/Particles/` |
| 编辑器侧 | `grep Particle` 在 PrimalEditor_Avalonia 全部 .cs **零命中**；component 未进场景文档序列化 | — |
| **缺口** | ① C ABI 导出层整体缺失；② 编辑器零接线；③ 场景文档无粒子组件；④ 曲线编辑器需在 Avalonia 重写（引擎 stub 不可复用） | — |

### 1.5 静态光照链路现状修正（2026-09-03 勘察补充）

两个修正性结论 + 两个易混淆点，直接影响 §3/§4 的排期判断。

**结论一：StaticProbeBake 存在，但角色是 DDGI warm-start，不是 UE 式静态光照**
- 烘焙器：`Engine/Graphics/Lumen/StaticProbe/StaticProbeBaker.h` —— CPU 侧 BVH 光线追踪，烘焙 radiance（默认 512 rays/probe、3 bounce、收敛阈值判定）、visibility（octahedral depth + sky factor）、sky SH → `StaticProbeVolume`。
- 入口已接线：`GIEditorPanel.h:110` 的 bake_static_probes 面板按钮 → `BakeStaticProbes()` → UploadToGPU；ABI `GIBakeStaticProbes`（`RenderPipelineAPI.cpp:864`）。
- 但 `StaticProbeBaker.h:24-29` 注释明确：运行时 DDGI 动态模式每帧覆写 octahedral depth + sky factor，**烘焙值只在收敛前几帧被读到**——即 bake 是加速收敛、避免从黑开始的 warm-start，**不减少运行时计算**，烘焙后开销结构不变。
- 修正原评估："无静态化等价物" → "有探针级预烘焙，但它不减少运行时计算"。

**结论二：lightmap UV 生成已具备，消费链整体断裂**
- 生成侧完整：ContentTools 的 `UvAtlas.h` 包装 xatlas（third_party submodule 直编），`UVSetPurpose::Texture/Lightmap/Detail` 三种用途，`ForLightmap()` 预设（gutter=4、strict_unique_pack=true、不用 artist UV 作 chart hint），`TestUvAtlas.cpp` 覆盖 Lightmap 路径；CLI 参数齐全（`--uvatlas` + `uvatlas_target_purpose/gutter/resolution`）。
- 断点 1（序列化丢弃）：`SceneBlobWriter.cpp:146` 打包时只取第一套 Texture-purpose UV 写入 static_normal_texture 元素，**生成的 Lightmap UV 进不了 SceneBlob**。
- 断点 2（运行时无处安放）：引擎顶点元素格式只有单 UV 通道（`ProceduralMesh.h` 的 uv[2]）；Engine/ 侧 grep "Lightmap" 仅 `lighting_light_mapped_` 类 UBO 命名误命中——shader 无 lightmap 采样路径，也无烘焙纹理的存储/流送格式。
- 修正原评估："缺 lightmap UV 生成" → "生成已在（uvatlas/xatlas），缺整条消费链：多 UV 通道顶点格式 → SceneBlob 序列化 → shader 采样 → lightmap 纹理烘焙与存储。工作量主要在引擎侧而非内容工具侧"。

**易混淆点**：
- `vtbake/` 是 virtual texture 烘焙（PNG → vtex 平铺格式），与 lightmap 无关。
- `Engine/Graphics/Utilities/PRTBaker.h`（逐顶点 SH9 shadowed transfer）除自身 .cpp 外无任何调用方，是**未接线的休眠代码**——若未来补静态光照链路，它是现成起点之一。

**对规划的推论**（已并入 §3/§4 重排）：
1. 消费链拆两段。**地基段**（多 UV 通道顶点格式 + SceneBlobWriter 透传，新任务 A9，M）便宜且是止损——uvatlas 的产出现在生成即丢；**路线段**（shader 采样 + lightmap 纹理烘焙/存储，XL）是架构路线决策，暂缓。
2. 本引擎 GI 走 probe 路线，静态场景的"静态化"收益可以先用 **A10 静态场景 GI 冻结**（S：场景级开关 = `GIBakeStaticProbes` warm-start + 冻结探针更新）吃到大部分——bake 已接线、`freeze_probe_updates` 已有（`GIDebugParams.h:43`），只差组合成场景开关。UE 式 per-texel lightmap 只有在出现"完全静态场景打包出货"需求时才立项。
3. PRTBaker 接线（逐顶点 SH9 间接光）作为路线段的替代方案**储备**（M~L），不排期。

---

## 2. 功能设计

### B1. DDGI Probe 可视化与位置编辑

**语义界定**：probe 是体积内规则网格自动放置，**per-probe 摆位不是引擎模型**。第一版"位置编辑" = **体积级**：gizmo 移动/缩放 `volume_overrides[2..3]`（或设 origin_override），probe 网格随之变化；per-probe offset 记为非目标（如需再立项）。

**分层实施**：

- **L1 体积可视 + gizmo 编辑（先行）**
  - 引擎：零新增（`GIGizmo*` 五个 ABI + `GetWireframe` 端点导出现在就能用；`DebugProbeSphereModule` + `SetProbeSphereDebug` 画真球）。
  - 编辑器：GI Volume 面板（实体列表 / probe counts / spacing / rays / origin 数值输入 → `GIVolumeOverrideSet`）+ 视口线框与球 overlay（`GIGizmoGetWireframe` 端点复用 `GizmoOverlay`/`WorldProjector` 投影画线）；拾取走 `GIGizmoPick`。
  - 调试联动：面板放 `freeze_probe_updates`、`ProbeVisMode` 切换（`GISetDebugParams` 整 struct 直写）。
- **L2 probe 纹理查看（readback）**
  - 引擎新增 ABI（S）：`GIReadbackProbeData(kind: irradiance/depth/confidence, out buffer)` —— 按 DDGI 侧既有约定 "consumer 自己 stage-copy off the render queue"（`LumenDDGIPass.h:190`），参考先例 `Graphics/PCG/PCGSDFReadbackManager.h`。
  - 编辑器：Probe Inspector 面板，把 buffer 转 `WriteableBitmap` 显示三视图（irradiance atlas / depth / confidence），带 probe 索引高亮；读取前自动置 `freeze_probe_updates` 防撕裂。
- **L3 probe 球预览（"看到 probe 的 shader"）**
  - 在 `DebugProbeSphereModule` 的注入点（probe_sphere_vs/ps 句柄，`StandardRenderPipeline.h:114-115`）挂**单球预览 shader**：icosphere 表面按方向采样该 probe 的 irradiance buffer（UE Lumen probe preview 风格），选中单个 probe 时高亮显示。
  - 同时编辑器资产预览面板可做离屏版（复用资产编辑器宿主），此项放最后。

**验收**：① 视口可见每个 probe 球位置与体积线框；② gizmo 移动/缩放体积后 probe 网格随动；③ Inspector 显示 irradiance/depth/confidence 三视图 + 单球预览；④ Vulkan 与 Metal 双端可用（Metal 需先接 probe_vis，见依赖）。
**工作量**：L1 S（引擎零改动）｜L2 S~M（ABI + 面板）｜L3 M（shader）。
**依赖/风险**：Metal probe_vis 接线（M，独立 PR）；**GI 内容质量不在本功能验收内**——当前 SDF trace miss 97% 导致场可能为空，验收以"显示管线通"为准，内容修复走 `2026-08-24-gi-optimization-plan.md`。

### B2. 场景大气雾（开关 + shader 编辑）

**目标拆解**：① 场景级简单距离/高度雾（参数面板已有、可开关）；② Froxel 体雾作为"大气雾"高级档（体散射/相函数/噪声/SDF 遮挡）；③ 雾 shader 可在编辑器内编辑并热更。

**方案**：

- **引擎接线（S~M）**：
  1. `UpdateSceneEnvironment` 补齐 params[4..10] 消费：FogEnabled/FogRGB/Near/Far/Density → 映射到简单雾 uniform + Froxel `FroxelGridConfig`（RGB→scattering_albedo 色调、Density→extinction_scale、Near/Far→fade）。
  2. 简单距离雾：新增一个解析 fog pass（或并入 FinalBlit 前 full-screen pass；MSL 单源三端共用，沿 Froxel 的"运行时读源"路径）。
  3. 开关统一走既有 `SetRenderPassEnabled(FroxelFog/VolumePass)` + UpdateSettings dirty 重建。
- **shader 编辑入口（M，风险点在跨后端）**：
  - 雾着色器目标加入编辑器 Shader 编辑器（`Editors/Shaders/`）：第一版编辑 MSL 源 → 保存即 `ReloadPipelineShader` 热更（ABI :223-229）+ Froxel 的重启路径。
  - Vulkan 侧无 .comp：第一版 Vulkan 跟随 MSL 同步（文档标注"编辑以 MSL 为源"）；后续再评估 GLSL 编译路径。`EditorShaderManager` 目前 Metal 专用 inline-MSL（`EditorShaderManager.cpp:21-25`），不承载此功能，避免混淆。
- **编辑器**：`EnvironmentView` 已有 Fog 参数组 → 接开关 + 新增"雾模式"选择（Simple / Analytic(VolumePass) / Froxel）+ "编辑雾着色器"按钮（打开 Shader 编辑器的 Fog 资产）。

**验收**：① 面板开关即时生效（视口内可见）；② 高度雾/密度/颜色可调且写回场景文档往返一致；③ 改雾 shader 源保存后视口热更新；④ Metal+Vulkan 双端视觉一致（以 `metal_visual_alignment` 流程对拍）。
**工作量**：引擎 S~M；编辑器 S；shader 编辑入口 M。
**建议**：排在 B3 之后做（复用同一套 pass 开关/重建机制）。

### B3. 渲染模式选择器（Run/Stop 旁）

**语义界定**：两个下拉——**RenderMode**（作用于编辑器嵌入视口的实时管线，"在 render surface 中直接跑"）与 **Config**（Debug/Release，作用于 Build/Run 产物；视口不受影响）。工具栏已有 Build 配置选择，若语义重复则复用/移位，不造第二个真源。

**模式映射表**（编辑器侧常量，切换 = 组合调既有 ABI）：

| 档位 | 映射 |
|---|---|
| Forward | editor-mode 轻量前向分支（`SetEditorMode(true)` 路径；语义=轻量预览前向，非完整前向管线——文档明确标注） |
| Deferred | `enable_deferred_lighting=true`，Shadow/SSAO/DDGI 关 |
| Deferred+Shadow+AO | 同上 + `SetRenderPassEnabled(Shadow,SSAO)` |
| **Deferred+Shadow+AO+GI（默认）** | 同上 + DDGI 开 + `LumenQualityPreset` Medium（`PipelineSetLumenConfig`） |

**实施**：
- 引擎改动 0~S：`GetRenderPipelineSettings/UpdateRenderPipelineSettings` + `SetRenderPassEnabled` 全齐；如需原子多开关切换，加一个 `PipelineApplyModePreset(mode)` 导出（S），避免编辑器多次 set 中间态渲染。
- 编辑器：工具栏 RenderMode 下拉（默认 Full 档）+ Config 下拉（默认 Debug）+ 切换时状态栏显示当前模式；用 `GetPipelineParamDescriptors`（ParamDescriptor 自省）做"当前模式详情"弹层（加分项）。
- 验收：① 四档切换视口立即变化、可来回切不崩；② 切换延迟与 pipeline 重建成本实测记录（UpdateSettings 走 dirty 下帧重建，`StandardRenderPipeline.cpp` UpdateSettings）；③ Config 下拉影响 Build/Run 的构建配置与启动产物。
- 工作量：编辑器 S~M；引擎 0~S。**建议第一个做**（本周内可落地，且是 B2 的机制依赖）。

### B4. 粒子系统编辑器

**分阶段**：

- **P1 数据通道（S~M）**：新增 `ParticleEmitterAPI.cpp`：
  - `EmitterCreate/Destroy/SetConfig/GetConfig`（emitter_config 整 blob 传递，版本号 + `static_assert(sizeof)` 锁尺寸）；
  - `PresetApplyToFile/ListPresets/SaveToFile/LoadFromFile`（直接包 `preset_json`/`preset_manager`）；
  - `EmitterSetActive/SetSpawnRate/Burst`（component 运行时 API 镜像）。
  - 场景文档：`EntityDocument` 扩展 ParticleComponent（存 preset 路径 + override 参数，走文档模型）；UndoRedo 接入 config 替换动作。
- **P2 属性面板（M）**：PropertiesView 新增 Particle 区块，参数按 `emitter_config` 分组 UI（emission/lifetime/velocity/color/scale/forces/gravity/wind/drag/blend/sprite sheet/local space）；**Add Component 最小落地**：先只支持添加 Particle 组件（`EditorComponentRegistry` 的第一个真实注册者，激活该桩）。
  - 引擎侧需支持**运行时全量 config 替换**（现有 `set_spawn_rate` 等局部接口不足；config 整体热替换需重建/复用 emitter——列为 P2 的引擎前置任务）。
- **P3 曲线编辑器（M~L，本功能最大件）**：Avalonia 自绘曲线控件：keyframe 拖拽、in/out tangent 手柄、插值模式切换（constant/linear/smooth/bezier）、6 条曲线 tab（scale/alpha/color gradient/velocity/force/rotation）+ use_* 开关；数据模型直接镜像 `float_curve/color_gradient/vector_curve`。引擎 ImGui 版不可复用，仅作行为参考。
- **P4 预设库（S~M）**：内容浏览器集成 `.particle.json` 资产类型（注册到 AssetRegistry + 缩略图色板起步）；映射 `preset_manager` 目录；右键新建/另存/应用；8 个内置预设导出为文件。
- **P5（可选，最后）**：粒子资产离屏预览面板（复用资产编辑器宿主 + embedded surface 惯例）；场景内实体本身即实时预览，P5 非必需。

**验收**：内容浏览器右键新建粒子预设 → 拖入场景创建实体 → 属性面板改 spawn_rate/blend/sprite sheet 立即生效 → 曲线编辑器改 scale/alpha over life 生效 → 保存/重开工程往返一致 → 8 个内置预设可加载。
**风险**：① config blob ABI 版本化（沿 AssetAPI.h 惯例）；② 运行时全量替换的 emitter 生命周期管理；③ undo 深拷贝语义（曲线数组）。

---

## 3. 内容制作基础闭环（Track A，前次讨论结论归档）

按"是否加速摆放—组织—复用循环"排序，与四大功能合并排期：

| # | 项 | 工作量 | 引擎依赖 |
|---|---|---|---|
| A1 | 选中可视化 + 真实 AABB 拾取（替换固定 1m 立方；选中描边/包围盒，`Engine/Graphics/DebugDraw` 现成） | S~M | 1 个 AABB 导出 |
| A2 | 吸附系统（网格 0.25/0.5/1m、角度 5/15°、Ctrl 临时开关；挂在 gizmo delta 量化处） | S~M | 无 |
| A3 | 层级树 reparent 拖拽（**勘误：搜索过滤已实现**——`WorldEditorView.axaml` L119-121 HierarchyFilter → `WorldEditorViewModel.ApplyFilter` L51-68；本项收窄为 reparent，见 §6.1） | S | 无 |
| A4 | Prefab UI（`PrefabDocument`/`PrefabInstanceComponent` 数据层已完工；创建/实例化/override；顺带 Copy/Paste 升全组件深拷贝） | M | 无 |
| A5 | Array 批量复制 + 轻量散布 | M | 无 |
| A6 | PCG 图编辑器（`Engine/Graphics/PCG` Graph/Nodes/Serializer 全在；复用材质 Nodify 节点画布；**最大杠杆，可并行启动**） | L | 无 |
| A7 | 视图模式最小集（线框/无光照/碰撞线框，走 DebugDraw）+ 相机书签 | S | 无 |
| A8 | 源码控制最小集成（Git 状态图标 + 提交面板 + 场景 JSON 排序稳定化；多人则提前） | M | 无 |
| A9 | Lightmap UV 消费链**地基段**：多 UV 通道顶点格式（`ProceduralMesh.h` uv[2] → 双通道）+ `SceneBlobWriter.cpp:146` Lightmap UV 透传（生成侧 UvAtlas/xatlas 已在，现状生成即丢，此为止损） | M | 顶点格式跨后端同步 |
| A10 | **静态场景 GI 冻结模式**：场景级开关 = `GIBakeStaticProbes` warm-start + `freeze_probe_updates`（全部复用现有开关，probe GI 架构下吃"静态化"大部分收益，运行时跳过 DDGI 每帧更新） | S | 无 |
| — | 暂缓：地形/建模模式/植被笔刷（PCG 顶替）、音频、动画、Sequencer、物理、UI runtime、本地化；**lightmap 路线段**（shader 采样 + lightmap 纹理烘焙/存储，XL——probe GI 架构下以 A10 替代大部分收益，出现"完全静态场景出货"需求再立项；PRTBaker 逐顶点 SH9 为储备方案，见 §1.5） | XL×n | 运行时缺/路线决策 |

---

## 4. 总排期

### 4.1 起手顺序（Track A 主线推进逻辑）

- **第 1 个 PR 周期：A1–A3 一个周期内全部落地**（拾取 AABB 导出、选中描边、吸附、reparent 拖拽、层级过滤——全是 S~M 且互相独立，可拆成并行小 PR）。这批做完，编辑器的**"日常摆放手感"达到可长时间工作的水平**：点得准、看得见选中、摆得齐、几百实体的场景管得住。它是后面一切内容制作工作的地基，不完成后每一步都在给交互债付利息。
- **第 2 周期起：A4 Prefab UI 打通"复用"闭环**（数据层已完工，纯接线）。复用闭环一通，模块化搭建（墙板/灯组/道具组合）的生产力立刻上一个台阶。
- **A6 PCG 编辑器与 A4 同时并行启动**：它工程量大（L），但**引擎侧零阻塞**（Graph/Nodes/Serializer 全在，只差编辑器面），且复用材质编辑器的 Nodify 节点画布需要一段搭框架的时间——这类长周期任务只有尽早并行才不卡主线。PCG 编辑器交付即同时解决散布/程序化摆放/植被类需求。
- **A5 Array 工具作为 A4 与 A6 之间的间隙任务**：批量阵列复制工作量小（M）、独立无依赖，正好填在 Prefab 交付后、PCG 编辑器交付前的空档，且它本身是模块化摆放的高频工具。
- **排序原则：同层内"先快赢后大件"，快赢内部按重要性**。§1.5 勘察补充产生两个新快赢：**A10 静态场景 GI 冻结**（S，`GIBakeStaticProbes` + `freeze_probe_updates` 全是现成开关，probe GI 架构下先吃到"静态化"收益）随第 1 批搭车；**A9 Lightmap UV 透传地基**（M，止损 uvatlas 生成即丢）进第 2 批间隙位。lightmap 路线段（XL）列为路线决策暂缓，PRTBaker 为储备方案。

### 4.2 批次表（Track A + 四大功能合并）

| 批次 | 内容 | 说明 |
|---|---|---|
| 第 0 批 = PR 周期 1（~1 周） | **A1 选中+AABB 拾取、A2 吸附、A3 reparent+层级过滤**；**B3 渲染模式选择器**（引擎零改动，同期搭上） | 手感地基 + B2 的 pass 开关机制依赖先立 |
| 第 1 批 = PR 周期 2 | 主线：**A4 Prefab UI**（打通复用闭环）。并行长线：**A6 PCG 编辑器启动**（搭框架、接 PCGGraph）。并行功能线：**B1 probe 可视化 L1→L2**（readback ABI 独立 PR）、**B2 大气雾接线**。快赢搭车：**A10 静态场景 GI 冻结**（S，与 B1 同属 GI 线） | B1 与 GI 优化线并行（GI 内容质量另线，见 §B1 风险） |
| 第 2 批 = 间隙 + 功能 | **A5 Array 批量布设**（A4 与 A6 交付之间的间隙任务）、**A9 Lightmap UV 透传地基**（快赢插入）、**B4 粒子 P1+P2**、B1 L3（probe 球 shader）、A7 视图模式 | |
| 第 3 批 = 长线交付 | **A6 PCG 编辑器交付**、**B4 粒子 P3 曲线编辑器 + P4 预设库**、A8 源码控制（多人则提前） | |
| 路线决策/暂缓 | 地形、建模模式、音频、动画/Sequencer、物理、UI runtime；**lightmap 路线段**（shader 采样 + lightmap 纹理烘焙/存储，XL；PRTBaker 为储备） | 按题材与"静态出货"需求再议（户外场景则地形提前评估，见 §1.5） |

> 各任务的实施级展开（触点文件、改动步骤、ABI 签名草案、验收点）见 **§6 附录**。

> **实施状态（2026-09-03 续）**：第 1 批 A4（Prefab UI）与 A10（静态场景 GI 冻结）已实施。
> - A4：`Serializer.ToEntityDocuments`/`InstantiateEntityDocuments`（guid 重分配防多实例身份冲突）+ 层级右键"Create Prefab…" + `DropAssetAsync` prefab 分支（undo 事务 + PrefabInstanceComponent）+ 只读检查器（`Editors/Prefab/`，注册进 AssetEditorRegistry/Host）+ 预览状态行 + Copy/Paste 升级为全组件深拷（经 EntityDocument 往返）。往返单测 2 个 ✅。
> - A10：引擎新增 `GISetProbeFreeze(u32)`（`GIDebugParams.freeze_probe_updates` 的单字段开关，规避 C# 侧结构体偏移硬编码）+ `SceneEnvironment.StaticGIFrozen`（文档字段，序列化接线）+ EnvironmentView 勾选框，`ApplyToNative` 统一推送（场景切换/管线重建后状态自动恢复）。烘焙 warm-start 仍走 GIEditorPanel。
> - 测试基线：CLI+Prefab 45/45 ✅；全量 609 中 11 失败均为**既有**问题——8 个主题/可视化失败源于工作区未提交的主题 WIP（WarcraftTheme.Styles.axaml / MaterialEditorView.axaml / WarcraftVisualDump.cs，stash 对照实验证实），3 个纹理烘焙端到端在干净 HEAD 上同样失败（环境依赖）。

> **实施状态（2026-09-04 续 22）**：B1 L3 遮挡阶段已实施。
> - probe 球 overlay 原为无深度测试（墙后也画）。实现：模块管线开深度测试（D32_Float/Less/不写深度），overlay pass 经 `RGRenderPassDesc.depthStencil` 挂 GBuffer 深度附件（`graph.ImportResource("GBufferDepth_ProbeSpheres", GetGBufferDepthSampleable())`，Load/Don'tCare 只读）；render_scale != 1 时 GBuffer 深度与 overlay 目标尺寸不一致，自动退回无遮挡 overlay（尺寸守卫）。零 shader 改动——硬件深度测试完成遮挡。
> - 活体验证路径：选中带 Environment 组件的实体 → 属性面板 ENVIRONMENT → PROBE DEBUG → 勾 "Probe Spheres"（编辑器已留运行，dylib 已同步新构建）。

> **实施状态（2026-09-04 续 25）**：B2 增量——雾着色器编辑器已实施（规划内最后一项增量收口）。
> - **实现**：`Editors/Shaders/FogShaderEditorWindow`（非模态单例）——下拉选择 4 个 Froxel MSL 文件（DensityInject/LightIntegrate/Resolve/Common）、monospace 文本编辑、**Save + Hot Reload**：写盘后 `SetRenderPassEnabled(FroxelFog)` off→on 触发 pass 重建，`CreatePipelines` 从盘上重读 MSL 完成热更（重建路径为 B2 v1 已验证的既有机制）；雾未开启时仅写盘并提示"下次开启时生效"。目录定位：cwd 相对 → 可执行位置上溯 → 仓库绝对兜底（引擎 LoadShaderBytecode 双候选同款）。入口：Environment 面板 FOG 区块 "Fog Shader…" 按钮。
> - **设计取舍**：未走 ShaderDocument 资产系统（雾 MSL 是引擎源文件而非工程资产，沿用计划"B2 v1 未新增资产类型"的取向）；Vulkan 端无 .comp，编辑以 MSL 为源（计划 §B2 既定）。
> - 门禁：C# 0 错误 ✅。**活体 UI 验证挂起**：会话 GUI 再次间歇失效（Avalonia RenderTimer -6661，同续 15 模式——该故障历史上会自愈，恢复后打开 Environment 面板 FOG 区 "Fog Shader…" 即可目验：改 MSL 保存 → 视口雾变化 + 状态行 "hot reloaded"）。

> **实施状态（2026-09-04 续 24）**：L3 遮挡稳健化 + Vulkan 验证 + 交叉树污染处置。
> - **遮挡稳健化（管线族）**：遮挡改动引入一个隐患——Vulkan 测试（TestVulkanSponzaRenderGraph 也调本模块 AddPass）不传 depth_rg，会出现"管线要深度、pass 无深度附件"错配。重构为**管线族四变体**（色相/SH9 × 深度/无深度，`makePipeline` lambda），AddPass 按 `occlusion` 选择；无深度变体与 L3 之前行为逐字节一致 → 测试路径零变化，编辑器路径获得遮挡。
> - **Vulkan 活体验证**：DebugVulkan 树构建通过；窗口式运行 100s——probe 球模块在 Vulkan 上跑 1800 帧（98 万展开顶点，nodepth 变体正确选中）、无 fault；发现并修复一个自引入问题：AddPass 无条件写 binding 1 会在 irradiance 缓冲为空的会话（Vulkan 测试）产生 NULL storage buffer validation 错误——已改为**条件写入**（干净 HEAD 对照基线 0 错误确认为本会话引入）。Vulkan irradiance 采样入口（独立 spv）在"句柄未注入"会话才激活，注入句柄的测试路径保持色相。
> - **交叉构建树污染（AGENTS.md 警告的坑实战命中）**：DebugVulkan 构建把共享 `Darwin/Debug/libEngine.a` 覆盖为 Vulkan 对象（时间戳证据）。已按规程处置：`rm libEngine.a` → 重建 Darwin/Debug Engine+EngineDLL → nm 确认 archive 无 Vulkan 符号 → 重同步编辑器 dylib → 重启 + 冒烟 PASSED（管线族 25/26/27/28 重建 ✅）。
> - **流程留痕**：编辑器运行时 dylib 真实加载点 = `Darwin/PrimalEditor_Avalonia/DebugEditor/`（见续 19）；跨树构建后必须 `rm Darwin/Debug/libEngine.a` 再重建 Debug 目标——本次是该文档警告的首次实战命中与处置记录。

> **实施状态（2026-09-04 续 23）**：B1 L2 探针检查器已实施（**B1 功能 L1–L3 全部完成**）。
> - **工作量大降级**：续 4 曾估 M（帧内 stage-copy hook）——本轮确认 DDGI 缓冲是 Metal **Dynamic = 共享内存可 Map**（仓内 :871/:903/:1054 既有直读先例），readback 退化为 `MapBuffer→memcpy`，**零帧内 hook**。
> - **导出（只增）**：`GIGetProbeDebugMeta`（格参数+总数+最新帧槽 `FrameIndex%3`）、`GIProbeConfidenceReadback`（probeIdx×4：rayHitRatio/temporalStability/visibilityConf/convergenceAge）、`GIProbeIrradianceReadback`（probeIdx×9 float4，最新写槽）。配套只增 getter：`LumenDDGIPass::GetDevice/GetConfidenceBuffer/GetIrradianceBufferLatest`（首个 GetConfidenceBuffer 与 ：280 既有签名撞名，已去重）。
> - **检查器**：`Editors/Gi/ProbeInspectorWindow`（非模态单例）——每 probe 行 = convergenceAge 色条（绿→红）+ 6 方向（±X±Y±Z）SH9 CPU 评估色板（逐行镜像 EvalSH9Irradiance + 同款 tone map）。入口：Environment 面板 PROBE DEBUG 行 "Inspector…" 按钮。512 probe 显示上限（大格只看前 512）。
> - 门禁：Engine/EngineDLL 构建 ✅、C# 0 错误 ✅、编辑器带新构建启动稳定 + 冒烟 PASSED ✅。**活体目验**：编辑器留运行——Environment 面板 PROBE DEBUG → "Inspector…" 打开检查器（需先有 GI 渲染帧写入数据；纯启动未跑 GI 帧时显示空/提示）。

> **实施状态（2026-09-04 续 21）**：B1 L3 Vulkan 端 irradiance 路径补齐（跨后端对齐完成）。
> - 新增独立 `ProbeSphereIrradiance.frag`（合法单入口 GLSL：与 ProbeSphere.vert 输出接口对齐 + set0/binding1 SSBO + SH9 评估同 Metal 数学），`glslc --target-env=vulkan1.2` 编译为同目录 `.spv`（4256B）✓；模块 Vulkan 分支 irradiance FS 加载名指向该 spv（入口名 main）。加载失败仍自动回退色相管线。
> - Vulkan 运行目验待切 API 下拉的 Vulkan 会话（MoltenVK）；spv 编译成功 + 加载路径对齐即本轮闭环。
> - 顺手修正：A6 保存路径的 `ApplyPcgPointsToScene` 在无 Project 引用时不再越界（slotGuids 循环加了 Project 空保护——此前轮次已含）。

> **实施状态（2026-09-04 续 20）**：B1 L3 step 2——probe 球 SH9 irradiance 采样已实施。
> - **关键勘察**：运行时 DDGI 的 irradiance 缓冲**不是**经典 octahedral atlas，而是 **SH9 表示**（`DDGIUpdateIrradiance.metal` 写入 `irradianceOutput[probeIdx×9 + i]`，9 个 float4 系数/probe）——probe 球采样因此退化为按片元法线评估 SH9（数学逐行镜像 `CommonFunction.metal::EvalSH9Irradiance`），无需 atlas 常量。球面法线天然等于"probe 中心→片元"方向。
> - **实现**：①`ProbeSphere.metal` 新增 `probe_sphere_irradiance_fs` 入口（`[[buffer(1)]]` device 只读缓冲，色调映射防 SH 直值过曝）；②模块 descriptor layout 加 binding 1（FS StorageBuffer），`AddPass` 时绑定 `ddgi_pass_->GetIrradianceBuffer(cbIdx)`，第二条像素管线（同 VS）按 `useIrradiance` 选择——irradiance 缓冲无效或管线未建成自动回退色相着色；③调用点传入缓冲（`psIn.irradiance_buffer`）。Vulkan 端 spv 未重编（GLSL 单入口限制需独立 frag 文件+glslc 流程），Vulkan 继续色相回退。
> - **活体验证**：编辑器日志 `[ProbeSpheres] initialized (pipeline=24 irradiance=25)`——采样管线编译/创建成功；开关目验（球面色相→真实 GI 颜色）留桌面会话：Environment 面板 PROBE DEBUG 行勾选 "Probe Spheres"。门禁：构建 ✅、回归 68/68 ✅。

> **实施状态（2026-09-04 续 19）**：B1 L3 probe 球 overlay 已实施（**修正续 17 结论**）。
> - **续 17 结论修正**：生产管线是**逐模块自加载字节码**（GPUDrivenDrawPipeline::LoadShaderBytecode 模式——GPUDriven/SSAO/SSGI/LumenDDGI 全这样初始化，编辑器日志可见其 Initialized 证据），并非"引导体系整体缺失"；缺的只是 probe 球等少数模块没有对齐该模式。B1 L3 因此可单点落地（本轮即证）。
> - **实现**：①`EngineTest/shaders/ProbeSphere.metal`（MSL，VS/FS 双入口，[[stage_in]] 顶点流 + [[buffer(2)]] push constants——Metal RHI 把 PushConstants 约定映射到 setVertexBytes index 2；颜色 = per-probe 黄金比例稳定色相 + 方向光漫反射，相机 2m 内探针推出裁剪域）；②`DebugProbeSphereModule::Initialize` 句柄 INVALID 时**自加载**（Metal 读 MSL 源运行时编译、Vulkan 读 Debug/ProbeSphere.{vert,frag}.spv、其它平台禁用）；③`StandardRenderPipeline::InitializeSubsystems` 模块创建放宽为始终尝试（测试注入路径不变）；④导出 `PipelineSetProbeSphereDebug/Get` + C# 绑定 + EnvironmentView PROBE DEBUG 行新增 "Probe Spheres" 开关（直推引擎 per-frame bool）。
> - **活体验证**：编辑器日志 `[ProbeSpheres] initialized (pipeline=24)`——MSL 运行时编译 + 管线创建成功；冒烟 PASSED、C# 回归 58/58 ✅。颜色/遮挡观感留桌面会话勾选 "Probe Spheres" 目验（irradiance 方向采样属 L3 step 2，同 Vulkan bring-up 的分步节奏）。
> - **调试留痕**：编辑器运行时 dylib 实际加载点 = `Darwin/PrimalEditor_Avalonia/DebugEditor/`（build_app.sh 布局），csproj 邻近的 `Darwin/DebugEditor/` 是诱饵位置——重编译后必须 cp 到前者，否则改动静默不生效（本轮两次空转的根因）。

> **实施状态（2026-09-04 续 18）**：GUI 会话恢复——受阻验证全部补跑完成。
> - 编辑器活体冒烟 `scripts/primal_smoke.sh` **PASSED**（续 15/16 受阻项闭环：含 PCG 编辑器/曲线编辑器/SCM 面板代码在活体内的启动与闭环）。
> - 全量 C# 测试基线更新：**640 中 634 通过、6 失败**，全部为记录在案的既有范畴——3 个纹理烘焙端到端（环境依赖）+ Nodify/LiquidGlass 主题 WIP 失败 + MaterialPreview 可视化（工作区未提交主题 WIP 范畴；此前记录为 8 个，WIP 演进中部分自愈）。本会话新增的 31 个测试（A6 PCG/坐标持久化、B4 P3 曲线 wire、A8 GitService 端到端/porcelain、排序稳定化、A7）全部通过，无新增失败。
> - GitService 端到端测试修正两处环境事实：①`git commit` 的 "nothing to commit" 走 **stdout**（Commit 软成功判定补 stdout 检查）；②macOS `/var`→`/private/var` 中间组件符号链接使 `git rev-parse --show-toplevel` 返回物理路径（测试比较用前缀剥离规范化；GitService 本体不受影响——面板展示根路径时两种写法等价）。
> - ParticleCurveData wire 契约 5 个单测（float 切线+排序往返 / RGBA / XYZ / 截断防御 / 8 字节头与引擎注释逐字节对齐）。

> **实施状态（2026-09-04 续 17）**：A6 增量——画布坐标持久化 + B1 L3 判断升级。
> - **坐标持久化**：勘验 PCGSerializer 的手写解析器按键名取字段（ExtractStringValue/ApplyParams 定位 `"type"`/`"params"`），未知字段天然被忽略——故画布坐标以 `"pos":[x,y]` 嵌进引擎 JSON 的 nodes 对象，**引擎侧零改动**（旧引擎读新文件不受影响）。实现为两个纯函数：`MergePositions`（保存时按 nodes 序 merge 画布坐标，JSON 畸形/无 nodes 原样返回不阻塞保存）+ `TryReadPos`（加载时有 pos 用 pos、缺失/畸形回退网格摆位）。单测 6 个（merge 写入/部分覆盖/malformed 回退/无 nodes/往返/畸形 pos）。
> - **B1 L3 判断升级（probe 球 overlay 搁置）**：勘察发现编辑器 shader 引导体系整体缺失——`CreateStandardRenderPipeline`（EngineDLL 生产引导）不调 SetShaderHandles（全仓仅测试调用），fullscreen 模块族（DeferredLighting/SDF viz/probe sphere）在编辑器路径全部 INVALID_SHADER 优雅跳过；probe 球 shader 仅存在 Vulkan .spv（测试从仓库相对路径加载），Metal 端无 MSL。L3 实做 = Metal 运行时 shader 编译引导 + DDGI irradiance 布局采样 + 模块接线，超出单点任务且当前无 GUI 会话无法视觉验证——挂起待 GUI 会话 + shader 引导体系专项。

> **实施状态（2026-09-04 续 16）**：A6 "应用到场景"路径已实施（A6 全闭环）。
> - **文档模型优先**：不新增引擎建实体导出——编辑器经既有输出读回导出（`PCGGetOutputPositions/Transforms/MeshSlots`，C# 绑定本轮补齐）取点，交给 `WorldEditorViewModel.ApplyPcgPointsToScene` 批量建实体：`Transform`@点位 + scale/rotY（PCG 与引擎边界同为弧度直传，无需换算）+ `MeshRendererComponent`（mesh 库索引 → 注册资产 GUID，工程相对路径 + .model.json 双候选解析，失败点只带 Transform 并告警）；整批单 undo 事务（A5 `BeginTransaction` 同款）+ SyncEntity 逐实体推引擎。
> - **引擎增量（只增）**：`PCGGetMeshSlotFlat`（PCGMeshSlot 含两个 std::string，不可 marshal，扁平出参替代，指针指向图内槽位、mesh 库变更前有效）。
> - **接线**：`PcgApplyPayload`（positions/transforms/meshSlots/slotPaths）+ `PcgEditorViewModel.ApplyToSceneCallback` 静态回调（WorldEditorViewModel 接线，同 ParticleSavePresetCallback 模式）+ 目标节点解析（选中优先，否则最后一个有输出点的节点）+ 工具栏 "Apply to Scene" 按钮。
> - 门禁：EngineDLL/C# 构建 ✅、测试 108/108 ✅（活体冒烟仍待 GUI 会话恢复，见续 15 留痕）。

> **实施状态（2026-09-04 续 15）**：A6 PCG 图编辑器已实施（Track A 主线收官）。
> - **勘察修正**：引擎侧 PCG 全栈早已就绪——`PCGGraph`/13 种节点/`PCGSerializer`（自写 JSON，descriptor 驱动，自动支持新节点）/`PCGReflection`，且 `EngineDLL/PCGAPI.cpp` 已有 30 个导出（图生命周期/节点 CRUD/反射/参数 Set/mesh slots/Execute/输出读回/错误/Serialize/Deserialize/注册表），此前会话已提交。**剩余全部为编辑器侧工程**。
> - **导出面增量（只增）**：`PCGGetNodeParamDescriptorFlat` / `PCGGetNodePinDescriptorFlat`——扁平字段出参替代结构体 marshal，规避 PCGParamDescriptor 跨 ABI 布局风险。
> - **C# 绑定**：`NativeEngineBridge.Pcg` 嵌套静态类（~20 绑定：Create/DestroyGraph、AddNode 返回引擎 id、Remove/Connect/Disconnect、Deserialize/SerializeGraph、注册表、Flat 描述符、SetParamFloat/Vec3/Array、Execute、GetOutputPointCount、错误读回）。
> - **编辑器**：`Editors/Pcg/` 四件——`PcgGraphViewModels`（Node/Pin/Param/Connection VM，NodifyM 契约镜像材质编辑器：ConnectorViewModelBase/ConnectionViewModelBase+占位 NodifyEditorViewModelBase/ConnectionCompletedRelayCommand 的 ITuple 载荷识别）；`PcgEditorViewModel : AssetEditorViewModel`——加载 = 文件 JSON 引擎 Deserialize + C# 解析同 JSON 重建画布（**params 初值取自 JSON**，引擎无 GetParam 值导出）；节点增删严格同步引擎 id 移位语义（RemoveNode 后续 id 前移，画布节点/连接端点同步改写）；参数编辑即推 SetParamXxx；保存 = 画布状态已在引擎，SerializeGraph 写盘；Execute 回显错误与逐节点输出点数。`PcgEditorView.axaml` 三栏（palette/Nodify 画布/检查器+Execute）。注册：`AssetType.Pcg` + ContentHelper ".pcg" 映射 + AssetEditorRegistry/Host BuildView + 预览状态行。
> - **留痕**：①PCGSerializer JSON 无节点坐标字段，画布布局不持久化（加载为网格摆位）——加坐标字段需双侧改序列化器；②FloatArray 参数（MeshAssign weights）v1 只读展示；③"应用到场景"（PCGEntityFactory 包装导出 → 批量建实体）未接，为 A6 增量；④PCG 全局图为 DLL 单例，多 .pcg 同时编辑会互相覆盖（v1 单开语义）。
> - 门禁：EngineDLL 构建 ✅、C# 0 错误 ✅、测试 103/103（Cli+Material 回归+新增 4 个 PcgEditor 降级回归）✅。**编辑器活体冒烟本轮环境受阻**：会话 GUI 连接失效（Avalonia.Native RenderTimer -6661，平台初始化层崩溃，先于任何编辑器代码执行，与此轮改动无关——此前 B4 P3 轮同环境冒烟通过），留待 GUI 会话恢复后补跑 `scripts/primal_smoke.sh`。

> **实施状态（2026-09-04 续 14）**：B4 P3 曲线编辑器已实施（B4 全部完成）。
> - **引擎导出面（只增不改）**：`ParticleEmitterAPI.cpp` 新增三导出，参数为**实体 id**（与 EmitterSetSpawnRate 同款，C# 只持实体 id）——`EmitterGetCurveData`（wire 读回，无曲线时 out_size=0）、`EmitterSetCurveData`（wire 全量替换）、`EmitterDisableCurve`（关槽位并释放）。槽位 0..5 = scale/alpha/color_gradient/velocity/force/rotation；wire = u32 interpolation + u32 keyCount + keys（float 槽 16B 键 {t,v,inTan,outTan}、color 20B、vector 16B）。
> - **所有权纪律**：`emitter_config.curves` 是裸指针、`~particle_emitter = default`（引擎不拥有）、预设只值拷标量——故 P3 新建的曲线对象登记在 DLL 内 `g_owned_curves`（emitter_id→[6]），替换/`EmitterRemoveFromEntity`/整体 `EmitterSetConfig`（SanitizeConfig 清发射器侧指针之后）统一 delete。无共享、无二次释放。`float_curve::keyframe_count()`、`vector_curve::get_interpolation()` 两个只增 getter。
> - **编辑器**：`ParticleCurveData`（wire 序列化镜像）+ `EmitterCurveEditor`（Avalonia 自绘 Control：网格+折线渲染、拖键帧、双击加键、右键删键、Ctrl+拖 out 切柄）+ `ParticleCurveEditorWindow`（非模态单例，6 tab = 启用开关 + 插值下拉 + 曲线控件；启用即建默认两点曲线；编辑即推引擎）。入口：属性面板 PARTICLE EMITTER 区块 "Curves…" 按钮（code-behind Click → ShowFor(SelectedEntityNativeId)）。
> - **留痕**：曲线数据仅存于运行中的发射器，场景文档不持久化曲线——持久化走预设文件链路（preset_json 自含曲线快照，待后续把编辑器曲线写回预设）；`EmitterSetConfig` 全量替换会清曲线（编辑器的重建路径随后需重推曲线，当前以"编辑即推"缓解）。
> - 门禁：EngineDLL 构建 ✅、导出符号 nm 验证 ✅、Particle ctest 3/3 ✅、C# 0 错误 ✅、作用域 46/46 ✅、编辑器冒烟 PASSED ✅（无 GUI 会话，曲线控件的视觉验证待桌面会话复核）。

> **实施状态（2026-09-04 续 13）**：A8 源码控制最小集成已实施。
> - **场景 JSON 排序稳定化（git 可用地基）**：勘察确认实体/组件/属性序本就确定（List 保序 + STJ 声明序），真正的不稳定源是 3 个 `Dictionary<string,T>` 字段——`MaterialOverrideSlot.Parameters` / `PrefabInstanceComponent.Overrides` / `ScriptableComponent.Parameters`，STJ 按枚举序（=插入序=编辑序）写盘，不同用户相同逻辑修改产生假 diff。新增 `SortedStringKeysConverter<TValue>`（`GameProject/Documents/`，写侧 Ordinal 排序、读侧普通字典语义）以 `[JsonConverter]` 挂到三属性——不换集合类型、零使用点改动。单测 4 个固化（不同插入序字节一致 / 键序断言 / 往返）。
> - **GitService**（`Common/GitService.cs`）：Process.Start 调系统 git（15s 超时、永不抛出）——`ProbeRepo`（rev-parse --show-toplevel）、`Status`（porcelain v1 + `--untracked-files=all` + pathspec `-- .` 限定工程子树——工程在更大仓库内时面板不洪水）、`Commit`（add 指定路径 + commit -m；"nothing to commit" 软成功）。porcelain 解析抽为纯函数 `ParsePorcelain` 单测 4 个（混合状态码/rename 追新路径/带引号空格路径/空行）。
> - **SCM 面板**：`SourceControlPanel` 非模态单例窗口（工具栏 "SCM…" 按钮）——状态文件列表（勾选包含）+ 提交信息 + Commit；不在仓库内显示引导文案。`SourceControlViewModel` 全 git 调用走 Task.Run（UI 线程零阻塞）。
> - **状态圆点**：`AssetInfo.GitStatus`（INPC setter）+ `GitStatusConverters`（可见性/颜色：M/D 橙、A/? 绿）→ 内容浏览器列表行 + 网格块两处模板；面板每次 Refresh 后把状态推给 AssetRegistry。
> - 门禁：C# 0 错误 ✅、作用域测试含新增 8 个（排序 4 + porcelain 4）全过 ✅、编辑器冒烟 PASSED ✅。

> **实施状态（2026-09-03 续 12）**：B4 P4 UI 面收尾（P4 全部完成）。
> - **另存预设按钮**：勘察发现属性面板 "Save…" 按钮 + `ParticleSavePresetCallback` 链路**已存在**（P2/P4 脊柱实现时已带）——本轮只补了保存成功后 `RefreshParticlePresetOptions()`（新列表缓存失效，预设下拉立即可见自定义项）。
> - **内容浏览器右键应用预设**：列表/网格两个资产模板挂 ContextMenu（`IsParticlePreset` 可见性控制，仅 .particle.json 显示）→ "Apply to Selected Particle Entity" → `WorldEditorViewModel.ApplyPresetToSelectedParticleEntity`：先 `ParticlePresetLoadFromFile`（**C# 绑定本轮补齐**——引擎侧 `ParticlePresetLoadFromFile` 早导出但无人绑，自定义预设不装载则 apply 回落 "fire"）再穿 Inspector 的 PresetName 切换路径（拆旧发射器重建）；`PropertiesViewModel.RefreshParticlePresetOptions` 为本轮新增。
> - **拖入场景创建粒子实体**（B4 验收单句"拖入场景创建实体"的缺口）：`DropAssetAsync` 补 ParticlePreset 分支（Prefab 之后、mesh 之前）——实体名=文件 stem，Transform@落点 + ParticleEmitterComponent，装载预设后 AddEntity + AddEntityAction undo + SyncEntity。
> - 门禁：C# 0 错误 ✅、作用域测试 46/46（Cli+Particle+Serializer）✅、编辑器冒烟 PASSED ✅。

> **实施状态（2026-09-03 续 11）**：A7 视图模式最小集（v1）已实施。
> - **线框（Wireframe）**：勘察确认 RHI 管线描述早已支持 `FillMode::Wireframe`（Metal → `TriangleFillModeLines`，Vulkan → `VK_POLYGON_MODE_LINE`，均为 PSO 创建参数），故实现为 **GBuffer 管线的 fill-mode 孪生**：`GPUDrivenDrawPipeline` init 时与 `draw_pipeline_` 一起创建 `draw_pipeline_wireframe_`（同 shader 同状态，仅 fillMode 不同；失败安全回落 Shaded 并告警）；`NaniteSceneRender` pass 绑定处按 `view_mode_` 选择（`GPUDrivenDrawPipeline.cpp:2609` 附近）。流式网格走同一绑定管线，自动跟随。
> - **导出面**：`StandardRenderPipeline::SetViewMode/GetViewMode`（cheap per-frame 字段，无重建）+ NaniteSceneRender pass 每帧转发给 gpuDraw 单例；EngineDLL 新增 `PipelineSetViewMode(u32)` / `PipelineGetViewMode()`（只增不改）；C# `NativeEngineBridge` 绑定 + 工具栏 "VIEW" 下拉（Shaded/Wireframe，模式 2=Unlit 引擎侧预留未暴露）。与 B3 的 RENDER（pass 组合）正交。
> - **AABB 全显开关**：工具栏 "Bounds" 勾选（`WorldEditorViewModel.ShowAllBounds`）→ 每帧 `GetAllEntityBounds()`（复用 A1 拾取候选的 AABB 查询）→ `ViewportOverlay.AllBounds` 以暗灰蓝细线绘制（选中橙框仍在上层）；关闭时 provider 返回空，**零每帧开销**。绘制路径复用 A1 的 `DrawBoxList`（选中/全显共用）。
> - **相机书签**：`EngineViewport3D` 5 个 orbit 位姿槽（yaw/pitch/distance/target），Ctrl+Shift+D1..D5 存、D1..D5 取（空槽 no-op；仅会话内存，不持久化——书签键处理挂在视口 `OnKeyDown`，与 F 重置同层）。
> - **Unlit（2）v1 不做**：需要 deferred 像素着色器三端变体（MSL/WGSL/spv）+ engine shader blob 扩展，M 级成本，超出本任务 S 预算；枚举位已预留，Dawn 端 WebGPU 无 fill-mode 概念（线框请求在 Dawn 自动回落 Shaded）。
> - 门禁：Engine/EngineDLL 构建 ✅、C# 0 错误 ✅、CLI 43/43 ✅、内容域 ctest 6/6 ✅、编辑器活体冒烟 PASSED（线框孪生创建无告警、渲染基线无回归）。

> **实施状态（2026-09-03 续 10）**：A9 PR2（CLI 端到端验证）已实施。
> - **primal-engine cook-mesh** 新增 `--uvatlas` / `--uvatlas-lightmap`（`Tools/PrimalEngineCli/main.cpp`）：带旗标时改走 `CookMeshWithPipeline`（POD 按 ContentToolsCLI --pipeline 默认组合装配；lightmap = `uvatlas_target_purpose=1` + gutter 4 + strict pack，直接 include 权威头 `AIAssetPipelineParams.h`，遵守 §9"禁止 POD 手工镜像"）；两种 cook 完成后都做**产物读回校验**（SceneBlobReader `WalkSceneBlob` 重走磁盘 blob），JSON 响应新增 `"meshes"` 与 `"lightmap_uv_meshes"`；offline schema 表（`schema` verb，offline-v2）同步补两个参数。**C# 编辑器侧零改动**——`GeometryImportSettings.UvatlasTargetPurpose`（`PipelineUvAtlasPurpose.Lightmap`）与 208B POD 镜像本已存在，导入面早已能请求 Lightmap 用途。
> - **读回统计面**：`ValidationReport` 新增 `lightmap_uv_mesh_count/lightmap_uv_coord_count`（Validator visitor 从 `PackedMeshView.lightmap_uv_count` 聚合）；`ContentToolsCLI --validate` 打印 "Lightmap meshes / Lightmap UVs" 两行。
> - **端到端实证**（primal-engine，同一 tetra 输入三路径）：default → `6 meshes, 0 lightmap`；`--uvatlas` → `6, 0`；`--uvatlas-lightmap` → `6, 1`；`--validate` 对 lightmap 产物报 `Lightmap meshes: 1 / Lightmap UVs: 19 / PASS`（19 > 6 为 xatlas seam 展开重复顶点，合理）；`a9_lm.model.json` sidecar 正常产出；引擎 `normalize_cooked_mesh_scene` 接受该 blob。
> - **PR2 期间抓出并修复两个漏网的逐 mesh 步进器**（PR1 全仓扫描的缺口，`--uvatlas-lightmap` 首跑即 SIGBUS 暴露）：① `ContentTools/Geometry.cpp` `skip_mesh_metadata_only`（`.model.json` sidecar 写入器 `write_model_sidecar` 的 blob 步进器，不认 UVLM 段导致 memmove 越界 `EXC_BAD_ACCESS`）；② `Engine/Graphics/Nanite/OfflineSDFMerger.cpp` `LoadFromPipelineModel`（离线 SDF 合并的步进器，补 flag 感知跳段 + 边界守卫）。修复后全链路复测通过。教训：blob 布局扩展点的完整清单应以"所有出现 SDF-magic 的文件"为界（本次扫尾 grep 即据此兜底），而非仅"显式解析 elements_type 的文件"。
> - 门禁：内容域 ctest 6/6 ✅、primal selftest ✅、C# 构建 ✅（DebugEditor dylib 已刷新）。

> **实施状态（2026-09-03 续 9）**：A9 Lightmap UV 消费链地基段（PR1）已实施。
> **方案修正**：勘察发现元素格式已存在 20B（紧凑）/24B（Mac `math::v2` simd 对齐）两种在线布局、消费端靠 stride 嗅探区分且无类型位可辨（`SceneDataAdapter.cpp` 的 `>=24` 分支、`elementSize==20` 嗅探），原地扩容 20→28B 会产生 4 种布局混存并波及 ~12 个 Metal shader + Vulkan/WGSL + 已提交烘焙资产重烘——即计划自标的"顶点格式跨后端同步"风险实锤。故改为**容器级附加段**（blob 格式只增不改，沿 MSHL/SDF 同模式，也正是 `ProcessableMesh.h:29` 注释预留的 "Phase 2 sidecar target"）：
> - **Blob 格式**：`elements_type` 空闲 bit **0x10（`lightmap_uv_flag`，ContentTools/Geometry.h）** 置位表示 mesh 尾部（SDF 段后）存在 UVLM 段：`u32 magic 0x4D4C5655("UVLM") | u32 uv_count | uv_count × f32(u,v)`，不变量 uv_count == num_vertices（部分覆盖则告警丢弃）。flag 确定性判定替代 peek——旧 blob（flag 清）解析路径零改动且无越界 peek；无 Lightmap UV 的新 blob **字节级等同现状**（默认烘焙实测 0 段）。
> - **ContentTools**：`PackedMesh.lightmap_uvs`（PipelineTypes.h）+ `BuildPackedMesh` 从首个 Lightmap-purpose UVSet 提取 + `Serialize/GetPackedMeshSize` 段写入 + `SceneBlobReader`（flag 剥离 + 段读出，`PackedMeshView.lightmap_uv_count/uvs` 零拷贝）+ `PackedMeshDecoder.DecodeScene` 把段还原为 Lightmap UVSet（decode→re-encode 往返保真）+ **新增 CLI 旗标 `--uvatlas-lightmap`**（ContentTools/main.cpp；修正原评估"CLI 参数齐全"——实际只有 `--uvatlas` 且默认 Texture 用途，C ABI POD 的 `uvatlas_target_purpose` 桥接早已存在只是无人能触达 Lightmap）。
> - **引擎**：`RHIMeshAsset.lightmap_uv_buffer`（f32 uv 对 sidecar，无 GPU 消费者）+ 四处解析点全接：`ContentToEngine.cpp`（`parse_mesh_to_asset` 读入 + `skip_mesh_in_blob` 跳段 + `create_rhi_*` 路径）、`CookedMeshAdapter::read_mesh`（守门人：新 blob 若不接会被 "unexpected trailing scene data" 拒绝）、`SceneDataAdapter` 三个解析器（flag 剥离 + 段读入 meshAsset）。
> - **测试 5 新**：Writer 侧 `test_lightmap_uv_absent_by_default`（字节级兼容）/`test_lightmap_uv_sidecar_round_trip`（含 flag 位与读回值校验）/`test_lightmap_uv_partial_coverage_dropped`；Decoder 侧 rehydrate 往返；引擎侧 `LightmapUVSidecar_Parse`（flagged 子网格居首，证明层级遍历同步）。
> - **端到端实证**：`ContentToolsCLI --pipeline t.obj out.mesh --uvatlas-lightmap` → blob 含 UVLM 段（uv_count=12 atlas 归一化坐标）→ `normalize_cooked_mesh_scene` 接受；默认烘焙产物 0 段。
> - **门禁**：内容域 ctest 6/6 ✅（TestSceneBlobWriter 16/16、TestPackedMeshDecoder 10/10、MeshFormatRoundTrip 3/3、TestCookedMeshAdapter、TestSceneBlobReader、TestMetalContentFormatConversion）；C# 0 错误 ✅、CLI 43/43 ✅、primal selftest ✅；编辑器闭环冒烟 PASSED（加载已提交 24B Mac 布局 corridor .mesh 零回归，渲染基线 identical）；`primal-engine` engine info/render（Metal）/cook-mesh ✅；TestModularPipeline 75s 窗口无 fault（仅既有 metallib 环境警告）。既有失败（与本次无关）：TestRendererMac.cpp 编译、TestVulkanSponzaRenderGraph（需独立 Vulkan 树）、TestRenderFrameAPI.cpp。
> - **PR2 待办**（CLI 端到端接线）：编辑器 CookMeshWithPipeline 面暴露 `uvatlas_target_purpose=1`（POD 字段已存在）+ primal-engine cook-mesh 加 `--uvatlas-lightmap` 透传 + 读回校验命令；shader 采样/烘焙存储仍属 XL 路线段（届时 sidecar 以独立 vertex-pulling buffer 绑定，或按需再议顶点格式扩容）。

> **实施状态（2026-09-03 续 8）**：B4 P4 脊柱已实施——引擎 `ParticlePresetSaveToFile(name, path)` + C# 绑定；`AssetType.ParticlePreset` 枚举 + `ContentHelper` `.particle.json` 映射 + 预览状态行。至此预设的列（PresetList）/装（LoadFromFile）/存（SaveToFile）/套（ApplyToConfig）四条 ABI 全通。P4 剩余 UI 面：属性面板"另存预设"按钮与右键应用预设入口（薄接线，ABI 已齐）。

> **实施状态（2026-09-03 续 7）**：B4 P2 属性面板已实施——PropertiesView 新增 "PARTICLE EMITTER" 区块（单选实体时显示）：Preset 下拉（`ParticlePresetList` 懒加载，切换 = 拆旧建新经 `PushParticlesRebuildCallback`）、SpawnRate 文本框（`EmitterSetSpawnRate` 热更）、Enabled 勾选（关 = spawn rate 0）。旗标走 `_msEntity.SelectedEntities`（与既有组件区块同款），选中刷新处补 `HasParticleComponent`/`Particle` 通知。门禁：0 错误 ✅、70/70 ✅。

> **实施状态（2026-09-03 续 6）**：B4 粒子 P1 已实施。
> - 引擎：新增 `EngineDLL/ParticleEmitterAPI.cpp`（CMake 已挂）——`ParticleEnsureSystems`（幂等补 particles::initialize + preset_system + 8 内置预设）、`ParticleEmitterConfigSize`、`EmitterCreateForEntity`（entity→component::create；**SanitizeConfig 把 curves 6 个堆指针置空 + use_* 关闭**，跨 ABI 不传引擎堆对象）、`EmitterRemoveFromEntity`、`EmitterSetConfig/GetConfig`（get_emitter→set_config，**运行时整体替换无需重建**——修正原规划"需重建"的假设）、`EmitterSetActive/SetSpawnRate/Burst`（组件层成员路由，burst 需 pool 走组件内部）、`ParticlePresetList/LoadFromFile/ApplyToConfig`。
> - 勘察修正：emitter_config sizeof≈304（v3/v4 alignas16，**offset 8 = spawn_rate**）；preset apply_to 只值拷标量；material_id 被 component::create 忽略。
> - 编辑器：`ParticleEmitterComponent`（PresetName/SpawnRate/IsEnabled）+ Serializer 三处 switch（"particleEmitter"）+ Synchronizer 推送分支（preset apply → SpawnRate 覆盖 offset8 → EmitterCreateForEntity）+ 层级右键 "Add Particle Emitter" + INativeEngineBridgeLike 接口/Adapter/测试 Fake 桩。
> - 门禁：EngineDLL ✅、C# 0 错误 ✅、作用域 70/70 ✅。
> - P1 剩余：组件级 undo、Remove 入口 UI；P2：属性面板（预设下拉/SpawnRate/Active/Burst）+ Add Component 注册表激活；P3：曲线编辑器；P4：预设库资产化。

> **实施状态（2026-09-03 续 5）**：A5 Array 批量布设已实施——工具栏 "Array…" 按钮 + 层级右键 "Array Duplicate…" → `Views/ArrayDuplicateDialog`（轴/数量 1..256/间距，间距取吸附步长时阵列落格）→ `WorldEditorViewModel.ArrayDuplicateSelected`（Serializer 深拷循环偏移、整批单 undo 事务）。**语义发现**：引擎无父子变换组合（transform 全世界系），平移必须落到克隆子树每个实体，否则子级掉队——单测 2 个固化该行为（含子级 0.5+3=3.5 断言）。**关联缺口**（未修，记档）：gizmo 多选拖拽同样存在子级掉队问题，属"层级变换组合"缺失的同一根因；reparent 的世界位置保持也受此限制。

> **实施状态（2026-09-03 续 4）**：B1 L1.5（probe 格点可视）已实施——`GiVolumeViewModel.ShowProbes` + `ComputeProbePoints()`（CPU 解析格点 origin+(i+0.5)×spacing，>2048 点自动抽稀）+ `ViewportOverlay.ProbePoints` 黄点绘制 + EnvironmentView "Show Probes" 开关。与引擎 Initialize 上传的 probe_positions_ 格点一致，零 GPU 读回。
> **L2 重新评估**：GPU readback（irradiance/depth/confidence 三视图）需在 DDGI AddPass 帧内 hook 命令缓冲做 stage-copy（PCGSDFReadbackManager 模式），非独立 ABI 调用可完成——工作量从 S 修正确认为 M，且 probe 位置可视（用户第一诉求）已由 L1.5 以零成本覆盖。readback 立项前建议先明确要看的字段（irradiance atlas 排布需读 DDGI shader 常量）。

> **实施状态（2026-09-03 续 3）**：B2 v1 已实施——雾参数消费接进既有 Froxel 体积雾（免写新 shader）。
> - `UpdateSceneEnvironment` 补齐雾段消费：Density→extinction_scale（×10）、Fog RGB→scattering_albedo（亮度）、Far−Near→density_fade_range、FogEnabled→`enable_froxel_fog`；参数未变化时跳过 UpdateSettings（防面板拖动每 tick 重建）。
> - `StandardRenderPipeline::ApplyConfigChanges` 补 froxel 运行时生命周期：开关翻转时创建/销毁 pass（原本只在 init 创建、Lumen-only 重建不覆盖）。语义：**参数编辑后需 off→on 一次重建生效**（Initialize 只读一次 grid config）。
> - 编辑器零改动（Fog 参数本就每帧经 ApplyToNative 发送）；C# 45/45 ✅；引擎级冒烟 ✅（player 包 FogEnabled=1 → `[FroxelFogPass] Initialized` 运行时创建证据）。
> - 留痕：FogNear/Far 不直接消费（只进 fade range）；简单解析距离雾 pass、雾 shader 编辑入口、简单/体积模式选择均为后续增量。

> **实施状态（2026-09-03 续 2）**：B1 L1 核心已实施。
> - 引擎新增 `GISetProbeVisMode(u32)`（probe 调试可视化 0..4，数据面当前接线 Vulkan GIGather）。
> - C# 绑定：volume CRUD（`LumenVolumeConfig` 以 64 字节不透明缓冲 + 固定偏移镜像，见 NativeEngineBridge GIOff* 常量）+ gizmo（SetMode/GetSelectedVolume/Translate/Scale）+ vis mode。
> - 面板：EnvironmentView 新增 "GI VOLUMES" 区块（体积下拉/Refresh/Append/Remove/Materialize + counts/spacing/rays/max dist/origin+Use 编辑 + Apply）与 "PROBE DEBUG" 下拉；数据源 `ViewModels/GiVolumeViewModel.cs`（SelectedBox 按 GIVolumeGizmo AABB 语义解析计算）。
> - 视口：EnvironmentView 侧选中体积以青色线框绘制（`ViewportOverlay.VolumeBoxes` + `EngineViewport3D.VolumeBoxProvider`，WorldEditorView 接线）。
> - **L1 留待后续**：GIGizmoPick 屏幕点选/拖拽需要 C# 侧逆视投影矩阵（与引擎相机数学对齐风险，宁可后置）；probe 球体 overlay 需引擎侧 probe_sphere_vs/ps 注册；probe_vis 数据面仅 Vulkan。
> - A4：`Serializer.ToEntityDocuments`/`InstantiateEntityDocuments`（guid 重分配防多实例身份冲突）+ 层级右键"Create Prefab…" + `DropAssetAsync` prefab 分支（undo 事务 + PrefabInstanceComponent）+ 只读检查器（`Editors/Prefab/`，注册进 AssetEditorRegistry/Host）+ 预览状态行 + Copy/Paste 升级为全组件深拷（经 EntityDocument 往返）。往返单测 2 个 ✅。
> - A10：引擎新增 `GISetProbeFreeze(u32)`（`GIDebugParams.freeze_probe_updates` 的单字段开关，规避 C# 侧结构体偏移硬编码）+ `SceneEnvironment.StaticGIFrozen`（文档字段，序列化接线）+ EnvironmentView 勾选框，`ApplyToNative` 统一推送（场景切换/管线重建后状态自动恢复）。烘焙 warm-start 仍走 GIEditorPanel。
> - 测试基线：CLI+Prefab 45/45 ✅；全量 609 中 11 失败均为**既有**问题——8 个主题/可视化失败源于工作区未提交的主题 WIP（WarcraftTheme.Styles.axaml / MaterialEditorView.axaml / WarcraftVisualDump.cs，stash 对照实验证实），3 个纹理烘焙端到端在干净 HEAD 上同样失败（环境依赖）。

## 5.5 Run/Stop 重写实施状态（2026-09-03，规划外追加）

Run 旧语义（`build_run.sh` 重启编辑器自身）已替换为：**导出运行时场景包 → 启动外部宿主 PrimalPlayer**。

- 编辑器导出：`Content/RuntimeSceneExporter.cs`（"primal-runtime-scene v1" 行式格式；env 为 UpdateSceneEnvironment 24-float 布局原样直传，与 `SceneEnvironmentViewModel.ApplyToNative` 逐位对齐；实体含 transform/mesh 源资产路径/light/camera + is_main 标记）。
- 宿主：`Tools/PrimalPlayer/`（main.cpp 行式解析 + ABI 注入 + 渲染循环；player_window.mm AppKit 窗口；CMake 沿 PrimalEngineCli 模式，链接 EngineDLL 并部署同目录）。关键 ABI 契约：带网格实体走 `ImportMeshAsync` 轮询 + `PipelineRegisterMeshEntity`；灯光走 `CreateEntity`+`AddEntityLight`；相机由主函数对选中实体调一次 `CreateCamera`。**槽位哨兵纪律：slot 0 是合法分配，只认 0xFFFFFFFF**；`CreateRenderSurface` 返回 0 同样是合法索引（首 surface）。
- 编辑器接线：`Common/RuntimeLauncher.cs`（repo root 解析 + (api,config)→构建树映射 + 存在性校验）+ `BuildService.StartPlayer`（复用进程句柄/Stop/日志事件）+ 工具栏 API 下拉（Metal→Darwin/Debug|Release，Vulkan→Darwin/DebugVulkan）。
- 门禁：PrimalPlayer 构建 ✅；无 GUI 会话冒烟 ✅（包解析 → InitializeEngine → 注入 → 渲染循环 → `presentDrawable + commit done` 每帧证据）；C# 0 错误 ✅；CLI 43/43 ✅。
- 已知留痕：①Esc 退出暂缺（`addLocalMonitorForEventsMatchingMask` 在无 GUI 会话报 unrecognized selector，待桌面会话核实），关闭按钮可用；②材质 Phase 1 走默认白材质（texture id 0，primal-engine 同款），材质图/贴图注入留给 Phase 3（PrimalBuild cooked 包路线）；③Vulkan/Release 树的宿主要先构建对应树才可 Run。

## 5. 交叉验收门禁（每个 PR）

1. `ctest --test-dir Darwin/Debug -R "Unit"`；集成窗口测试跑 `TestModularPipeline` / `TestVulkanSponzaRenderGraph` 无 fault。
2. 新增 ABI：`static_assert(sizeof)` + 只增不改；编辑器通道改动跑 `dotnet test PrimalEditor_Avalonia.Tests -c DebugEditor --filter "FullyQualifiedName~Cli"`（改命令面则 `PRIMAL_UPDATE_SCHEMA=1` 重生成快照随 PR 提交）。
3. 双端对拍：Metal/Vulkan 视觉一致性（沿 `Docs/metal_visual_alignment_20260902.md` 流程）。
4. 场景往返：保存/重开工程后 probe 体积配置、雾参数、粒子组件无损。

---

## 6. 附录：分批实施详规（贴合代码）

> **实施状态（2026-09-03）**：第 0 批（A1/A2/A3/B3）已实施。引擎新增 `EntityGetWorldAABB`/`PipelineApplyModePreset` 两导出（`RenderPipelineAPI.cpp`，纯新增）；编辑器改 `NativeEngineBridge.cs`/`WorldEditorViewModel.cs`/`WorldEditorView.axaml(.cs)`/`ViewportOverlay.cs`/`EngineViewport3D.axaml.cs` + 新增 `SnapSettings.cs`/`ReparentEntityAction.cs`。门禁：EngineDLL 构建 ✅、C# 构建 0 错误 ✅、CLI 测试 43/43 ✅、单测（RenderProxy/RenderScene/MaterialGraphAPI/ParticleMaterialAPI）✅、TestRenderPipelineAPI 19/20（唯一失败 "Near spacing default" 为既有 GI 默认值漂移，见下）。既有问题（与本次无关）：`TestRendererMac.cpp` 编译失败（stale `axis`/`camera_id` 字段）；单测需 `DYLD_LIBRARY_PATH=Darwin/Debug` 才能加载 libfbxsdk；本会话无 GUI 无法做编辑器活体冒烟。

> 行号为 2026-09-03 勘察值。桥绑定模式统一说明：C++ 侧导出用 `EDITOR_INTERFACE` 宏（`EngineDLL/Common.h:4-9`），每个域一个 API cpp 自带 `extern "C"` 块（仿 `ParticleMaterialAPI.cpp:16-20`），新 cpp 加入 `EngineDLL/CMakeLists.txt` 的 SOURCE_FILES（L9-35）；C# 侧 `Native/NativeEngineBridge.cs`（单 static class，2196 行）加私有 `[DllImport("PrimalEngine", EntryPoint=..., CallingConvention=Cdecl)]` + public 包装（`_isLoaded` 检查 + try/catch + `Logger.Error`，风格沿 L341-347）。线程模型：渲染帧与 VM 调用都在 UI 线程（DispatcherTimer 30Hz 驱动，`EngineViewport3D` L325-331），无需 marshal。

### 6.0 先行核清（开工前半天）

1. **每实体世界包围盒来源**：核 RenderProxy/RenderScene 是否维护 per-entity bounds（Nanite 集群化后语义需确认）。若无现成字段，A1 走退路：编辑器合成（mesh 资产 bounds × 实体 transform，bounds 从 .model.json/.mesh sidecar 读取，导入时已具备）。
2. **SyncEntity 推送的是世界还是本地 transform**（`Native/SceneRuntimeSynchronizer.cs` SyncEntity L621）——决定 A3 reparent 是否需要变换补偿。

### 6.1 第 0 批

#### A1 真实 AABB 拾取 + 选中描边

**引擎（S）**：新导出
```c
EDITOR_INTERFACE bool EntityGetWorldAABB(u64 native_id, f32 out_min[3], f32 out_max[3]);
```
实现取决于 6.0 结论：有 bounds 直查；无则在实体创建/mesh 绑定处计算缓存。

**桥（S）**：NativeEngineBridge.cs 加绑定。

**编辑器（S~M）**：
- `WorldEditorViewModel.GetPickerCandidates`（L414-447）：候选元组 `(uint id, AABB bounds)` 与 `ViewportPicker.PickNearest` 签名（`ViewportPicker.cs:40`，AABB 为 6-float 结构 L26）已匹配——把手工 1m 立方（L429 注释处）换成 bridge 查询；失效通知沿用 `PickerCandidatesChanged`（L447）。
- **选中描边：推荐 managed overlay**。新 `SelectionOverlay`（仿 `ViewportOverlay.cs` Control 模式，与 `LightIconRenderer.DrawEditorVisuals` 一起在 `EngineViewport3D` L461-464 每帧调用），用 `WorldProjector.Project`（L69）投影 AABB 8 角画 12 条边。备选：引擎 `DrawDebugBoxWireframe`（C# 绑定已在 NativeEngineBridge.cs L1397）——但它经 ForwardSceneRenderer Pass 6 排空，仅 editor-mode 前向路径可见，B3 切 deferred 后不可见，不作为唯一方案。

**验收**：间距 <1m 的两实体可分别选中；线框随相机/正交切换正确；1000 实体场景拾取无感。

#### A2 吸附

**编辑器（S~M）**：
- WorldEditorViewModel 加 SnapSettings（Enable、TranslateStep ∈ {0.05,0.1,0.25,0.5,1}m、RotateStep ∈ {1,5,15}°、Ctrl 临时取反）。
- 量化钩子推荐 **`WorldEditorView.axaml.cs::OnGizmoDrag`（L205-248）应用分量前量化**（备选 `GizmoOverlay.DragDelta` 产出前 L261/266/271——机制更干净但 overlay 不应知道 VM 设置）。Translate 对累计位置先量化再回推 delta（避免累计误差）；Rotate 直接按步长取整。
- **同 PR 顺带修复**：gizmo 拖拽目前不入 undo（OnGizmoDrag 无 Add 调用）——拖拽结束以起/止 Transform 构造 `TransformAction`（`Common/TransformAction.cs` 模式）入栈；多选用 `MultiEntityTransformAction`。注意 OnGizmoDrag 目前只取 `SelectedEntities[0]`（L205-248）——多选 gizmo 一并在此扩展。

**验收**：1m 网格两块板严丝合缝；Ctrl 临时自由；旋转 15° 步进；拖后 Ctrl+Z 回到拖前。

#### A3 reparent（过滤已存在，收窄）

**编辑器（M）**：HierarchyTree（axaml L125-138，`ItemsSource="{Binding Entities}"` 是过滤后集合）挂 Avalonia 内置 DragDrop（csproj 无需新包，视口 `AssetDropHandler.cs` 已用同机制）：
1. 文档层重挂 Parent/Children（`WorldEditorViewModel.AddEntity` L270-288 已有挂接逻辑可复用）；`EntityDocument.ParentEntityGuid` 已支持（Documents/EntityDocument.cs），**无 schema 变更**。
2. 引擎同步按 6.0 结论：保持世界 transform 不变则 reparent 前读世界变换、重挂后写回。
3. undo：新 `ReparentEntityAction : IUndoRedoAction`（接口 `Common/UndoRedo.cs:7-11`），记 old/new parent + transforms。
4. 过滤态一致性：操作落 `_allEntities`，完成后 `ApplyFilter` 重建 `Entities`；drop 目标为自身后代时拒绝。

**验收**：A 拖为 B 子级 + Undo 恢复；拖入自身后代被拒；过滤态无幽灵节点。

#### B3 渲染模式选择器

**编辑器（S~M）+ 引擎（0~S）**：
- 工具栏（axaml L22-49 StackPanel）加 RenderMode ComboBox，绑定 WorldEditorViewModel 新属性 `EditorRenderMode`（enum：Forward / Deferred / DeferredShadowAO / DeferredShadowAOGI，**默认 DeferredShadowAOGI**）。
- **Config 下拉已存在**：axaml L28-30 绑 `Project.BuildConfigName`（`Project.cs:94`，序列化进 ProjectDocument.BuildConfig）。只改默认值：新项目模板与 `?? "DebugEditor"` fallback（axaml.cs L436/447/456）改为 "Debug"。
- 切换实现**推荐原子导出**（避免逐 pass set 的多帧中间态）：
```c
EDITOR_INTERFACE void PipelineApplyModePreset(u32 mode);  // RenderPipelineAPI.cpp
```
内部一次设 `enable_deferred_lighting` + `SetPassEnabled(Shadow/SSAO/DDGI)` + `PipelineSetLumenConfig`，走 UpdateSettings dirty 下帧重建。纯编辑器组合方案（`SetRenderPassEnabled` C# 绑定已在 L343、`PipelineSetLumenConfig` L657）作为备选。
- 映射表落 C# 静态表；状态栏显示当前模式；加分项用 `GetPipelinePassInfo`（绑定已在 L380）做激活 pass 详情弹层。Forward 档 = `SetEditorMode` 轻量前向分支（StandardRenderPipeline.cpp:341），tooltip 注明"轻量预览"。

**验收**：连续切换 20 次无崩溃/泄漏、视口即时变化；Config 影响传入 `BuildService.Start/StartApp` 的参数（axaml.cs L436/456）；模式可持久化（可选，写 ProjectDocument）。

### 6.2 第 1 批

#### A4 Prefab UI

数据层已完备：`PrefabDocument`/`PrefabSerializer`（PrefabDocument.cs L25-98，原子写 .partial+回读校验）、`PrefabInstanceComponent`（L17-27）、`AssetType.Prefab` + ".prefab" 映射（AssetImporter.cs L10-20、ContentHelper.cs L41-66）、Serializer 三处 case 已在（写 L270 / 判别 L304 / 读 L407）。

**实施（M）**：
1. **创建**：HierarchyTree 右键"Create Prefab…"——收集选中根实体的 `EntityDocument`（复用 Serializer 实体→文档路径），组 `PrefabDocument`，`PrefabSerializer.Write` 到 `<project>/Content/`；ContentWatcher（ContentModified L53）自动入库。
2. **编辑器注册**：Program.cs L418-435 加 `Register(AssetType.Prefab, ...)`；v1 = 只读面板（实体列表 + GUID + 在 Finder 显示）。
3. **实例化**：`DropAssetAsync`（WorldEditorViewModel L459）加分支 `info.Type == AssetType.Prefab`：Read → 逐 EntityDocument 创建实体（复用 `CreateEntityFromFactory` L244），根实体挂 `PrefabInstanceComponent{PrefabGuid}`，应用 Overrides；`BeginTransaction` + `AddEntityAction` 入 undo。
4. **缩略图**：ContentBrowserPreviewViewModel switch（L266 附近）加 case（静态图标起步）。
5. **Copy/Paste 升级**：`MainWindowViewModel.PasteExecute` 从"只拷 Transform"改为经 ComponentDocument 序列化往返克隆全组件；补 `PrefabInstanceComponent.GetMultiselectionComponent`（L26 现抛 NotSupportedException）。

**验收**：选中实体 → 存 .prefab → 内容浏览器出现 → 拖入场景实例化、transform 正确 → 存/读工程往返一致 → Undo/Redo 完整。

#### B1 probe 可视化 L1→L2

**桥缺失清单**（C++ 已导出、C# 无绑定）：GI gizmo 五件套 + volume CRUD（RenderPipelineAPI.cpp:976-1061 / 821-857）、`GIGetDebugParams/GISetDebugParams`（:800/:805，照 `GetRenderPipelineSettings` 的 size+byte[] 模式，NativeEngineBridge L398/L406-422）。

**L1（S~M）**：
1. 补上述绑定。
2. GI 面板（Dock 面板或 Inspector 节）：体积列表（GetCount/Get）、probe counts/spacing/rays/origin 数值编辑（`GIVolumeOverrideSet`，set 后引擎自动 dirty 下帧重建）、`freeze_probe_updates` + `ProbeVisMode`（GISetDebugParams 整 struct 写）。
3. 视口：`GIGizmoGetWireframe` 端点 → overlay 画体积线框；`GIGizmoPick` 拾取；`GIGizmoTranslate/Scale` 拖拽。
4. probe 球：引擎给 `DebugProbeSphereModule` 注册默认 probe_sphere_vs/ps（注入点 StandardRenderPipeline.h:114-115）+ 桥加 `PipelineSetProbeSphereDebug(bool)` 绑定（C++ h:201 已有）。

**L2（S~M）**：新导出（沿"consumer 自己 stage-copy off render queue"约定 LumenDDGIPass.h:190；先例 `PCGSDFReadbackManager.h`）：
```c
EDITOR_INTERFACE u32 GIProbeGridInfo(u32* out_count_x_y_z, u32* out_texels_per_probe);
EDITOR_INTERFACE u32 GIProbeReadbackBegin(u32 kind /*0 irradiance/1 depth/2 confidence*/, u32 volume_idx);
EDITOR_INTERFACE bool GIProbeReadbackTryGet(u32 token, u8* out, u32 cap, u32* out_size);
```
数据源即既有 accessor（GetIrradianceBuffer/GetDepthBuffer/GetConfidenceBuffer，LumenDDGIPass.h:172-214）。Probe Inspector 窗口：buffer → WriteableBitmap 三视图，读前置 freeze。

**验收**：线框/球随 gizmo 动；改 spacing 下帧网格变；Inspector 出图（当前 SDF miss 97% 下 irradiance 可能近空——以管线通为验收）；Metal 面板若被 probe_vis 未接线阻塞（GIGatherModule.cpp:28-30 仅 Vulkan），Metal 接线拆独立 PR。

#### B2 大气雾

**关键事实：C# 侧每帧都在发 fog 参数**——`SceneEnvironmentViewModel.ApplyToNative` 组 24-float（fog 占 p[4..10]，L79-85）调 `UpdateSceneEnvironment`（绑定 L1457-1465），C++ stub 只消费 ambient（RenderPipelineAPI.cpp:1063-1082）。UI/序列化零工作，全部在引擎消费端。

**引擎（S~M）**：
1. v1 简单雾：实现 p[4..10] 消费 → 管线级 env 状态；FinalBlit 前加解析距离/高度雾 full-screen pass（MSL 单源，沿 Froxel 运行时读源模式 FroxelFogPass.cpp:110-118/249）；`RenderPassID` 加 `FogSimple` 项（PipelineQualityConfig.h:14-32），FogEnabled → SetPassEnabled。
2. v2 大气档：模式=Volume/Froxel 时启用既有 `VolumePass`/`FroxelFogPass`（开关已存在默认 false，h:116/118），p[4..10] 映射 VolumeRuntimeParams/FroxelGridConfig。

**编辑器（S）**：
3. EnvironmentView 加"雾模式"下拉（Off/Simple/Volume/Froxel）→ `SetRenderPassEnabled`（绑定已在 L343）；模式存 SceneEnvironmentDocument 新字段（Fog 数值字段已在 SceneEnvironmentDocument.cs:20-26，无增量）。
4. **雾 shader 编辑**：不新增资产类型，沿 AssetType.Shader——`ShaderDocument.Techniques` 加 "Fog" 标记（LanguageVariants 存源路径，ShaderDocument.cs）；Save 后链路 = `ShaderCompilerNative.Compile`（封装 ShaderCompilerAPI.cpp，现仅 MaterialGraph 用）→ 引擎补"按名取 fog shader handle"导出 → 既有 `ReloadPipelineShader` 绑定（L460-471）。Vulkan 无 .comp：v1 以 MSL 为源。

**验收**：FogEnabled 即时出雾；Density/Near/Far 拖动即时；模式切换生效；改 MSL 保存视口热更新；双端对拍。

#### A10 静态场景 GI 冻结（S）

1. SceneEnvironmentDocument 加 `StaticGIFrozen(bool)`。
2. EnvironmentView 加开关"静态场景（烘焙并冻结 GI）"。
3. 开启 = `GIBakeStaticProbes`（绑定需补，C++ :864）→ `GIDebugParams.freeze_probe_updates=true`（GISetDebugParams 新绑定，与 B1 共用）；关闭解冻。面板文案注明：冻结期动态光/移动物体不更新 GI。

**验收**：开启后 GI 与收敛态一致且不再逐帧变化；`IsRenderPassActive(DDGI)`/pass 耗时验证 DDGI 三子 pass 停更。

### 6.3 第 2 批

#### A5 Array 批量布设（S~M）
HierarchyTree/工具栏 "Array Duplicate…"：轴 + count + offset（默认取 A2 步长）；循环 `CreateEntityFromFactory` + 平移，事务 + AddEntityAction 组。验收：10×1m 板阵一次生成、Undo 一次全撤。

#### A9 Lightmap UV 地基段（M）
- 引擎双 UV 通道：`ProceduralMesh.h` uv[2] → +uv2；触点：RHI 三端 vertex input layout、GBuffer 顶点着色输入、RenderMesh/RenderProxy 上传、`SceneBlobWriter.cpp:146`（Lightmap-purpose 透传为新元素）+ Reader 对应。ContentTools 零改动（uvatlas/CLI/TestUvAtlas.cpp 已齐）。
- 分两 PR：PR1 双通道 + blob 往返；PR2 CLI 端到端验证（--uvatlas → blob → 读回校验）。shader 采样/烘焙存储不做。
- 验收：blob 含两套 UV；三端渲染零回归（无 uv2 消费者，纯通道扩容）。

#### B4 P1+P2（S~M / M）
**P1**：新建 `EngineDLL/ParticleEmitterAPI.cpp`（extern "C" + EDITOR_INTERFACE，CMakeLists 加一行）：
```c
EDITOR_INTERFACE u32  EmitterCreate(void);
EDITOR_INTERFACE void EmitterDestroy(u32 id);
EDITOR_INTERFACE bool EmitterSetConfig(u32 id, const void* blob, u32 size, u32 version);  // blob=emitter_config, static_assert+版本头（沿 AssetAPI.h 惯例）
EDITOR_INTERFACE bool EmitterGetConfig(u32 id, void* out, u32 cap, u32* out_size);
EDITOR_INTERFACE bool PresetSaveToFile(u32 id, const char* path);
EDITOR_INTERFACE u32  PresetLoadFromFile(const char* path);
EDITOR_INTERFACE u32  PresetList(char* out_buffer, u32 cap);   // names\0 分隔
EDITOR_INTERFACE void EmitterSetActive(u32 id, bool active);   // + SetSpawnRate/Burst
```
C# 绑定；编辑器新建 `ParticleEmitterComponent`（presetPath + 覆盖参数）；Serializer 三处 switch 加 "particleEmitter"（L259-273/L291-308/L394-411）；`SceneRuntimeSynchronizer.SyncEntity`（L621）加组件 push 分支。**引擎前置**：emitter 运行时全量 config 替换（现仅 set_spawn_rate 等局部接口，`Components/Particle.h` 运行时 API）。
**P2**：`EditorComponentRegistry` 静态初始化（EditorComponentRegistry.cs L37-42，现有 transform/meshRenderer/light/camera）加 particle 描述符——Add Component 桩首次激活；`MSComponentBase` 包装 + `PropertiesViewModel.HasParticleComponent` + PropertiesView 分节（沿 `HasLightComponent` IsVisible 模式 L98）；参数分组按 `emitter_config`（ParticleTypes.h:126-183）。
**验收**：实体挂粒子组件 → 改 spawn_rate/blend 即时生效 → 存/读往返 → 集成测试不破。

#### A7 视图模式最小集（S）
线框/无光照两档（引擎沿 `SetSDFVisualization` h:196-197 的"便宜 per-frame bool"模式）+ RenderMode 旁 View 下拉；A1 的 AABB overlay 扩展"全部显示"开关；相机书签 = WorldEditorViewModel 5 个位姿槽（`GameProject/EditorCameraState.cs`），Ctrl+Shift+数字存 / 数字取。

### 6.4 第 3 批（开工前各自出详规）

- **A6 PCG 编辑器**：Nodify 画布复用材质编辑器（Editors/Material/）；节点面板数据源 = `PCGReflection`/`PCGNode` 注册表（Engine/Graphics/PCG/）；读写走 `PCGSerializer`；新 .pcg 资产类型（ContentHelper 扩展映射 + AssetEditorRegistry）。
- **B4 P3 曲线编辑器**：Avalonia 自绘 Canvas Control，数据模型镜像 `float_curve/color_gradient/vector_curve`（ParticleCurve.h：keyframe+in/out tangent+插值枚举），6 曲线 tab，拖点/拖切柄/右键改插值/双击加点，写回走 EmitterSetConfig 全量。**P4**：.particle.json 资产类型 + 缩略图 case + 右键新建/应用；8 个 default_presets 导出文件。

### 6.5 每批通用门禁

- 引擎批次必跑：`ctest --test-dir Darwin/Debug -R "Unit"`；`TestModularPipeline` / `TestVulkanSponzaRenderGraph` 各 60-120s 无 fault。
- 动了 EngineDLL：只增不改 + 新 POD `static_assert`；交叉构建树前 `rm Darwin/Debug/libEngine.a`（AGENTS.md 已记的坑）。
- 动了 CLI 面：`PRIMAL_UPDATE_SCHEMA=1 dotnet test --filter CliSchemaSnapshotTests` 重生成快照随 PR（测试入口 `CliSchemaSnapshotTests.cs`，快照 `Testing/CliSchema/editor-schema-snapshot.json`）。
- `dotnet test PrimalEditor_Avalonia.Tests -c DebugEditor --filter "FullyQualifiedName~Cli"`；编辑器在跑时 `primal selftest` + `scripts/primal_smoke.sh`；收口 `./scripts/ci_local.sh`。
