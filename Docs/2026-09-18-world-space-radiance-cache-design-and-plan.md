# World Space Radiance Cache（WRC）设计文档与实施计划

> 标签：**[WRC]** ｜ 关联：**[DDGI]** [SC] [SPGI]
> 日期：2026-09-18 ｜ 状态：✅ 已实施（2026-09-19 全 Stream 落地，完工记录见
> `Docs/completed/2026-09-19-world-space-radiance-cache-completion.md`——含逐 Phase
> 交付对照、验证记录、已知问题与回滚路径；本文档保留作设计参考）
> 决策一句话：**DDGI 全量保留并冻结（Medium 档正解，命名不动）；World Space Radiance Cache 全新实现（`WRC*` 命名域），在 High/Ultra 档接管"世界空间缓存"角色。两条线在目录、类名、shader、配置、env var、文档标签六个维度硬隔离，禁止交叉演化。**

---

## 0. 背景与判定

### 0.1 外部事实（2026-09 核实）

- UE5 Lumen **自 SIGGRAPH 2022 起就包含 World Space Radiance Cache（WSRC）**：世界空间探针缓存远处照明，"方向分辨率高、空间分辨率低"，由 `r.Lumen.ScreenProbeGather.RadianceCache.*` 族 CVar 管理（`ProbeResolution` / `NumProbesToTraceBudget` / `GridResolution`）。WSRC 探针按均匀球面 16×16/32×32 方向 traced，被 Screen Probe 的远场射线消费。
- UE 5.8 新增 **Lumen Lite / Irradiance Field Gather（IFG）**：在 WSRC 之上预积分 irradiance + probe occlusion，直接插值到像素，服务 Medium 档（宣称 ~2× 提速）。这证明"DDGI 式直采像素"不是架构错误，而是**低档正解**；但 UE 的分层是 radiance（WSRC）→ irradiance（IFG）两级，方向性保留在底层。
- 本引擎现状是三条线并存：SC（surface cache）+ DDGI（RTXGI 谱系）+ SPGI（Lumen 式屏幕探针），经五轴配置（X3/A6）的 axis-4 传播边（`propagate_sc_to_ddgi`、`fallback_spgi_to_ddgi`）连接，Fusion 逐像素加权合成。

### 0.2 判定：为什么新增 WRC 而不是改造 DDGI

| 维度 | DDGI（现状） | WRC（目标身份，对齐 Lumen WSRC） |
|---|---|---|
| 身份 | **独立 GI solver**：探针插值直接出像素，还回灌 SC relight（E_probe_floor） | **纯 cache**：缓存"trace SC/SDF 得到的 radiance"，只被上层消费，**从不下注能量到任何被缓存对象** |
| 存储形态 | 预积分 irradiance SH2（9 系数）+ 八面体 depth（mean/variance） | **方向性 radiance**（均匀球面方向格），irradiance 仅在消费端/预积分档出现 |
| 像素关系 | 直接合成进像素（Fusion ddgi 通道） | High 档像素只经 SPGI；直采属 Medium 档 IFG 式路线 |
| 能量回路 | SC↔DDGI 双向（五个 Phase 修复的病根） | 单向：SC →（trace 采样）→ WRC →（消费）→ SPGI → 像素 |
| 网格 | 规则网格（32×16×32 @ 3m，far volume 16×8×16 @ 8m） | 起步同为场景贴合规则网格，A5 起向"需求驱动 + 预算化"演进（UE `NumProbesToTraceBudget` 精神） |

改造 DDGI 去承担 WRC 身份 = 在正在运行的系统上换存储语义 + 换能量方向 + 换消费关系，风险集中在唯一在跑的 Medium 档。**结论：DDGI 冻结保留（它本身就是 Medium 档/UE IFG 式路线的正解），WRC 全新代码域实现。**

### 0.3 目标架构（三级 Radiance Cache）

```
Scene（Sponza / arcade）
   │  SDF trace（GlobalSDF + per-mesh detail SDF —— 已有，两端共用）
   ├─ 近场命中 → SC lighting atlas          = Surface-space radiance cache（真值）
   ├─ 远场命中 → WRC probes                 = World-space radiance cache（只消费、不回灌）
   ↓
SPGI screen probes                          = View-space cache（High/Ultra 档唯一像素入口）
   ↓
Pixel

Medium 档：SSGI + DDGI 直采（[DDGI] 域，现状不动 —— 对应 UE IFG/Lumen Lite 语义）
```

**能量单向律（WRC 的第一设计约束）**：cache 不得向被缓存对象注入能量。
- WRC trace 采样 SC：合法（cache 填充）。
- SC LightEval 读 WRC：**禁止**（这正是 DDGI 线 E_probe_floor 的教训，见
  `Docs/completed/2026-09-18-gi-energy-ownership-audit-and-remediation.md`）。
- SPGI 远场读 WRC：合法（消费）。
- 像素读 WRC：仅 Medium/回退档（IFG 式语义），High 档禁止。

---

## 1. 命名与代码分离公约（硬性规则）

**WRC 是独立代码域，前缀统一为 `WRC`（World Space Radiance Cache），与 DDGI 逐项同构、互不包含。**

### 1.1 命名映射总表

| 维度 | [DDGI]（保留 · 冻结） | [WRC]（新建） |
|---|---|---|
| 目录 | `Engine/Graphics/Lumen/DDGI/` + `Lumen/DDGISlotMap.h` + `Lumen/DDGIFitGrid.h` | `Engine/Graphics/Lumen/WRC/`（新建，A0） |
| 主类 | `LumenDDGIPass` | `LumenWRCPass`（文件 `LumenWRCPass.h/.cpp`） |
| 类型前缀 | `DDGIProbeState` / `DDGIVolumeData` / `DDGIRayData` / `DDGIRuntimeParams` | `WRCProbeState` / `WRCVolumeData` / `WRCRayData` / `WRCRuntimeParams` |
| Slot 三缓冲 | `DDGISlotMap.h` / `DDGISlotsForFrame` | `WRCSlotMap.h` / `WRCSlotsForFrame`（**刻意复制不共享**，见 §1.2-4） |
| 场景贴合 | `DDGIFitGrid.h`（已落地） | `WRCFitGrid.h`（A5 之前不做） |
| Vulkan shader | `DDGITraceRays / TraceSDF / TraceFinalize / UpdateDepth / UpdateIrradiance / GIGather / ProbeIrradianceFromCards / CardRadianceAvg.comp` | `WRCTraceRays / WRCTraceSDF / WRCTraceFinalize / WRCUpdateRadiance / WRCPrefilterIrradiance / WRCCardRadianceAvg.comp`（新增 entry point 一律 `wrc` 前缀） |
| Metal shader | `DDGI*.metal`（同构八件） | `WRC*.metal`（A 线 Vulkan 先行，Metal 按 `Docs/RHI/GraphicsAPI_Parity_Matrix.md` 跟进） |
| Config 字段 | `LumenConfig::ddgi_*`（冻结） | `LumenConfig::wrc_*`（新增段） |
| 五轴 axis-4 | `propagate_sc_to_ddgi` / `fallback_spgi_to_ddgi`（冻结） | `propagate_sc_to_wrc` / `fallback_spgi_to_wrc`（新增） |
| Env var | `PRIMAL_DDGI_*`（冻结） | `PRIMAL_WRC_*` |
| 消费视图 | （DDGI 自有 buffer getter） | `WRCCacheView`（对齐 `GICacheView.h::SurfaceCacheView` 模式的数据快照） |
| GIDebug 通道 | ddgi | wrc（独立探针可视化 / radiance dump / 消费计数热图） |
| 文档标签 | `[DDGI]` | `[WRC]` |
| Commit 前缀 | `fix(gi): [DDGI] ...`（仅 bug） | `feat(gi): [WRC] Phase N ...` |

### 1.2 禁止事项与隔离验收

1. **include 禁令**：`WRC/` 下代码禁止 include DDGI 专有头（`DDGISlotMap.h`、`DDGIFitGrid.h`、`LumenDDGIPass.h`）。共享设施白名单：`GICacheView.h`、`GIWarmup.h`、`GIDebugParams.h`、`GIPlatformProfile.h`、`LumenTypes.h`。
2. **文件属地禁令**：禁止在 DDGI 文件内新增 WRC 功能，反之亦然。`StandardRenderPipeline` 的接线段各自独立 block，块首注释 `// [DDGI]` / `// [WRC]`。
3. **shader include 禁令**：WRC shader 禁止 include DDGI 私有 glsl/metal；`SurfaceCacheData.glsl` 是公共真值层，双方都可以 include。
4. **Slot map 刻意不共享**：`WRCSlotMap.h` 复制 `DDGISlotMap.h` 的语义（writeIndex/prev 契约）而非 include 复用。理由：两条线的时域链路（EMA 参数、reset 时机、失效语义）必须能独立演进，共享头会重新耦合。这是刻意的代码重复，不做 DRY 重构。
5. **grep 验收（A0 门禁）**：`grep -rln "WRC" Engine/Graphics` 与 `grep -rln "DDGI" Engine/Graphics` 的文件集合交集必须只落在白名单文件（`LumenTypes.h`、`GIWarmup.h`、`GIDebugParams.h`、`StandardRenderPipeline.cpp` 的接线区）。
6. **冻结政策**：DDGI 线只修 bug（PR 标 `[DDGI][fix]`），不新增功能；WRC 的缺陷**不得**以修改 DDGI 代码的方式修复。

### 1.3 文档标注公约

- 新 GI 文档头部增加标签行：`> 标签：[WRC] ｜ 关联：[DDGI] [SC] [SPGI]`。
- 存量 GI 文档在下次触碰时补标签；仅讨论探针密度/间距的长期参考文档（如 `2026-09-16-ddgi-world-space-probe-grid-reference.md`）标 `[DDGI][WRC]`（几何经验两线通用）。

---

## 2. WRC 设计

### 2.1 探针数据布局（radiance 形态）

```
WRCProbeData（GPU, 每探针）:
  radiance_dirs[K]   : RGB16F × K     —— K 个均匀球面方向的 incoming radiance（不烘 cos）
  hit_dist_mean      : f32            —— 可见性 Chebyshev 用（对齐 DDGI 已验证思路，独立实现）
  hit_dist_var       : f32
  state              : u32            —— valid / in-geometry 排除位 / 消费计数

K 起步 = 64（Fibonacci 球面）。内存 ≈ 64×6B + 12B ≈ 400B/探针；
near volume 32×16×32 = 16384 探针 ≈ 6.3 MB，far volume 更小，cap-4 多体积上限 ~25 MB（A1 实测确认）。
```

选型依据：UE WSRC 用 16×16/32×32 均匀球面方向；K=64 是其 1/4 预算起步值，方向性足够支撑 SPGI 消费端的 cos 加权积分（这是与 SH2 预积分的本质区别——irradiance 在生成端烘死法线假设，radiance 保留到消费端）。

### 2.2 Trace 与更新

- `WRCTraceRays.comp` / `WRCTraceSDF.comp`：每探针每帧预算 traced N 个方向；射线打 GlobalSDF，近场命中**采样 SC lighting atlas**（经 `SurfaceCacheView` 快照，DDGI 的 GICacheView 用法是参照但不复用其 DDGI 绑定代码）；远场落 sky 契约（与 `DDGITraceFinalize` 同源的 sky tier，常量独立定义于 `WRCTypes.h`）。
- `WRCUpdateRadiance.comp`：EMA 融合（α_radiance 起步 0.1，独立于 DDGI 的 0.02/0.2 双系数）+ Chebyshev 可见性门 + 零穿越命中判定（移植 DDGI Phase 4 的判定原则，`wrc` 前缀重新实现）。
- 预算化：`wrc_update_budget_probes_per_frame`，优先级 = 近期被 SPGI 消费次数（探针 state 里的消费计数器）——UE `NumProbesToTraceBudget` 的精神。
- 暖启动：受 `GIWarmupFrames()` 统一门控（单一事实源，与 SC/DDGI 同源）。

### 2.3 预积分档（IFG 式，可选/调试）

- `WRCPrefilterIrradiance.comp`：从 radiance 方向格预积分 irradiance，供两处：① Medium-grade 降级直采路径（对齐 UE 5.8 IFG 语义）；② 调试对照。**注意**：5.8 IFG 论坛反馈尚有可用性问题，本仓不追其最新形态，此档定位为调试/降级，不是 High 档依赖。

### 2.4 消费接口

- `WRCCacheView`（`WRC/WRCCacheView.h`）：对齐 `GICacheView.h` 的纯数据快照模式，携带 radiance buffer、K、网格 header、探针状态；`ScreenProbeGIPass` 远场段经它消费，不摸 pass 内部。
- SPGI 远场段新增积分路径：命中超过 near distance 后，对 WRC 做 cos 加权 radiance 积分（消费端积分，方向性保留到此处）；axis-4 新边 `fallback_spgi_to_wrc`，冻结边 `fallback_spgi_to_ddgi` 保留作为回退。

### 2.5 Placement（A5 前 = 场景贴合规则网格）

- 起步复用场景贴合思想但独立实现：`wrc_fit_to_bounds` / `WRCFitGrid.h`（A5）。
- 入几何排除：探针 state 的 in-geometry 位 + 零穿越检测；GBuffer 辅助移出（UE 5.8 IFG 行为）挂在 A5。
- 多体积：沿用 cap-4 多体积运行时（T5.5），entry 语义在 `WRCVolumeData` 独立声明。

---

## 3. DDGI 共存与迁移

| 阶段 | Medium 档 | High/Ultra 档世界缓存 |
|---|---|---|
| 现状 | DDGI 直采 | DDGI（fallback 边 + Fusion ddgi 通道） |
| A3 落地 | DDGI 直采（不变） | **WRC（`PRIMAL_WRC_ENABLE=1`）**，回退 = 0 时 DDGI |
| 迁移完成（判据见 §5.3） | DDGI 直采（长期保留，即 UE IFG 语义） | WRC 唯一 |

- DDGI 的 Medium 档角色**永久保留**，不设退役日期；"迁移完成"只指 High 档世界缓存的默认值切换。
- 共存期的默认值：`PRIMAL_WRC_ENABLE` 默认 0（A3 合入时），High 档默认切换在 A4 效果验收后翻 1。

---

## 4. 实施计划（工作流分 Stream，Phase 为可交付单元）

依赖总图：

```
B(能量单向化) ──→ C(Fusion 档位化) ──→ A4(档位接线)
A0(骨架) → A1(radiance trace/存储) → A2(EMA/occlusion+prefilter) → A3(SPGI 消费) ─┘
D(SC 调度/流送) 、E(SC 失效粒度) —— 与 A/B/C 无耦合，独立推进
```

B 必须先于 A3/A4：能量单向律是 WRC cache 身份成立的前提，先建 cache 再断旧回路会让两套世界缓存同时处于病态回路里。

### Stream B —— 能量单向化（先行，与 WRC 无代码耦合）

| Phase | 内容 | 触点 | 门控 | 回滚 |
|---|---|---|---|---|
| B1 | SC LightEval 的 E_probe_floor（`gatherProbeIrradiance`）改暖启动引导项：`GIWarmup` 窗口内生效、窗口内线性衰减到 0 | `SurfaceCacheLightEval.comp`（Vulkan/Metal 双份） | `PRIMAL_GI_PROBE_FEED_BOOTSTRAP=1` 生效新语义，默认 0 | 置 0 |
| B2 | tile radiosity 成为 volume 外 texel 的间接来源（消除 legacy self-feedback 双轨） | `SurfaceCacheLightEval.comp`、`SurfaceCacheIndirectResolve/Reconstruct.comp` | 随 B1 门控 | 同上 |
| B3 | 删稳定器：FusionIndirect 地板项、0.02 灰占位符改 "A=0 跳过" 显式无效语义 | `FusionCompositeModule.cpp`、`SurfaceCacheIndirectResolve.comp` | 随 B1 | 同上 |

验证：ladder dump 逐回路增益断言（<0.5，含新 WRC 环预留位）；`SC_DUMP_CARDS` IND_STATS；fusion_darkening 故障计数；Sponza + arcade 亮度基线（±10%）。

### Stream C —— Fusion 档位化

| Phase | 内容 | 触点 |
|---|---|---|
| C1 | 逐像素 ddgi+spgi 加权 → mode switch：High/Ultra 像素只读 SPGI；Medium 只读 DDGI。`use_screen_probes` 档位结构不变 | `FusionCompositeModule.cpp`、fusion shader |

### Stream A —— WRC 全新实现（`WRC*` 命名域）

| Phase | 内容 | 关键产出 | 验收 |
|---|---|---|---|
| A0 | 骨架：目录 `Engine/Graphics/Lumen/WRC/`、`WRCTypes.h`、`LumenWRCPass`（no-op AddPass）、`LumenConfig::wrc_*` 段、axis-4 wrc 边字段、`PRIMAL_WRC_ENABLE`（默认 0）、CMake、§1.2-5 grep 门禁 | 零行为变化合入 | 编译 + `ctest -R Unit` + 两棵树回归日志无变化 |
| A1 | radiance trace/存储：`WRCTraceRays/SDF/Finalize.comp`、`WRCUpdateRadiance.comp`、`WRCCacheView`、GIDebug wrc 通道；A1 前先拉一次 UE `LumenScreenProbeGather` RadianceCache shaders 对照存储布局（更新 `2026-09-07-lumen-source-analysis-notes.md`） | `PRIMAL_WRC_DUMP=1` 可视化 radiance 方向格 | GIDebug 探针视图无穿模热点；内存实测 §2.1 预算内 |
| A2 | occlusion + 预积分：零穿越移植、`WRCPrefilterIrradiance.comp`（调试/降级档） | 探针 in-geometry 排除率仪表 | 入几何探针数 → 0；prefilter 与消费端积分误差 <5% |
| A3 | SPGI 消费：`ScreenProbeGIPass` 远场 cos 加权积分、`fallback_spgi_to_wrc` 边 | PRIMAL_WRC_ENABLE=1 下 SPGI 远场走 WRC | A/B 对照 DDGI 回退：远场时间方差、暗角/死列症状消失 |
| A4 | 档位接线 + 默认切换：High/Ultra 默认 WRC（与 C1 合流验收） | 预设表更新 | 三场景基线 ±10% + 故障计数周环比下降 |
| A5 | placement 自动化：`WRCFitGrid.h`、需求驱动预算（消费计数优先级）、GBuffer 移出几何 | — | 探针有效率（非排除/非空闲占比） |

### Stream D —— SC 视图驱动调度与流送（独立，与 UE 差距的核心）

| Phase | 内容 | 门控 |
|---|---|---|
| D1 | 卡片重要性打分（屏幕占比 × 1/距离² × 新鲜度，复活 `importance_weight`/`update_distance` 死参数）+ Top-K 替换 LightEval round-robin 与 card-view 轮转游标 | `PRIMAL_SC_PRIORITY=1` |
| D2 | 页表间接化（无淘汰）：card slot → 物理 page 映射，`SurfaceCacheData.glsl` 采样与 resolve 查询契约过页表 | `PRIMAL_SC_PAGE_TABLE=1` |
| D3 | 物理页池 + LRU 淘汰；`capacity_full` 静默丢弃改挤页 | 随 D2 |

### Stream E —— SC 失效粒度

| Phase | 内容 |
|---|---|
| E1 | fallback 卡 AABB 从 `bounds_radius` 球转立方体换 mesh 紧致 AABB（`StandardRenderPipeline.cpp` 注册段） |
| E2 | `CardGenerator` free-list 区域分配器 + `RebuildInstance(i)`；变换脏走增量路径，30 帧冷却改事件驱动 |

### 5.3 迁移完成判据（A4 → High 档默认 WRC）

1. 能量断言：ladder dump 全环增益 <0.5（含 SC→WRC→SPGI 链）。
2. 故障计数：`fusion_darkening` / indirect 类故障计数 7 天窗口无复发。
3. 效果基线：Sponza / arcade / TestProject_01 三场景 GI 亮度与现有 High 档基线偏差 ±10% 内，暗角/死列/室内平填充三类已知症状不回归。
4. 回归门禁：`ctest --test-dir Darwin/Debug -R Unit`、`TestVulkanSponzaRenderGraph` / `TestModularPipeline` 日志无 fault、`./scripts/ci_local.sh` 通过。
5. 回退开关：`PRIMAL_WRC_ENABLE=0` 全量回 DDGI 线，行为与 A3 前一致。

### 纪律提醒（实施时逐条过）

- 不新增 CLI 命令则无 schema 快照影响；若后续加 WRC 调试命令，按 AGENTS.md 需 `PRIMAL_UPDATE_SCHEMA=1` 重生成快照随 PR 提交。
- 新日志走 `PRIMAL_LOG`/stderr；新增 C++ 遵循 `Initialize/AddPass/Shutdown` 与三缓冲惯例。
- 完工文档移入 `Docs/completed/` 并同步 `Docs/ARCHIVE.md`。

---

## 6. 风险与开放问题

| # | 风险/问题 | 对策 |
|---|---|---|
| 1 | K=64 radiance 直采远场方差偏高 | SPGI 时空滤波兜底 + 预算分帧；不足则升 K 或把 A2 prefilter 提前为正式中间档 |
| 2 | WRC 采到未收敛 SC（暖启动窗口内） | `GIWarmupFrames()` 单一门控同步适用；WRC EMA 在暖启动期冻结 |
| 3 | 内存：多体积 cap-4 × ~6.3MB | A1 实测；超出则 far volume 降 K 或减探针 |
| 4 | 共存期双世界缓存的调试复杂度 | GIDebug 独立 wrc 通道 + 档位强制开关；严禁一帧内两条线同时喂 SPGI |
| 5 | UE IFG 本身有可用性反馈坑 | 不追最新形态；prefilter 档仅调试/降级 |
| 6 | WSRC probe 存储布局未知细节 | A1 前拉 UE 源码对照（`2026-09-07-lumen-source-analysis-notes.md` 增补） |

---

## 7. 本文档的登记

- `Docs/ARCHIVE.md` §3.1 在役清单：`📋 2026-09-18-world-space-radiance-cache-design-and-plan.md`。
- ARCHIVE §2 GI 子图新增节点 `GI_WRC`。

（完工后移入 `Docs/completed/` 并同步 ARCHIVE。）
