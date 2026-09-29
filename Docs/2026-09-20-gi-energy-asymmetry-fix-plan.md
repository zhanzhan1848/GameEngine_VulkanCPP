# [GI] 能量不对称修复计划——F0-F6

> 标签：**[GI] [SC] [SPGI] [DDGI] [WRC]** ｜ 日期：2026-09-20 ｜ 状态：✅ 已落地（2026-09-20，
> 完工报告与 A/B 数据见 `Docs/completed/2026-09-20-gi-energy-asymmetry-fix-completion.md`，
> 对比截图见 `Docs/completed/render-comparisons/2026-09-20-gi-energy-asymmetry-fix/`）
> 上游调查：`Docs/2026-09-20-gi-energy-flow-deep-investigation.md`（下称 [调查]，A1-A3 不对称、
> 假设筛查、隔离实验可行性均在彼处归档）
> 关联：`Docs/2026-09-20-sponza-vs-lumen-visual-gap-analysis.md` §4 统一对照配方（下称 [GAP]）、
> `Docs/2026-09-18-gi-energy-root-cure-plan.md`（下称 [根治]——本计划是其 Phase 1"天空单一
> 事实源"的补完 + 轻量前置，见 §0.1）、
> `Docs/2026-09-20-sc-capture-near-plane-mechanism.md`（下称 [NEAR]）、
> `Docs/2026-09-20-sc-correctness-and-reflections-integrated-plan.md`（下称 [R-线]）、
> `Docs/2026-09-20-pass-debug-view.md`（下称 [PDV]）

---

## 0.1 与《GI 能量根治方案》（[根治]）的关系

[根治]（2026-09-18 定稿）的 Phase 1"能量单位与天空单一事实源"**已部分落地**（本次审计核实）：
R1 gain 默认 1.0 ✓（`FusionCompositeModule.cpp:897-903`）、R2 `DDGI_SKY_COLOR` 常量已删除 ✓
（`DDGIUpdateIrradiance.comp:130` 注释自证）、Phase 1.4 DDGI prev-screen 亮度惩罚 ✓
（`DDGITraceFinalize.comp:488-489`）。但**漏了一个同族源**：SC tile 反弹路径的天空
（[根治] R1-R7 根因清单未列它），其注释还在引用已删除的 `DDGI_SKY_COLOR` 常量
（`SurfaceCacheIndirectResolve.comp:242`）——化石证据。本计划 F0 即补完 [根治] Phase 1 的
这最后一个源；F1（限幅 fallback）是 [根治] Phase 3（天空可见性分类）的轻量前置；
F2 是 Phase 1.4 惩罚对称性的 SC 命中版；F6 对应其"单位制"阶段的验证闭环。
**实施本计划不重复 [根治] 已落地项，不改变其 Phase 2/3 的设计。**

---

## 0. TL;DR 与优先级

目标：消除 [调查] §5 的三处能量不对称，补齐能量追踪可视化基建；**不动已经审计自洽的
量纲/π/albedo 数学**。全部条目 env 门控、可回滚，验收走 [GAP] §4 统一配方。

| 项 | 内容 | 规模 | 针对观感 | 依赖 |
|---|---|---|---|---|
| **F0** | SC tile 天空档接线（A1） | 一行 shader + 一个 UBO 字段 | 中庭反弹饥饿、柱面无梯度 | 无（**先做**：它污染所有 A/B 基线） |
| **F1** | 失败语义限幅 fallback（A2 治理） | 小（shader + 一致性） | 间接光二值化 | F0 后复测再定参数 |
| **F2** | SC 命中惩罚对称性 A/B（A3） | **零代码**（diag mode 已内置） | 近处过亮/直射泵入探针 | 无 |
| **F3** | 可视化基建小包（V1-V4） | 各小时级 | 一切排查效率 | 无 |
| **F4** | boundsFail 覆盖缺口（对接 [R-线]/[NEAR] 在案方向） | 已立项专项 | SC 命中暗侧 | 在案，本文只登记接口 |
| **F5** | 资产残项 + 手工卡片 override | 小-中 | 单点实验能力 | 无 |
| **F6** | π 注释冲突归档 + 回路增益复验 | 文档 + 一次 `GI_DCGAIN` 跑 | 防 future 回归 | 无 |

实施顺序：**F0 → 配方 A/B → F2（零代码）→ F3-V1/V2（服务后续排查）→ F1 参数定案 →
F6 → F4/F5 按既有专项节奏**。

---

## F0 SC tile 天空档接线（P0，最先做）

**现状**（[调查] §5-A1）：`SurfaceCacheIndirectResolve.comp:243`
`radiance = vec3(2.75,2.88,3.00) * 0.4;` 是 0.4 档时代对齐 DDGI 的同步值——其上一行注释
引用的 `DDGI_SKY_COLOR` 常量在 [根治] Phase 1.2 中已被删除，此路径成为天空单一事实源的
最后一个漏点；DDGI/SPGI/WRC 均已改 `GIDebug().sky_color_intensity` 驱动（现默认 2.0），
唯此路径被遗留——中庭反弹源比探针路径暗 ~8×。

**方案 A（推荐起步，最小改动）**：
1. `SurfaceCacheTypes.h::FlattenedLightingParams`（`:113-138`）新增对齐 vec4 的
   `vec3 tile_sky_color + float tile_sky_pad`（或复用 radiosity_gate 空闲分量）；
2. `SurfaceCachePass.cpp` 填充处写 `GIDebug().sky_color_intensity * base_gradient`
   （基色暂保留现硬编码 2.75/2.88/3.00——**只恢复档位联动，一次只动一个变量**）；
3. shader `:243` 改 `radiance = flp.tile_sky_color;`，删除硬编码与过时注释。

**方案 B（完全同源，A/B 达标后可选升级）**：Trace 侧对 sky 命中直接存
`EvaluateSkyBase(rayDir) × tier`（方向已知处计算，与 SPGI/DDGI/WRC 四源同函数），
Resolve 只读。改动 ray_hits 布局，收益是消除基色梯度差（2.75-3.0 vs 4.2-6.0）。

**门控与回滚**：`PRIMAL_SC_TILE_SKY_OVERRIDE=<r,g,b>` env 可强制回旧硬编码值；默认新行为。

**验收**（[GAP] §4 配方，frame 780）：
- 中庭 ROI 柱背光面亮度上升且出现"向中庭亮→向内廊暗"梯度；
- `GI_LADDER` ROI 弹射区色度上升（tile 天空是色弹载体）；
- **回归红线**：室内暗部 ROI 不回升（bootstrap 切断后预期安全，仍须实测）；
  `GI_DCGAIN` amp<0.05 线；`[WRC_SC_FUNNEL]` found/hitLum 不回退（本修复不改查询）。

---

## F1 失败语义限幅 fallback（A2 治理，参数在 F0 后定）

**现状**：SC 查询失败→黑（`WRCTraceFinalize.comp:251` "测量缺失不伪造能量"、
`ScreenProbeTraceFinalize.comp` DDGI 回退失败→黑 `:561`），同射线落天空回退得 9–12。

**原则**：保留"不伪造 SC 测量"契约——不给失败射线编造卡片辐射；治理对象是
**fallback 亮度的空间不连续**。

**方案选项**：
- **F1-a（推荐）**：失败射线走**低频有界回退**——取 RadianceCascade L0（32³ 低频场，
  已有）或 DDGI 低频腿（GIGather 已有 30% 低频混合先例 `DDGIGIGather.comp:802`），
  并设亮度上限 `min(fb, sky×k)`（k≈0.3 起步）。消除黑↔亮的硬边界，保留方向性给真命中。
- F1-b：失败射线天空回退亮度全局限幅（更粗暴，可能压暗合法见天区——不推荐先试）。

**验收**：`SPGI_TRACE_DIAG` branch-3 桶的 y(lum×64) 分布收紧；截图二值化边界消融；
`[WRC_SC_FUNNEL]` found 不回退。

---

## F2 SC 命中惩罚对称性（A3，零代码先行）

diag mode 已内置：`SPGI_TRACE_DIAG=2`（HZB 去惩罚对照）/ `=3`（SC 同惩罚）
（`ScreenProbeTraceFinalize.comp:372-377`，读回 `TestVulkanSponzaRenderGraph.cpp:2096-2180`）。

**步骤**：配方下三跑（mode 1 基线 / 2 / 3），对比 branch-0 与 branch-1 桶的 z(max lum×64)
与 y 均值 → 若 mode 3 显著压近处过亮且不伤反弹 ROI，则把惩罚设为 SC 命中分支默认
（一行 + env 回退 `PRIMAL_SPGI_SC_NOPENALTY=1`）。DDGI 侧 `prevRadiance` 惩罚已存在
（`DDGITraceFinalize.comp:488-489`），不需要动。

---

## F3 可视化基建小包（服务全部后续排查）

| 子项 | 内容 | 改动点 |
|---|---|---|
| **V1** SC 三 atlas 视图 | `PassDebugView` 枚举 +3（`sc_albedo`/`sc_normal`/`sc_depth`）+ `ResolvePassDebugViewSource` 分支绑 `GetAlbedoAtlas()/GetNormalAtlas()/GetDepthAtlas()`（accessor 已存在 `SurfaceCachePass.h:89-91`）+ frag 解码分支（normal 八面体解码、depth 线性标尺）+ `.frag.spv` 重编 + Metal 孪生 | `PassDebugView.h:23-40`、`StandardRenderPipeline.cpp:2438-2458`、`PassDebugView.frag/.metal` |
| **V2** spgi_raw 视图 | 枚举 +1 绑 `GetOutputTexture()`（`ScreenProbeGIPass.h:194`，注释即"DEBUG: bypass denoise"）；当前 `spgi` 视图绑 filtered（`StandardRenderPipeline.cpp:2455-2458`） | 同上 |
| **V3** 逐射线 buffer getter | `ScreenProbeGIPass` 加 `GetRayRadianceBuffer()/GetHitDistanceBuffer()`（`probe_radiance_buffer_[3]`/`hit_distance_buffer_` 现为 private 无 getter，`:460,463`；DDGI 先例 `LumenDDGIPass.h:242-247`）；测试壳加 readback dump | `ScreenProbeGIPass.h/.cpp` |
| **V4** 死旋钮处置 | GIDebug `sc_capture/sc_lighting/sc_shadow` 在 `SurfaceCachePass` 无消费者：三选一——(a) 在 `AddPass` 各 dispatch 前接门控（推荐，顺带修好测试壳 `GI_ALL_OFF` 的 direct-only A/B 语义，现对 SC 不生效 `TestVulkanSponzaRenderGraph.cpp:1944-1953`）；(b) 从 GIDebugParams/UI 删除。**禁止维持现状**（误导隔离实验） | `SurfaceCachePass.cpp`、`GIDebugParams.h:24-31`、`GIEditorPanel.cpp:104-106` |

V1/V2 完成后 [PDV] 缺口清单同步收口；ABI 只增不改，schema 快照如涉及 CLI 名须重生成。

---

## F4 boundsFail 覆盖缺口（在案专项，登记接口不动设计）

残余主攻=未卡化几何/掠射带的 boundsFail 50.6%。方向已在案且部分落地：
- [NEAR]：near=0 投影退化证伪；真机制=掠射薄层内容污染；`PRIMAL_SC_CAPTURE_NEAR` 已参数化；
- [R-线] R0-R5：own-surface 裁决、qpad A/B、漏斗自洽收口。

**本文新增的配合项**：
- **逐卡 boundsFail 计数**（现只有全局 + 8³ 分箱 `LumenWRCPass.cpp:946-991`）：在
  `scQueryCardCandidate` 失败路径按 `card_index` 原子累加（新 debug buffer），
  注册侧输出 per-card/top-N——定位"柱子哪张卡失败"；
- V1 的 sc_depth 视图直接目检 capture 深度有效性（配 [NEAR] §3-H1 的薄层鉴别）。

## F5 资产残项与手工卡片 override

1. **4 个 4 顶点退化面片**：bake 壳生成失败的根因修复（generate_sdf 面壳对 4 顶点面的
   最小面积门槛）或运行时 fallback 豁免——随 F4 专项裁决；
2. **单 mesh 手工卡片 override**：`CardGenerator` 加按 geometry_id 的 override 表，
   插入点 `StandardRenderPipeline.cpp:1801`（`RegisterMeshBaked` 调用处），
   数据源可用 JSON/env（`PRIMAL_SC_CARD_OVERRIDE="mesh_id:x0,y0,z0,x1,y1,z1,..."`）；
   服务用户关键实验（单柱+墙+地面强制高质量卡 → 强制 SC-only 观察梯度）。
   实验的隔离旋钮组合见 [调查] §6（`PRIMAL_WRC_ENABLE=0` + 天空 0 + fallback 边关断）。

## F6 π 注释冲突归档 + 回路增益复验

[调查] §4.3：`DDGIGIGather.comp:155-166`（"NEVER ×π in SC gather"）与
`SurfaceCacheLightEval.comp:383-389`（现行 ×π，"两腿 π 一致"）矛盾。
**步骤**：配方下跑 `GI_DCGAIN=1` 确认现行组合 amp<0.05 有界 → 修订两处注释互相引用
（说明 ×π 是 radiance-SH 的正确 L→E 换算、当年发散前提是已被取代的 CPU /π 平衡方案）→
在 `SurfaceCacheLightEval` 留一行回滚警示（改 π 必须两腿同步）。

---

## 验证协议（全部条目共用）

```bash
# 统一配方（[GAP] §4，抓帧 780：content-ready 门禁 600 帧后）
SPONZA_REFERENCE=1 SPGI_CAM_LOCK=1 GI_LADDER_FRAME=780 \
PRIMAL_WRC_ENABLE=1 PRIMAL_WRC_TRACE_STATS=1 \
SPONZA_CAPTURE=/tmp/fix_<项>.ppm SPONZA_CAPTURE_FRAME=780 \
  Darwin/DebugVulkan/TestVulkanSponzaRenderGraph 2>&1 | tee /tmp/fix_<项>.log
```

量化指标与回归红线：
- 反弹结构：GI 场去材质 P90/P10（目标 >3×）；lit-vs-shadow 对比；
- 色彩弹射：GILadder ROI 弹射区色度（列位对照 `FusionCompositeModule.cpp:307-327`）;
- 室内外对比：室内 ROI vs 中庭 ROI 亮度比（向 UE ≥3 EV 靠拢）；
- **红线**：室内暗部 ROI 不回升；`GI_DCGAIN` amp<0.05；`[WRC_SC_FUNNEL]` found/hitLum 不回退；
- 每项修复前后各一跑，入 `Docs/completed/render-comparisons/<日期>-<主题>/`。

## 回滚路径

| 项 | 回滚 |
|---|---|
| F0 | `PRIMAL_SC_TILE_SKY_OVERRIDE="2.75,2.88,3.0*0.4"` 强制旧值；或 env 门控默认翻回 |
| F1 | 限幅系数 k→0（等效关闭） |
| F2 | `PRIMAL_SPGI_SC_NOPENALTY=1` |
| F3 | 纯增量（新视图/新 getter），无需回滚 |
| F5 | override 表为空即原行为 |
