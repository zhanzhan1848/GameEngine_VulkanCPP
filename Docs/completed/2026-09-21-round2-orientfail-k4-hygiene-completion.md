# [GI] [SC] [SPGI] [DDGI] 第二轮：orientFail 治理 + K-4 切片 + radiance 卫生 + moments 视图——完工记录

> 标签：**[GI] [SC] [SPGI] [DDGI] [WRC]** ｜ 日期：2026-09-21 ｜ 状态：✅ 四项落地 + ⚠️ 发现并定位预存 tile 回路双稳态（未消根治，见 §5）
> 顺序执行：①SPGI orientFail 治理 → ②K-4 显式 V(x,ω) 切片 → ③radiance 卫生 → ④DDGI moments 探针球。
> 上轮：`completed/2026-09-21-probe-repr-e0-energy-dual-track-completion.md`。

---

## 0. TL;DR

1. **①orientFail**：漏斗扩到 16 项（候选级 + **射线级**分类），G2 hint 门确认为过杀主源
   （关掉它 found 1.87%→5.69% 三倍，但拱顶借光回归）。实现**软键惩罚**（错位卡 key 加罚而非
   一票否决）——`PRIMAL_SPGI_HINT_SOFT` 开关，**默认关**（旧硬语义）：软罚的 3× 能量流与
   tile 回路双稳态边界相互作用，单变量重开验证后再定默认。
2. **②K-4 切片**（根治 Phase 3 条目 4）：SPGI per-probe openSky 统计（`GetProbeOpenSkyBuffer`）
   + 纯天空探针 confidence 地板参数化（`PRIMAL_GI_SKY_CONF_FLOOR`，默认 0.35=旧值 no-op；
   目标值 0.18 待与稳定性一起重开）。
3. **③radiance 卫生**：finalize 全部远场腿（WRC/级联/DDGI-miss/F1-a）+ WRC 写出/EMA 端
   NaN/Inf 消毒与 64 上限；EyeAdaptation 亮度采样 NaN 防御。**注意**：适应防御不是坍黑根因
  （打上后坍黑依旧），但作为系统性防御保留。
4. **④moments 接探针球**：`ddgi_depth_buffer` 注入 + ProbeSphere.frag mode 2/3 读八面体
   mean/var 渲染热图——confidence（1/(1+var)）/variance（var/mean²）双视图工作
  （收敛静态场景 confidence 全绿=方差≈0，符合数据实况）。
5. **⚠️ 最重要的发现**：间歇性 780 帧坍黑（~15-20%）**根因定位到 SC tile radiosity 回路
   双稳态**——`SC_INDIRECT=0` 时 6/6 全稳，默认配置 ~1/5 坍黑，坍黑态逐位可复现
  （meanY 9.3，第二稳态）。frame 700 时间线显示封闭龛局部泵浦（alcove 86.8）→ 全局坍缩。
   已发增长率钳制（`PRIMAL_SC_TILE_GROWTH_CAP`，默认 0.5，延缓但未根治）；**根治 = 回路
   增益分析（[根治] Phase 2 范畴），下一会话最高优先级**。

---

## 1. ① SPGI orientFail 治理

### 1.1 仪表升级（`fail_stats` 8→16 项）

- `scQueryCardCandidateDbg` 拒绝码拆分：2 = orient-朝向（face×ray），**7 = orient-hint（G2）**
  （WRC 侧计数同步并入 orient 口径）；
- **射线级分类**新增：raysFound / raysAllRej{Orient,Bounds,Depth} / raysZeroCand——区分
  "候选级拒绝率"（受卡密度影响）与"射线级失败率"（真正决定 transport 送达）。

### 1.2 FACE_GATE 扫参（frame 360，累计 frame 0-350）

| FACE_GATE | found(候选级) | orient 合计 | depthFail |
|---|---|---|---|
| **-1（hint 门关）** | **5.69%** | 30.5% | 16.2% |
| 0.0 | 2.51% | 44.4% | 7.3% |
| **0.25（默认）** | 1.87% | 47.9% | 4.9% |
| 0.5 | 1.49% | 49.3% | 4.0% |

结论：**G2 hint 门贡献 orientFail 的 ~17.4 个百分点**（相对过杀 3 倍）。关掉后 arch 回亮
（71.0 vs 55.4）——全局关闭会复活 G2 当初治的"拱顶借光"。

### 1.3 软键惩罚（`PRIMAL_SPGI_HINT_SOFT`，默认关）

错位候选不再一票否决：key 加罚 `max(d+1,0)×(perp+|along|+tol)`——有对齐替代卡时错位卡必输
（拱腹/受光外墙判别保留），无替代时仍被接受（横幅/薄几何唯一候选不再被噪声 hint 杀死）。
软罚实测：found **5.70%**（与全关等价）+ arch 67.4（介于硬门 55.4 与全关 71.0——保留部分判别）。
射线级：foundRate 36.2%，allRejOrient 56.0%（残余=薄片墙对侧卡的正确拒绝 + 未卡化面，
归 B-2 卡片覆盖专项）。

**默认关闭原因**：软罚验证期间撞上 §5 的间歇坍黑（能量流 3× 改变双稳态触发概率），单变量
归因未完成；开关保留、复开验证流程已写明（§5.3）。

---

## 2. ② K-4 显式 V(x,ω) 切片（根治 Phase 3 条目 4）

- **per-probe openSky 统计**：Average 归约 miss/hit 占比写 `probe_opensky_buffer_`
  （vec4/probe：x=miss 占比=天空/回退，y=命中占比；Vulkan 常配 host-visible）；
  `GetProbeOpenSkyBuffer()` 暴露。探针场"哪里看得见天"从隐式（SH 内容）提升为显式统计；
- **纯天空探针 confidence 地板参数化**：`mix(0.35→skyConfFloor, 1.0, hitRatio)`，
  env `PRIMAL_GI_SKY_CONF_FLOOR`（默认 **0.35 = 旧值 no-op**；目标 0.18 待重开验证）。
  Gather 以 sh0w 为混合权重——降档即"天空 fill 只在确实看得见天的邻域起主导"；
- AvgCB 32→48B（新增 skyConfFloor）；Average 布局 +binding 7。

---

## 3. ③ radiance 卫生

| 位置 | 防御 |
|---|---|
| finalize `wrcSampleRadiance` | NaN/Inf→0 + `min(64)`（对齐 WRC 混合钳制；far-cache maxLum 3390 溯源） |
| finalize 级联快速路径 / DDGI-miss 三线性 / F1-a 级联兜底 | 同上消毒 |
| `WRCTraceFinalize` 写 `wrc_rays` | 写出前消毒+钳制（源头掐断，EMA mix 与 half 打包不再扩散） |
| `WRCUpdateRadiance` EMA | NaN rad→0 且 dist=-1（不进 Chebyshev 统计） |
| `EyeAdaptation` 亮度采样 | NaN/Inf→0 + avgLogLum 域宽校验（防单帧毒化适应态） |

**修正一个假设**：适应 NaN 防御打上后坍黑依旧（8 连 2 坍）——适应不是坍黑根因，防御作为
卫生保留。NaN 的上游源（3390 量级）仍未逐一定位，随 §5 回路根治一并查。

---

## 4. ④ DDGI moments 接探针球 confidence/variance

- `DebugProbeSphereInputs.ddgi_depth_buffer` 注入（两处注入点；上一完成帧槽 (cbIdx+2)%3）；
- `ProbeSphere.frag` mode 2/3 读八面体 moments（8×8 mean[0..63]+var[64..127]，与
  DDGIUpdateDepth 同式编码）：confidence=1/(1+var) 红→绿；variance=var/mean² 蓝→黄；
- CPU 模式门重构：mode 2/3 有 moments → hue 管线（frag 内解释 binding 1=float[]）；
  无 moments 回退 radiance+一次性日志。
- 截图：`render-comparisons/2026-09-21-round2-orientfail-k4-hygiene/frame360_spheres_{confidence,variance}.png`
  （收敛静态场景 confidence 全绿=方差≈0——数据如实；更判别的度量可按射线数/年龄归一，登记 polish）。

---

## 5. ⚠️ 间歇性 780 帧坍黑——根因定位与现状（下一会话 P0）

### 5.1 现象与定位

- 默认配置 frame 780：~15-20% 概率坍黑（meanY 65.4 → 9.3，直射过曝区也塌），frame ≤550 从未复现；
- **坍黑态逐位可复现**（9.3/0.0/5.8/10.2/38.8/18.5 每次一致）= 确定性第二稳态（双稳态暗支），
  触发随机（GPU 时序敏感）；
- frame 700 时间线：封闭龛局部能量泵浦（alcove 86.8、arch 0.0，层次翻转）→ 780 全局坍缩；
- **`SC_INDIRECT=0`（关 tile radiosity）→ 6/6 全稳**——振荡器 = SC↔tile↔prev 反馈回路；
- 排除：adaptation NaN（防御打上仍坍）、②confidence 地板（0.35 no-op 仍坍）、曝光配置
  （engine_reported 恒定）。

### 5.2 已发缓解

`PRIMAL_SC_TILE_GROWTH_CAP`（默认 0.5）：tile 每帧新增幅度 ≤ cap×prev+ε——限速率不保证
增益<1，实测坍黑率未清零（1/6）。**已验证的稳定配置：`SC_INDIRECT=0`**（代价：floor 12.7 vs 19.1，
tile 贡献的地面反弹损失）。

### 5.3 重开验证流程（①软罚与②地板共用）

1. 先解 §5 回路（增益分析：tile 权重 w、ρ/π 反馈、enclosed 局部增益——[根治] Phase 2）；
2. 单变量：`PRIMAL_SPGI_HINT_SOFT=1` 780×6 全稳 → 翻默认；
3. 单变量：`PRIMAL_GI_SKY_CONF_FLOOR=0.18` 780×6 全稳 → 翻默认；
4. 任一坍黑 → 回退该变量并入档。

### 5.4 证据索引

- 稳定对照：`render-comparisons/2026-09-21-round2-orientfail-k4-hygiene/`
  （frame780_stable / collapsed_sample / sc_indirect_off_stable / facegate 两组 / spheres 双图）；
- 扫参/漏斗数据：§1.2 表 + `SPGI_SC_FUNNEL` 16 项输出（frame 350）；
- 双稳态史：memory `sc-probe-bistability`（gi_frame 闩锁修的是暖启动竞态，本发现的暗支
  是**收敛后**的第二稳态，机制不同）。

---

## 6. 文件触点（本轮增量）

`SurfaceCacheData.glsl`（码 7 + 软键）、`ScreenProbeTraceFinalize.comp`（16 项漏斗 + hintSoft 门
+ ③消毒 + SoA）、`WRCTraceFinalize/WRCUpdateRadiance.comp`（码 7 同步 + ③消毒）、
`ScreenProbeAverage.comp`（②opensky + confidence 地板 + 48B UBO）、`EyeAdaptation.comp`（③防御）、
`SurfaceCacheIndirectResolve.comp`（稳定性钳制 + K-1 字段）、`SurfaceCacheTypes.h`（128B 结构）、
`SurfaceCachePass.cpp`（cap 填充 + 缓冲 128B）、`ScreenProbeGIPass.{h,cpp}`（16 项缓冲 + opensky
+ hintSoft + AvgCB）、`DebugProbeSphereModule.{h,cpp}`（④moments + 模式门重构）、
`StandardRenderPipeline.cpp`（④两处注入）、`ProbeSphere.frag`（④热图）、
`TestVulkanSponzaRenderGraph.cpp`（16 项漏斗打印 + 射线级行 + IRR_DUMP）。

## 7. 回归状态

- Unit 5/5；Vulkan 集成 60s 无新增 validation（20 条=预存 ORM 警告）；
- frame 780 稳定样本与坍黑样本均已归档（§5.4）。
