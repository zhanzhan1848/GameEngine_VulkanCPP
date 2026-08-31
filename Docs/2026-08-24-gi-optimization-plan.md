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
## Vulkan ↔ Metal 同步清单（2026-08-28 移交，2026-08-28 同步完成 ✅）

T8.7–T8.13 全部落在 Vulkan 端；Metal 侧待同步。**引擎 C++ 层改动（ShadowMapModule 模糊半径、SkyColor 中性化、SSGI 降级策略、SCREEN_PROBE_RAYS/SSAO_DIRS/SSAO_SAMPLES/SC_MIN_CARD_RES/SHADOW_BLUR 等 env、CardGenerator 混合分辨率、GPUDrivenDrawPipeline::ExecuteCardCapturePass）为平台共享代码，Metal 自动生效，无需动作**。需要手工同步的是 Metal 专属 shader 源（**同步原则：API 限制不同、代码允许不一致，最终视觉效果必须一致**——即以当前 Vulkan .comp/.frag 的语义为准移植，MSL 惯用改写不视为偏差）：

| # | Vulkan 端改动 | Metal 对应文件 | 同步内容 | 状态 |
|---|---|---|---|---|
| 1 | DDGITraceFinalize.comp：反馈 0.85→0.70、lane-striding(128 射线)、命中带 1.25 voxel、PCSS 光源尺寸(死代码可缓) | `Engine/Graphics/Metal/shaders/Lumen/DDGITraceRays.metal` `ddgi_trace_finalize`（运行时 Lumen 模块加载引擎目录；EngineTest/shaders/Lumen 为陈旧副本不加载） | **✅ 整体重写为 SC-first 层级**：SC atlas（能量门 >0.0001）→ prev-screen + materialBounce(saturation 混权) + albedo·L_i_prev·**0.70** → analytic + π·L_i_prev·**0.70**；miss 路径加能量门（黑卡不再压天空项）；striding 循环；**shDot4→shDot9**（L0-L2 全基线）；ray 方向统一走 ddgiSampleRayDirection(RaySampleMode)。PCSS 死代码未搬（Vulkan 端亦未启用） | ✅ 2026-08-28 |
| 2 | DDGITraceSDF.comp：striding 循环、hitThreshold 0.5→1.25 voxel、MAX_STEPS 64、relax ×1.5 | 同文件 `ddgi_trace_sdf` entry | **✅** striding + 1.25 命中带 + 64 步（kernel 内局部常量，共享 DDGI_MAX_SDF_STEPS=4 不动以免波及其它 pass）+ relax 1.5 + **probe relocation 移植**（14 向深搜、0.45·spacing 上限——清单未列但 Vulkan 视觉行为依赖它，埋墙探针烘黑问题）+ probe origin 语义（trace 用 relocation 后原点、finalize 用未 relocation 网格点，逐位同 Vulkan） | ✅ 2026-08-28 |
| 3 | DDGITraceRays.comp（单体，Vulkan 不派发仅对齐）：反馈 0.70 | 同文件 `ddgi_trace_rays` entry | **⏭️ 未改动**——Metal 端与 Vulkan 端一样只在 split 管线不可用时才派发单体（LumenDDGIPass 恒建 split 管线），Dawn 专用路径，无视觉影响 | ✅ 2026-08-28（豁免） |
| 4 | SurfaceCacheLightEval.comp：SC_FEEDBACK 0.85→0.70 | `Engine/Graphics/Metal/shaders/Lumen/SurfaceCacheLightEval.metal` + `SurfaceCacheData.metal` + **C++ SurfaceCachePass.cpp Metal 布局分支** | **✅ Metal 端缺的不止系数，整块补齐**：8-probe 三线性 SH9 probe gather（clamp 4，单位与增益预算注释同步）+ depth_atlas 表面点重建（替换 card 中平面）+ SC_FEEDBACK **0.70** 回退自反馈（出格/未喂探针时）+ prev_light NaN/Inf 清洗 + clamp 64。配套：`FlattenedLightingParams` 补 `ddgi_origin_spacing/ddgi_counts_valid`（尾部追加，布局同 C++）；C++ Metal LightEval 布局 9→12 绑定（texture4=prev_lighting、texture5=depth_atlas、buffer6=ddgi_irradiance）+ 描述符写入同补；histIdx UAV 屏障本就双平台共享。另将该文件移出 MetalShaderValidation.cmake 跳过名单（现可编译） | ✅ 2026-08-28 |
| 5 | FusionIndirect.frag：亮度优势裕量门、遮挡调制底光、SSGI 降级联动 | `DeferredLighting.metal::fragmentFusionIndirect`——**两份**：`EngineTest/shaders/`（TestModularPipeline 加载）+ `Engine/Graphics/Metal/shaders/Forward/`（StandardRenderPipeline/ForwardSceneRenderer 加载） | **✅** 全公式同步（T2.3 旧交叉淡出 → 当前 Vulkan 公式）：ao=mix(0.4,1,pow(ssao,1.5))、ssgi_conf 下限 0.3、T8.7 radianceValidity smoothstep(0.02,0.15)、spgi_w 上限 0.9×validity、T8.8 replaceW smoothstep(1.10,1.35)、SSGI×mix(0.25,0.15,spgi_w·replaceW)、T8.9+T8.10c 底光 (0.17,0.18,0.21)×mix(0.45,1,pow(ssao,1.1))。**移除 EngineTest 版旧 3×3 DDGI blur**（当前 Vulkan 基线直采，52.5/46.9/97.2/24.7 即无 blur 状态） | ✅ 2026-08-28 |
| 6 | DeferredLighting.frag：DIRECT_INTENSITY 2.5→3.125 | Metal 直射常量——EngineTest 版 `fragmentLighting_gpuDriven` + Engine 版 `fragmentLighting_v3`（两份各自入口） | **✅** DIRECT_INTENSITY=3.125 乘入 Lo（同为屏内直射项缩放；T8.10h 注释随行） | ✅ 2026-08-28 |
| 7 | SkyColor tint 中性化（LumenDDGIPass.cpp 共享 ✓ 自动） | — | 无需动作（引擎层） | ✅ 自动 |
| 8 | Nanite/CardCapture.vert/.frag + ExecuteCardCapturePass | Metal 需 `CardCapture.metal` | **✅ 新建 `EngineTest/shaders/CardCapture.metal`**（GPUDrivenDrawPipeline 的 Metal 加载器解析 EngineTest/shaders/）：单文件双命名入口 `card_capture_vertex`/`card_capture_fragment`（MSL 库不能同源双 "main"，RHI newFunction 按名解析；清单所记 "'::main' 约定"按实际 RHI 实现落地为命名入口）。几何解码/材质 stride12/法线解包/UV 翻转/front-facing 翻转/oct 编码/axis-depth 公式逐项同 GLSL；InstanceData 192B 布局以 float4 尾部字段逐字节对齐；绑定 0-9 buffer + texture10 + sampler11 恒等槽位；`[[vertex_id]]/[[instance_id]]` 对应 DrawIndirect 路径。配套 C++：InitializeCardCaptureResources 加 Metal 分支（加载名与入口名切换，Vulkan 路径不动）。共享侧 ExecuteCardCapturePass/RT 格式/间接绘制 Metal RHI 均已支持，无需改 | ✅ 2026-08-28 |
| 9 | LumenDDGIPass.cpp：AtlasInit 语义/初拥离线 SDF/未捕获=黑（共享 ✓ 自动） | — | 无需动作 | ✅ 自动 |
| 10 | 阴影：VSM moments blur 半径 3→8（ShadowMapModule.cpp 共享 ✓ 自动） | — | 无需动作 | ✅ 自动 |

**同步验证（2026-08-28）**：① xcrun metal 全量编译 5 个改动/新建 .metal 零错误；② Darwin/Debug 重 configure（修存量 GLOB 过期缺 DebugProbeSphereModule.cpp，与本次无关）后 Engine + TestModularPipeline 构建通过；③ TestModularPipeline 实机 210 帧冒烟：fragmentFusionIndirect 编译并逐帧执行、LightEval 12 绑定每帧派发（395 卡）、DDGI split 双管线（sdf/finalize）创建成功、无一条来自改动文件的编译/入口错误（现存 GBufferVT/buildin 材质/SPGI 常量三条编译失败均为存量、与本次无关，其中 GBufferVT 代码路径本就声明缺失不阻塞）。


**白洗诊断与修复（2026-08-28 第二轮，TestModularPipeline 实测驱动）**：同步后 Metal 端仍整体白洗。以"同机窗口截图 luma 统计 + SC_DUMP_CARDS 探针/ray 读回 + GI_DEBUG_STRIP 四条带"为仪器，与 TestVulkanSponzaRenderGraph（Vulkan 参考）逐级对照，定位并修复六项平台缺口：

| # | 缺口 | 修复 | 证据 |
|---|---|---|---|
| D1 | ScreenProbeAverage.metal / ScreenProbeTraceRays.metal MSL 地址空间错误（static const 程序作用域、函数内 constant 局部）→ **整条 SPGI 管线未创建** | 改 `constant uint` / `constexpr float`；SPGI 全链管线建成并逐帧派发 | 修复前 "[ScreenProbeGI] Average shader compilation failed" → CreatePipelines 提前 return |
| D2 | EngineTest/shaders/DDGIGIGather.metal（TestModularPipeline 实际加载）为旧架构：可见性加权 + 静态探针回退 + 时域/双边滤波、**无 π 换算**，与 Vulkan 现行 legacy 路径（纯三线性 ×π + 0.05 地板）语义脱节 | 整体重写为 Vulkan legacy 语义（保留 Apple Silicon 线程组预载结构）；far-volume 槽位 C++ 本就 Metal-deferred，维持近场+dim ambient | Vulkan GIGather 970 行 vs Metal 428 行 |
| D3 | DDGIUpdateIrradiance.metal 时域方案陈旧：alpha 收敛到 ProbeHysteresis（帧序 ramp），且 ddgiRayDirection 不感知 RaySampleMode | 同步 Vulkan 方案：confidence[3] 首更直发 + 幼探针 α=0.35 → 收敛到 TemporalAlpha(0.05)；mode-aware MC 权重；ddgiSampleRayDirection 上移至 DDGIVolumeData.metal 单源 | Vulkan comp L207-243 |
| D4 | **TestModularPipeline 从未接离线 384³ 合并 SDF**（Vulkan 参考 app 的 GI 质量依赖它：runtime 1m/2m/4m 级联"打偏命中、扭曲 GI 分布"）；且 OfflineSDFMerger::UploadToGPU 的 CopyBufferToTexture 包在 `#if OFFLINE_SDF_VULKAN_PATH` 内 —— **Metal 上纹理从未上传**（"Upload complete" 无条件打印，纯误导） | TestModularPipeline 接线 SetOfflineSDFSource（同 Vulkan app）；上传改平台通用 `rhi::GetCommandBuffer` 路径 | 修复前 ray dump：`sdf@origin d0 p50=0.10、hitDist p50≈0.26`（瞬间命中）→ 修复后 `d0 p50=0.3`（= 场设计 clearance 0.19-0.38）；Vulkan 探针 L0 p50=1.076 恰为天空 0.3035×3.545 |
| D5 | fragmentLighting_gpuDriven（TestModularPipeline 主光照）无 VSM 分支、SceneData 缺 shadowParams 字段；共享 ShadowMapModule 默认 VSM 模式 → texture(6) 白色回退 → **全屏无影** | SceneData 补 shadowParams(368)；补 GetVSMShadowVisibilityGpuDriven + moments 13/14 绑定 + 模式分支；v3 的 ssaoTex 13→7 槽（对齐共享 C++：7=SSAO、13/14=moments） | 用户截图全图无影；vsm_enabled_ 默认 true（ShadowMapModule.h L80） |
| D6 | SC AtlasInit 管线/布局/编译三处均 `isVk` 门控 → **Metal 光照图集从不播种**，未捕获页=堆垃圾，SC↔probe 反馈环中毒（SC 369 卡亮 → 90 帧内崩到 1 卡） | 新建 SurfaceCacheAtlasInit.metal（Metal 槽位约定）；布局/管线/编译三处解锁 Metal | cap_offsdf: frame0 lit=369 → frame90 lit=1 |

**修复后剩余问题（下一 session 接手）**：① SC↔probe 环仍有毒源 —— ray dump 显示 `hitLum p10≈-2.5~-4`（负辐亮度进入 SH 投影，候选：finalize 负 L_i_prev 振铃 / SC grid 卡查找垃圾）与探针 L0 离群值（max≈10²-10³，候选：worldToCardUV 越页采样 / mode-aware mcWeight 的 pdf 下限路径）；② SC 卡亮度标度 Metal≈2.85 vs Vulkan 收敛 0.88（≈3×，候选：SC 直射的 SDF 阴影可见度两侧用的场不同——Vulkan 疑似同样受益于离线场精度、Metal 的 SC 阴影仍走 runtime 级联）；③ 正式验收前需把 SPGI_CAM_LOCK + 帧戳差分测量基建移植到 TestModularPipeline（当前仅 Vulkan app 有）。诊断基建保留：GI_DEBUG_STRIP（fusion 四条带）、DDGI_DBG_PATH（finalize 路径戳，#ifdef 零开销）、SCDDGIIntegration 的 rays/d0 dump（SC_DUMP_CARDS=1 触发）。


**第七项修复（2026-08-28 第三轮，最大单一来源）——App 级光照标定缺失**：对照 TestVulkanSponzaRenderGraph 的配置代码发现，Vulkan 参考 App **显式下发** `lighting.light_color = (2.0,1.9,1.8)` 与 `ddgi_light_color = (0.318,0.302,0.286) = 1/π`，并注明"legacy 默认 RGB=20 配合 LightColor.w=3 的有效强度=60，会把 GI 受光面推进 ACES 白肩"；而 TestModularPipeline 从未配置 lighting —— 跑的是管线默认 **light_color=5.0、ddgi_light_color=20.0**，探针解析项有效驱动 ≈60× 参考值。已把同款三行配置照搬进 TestModularPipeline::InitializePipeline。标定后（SC_DUMP_CARDS staging 稳态读数）：SC 卡 364→254/395 lit、p50 0.30→0.127、值域 0.1-0.3（合理）；ray dump p50 0.30→0.059、p90 7.9→1.11。**注意**：SCDDGIIntegration 的 ray/probe 直接 Map 共享内存读回与 GPU 写入存在撕裂，其负值/离群值（hitLum p10≈-3.5、probe L0 max=5153.86 恒定复现）只可作线索不可作定论；可信仪器 = staging 拷贝+延迟打印（卡值）与最终图像。剩余待验证：视觉确认（需解锁屏幕后截图）、SC 阴影可见度对卡的最终标度、以及正式验收（SPGI_CAM_LOCK 移植）。

**决定性修复（2026-08-28 深夜，用户怀疑"后处理导致"直接命中）——双重 tonemap**：合成段五条带分解（scene/indirect/reflection/volume/total）在最终画面上**切不出条带** → 最终像素根本不是 fragmentFusion 的输出路径 → 顺藤摸出 FinalBlit 链的双重 tonemap：
- **D8a**：TestModularPipeline 的 `kBlitPS` 指向 `fragmentBlit`（内嵌 ACES+gamma），而它采样的 postToonColor/fusionOut 是已 tonemap 的 LDR → 双重 ACES+gamma 把全帧钉在白肩。改指 `fragmentBlitNoTonemap`（EngineTest 版 753 行，注释本就写明为此场景准备）。mean 220→187，albedo/布料细节回归。
- **D8b**：Metal `fragmentFusion` 内嵌 `toneMap(result)`，而链路后续还有 ToneMappingPass（ToneMapping.metal tonemap_fs）→ 仍是双重（且 gamma² 抬亮阴影）。Vulkan FusionComposite.frag 注释明确设计："Output HDR linear — tonemap is handled by FinalBlit to avoid double tonemap (which caused black patches in indirect-lit areas)"。删去两份 Metal DeferredLighting（EngineTest + Engine/Graphics/Forward）fragmentFusion 的内嵌 tonemap，链路变为 HDR → TAA → Bloom → **ToneMappingPass（唯一一次 ACES+gamma）** → Toon → FinalBlit(NoTonemap)，与 Vulkan 设计对齐。mean 187→**157**，画面性质从"白色雾"变为"真实渲染"（石纹/拱廊/布料+金边/狮子浮雕全部可见，与 Vulkan 参考同风格）。
- **剩余差距（视觉已接近，量级收尾）**：① 上半区黄色染（疑直射暖色 × bloom 提取 HDR 后过宽）与天顶区块状 artifact（疑 SPGI/SSGI 半分辨率块或 TAA 穿透）；② 相机锁定（SPGI_CAM_LOCK）已移植 TestModularPipeline，测量机位不再受键盘泄漏污染；③ TestMetalGIFusion 的固定机位贴墙不可用（另一目标，与主链共用源，后续修相机参数）。 tonemap 次数问题在 Dawn/WGSL 链同样存在的可能性未查。**第三轮补充排查（2026-08-28 晚）**：① 后处理假设已验证排除 —— 给 TestModularPipeline 补齐与 Vulkan 参考 App 相同的通道范围（SSR/VolumePass/VolumeRenderer/FroxelFog 显式关闭）后画面不变（mean 220.6 vs 221.3），排除 vol.rgb 加白与双 tonemap；fragmentFusion 直接读 `GetSettings()` 标定后的光色生效路径确认无误。② 通道对齐保留（测量口径一致）。③ 剩余差距定性：标定后 Metal 探针场 p50=天空(0.3036)、GIGather ×π 后 GI≈0.95 天花板式均匀环境项 + SSAO/AO 压暗不足 → 全画面 1.0 量级乳白；Vulkan 同样天空主导的探针场在拱廊读数 46 依赖暗本地 albedo×强 AO×SPGI 权重空间的正确组合。④ 相机键盘泄漏（T8.12b）在 Metal App 未修，用户交互会污染测量机位 —— 正式验收前必须先移植 SPGI_CAM_LOCK。诊断资产：GI_DEBUG_STRIP、DDGI_DBG_PATH、SC_DUMP_CARDS/SC_DUMP_INTERVAL、窗口级截图脚本（/tmp/capapp.sh 思路：Quartz 找窗 + screencapture -l）。**量化轨迹（TestModularPipeline 同机位窗口 mean luma）**：同步前基线 150.6 → 清单同步完成 209.0（半同步态：新公式配旧上游，单位失配）→ SPGI/GIGather/UpdateIrr 修复 217-222 → 离线 SDF 接入 + AtlasInit 220-227。亮度未降的原因：SC↔probe 环毒源（①②）仍在向 GI 注入过量能量；根因链已收窄至 finalize 辐射链与 SC 阴影标度两项。**正式验收（待办）**：Metal 端同机位帧戳差分 FPS + L_2F/OUTER/REDBOX 三区读数（多跑中位），对照 Vulkan 基线 52.5/46.9/97.2/24.7±1；DDGI_RECT RGB 红染 R−G≥+0.03。**前置缺口**：`SPGI_CAM_LOCK` 相机锁定与帧戳读数基建目前只在 TestVulkanSponzaRenderGraph（Vulkan）实现，TestModularPipeline（Metal）尚无——正式验收前需先把测量纪律移植到 Metal 端。

**对齐第二轮（2026-08-29，配置级）**：逐字段对比两 App 的管线配置，再修两项 Metal 独有缺陷：
- **D9 画质预设错配**：Vulkan 参考 = High（SPGI 默认开 + **PCSS 软阴影** + render_scale **2.0**），Metal = Medium（PCF_16 硬阴影 + 1× 分辨率）——阴影软硬与锐度差异的直接来源。TestModularPipeline 改 High，删除与预设重复的 SetQualityOverride 与 FroxelFog enable。运行验证：render 2560×1440、卡值健康、无新增错误。
- **D10 TAA 在 Metal 上静默失效**：`[TAA] Shader load failed` ×44/次运行——TAAPass::LoadShaderBytes 用裸 CWD 相对路径（Engine/Graphics/Metal/shaders/TAA.metal），从 Darwin/Debug 启动时不存在且无 ResolveShaderPath 兜底 → **TAA 从未在 Metal 上跑过**（时域累积/抗锯齿缺失，疑似天顶块状 artifact 的来源）。补 ResolveShaderPath 后 TAA 激活（depth reprojection active）。
- 待屏幕解锁后做锁机位同 pose 终验收（Vulkan 侧 `SPGI_CAM` 可覆写机位，两侧默认 {0,5,0} rot 0 一致）。

**两态翻转定位（2026-08-29，回答"画面不停改变的是什么"）**：锁相机（SPGI_CAM_LOCK）下双帧差分 + 连拍周期测量 + `[live]` 卡亮度高频转储，结论：**几何/纹理/布料/阴影完全静止（相机锁定生效、TAA 收敛后态内逐位不变），不停变化的是间接光场** —— SC↔DDGI 探针多反弹反馈环在 Metal 上形成张弛振荡：整个场景在"暖黄（直射反弹主导，如 RGB 226,224,170）"与"中性（天空环境主导，190,189,180）"两个吸引域之间来回翻转，态内零噪声（160×90 luma diff=0.00）、态间全场景均匀差（mean 19-43）。证据：① `[live] lit` 每 4 帧 234~374 波动、p50 0.09↔0.30；② 差分图=全场景块状（GI 内容而非边缘噪声）；③ 周期不规律（~5-15s @1.5FPS，排除固定缓冲奇偶）；④ SPGI_OFF 仍翻（排除 SPGI）。已留 A/B 开关：`SPGI_OFF` / `SSGI_OFF`（StandardRenderPipeline，High 预设下 SSGI 本被 spgiActive 抑制）。修复方向（下轮）：环上 Metal↔Vulkan 残余增益差逐项排除（UpdateIrradiance EMA 单位、SC gather clamp 单位、finalize SC-first 与 prev-screen 路径占比），或先临时压环增益（SC_FEEDBACK 0.70→0.5 / TemporalAlpha 0.05→0.02）验证阻尼敏感度。

**振荡修复（2026-08-30 凌晨）——两个真 bug，非环增益**：阻尼实验（SC_FEEDBACK 0.70→0.40 + take-over 0.85→0.50）**无效**（序列反而更狂野）→ 排除反馈增益，转向结构性间歇故障。逐层排除（SPGI_OFF 仍翻 → 静音注入 SC_INJECT_OFF 仍翻 → 排除单一路径）后定位并修复：

**系统稳定性排查（2026-08-31，响应"打开就卡/疑似系统崩溃"）**：
- **实锤崩溃机制**：MetalCB 完成回调捕获到 `GPU Address Fault (kIOGPUCommandBufferCallbackErrorPageFault)` ≈8% 帧率 —— 每次 fault 触发 Metal 命令流复位，**全系统 GPU 客户端（含 WindowServer）被阻塞**，后续提交收 "Ignored (excessive GPU errors)"；反复 fault+复位即用户感知的"系统卡死/崩溃"（08-30 23:03 系统重启与此吻合）。故障链锁定：SPGI（ScreenProbe）Metal WIP shader（地址空间错误修复后可编译可运行，但更深结构错配 → device 指针越界读）。
- **处置**：① TestModularPipeline 默认 SPGI 关（SPGI_ON=1 可开）；② 帧 fence 等待加 30s 上界（原无限重试在 GPU 卡死时会永久持有全部显存）；③ 渲染规模默认钳回 1×（High 的 2×=2560×1440 Debug 会饱和统一内存 GPU 饿死 WindowServer；RENDER_SCALE=2 可开）；④ **回退初始化顺序实验**——SetViewportSize 提前触发子系统以默认配置初始化被证明是稳定态，一切 late-config（含 High 预设实验）都会经 ApplyConfigChanges 触发 Lumen 重建连锁。验证：SPGI 关 + 1× 下 **640+ 帧 0 fault、约 2 倍帧率**。
- **新发现的存量 bug（未修，记录）**：SC 卡生成器运行中会整场回退到占位卡布局（中心 (0,4,0)/(0,5,0)/(0,7,0) 测试网格），场景 374 卡注册被清且不恢复 → LightEval 只亮 2 卡 → SC 全黑。触发条件疑为流送快照抖动（低帧率下必现）。需加"空/缩水快照不覆盖已注册卡集"守卫 + 注册恢复路径。诊断开关保留：SPGI_OFF/SPGI_ON、SSGI_OFF/SSGI_ON、SC_INJECT_OFF、RENDER_SCALE。

**离线 SDF 全链接入 + 种子链修复（2026-08-31 晚）**：
- **D15 SC 阴影切离线 SDF**：SurfaceCacheShadow 的逐 texel 可见度行进此前用运行时 1m/2m/4m 级联（粗、错位）；现已优先绑定 offline 384^3（单级联参数），运行时级联降为无离线时的回退。StandardRenderPipeline::SetOfflineSDFSource 转发到 surface_cache_pass_，InitializeLumenPasses 重建后重放。至此 GI 链三处 SDF 消费者（DDGI trace / SPGI[关] / SC shadow）全部使用离线 384^3。
- **D16b AtlasInit Metal 种子仍不落地**：管线有效、已派发（[ATLAS_INIT] 日志实证），但卡亮度仍全零 + cov=0（albedo alpha 从未变 1）。剩余嫌疑：① Metal 描述符布局里 image 0-3 与 buffer 1-2 的 binding 号重叠在 SetLayout 内部的处理；② AtlasInit kernel 的 Metal 编译产物本身。需专用 session 用 Metal API validation 抓 AtlasInit 派发的实际绑定/写入。
- **当前默认配置（稳定态）**：Medium 预设 + SPGI off（fault 源）+ SSGI on + 1x 渲染 + 离线 SDF（DDGI trace/SC shadow）+ 卡注册自动恢复 + fence 30s 上界。0 GPU fault 长跑确认。

**SC 全黑最终定位（同日深挖，下一 session 第一步照做）**：AtlasInit 派发确认执行（[ATLAS_INIT] 日志 + 管线有效），但 7,920 帧后全部 374 卡 radiance/cov 仍 0。决定性实验（下一 session 第一步）：在 SurfaceCacheAtlasInit kernel 内 tid==0 打印 `params.total_texels` 与 `cd[0].texel_count`——
- 若 total_texels 是垃圾 → **Metal 描述符布局重号 binding（image 0-3 与 buffer 1-2 同号）在 MetalDescriptorSet::GetBindings/MetalCommandBuffer 绑定循环中互相覆盖**（LightEval 同构布局但工作，需比对差异——疑点在 SetLayout 存储/去重路径）。修法：kernel binding 改非重叠号（buffer→5,6）+ C++ 布局同步。
- 若 total_texels=374×1024 正常 → 写路径问题：texture write 被后续 pass 覆盖（CardCapture 每 64 卡/帧重写页）或 storage write 静默失败（用 MTL_SHADER_VALIDATION + API validation 双开跑至 fault）。

**哨兵实验结果（同日）**：AtlasInit 写纯红 albedo 哨兵后 SC_CARDS 仍 lit=0/cov=0 → kernel 写入确实未落地（非读取/聚合问题）。且 Metal API+shader 双验证层全程 0 报错 → 排除非法用法，指向：① MetalDescriptorSet/MetalCommandBuffer 对 storage image 写绑定的静默处理（image 0-3 与 buffer 1-2 重号布局），或 ② heap 纹理 write 路径在本管线布局下的静默失效。下一 session 工具：Xcode GPU frame capture（CLI 无法做）直接看 AtlasInit dispatch 的绑定面板与 texture 写入——一次 capture 即裁决。另：SC_CARDS dump 的 top 卡排序在全零时任意（按 radiance 排序），勿误读。

**离线 SDF 内容裁决（2026-08-31）**：T8.10b 自检的回读拷贝同样被 `#if OFFLINE_SDF_VULKAN_PATH` 门控（Metal 上 vStage 从未写入，"cpu=50.59 gpu=0" 全是假象）。改通用路径后重跑：**`[OfflineSDFVerify] texels=56623104 mismatched=0 (0%) => upload bit-exact`——离线 SDF 纹理上传逐位精确，内容完全正确**。此前"纹理为空/未生效"假设被推翻。剩余的"探针=纯天空"根因收窄到：DDGI trace 的**采样/阈值路径**（离线场地板 0.19-0.38 vs trace 阈值 1.25×voxel=0.238 的相互作用——地板高于阈值的区域全 miss，需对照 Vulkan 同字段确认行为一致）与 SC 卡照明（AtlasInit Metal 写入落地问题，见上）。

**收敛性长跑结论（2026-08-31，1x 渲染 + Medium + SPGI off，21,500+ 帧）**：系统稳定性指标全绿（0 GPU fault、RSS 488MB 恒定、fence 0 次触 30s 上界），**但 SC 卡亮度恒 0**（374 卡全暗，非收敛慢）且**探针 L0 恒等于天空 0.30361**（15,600 帧钉死）= SDF 追踪对所有射线全 miss → 探针场=纯天空 → SC 无输入 → GI 贡献消失。结合 offline SDF 阈值链（field 地板 0.3 vs trace 阈值 0.238 → 全 miss 的边界特征），剩余根因收敛为：**offline SDF 纹理内容在 Metal 上未正确生效**（上传路径已修但内容待验证——需 staging readback 对照 cache 文件字节），或 offline 纹理虽写入但 cascade 绑定/格式（r16 half vs cache 编码）在 Metal 采样路径上错位。下一步：readback offline 纹理前 1KB 与 cache 文件比对 + 打印 DDGI trace 一次采样的 d 值分布。注意：All-sky 探针恰好解释用户最初的"泛白但结构可见"画面（GI=均匀天空 wash），也与 Vulkan"拱廊 46/255 暗"形成对照——Vulkan 同配置探针命中表面（hitDist 0.3-2）。诊断开关保留：SPGI_OFF/SPGI_ON、SSGI_OFF/SSGI_ON、SC_INJECT_OFF、RENDER_SCALE、SC_DUMP_CARDS/SC_DUMP_INTERVAL。

| # | 缺口 | 修复 | 证据 |
|---|---|---|---|
| D11 | **帧 fence 等待超时后放行**：`RenderSystem::Wait` 单次 WaitForSync(1000ms) 超时仅打日志即继续 → High 预设（render_scale 2.0，Debug 帧耗时 1-2s）下必然超时，CPU 复用 GPU 仍在执行的三缓冲槽位（UBO/参数），GI 链读瞬态垃圾（SDF 范围/光色/体积参数）→ 全帧 illumination 状态翻转。Vulkan 52FPS 从不超时故稳定；Metal 0.5-1.5FPS 必现；阻尼无效（非增益问题）；翻转周期不规则（随逐帧 GPU 抖动过阈值） | fence 等待改重试循环（上限 5 分钟，防死锁响亮报错），不再放行 | 修复前日志每次运行 "Wait timeout for frame 2"；修复后 "fence not signaled ×51" 且不再放行 |
| D12 | **SC 卡集运行时重建后贡献表索引过期**：`SCDDGIIntegrationModule::EnsureResources` 的 `assignment_built_` 一次性门控把 per-probe 卡贡献表（card_index 烘焙）只构建一次，而 SC CardGenerator 运行时重建卡集（日志实证 8 卡→395 卡，索引洗牌）→ 注入 pass 按旧索引聚合**错卡**的 radiance → 探针间歇野值（dump 实测 L0 max=1574、p10 负值——UpdateIrradiance 的 clamp≤10 不可能写出，唯一来源是注入读错卡）| 追踪 `assignment_card_count_`，卡数变化即重建贡献表 | Vulkan 52FPS 下场景先载完、首次构建即全量 374 卡 → 恒定；Metal 0.5FPS 下首次构建落在占位 8 卡集 → 必坏 |

**修复后数值**（SC_DUMP_INTERVAL=4 长序列）：修复前 lit 234↔374 持续摆动、p50 0.09↔2.2 大幅振荡；修复后 lit 稳定 383-391、p50 钉在天空值（0.3036）、仅偶发 p90 尖峰（4.6，疑同计数重建或 cbIdx/frameCount 双计数残余，见下）。**对照 Vulkan 同仪器**：lit 224→360 单调收敛后恒定、探针 L0 收敛 1.0762±0.04。**剩余收尾**：① 目检确认（需解锁屏幕）；② cbIdx（交换链序号）与 frameCount_（帧计数）双计数体系在 present 跳帧时仍可能漂移——环上 CB 槽位建议统一到单计数；③ 同计数不同序的卡重建不触发重建（可改卡中心哈希触发）；④ Dawn 链是否有同款 D11（fence 超时放行）待查。诊断开关保留：SPGI_OFF / SSGI_OFF / SC_INJECT_OFF（SCDDGIIntegration）。

**已知平台差异坑**（同步时注意）：Metal CommandBuffer 的 WriteTimestamp 在 RenderPassEncoder 内被禁用（Apple Silicon）——RG 时间戳对图形 pass 走 desc 括号、compute 走显式 WriteTimestamp 的现有分工即是为此，勿混；D32 不要加 RenderTarget usage（Vulkan VUID）；Dynamic 内存才可 Map。

**SC 全黑根因终审 + 修复（2026-08-31 第二轮，决定性修复之三）**：「重号 binding 互相覆盖」假设被证伪——代码级比对证明 `MetalDescriptorSet::Update` 按 **(binding 号, 描述符类型) 双键**匹配（重号可共存）、`MetalCommandBuffer::BindDescriptorSets` 按类型分派到独立命名空间（buffer→setBuffer / image→setTextures）。**真根因是派发点的 binding 编号错配**：AtlasInit 派发点（SurfaceCachePass.cpp）用单一份共享 `DescriptorData` 数组写 Vulkan 编号 `{4,UBO}/{5,SSBO}`，而 Metal 布局声明 `{1,UBO}/{2,SSBO}`（对应 kernel `[[buffer(1)]]/[[buffer(2)]]`）→ Update 双键匹配失败**静默跳过**（"[MetalDescSet] UPDATE SKIP" 日志有打印但被淹没）→ params/card_dispatch 在 Metal 上从未绑定 → kernel 读未绑定 buffer 得 total_texels=0 → 全部线程早退 → 零写入。哨兵实验的"写入未落地"由此完全解释。佐证链：LightEval 派发点有 Metal 专属 else 分支写 1/2/3/4/5/6（所以工作）；Shadow 派发点有 `shIsVk ? 5u : 0u` 分支；AtlasInit 是唯一漏掉平台分支的派发点。**叠加坑**：上一轮埋的 kernel 内 `printf` 诊断在引擎运行时 `newLibrary(source)` 编译路径下**编译失败**（printf 未声明——`xcrun metal` CLI 默认带语言版本标志可过、运行时编译不带），导致 AtlasInit 管线 INVALID 从未派发，本轮首次运行即暴露（无 [ATLAS_INIT] 日志 + 编译源回显报错）。已移除该 printf（教训：MSL kernel 诊断勿用 printf 走运行时源码编译，改 storage 写入+host 回读）。
- **修复**：AtlasInit 派发点补平台分支 `{aiIsVk ? 4u : 1u, UBO}, {aiIsVk ? 5u : 2u, SSBO}`，与 Shadow/LightEval 惯例一致；移除 kernel printf。
- **验证（6540 帧，SPGI_CAM_LOCK=1，0 GPU fault、0 UPDATE SKIP）**：[ATLAS_INIT] 正常派发；SC dump 从第 2 轮起 **24/24 卡全亮**（此前 7,920 帧恒全零），值域有界（top 卡 0.67–2.0 无发散）、cov 稳定 0.44–1.0；CPU ~11ms/帧。SC↔probe 反馈环供能链恢复。
- **新状态（下一 session 主线）**：SC 供能恢复后画面**稳定过曝**（锁相机三时点 mean luma 179 恒定、~49% 像素 ≥250、非两态振荡）——即挂账多时的 **SC 卡亮度标度问题（Metal 卡值 ~1.0–2.0 vs Vulkan 收敛 0.88，及 SC 阴影可见度标度）**浮出为主要矛盾；此前所有 GI 调平均在 SC 实效死亡态下进行。下一步：对卡 dump 做 Vulkan↔Metal 同字段对照（直射进卡项 / SDF shadow visibility / SC_FEEDBACK 环增益逐项），必要时先用 SC_INJECT_OFF/权重旋钮做阻尼 A/B 定位过曝能量入口。

**CardCapture 零落地定位 + 三连修复（2026-08-31 第三轮，用户目检驱动）**：用户目检图集发现 albedo 全灰 / normal 全黄——恰为 AtlasInit 种子精确值（灰 (128,128,128) / oct 法线 (0.5,0.5) 裸看即黄），即 T8.12 卡视角捕获从未覆盖图集，SeedLightEval 在用假种子法线（全 +Z）/假深度评估全部卡 texel。
- **根因 A（结构性缺线）**：`AddCardCapturePasses` 的调用只接在 `StandardRenderPipeline::BuildRenderGraph`，而 **TestModularPipeline 实际走 `RenderWithCommandBuffer`**——该路径的 SurfaceCache 段没有捕获接线。卡视图启用（默认 ON）时 AddPass 内的屏幕空间 gather 被 `!card_view_enabled_` 禁用 → **Metal 上两种捕获都不存在**，图集永远停在种子值。取证：f1200 图集回读 `seed_exact=3,995,648/4,194,304`（100% 精确种子）。修复：RenderWithCommandBuffer 的 SC 段补同款调用（IsCardViewEnabled && scene_snapshot 门控，先于 AddPass）。
- **根因 B（接上后 GPU 挂死）**：捕获 scratch（visible/indirect/params buffer + 描述符集）只有 32 份**不分帧**，CPU 在编码期写、GPU 在执行期读，跨帧飞行（三缓冲）下 frame N+2 的写会撞 frame N 的读 → 撕裂 indirect args → 野 instanceCount → **GPU 挂死**（fence 30s 超时、100 帧内必现）。二分证据链：`CC_NO_DRAW`（跳过 draw）零超时 2700 帧（render pass 无辜）；`CC_MAX_INST` 1/128/512 干净、1024/1536/2278 挂死（实例越多读窗口越长）；`MTL_SHADER_VALIDATION=1`（时序大幅拖慢）下 1024 实例也能跑完 1200 帧 → **时序敏感竞争而非越界访问**（validation 零断言佐证）。修复：三件套+ds 按三帧飞行复制（96 份，`slot=(frame%3)*32+face_slot%32`，与引擎 `params_cb_[frameIdx]` 惯例一致）；seed batch 64→32（≤kCardCaptureSlots，防帧内编码期写覆盖已编码 draw 的数据）。注：Vulkan 路径同款竞争仅因 52FPS+短执行窗口未暴露。
- **顺带修复 C（潜伏 validation 断言 + VSM blur 静默失效）**：`BlurPass.metal` 两 kernel 的 `params [[buffer(0)]]` 与 C++ 布局/描述符写入的 binding 2 错配（同 AtlasInit 病）——API validation 下硬断言（挡捕获诊断），无 validation 时 params 读 0 → **VSM blur 半径恒 0，blur 在 Metal 上从未实际生效**（82ed01ed 的"radius 8 得以生效"只验证了不崩；此前的双断言修复均未动绑定）。已改 `[[buffer(2)]]` 对齐三端。
- **验证**：5400 帧 **0 fence 超时**（修复前 ~100 帧挂死）、0 GPU fault；f1200 回读非种子 texel **1,248,114**（albedo/normal 两图集 1,248,156 逐 texel 一致，捕获自洽）；每面可见簇 1000-1571、覆盖按 8 卡/帧轮转推进。诊断设施保留：`SC_RB` f60/f1200 种子匹配回读（随 SC_DUMP_CARDS）、CC 系列 TEMP 插桩已全部移除。
- **状态不变项**：整体过曝与捕获修复前完全一致（mean luma 179、~48% 像素 ≥250）→ 实证过曝源**不是**捕获缺失，而在 SC 照明/探针环能量标度（遗留②）——现在两类"假输入"（种子法线/假标度）已排除其一，②的对照可以建立在真实捕获内容上了。

**白爆源分解 + GIGather 输出纹理覆写实锤（2026-08-31 第四轮，仪器驱动）**：用户目检三图（albedo 图集真实内容 ✓ / normal 图集真实内容 ✓ / 最终渲染仍白爆）。A/B 分解（锁相机 + 窗口 luma 统计）：SC_INJECT_OFF=1 → mean 179→80、clip 0%（白能量走 SC→probe 注入路）；SSGI_OFF → fusion 整体被门控跳过 → mean 28（直射-only 基线）；BLOOM_OFF（临时开关）→ 无变化 ✗；TAA_OFF（临时开关）→ 无变化 ✗；SSGI_SILENCE（静音 SSGI 但保留 fusion）→ 无变化 ✗。GI_DEBUG_STRIP 四条带 + 新仪器 IND_STATS（indirect_output_ 全量回读，需 CopySource+TransferDst+延迟 120 帧才可信）测得 fusion 的 ddgi 输入 **mean=72 线性、max≈1390**。可信化后逐级测量收窄：① IRR_DUMP（gather 槽位探针缓冲 raw BufferCopy）maxAbs=1.2-1.9、无 NaN——**探针数据正常**；② SHeval axis-max=0.006——SH 正向瓣上限 0.006，kernel 数学上输出不可能超 ~2.5×π；③ kernel 内加 [0,10] 钳制后输出纹理仍实测 **25227-27731 + 负值 + p50=0**——**巨值/负值/零全部来自 kernel 之外 = gi_halfres_texture_ 被其它 pass 覆写（Metal 纹理分配器别名/复用 bug）**。半分辨率白噪斑纹（用户截图布料区绿白 speckle）即该污染的上采样特征。奇数尺寸（+3）打破别名对齐测试无效（34.2% vs 35.4%，非同尺寸别名）。
- **本轮顺带修复**：a) `black_texture_` 显式清零（Metal 堆纹理**非零初始化**，原代码注释 "Apple Silicon zero-initializes" 是错误假设——回收残留曾被 SPGI/AO 回退槽采样，GI_DEBUG_STRIP 下修后条带版画面 239→88 即证明）；b) GIGather `camPos` CB 错位（`viewCB[4]` 实为 probe_origin_spacing，camera_pos 在 `viewCB[6]`——garbage V/R 污染 specular 瓣）；c) 探针 irradiance 索引 float3→float4（MSL float3 数组步长 16B 与 GLSL std430 一致，等价变换；修正早前"12B 步长"误判）。
- **重大仪器教训**：staging 回读缓冲**必须**设 `bindFlags = TransferDst`，否则 CopyTextureToBuffer 静默无效、读回的是分配垃圾——GIGather 自带 out-lum dump 一直缺此标志，**此前所有"56.6 尖峰→2.07"读数全部无效**（垃圾内存模式，且随堆状态"变化"造成假因果）。修复后真值：gather 输出 p50=0 / p90=1.4 / max≈27000。
- **重大流程教训（CWD/shaders 陈旧副本陷阱）**：TestModularPipeline 的 shader 编译路径优先取 `$(cwd)/shaders/`，而构建系统 `IntegrationTestsAssetsCopy` 是 **stamp 门控的一次性 copy_directory**（构建目录首次配置时拷一次，此后源目录改动永不刷新）——EngineTest/shaders 的修改对已存在的构建目录**不可见**。本轮"float4 修复无效""条带版与正常版矛盾"均由此起。已 `rsync EngineTest/shaders → Darwin/Debug/shaders` 临时解决；长期应在 CMake 里改 per-file copy_if_different 或去掉 CWD 优先。
- **下一 session 主线（白爆最后一环）**：定位覆写 gi_halfres_texture_ 的 pass——嫌疑：SSAO/SSGI/HZB/velocity 等 half-res RT 在 TextureAllocator 中的堆偏移碰撞（多次 InitializeLumenPasses 重建周期的 free-list 状态可疑）。武器已备齐：Xcode GPU capture（两处启动断言已修）、IND_STATS/IRR_DUMP/钳制二分法。判据：capture 里看 gi_halfres_texture_ 的 draw/write 序列；或按堆偏移打印所有 half-res RT 的分配区间找重叠。

**红色写入探针判决 + 根因再修正（同日第五轮）**：在 gather kernel 里写入常量红 (0.5,0,0) → 屏幕 **82.2% 红色主导、0% clip**——① gather 写入端到端直达最终画面（fusion 公式/后处理链全部无辜，白爆修复后整链畅通）；② 白爆 = kernel 算出的 diffOut 自身巨大（半屏 ~8+ 线性）。追加 L0 探针（kernel 直出 `tg_probes[0].sh[0]`，CPU 视图应为 sky≈0.03）→ dump 出 **-87 / 26873**——**kernel 的 threadgroup 视图与探针缓冲的 CPU raw-copy 视图（≤1.9、SH eval≤0.006）矛盾**。CB 上传已验证正确（counts 32×16×32=16384、spacing 3、cam 合理，binding 1/2 偏移 64/80 与结构体对齐）。**结论修正**：上一轮"分配器别名外部覆写"结论作废（newTexture 独立分配不可能别名；gdump 的 27000 是 kernel 自己的垃圾写入——threadgroup 读到的探针数据即垃圾）。真正待解：**为什么 gather kernel 在 GPU 上读 buffer(7) 得到的内容与 CPU raw-copy 不同**——嫌疑集中：① slot 7 绑定未生效/被同 encoder 内其它 setBuffer 覆盖；② MetalDescriptorSet 的 (binding,type) 匹配在纹理+缓冲混合 15-binding 布局下的行为；③ descriptor_set_ 复用 cbIdx 轮换中的脏状态。判决手段：Xcode GPU capture（已解锁）看 ddgi_gi_gather 派发的 buffer(7) 实际内容；或临时把 probe 缓冲同时绑到 slot 9 并让 kernel 双读对比（同数据不同槽=绑定问题实锤）。诊断探针（红色写入/L0 直出/钳制）已回退，保留结论入档。



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
