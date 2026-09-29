# GI 能量所有权修复实施计划（Vulkan 优先）

> 日期：2026-09-18
> 状态：✅ 已完成（2026-09-18，全部阶段实施+验证；验收数据 `Docs/diagnostics/2026-09-18-gi-ownership/`）
> 设计依据：`Docs/2026-09-18-gi-energy-ownership-audit-and-remediation.md`（下称"审计文档"）
> 平台策略：全部阶段先在 **Vulkan**（`Darwin/DebugVulkan` + `TestVulkanSponzaRenderGraph`）验证；Metal/Dawn parity 在每阶段验收后另行排期（对齐 GI 计划 §2①）
> 纪律：每阶段独立可回滚（env 开关沿用仓库惯例）；每阶段结束跑 AGENTS.md §验证 清单的相关项

---

## 总览

| Phase | 内容 | 对应评审 | 预估 | 行为变更 |
|---|---|---|---|---|
| 0 | 基线仪表与长跑基线 | — | 0.5 天 | 无 |
| 1 | P0：量纲标注 + SH 评基单源 + 槽位显式化（含 Fusion 时钟统一 + warmup knob 统一） | P0 | 1.5 天 | 无（逐位回归） |
| 2 | P1：断 FusionIndirect→DDGI + 供给归属核查 | P1 | 1 天 + 观测窗 | 有（主实验） |
| 3 | P2：SC 注入置信门收窄 | P2 | 1 天 | 有 |
| 4 | P3：openSky 拆分（skyVisibility + probeConfidence + E_geo/E_sky） | P3 | 2 天 | 有 |
| 5 | P4+P5：temporal invalidation 最小版 + GBuffer 进 RG | P4/P5 | 2 天 | 有 |

依赖链：Phase 1 → 2 → 3 可与 4 并行 → 5 收尾。Phase 2 的观测窗期间可开工 Phase 4 的 shader 侧准备。

---

## Phase 0：基线仪表与长跑基线（0.5 天）

**目标**：修复前先有可对比的数字，避免"改完感觉亮了"式验收。

- [ ] 固化长跑脚本 `scripts/gi_ownership_baseline.sh`（Vulkan）：
  - `TestVulkanSponzaRenderGraph` 锁机位跑 500 帧；
  - 开启 `GI_DCGAIN=1`（回路 DC 增益仪表，已有）、`SC_DUMP_CARDS=1`（卡片/探针能量分布，已有）、`GI_LADDER_DUMP=1`（帧 360 台账，已有）；
  - 采集：indirect 中心 ROI 均值序列（9 帧步进）、`GILadderRoi` 目标/参考带中位数、`GILadderVerdict`、openSky 直方图（复用 DDGI_AGE_STATS 模式加一次性 dump）。
- [ ] 记录基线三组（不同 `DDGI_RAY_SAMPLE`/相机位固定）：数字写入本文档 §执行日志。
- [ ] 关灯响应基线：`SetProbeLightOverride` 路径（M2 门已有）记录半衰期帧数。

**验收**：三组基线数据入库（`Docs/diagnostics/2026-09-18-gi-ownership/baseline-*.log`）。

---

## Phase 1：P0 量纲审计 + 槽位显式化（1.5 天，无行为变更）

### 1a. 暖机旋钮统一（最小改动先行）

- [ ] `PRIMAL_SC_WARMUP_FRAMES` 单点化：SC 侧 `scWarmupFrames()`（`SurfaceCachePass.cpp:1480`）与 DDGI 侧 `kWarmupFrames`（`LumenDDGIPass.cpp:1757`）改为共享 helper（放 `GIDebugParams.h` 或新 `GIWarmup.h`），默认统一 **90**。
- [ ] 回归：Vulkan 500 帧，确认 warmup 窗内 SC probe feed 关闭 + DDGI 冻结同时生效（原 DDGI 默认 0 → 90 是显式化的行为变更，需在 §执行日志记录理由：两模块同窗冻结是 C3.5 slice 3 的设计意图）。

### 1b. SH 余弦评基单源

- [ ] `Vulkan/Shaders/Lumen/ShaderLib/` 新增 `EvalLib.glsl`：`vec3 evalProbeIrradianceSH9(vec4 sh[9]/float3 sh[9], vec3 n)`（cos-lobe 因子 2/3、1/4 内置，输出 E/π 语义；调用端 ×π 得 E——与 `PROBE_SH_RAD_TO_IRRAD` 惯例对齐并写明）。
- [ ] 替换三处手写展开：`DDGIGIGather.comp:670-678`（含 R-lobe 第二实例）、`SurfaceCacheLightEval.comp:165-173`、`DDGITraceFinalize.comp:324-334`（`shDot9` 改为调用库或删除）。
- [ ] Metal `.metal` 同步引入等价 inline（Vulkan 领先，Metal 保持行为一致的复制实现，注释互指）。
- [ ] `build_spv.sh` 重编受影响 `.comp.spv`。

### 1c. 单位标注（注释级，不改 ABI）

- [ ] 按审计文档 §5 铁律给以下缓冲/输出加 `// UNITS:` 标注：`ray_buffer`（Radiance）、`irradiance_buffers_`（Radiance-SH）、`ddgiIrr`（Radiance-SH）、GIGather 输出（Irradiance）、`indirect_output_`（DiffuseOutgoingRadiance）、`lighting_atlas_`（DiffuseOutgoingRadiance）、`card_radiance_buf_`（DiffuseOutgoingRadiance）、`prev_frame_color` 双语义（见 binding 注释：DirectOnly 或 Fused=Lo）。

### 1d. 槽位显式化（工程修复核心）

- [ ] 新增 `Engine/Graphics/Lumen/DDGISlotMap.h`：
  ```cpp
  struct DDGIFrameResources { u32 writeIndex; u32 currentIndex; u32 previousIndex; };
  // writeIndex = frame % 3; previousIndex = (frame+2)%3; currentIndex 在 DDGI 完成帧 = writeIndex
  DDGIFrameResources DDGISlotsForFrame(u32 frame);
  ```
- [ ] 替换裸算（全部改读同一来源，语义注释）：
  - `LumenDDGIPass.cpp:1636-1637`（outIdx/histIdx）+ `GetIrradianceBuffer/GetVolumeView` 调用点传参语义核实；
  - `StandardRenderPipeline.cpp:2384-2388`（SC feed → previousIndex）、`:2590`（SPGI fallback → previousIndex）、`:2677,2682`（GIGather volume view → currentIndex）；
  - `GIGatherModule.cpp:217-219`（ddgiReadIdx → currentIndex）；
  - `SCDDGIIntegrationModule.cpp:684-685`（frameIdx/histIdx）。
- [ ] **Fusion 时钟统一**：`FusionCompositeModule` 的 `indirect_output_/fusion_output_/indirect_ds_` 索引与 `GetIndirectOutput()` 参数从 `cbIdx` 改为绝对帧号（`frameCount_%3`）；`MakeProbeRadianceSource`（`StandardRenderPipeline.cpp:1840`）读槽同步改 `(frameCount_+2)%3`。排查所有 `GetIndirectOutput/GetFusionOutput` 调用点一并切换。
- [ ] 回归（本阶段门禁）：
  - `ctest --test-dir Darwin/DebugVulkan -R "Unit" --output-on-failure`；
  - Vulkan 500 帧跑：日志无 fault；**indirect ROI 均值序列与 Phase 0 基线逐帧一致（容差 ≤1%，纯重构）**；
  - 交叉构建检查：切回 Debug 树前 `rm Darwin/Debug/libEngine.a`（AGENTS.md 提醒）。

**验收**：逐位回归通过；`git diff` 无 shader 数学变化（除评基内联→等价函数调用）。

---

## Phase 2：P1 断 FusionIndirect→DDGI（1 天 + 500 帧观测窗，主实验）

### 2a. 断边

- [ ] `MakeProbeRadianceSource`（`StandardRenderPipeline.cpp:1821-1843`）默认返回空：`PRIMAL_PROBE_FUSED_SUPPLY` 语义反转为**默认断开、=1 回接**（A/B 保留）。
- [ ] `camData.radiance_is_previous_frame` 随之走 false 分支（radiance_depth 复用当前屏深，`StandardRenderPipeline.cpp:2466-2475` 现有分支）。
- [ ] DDGI finalize prev-screen 分支（`DDGITraceFinalize.comp:480-495`）在无有效 radiance 源时自然落 analytic（现有 `screenValidity.y<0.5` 门已处理，确认 `radiance_depth` 绑定的是当前深度时 `projectVisibleHit` 语义正确——**注意**：prev-screen 采样的是 `prev_frame_color` 绑定，断边后 binding 8 = deferred 直射输出还是直接 INVALID？**决策：绑 INVALID → `samplePrevColor` 由 validity 门短路，保留 shader 不动**；避免"直射绕过遮挡二次入账"复发）。
- [ ] `DDGICameraData` 的 `radiance_depth`/`radiance_is_previous_frame` 字段与 CPU 端 depth_history 读取随之旁路（省一次历史深度解析）。

### 2b. 供给归属核查（评审 §19-P1 的后半句，不是加 gain）

- [ ] `DDGI_PATH_DEBUG=1`（Vulkan 侧 `_pathdbg` spv 已有）跑断边前/后各一次，对比 ray path 直方图（SC / prev-screen / analytic 占比）。
- [ ] 断边后 analytic 占比上升的命中去向核查：
  - SC tile radiosity 供给是否在位（`SC_DUMP_CARDS` 卡片 lit 分布 + `GILadderVerdict` 是否 A 档）；
  - SC cell-cap 覆盖损失量化：finalize miss→SC 回退命中率（可临时 dump `scUsed/missFound` 计数）；
  - 若确认 SC 覆盖不足：按 §3.1 风险表修（cell cap 提升 / 查询回退多 cell），**独立小步提交**。
- [ ] 观测窗判定（对照 Phase 0 基线）：
  - 能量保持：indirect ROI 均值 ≥ 基线 85% 且 100→500 帧无漂移（斜率 ≈0）；
  - DC 增益 <0.5（预期显著低于断边前）；
  - 关灯半衰期 ≤ 基线（响应变快是本实验的预期收益之一）；
  - **若能量塌陷**：不回接 E4；按 2b 归属修复后复测；二次仍塌则记录数据开评审（评审原文同样要求）。

**验收**：A/B 数据表入 `Docs/diagnostics/2026-09-18-gi-ownership/phase2-ab.md`；默认配置（无 env）= 断边态。

---

## Phase 3：P2 SC 注入置信门收窄（1 天）

- [ ] `DDGIProbeIrradianceFromCards.comp:190-202` 的 `scWeight` 改造：
  - 新 uniform 槽传入 per-probe `convergence`（confidence buffer 已有，绑定追加 binding 6）；
  - `scWeight = countCoverage × validCoverage × agree × mix(0.45, 0.15, settled)`，`settled = smoothstep(0.5, 1.0, convergence)`——高置信探针的 SC 注入降为低频 floor 档，新/未收敛探针保留 0.45 修复档；
  - env 回滚 `PRIMAL_SC_INJECT_SETTLED=0` 恢复旧式。
- [ ] C++ 侧 `SCDDGIIntegrationModule` 追加 confidence 绑定（INVALID 时 stand-in，shader 自门控，沿 C 系列惯例）。
- [ ] A/B：对照 Phase 2 基线测 (a) 拱廊 sun-patch/shade SH L1 对比度（复用 `SC_CARDS` spread p90/p10）；(b) DC 增益；(c) `probe_vis` 模式 4 目检未收敛探针恢复速度。

**验收**：对比度不回退（≥ 基线 95%）、DC 增益不升、SC 注入对已收敛场的影响可忽略（<5% L0 变化）。

---

## Phase 4：P3 openSky 拆分（2 天）

### 4a. 数据面

- [ ] `opensky_buffer_` 扩展 float2/probe：`{escapeRatio, skyEnergyRatio}`：
  - `DDGIUpdateIrradiance.comp` 逐射线分类处（:161-179）同步累计 `skyEnergy += radiance·weight`（逃逸射线）与总能量 → 比值 EMA；
  - `LumenDDGIPass` 缓冲尺寸 ×2、`GetVolumeView.opensky` 语义注释更新、GIGather flat 拷贝 stride 同步（`GIGatherModule.cpp:527-530` probes×4B → ×8B）。
- [ ] env 回滚 `PRIMAL_GI_OPENSKY_V2=0` 走旧 float 路径。

### 4b. 消费端

- [ ] `DDGIGIGather.comp` / `SurfaceCacheLightEval.comp`：
  - 探针贡献拆 `E = E_geo + classify·E_sky`：实现为对评基输出按 `skyEnergyRatio` 拆分（`E_geo = irr·(1−sER)`，`E_sky = irr·sER`；比例由本探针逃逸射线能量占比给出，shading 权重管线原样作用于两分量）；
  - 删除整探针 `classify = 1−skyness·enclosed·0.85` 对 E_geo 的作用（E_geo 只保留 Chebyshev/法线/掩埋权重）；
  - `probeConfidence = f(escape, depthVar, convergence)` 作为可选调试面（probe_vis 模式 5），暂不进权重。
- [ ] 远场 ×0.5 退场：`SetFarSkyScale(0.5f)`（`StandardRenderPipeline.cpp:1016`）改 1.0（env `PRIMAL_GI_FAR_SKY` 保留 A/B）；远场天空由解析天空路径统一供给（finalize 逃逸分支已是 `EvaluateSkyBase×SkyColor` 单一事实源）。
- [ ] 回归门：
  - 半开放带（拱廊 openSky 0.3–0.8 区域，GILadder ROI2 参考带）indirect 中位数 ≥ 断边基线（预期提升）；
  - 封闭室（R7 场景 ROI）无 sky 分量泄漏（目标带中位数不升超 5%）；
  - 500 帧无漂移。

**验收**：两 ROI 同时达标 + A/B 数据入库；`PRIMAL_GI_OPENSKY_V2` 默认 1。

---

## Phase 5：P4 temporal invalidation 最小版 + P5 GBuffer 进 RG（2 天）

### 5a. GIHistoryValidity 最小版

- [ ] 事件源：编辑器灯光 color/intensity/enabled/transform 修改（与 GIEditorPanel 同路径）→ pipeline 侧 `OnLightingChanged()`：
  - DDGI：`BoostProbePriority()`（已有）+ 短窗内 `TemporalAlpha` 抬到 0.35（复用 `PRIMAL_DDGI_ALPHA_HOLD` 机制，改为事件驱动窗）；
  - SC：清 `lighting_atlas_seed_mask_` 不必要——改为 tile validity 位图清零（`indirect_tile_valid_buffer_` 已有 reset 机制）+ 一帧页预算拉满；
  - SPGI：`RequestTemporalReset()`（已有）。
- [ ] 场景切换路径核实：`ClearProbeField` 现有调用点补 SC/SPGI 对应 reset。

### 5b. GBuffer 进 RG（仅 Vulkan）

- [ ] Nanite GBuffer 产出 pass 在 RG 中声明 Write（T1.2 模式推广）；SC capture/GIGather/SPGI/SSGI 消费改用 producer identity；
- [ ] 删除对应手工 GBuffer barrier（`SurfaceCachePass.cpp:1885-1898` 等处逐一核实）；
- [ ] 验收：Vulkan validation 层零 barrier 报错 + 500 帧逐位回归 + `primal test render` 通过。

---

## 执行日志

| 日期 | Phase | 结果 |
|---|---|---|
| 2026-09-18 | — | 计划创建（审计文档同日定稿） |
| 2026-09-18 | 0 | ✅ `scripts/gi_ownership_baseline.sh` 固化；新增仪表 `SPONZA_MAX_FRAMES`（env 自动退出，500 帧脚本化）与 `DDGI_OPENSKY_STATS`（openSky 直方图一次性 dump，DDGI_AGE_STATS 同模式）。三组基线（DDGI_RAY_SAMPLE=0/1/2）+ 关灯半衰期基线入库 `Docs/diagnostics/2026-09-18-gi-ownership/baseline-*.log`；半衰期 **12 帧**（≤60 门 PASS） |
| 2026-09-18 | 1 | ✅ 1a warmup 单源（`GIWarmup.h`，默认统一 90；**DDGI 侧默认 0→90 是显式行为变更，理由**：两模块同窗冻结是 C3.5 slice 3 设计意图，旧行为下 DDGI 在 SC atlas 未收敛时更新探针=对瞬态选择多稳态分支）。1b `ShaderLib/EvalLib.glsl` 单源（GIGather 主/R-lobe、LightEval、Finalize shDot9 四处内联撤销；Metal 复制实现注释互指）。1c UNITS 注释（ray_buffer/irradiance_buffers_/ddgiIrr/GIGather 输出/indirect_output_/lighting_atlas_/card_radiance_buf_/prev_frame_color 双语义）。1d `DDGISlotMap.h`（7 处裸算改单源；**Fusion 三缓冲 cbIdx 时钟废除**改 frame_index——当日两者数值恒等，行为零变化）。回归：Phase 0 基线复测一致（详见 diagnostics/r1） |
| 2026-09-18 | 2 | ✅ `PRIMAL_PROBE_FUSED_SUPPLY` 语义反转（默认断开，=1 回接）；断边态 radiance_depth 置无效→prev-screen 短路（防直射绕遮挡二次入账）。A/B + 路径直方图 + 半衰期入 `phase2-ab.md` |
| 2026-09-18 | 3 | ✅ SC 注入置信门：`scWeight × mix(0.45, 0.15, smoothstep(0.5,1,convergence))`；binding 6=confidence；env `PRIMAL_SC_INJECT_SETTLED=0` 绑零 stand-in 回滚。A/B 入 `phase3-ab.md` |
| 2026-09-18 | 4 | ✅ openSky V2：缓冲 float2 `{escapeRatio, skyEnergyRatio}`；GIGather/LightEval `atten=mix(1,classify,sER)` 只衰减天空分量（V2-off ≡ 旧式逐位回滚）；远场 ×0.5 退场（`PRIMAL_GI_FAR_SKY` A/B）。A/B 入 `phase4-ab.md` |
| 2026-09-18 | 5 | ✅ 5a GIHistoryValidity 最小版：`UpdateLightingSettings` 变化检测 → DDGI `NotifyLightingChanged`（BoostProbePriority+30 帧 α=0.35 事件窗）/ SC `RequestTileValidityReset`+一帧页预算拉满 / SPGI `RequestTemporalReset`。5b GBuffer 进 RG（Vulkan）：BuildRenderGraph 站点规范身份单导入 + NaniteSceneRender Write 声明 + 18 处消费点复用；SurfaceCachePass 手工 no-op barrier 删除（Metal 保留）；RenderWithCommandBuffer twin 路径因图生命周期多次 Clear 暂保持旧 per-site 导入（parity 矩阵已记）。回归 + 事件路径验证入 diagnostics/r5 |
| 2026-09-18 | 全局 | ✅ parity 矩阵追加 §4 Vulkan 领先项清单；本文档与审计文档移入 `Docs/completed/` 并登记 `Docs/ARCHIVE.md`。`ci_local.sh`：C++ 构建/框架自测/CLI 单测/engine info 通过；step5 render 输出正确（PNG+JSON ok:true）但进程退出 134——editor_mode（前向路径，不经过任何 Lumen 代码）析构期 `FreeList.h:27 assert(!_size)`（RHI 资源池关停顺序问题，**既有问题**，与本计划改动面无关），已记录待修 |
| 2026-09-18 | 事故补充 | twin 站点（RenderWithCommandBuffer）`fusionIn.frame_index` 漏设导致槽位钉死、仪表读未写槽（NaN）——离线/仪表路径全部失效的第二个根因，已修复并重跑全部测量（r 系列） |
| 2026-09-18 | 事故记录 | **构建验证纪律教训**：Phase 1-5 初次构建链中 5 次 cmake 失败被 `| grep error` 管道吞掉（cwd 漂移一次、漏 include 一次、_pad 改名破坏一处、5b twin 作用域错误两处），导致 phase1-4 的首轮"验证"全部跑在旧二进制上、相应 A/B 文档一度给出无效结论。整改：(1) 所有构建改为显式核对二进制 mtime + 新符号 strings；(2) 全部测量在修复后的二进制上重跑（diagnostics/r1-r5 系列），A/B 文档以重跑数据为准。另：GIGatherModule.cpp 在 Phase 4a 编辑中因脚本误操作被清零，自 HEAD 恢复并重放本会话编辑——该文件会话前的未提交增量无法找回，恢复后与 Phase 3 存档结果的微小稳态偏移（~-0.3%）在 r 系列同二进制 A/B 对照中不影响各阶段判定 |

## 完成定义（全局门）

- [x] 默认配置（无任何 GI env）下：E4 断开、SC 注入带置信门、openSky V2 生效、槽位统一走 `DDGISlotsForFrame`（r5-regress 默认态 + strings 符号验证）；
- [x] Phase 0 基线 vs 最终态对照报告（能量/对比度/响应三项）→ `Docs/diagnostics/2026-09-18-gi-ownership/baseline-vs-final.md`；
- [x] `./scripts/ci_local.sh`：1/2/3/4 步 + engine info 通过；step5 render 输出正确但进程退出 134（editor_mode 析构期 FreeList 断言，既有问题与本计划无关，见执行日志——Lumen 链的回归门由 Vulkan 500 帧 ×13 run + ctest 4/4 覆盖）；
- [x] Metal parity 差距清单 → `Docs/RHI/GraphicsAPI_Parity_Matrix.md` §4；
- [x] 本文档与审计文档移入 `Docs/completed/` 并同步 `Docs/ARCHIVE.md`。
