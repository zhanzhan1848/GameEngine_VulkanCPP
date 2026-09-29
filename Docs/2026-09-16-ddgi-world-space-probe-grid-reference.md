# DDGI World-Space Probe Grid 分布参考（密度 / 间距 / 多尺度）

> 日期：2026-09-16
> 状态：📖 长期参考 —— 记录 RTXGI/DDGI 官方经验与实时引擎实践中 world-space probe 网格的分布规律，并对照本引擎 DDGI 现状。**凡讨论 GI/probe 数量、间距、覆盖范围、漏光、多尺度分级时，以本文为锚点。**
> 信息来源：NVIDIA RTXGI-DDGI 官方仓库（`docs/DDGIVolume.md`、`src/ddgi/DDGIVolume.cpp`、`include/rtxgi/ddgi/DDGIVolume.h`）+ 实时引擎实践整理（见 §9 参考文献）
> 关联文档：见 §8 索引

---

## 1. 结论速览

1. **Probe 数量没有"标准值"**。真正重要的是四件事：**Probe 间距、覆盖范围、场景尺度、每帧更新预算**。RTXGI 官方经验是典型场景推荐**每 2–3 米一个 Probe**，且明确指出 Probe 并不是越密越好——过密反而可能暴露规则网格结构。
2. **覆盖范围按 `(N-1) × S` 计算**（不是 `N × S`）。RTXGI 计算 Volume AABB 就是按 `probeSpacing * (probeCounts - 1)`。
3. **密度主要决定 GI 的空间频率上限**，加 density 治不了"墙太薄、间距太大"的 light leaking；墙厚需要与 probe density 匹配。
4. **本引擎已经走了比"单级加密"更对的路**：T4.4 near/far 双 volume（粗+细两级）+ T5.5 多 volume 运行时（cap 4）+ 分档预设 + 平台预算钳制。当前 known-working Sponza 配置为 near **32×16×32 @ 3.0**（128 rays/probe）+ far **16×8×16 @ 8.0**。
5. 下一步的增益方向不是 `32×16×32 → 64×32×64`（probe×rays 的 trace/update 成本爆炸），而是 **scrolling 滚动网格激活、probe classification/relocation、importance 更新调度**（RTXGI 同款三件套；本引擎已有骨架但部分处于锁死/未接线状态，见 §4.3）。

---

## 2. Probe 数量没有标准值——关键变量

DDGI 的 World-Space Probe Grid 没有固定"标准数量"。做参数决策时只看四个变量：

| 变量 | 决定什么 |
|---|---|
| Probe 间距 S | GI 场的空间频率上限（能分辨多细的明暗变化） |
| 覆盖范围 `(N-1)×S` | 世界里多大范围内的像素能拿到 world GI（框外落到 far volume 或 fallback） |
| 场景尺度 | 室内小场景 vs 开放世界需要的密度完全不同 |
| 每帧更新预算 | `probe_count × rays_per_probe` 的 trace + update 成本，才是真正的开销大头 |

NVIDIA RTXGI 官方给的经验值：**典型场景 2–3 m 一个 Probe**（假设 1 world unit = 1 m）。同时官方特别强调 probe 不是越密越好：过密的规则网格反而会在插值权重中暴露出网格结构（规律试图条纹/banding）。

### 2.1 覆盖范围的正确算法

覆盖范围严格按：

```
Extent_x = (N_x - 1) × S_x        （probe 中心跨距）
```

RTXGI 本身计算 Volume AABB 也是按 `probeSpacing * (probeCounts - 1)` 确定范围。本引擎实现一致，并在 AABB 外再 pad 一个 cell 防止边缘 probe 射线落空：`LumenDDGIPass.cpp` `GetVolumeBounds()`（origin ± spacing 外扩，即 `(count-1)*spacing + 2*spacing` 的包围盒）。

### 2.2 典型配置对照表（通用参考值，非本引擎）

| 场景 / 质量 | Probe Grid | Probe 数 | 间距 | 覆盖范围约（按 (N-1)×S） |
|---|---:|---:|---:|---|
| 小型室内 | 8×4×8 | 256 | 1–2 m | 14×6×14 m |
| 普通室内/半开放 | 16×8×16 | 2,048 | 2–3 m | 30–45 × 14–21 × 30–45 m |
| 中型开放场景 | 24×12×24 | 6,912 | 3–4 m | 69–92 × 33–44 × 69–92 m |
| 大范围 Camera-centered | 32×16×32 | 16,384 | 3–5 m | 93–155 × 45–75 × 93–155 m |

---

## 3. 为什么密度不是万能药：空间频率与漏光

DDGI 本质是在做**空间低频辐照度场重建**：

```
E(x, n) ≈ Σᵢ  wᵢ(x, n) · Eᵢ(n)
```

Probe density 控制的是这套重建的**空间频率上限**。如果场景里"墙厚 = 0.2 m"而 "probe spacing = 4 m"，DDGI 就无法描述墙两侧完全不同的 radiance field：

```
Room A       wall        Room B

 P-----------|-----------P
      4m（一个 probe 的影响区跨过墙体）
```

此时增加 rays/probe 治不了根——ray 数提高的是每个 probe 采样的角向质量（降噪），不是空间分辨力。RTXGI 官方同样强调**墙体厚度需要与 probe density 匹配**，否则产生 light leaking；其应对手段是 relocation（把 probe 挪出墙/贴到表面）+ classification（标记 non-static/invalid probe），见 §6.3。

**判断准则**：遇到 GI 问题时先分诊——

- 角落/柱子/小房间的高频明暗错误 → 不是加 probe 数，是这部分频率不该由 DDGI 管（交给 SPGI/Surface Cache，见 §6）；
- 穿墙漏光 → spacing vs 墙厚不匹配，走 relocation/classification 或加大墙厚几何/降 spacing；
- 远处/框外死黑 → 覆盖不足，加 far volume 或扩 spacing，而不是全局加密。

---

## 4. 本引擎现状对照（2026-09-16 核对）

### 4.1 参数源链

```
PipelineQualityConfig::LumenConfigFromPreset()     RenderPipelineConfig.cpp（分档默认）
        ↓
lumen::LumenConfig（ddgi_probe_count_* / ddgi_probe_spacing / ddgi_far_*）
        ↓  LumenTypes.h:88-114（不选档时的基线默认也在这里）
StandardRenderPipeline::InitializeLumenPasses()     T5.5 多 volume 运行时（cap 4）
        ↓  StandardRenderPipeline.cpp:783-830（near+far 推导 / volume_overrides 覆盖）
lumen::DDGIRuntimeParams                            LumenDDGIPass.h:39-49（每 volume 运行参数）
```

注意：`LumenDDGIPass.h` 里 `DDGIRuntimeParams` 的默认值（16×8×16 @ 4.0、64 rays）只是**结构体默认**，实际运行参数由 `LumenConfig`/preset 覆盖——查配置以 `LumenTypes.h` 与 `RenderPipelineConfig.cpp::LumenConfigFromPreset()` 为准。

### 4.2 当前档位与覆盖

**LumenConfig 基线默认（`LumenTypes.h:89-110`，Sponza known-working）**：

| Volume | 网格 | 间距 | rays/probe | ray_max_distance | probe 中心跨距 (N-1)×S |
|---|---|---:|---:|---:|---|
| Near | 32×16×32 | 3.0 | 128（T8.13: 64→128） | 50 | 93×45×93（±46×±22×±46） |
| Far（T4.4） | 16×8×16 | 8.0 | 同 near | 128 | 120×56×120（pad 后 AABB 128×64×128） |

**分档预设（`RenderPipelineConfig.cpp::LumenConfigFromPreset()`）**：

| 档位 | Near volume | rays/probe | Far volume | SC capture budget |
|---|---|---:|---|---:|
| Off | 8×4×8 @ 8.0 | 16 | 关 | 0 |
| Low | 16×8×16 @ 4.0 | 32 | 关 | 64 |
| Medium（Sponza 调优基线） | 32×16×32 @ 2.0 | 64 | 开 | 256 |
| High / Ultra / UltraRT | 32×16×32 @ 2.0 | 64 | 开 | 512 |

对照 §2.2：Medium 档的 near volume（32×16×32 @ 2.0 ≈ 62×30×62 m 跨距）密度已略高于 RTXGI 推荐区间的上限；16×8×16 @ 4.0（Low 档 / 旧默认）属于**偏稀疏、偏大范围**的一档，probe 中心覆盖 60×28×60 m。

**预算侧**：T4.5/C5 平台画像（`GIPlatformProfile::Detect`）按每线程读数上限钳制 `max_probes_per_frame`/page budget；far volume 的 `update_budget_divisor = 4` 把远场 probe 摊到 1/4 的每帧更新配额（`StandardRenderPipeline.cpp` T5.5 段）。这正对应"probe × rays/probe 才是真实成本"的原则。

### 4.3 RTXGI 三件套（scrolling / relocation / classification）落地状态

| 机制 | 状态 | 代码锚点 |
|---|---|---|
| Camera-centered 滚动网格 | **骨架在、当前锁死**：`UpdateProbeOrigin` 有 camera-snapped 滚动 + relocation_shift 计算，但入口 early-return（网格锁定），滚动为死代码 | `LumenDDGIPass.cpp:1363-1407`；结论见 `Docs/completed/2026-09-10-cascade-sh-l1-unification.md` T4 |
| Importance 更新调度 | **脚手架在**：`DDGIProbeState`（max_depth_variance + last_update_frame）为 importance-based partial update 预留；当前 `max_probes_per_frame = 16384` 全量更新（无闪烁优先） | `LumenDDGIPass.h:26-32,48` |
| Probe relocation（挪出墙体） | **未实现**（UBO 里 ProbeRelocationShift 字段已声明但无人读） | `LumenDDGIPass.cpp:1363-1365` 注释 |
| Probe classification（invalid/non-static 标记） | **未实现** | — |

存储格式背景：每 probe 存 SH L2 9 系数 RGB irradiance + depth variance；SPGI 的 DDGI fallback 只读 L0+L1（已知降阶，见 cascade 文档 T3）。probe 数从 2048 提到 16K 时不只是存储线性涨，**真正昂贵的是 `probe × rays/probe` 的 trace/update**。

---

## 5. 与 SC + DDGI + SPGI 架构的分工（多尺度方案）

本引擎的 GI 信号链是：

```
Direct Light → Surface Cache ↕ DDGI → SPGI / Screen GI → Fusion → Deferred Lighting
```

在这个架构下**不应该把 DDGI 做得特别密**——高频近场归 screen-space 系，DDGI 只负责稳定的低频 world-space diffuse bounce：

| 空间尺度 | 负责方 |
|---|---|
| 0.1 ~ 1 m | Screen GI / SPGI + Surface Cache（近场高频） |
| 1 ~ 3 m | Near DDGI |
| 3 ~ 8 m | Normal DDGI |
| 8 ~ 20 m+ | Far DDGI / coarse GI |

由此 `spacing = 3~4 m` 对 DDGI 完全合理；墙角、柱子、薄墙、小房间等 <1~2 m 的局部 GI 交给 SPGI + Surface Cache。

### 5.1 Clipmap 式多级 spacing（参考形态）

同一场地用多级 volume 叠加，每级同网格、间距倍增：

```
Camera
   │
   ├─ DDGI L0   16×8×16   spacing = 1.5 m   覆盖 ≈ 22×10×22 m
   ├─ DDGI L1   16×8×16   spacing = 3.0 m   覆盖 ≈ 45×21×45 m
   └─ DDGI L2   16×8×16   spacing = 6.0 m   覆盖 ≈ 90×42×90 m
```

总 probe 数仅 `2048 × 3 = 6144`，而不是单级硬堆 `64×32×64 = 131,072`。

**本引擎对应物**：T4.4 的 near/far 双 volume（near 32×16×32 @ 2~3 + far 16×8×16 @ 8）就是这套思路的两级实现；T5.5 运行时支持显式 `volume_overrides` 列表（cap 4），要扩成三级只需在配置层加 volume，不动 pass 代码。RTXGI 侧同样支持 Volume 跟随 Camera/Player 并以 scrolling grid 更新 probe（滚动复用 probe，不重建）。

---

## 6. 调参决策树（把 §3/§5 落成操作）

1. **先确认问题归属哪一级**：近场高频（<1~2 m）→ 查 SPGI/Surface Cache；中低频色偏/能量 → 查 DDGI（本文）；框外死黑 → far volume 覆盖。
2. **调 near spacing**：以 RTXGI 推荐区 2–3 m 与"墙厚 ≥ ½ spacing"为界；Sponza 类室内+半开放建议围绕 `spacing ∈ {2.0, 2.5, 3.0}` 扫（Medium 档已是 2.0）。
3. **不要先用 probe 数换质量**：`32×16×32 → 64×32×64` 会让 trace/update 成本 ×8；等价预算更值得花在 scrolling 激活、relocation/classification、importance 调度（§4.3 的三个缺口）。
4. **每档改动必须带测量**：FPS ≥ 30 硬底线 + GIVerify/能量审计读数，遵守 08-24 总计划 §6 执行红线。

---

## 7. 常用速查

- 本引擎当前 known-working（Sponza）：near **32×16×32 @ 3.0 / 128 rays** + far **16×8×16 @ 8.0**（`LumenTypes.h:89-110`）。
- 分档预设：`RenderPipelineConfig.cpp::LumenConfigFromPreset()`；Off 8×4×8@8 / Low 16×8×16@4 / Medium+ 32×16×32@2。
- 覆盖跨距口诀：**(N−1)×S**，AABB 外再 pad 一个 cell。
- RTXGI 推荐间距：**2–3 m**；过密暴露网格结构；墙厚需 ≥ ½ spacing 量级。

---

## 8. 相关文档索引

| 文档 | 关系 |
|---|---|
| `Docs/2026-08-24-gi-optimization-plan.md` | GI 总体计划（架构/效果/接口三轨道、执行红线）。⚠️ 其 §1.1 "无 clipmap、无远场" 的表述写在 T4.4 之前，现状以本文 §4 为准 |
| `Docs/2026-08-09-vulkan-lumen-gi-port-design.md` | Lumen 全家桶 Vulkan 移植设计（DDGI 走 WGSL Mode 11 自循环的 D3 决策） |
| `Docs/2026-08-26-spgi-lumen-screen-probe-fusion.md` | Screen Probe 侧（SPGI）与 DDGI 的分工/融合 F1–F5 |
| `Docs/2026-09-16-spgi-screen-probe-distribution-reference.md` | **姊妹篇**：SPGI 屏幕空间分布参考（密度按像素/Tile、GBuffer 布点、ray 预算、重投影历史）；本文管世界空间（米），它管屏幕空间（像素），两者分工表见其 §8 |
| `Docs/completed/superpowers/specs/2026-07-06-mode10-ddgi-design.md` | DDGI 最初设计（Mode 10 静态烘焙 + Mode 11 动态） |
| `Docs/completed/2026-09-04-spgi-energy-calibration-and-ddgi-overexposure.md` | DDGI 过曝根因（SC 注入钳位绕过）——能量类问题先查这里再动参数 |
| `Docs/completed/2026-09-10-cascade-sh-l1-unification.md` | SH L2 存储格式、滚动锁死（T4）、SPGI fallback 降阶（T3）的核验记录 |
| `Docs/2026-09-09-gi-physical-relation-calibration-plan.md` | GI 物理关系校准（C0–C5），能量/亮度标定主线 |
| `Docs/plans/2026-09-09-vulkan-direct-gi-recovery-plan.md` | Vulkan 直射 GI 恢复（含 DDGI 探针索引 bug 闭环） |
| `Docs/2026-09-10-gi-energy-audit-new-session-prompt.md` / `Docs/plans/2026-09-10-gi-transport-session-prompt.md` | 能量审计与多弹射恢复的 session 交接 |

## 9. 参考文献

1. RTXGI DDGIVolume 文档（probe spacing 建议、网格结构暴露、滚动更新、relocation/classification）：https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/main/docs/DDGIVolume.md
2. RTXGI `DDGIVolume.cpp`（Volume AABB 按 `probeSpacing * (probeCounts - 1)` 计算）：https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/main/rtxgi-sdk/src/ddgi/DDGIVolume.cpp
3. RTXGI `DDGIVolume.h`（Volume 定义与 probe relocation/classification 接口）：https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/main/rtxgi-sdk/include/rtxgi/ddgi/DDGIVolume.h
