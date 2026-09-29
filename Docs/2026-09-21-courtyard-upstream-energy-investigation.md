# [GI] 中庭偏暗上游能量链排查——用户五假设逐条代码核验与实验映射

> 标签：**[GI] [SC] [SPGI] [DDGI] [WRC]** ｜ 日期：2026-09-21 ｜ 状态：✅ 诊断完成 + 修复项落地（§6 第 0 步/Bounce 轨/Sky 轨 K-1~K-3 已实施；K-4 维持远期立项）
> **完工记录：`Docs/completed/2026-09-21-probe-repr-e0-energy-dual-track-completion.md`**。
> 首跑仪表结论：SPGI SC 拒绝主因 orientFail 47.9%（非 boundsFail）；下行黑区可忽略（5 射线）。
> 触发：用户基于基线/P0+P1/P0-P4 三图对比提出的五条根因假设（sky miss 返 0 / 能量结构缺 sky+bounce /
> π 失配 / 多层 visibility 过杀 / SPGI sampling），要求按文档排查、出结论、并重排
> `Docs/2026-09-21-probe-representation-optimization-plan.md` 优先级。
> 修订 R2（2026-09-21 晚）：用户合成**双通路判读**——"低频 sky fill 与局部 surface bounce 两条能量通路
> 都没把中庭托起来"，目标形态 L_i = L_sky(方向性×遮挡×cosine，大尺度低频) + L_surface(反弹结构/色弹)；
> 并提出 Debug A/B/C 分通路隔离实验。新增 §2.6（反模式核查：引擎无加性常量天空，全部逐射线方向采样）、
> §5.5（diag mode 4/5 规格），§6 修复分级改为双轨排序。
> 关联：`Docs/2026-09-20-gi-energy-flow-deep-investigation.md`（下称 [深查]，单位审计 + A1-A3）、
> `Docs/completed/2026-09-20-gi-energy-asymmetry-fix-completion.md`（下称 [不对称-F]）、
> `Docs/2026-09-20-sponza-vs-lumen-visual-gap-analysis.md`（下称 [GAP]）、
> `Docs/completed/2026-09-21-spgi-probe-distribution-completion.md`（下称 [分布-完工]）、
> `Docs/2026-09-18-gi-energy-root-cure-plan.md`（下称 [根治]）

---

## 0. TL;DR

用户的总判断（"问题在上游能量来源/传输链，不在 SPGI sampling"）**成立**，且文档链对此已有大量在案证据。
五假设核验结果与概率排序：

| # | 用户假设 | 核验结论 | 一句话机制 |
|---|---|---|---|
| ① | sky miss 返 0 | **历史成立，现状已修，残余两处** | 9-21 01:34 UBO 对齐前天空档位门恒 {0,0,0}=全黑；修复后 miss→EvaluateSkyBase×2.0≈8.4-12。残余：纯 miss 先走 DDGI（逃逸天空 ×0.5 半档）、adapt 会吸收全局抬档 |
| ② | SC/bounce 能量不足 | **证实——当前第一嫌疑** | HZB/SC 命中共用亮度惩罚 `1/(1+2(lum−1))`，太阳白地面 L_o≈10-50 被压到 ≤1；叠加查询产出 found 24.7%、tile 天空基色残留旧梯度（差 ~2×） |
| ③ | π 失配 | **排除**（维持 [深查] §4 结论） | 全链审计：无 albedo 双乘、π 成对、L→E 恰一次；F6 复验 dc_gain 0.96<1 有界 |
| ④ | visibility 过杀 | **字面不成立，精神成立但机制不同** | B9 已删全栈 `indirect×ao`；Chebyshev 只在 DDGI 腿。真正的"过杀"是**单点亮度惩罚**——对亮面远比 V1V2V3V4≈0.31 狠（lum=10→×0.053） |
| ⑤ | SPGI sampling 非瓶颈 | **证实** | P0-P4 ROI 变化量级小（meanY 65.3→62.0、floor +25%）；分布优化改的是稳定性/漏光，不创造缺失的 radiance |

**双通路结论（R2 合成）**：用户"两条通路都没把中庭托起来"的判读与代码实况一致——
**天空通路**形态正确（逐射线方向采样，无加性常量反模式，§2.6），但到达份额被"小天空立体角 ×
DDGI-first 半档 × adapt 吸收均值"三层限制；**表面反弹通路**被亮度惩罚 + 查询产出率钳死（§3）。
目标形态 `L_i = L_sky(ω)·V(x,ω)·cosω（低频 fill）+ L_surface（结构/色弹）`中，前者要提的是
**到达份额**（不是再抬档），后者要提的是**根本没被送达**——修复动作因此分双轨（§6）。

**方法学前置核实（§1）**：三张对比图（9-21 01:46-01:49 拍摄）经时线与交叉证据确认**全部为 UBO 修复后二进制**、
**SC 已注册**（R2.3 门禁修复后 frame 360 有效）——"三图中庭几乎不变"是同态真实现象，不是二进制混淆。

**对用户实验清单的关键修正**：实验 1（miss=白）在本引擎的等价实验（抬 sky 档 0.15→0.25→0.4）**已经做过**——
[GAP] §5 P2 实测 adapt 开启下档位差异被适应曝光吸收（0.4 vs 0.25 逐像素差 0.3/255，R1≈R2）。
全局抬天空预期抬不动中庭观感；中庭缺的是**空间结构化**的"太阳→地面→墙"反弹分量，即 ②。

---

## 1. 对比图的二进制状态核实（先排混淆）

三张图：`Docs/completed/render-comparisons/2026-09-21-spgi-probe-distribution/`（01:46-01:49）。

| 事实 | 证据 | 结论 |
|---|---|---|
| `ScreenProbeTraceFinalize.comp` 的 UBO 对齐修复（sky 门黑→活）保存于 9-21 **01:34** | 文件 mtime；[分布-完工] §1.6 | 三图拍摄（01:46+）晚于修复 |
| 基线跑与回滚跑差异 mean **2.3/255**，小于同二进制重跑噪声 **15.1/255** | [分布-完工] §4.1 | 若基线缺 UBO 修复（天空黑→亮），差异必然远超噪声底——**基线二进制同样含修复** |
| content-ready 门禁经 R2.3 修复（退化 mesh 显式降级、不再阻塞） | `StandardRenderPipeline.cpp:1740-1746`；超时 600 仅兜底（`:1767-1770`） | [GAP] §4 "frame360 落在 SC 注册前" 的协议变更前提已被 R2.3 消解；frame 360 时 SC 已注册（warmup 90 帧冻结窗，`GIWarmup.h:23-29`） |

**判定**：三图同态（天空门活、WRC header 活、F1-a 活、SC 卡在役）。P0-P4 的"几乎不变"可作为 ⑤ 的证据使用。
但注意反面推论：**9-20 全天（gap-actions、[不对称-F] 的 before 组）的所有截图都出自天空门全黑的二进制**——
若用户的中庭偏暗印象部分来自那批图，其中 sky-miss=0 的成分已在 9-21 修复，A/B 结论须在新二进制上复拍。

---

## 2. 假设① sky miss——历史态、现状、残余

**现状（修复后）**：SPGI 射线归属四分支的兜底链（`ScreenProbeTraceFinalize.comp`）：

```
HZB hit(:326) → SC hit(:344) → far-cache: WRC(:433)→RC(:442)→DDGI SH(:472)→sky(:531-541)
miss(:567)  → DDGI 8-probe trilinear(:573-529) → sky(:634-639)
```

天空项 = `EvaluateSkyBase(rayDir) × sky_params.xyz`（:540/:638），`sky_params` = `GIDebug().sky_color_intensity`
（默认 **2.0**，`GIDebugParams.h:43`；env `PRIMAL_GI_SKY_INTENSITY`，0-4 钳制；CPU 注入 `ScreenProbeGIPass.cpp:2018-2020`）
→ **每射线 8.4-12**，不是 0。可见天空/DDGI miss/WRC 逃逸（`WRCTypes.h:82-99`）同源同档。

**历史态（为什么用户怀疑到这里是对的方向）**：9-21 01:34 前，shader 侧 `sky_params` 因缺 `hint_params`
声明实际读到 {0,0,0}——**天空回退全黑**（[分布-完工] §1.6 自证），同时 F1-a 限幅恒 0、WRC header 错位。
即 9-19→9-20 两天所有验证跑（含 [不对称-F] 的 A/B 双方）里，**中庭探针看到的天空 ≈ 0**。

**残余问题（修复后仍在）**：
- **a. 纯 miss 的 DDGI-first 顺序**：射线打不到任何几何时先评 DDGI SH（:573），天空只在 DDGI 无效/出体积时直取。
  DDGI 自身逃逸天空带 **×0.5 半档**（`LumenDDGIPass.cpp:2019-2024`，0.15 时代压"室内平填充"的产物）——
  中庭探针的天空能量被 DDGI 内容中介后可能只有直取的一半。
- **b. 下行射线黑**：`skyWeight = dot(probeNormal, rayDir) ≤ 0` 时天空门关，F1-a 兜底要求 cascade 有效
  （:554-557），cascade 无效/出体积仍是黑。中庭拱底向下射线有此暴露面。
- **c. 全局抬档无效化（方法学教训，最重要）**：[GAP] §5 P2 阶梯实测——adapt 默认开（key 0.55）下
  sky 档 0.25 与 0.4 的显示差仅 0.3/255。**"把 miss 抬白"作为全局能量操作会被适应曝光吸收**；
  实验判读必须走 fixed-exposure ROI（`scripts/gi_roi_stats.py`）+ `GI_LADDER_DUMP` 分腿，不能看成片观感。

### 2.6 R2 追加：双通路框架与反模式核查（"会不会一抬就平"）

**目标形态**（用户 R2 合成）：`L_i(x) = L_sky(ω)·V(x,ω)·(n·ω) dω + L_surface`——天空负责大尺度
低频 fill，表面反弹负责局部空间结构与 color bleeding；两者叠加才接近 Lumen 参照。担心的是这类
反模式把间接光抹平：

```cpp
probeIrradiance += skyColor * skyIntensity;   // 无方向、无遮挡、无 cosine —— 会平
```

**核查结论：本引擎不存在该反模式，天空通路形态已正确**。全引擎天空消费点逐点对过（无一处加性常量）：

| 消费点 | 形态 |
|---|---|
| SPGI finalize 天空门 | `EvaluateSkyBase(rayDir) × sky_params.xyz`（`ScreenProbeTraceFinalize.comp:540,559,638,653`）——**仅 miss 才取**，V(x,ω) 由"射线是否 miss"隐式承担，cosθ 逐射线累计 |
| WRC finalize 逃逸 | `EvaluateSkyBase(rayDir) × SkyColor.xyz × tier`（`WRCTraceFinalize.comp:283,297`）逐射线 |
| DDGI finalize 逃逸 | 同上（`DDGITraceFinalize.comp:527`）逐射线 |
| ReflSpecularTrace | 同上（`ReflSpecularTrace.comp:361`）逐射线 |

历史上的两类加性 floor 均已删除：SC texel 侧 ambient floor（B14，`SurfaceCacheLightEval.comp:557-569`
注释保留恢复配方）、Fusion 侧 far-field floor（B12，`FusionIndirect.frag` "No constant ambient floor"）。

**真正的缺口不是"形态"而是"显式 V(x,ω)"**：天空可见性当前完全由射线 miss 隐式承担，但被两处折叠——
① 纯 miss 先走 DDGI SH（§2-a，DDGI 自身逃逸天空 ×0.5 半档）；② **点级显式天空可见性分类
（[根治] Phase 3"核心根修"）仍待实施**——那正是用户公式里 V(x,ω) 的显式形态，也是"中庭中心高 /
拱顶下低 / 深室内≈0"空间差异化的正解来源。V 的空间变化当前只能靠射线密度与 gather 插值间接呈现。

**通路现状判读（与用户 R2 判读对齐）**：
- **sky 通路**：形态对、份额低——白地面/墙面探针的半球大多被几何占据（深井），天空立体角份额小；
  仅有的天空射线还要过 DDGI-first 折叠与 adapt 均值吸收。**要提的是到达份额，不是档位**。
- **bounce 通路**：根本没被送达——惩罚钳制（§3.1）+ 查询产出（§3.2）。"中庭地面白到 clipping
  而邻墙不亮"即此通路失效的直接观测。

---

## 3. 假设② SC/bounce——三重饥饿（当前第一嫌疑）

中庭最强的合法反弹源是太阳直射的白地面（L_o 10-50 级）。它进入探针场的路上有三道衰减：

### 3.1 亮度惩罚压强源（最大项）

`ScreenProbeTraceFinalize.comp` HZB 分支（:336-341）与 SC 分支（:413-416，[不对称-F] F2 起**默认同罚**）：

```glsl
radiance *= 1.0 / (1.0 + max(lum - 1.0, 0.0) * 2.0);
```

| 命中面 L_o | 惩罚因子 | 出值 |
|---|---|---|
| 1 | 1.0 | 1.0 |
| 5 | 0.20 | 1.0 |
| 10 | 0.053 | 0.53 |
| 47.6 | 0.011 | 0.52（F2 实测 max 47.6→0.98） |

**太阳白地面对中庭墙的贡献被结构性钳在 ~0.5 量级**，与它物理上应有的 10-50 差 20-50×。
惩罚的存在根因：SC lighting atlas 把直射烘进 L_o（`SurfaceCacheLightEval.comp:584-590`：
`(direct_light + E_indirect)·albedo/π + tile + emissive`），探针直读会双计直射（"yellow arch" 历史案例，
:330-335 注释）——**用亮度钳制冒充 direct/indirect 分离**。Lumen 的正解是 direct 与 indirect 分 atlas、
探针只读 indirect，惩罚即无必要。

### 3.2 查询产出率（已在案，残余继续）

[GAP] §2.1：诚实漏斗 found **24.7%**、hitLum 0.434（vs 逃逸天空 0.756）；boundsFail **50.6%** 为真实卡片
覆盖缺口（未卡化几何/掠射带），继续 capture/query 专项。SPGI 的 SC 分支（`scQueryCardCandidate` + face gate，
:364-399）共享同一捕获数据、另有自身拒绝——**SPGI 侧没有对应的分级拒绝计数器**（WRC 侧有
`PRIMAL_WRC_TRACE_STATS`），"惩罚压制"与"查询拒绝"当前无法在 SPGI 腿上区分（§5 实验 6）。

### 3.3 tile 天空基色残留（A1 的半程修复）

F0 把 tile 反弹路径的档位接到了 `sky_color_intensity`，但基色仍是旧梯度 `(2.75,2.88,3.00)`
（[不对称-F] F0 行）→ 2.0 档下 **5.5-6.0**，而探针直取天空 `EvaluateSkyBase(4.4-6.0)×2.0 = 8.4-12`
（`SkyAtmosphere.glsl:21-25`）——**tile 反弹的天空源仍比探针天空暗 ~2×**（A1 从 ~8× 收窄到 ~2×）。

### 3.4 用户观察的对应

"地面亮到接近白、旁边阴影墙很暗"——正是 3.1+3.2 的预期表现：地面→墙的射线要么被惩罚钳死（亮命中），
要么查询拒绝（boundsFail 带）；墙能拿到的只有天空条带方向（①的现状值）与对面阴影墙（本来就暗）。
[不对称-F] 的 A/B（中庭地面 +11%、target 融合间接 +26%）是"把惩罚关掉"方向的初步正信号，
但 F2 的默认态把 SC 腿重新压平了（meanLum 1.212→0.182）。

---

## 4. 假设③④——排除与机制修正

**③ π 失配：排除**（不重开）。[深查] §4 全链逐 shader 审计：无 albedo 双乘（tile 支路出口恰乘一次、
屏幕支路只在 Fusion 乘一次）、π 成对（DDGI 腿 E/π→×π 与 SPGI 腿 A0=π 数学等价）、L/E 不混用；
F6 复验 `GI_DCGAIN` dc_gain 0.96<1 有界。若链上真有 1/π² 失配，深室内（纯 DDGI/SC 供给）会整体差
3.14×——与"深室内控制得比较合理"的观察矛盾，反证排除。

**④ visibility 堆叠：字面不成立**。B9 已删全栈 `indirect *= ao`（`FusionIndirect.frag:115-118`，
GTAO 与探针可见性双遮蔽）；Chebyshev（下限 0.38 + normal 权 0.40）只在 DDGI gather 腿
（`DDGIGIGather.comp:229-234`）；confidence 只在 mode 0 消费。**精神成立但真实机制是 3.1 的单点亮度惩罚**：
它亮度依赖、无上限，对亮面的过杀（×0.01-0.05）比用户设想的 V1V2V3V4≈0.31 狠一个量级以上，
且分布不均匀（专门杀"亮源邻近区"）——恰好命中中庭。

---

## 5. 用户五实验 → 现成旋钮映射（增量标注）

| # | 用户实验 | 现成手段 | 判读要点与预期 |
|---|---|---|---|
| 1 | miss 强制白 | `PRIMAL_GI_SKY_INTENSITY=4`（env 上限 4；~20/射线）；严格 miss=白需小改 shader（无现成开关） | **预期中庭观感不动**（adapt 吸收，[GAP] P2 已证）。看 fixed-exposure ROI 与 GI_LADDER sky 项；若要模拟"只有 miss 变"须同时锁直射，否则直射区同步过曝 |
| 2 | visibility=1 | `PRIMAL_SPGI_SC_NOPENALTY=1`（SC 腿）+ `SPGI_TRACE_DIAG=2`（HZB 腿去惩罚对照）+ `PRIMAL_SPGI_FACE_GATE=0` + `PRIMAL_SPGI_ADAPTIVE_NORMAL=0` | 预期中庭亮面邻近区显著抬升（F2 对照数据即此态：SC 分支 meanLum 0.182→1.212）。**这是五实验里信息量最大的一个，且零代码** |
| 3 | 只看 indirect | `PRIMAL_PASS_DEBUG=spgi_raw`（F3-V2，`PRIMAL_PASS_DEBUG_GI_SCALE` 默认 0.05）或 `spgi`；数值级 `GI_LADDER_DUMP=1`（列定义 `FusionCompositeModule.cpp:307-327`） | 已有；raw 视图已带曝光修正，黑洞=gather 无覆盖 |
| 4 | ray source 分类 | `SPGI_TRACE_DIAG=1`（branch 0-3 × 屏幕 4×4 桶：count/avgLum/maxLum/cosΘ，读回 `TestVulkanSponzaRenderGraph.cpp:2096-2180`） | 数字分桶已有；逐像素颜色可视化**没有**（小时级小改，可在诊断需要时补） |
| 5 | 单像素能量预算 | `GI_LADDER_DUMP=1` CSV（direct/sky/SC/DDGI 分腿台账） | 已有，即用户要的预算表 |
| **6** | （增量建议）SPGI SC 分支产出率计数器 | 无——需把 WRC_SC_FUNNEL 的 boundsFail/orientFail/depthFail 分级移植到 finalize SC 分支 | 小改；区分 3.1 惩罚压制与 3.2 查询拒绝，是 S2 决策的前提仪表 |

注意实验 2 的语义边界：关惩罚后直射泵入会回来（F2 当初治的"直射经 SC/HZB 灌探针"是真的）——
所以实验 2 的正确用法是**量化上限**（"中庭能亮到哪"），而不是直接翻默认；翻默认须等 S2 的 direct/indirect 分离。

### 5.5 Debug A/B/C 分通路隔离实验（R2 采纳，diag mode 4/5 提案）

用户 R2 提案的落地设计——把"天空 fill"与"表面传输"拆开各看一眼，比抬 miss 亮度更有区分度：

| 实验 | 语义（对 finalize 四分支） | 实现 | 判读 |
|---|---|---|---|
| **A = fill-only** | `hit=0`：HZB/SC 分支（:326/:344）→ 0；保留 far-cache/miss 天空链 | `SPGI_TRACE_DIAG=4` | 中庭整体抬起但"柔和平"= 正常 sky fill 形态；若**抬不动**（预测：受小天空立体角 + DDGI 半档 + adapt 三重限制）→ §2 残余 a/c 坐实，sky 轨按"提份额"修 |
| **B = transport-only** | `fill=0`：far-cache/miss 分支（:420/:567）→ 0；保留 HZB/SC | `SPGI_TRACE_DIAG=5` | 白地板邻墙变亮 / 柱背局部反弹 / 拱顶梯度 = 结构性 GI 存在；预测当前二进制下**很弱**（惩罚+产出钳死）→ ②坐实、S2 立项的直接依据 |
| **B′ = B + 摘惩罚** | B 且 `PRIMAL_SPGI_SC_NOPENALTY=1` + `SPGI_TRACE_DIAG=2` | 零代码（mode 5 与 2 组合） | 传输**上限**；B′−B 差值 = 惩罚的钳制量，给 S2 的收益预期定标 |
| **C = 基线** | 默认（A/B 均不启用） | 无 | 对照 |

**实现位（小改，~15 行）**：`diag_params.x` 现用模式 0-3（CPU 填充 `ScreenProbeGIPass.cpp:2011`，
shader 消费 `:238,:339`），**模式 4/5 空闲**；env `SPGI_TRACE_DIAG` 解析为 int 直通
（`TestVulkanSponzaRenderGraph.cpp:1916`），无需新增旋钮。shader 侧在分支出口加语义化门
（mode 4 下 branch 0/1 的 radiance 置零、mode 5 下 branch 2/3 置零），**归因统计照记**
——同跑即可读回"fill/transport 各自贡献份额"，一次跑回答两个问题。

**验收语义**：A 抬轮廓（低频）、B 给结构（色弹/梯度）、两者叠加才接近 Lumen——这正是 §6 双轨
修复的验收镜像：sky 轨用 A 的幅度验收"份额"，bounce 轨用 B/B′ 的结构与幅度验收"送达"。

---

## 6. 修复建议分级（R2 改双轨：两通路分治，先仪表后根治）

> 执行序：**第 0 步决策仪表 → bounce 轨（送达）→ sky 轨（份额）**。bounce 轨排前，因为惩罚钳制是
> 已证实的结构性钳制（B 预测已弱），无论仪表读数如何都要做；sky 轨的**幅度**则依 Debug A 读数定
> ——A 若抬不动，说明问题在到达份额（半档/立体角/显式 V）而非档位，恰好避免"抬档又怕平"的反复。

**第 0 步：决策仪表（一次跑定双轨幅度）**
- **I-1** Debug A/B/C 分通路隔离（diag mode 4/5，§5.5）——fill/transport 各自份额 + 结构判读。
- **I-2** SPGI SC 分支产出率分级计数器（§5 实验 6，移植 WRC_SC_FUNNEL 的 boundsFail/orientFail/depthFail
  分解）——把"惩罚压制 vs 查询拒绝"定量化。

**Bounce 轨（提"送达"——主根治）**
- **B-1** SC direct/indirect 分离：LightEval 直射项另存（或查询侧按直射掩码剔除），探针只读 indirect；
  HZB/SC 亮度惩罚退役为纯 firefly 软限幅（lum>8 才起效）。这是把"太阳→地面→墙"合法反弹放回来的正解，
  与 [根治] Phase 2 合流；直射泵入由分离天然消除，不再依赖亮度钳制。用户预期结果（"白直射不变、
  中庭显著抬升、深室小幅变化"）主要由本项兑现；惩罚翻转永久化不是替代——会连直射泵入一起放回。
- **B-2** SC 查询产出残余：boundsFail 50.6%（未卡化几何/掠射带）——[GAP] §2.1 专项继续。

**Sky 轨（提"到达份额"，不做全局抬档）**
- **K-1** F0-b：tile 天空基色 `(2.75,2.88,3.00)` → `EvaluateSkyBase` CPU 镜像（消 §3.3 的 ~2× 残差）。
- **K-2** DDGI 逃逸 ×0.5 半档复核（§2 残余 a）：0.15 时代产物，现 2.0 档 + bootstrap 切断 + WRC 注入
  门控下是否仍需——A/B 后定（回滚位即恢复 ×0.5）。纯 miss 天空被 DDGI-first 折叠的最大单项。
- **K-3** 下行射线 cascade 无效兜底（§2 残余 b）：至少计"失败且无兜底"计数器，确认黑区面积。
- **K-4（远期正解）**：点级显式天空可见性分类（[根治] Phase 3"核心根修"）——用户公式 V(x,ω) 的显式
  形态，"中庭中心高 / 拱顶下低 / 深室内≈0"的空间差异化由此而来；射线 miss 隐式承担在探针稀疏处失真。

**S3 显示链（独立于 GI，已部分在案）**
- bloom 维持 opt-in（9-17 用户反馈增益 4.0 洗白受光区）；adapt key 0.55 已调。中庭抬升后如需"空气感"
  再评估，不与能量修复混变量。

**明确不做（与用户判断一致）**
- 继续调 ray count/tier/normal 阈值/lightVar（⑤ 已证非瓶颈）；
- 全局 indirect multiplier / 全局抬天空（室内复曝 + adapt 吸收，两头不讨好）；
- `probeIrradiance += skyColor × intensity` 型加性常量天空（§2.6 反模式）——历史上删掉的
  SC texel floor / Fusion floor（B14/B12）不得以"提亮中庭"为由恢复。

---

## 7. 证据索引

- 惩罚两处 + 天空两处 + F1-a + DDGI fallback：`Engine/Graphics/Vulkan/Shaders/Lumen/ScreenProbeTraceFinalize.comp:336-341,413-416,531-564,567-660`
- UBO 错位修复自证：同文件 :66-80 注释；[分布-完工] §1.6
- sky 档单一事实源：`Engine/Graphics/Lumen/GIDebugParams.h:42-93`（默认 2.0，env 0-4）、`ScreenProbeGIPass.cpp:2013-2020`
- 天空基色：`Engine/Graphics/Vulkan/Shaders/Lumen/ShaderLib/SkyAtmosphere.glsl:21-25`（horizon 4.4/zenith 6.0）
- SC L_o 含直射 + ambient floor 历史注释：`SurfaceCacheLightEval.comp:555-590`
- tile 天空 F0 现状：[不对称-F] F0 行（CPU 填 `intensity × (2.75,2.88,3.00)`）
- DDGI 逃逸半档：`LumenDDGIPass.cpp:2019-2024`（[深查] §5-A1 交叉引用）
- WRC 天空契约档：`Engine/Graphics/Lumen/WRCTypes.h:82-99`
- 查询漏斗：[GAP] §2.1（found 24.7% / boundsFail 50.6% / hitLum 0.434）
- adapt 吸收实测：[GAP] §5 P2（0.25 饱和、R1≈R2）
- 三图时线：`git`/mtime（finalize .comp 9-21 01:34 vs 截图 01:46-49）；门禁 R2.3：`StandardRenderPipeline.cpp:1725-1790`
- F2 惩罚 A/B 数据：[不对称-F] §A/B（SC meanLum 1.212↔0.182、max 47.6→0.98）
- Fusion 合成与 B9 去 ao：`Engine/Graphics/Vulkan/Shaders/Lumen/FusionIndirect.frag:85-120`
- 天空消费点全表（§2.6 反模式核查）：`ScreenProbeTraceFinalize.comp:540,559,638,653`、`WRCTraceFinalize.comp:283,297`、`DDGITraceFinalize.comp:527`、`ReflSpecularTrace.comp:361`——全部逐射线 `EvaluateSkyBase(rayDir)`，无加性常量；旧 floor 删除证据 B14（`SurfaceCacheLightEval.comp:557-569` 注释）/B12（FusionIndirect "No constant ambient floor"）
- diag mode 4/5 落点位：CPU `ScreenProbeGIPass.cpp:2011`（`diag_params = {trace_diag_mode_, …}`）、env 解析 `TestVulkanSponzaRenderGraph.cpp:1916`、shader 消费 `ScreenProbeTraceFinalize.comp:238,339`
