# GI 系统补充规划（四轮审计收口）

**Date**: 2026-09-07
**Status**: 待评审 → 执行
**执行 Todo**: `tasks/gi-supplement-todo.md`
**审计背景**: 2026-09-07 对 GI 栈（SurfaceCache + DDGI + SPGI + SSGI + GTAO + GlobalSDF/HZB）做了四轮审计：
①对照 Lumen 源码笔记的特性面对比（`2026-09-07-lumen-source-analysis-notes.md`）；②动态响应/漏光对抗/集成层；
③平台 parity/物理记账/性能足迹；④shader 数学内核逐行审读 + 越界同步 + 生命周期 + 文档漂移。
四轮定性：**缺**（65% 特性面）→ **静态**（三大缓存对几何变化零响应）→ **错/漂/费**（能量记账错误、三端三个 GI、白烧资产）→ **烂**（代码/字节级真 bug，GI 在产品形态里未运行）。

**关联文档**: `2026-08-09-vulkan-lumen-gi-port-design.md`（D1-D7 决策仍有效）、`2026-08-19-rhi-vulkan-metal-parity-plan.md`、`2026-08-24-gi-optimization-plan.md`（T8.x 执行日志可信，"当前默认值"描述已过时）、`tasks/SDF-GI-Enhancement-Spec.md`（僵尸 DRAFT，已被本规划的 D3 包实质取代，待补 superseded 标注）。

---

## 1. 发现索引（销账编号）

> 编号约定：**A**=功能损坏、**B**=数学/能量记账错误、**C**=同步/越界、**D**=结构/鲁棒缺口、**E**=三端 parity/构建、**F**=集成/工具/文档。Todo 条目以"关闭 A1"等标注销账。

### A. 功能性损坏（P0）

| ID | 发现 | 证据 |
|---|---|---|
| A1 | Vulkan 体素化 min_dist 不落盘（恒 77.0 调试 store）+ InstanceData 偏移错（cluster 应@136/140 实取 72/76；radius 应@172 实取 176） | `GlobalSDFVoxelizationGLSL.comp:199-200,150-151,63`；Metal 版布局正确 |
| A2 | octDecode 加法折叠与 octEncode replace 不互逆 → z<0（地板）法线畸变 | `SurfaceCacheData.metal:142-157`；GLSL `:143` 同病 |
| A3 | Metal 活捕获路径 6 组 axis×dir 中 3 组 u 镜像（right=cross(fwd,up) 与查找侧约定相反）；两后端 atlas 内容不一致 | `SurfaceCachePass.cpp:1073-1090`、`RHIMath.h:398-413` |
| A4 | SC 光源上传把所有槽位克隆为同一盏方向灯（position=0/radius=1000/w=1）→ 点光路径死 | `SurfaceCachePass.cpp:1276-1289` |
| A5 | governor 的 DDGI probe 预算断线：SetMaxProbesPerFrame 写 `params_`，更新循环读 Initialize 时冻结的 member；WASM 1024 上限双重断线 | `LumenDDGIPass.h:234` vs `cpp:380,1350,1692` |
| A6 | PRIMAL_SC_BUDGET static 一次性覆盖 governor 每帧 SetPageBudget | `SurfaceCachePass.cpp:1204-1212`、`StandardRenderPipeline.cpp:1153-1162` |
| A7 | Vulkan gather 捕获同 texel 无深度比较（后写覆盖，远可胜近） | `SurfaceCacheCapture.comp:128-138` |
| A8 | monolith trace `float3*`（12B）读 finalize `float4*`（16B）写的 SH → 回退路径错位 | `ScreenProbeTraceRays.metal:279 vs :663` |
| A9 | bake 只 UploadToGPU 从不 SaveToFile；任何 GI 设置变更重建 pass 并重载磁盘旧 cache → 烘焙无声丢失/跨场景污染 | `GIEditorPanel.cpp:319-331`、`StandardRenderPipeline.cpp:760-764` |
| A10 | DDGI/SSGI/GTAO 的 prev 矩阵传当前帧 → 时域重投影恒等 | `StandardRenderPipeline.cpp:1591-1592,1662-1663,1379-1380`、`LumenSSAOPass.cpp:457` |
| A11 | 相机 cut 零 history 重置（ResetTAAHistory 仅测试调用） | 全仓无 cut 检测 |
| A12 | 场景切换零 rebuild（NeedsFullRebuild 仅 Initialize/扩容置位）→ 旧 probe 场/旧卡片/别场景 `scene.probe_cache` | `RenderSceneSnapshot.cpp:153,300`、`StaticProbeVolume.h:9-15` |
| A13 | resize 永不重建 GI/GBuffer（TAA 按新尺寸重建并采样旧尺寸输入；有黑屏前科） | `StandardRenderPipeline.h:94-99`、`TAAPass.cpp:126-138`、`Program.cs:122-126` |
| A14 | 游戏模式 spawn 死锁：扩容检查仅在 Rebind/PartialUpdate 内部，而二者从不被调 → 新实体永远不可见 | `RenderSceneSnapshot.cpp:316,359` |

### B. 数学/能量记账错误（P1）

| ID | 发现 | 证据 |
|---|---|---|
| B1 | SPGI 与 DDGI 输出全局标度差 2π；Fusion 却按两者亮度比做替换门 | `ScreenProbeAverage.metal:258-281` vs `DDGIGIGather.metal:278`；`FusionIndirect.frag:103-105` |
| B2 | SPGI L0 相对 L1 强 2×（自归一化比值估计器推导：L0=2·C₀、L1=C₁） | `ScreenProbeAverage.metal:260-281` |
| B3 | 端到端 π² 不自洽（SC atlas 缺 1/π → 探针当 radiance → 屏幕 ×π → Fusion 无 1/π；Metal ddgi×0.5 vs Vulkan ×1.0 吸收） | `SurfaceCacheLightEval.metal:197`、`DeferredLighting.metal:654`、`FusionIndirect.frag:94` |
| B4 | L1 频带 1.5× 过计（L×π，Ramamoorthi 余弦瓣应为 A₁=2π/3） | `DDGIGIGather.metal:55,278` |
| B5 | hitRatio≡1（miss 射线 w=cosθ≥0 被 Average 判为 hit） | `ScreenProbeTraceRays.metal:430,852`、`ScreenProbeAverage.metal:175` |
| B6 | finalize 路径2 双重计入（prevRadiance + albedo×lum）；路径2/3 对 L_i_prev 的 π 处理不一致 | `DDGITraceRays.metal:629-653` |
| B7 | SH 钳位 L0≤10 把探针场动态范围钉死在天空亮度（≈2.8），HDR 反弹整体截断 | `DDGIUpdateIrradiance.metal:188-213`、`DDGIVolumeData.metal:27` |
| B8 | 深度 binning 用固定采样器，与 mode 感知的 trace/SH 投影脱钩（mode≠0 时方向错位） | `DDGIUpdateDepth.metal:65` vs `DDGIVolumeData.metal:155-157` |
| B9 | GTAO×GI 双重遮挡；加性天空地板无可见性来源（深影抬亮）；GTAO 实际未作用于直接光（IBL=0） | `FusionIndirect.frag:42-48,131-132` |
| B10 | 间接 Fresnel 无 (1-F) 扣减（直照链有 kD=(1-kS)） | `FusionComposite.frag:56-59` |
| B11 | StaticProbe PropagateBounce 把 radiance SH 当 irradiance 用（注释与实现矛盾） | `StaticProbeBaker.cpp:320-330` vs `:250` |
| B12 | meshlet 半径不随实例缩放 → 缩放实例被误 cull → SDF 空洞 | `GlobalSDFVoxelization.metal:180-182`、GLSL `:163-165` |
| B13 | ω 超松弛无 prev_dist 趋势条件 → 无符号场跳薄墙（ω=1.5 时厚度 <2.5 体素大概率整段漏检） | `SDFTraceCommon.metal:130-137`、`DDGITraceRays.metal:349-354` |
| B14 | 体素化空域 1e10 与越界哨兵混叠 → SPGI marcher 见 1e10 即 miss，稀疏区域整段丢失 | `ScreenProbeTraceRays.metal:209-214` |
| B15 | emissive 全链死（Metal 捕获 TODO 0；Vulkan comp 连输出语句都没有） | `SurfaceCacheCapture.metal:156-157` |

### C. 同步/越界（P1）

| ID | 发现 | 证据 |
|---|---|---|
| C1 | SPGI Metal frame-2 GPU fault 三嫌疑：H1 dummy→real 资源翻转恰在 pass-frame 2；H2 buffer id 立即回收无 fence（句柄别名）；H3 `ddgi_valid` 恒 1 时以 card_dispatch 顶班 irradiance buffer | `ScreenProbeGIPass.cpp:1390-1401`、`StandardRenderPipeline.cpp:1704`；`MetalDevice.cpp:784-792`、`RHIAllocator.h:216-224`；`SurfaceCachePass.cpp:1728-1731,1755-1758` vs `StandardRenderPipeline.cpp:1542-1551` |
| C2 | RG 别名盲区：同一 lighting atlas 以三个身份 import，跨身份 RAW 依赖对 RG 不可见 | `SurfaceCachePass.cpp:1130`、`StandardRenderPipeline.cpp:1564`、`ScreenProbeGIPass.cpp:1130` |
| C3 | HZB 在 RG 外构建、kernel 绑原始句柄（声明的 Read 不产 barrier） | `StandardRenderPipeline.cpp:2214-2216`、`ScreenProbeGIPass.cpp:1437` |
| C4 | SC atlas bilinear 采样无 half-texel inset（shelf 打包卡边渗色，5 处采样点） | `ScreenProbeTraceRays.metal:370,736`、`DDGITraceRays.metal:610,690`、`ScreenProbeTraceFinalize.comp:201-213` |
| C5 | DDGI 每帧 MapBuffer 读 GPU 在写的 depth history（CPU sync 点 + Metal 共享内存数据竞争） | `LumenDDGIPass.cpp:1642-1653`、`MetalDevice.cpp:808` |
| C6 | worldToCardUV 允许 u==1.0 → 越界一 texel 写邻卡 | `SurfaceCacheData.metal:134-137` |
| C7 | 零向量 normalize NaN 两处（LightDirection=0 的 mode2、curNorm=0 门失效） | `DDGIUpdateIrradiance.metal:78` 等 |

### D. 结构/鲁棒缺口（P2，对应 Lumen 笔记特性面）

| ID | 发现 | 证据 |
|---|---|---|
| D1 | GIGather legacy 单 volume 路径纯三线性、零可见性门（单 volume 配置下漏光防护归零） | `DDGIGIGather.comp:869-968` |
| D2 | Metal 远场 volume 每帧 trace+update 但输出无消费者（DSL 无槽位） | `GIGatherModule.cpp:99-117,508-568` |
| D3 | SC 采样无 depth²/Chebyshev 平面测试（UE Radiance Occlusion 语义整体缺失） | `SurfaceCacheData.glsl:105-131` |
| D4 | 无 irradiance bailing | `DDGIUpdateIrradiance.metal:148-200` |
| D5 | probe relocation 只移 ray origin 不回传（UBO 预留 ProbeRelocationShift 未读） | `DDGITraceRays.metal:96-101,519-521` |
| D6 | GlobalSDF unsigned（无内外符号）+ MergeCascades 空函数 | `GlobalSDF.cpp:245-246` |
| D7 | 无 cone tracing（SDF 无 mip；全仓零 conetrace） | — |
| D8 | 三大缓存静态：卡片不随物体移动/增删失效、SDF 只认相机跨格、DDGI 盒子锁死初始位置 | `StandardRenderPipeline.cpp:874-897`、`GlobalSDF.cpp:240`、`LumenDDGIPass.cpp:1183-1190` |
| D9 | SC 虚拟化整层缺失：离线 card 生成、Surface Mipmap、Virtual Page/PageTable、BC 压缩、LRU、视距直方图（现为静态 shelf 图集 + 平坦轮询） | `CardGenerator.cpp`、`SurfaceCachePass.cpp` |
| D10 | Radiosity 脚手架未接线（IndirectTrace/Resolve 管线创建后从不 dispatch） | `SurfaceCachePass.cpp:853,861` |
| D11 | IS 体系缺失：八面体光线/立体角 LUT/结构化细分（现为 Fibonacci + 逆 CDF 简化版） | `ScreenProbeTraceRays.metal:130-172` |
| D12 | 防御缺口：cardIdx≥4096 硬上限与 max_cards 脱钩、SetSurfaceCacheResources 单边判 INVALID、scSamplePrevColor nearest 量化 | `DDGITraceRays.metal:219,606,686`、`LumenDDGIPass.cpp:1133-1150,390-391` |
| D13 | SPGI 无 resize 路径（Initialize 早退） | `ScreenProbeGIPass.cpp:247` |

### E. 三端 parity / 构建（P1-P2）

| ID | 发现 | 证据 |
|---|---|---|
| E1 | 同名 shader 漂移到算法级：GIGather 三端三种重建算法；DIRECT_INTENSITY 四入口四值（3.125/5.0/7.5/7.5）且注释自相矛盾 | `DDGIGIGather.{comp,metal,wgsl}`、`DeferredLighting.metal:313,560`、`DeferredLighting.frag:58` |
| E2 | 端能力断点：Metal far 绑定缺、Dawn SC 套件全缺/无 Fusion/GTAO legacy、Froxel 仅 Metal、DawnInterface stub | `GIGatherModule.cpp:508-568`、`FroxelFogPass.cpp:61-63`、`Dawn/DawnInterface.cpp` |
| E3 | GIGatherModule 编译期 include VulkanDevice.h | `GIGatherModule.cpp:5` |
| E4 | 68 个 env 无清单无优先级文档；PRIMAL_SPGI_PARITY no-op；FusionIndirect_noAll.frag.spv 测试引用但构建脚本不产 | `ScreenProbeGIPass.cpp:190-199`、`build_spv.sh:375-392`、`TestVulkanSponzaRenderGraph.cpp:395` |

### F. 集成 / 工具 / 文档（P2）

| ID | 发现 | 证据 |
|---|---|---|
| F1 | GI 只活在测试 exe：编辑器模式走 forward 不跑 GI 链；PIE 不画场景实体 | `StandardRenderPipeline.cpp:2131-2157`、`EngineViewport3D.axaml.cs:1483-1497` |
| F2 | CLI 零 gi.* 命令（EngineDLL ~25 个 GI 导出未投影进命令面，违反自家 CLI 纪律） | `RenderPipelineAPI.cpp:414-1158`、`PrimalEditor_Avalonia/Cli/` |
| F3 | Mode 10（静态模式）修复过竞争但零调用方激活 | `LumenDDGIPass.h:422-436` |
| F4 | 死旋钮群：max_gi_time_ms、ssdo_*（5 字段）、shadow_cascade_split、axes.directional/dfg_path、sc_capture/sc_lighting/sc_shadow 面板开关（无 pass 读）、fusion_glossy_strength、ssgi_ray_count（配 4 实际 8）、surface_cache_update_distance/importance_weight | `LumenTypes.h:83-156`、`GIDebugParams.h:26-38` |
| F5 | 死资源：67MB ray_hits_buffer + 4 条死管线（.comp 编译进包）、staticSkySH/skyFactor/staticProbeParams 每帧绑定无人读、gi_halfres_history_ 永不采样、IBL 每次换 env 全量预计算但 IBL_INTENSITY=0、per-mesh SDF 三张 3D 纹理只喂 debug、GI 栈内存 ~740MB(High+)/1GB(含离线 SDF) 无 WASM 护栏 | `SurfaceCachePass.cpp:350-360,737-819`、`GIGatherModule.cpp:432-433`、`ForwardSceneRenderer.cpp:1763-1829`、`RHIGpuMesh.cpp:190-256` |
| F6 | 文档漂移：SDF-GI-Enhancement-Spec 僵尸 DRAFT；预设阶梯注释三方矛盾（Medium 已含 SC/High 已抑制 SSGI/Ultra≡High）；LumenConfigFromPreset 死代码；EngineDLL 默认静默 Low | `tasks/SDF-GI-Enhancement-Spec.md`、`RenderPipelineConfig.cpp:133-142,195-209`、`RenderPipelineAPI.cpp:650-659` |
| F7 | 测试门禁缺口：Vulkan GI 零数值门禁；Metal 仅 5 个全图亮度带（抓不住 L1 方向性错）；无 NaN/golden-image/能量守恒断言；SPGI Metal 不在默认门禁 | `TestModularPipeline.cpp:1525-1548` |

---

## 2. 架构决策

### 2.1 总原则

> **机制进 RHI（三端实现同一语义），算法进 pass（一份实现三端受益），事件进引擎层。**
> RHI 准入判据：只有当 **(a) 三端能给出同一语义** 且 **(b) pass 层无法用数据布局规避** 时，才新增 RHI 抽象。

### 2.2 两个关键数据布局决策（替代 RHI 新抽象）

1. **DDGI 多 volume → 单一 flat buffer + 偏移表**。近/远场 N 个 volume 打包进一个 storage buffer（volume header 数组 + 连续 probe 数据），gather 按偏移索引。三端天然一致，**顺带关闭 D1（legacy 双槽路径整体删除）与 D2（Metal 远场复活）**，不再需要 descriptor indexing（`SupportsDescriptorIndexing` 降级为性能提示位）。
2. **硬件 RT 不进 RHI**。`supportsRayTracing` 能力位保留但无抽象：MoltenVK 与 Dawn 均无 RT，抽象只有 Metal 一个实现者必然腐烂。SDF 路线保持可移植主线；Metal 专属 RT 加速列为 tier-3 远期，触发条件：cone tracing 落地后仍有精度/性能缺口时再评估。

### 2.3 不做清单（本轮明确排除）

| 项 | 理由 | 替代 |
|---|---|---|
| RHI bindless / descriptor indexing 抽象 | Dawn 无此能力；只有 Vulkan 一个真实现 | flat buffer + 偏移表（§2.2.1） |
| RT 加速结构抽象 | 单端无第二实现者 | 能力位保留，tier-3 再议 |
| RHI 层 resize API | resize 是引擎策略（哪些资源重建、何时 reset history），不是机制 | 引擎层重建策略（§7）+ RHI 资源池 |
| Voxel Lighting 整模块复刻 | DDGI 已占"世界空间低频缓存"生态位 | cone-trace 采样路径 + 世界 probe 语义强化（D3 包），与笔记机制等价、复杂度低一档 |

### 2.4 后端分级

- **Tier-1（Vulkan + Metal）**：全功能对等，是所有验收门禁的对象。
- **Tier-2（Dawn/WASM）**：GI 精简档（DDGI 直加 = 现状），只要求不回归、不阻塞主端设计；WASM 增加总内存护栏（GI 栈 ≤ 上限，超限自动降档到 Off）。

---

## 3. 分层归属总表

| 层 | 本轮补充内容 |
|---|---|
| **RHI** | R1 延迟释放、R2 异步读回、R3 时间戳核实/补齐、R4 indirect dispatch、R5 barrier 语义收口 |
| **RenderGraph** | 单一 import 身份规则、RG 外写收编（HZB）、resize/history-reset 钩子 |
| **Shader 构建** | 单源规范 + 转译链、过渡期三拷贝 hash 门禁、env 清单文档化 |
| **GI 算法** | D0 热修包、标度统一（B1-B7）、cone tracing/Radiosity（D6-D7,D10）、IS（D11）、SC 采样防护（D3-D4） |
| **引擎/生命周期** | GI 事件总线（A11-A14、D8）、PIE/编辑器模式统一（F1）、CLI 命令面（F2） |
| **内容/离线** | 离线 card bake（D9 前半）、per-mesh SDF 运行时接入、emissive 资产链（B15） |

---

## 4. RHI 层详述

### R1. fence 感知的延迟释放（关闭 C1-H2）

- **现状**：`RHIAllocator::FreeInternal` 立即析构 + 回收 id，无 fence 延迟（`RHIAllocator.h:216-224`、`MetalDevice.cpp:784-792`）。in-flight 命令靠 MTLBuffer retain 保命，但 **id 立即可复用** → 旧句柄别名到新 buffer（frame-2 fault 的 H2 嫌疑；Vulkan 理论同险）。
- **补法**：ResourceHandle 增加 generation 字段（首选，编译期 static_assert 锁尺寸沿用 ABI 纪律），或统一延迟 free-list（挂 3 帧 fence 后真释放）。销毁 API 语义改为"标记销毁"。
- **三端要点**：Metal/Vulkan 各自在 GC 队列消费；Dawn 天然引用计数可空实现但保持 generation 校验一致。
- **验收**：`BuildCardGrid` 的 Destroy/Create 压力测试（每帧重建 grid）下 Metal 无 fault；C1-H2 嫌疑可定罪/排除。

### R2. 异步读回统一（关闭 C5）

- **现状**：`RHIReadbackRing` 已存在但 GI 未使用；DDGI 每帧 MapBuffer 读 GPU 在写的 depth history——CPU sync 点 + Metal 共享内存竞争。
- **补法**：probe 方差调度改走 ReadbackRing（N=2 帧延迟换零阻塞）；写进引擎纪律：禁止每帧 Map 读 GPU 本帧/上帧写过的资源。
- **验收**：RG_PROF/日志确认 GI 链无每帧 CPU 等 GPU 点；probe 调度行为在 N-1 帧延迟下无回归（对比测试）。

### R3. 时间戳/QueryPool 三端核实与补齐（M1 性能工作前置）

- **现状**：`gi-optimization-plan` T1.5 曾记录"三端已具备 timestamp（RHICommand.h:521）"，但 RG_PROF Top-24 实际只在 Vulkan 出过数；`RHIDevice.h:196` ResetQueryPool 对无 query pool 后端为空默认。**先核实 Metal 端到端可用性，再决定是补实现还是只补 instrumentation。**
- **验收**：Metal 跑 `RG_PROF=1` 输出 GI 各 pass 耗时（与 Vulkan 同格式）。没有它，M2/M3 的性能验收不成立。

### R4. Indirect dispatch / GPU counter 语义统一

- **现状**：`DispatchIndirect` Vulkan 已实现但全仓无调用者（`VulkanCommandBuffer.cpp:1273`）。
- **补法**：三端统一 DispatchIndirect + count buffer 语义（Metal `dispatchThreadgroupsWithIndirectBuffer`；Dawn indirect 已有，注意 WGSL 限制）。首消费者：GI 页预算/probe 调度逐步 GPU 化（256 灯上传、partial_sort 的替代路径在 M5 评估）。
- **验收**：三端各有一个 GI 链路的 indirect dispatch 冒烟。

### R5. Barrier/同步语义收口

- **补法**：① 三端 MemoryBarrier 语义文档（尤其 Metal "encoder 末尾隐式同步"的边界与例外）；② Submit 全局 fence 暴露（resize/场景切换 CPU 锚点）；③ 配合 RenderGraph 规则：**同一物理资源只允许一个 import 身份**（关闭 C2），HZB 等 RG 外写收编进 RG（关闭 C3）。
- **验收**：Vulkan validation 全绿 + RG 断言"多身份 import 即报错"。

---

## 5. Shader 构建层（与 RHI 同级的第二支柱）

### 5.1 现状

GI kernel 三份手拷贝（Vulkan .comp 37 + Metal .metal 36 + Dawn .wgsl 11）+ EngineTest 第四份；已漂移到算法级（E1）。`ShaderIR/` 是材质图的 Metal emitter，不承担跨端 kernel。`gi-optimization-plan` T3.5 曾立"GLSL primary 共享库"目标未完成——本规划继承该方向。

### 5.2 目标链路

**GI 计算内核以 GLSL（comp 风格）为单一规范源** → glslang → SPIR-V（Vulkan 直用）+ spirv-cross → MSL（Metal）。这是 MoltenVK 同款成熟链路；反方向（MSL→SPIR-V）无可靠工具链。Fusion/Deferred 片元同理。共享数学库（SDFTrace/octEncode/SH 基/采样器）先抽 GLSL canonical 头，两端 include/生成消费。`ShaderIR/` 或新建 `tools/shader_build` 承载构建脚本。

### 5.3 过渡期闸门（单源迁移完成前必须先上）

- **三拷贝 hash 一致性 CI**：同名文件内容 hash 不一致即红。立即止住 DIRECT_INTENSITY 式漂移；存量分叉一次性对齐或显式标注豁免清单。
- **诊断变体机制统一**：DDGI_PATH_DEBUG 一类宏改统一注入方式（当前 Vulkan 独立 .spv、Metal env 注入宏，两套机制，E4）。

---

## 6. GI 算法层（五个包，一份实现三端受益）

### D0 正确性热修包（P0，对应发现 A1-A8、B8、B12-B15、C4、C6-C7）

单点修复清单（每项证据见 §1 索引）：A1 Vulkan 体素化（删 77.0 + 修偏移）、A2 octDecode 改 replace、A3 捕获 u 镜像（per-dir 选 up 或 FS 翻转）、A4 灯上传接 FrameData 真实光源、A8 步长统一 float4、B8 深度 binning 走 mode 感知采样器、B12 半径 × 实例最大缩放、B13 ω 判据加 `d_i ≥ d_{i-1}` 趋势条件、B14 空域哨兵与越界哨兵分离（空域填远距常量）、B15 emissive 进捕获与 LightEval、C4 half-texel inset、C6 u<1.0 判定、C7 零向量 guard。
**验收**：每项有前后 dump 对比（现有 SC_DUMP_CARDS/DDGI_DUMP 体系）；A1 验收 = Vulkan 无离线 bake 时 DDGI_DUMP=2 路径直方图出现有效 hit。

### D1 时域卫生包（P0，对应 A9-A13 + C1 定罪）

prev 矩阵真传（DDGI/SSGI/GTAO）、camera cut 检测 + history 重置 API（RG 层信号，TAA/SPGI/GI 共用）、场景切换置 NeedsFullRebuild + probe 场清零 + probe_cache 按场景哈希、resize 重建策略（RG 钩子 + 资源池）、游戏模式 spawn 走 PartialUpdate。**同步做 C1 定罪**（R1 落地后按 H1→H3 顺序排除）。
**验收**：sequencer 切镜无拖影帧（录屏对比）；切场景 3 帧内 probe 场重建；resize 后无黑屏/混用帧。

### D2 能量记账统一包（P1，对应 B1-B7、B9-B11）

**一次性把全链单位制定为 SI**（直照惯例 `(albedo/π)E`），消灭：SC atlas 缺 1/π、L1 频带 A₁、SPGI (2π,π)→统一投影/求值约定、path2 双计、ddgi 平台旋钮归一、GTAO 对 GI 降权为短程项、间接 Fresnel (1-F)、StaticProbe 传播单位修正。
**前置**：F7 的数值门禁先落地（否则修完无法证明）。
**验收**：均匀场景标定（已知天光/单灯）下三端 gather 输出与解析解误差 < 设定阈值；灯开关响应半衰 ≤ N 帧（量化入 CI）。

### D3 SDF 地基 + Radiosity 包（P2，对应 D1/D2/D3/D4/D5/D6/D7/D10）

顺序：signed SDF（离线已有 signed bake，运行时 cascade 补符号判定）→ MergeCascades 真实现（单 clipmap 纹理或采样器内统一寻址）→ SDF mip + **cone tracing**（立体角→mip 选择）→ 以现有 IndirectTrace 脚手架接活 **Radiosity**（Tile 化 + cosine 采样 + PlaneWeight 滤波 + SH2 投影 + 时域累积，对照笔记 0x32）→ SC 采样加 depth² 平面测试 + irradiance bailing + relocation 回传。**Voxel Lighting 不整模块复刻**（§2.3）：cone-trace 采样路径 + 世界 probe 语义强化（SPGI far-cache 升级为带 mip 的真 radiance cache）即达成笔记等价机制。
**验收**：二次反弹路径可视化（GI_ISOLATE 变体）；漏光用例（薄墙/地牢）通过率门禁。

### D4 动态场景包（P2，对应 D8、A14 深层）

QueryDirtyTransforms → GI 事件总线：卡片捕获失效（移动/材质版本号）、GlobalSDF revoxelization 置位（几何 dirty AABB）、DDGI probe 优先级 boost（灯变化/几何变化加速收敛）、SC 直照灯变化即时性保持。管道前半截（Transform dirty flag）已存在（`Transform.cpp:247-258`、`SceneExtractionSystem.cpp:110-146`），缺的是接进 GI 门口。
**验收**：移动/删除物体后卡片内容更新延迟 ≤ 设定帧数；Sponza 移灯场景 GI 收敛时间量化。

### D5 功能面收尾包（P3，对应 D9、D11、F4、F5）

视距直方图 card 调度（替换平坦轮询）、离线 card bake 接入后的按需更新、结构化 IS（八面体 + 立体角 LUT + 细分重定向，对照笔记 0x34）、死资源清理（67MB ray_hits、4 死管线、绑定无人读的 staticSkySH 等）、死旋钮接线或删除、文档回填（F6）。

---

## 7. 引擎 / 生命周期层

1. **GI 事件总线**：场景 switch/spawn/destroy/材质改/灯改/cut/resize → 单一失效事件入口（消费方：SC、GlobalSDF、DDGI、SPGI）。解决"三大缓存只认全量重建 + 相机移动"（A11-A14、D8）。
2. **编辑器/游戏模式统一**（F1）：编辑器分支接入 deferred+GI（或明确产品决策"编辑器不做 GI"并在 UI 标注）；PIE 接 RenderSceneSnapshot（当前不画实体）。
3. **CLI 命令面**（F2）：EngineDLL 既有 ~25 个 GI 导出投影为 `[CliCommand]`：`gi.status / gi.set / gi.preset / gi.bake / gi.probe.dump / gi.volume.*`，重生成 schema 快照（遵守 AGENTS.md §6.3：单位/坐标/失败语义写清）。
4. **Mode 10 处置**（F3）：接入 SPGI_CAM 类测试激活验证，或显式删除分支——不留"修了但没人用"的状态。

---

## 8. 内容 / 离线层

1. **离线 card bake**（ContentTools）：体素化 mesh + 每 Cell 射线采样 + Surfel 内外判定 → 6 方向 Card bounds。现有 `ContentTools/common/SDF` 烘焙管线可复用；产出替代运行时 AABB 派生卡，GBuffer-gather/CardFill 兜底降级为刷新机制而非内容来源。
2. **per-mesh SDF 运行时接入**：已上传 GPU 的 sdf/voxel/vector_field 三张纹理（`RHIGpuMesh.cpp:190-256`）目前只喂 debug。最低目标：SDF 命中点用 mesh SDF 精化 + 梯度法线重建（detail tracing 的第一步）。
3. **emissive 资产链**：GBuffer 输出 emissive → 捕获 → LightEval → 探针反弹（配合 B15 修复与笔记 0x32 的 Radiance Intensity Clamp）。

---

## 9. 测试与门禁策略（贯穿所有里程碑，先门禁后动手）

| 门禁 | 内容 | 覆盖 |
|---|---|---|
| 三端数值 band | 把 TestMetalGIFusion 的 5 断言模式推广到 Vulkan（TestVulkanSponzaRenderGraph 增帧 90 数值断言 + `_Exit(0/1)`） | F7 |
| SH 方向性 golden | 固定射线集/固定场景的探针 SH 期望 CSV diff（抓 L1 符号/轴向错——现有门禁盲区） | B1/B2/F7 |
| GPU NaN 断言 | gather/final 输出 NaN/Inf 检查 | C7 |
| 图像回归 | ImageCompare（SSIM）基建扩到 GI 冒烟（锁定机位 PPM 已有，补 diff 步骤） | F7 |
| shader 一致性 | 三/四份拷贝 hash CI（§5.3） | E1 |
| barrier 断言 | RG 多身份 import 报错 | C2 |
| 内存护栏 | WASM GI 总量上限 + 自动降档 | F5 |

---

## 10. 里程碑与验收

| 里程碑 | 内容 | 关闭发现 | 验收摘要 |
|---|---|---|---|
| **M0 热修周** | D0 + R1 + D1（含 C1 定罪） | A1-A8、A9-A14、B8、B12-B15、C1、C4、C6-C7 | 逐项前后 dump；切场景/resize/cut 无伪影；fault 关闭或定罪 |
| **M1 一致性地基** | R2-R5 + shader hash 门禁 + RG 单身份 + RG_PROF Metal | C2、C3、C5、E4、E3 | 三端 RG_PROF 出数；拷贝 CI 全绿；validation 全绿 |
| **M2 标度统一** | D2 + 三端数值 band + SH golden + NaN 断言 | B1-B7、B9-B11、F7(部分) | 标定场景三端误差 < 阈值；灯响应半衰量化 |
| **M3 SDF 地基** | D3 | D1-D7、D10、C1(H2 残留) | Radiosity 四层链路跑通（消融开关）；漏光用例门禁 |
| **M4 动态场景** | D4 + 事件总线 + 离线 card bake + emissive 链 | D8、A14(深层)、B15(闭环)、D9(前半) | 移动/增删失效延迟量化达标 |
| **M5 功能面收尾** | D5 + CLI + PIE/编辑器决策落地 + 文档回填 | D9(后半)、D11、F1-F6 | 笔记特性面自查表复核；`primal gi.*` 走通 |

顺序依赖：M2 依赖 M1 门禁；M3 依赖 M0 的 A1（Vulkan SDF 必须先活）；M4 依赖 M1 的事件机制。M0/M1 可并行推进。

---

## 11. 开放决策点（需产品/负责人拍板）

1. **编辑器模式是否跑 GI**（F1）：接入 deferred+GI 的成本 vs "编辑器明确无 GI"的产品定位。
2. **Voxel Lighting 路线确认**（§2.3）： cone-trace + 世界 probe 强化（推荐）vs 整模块复刻。
3. **Dawn tier-2 的最低 GI 档**（§2.4）：DDGI 直加维持现状即可，还是补最低档 SPGI monolith。
4. **单源迁移的粒度**：全部 GI kernel 一次性切换 vs 新代码单源、存量渐进（推荐后者，配 hash 门禁兜底）。
5. **RT tier-3 的启动条件**（§2.2.2）：建议 cone tracing 落地后按精度/性能缺口评估。

---

## 12. 附：现状关键事实（规划依据）

- RHI 抽象面已具备：SyncHandle、QueryPool（三端有骨架，Metal 完整度待核）、ReadbackRing（未接 GI）、GarbageCollector、DispatchIndirect（Vulkan 已有）。
- `SupportsDescriptorIndexing` 仅 Vulkan override；多 volume 需求由 flat buffer 方案吸收后可降级为提示位。
- 40+ env 旋钮、4 份 shader 拷贝、数值门禁仅 Metal 5 条——是 M1/M2 的主要对象。
- GI 栈显存 ~611MB(Medium)/~740MB(High+)/~1GB(含离线 SDF)；WASM 无总量护栏。
