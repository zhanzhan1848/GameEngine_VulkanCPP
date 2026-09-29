# 2026-09-10 Cascade SH L1 升级与 DDGI 后端行为统一

> 相关参考：`Docs/2026-09-16-ddgi-world-space-probe-grid-reference.md`（DDGI probe 网格密度/间距/多尺度参考；本文 T4 的"滚动锁死"结论已收录为其 §4.3 现状表一栏）

对 DDGI / SPGI / RadianceCascade 数据表示做了一次代码级评审（起因：外部架构评审文档对
存储格式提出 5 项质疑），落地其中 3 项代码改动 + 2 项防护注释。本文记录设计决定与
核验结论，供后续回归时对照。

## 评审结论速览（先核验、后动手）

| 质疑 | 核验结果 | 处置 |
|---|---|---|
| cascade 只存 RGB，方向信息丢失 | **实锤且更严重**：更新只取 DDGI SH 第 0 系数（9→1，非 9→3） | T1 升级 SH L1 |
| Metal irradiance 邻居混合会穿墙 | 部分成立：仅 Metal 有，权重 0.04×3 正轴邻居，且 +x/+y/+z 单向选邻是系统性方向偏置 | T2 移除 |
| depth 存 variance 还是 E[d²] 口径混乱 | 不存在：`DDGIUpdateDepth.comp` 存 variance 本身，gather 直接 Chebyshev，两端口径一致 | 销案 |
| 网格滚动 vs probe_positions_ 静态填写 | 前提不成立：`UpdateProbeOrigin` 无条件 early-return（网格锁定），滚动是死代码 | T4 防护注释 |
| triple-buffer + EMA 资源冲突 | 条件不成立：MAX_FRAMES_IN_FLIGHT=3 + BeginFrame fence 等待 N-3 + 单队列序 | T5 结论注释 |

另有记录在案的已知降阶：SPGI 的 DDGI fallback 只读 L0+L1（4 reads），DDGI 实为 L2
9 系数（T3 注释，暂不读全 9 系数）。

## T1 — Radiance Cascade SH L1

**动机**：cascade 原本每 texel 只存 DC 系数（`C0 * coeff0`），far-cache 射线无论方向
采到同一 RGB——门洞两侧亮暗不可分。这是整条 GI 链路（DDGI L2 → cascade → directional
query）最大的一次不可逆降阶。

**新表示**：4 张 32³ RGBA16F（双缓冲 → 每槽 L0+L1x/L1y/L1z 共 4 张，总计 8 张 +
1³ dummy）。texel 语义：**RGB = 原始 SH 系数颜色，A = 有效性（1.0 = 本系统写入）**。
注意 L0 存原始系数（不再预乘 C0），评估端统一乘基函数。

**更新**（`RadianceCascadeUpdate.comp/.metal`，手工镜像必须同步改）：
- 8-probe trilinear 对 4 个系数分别加权（原逻辑 ×4）。
- 每系数独立 EMA `mix(new, prev, tw)`，frame≥2 才启用（保持原时域语义）。
- DDGI 不可用 → 全零 + A=1；grid 外 → L0 = `vec3(0.03,0.03,0.035)/C0`（保持旧评估后
  量级）、L1 = 0。
- 绑定布局：Vulkan bindings 0-3 write image / 4-7 prev sampled / 8 params / 9 ddgi；
  Metal texture(0..7) + buffer(2) ddgi + buffer(3) params（setBytes）。

**消费**（`ScreenProbeTraceFinalize.comp` binding 13-16；Metal finalize kernel 在
`ScreenProbeTraceRays.metal`，texture(2)-(5)）：
```
rad = C0*sh0 + C1*(-sh1*ωy + sh2*ωz - sh3*ωx)   // 与 DDGI L0+L1 fallback 同基
rad = max(rad, 0)
```

**Guard 语义变化（重要）**：旧 guard `all(cached > 0)` 在 L1 引入负系数后不成立
（L1 合法地为负），改为 `A > 0.5 && any(sh0 > 0)`。disabled cascade 写全零仍会
fall-through 到 DDGI 四探针回退，行为等价。

**C++**（`ScreenProbeGIPass.{h,cpp}`）：纹理创建、RG import（每系数唯一名
`RadianceCascade_Write_L0/L1x/...`）、cascade update pipeline layout 10 绑定、
finalize layout 17 绑定、`DispatchCascadeUpdate` 签名改 4+4 物理句柄数组。
内存增量 ≈ 1.5 MB。Kill switch `PRIMAL_GI_CASCADE=0` 不变。

## T2 — 移除 Metal DDGIUpdateIrradiance 邻居混合

删除 `DDGIUpdateIrradiance.metal` 的 SPATIAL_W=0.04 +x/+y/+z 邻居 history 混合块
（2026-09-04 棋盘格修复），与 Vulkan 对齐（Vulkan 从来没有）。理由：更新期混合绕过
gather 阶段 Chebyshev visibility（能量穿墙）；+轴单向选邻引入方向漂移。Parity 变体
本就没有该块，未动。**若 Metal 探针接缝复发，正确解法是 geometry/visibility-aware
混合，不是恢复无条件历史平均。**

## 验证记录（2026-09-10 全绿）

- `build_spv.sh` 全量重编（RadianceCascadeUpdate 13.9KB、ScreenProbeTraceFinalize 53KB）。
- 双树构建通过（注意：集成测试二进制实际都输出到共享的 `Darwin/Debug/`，
  `RUNTIME_OUTPUT_DIRECTORY = Darwin/$<CONFIG>/`）。
- `ctest -R Unit`：UnitScreenProbeCacheValidation 通过。
- Vulkan `GI_VERIFY=1`（frame 90）：probe_sh0_lum=2.59 ∈ [0.08,6]，
  gather_avg=1.77 ∈ [0.005,6.3]，trace_inbox 三带全过 → ALL BANDS PASS。
- Vulkan golden（`GI_GOLDEN=1` 差分模式）：worstRel=8.24% < 25% 容差，无需再生基线
  （cascade 只影响 SPGI 远场，不回写 DDGI probe SH）。
- Metal `TestMetalGIFusion`：probe_sh0_lum=9.99 ∈ [0.08,13]（T2 改动后带宽内），
  gather 两带过 → ALL ALIGNED；无 shader 编译错误。
- `scripts/check_shader_copies.sh`：gate OK（RadianceCascadeUpdate.metal /
  DDGIUpdateIrradiance.metal 为 engine-only；ScreenProbeTraceRays.metal 测试副本
  本就 DRIFT 且不被 engine 加载）。

## 遗留 / 后续可选

- SPGI fallback 读全 9 系数（+5 buffer reads/ray）——等有方向性质量需求再做。
- Metal 探针接缝若复发 → geometry-aware 邻居混合（带 visibility/法线权重）。
- DDGI 更新期 probe-shift 支持（logical coord + ring offset）在重新启用相机跟随网格
  前必须先落地，见 `LumenDDGIPass.cpp UpdateProbeOrigin` 注释。
- SPGI 每 probe 64-ray 中间数据的 trace→SH fusion 是带宽优化候选，先 profile 再动
  （trace/finalize 拆分是 Apple Silicon 寄存器压力的有意设计）。
