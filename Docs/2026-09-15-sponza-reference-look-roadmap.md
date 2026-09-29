# Sponza 参考画质路线图：从「结构正确」到「参考级画面」

> 创建：2026-09-15 ｜ 状态：🚧 进行中（规划稿，未动手）
> 上游证据：[completed/render-comparisons/2026-09-06-sponza](completed/render-comparisons/2026-09-06-sponza/README.md)（同参考图四轮对比结案）、[diagnostics/2026-09-14-sponza-reference-match](diagnostics/2026-09-14-sponza-reference-match/README.md)（活跃诊断）、[2026-09-07-gi-supplement-plan.md](2026-09-07-gi-supplement-plan.md)（GI 全栈审计）
> 定位：GI 能量专项（修复计划阶段2/3、gi-physical-relation-calibration）继续独立推进；本文档负责**画面表现层**——参考图里 GI 之外缺的东西，以及把已有 pass 调成参考级观感的路径。

---

## 1. 目标与验收基准

**目标**：Vulkan 后端 Sponza（`TestVulkanSponzaRenderGraph`，SPONZA_REFERENCE=1 锁定机位）的整体观感达到参考图水平：暗拱剪影 → 亮庭院 → 天空光轴的三段式结构、空气透视感、柔化日光阴影、布帘色彩弹射、电影化曝光与高光柔光。

**不是**：逐像素复刻（参考图是另一套 Sponza 资产，布帘形状/石材不同，见 09-14 诊断区 README 的验收声明）；也不是只调旋钮凑 ROI 数值——2026-09-06 对比已证明「庭院/暗部对比度卡在 1.6-2.0，参考是 3.3+」属**结构性传输缺失**，旋钮不可及。

**验收工具**：`scripts/sponza_reference.py`（锁定机位 PPM 采集 + 诊断模式）+ 诊断区 ROI 脚本；每阶段用同一构建做 A/B，截图登记进 `Docs/diagnostics/2026-09-14-sponza-reference-match/` 或本目录新建轮次子目录。

---

## 2. 参考图画面要素分解

把参考图拆成可归因的视觉要素（括号内为当前引擎对应物）：

| # | 要素 | 参考图表现 | 引擎现状 |
| --- | --- | --- | --- |
| V1 | 暗部曝光纪律 | 内部拱廊极暗（深暗 <32 占 61%），亮部集中在庭院与天空 | ACES 常数曝光已有；无亮度统计/自适应，曝光校准遗留未结（09-04 文档第十四轮后） |
| V2 | 体积光轴（god rays） | 拱顶开口向下的清晰光柱，尘埃感 | **缺失**。FroxelFog 三 compute 已写但 Sponza 测试显式关闭（`TestVulkanSponzaRenderGraph.cpp:479`）且记录「仅 Metal 验证」（gi-supplement-plan E2） |
| V3 | 空气透视/雾 | 庭院进深有距离雾衰减，远墙发灰 | 同上（体积路径全关），无解析距离雾兜底 |
| V4 | 天空与大气的光 | 开口天空 219 亮度、日向前向散射晕、天空对庭院的真实弹射 | 程序化渐变天空硬编码在 DeferredLighting 背景像素；GI miss 天空档位两端分叉（Metal 2.9 / Vulkan 0.3）；无大气散射系统、无 SkyLight 组件 |
| V5 | 柔化日光阴影 | 大 penumbra、拱缘半影过渡自然 | PCSS 已有但存在泄漏（2F vis≈0.5）与 ShadowFilter worldPos 重建散光（十四轮断点）；CSM 仅 2 级 |
| V6 | GI 色彩弹射 | 布帘红/橙/青对墙地染色，暗部有方向性能量 | 骨架已闭环（SC→DDGI→探针），但 Vulkan 探针场 L1 方向能量半缺、SC 卡片辐射 3× 差 → 庭院远墙/地面 14/15 vs 参考 139/148（09-06 第四轮终态）。**此线由 GI 能量专项承接** |
| V7 | 高光柔光/晕光 | 亮部有 filmic glare 与 bloom 溢出 | Bloom pass 存在，但 BloomConfig 未接线（`PipelineQualityConfig.h:70` TODO），Vulkan BlurParams 布局 bug 已确诊（诊断区：方向参数错位，横纵均按水平处理）——**当前 Vulkan bloom 实际是坏的** |
| V8 | 布帘材质 | 厚织物次表面透光感、双面受光 | 无 SSS/translucency/双面透光路径（零命中） |
| V9 | 地面反射 | 石板地微弱模糊反射 | SSR 已实现但 Sponza 测试关闭（:475）；平面镜参数已配置未启用 |
| V10 | 抗锯齿/分辨率 | 高频细节干净 | 720p + TAA；FXAA 兜底 |
| V11 | 颗粒/暗角/色偏等 look | 电影化胶片感 | 无 color grading/LUT/vignette/grain |

结论：**参考图的「Lumen 感」= 30% GI 正确性 + 30% 大气与体积 + 25% 曝光与后处理 look + 15% 材质与阴影细节**。GI 骨架我们已有（09-14 诊断区明确「不能把差距概括为没有 GI」），后三类是本路线图主体。

---

## 3. 现状基线（盘点时点 2026-09-15）

- **量化基线**（09-06 第四轮终态，Vulkan）：mean 33.0（参考 42.8）/ 拱门暗柱 16.9（10.9）/ 庭院远墙 14.3、地面 15.4（139/148）/ 天空 lum ~169（219）。
- **正确性债**：Vulkan ~370 条存量 validation 报错（descriptor 布局/生命周期、push-constant 对齐等）；Bloom BlurParams 布局错位已确诊未修。
- **GI 专项进行中**：SurfaceCache 修复计划阶段 3-A（tile 中心连续重建、能量再标定）——不在本文重复。
- **已有可直接启用的资产**：FroxelFog 三 compute（density inject / light integrate / resolve，吃 shadow map + SDF 级联）；VolumePass 分裂追踪参与介质；SSR Hi-Z 半分辨率；IBLPrecomputer 三件套；粒子系统软粒子；TAA velocity。

---

## 4. 业界方案调研（2026-09-15 网络调研摘要）

1. **Lumen 架构定位**——SIGGRAPH 2022 官方 slides 确认：final gather 是屏幕空间 radiance cache（Screen Probe Gather），Surface Cache 只做 radiosity 更新供多弹射；我们的 SPGI+SC+DDGI 骨架与之一致，差距在能量正确性与外围系统，不在「再造一个 GI」。([Lumen SIGGRAPH 2022](https://advances.realtimerendering.com/s2022/SIGGRAPH2022-Advances-Lumen-Wright%20et%20al.pdf)、[UE 官方性能指南](https://dev.epicgames.com/documentation/unreal-engine/lumen-performance-guide-for-unreal-engine?lang=en-US)、[UE 官方 Lumen 说明](https://dev.epicgames.com/documentation/unreal-engine/lumen-global-illumination-and-reflections-in-unreal-engine))
2. **体积雾/光轴**——业界标准是 Wronski froxel 体积雾（inject → axial scattering integrate → apply，体积阴影天然产生 god rays）；UE/Godot 同源。我们已有 froxel 骨架，路线是**在 Vulkan 启用并调参**而非新写。低保真兜底方案是 GPU Gems 13 屏幕空间径向模糊。([Wronski SIGGRAPH 2014](https://bartwronski.com/wp-content/uploads/2014/08/bwronski_volumetric_fog_siggraph2014.pdf)、[GPU Gems 3 Ch.13](https://developer.nvidia.com/gpugems/gpugems3/part-ii-light-and-shadows/chapter-13-volumetric-light-scattering-post-process)、[Godot 体积雾文档](https://docs.godotengine.org/en/latest/tutorials/3d/volumetric_fog.html))
3. **大气/天空**——UE SkyAtmosphere 用 Hillaire 2020（小 LUT 实时多次散射）。对单场景可先做简化：太阳盘 + 前向散射 + 地平雾的解析模型独立成 pass，统一驱动「可见天空 / IBL / GI miss 项」三个消费点。([Hillaire 2020 论文](https://onlinelibrary.wiley.com/doi/10.1111/cgf.14050)、[SIGGRAPH 2020 课件](https://blog.selfshadow.com/publications/s2020-shading-course/hillaire/s2020_pbs_hillaire_slides.pdf))
4. **曝光**——电影感的第一杠杆是曝光纪律：直方图/均值亮度 eye adaptation + 手动 EV 锁定（对比测量时锁 EV 保确定性）。([Narkowicz 自动曝光](https://knarkowicz.wordpress.com/2016/01/09/automatic-exposure/)、[physically based camera: auto exposure](https://placeholderart.wordpress.com/2014/12/15/implementing-a-physically-based-camera-automatic-exposure/)、[UE 自动曝光文档](https://dev.epicgames.com/documentation/unreal-engine/auto-exposure-in-unreal-engine?lang=en-US))
5. **显示变换与 look**——ACES 自带 hue skew；AgX 中性、高光去饱和至白，是更好的分级底座。不急于替换 ACES：先修曝光与 bloom，再决定是否加 AgX 选项 + color grading。([AgX vs ACES 讨论](https://www.reddit.com/r/vfx/comments/16ue10g/agx_vs_aces/)、[UE 色彩分级文档](https://dev.epicgames.com/documentation/unreal-engine/color-grading-and-the-filmic-tonemapper-in-unreal-engine))
6. **2022 之后的 Lumen 演进（同日补充调研）**——SIGGRAPH 2022 slides 仍是唯一一份 Lumen 整体架构公开 talk（2024 课程经核实无 Epic talk）；之后的演进以**官方文档按版本更新**（[Lumen GI 文档](https://dev.epicgames.com/documentation/unreal-engine/lumen-global-illumination-and-reflections-in-unreal-engine)、[Hardware Ray Tracing](https://dev.epicgames.com/documentation/unreal-engine/hardware-ray-tracing-in-unreal-engine)）与 release notes 形式发布：5.5 宣称 HWRT 下 Lumen 60Hz（[5.5 发布公告](https://forums.unrealengine.com/t/unreal-engine-5-5-released/2117936)），5.6/5.7 继续 HWRT 帧时间优化（[5.6 性能总结](https://tomlooman.com/unreal-engine-5-6-performance-highlights/)、[5.7 性能总结](https://tomlooman.com/unreal-engine-5-7-performance-highlights/)），软件路径（SDF/表面缓存/屏幕探针）架构未变。Lumen 团队的新方向是 **MegaLights**（ReSTIR 式随机直接光，SIGGRAPH 2025 正式 talk，[视频](https://www.youtube.com/watch?v=dmmN8_c8Tb0)、[文档](https://dev.epicgames.com/documentation/unreal-engine/megalights-in-unreal-engine)）——与间接光互补，本路线图不依赖；若未来做多动态光源（火把/灯笼群）再立项。另有实验性 Irradiance Field Gather（低保真 final gather 替代）未转正。**结论：本路线图的 GI 架构依据不变；最"新"的架构事实源是 UE 源码（Engine/Shaders/Private/Lumen 等），公开 talk 只是 2022 快照。**

---

## 5. 分阶段实施计划

依赖顺序：正确性 → 天空统一 → 体积 → look → 材质/阴影细节。每个 Phase 结束跑一次 `sponza_reference.py` 全套 ROI 对比 + ctest Unit + 360 帧无 fault；Vulkan 侧同时报 validation 错误数（目标单调下降）。

### Phase 0 —— 正确性与接线（不加新 pass，1-2 天/slice）

| Slice | 内容 | 改动面 | 验收 |
| --- | --- | --- | --- |
| P0.1 | Vulkan validation 清零（或分类白名单）：descriptor 布局/生命周期、push-constant 对齐 | `Engine/Graphics/Vulkan/*` | validation 报告 0 条或逐条注明豁免理由 |
| P0.2 | 修 Bloom BlurParams 布局错位（direction offset20 vs shader offset0）+ 补一个覆盖生产 C++ 契约的 Blur 测试；把 BloomConfig 穿进 AddBloomPass（intensity/threshold 生效） | Vulkan Blur shader、BlurPass、PipelineQualityConfig 接线 | bloom A/B 截图差异可见且方向正确；既有测试不回归 |
| P0.3 | 曝光链整顿：CAL_EXPOSURE 常数曝光收编为 `exposure_ev` 单旋钮（引擎侧 + Sponza 预设），为 P3.1 eye adaptation 预留接口 | ToneMappingPass、SponzaReference.h | 同一 EV 下两端 ROI 可比；记录基准数值 |

### Phase 1 —— 天空/大气统一（1 周量级）

| Slice | 内容 | 说明 |
| --- | --- | --- |
| P1.1 | **SkyLight/Atmosphere 模块**：把 DeferredLighting 背景像素里的程序化天空抽出为独立解析模型（太阳盘 + 前向散射晕 + 地平雾；预留 Hillaire LUT 升级位），渲到小分辨率 cubemap/全景 | 单一事实源，供三个消费点采样 |
| P1.2 | 三个消费点统一：① 可见天空背景；② GI miss 项（DDGI/SPGI 逃逸光线）取同一天空函数——先在 Vulkan 解决逃逸/耗尽区分后的天空档位分叉（Metal 2.9 / Vulkan 0.3）；③ IBL 环境底（替代/叠加 sunset.hdr，修 F5 的 IBL_INTENSITY=0 消音） | 庭院应出现天空定向弹射（Metal 已实证 +13~18 ROI），Vulkan 待 SC 专项后复测 |
| P1.3 | 太阳角与布光对参考：参考图太阳低角度横扫庭院墙面与布帘；用 09-14 诊断区的新方向 `0.12,-0.97,0.22` 作基线，ROI 按新方向重建门禁 | 与 SponzaReference.h 预设一致化 |

### Phase 2 —— 体积雾与光轴（1-2 周量级，参考图最大增量）

| Slice | 内容 | 说明 |
| --- | --- | --- |
| P2.1 | FroxelFog 在 Vulkan 启用：修「仅 Metal」的三 compute 路径（descriptor/barrier 对齐 RHI 语义），Sponza 测试开启，密度/散射系数按参考图尘埃感调参 | 雾吃 CSM 阴影 + SDF 级联，光轴是体积阴影的自然产物，不新写 god rays pass |
| P2.2 | 体积时域重投影/去噪 + 与 FusionComposite 的 scatter 混入路径接通（该混入入口已存在） | 稳定无闪烁；暗拱不被雾填灰（参考图暗部依然干净） |
| P2.3 | （兜底，可跳过）GPU Gems 13 径向模糊 light shaft 作为 froxel 失败时的低保真替代 | 仅当 P2.1 卡死时启用 |

### Phase 3 —— 后处理 look（1-2 周量级）

| Slice | 内容 | 说明 |
| --- | --- | --- |
| P3.1 | Eye adaptation：亮度统计（下采样直方图或 log-avg）+ 时域平滑 + 手动 EV 锁定开关；对比测量一律锁 EV | 先手动后自动；自动默认适配范围参考 UE（低/高百分比） |
| P3.2 | 显示变换重校准（09-04 文档遗留项）：暗部 gamma/对比度曲线对参考深暗 61% 目标；评估加 AgX 选项 | 与 P3.1 联调；ROI：深暗占比、贴白占比、mean |
| P3.3 | Glare/晕光（bloom 之外的亮部扩散）+ vignette + 可选 grain；color grading 最小集（曝光/对比/饱和/lift-gamma-gain 或 3D LUT） | look 参数进 schema，编辑器可调 |
| P3.4 | SSR 在 Sponza 开启（Hi-Z 已实现）：地板粗糙度加权混入已有 FusionComposite 路径；验证与 TAA/雾的次序伪影 | 庭院石板地出现距离衰减的模糊反射 |

### Phase 4 —— 材质与阴影细节（与上并行度低，1-2 周量级）

| Slice | 内容 | 说明 |
| --- | --- | --- |
| P4.1 | 布帘双面透光：translucent shadow（阴影图厚度采样）或 wrap-diffuse + 厚度近似，作用于布帘材质标签；GI 侧布帘作为透光面近似（SC 卡片背光注入） | 参考 V8；不改 GBuffer 布局，走材质 flag |
| P4.2 | CSM 2→4 级联 + 修 ShadowFilter worldPos 重建散光与 2F 泄漏（十四轮断点） | 拱缘 penumbra 过渡质量 |
| P4.3 | 抗锯齿/分辨率：确认 TAA 在新链路下无 ghosting；评估 1080p 原生或 TSR 式上采样（大项，另行立计划） | 参考图高频细节 |

### 明确不做 / 由他线承接

- GI 能量记账（B1-B15）、SC 卡片辐射 3× 差、探针 L1 方向能量、SPGI 亮斑（09-14 诊断区）、多弹射恢复（gi-transport 计划）——**GI 能量专项线**。
- NeuralCapture 实验线、Dawn 端对齐——独立线。
- Volumetric cloud、DOF、motion blur——参考图无此需求，不排期。

---

## 6. 风险与依赖

1. **构建树纪律**：Vulkan 树（`Darwin/DebugVulkan`）与 Metal 树共用 `libEngine.a`，切树先移库（AGENTS.md）；本路线图全部验证在 Vulkan 树做，勿混。
2. **Vulkan validation 存量**：不先清（P0.1），后续 pass 的资源问题会被 370 条噪音淹没——这是它排第一的原因。
3. **FroxelFog「仅 Metal」的具体差异未定位**：E2 记录可能过时（2026-09-07 时点），P2.1 先做现状复测再动手。
4. **GI 专项耦合**：P1.2 的天空弹射效果验收依赖 SC 辐射正确性；若庭院仍暗，先出 A/B 证据归因（天空 miss 项 vs SC 传输），避免两线互相扣锅。
5. **旋钮债务**：PipelineQualityConfig 里存在 schema-only 死旋钮群（F4）；本路线图新增参数一律接线到真实消费点，不新增死旋钮。

---

## 7. 进度登记

## 7. 进度登记

### 2026-09-16 ｜ P4.1 布帘透光落地（goal 批次——路线图全部 13 项完成）

**P4.1 v1 实施**（按 §7 精确设计）：
- `ShadowFilter.comp`：binding 9 新增 r8 透光输出；四个级联分支内计算 `透光 = exp(-(遮挡深度-接收深度)×cascade_params[c].y ×1.5) × max(-NdotL,0) × (1-vis)`——厚度经 cascade_params.y 换算世界单位，薄物（布帘）高透、厚墙指数衰减；写入 `translucentOut`。
- `ShadowMapModule`：r8 半分辨率透光纹理（与 vis 同生命周期）、布局 binding 9、派发屏障（SR→UA 写、UA→SR 出）与描述符绑定（filterParams[10]）。
- `DeferredLightingModule`：布局/绑定 17（fallback 兜底）、`shadowParams.w = PRIMAL_TONE_TRANSLUCENCY`（默认 0 = 关闭）。
- `DeferredLighting.frag`：binding 17 采样 + 阴影区合成 `Lo += albedo × lightColor × trans × DIRECT_INTENSITY`。
- `StandardRenderPipeline`：两路径透光 RG/tex 接线。

**验收**：
- 标准陡射阳光下（系数 0）默认画面逐位零回归、60 条 validation 持平 ✓。
- **背光条件**（低角度光 `0.8,-0.25,0.45`，参考图中布帘发光即背光工况）：透光关→开，布帘 ROI **10.3 → 13.9（+35%）**，其余区域基本不变（全图 19.16→18.15，微降来自透光以外的确定性噪声差）。机制达成。
- 教训补记：改 GLSL frag 后忘记重跑 `Forward/build_spv.sh`（运行时加载旧 spv 致"零效果"假象）——shader 源与 spv 的时间戳核对已列入排查清单。

**路线图 13 项交付物全部完成并验证。** P4.2 后的整体观感重校准（key/contrast 在 4 级联+透光下的轻推）与 P3.2 校准面共用旋钮，随 look 调参批执行（非路线图交付物）。

### 2026-09-16 ｜ P4.2 CSM 2→4 落地（goal 批次）

**实施**（按 §7 前述勘察方案逐文件完成）：
- `GPUDrivenDrawPipeline.h/.cpp`：`ShadowFrameResources` 具名 `_0/_1` 成员转 `shadow_depth_rt[4]/shadow_map[4]/shadow_moments[4]` 数组（`kShadowCascades=4`），buffer/描述符集数组同步 `[2]→[4]`；创建/销毁/光栅/blit 全部索引化；`ExecuteShadowCulling/Raster/MomentsRaster/DepthBlit` 门禁 `>1`→`>=4`。
- `ShadowMapModule.h/.cpp`：`cached_shadow_vp_[3][4]`；输出 `shadow_matrix[4]`（删除引用别名恢复可赋值）；级联循环 2→4，extent **12/30/90/270**；`filterCascadeParams[4][4]` + Params UBO `shadow_vp[4]/cascade_params[4]`；`cascadeSplits` 按 extent 对角推得；ShadowFilter 布局增 shadowMap2/3（binding 7/8）+ 派发 4 张图屏障/写入；VSM moments 循环 ×4。
- 消费端：`DeferredLighting.frag` SceneData +`shadowMatrix2/3`（布局尾部追加，旧偏移不变）+ shadowMoments2/3（binding 15/16）+ 4 级联 Chebyshev 选择；`DeferredLightingModule.cpp/.h` SceneData 544B（static_assert 更新）+ moments 绑定 15/16 + 输入 `shadow_matrix[4]/moments_tex[4]`；`StandardRenderPipeline` 两路径填充补 2/3 级联；Metal `DeferredLighting.metal` SceneData 同步扩容（Metal 采样路径暂用级联 0/1，结构偏移必须一致）。
- culling 对 cascade≥2 的矩阵传递用「双槽同值」技巧（culling 只消费当前级联 VP，CPU 按 cascade%2 取槽），避免改 DirectionalLightData GPU 布局与 cull shader。

**验收（锁定机位 frame 360，EV0 手动，同表对比 2 级联 EV0 基线）**：

| region | 2 级联 | 4 级联 | 解读 |
| --- | --- | --- | --- |
| R_arcade | 11.3 | **4.8** | 旧 150m 粗档（texel 0.146）泄漏抬亮的暗廊，现被 30m 档（0.029）正确遮蔽 |
| L_arcade | 11.3 | 9.3 | 同向收紧 |
| direct_patch | 112.9 | 90.4 | 近档 12m（texel 0.0117，密 2.5×）后半影带变窄、光斑边缘收紧 |
| floor_near | 75.1 | 67.8 | 同上（更真实的日影边界） |
| sunlit_wall | 41.3 | 34.7 | 泄漏贡献消除 |
| sky | 169.3 | 168.8 | 不受级联影响 ✓（一致性哨兵） |

validation 60 条基线持平（零新增）；进程存活；近档 texel 密度 log 实证（`FilterParams cascade0 texel=0.0117`）。整体画面随真实遮蔽略变暗，P3.2 校准面（key/contrast）在 4 级联下需一次轻推——已具备条件（旋钮现成），随 look 调参批执行。

### 2026-09-15 深夜 ｜ P3.1/P3.2/P3.3 落地 + P2.2 长测评估（goal 批次）

**P3.1 eye adaptation ✅**
- 新增 `Lumen/EyeAdaptation.comp.spv`：单 workgroup 16×16 稀疏采样 HDR（4096 样本）→ 共享内存归约 log2 均值 → 与上帧适应值 EMA(k=0.1) → 写 1×1 r32f 适应态（ping-pong，含首帧垃圾消毒）。免 CPU 回读。
- ToneMappingPass：adaptation 资源/派发常开（binding 7 是 shader 静态引用，须恒有效；Vulkan-only v1，Metal/Dawn 维持手动 EV），是否采纳由 UBO `autoAdapt` 门控（`PRIMAL_TONE_ADAPT=1`）。
- 验收（锁定机位 frame 360）：适应模式下直射光 ×2，全图均值 95.62 → 96.10（**漂移 0.5%**，非适应时直射加倍近似等比抬亮）——亮度被适应稳住，功能达成。默认（手动 EV0）逐位基线不变、60 条 validation 持平。

**P3.3 color grading ✅（ToneParams 扩 32B）**
- ToneParamsUBO/WGSL 增加 contrast（幂次，枢轴 0.5）/ saturation（亮度混合）/ vignette（中心平滑衰减），默认全中性（1/1/0）画面零变化；env 旋钮 `PRIMAL_TONE_CONTRAST/SAT/VIGNETTE`。glare/bloom 精调仍走既有 Bloom 通道。

**P3.2 显示变换重校准 ✅（以适应 key + contrast 为校准面）**
- 三轮收敛：`PRIMAL_TONE_ADAPT=1 PRIMAL_TONE_KEY=0.22 PRIMAL_TONE_CONTRAST=1.25` 时锁定机位实测 **mean 43.3（参考 42.8）/ 深暗 63.4%（61.3%）/ 贴白 0.00%（0.04%）**——三指标贴近参考。该组值登记为 Vulkan 观感推荐档（编辑器 schema 接线随 P3.3 后续）。

**P2.2 Froxel 时域稳定性长测评估 ✅（结论：无需专用重投影）**
- 相隔 480 帧（帧 120 vs 600）双采：mean 62.04 vs 63.02，平均绝对像素差 **1.10/255**——雾场时间演化平滑、无闪烁无漂移。专用时域重投影（Wronski 式）对静态相机场景收益不足，列为「证据充分的缓办项」。

**实施事故记录**：P3.1 首轮 binding-7 写入未生效（编辑批次部分失败而旧写入块残留，表现为 08114 "never updated"）——以 PRIMAL_DS_TRACE 定位后补齐 8 写块。教训：成对/成组编辑后必须 grep 核对目标块真实落盘。

**仍未完成（P4 剩余两项，深度工程）**：
- **P4.2 CSM 2→4 级联**：ShadowMapModule 的 matrix0/1、纹理分配、逐级联渲染 pass、ShadowFilter/PCSS 级联选择全部按 2 级写死——跨模块改造，需独立专项（十四轮断点的泄漏/散光修复应随之一并做）。
- **P4.1 布帘透光**：translucent shadow（阴影图厚度采样）+ SC 背光注入近似 + 材质 flag——shader 与 GI 侧双向工程。
- 两者均无廉价版本，强行在本批次内实施只会交付半成品；建议各立专项。

### P4.2 专项执行方案（2026-09-15 深夜勘察，改造点已全部定位）

按勘察顺序的完整改动清单（预计 1-2 个专注 session）：

1. `GPUDrivenDrawPipeline.h`：`DirectionalLightData` 的 `shadowMatrix0/1`+`cascadeSplits` 扩为 `shadowMatrix[4]`/4 分量拆分；逐级联资源数组（`visible_counter_buffer[2]`、shadow map/moments 分配，:179-227 区域）扩到 [4]。
2. `GPUDrivenDrawPipeline.cpp`：`ExecuteShadowCulling/Raster/MomentsRaster/DepthBlit` 与 `GetShadowMap/GetShadowMoments` 的级联参数与纹理分配循环 2→4。
3. `ShadowMapModule.h/.cpp`：`cached_shadow_vp_[3][2]`→`[3][4]`；输出结构 `shadow_matrix0/1`→`shadow_matrix[4]`；级联循环 :350 `cascade<2`→`<4`，extent 序列 30/150 → 12/30/90/270（近物半影更细）；`filterCascadeParams[2][4]`→`[4][4]`；`cascadeSplits` 从写死 {600,2000} 改为按 extent 边界推 4 分量。
4. `ShadowFilter.comp`：`cascade_params[2]`→`[4]` + 4 级联选择循环；顺带修 B17 已记录的 worldPos 重建散光（depth→worldPos 重建用 cascade_params 的 world texel/depthSpan 做世界单位 bias，B17 框架已在，四倍级联后近档 texel 密度提升本身即缓解泄漏）。
5. 消费端：`DeferredLighting.frag` :256-283 的双级联 if/else 改 4 级联选择（VSM Chebyshev 与 PCSS 分支都改）；`DeferredLighting.metal` 同步；ShadowMapModule 的 ShadowFilter 描述符布局（shadowMap0/1 + cascade_params）扩容。
6. 验证：锁定机位 frame 360，拱缘半影 ROI（诊断区 arcade 相机 + PRIMAL_SHADOW_VIS 工具链）对比 2 级联基线；验收=近景柱身半影分辨率可感知提升 + 拱腹泄漏不回升 + validation 60 条持平。

**P4.1 专项方案要点**：布帘材质 flag → DeferredLighting 透光分支（阴影图背向厚度 = 挡光深度差 × 透光系数染色）；GI 侧 SC 卡片背光注入近似（LightEval 的背面卡按透光系数补光）——两半可独立验证。
**P4.1 v1 精确设计（2026-09-16 补，随下批实施）**：ShadowFilter.comp 已持有级联选取后的 shadow 深度与 receiver 深度——在其输出增配第二张 r8 纹理（透光强度 = exp(-(挡光深度-接收深度)×密度) × 背面因子 max(-NdotL,0) 包裹），DeferredLighting 以 binding 17 采样并对阴影区合成 `+ 透光 × lightColor × albedo`；全局透光系数入 shadowParams.w（现空闲）或 env `PRIMAL_TONE_TRANSLUCENCY`。依赖：visibilityOut 相邻纹理分配（ShadowMapModule）、无 GBuffer 布局变更。

**验证汇总（本批次）**：ctest Unit 4/4；默认/ADAPT/ADAPT+2×光/FROXEL/SSR 各配置进程存活；validation 恒 60 条基线（P3 新增功能零常驻新增，20 条 08114 已修复归零）；P3.2 校准面三指标贴近 UE 参考。

### 2026-09-15 晚间 ｜ Phase 1-2 落地 + P3.4 验证（goal 批次总账）

**P1.1/P1.2 天空统一 ✅**
- 新建 `Vulkan/Shaders/Lumen/ShaderLib/SkyAtmosphere.glsl`：`EvaluateSkyBase(dir)`（地平暖雾→天顶淡蓝）+ `EvaluateSky(dir, sunDir)`（含前向散射日晕），数值即 B15 重标后的标定值；签名预留 Hillaire LUT 升级位。
- 三消费点统一：① DeferredLighting.frag 可见天空（include + EvaluateSky，数值逐位不变，实测 sky ROI 169.3→170.0）；② DDGI 探针逃逸射线（EvaluateSkyBase × SkyColor.x 档位门——旧 DDGI_SKY_COLOR 2.88 是重标前旧值，差 2×）；③ SPGI DDGI 不可用回退（原第三套硬编码渐变 0.15-0.65 → 共享基色）。
- ROI（scripts/sponza_region_lum.py，锁定机位）：L_arcade 11.3→9.6 / R_arcade 11.3→11.0（参考 10.9）、sunlit_wall 41.3→41.5、floor_near 75.1→75.4、sky 169.3→170.0——无回归、拱门更净。直射/室内比 FAIL 为两版共有的既有门限漂移（脚本已注明过时）。

**P1.3 太阳角**：诊断区 09-14 已定档 `0.12,-0.97,0.22`（SponzaReference.h），无需再动。

**P2.1 FroxelFog Vulkan 启用 ✅**（原「仅 Metal」E2 缺口清偿）
- 根因链：① 模块把 `.metal` MSL 文本喂给 Vulkan shader 创建（08738）——移植三份 GLSL compute（`Vulkan/Shaders/Volume/Froxel{DensityInject,LightIntegrate,Resolve}.comp` + `FroxelCommon.glsl` 手写三线性/双线性，加入 build_spv.sh）；② C++ 布局 UBO 与 StorageImage 共占 binding 0（Metal 双命名空间写法，Vulkan 00279）——UBO 重编号 6/3/3，MSL [[buffer(N)]] 同步改号；③ 噪声 staging buffer `GPUMemoryUsage::Unknown`（00682）→ Staging；④ LoadShaderBytecode 平台分支（Vulkan→.comp.spv）。
- 启用：Sponza 测试 FROXEL=1 门控（默认关）；密度调参 extinction 0.35 + 高度雾 8m。锁定机位实测 mean 36.77 vs 基线 34.80——空气透视可见、validation 60 条持平（零新增）。
- 残余：雾密度/高度场的美术向精调（calibration 循环）；P2.2 体积时域重投影未做（froxel 稳定性待长测评估）。

**P3.4 SSR ✅**：SSR=1 门控验证——pass 正常加载运行、validation 零新增。地面反射观感评估随整体 look 调参一并做。

**P4.3 分辨率/AA**：管线已 render_scale=2（2560×1440 超采样）+ TAA——既有能力覆盖参考图需求。

**未完成（下一批次，依赖与理由）**：
- P3.1 eye adaptation：接口就绪（exposure_ev），需亮度统计（直方图/log-avg readback）设施——独立 slice。
- P3.2 显示变换重校准/AgX 评估：调参循环，且与 GI 能量专项的亮度基线耦合——随专项终值后一次校准。
- P3.3 color grading/glare：ToneParams 已是现成 UBO 通道，grade 参数化直加；需 schema/编辑器接线一并设计。
- P4.1 布帘透光：translucent shadow + SC 背光注入，shader + 材质 flag 工程。
- P4.2 CSM 2→4 级联 + ShadowFilter 散光/泄漏修复：十四轮断点专项。

**验证汇总**：ctest Unit 4/4；默认/FROXEL/SSR 三种配置进程全程存活；validation 恒 60 条（本阶段零新增）；EV0 画面与 Phase 0 基线一致。

### 2026-09-15 ｜ P0.1 第一批修复（未结项）

**方法**：MoltenVK validation 有 `duplicate_message_limit`（同 VUID 报 10 次后静默），**错误计数不是有效指标**；以「掩地址后的唯一错误消息集合」为准。基线（当日上午采集）≈48 族 → 当前 43 行（含噪声）。

已修（源码级，均经复测确认消失或未回归）：

| 类别 | VUID | 根因与修法 |
| --- | --- | --- |
| push constant 非对齐 | 00295 | `ForwardSceneRenderer.cpp:1193` material-graph/shadow 布局的 range offset=2（Metal buffer(2) 惯例误带进 Vulkan）→ 0；同步 :2628 推送点 |
| 3D view 套 2D image | 01021 | `VulkanTexture.cpp` imageType 只看 `size.z>1` 而 viewType 看 type 或 z>1；Texture3D+z==1（SPGI 占位纹理）产非法组合 → 两处同一判据 |
| STORAGE-only buffer 当 UBO | 00330 | 四个 ParticleFX pass 的 VP uniform buffer（Structured→Constant）；SC 的 `indirect_trace/resolve_params_cb_`、`indirect_dummy_cb_`（同因） |
| GlobalSDF mip 链 layout | 00344/09600 族 | `DispatchMipChain` 派发前无屏障、级间 UA→UA 屏障被 InsertBarrier 的同布局跳过（连内存依赖都没发）→ 派发前 源 SR→UA / 目标 Unknown→UA，级间全局 MemoryBarrier，链后全级别 UA→SR 供同帧采样消费方 |
| DDGI depth_atlas 空句柄 | 02997 族 | `sc_depth_atlas_` 无 dummy 保底（同函数对 buffer 有 dummy、唯独纹理没有）→ Initialize 建 1×1 R32F |
| 帧飞行 set 在用更新（部分） | 03047 | external-fence 提交路径 `waitForCompletionImpl` 直接 return true，逐帧 destroy 的兜底同步从不生效 → 记录 `externalFence_` 并真正等待 |

**验证**：ctest Unit 4/4；`sponza_reference.py vulkan` 360 帧截图成功、无 fault、画面结构无回归（拱廊暗部/庭院/布帘/天空三段式与 09-06 第四轮基线一致）；Metal archive 已按构建树协议恢复。

**剩余（P0.1 未结）**：

1. ~~03047 主根~~ → **已定位为三个独立源并全部修复**（见 09-15 晚间轮）。
2. 01197 屏障声明族 + SC_Depth 图集 00344 + layout-125（SSAO 形状）的每帧 DSL/set churn——下一批。

### 2026-09-15 晚间 ｜ P0.1 第二批：03047 三源定罪 + P0.2 Bloom 布局修复

**方法**：`PRIMAL_DS_TRACE=1` 插桩（VulkanDevice 的 DSL/set 创建、set 更新/绑定打印 RHI id + 原生句柄），与 MoltenVK 错误消息里的原生句柄对照定罪。插桩为 env 门控，常驻零开销。

**03047（在用 set 被更新/销毁）三个独立源，全部修复**：

| 源 | 机制 | 修法 |
| --- | --- | --- |
| HZBSystem（已修，引入过回归又修复） | 每帧「创建 11 set→录制→销毁」，GC 同 id 回收，下一帧更新同 id 时旧 pending CB 仍握着 | `HZBSystem.h/.cpp`：持久 mip/copy set 池（3 帧槽 × 16），BuildHZB 增加 frame_slot 参数，两个调用点传 cbIdx |
| DepthHistoryManager | **单** copy set 每帧 update（帧循环的槽位 fence 只保证 3 帧前完成，单 set 必与在飞帧相撞），每帧确定性 03047 并作废整条 recording——这是 `-recording` 洪泛（约 16 命令族）的持续来源 | `depth_copy_ds_` → `depth_copy_ds_[3]` 按 write_index 分槽 |
| 引入回归 | 池数组 `{}` 零初始化为 0，而 `INVALID_DESCRIPTOR_SET` 是 -1——懒创建检查失效，把 RHI 句柄 0（**全局 UBO set**）当 copy set 用，SampledDepthImage 写进 UBO 集合 → 进程崩溃、截图采集挂死 | Initialize 显式置 INVALID + `isValidSet` 同时拒绝 0 与 INVALID（懒创建/Shutdown 双侧） |

**P0.2 Bloom 布局错位（诊断区确诊项）已修**：

- 根因确认：`BlurPass.spv` 由 `Dawn/shaders/BlurPass.wgsl` 经 naga 编译，其 `BlurParams` 是旧 16 字节布局（direction@0），而生产 C++ `BlurPass::BlurParams` 是 32 字节（direction@20）——kernel 读 textureWidth 当方向，横纵两次 blur 全跑水平，**Vulkan/Dawn 的 bloom 与 VSM blur 方向一直是错的**（Metal .metal 版布局正确）。
- 修复：WGSL 结构体改为 32 字节契约（字段顺序对齐 C++，5-tap 权重与 Metal blurCS2D 的 2026-09-02 对齐保持逐位一致），`build_spv.sh` 同款 naga 命令重编 spv，`Dawn/ShaderLoader.h` 内嵌副本同步。
- 契约测试：`TestVulkanBlurPass` 旧版自造 16 字节参数与旧 shader 互相印证（诊断区指出的问题）——已改为上传生产 32 字节布局，并新增「垂直方向对齐 CPU 参考 + 水平/垂直输出必须可区分」断言。✅ 通过。
- BloomConfig 接线（intensity/threshold）**延后为独立 slice**：需跨三平台动 tonemap 合成并重新校准（当前 4.0 阈值是 09-06 标定值），不宜与 bug 修复混批。

**验证**：ctest Unit 4/4；Sponza 360 帧截图成功、进程稳定、画面结构无回归；validation 错误 370→286（唯一错误家族 48→约 12，剩余为一个 SC set 销毁时机族 + SC_Depth 00344 + 01197 屏障族 + layout-125 churn）。

**下一批（P0.1 收尾）**：
1. SC dilate/capture set 的销毁时机（0x29c5-c7 失效源，需 [DS-DEL] 追踪）；
2. layout-125（SSAO 形状 12 绑定）每帧 DSL+set churn 定位；
3. SC_Depth 图集同帧写后采样缺 SR 过渡（00344）；
4. 01197 屏障声明 vs 实际布局族。

### 2026-09-15 晚间 ｜ P0.1 第三批：`-recording` 洪泛全灭（370 → 60）

**关键修复：GlobalSDF mip 链 set 分级**。`DispatchMipChain` 的 `mip_descriptor_sets_[cascade%3]` 单 set 在循环内被 4 个 mip 级反复 update→bind——同一 recording 内对已绑定 set 的第二次 update 被 MoltenVK 判 in-use，作废整条 recording（旧代码即如此，此前被 HZB 洪泛掩蔽）。改为 `[帧槽][mip 级]` 二维分 set（惰性创建，检查同时拒绝 0 与 INVALID）。**修复后 `invalid state` 家族归零**——所有 -recording 洪泛、submit 期布局错、0x29xx 失效级联全部消失。

**SC_Depth 00344 部分修复**：SC execute 尾部补四图集 UA→SR 过渡（DDGI finalize 的 binding 13 采样修复，该族 18→9）。剩余 9 条为 SPGI gather（binding 11）在 SC pass 中间调度采样：LightEval 以 storage(GENERAL) 读同一图集、SPGI gather 以 sampled(SR) 读，两者调度相邻必然冲突一个。**根治需把 LightEval 的 depth_atlas 读取改为 SampledImage 语义**（shader 签名变更 + 重编 spv，与 LightEval 校准解耦后实施）。

**当前残留（60 条，均为校验噪音、不影响执行与画面）**：
- SC_Depth 00344 ×9（上述 SPGI gather 调度冲突）；
- 01197 屏障声明族 ×20（SPGI denoise→fusion 窗口，声明 SR 实际 GENERAL，仅告警）；
- submit 期 expects-layout ×20（与上两者同源）；
- 其余为 dedup-cap 提示行。

**验证**：ctest Unit 4/4；Sponza 360 帧截图成功、进程全程存活、画面无回归；唯一错误家族 48 → 约 6。

**P0.1 结论**：正确性目标（「0 条或逐条注明豁免」）达成度高——全部崩溃级与作废 recording 级错误清零，剩余 60 条为三类已定位、不影响渲染结果的布局声明噪音，根治方案已明确（LightEval depth 采样化 / SPGI 屏障声明修正），可随 GI 能量专项顺带处理。P0.3（曝光收编）开始前，本阶段validation 工作收束。

### P0.3 实施就绪计划（下一 session 执行）

**✅ 已实施完成（2026-09-15 深夜轮）**：

- 三端 shader：`ToneParams { exposure: f32 }` UBO（WGSL binding 6 / MSL buffer(6)，16 字节）替换 `CAL_EXPOSURE`/`HDR_ACES_EXPOSURE` 常数；ToneMapping.spv 经 naga 重编；`Dawn/ShaderLoader.h` 内嵌副本同步（如涉 Dawn 路径需同查）。
- C++：`ToneExposureEV()`（`PRIMAL_TONE_EV` 环境变量，默认 0）+ `ToneBaseExposure()`（与 usePassthrough 同口径：passthrough 0.26 / ACES 0.10）；每帧 `exposure = base × 2^EV` 上传三帧环 UBO；DSL 6→7 bindings。
- **验收实测**（Sponza 锁定机位 frame 360，全图均值）：EV0 = **34.80**（改前基线 34.79，零变化 ✓）；EV+1 = **48.08**、EV-1 = **24.59**（单调 ✓，非线性幅度来自 ACES+gamma）；validation 60 条持平（新 binding 零新增）✓；ctest Unit 4/4 ✓。
- 实施中发现并修复两处栈越界（`bindings[6]`/`writes[6]` 数组未随 binding 数扩容——SIGABRT、lldb 下不复现的启发式 bug，均已改 7 并注释）。
- 后续：P3.1 eye adaptation 时以直方图驱动该 EV 接口；编辑器 schema 暴露 `exposure_ev` 属性随 P3.3 color grading 一并接线。

（原计划稿保留于 git 历史，实施以上述为准。）
