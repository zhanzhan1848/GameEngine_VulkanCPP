# [GI] [WRC] [DDGI] [SPGI] Probe 表示优化计划——方向表示 × 存储布局 × 生命周期 × Debug 解耦

> 标签：**[GI] [WRC] [DDGI] [SPGI] [RC]** ｜ 日期：2026-09-21 ｜ 状态：✅ 已完工（E0/P0/P4/P1/P2 落地，P3 条件未触发）
> **完工记录：`Docs/completed/2026-09-21-probe-repr-e0-energy-dual-track-completion.md`**（含 frame 780
> 验收数据、Debug A/B/B′/C 读数、SC 漏斗首跑结论、780 帧稳定性修复、遗留清单）。
> 修订 R2（2026-09-21）：SPGI 分布计划（`completed/2026-09-21-spgi-probe-distribution-completion.md`，
> P0-P4 落地）后重新评估——新增 §0.1「SPGI 再评估」，P1/射线预算表补 tier3 态口径。
> 修订 R3（2026-09-21 晚）：中庭偏暗上游能量排查（`Docs/2026-09-21-courtyard-upstream-energy-investigation.md`，
> 下称 [能量-排查]）后**重排序**——新增 E0 能量链专项为第一执行序；P0/P4（debug 解耦、定量指标）
> 前移为配套仪表；P1/P2（存储/布局）后移——能量链修复前不产生可感知收益。P 编号保留为稳定 ID
> （跨文档引用不变），执行序见各标题括注与 §汇总表。
> 修订 R4（2026-09-21 深夜）：用户双通路判读（sky 低频 fill 与 surface bounce **两条通路都没托起中庭**，
> 目标形态 L_i = L_sky 低频 fill + L_surface 结构/色弹）后 E0 细化双轨——第 0 步决策仪表
> （Debug A/B/C = diag mode 4/5 + SC 产出率计数器）→ Bounce 轨（送达）→ Sky 轨（份额）。
> [能量-排查] 升 R2：§2.6 反模式核查（引擎天空全部逐射线方向采样，无加性常量）、§5.5 A/B/C 规格、§6 双轨。
> 上游：用户 probe 架构评审（2026-09-21，10 点 + 优先级排序）；本会话逐项代码核实。
> 关联：`Docs/completed/2026-09-20-gi-energy-asymmetry-fix-completion.md`（F0-F6 落地 +
> probe 可视化/debug 状态着色误判案例）、`Docs/2026-09-20-pass-debug-view.md`（视图 24 项）。
> **原则**：方向信息完整保留 + 多层级近远场已是对的；优化点在表示-用途匹配、存储布局、
> 更新成本、过滤链路与跨体系重复度——不在"要不要方向信息"。

---

## 0. 现状普查（评审要求："每体系的颜色是 vec4 还是 texture"）

**结论先行：没有任何探针体系是 vec4，全部携带方向结构。** 实测（2560×1440 渲染档）：

| 体系 | 表示 | 存储 | 实测规模 | 方向性 |
|---|---|---|---|---|
| **WRC** | 每探针 **64 方向 radiance**（half3 打包 2×u32）+ 状态位（valid/inGeo） | `probe_data_` SSBO，AoS，stride 130 u32 = 520B/探针 | 32×16×32 @2m = 16,384 探针 = **8.5MB**（日志实测） | 64 样本球面函数（authoritative）；另 env 门控的预滤波 **SH9 派生副本**（144B/探针 = 2.4MB） |
| **DDGI** | 每探针 **SH9 irradiance**（9×float4）+ **八面体深度 moments**（64 texel mean+variance）+ 重定位偏移 + 状态 | `ddgi_irradiance` SSBO + `ddgi_depth` 纹理 | 近场 volume 配置定 | SH 方向函数 + 可见性 moments，**不是 vec4** |
| **SPGI** | 每探针 **48 射线 × float4 原始 radiance** + SH2（4×float4 = L0+L1） | `probe_radiance_buffer_[3]` + `probe_avg_radiance_[3]` + `probe_temporal_radiance_[3]` SSBO | grid 320×180 = 57,600 探针；射线缓冲 **132.7MB（三缓冲，48 射线档）**；SH 缓冲 6×3.7MB = 22MB | 逐射线原始数据 + L0+L1 |

### 每帧射线预算（2026-09-21 补查："WRC/DDGI 没有多光线吗？"）

| 体系 | 射线/探针·次 | 每帧更新范围 | 射线/帧 |
|---|---|---|---|
| SC indirect trace | 32/tile | 32,768 tiles（stride 2 半速率） | ~1.05M |
| **SPGI** | **48**（T8.13 64→48， Average 64-lane 上限） | 全部 57,600 探针 | **2.76M** |
| **DDGI** | **128**（T8.13 P2 64→128，lane-strided） | 全部 16,384 探针（`max_probes_per_frame=16384`，"no flicker"全量更新） | **2.10M** |
| **WRC** | **64 = K**（射线即存储方向，固定 Fibonacci） | 全部 16,384 探针（budget=全量） | **1.05M** |
| 合计 | | | **~7.0M 射线/帧** |

即：**四个体系全部是多射线、全量更新**。差异在每探针射线数（DDGI 128 > SPGI 48 = WRC 64
≈ SC 32）与更新粒度（SPGI/SC 按屏幕/Tile 密集，DDGI/WRC 世界格 2-3m 稀疏）。注意
WRC 的"射线"与"存储方向"是同一组 64 个 Fibonacci 方向——trace 一次即填充整个方向
表示，无独立采样域。
| **RadianceCascade** | 32³ **3D 纹理 ×4**（L0 + L1x/y/z，RGBA16F） | textures | 常量 | 空间 × SH-L1（SH4） |

### 0.1 SPGI 再评估（2026-09-21 分布计划 P0-P4 落地后）

分布计划落地改变了 SPGI 的槽位/射线结构，本文件口径分两个状态：

**默认态（P0/P1 开，tier3/lightVar/tierRays 关）**——与 §0/P1 数字一致：
均匀格 57,600 探针 × 48 射线，SH2，ray 缓冲三缓冲 132.7MB。P0（代表像素选择）/
P1（F4 normal 门）只改布点质量，不改表示与规模。

**tier3 pilot 态（`PRIMAL_SPGI_TIER3=1`，可叠 `SPGI_TIER_RAYS`/`LIGHTVAR_TOPK`）**：
- 槽位 = tiles×4（230,400），活跃槽 compaction（实测 75,441 = 32.7%），分档射线
  粗档 16 / 象限档 32 / 全额 48——ray 工作量 3.62M → 1.50M（**2.4× 削减**）。
- **表示层结论不变**：SH2 仍是持久表示（评审 #7 的"SH2 过早丢方向"判断保持）；
  raw per-ray radiance 仍是"花了射线得到的方向信息"，P4 tier 门控的"按有效参与
  样本数归一化"恰好为按档升阶（SH2→SH4）或 oct 化铺了路。
- **ray 缓冲分配变更为本文件 P1 的新输入**：tier3 态按 **compact 容量（tiles×2 =
  115,200）** 分配 = 88.5MB/槽 × 3 = **265.5MB，比默认态大 2×**——compaction 缩的
  是工作集，不是分配量（容量按最坏激活预留）。P1 的单缓冲化在 tier3 态收益翻倍
  （省 177MB）；进一步可把容量绑定活跃封顶（lightVar Top-K 512 / 门控上限）而非
  tiles×2。

**每帧射线预算补 tier3 态**：SPGI 默认 2.76M/帧 → tier3+tierRays 态 1.50M/帧（-46%）。

**附带发现：F1-a 生效性修正（重要）**——分布计划顺带修复了
`ScreenProbeTraceFinalize.comp` 的 UBO 声明错位（此前缺 `hint_params` 声明，shader 侧
`sky_params` 实际读到 C++ hint_params）。这意味着能量不对称修复的 **F1-a（失败射线
低频回退限幅）在 9-20 的验证跑中实际恒 0（等价回滚位），天空档位门同样错位**——
F1-a 的设计语义自 9-21 UBO 对齐起才真正上线。F0（SC tile 天空，另一 shader）与
F2（diag_params 位于错位点之前）不受影响。若需 F1-a 的有效性 A/B，须在修复后
二进制上重跑（回滚位 `PRIMAL_GI_FALLBACK_CLAMP=0`），9-20 的 F1 结论应视为
"未验证"而非"已验证"。

### 0.2 能量链再定位（R3，2026-09-21 晚）

用户三图对比（基线/P0+P1/P0-P4，frame 360）显示中庭亮度对 SPGI 分布/表示优化不敏感。
[能量-排查] 逐条核验结论：瓶颈在**能量来源/传输链**，不在探针表示/分布/存储——

1. HZB/SC 命中共用亮度惩罚 `1/(1+2·max(lum−1,0))`（`ScreenProbeTraceFinalize.comp:336-341,413-416`），
   把太阳直射白地面（L_o 10-50，中庭最强合法反弹源）钳到 ≤1——SC atlas 直射烘进 L_o 的结构性后果；
2. SC 查询诚实产出 found 24.7%、boundsFail 50.6% 残余（[GAP] §2.1）；
3. tile 反弹天空基色残留 0.4 时代梯度，2.0 档下仍比探针天空暗 ~2×（A1 的半程残余）；
4. π 失配经 [深查] §4 全链审计**排除**；visibility 无乘积堆叠（B9 已删全栈 ao、Chebyshev 仅 DDGI 腿）。

时线核实：三图均为 9-21 01:34 UBO 对齐后二进制（天空门活）、SC 已注册（R2.3 门禁修复后 frame 360
有效）——"P0-P4 不改变中庭"是同态真实现象，**可作为"SPGI sampling 非瓶颈"的证据使用**。

对本计划的影响：表示/存储类优化（P1/P2/P3）在能量链修复（E0）之前不产生可感知收益，全部后移；
直接服务能量诊断的项（P0 debug 解耦、P4 定量指标）前移为 E0 配套仪表。

### 对评审两处假设的修正（以代码实况为准）

1. **DDGI 可见性 moments 已存在**：`DDGIUpdateDepth.comp` 把射线命中距离分箱进每探针
   64（8×8）八面体 texel，算 **mean + variance** 并做 temporal EMA；Chebyshev 可见性在
   `DDGIGIGather.comp:238` 与 `SurfaceCacheLightEval.comp:130` 两处消费（封锁隔墙采光）。
   评审第 5 点的标准结构（irradiance + visibility + relocation + validity）**已全部落地**，
   不列优化项；残余缺口只是 debug 球没有 visibility/confidence 模式（归入 P0）。
2. **WRC 64→SH9 重建误差统计已有雏形**：`LumenWRCPass.cpp` 的 prefilter A/B
   （`PRIMAL_WRC_PREFILTER=1 + PRIMAL_WRC_DUMP=1`）已实现逐探针 64 方向 vs SH9 的
   相对误差收集（`relErrs`）。P4 只需把它从 A/B 附属输出提升为标准指标 + 阈值门。
3. **WRC 实际规模 8.5MB**（32×16×32@2m），不是评审推算的 20-80MB 级——SoA 改造的
   收益在 clipmap/更大覆盖扩容**之前**不成立，定级 P2（未来导向）。
4. **SH9 派生副本无双写问题（评审 #1 的担忧当前不成立）**：预滤波唯一数据源就是
   `probe_data_`（只读派生），不存在独立更新 SH 的路径。本文档把"64-dir = authoritative、
   SH9 = derived cache"写成架构约定防回归。

---

## E0（第一执行序，R4 双轨化）：上游能量链排查与修复——天空 fill × 表面 bounce 分治

**目标形态**（用户 R2 合成）：`L_i = L_sky(ω)·V(x,ω)·(n·ω) + L_surface`——天空负责大尺度低频 fill、
表面反弹负责局部结构与色弹，两者叠加才接近 Lumen 参照。形态核查（[能量-排查] §2.6）：引擎天空
消费点全部逐射线 `EvaluateSkyBase(rayDir)` 方向采样（miss 才取 = V(x,ω) 隐式承担；加性常量
`probeIrradiance += skyColor×intensity` 反模式不存在，旧 ambient floor 已 B12/B14 删除）——
**缺口在两通路的份额/送达，不在形态**：

| 通路 | 现状 | 缺口性质 → 动作 |
|---|---|---|
| Sky fill | 形态对；到达份额被"深井小天空立体角 × DDGI-first 半档 × adapt 吸收均值"三层限制 | 提**份额**（不是抬档） |
| Surface bounce | 惩罚 `1/(1+2(lum−1))` 把太阳白地面 L_o 10-50 钳到 ≤1 + SC 产出 found 24.7%/boundsFail 50.6% | 根本没**送达** → 根治 |

**执行序（R4：第 0 步仪表 → Bounce 轨 → Sky 轨）**：
- **E0-I 决策仪表（先行，一次跑定双轨幅度）**：
  Debug A/B/C 分通路隔离——diag mode 4（fill-only：branch 0/1→0）/ mode 5（transport-only：
  branch 2/3→0），`diag_params.x` 模式位空闲、env 直通，shader ~15 行（[能量-排查] §5.5 规格）；
  SPGI SC 分支产出率分级计数器（移植 WRC_SC_FUNNEL 分解）。零代码预检可先跑
  `PRIMAL_SPGI_SC_NOPENALTY=1 + SPGI_TRACE_DIAG=2`（量化惩罚钳制量）。
- **E0-B Bounce 轨（主根治，与 [根治] Phase 2 合流）**：SC direct/indirect 分离（LightEval 直射
  另存或查询侧剔除）→ HZB/SC 亮度惩罚退役为 firefly 软限幅（lum>8 才起效）；boundsFail 50.6%
  覆盖残余继续。单纯翻惩罚默认会连直射泵入一起放回，不作替代。
- **E0-K Sky 轨（幅度按 Debug A 读数定）**：F0-b tile 天空基色 → EvaluateSkyBase CPU 镜像（消 ~2×
  残差）；DDGI 逃逸 ×0.5 半档复核 A/B（0.15 时代产物，纯 miss 天空被 DDGI-first 折叠的最大单项）；
  下行射线无兜底计数器；远期 = [根治] Phase 3 点级显式天空可见性——V(x,ω) 的显式形态，
  "中庭高/拱顶低/深室内≈0"空间差异化的正解。

**验收**：A 抬轮廓（低频）、B/B′ 给结构（色弹/梯度）、修复后叠加态接近 Lumen 参照；统一配方
（`SPONZA_REFERENCE=1`，frame 780）中庭阴影面 ROI 显著抬升、深室内 ROI 不回升（红线）、
fixed-exposure 口径（adapt 吸收全局量，[GAP] P2 教训）。反向红线：不得以"提亮"为由恢复已删除的
ambient floor（B12/B14）或引入加性常量天空。

---

## P0（执行序 2）：Probe debug 的状态/辐射度解耦

**问题（已发生一次误判）**：`ProbeSphere.frag` 的状态着色分支（绿=self/黄=relocated/
红=borrowed/灰=untraced）在状态数据到达（60 帧）后**无条件覆盖**辐射度着色——DDGI 球
从此永远显示重定位状态，用户与 agent 都把"单色球"误读成辐射度数据问题。

**改法**（`DebugProbeSphereModule` + 三个 frag）：
1. 通用模式门：`PRIMAL_PROBE_SPHERE_MODE = state | radiance | confidence | variance`
   （默认 state 保持现行为）。frag 经 UBO（现有 `PassDebugParams` 风格）收 mode；
   `radiance` = 现有 SH9 / WRC-64dir 求值；`confidence` = DDGI 用 moments variance、
   WRC 用更新计数；`variance` = P4 的指标着色。
2. WRC 辐射度球 env 入口：`PRIMAL_WRC_PROBE_SPHERES=1` → `SetWRCProbeDebug(true)`
   （现仅编辑器 ABI 可达）。
3. 视图名归一：`PRIMAL_PASS_DEBUG` 不动（球是 overlay 不是视图），但 completion 文档
   与 env 文档登记两组开关的语义区分（**状态=生命周期诊断，辐射度=能量诊断**）。

**验收**：同一帧能分别截出 state 球与 radiance 球；radiance 球上可见方向渐变（拱内暗、
庭院亮）；env 文档登记。**回滚**：env 默认值即现行为。

---

## P4（执行序 3）：三个定量指标（评审："比凭画面判断可靠"）

| 指标 | 现状 | 改法 |
|---|---|---|
| **WRC 64-dir → SH9 重建误差** | prefilter A/B 已算逐探针 `relErrs`（`LumenWRCPass.cpp`） | 提升为 `PRIMAL_WRC_DUMP=1` 的标准输出：P50/P95/max + 超阈探针坐标；阈值门（如 P95>15% 告警） |
| **WRC 角向方差（各向异性健康度）** | 无（dump 只有 meanLum） | dump 循环内对 64 方向亮度算 variance/mean，聚合输出分布；单色球争议的直接量化 |
| **SPGI SH2 重建误差** | 无 | Average 后用 raw rays 留一验证：对每探针 SH2 积分 vs 64 射线加权的 |Δ|；输出 P50/P95 |

这三项直接回答"WRC SH9 要不要降 SH4 / SPGI SH2 够不够"（评审 #4/#7）——**先测后改**；
DDGI 维持 SH9（近场 diffuse volume 定位，评审同意见）。
R3 前移理由：定量指标是 E0 能量链决策（惩罚去留、表示升降档）的判据面，先于存储类项。

---

## P1（执行序 4）：SPGI 原始射线缓冲生命周期审计——实测最大显存项

**实况**（2560×1440，downsample 8 → 320×180 = 57,600 探针 × 64 射线 × 16B）：

- `probe_radiance_buffer_[3]` 三缓冲 = **132.7MB**（48 射线档实测口径；此前按 64 射线
  误算 176.6MB——SPONZA_SPGI_RAY_DUMP 的硬编码 64 已同步修正为读 params）；
- 消费者只有**当帧** TraceFinalize（写）→ ScreenProbeAverage（读，投影 SH2）；
  `ScreenProbeTemporal/SpatialFilter/Gather` 均不读历史原始射线（grep 证实）——
  temporal 链读的是独立的 `probe_temporal_radiance_[3]`（SH 域）。
- 即：**历史原始射线无任何消费者**，三缓冲中只有 1 个槽是当帧数据，其余 2 个是
  in-flight 冗余。
- 若未来 downsample 4（230k 探针）：线性放大到 **530MB**——评审"最大浪费点"判断成立。

**改法**：
1. 三缓冲 → **单缓冲**（finalize 写 → Average 读在同一帧 RG 内，barrier 由 RG 插入；
   先例：`hit_distance_buffer_` 已是单缓冲）或至多 double。预期节省 **88.5MB**（当前档，
   132.7×2/3）/ **353MB**（downsample 4 档）。
2. 若 temporal rejection 未来需要历史命中信息：持久化的应是 **方向化 hit distance
   moments + confidence**（对齐 DDGI 的 oct-depth 先例），不是完整历史 radiance float4。
3. 评审 #7 的中期路径一并登记：raw 64 rays → 8×8 **octahedral radiance**（持久），
   diffuse 走 SH2 投影、rough GI 走 oct 采样——把"花了 64 条射线得到的方向信息"留在
   持久表示里。此项与 ReflSpecularTrace（已存在的镜面专项）分工后再立项。

**验收**：A/B 帧抓图逐位对比（单缓冲切换不改变数值，只改生命周期）；显存
（`vmmap` 或 RHI 统计）前后对照；`SPONZA_SPGI_RAY_DUMP` 仍工作（该 dump 已于
2026-09-21 修正为读实际 rays_per_probe——此前硬编码 64 造成跨探针错位读）。

**tier3 口径（§0.1）**：单缓冲化收益翻倍（265.5MB → 88.5MB，省 177MB）；且
P1 实施前须在 compaction 布局下重核消费者矩阵（finalize/average 走密集 id，
temporal/spatial 读 SH 域不受影响）。

---

## P2（执行序 5）：WRC 520B AoS → SoA / probe-block（未来导向）

**实况**：16,384 探针 × 520B = 8.5MB——当前不是痛点。但 layout 决定所有 pass 的
cache 行为，扩容（clipmap、覆盖加大）前完成。

**消费者清单**（改布局的全部触点）：
| Pass | 访问模式 |
|---|---|
| WRCTraceSDF/Finalize | 写射线结果（`wrc_rays` 暂存）→ `WRCUpdateRadiance` EMA 写 radiance |
| WRCPrefilterIrradiance | 只读 radiance → 派生 SH9 |
| ScreenProbeTraceFinalize（A3 边）| 只读 radiance（`wrc_probe_data` binding 19）|
| DebugProbeSphere（WRC 球）| 只读 radiance + state |
| relocation/classification | 只读 state/position |

**方案**：radiance / state / position 拆独立数组（SoA）；radiance 再按
`[probeBlock=8][direction=64]` tile 化（wave 按探针并行时同 block 方向连续）。
state（2×u32）与 position 与 radiance 的更新频率完全不同，拆开后 relocation pass
不再拉取 520B。

**验收**：重构前后 `PRIMAL_WRC_DUMP` 的 meanLum 逐位一致（纯布局变更）；prefilter
relErr 分布不变；trace dispatch 计时无回退。

---

## P3（执行序 6）：方向表示的 octahedral 抽象（底层暂不换）

评审 #3 采纳其"短期保留 64 samples，长期换抽象"的节奏：
1. `ShaderLib`（已公共化 octEncode/octDecode——`Lumen/ShaderLib/MathLib.glsl`，注意
   Metal 镜像同步纪律）新增 `SampleProbeRadiance(dir)` / `ProjectToProbe(dir, value)` /
   `ProbeDirToOctUV(dir)` 抽象层；WRC 后端先映射到 64-dir 最近邻/双线性（Fibonacci
   环查找），调用方不再感知离散方向。
2. SPGI raw rays 的 oct 化（P1 第 3 点）成为抽象的第一个双后端消费者。
3. 触发条件：任一体系需要"方向 bilinear / 方向过滤 / mip"时落地；不设日期。

---

## P5：统一 Probe GPU 公共库（伴随性，不单独立项）

`EvaluateSH4/SH9/ProjectDirectionalToSH/OctEncode/OctDecode/ChebyshevVisibility` 收敛进
`ShaderLib`（现状：oct 系已在 MathLib.glsl，射线采样已在 ProbeSampleLib.glsl；SH 基与
Chebyshev 仍散在 4+ 处——`ScreenProbeTraceFinalize.comp:546-556`、`DDGIGIGather.comp:210-239`、
`SurfaceCacheLightEval.comp:130`、`ProbeSphereIrradiance.frag` 各有副本）。新消费端
（Fusion 改造、oct 抽象）一律引库，存量逐步替换；**Metal 镜像同步纪律不变**。

---

## 分层原则声明（评审 #8 采纳，写成约定）

> 距离越远，空间频率与角频率同时降低。

层级表示随之定档：near/screen = 64 dir + SH2 → world（WRC）= 64 dir（+SH9 派生）→
DDGI 近场 = SH9 + oct moments → far cascade = SH4 3D。改动任何一档的表示前，先跑
P4 指标证明该档方向带宽不足/过剩。

## 执行序与验收汇总（R3 重排：能量链诊断 → 仪表 → 存储/表示）

| 执行序 | 项 | 规模 | 验收 |
|---|---|---|---|
| **E0** | **上游能量链双轨（R4）**：E0-I 决策仪表（Debug A/B/C diag 4/5 + SC 产出率计数器）→ E0-B bounce 轨（SC direct/indirect 分离 + 惩罚退役）→ E0-K sky 轨（基色/半档/下行黑，幅度按 A 读数定） | 仪表小 → 根治中 | A 抬轮廓 / B′ 给结构读数；统一配方 frame 780 中庭 ROI 抬升 + 深室不回升（红线）+ fixed-exposure 口径 |
| 2 | P0 debug 状态/辐射度解耦 + WRC 球 env 入口 | 小（shader mode 门 + 2 env） | 同帧双截图；env 文档登记 |
| 3 | P4 三指标 | 小（dump 扩展） | 指标进 env 文档 + 阈值 |
| 4 | P1 SPGI raw ray 三缓冲→单缓冲 | 小-中（生命周期 + 验证） | 逐位 A/B + 节省 88.5MB 实测 |
| 5 | P2 WRC AoS→SoA/block | 中（5 触点） | dump 逐位一致 + 计时不回退 |
| 6 | P3 octahedral 抽象层 | 中（ShaderLib + 双消费端） | 触发条件触发时立项 |
| 伴随 | P5 Probe GPU 公共库 | 持续 | 新消费端零副本 |

> R3 排序键变更：从"表示/存储收益"改为"对中庭能量问题的诊断与修复贡献"。P1-P3 的技术内容
> 不变（见 §0.2：能量链修复前它们不产生可感知收益），触发时机后移。R4 仅细化 E0 内部执行序
> （仪表 → bounce → sky），P 系列顺序不变。

## 明确不做（评审共识 + R3 能量链补充）

- DDGI 补 visibility moments——**已存在**（见 §0 修正 1），只补 debug 模式（归 P0）。
- WRC 立即换 oct texture——64-dir 保持，先建抽象（P3）。
- DDGI SH9 降级——保留（近场 diffuse 定位）。
- 任何"探针改 vec4"——方向为伪命题（§0 普查：无一体系是 vec4）。
- **（R3）E0-S2 落地前继续调 ray count/tier/normal 阈值/lightVar**——[能量-排查] §0 ⑤：
  P0-P4 三图对比已证 SPGI 分布非中庭瓶颈。
- **（R3）全局 indirect multiplier / 全局抬天空档**——adapt 默认开下被曝光吸收
  （[GAP] §5 P2 实测 R1≈R2）且室内同步复曝；差异化提亮只能走传输结构（E0-B bounce 轨）。
- **（R4）加性常量天空 / 以"提亮中庭"为由恢复 ambient floor**——[能量-排查] §2.6 反模式核查：
  天空通路形态已正确（全消费点逐射线 `EvaluateSkyBase(rayDir)`，miss 才取 = 遮挡隐式承担），
  会"抹平"的不是方向性天空而是实现形态；B12/B14 删除的 SC texel floor / Fusion floor 是回路治理成果。
