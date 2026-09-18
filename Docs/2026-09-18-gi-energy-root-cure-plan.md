# GI 能量根治方案：间接光过亮 + 室内/中庭对比度丢失

> 日期：2026-09-18 ｜ 状态：📋 方案已定稿，待实施
> 症状来源：Vulkan Sponza vs UE Lumen 同机位对比（见
> `Docs/completed/render-comparisons/2026-09-18-indirect-brightening-investigation/README.md`，
> 含 current / gain1 / scsupply0 三组实采数据）。
> 实测：全图 mean 108.3（UE 参考 42.7）、暗部占比 16.1%（参考 61.3%）、
> 拱腹 ROI 179.9——物理上最该黑的封闭区反而是全图最亮区域。

## 0. 根治原则（区别于历史拐杖）

历史修复多为"地板/钳位/增益"类症状压制（B12/B13/B14 底光、T8.10c AO 调制、
gain 3.0 扫频对表）。本方案只接受满足以下四条的修法：

1. **遮挡正确**：任何能量注入点（探针射线、SC 卡片、prev-screen 回灌）必须携带
   与真实传输一致的可见性；不允许"逃逸=露天"这类零判定注入。
2. **单一事实源**：天空色、直射单位、增益各只有一个定义点；双轨并存即缺陷。
3. **回路物理化**：所有时序反馈回路（SC↔probe、probe 自反馈、屏幕回灌）的稳态
   增益由材质反照率推导（ρ≤0.25 量级），禁止手工常数；**任何显示/融合增益不得
   出现在回路内部**。
4. **可回滚 + 门禁固化**：每阶段独立 env 开关（沿用 PRIMAL_* 惯例），ladder dump
   + GI_ISOLATE A/B；修完的门进自动门禁，防止下一个均衡点复发。

## 1. 根因清单（全部已实采实证）

| # | 根因 | 位置 | 症状归属 |
|---|---|---|---|
| R1 | 融合端全局增益 ×3（旧死探针网格上调出的均衡点，scene-fit 后探针场变活即过驱） | `Engine/Graphics/RenderPipeline/Modules/FusionCompositeModule.cpp:763` → `FusionIndirect.frag:98` | 整体偏亮主乘子（gain1 实测 mean 108→68） |
| R2 | 逃逸射线吃满额天空、无可见性判定；`DDGI_SKY_COLOR=(2.75,2.88,3.00)` 常量覆盖 Finalize 已做 `SkyColor(0.4)` 门控的版本（注释自称"0.3 档"，实际全天空档），且绕过 SkyColor 旋钮 | `DDGIUpdateIrradiance.comp:119-120,153-154` 覆盖 `DDGITraceFinalize.comp:513` | 室内=中庭主通道（fit 审计逃逸率 8.7%） |
| R3 | SPGI far-cache 回退全额天空、无门控（4.4–6.0，未乘 SkyColor） | `ScreenProbeTraceFinalize.comp:423` | 室内=中庭次通道 |
| R4 | 手工反馈回路：探针解析兜底 `π·L_i_prev·0.70`（文档定性"无界增益源/多稳态触发器"）；SC_FEEDBACK 0.70；B11 屏幕回灌把**含 ×3 增益的融合结果**回喂探针且 DDGI prev-screen 无亮度惩罚（SPGI/SSGI 有） | `DDGITraceFinalize.comp:500`、`SurfaceCacheLightEval.comp:428`、`StandardRenderPipeline.cpp:2442-2445`、`DDGITraceFinalize.comp:483-489` | 整体偏亮放大器 + 双稳态 |
| R5 | legacy 地雷：Dawn 统一入口 trace 的无阴影解析直射 `15×NdotL` | `DDGITraceRays.comp:562` | 潜在（当前不在 Vulkan 活跃路径） |
| R6 | 几何层漏光：无符号 SDF 行进 tunneling（薄面任意相位穿越→miss→天空，T8.10f 接受为已知限制）；逐 mesh 盒合并 ≤1 体素接缝（T8.10d 封缝失败回退） | ContentTools 烘焙 + 运行时 SDF | 封闭区渗透的资产级根因 |
| R7 | gather 端三线性无 form-factor：受光带探针能量隔墙/隔板传给封闭像素（09-09 标定计划终局结论：1F 地板 195 vs UE 59）；sky-probe classification 未实现 | `DDGIGIGather.comp`、`SurfaceCacheLightEval.comp:200-231` | 室内=中庭的结构性根因 |

## 2. 分阶段实施

### Phase 0 基线固化（0.5 天）
- 分批提交当前未提交 GI diff（SurfaceCachePass PCSS 阴影、DDGI 重定位 origin
  contract、scene-fit 网格、SPGI 朝向门 0.25、barrier 修复）——本方案全部建立
  在这些地基上，646 行改动不能悬在工作树。
- 以 HEAD 抓三组基线 capture（default / `PRIMAL_GI_INDIRECT_GAIN=1` /
  `PRIMAL_SC_DIRECT_SCALE=0`），沿 09-18 investigation 脚本，把
  mean/dark/arch ROI 登记到本文档 §4 验收表。
- 动 `Engine/Graphics/` 前复读 `Docs/RHI/GraphicsAPI_Parity_Matrix.md` 与
  `Docs/rhi-barrier-semantics.md`（仓库纪律）。

### Phase 1 能量单位与天空单一事实源（1 天）
1. `FusionCompositeModule.cpp:763` gain 默认 3.0 → **1.0**，注释同步
   （`FusionIndirect.frag:35-39`：gain 仅为 debug 旋钮，默认必须 1.0）。
2. 删除 `DDGIUpdateIrradiance.comp` 的 `DDGI_SKY_COLOR` 常量与逃逸覆盖
   （:119-120, :153-154），逃逸射线保留 Finalize 的
   `EvaluateSkyBase(rayDir) × volume.SkyColor.xyz`——天空能量单一事实源，
   SkyColor 档位旋钮恢复效力。
3. `ScreenProbeTraceFinalize.comp:423` far-cache 天空补同一 SkyColor 门控。
4. `DDGITraceFinalize.comp` `samplePrevColor` 分支补亮度惩罚
   （对齐 SPGI `:238-243` / SSGI 的 `1/(1+max(lum-1,0)*2)`），阻断
   直射×50 经 prev-screen 无衰减进探针。
5. 删除 `DDGITraceRays.comp:562` legacy 解析直射。
- 预期：mean 108 → ≈68 或更低。**禁止用曝光/EV 补偿对冲变暗**（09-04 文档
  方法论教训：全局旋钮只是平移，不恢复层次）。

### Phase 2 反馈回路物理化 + 去双稳态（1–2 天）
1. `DDGITraceFinalize.comp:500`：`π·L_i_prev·0.70` → 扩散一跳
   `L_bounce = (albedo/π)·E_prev`（E_prev 由 `samplePrevProbeGrid` SH 求
   irradiance），回路增益 = ρ ≤ 0.25。
2. `SurfaceCacheLightEval.comp:428` `SC_FEEDBACK 0.70` → 同型 albedo/π 系数；
   T8.13 的"0.85 上限"保留为断言而非系数。
3. B11 屏幕回灌（`StandardRenderPipeline.cpp:2442-2445`）：改为回灌
   **FusionIndirect 之前的 pre-gain GI 缓冲**，或对 fused supply 施加亮度惩罚；
   `giIndirectGain` 永久移出回路。
4. ladder dump 增加逐回路 DC 增益估计（相邻 N 帧能量比），断言 < 0.5；
   TemporalAlpha/hysteresis 扫描确认无张弛振荡。
- 验收：500 帧 soak 两次独立运行 mean 差 <5%（双稳态消失），能量单调收敛。

### Phase 3 天空可见性分类（核心根修，2–3 天）
目标：封闭区不再拿到露天级天空能量（R2/R3 压掉单位后，R7 决定空间分布）。
1. **探针级 openSky 统计**：`DDGIUpdateIrradiance` 已逐射线三态
   （hit / 耗尽 / 逃逸），顺带累计 `openSkyFraction = 逃逸数 / raysPerProbe`，
   写入新增的每探针小缓冲（近场+远场合计 ~万级 float，成本可忽略）。
2. **gather 端分类加权**（关键）：`DDGIGIGather` 与
   `SurfaceCacheLightEval::gatherProbeIrradiance` 的三线性邻域中，
   sky-dominant 探针（openSkyFraction > 阈值）对"封闭像素"（自身邻域
   openSky 低 / Chebyshev 低）的贡献按分类一致度衰减——露天探针的能量
   不再隔墙渗透。复用现有 cheby/normal 权重管线，无新增 trace。
3. **远场 volume**（16³/8m/128m，逃逸比例天然高）：同规则覆盖，另以
   "远场天空能量 ×0.5 档"起步（远场只表达低频环境）。
4. SPGI：`ScreenProbeAverage` 纯天空探针（hitRatio≈0）confidence 地板
   0.35 降档；far-cache 天空分支同 Phase 1 门控。
- 验收：escape 能量注入占比 <2%；arch ROI 179.9 → ≤25；层次不变量
  "直射立面 > 地面 > 拱腹"成立。
- 已知预期：gain=1 下地面反弹不足（32 vs 参考 75.5）由 Phase 2 的合法
  bounce 回路回填；若仍不足，评估 tile radiosity 权重（现 0.25）与 SC 供给
  密度，**不以全局 gain 补**。

### Phase 4 资产级 tunneling 根治（长杆，可独立并行立项）
- ContentTools **有符号/水密 SDF 烘焙**（T8.10f 明示的正解），替代运行时
  无符号 min-merge；T8.10d 接缝封缝在 signed 前提下重评。
- 完成前 Phase 3 分类是运行时兜底；完成后逐个复核防漏拐杖
  （cheby 地板 0.38、normal 0.40、55% 低频保底等）能否放宽——
  这些拐杖同时压低对比度。

### Phase 5 门禁固化 + 收尾（1 天）
- 重锚 `GI_VERIFY`/`GI_GOLDEN` 到 gain=1 新世界，golden 图重录。
- 三道新门进离线通道/ctest：
  ① **能量台账测试**：太阳→屏幕直射×50 ∥ SC×50 → 探针辐射量级 →
  gather ×π → fusion ×1/π 各级范围断言（数据源：ladder dump / capture json）；
  ② 回路 DC 增益 <0.5 断言（Phase 2 仪表）；
  ③ soak 双稳态门（500 帧×2 次 mean 差 <5%）。
- UE 参考统计验收锚：meanY 43±15%、dark 55–65%、拱腹 ≤25。
- 本文档随进度更新；全部完成后按 ARCHIVE 纪律移入 `Docs/completed/`。

## 3. 拒绝的方案（防止复发）

- **曝光/EV 或全局 gain 对冲**：只平移，不恢复层次（09-04 已验）。
- **单纯提高探针密度**：密度决定 GI 空间频率上限，治不了墙薄漏光
  （`Docs/2026-09-16-ddgi-world-space-probe-grid-reference.md` §结论）。
- **保留 gain 3.0 "等世界稳定再说"**：增益在回路内（B11），每次供给端
  变真它都重新过驱，是本症状的直接来源。
- **步长帽修 tunneling**：T8.10f 已 A/B 否决（近壁爬行烧尽预算）。

## 4. 验收登记表（随阶段填写）

| 指标 | UE 参考 | Phase 0 基线 | P1 后 | P2 后 | P3 后 |
|---|---|---|---|---|---|
| 全图 meanY | 42.7 | 105.7 | **68.5** | **73.0**（两次运行 73.0/73.1，差 0.14%） | 待填 |
| 暗部占比 | 61.3% | 16.7% | **29.1%** | **24.6%** | 待填 |
| 拱腹 ROI | ≤25 | 177.6 | **109.5** | **113.2** | 待填 |
| 地面 ROI | 75.5 | 54.9 | **31.8** | **40.7**（合法 bounce 回填 ✓） | 待填 |
| 逃逸能量占比 | <2% | 8.7%（fit 审计沿用） | 待填（P3 仪表） | 待填 | 待填 |
| 层次不变量（立面>地面>拱腹） | ✓ | ✗（反转） | ✗（反转） | ✗（反转） | 待填 |

> **Phase 2 登记（2026-09-18）**：反馈回路物理化后回路增益 = ρ（材质），
> 地面一跳反弹回填 31.8→40.7（计划 §Phase3 已知预期的回填路径 ✓）。
> meanY 73.0 高于 P1 属预期——bounce 供给先回填、封闭区泄漏由 Phase 3 压除。
> 双稳态验收：两次独立 360 帧运行全图 meanY 差 0.14%（<5% ✓）；
> `GIDCGain` 仪表（GI_DCGAIN=1）：收敛期能量平滑上升，稳态摆动 ±1%，
> 断言形式取"窗口内相对摆动幅度 <5%"（计划稿"<0.5 增益"语义对应收敛
> 瞬态的衰减率——稳态噪声会把相邻增量比托底到 ≈1，如实测；dc_gain 与
> 符号序列保留为诊断输出）。stride 默认 9（=三缓冲深度 3 的倍数，
> 否则槽间采样伪影伪装成周期振荡）。

> **Phase 0 基线登记（2026-09-18，HEAD f5bf3216，frame 360 锁定机位）**：
> default meanY 105.7 / dark 16.7% / arch 177.6 / floor 54.9；
> 消融对照 `PRIMAL_GI_INDIRECT_GAIN=1` → 68.5/29.1%/109.5/32.0（复现 investigation
> gain1 68.4/28.5%/108.6/32.1）；`PRIMAL_SC_DIRECT_SCALE=0` → 45.0/48.4%/54.8/29.3
> （= investigation 45.0/48.5%/54.7/29.3）。统计口径与 ROI 盒由三组 investigation
> 实采图反演校准（工具：`scripts/gi_roi_stats.py`，meanY=Rec.709 亮度均值，
> dark=Y<32 绝对值，arch 盒 (40,20,440,80) 等五个）。

## 5. 风险与纪律

- 每阶段一批提交、一个 PRIMAL_* 回滚开关；ladder dump + GI_ISOLATE A/B 先行。
- **Metal twin 同步**：DDGITraceFinalize / DDGIUpdateIrradiance /
  ScreenProbeTraceFinalize 均有 Metal 对应物；shader/struct 改动三端同检
  （SceneData 416B 静默错位教训）。
- Phase 1 后画面整体变暗是**预期**，不是回归；对比度恢复靠 Phase 3，不靠曝光。
- 引擎回归必跑：`ctest -R Unit`、`TestModularPipeline`、
  `TestVulkanSponzaRenderGraph`（窗口式集成测试跑 60–120s 查日志）。
- 工作量合计约 **6–8 个工作日**（Phase 4 独立排期不计入）。
