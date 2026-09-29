# [SC] 捕获近平面 near=0 崩溃机制排查（R2.1 产出）

> 日期：2026-09-20 ｜ 标签：**[SC]** ｜ 状态：✅ 机制文档 + 参数化方案（不承诺 near=0 落地）
> 上游：`Docs/2026-09-20-sc-correctness-and-reflections-integrated-plan.md` §R2.1
> 历史悬案：near=0.05 → 0 实验曾致探针场有效率崩溃 0.42→0.03（已回滚，机制未明）

## 1. 结论（TL;DR）

**正交投影 near=0 在数学上不退化**。`CreateOrthographicMatrix`（RHIMath.h:379）
的深度映射为线性

```
z_clip = z_view / (near - far) + near / (near - far)
near=0:  z_clip = -z_view / far        ∈ [0,1]，z_view=-near=0 → z_clip=0
```

无除零、无 NDC 塌缩、无 reversed-Z 假设破坏（引擎为 Metal/Dawn [0,1] 深度
惯例，正交情形 near=0 与 near=0.05 仅平移映射零点 0.05）。**"near=0 退化"
这一原始假设确认为伪**——与 09-19 WRC 完工文档 §3.1 的怀疑（CreateOrthographic-
Matrix near=0 退化）相反。

真正的行为差异与崩溃机制假说见 §3：near=0 放行的是**贴着卡片平面掠过的
几何薄层**，它们以 depth≈0 的合法测量身份大规模进入卡片内容，与
content-ready 门禁期 SC↔probe 双稳态分支（`sc-probe-bistability` 记忆）耦合，
造成探针场质量塌方——是**内容语义突变**而非投影数学故障。

## 2. 捕获链 near 耦合点全景（逐项核查）

| 环节 | 文件 | near 耦合方式 | near=0 影响 |
|---|---|---|---|
| 正交投影 | `RHIMath.h:379` CreateOrthographicMatrix | proj 矩阵参数 | 无退化（§1） |
| 光栅化裁剪 | proj near 平面 | 几何 < near 被裁 | 0.05→0 放行贴面薄层 |
| CPU cluster 剔除 | `GPUDrivenDrawPipeline.cpp:4346` ExtractFrustumPlanes(view_proj) | 同一 proj | 同步放行 |
| **存储深度** | `CardCapture.frag` `out_axis_depth = dot(world_pos - center, fwd)` | **与 near 解耦** ✓ | 不变 |
| CPU 供给 | `SurfaceCachePass.cpp:1456-1462` misc.x/y = 0.05, 2*halfAxis+0.5 | 显式参数 | 唯一改动点 |
| VS 消费 | `CardCapture.vert` misc 仅声明未使用 | 无耦合 ✓ | 无 |
| 查询侧 | `SurfaceCacheData.glsl` `depth < 0 → 拒绝` | axis-depth 语义 | 不变 |

结论：捕获深度编码（axis-depth，世界坐标点积）与 near 完全解耦——near 只
控制**哪些几何被光栅化**，不改变任何已捕获 texel 的数值。

## 3. 崩溃机制假说（按可能性排序）

**H1（主假说）掠射薄层伪表面潮**：near=0.05 时，距卡片平面 <0.05 的几何被
裁掉；其中大量是掠射穿越卡片视锥的邻面几何（Sponza 拱廊的斜墙、旗帜边缘）。
near=0 后这批几何以 axis_depth≈0 被捕获——在查询侧是合法测量（depth≥0、
along 误差 <0.9 容差），会赢得"最近的表面"身份，把本应命中的真实表面查询
引到这些薄层 texel 上。数百张卡 × 数千 texel 同时突变，等效于一次全卡内容
污染事件；探针场 EMA（α=0.1）需要上百帧消化，暖启动窗口内表现为有效率
0.42→0.03。

**H2（耦合放大）双稳态分支翻转**：09-20 双稳态修复前，卡片内容突变会翻转
gi_content_ready 分支选择（snapshotStable ↔ MarkFullRebuild 互锁），当时的
实验跑在修复前代码上，崩溃幅度可能被该机制放大。

**H3（已排除）投影数学退化 / 深度编码耦合 / reversed-Z 破坏**：§1、§2 核查
排除。

## 4. 参数化方案（R2.1 承诺的交付物）

1. **捕获近平面提参**：`PRIMAL_SC_CAPTURE_NEAR`（默认 0.05 = 现行为），
   SurfaceCachePass.cpp 两处 `0.05f`（proj 与 misc.x）同源替换；env 不设时
   逐位现行为。
2. **近贴面 texel 下限**（若实验要重跑 near<0.05）：捕获端对 axis_depth <
   near_floor 的 texel 写 depth=-1（无效语义，查询侧已拒绝）——把 H1 的
   薄层伪表面在源头无效化，而非依赖查询侧容差兜底。
3. **实验裁决口径**：PRIMAL_WRC_TRACE_STATS=1 六段漏斗 + hitLum +
   GI_DCGAIN amp<0.05 线；near ∈ {0.05, 0.02, 0} 三档，若 H1 成立，near=0
   的 boundsFail/orientFail 会显著上升而 found 不涨——即伪测量回潮特征。

## 5. 与 R2 其它两项的关联

- own-surface capture（B2，`PRIMAL_SC_CAPTURE_OWNER=1`）与 near 参数正交：
  前者管"谁能贡献 texel"，后者管"从哪个距离开始光栅化"。
- 退化面片（B4）已在本计划内解决：全零 bounds mesh 显式降级 +
  content-ready 门禁判据收口（StandardRenderPipeline [R2.3] 标注），注册
  不再等 600 帧超时。
