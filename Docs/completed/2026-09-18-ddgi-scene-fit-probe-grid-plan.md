# DDGI 场景贴合 Probe 网格（scene-fit grid）实施方案

> 日期：2026-09-18 ｜ 状态：✅ 已实施（P0/P1-A/P1-B 完成；P2 = 后续工作）
> 实施与对比结果：[2026-09-18-ddgi-scene-fit-grid](render-comparisons/2026-09-18-ddgi-scene-fit-grid/README.md)
> ——trace 主 pass 3.62→0.65ms、门禁重锚定后全绿；中庭反弹朝参考收敛、封闭拱腹过亮
> （修复方向 = P2 sky-probe 分类 + 亮度标定复核）。
> 目标：把 Vulkan DDGI 的 near volume 从"固定 32×16×32 @ 3.0、原点居中"改为
> **以场景 bbox 为覆盖目标、由 spacing 反推 probe 排布、原点对齐 bbox 角点**。
> 上游依据：[probe 网格参考](../2026-09-16-ddgi-world-space-probe-grid-reference.md) §1.5/§4、
> [probe 选择密度复查](render-comparisons/2026-09-17-probe-selection-density-audit/README.md) §3.2/§7、
> [effective origin 契约修复](2026-09-17-ddgi-effective-origin-contract-fix.md)。

---

## 1. 背景与动机

### 1.1 现状（2026-09-18 核对）

| Volume | 网格 | 间距 | probe 数 | 覆盖 (N-1)×S |
|---|---|---:|---:|---|
| Near | 32×16×32 | 3.0 | 16,384 | 93×45×93（±46×±22×±46） |
| Far | 16×8×16 | 8.0 | 2,048 | 120×56×120 |

`LumenDDGIPass::Initialize()` 把 `probe_origin_` 固定为**世界原点居中**
（`-(count-1)*spacing*0.5`，`LumenDDGIPass.cpp:411-416`），counts 完全由配置给定，
与场景实际几何无关。

### 1.2 场景实际尺寸（`Sponza_pipeline_signed_v5.model`，顶点缓冲逐顶点实测）

| 口径 | min | max | 尺寸 |
|---|---|---|---|
| **几何 AABB**（精确） | (-19.21, -1.26, -11.83) | (18.00, 14.29, 11.05) | **37.2 × 15.6 × 22.9** |
| mesh SDF bounds 并集（含每 mesh padding） | (-20.81, -6.93, -13.60) | (19.58, 19.82, 12.52) | 40.4 × 26.8 × 26.1 |
| Global SDF 合并体积（+2% margin，res 384³） | (-21.62, -7.47, -14.12) | (20.39, 20.36, 13.04) | 42.0 × 27.8 × 27.2（体素 ≈0.109） |

**Near volume 覆盖盒是几何的约 2.5 倍**（93/37 ≈ 2.5）。多出的 probe 绝大多数悬在
建筑外的空气里：

- SH 内容 = 纯天空（全方向逃逸），对室内像素无贡献；
- 更新侧每帧全量烧预算（16,384 = `max_probes_per_frame` 上限），逃逸光线按
  `fallbackStride` 跑满 50m，是**单条成本最高**的一类 ray；
- CPU 每帧全量回读 16,384 份深度图（128 float/probe ≈ 8MB/帧）计算 variance，
  其中空气 probe 的份额全是白读。

### 1.3 为什么现在做

- 参考文档 §1.5 已明确：下一步增益**不是**加密（`32×16×32 → 64×32×64` 成本爆炸），
  而是 scrolling / relocation / classification；本方案是其中"收覆盖"的最小第一刀，
  且不依赖锁死的 scrolling（`UpdateProbeOrigin` early-return）。
- 密度复查审计 §7 的同预算 A/B 矩阵（"局部 DDGI 3m/1.5m：**保持同一世界 AABB**、
  改变 counts"）需要一个能按 AABB 推导 counts 的入口——本方案提供该入口。
- effective origin 契约（2026-09-17）已把重定位坐标统一到 relocated-origins 缓冲，
  网格重排不会再踩 P0 坐标错位问题。

---

## 2. 目标 / 非目标

**目标**

1. near volume 覆盖盒 = 场景 bbox（可配 margin），probe 数由 spacing 反推；
2. probe 原点 = bbox min 角点（RTXGI AABB 约定 `(N-1)×S` 覆盖，参考文档 §2.1）；
3. 排布经既有 `DDGIRuntimeParams` 管线生效于 Vulkan 端 DDGI（trace/update/gather
   全链路，**零 shader 改动**）；
4. 默认关闭（flag），Sponza 测试显式开启；Metal/Dawn/生产路径行为不变。

**非目标**

- 不动 SPGI（屏幕驱动，无世界网格）；
- 不实现 scrolling / importance 调度 / classification（后续步骤，见 §8）；
- 不改 EngineDLL 导出面、CLI schema、POD 镜像。

---

## 3. 布局推导规则（本方案的核心公式）

对目标覆盖盒 `[fit_min, fit_max]`（extent 记 `E_i`）与给定 spacing `S`：

```
N_i = ceil(E_i / S) + 1          （保证 (N_i - 1) × S ≥ E_i，逐轴）
origin = fit_min                  （probe[0] 位于 min 角点，角点约定与
                                   GetVolumeBounds 的 padding 语义一致）
counts clamp 到 [4, 64]（沿用 GIEditorPanel 既有钳制范围）
```

### 3.1 Sponza 具体排布（按几何 AABB 37.2 × 15.6 × 22.9）

| spacing | 推导 counts | probe 数 | 实际覆盖 | vs 现状 near |
|---|---|---:|---|---:|
| 3.0（现行） | 14×7×9 | **882** | 39×18×24 | 18.6× |
| 2.0 | 20×9×13 | 2,340 | 38×16×24 | 7.0× |
| 4.0 | 11×5×7 | 385 | 40×16×24 | 42.6× |

（按 SDF 合并体积 42.0×27.8×27.2 推导时，3.0 间距得 16×11×11 = 1,936——
两套 bbox 口径差 ≈2×，§5 的 margin 参数用于在此之间取舍。）

每 probe 光线 128 条不变：3.0 间距下 near trace 从 2.10M ray/帧降到 0.11M ray/帧。
按 P0 实测 DDGI 全子过程 1.6ms 线性折算，预期 **≤0.2ms**；深度回读 8MB → ≈0.45MB。

---

## 4. bbox 来源（复用既有供给链，不新增资产依赖）

现成链路已经把"场景贴合的合并 SDF 范围"送进渲染管线：

```
OfflineSDFMerger::LoadFromPipelineModel / LoadFromCache
    → GetOrigin() / GetExtent()            （merger 体积 = 几何 ∪ mesh padding + 2% margin）
    → TestVulkanSponzaRenderGraph:1012  pipeline_->SetOfflineSDFSource(tex, origin, extent, res)
    → StandardRenderPipeline 的 offline_sdf_origin_ / offline_sdf_extent_   ★ bbox 在这里
```

两个候选口径：

| 口径 | 优点 | 缺点 |
|---|---|---|
| A. 离线 SDF 合并体积（`offline_sdf_origin_/extent_`） | 零新增输入；与 tracer 看到的场一致；自带 padding | 比几何大 ≈2×（mesh 20% padding 叠加）；依赖 SDF 源存在 |
| B. 场景几何 AABB + 显式 `fit_margin` | 贴得最紧；不依赖 SDF | 需要测试/宿主侧先算出并传入 AABB |

**决策：P0 用 B（显式传入，Sponza 测试硬编码本文件 §1.2 实测值），A 作为 P1 自动化。**
理由：P0 改动面最小（见 §5 时序问题），且审计 A/B 需要"同一 AABB 只改 counts"，
显式传入恰好满足口径稳定性；口径 A 可在 P1 由 `SetOfflineSDFSource` 自动填充。

---

## 5. 代码改动点（全部为 C++ 配置/编排层，零 shader）

### 5.1 `LumenTypes.h` — `LumenConfig` 新增 fit 字段

```cpp
// DDGI scene-fit grid：bbox + spacing 反推 counts（优先于 ddgi_probe_count_*）
bool     ddgi_fit_to_bounds = false;
math::v3 ddgi_fit_bounds_min{0.0f};
math::v3 ddgi_fit_bounds_max{0.0f};
float    ddgi_fit_margin = 0.0f;   // 每 side 外扩（世界单位），0 = 贴 bbox
```

`LumenVolumeConfig` 不动（`origin_override`/`use_origin_override` 保留给静态烘焙
与 gizmo，语义不混用）。

### 5.2 `LumenDDGIPass.h` — `DDGIRuntimeParams` 补 origin

```cpp
math::v3 probe_origin{0.0f};
bool     use_probe_origin = false;   // false = 居中（现状），true = 用 probe_origin 作 min 角点
```

### 5.3 `LumenDDGIPass::Initialize()`（`LumenDDGIPass.cpp:411-416`）

```cpp
if (params_.use_probe_origin) {
    probe_origin_ = params_.probe_origin;
} else {
    probe_origin_ = { -(count_x-1)*S*0.5f, ... };   // 现状居中，缺省不变
}
```

其下所有消费方**无需改动**：probe position buffer 填充（:421-450）、
`volume_data_.ProbeOrigin` 同步（:459，注释里"positive octant"教训的防护点）、
`GetVolumeBounds()`（:1070，origin-aware）。GLSL 侧全程只读 UBO 的 ProbeOrigin +
counts 做格点算术，与排布来源解耦——这就是零 shader 改动的依据。

### 5.4 `StandardRenderPipeline::InitializeLumenPasses`（约 :936-996）

`vols.empty()` 推导分支中，当 `ddgi_fit_to_bounds`：

```cpp
// margin 外扩后逐轴推导；counts 落入 vp.probe_count_*，min 角点落入 vp.probe_origin
vols[0].probe_count_x = clamp(ceil(E.x / S) + 1, 4, 64);  // y/z 同理
// 新增：把 origin 传给 runtime params（现状该结构体没有 origin 字段，
// config 的 origin_override 从未流进 DDGI pass——本改动顺带闭合此缺口）
```

far volume 保持现有独立配置（它的职责是 near 盒外 fallback，覆盖仍应大于场景）。

### 5.5 时序问题与 P0 落位

`SetOfflineSDFSource` 在管线 Initialize **之后**才被测试调用——若 P0 选口径 A，
需要触发 DDGI volume 重建（`SetLumenConfig → ApplyConfigChanges` 拆建路径已有
先例，见 `StandardRenderPipeline.cpp:989` 注释）。**P0 因此选口径 B**：测试在
构造 pipeline 前把 fit 字段写进 `LumenConfig`（值 = §1.2 实测 bbox），一次
Initialize 即生效，无重建时序。口径 A 的自动化留 P1。

### 5.6 一致性守卫（必须逐条核对）

| 守卫点 | 现状 | 动作 |
|---|---|---|
| `InitializeProbesFromStatic`（:1363-）维度校验 | counts 不匹配 → 落 sky_estimate | 无需改；Sponza Vulkan 走 dynamic mode，不触及 |
| `UpdateProbeOrigin` 滚动锁死（early-return） | 网格静止 | 保持锁死；fit 是"静态网格换排布"，不是滚动 |
| `volume_data_.ProbeOrigin` 同步（:459） | Initialize 一次性同步 | use_probe_origin 路径必须继续走该同步（positive-octant 教训） |
| `RadianceCascadeUpdate` / SPGI miss 回退 | 读 flat irradiance + counts，按 UBO 推导 | 自动适配新 counts，无改动 |
| 编辑器 gizmo（`GIVolumeGizmo`） | 读 `origin_override` 显示体积 | P1 跟进：从 pass 回读 fit 后的 origin/extent 做显示，P0 不阻塞 |

---

## 6. 验证计划

1. **单元**：`ctest --test-dir Darwin/Debug -R "Unit"`——新增一个纯函数级用例锁定
   `ceil(E/S)+1` 推导（37.2/3.0 → 14、15.6/3.0 → 7、22.9/3.0 → 9，含 clamp 边界）。
2. **Vulkan 冒烟**（`TestVulkanSponzaRenderGraph`，按 AGENTS.md 跑 60-120s kill）：
   - init 日志出现 `[Lumen] DDGI volume 0 initialized (14x7x9 @ spacing 3)` 与
     `ProbePositionBuffer filled: 882`；
   - `DDGI_ORIGIN_STATS=1`：近场 self/displaced 分布仍健康（对照 2026-09-17 基线：
     self=100%、displaced 2.95%、max ≤ 0.95×spacing）；
   - `DDGI_AGE_STATS=1`：`update_ratio = 882/882`（远低于 16384 预算 → 恢复全量
     无饥饿更新）。
3. **数值门禁**：GIFerify/GIGolden 双端回归。网格重排会改变探针场 SH 采样位置，
   golden 值预期漂移——**需重生成并附 justification**（本方案 = 布局变更，非能量
   语义变更；UpdateIrradiance/权重公式未动）。
4. **画质 A/B**（锁定参考机位，复用审计的 measure.py / comparison 管线）：
   当前配置 vs 14×7×9 @ 3.0 同 spacing 同 AABB——中庭立面/暗地面的反弹层次
   不应回退（审计 §3.2 已确认中庭在 near 覆盖内，紧缩到几何 bbox 后仍在覆盖内）。
5. **性能**：P0 口径复测 DDGI 子链耗时（基线 1.6ms @ 128 rays），预期 ≤0.2ms，
   记入本文件 §7 实测表。

---

## 7. 预期收益与风险

**收益**

| 项 | 现状 | fit 后（3.0） |
|---|---|---|
| near probe 数 | 16,384 | 882 |
| near trace ray/帧（128 rays） | 2.10M | 0.11M |
| 深度回读/帧 | ≈8MB | ≈0.45MB |
| 天空 probe 占比 | 高（盒外空气） | 大幅下降（盒贴几何） |
| `max_probes_per_frame` 预算 | 顶满（半预算 governor 运行） | 大量富余 → 全量更新无饥饿 |

**风险与缓解**

- **贴墙 probe 更稀 + 重定位上限 0.95×spacing**：网格更粗意味着单格内埋没 probe
  占比可能上升；effective origin 契约（2026-09-17）已保证重定位/借格坐标一致，
  且 Gather 埋没门控与借格半权已落地。A/B 中若暗部回退，第一步是降 spacing 到
  2.0（2,340 probes 仍只有现状 1/7），而不是回退方案。
- **Y 轴特殊**：几何 Y 跨度只有 15.6 且地板在 -1.26，现状网格在 y<0 有大量地下
  止损 probe；fit 后 Y 仅 7 层，需在 A/B 中确认地面/地下一层无新增漏光。
- **golden 漂移误判**：§6.3 的重生成必须注明布局变更语义，防止被当成能量回归。
- **口径漂移**：fit bbox 硬编码在测试里（P0），模型重烘焙后需同步；P1 切口径 A
  自动化后消除。

---

## 8. 实施步骤

- [x] **P0**（本文件 §5.1–5.5 + §6.1/6.2/6.5）：fit 字段 + origin 贯通 + Sponza 测试
      开启 + 单测/冒烟/计时。验收 = 日志计数正确、DDGI 子链 ≤0.2ms、进程稳定。
      **实测**：14×7×9=882、trace 主 pass 3.62→0.65ms（子链固定项 ≈0.8ms 使绝对值
      高于线性折算的 0.2ms 预期，见对比文档 §4）、进程稳定。
- [x] **P1-A**：bbox 自动化——`SetOfflineSDFSource` 时以 merger 体积回填 fit 字段
      （含 §5.5 重建时序）；GIVolumeGizmo 显示 fit 后实际体积。
      **实测**：SPONZA_FIT_AUTO=1 → 42.0×27.8×27.2 → 16×11×11=1,936，与 §3.1 口径 A 一致。
- [x] **P1-B**：同 AABB 密度 A/B（3.0 vs 1.5，审计 §7 矩阵行），产出对照结论。
      **结论**：1.5 无可见画质收益、trace 成本 2–4×，维持 3.0（对比文档 §3/§4）。
- [ ] **P2**：与 scrolling/classification 合流——fit 提供的是"静态排布的正确起点"，
      滚动激活与 sky-probe 分类在此之上继续收预算（参考文档 §1.5 三件套）。
      **优先级上调**：fit 后封闭拱腹/暗龛过亮（天空逃逸探针渗入），sky-probe
      分类从"收预算"同时变为"画质修复项"，见对比文档 §6。

---

## 9. 关联文档

- [DDGI World-Space Probe Grid 分布参考](../2026-09-16-ddgi-world-space-probe-grid-reference.md)
  —— 密度/间距/覆盖公式（`(N-1)×S`）与 RTXGI 经验的长期锚点
- [Probe 选择密度复查 2026-09-17](render-comparisons/2026-09-17-probe-selection-density-audit/README.md)
  —— §3.2 覆盖现状、§7 同 AABB 密度 A/B 矩阵、§5 重定位坐标契约
- [DDGI effective origin 契约修复](2026-09-17-ddgi-effective-origin-contract-fix.md)
  —— relocated origins 缓冲与借格半权，本方案的坐标安全前提
- [SPGI Screen Probe 分布参考](../2026-09-16-spgi-screen-probe-distribution-reference.md)
  —— 屏幕侧 probe 体系（本方案不触及）
