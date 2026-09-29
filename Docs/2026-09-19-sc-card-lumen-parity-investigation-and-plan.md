# SC 卡片 vs Lumen 参照：现状调查与改造规划

> 日期：2026-09-19 ｜ 状态：✅ 已实施（Phase 0–5 全部落地，完工记录
> `Docs/completed/2026-09-19-sc-card-lumen-parity-implementation.md`）
> 参照标准：UE Lumen Surface Cache（Epic 官方文档：Lumen Technical Details；
> per-mesh Card representation，默认 `Max Lumen Mesh Cards` = 12）
> 关联：`Docs/completed/2026-09-19-world-space-radiance-cache-completion.md`（遗留专项 #1/#2 的根因回流）、
> `Docs/completed/superpowers/specs 与 plans/2026-05-07-surface-cache`（SC 原始设计）

---

## 0. TL;DR

**结论先答"卡片现在是不是 per mesh"：所有权上是，生成规则上不是 Lumen 语义。**

- 所有权/粒度：卡片**已经是** per-mesh（per scene instance）——Sponza 按 393 个 mesh 注册，
  每实例独立出卡，`SurfaceCacheCard.mesh_instance_id` 指回实例。**不需要**也不应该改成
  1 meshlet = 1 card。
- 真正的差距在两处：
  1. **布局生成不是 coverage 驱动**——两条来源（离线 bake / 运行时 fallback）都是
     "一个包围盒 × 6 个轴对齐视图"，卡片居中于整个对象而不是贴着表面（surface-hugging），
     没有"贪心选卡直到覆盖率达标"的过程。对凹体建筑，居中视图被外壳遮挡，内壁无表示。
  2. **capture 是整场景重渲**，视线上最近的几何赢深度测试——卡片捕获的是"这个视图里
     离卡最近的表面"，不是"卡片所属 mesh 的表面"。卡片不贴面时两者不相等，
     这正是 WRC 遗留专项 #1（SC 查询产出率 14–20%，queryFail = 捕获深度 vs SDF 命中面
     误差数米）的机制根源。
- 已有资产雪上加霜：**三个 Sponza blob 均无 CDBK 离线 bake 段**（实测字节计数 = 0），
  运行时全部走最弱的 AABB fallback。

修复路径恰好不需要推翻 capture：**把卡片贴到表面**（coverage 驱动离线布局）之后，
"整场景 capture + 深度测试"的语义自动变对（离卡最近的表面 = 卡片负责的表面），
这与 Lumen "Card 决定参数化、Nanite 决定几何提交、两者在 Card Capture 交汇"的分工一致。

---

## 1. 参照标准（从 Lumen 文档提炼的六条判据）

| # | 判据 | Lumen 行为 |
|---|------|-----------|
| R1 | 所有权粒度 | Card = per mesh（per mesh representation），与 Nanite cluster/meshlet 不是 1:1 |
| R2 | 每 mesh 卡数上限 | 默认 ≤ 12（`Max Lumen Mesh Cards`），覆盖不足 → 调大上限或拆 mesh |
| R3 | 卡片形态 | 局部平面参数化 + 正交投影视角；多张卡从**不同位置/方向**贴着表面覆盖，不是"包围盒中心 6 向" |
| R4 | 生成过程 | Surface Coverage 问题：候选卡 → 可见性/贡献评估 → 贪心选择，直到 coverage 达标或卡数上限 |
| R5 | 捕获 | Card view 定义 capture 视角/投影；Nanite cluster/LOD/multi-view 加速几何提交（`ENaniteMeshPass::LumenCardCapture`） |
| R6 | 凹面限制 | 正交投影有遮挡；凹性靠"单 mesh 内多卡 + 资产拆分"消化，被遮挡区域无 cache 表示（回退屏空间 trace） |

---

## 2. 现状调查（代码证据）

### 2.1 所有权：per-mesh ✓（R1 满足）

- 注册循环：`Engine/Graphics/RenderPipeline/StandardRenderPipeline.cpp` ~1481
  （snapshot 全量 Rebuild 时 `ResetRegistrations()` → 逐实例
  `RegisterMeshBaked(bake, world, i)` / `RegisterMesh(wmin, wmax, i)` → `RebuildCardAllocation()`）。
- Sponza 测试按 **393 个 mesh** 建实体/代理
  （`EngineTest/IntegrationTests/TestVulkanSponzaRenderGraph.cpp:669` 注释 "across 393 meshes"），
  即卡片粒度 = mesh 实例，不是 meshlet。
- 变换脏 → 事件驱动 `CardGenerator::RebuildInstance` 单实例增量重建（WRC E2 已落地）。

### 2.2 布局生成：两条来源都不是 coverage 驱动（R3/R4 不满足，核心差距）

**来源 A：离线 bake（T4.2，`ContentTools/common/CardBake.cpp`）**
- 输入 = `generate_sdf` 的面壳体素占据网格（体素中心距表面 < 半体素对角 → 255）。
- 先对**全部非空体素算一次全局紧致包围盒**（`CardBake.cpp:34-45`），然后
  **6 个方向（3 轴 × 2 向）共用这同一个盒**的中心/尺寸出卡（注释写"每向紧致投影包围盒"，
  实现是全局一盒六视图）。
- `extent` 每侧外垫 1 体素（补偿面壳标记在体素中心）；分辨率提示 = 投影大边×2 ×
  64 texels/unit 钳 [16,256]（运行时仍按页重取整，见 2.3）。
- 剔除：投影面积 < 0.01 单位² 或 < 最大投影 **5%**。
- 同时产出 surfel（每个非空体素 6 向暴露面，pos/area/normal，32B/条）随 CDBK 入 blob
  ——**目前无运行时消费者**，但它是现成的表面采样集，Phase 1 的覆盖评估直接用它。

**来源 B：运行时 fallback（`CardGenerator::RegisterMesh`）**
- 无 bake 数据时 6 方向 AABB 推导：center = AABB 中心，extent = AABB 半尺寸。
- AABB 来源优先级（`StandardRenderPipeline.cpp` 注册循环）：mesh 紧致本地 AABB 经 world
  轴展开（E1）→ 无 bounds 才 `bounds_radius` 球转立方体。

**与 R3/R4 的差距**：两种来源都是"一个盒 × 6 个居中视图"。没有 per-direction 的局部卡，
没有位置搜索，没有贡献/覆盖评估，没有贪心迭代，没有 per-mesh 卡数上限概念。
对薄板类 mesh 六向中 4 向被 5% 规则剔除后只剩 2 向；对凹形 mesh（拱券、多层立面）
居中视图的内侧被外侧遮死——被遮挡表面在卡片上根本不存在。

### 2.3 容量、装箱与查询结构（现状数字）

- 配置默认（`Engine/Graphics/Lumen/LumenTypes.h:138-141`）：atlas **2048²**、page **32**、
  LightEval 页预算 **256 页/帧**（档位 0/64/256/512，`RenderPipelineConfig.cpp:162-188`）、
  **max_cards = 4096**。
- 统一发卡（`CardGenerator::RebuildCardAllocation` 的 `emit_card`）：面积过滤（同 5%/0.01）
  → 分辨率 ×64 texels/unit 钳 [max(page, SC_MIN_CARD_RES=64), 4 页] 页对齐 → 货架装箱、
  放不下降档单页 → 页表模式下 LRU 挤页（D3）。
- 排序：lookup 按 AABB 体积**降序**（`CardGenerator.cpp:353` 注释：trace 只扫前 N 个
  lookup）。消费端 `kMaxLookups = 128`（`Metal/shaders/Lumen/DDGITraceRays.metal:200`），
  而 Sponza 有 **393 个 lookup** → 排序只能保住体积最大的 128 个 mesh 可被 lookup 扫描路径
  命中；网格查询路径（`BuildCardGrid`，≤64 cell/轴、`kMaxCardsPerGridCell=8`）不受此限，
  但每 cell 只保留 8 卡。
- 已知不稳定：运行间卡片集合两态 **374 vs 256 卡**——异步 mesh 加载时序影响首次注册的
  AABB 质量（WRC 完工文档遗留专项 #5；content-ready 门禁在
  `StandardRenderPipeline.cpp` NeedsFullRebuild 分支有半成品实现）。

### 2.4 内容烘焙：card-view capture = 整场景重渲（R5 部分满足）

- 入口 `SurfaceCachePass::AddCardCapturePasses`（`SurfaceCachePass.cpp:1354`）：
  seed 期 32 张/帧、稳态 8 张/帧（`SC_CARDVIEW_BATCH` 可调），轮转游标或
  `PRIMAL_SC_PRIORITY=1` Top-K（面积代理 × 新鲜度，30 帧饱和）。
- 单卡捕获 `captureOne`（`SurfaceCachePass.cpp:1406-1457`）：从卡片中心沿
  `axis_direction` 建正交视图，范围 ±(hu,hv)，**near=0.05、far=2×halfAxis+0.5**
  （near 与捕获深度编码耦合，near=0 实验已证伪回滚——见代码注释与遗留专项 #1）。
- 几何提交 `GPUDrivenDrawPipeline::ExecuteCardCapturePass`
  （`GPUDrivenDrawPipeline.cpp:4299`）：
  - **CPU 两级视锥剔除已存在**：实例球 → 逐 meshlet 世界球 vs 卡片正交六平面
    （`GPUDrivenDrawPipeline.cpp:4338-4377`）——用户参照文档建议的
    "Card frustum → cluster culling → rasterize" 在这条路径上**已经落地（CPU 版）**；
  - 但剔除范围是**整场景全部实例**，可见 cluster 以一次 indirect draw 提交，
    **深度测试决定卡片 texel 归属**——没有"owning mesh"概念。
- 后果：卡片 texel 上记录的是"该视图里离卡最近的表面"。卡片居中于对象 AABB 时，
  外壳先于内壁被捕获；capture 深度 ≠ 卡片负责的表面深度。
- legacy 屏幕空间 gather 路径（三 dispatch clear→atomicMin→write）仍在
  （`SC_CARDVIEW` 关闭时启用），本文规划以 card-view 为主路径。

**关键推论**：R5 的"整场景 capture"本身不是错——Lumen 的 capture 也是从卡片视角渲染、
靠深度解析归属。错的是 R3：卡片不贴面时，"离卡最近的表面"不是"卡片负责的表面"。
**修布局（Phase 1）比修 capture 更根本，且让 capture 语义自动变对**：贴面薄卡的 far
收缩到表面附近，视锥内最近者就是所属表面片。

### 2.5 资产现状：Sponza 三个 blob 均无 CDBK（实测）

| blob | 大小 | CDBK 段 |
|---|---|---|
| `EngineTest/assets/Sponza.model` | 7.4 MB | **0** |
| `EngineTest/assets/Sponza_process.model` | 150.9 MB | **0** |
| `EngineTest/assets/Sponza_pipeline_signed_v2.model` | 150.9 MB | **0** |

- cook-mesh 离线通道**已默认开 bake**（`Tools/PrimalEngineCli/main.cpp:723`
  `p.enable_card_bake = 1`），但上述资产是 T4.2 之前或未走该通道烹饪的旧货——
  运行时 393 个实例**全部走 AABB fallback**，离线 bake 链路在主力测试场景上是零覆盖。
- 症状侧写：日志 `cards source = baked (X meshes) / fallback (Y meshes)` 目前应为
  baked=0。

### 2.6 症状 ↔ 机制链（复用已有仪表数据）

已有观测（WRC 完工文档 §3、`WRC_SC_FUNNEL` 360 帧近场累计）：

| 症状 | 数值 | 机制（本调查定位） |
|---|---|---|
| SC 查询产出率低 | ~14–20% | 见下面三行分解 |
| ├ boundsFail | 51% | hitPos 投影不到任何卡片 UV——覆盖缺口（R4 缺失）+ 5% 规则丢薄视角 |
| ├ orientFail | 24% | 卡片轴向 vs 命中法线不匹配——6 向固定轴向覆盖不了任意朝向表面 |
| └ queryFail | 幸存者 ~86% | 卡片捕获深度 ≠ SDF 命中面（误差数米 ≫ 0.48m 容差）——整场景 capture 的"最近者赢"记录了遮挡物 |
| 命中在卡片网格外 | 33% | 同 boundsFail；红毯色彩弹射弱的直接机制 |
| 卡片集合两态 | 374 vs 256 | 异步加载时序 → 注册 AABB 质量漂移（遗留 #5） |

---

## 3. 差距分析对照表

| 判据 | 现状 | 结论 | 对应 Phase |
|---|---|---|---|
| R1 per-mesh 所有权 | 393 实例独立注册、`mesh_instance_id` 回指 | **已满足**，无需重构 | — |
| R2 per-mesh 卡数上限 | 只有全局 max_cards=4096，无 per-mesh 概念 | 缺失 | P1 |
| R3 贴面局部多卡 | 一盒 × 6 居中视图（两条来源皆然） | **核心差距** | P1 |
| R4 coverage 驱动生成 | 固定规则，无覆盖评估/贪心 | **核心差距** | P1 |
| R5 capture 几何提交 | CPU meshlet 视锥剔除已有；整场景 capture 保留 | 部分满足；布局修好即语义自洽 | P3（可选增强） |
| R6 凹面消化 | 靠 393 mesh 拆分部分缓解；单 mesh 内凹形无解 | 部分 | P1+资产政策 |
| 资产 | CDBK 缺失，bake 链路零覆盖 | 工程债 | P2 |

---

## 4. 修改规划

> 总原则：**不动 SurfaceCacheCard ABI（64B）、不动 CDBK wire format 的 32B/条布局、
> 不动 capture 深度编码（near=0.05）**。改动集中在离线 bake 算法、资产重烘、
> 注册门禁三处，运行时消费端（RegisterMeshBaked/emit_card/装箱/调度）零改动或极小改动。

### Phase 0 — 仪表补全（~0.5 天）

- `RebuildCardAllocation`：增加 `capacity_full` 触发计数与 per-mesh 卡数分布日志
  （现只有分辨率分布 printf，`CardGenerator.cpp:333-351`）；
  输出 `baked/fallback` 已有，补充"因 max_cards 被截断的实例数"。
- CardBake v2 报告：每 mesh 输出 `cards=N, coverage=XX%`（Phase 1 的覆盖估计器顺带产出），
  cook-mesh JSON 汇总（`main.cpp` CountMeshStats 已数 `card_baked_meshes`，加均值/最差值）。
- 验收：一次 cook + 一次运行能回答"每个 mesh 几张卡、谁被截断、覆盖率多少"。

### Phase 1 — CardBake v2：coverage 驱动的贴面卡布局（核心，~2–3 天）

位置：`ContentTools/common/CardBake.cpp`（新增 v2 算法，`bake()` 按 env
`PRIMAL_SC_BAKE_V2`（默认 1）分流，0 回落旧算法——回滚即资产重烘）。

算法（把用户参照文档的贪心流程映射到已有的体素壳 + surfel 数据）：

```text
输入: 面壳体素占据网格（grid.voxels）+ 暴露面 surfel 列表（含 pos/normal/area）
样本: S = 朝 6 向中任一方向暴露的 surfel（全向表面采样，面积加权和 = 总表面积）

for axis in {0,1,2}, dir in {+,-}:
    F = S 中法线朝 (axis,dir) 的 surfel                     # 只关心"迎向该视角"的面
    在 (u,v) 投影平面上按体素块(约4×4 voxel)分桶聚成连通块   # 候选区域，天然贴面
    for 每个连通块 c (面积 ≥ min_view_area):
        候选卡:
          axis_direction = axis|dir<<8                      # ABI 不变，仍轴对齐
          卡平面沿 axis 放在 c 的 surfel 深度中位数处        # ← 贴面（关键差异）
          extent[u,v] = c 的投影包围 + 1 voxel 外垫
          沿轴半厚 = c 深度范围/2 + 1 voxel 外垫             # 薄板
        可见性: surfel s ∈ c 被覆盖 ⇔ 投影落入卡矩形 且
               s 与卡平面之间沿 -dir 无占据体素（体素 march，步数 = 薄板厚度）
贪心: 反复选"新增覆盖 surfel 面积"最大的候选卡并标记其覆盖的 surfel
      直到 coverage ≥ 阈值(默认 85%) 或 增益 < 0.5%·总表面积
      或 卡数 == PRIMAL_SC_BAKE_MAX_CARDS_PER_MESH(默认 12, 对齐 UE)
输出: card 列表（走现有 32B 布局）+ coverage 报告；surfels 照旧
```

- **兼容性**：CDBK 段头加 `version` u32（旧段视作 v1）；卡数动态，
  `RegisterMeshBaked` 循环本来就是 `for (bc : bake.cards)`——**运行时零改动**。
- 分辨率提示仍按现公式（贴面后投影更小 → 卡更小更省 atlas，装箱自然受益）。
- 单测：扩展 `ContentTools/tests/TestCardBake.cpp`——嵌套双盒（凹体）用例断言：
  内盒表面获得独立卡片、总覆盖 ≥ 85%、卡数 ≤ 12；开放薄板（单面墙）用例断言
  v1 会丢的薄视角在 v2 中以局部卡形式保留。

### Phase 2 — 资产重烘 + 注册门禁（~1 天）

- 用现役 cook-mesh（bake 已默认开）重烘 Sponza 三 blob（或统一到一份新 blob），
  运行日志确认 `cards source = baked (393 meshes)`、fallback=0。
- 落地 content-ready 注册门禁（WRC 遗留 #5）：首次卡片注册推迟到 mesh 内容就绪，
  消除 374/256 两态。`StandardRenderPipeline.cpp` 中已有半成品（`scMeshesReady`
  检查），小心补完花括号嵌套并验证。
- kMaxLookups=128 vs 393 lookup 的矛盾文档化：grid 查询路径为主时 lookup 仅兜底；
  若 funnel 显示 lookup 路径仍是瓶颈，再评估提升扫描上限或按体积排序的截断提示
  （**不改 ABI**，属观察项，不承诺本期）。

### Phase 3 — capture 语义对齐（~1 天，多数情况零改动）

- 贴面卡落地后，整场景 capture + 深度测试**不再需要改**：薄卡 far 收缩到表面邻域，
  最近者 = 所属表面片（R5 自洽，同 Lumen 分工）。
- 可选增强（实验项，env 门控）：`captureOne` 把 owning `mesh_instance_id` 传入
  `ExecuteCardCapturePass`，CPU 剔除时给 owning 实例的 cluster 置顶/保底，
  或对非 owning 命中加深度偏置——用于对抗"近邻遮挡物仍比贴面卡更近"的残余场景。
  默认关，A/B 用 `WRC_SC_FUNNEL` 的 queryFail 率裁决。
- 性能观察项：CPU 剔除成本 = batch(8) × 全场景 meshlet 数/帧。贴面卡不改变该成本；
  若实测超预算（>1ms/帧 CPU），GPU 化剔除列 future（本期不做）。

### Phase 4 — fallback 政策（~0.5 天）

- 政策：**cook 必带 bake**（bake 已默认开；对无 CDBK 的 blob 在注册循环把
  `fallback` 计数升级为 Warn 日志，便于发现漏烘资产）。
- 无几何可用的运行时 fallback 保持 AABB 6 向不变（无覆盖信息可算，改造收益趋零）；
  如确需改进，未来在 `RebuildInstance`/注册侧用 cluster包围盒做轴向 2×2 切分——
  本期不承诺。

### Phase 5 — 验收复测（~1 天）

| 指标 | 现状 | 目标 |
|---|---|---|
| Sponza baked mesh 数 | 0 / 393 | 393 / 393 |
| SC 查询产出率（WRC_SC_FUNNEL） | 14–20% | **> 40%** |
| boundsFail | 51% | < 15% |
| queryFail（幸存者中） | ~86% | 显著下降（深度一致性成立） |
| 卡片集合运行间两态 | 374 / 256 | 唯一（门禁后确定性） |
| 红毯色彩弹射（观感） | 弱/缺失 | 可见（33% 网格外命中收复） |
| 三端 | Vulkan 主验证 | Metal 跟验；Dawn 按 parity matrix 降级边界注记 |

---

## 5. 对参照文档建议的逐条回应

| 参照建议 | 采纳情况 |
|---|---|
| Card 保持 per-mesh，绝不做 1 meshlet = 1 card | **一致**（现状已是 per-mesh，393 实例佐证） |
| coverage 驱动贪心生成（法线聚类→候选→可见性→贪心→阈值/上限停止） | **采纳**，Phase 1；样本集用现成 surfel/体素壳而非三角形 |
| Card capture 用 cluster/视锥剔除只提交相关 cluster | **已存在**（CPU meshlet 视锥剔除，`GPUDrivenDrawPipeline.cpp:4338`）；GPU 化列 future |
| metadata：CardData{mesh_id, origin/axes, atlas_page/offset} | **已满足**：`SurfaceCacheCard.mesh_instance_id` + `atlas_offset_x/y` + `axis_direction`（轴对齐薄板，v2 不引入任意朝向，避免 64B ABI 扩张；非轴向卡列 future） |
| 凹面靠多卡 + 拆 mesh | **一致**：Sponza 已拆 393 mesh；单 mesh 内凹形由 Phase 1 局部卡消化 |
| "Max Lumen Mesh Cards = 12" | 对齐：`PRIMAL_SC_BAKE_MAX_CARDS_PER_MESH` 默认 12 |

---

## 6. 风险与回滚

| 风险 | 缓解 |
|---|---|
| 卡数膨胀：393 mesh × ≤12 = 4716 候选 > max_cards 4096 | 贪心早停（增益 < 0.5% 停）实际卡数远低于上限；LRU 挤页/页表（D2/D3）与 Top-K 调度（D1）已在役；Phase 0 仪表盯截断计数 |
| atlas 容量不足（2048²，页 32） | 贴面卡投影普遍小于 AABB 全尺寸 → 分辨率与占页双降；必要时 page 32 不动、atlas 与 max_cards 由 governor 档位裁剪 |
| capture 成本随卡数上升 | 页预算 256 页/帧 + 批次 8 卡/帧不变，预算机制天然限流 |
| 旧 blob 兼容 | 无 CDBK → fallback（现状即如此，行为不劣化）；CDBK v1 段仍可读 |
| 深度编码回归 | near=0.05 一律不动；Phase 3 实验项不触碰编码 |
| 回滚 | 算法级：`PRIMAL_SC_BAKE_V2=0` + 重烘；资产级：保留旧 blob 备份；运行时无新开关面 |

---

## 7. 附录：关键文件索引

| 文件 | 角色 |
|---|---|
| `ContentTools/common/CardBake.{h,cpp}` | 离线 bake v1→v2 改造点 |
| `ContentTools/pipeline/AssetPipeline.cpp:73-130` | bake 挂接（SDF 壳复用） |
| `ContentTools/Geometry.cpp:641-789` | CDBK 段序列化 |
| `ContentTools/tests/TestCardBake.cpp` | 单测扩展点 |
| `Engine/Graphics/RHI/Core/RHIMeshAsset.h` | `RHICardBakeCard/Surfel/Data`（32B wire，static_assert 锁尺寸） |
| `Engine/Graphics/Lumen/SurfaceCache/CardGenerator.{h,cpp}` | 注册/发卡/装箱/排序/网格 |
| `Engine/Graphics/Lumen/SurfaceCache/SurfaceCachePass.cpp:1354` | card-view capture 调度 |
| `Engine/Graphics/Nanite/GPUDrivenDrawPipeline.cpp:4299` | capture 几何提交（CPU meshlet 剔除） |
| `Engine/Graphics/RenderPipeline/StandardRenderPipeline.cpp:~1481` | 注册循环 + content-ready 门禁半成品 |
| `Engine/Graphics/Lumen/LumenTypes.h:138-141` | atlas/page/budget/max_cards 配置 |
| `Tools/PrimalEngineCli/main.cpp:665-744` | cook-mesh（enable_card_bake=1、资产统计） |
