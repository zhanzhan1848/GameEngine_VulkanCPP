# GI 系统优化总体计划（架构 / 效果 / 接口）

> 日期：2026-08-24
> 状态：已评审定稿（决策记录见 §2）
> 范围：Lumen GI 体系（DDGI / SurfaceCache / ScreenProbe / SSGI / Fusion / GIGather）
> 关联文档：`Docs/2026-08-09-vulkan-lumen-gi-port-design.md`、`Docs/2026-08-19-rhi-vulkan-metal-parity-plan.md`、`Docs/2026-08-21-virtual-texture-system-design.md`

---

## 1. 背景与现状结论

### 1.1 现状盘点（与 Unreal Lumen 三支柱对照）

| 支柱 | 现状 | 差距评级 |
|---|---|---|
| Surface Atlas + Irradiance | AABB 6 面 card + 屏幕回投 gather（离屏覆盖 ~0.1%）+ 标量自反馈；真 radiosity（IndirectTrace/Resolve）已写好但未接线 | 骨架在、闭环弱 |
| World-Space Probe + Radiance | 单 volume 32×16×32 SH3 **irradiance**（L0+L1 稳定），无 clipmap、无远场、origin 锁定 | 类别级差异 |
| Screen-Space Probe + Gather | 全链路实装但默认关闭；均匀网格、仅 SDF trace、miss→天空常量（DDGI fallback TODO）、SH2、temporal 无重投影 | 链路在、关键能力缺 |

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

## 8. 已明确的非目标（Non-Goals）

- 硬件 RT（无设备，决策④）；
- 运行时任意组合的通用 GI 框架（轴化接口但具体化管线）；
- per-probe 自由摆放（仅 volume 级编辑；per-probe 仅在静态烘焙有明确需求时重议）；
- 跨平台像素级一致（只保证同 Profile override 下一致）；
- 传统页表式软件 VT（若 M6 立项，走 tile-array 形态，见 `2026-08-21-virtual-texture-system-design.md` 的衔接评估）。
