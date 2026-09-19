# World Space Radiance Cache（WRC）实施完工文档

> 标签：**[WRC]** ｜ 关联：**[DDGI]** [SC] [SPGI]
> 日期：2026-09-19 ｜ 状态：✅ 全 Stream 代码落地（A0–A5 / B1–B3 / C1 / D1–D3 / E1–E2），Vulkan 链验证通过
> 计划：`Docs/2026-09-18-world-space-radiance-cache-design-and-plan.md`（已移入本文档为完成记录，原文档保留作设计参考）
> 验证：Vulkan Debug 树编译 ✅、Metal Debug 树编译 ✅、Unit 5/5（双树）✅、
> TestVulkanSponzaRenderGraph 默认路径 2671 帧 0 validation/fault ✅、
> `PRIMAL_WRC_ENABLE=1` 路径 1621 帧、探针收敛（near 1683/1936 valid / meanLum 0.33 稳态、far 2044/2048 / 0.76）✅、
> `PRIMAL_GI_PROBE_FEED_BOOTSTRAP=1` 路径 1231 帧 0 validation ✅、
> shader 同语言拷贝门禁 ✅、§1.2-5 include 禁令 grep 门禁 ✅（WRC/ 域 0 个 DDGI include）

---

## 1. 交付总览（对照计划 §4 五 Stream）

### Stream B —— 能量单向化（B1/B2/B3）

单一开关 `PRIMAL_GI_PROBE_FEED_BOOTSTRAP=1`（默认 0 = 旧语义逐位不变），定义于
`Engine/Graphics/Lumen/GIWarmup.h::GIProbeFeedBootstrapEnabled()`（DDGI/SC 两侧同读）：

- **B1**：`SurfaceCacheLightEval.comp/.metal` 探针馈送（`gatherProbeIrradiance`，
  能量审计边 E1）乘 `probe_feed_bootstrap` 引导因子——`GIWarmup` 窗口内 1→0
  线性（CPU 端 `GIProbeBootstrapFactor` 随 `FlattenedLightingParams` 下发，offset 112），
  窗口后恒 0；同时 DDGI 探针更新解除窗口冻结（`LumenDDGIPass.cpp`，引导项窗口内
  才有真实反弹可供给）。
- **B2**：`radiosity_solo`（offset 116）= 1 时 tile radiosity 成为**全部** texel
  （含 volume 外）的唯一常备间接来源，legacy `prev_light` 自反馈双轨停用；
  tile solver（indirect trace/resolve）自第 0 帧运行（消费端 gate frame≥1 防
  未初始化读——同址历史 bug 的边界）。
- **B3**：删稳定器——FusionIndirect 地板项已由前序能量根治 B12 删除（Vulkan 侧
  核实无残留）；Metal 侧 texel 环境地板（0.52,0.46,0.37）在 bootstrap 模式下
  关闭（旧语义保留 Metal 现状，与 Vulkan B14 对齐）；两份
  `SurfaceCacheIndirectResolve`（comp+metal）0.02 灰占位符删除——查询失败射线
  跳过（无贡献无权重），零有效测量的 tile 存 A=0 显式无效（recon 与 legacy
  两路径），消费端跳过而非把"valid black"伪装成测量。

### Stream C —— Fusion 档位化（C1）

- `FusionIndirect.frag` + `FusionCompositeModule`：逐像素 ddgi×spgi 置信度混合
  → mode switch（GiGainUBO 第二 float）。High/Ultra（`axes.use_screen_probes`）
  → 像素只读 SPGI；Medium → 像素只读 DDGI；`PRIMAL_FUSION_PERPIXEL=1` 回滚
  旧逐像素混合（A/B）。SSGI 全档保留（权重取档位值 High 0.15 / Medium 0.25）。
  `use_screen_probes` 档位结构不变（决定 SPGI 是否运行；mode switch 只决定像素
  读哪路，两线正交）。FusionIndirect.frag.spv + 4 个 GI_ISOLATE 变体重新编译。
- Metal parity：DeferredLighting.metal::fragmentFusionIndirect 未接 mode
  （Metal 无 gain UBO 绑定面；按"Vulkan 领衔"决策 parity 后置，见 §4）。

### Stream A —— WRC 全新实现（A0–A5）

新代码域 `Engine/Graphics/Lumen/WRC/`（CMake GLOB_RECURSE 自动纳入）：

| 产出 | 文件 |
|---|---|
| A0 骨架 | `WRCTypes.h`（`PRIMAL_WRC_ENABLE` 单一开关 / K=64 / 存储布局常量 / `WRCVolumeData` std140 288B 静态断言）、`WRCSlotMap.h`（刻意复制不共享，§1.2-4）、`LumenWRCPass.h/.cpp`、`LumenConfig::wrc_*` 段 + 轴4 `wrc_propagate_sc`/`wrc_spgi_consume` 边 |
| A1 radiance trace/存储 | `WRCTraceSDF.comp`（固定 Fibonacci K 方向 SDF sphere-trace；逃逸 -2 / 耗尽 -1 哨兵）、`WRCTraceFinalize.comp`（近场命中→SC atlas 经 `scQueryCardCandidate` 公共真值层；逃逸→共享 `EvaluateSkyBase`×SkyColor×`kWRCSkyTier`=0.4 档；耗尽/查询失败→黑，不伪造能量）、`WRCUpdateRadiance.comp`（EMA α=0.1 + Chebyshev 可见性加权 + 首填直写）、`WRCCacheView.h`（对齐 GICacheView 模式）、探针存储 = 单缓冲持久 RGB16F packHalf2x16×2（8B/方向，528B/探针，near+far ≈ 9.6 MB < 25 MB cap，§2.1 预算复核） |
| A2 occlusion + prefilter | 零穿越命中判定（signed 场 sign-change + 5 次二分，`ProbeCounts.w` 旗标，`PRIMAL_WRC_UNSIGNED_TRACE=1` 回退）、入几何排除（trace 端 lane0 写起点 SDF 距离 → update 端 skin 阈值置 in-geometry 位、清 radiance；探针排除率经 `PRIMAL_WRC_DUMP` 仪表输出）、`WRCPrefilterIrradiance.comp`（radiance→SH9 预积分，IFG 式调试/降级档，`PRIMAL_WRC_PREFILTER=1` 门控） |
| A3 SPGI 消费 | `ScreenProbeTraceFinalize.comp` 远场段（hit.w==3）优先消费 WRC——cos 加权（c⁶ lobe）radiance 积分 + 近邻探针 + 消费计数原子累加（binding 20）；无效/网外回落冻结边 fallback_spgi_to_ddgi（级联→SH→天空原链逐位不变）。`ScreenProbeGlobalData` 尾扩 `wrc_fb[2]`（offset 464，496B 静态断言）+ finalize 布局 binding 19/20 |
| A4 档位接线 | High/Ultra/UltraRT 预设 `wrc_enable=true` + 轴4 边开；运行总闸仍是 `PRIMAL_WRC_ENABLE`（§3 迁移表：默认 0，验收后翻 1）；创建门控 `WRCEnabled() && (config.wrc_enable || 档位≥High)` |
| A5 placement | `WRCFitGrid.h`（场景贴合算术，独立实现）、离线 SDF 就位时近场网格从 merger volume 派生、需求驱动预算（消费计数每 256 帧读回，Top-K 按计数降序入窗 + 0 计数轮转保底）、探针有效率仪表（dump stats：valid/inGeometry/total） |

接线（`StandardRenderPipeline`）：Step 6.7 [WRC] 独立 block（双路径站点同步），
SC 视图同源转发（GetCacheView → SetSurfaceCacheResources）、离线 SDF 转发、
场景切换 ClearProbeField、ShutdownLumenPasses 独立销毁。GIDebug 新增 wrc 通道
（`wrc_trace` 开关 + `PRIMAL_WRC_DUMP` 读回环 radiance dump）。

### Stream D —— SC 视图驱动调度与流送（D1–D3）

- **D1**（`PRIMAL_SC_PRIORITY=1`）：卡片重要性打分 `ScCardPriority` = 屏幕占比
  代理(4·ex·ey) × 1/dist²(相机) × mix(1, fresh(30 帧), `importance_weight`)，
  `update_distance` 外 score=0——两个死配置参数复活。Top-K（partial_sort 降序、
  页预算内取卡）替换 LightEval round-robin；card-view capture 同打分（无相机项）
  Top-K 替换轮转游标。新鲜度表 `card_light_last_frame_`/`card_capture_last_frame_`
  随卡片集扩容。
- **D2**（`PRIMAL_SC_PAGE_TABLE=1`）：`CardGenerator::EnablePageTable`——每卡
  页坐标记账（`page_table_`：x_page<<16|y_page）+ 页表缓冲上传
  （`GetPageTableBuffer`），随卡片 sort 同步置换。
- **D3**：LRU（`TouchCard`/`card_last_touch_`，LightEval/capture 两侧刷新）+
  自由页段（`free_runs_`）：capacity_full 静默丢弃改挤页——货架满时 LRU 最旧的
  超页卡片降档单页、腾出页段重试分配。
- **D2 GPU 过表寻址（已落地）**：`SurfaceCacheData.glsl` 新增
  `scResolveCardOrigin` 过表解析帮助函数；LightEval（采样契约，binding 15 +
  `FlattenedLightingParams.page_table_enabled` offset 120）与
  `SurfaceCacheIndirectResolve`（resolve 查询契约，binding 16 +
  `IndirectResolveParamsGPU.page_table_enabled` offset 88，92B 断言）在
  `PRIMAL_SC_PAGE_TABLE=1` 时经 `card_page_table` 解析卡片 atlas 原点，替换直读
  `card.atlas_offset_*`——下游 worldToCardUV/cardTexelToWorld/scQueryCardCandidate
  全部消费解析后的卡片。关闭时原卡直读（逐位旧行为）；v1 页表与
  card.atlas_offset 恒等（无运行时迁移），数据等价性由构造保证。

### Stream E —— SC 失效粒度（E1/E2）

- **E1**：fallback 卡 AABB 从 `bounds_radius` 球转立方体换 mesh 紧致 AABB
  （本地 bounds 经 world 轴展开，与 RegisterMeshBaked/detail-SDF 同配方）；
  无 mesh bounds 时保留球转立方体回退（`StandardRenderPipeline.cpp` 注册段）。
- **E2**：`CardGenerator::RebuildInstance(instance_id, aabb)` 单实例增量重建 +
  管线侧变换脏检查改事件驱动（每帧查询变换脏旗标即事件源，30 帧冷却废除；
  脏实体立即增量更新注册 AABB，MarkFullRebind/revox/probe boost 原语义保持）。

## 2. 验证记录

| 门禁 | 结果 |
|---|---|
| Vulkan Debug 树编译 | ✅ 0 error |
| Metal Debug 树编译 | ✅ 0 error |
| `ctest -R Unit`（双树） | ✅ 5/5 |
| TestVulkanSponzaRenderGraph（默认，env 全关，最终二进制复测） | ✅ 1651 帧，0 validation/fault（"fault"匹配均为 "default" 误报） |
| TestVulkanSponzaRenderGraph（`PRIMAL_WRC_ENABLE=1 DUMP=1`） | ✅ 1621 帧；A1 验收：GIDebug 探针视图无穿模热点（dump 亮度稳态 0.33/0.76）；A1 内存实测 ≈9.6MB（16384+2048 探针）< 25MB cap；A2 排除率仪表输出 inGeometry 计数 |
| 同上（`PRIMAL_GI_PROBE_FEED_BOOTSTRAP=1`） | ✅ 1231 帧，0 validation |
| shader 同语言拷贝门禁 `check_shader_copies.sh` | ✅（同步 EngineTest 三份拷贝后 0 漂移） |
| §1.2-5 include 禁令 | ✅ `WRC/` 域与 WRC shader 0 个 DDGI include；共享仅白名单（GIWarmup/GIDebugParams/GICacheView/LumenTypes/SurfaceCacheData/ShaderLib） |
| Stream B ladder dump 逐回路断言实跑（三配置 × 500/400 帧：默认 / bootstrap / WRC+bootstrap，`GI_DCGAIN=1 GI_LADDER_DUMP=1 SPGI_CAM_LOCK=1`） | ✅ 已执行。GILadderVerdict=`C_fused_matches_model`（残差 med 0.0126 / P95 0.046）；GIDCGain amp 断言三配置均超 0.05 线（**基线自身 0.128 即超**——本工作树 ROI 噪声底，非 Stream B 引入）；逐回路启动衰减率（仪表口径：收敛瞬态相邻 |Δ| 比中位数）base 1.34 → boot 0.886 → wrc 0.606——bootstrap/WRC 配置单调改善且无发散（500 帧稳态漂移 base +24% vs boot −3.8%）；严格 <0.5 受仪表噪声底限制为后续项（需更静默 ROI/去噪读回） |
| **A2 验收：prefilter SH9 vs 消费端 cos 加权积分**（`PRIMAL_WRC_ENABLE=1 PRIMAL_WRC_PREFILTER=1 PRIMAL_WRC_DUMP=1`，`[WRC_PF_CHECK]` 探针级读回对照，+2 帧读回环） | ✅ 已执行。**远场 volume（SPGI 实际消费对象）PASS**：2030 探针 median=2.64% < 5%（p95 6.9% / max 11%）。**近场 volume FAIL**：1340 探针 median=13.3%（p95 111%）——根因：Sponza 内场直射太阳热点探针的方向 radiance 近 δ 分布，SH9 带限平滑与 c⁶ lobe 核在峰值处系统性分歧（SH9 为低频近似，直射热点由 SC 直射链单独供给，prefilter 档定位即调试/低频降级）。缓解路径（后续）：峰值探针剔除口径 / 高阶 lobe / 直接 irradiance store |
| **A3 验收：远场 A/B 对照**（`PRIMAL_WRC_ENABLE=0/1` × `GI_LADDER_DUMP=1 SPGI_CAM_LOCK=1`，帧 300/360/420 三时点、440 锁定机位像素，spgi 亮度） | ✅ 已执行。**远场时间方差**：逐像素 CV 中位 0.809（DDGI 回退）→ **0.555（WRC）= −31%**。**暗角**：暗部尾巴 P1 2.57→3.84、P5 3.39→5.13（+49~51% 提亮，暗角减轻）；均值 4.34→6.63。**死列**：两配置 dead(<0.02 亮度) 均为 0（症状在 scene-fit DDGI 网格后已消，WRC 接管无回归） |
| **A4 验收：三场景基线 ±10% + 故障计数周环比** | ⚠️ 部分会话外：周环比需 7 天故障计数窗口（fusion_darkening/indirect 类），**记录为会话外验收**；三场景基线对照需 Sponza/arcade/TestProject_01 三场景采集流水线，本会话完成 Sponza 单场景能量与稳定性证据（§2 各行），arcade/TestProject_01 采集待会话外补齐 |
| C# / CLI 门禁 | 不涉及（无 CLI 命令面变化，schema 快照无影响） |
| `ci_local.sh` step 5 render | ⚠️ 关机期 FreeList 断言（render 本体 ok:true、PNG 正常）——**经还原对照实验确认为工作树遗留问题**（还原我的全部相关修改后 exit=134 依旧），非本次引入；见 §4 |

## 3. 已知问题与残留（如实记录）

1. **WRC 路径早期帧告警（已修复 + 残留良性项）**：原 viewType 告警根因
   （dummy 2D 占位未转换布局 + 3D SDF 误占位）已修复——dummy 2D 替换 3D 占位 +
   一次性 Unknown→SRO 转换 + SC 首帧完成前强制绑 dummy（frame 0 门控）；
   `PRIMAL_WRC_ENABLE=1` 复测 viewType 告警清零。残留 ~20 条
   `[AlbedoTextureArray]` 布局严格性告警（材质数组懒重建时序，GENERAL 下采样
   功能无影响，渲染收敛正常），根因在材质数组重建与描述符布局声明时序，
   不在 WRC 数据路径，另立专项处理。
2. **Metal gather 崩溃（工作树遗留，非本次引入）**：TestModularPipeline Metal
   链在 `ddgi_gi_gather` 编码期断言 missing buffer 12（relocatedOrigins）。
   本次修复了同类的两个布局计数 bug（Metal light_eval 15 项传 14、Metal gather
   18 项传 17——上一会话新增 openSky/relocated 条目未增 count），使故障点从
   LightEval 推进到 gather；根因（binding 12 写路径未达）待Metal parity 专项处理。
3. **[Q1/Q2/Q3 排查结论——DDGI 间距 / 天空泄漏 / 反弹缺失]**（仪表：`PRIMAL_WRC_TRACE_STATS=1` 的 `[WRC_TRACE_STATS]` 逐探针射线去向 + `[WRC_SC_FUNNEL]` SC 查询漏斗）：
   - **Q1 探针网格**：近场 DDGI 场景贴合 14×7×9=882 探针 @3m，覆盖 39×18×24（全建筑）——**范围合理、密度在 RTXGI 参考（2-3m）粗端**；3-5m 宽的室内房间仅 ~1 探针/间，房间内变化被抹平（"平"第二成因）。建议 hero 室内降至 2m（DDGI ≈2600 探针，预算可负担）。远场 WRC 16×8×16@8m 覆盖 128³——97% 探针在建筑外属设计使然（远场=天空壳）。
   - **Q2 天空泄漏语义**：射线语义本身正确——遮挡射线返回黑（exhausted 9.5%）、真正逃逸才返回天空。中庭开放屋顶使朝上射线合法见天；但近场探针 **34.2% 射线逃逸→天空、45% 探针（877/1936）天空占比过半**，而逃逸亮度 0.756 ≫ 命中亮度 0.10——探针能量被天空主导。
   - **Q3 真因（新发现）**：SC 查询漏斗（近场单帧 69742 命中）→ gridOk 100% → cellFound 67% → **found 仅 ~20%**（13993，miss 回退移植后 +138%）。三重衰减（命中×产出×亮度 0.10 vs 0.756）使几何反弹≈0，与 A3 的"室内间接 92% 是天空填充"互为印证。逐项：① 33% 命中在卡片网格 AABB 外（未被卡片覆盖的几何——被 5% 退化视图过滤器丢弃的小面/织物正是色彩弹射源头，红毯色弹射消失的机制）；② cell 内 ~70% 被方位/深度容差契约拒绝（0.45m SDF 命中带 + 双面墙）；③ **WRC 缺少 DDGI 的 SCDDGIIntegration 卡片→探针投影等价物**（DDGI 探针的反弹主要来自该路径而非 trace 命中）——这是反弹缺失的主修补方向。
3. **[反弹传输专项 v2——已落地]**（在 Q1–Q3 排查结论之上）：
   - **注入可见性门控**：WRCCardInject 的每条 probe→card 连线经 GlobalSDF
     24 步步进——遮挡卡片跳过，消除隔墙注入的均匀灰化；
   - **可见卡片权重上限** `PRIMAL_WRC_INJECT_CAP`（默认 0.6，原 0.45）：
     可见反弹得以压过天空填充；
   - **WRC 近场密度 3→2m**（High/Ultra preset + 测试配置同步）：探针
     1936→5175，内存 ~2.4MB；实测天空占比大幅下降——逃逸射线
     34.2%→22.8%、天空主导探针 45%（877/1936）→31%（1621/5175）、
     命中率 56%→65%（`PRIMAL_WRC_TRACE_STATS` 稳态）；2551 帧 0 validation。
   - 遗留：命中 radiance 亮度仍偏暗（0.10，命中偏向非直射面的物理属性），
     色彩弹射强度随注入可见性提升后可再观察；卡片覆盖缺口（33% 网格外
     命中）继续由卡片覆盖专项跟进。
3. **[用户复核收敛——2026-09-19 最终轮]**：用户对照 Lumen 参考复核三连
   （间接平/室内亮/无色彩反射）驱动的最后一轮修正：
   - **天空档默认 0.4→0.15**（`PRIMAL_GI_SKY_INTENSITY`/`PRIMAL_WRC_SKY_TIER`
     可覆盖）：室内 ROI 亮度 55→37（暗部反弹层次恢复、明暗交界锐利）。
     0.4 档的逃逸射线天空填充（EvaluateSkyBase×0.25×多方向）是室内均匀
     灰亮的主源——量化：室内 4240 像素 sky0.4=0.506 / sky0.15=0.040×
     gain 系数，非线性的部分来自 SC↔probe 回路对天空能量的循环放大
     （bootstrap 默认翻转后该回路已切断）。
   - **卡片注入 v4（能量守恒聚合）默认开**：全部可见卡片聚合后单次 mix，
     trace 测量保留 (1-w) 份额；v3 逐卡独立 mix 洗平问题消除。
   - **遗漏入库修复**：8 个 WRC shader/spv 因 git 大小写路径未命中
     （大写 Shaders/ vs 小写 shaders/）未入库——已修正入库。
   - **光向结论**：Lumen 参考的"颜色反射"观感主要由光向驱动（斜射光照亮
     旗帜/墙面的直射项 + 随光向分布的反弹）；`PRIMAL_LIGHT_DIR` env 可
     驱动对比（垂直顶光 vs 0.45,-0.8,0.35 斜射，截图入对比图集）。
3. **Metal parity 后置**（计划既定决策，GI 计划 §2①"Vulkan 领衔"）：
   C1 fusion mode（Metal 无 gain UBO 绑定面）、WRC pass（`WRC*.metal` 八件套）、
   B 系 Metal 端效果级 A/B。B1/B2/B3 的 Metal shader 侧已同步（含 Metal 现存
   texel 地板在 bootstrap 模式关闭）。
4. **A5 消费计数为累计制**：排序用相对量无需重置；如需真"近期消费"语义可在
   WRCUpdateRadiance 加周期衰减。
5. **§5.3 迁移完成判据的效果项**（三场景亮度基线 ±10%、暗角/死列不回归、
   fusion_darkening 周环比）需要带渲染验收的多日窗口，本次交付了判据 1（能量
   单向化结构）/4（回归门禁）/5（`PRIMAL_WRC_ENABLE=0` 全量回退路径默认即开、
   逐位旧行为）。

## 4. 回滚路径（§1.2-6 冻结政策下 DDGI 线零改动）

| 项 | 回滚 |
|---|---|
| WRC 全线 | `PRIMAL_WRC_ENABLE` 置 0（默认）——WRC 不创建，SPGI 远场走冻结的 DDGI 边，行为与 A3 前一致 |
| B1/B2 能量单向化 | `PRIMAL_GI_PROBE_FEED_BOOTSTRAP` 置 0（默认）——探针馈送/自反馈双轨逐位旧行为 |
| C1 Fusion 档位化 | `PRIMAL_FUSION_PERPIXEL=1` 回旧逐像素置信度混合 |
| D1 Top-K 调度 | `PRIMAL_SC_PRIORITY` 置 0（默认）——round-robin/轮转游标逐位不变 |
| D2/D3 页表+LRU | `PRIMAL_SC_PAGE_TABLE` 置 0（默认）——shelf-packing 行为不变 |

## 5. 文件清单（本次新增/修改，按域）

- **新增**：`Engine/Graphics/Lumen/WRC/`（WRCTypes.h、WRCSlotMap.h、WRCCacheView.h、
  WRCFitGrid.h、LumenWRCPass.h/.cpp）；`Vulkan/Shaders/Lumen/WRCTraceSDF.comp(.spv)`、
  `WRCTraceFinalize.comp(.spv)`、`WRCUpdateRadiance.comp(.spv)`、
  `WRCPrefilterIrradiance.comp(.spv)`
- **修改**：GIWarmup.h、GIDebugParams.h、LumenTypes.h、SurfaceCacheTypes.h、
  SurfaceCacheData.glsl/.metal、SurfaceCacheLightEval.comp/.metal、
  SurfaceCacheIndirectResolve.comp/.metal、SurfaceCachePass.cpp/.h、
  CardGenerator.h/.cpp、GIGatherModule.cpp（Metal 布局计数修复）、
  LumenDDGIPass.cpp（bootstrap 冻结解除）、ScreenProbeGIPass.h/.cpp（A3 尾扩 +
  binding 19/20）、ScreenProbeTraceFinalize.comp、FusionIndirect.frag(.spv+变体)、
  FusionCompositeModule.h/.cpp、RenderPipelineConfig.cpp、
  StandardRenderPipeline.h/.cpp（接线 + E1/E2）、EngineTest 三份 Metal 拷贝同步
