# SPGI × Unreal Lumen Screen Probe 对照与融合升级方案

> 日期：2026-08-26
> 状态：v6 —— **M7 融合轮 F1-F5 全部实装完成**（GLSL+Metal+C++ 全链路；SPIR-V 编译 / 引擎构建 / Vulkan 设备冒烟，validation 计数与基线一致 106=106；RHI 单测 22/22）。效果级验收（banding/收敛/SDF 步数量化）待 T1.5 基线与录屏。F3 含勘误：双线性混合 T5.2 已有，实装的是 variance clamp + 策略枚举激活。
> 范围：Screen Probe GI（`Engine/Graphics/Lumen/ScreenProbes/`）与其上下游（HZB / GlobalSDF / SurfaceCache / DDGI / Fusion）
> 关联文档：`Docs/2026-08-24-gi-optimization-plan.md`（总体计划，本文是其补集）、`Docs/2026-08-09-vulkan-lumen-gi-port-design.md`
> 信息来源：Vulkan SPGI 全链路代码走查（C++ pass + GLSL kernel）× UE Lumen Screen Probe (2021) 公开方案对照

---

## 1. 背景与文档定位

08-24 总体计划以"三支柱对照"盘点过 GI 体系，其中 SPGI 一行的现状描述（"miss→天空常量、temporal 无重投影"）已过时：T2.2（miss→DDGI fallback）、T5.1（混合 trace 拆分）、T5.2（重投影）、T5.3（Denoise 修复启用）均已合入。本文基于当前代码重新走查 SPGI 全链路，与 UE Lumen 2021 的 screen probe 方案逐维度对照，产出两类结论：

1. **对齐项**：08-24 计划已立项、方向与 UE 一致的任务（只列对齐关系，不重复）；
2. **缺口项**：UE 有、计划未覆盖的融合点，编为 **F 系列**（Fusion），共 4 项。

定位：F 系列不改变五轴框架与三条轨道结构，全部遵守 08-24 §6 执行红线（读数 ≤32/线程、FPS ≥30 带测量、GLSL 单源语义、任务独立可开关）。

---

## 2. 现状：Vulkan SPGI 实现链路

### 2.1 参数（`ScreenProbeParams`）

| 参数 | 值 | 说明 |
|---|---|---|
| downsample_factor | 16 | 每 16×16 像素 1 个 uniform probe |
| rays_per_probe | 64 | Fibonacci 球采样，绕 probe 法线定向，cosine 权重 |
| max_ray_distance | 50 | SDF 远场追踪上限（世界单位） |
| gather_radius | 2 | Gather 双边半径（probe 网格单元） |
| hzb_max_distance | 25 | HZB 近场追踪范围（二楼 probe 打到中殿阳光地板的调参值） |

### 2.2 每帧 dispatch 链（`ScreenProbeGIPass::AddPass`，`ScreenProbeGIPass.cpp:1014`）

单 RG pass "ScreenProbeGI"（Compute/Lighting）内顺序 dispatch，手动 barrier：

| # | 子 pass | shader（Vulkan） | dispatch | 要点 |
|---|---|---|---|---|
| 1 | Place | ScreenProbePlace | (probes+63)/64 | 均匀网格布点；中心采样 GBuffer 重建世界坐标；天空 w=0 标记无效 |
| 2a | HZB 近场 | ScreenProbeTraceHZB | totalRays/64 | HZB min-mip 屏幕空间 march（≤25 单位）；命中写 hit buffer（w=1） |
| 2b | SDF 远场 | ScreenProbeTraceSDF | totalRays/64 | 跳过 w==1 与 w<=0 probe（`ScreenProbeTraceSDF.comp:77-80`）；3 级 cascade sphere tracing；命中写 w=2 |
| 2c | Finalize | ScreenProbeTraceFinalize | totalRays/64 | 按来源取辐照度：HZB→前帧颜色；SDF→SC lighting atlas（card grid 定位）；miss→DDGI 8-probe trilinear SH（w=0，`ScreenProbeTraceFinalize.comp:195`）→天空常量兜底 |
| — | （单体兜底） | ScreenProbeTraceRays | totalRays/64 | 拆分 SPIR-V 缺失时的回退路径 |
| 3 | Average | ScreenProbeAverage | probes（TG=64） | 每 probe 一 workgroup 协作归约 64 ray → SH L0+L1（4×float4）+ confidence |
| 4 | Temporal | ScreenProbeTemporal | (probes+63)/64 | 重投影找历史 cell + 世界一致性校验（`ScreenProbeTemporal.comp:111-138`）；EMA α=0.1 + 梯度/clamp 门控 |
| 5 | Spatial | ScreenProbeSpatialFilter | (probes+63)/64 | probe 网格世界距离双边（sigma=2，中心权重 2.0），histIdx buffer ping-pong |
| 6 | Gather | ScreenProbeGather | (W+7)/8×(H+7)/8 | 逐像素 4 邻近 probe 双线性 + evalSH(N)（`ScreenProbeGather.comp:176`） |
| 7 | Denoise | ScreenProbeDenoise | (W+7)/8×(H+7)/8 | 全分辨率联合双边（depth/normal，radius=1）；输出即 `gi_output` |

### 2.3 数据结构与平台要点

- **hit buffer**（单缓冲 GPU-only，中间量）：vec4/ray = (hit distance, HZB UV.xy, source flag)，flag：0=miss / 1=HZB / 2=SDF —— **F1 将扩展 3=far-cache**。
- **probe 数据**：positions/normals/radiance/SH 全 SSBO 三缓冲（`frame % 3`，历史取 `(frame+2)% 3`），Vulkan 扁平绑定 + readonly 标记，SDF 纹理 is3D。
- **常量**：`ScreenProbeGlobalData`（std140，VP/invVP、网格、SDF cascade、SC 参数、DDGI fallback、HZB march 参数）。
- **shader 源**：GLSL 手写 → `.comp.spv` 预编译；Metal/Dawn 各有移植版（Dawn 端口为 T5.4 未完成项，F 系列改动需三端同步）。
- **trace 三拆**（HZB/SDF/Finalize）源自 Apple Silicon 单 dispatch 读类别约束，Vulkan 上为主路径。

### 2.4 管线接入（`StandardRenderPipeline.cpp:2239`）

- 门控：`frameCount_ > 0`（保证前帧颜色有效）；DDGI fallback 由 `axes.fallback_spgi_to_ddgi` 轴控制，volume view 取 `(frame+2)%3` 历史缓冲。
- 输入：GBuffer depth/normal、前帧 deferred 输出、HZB 纹理（`HZBTexture_SPGI` import + `SetHZBData`）。
- 输出：`spgiOut.gi_output`（filtered）→ FusionComposite（`fusionIn.spgi_rg`，`StandardRenderPipeline.cpp:2423`），与 SSGI/DDGI/AO 融合加权。

---

## 3. 与 UE Lumen Screen Probe (2021) 的差异对照

### 3.1 维度对照表

| 维度 | UE Lumen (2021) | 本引擎 SPGI（现状） |
|---|---|---|
| 布点 | uniform 16×16 + adaptive（插值失败测试，offset+count 列表） | 仅 uniform 16×16，SSBO，天空探针无效标记 |
| 采样抖动 | probe 位置与射线方向逐帧随机偏移（jitter），temporal filter 覆盖抖动、多帧平均（SIGGRAPH 2022） | 固定网格 + 固定 Fibonacci 方向集（结构化 banding 来源） |
| 射线生成 | importance sampling（lighting pdf × BRDF pdf，0-pdf 剔除重分配，texel 最多 4 ray） | Fibonacci 固定 64 方向（`ScreenProbeTraceRays.comp:179`），cosine 权重，无 IS |
| 追踪后端 | Lumen ray casting（surface cache / mesh SDF；近处 screen traces） | 混合三段：HZB 近场 → GlobalSDF 远场 → Finalize 采 SC atlas（T5.1） |
| world probe | ray 超距离阈值**提前停止**转查 8 probe + parallax 校正；world probe 高分辨率且自 trace | 仅**完全 miss** 回落 DDGI：8-probe trilinear SH、无 parallax；DDGI 由 SC cards 更新（非自 trace） |
| probe 表示 | radiance map（texel 级） | SH L0+L1（4×float4）+ confidence |
| 滤波 | texel 级 spatial（G-buffer 指导）+ temporal（4 历史 probe 复用） | probe 级：temporal EMA+重投影门控；spatial 世界距离双边 |
| Gather | 逐像素 BRDF 加权积分 radiance map | 逐像素 4 probe 双线性 + evalSH(N)，再全分辨率 Denoise |

### 3.2 结构性差异（影响能力上限的）

1. **无重要性采样**：方向集每帧每 probe 相同，小面积强光/远距光源收敛全靠 temporal EMA 堆帧数；UE 的 lighting pdf 机制无对应物。
2. **SH L1 表示**：滤波与 Gather 的对象是 16 字节系数，定向性强的 GI（锐利光斑）理论不可表达；UE 的 texel 级滤波/BRDF 积分无对应物。
3. **world probe 触发语义**：UE 是"阈值内自 trace、超阈值查缓存"的性能/稳定性交换机制；我们只有 miss 才查（拿到稳定性一半，没拿到省 trace 的另一半），且 DDGI irradiance 是 SC cards 的低频缓存而非 UE 的独立高分辨率层。
4. **无 adaptive placement**：深度突变处（栏杆、薄几何）靠 Gather 双边权重与 spatial filter 事后缓解，UE 是源头补探针。
5. **temporal 取单 cell**：重投影定位历史 cell 后取命中者的 SH，UE 是 4 历史 probe 双线性混合——快速旋转时屏幕边缘抗丢史能力弱一档。
6. **无逐帧抖动**：UE 对 probe 位置/方向每帧随机偏移并用 temporal filter 平均（`TemporalFilterProbes` 开启时跨帧摊销至 ~1024 rays）；我们每帧输入完全相同，temporal EMA 无法平均掉结构化 banding——这不是质量问题，是"免费采样数"没拿。

### 3.3 同构点（无需动）

近场 HZB + 前帧颜色 ≈ UE screen trace 路径；SDF 远场 ≈ mesh distance field tracing；temporal 重投影门控解决同一问题（UE 换用 4-probe 复用）；trace 三拆的平台约束在 Vulkan 上顺带成为主路径。

---

## 4. 融合升级点（F 系列）

### F1 远场提前停止 + DDGI 缓存查询（优先级最高：省 trace + 稳定性双赢）—— ✅ 已实现

**实现落点**（2026-08-26）：`cache_params`（UBO 偏移 400）承载 `far_cache_start_distance`（默认 30，介于 hzb_max=25 与 max_ray=50 之间，0 关闭）；TraceSDF/单体 march 上限截断，未命中射线写 hit 标志 3（停止点距离）；Finalize 新增 w==3 分支——**4-probe 四面体（Crasin 式按分数分量排序的角链）× L0+L1 = 16 读**，比规划的"8-probe 降档 32 读"更安全；DDGI 不可用/出界回落天空梯度。Metal sdf/finalize/单体三 kernel 字节等价同步。存量注意：GLSL Finalize 的 miss 分支（w==0）8-probe trilinear 为 72 读，超 32 红线——F1 默认开启后该路径仅剩禁用/回退场景触达，建议后续单独收口（并入 A2/F3 轮）。

- **语义**：SDF march 到 `cache_start_distance`（建议 25~30，衔接 `hzb_max_distance=25`）即停，`hit_buffer.w=3`；Finalize 对 w==3 的 ray 在停止点做 DDGI trilinear 求值（复用现有 miss 分支的 8-probe 块，`ScreenProbeTraceFinalize.comp:195-232`）。
- **改动面**：`ScreenProbeTraceSDF.comp`（march 上限 + 新标志）、`ScreenProbeTraceFinalize.comp`（far-cache 分支）、`ScreenProbeParams` 新参数；三端移植（Metal split kernel / WGSL，后者依赖 T5.4）。
- **读数红线核算**：8 probe × 9 系数 = 72 读/线程，**超 32 红线**。远场分支必须降档：L0+L1（8×4=32 压线）或 4-probe 四面体（4×4=16 安全）。T2.2 验收口径为 ≤20 读/线程，需重新核预算并更新预算注释。
- **parallax（可选增强）**：simple parallax 需要 probe 平均距离；DDGI irradiance 的 w 通道目前仅被保留传递（`DDGIProbeIrradianceFromCards.comp:187-188` 的 `vec4(blended, prev.w)`），把其写入语义显式定义为平均距离即可启用。
- **配套**：与 T4.4（远场第二层粗 volume）天然组成两级查询——近场 volume 精确、超界落粗 volume；与决策②方向一致（成本是降不是升）。
- **验收**：SDF kernel 平均步数/GPU time 较 T1.5 基线下降；快速旋转场景屏幕边缘光斑减少（录屏对比）；读数注释合规。

### F2 轻量重要性采样（收敛质量缺口，计划空白）—— ✅ 已实现（2026-08-26，直落形态 c）

**实现落点**：跳过形态 a/b 直接落地目标态——`spgiImportanceDirection` 进 `ShaderLib/EncodeLib.glsl`（Metal 双份字节等价拷贝）：proposal 在旋转后的 Fibonacci 候选集上取 q ∝ max(cosθ, 0.02) × (SH-luminance + 0.05)，ray j 经逆 CDF 在 (j+0.5)/N 处取候选（亮方向吸收暗方向的射线——UE 重分配语义的离散等价物）；消费端权重 cosθ·invNq（invNq = 1/(N·q_c)）保持重要性重采样的无偏性（自归一化比估计）。五个 kernel（TraceSDF/HZB/Finalize/单体/Average）各 +1 只读 SSBO 绑定指向 `probe_avg_radiance_[histIdx]`（上一帧最终 SH，零历史时退化为 cosine-only proposal）；开关走 `cache_params.y`（Average 走常量 pad[1]），`LumenConfig.screen_probes_importance` 默认开。读预算：方向消费者 +4 读（候选扫描为纯 ALU，×4 kernel 重复生成是本设计的代价，记录为后续可优化点——可由 Place 预计算分配表）。

**UE 机制（2026-08-26 联网核实，源码级拆解见 §8 知乎文）**：

1. `GenerateBRDF_PDF` kernel 对 probe 所在 tile 的 GBuffer 材质按**平面距离权重**（exp2(-10000·Δd²)，阈值 0.1）求 diffuse transfer SH3 平均 → probe BRDF pdf，即"probe BRDF = 邻近像素 BRDF SH 混合"（解决一个 probe 服务多法线像素的问题）；
2. LightingPDF = **重投影 2×2 历史 probe**（平面距离有效性 + 曝光校正）与 **World Radiance Cache** 的 alpha 混合——历史命中权重越低，WRC 占比越高；pdf 取 luminance(radiance)；
3. 合成 pdf = BRDF_SH·SH(dir) × LightingPDF，按 pdf 排名重分配：每 3 根低 pdf ray 合并、腾出配额把 1 个高 pdf texel 细分为 4（八面体 mip 一级），MinPDFToTrace 剔除；ray 总数不变，只挪位置；
4. ray 以锥角（texel footprint）trace。

**我们的落地形态（按成本递进）**：

- **形态 a：L1 引导方向扭曲**。history probe SH 的 L1 指向"光从哪来"，将 Fibonacci 方向集向该方向做混合偏移，Average 端同步做权重补偿（MIS 式，防能量偏置）。改动集中在方向生成与权重，**零额外 device 读取**。
- **形态 b：tile 级 ray 配额**。Place 时按 tile 内法线方差在相邻 tile 间挪 64 ray 配额。更粗、更简单，可与形态 a 叠加。
- **形态 c：CDF 重采样（UE 简化版，推荐目标态）**。LightingPDF 代理 = luminance(evalSH(historySH, dir))——历史 SH 已在寄存器，零读取；BRDF 代理 = cosine lobe（probe 法线），可选升级为 Place 改 2D threadgroup（8×8）逐 texel 求平均 diffuse SH（4 texel/线程，读数合规），即 UE `GenerateBRDF_PDF` 的等价物；64 方向按合成 pdf 的 CDF 重采样分配，替代 UE 的八面体细分树。**无 radiance map 也可落地完整的 pdf 分配语义**。
- **真 IS（texel 级 pdf + 细分树）观望**：依赖 radiance map 表示，属架构分叉；沿用 T6.3 立项逻辑——**仅当 T4.2（SH9）+ 形态 c 落地后仍有收敛缺口时立项**（A2 encode 库的八面体编码已为此预留）。
- **验收**：同等帧数下 Sponza 收敛速度提升（SSIM/视觉对比）；均匀照明场景输出与基线逐像素一致（证明无偏置）。

### F3 Temporal 4-probe 双线性 + variance clamp（X3 时域策略的落地形态）—— ✅ 已实现（2026-08-26）

**勘误**：08-26 初版文档称"取单命中 cell"——复查代码确认 T5.2 已实现 2×2 历史 cell 双线性混合（一致性门控 + 权重重归一化，`ScreenProbeTemporal.comp` 的 histSH 循环），本项真正的缺口只有 variance clamp 与枚举接线。

**实现落点**：TemporalConstants 追加 `mode`（96→112B，三端同步）；mode 1（EMAVarianceClamp）= 现有路径 + 邻域 AABB 方差钳制——统计量取自身 + 左右邻 probe 的 L0（2 额外读，预算 31/32），经 `TemporalLib::varianceClampAABB`（mean ± clampScale·σ + floor，SSGI/Metal 谱系）钳制历史 L0，L1 三带按同通道比例收缩（保号）；mode 2（Reprojection）= 纯双线性历史复用（跳过相对钳制与梯度限幅，用于隔离重投影质量）。`GITemporalStrategy` 枚举经轴 5 → `SetTemporalStrategy` → mode 参数**真实生效**（GICacheView.h 映射注释已更新）；默认 EMA=0 行为不变，满足回滚红线。clamp 谱系依据（Kaplanyan 2017 → SVGF → ReLAX/FidelityFX）见 §8 外部参考。

- **验收**：编译/构建/RHI 单测 22/22/设备冒烟 40s/validation 计数与基线一致 ✓；快速旋转无拖影与静态场景无漂移的量化对比待录屏（EMAVarianceClamp 为非默认策略，需轴 5 切换后目检）。

### F4 Adaptive placement 实现细化（对 T5.3 的建议，非新任务）—— ✅ 已实现（2026-08-26）

**实现落点**：Place 的 fine 档激活测试替换为 UE 式插值失败驱动（`cache_params.z` 阈值，默认 0.35 世界单位，0 回退 T5.3 方差启发式）——奇数格中心用 4 个棋盘边邻粗档 probe 的深度反投影位置做双线性重建，与实际位置比较；任一粗档邻居为天空 → 直接判失败（深度不连续处插值不可信，正对应 UE 测试的原始动机）；屏幕边缘部分邻居缺失按缺失比例放宽阈值。**读数反而下降**：F4 路径 4 次深度读取 vs 旧方差路径 8 次（depth+normal 各 4）。三端同步（GLSL/Metal Place + 参数/配置链）。

- T5.3 现规划"方差驱动密度"；建议改用 **UE 式插值失败测试**：用 Gather 同款双线性从 uniform probe 位置插值出世界坐标，与该像素深度反投影实际坐标比较，超阈值处补 probe。
- **结构性优点**：直接度量最终重建误差，"补在哪"与"Gather 怎么采"数学闭环，比深度方差更准；Place kernel 增量十几行。
- 数据结构沿用计划：第二级稀疏网格 + offset+count 列表。
- **验收**：栏杆/薄几何场景漏光伪影减少；probe 总数增幅 ≤ 预算（建议 ≤2×，配 T4.1 调节器档位）。

### F5 探针/射线逐帧抖动（jitter）（2026-08-26 补，性价比最高）—— ✅ 已实现

**实现落点**（2026-08-26）：抖动函数进 `ShaderLib/EncodeLib.glsl`（`spgiJitterHash` / `spgiJitterRotation` / `spgiJitterOffset01`，Rz·Rx90°·Rz 两参数旋转族）；六个 GLSL kernel 接线（Place 位置抖动 + 世界幅度钳制；TraceRays/HZB/SDF/Finalize/Average 方向旋转，Average 改引 EncodeLib 并删除本地副本）；Metal 三个文件字节等价同步；种子经 `camera_position.w`（Average 走 AvgConstants.pad[0]），幅度经 `surface_cache_params.w`；`ScreenProbeParams.enable_jitter / jitter_max_world`（默认 0.2 < posThreshold/2）+ `LumenConfig.screen_probes_jitter` 开关。零额外读取（Place 最坏 +3 读，预算 13/32）。

- **UE 机制（SIGGRAPH 2022）**：probe 位置与射线方向每帧随机偏移，temporal filter 专门为覆盖抖动而设计；多帧平均等效提升采样数。`TemporalFilterProbes` 开启时跨帧摊销至 ~1024 rays——与我们 EMA 的跨帧积分同族，但**前提是每帧输入要有变化**。
- **我们缺失的后果**：固定网格 + 固定 Fibonacci 方向集 = 空间与方向双结构化采样；静态场景下每帧输入完全相同，temporal EMA 平均不掉 banding/网纹伪影。
- **改动面**：Place 里 probe 中心加 hash(frameIdx, cellId) 屏幕空间偏移（世界幅度 ≤ posThreshold ½）；Trace 里 Fibonacci 方向集按 hash 做整体随机旋转（cosine 权重不因整体旋转改变，Average 无需改）。两处各几行，**零额外 device 读取**。
- **约束**：抖动幅度必须小于 temporal 一致性阈值（posThreshold=0.5），否则每帧触发历史拒绝、收益归零——需带"超限自动关闭"降级；三端同步。
- **验收**：静止场景 banding/结构伪影消除（连续帧抖动 + temporal 平均生效）；快速移动场景与基线无回退。

---

## 5. 与 08-24 总体计划的对齐关系

| UE 机制 | 本文档编号 | 08-24 归属 | 关系 |
|---|---|---|---|
| world probe 提前停止 + 缓存查询 | **F1** | M7 / 与 T4.4 配套 | **缺口（新增）** |
| importance sampling | **F2**（轻量版） | M7 | **已实装**（直落形态 c） |
| temporal 4-probe 复用 + variance clamp | **F3** | M7；A2 时域库 / B6 / X3 暂存枚举 | 双线性系 T5.2 既有；clamp+枚举已实装 |
| adaptive placement | **F4** | M7（原挂 T5.3） | **已实装**（插值失败测试驱动） |
| 探针/射线逐帧抖动 | **F5** | M7 | **缺口（新增，v2 补）** |
| 表达力（SH L2 / ray 128） | — | T4.2 | 已立项，无需重复 |
| Gather 端 BRDF 积分（光泽反射） | — | T4.3（反射向量 SH9 + DFG LUT） | 已立项；我们做在像素端，UE 在 probe 端，等价 |
| radiance map 表示 | 观望 | A2 八面体 encode 预留 | 条件立项（T4.2 后评估） |
| SSGI/screen probe 近场合流 | 观望 | B5 Fusion 统一 | T4.1 GPU time 基线后评估 |

---

## 6. 实施排序与依赖

```
F5 逐帧抖动（最便宜：Place/Trace 各几行、零额外读取，先行）
F1 提前停止（省 trace，独立可开关）
 └─ 可选增强：parallax（w 通道语义定义）
F3 Temporal 双线性 + clamp（A2 时域库一处改动，X3 收尾）
F2 形态 b tile 配额 → 形态 a L1 引导扭曲 → 形态 c CDF 重采样（质量递进）
 └─ 真 IS 观望：T4.2 SH9 + 形态 c 落地后评估
F4 T5.3 实现细化（随 T5.3 排期采纳）
```

排序理由：F5 改动最小且独立于其他项；F1 是唯一"降成本"项且基础设施齐备；F3 改动集中、直接兑现暂存枚举；F2 收益递进可拆；F4 挂靠既有任务。全部任务独立 flag 可回滚（红线 ④）。

里程碑归属：F 系列已并入 08-24 总体计划为 **M7 融合轮**（B7 轨道条目）；WGSL 侧依赖 T5.4 端口同步。

---

## 7. 非目标与风险

**非目标**（沿用 08-24 §8，另增两条）：

- 硬件 RT（决策④）；per-probe 自由摆放（仅 volume 级编辑）；
- **radiance map 全表示**：T4.2 SH9 落地且收敛达标则不立项；
- **SSGI 合流**：SPGI 的 HZB 近场与 SSGI pass 存在重叠计算，长期可共享 hit buffer 或将 SSGI 降级为 SPGI 关闭时兜底，但属架构收敛，T4.1 基线数据齐前不评估。

**风险与对策**：

| 风险 | 对策 |
|---|---|
| F1 读数超 32/线程红线 | 远场分支降档（L0+L1 或 4-probe）；预算注释随 PR 更新 |
| F2 引入能量偏置 | 权重补偿 + 均匀场景逐像素回归测试进验收 |
| F5 抖动幅度与 temporal 一致性阈值冲突 | 幅度约束 ≤ posThreshold ½；超限自动关闭（降级路径） |
| F3 双线性混合引入拖影 | neighborhood clamp + 一致性门控；静态场景与 EMA 基线对比 |
| 三端漂移 | GLSL primary 单源（红线 ③），Metal/WGSL 移植 checklist 进 PR 模板；F1 的 WGSL 侧依赖 T5.4 |
| DDGI w 通道语义变更破坏现有消费者 | F1 parallax 增强独立 flag，写入语义变更前核查 `DDGIGather` 等读取方 |

---

## 8. 参考

**代码（走查基线，dev 分支 220cbda）**：

- Pass 主体：`Engine/Graphics/Lumen/ScreenProbes/ScreenProbeGIPass.cpp` / `.h`
- Vulkan kernels：`Engine/Graphics/Vulkan/Shaders/Lumen/ScreenProbe{Place,TraceHZB,TraceSDF,TraceFinalize,TraceRays,Average,Temporal,SpatialFilter,Gather,Denoise}.comp`
- DDGI 侧：`Engine/Graphics/Vulkan/Shaders/Lumen/DDGIProbeIrradianceFromCards.comp`、`Engine/Graphics/Lumen/GICacheView.h`（GITemporalStrategy）
- 接入与融合：`Engine/Graphics/RenderPipeline/StandardRenderPipeline.cpp:2239`、`Engine/Graphics/RenderPipeline/Modules/FusionCompositeModule.h`

**文档**：`Docs/2026-08-24-gi-optimization-plan.md`（五轴框架 / 三轨道 / M1-M6 / 执行红线）、`Docs/2026-08-09-vulkan-lumen-gi-port-design.md`

**外部**（2026-08-26 联网核实）：

- Epic 官方技术博客 [Unreal Engine 5 goes all-in on dynamic global illumination with Lumen](https://www.unrealengine.com/tech-blog/unreal-engine-5-goes-all-in-on-dynamic-global-illumination-with-lumen)（SIGGRAPH 2022 课程配套）——final gather / screen-space radiance cache；
- Wright，*Radiance Caching for Real-Time Global Illumination*（SIGGRAPH 2021 Advances）——importance sampling（incoming radiance × BRDF）与 SSRC 多反弹应用；
- Wright et al.，*Lumen: Real-Time Global Illumination Techniques*（SIGGRAPH 2022 Advances）——probe 位置/方向逐帧 jitter + temporal filter 覆盖、bilateral history reuse；
- 知乎 [Lumen系列之Importance Sampling](https://zhuanlan.zhihu.com/p/713507304)——`GenerateBRDF_PDF` / LightingPDF 历史混合 / 3 合 1 细分 / cone trace 的源码级拆解（§4 F2 机制依据）；
- Schied et al.，[Spatiotemporal Variance-Guided Filtering（SVGF, NVIDIA 2017）](https://research.nvidia.com/publication/2017-07_spatiotemporal-variance-guided-filtering-real-time-reconstruction-path-traced) 及 Kaplanyan 2017 深度 G-buffer——neighborhood/variance clamp（mean ± k·σ）谱系（F3 依据）；[AMD FidelityFX Denoiser](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/denoiser/) 为工程参考；
- Masaryk 大学论文 [Real-Time Global Illumination in Unreal Engine 5](https://is.muni.cz/th/n1qq4/real-time_GI_in_UE5.pdf)——SSP→Radiance Cache probe atlas 第三方实现分析；
- 行业动向（仅跟踪，不立项）：UE 5.8 Lumen Probe Occlusion；Irradiance Field Gather（SIGGRAPH 2025，低端设备 final gather 替代）——远期 Dawn/WASM 档可关注。
