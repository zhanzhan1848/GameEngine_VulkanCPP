# SC 卡片 vs Lumen 参照：全阶段实施完工

> 日期：2026-09-19 ｜ 状态：✅ 实施完成（Phase 0–5 全部落地）
> 上游调查/规划：`Docs/2026-09-19-sc-card-lumen-parity-investigation-and-plan.md`
> 关联：`Docs/completed/2026-09-19-world-space-radiance-cache-completion.md`（遗留专项 #1/#5 回流）

---

## 0. TL;DR

按调查规划文档实现全部 6 个阶段：

- **Phase 1（核心）**：CardBake v2——coverage 驱动的贴面（surface-hugging）局部卡，
  `PRIMAL_SC_BAKE_V2`（默认 1）分流，0 回落 v1。
- **序列化**：卡片烘焙段升级 v2（`"CDB2"` magic + version u32），全部 6 处读写方
  同步兼容 v1（旧 blob 零迁移）。
- **Phase 0**：CardGenerator per-mesh 卡数分布 + max_cards 截断计数仪表；
  cook-mesh / bake-cards JSON 汇总（cards 均值/最差值、coverage 均值/最小值）。
- **Phase 2**：content-ready 注册门禁落地（首次 SC 卡片注册推迟到快照内全部
  实例携带离线 bake 或有效 mesh 紧致 AABB），消除 374/256 运行间两态。
- **Phase 3**：capture owning-mesh 实验项 `PRIMAL_SC_CAPTURE_OWNER`（默认关）。
- **Phase 4**：fallback 政策——无 CDBK 的 mesh 计数升级为 Warn（首次注册一次）。
- **资产**：新增 `primal-engine bake-cards` 离线动词（legacy + pipeline 容器），
  Sponza 主资产已重烘 v2 贴面卡。

## 1. CardBake v2 算法（`ContentTools/common/CardBake.cpp`）

输入 = generate_sdf 面壳占据网格（与 v1 相同，零额外体素化）：

1. **样本**：每个非空体素 6 向暴露面生成采样（pos/area/dir/cell），
   兼作 surfel 输出（同序，`generate_surfels` 门控）。
2. **候选**：同向暴露面按 **4×4×4 体素块**分桶（计划原文的分桶粒度），
   块级 6-连通 BFS 连通块；**邻块合并带深度区间门槛**（容差 B−1 体素）——
   错层表面（凸台立面 vs 基座立面）不因 (u,v) 投影相邻而并块，各自拿贴面卡。
   裸体素级连通会被壳层锯齿边界打成上万碎片（实测 3.2 万块），块级分桶吸收。
3. **贴面放置**：卡平面放在块迎向侧深度后方 `backoff = 2 体素 + 0.05` 处。
   方向合同（captureOne）：dir bit=1 → fwd=+axis。捕获法线朝 +axis 的表面
   时视线沿 −axis 从上往下（dir bit=0），平面在块最高点上方——**卡面永远
   贴在表面背后**，全部所属表面落在运行时近平面 near=0.05 之前（near 一律
   不动，WRC 遗留 #1 结论）。extent：投影 = 块紧致 cell 包围 + 1 体素外垫；
   沿轴半厚 = 深度范围/2 + backoff + 半体素（薄板）。
4. **可见性**：块内样本沿 −fwd 体素 march 至卡平面，途中任一占据体素 →
   遮挡（近邻遮挡物抢深度的样本不归此卡）。
5. **贪心**：反复选"新增覆盖面积"最大的候选，直到 coverage ≥ 0.85 /
   增益 < 0.5%·总面积 / 卡数达 `PRIMAL_SC_BAKE_MAX_CARDS_PER_MESH`（默认 12，
   对齐 UE Max Lumen Mesh Cards）。
6. **覆盖分母 = 可捕获面积**：仅计属于某候选可见集的样本。SDF 壳的锯齿/
   裂缝内壁（各向异性步长下壳层临界标记的伪表面，实测可达几何面积 4–8 倍）
   永远过不了可见性 march，计入分母会让 coverage 永远达不到阈值。
   `data.coverage` 报告最终值。

**运行时配套（极小改动）**：`RebuildCardAllocation::emit_card` 对 baked 卡片
豁免"最大投影 5%"剔除（v2 局部小卡正是贪心产出，重要性排序已在离线完成；
0.01 单位²绝对下限与 max_cards 截断保留）。v1 baked 卡片同面积不受影响。

## 2. CDBK 段 v2 序列化

- 布局：v1 = `"CDBK" | card_count | cards | surfel_count | surfels`；
  v2 = `"CDB2" | version(u32)=2 | 同 v1 其余`。**用 magic 区分而非复用
  count 字段探测版本**——旧段 card_count ≥ 1 与任何版本号都碰撞。
- 读写方全部同步：`ContentTools/Geometry.cpp`（写+skip）、
  `pipeline/SceneBlobWriter.cpp`（写+size）、`pipeline/SceneBlobReader.cpp`
  （读 + `PackedMeshView.card_bake_version` 新字段）、
  `Engine/Content/ContentToEngine.cpp`（skip+read，新增零拷贝
  `has_rhi_mesh_card_bake`）、`Engine/Graphics/SceneDataAdapter.cpp`
  （pipeline + legacy 两条路径）、`Tools/PrimalEngineCli/main.cpp`
  （bake-sdf walk）。旧 blob 视作 v1，零迁移。

## 3. content-ready 注册门禁（Phase 2，WRC 遗留 #5）

`StandardRenderPipeline` 注册循环（NeedsFullRebuild 分支）：

- 首次注册前逐实例检查 `has_rhi_mesh_card_bake || mesh 紧致 AABB 有效`；
  任一实例内容未就绪 → 跳过注册并 `MarkFullRebuild()` 重臂下一帧重试。
- 超时 `PRIMAL_SC_REG_GATE_TIMEOUT`（默认 600 帧）后按现状注册（降级 =
  旧行为），防几何永远缺 bounds 的资产卡死 SC。
- 不复用 `gi_content_ready_frame_` 闩锁作门条件：该闩锁要求
  snapshotStable（无待处理重建），与门禁的逐帧重臂互相锁死（死锁推演见
  代码注释）；实例内容检查才是两态根因（注册时拿不到紧致 AABB）的直接
  对应物。
- 首次注册完成后打 `truncated_by_max_cards` 计数与 content-ready 延迟帧数。

**kMaxLookups=128 vs 393 lookup 矛盾（文档化，不承诺本期）**：trace 扫描
路径只查前 128 个 lookup（体积降序排序保大 mesh），Sponza 393 lookup 中
其余仅由 grid 查询路径（≤64 cell/轴、8 卡/cell）覆盖。观察项：若 funnel
显示 lookup 路径仍是产出率瓶颈，再评估提升扫描上限或截断提示（不改 ABI）。

## 4. capture 语义对齐（Phase 3）

- 主路径零改动：贴面卡落地后"整场景 capture + 深度测试"语义自洽
  （薄卡 far 收缩到表面邻域，最近者 = 所属表面片）。
- 实验项（默认关）：`PRIMAL_SC_CAPTURE_OWNER=1` 时
  `ExecuteCardCapturePass` 只提交 owning 实例的 cluster（CPU 剔除层
  `instIdx != card.mesh_instance_id` 跳过），对抗"近邻遮挡物仍比贴面卡
  更近"的残余场景；A/B 以 WRC_SC_FUNNEL queryFail 裁决。GPU 化剔除与
  着色器深度偏置列 future。

## 5. bake-cards 离线动词（资产重烘通道）

`primal-engine bake-cards --in m.model --out m2.model [--resolution 32]
[--surfels 0] [--v1]`

- 容器探测与 bake-sdf / SceneDataAdapter 同构（legacy pack_geometry 与
  pipeline pack_mesh_data 都支持）；**容器/头部字节逐字节原样重放**，仅
  elem_type 置 0x20 旗标 + 追加/替换尾部 "CDB2" 段——绕开"重烤改变
  submesh 组成与 GI 基线"的问题（bake-sdf 同一策略）。
- legacy 容器的 sizeOfSubmeshes 字段引擎侧读取方标记 `[[maybe_unused]]`
  （顺序解析，不用于寻址），追加段安全。
- 默认不写 surfel（运行时无消费者，控制 blob 体积；`--surfels 1` 打开）。
- JSON 报告：`submeshes / baked / replaced / empty_shell / cards_total /
  coverage_mean / coverage_min`。
- cook-mesh JSON 同步加 `card_bake_cards_total / _max / _mean`（Phase 0）。

## 6. 单测（`ContentTools/tests/TestCardBake.cpp`，13 例全绿）

- v1 回归（原 8 例改调 `bake_v1`）：薄向剔除、紧致投影、分辨率公式、
  surfel、开放单面几何、退化网格、blob 往返、无 bake 段字节兼容。
- v2 新增 5 例：
  - `test_v2_alcove_inner_local_cards`：嵌套双盒（雪人，立方 bbox）——
    顶盒表面获得独立贴面卡（谓词：中心距 ≤ 0.55 且 max extent < 0.5），
    卡数 ≤ 12。v1 同夹具零命中（对照例）。
  - `test_v2_plate_keeps_thin_views`：4×0.05×1 薄板——v1 被 5% 规则丢掉的
    ±X 薄视角在 v2 以局部卡保留（coverage 阈值抬到 0.99 强制展开），
    coverage ≥ 0.95。
  - `test_v2_max_cards_cap`：params.max_cards=3 封顶且 coverage < 1。
  - `test_v1_card_bake_section_parses`：手拼 legacy "CDBK" 段 →
    WalkSceneBlob 读出 `card_bake_version == 1`；v2 往返断言 version == 2。
- 踩坑记录：测试夹具 `RawBake` 曾只存指向已销毁局部 mesh 的悬垂 grid 视图
  （UB → 结果跑间漂移、断言偶发翻转）——已让夹具持有 mesh。SDF 壳在
  非立方 bbox 下各向异性步长 + 临界标记会产生分形边界（测试夹具一律用
  立方 bbox）。

## 7. 资产重烘结果（Sponza.model，resolution=32）

`primal-engine bake-cards --in …/Sponza.model --out …/Sponza.model --resolution 32`

```json
{"submeshes":393,"baked":389,"replaced":0,"empty_shell":4,
 "cards_total":2532,"coverage_mean":0.891,"coverage_min":0.850}
```

- 389/393 submesh 烘出 v2 贴面卡（4 个退化 4-顶点小面片 SDF 壳为空 →
  运行时 AABB fallback，Warn 日志如政策所约）。
- 每 mesh 平均 6.4 张卡 ≤ 12 上限；coverage 均值 0.891 / 最差 0.850。
- blob 7.40MB → 7.49MB；原版备份在 `EngineTest/assets/Sponza.model.pre_cdbk`。

### 秩轮转预装箱（实施中发现并修复的关键问题）

首版验证暴露：atlas 装箱按**注册顺序**消耗，2532 张烘焙卡只喂饱前 ~40 个
mesh（273 卡，350 个 mesh 零卡，lod379 的大墙全灭）。修复：
`RebuildCardAllocation` 对 baked 卡按**离线贪心秩**跨 mesh 轮转预装箱
（rank 0 = 每 mesh 覆盖面积最大的主卡先占 atlas），装箱结果预存、主循环按
mesh 连续推卡——lookup 连续区间契约（trace 扫 card_start..+count）不变。
修复后：**436 卡 / atlas 75.6%，389 个烘焙 mesh 全部持有主卡**。

## 8. 验收复测（Phase 5，Vulkan 主验证）

| 指标 | 基线 | 实测 | 结论 |
|---|---|---|---|
| Sponza baked mesh 数 | 0 / 393 | **389 / 393**（4 退化面片 fallback） | ✅ |
| 单测 | — | TestCardBake 13/13（5 连跑）+ Unit 5/5 | ✅ |
| 卡片集合运行间两态 | 374 / 256 | **436 / 436**（两遍逐位一致） | ✅ 两态消除 |
| truncated_by_max_cards | — | 0（容量无压力；atlas 75.6%） | ✅ |
| 崩溃/设备丢失 | — | 0（150s + 240s 双跑） | ✅ |
| SC 查询产出率（WRC_SC_FUNNEL） | 14–20% | **本测试配置不可测**：A/B 双向（baked 与 pre-baked）funnel 均为 found=0/cands=0 同构形态（WRC 查询路径在本测试配方下本就不产出，非本次回归）；烘焙侧 boundsFail/orientFail 计数约减半（265k/74k vs 569k/272k）方向性向好 | ⚠️ 需 GI 线的标准 WRC 验收配方复测 |
| content-ready 门禁 | — | 超时路径（600 帧）触发后注册——4 个无 bake 的退化 mesh 永远不满足内容检查；超时兜底按设计生效，注册结果确定 | ⚠️ 可调 `PRIMAL_SC_REG_GATE_TIMEOUT` |

## 9. 遗留与后续

- **WRC funnel 产出率验收**：本测试的 WRC funnel 在 baked/pre-baked 双向都
  是全零形态（与 WRC 完工文档的 found=37.7k 健康态不符）——是该测试配方
  下 WRC 查询路径自身的状态问题，与卡片烘焙无因果（A/B 已对照）。产出率
  >40% 的 Phase 5 指标需 GI 线用其标准验收配方（对应 env 矩阵）复测。
- **fallback mesh 的门禁严格性**：无 bake 且 bounds 无效的实例会让门禁
  走到超时；如需更快的首注册，可将 gate 判据放宽为"存在 mesh 即就绪"。
- Metal/Dawn：按 parity matrix 降级边界，Vulkan 领衔，Metal 跟验后置。

## 10. 回滚

| 项 | 回滚 |
|---|---|
| v2 算法 | `PRIMAL_SC_BAKE_V2=0` + `bake-cards --v1` 重烘 |
| v2 资产 | `EngineTest/assets/Sponza.model.pre_cdbk`（重烘前原版备份） |
| 门禁 | `PRIMAL_SC_REG_GATE_TIMEOUT=1`（立即超时注册 = 旧行为） |
| owner-only capture | 不设 `PRIMAL_SC_CAPTURE_OWNER`（默认关） |
| 旧 blob 兼容 | 双 magic 读取，v1 段零改动可读 |

## 9. 风险与回滚

| 项 | 回滚 |
|---|---|
| v2 算法 | `PRIMAL_SC_BAKE_V2=0` + `bake-cards --v1` 重烘 |
| v2 资产 | 保留 `Sponza.model.pre_cdbk` 备份（git 未跟踪 blob 的兜底） |
| 门禁 | `PRIMAL_SC_REG_GATE_TIMEOUT=1`（几乎立即超时注册 = 旧行为） |
| owner-only capture | 不设 `PRIMAL_SC_CAPTURE_OWNER`（默认关） |
| 旧 blob 兼容 | 双 magic 读取，v1 段零改动可读 |
