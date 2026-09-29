# SPGI Probe 分布优化计划（代表像素 / F4 normal 门 / 1-2-4 三档 / 辐亮度判据 / 分档 ray）

> 日期：2026-09-21
> 状态：✅ 已实施完成（2026-09-21 当日落地 P0-P4 全阶段；完工记录
> `Docs/completed/2026-09-21-spgi-probe-distribution-completion.md`，
> 对照图集 `Docs/completed/render-comparisons/2026-09-21-spgi-probe-distribution/`。
> 实现与本文的偏差以完工记录 §4 为准：P2 槽位不对称消解、P4 tier 门控替代
> 分批 dispatch、P3 总活跃封顶替代每帧 Top-K、tier3 仅 Vulkan pilot）
> 范围：SPGI（Screen Probe GI）probe 的**布点代表性、自适应判据、per-tile 档位、ray 预算分配**；不动 SC / DDGI / WRC 职责边界与能量流向
> 信息来源：外部架构评审（Lumen 对照，2026-09-20/21）逐条对码核实 + 代码走查（`Engine/Graphics/Lumen/ScreenProbes/`、`Engine/Graphics/Vulkan/Shaders/Lumen/ScreenProbe*.comp`，dev 分支）
> 前置文档：`Docs/2026-09-16-spgi-screen-probe-distribution-reference.md`（SPGI 分布第一入口；本文是其 §4.2 布点缺口、§5 自适应、§10.2 二阶段路线的展开）
> 执行约束：`Docs/2026-08-24-gi-optimization-plan.md` §6 执行红线（读数 ≤32/线程、FPS ≥30 带测量、GLSL 单源语义、任务独立可开关）

---

## 1. 结论速览

1. SPGI 的定位不变：**camera-local final gather / reconstruction 层**，补当前视角高频间接光；稳定低频由 DDGI/WRC 提供，radiance 数据库由 SC 提供。本计划只在"probe 放哪、放几个、花多少 ray"上演进，不改变四方分工。
2. **最高优先级是布点代表性（P0）**：tile 中心采点在混合 tile 会落错深度面，目前靠 gather/denoise 双边过滤事后兜底——这是"后处理修补 placement error"。改为 5 候选中位深度 + 法线一致性评分，读预算 13→≈22，红线 32 内。
3. **F4 判据补 normal 项（P1）是小改动不是新建**：legacy T5.3 路径已有 normal 门（`minNDot < 0.8`），缺口只在 F4 默认路径；加一项 `minNDot < T_n` 即可，wall-floor 墙脚类漏激活直接受益。
4. **三档 1/2/4 per-tile（P2）用固定 4 槽位布局**而非动态分裂，槽位索引静态可推导；**必须伴随活跃槽 compaction**（否则 radiance 数组 ×4 ≈ 177MB 不可接受）；80% 工作量在 Gather 侧，需单独设计评审。
5. **辐亮度方差进判据（P3）必须带双稳态护栏**：SC↔probe 之间发生过 3× 运行间方差的双稳态（content-ready 闩锁修复），任何 SC→placement 反馈路径都要一帧滞后 + 激活 Top-K 封顶。
6. **分档 ray 数（P4）放最后**：`raysPerProbe` 是 dispatch 级 uniform，逐 probe 变步长破坏平坦索引；可行路径是按档位分批 dispatch，且依赖 P2 的 compaction 结构。
7. **真实 ray 预算口径修正**：trace 对非活跃 probe 提前返回，实际成本 ≈ 活跃 probe × 64 ≈ **0.46–0.7M rays/帧**（1440p），比参考文档 §9 按全网格算的 0.92M 更低；当前配置不存在预算危机，本计划是**质量/再分配**计划，不是省钱计划。
8. 不动的部分：tile size 维持 16×16（与 8×8×16 预算等价）；jitter 0.2 / 历史门 0.5（比值 0.4，落在健康区间）；SPGI probe 绝不按 id 持久化；不引入任何 SC↔DDGI↔SPGI 写回路径。

---

## 2. 现状基线（2026-09-21 对码核实）

### 2.1 分布机制全景

| 环节 | 现状 | 锚点 |
|---|---|---|
| Tile size | 16×16 px（`downsample_factor=16`，运行时 `screen_probes_spacing` 可配） | `ScreenProbeGIPass.h:25`、`StandardRenderPipeline.cpp:1465` |
| Probe 网格 | `ceil(W/16) × ceil(H/16)` → 1440p: 160×90 = 14,400 候选 | `ScreenProbeGIPass.cpp:268-269` |
| 布点 | tile 中心像素 + F5 逐帧抖动（±1/4 cell 屏幕、世界空间钳 `0.2`、超限缩回重采） | `ScreenProbePlace.comp:97-98, 100-144` |
| 无效化 | 天空/远平面（depth ≤0.0001 或 ≥0.999）→ `w=0` | `ScreenProbePlace.comp:146-147, 221-226` |
| 自适应 | 棋盘两档：偶格粗档常开；奇格细档由 F4 插值失败测试激活（`adaptive_pos_threshold=0.35`，0 回退 T5.3 深度/法线方差启发式） | `ScreenProbePlace.comp:149-219`、`ScreenProbeGIPass.cpp:1721-1726` |
| Ray | 64/probe（Fibonacci + cosine，硬上限 64；`screen_probes_rays` 可配） | `ScreenProbeGIPass.h:26`、`StandardRenderPipeline.cpp:1491-1495` |
| Trace | 非活跃 probe 提前返回；F2 importance / F5 方向抖动二选一 | `ScreenProbeTraceRays.comp:173-202` |
| 复用链 | F2 importance（默认开）、F3 temporal（重投影+世界一致性+EMA）、spatial 世界距离双边、全分辨率 denoise | 参考文档 §9.1 |
| Probe 表示 | position/normal + SH L0+L1 + confidence，SSBO 三缓冲 | `ScreenProbeAverage` |

### 2.2 两个口径修正（对参考文档与外部评审的校准）

1. **normal 项范围修正**：外部评审认为"adaptive 判据只有 position、缺 normal term"。对码结果：**legacy 路径已有 normal 门**——`adaptive_pos_threshold=0` 时走 T5.3 启发式，`minNDot < 0.8` 即激活（`ScreenProbePlace.comp:196-216`）。真正的缺口只在 F4 默认路径（插值失败测试只有 position 误差）。因此 P1 是"把 normal 项补进 F4 路径"，不是从零建判据。
2. **ray 预算口径修正**：`TraceRays` 按 `gridW×gridH×rays` 铺 dispatch，但对 `posData.w ≤ 0` 的 probe 在读位置后立即返回（`ScreenProbeTraceRays.comp:173-180`）。棋盘粗档常开 ≈ 7,200 + fine 档若干 → **真实 ray 成本 ≈ 0.46–0.7M/帧 @1440p**（参考文档 §9 的 0.92M 按全网格计算，偏高）。副作用：dispatch 宽度仍按全网格铺、死线程 early-return——compaction 是顺带的小机会，在 P2 一并收。

### 2.3 外部评审意见处置表

| 外部意见 | 处置 | 说明 |
|---|---|---|
| 保留 16×16 base spacing | ✅ 采纳为约束 | 与 8×8×16 预算等价（参考文档 §9.2） |
| 保留 checkerboard + world-position interpolation failure | ✅ 采纳为约束 | F4 语义有物理意义，优于纯屏幕空间梯度 |
| tile center → representative pixel | ✅ 采纳，列为 **P0 最高优先级** | 参考文档 §4.2 点名的第一缺口，两源一致 |
| adaptive test 加 normal 项 | ✅ 采纳，范围修正为"F4 路径补项"（§2.2） | legacy 已有 `minNDot<0.8` |
| 1/2/4 probes-per-tile 三档 | ✅ 采纳，落地形态改为**固定 4 槽位** | 参考文档 §10.2 二阶段；固定槽位避免动态索引 |
| SC 辐亮度方差进细分判据 | ✅ 采纳，**加双稳态护栏 + Top-K 封顶** | 外部评审不知道 [[sc-probe-bistability]] 前科 |
| adaptive ray count | ✅ 采纳，放最后 | 受 dispatch uniform 约束，走分批 dispatch |
| jitter 0.2/0.5 组合 | ✅ 维持不动 | 比值 0.4 落在建议的 0.25–0.4T 区间，且世界钳制已有 |
| SPGI 不要过重依赖 DDGI、防 cache 反馈环 | ✅ 与既有纪律一致 | WRC 能量单向化（SC 只出不进）已落地 |
| tile size 降到 8×8 | ❌ 现在不做 | 预算等价，密度再分配等 P0–P2 落地后再议 |

---

## 3. 红线与总原则（全程有效）

1. **能量单向流**：SC 只出不进；WRC bootstrap 馈送 + tile radiosity 单轨（2026-09-19 完工确立）。本计划所有新耦合**只增读不增写**。
2. **SPGI probe 不是持久 world probe**：历史匹配只靠重投影一致性（motion+depth+normal+world pos），任何"复用/缓存"优化不得把 probe 改成按 id 持久化——那是 DDGI/WRC 的职责，远场缺失走 F1 fallback。
3. **读预算 ≤32 reads/thread**：Place shader 每步改动后重算最坏路径读数（§4 各项附账）。
4. **任务独立可开关**：每项有独立 env 回滚位，关掉后数值路径与现状逐位一致。
5. **Metal 孪生同步**：所有 shader 改动 Vulkan/Metal 双树同步，GLSL 单源语义。
6. **env 登记义务**：新增旋钮实施时必须登记 `Docs/environment-variables.md`（活文档）。

---

## 4. 分阶段详案

### P0 — 代表像素选择（tile center → 5 候选评分）★最高优先级

**动机**。混合 tile（前景柱 + 后墙）若中心像素采错深度面，整个 tile 的 GI 用错 surface，靠 gather/denoise 双边过滤兜底属于"后处理修补 placement error"；Sponza 柱列、拱门边缘的 GI 断层/halo 与此直接相关。参考文档 §4.2 与外部评审在此完全一致。

**改动点**。`ScreenProbePlace.comp` 布点段（现 `:96-144`），纯 shader 改动，CPU 侧零改动。

**设计**。

```
候选 = { tile 中心, ±half-cell X, ±half-cell Y }   // 5 个，half-cell = downsample/2 = 8px
sky 候选剔除出统计；全部 sky → tile 无效（现状语义不变）
z_med   = 5 候选深度的中位数（5 元素中位，小排序网络 ~7 次比较，无额外 fetch）
N_med   = 中位深度候选的 normal
p*      = argmin_p [ w_d·|z_p − z_med| + w_n·(1 − dot(N_p, N_med)) ]
权重    = w_d 1.0、w_n 0.5 起步，A/B 定档
```

- depth 一律 POINT 采样（现有注释：线性过滤在物体边缘产生幽灵深度、生成悬空 probe）；
- **jitter 交互**：F5 抖动绕选出的代表像素 p* 偏移，世界空间钳制 + 超限缩回重采逻辑原样保留（`:100-144` 只是 `pixelCenter` → `p*`）；
- 不做边缘检测/9 候选等更复杂的 cost-based 采样——5 候选先验证收益。

**读预算账**。现 F4 路径最坏 13（含 jitter 钳制重采，`Place.comp:157-158` 口径）；+4 depth（候选）+ ~5 normal（候选）≈ **22**；legacy 回退路径同量级。红线 32 内。

**验收**。Sponza A/B：`PRIMAL_PASS_DEBUG` 的 spgi_raw / gather 视图截图对照，重点拱门边缘、柱轮廓 GI 断层与 halo；arch ROI 三口径读数（历史基线 18.3，参考 13.4）。**附加检查**：temporal 历史命中率读回不回退（probe 位置整体移动会轻微改变重投影命中分布）。

**回滚**。`PRIMAL_SPGI_PLACE_REPR=0` → 精确回退 tile center 路径。

**风险与连锁**。probe 位置整体漂移后，F4 阈值 `0.35` 的语义随之变化——P0 落地后必须做 `adaptive_pos_threshold` 0.25–0.5 的重标定 A/B（沿用 face-gate 0.5→0.25 的定档流程），并观察 F4 fine 档激活率变化。

**工作量**：1–2 人日。

### P1 — F4 判据补 normal 项

**动机**。wall-floor 墙脚、柱-地交线处世界位置插值误差不大，但法线场已 ~90° 跳变——F4 纯 position 判据在这类 tile 漏激活 fine probe；Sponza 柱脚/拱脚/台阶是直接受益区。

**改动点**。`ScreenProbePlace.comp:160-194`（F4 路径）+ UBO 打包（`ScreenProbeGIPass.cpp:1721-1726` 一带）。

**设计**。

```glsl
// F4 激活条件，从单项改双项：
interpFailed = (posErr > posErrThresh * scale) || (minNeighborNDot < normalThresh);
// minNeighborNDot：4 个边邻粗档采样点的 normal 与中心 normal 的最小 dot
// normalThresh (T_n)：0.85 起步（legacy 用 0.8，外部建议 0.8–0.9，A/B 定档）
```

- T_n 的 UBO 通道：优先复用 `cache_params` 空闲槽；若被占则照 face-gate 标定的 `hint_params` 尾扩 448B 惯例尾扩，CPU 打包处同步，diag 面板（`GIEditorPanel`）联动可调（照朝向门 `diag_params.y` 模式）。

**读预算账**。P0 后 ≈22，+4 normal fetch（4 邻）≈ **26**，红线内。

**验收**。关 normal 项作对照，统计"几何平但亮度跳变"tile 的 fine 档激活率变化；Sponza 柱脚/拱脚 ROI；朝向门标定的 A/B 矩阵格式复用。

**回滚**。`PRIMAL_SPGI_ADAPTIVE_NORMAL=0`。

**工作量**：0.5–1 人日；可与 P0 同 PR 交付但开关独立；T_n 定档并入 P0 引发的阈值重标定轮。

### P2 — 两档升级为 1/2/4 per-tile 三档（固定 4 槽位 + 活跃槽 compaction）

**动机**。probe 预算应随几何复杂度不均匀分配：Sponza 大部分墙面/地面平坦（1 probe 够），柱/拱/窗框复杂（4 probe）。目标是用 ~60–70K 活跃 probe 达到全局 8×8（230K）的效果——比全局加密划算一个数量级（参考文档 §5 论证）。

**布局设计：固定 4 槽位，不做动态分裂**。

```
每个 16×16 tile 固定 4 槽：
  slot0 = coarse 槽（tile 中心，P0 代表像素）
  slot1..3 = quadrant 细分槽（tile 四象限中心）
probeIdx = tileIdx * 4 + slot   // 静态可推导，无间接分裂表
激活级数由 P0/P1 判据分级：
  level1（平坦）= 仅 slot0
  level2（边缘）= slot0 + 失败侧象限槽
  level4（复杂）= 全 4 槽
上限 = 14.4K tile × 4 = 57.6K 槽（1440p），有界
```

**存储与 compaction（关键约束）**。

- position/normal/importanceSH 数组 ×4 直扩可忽略（≈5.5MB 级）；
- **radiance 逐 ray 数组不能 ×4 直扩**：现 14.4K×64×16B×三缓冲 ≈ 44MB → ×4 ≈ 177MB，不可接受；
- 因此 P2 必须带**活跃槽 compaction**：间接表 `slotId → compactedProbeId`，trace/update/gather 全部按 compacted 索引工作，dispatch 只铺活跃数。附带收掉 §2.2 的死线程 dispatch 浪费。

**Gather 侧重设计（本项 80% 工作量，需单独设计评审）**。

- 现状：逐像素对 4 邻格 probe 双线性 + evalSH；
- 目标：逐像素在"本 tile 槽位 + 8 邻 tile 槽位"集合内按 depth/normal/世界距离做最佳匹配（1~2 probe），双线性退化为一维混合或最近邻 + 既有 denoise 兜底；
- `ScreenProbeGather.comp` / `ScreenProbeDenoise.comp` 均受影响；设计稿单独出评审后再动手。

**调度**。激活数统计走 GPU atomic + 下一帧间接 dispatch（照 SC Top-K 调度 / E2 事件驱动增量的既有惯例），不做同帧 CPU 读回回路。

**验收**。①激活分布统计（预期 Sponza 大部分 tile level1、柱/拱 level4）；②同 ray 预算 vs 全局 8×8 的画质对照；③FPS ≥30 带测量（激活上限 57.6K 槽的最坏情况也要跑）。

**回滚**。`PRIMAL_SPGI_TIER3=0` → 回退棋盘两档语义（槽位布局保留，只激活 slot0 + 奇格语义映射回旧路径）。

**工作量**：3–5 人日。

### P3 — SC 辐亮度方差进入细分判据（几何判据之后）

**动机**。平坦但跨 sunlit|shadow 或 bright bounce|dark corner 的 tile，几何判据不会激活、radiance 重建却最需要 probe。GI 的目标是逼近 L_i(x,ω) 而非几何本身。

**判据升级**：`C = w_p·E_position + w_n·E_normal + w_l·E_lighting`，其中 E_lighting 取邻域 probe **前帧最终 SH 的亮度方差**（或 max 亮度差，实现取简）。

**两条仓库特有护栏（外部评审覆盖不到的）**：

1. **双稳态护栏**：SC↔probe 曾发生 3× 运行间方差的双稳态（content-ready 闩锁修复，见 `Docs/completed/` 09-19/09-20 记录）。radiance 驱动布点会开新的 SC→placement 反馈路径，必须：只用**上一帧** SH（一帧滞后，天然防同帧闭环）+ 激活后判据输入指数衰减；
2. **Top-K 封顶**：日光照进拱廊这类突发明暗边界可能大面积触发，每帧激活增量 ≤ K（起步 512 槽），按方差排序取 Top-K（沿用 SC Top-K 调度模式），突发不失控。

**实现形态**。Place 之前的小 pass：读 prev final SH + 几何判据结果 → 产出激活掩码；或并入 Place 以 SSBO 传 prev SH。**去激活加迟滞**（连续 N 帧低方差才关），防激活/去激活抖动闪烁。

**验收**。构造明暗边界场景 A/B（Sponza 日光拱廊）；闪烁评估（连续 300 帧激活掩码变化率）；双稳态回归（3× 运行间方差复测口径）。

**工作量**：2–3 人日。**依赖**：P2 槽位结构。

### P4 — 按档分批 ray 数（16/32/64）

**硬约束**。`raysPerProbe` 是整次 dispatch 的 uniform（`grid_params.w`，`ScreenProbeTraceRays.comp:163`），`global_id / raysPerProbe` 的平坦索引不容逐 probe 变步长。

**路径**。P2 compaction 落地后，按档位**分批 dispatch**：coarse 批 16 rays、mid 批 32、fine 批 64，每批独立 dispatch + 各自 uniform，radiance 数组按批偏移。F2 importance（前帧 SH 方向重采样）与 temporal 累积为低 ray 档兜底。

**收益估算（示例）**。Sponza 假设 level1 70% / level2 20% / level4 10%：平均 rays/probe = 0.7×16 + 0.2×32 + 0.1×64 = **24**，较全 64 约 **2.7×** 削减；省出的预算可反投 P2 的 level4 密度。

**验收**。ray 总量读回；coarse 档 SH 收敛质量对照（F2 生效性）；低 ray 档 + temporal 的 banding 复测（T1.5 口径）。

**工作量**：2–3 人日。**依赖**：P2。

---

## 5. 明确不做清单

| 项 | 理由 |
|---|---|
| tile size 降到 8×8 | 16×16×64 ≡ 8×8×16 预算等价；密度再分配等 P0–P2 落地后凭数据再议 |
| SPGI probe 按 id 持久化 | 红线（§3.2）；远场缺失一律 F1 DDGI fallback |
| SC↔DDGI↔SPGI 任何写回 | WRC 能量单向化刚落地；新耦合只增读不增写 |
| 完整 cost-based 布点（9+ 候选/边缘检测评分） | P0 的 5 候选先验证收益；复杂评分读预算与维护成本先记账再说 |
| 动态 probe 分裂（非固定槽位） | 索引/间接层复杂度不划算，固定 4 槽已覆盖 1/2/4 三档 |

---

## 6. 执行顺序与前置项

```
Phase 0  T1.5 效果级验收基线（融合文档遗留：banding/收敛多帧序列 + ROI 对照集）
         ← 先补基线；没有它，后续每步"是否变好"没有锚
   → P0  代表像素选择        （1–2 人日，shader only）
   → P1  F4 normal 项        （0.5–1 人日，与 P0 同 PR 可、开关独立）
   → P2  三档 4 槽位 + compaction（3–5 人日，Gather 设计稿先单独评审）
   → P3  辐亮度方差 Top-K    （2–3 人日，双稳态护栏先行）
   → P4  分档 ray 数         （2–3 人日，依赖 P2）
```

每步独立 PR、独立 env 开关、Metal 孪生同步；P0 与 P1 之间执行一次 `adaptive_pos_threshold`（0.25–0.5）与 `T_n`（0.8–0.9）的联合定档轮。

## 7. 验收标准汇总（每阶段通用 + 专项）

**通用门禁**：`ctest --test-dir Darwin/Debug -R "Unit"` 全绿；`TestVulkanSponzaRenderGraph` 跑 90–120s 无 fault、validation 0；FPS ≥30 带测量；回滚 env 开→关→开数值逐位一致；Metal 树编译 + 80–90s 无 fault。

**Sponza A/B 口径**（沿用 09-16 系列完成记录的三口径）：arch ROI（基线 18.3 / 参考 13.4）、darkarch、ground；`PRIMAL_PASS_DEBUG` spgi_raw / gather 截图；必要时 `SPONZA_SPGI_RAY_DUMP` 逐射线核对。

**专项**：P0 temporal 历史命中率不回退；P2 激活分布统计 + 最坏激活率帧测；P3 激活掩码 300 帧变化率 + 运行间方差复测；P4 ray 总量读回 + coarse 档收敛对照。

## 8. env 旋钮登记（实施时新建，登记 `Docs/environment-variables.md`）

| 旋钮 | 阶段 | 默认 | 语义 |
|---|---|---|---|
| `PRIMAL_SPGI_PLACE_REPR` | P0 | 1 | 0 = 回退 tile center 布点 |
| `PRIMAL_SPGI_ADAPTIVE_NORMAL` | P1 | 1 | 0 = F4 只看 position（现行为） |
| `PRIMAL_SPGI_TIER3` | P2 | 0→1 灰度 | 0 = 棋盘两档（现行为） |
| `PRIMAL_SPGI_LIGHTVAR_TOPK` | P3 | 512 | 辐亮度判据每帧激活增量上限；0 = 关闭辐亮度判据 |
| `PRIMAL_SPGI_TIER_RAYS` | P4 | 0 | 0 = 全档 64（现行为）；1 = 16/32/64 分档 |

## 9. 相关文档索引

| 文档 | 关系 |
|---|---|
| `Docs/2026-09-16-spgi-screen-probe-distribution-reference.md` | **前置第一入口**：分布原理与本引擎现状对照（本文 §2 的出处） |
| `Docs/2026-08-26-spgi-lumen-screen-probe-fusion.md` | F1–F5 机制细节与 dispatch 链；T1.5 效果验收待补项 |
| `Docs/2026-08-24-gi-optimization-plan.md` | §6 执行红线；本计划全程受其约束 |
| `Docs/completed/2026-09-19-world-space-radiance-cache-completion.md` | 能量单向流纪律的落地记录 |
| `Docs/completed/2026-09-20-gi-energy-asymmetry-fix-completion.md` | SC↔probe 竞态/双稳态修复记录（P3 护栏依据） |
| `Docs/completed/render-comparisons/2026-09-16-spgi-face-gate-calibration/` | 运行时阈值 A/B 定档流程范本（P0/P1 重标定复用） |
| `Docs/2026-09-16-ddgi-world-space-probe-grid-reference.md` | 姊妹篇：DDGI 侧分布问题第一入口 |
