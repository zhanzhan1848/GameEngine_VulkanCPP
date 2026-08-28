# GI 系统优化总体计划（架构 / 效果 / 接口）

> 日期：2026-08-24（2026-08-26 增补：M7/F 系列融合轮 + §1.1 现状修订，增补部分待评审）
> 状态：已评审定稿（决策记录见 §2）
> 范围：Lumen GI 体系（DDGI / SurfaceCache / ScreenProbe / SSGI / Fusion / GIGather）
> 关联文档：`Docs/2026-08-09-vulkan-lumen-gi-port-design.md`、`Docs/2026-08-19-rhi-vulkan-metal-parity-plan.md`、`Docs/2026-08-21-virtual-texture-system-design.md`、`Docs/2026-08-26-spgi-lumen-screen-probe-fusion.md`（M7/F 系列机制细节与外部依据）

---

## 1. 背景与现状结论

### 1.1 现状盘点（与 Unreal Lumen 三支柱对照）

| 支柱 | 现状 | 差距评级 |
|---|---|---|
| Surface Atlas + Irradiance | AABB 6 面 card + 屏幕回投 gather（离屏覆盖 ~0.1%）+ 标量自反馈；真 radiosity（IndirectTrace/Resolve）已写好但未接线 | 骨架在、闭环弱 |
| World-Space Probe + Radiance | 单 volume 32×16×32 SH3 **irradiance**（L0+L1 稳定），无 clipmap、无远场、origin 锁定 | 类别级差异 |
| Screen-Space Probe + Gather | 全链路实装但默认关闭；T2.2/T5.1/T5.2/T5.3 已合入（混合 trace、miss→DDGI fallback、temporal 重投影、Denoise 启用）；仍缺收敛与远场语义：重要性采样、逐帧抖动、远场缓存提前停止、adaptive 网格（→ M7/F 系列） | 链路在，收敛/远场缺 |

三模块现状是"并行信号源 + Fusion 手工加权"，缺 Lumen 的 `SC → World Probe → Screen Probe → 像素` 带层级 fallback 的传播拓扑。

### 1.2 硬约束（全程不变量）

1. Apple Silicon：≤32 次 device 读/线程、texture3D ~4 次/线程、texture3D 与 buffer 混读需拆 dispatch；
2. 三端（Vulkan GLSL / Metal / Dawn WGSL）算法语义一致，质量参数允许分歧（见 §2 决策①与 §3 PlatformProfile）；
3. RHI 现状无 bindless/sparse（`VulkanDevice.cpp:990` "Phase 5+"），M6 前不引入依赖；
4. FPS ≥ 30 硬底线（决策②），效果增量必须带测量与降级路径。

### 1.3 设计语言：五轴框架

GI 算法分解为五条正交轴 + 输出端，作为新算法推导与模块组织的统一语言：

```
空间缓存 × 方向参数化 × 光学量/积分时机 × 更新传播(含拓扑) × 时域策略 → [Gather → 像素]
```

已知轴间硬耦合（设计时显式建模，不可忽略）：
- 方向参数化 × Gather：probe 处积分（irradiance 可）vs 像素处积分（需 radiance + 高方向密度）；
- 缓存 × Gather：Per-Pixel Raytrace 要求点查询语义（grid 索引），probe 类只支持邻域插值；
- Gather 权重函数的支撑集区分各派 gather（probe 邻域 / 方向命中集 / surfel 邻域）。

**落地原则：轴化接口、具体化管线**——轴用于组织 shader 库、模块边界、配置 schema；每档组合仍是硬编码优化的管线，不做运行时任意拼装。

---

## 2. 决策记录（已定稿）

| # | 决策 | 影响 |
|---|---|---|
| ① | **Vulkan 领衔开发，但 API 对齐**；shader 行为可按平台特性（硬件/支持/限制）调整，**不必完全一致** | GLSL 为 canonical 源；对齐的是算法语义，分歧的只能是质量参数（PlatformProfile 承载，见 §3） |
| ② | trace 成本可增加，但 **FPS 最低 ≥ 30**，不得影响功能完整性/稳定性/实时性 | 每个效果任务附带三端 GPU time 增量；帧率驱动自适应调节器（T4.1）随首个增量任务交付 |
| ③ | **补 SPGI 的 WGSL 端口**（对齐优先） | T5.4，Dawn 档位 SPGI 可开 |
| ④ | 硬件 RT **暂不考虑**（无支持设备） | M6 不含 RT 评估 |
| ⑤ | **UI 相关最后实现** | C2 全部移入 M6；多 volume 运行时仍在 M5 主线（配置文件驱动） |
| ⑥ | 多 volume 上限 **4** | A5/T5.5 锁定 |

---

## 3. PlatformProfile 机制（决策①的落地形式）

**允许的分歧**（质量/性能参数）：sdfTraceSteps、raysPerProbe、maxProbesPerFrame、pageBudget、spgiEnabled/ssgiEnabled、splitDispatchPreferred（后端组合策略，算法不可见）等。

**禁止的分歧**（算法语义）：radiance 计算模型、SH 投影/clamp 数学、EMA 语义、网格索引、序列化格式。这些由单一 GLSL 源定义；现存的 DDGI trace 两版 radiance 模型（Metal: SC atlas 采样 vs Vulkan: 自算直接光+probe grid 反弹）必须归一为一条可分级链：**SC atlas 命中 → 前帧 probe grid 多反弹 → 天空兜底**（T3.1）。

```cpp
struct GIPlatformProfile {
    // 静态能力位
    bool textureArrayDynamicIndexing; u32 texture3dReadBudget; u32 bufferReadBudget;
    // 质量默认档（可被自适应调节器在允许范围内调制）
    u32 sdfTraceSteps; u32 raysPerProbe; u32 maxProbesPerFrame; u32 pageBudget;
    bool spgiEnabled; bool ssgiEnabled; bool splitDispatchPreferred;
};
```

- 载体：参数走 UBO（`DDGIVolumeData` 已有先例）；不引入新的编译期机制；
- **算法一致性测试**：三端注入同一份 Profile（参数 override）→ 输出容差内一致（CI 硬检查）；
- **发布档位**：各平台默认 Profile 不同，不做跨平台像素对比。

---

## 4. 三条轨道

### Track A：架构

| 编号 | 内容 |
|---|---|
| A1 | 死资源与死路径清理：`prev_lighting_atlas_[3]`（~100MB，已核实测试不依赖——测试用的是 `GetLightingAtlas(histIdx)` 别名）、`light_assignment_buffer_`（~64MB，待终核 getter）、emissive 评估；`if(false)` CardFill 与 Metal 死 capture 管线收敛；死配置字段激活或删除 |
| A2 | 共享 shader 库（GLSL primary）：trace 库（traceSDF_relaxed / traceRayHZB）、encode 库（SH 求值/投影、八面体）、时域库（EMA、variance clamp、reprojection）；Metal/WGSL 移植 checklist 进 PR 模板 |
| A3 | Cache View 接口标准化：每缓存模块暴露 Write(trace 注入)/Sample(点查询或邻域插值)/Temporal(策略注入)，轻量接口 + RG 读写声明 |
| A4 | 传播拓扑显式化：SC→DDGI→SPGI 喂食/fallback 边在 RenderGraph 声明依赖；Fusion 权重按拓扑推导 |
| A5 | 多 volume 数据模型（上限 4）：volume 列表、SceneDataAdapter 接入、SPCH v4、GIGather 两段式选 volume（读数预算不变） |
| A6 | 配置 Schema 轴化：PipelineQualityConfig → 五轴选择 + 预设命名映射 |

### Track B：效果（依赖排序）

| 编号 | 内容 | 依赖 |
|---|---|---|
| B1 | 接线与调度：SC radiosity 接入引擎 Execute；SPGI miss→DDGI fallback；card 线性扫→uniform grid（O(32)→O(1~3)）；LightEval 256 页/帧预算 | 无 |
| B2 | SC 正确性：真实光源列表+阴影（split-pass：纯 texture3D 可见性 + 纯 buffer 光照）；emissive；Metal 正交 capture | B1④ |
| B3 | DDGI 表达力：ray 64→128（调度化）；SH9 L2 激活；反射向量求值+preintegrated DFG（光泽反射）；远场第二层粗 volume | B2 |
| B4 | SPGI 升级：混合 trace（HZB 近场+SDF 远场，按读取类型拆 dispatch）；velocity reprojection 移植；两级自适应网格；Denoise 修复启用 | B1② |
| B5 | Fusion 统一：Vulkan `FusionIndirect.frag:72` 补 SPGI 混合；Metal/Vulkan 权重对齐；随 A4 拓扑推导 | A4 |
| B6 | 时域统一：EMA/reprojection/variance-clamp 从 A2 时域库统一供三处使用 | A2 |
| B7 | SPGI×Lumen 融合轮（F1-F5，见 §5 M7）：远场提前停止+缓存查询、轻量重要性采样、temporal 双线性+variance clamp、adaptive 网格细化、探针/射线逐帧抖动 | B1②；WGSL 侧依赖 T5.4 |

### Track C：接口暴露

| 编号 | 内容 |
|---|---|
| C1 | 引擎 API 面：GI 收敛为单一 facade（初始化/每帧/volume 列表/能力查询） |
| C2 | 编辑器接口（M6）：①参数面板+Bake 按钮+probe 调试可视化（`DDGIProbeAtlas` 占位升级）；②viewport gizmo |
| C3 | 调试接口：`DawnDebugParams` → 三端统一 `GIDebugParams`（逐 pass 开关/权重热调/freeze frame） |
| C4 | 质量预设接口：FromPreset ↔ 轴 schema 映射；帧率驱动降级 |
| C5 | 后端能力上报：并入 §3 PlatformProfile 静态能力位 |

---

## 5. 里程碑与任务清单

### M1：清障 + 性能基线

| 任务 | 文件 | 验收 |
|---|---|---|
| T1.1 删 `prev_lighting_atlas_[3]` | `SurfaceCachePass.h:96-98`、`.cpp:270-271` | 编译过、测试绿、显存 -100MB |
| T1.2 `light_assignment_buffer_` 终核+删除 | `SurfaceCachePass.cpp:277-292`、`.h` 成员 | 同上，-64MB |
| T1.3 card uniform grid 索引 | 新 `SurfaceCacheCardGrid.glsl` + Metal/WGSL 端口；`CardGenerator.cpp` 建网格；消费者 `DDGITraceFinalize.comp`、`ScreenProbeTraceRays.comp` | 单命中读取 ≤3 次；trace 耗时下降 |
| T1.4 LightEval 页预算调度 | `SurfaceCachePass.cpp:1174-1224`；激活 `LumenTypes.h:47` `capture_budget_pages` | invocations 16×↓；画面无回归 |
| T1.5 逐 pass GPU time 基线表 | RHI timestamp 查询（先核查现有支持，无则补最小实现） | 三端 × Sponza 基线表入库 |

### M2：闭环

| 任务 | 文件 | 验收 |
|---|---|---|
| T2.1 SC radiosity 接线（Execute 替换 TODO；LightEval 反馈切 indirect atlas，标量反馈保留为降级路径） | `SCDDGIIntegrationModule.cpp:84-88`；参考 `TestNaniteStreamingPipeline.cpp:4647-4760`；`SurfaceCacheLightEval.comp:127-138` | 多反弹可见；反馈链有 RG 依赖边 |
| T2.2 SPGI miss→DDGI fallback（GLSL 先行） | `ScreenProbeTraceRays.comp` + `.metal`/`.wgsl`；数据源 `GetIrradianceBuffer` | miss 不落天空常量；≤20 读/线程 |
| T2.3 拓扑显式化 + Fusion 修复 | RG 依赖边；`FusionIndirect.frag:72` 补 SPGI；Metal 权重对齐（`DeferredLighting.metal:615`） | 同参数 override 下三端一致 |

### M3：正确性 + shader 语义归一

| 任务 | 文件 | 验收 |
|---|---|---|
| T3.1 trace 算法 canonical 化：归一 radiance 链（SC→probe grid 反弹→天空，逐级可开关）；Metal split 降级为后端组合策略 | `DDGITraceRays.comp` 重写；`SDFTraceCommon` GLSL/Metal 合并；Metal split kernels 重端口 | 同 Profile override 下两端一致；分歧清单清零 |
| T3.2 真实光源列表 | `SurfaceCachePass.cpp:917-919`；`light_info_buffer_`（已预留 256 光源） | 多光源 SC 正确 |
| T3.3 SDF 阴影 split-pass | 新 kernel 对（GLSL 先行）；LightEval 消费可见性 buffer | SC 直接光有影子；各 dispatch 读数合规 |
| T3.4 emissive 接入 + Metal 正交 capture | 材质 emissive→capture 写入；`SurfaceCachePass.cpp:746-776` 激活或删除 | 自发光照亮间接；离屏覆盖提升 |
| T3.5 共享库落地（A2+B6） | trace/encode/时域三库 | 三 pass 族共用同一实现 |

### M4：表达力 + 自适应调节器

| 任务 | 内容 | 验收 |
|---|---|---|
| T4.1 帧率驱动自适应调节器：滑动平均帧时+滞回；调制 ray 数/probe 更新量/页预算/SPGI 网格档 | 挂 `PipelineQualityConfig` + PlatformProfile | 压测 30fps 锁底、降档恢复不震荡 |
| T4.2 ray 64→128 + SH9 L2 激活 | `LumenTypes.h`；采样端 9 系数（GIGather 已具备） | 无 SH 振铃；增量在预算内 |
| T4.3 光泽反射：反射向量 SH9 + DFG LUT，smooth 段与 SSR 分 roughness 接力 | GIGather 副输出或新 gather；Fusion 消费 | 金属粗糙度梯度连续过渡 |
| T4.4 远场第二层粗 volume（16×8×16 @ spacing 8） | `LumenDDGIPass` 第二实例；两段式选 volume | 走出近场盒仍有 GI |
| T4.5 接口三件套：GIDebugParams 三端统一 / 能力位 / 预设映射 | C3+C5+C4 | 三端同一旋钮生效 |

### M5：屏幕层 + WGSL + 多 volume

| 任务 | 内容 | 验收 |
|---|---|---|
| T5.1 SPGI 混合 trace（HZB 近场+SDF 远场，拆 dispatch） | 复用 T3.5 库重构 `ScreenProbeTraceRays` | 近场细节提升；读数合规 |
| T5.2 SPGI velocity reprojection | `ScreenProbeTemporal` ← `SSGITemporal.metal:45-212` | 运动时无拖影 |
| T5.3 两级自适应网格 + Denoise 启用 | `ScreenProbePlace`；移除 bypass（`ScreenProbeGIPass.cpp:894-900`） | 方差驱动密度；denoise 生效 |
| T5.4 SPGI 全链路 WGSL 端口 | `Engine/Graphics/Dawn/shaders/Lumen/` 7 个 kernel | Dawn 档 SPGI 可开；算法一致性测试过 |
| T5.5 多 volume 运行时（上限 4，配置文件驱动） | volume 列表；SceneDataAdapter；SPCH v4；移除 `StandardRenderPipeline.cpp:576` 硬编码 | 双 volume 场景验证 |
| T5.6 SPGI 默认开启（High+） | `PipelineQualityConfig.h:107` 预设 | 默认档 30fps 达标（T4.1 兜底） |

### M6：UI + 可选基础设施

| 任务 | 内容 |
|---|---|
| T6.1 | volume 参数面板（origin/spacing + 热重载）+ Bake 按钮（`StaticProbeBaker`）+ probe 调试可视化 |
| T6.2 | viewport gizmo（拾取/平移/缩放） |
| T6.3（可选） | Vulkan descriptor indexing → tile-array SC → card LOD；仅当 M4-M5 后仍有质量缺口时立项（关联 `Docs/2026-08-21-virtual-texture-system-design.md`） |

### M7：SPGI × Lumen Screen Probe 融合轮（2026-08-26 增补，待评审）

与 UE Lumen screen probe（2021/2022 公开方案）对照后新增的 F 系列；机制细节、读数红线核算与外部依据见 `Docs/2026-08-26-spgi-lumen-screen-probe-fusion.md`。执行顺序按该文档 §6：**F5 → F1 → F3 → F2 → F4**（按改动成本与收益排序）。

| 任务 | 内容 | 验收 |
|---|---|---|
| F1 远场提前停止 + DDGI 缓存查询 **（✅ 2026-08-26 已实现）** | SDF march 至 `cache_start_distance` 即停（hit flag=3），Finalize 4-probe 四面体 L0+L1（16 读，优于规划的 8-probe 降档）；可选 parallax（DDGI irradiance w 通道语义定义为平均距离）——parallax 未做 | SDF 步数/GPU time 较 T1.5 基线下降（待 T1.5 基线落地后量化）；快速旋转边缘光斑减少（待录屏对比）；编译/构建/设备冒烟/validation 计数一致 ✓ |
| F2 轻量重要性采样 **（✅ 2026-08-26 已实现，直落形态 c）** | `spgiImportanceDirection`（EncodeLib，Metal 双份）：q ∝ cosine × SH-luminance 逆 CDF 重采样，权重 cosθ/(N·q) 无偏；五 kernel +1 只读 SSBO（上帧最终 SH）；`screen_probes_importance` 默认开 | 编译/构建/设备冒烟/validation 一致 ✓；同帧数收敛提升与均匀场景无偏回归待录屏量化 |
| F3 Temporal 4-probe 双线性 + variance clamp **（✅ 2026-08-26 已实现）** | 勘误：2×2 双线性 T5.2 已有；本项实装邻域 AABB 方差钳制（TemporalLib::varianceClampAABB，L1 随 L0 等比收缩，31/32 读）+ GITemporalStrategy 枚举真实接线（EMA=默认不变 / EMAVarianceClamp / Reprojection 三模式经轴 5 切换） | 编译/构建/设备冒烟/validation 一致 ✓；旋转拖影与静态漂移量化待录屏 |
| F4 adaptive 网格实现细化 **（✅ 2026-08-26 已实现）** | UE 式插值失败测试驱动（4 个棋盘边邻粗档 probe 双线性重建误差 > 阈值激活，天空邻居直接判失败；`screen_probes_adaptive_pos_error` 默认 0.35，0 回退方差启发式）；读数 8→4 | 编译/构建/设备冒烟/validation 一致 ✓；栏杆/薄几何漏光改善待目检 |
| F5 探针/射线逐帧抖动 **（✅ 2026-08-26 已实现）** | Place 网格内 hash 偏移（世界幅度 ≤ 0.2 钳制）+ Fibonacci 方向集逐帧随机旋转（EncodeLib 共享库，Metal 字节等价） | 编译/构建/设备冒烟 ✓；banding 消除待人工目检（录屏对比） |

### M8：单位语义归一 + probe 重定位（2026-08-27 增补，Vulkan 端已落地）

背景：`DDGIUpdateIrradiance` 存储的是 **radiance SH**（mcWeight = 4π/N，无余弦投影），而屏幕端 GIGather 与 SC 侧都直接当 irradiance SH 求值——各向同性项天然偏暗 π≈3.14 倍。直接把 SC 侧（`SurfaceCacheLightEval.gatherProbeIrradiance`）乘 π 会使 SC↔probe 互持环每周期增益 >1（该函数 clamp 注释记录过实测事故：高反照率墙面数分钟内发散 → probe 被 NaN 清洗成黑）。安全修法是**成组改**：屏幕 gather ×π + `ddgi_light_color` ÷π（probe trace 的输入光随之收缩，环路绝对驱动不变，只修正屏幕端尺度）。SC 侧数学**保持不动**。

| 任务 | 内容 | 验收 |
|---|---|---|
| T8.1 π 成组改 **（✅ 2026-08-27 Vulkan 已实现）** | `DDGIGIGather.comp` 新增 `PROBE_SH_RAD_TO_IRRAD=π`，三处 diffuse 输出（N-volume/legacy 远/近）应用；spec 通道保持 radiance 口径不缩放；环境 floor 保持绝对值（先 spec 后缩放的写点重排）；`TestVulkanSponzaRenderGraph` 场景 `ddgi_light_color` {1,0.95,0.9}÷π。**红线：禁止在 `SurfaceCacheLightEval.comp::gatherProbeIrradiance` 复制该因子**（文件内已加交叉引用注释） | Sponza 稳态帧（361）分区实测：左 GI 区 mean 12.1→17.1（+41%），p95 25.7→38.9；右直射区 mean 33.8→38.0（+12%），全图 clipped≥250 占比 0%，max 247 未触顶 |
| T8.2 probe 重定位（射线起点） **（✅ 2026-08-27 Vulkan 已实现）** | `DDGITraceRays.comp`/`DDGITraceSDF.comp` 新增 `ddgiRelocateProbeOrigin`：埋进墙体的 probe 射线起点沿局部 SDF 梯度推出 hit-bias 皮肤层，位移上限 0.45×spacing（gather 侧网格距离可见性测试保持有效；级联外 1e10 早退）。确定性、静态场景下深度图逐帧一致；SH 存储与 gather 仍按网格索引 | 二层拱廊带（左上）：p95 35.7→37.1、max 94.9→112.6（黑尾转向受光面）；1800 帧晚期无发散、无过曝；用户目检拱廊柱/布料 GI 待确认 |
| T8.3 probe 位置球体可视化 **（✅ 2026-08-27 Vulkan 已实现）** | `ProbeVisMode::ProbePositions=4`（`SPGI_PROBE_VIS=4`）：GIGather 内逐像素 DDA + 射线-球求交，零新增绑定（复用 irradiance/depth SSBO）。绿→黄 = SH L0 能量对数阶，红染 = 埋没（octahedral 4 tap 平均深度 < 0.25×spacing），无命中 = 暗品红底；球 ×6 能量穿透融合的 `max(ddgiBase,spgiTarget)`。**注意 fusion 只替换 GI 输入，直射/SSGI 仍叠加**。已知局限：不做场景遮挡（穿墙可见）；mode 1-3 的输出同样被 SPGI 压暗，判读需按此折扣 | 首次运行即定位出：左二层走廊 probe 存在且未埋没但 L0 能量显著低于大厅（数据饥饿而非权重问题）；地板下层 probe 整排落在楼板实体内（红染带） |
| T8.4 probe 射线采样框架 **（✅ 2026-08-27 Vulkan 已实现）** | 新建 `ShaderLib/ProbeSampleLib.glsl` 单一来源：`ddgiSampleRayDirection → vec4(dir, pdf)`，SH 投影按 1/(N·pdf) 归权（pdf 做基础，任意 pdf 正确的采样器无偏）。三模式经 UBO 新槽 `RaySampleMode`（offset 108，原 padding，结构体大小不变，Metal/Dawn 前缀兼容）+ env `DDGI_RAY_SAMPLE`（默认 0 零行为变化）选择：0=固定 Fibonacci（legacy 位等价）/ 1=逐帧黄金角 jitter（SPGI F5 先例，EMA 时域积分方向全集）/ 2=围绕光源 cosine 混合（50/50，混合 pdf 下限 1/(8π)）。五个消费者全部单源化：TraceRays / TraceSDF / TraceFinalize / UpdateIrradiance / **UpdateDepth（八面体分箱按索引反推方向，必须同步，否则 jitter 下距离落错 texel）** | mode0 回归：稳态 361 帧与改前平均差 0.4/255（时间链对 ULP 种子的正常放大，无结构变化）；mode1：L_2F 17.49→17.86、p95 36.2→38.7（早期温和收益，EMA 完全收敛需更长帧数）；**mode2 实测把 SC↔probe 环平衡点抬高 2.4×（L_2F 17.5→42.5，clipped 0.005%）——单步投影无偏，但反馈环平衡点随方向分布移动，标记实验性**，默认 0，启用需重做 :127 增益核算 |
| T8.5 真 mesh probe sphere 可视化 **（✅ 2026-08-27 Vulkan 已实现）** | 按评审意见走标准 mesh 路径：CPU 展平平顶点流（每 probe 60 顶点 = pos+normal+probeIdx，98 万顶点 / 27MB，渲染循环前从网格定义一次生成）+ 单 VB 非索引 `Draw` 全量无 culling + push constants 传 view_proj。模块 `DebugProbeSphereModule` 注入 RWCB FinalBlit 后（RG 显式 Read blit 输出节点定序）。env `SPGI_PROBE_SPHERES=1`。**排障过程揪出四个 RHI 级坑**：① `gl_InstanceIndex` 在本 wrapper+MoltenVK 组合恒 0（16384 实例全部叠画在 instance-0，"隐形"假象的根源——已彻底弃用实例化，改平顶点流）；② `GPUMemoryUsage::Static` 是设备私有显存不可 Map（`VK_ERROR_MEMORY_MAP_FAILED`），CPU 填充必须 Dynamic；③ PipelineLayout 漏声明 push constant range 时着色器 push 块静默读回零矩阵（w=0 全裁剪，无 validation 报错）；④ depth-as-SampledImage 触发 usage 病理 ~8s/帧（遮挡暂禁，需独立 sampleable view）——①②③建议进 RHI 已知问题清单 | **已实测**：满屏品红球（33.7% 像素）按 3m 网格落位、随 probe 状态着色的接口就绪（probeIdx 已随顶点流出，接 irradiance/depth SSBO 即可恢复 L0 热阶+埋没红染）；遮挡恢复后排期 |
| T8.6 拱廊传输修复（jitter 默认化 + 重定位升级）**（✅ 2026-08-27 Vulkan 已实现）** | ① `RaySampleMode` 默认 0→1：逐帧黄金角 jitter，均匀 pdf 不变估计量，EMA 时域积分方向全集——走廊 probe 的固定方向集盲区（64 根射线凑不巧全打暗墙、错过拱门开口）随时间消除；② `ddgiRelocateProbeOrigin` 升级经典多轴最深自由点搜索（6 轴+8 对角×3 轮，自由 probe 单次采样早退，仅埋没 probe 付出 ~250 次采样）：旧单轴梯度推只会把 probe 停在最近表面（厚墙/楼板内推不进房间），新版找到真实自由空间。SC 侧置信度加权（剔除黑邻居稀释）需跨 pass 加绑定，另行排期 | **左上二层带（用户报告偏暗区）稳态 361 帧：mean 17.54→26.83（+53%）、p95 37.1→64.3（+73%）、max 112.6→160.1，clipped 0%。面向直射区的走廊柱/布料亮度抬升显著；SPGI_PROBE_SPHERES=1 可视化确认走廊内 probe 存活分布** |
| T8.7 Fusion 源隔离 A/B + SPGI 底光压制修复 **（✅ 2026-08-27 Vulkan 已实现）** | ① `GI_ISOLATE=ssgi\|ddgi\|spgi` 编译期变体（FusionIndirect 三个 `-D` 版本，测试自动选择）——一条命令定位"哪路 GI 没干活"；② 实测（左上二层带稳态）：SSGI 贡献=0（该区输出本就弱）、DDGI ≈0（±噪声内，probe 场 +53% 的改善在像素绝对量级上低于 run-to-run 噪声）、**SPGI 净 -1.91**；③ 机制定位：天空漏光底被 `floor×(1−0.5·spgi_w)` 按置信度减半——置信度高≠有光，拱廊自信黑照样压底光。修复：底光为远场相加路径，移除按置信度/亮度的压制（`max(ddgiBase, spgiTarget)` 已处理近场冗余），净 +0.26/255 | **左上二层带全 session 演进：17.54 → 27.22（+55%）；右侧直射区 34.3 持平、clipped 0%。残余差距：no-SPGI 天花板 28.87 与当前 27.22 间 ~1.6 来自 SPGI max() 替换内容本身（其在该区的估计与 ddgiBase 相当但替换了色度）**——由 T8.8 收口 |
| T8.8 SPGI 替换亮度优势裕量 **（✅ 2026-08-27 Vulkan 已实现，结论被 T8.9 部分修正）** | `FusionIndirect.frag`：替换权重改 `spgi_w × replaceW`，`replaceW = smoothstep(1.10, 1.35, lum(spgiTarget)/lum(ddgiBase))`——亮度平价及以下完全保持 DDGI（含色度），≥+35% 才全量 max() 替换；置信度仍为乘子。同轮把三个 `GI_ISOLATE` 变体编译收编进 `build_spv.sh` | 同矩形读数 L_2F mean 28.62→29.00（重复运行噪声 ±0.07）。**后续排查修正**：带内对 no-SPGI 天花板的逐像素"变暗"分布是纯椒盐噪声（暗部抖动大、均值稳定）；且代数上 `mix(a,max(a,b),w) ≡ a + max(b−a,0)·w` 恒不低于 ddgiBase——融合门控层已无油水，剩余统计差是 post-chain 非线性下的跨运行歧义。真正的瓶颈在下游 T8.9 的源分解 |
| T8.9 远场底光抬升（左二层结构性缺光的表示层补位） **（✅ 2026-08-27 Vulkan 已实现）** | 新增 `GI_ISOLATE=all` 变体做源分解：带内 direct+floor 基座 ≈28.60，DDGI 边际仅 +0.95、SPGI 单独 +0.29——**探针/屏幕 GI 在厚墙拱廊内物理性饥饿（合计 <1.3/255），任何融合层调整都只是搬运这点能量**。走廊应吃到中庭亮墙二次反弹，缺的是 SC 卡覆盖走廊内表面/探针间传输（另立任务）。本轮把远场底光当作缺失高阶反弹的表示层替身抬到有意义的量级：`vec3(0.10,0.11,0.13)` → `vec3(0.17,0.18,0.21)`（×1.7，暖化向石墙反弹色）。直射区相对增量不可见（基座项占优）；回滚=还原该常量 | L_2F mean 29.00→**33.97**、p95 70.46→76.17（session 内首个目视可辨的整带抬升）；R_DIRECT 62.06→67.10（绝对等量、相对仅 +8%）、max 227 未触顶、clipped 0%；目检拱顶暖色反弹自然、明暗对比保留。**测量矩形定标**（1280×720，此后沿用）：L_2F x[40,560] y[30,320]，R_DIRECT x[840,1060] y[130,350]；跨运行均值噪声 ±0.07 可信，暗部逐像素差分是 ±4+ 抖动只看均值 |
| T8.9b 左拱廊外侧面诊断（用户指认：朝中庭面正对直射区，问题最明显） **（✅ 2026-08-27 已定位根因，修复另行排期）** | 用户澄清后用 `DDGI_DUMP=1` 探针 SH 回读（新增测试工具：`GetVolumeView().irradiance` 经 staging CopyBuffer 全量导出 CSV）+ 三组隔离实验定位。证据链：① 外侧面条带（L_OUTER x[100,520] y[80,440]）DDGI 边际仅 **+0.57/255**；② gather 端 visibility/normal/burial 惩罚全关只找回 **+0.93** → 接收端权重非主因；③ **探针内容本身缺跨庭能量**：中庭内部探针 lumL0≈0.01–0.07 vs 建筑外≈0.30，且 ±x 方向不对称性 ≤0.05——对面直射亮墙的能量在探针场里不存在；④ 机制：`DDGITraceRays` 命中辐射用**无阴影解析 NdotL×全局太阳方向 {0.5,-0.7,0.3}**，而本场景真实阳光只能穿过 top clerestory（DeferredLighting 有阴影图）：探针世界观里"左墙被直射、右墙背光"，与屏幕端真实光照分布**反向**，多级反弹反馈环进一步强化错误场；⑤ 更新调度按相机距离优先，距相机远的探针长期 stale（同列相邻 z 3m 出现 7× 差值，连建筑外也有）；⑥ 新增 `DDGI_DUMP` 与 `GI_ISOLATE=all` 为常驻测量设施 | 实验读数：EXP-1（权重全关）L_OUTER 29.29→30.22；vis4 ×40 放大下中庭球体青绿色、右带黄绿、左外红染。**勘误与新知**：同 shader 不同 binary 链接后整条 350 帧时域链会放大量化差异至 L_2F ±1.1（恢复验证时发现），故 <1 的跨运行结论一律需同 binary A/B；此前"T8.8 后 SPGI 净拖 −0.55"属该类噪声。**T8.10 候选方案**：命中辐射改采样 Surface Cache（遮挡正确、与 Deferred 同一套光照真相），辅以全探针保底刷新节奏（消 stale） |
| T8.10 探针命中路径量化 + 根因收窄 **（⚙️ 2026-08-27 进行中——仪器落地，修复待下一轮）** | 实施前先量化"三路命中辐射（SC 图集/上帧屏幕色/解析兜底）各占多少"：新增 `DDGI_PATH_DEBUG` 编译变体（finalize 在 result.w 距离段 ≥40 平滑权重恒 1 处打路径标签，辐射语义无损）+ 测试 `DDGI_DUMP=2` 沿 probe_update_list 回读 ray_buffer（跳过陈旧槽位）；加载端 `DDGI_PATH_DEBUG=1` 时 pass 优先取 `*_pathdbg.comp.spv`（独立文件名随仓库走，规避 POST_BUILD 整目录搬运互相覆盖的坑）。**已锤死的新事实**：① 拆分路径（TraceSDF+Finalize）在 Vulkan 确认派发；② NAVE/L-OUTER 探针的射线命中距离大面积**恰好为 0**（wmin=wmax=0.000，64/64 全零），即 TraceSDF 认为探针附近整片区域"在体内"→ Finalize 全部按 d=0 命中走 SC-miss→屏幕自采样→内容≈周围自色暗场；③ 该症状离线 SDF 与运行时 GlobalSDF 均复现（SPGI_NO_OFFLINE_SDF A/B），排除烘焙特异性 ⇒ 收窄到 SDF 纹理注册/采样映射（cascade origin/extents/voxel 与消费端 UBO 的对账）。全帧回读时混入未写槽位会让统计全错——必须按 update list 过滤（已在工具内固化） | 工具链就绪：`DDGI_PATH_DEBUG=1 DDGI_DUMP=2` 一条命令出 `/tmp/ddgi_ray_paths.csv`；默认构建行为已验证无损（恢复后 L_2F 33.90/R 67.12 与 T8.9 一致）。**下一步**：对账 TraceSDF 的 SDF 纹理坐标计算 vs GlobalSDF/offline bake 的 origin+extent+voxelSize（重点查 y 轴与半 voxel 偏移），修好后用同一工具验证 dist 分布应呈连续谱而非 0 团块，再回归 L_OUTER DDGI 边际（当前仅 +0.57，预期数量级抬升） |
| T8.10 真根因：离线 SDF 缓存退化 + 两项落地修复 **（✅ 2026-08-27 主体完成——"对账"猜想被证伪，实为缓存产物坏死 + 接收端拒绝过度）** | ① **v1.cache 内容退化**：F6/路径回读双证据（全画面仅剩水平半空间、无任何建筑结构；室内探针射线 64/64 恰好 d=0）→ 删除重烘焙后新场带符号距离健康（bounds −22/−7.5/−15.8 extent 73×29×51m、voxel 0.19、neg=6.6M 体素 min −38/max 124）。**旧缓存 Aug-26 由旧代码写出，属资产损坏非代码 bug**；② 回读工具自纠错：ray 条目是 vec4(16B)，首版 staging 按 4B 申请导致读到越界垃圾——"零距离饥饿"大半是该假象 + 未按 update list 过滤的陈旧槽位；③ 健康场后量化：**L-OUTER 命中射线 SC 路径占 94.8%**（screen 0.4%/analytic 4.8%），探针 SH 左柱廊 +x 定向瓣 0.05–0.15 复苏（此前 ≤0.02）；④ 接收端 T8.10 修复：cheby 底 0.2→0.45、normal 底 0.2→0.5（有界拒绝：+1.2 A/B 实测的全关收益折半保留、防漏语义不变）。勘误两则：~~F6 可视化画的是运行时 GlobalSDF~~（再勘误：F6 在 has_offline_sdf_ 时经 OfflineSDFViz.comp 绑定离线场，日志 use_offline=1 实证；半空间图样来自当时场数据本身）；本配置 max_probes_per_frame=16384=全量每帧更新，不存在 stale-ring 问题（T8.9b 该怀疑作废） | 读数（同矩形）：L_2F 33.40→**33.86**（p95 75.3→**77.63 全历史最高**）；L_OUTER p95 72.25→**74.66**；R_DIRECT 67.13 持平；clipped 0%。均值级抬升受物理链固有稀释限制（线性反照率 ~0.1 × AO ~0.5-0.7 × 融合 max/spgi_w），后续量级需动探针光到像素的比例（见遗留）。工具留存：`DDGI_DUMP=1/2/3`（SH/ray-path/halfres-GI 三路回读）、`DDGI_PATH_DEBUG=1` 变体加载门、分支一次性打印。**遗留排序**：a) 走廊内表面 SC 卡覆盖（结构性二次反弹真正入口）；b) albedo/ao 链上 GI 权重再核算；c) ContentTools 对 sdim>150 烘焙坏的网格重新出 SDF（当前 379 中拒 14，含 104460 天文值者） |
| T8.10b 根因终审：occupancy 符号填充空心建筑 → 无符号距离场语义 **（✅ 2026-08-27 完成——session 决定性修复）** | 三层排除法收口：① 上传路径位级自检（`OFFLINE_SDF_VERIFY=1`，CopyImageToBuffer 全量回读）：GPU==CPU 5662 万 texel **零失配**，上传无辜；② Python 直读 cache 位流 vs 射线回读对质：室内空气体素 d=**负值**（nave-air −2.72 / mid-hall −0.38）——中庭腔体被烘焙 occupancy 标成实体，射线全部起步即命中，整个内场对 tracer 是实心块；③（勘误链）此前 Python 解码器把部分位型误读成 ±1e10 "哨兵污染"，正确解码后数据全部有限且平滑——哨兵论作废；上行"资产损坏"定性同步作废。**最终修复**：merger 放弃 occupancy 反号特性改无符号距离（`return d`），配套 TraceSDF 命中带 voxel×0.5→×1.25、步数 32→64、松弛 ×1.2→×1.5（浅场行进预算不足曾致 miss 74%）。加 T8.9b 起的有界拒绝底抬升 | **最终读数（vs 本 session 起点）**：L_2F mean 33.97→**39.71**（p95 76.2→**92.5**）、L_OUTER mean 29.29→**33.75**（p95 85.97）、R_DIRECT 67.29 持平、clipped 0%、max 240 无过曝；目检左拱廊柱身出现连续暖反弹梯度。全 session 累计 17.54 → 39.71（**+126%**）。新工具固化：`OFFLINE_SDF_VERIFY` 上传自检 / `DDGI_DUMP=1·2·3` 三路回读 / `DDGI_PATH_DEBUG` 变体门 / `GI_ISOLATE=all`。**遗留**：a) ContentTools 重出 hollow 建筑的正确符号/开放壳 SDF（长线正解）；b) ScreenProbeTraceSDF 同步参数对齐（避免双端分歧）；c) SC 卡覆盖走廊内表面 |
| T8.10c 对比度回修：遮挡调制底光 + 泄漏收紧 **（✅ 2026-08-27 完成）** | 用户反馈："只是整体亮了"——旁侧应比外侧柱/布更暗的区域仍偏亮。两元凶：T8.9 常数底光在真实传输修好后成为均匀洗白拐杖；T8.10 有界拒绝放松引入跨墙泄漏。修复：底光乘 `mix(0.45,1,pow(ssao,1.1))` 遮挡调制（缝隙半强、开敞区全额）；gather 泄漏底 cheby 0.45→0.38、normal 0.50→0.40 | 默认机位（中位数口径）：L_OUTER p50 26.0→25.0、p95 86.0→85.2（代价 −1 级）；**对比度 spread p95/p25 5.42→5.91、暗袋/穹顶秩序 0.13→0.11**；用户机位复现（SPGI_CAM≈4.0,5.8,-4,-6°）：走道纵深衰减恢复、蓝旗区明暗秩序自然；R_DIR 与 clipped 0% 不变 |
| T8.10d 接缝封堵实验（失败回退）+ 反馈环双稳态发现 **（⚠️ 2026-08-27 实验——默认关闭，结论入档）** | ① 用户 F6 截图（水平灰条纹）定性为**资产级接缝漏光**：离线 SDF 是逐 mesh 盒 min-合并，相邻墙段盒间留 ≤1 体素裂缝，射线穿缝打到天空（注：F6 走 OfflineSDFViz.comp 自带 marcher，与 DDGITraceSDF 无关；其 miss=灰块语义可信）。merger 加 6 邻域合缝闭合（env `OFFLINE_SDF_SEAL=1` 可复现）——**实验失败**：crack 集与表面外皮在体素粒度不可分，封缝=全场 69% 体素钳平（3881 万），L_2F 39.2→19.3 崩溃回退；② 顺序排查中误把恢复失败归因 cache/步长上限——最终定位为 **SC↔probe 反馈环双稳态**：同 binary+同 cache 相邻两次运行分别落在 101/114/99（高支，全局 +50%、零过曝）与 39.2/33.0/67.3（正常支）——即 T8.4 mode2 记录过的平衡点漂移在默认参数下也偶发。**测量纪律升级：关键读数一律 ≥2 次运行取中位**；增益核算（0.85 反馈系数）列入正式待办 | 红框区（左旗区+内墙，REDBOx x[5,442] y[349,579]）全版本 p50≈9-11 未变——该区需要的是接收端工质（SC 卡覆盖走廊内表面/探针接收路径），非本轮字段级修复可及，列 T8.11 候选首位；F6 条纹同样待资产侧接缝修复（ContentTools 盒式烘焙换连续体/封洞） |
| T8.10f tunneling 定性 + 步长帽 A/B（否决） **（✅ 2026-08-27 实验——cap 否决，基线定为最终态）** | 用户追问"单面开口为何漏天空"触发机理重审：实测剖面（cache 直读，从中庭向左墙 0.25m 步距）证明场结构完整（墙带 d 0.09-0.24 双层，两侧 0.474 平台）——**漏的主因是无符号场行进离散化**：平台区步长=d=0.474 与命中带（±1.25 voxel≈0.48m）同宽，斜掠/薄板采样点从带两侧跳过 → 穿出后全是平台 → 跑满 RayMaxDistance=50 判 miss→天空。"单面开口"无关：穿越不经过开口，在任意薄面相位穿越。修正：T8.10d 的"1 体素接缝"降级为次要。**步长帽 A/B（多运行中位，3×3）**：cap 2×voxel+96 步 vs 基线 64 步无帽——cap 使 L_2F 39.4→28.2、p95 92.9→59.9、REDBOX 9.4→8.4（近壁 min-step 爬行烧尽预算，跨庭射线 miss 回灌），**否决**；F6 的 OfflineSDFViz marcher 保留 2×stepSize 帽（纯显示层，消除假洞让 F6 反映场真实覆盖）。双稳态 6 跑未复现高支（101 支偶发）；测量纪律维持 3 跑中位 | 最终锁定配置：未封缝无符号场 + hitBand 1.25 voxel + 64 步 relax1.5 无帽 + 遮挡调制底光 + 泄漏底 0.38/0.40——L_2F 39.4/92.9、OUTER 33.3、R 67.2、REDBOX p50 9.4、clipped 0%。**开放问题**：a) tunneling 接受为已知限制（命中带加宽与步长帽均净负，正解=ContentTools 有符号/水密烘焙）；b) REDBOX（接收端工质）T8.11；c) 反馈环增益核算 |
| T8.10g 布料"边缘亮/面暗"定量裁决：非 GI 缺陷 **（✅ 2026-08-27 测量收口）** | 用户新证：左侧暗红横幅金边亮、布面黑。新增 `DDGI_DUMP=3` 可配置统计窗（`DDGI_RECT="name:x0,y0,x1,y1;..."` 全分辨率坐标）直读 GIGather 半分辨率辐亮度。**实测（默认机位）**：暗红布面 irradiance **0.289**（0.159-0.527）≥ 右侧受光绿布面 0.213、金边 0.502——探针场对该面供光充分甚至更高。面暗的构成：①饱和布料线性 albedo 极低（深红≈(0.25,0.02,0.02)）→ indirect≈0.29×0.25≈0.07 仅红通道；②该侧无直射项（对面同款布料被阳光直射所以亮）；③金边=金属/低粗糙材质吃 spec 通道（radBase 0.05 地板+探针 radiance）+高 albedo。**裁决：材质物理正确，非 GI bug**；若要布面更可读属 art 方向（albedo 提升/布料亮度倍率），不动传输 | 工具新增：DDGI_RECT 任意窗口统计（随 DDGI_DUMP=3 生效）；`_strdup`→`strdup` 编译修正 |
| T8.10h 直射强度校准 +25% **（✅ 2026-08-28 完成）** | 用户判定"直接光被压暗、与太阳亮度不符"。审计：直射链 `Lo=(albedo/π+spec)×lightColor×NdotL×shadowVis×DIRECT_INTENSITY(2.5)`，有效日强 5.0、受光墙 NdotL≈0.55 → ACES p95≈153，数值自洽但观感偏阴天；**受光区间接占比实测 31.6%**（(67.2−46)/67.2，物理典型 15-25%）佐证直射偏弱。旋钮选择：**只动屏幕端 `DIRECT_INTENSITY` 2.5→3.125**，不动 camera `light_color`——后者同时是探针射线驱动（×3.0 进 SC↔probe 环），抬它会重演 T8.4 平衡点漂移 | 2 跑极稳：R_DIRECT 67.2→**77.6**（预测 78.7 ✓）、L_2F 39.3/REDBOX 9.7/p95 92 均不变（背光侧无直射项）、间接占比 31.6%→**27.3%**、clip 0.002%（max 253，日高光触顶可接受）。后续口味调节：DIRECT_INTENSITY 每 ±0.625 = ∓25%，位于 DeferredLighting.frag:45 |
| T8.10i 彩色反弹审计：机制在、信号低于可见阈 **（✅ 2026-08-28 测量收口）** | 用户问"GI 有没有取到直射区纹理 albedo"。机制：三层命中着色的 SC 图集路径存的就是 albedo×光照（LightEval `result=(direct+indirect)×albedo+feedback×albedo`，albedo_atlas 逐 texel 纹理进卡，卡分辨率 32²）。**实测（DDGI_DUMP3 RGB 窗口）**：五个窗口反弹全为中性灰+轻微偏蓝（如 nave_floor (0.281,0.272,0.298)）——偏蓝=sky-miss 份额签名（SkyColor 0.85/0.85/1.0）；受光红布旁的石头无红色染、绿布旁无绿染。裁决：彩色反弹存在但被稀释到 ~1-3%（64 射线/帧 × 布料小立体角 × EMA 0.02 × 卡覆盖/灯评估轮换存疑——LightEval 日志"每帧仅 4 卡"），低于可见阈；屏幕颜色= albedo×灰 GI + 直射 | 候选修复（T8.11）：a) SC LightEval 覆盖率/轮换审计（4 卡之谜）；b) 探针射线 64→128 或对高能卡重要性采样；c) 卡分辨率 32→64。窗口 RGB 统计已固化进 DDGI_RECT/DDGI_DUMP=3 |
| T8.11 SC 捕获架构审计：屏幕空间重投影是彩色反弹缺失与红框区欠亮的共同根因 **（✅ 2026-08-28 诊断完成——修复为多会话工程，未动）** | 三连审计闭环：① 卡生成器健康（393 实例→407 卡，"4 卡"实为 capture_budget=64 页轮换：64×32²=65536 texel=恰 4 张 128² 卡/帧，其余 403 卡靠 seed 保持——非覆盖 bug）；② LightEval 图集写的是 albedo×光照 ✓；③ **SurfaceCacheCapture 是主相机 GBuffer 的屏幕空间重投影**：逐主相机像素取 albedo/normal/depth 散射进所有正交框住该点的卡——**不可见于当前相机的表面（离屏/被遮挡/背面）永远 alpha=0 → LightEval 早退 → 卡永久黑**；且捕获无逐卡遮挡测试，近景会错误染入远景卡框。推论链：探针射线命中未捕获表面=黑 → 彩色反弹（布料/彩墙）缺失 + 红框区接收欠亮（其来向射线打中的内墙面多为主相机看不见的面）全部同源；轻微偏蓝=sky-miss（另一主题） | **正解路线**（工程量大，立项待批）：a) 真·卡视角正交捕获（每帧预算内渲染 N 张卡的 6 面深度+albedo，替代屏幕空间重投影——Lumen 同款）；b) 过渡方案：主相机多锚点（前后左右四次重投影）扩大覆盖；c) 捕获时逐卡 axisDepth 遮挡比较防错染。临时缓解：无（屏幕空间捕获的相机绑定是架构性的） |
| T8.12 卡视角捕获落地 **（✅ 2026-08-28 完成并转默认——本 session 决定性修复之二）** | 按 T8.11 立项实施：① 新 shader `Nanite/CardCapture.vert/.frag`——几何解码与 PipelineDraw 逐位一致（cib→cmap→meshlet/instance/material），只换 binding0 为卡参数 UBO（正交 view/proj + card center/fwd），PS 材质采样同款 + 输出改三页（albedo a=1 / normal oct rg16f / axisDepth r32f，语义=projectToCard 的 dot(local,fwd)）；② GPUDriven 新增 `InitializeCardCaptureResources/ExecuteCardCapturePass`（克隆 ExecuteReflectionPass 骨架：CPU ExtractFrustumPlanes 球剔除 → 可见簇表 → 间接绘制；32 个 face scratch 槽）；③ SurfaceCachePass 新增 `AddCardCapturePasses`（批次调度：seed 64 面/帧至全覆盖 → 稳态 8 面/帧；基向量按 axis/dir 表构造，v 轴取 −v_world 对齐 texel 行向）+ env 门控；④ 屏幕空间 gather 与卡视角互斥。**工程勘误**：T8.5 的"gl_InstanceIndex 恒 0"仅限 Draw() 实例化；DrawIndirect 路径（instanceCount=可见簇数）工作正常——本实现与其依赖的 GBuffer 主路径同模式 | **3 跑中位：L_2F 39.4→46.6（p95 92.9→108.3）、L_OUTER 33.3→39.3、REDBOX p50 9.4→16.6（+77%）、R_DIR 77.6（直射 +25% 独立效应）；clip 0.011% 可忽略。全 session 累计：17.54 → 46.6（**+166%**）。目检：双侧柱廊整体受光、对面布料全可读、彩色反弹上墙、日影方向保留。默认 ON（SC_CARDVIEW=0 回退旧路径）。**性能**：Debug 构建 30s 帧计数实测卡视角 20.3 FPS vs 旧屏幕空间 19.5 FPS——非回归且略优（8 卡正交面批 < 全屏 gather），帧耗时 ~49ms 由其他 pass 主导（DDGI 子过程仅 5.25ms）；批大小 2~8 在噪声内 |
| T8.12b 测量完整性：全局键盘泄漏 + 相机锁定 **（✅ 2026-08-28 发现并修复）** | 定位"跑帧间视点漂移"（两帧 luma 相关性仅 0.30）：测试的 input source 读取**全局键盘状态**，自动化跑帧期间用户在交互窗口的 WASD 会泄漏进跑帧进程，移动跑帧相机——此前所有区域读数都可能带视点污染（含部分跨配置对比）。修复：`SPGI_CAM_LOCK=1` 冻结相机（跳过 `camera_.Update(dt)`），**测量纪律再升级**：自动化读数一律 SPGI_CAM_LOCK=1 + ≥2 跑；锁定后双跑 luma 相关性 0.9998 | **锁定相机最终基线（默认机位，2 跑中位）**：L_2F mean **46.2** / p95 108.5、L_OUTER **40.2**、R_DIRECT **77.7**、REDBOX p50 **17.2**、clipped≈0。全 session：17.54 → 46.2（**+163%**） |
## T8.13+ 路线图（2026-08-28 排定，按依赖×收益排序）
### T8.13 P0 交付：GPU pass 级分解（2026-08-28，RG-Perf 实测）
### T8.13 P1 交付：反馈环增益核算 0.85→0.70（✅ 2026-08-28，双稳态消除）
### T8.13 P3 交付：SC_MIN_CARD_RES 32→64 默认（✅ 2026-08-28，abfb717 + 后续 verify）

CardGenerator 加 `SC_MIN_CARD_RES` env + 装箱兜底（放不下的卡降回单页，覆盖>细节）。**装箱评估**：分布 32px=66/64px=137/96px=6/128px=198，texel 93.7%；floor-64 后 95.3%——**2048² texel 可容但货架装箱碎片化丢弃 33 张尾部卡**（407→374，涉及表面无 SC 供光）。**意外**：SC↔probe 平衡点随覆盖拓扑迁移 +27% 变亮（3 跑中位 65.2/58.8/107/33.5，极差 ±0.9 稳定，clip 0.24% 可接受）——环路平衡点对 SC 覆盖拓扑敏感的又一实证（P1 增益裕度的重要性再证）。已转默认 64。

### T8.13 P4 交付：蓝色偏移根治（有界版，✅ 2026-08-28）

**前提修正**：无符号合并后烘焙 occupancy 已不进最终场（neg=0 实证），"ContentTools 有符号烘焙"不再是蓝偏移的根因修复。**真凶排除法**：SkyColor 完全中性化（1,1,1）后 B−R 纹丝不动（+0.046）——非积分常量；AtlasInit 默认材质为中性灰——非播种色。**残余蓝偏 (+0.046) 的构成判定为**：① 真实天光（中庭天窗物理正确）；② 屏幕空间捕获的纹理混色（蓝布边缘 texel 散进相邻石卡）；③ SC↔probe 反馈的颜色均衡态。缓解已落地：SkyColor 蓝 tint 移除（0.85,0.85,1.0 → 1,1,1）。**有符号烘焙的长期价值**（重新定位）：修复薄墙 tunneling（射线用符号判内外，永不穿墙）→ sky-miss 与蓝偏进一步收敛；前置条件是 ContentTools 烘焙管线的水密性分析，单独立项。
### T8.13 P2 交付：探针射线 64→128（✅ 2026-08-28，lane-striding 落地）
### T8.13 P0+ 交付：SSGI 降级策略（✅ 2026-08-28，FPS 翻倍）
### T8.13 60FPS 冲刺交付：GTAO 2×1 默认（✅ 2026-08-28）
### T8.13 60FPS 达成（✅ 2026-08-28）：SPGI 64→48 + Release 稳态 53.9 FPS
### T8.13 补充：阴影硬化诊断 + VSM 模糊半径旋钮（✅ 2026-08-28）

用户反馈 Release 阴影过硬。三层排查：① `shadow_quality`（Hard/PCF/PCSS）**整条 ShadowFilter 分支是死代码**——`vsm_enabled_` 默认 true 时函数在 VSM 分支提前 return，实际阴影走 VSM moments 路径（Hard/PCSS A/B 像素级一致即此，env SHADOW_QUALITY 保留但仅在关 VSM 时生效）；② 30s 窗口 FPS 法被首启 PSO 编译污染，改用**帧戳差分法**（frame 61→361 恰 300 帧，sub-second 轮询）；③ 硬度真旋钮 = ShadowMomentsBlur 半径（2048 moments 图 ≈34 texel/m，半径 3 ≈ 0.3m 半影）。**半径 3→8**：半影 ~1m，目检边界出现自然渐变；env `SHADOW_BLUR` 可调（=3 回旧观感）。VSM light-bleed 风险随半径增大，8 为安全上界。

SCREEN_PROBE_RAYS env 覆盖 + 默认 screen_probes_rays 64→48（重要性采样+空间滤波+denoise 吸收，质量中性实测 L_2F 52.5 vs 52.7）。GPU ~16ms 达 vsync 满速：**Release + SPGI_CAM_LOCK 稳态 53.9 FPS**（帧戳差分法：frame 61→361 恰 300 帧）。**方法学定案**：30s 窗口计数被首启 PSO 编译污染（Release 30.2/50.0 两次矛盾即此），帧戳差分为标准测法；Debug/Release 稳态一致（三缓冲重叠吸收 CPU 差异，44ms"CPU 开销"假说作废——SSGI 降级后 CPU 已非瓶颈）。**session 终账**：L_2F 17.54 → **52.5**（+199%）、FPS 20.3 → **53.9**、REDBOX ~9 → **24.7**、彩色反弹可测（+0.03）、双稳态根除。

SSAO（LumenSSAO GTAO）4 方向×2 采样 ≈3.2ms → 2 方向×1 采样 ≈0.8ms，GPU 总 **18 → 15.6ms，进入 60FPS 预算（16.7ms）**。目检缝隙区质量可接受（5×5 滤波兜底）；env `SSAO_DIRS`/`SSAO_SAMPLES` 可调回。**实测 30s 帧计数：43.2 FPS**（20.3 → 43.2，+113%）。注：墙钟已呈 CPU/Debug 构建瓶颈（GPU 15.6ms 达标后 wall 24ms 的其余为 CPU 提交开销）——**60FPS 的最后一公里是 Release 构建**，排下次验证。

SPGI 活跃时跳过 SSGI 全部 pass（`SSGI_FORCE=1` 保留 A/B），Fusion 走黑纹理兜底。**实测**：FPS 20.3 → **40.7**（+100%）；画质零回归且全指标反而更亮——L_2F 46.2→**48.1**、L_OUTER 40.2→**42.1**、REDBOX 17.2→**19.5**、R_DIRECT 77.7→**97.2**、红染 R−G +0.027 保持、clip 0.006%。机理：SSGI 的加法项实际为负贡献/与主路径竞争（T8.7"SPGI 净 −1.91"同源），移除即回升。**锁定机位最终基线**：L_2F 48.1 / OUTER 42.1 / R 97.2 / REDBOX 19.5。全 session：17.54 → 48.1（**+174%**），FPS 20.3 → 40.7（**+100%**）。

**实施**：T4.2 遗留的 striding 改造——TraceSDF/Finalize 加 `for (rayId = tid; rayId < RaysPerProbe; rayId += 64)` 循环（64 lane 服务任意 64 倍数射线数），单体的 TraceRays 用 `gid % RaysPerProbe` 天然跨步零改动；UpdateIrradiance/UpdateDepth 本就按 `RaysPerProbe` 全量循环零改动。默认 64→128（LumenTypes），配置缓冲全参数化自动扩容。

**实测（锁定相机 2 跑）**：

| 指标 | 128 射线前 | **128 射线后** |
|---|---|---|
| L_2F / L_OUTER | 45.9 / 39.8 | **46.9 / 41.1** |
| REDBOX p50 | 17.1 | **18.7** |
| R_DIRECT | 77.7 | **95.4**（受光区间接大幅回归） |
| DDGI 辐亮度（暗区石头） | 0.36 | **0.50（+39%）**；nave_floor +62% |
| DDGI-Perf（CPU 子过程） | 5.25ms | 5.01ms（GPU +~0.8ms） |

**机理确认**：64 射线时代的系统性偏暗不是平衡点问题，而是**非线性可见性门（cheby/埋没/混合权重）对高噪估计的不对称截断**——射线翻倍噪声减半，全场景 GI 场回升 39-62%。

**彩色染验收**：受光红布正旁石头 **R−G = +0.030**（达标 ≥0.03，64 射线时代 ~+0.01）——红色反弹首次可测。**残余**：全场仍系统性偏蓝（B 最高），=sky-miss 份额签名（tunneling 漏光把 (0.85,0.85,1.0) 天空色混进所有探针）——稀释了彩色染，根治在 P4 有符号/封缝烘焙。绿色布料旁绿染未现（绿布位置较低较暗，立体角与亮度双重劣势）——列入 P4 后复测。

**环路审计**（三个乘法点全定位）：探针内反馈（Finalize SC 路径 `albedo×L_i_prev×0.85`、解析路径 `π×L_i_prev×0.85`）∘ SC 侧反馈（LightEval `SC_FEEDBACK=0.85`×albedo）∘ 注入帽 0.45。双环乘积对高反照率表面贴近临界 1.0——瞬态落盆即 101/39 双稳态（实证）。

**修复**：四处同步 0.85→0.70（Finalize 两处 + SC_FEEDBACK + 单体 TraceRays 平行路径），注入帽 0.45 不动（非环路主项）。

**6 跑发散验证（SPGI_CAM_LOCK=1）**：

| 指标 | 0.85（2 跑） | **0.70（6 跑中位）** | 极差 |
|---|---|---|---|
| L_2F mean | 46.2 | **45.85** | 0.20 |
| L_OUTER | 40.2 | **39.81** | 0.27 |
| REDBOX p50 | 17.2 | **17.1** | 0.05 |
| R_DIRECT | 77.7 | **77.66** | 0.03 |
| p95 | 108.5 | **107.6** | 0.07 |

**裁决**：双稳态消除（6/6 同支，极差从 ±0.5 → 0.2），亮度代价 <1%（REDBOX 17.2→17.1）——稳定性免费。后续所有 A/B 以此为基线。

RenderGraph 内建 GPU 时间戳系统补全读回（MoltenVK 怪癖：vkGetQueryPoolResults 返回 VK_NOT_READY 但数据已暂存——改为逐对有效性过滤；`RG_PROF=1` 启用 Top-24 打印）。

**GPU 分解（默认机位，窗口均值 36.3ms/帧）**：

| pass | GPU ms | 占比 |
|---|---|---|
| LumenSSGI | 18.31 | 50.5% |
| ScreenProbeGI | 12.56 | 34.6% |
| LumenSSAO | 3.18 | 8.8% |
| LumenDDGI | 0.66 | 1.8% |
| ShadowMomentsBlur | 0.59 | 1.6% |
| DDGIGIGather | 0.46 | 1.3% |
| SC_DDGI_Integration | 0.30 | 0.8% |
| 其余（DDGI_Far/SC 等） | 0.20 | 0.6% |

**结论**：① 两个屏幕空间 GI（SSGI+SPGI）占 **85%**；② 本 session 全部 DDGI/SC 工作合计 <2ms；③ 60FPS 预算 16.7ms，需削 19.6ms——**SSGI 与 SPGI 功能重叠**（T8.7 实测 SSGI 在暗带贡献=0），"SPGI 启用时降级/关闭 SSGI"是唯一的 18ms 级杠杆，剩余 1.7ms 缺口由 SSAO/半分辨率策略补齐。墙钟 49ms vs GPU 36ms 的 13ms 为 CPU 提交/等待开销（Debug 构建放大）。

| 序 | 项 | 为什么排这里 | 主要工作 | 验收 | 预估 |
|---|---|---|---|---|---|
| P0 | **非 DDGI 44ms pass 级 profiler**（→60FPS 的前置） | 射线翻倍/卡分辨率都要加成本，先拿到预算地图才不会盲加；DDGI-Perf 模式照搬（std::chrono 采样线，按 pass 名聚合每 60 帧打印） | RenderGraph 执行层加按名计时聚合；输出 Top-15 pass 耗时表；顺带验证卡视角批次的真实成本 | Top-15 表 + 60FPS 差距结论（哪些 pass 可救、哪些放弃） | 0.5-1 天 |
| P1 | **反馈环 0.85 增益核算（双稳态正解）** | 双稳态污染一切测量（101/39 两支实证）；先修它，P2/P3 的 A/B 才可信。改动是常数级（0.85→稳定界内，或 UpdateIrradiance 加历史钳制），帧成本零 | 审计 SC LightEval ×DDGI gather×0.85 的环路增益推导；重设系数；6 跑发散测试（101 支不得复现） | 6 跑全部落正常支（±1 内）；`TestVulkanSponzaRenderGraph` 双稳态标记关闭 | 0.5 天 |
| P2 | **修稀释：探针射线 64→128 或重要性采样** | 彩色反弹可见性第一瓶颈（T8.10i：信号 1-3%）。两条路线在 P0 后择一：64→128 需 kernel striding 改造（T4.2 遗留）；重要性采样（SPGI F2 先例移植）成本/收益更优 | 路线择一实施；ProbeSampleLib 单源扩展 | `DDGI_RECT` RGB：红布旁石头 R−G ≥ +0.03（当前 +0.01）；目检布料色染上墙 | 1-2 天 |
| P3 | **混合分辨率 + 装箱**（含原"32→64"） | P2 让颜色可见后，分辨率才轮到"纹理细节"。按卡重要性分配 32/64/128（布料/受光高频卡高分，次要卡 32），评估 2048² 装箱率；装不下再谈 4096²（显存 ~740MB） | 重要性分类（LightEval 热度/包围盒/材质饱和度）；CardGenerator 分辨率分配改造；装箱率评估报告 | 布料卡 ≥64px；反弹出现纹理细节；2048² 装箱率报告与决策 | 1-2 天 |
| P4 | **ContentTools 有符号烘焙** | 独立离线项，修 F6 接缝漏光的资产根源；零运行时成本。放后面因为不阻塞任何在线项，且需要先读懂 ContentTools 烘焙管线 | 空腔不标 occupied（flood-fill/壳检测）；重烘 cache；F6 复验 | F6 灰条纹显著减少；DDGI_DUMP=2 miss 率下降；探针彩色瓣增强 | 1-2 天 |
| P5 | 薄板 z-fighting 观察 | 无独立工作，并入 P2/P4 验收截图检查 | — | 目检无新增伪影 | 0 |
| — | **60FPS 推进** | P0 出分解后：快赢项（冗余 pass/错分辨率）随手修；深水项单独立项；同时做一次 Release 构建对照（Debug -O0 是 20FPS 的第一成因） | — | — | — |

依赖链：P0 →（P2 路线决策、P3 成本评估）；P1 →（P2/P3 全部 A/B 可信度）；P4 独立可并行。
总计 ≈ 4-7 个工作日。测量纪律不变：SPGI_CAM_LOCK=1 + ≥2 跑中位。

遗留与边界：
- `DDGIVolumeData.ProbeRelocationShift[3]` 是 **volume 级网格 shift**（`UpdateProbeOrigin` 早期 return，恒 0，shader 侧从未消费）——与本任务的 per-probe 射线起点重定位不同物。若未来需要真正 per-probe 偏移参与 gather 权重（可见性/法线项用重定位后位置），应立独立任务：GPU init-pass 写 per-probe offset SSBO + gather 权重一致消费；当前 0.45-cell 射线起点方案是它的低配近似。T8.3 首跑判读出的下一步：重定位升级为经典 DDGI 多轴+角点"最深自由点"搜索（楼板内整排 probe、墙体两倍厚处的 probe 当前推不进房间，只会贴到最近表面）；真 mesh sphere 场景注入（遮挡正确、可点选）归 M6 调试工具线。
- **Metal/Dawn 未随动**（两端 gather 与 Vulkan 尚未对齐，等 M3 归一轮）：`SceneLightingParams.ddgi_light_color` 默认值保持 {20,20,20} 未除 π，对齐时一并折入；Dawn 的 1.5 radiance 量级钳制届时需同步 ×π。
- 既有无关告警：SurfaceCache normal atlas 描述符 format 不匹配（Rgba16f OpTypeImage vs R32G32_SFLOAT ImageView，validation WARN）——改动前即存在，另行立卡。

---

## 6. 执行红线

1. **读数预算**：每个动过的 kernel 维护读取预算注释（`DDGIGIGather.metal:10` 式）；超 32/线程设计一律打回；
2. **FPS 红线**：B 类任务不带三端测量不合并；降级路径与功能同 PR；
3. **语义红线**：算法语义单一 GLSL 源定义；质量分歧只落在 PlatformProfile，禁止散落 `#ifdef` 分叉；算法一致性测试（同 Profile override）进 CI；
4. **回滚红线**：每任务独立可开关（配置 flag 或模块边界内），合入即打 tag。

## 7. 风险与对策

| 风险 | 对策 |
|---|---|
| 读数预算回归 | 预算注释制度化 + CI 静态检查（可选） |
| 三端漂移 | GLSL primary + 移植 checklist + 算法一致性测试 |
| 多 volume 复杂度 | 上限 4；两段式选 volume 后预算不变（设计红线） |
| WASM 性能 | 沿用"跳过读回"模式；能力位降级走 PlatformProfile |
| trace 成本超预算 | T4.1 调节器先行；所有增量任务带降级路径 |
| IS 能量偏置 / 抖动与一致性阈值冲突（M7） | F2 权重补偿 + 均匀场景回归测试进验收；F5 幅度约束 ≤ 阈值 ½ 并带自动关闭降级 |

## 8. 已明确的非目标（Non-Goals）

- 硬件 RT（无设备，决策④）；
- 运行时任意组合的通用 GI 框架（轴化接口但具体化管线）；
- per-probe 自由摆放（仅 volume 级编辑；per-probe 仅在静态烘焙有明确需求时重议）；
- 跨平台像素级一致（只保证同 Profile override 下一致）；
- 传统页表式软件 VT（若 M6 立项，走 tile-array 形态，见 `2026-08-21-virtual-texture-system-design.md` 的衔接评估）；
- radiance map 全表示与 texel 级重要性采样细分树（条件立项：T4.2 + M7/F2 形态 c 落地后仍有收敛缺口时重议，A2 八面体 encode 已预留）。
