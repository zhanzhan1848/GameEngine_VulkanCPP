# GI 能量所有权审计与修复设计（Energy Ownership Audit）

> 日期：2026-09-18
> 状态：✅ 审计定稿，P0–P5 修复已全部实施并通过验证（2026-09-18；实施记录见
> `Docs/2026-09-18-gi-energy-ownership-remediation-plan.md` §执行日志，验收数据见
> `Docs/diagnostics/2026-09-18-gi-ownership/`——phase2/3/4-ab.md、phase5-validation.md、
> baseline-vs-final.md）
> 范围：Lumen GI 全链路（SurfaceCache / DDGI 近+远场 / SC→DDGI 集成 / GIGather / SPGI / Fusion），**Vulkan 后端优先**
> 触发：外部架构评审提出 6 项结构性风险（反馈环、槽位语义、SC 双路径、量纲、openSky 三职、temporal 链）
> 关联：`Docs/2026-08-24-gi-optimization-plan.md`（五轴框架）、`Docs/2026-08-26-spgi-lumen-screen-probe-fusion.md`、
> `Docs/completed/2026-09-04-spgi-energy-calibration-and-ddgi-overexposure.md`（Phase 1–4 能量根治系列）

---

## 0. 结论速览

外部评审的 **6 项结构性风险全部在代码中核实成立**，其中 3 项比评审预期更严重或另有细节：

1. **FusionIndirect → DDGI 自反馈环真实存在且 Vulkan 默认开启**（`MakeProbeRadianceSource`，Phase 2.3 pre-gain 化只切断了显示增益，没有切断层级回流）。
2. **SC atlas 的内容不是"Surface Radiance Truth"**：LightEval 写入 = 直射 + DDGI gather 间接 + tile radiosity 自反弹 + emissive——DDGI trace 反向采样它时读到的是"含自己贡献的回波"。这是比"槽位/系数"更根本的角色越界。
3. **SC→DDGI 双路径已经是 estimator fusion 而非能量叠加**（`mix(pathA, scResult, w≤0.45)`），评审 §7 的最坏担心不成立；但 SC 注入路径无遮挡感知、每帧全探针覆盖，与"只做 acceleration"的边界仍有距离。
4. **槽位语义除 `frame%3` 双义外，新发现 Fusion 三缓冲使用 `cbIdx`（swapchain 槽）时钟**，与 DDGI 体系的 `frameCount%3` 时钟无同步保证——评审建议的显式 `DDGIFrameResources` 完全对症且必须顺带统一两处时钟。
5. **`PRIMAL_SC_WARMUP_FRAMES` 同名旋钮在 SC 默认 90、在 DDGI 默认 0**——同一开关两处语义漂移的现成实例。
6. Surface Card **几何创建**总体健康；不符合角色表的不是 card 分配，而是 **SC atlas 内容**（见 §3）。

修复按评审的 P0–P5 排序落地，全部先在 Vulkan 链验证（决策：GI 计划 §2①"Vulkan 领衔"），Metal parity 后置。

---

## 1. 审计范围与方法

逐文件通读（非抽查）以下代码，并对照评审逐条核实：

| 层 | 文件 |
|---|---|
| Card 创建 | `Engine/Graphics/Lumen/SurfaceCache/CardGenerator.{h,cpp}`、`SurfaceCacheTypes.h` |
| SC pass | `Engine/Graphics/Lumen/SurfaceCache/SurfaceCachePass.cpp`（3020 行全量） |
| DDGI pass | `Engine/Graphics/Lumen/DDGI/LumenDDGIPass.{h,cpp}`（3322 行全量） |
| 集成模块 | `Engine/Graphics/RenderPipeline/Modules/SCDDGIIntegrationModule.cpp`、`GIGatherModule.cpp`、`FusionCompositeModule.{h,cpp}` |
| 编排 | `Engine/Graphics/RenderPipeline/StandardRenderPipeline.cpp`（GI 相关 Step 5–10 段落 + `MakeProbeRadianceSource`） |
| Vulkan shader | `Vulkan/Shaders/Lumen/`：`DDGITraceFinalize.comp`、`DDGIUpdateIrradiance.comp`、`DDGIProbeIrradianceFromCards.comp`、`DDGIGIGather.comp`、`SurfaceCacheLightEval.comp`、`FusionIndirect.frag` |
| 集成测试 | `EngineTest/IntegrationTests/TestVulkanSponzaRenderGraph.cpp`（确认 Vulkan 链走 `StandardRenderPipeline`） |

---

## 2. 评审论点逐条核实

### 2.1 反馈环（评审 §1/§2）——✅ 证实

逐 shader/绑定核实的完整反馈边清单（t = 当前帧）：

| # | 边 | 实现 | 证据 |
|---|---|---|---|
| E1 | DDGI(t−1) → SC atlas(t) | SC LightEval `gatherProbeIrradiance`（8/27 探针三线性 + Chebyshev 可见性 + openSky 分类），槽位 `(frame+2)%3` | `StandardRenderPipeline.cpp:2382-2400`、`SurfaceCacheLightEval.comp:269-384` |
| E2 | SC atlas(t) → DDGI(t)（path A） | finalize 逐射线 SDF 命中 → SC grid 查询 → `scQueryCardCandidate` 采样 lighting_atlas | `DDGITraceFinalize.comp:439-479` |
| E3 | SC atlas(t) → DDGI(t)（path B） | `SC_DDGI_Integration`：CardRadianceAvg → ProbeIrradianceFromCards，每帧**全探针**重写，`mix(pathA, scResult, w)`，w ≤ 0.45 | `SCDDGIIntegrationModule.cpp:752-792`、`DDGIProbeIrradianceFromCards.comp:190-202` |
| E4 | FusionIndirect(t) → DDGI(t+2) | `MakeProbeRadianceSource` 把 `fusion_module_->GetIndirectOutput((cbIdx+2)%3)` 作为 DDGI finalize 的 prev-screen 供给（binding 8） | `StandardRenderPipeline.cpp:1821-1843,2460-2465`、`LumenDDGIPass.cpp:2413,2421` |
| E5 | DDGI(t−1) SH → DDGI(t)（内部多反弹） | finalize 解析回退分支 `result = (ρ/π)(E_sky + π·L_i_prev)` | `DDGITraceFinalize.comp:496-515` |
| E6 | SC atlas 自反馈 | prev_lighting 多反弹回退分支 + tile radiosity（indirect_out 的 resolve 以 prev_lighting 作 EMA history，W=2.0 默认） | `SurfaceCacheLightEval.comp:479-537`、`SurfaceCachePass.cpp:2788` |
| E7 | DDGI(t−1) → SPGI miss fallback | `SetDDGIFallbackData(view[(frame+2)%3])` | `StandardRenderPipeline.cpp:2586-2595` |

评审给出的环 `Fusion → DDGI → SC → DDGI` 与 `SC → DDGI integration` 均真实存在；E4 使 E1/E2/E3 携带的能量在 2 帧后再入探针场。现行稳定手段（回路 DC 增益预算 `collection ≤ 0.85 × ρ < 1`、SH 钳位 64、prev-screen 过亮惩罚 `1/(1+max(lum−1,0)·2)`、hysteresis EMA）保证的是**收敛**，不保证**能量归属唯一**——同一份地板反弹可经 E6（SC 内）、E1+E2（探针往返）、E4（屏幕回流）最多三次入账，稳态偏亮且无法归因。

### 2.2 FusionIndirect→DDGI 是否该拆（评审 §2）——✅ 同意拆，附两个执行细节

- **量纲现状**：`FusionIndirect.frag:89-100` 输出 `indirect = albedo · gi · gain · (1/π)`，其中 `gi = mix(ddgi, spgi, w) + ssgi·conf·mix(.25,.15,w)`，ddgi/spgi 输入均为 irradiance（GIGather 已 ×π）。即 **FusionIndirect 是 DiffuseOutgoingRadiance (Lo)**。finalize 把它当逐射线 radiance 直用（radiance 沿射线不变，量纲恰好成立），**没有二次 ×ρ/π**——评审 §3 担心的双重 albedo 在这条边上不存在；真正的问题是**语义回灌**（lo 里含 DDGI(t−2) 的 gather 输出）。
- **拆不掉供给就塌**：B11 引入本边的历史原因是旧 prev-screen（DeferredLighting 直射-only）供给比物理阴影墙辐射低 ~30×。直接切断后射线命中屏幕内表面将落到 analytic 分支（E_sky×0.3 + ρ·L_prev）。这与评审的判断一致：**若切断后变暗，不许加 gain 回来，要追查 bounce 该由 SC（E2/E6）还是 DDGI 自身产生**——具体见计划 Phase 2 的归属检查步骤（`DDGI_PATH_DEBUG` 路径直方图 + SC tile radiosity 供给核查）。

### 2.3 SC→DDGI 双路径（评审 §6/§7）——✅ 证实双路径，❌ 修正"additive"预期

`ProbeIrradianceFromCards` 的合并式是 `blended = mix(pathA, scResult, scWeight)`，`scWeight = countCoverage × validCoverage × 0.45 × agree`——**已是 estimator fusion，不是 E_trace+E_inject**。剩余问题：

1. SC 路径 occlusion-unaware（shader 注释自认，solid-angle 投影无可见性），0.45 封顶是症状压制而非修复；
2. path B **每帧全探针执行**（"SC-first"层级声明），而 path A 只更新 top-N——未更新探针的 pathA 是历史 EMA，两者时间常数不同；
3. `agree = smoothstep(scLum/pathLum)` 是亮度比值启发式，无物理含义，可参数化但没有归属语义。

评审建议的"injection 仅用于初始化/重定位/低置信恢复"方向正确，落点是把 scWeight 的覆盖项升级为置信门（Phase 3），而不是删除该路径。

### 2.4 槽位语义（评审 §8/§9）——✅ 证实 + 新发现两处

- 全链裸算 `frame_index%3`：DDGI AddPass（`LumenDDGIPass.cpp:1636-1637`）、SC feed（`StandardRenderPipeline.cpp:2384-2388`）、SPGI fallback（:2590）、SCDDGI（`SCDDGIIntegrationModule.cpp:684-685`）、GIGather（`GIGatherModule.cpp:217-219`，注释自认曾因 `(cbIdx+2)%3` 相位漂移翻车一次）。
- **问题咬过人的实锤**：confidence 缓冲曾是三缓冲 + 消费端 `frame%3` 读取，"effectively never seeing a settled probe"，已改单缓冲修复（`LumenDDGIPass.h:329-337` 注释）。
- **新发现 ①（时钟分裂）**：`FusionCompositeModule` 三缓冲按 `cbIdx`（swapchain 槽）索引（`FusionCompositeModule.h:73`），`MakeProbeRadianceSource` 读 `(cbIdx+2)%3`；DDGI/SC 体系全用 `frameCount%3`。两时钟无同步不变量——cbIdx 与 `frameCount%3` 一旦错位（resize、多视图、未来异步），prev-screen 供给会读到**本帧正在写的槽**（写读同槽竞态）而非 2 帧前的完成槽。
- **新发现 ②（同 knob 双默认）**：`PRIMAL_SC_WARMUP_FRAMES` 在 SC 侧默认 90（`SurfaceCachePass.cpp:1480-1487`），在 DDGI 侧默认 0（`LumenDDGIPass.cpp:1756-1761`）——不设 env 时两模块的 warmup 行为本就不一致。

评审建议的显式 `DDGIFrameResources{writeIndex, currentIndex, previousIndex}` 正确，且必须**顺带把 Fusion 的 cbIdx 时钟统一到绝对帧号**。

### 2.5 openSky 三职（评审 §10–§13）——✅ 证实

openSky 定义 = 每探针逃逸射线占比（w≤−1.5），EMA 0.35（`DDGIUpdateIrradiance.comp:157-169,269-276`）。三个消费点：

1. **GIGather 分类加权**：`classify = 1 − skyness·enclosed·0.85`（`DDGIGIGather.comp:684-691`）——作用于**探针整体贡献**（含 geometry bounce），sky-dominant 探针在封闭像素处最多 ×0.15；
2. **SC LightEval 孪生分类**（`SurfaceCacheLightEval.comp:241-251`/`341-347`）——同一公式作用于卡片 texel gather；
3. **远场天空 ×0.5 档**（`StandardRenderPipeline.cpp:1016` → `LumenDDGIPass.cpp:2019` `vd.SkyColor × far_sky_scale_`）——供给端全局压暗。

评审 §12 的担忧成立：露天探针（openSky≈0.8）提供的**地面/建筑反弹**与天空辐照一起被压；§13 判断正确：`far ×0.5` 不物理，是"远场只表达低频"的代理。拆分方案（skyVisibility + probeConfidence + E=E_geo+classify·E_sky）在 Phase 4 落地，数据基础（逃逸统计 + 八面体深度 + convergence age）已齐备。

### 2.6 Temporal 链（评审 §14/§15）——✅ 证实

| 层 | 时域机制 | 失效感知 |
|---|---|---|
| SC atlas | 无显式 EMA；页预算轮转（governor/env）+ 持久 atlas = 隐式时域；tile radiosity EMA 0.9 | 无 |
| DDGI | EMA α=0.05（年轻 0.35）+ hysteresis；convergence age | 仅 scene switch（`ClearProbeField`/`BoostProbePriority`） |
| SPGI | temporal 重投影 | camera cut / viewport resize（`RequestTemporalReset`） |
| GIGather | history 已禁用（`GIGatherModule.cpp:777-780`） | — |
| TAA | velocity 重投影 + YCoCg clamp | resize |

无统一 Radiance Change Signal：灯光移动/强度变化不会降低任何一层的 history 权重。评审 §15 的 `GIHistoryValidity` 是正确的长期形态，Phase 5 先落最小版（灯光编辑事件 → DDGI 提权 + SC 短 warmup + SPGI reset）。

### 2.7 GBuffer 不进 RG（评审 §16）——✅ 证实，P5 判断正确

`StandardRenderPipeline.cpp:1878-1883` 注释原文确认；SC pass 内手工插 GBuffer barrier（`SurfaceCachePass.cpp:1885-1898`）。T1.2 (C3) 已开始偿还（HZB/SC atlas 走 producer RG identity），推广路径存在。短期不产生画质 bug，维持 P5 排序。

### 2.8 量纲审计（评审 §4/§19-P0）——✅ 部分成立

四种量客观上**已经存在且大体自洽**（DDGI SH = radiance 投影 → GIGather ×π 输出 E → FusionIndirect ×ρ/π 输出 Lo → SC atlas 存 Lo），但：

- 命名全部是 `gi`/`indirect`/`irradiance` 混用，`DDGIGIGather.comp` 的 `ddgi` 变量实际装的是 irradiance；
- cos-lobe 因子（L1 ×2/3、L2 ×1/4）在 `DDGIGIGather.comp:670-678`、`SurfaceCacheLightEval.comp:165-173`、`DDGITraceFinalize.comp:324-334` **三处手写展开**，靠注释互相警告维持一致（"NEVER apply this factor inside…"）；
- π 一致性靠 Phase 1/2 的历史补丁（`PRIMAL_SC_GATHER_PI` 注释记载了 π 缺省是"deliberate loop-gain headroom"的历史）。

P0 审计的落点：命名规范 + 把 SH 评基收敛到 ShaderLib 单函数 + 每个跨 pass 缓冲标注单位（见计划 Phase 1）。

---

## 3. Surface Card 创建专项评估（点名项）

**结论：几何/分配侧符合要求，无阻塞缺陷；不符合"SC = Surface radiance truth"的不是 card 几何，而是 atlas 内容（§3.2）。**

### 3.1 几何/分配（符合）

- **双源统一**：离线 bake（T4.2，mesh-local → world 变换正确，含旋转缩放的保守 AABB 投影）与 6 向 AABB fallback 共用 `emit_card`（过滤 → max_cards 截断 → shelf 装箱 → 分辨率策略），`CardGenerator.cpp:164-238`；
- **分辨率策略合理**：64 texels/unit、page 对齐、`[page, 4×page]` 夹取、`SC_MIN_CARD_RES` 提底、放不下降 1 页再跳（T8.13 P3 "coverage > detail"）；
- **查询结构健康**：T1.3 均匀网格 + 前缀和，DDGI finalize O(1) 查询；dummy 网格兜底无 SC 场景；大网格优先排序继承到 per-cell 列表；
- **可观测性**：分辨率分布/装箱审计日志、`SC_DUMP_CARDS` 卡片亮度直方图。

风险点（记录在案，非本轮修复）：

| 点 | 位置 | 影响 |
|---|---|---|
| `screen_space_area` 恒 0 | `CardGenerator.h:13`、`RegisterMesh:36` | 重要性分配从未实现；容量满时按注册顺序静默丢弃（`capacity_full`），无重要性信号 |
| `kMaxCardsPerGridCell = 8` | `SurfaceCacheTypes.h:205`、`CardGenerator.cpp:366` | 稠密单元小卡片被大卡片挤出 → finalize SC 查询 miss → 落 prev-screen/解析回退，能量与色彩失真（这与"缺失的 bounce 由谁产生"直接相关，Phase 2 归属检查时一并量化） |
| CardFill 与 GBuffer gather 竞写 | `SurfaceCachePass.cpp:1988-2013` | last-writer-wins 靠 albedo-alpha skip guard，注释自认 TODO |
| Metal 无 CardFill/capture graphics | 同上 | parity 缺口（Vulkan 优先策略下后置） |

### 3.2 atlas 内容（不符合角色表——本轮核心修复对象）

`SurfaceCacheLightEval.comp:533-537` 的产出：

```
result = (direct + E_indirect) × ρ/π            // E_indirect = DDGI gather（E1）
       + tileRadiance × W                        // SC 自反弹（E6）
       + emissive
```

即 **SC atlas 已经是"直射 + 探针间接 + 自反弹"的融合产物**。后果：DDGI trace 采样 SC（E2）读到含自己贡献的回波；SC 注入 DDGI（E3）同；评审要求的"SC = surface-domain radiance truth"边界在内容层面被打破。**修复不是把 DDGI 项删掉**（那会让阴影 texel 塌黑，B11 前的历史已验证），而是**显式声明 SC 的合法构成并让每个分量可归因**：

- SC 合法构成 = 直射（含 PCSS 可见性）+ **SC 自有 bounce**（tile radiosity / prev_lighting，surface-domain 内传播）+ emissive；
- 探针间接（E1）定位降级为**冷启动地板**：仅探针场未收敛（convergence age < 阈值或 warmup 窗）时供给，收敛后退出，由 E2 的逐射线 SC 采样承担"探针从 SC 拿 bounce"的唯一通道；
- 这样 E1 从常开反馈边变为受控初始化源，评审"SC↔DDGI 受控 feedback"即成立。

---

## 4. 修复设计（对应评审 P0–P5）

### P0 量纲审计（Phase 1，前置无行为变更）

1. 四量命名规范写入本文档 §5，代码先行**注释级标注**（不改 ABI）：每个跨 pass 缓冲声明 `// UNITS: <Radiance|Irradiance|DiffuseOutgoingRadiance|ScreenIndirect>`；
2. SH 余弦卷积评基收敛到 `ShaderLib/ProbeSampleLib.glsl`（或新 `EvalLib`）单函数 `evalProbeIrradianceSH9(sh, n)`，替换三处手写展开（GIGather/LightEval/Finalize）；
3. `PRIMAL_SC_WARMUP_FRAMES` 单点定义（共享 helper），两处默认统一为 90（现行为取交集前的显式声明）；
4. 纯重构，验收 = Vulkan 100 帧逐位回归。

### P0' 槽位显式化（Phase 1 内，工程修复）

- 新增 `lumen::DDGIFrameResources{ writeIndex, currentIndex, previousIndex }`（CPU 侧由绝对帧号统一计算），替换 7 处裸 `frame%3`；
- **Fusion 三缓冲时钟从 `cbIdx` 统一到绝对帧号**（`indirect_output_/fusion_output_` 索引与 `MakeProbeRadianceSource` 的读槽同步切换），消除时钟分裂；
- GIGather 的"读本帧槽"语义改为显式 `currentIndex`（不再是 write slot 的巧合复用）。

### P1 断 FusionIndirect→DDGI（Phase 2，主实验）

1. `MakeProbeRadianceSource` 默认返回空（`PRIMAL_PROBE_FUSED_SUPPLY` 语义反转：默认断开，=1 供 A/B 回接）；prev-screen 分支失去供给后自然落入 analytic 回退；
2. **切断后供给核查**（评审要求，不是加 gain）：
   - `DDGI_PATH_DEBUG=1` 路径直方图：SC/prev-screen/analytic 占比变化；
   - SC 侧 tile radiosity 供给核查（`SC_DUMP_CARDS` 卡片亮度 + GILadder A 档）；
   - 若 SC 覆盖不足（对应 §3.1 的 cell-cap/覆盖风险），优先提高 E2 命中率，而非恢复 E4；
3. 观测窗：Sponza 锁机位 500 帧 × 3 种子，指标 = indirect 中心 ROI 均值序列（`GI_DCGAIN` 环）、回路 DC 增益（断言 <0.5）、100–500 帧能量漂移、关灯响应半衰期（M2 门）。

### P2 SC 注入收窄（Phase 3）

- `scWeight` 的覆盖项升级为置信门：高置信（convergence ≥0.5 且 countCoverage 足）探针注入权重降至低频 floor 档（≤0.15），低置信/新探针保留 0.45 修复档——即评审"injection = initialization / fallback / low-confidence recovery"语义；
- A/B 对照 Phase 2 基线：SH 对比度（拱廊 sun-patch/shade 的 L1 比）不回退、DC 增益不升。

### P3 openSky 拆分（Phase 4）

1. `opensky_buffer_` 扩展为 per-probe `{escapeRatio, skyEnergyRatio}`（UpdateIrradiance 已逐射线分类，sky 能量分离零新增 trace）；
2. GIGather / LightEval 的分类改为**只衰减天空分量**：`E = E_geo + classify · E_sky`，其中 E_sky 来自逃逸射线的天空辐照累积；`probeConfidence`（escape + depth variance + convergence 组合）接管原"可信度"职责；
3. 删除整探针 ×0.15（`skyness·enclosed·0.85`）与远场 `×0.5`（远场改为只供 E_geo，天空由解析天空统一供给）；
4. 回归门：半开放带（Sponza 拱廊 openSky 0.3–0.8）GI 亮度提升；封闭室内不因 sky 分量回泄漏（R7 场景复测）。

### P4 temporal invalidation（Phase 5a）

- 最小 `GIHistoryValidity`：灯光开关/移动/强度事件 → DDGI `BoostProbePriority` + SC 短 warmup 窗 + SPGI `RequestTemporalReset`；
- 事件源接编辑器灯光修改路径（与 GI 编辑面板同源），不新增第二事实源。

### P5 GBuffer 进 RG（Phase 5b）

- 按 T1.2 模式把 GBuffer producer 声明为 RG pass 身份，SC/GIGather/SPGI 消费走 producer identity（替换 re-import + 手工 barrier）；仅 Vulkan 先行，Metal 依赖 encoder 边界语义另案。

---

## 5. 四量命名规范（P0 交付物之一）

| 量 | 符号 | 单位语义 | 现存载体 | 目标命名 |
|---|---|---|---|---|
| Radiance | L | W/(m²·sr) | DDGI SH 系数、ray_buffer.xyz、SC atlas（Lo） | `RadianceRGB` / 场景注释 `// UNITS: Radiance` |
| Irradiance | E | W/m² | GIGather 输出（×π 后）、SPGI 输出 | `IrradianceRGB` |
| DiffuseOutgoingRadiance | Lo=ρE/π | W/(m²·sr) | FusionIndirect 输出、SC atlas 本体 | `DiffuseOutgoingRadiance` |
| ScreenIndirect | — | 屏域合成项 | Fusion 输入侧 ssgi/ssao 融合项 | `ScreenIndirectLighting` |

铁律（写入 shader 头注释）：

1. DDGI SH 是 **radiance 投影**（mcWeight=4π/N）；任何消费端评 `Σc_iY_i` 得到的是 L̄(N)，入 BRDF 前必须 ×π（或用带 cos-lobe 因子的评基直接得 E/π 后再 ×π——二选一，单源实现）；
2. prev-screen / FusionIndirect 回灌一律是 **Lo**，作为 ray radiance 合法、作为 irradiance 非法；
3. 显示增益（`giIndirectGain`、曝光）永不进入任何反馈边（Phase 1 已确立，本文档固化为铁律）。

---

## 6. 风险与回滚

| 风险 | 缓解 |
|---|---|
| 断 E4 后室内探针场能量塌陷（B11 场景重演） | Phase 2 不直接删代码，env 反转默认 + 观测窗；塌陷处置路径预定义（查 E2 命中率/SC 供给，禁止加 gain） |
| P3 sky 分量分解引入新的泄漏通道 | E_geo/E_sky 分解保持 Chebyshev/法线/掩埋权重原样作用于两个分量；封闭室回归场景先跑 |
| 槽位统一触碰并发契约 | DDGI 三缓冲并发契约注释（`LumenDDGIPass.h:502-509`）为审过的基线；统一只改索引来源不改 MAX_FRAMES_IN_FLIGHT 依赖，ctest + 100 帧回归门 |
| Metal parity 分叉 | 全部改动带 env 回滚开关（沿用仓库惯例），Metal 侧在 parity 矩阵标注"Dawn/Metal 后置"（对齐 GI 计划 §2①） |
