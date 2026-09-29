# [GI] 能量流深度调查——用户假设筛查、辐射计量单位全链审计与能量不对称发现

> 标签：**[GI] [SC] [DDGI] [WRC] [SPGI]** ｜ 日期：2026-09-20 ｜ 状态：🚧 诊断分析（修复计划另立：
> `Docs/2026-09-20-gi-energy-asymmetry-fix-plan.md`）
> 触发：用户基于中庭对比截图提出的五条根因假设（SC coverage 缺口 / DDGI·WRC 近场泄漏 /
> 中庭反弹饥饿 / tone mapping 放大 / 辐射计量单位不统一），要求按文档深度筛查并给出实验与修复方案。
> 关联：`Docs/2026-09-20-sponza-vs-lumen-visual-gap-analysis.md`（下称 [GAP]）、
> `Docs/2026-09-20-sc-capture-near-plane-mechanism.md`（下称 [NEAR]）、
> `Docs/2026-09-20-sc-correctness-and-reflections-integrated-plan.md`（下称 [R-线]）、
> `Docs/2026-09-19-world-space-radiance-cache-design-and-plan.md`（下称 [WRC-设计]）、
> `Docs/2026-09-20-pass-debug-view.md`（下称 [PDV]）

---

## 0. TL;DR

五条假设：**三条方向正确、一条前提过时、一条（辐射计量单位）经全链逐 shader 审计排除**。
审计同时发现**三处能量不对称**——它们不是量纲错误，但精确解释了截图里"间接光二值化
（要么被 probe/sky 照亮、要么接近黑）"的观感：

| # | 发现 | 性质 | 观感对应 |
|---|---|---|---|
| A1 | **SC tile 反弹路径的天空被硬编码在 0.4 档时代（≈1.1），不吃 `sky_color_intensity`（探针路径现 2.0 档 ≈9–12）** | 陈旧校准（`SurfaceCacheIndirectResolve.comp:243`） | 中庭"天空→地面→柱子"二次反弹暗 ~8×（假设③的具体机制） |
| A2 | **SC 查询失败→黑**（"不伪造能量"契约）**vs 天空回退→9–12 高亮** | 设计语义副作用 | 间接光二值化的直接代码形态 |
| A3 | **亮度惩罚不对称**：HZB/prev-frame 命中带 `1/(1+max(lum-1)×2)`，SC atlas 命中默认不带（diag mode 3 可 A/B） | 校准不一致 | 近处直射能量经 SC 命中更"生"地灌入探针 |

**用户假设定性**：①SC coverage 缺口——方向对但"393 全 AABB fallback"前提已过时（资产
已于 9-20 重烘，CDB2 389/393 在役）；②DDGI/WRC 近场泄漏——部分成立但机制修正（High 档
DDGI 直采通道已切，均匀感来自 SPGI 自身滤波 + A2 天空回退）；③中庭反弹饥饿——证实
（P90/P10 2.1×、corr(indirect,albedo)=0.888），A1 是新增具体嫌疑；④tone mapping 非根因
——一致；⑤单位不统一——**排除**（见 §4，无双乘/无 π 失配/无 L-E 混用）。

修复优先级与实验方案见姊妹文档《能量不对称修复计划》。本文档归档筛查证据与审计全文。

---

## 1. 调查方法

1. 三路代码探查（渲染流程 / SC 系统 / 双探针+可视化钩子），全结论带 文件:行号；
2. **辐射计量单位全链审计**：对 SC→DDGI、SC→WRC→SPGI、SC→SPGI hit、SSGI、SCDDGI 注入
   五条支路逐 shader 核对存储量纲（E / L_o / L_i）、albedo 乘法位置、π 因子成对性；
3. **隔离实验可控性审计**：用户提议的 5 步实验逐一映射到现有旋钮，核实每个开关的语义
   安全性（关掉后下游吃过期数据还是干净短路）；
4. 测试资产字节级核查（CDB2 魔数计数）。

---

## 2. 架构事实基线（筛查依据，详版结论带行号）

### 2.1 渲染流程（Vulkan RG 路径，`StandardRenderPipeline::BuildRenderGraph`）

| 步骤 | Pass | 说明 |
|---|---|---|
| 1 | NaniteSceneRender + GlobalSDF | GBuffer（albedo/normal/ORM/depth/velocity）+ 级联 SDF |
| 2-4 | Shadow → SSAO → DeferredLighting | 直射 HDR |
| 5 | `LumenSurfaceCache` | 卡片辐射缓存（capture→fill→shadow→LightEval→tile radiosity） |
| 6 | `LumenDDGI` | 近场探针（radiance-SH 三缓冲），探针插值 Medium 档直采进像素 |
| 6.7 | `LumenWRC` | 世界空间辐射探针（Vulkan-only），**只被 SPGI 远场消费** |
| 7 | `SC_DDGI_Integration` | 卡片 L_o 直投影进 DDGI 探针（coldstart 模式） |
| 8 | SSGI（High 档被抑制）/ SSR | 屏域 |
| 9 | `ScreenProbeGI` → `DDGIGIGather` | 屏幕探针（High/Ultra 唯一像素入口）/ DDGI gather |
| 10 | `FusionIndirect` + `FusionComposite` | `indirect = albedo × gi × gain × (1/π)`，无 ambient floor |

- **能量单向律**：SC 只出不进（SC 读 WRC 明令禁止）；WRC 只被 SPGI 远场消费（[WRC-设计] §0.3）。
- **Fusion 档位**：mode 0 per-pixel（`PRIMAL_FUSION_PERPIXEL`，`mix(ddgi,spgi,spgi_w)`）/
  mode 1 High+（只读 SPGI）/ mode 2 Medium（只读 DDGI）。`spgi_w = clamp(spgi4.a,0,0.9)`，
  **`spgi4.a` 是 confidence**（贯穿 temporal/denoise，`ScreenProbeGather.comp:231-233`），
  仅 mode 0 消费——用户 High 档下 confidence 切换分段不适用。
- DDGI 与 WRC **不是重复**：DDGI=独立 solver（Medium 正解，冻结保留）；WRC=纯 cache
  （High 档 SPGI 远场辐射源），六维硬隔离（[WRC-设计] §0-1）。

### 2.2 近场间接光的归属（High/Ultra）

**追迹层 = SPGI 屏幕探针**（HZB march 0–25m），**内容层 = SC lighting atlas**（命中点卡片
查询）。射线归属四分支（`ScreenProbeTraceFinalize.comp` 归因桶）：0=HZB hit / 1=SC card /
2=far-cache / 3=fallback·miss；远场 fallback 顺序 **WRC 优先 → RadianceCascade → DDGI → 天空**
（`:391-395,403,433,482`）。DDGI 在近场的角色只有：SC LightEval 的 E_probe 反弹馈送、
SPGI miss 兜底、SC_DDGI 卡片注入。

### 2.3 SC 系统要点

- **Atlas**：albedo RGBA8 / normal RG16F 八面体 / depth R32F 轴向深度（=dot(world−center,fwd)，
  与 near 解耦 [NEAR] §2）/ emissive RGBA16F / **lighting_atlas[3]** RGBA16F（出射辐射）/
  indirect_tile·out[3]。
- **LightEval 输出**：`L_o = ρ/π·(E_direct + E_probe) + tile 项 + emissive`，clamp 64
  （`SurfaceCacheLightEval.comp:571-579`）。
- **谁有 SC**：带 `cluster::component_cache` 的 Nanite mesh 实例（唯一注册循环
  `StandardRenderPipeline.cpp:1546-1706`）；三选一 baked CDB2 → AABB 6 向 fallback → 包围球。
  Terrain/粒子/蒙皮**无 SC**。**测试资产已重烘**：`EngineTest/assets/Sponza.model` 含 378 个
  CDB2 段（389/393 baked，coverage 均值 0.891、2532 卡；旧版 `.pre_cdbk`）——"393 全 fallback"
  说法作废；残余 4 个 4 顶点退化面片。
- **更新**：capture 每帧 seed 32/稳态 8 张（Top-K 或轮转）；LightEval 页预算 256 页/帧；
  tile radiosity 32 射线/tile、步幅 2、暖启动 90 帧后启动——每帧一跳时域反馈累积。

---

## 3. 用户假设逐条筛查

### H1 "SC coverage 有明显缺口（393 全 AABB fallback）" —— 方向对，前提过时

- 资产核查（字节级）：`EngineTest/assets/Sponza.model`（7,491,644B，9-20 更新）含 **378 个
  CDB2 魔数**；`.pre_cdbk` 备份为 0。重烘已在测试加载路径上生效。
- 但缺口残余为真：4 个退化面片 fallback；WRC 漏斗 **boundsFail 50.6%**（[GAP] §2.1 状态更新，
  归因未卡化几何/掠射带）；[R-线] R0-R5 已落 own-surface 裁决/qpad A/B/漏斗自洽，
  **near=0 投影退化假设已被 [NEAR] §1 证伪**（真机制=掠射薄层内容污染，非数学故障；
  `PRIMAL_SC_CAPTURE_NEAR` 已参数化）。
- 定位手段现状：boundsFail 只有全局计数 + 8³ 世界分箱（`PRIMAL_WRC_BOUNDS_DUMP`，
  `LumenWRCPass.cpp:946-991`），**无逐卡/逐实例计数**。

### H2 "DDGI/WRC 近场泄漏过量低频 GI" —— 部分成立，机制修正

- High 档 DDGI 直采通道已切（mode 1），近场均匀感不来自 DDGI gather 铺场。
- 真嫌疑：① SPGI 自身 gather（4×4 tent）+ temporal（α=0.1 重投影累积）+ spatial filter 铺平；
  ② **A2 天空回退亮度 9–12**（EvaluateSkyBase 4.2–6.0 × 2.0 档）远高于 SC 命中暗侧——miss/
  fallback 占比高的区域被天空整体抬亮；③ DDGI 对近场的隐性影响经 SC E_probe 间接进入。
- 用户提议的"冻结探针+移相机"实验可行但有两个语义陷阱（见 §6）。

### H3 "中庭 SC→probe→SPGI 链路能量不足" —— 证实，新增具体机制 A1

在案量化：GI 入射场 P90/P10≈2.1×、corr(indirect,albedo)=0.888（[GAP] §2.4——反弹场没有
空间结构）。本审计新增：**A1（tile 天空档陈旧）直接压制"天空→地面→柱子"的反弹源**，
因为该路径的天空能量不随 9-20 梯度专项的 2.0 档上调。

### H4 "Tone mapping 放大但非根因" —— 一致

adapt 默认开 + key 0.55 + contrast 1.35（[GAP] P3 已落地）。显示链是放大器不是根因。

### H5 "Radiometric 单位不统一（albedo 双乘 / π 失配）" —— **排除**（审计见 §4）

五条支路全部量纲自洽。但审计的三处不对称（A1-A3）恰好产生用户描述的
"近处 probe GI 亮、SC 命中暗"的量级差——机制不同，观感相同。

---

## 4. 辐射计量单位全链审计（H5 排除依据）

### 4.1 各站存储量纲

| 站 | 存储/输出量 | 关键表达式 | 位置 |
|---|---|---|---|
| SC LightEval 写 atlas | **L_o**（出射辐射） | `(direct+E_indirect)·ρ/π + tile + emissive`，clamp 64 | `SurfaceCacheLightEval.comp:571-579` |
| ├ E_probe 读 DDGI | radiance-SH → E | `evalProbeIrradianceSH9`(E/π) **×π**（L389/L277，注释"两腿 π 一致"）；×`probe_feed_bootstrap`（L530） | 同文件 |
| SC tile（recon 模式） | 入射 cos 加权均值 ≈E_tile/π，**无 albedo** | `indirect*albedo` 只在 Reconstruct 出口一次 | `IndirectResolve.comp:269-291`、`IndirectReconstruct.comp:127` |
| SC tile（legacy） | 出射 | `current = indirect*albedo`（L339） | 同上 |
| DDGI 探针 | **radiance-SH**（缓冲名 `irradiance_*` 是历史误导） | `shAccum += radiance*basis*mcWeight`（4π/N，无 cos 无 π） | `DDGIUpdateIrradiance.comp:222-227` |
| DDGIGIGather 输出 | **E**（不乘 albedo） | `evalProbeIrradianceSH9`(E/π) **×π**（L809 `PROBE_SH_RAD_TO_IRRAD`）；Chebyshev 权 var/(var+d²) 下限 0.38 + 法线权 ≥0.40，70/30 混低频 | `DDGIGIGather.comp:669,809,229-234,802` |
| SPGI Average | **radiance-SH** | `sumScale=4π/N`；`sh[0].w=confidence` | `ScreenProbeAverage.comp:213,235` |
| SPGI Gather 输出 | **E**（不乘 albedo） | `evalSHIrradiance` A0=π、A1=2π/3 **一步 L→E**（与 DDGI 腿数学等价）；`rgb=irr/w` | `ScreenProbeGather.comp:68-73,231-233` |
| WRC 探针 | **radiance**（逐方向 RGB16F） | EMA+Chebyshev；CardInject 直注卡片 L_o（"radiance 沿射线不变"，L232） | `WRCUpdateRadiance.comp:129-156`、`WRCCardInject.comp:211-252` |
| SPGI finalize 射线 | **L_i**，各来源同量纲 | SC 命中=L_o 直采（传输语义正确）；WRC/RadianceCascade/DDGI 全 radiance | `ScreenProbeTraceFinalize.comp:352-361,394-427,552-560` |
| SSGI | E（无 albedo） | 命中点 prev-frame L_o **×π** | `SSGITrace.comp:240` |
| SCDDGI 注入 | radiance-SH（与 DDGI 存储约定一致） | CPU 权重=立体角(封顶 π)×NdotL，shader 无额外 4π | `SCDDGIIntegrationModule.cpp:309-335` |
| **FusionIndirect** | `indirect = albedo×gi×gain×(1/π)` | gi 各支路均为 E → 单次 ρ、单次 π | `FusionIndirect.frag:99-120` |

### 4.2 结论

- **无 albedo 双乘**：tile 支路 albedo 在 Resolve（legacy）或 Reconstruct（recon）出口恰乘一次；
  屏幕支路 albedo 只在 Fusion 乘一次。
- **无 π 失配**：DDGI 腿（E/π→×π）与 SPGI 腿（A0=π 一步）数学等价；解析回退分支 π 显式成对
  （`DDGITraceFinalize.comp:506-509`）。
- **无 L/E 混用**：探针侧统一 radiance，屏幕侧统一做恰一次 L→E。

### 4.3 一处注释冲突（归档项，非能量 bug）

`DDGIGIGather.comp:155-166` 的 PAIRED CHANGE 注释写 "NEVER apply this factor inside
SurfaceCacheLightEval::gatherProbeIrradiance"（历史教训：加 π 会令高反照率墙在数分钟内发散），
但 `SurfaceCacheLightEval.comp:383-389` 现行默认**已**在消费端 ×π（注释"两腿 π 一致"）。
两者矛盾的原因：×π 是 radiance-SH 存储的正确 L→E 换算，与当年"回路增益校准"是两回事；
当年发散的前提（CPU 端 ddgi_light_color /π 平衡方案）已被后续校准取代。**处置**：不改能量
代码，先跑 `GI_DCGAIN` 回路增益断言确认现行组合有界，然后修订两处注释并互相引用（修复计划 F6）。

---

## 5. 三处能量不对称（A1-A3，"二值化"成形机制）

### A1 SC tile 天空档陈旧（优先修复）

```glsl
// SurfaceCacheIndirectResolve.comp:243 — 现状
// Sky — same tier as DDGITraceFinalize's escape
// (DDGI_SKY_COLOR × SkyColor.x with the 0.4 default intensity).
radiance = vec3(2.75, 2.88, 3.00) * 0.4;   // ≈1.1
```

注释自证这是**当年对齐 DDGI 0.4 档的同步值**。此后 DDGI/SPGI/WRC 的天空全部改为
`GIDebug().sky_color_intensity` 驱动（现默认 **2.0**，9-20 梯度专项），唯独 tile 反弹路径
被留在 0.4 时代；且基色用的是硬编码梯度（2.75-3.00）而非共享 `EvaluateSkyBase`（4.2-6.0，
`ShaderLib/SkyAtmosphere.glsl:20-25`）。对比：SPGI 天空射线 ≈9–12
（`ScreenProbeGIPass.cpp:1752-1754` CPU 注入 ×2.0），DDGI 逃逸同源（`LumenDDGIPass.cpp:2019-2024`，
far volume 再 ×0.5），WRC 逃逸 `EvaluateSkyBase×tier(2.0)`（`WRCTypes.h:92-99`）。
**中庭二次反弹的源头能量因此比探针路径暗 ~8×。**

**与 [根治] 的关系**：`Docs/2026-09-18-gi-energy-root-cure-plan.md` Phase 1.2 已删除
`DDGI_SKY_COLOR` 常量（`DDGIUpdateIrradiance.comp:130` 注释自证），但漏掉了本源——
`SurfaceCacheIndirectResolve.comp:242` 的注释还在引用这个已删除的常量（化石证据）。
本源是该计划"天空单一事实源"阶段的最后一个未收口点（R1-R7 根因清单未列它）。

### A2 失败→黑 vs 天空回退高亮

SC 查询失败→黑是刻意契约（`WRCTraceFinalize.comp:251` "测量缺失不伪造能量"）；
但同一射线落天空回退时拿到 9–12。命中/失败之间差一个数量级以上，配合 gather 插值
形成空间上的亮暗分区——用户观察到的"二值化倾向"。

### A3 亮度惩罚不对称

prev-frame/HZB 命中带 `1/(1+max(lum-1)×2)`（`ScreenProbeTraceFinalize.comp:301-306`、
`DDGITraceFinalize.comp:488-489`），SC atlas 命中分支**默认不惩罚**（同 shader 仅 diag
mode 3 有，`:374-377`——A/B 档已内置，切换走 `SPGI_TRACE_DIAG=3`）。直射 ×50 的能量
经 SC 命中不衰减地进探针，近处亮面更亮。

---

## 6. 隔离实验可行性矩阵（用户 5 步实验的落地映射）

| 实验 | 旋钮/操作 | 语义判定 |
|---|---|---|
| 基线配方 | `SPONZA_REFERENCE=1 SPGI_CAM_LOCK=1 … SPONZA_CAPTURE_FRAME=780`（[GAP] §4） | ✅ 现成 |
| **只留 SC→SPGI** | `PRIMAL_WRC_ENABLE=0`（管线不创建，`wrc_fb.w=0` 干净短路 `ScreenProbeTraceFinalize.comp:148`）+ `PRIMAL_GI_SKY_INTENSITY=0 PRIMAL_WRC_SKY_TIER=0` + 测试壳 `TestVulkanSponzaRenderGraph.cpp:490` 旁加 `axes.fallback_spgi_to_ddgi=false`（空 view → `ddgi_fb[1].w=0` → shader `:403/:433` 短路→天空(0)）。**需小改一行** | ✅ 干净；此后探针能量只能来自 SC 命中 |
| 只看 DDGI 直采 | Medium 档无 env：`PipelineSetLumenConfig(2)`（EngineDLL 已有）或编辑器面板，或小改 `:489-490` | ⚠️ 需小改 |
| 关 WRC 边（不关 WRC 本体） | `wrc_spgi_consume=false`——**无 env 无 UI**（`RenderPipelineConfig.cpp:328-335` 面板只暴露 5 个轴控件），只能改代码 | ⚠️ 需小改 |
| 冻结探针 | `freeze_probe_updates` **只冻 DDGI**（`LumenDDGIPass.cpp:1708`）；SPGI temporal 照常随相机累积；**切镜/瞬移触发 A11 重置清 SPGI 历史**（视角列差 >1.0，`StandardRenderPipeline.cpp:1640-1670`；平滑移动不触发） | ⚠️ 有陷阱：严格冻结 A/B 须锁机位 |
| 看原始射线 radiance | 观感级现成：`PRIMAL_SPGI_SKIP=denoise`（Fusion 直拿 raw gather，`ScreenProbeGIPass.cpp:1444-1453`）；**此时勿用 PassDebugView=SPGI**（仍绑 filtered 纹理，显示过期内容）。数值级现成：`SPGI_TRACE_DIAG`（§7）。逐射线 buffer 可视化需加 getter（DDGI 有先例 `LumenDDGIPass.h:242-247`） | ✅/⚠️ |
| SC 内容检查 | `PRIMAL_PASS_DEBUG=sc_atlas` 现成；**albedo/normal/depth atlas 无视图**（accessor 已在 `SurfaceCachePass.h:89-91`，加视图为小时级） | ✅/⚠️ |
| ~~关 sc_capture/sc_lighting~~ | **死旋钮**：GIDebug `sc_capture/sc_lighting/sc_shadow` 在 `SurfaceCachePass` 无任何消费点（仅 GIEditorPanel:104-106 与测试壳 GI_ALL_OFF:1944-1953 在写）——**GI_ALL_OFF 的"direct-only A/B"对 SC 不生效**，SC 照跑 | ❌ 不可用（修复计划 F3-V4） |
| 单 mesh 手工高质量卡片 | **无现成机制**（`get_rhi_mesh_card_bake` 只读 blob）；可在注册循环 `StandardRenderPipeline.cpp:1801` 加 override 表 | ❌/需小改（修复计划 F5） |

---

## 7. 量化仪表对照（本调查验证过定义）

| 仪表 | 内容 | 入口/定义 |
|---|---|---|
| `PRIMAL_WRC_TRACE_STATS=1` | `[WRC_SC_FUNNEL]` 16 计数：hits/gridOk/cellFound/found + boundsFail%/orientFail% 等 | `LumenWRCPass.cpp:909-944` |
| `PRIMAL_WRC_BOUNDS_DUMP=1` | boundsFail 8³ 世界分箱 top-4 热点 | `LumenWRCPass.cpp:946-991` |
| `SPGI_TRACE_DIAG=1/2/3` | `uvec4 stats[4][17]`：branch(0=HZB/1=SC/2=far-cache/3=fallback·miss) × bucket(0=全局,1..16=屏幕4×4)；x=count、y=lum×64、z=max lum×64、w=cosΘ×256；mode 2=HZB 去惩罚对照、3=SC 同惩罚 | `ScreenProbeTraceFinalize.comp:87-95,372-377`；读回 `TestVulkanSponzaRenderGraph.cpp:2096-2180` |
| `GI_LADDER_DUMP=1` | 逐像素能量台账 CSV（列以 `FusionCompositeModule.cpp:307-327` 头注释为准——历史教训：列位易读错） | `FusionCompositeModule.cpp:417-428` |
| `PRIMAL_SPGI_SKIP=<stages>` | trace/hzb/average(→黑)/temporal/spatial/post/denoise/gather(→过期) 逐级跳 | `ScreenProbeGIPass.cpp:948-961` |
| `SPGI_PROBE_VIS=1..4` | **DDGI gather** 可视化（irradiance/depth/confidence/探针球热力），非 SPGI | `TestVulkanSponzaRenderGraph.cpp:1908`、`DDGIGIGather.comp:492-515` |

---

## 8. 对既有根因文档的更新点

1. [GAP] §2.2 "393 全 AABB fallback" 的历史表述需以本文 §3-H1 的资产核查为准（已重烘）。
2. [GAP] §5 P0（capture/query 一致性）的状态以 [NEAR]（near=0 证伪 + 参数化）与
   [R-线] 完工记录（own-surface 裁决/qpad）为准；残余主攻=未卡化几何的 boundsFail。
3. 新增 A1-A3 三处不对称为 [GAP] §2 的补充机制——它们解释为什么"传输修好了观感仍二值化"。
4. [PDV] 的缺口清单新增：SC 三 atlas 视图、spgi_raw 视图、probe radiance getter、死旋钮处置。
5. [根治]（`2026-09-18-gi-energy-root-cure-plan.md`，ARCHIVE 标注"待实施"）状态需更新：
   Phase 1 已部分落地（R1 gain 1.0 / R2 DDGI_SKY_COLOR 删除 / Phase 1.4 prev-screen 惩罚，
   本次审计逐一核实）；R 根因清单需补列 SC tile 天空源（§5-A1）与 SC 命中无惩罚（§5-A3）。

## 9. 证据索引

- 单位审计全部 shader 位：§4.1 表（`Engine/Graphics/Vulkan/Shaders/Lumen/` 下 11 个文件）。
- 天空四源消费点：`ScreenProbeGIPass.cpp:1752-1754`、`LumenDDGIPass.cpp:2019-2024`、
  `StandardRenderPipeline.cpp:3223`（WRC sky=(1,1,1) 去双重压缩）、`SurfaceCacheIndirectResolve.comp:243`。
- 边关断短路路径：`StandardRenderPipeline.cpp:3323-3352`、`ScreenProbeTraceFinalize.comp:148,403,433,482`。
- 测试壳轴配置：`TestVulkanSponzaRenderGraph.cpp:488-522`；引擎面板轴暴露：`RenderPipelineConfig.cpp:328-335`。
- 资产 CDB2 计数：`grep -c CDB2 EngineTest/assets/Sponza.model`（378；`.pre_cdbk`=0）。
- bake-cards 动词：`Tools/PrimalEngineCli/main.cpp:1059`（`PRIMAL_SC_BAKE_V2` 门控
  `ContentTools/common/CardBake.cpp:487-488`）。
