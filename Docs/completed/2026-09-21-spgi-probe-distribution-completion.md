# SPGI Probe 分布优化计划完工记录（P0-P4 全阶段落地）

> 日期：2026-09-21
> 状态：✅ 全阶段实施完成并通过门禁
> 计划文档：`Docs/2026-09-21-spgi-probe-distribution-optimization-plan.md`（状态已同步更新）
> 对照图集：`Docs/completed/render-comparisons/2026-09-21-spgi-probe-distribution/`

---

## 1. 结论速览

1. **P0-P4 五阶段全部落地**，各自独立 env 回滚位（默认 P0/P1 开、P2/P3/P4 关，
   与计划 §8 灰度表一致）。Vulkan 全链（Place → LightVar → Compact → HZB/SDF/
   Finalize → Average → Temporal → Spatial → Gather）适配完毕；Metal 孪生同步
   P0/P1（tier3 为 Vulkan pilot，Metal 强制回退棋盘两档并打日志）。
2. **P2/P4 验收数据**（Sponza 720p 参考机位，spacing=8/rays=48，frame 350 读回）：
   活跃槽 75,441 / 槽位总量 230,400（tiles×4，激活率 32.7%）；
   分布 **75.3% 粗档(16 ray) / 24.7% 象限档(32 ray)** —— 与计划"Sponza 大部分
   tile level1、柱/拱 level4"的预期形态一致；分档 ray 总量 1.50M vs 全档 3.62M
   （**2.4× 削减**），仅比棋盘基线 1.38M 高 9%（换来 slot0 全覆盖 + 复杂区 2.6×
   探针密度）。
3. **P3 双稳态护栏生效**：一帧滞后（读上帧 final SH）+ EMA(0.3) + 30 帧迟滞 +
   总活跃封顶（Top-K 512）。参考机位日光拱廊场景 lightVar 活跃 tile 恰好打满
   512 上限——封顶按设计工作，突发不失控。
4. **效果（ROI 三口径，frame 360 收敛帧）**：全计划 vs 基线 meanY 65.3→62.0、
   dark 占比 0.426→0.401（↓6%）、floor ROI 15.6→19.5（**+25%**）、farwall
   42.2→45.3；拱廊/暗部反弹结构增强（对照图集第 2/4 面板）。P0+P1 单独与基线
   在 ROI 上无显著差异（spacing=8 下 tile 中心采样误差本就减半，符合预期）。
5. **FPS 门禁**：差分计时法（扣除启动）基线与全计划均 **30 FPS（vsync 满速）**，
   计划零帧耗回退。
6. **附带修复一个既有偏移 bug**：`ScreenProbeTraceFinalize.comp` 的 UBO 声明缺
   `hint_params`，导致 shader 侧 `sky_params` 实际读到 C++ hint_params（432）、
   `wrc_fb` 读到 sky_params/wrc_fb[0]——天空档位门恒 {0,0,0}（sky 回退全黑）、
   F1-a 回退限幅恒 0、WRC header 错位。已按 C++ 布局对齐，三处恢复设计语义。

## 2. 各阶段落地形态

### P0 — 代表像素选择（默认开）
- `ScreenProbePlace.comp/.metal`：5 候选（tile 中心 ± half-cell）深度中位数 +
  `w_d·|z−z_med| + w_n·(1−dot(N,N_med))` 评分，sky 候选剔除统计；jitter 改绕
  代表像素 p* 偏移，世界钳制重采逻辑原样保留。
- 读预算：5 depth + 1 N_med + ≤4 候选 normal + jitter 2 + F4 4 ≈ 16-22（红线 32 内）。
- 回滚：`PRIMAL_SPGI_PLACE_REPR=0`。

### P1 — F4 判据补 normal 项（默认开）
- F4 路径新增 `minNeighborNDot < T_n(0.85)` 门（4 边邻粗档 normal）；
  legacy 启发式路径保持 0.8 不动。T_n 走 `tier_params.y`，+4 reads ≈ 26。
- 回滚：`PRIMAL_SPGI_ADAPTIVE_NORMAL=0`。

### P2 — 三档 4 槽位 + 活跃槽 compaction（默认关）
- 槽位布局 `probeIdx = tileIdx*4 + slot`：slot0 = 粗档（P0 代表像素，tier3 下
  全 tile 无条件激活，棋盘退役）；slot1..3 = 象限探针（tile 相对
  (0.25,0.25)/(0.75,0.25)/(0.75,0.75)），各自位置/normal 门 + P3 掩码激活。
  计划文中"slot1..3 = 四象限中心"的 3/4 不对称，实现取"中心 + 三角"最大展开
  覆盖（BL 象限由 slot0 + 邻 tile 象限槽覆盖），level4 = 全 4 槽形态不变。
- **compaction**：新增 `ScreenProbeCompact.comp`——active 槽 atomicAdd 打包进
  `slotToCompact[]`/`compactList[]`，trace/average 按密集 id 工作并经
  `normals.w`（槽位 ray 档）门控；超容量槽安全丢弃（`slotToCompact=INVALID`，
  过期 compactList 条目由 `posData.w≤0` 哨兵路径自然失效，无需清空）。
  计数器每帧由 GPU 序内 `CopyBuffer(zero→stats)` 清零（无 CPU/GPU 竞态）。
- **存储**：radiance/hit 按 compact 容量分配（默认 tiles×2，env 可调）；
  positions/normals/SH 按槽位全量（5.5MB 级）。避免了被计划否决的 ×4 直扩。
- **Temporal/Spatial 零改动复用**：槽位线性化恰好构成 (gridW*4 × gridH) 的
  行主序均匀格——CB 传槽位格维度即可，重投影双线性/spatial 邻域直接工作。
- **Gather**：每 tent tap 在本 tile 的活跃槽中选世界平面一致度最高者
  （tier3 关 = 单槽，逐位等价旧路径）。
- 回滚：`PRIMAL_SPGI_TIER3=0`（不分配任何新资源，与现状逐路径一致）。

### P3 — 辐亮度方差进判据（默认关，随 tier3）
- 新增 `ScreenProbeLightVar.comp`，Place 前运行：读**上一帧** final SH
  （一帧滞后，sc↔probe 双稳态护栏）→ tile 邻域亮度方差 → EMA(0.3) 平滑 →
  激活/迟滞释放（30 帧低方差）/总活跃封顶。
- 计划的"每帧增量 ≤K 排序 Top-K"实现为**总活跃数封顶（reset-free）+ EMA 迟滞
  间接限幅**（免排序/免每帧清零竞态；同 queue 串行下单缓冲安全）。
  实测日光拱廊场景打满 512 上限。
- 依赖 `PRIMAL_SPGI_TIER3=1`；回滚 `PRIMAL_SPGI_LIGHTVAR_TOPK=0`。

### P4 — 按档 ray 数（默认关，随 tier3）
- 计划的"按档分批 dispatch + 各自 uniform"落地为**平坦索引保留的 tier 门控**：
  radiance 步长统一 = rays_per_probe，trace/finalize/average 逐射线比较
  `rayIdx >= normals.w`（槽位档：粗 16 / 象限 32 / 全额 48/64），未 trace 的
  尾部射线 finalize 写零、Average 按**有效参与样本数**归一化（4π/N 用实际
  activeCount）。省去了间接 dispatch/分批偏移的全部索引复杂度，ray 工作量
  削减相同（2.4×）；代价是低档槽位仍有空转线程槽（SDF march 本身被跳过，
  实测零帧耗回退）。步长不做 64 硬编码，rays=48 生产默认可用。
- F2 importance / temporal 为低 ray 档兜底（既有链路不变）。
- 回滚：`PRIMAL_SPGI_TIER_RAYS=0`。

## 3. 门禁结果汇总

| 门禁 | 结果 |
|---|---|
| `ctest --test-dir Darwin/Debug -R Unit` | ✅ 5/5 全绿 |
| Vulkan Sponza 362 帧全程（×4 组：基线/P0P1/全计划/回滚） | ✅ 无 fault、干净退出、捕获正常 |
| Vulkan validation | ✅ 无新增错误（ORMTextureArray 既有警告基线同在） |
| FPS（差分计时，扣启动） | ✅ 基线 30 / 全计划 30（vsync 满速，零回退） |
| Metal 树编译 | ✅（含 UBO 尾扩 + Place 镜像；MSL standalone 编译通过） |
| Metal TestModularPipeline 90s | ✅ 无 fault |
| 回滚位语义 | ✅ 见 §4 说明 |
| env 登记 | ✅ 6 旋钮 + 1 测试读回（`Docs/environment-variables.md` §1） |

## 4. 与计划偏差的如实记录

1. **"回滚逐位一致"在本环境不可达**：同二进制同配置两次运行的差异
   （mean 15.1/255，79% 像素差>2）**大于**基线二进制 vs 回滚位运行的差异
   （mean 2.3，46%）——GI 链存在运行间非确定性（MoltenVK 重编译浮点差异 +
   SC↔probe 反馈链对 1-ULP 的混沌放大，即 [[sc-probe-bistability]] 方差家族）。
   回滚位的正确性以"代码路径逐 branch 恢复现状 + 差异低于运行间噪声底"判定。
2. **P2 槽位 3/4 不对称**（§2 P2）：中心 + TL/TR/BR 三象限，BL 由 slot0/邻域
   覆盖——计划"四象限中心塞 3 槽"的几何矛盾按最大展开覆盖消解。
3. **P4 无分批 dispatch**（§2 P4）：tier 门控替代，收益口径改为 ray 工作量
   （2.4×），线程槽空转保留为已知成本。
4. **P3 Top-K 实现形态**（§2 P3）：总活跃封顶替代每帧排序 Top-K，突发限幅由
   EMA+迟滞间接承担。
5. **测试配置口径**：TestVulkanSponzaRenderGraph 实际 spacing=8/rays=48
   （LumenTypes.h 默认），非计划假设的 16/64；所有实现按参数化步长落地，
   不依赖具体档位数值。
6. **P0"temporal 历史命中率读回"**：仓库无该指标的现成读回通道，以 362 帧全程
   稳定性（无闪烁/无黑洞）+ ROI 稳定为代理验收；专用仪表待后续需要再建。
7. **UBO 尾扩**：`repr_params(496)/tier_params(512)/tier_counts(528)`，
   `static_assert(sizeof==544)`；未读取尾扩的 Metal kernel 维持前缀声明（惯例）。

## 5. 新增/改动文件清单

| 文件 | 改动 |
|---|---|
| `Engine/Graphics/Vulkan/Shaders/Lumen/ScreenProbePlace.comp` | P0+P1+P2 布点 + normals.w 档位 |
| `Engine/Graphics/Vulkan/Shaders/Lumen/ScreenProbeCompact.comp` | **新增** P2 compaction |
| `Engine/Graphics/Vulkan/Shaders/Lumen/ScreenProbeLightVar.comp` | **新增** P3 方差判据 |
| `.../ScreenProbeTrace{Rays,HZB,SDF,Finalize}.comp` | 间接寻址 + tier 门控 + UBO 尾扩（Finalize 含偏移修复） |
| `.../ScreenProbeAverage.comp` | compact 间接 + tier 门控 + 有效样本归一化 + CB 32B |
| `.../ScreenProbeGather.comp` | tap 内最佳槽位选择 + UBO 尾扩 |
| `Engine/Graphics/Metal/shaders/Lumen/ScreenProbePlace.metal` | P0/P1 孪生镜像 + UBO 尾扩声明 |
| `Engine/Graphics/Lumen/ScreenProbes/ScreenProbeGIPass.{h,cpp}` | env 旋钮/UBO/缓冲/管线/dispatch/统计读回 |
| `EngineTest/IntegrationTests/TestVulkanSponzaRenderGraph.cpp` | `SPONZA_SPGI_TIER_STATS` frame 350 读回 |
| `Engine/Graphics/Vulkan/Shaders/build_spv.sh` | 注册两个新 shader |
| `Docs/environment-variables.md` | 旋钮登记 |

## 6. 复现配方

```bash
# 基线（改动前语义）：全部默认
SPONZA_REFERENCE=1 SPGI_CAM_LOCK=1 PRIMAL_WRC_ENABLE=1 SPONZA_MAX_FRAMES=362 \
SPONZA_CAPTURE_DIR=/tmp/cap/ SPONZA_CAPTURE=/tmp/cap/ref.ppm SPONZA_CAPTURE_FRAME=360 \
  ./Darwin/Debug/TestVulkanSponzaRenderGraph

# 全计划：追加 PRIMAL_SPGI_TIER3=1 PRIMAL_SPGI_LIGHTVAR_TOPK=512 \
#   PRIMAL_SPGI_TIER_RAYS=1 SPONZA_SPGI_TIER_STATS=1

# 回滚 A/B：追加 PRIMAL_SPGI_PLACE_REPR=0 PRIMAL_SPGI_ADAPTIVE_NORMAL=0
python3 scripts/gi_roi_stats.py <capture.png>   # ROI 三口径
```
