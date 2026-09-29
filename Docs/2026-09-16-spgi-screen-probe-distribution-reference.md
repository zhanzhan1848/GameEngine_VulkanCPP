# SPGI Screen Probe 分布参考（屏幕空间密度 / GBuffer 布点 / Ray 预算）

> 日期：2026-09-16
> 状态：📖 长期参考 —— 记录 Screen Probe GI（SPGI）在**屏幕空间**的 probe 分布规律：密度按像素/Tile 而非世界空间米、布点位置由 GBuffer surface 决定、自适应细分、ray 预算与复用链，并逐项对照本引擎 SPGI 现状。**凡讨论 Screen Probe / SPGI 的 probe 数量、密度、布点位置、ray 预算、temporal/spatial 复用、历史匹配方式时，以本文为第一入口。** DDGI（世界空间网格）侧的对应问题见姊妹篇 `Docs/2026-09-16-ddgi-world-space-probe-grid-reference.md`。
> 信息来源：Screen Probe 分布原理整理（2026-09-16）+ UE Lumen Screen Probe 公开方案 + 本引擎代码走查（`Engine/Graphics/Lumen/ScreenProbes/`，dev 分支）
> 关联文档：见 §13 索引

---

## 1. 结论速览

1. **SPGI 的 probe 密度按屏幕像素/Tile 决定，不按世界空间"米"**。正确的问题是"每多少个屏幕像素放一个 probe、一个 probe 覆盖多大屏幕区域"，而不是"间隔 2 m 还是 4 m"——后者是 DDGI 的问题（DDGI 参考文档）。
2. **典型 Tile Size 是 8×8 ~ 16×16 px**，对应 1080p ~ 4K 下 8K ~ 230K 个 probe。数量级远高于 DDGI 的 2K–10K，但**两者不可直接比较**：DDGI probe 是跨帧持久的世界采样点，SPGI probe 更接近"一个屏幕 tile 的临时 radiance 采样器"，生命周期与每 probe 信息量完全不同。
3. **Screen space 决定 probe 数量，World space surface 决定 probe 位置**。Probe 必须从 GBuffer（depth → 反投影世界坐标、normal）生成，**不能简单取 Tile 中心**——混合 tile 会把背景面的 GI 采给前景面，产生漏光/halo/GI 断层。
4. **真正的成本大头是 `probe 数 × rays/probe`**（不是 probe 数本身）。1440p 下 8×8 tile × 64 rays = 3.69M rays/帧，已非常可观；正确路径是**低 sample count → temporal reuse → spatial reuse → filtering → pixel reconstruction**，而不是 probe 密 + ray 多 + 全量每帧更新三件事同时做。
5. **SPGI probe 不是持久 world probe**。历史匹配靠 Motion Vector + Depth + Normal + World Position 的重投影一致性，绝不是 `probe_id = worldGridIndex`。本引擎已按此实现（重投影 + 世界一致性校验 + EMA）。
6. **本引擎当前为 16×16 tile × 64 rays**（`ScreenProbeGIPass.h:25-26`），1440p 下 14,400 probe ≈ 0.92M rays/帧——ray 预算与"8×8 tile × 16 rays"的经典起点完全等价，说明当前配置在合理的预算包络内，密度/射线的再分配（而非双增）才是下一步的调参空间。
7. 与 SC + DDGI 的分工：**SPGI 管 camera-visible 高频（cm → 1~2 m），Surface Cache 管 radiance 数据库（ray hit 查询/多 bounce 源），DDGI 管低频远场（2 m → 几十 m）**。三者互补，SPGI 不需要承担"完整 GI"。

---

## 2. 提问方式：屏幕像素，不是米

DDGI 与 SPGI 的 probe 分布逻辑根本不同：

| | DDGI | SPGI（Screen Probe） |
|---|---|---|
| 密度定义域 | World space（米） | Screen space（像素/Tile） |
| 典型间距 | 2–3 m（RTXGI 推荐） | 8×8 ~ 16×16 px |
| Probe 本质 | 跨帧持久的世界采样点 | 屏幕 tile 的临时 radiance 采样器 |
| 可否在空气里 | 可以（规则网格） | 不应该（应落在 GBuffer surface 上） |
| 历史匹配 | `probe_id = worldGridIndex`（滚动网格） | Motion vector + depth + normal + world position 重投影 |
| 典型数量 | 2K – 16K | 8K – 230K（随分辨率走） |

所以 SPGI 的参数决策只问两件事：

```
每多少个屏幕像素放一个 Probe？
一个 Probe 覆盖多大的屏幕区域？
```

---

## 3. 数量级速查表与 Ray 预算

### 3.1 Tile Size → Probe 数（通用参考值）

`N_probe = ceil(Width / tile) × ceil(Height / tile)`

| Probe spacing | 1080p (1920×1080) | 1440p (2560×1440) | 4K (3840×2160) |
| ------------: | ----------------: | ----------------: | -------------: |
| 4×4 px | ~130K | ~230K | ~518K |
| **8×8 px** | **~32K** | **~58K** | **~130K** |
| **16×16 px** | **~8K** | **~14K** | **~32K** |
| 32×32 px | ~2K | ~3.6K | ~8K |

### 3.2 Ray 预算才是真实成本

以 1440p、8×8 tile（57,600 probe）为例：

| rays/probe | 总 rays/帧 | 评价 |
| ---: | ---: | --- |
| 16 | 0.92M | 合理起点 |
| 32 | 1.84M | 合理上限区 |
| 64 | 3.69M | 已非常可观 |
| 128 | 7.37M | 不可"粗暴全帧更新" |

Screen Probe GI 不能三件事同时做：**probe 很密 + 每 probe 很多 rays + 每帧全部更新**。应该依赖复用链：

```
低 sample count
      ↓
Temporal reuse
      ↓
Spatial reuse
      ↓
Filtering
      ↓
Pixel reconstruction
```

推荐起点（1440p）：`8×8 tile × 16~32 rays ≈ 0.92M ~ 1.84M rays/帧`，temporal 用当前帧少量 rays + 前帧 radiance/history 做累积。

### 3.3 为什么不能和 DDGI 的 2K–10K 直接比较

```
DDGI Probe:                          SPGI Probe:
一个 Probe                           一个 Screen Tile
  ↓                                    ↓
可能发射 64~256 rays                 生成一个 Screen Probe
  ↓                                    ↓
存 SH / irradiance / depth           少量方向采样
  ↓                                    ↓
跨帧持久存在                         Spatial / Temporal reuse
                                       ↓
                                     重建到 pixels
```

DDGI probe 是**资产**（跨帧存续、信息密度高、更新摊销）；SPGI probe 是**每帧重建的中间量**（数量大、单 probe 便宜、靠复用链补质量）。拿"58K vs 2K"说 SPGI 太贵或 DDGI 太稀，都是范畴错误。

---

## 4. Probe 放哪里：GBuffer Surface 决定位置

### 4.1 Screen Probe 至少要携带的信息

DDGI 的规则网格里 probe 可以存在于空气里；Screen Probe 应根据 **GBuffer surface** 生成。一个 probe 通常至少携带：

```cpp
struct ScreenProbe
{
    float3 world_position;   // screen_coord + GBuffer depth → 反投影
    float3 normal;           // GBuffer normal
    float  depth;
    uint2  screen_coord;     // tile 内代表性像素
    // radiance / SH / SG / octahedral ...
};
```

```
screen_coord → GBuffer Depth   → reconstructWorldPosition() → world_position
             → GBuffer Normal  → normal
```

即：**Screen space 决定 probe 数量，World space surface 决定 probe 的实际位置。**

### 4.2 不能简单取 Tile 中心

一个 8×8 tile 可能覆盖两个深度面（`A` = 前景柱子，`B` = 后面墙）：

```
AAAAAAAA
AAAAAAAA
AAAABBBB
AAAABBBB
AAAABBBB
AAAABBBB
AAAABBBB
AAAABBBB
```

若直接 `probePixel = tileCenter`，probe 可能落在 `B`，于是整个 tile 的 8×8 像素拿 B 面的 GI 去 reconstruct A 面，产生：

- 边缘漏光
- halo
- GI 断层
- 法线边界错误
- thin geometry 错误

所以 Screen Probe placement 需要 **depth / normal aware placement**：在 tile 内找一个代表性像素：

$$
p^*=\arg\min_p Cost(p),\qquad
Cost = w_d\,|D_p-D_{median}| + w_n\,(1-N_p\cdot N_{avg})
$$

即综合深度中值偏离与法线一致性，选更有代表性的 surface sample。

### 4.3 本引擎现状

- 布点为**均匀网格、取 tile 中心像素**采 GBuffer，天空像素标记 `w=0` 无效（`ScreenProbePlace`，dispatch 链见融合文档 §2.2）——即 §4.2 的 Cost-based 代表像素选择**未做**，混合 tile 的代表性问题当前靠 gather 侧 depth/normal 双边去过滤。
- Probe 实际携带：position / normal / radiance(64 ray) / SH L0+L1 + confidence，SSBO 三缓冲（`frame % 3`）。

---

## 5. 自适应布点：一个 Tile 不一定只有一个 Probe

提高质量时优先做**几何不连续驱动的细分**，而不是全局加密。检测条件：

$$
|Depth_A - Depth_B| > Threshold \quad\text{或}\quad N_A \cdot N_B < Threshold
$$

按 tile 复杂度分级：

```
Normal tile:        1 probe
Discontinuity tile: 2 probes
Complex tile:       4 probes
```

于是 probe 数从固定式变为动态：

$$
N_{probe} = N_{base} + N_{adaptive}
$$

1440p、8×8 base 的量级：

```
Base probes      320×180 = 57,600
+ edge/geometry adaptive  ~10K
≈ 60~70K
```

这比全屏直接提到 4×4（640×360 = 230,400）**划算一个数量级**——只有边缘 tile 花了加密度预算。

### 5.1 本引擎现状（F4 已实装，形态不同）

本引擎的 adaptive 是 **fine 档插值失败测试**（UE 式）：奇数格中心用 4 个棋盘边邻**粗档** probe 的深度反投影位置做双线性重建，与实际位置比较，误差 > `adaptive_pos_threshold`（默认 0.35 世界单位）→ 激活 fine probe；任一邻居是天空直接判失败（深度不连续处插值不可信）。0 回退 T5.3 depth/normal 方差启发式（`ScreenProbeGIPass.h:49-53`，融合文档 §F4）。与"每 tile 1/2/4 probe 分裂"的分级式自适应**不是同一形态**，但同属"几何不连续驱动、只在需要处花预算"的思路；§10 的二阶段规划是走向分级式的路径。

---

## 6. Ray 预算控制：复用链优先于堆采样

Probe 数 × rays/probe 是 trace + update 的乘积关系（同 DDGI 的教训，见 DDGI 参考文档 §2/§4）。提升质量/稳定性的顺序应该是：

1. **Temporal reuse**：当前 probe 少量新 rays + 前帧 radiance/reservoir 累积；
2. **Spatial reuse**：probe 网格邻域双边滤波共享采样；
3. **方向重要性分配**：把 rays 花在亮度方差大的方向上；
4. 以上都不够，再动 rays/probe；最后才是动 tile size。

### 本引擎现状

- `rays_per_probe = 64`，硬上限 64（`ScreenProbeGIPass.h:26`、`ScreenProbeGIPass.cpp:255`）；Fibonacci 球采样、绕 probe 法线定向、cosine 权重。
- **F2 importance resampling 已实装**（默认开）：按前帧最终 probe SH 对 Fibonacci 方向集做逆 CDF 重分配（lighting-pdf × BRDF-pdf 的轻量形态），SH 投影带 `1/(N·q)` 校正保持无偏。
- Temporal：重投影找历史 cell + 世界一致性校验 + EMA（α=0.1）+ 梯度/clamp 门控（F3 含 4-probe 双线性 + variance clamp）。
- Spatial：probe 网格世界距离双边（σ=2，中心权重 2.0）。
- 即复用链四件套已齐，当前 64 rays 的量级在有 importance + temporal 的前提下是**偏保守（够用但可能过量）**的一档，压 rays 换 tile 密度是可行的再分配方向（见 §10）。

---

## 7. SPGI 不是持久 World Probe（历史匹配方式）

这是与 DDGI 最容易混淆的架构区别：

```
DDGI:                                SPGI:
World Grid                           Screen Grid
P P P P P                            P P P P P
P P P P P                            P P P P P
Camera 移动                          Camera 移动
  ↓                                    ↓
Probe 仍属于 world /                  重新投影到 world surface
scrolling volume                       ↓
                                     Temporal Reprojection
```

SPGI 的历史匹配应主要依靠：

```
Motion Vector + Depth + Normal + World Position
```

判断 $Probe_t \leftrightarrow Probe_{t-1}$ 的一致性，而**不是** `probe_id = worldGridIndex`——屏幕格在镜头移动时内容整体换血，id 相邻的两帧 probe 往往指向完全不同的世界位置。

### 本引擎现状

已符合本原则：`ScreenProbeTemporal` 对历史 cell 做重投影 + 世界一致性校验（`ScreenProbeTemporal.comp:111-138`），不匹配则拒绝历史；probe 数据三缓冲。**注意纪律：后续任何"probe 复用/缓存"类优化都不得把 SPGI probe 改造成按 id 持久化的 world 探针——那是 DDGI 的职责（fallback 走 F1 的 4-probe tetrahedron 读 DDGI）。**

---

## 8. SC + DDGI + SPGI 三方分工

三套 GI 数据结构应该是互补，而不是三套系统同时算同一份 irradiance：

```
                 GI
                  │
        ┌─────────┼──────────┐
        │         │          │
       SPGI       SC        DDGI
        │         │          │
    Screen     Surface     World
     Space      Space       Space
        │         │          │
    高频细节    Radiance    低频远场
        │        Cache       GI
        │         │          │
      8~16px     Cards      2~4m
        │         │          │
      ~14~58K    ~374       ~2K
```

| 系统 | 空间域 | 负责内容 | 典型尺度 |
|---|---|---|---|
| **SPGI** | Screen space | contact GI（墙角、桌脚、柱子）、小尺度遮挡、camera-visible 高频间接光 | cm → 1~2 m |
| **Surface Cache** | Surface space | Surface Radiance、off-screen surface 信息、ray hit lookup、多 bounce radiance 源 | 全引擎的 **surface radiance database** |
| **DDGI** | World space | 大范围 diffuse GI、off-screen GI、稳定低频 illumination、远场 bounce | 2 m → 几十 m |

推论：**SPGI 不需要承担"完整 GI"**——它只做 camera-visible 高频修正；稳定低频 base 由 DDGI 提供；ray hit 的 radiance 由 SC 提供（本引擎即 `ScreenProbeTraceFinalize` 的 SDF 命中 → SC lighting atlas 路径）。

本引擎信号链与多尺度分工表见 DDGI 参考文档 §5（0.1–1 m 归 SPGI/SC，1–3 m near DDGI，3–8 m normal，8–20 m+ far）。

---

## 9. 本引擎现状对照（2026-09-16 核对）

### 9.1 参数与结构

| 项 | 引擎现状 | 代码锚点 |
|---|---|---|
| Tile size | **16×16 px**（`downsample_factor = 16`） | `ScreenProbeGIPass.h:25` |
| Probe 网格 | `ceil(W/16) × ceil(H/16)` → 1440p: 160×90 = **14,400** | `ScreenProbeGIPass.cpp:267-268` |
| rays/probe | **64**（Fibonacci + cosine，硬上限 64） | `ScreenProbeGIPass.h:26`、`cpp:255` |
| Ray 预算 | 1440p: **≈0.92M rays/帧** | 14,400 × 64 |
| 布点 | 均匀网格 tile 中心采 GBuffer；天空 w=0 | `ScreenProbePlace` |
| Probe 表示 | SH L0+L1（4×float4）+ confidence，SSBO 三缓冲 | `ScreenProbeAverage` |
| Trace | 三段混合：HZB 近场(≤25) → GlobalSDF 远场(≤50) → Finalize（SC atlas / DDGI fallback / 天空） | T5.1 |
| DDGI fallback | F1：距离截断 → 4-probe tetrahedron × L0+L1（16 读），`far_cache_start_distance=30` | `ScreenProbeGIPass.h:42` |
| 方向重要性 | F2：前帧 SH 逆 CDF 重采样 Fibonacci 方向集（默认开） | `enable_importance` |
| Temporal | F3：重投影 + 世界一致性 + EMA(0.1) + 4-probe 双线性 + variance clamp | `ScreenProbeTemporal.comp:111-138` |
| Spatial | 世界距离双边（σ=2）ping-pong | `ScreenProbeSpatialFilter` |
| Adaptive | F4：fine 档插值失败测试（阈值 0.35，0 回退方差启发式） | `ScreenProbeGIPass.h:49-53` |
| Gather | 逐像素 4 邻 probe 双线性 + evalSH(N) + 全分辨率 denoise | `ScreenProbeGather`/`Denoise` |

### 9.2 与本文参考值的对照

- **Ray 预算等价性**：16×16 × 64 rays ≡ 8×8 × 16 rays（同为 0.92M @1440p）。即引擎当前处于本文 §3.2 推荐预算包络内，只是"更稀的 probe × 更多的 ray"；方向再分配（如 16×16×32 + F2，或 8×8×16）是在同预算内换空间/角向分辨率的杠杆，不是加预算。
- **Probe 表示**：SH L0+L1 是 UE Lumen radiance map 的降阶（texel 级 → 2 阶球谐），质量天花板低于 UE，但换来了极低的 gather 读取量。
- **布点代表性**（§4.2）与**分级自适应**（§5）是当前两个明确缺口：uniform + tile 中心，混合 tile 的 GI 污染靠 gather/denoise 的双边过滤兜底。

---

## 10. 推荐配置（1440p 起点 + 二阶段）

### 10.1 一阶段起点（2560×1440）

```
SPGI
────────────────────────────
Tile Size             8×8        （或维持 16×16，见 §9.2 等价性）
Base Probe Grid       320×180
Base Probe Count      57,600

Rays / Probe          16~32
Total Rays            ~0.92M ~ 1.84M

Adaptive Probe        二阶段再开
Temporal Reuse        ON
Spatial Reuse         ON

Depth Rejection       ON
Normal Rejection      ON
Motion Reprojection   ON

Half-res Trace        可以考虑
Full-res Resolve      ON
```

对照引擎现状：temporal/spatial/rejection/reprojection 四项**已全部具备**；差异集中在 tile size（16 vs 8）与 rays（64 vs 16~32），且两者在 ray 预算上等价——先做效果验收（banding/收敛，融合文档 T1.5）再决定是否动分配。

### 10.2 二阶段：Adaptive Screen Probe Placement

```
8×8 Base Grid
        │
        ↓
Depth / Normal classification
        │
    ┌───┴────┐
    │        │
 smooth    edge
    │        │
 1 Probe   2~4 Probe
```

即 §5 的分级式：base 57.6K + adaptive ~10K ≈ 60~70K，把预算只花在几何不连续 tile 上。这比直接把全屏提到 4×4（230K）**更值得**，且与本引擎 F4 的 fine 档机制可平滑衔接（F4 的插值失败测试可升级为 per-tile 分裂判据）。

---

## 11. 调参决策树

遇到 SPGI 问题时先分诊归属，再动手：

1. **画面出现 tile 状色块/边缘漏光/halo** → 布点代表性问题（§4.2）：tile 中心采到了错误深度面。方向：代表像素 Cost 选择 / gather 侧双边收紧 / 二阶段 adaptive，而不是全局调 tile size。
2. **GI 方向感错误/高频丢失** → 角向分辨率问题：查 rays/probe 与 F2 importance 是否生效、SH 阶数限制（L0+L1）。
3. **闪烁/拖影** → 历史匹配问题（§7）：查重投影一致性校验、variance clamp（F3），确认没有把历史按 id 直通。
4. **性能不足** → 先看 `probe × rays` 乘积里砍哪个：优先压 rays（有 temporal+importance 兜底）、其次 half-res trace、最后才是 tile size。**不要 probe 和 rays 同时加。**
5. **远处/框外 GI 缺失** → 不是 SPGI 的职责：DDGI fallback（F1）或 far volume 覆盖问题，走 DDGI 参考文档 §6 决策树。
6. **能量类问题（过亮/过暗/反转）** → 先查 SC 注入与能量标定链（`Docs/completed/2026-09-04-spgi-energy-calibration-and-ddgi-overexposure.md`），不要动分布参数。

所有档位改动遵守 08-24 总计划 §6 执行红线（读数 ≤32/线程、FPS ≥30 带测量、GLSL 单源语义、任务独立可开关）。

---

## 12. 常用速查

- **口诀**：Screen space 定数量，world surface 定位置；成本看 `probe × rays`；质量靠复用链。
- 典型 tile：**8×8**（质量档）/ **16×16**（平衡档，本引擎当前）/ 32×32（性能档）。
- 1440p 速算：16×16→14.4K probe；8×8→57.6K；4×4→230K。
- Ray 预算红线感受值：≥4M rays/帧（8×8 × 64 @1440p）就应当复核 temporal/spatial 复用是否吃满。
- 历史匹配：**重投影一致性（motion+depth+normal+world pos）**，永远不是 probe id。
- 引擎当前：16×16 × 64 rays ≈ 0.92M rays/帧 @1440p；F1（DDGI fallback）/F2（importance）/F3（temporal clamp）/F4（adaptive fine）全部实装，效果级验收待补（融合文档）。

---

## 13. 相关文档索引

| 文档 | 关系 |
|---|---|
| `Docs/2026-09-16-ddgi-world-space-probe-grid-reference.md` | **姊妹篇**：DDGI（世界空间网格）的密度/间距/多尺度参考；两文合起来覆盖"probe 分布"的两侧 |
| `Docs/2026-08-26-spgi-lumen-screen-probe-fusion.md` | SPGI 与 UE Lumen 的逐维度对照、F1–F5 融合实装与 dispatch 链细节（本文 §9 的代码细节均出自其 §2） |
| `Docs/2026-08-24-gi-optimization-plan.md` | GI 总体计划（五轴框架/三轨道/执行红线）；本文所有调参动作受其 §6 约束 |
| `Docs/2026-08-09-vulkan-lumen-gi-port-design.md` | Lumen 全家桶 Vulkan 移植设计 |
| `Docs/completed/2026-09-04-spgi-energy-calibration-and-ddgi-overexposure.md` | SPGI 能量标定结案 + DDGI 过曝根因；能量类问题第一入口 |
| `Docs/2026-09-04-spgi-metal-crash-investigation.md` | SPGI Metal 平台崩溃排查 |
| `Docs/2026-09-07-lumen-source-analysis-notes.md` | UE5 Lumen 源码解析笔记（特性面对照基准） |
| `Docs/2026-09-15-sponza-reference-look-roadmap.md` / `Docs/diagnostics/2026-09-14-sponza-reference-match/` | Sponza 对照 Lumen 参考的现状与差距（含 SPGI 亮斑分支） |

## 14. 参考文献

1. Wright et al., *Lumen: Real-Time Global Illumination Techniques*（SIGGRAPH 2022 Advances）—— screen probe 布点/方向 jitter + temporal filter、adaptive placement、bilateral history reuse。
2. Epic 官方技术博客 [Unreal Engine 5 goes all-in on dynamic global illumination with Lumen](https://www.unrealengine.com/tech-blog/unreal-engine-5-goes-all-in-on-dynamic-global-illumination-with-lumen) —— final gather / screen-space radiance cache。
3. Wright, *Radiance Caching for Real-Time Global Illumination*（SIGGRAPH 2021 Advances）—— importance sampling（incoming radiance × BRDF）。
4. 知乎 [Lumen 系列之 Importance Sampling](https://zhuanlan.zhihu.com/p/713507304) —— `GenerateBRDF_PDF` / LightingPDF 历史混合源码级拆解（F2 机制依据）。
5. Schied et al., [Spatiotemporal Variance-Guided Filtering（SVGF, NVIDIA 2017）](https://research.nvidia.com/publication/2017-07_spatiotemporal-variance-guided-filtering-real-time-reconstruction-path-traced) —— neighborhood/variance clamp 谱系（F3 依据）。
6. Masaryk 大学论文 [Real-Time Global Illumination in Unreal Engine 5](https://is.muni.cz/th/n1qq4/real-time_GI_in_UE5.pdf) —— SSP → Radiance Cache probe atlas 第三方实现分析。
7. 本引擎代码：`Engine/Graphics/Lumen/ScreenProbes/ScreenProbeGIPass.{h,cpp}`、`Engine/Graphics/Vulkan/Shaders/Lumen/ScreenProbe*.comp`（2026-09-16 走查）。
