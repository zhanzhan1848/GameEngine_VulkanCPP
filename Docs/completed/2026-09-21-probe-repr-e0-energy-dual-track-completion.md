# [GI] [SC] [SPGI] [WRC] [PFX-无] Probe 表示优化 + 中庭能量链双轨——E0/P0/P1/P2/P4 全序完工

> 标签：**[GI] [SC] [SPGI] [WRC] [DDGI]** ｜ 日期：2026-09-21 ｜ 状态：✅ 完工（全阶段落地 + 仪表数据 + frame 780 统一配方验证）
> 完工范围：`Docs/2026-09-21-probe-representation-optimization-plan.md`（R4 执行序 E0→P0→P4→P1→P2，P3 条件项按计划登记）
> 与 `Docs/2026-09-21-courtyard-upstream-energy-investigation.md`（§6 第 0 步仪表 + Bounce 轨 + Sky 轨）。
> 上游：[不对称-F]（F0-F6）、[分布-完工]（P0-P4 SPGI 分布）、[GAP]、[深查]。

---

## 0. TL;DR

两文档的全部可执行阶段落地完毕，Vulkan 统一配方（frame 780）验证稳定。**核心成果**：

1. **E0-B 主根治**：SC lighting atlas 增加 nodirect 副本（LightEval 同帧另存 `result − direct·ρ/π`），SPGI 探针查询收敛到 indirect-only；HZB/SC 亮度惩罚退役为 firefly 软限幅（膝点 1.0→8.0）。**floor 反弹 ROI +73%（对回滚对照）/ +7.6%（对改动前基线），meanY 持平（无全局提亮）**。
2. **E0-I 决策仪表**：diag mode 4/5 分通路隔离 + SPGI SC 产出率漏斗 + 下行失败计数（K-3）。首跑即产出定论：**SPGI 侧 SC 拒绝主因是 orientFail 47.9%（非 boundsFail 43.9%）**；下行黑区仅 5 条射线（可忽略）。
3. **P4 三指标**：WRC 64-dir→SH9 重建误差标准化（近场中位 13.4% FAIL / 远场 2.6% PASS——**近场 SH9 带宽不足的定量证据**）、WRC 角向方差（近场 p50 0.79 vs 远场 0.031）、SPGI SH2 重代入误差（P50 0.47）。
4. **P1**：SPGI raw ray 三缓冲→单缓冲，**节省 88.5MB**（57,600×48×16B×2 槽）。
5. **P2**：WRC probe_data AoS(132 u32)→SoA 双区域（radiance 512B 对齐 + meta 独立区），6 个消费端全部迁移。
6. **P0**：探针球着色模式门（state/radiance/confidence/variance）+ `PRIMAL_WRC_PROBE_SPHERES` env 入口；同帧可分别截出 state 球与 radiance 球。
7. **过程发现并修复一个回路稳定性问题**：nodirect 若接入 prev_light/tile 查询源会抽走 SC 回路能量源 → frame 780 全图坍黑（meanY 64.6→9.4，B14 "indirect fading" 的回路机制复现）；修复后收敛到 nodirect 仅作探针查询侧静态直射项隔离。

---

## 1. E0-I 决策仪表（两文档 §6 第 0 步）

### I-1 Debug A/B/C 分通路隔离（`SPGI_TRACE_DIAG=4/5`）

`ScreenProbeTraceFinalize.comp` 尾部按 `diag_params.x` 门控分支置零（归因统计照记原始 radiance）：
mode 4 = fill-only（branch 0/1 置零）；mode 5 = transport-only（branch 2/3 置零）。Metal 孪生
`ScreenProbeTraceRays.metal` 同语义（就地派生分支号，无统计通道）。CPU 零改动（env 直通）。

**frame 360 实测（ROI meanY/255 显示空间）**：

| 配置 | meanY | arch | alcove | floor | mlwall | farwall |
|---|---|---|---|---|---|---|
| C（新默认，E0 生效） | 63.9 | 55.4 | 25.9 | 17.9 | 71.5 | 42.7 |
| **A = fill-only** | 64.4 | 48.3 | 32.3 | **8.1** | 78.8 | 43.0 |
| **B = transport-only** | 66.3 | 62.0 | 42.2 | **48.3** | 46.9 | 51.3 |
| B′ = B + 摘惩罚 | 66.1 | 61.5 | 41.8 | 47.9 | 46.3 | 51.0 |

判读（[能量-排查] §5.5 验收语义镜像）：
- **地面反弹完全由 transport 通路送达**（B floor 48.3 vs A 8.1）——"太阳→地面→墙"一跳在 SC/HZB 腿，不在天空 fill；
- fill 通路托起整体轮廓（A≈C 的整体结构），transport 给局部结构与色弹——与双通路假设一致；
- **B′≈B（meanY 差 0.2）**：nodirect 分离落地后，摘惩罚几乎不再增加能量——**惩罚退役的收益已被分离天然兑现**，膝点 8.0 只压 firefly。

### I-2 SPGI SC 产出率漏斗（`PRIMAL_SPGI_SC_FUNNEL`）

finalize SC 分支换用 `scQueryCardCandidateDbg`（qpadM=0 保持逐位同语义），拒绝级原子累计进
`fail_stats_buffer_`（binding 22，8×u32，恒累加无 diag 门）。frame 350 读回（累计 frame 0-350）：

```
candidates=4.00e9  boundsFail=43.95%  orientFail=47.88%  depthFail=4.88%  found=1.87%
downNoFallback=5（下行失败且无兜底）  downClampServed=0
```

- **SPGI 侧拒绝主因是 orientFail（47.9%）而非 boundsFail**——与 WRC 侧（boundsFail 50.6%）分布不同
  （SPGI 的 face-gate hint 缩放注入是额外拒绝源）。S2/B-2 专项的下一步应先攻 **SPGI 朝向门**，
  再谈卡片覆盖；
- **K-3 下行黑区可忽略**（350 帧 6.3 亿射线中 5 条）——F1-a + 天空门链路已闭合，§2 残余 b 结案。

---

## 2. E0-B Bounce 轨（主根治）

### 2.1 实现

- **LightEval 另存 nodirect 副本**（`SurfaceCacheLightEval.comp` binding 16，Vulkan；Metal [[texture(7)]]）：
  `result_nodirect = max(result − direct_light·ρ/π, 0)`（同 64 上限）——解析直射项剥离；
- **SPGI finalize 绑定交换**（`ScreenProbeGIPass.cpp`）：`PRIMAL_SPGI_SC_NODIRECT`（默认 1）时
  binding 1 改绑 `lighting_nodirect_atlas_`，混合/nodirect atlas 都缺省时回退 dummy 语义不变；
- **惩罚退役**：HZB/SC 两处 `1/(1+2(lum−1))` → `1/(2+(lum−knee))` 形式的软限幅
  （`knee = tier_counts.y`，env `PRIMAL_SPGI_SC_PENALTY_KNEE` 默认 8.0；mode 2 对照、NOPENALTY 全关保留）；
- **RG 身份闭环**：nodirect 以 `SurfaceCache_Lighting_NoDirect_<outIdx>` 单身份 import，
  SC Write(UA) 声明 + SPGI Read(SR)（`SetExternalNoDirectLightingRG` 传递，对齐 T1.1/C2 别名治理），
  LightEval 显式 barrier 对称覆盖 out/hist 两槽——**validation 0 错误**。

### 2.2 过程发现：nodirect 接入回路源 → frame 780 坍黑（重要教训）

首版把 `prev_light`（legacy 自反馈）与 tile radiosity 查询源都换成了 nodirect →
**frame 780 全图坍黑（meanY 64.6→9.4，arch 0.0）**，frame 360 尚正常（衰减需数百帧）。

机制：SC 场的能量源是**每帧 fresh 解析直射**烘进 L_o（经 prev_light 自反馈 / tile 查询回流持续注入）。
nodirect 每代递归剥一次直射 → 回路能量指数衰减 → B14 "indirect fading seconds after entry" 的
回路机制复现（当时以 ambient floor 症状压制，B14 删除后一直靠直射库隐性供能）。

**修复**：prev_light 与 tile 查询源保持混合 atlas（能量源不动），nodirect 仅作 SPGI 探针查询侧的
"静态直射项隔离"——探针少拿一份 texel 静态直射，但仍接收经一跳反馈携带的直射反弹（合法一跳）。
修复后 frame 780 稳定（meanY 63.0）。**B-1 的完整形态（DDGI→SC 常备馈送替代直射库）依赖
[根治] Phase 2/3 的能量单向化回摆，登记为后续项。**

### 2.3 验收数据（frame 780 统一配方）

| 配置 | meanY | dark | arch | alcove | floor | mlwall | farwall | hierarchy |
|---|---|---|---|---|---|---|---|---|
| 改动前基线（09-21 上午二进制） | 63.8 | 0.408 | 64.6 | 26.5 | 17.2 | 66.2 | 41.4 | INVERTED |
| 回滚对照（NODIRECT=0 + knee=1 + tile sky override） | 64.6 | 0.427 | 58.8 | 25.0 | 10.7 | 77.4 | 43.5 | INVERTED |
| **E0 默认（交付）** | **63.0** | 0.399 | 66.1 | **28.4** | **18.5** | 61.7 | 41.8 | INVERTED |
| UE Lumen 参考 | 42.7 | 0.613 | 18.9 | 18.6 | 75.5 | 165.4 | 176.7 | OK |

- floor：18.5 vs 回滚 10.7（**+73%**）/ vs 基线 17.2（+7.6%）——反弹送达改善；
- mlwall −20%（对回滚）：直射泵入撤出后受光立面读数回落——符合"中庭抬升不得以直射泵入为代价"；
- meanY 持平：**无全局提亮**（红线遵守；adapt 开、非 fixed-exposure 口径，绝对值受曝光吸收，相对结构有效）；
- hierarchy 仍 INVERTED：arch(66) > floor(18.5)，与参考（18.9 < 75.5）反向——残余缺口见 §7。

回滚矩阵：`PRIMAL_SPGI_SC_NODIRECT=0`（查询回混合）+ `PRIMAL_SPGI_PENALTY_KNEE=1`（旧惩罚）
+ `PRIMAL_SC_TILE_SKY_OVERRIDE="r,g,b"`（K-1 回退）。

---

## 3. E0-K Sky 轨

- **K-1 tile 天空方向化**（`SurfaceCacheIndirectResolve.comp`）：基色 `(2.75,2.88,3.00)×tier` 常量 →
  `EvaluateSkyBase(tileRayDir) × tier`（include ShaderLib/SkyAtmosphere.glsl；[能量-排查] §3.3 的 ~2×
  残差消除，且上仰射线按解析天空变亮）。射线方向重生成从 hit 分支提升到分支前（天空射线此前
  复用陈旧方向——常量基色时代无害，方向化后必须修）。CPU 只传 `(tier,tier,tier,1)`；
  `PRIMAL_SC_TILE_SKY_OVERRIDE` 回退常量路径（w=0）。
- **K-2 DDGI 逃逸半档 A/B**：`PRIMAL_GI_FAR_SKY=0.5` vs 默认 1.0（frame 360）：meanY 64.1 vs 63.9、
  各 ROI 差 ≤0.5——**半档复退无感**，维持退役（默认 1.0）。远场份额被小天空立体角主导，
  半档不再是显著单项。
- **K-3 下行射线计数**：并入 fail_stats（§1 I-2），黑区可忽略，结案。
- **K-4 点级显式天空可见性（[根治] Phase 3）**：未实施（计划原文即"远期正解"），维持立项登记——
  hierarchy INVERTED 的正解来源。

---

## 4. P0 Probe debug 状态/辐射度解耦

- **模式门**：`PRIMAL_PROBE_SPHERE_MODE = state|radiance|confidence|variance`（或 0-3；默认 state）。
  VS push constant 增 `misc`（x=mode，y=[P2] probeTotal）→ flat varying 下传；
  WRC frag 四模式全实现（confidence=消费计数热图 bits[16..31]、variance=64 方向 var/mean 热图）；
  DDGI 腿 radiance 模式切换 irradiance 管线（SH9 求值），confidence/variance 的 DDGI moments
  未接线 → 回退 radiance + 一次性日志（**登记缺口**）。
- **`PRIMAL_WRC_PROBE_SPHERES=1`**：WRC 球 overlay 测试/离线入口（此前仅编辑器 C ABI）。
- **两处存量 bug 顺带修复**（radiance 模式静默失效的根因）：
  1. irradiance 管线的自加载被错误嵌在 `pixel_shader == INVALID` 门内——测试注入 VS/PS 时永不创建
     （log 实证 `irradiance=INVALID`）；已移出门外；
  2. RWCB 注入点（测试驱动路径）不设 `irradiance_buffer` → 补绑 `(cbIdx+2)%3` 槽（当帧槽在 overlay
     采样时点未落笔，实测黑球）。
- **验收**：state/radiance 同机位双截图到手（球带 mean abs diff 30.3/255，state 特征绿占比 9.1%→1.5%，
  radiance 值即探针场物理亮度——拱内暗、受光面亮）。语义登记：**状态=生命周期诊断，辐射度=能量诊断**。

---

## 5. P4 三个定量指标

| 指标 | 实现 | frame 350/120 首跑读数 |
|---|---|---|
| WRC 64-dir→SH9 重建误差 | `[WRC_PF_CHECK]` 标准化（DUMP=1 即出，不再依赖 PREFILTER）：P50/P95/max + worstProbe 网格坐标 + **P95>15% 告警门** | **近场 FAIL**：median 13.4% / p95 90.5%（worstProbe=(20,3,1) 等）；**远场 PASS**：median 2.6% / p95 6.9% |
| WRC 角向方差 | `[WRC_ANGVAR]` 全 64 方向亮度 var/mean 分布 + mono(<1%) 占比 | 近场 p50 **0.79**（高度各向异性）；远场 p50 **0.031**（平滑）；mono 占比 0-7% |
| SPGI SH2 重建误差 | Average 尾部重代入 |Δ|lum（`PRIMAL_SPGI_SH2_CHECK`，per-probe 归约 → CPU 分位数） | P50 **0.47** / P95 2.04 / max 16.0（绝对 |Δ|lum；NaN 射线计 0——见下） |

判读（"先测后改"）：
- **近场 SH9 带宽不足有了定量证据**（p95 90%）——维持"64-dir = authoritative、SH9 = derived cache"
  架构约定，且 prefilter 降级档在近场不可信；远场 SH9 合格；
- 近场角向方差 0.79 ≫ 远场 0.031 ——分层原则（距离越远空间/角频率同降）首次有了逐体系数字；
- SPGI SH2 重代入误差 P50 0.47（相对 far-cache 亮度量级 ~1-2）——SH2 保留方向梯度的能力与
  评审 #7 的担忧同量级，升档（SH4/oct）待 P3 抽象落地后按此指标复测。

**附带发现**：far-cache 分支 maxLum 3390（diag 统计）+ SH2 检查 NaN——WRC/回退腿存在 NaN/超高亮
radiance 污染（`max(NaN,0)` 不消毒）。已入档为 **radiance 卫生专项**（不阻塞本计划）。

---

## 6. P1 / P2 / P3

### P1 SPGI raw ray 单缓冲（-88.5MB）

`probe_radiance_buffer_[3]` → 单缓冲（生命周期审计：唯一消费者当帧 finalize→Average，
`ScreenProbeTraceFinalize.comp:2524` ResourceBarrier 隔离；temporal 链读 SH 域独立缓冲）。
`GetRayRadianceBuffer(frameIdx)` 签名保留（参数忽略），RAY_DUMP 通道不受影响。
节省 = 57,600 探针 × 48 射线 × 16B × 2 槽 = **88.5MB**（downsample 4 档位时收益 353MB）。

### P2 WRC AoS→SoA

`WRCTypes.h` 布局 v2（单 buffer 双区域）：
- radiance 区：`probeIdx × 128 u32`（K×2；**512B 对齐**，每探针方向连续）；
- meta 区：`probeTotal×128 + probeIdx×4`（mean/var/state/pad 同序）；
- 消费端全迁移：WRCUpdateRadiance / WRCPrefilterIrradiance / WRCCardInject /
  ScreenProbeTraceFinalize(wrcSampleRadiance) / ReflSpecularTrace / ProbeSphereWRC.frag(+Metal)
  + LumenWRCPass 两个读回循环（probeTotal 经 `WRCVolumeData.ProbeMeta.y` /
  `wrc_fb[1].xyz` / PC misc.y 各自可达）；
- `[block=8][dir=64][slot=8]` tile 化**暂不引入**（现全部消费端单探针粒度，probe-major 已最优；
  出现跨探针 wave 消费端时再立项）；
- 验证：零 validation 错误 + PF_CHECK/ANGVAR 结构化分布正常 + 远场 meanLum(sub)=9.51527 在
  frame 120/240 逐位相同（EMA 收敛后跨 120 帧逐位稳定）+ 统一配方截图正常。**严格"AoS 旧二进制
  逐位 A/B"未执行**（改动前二进制已被覆盖，且 E0 改动改变 WRC 输入使跨二进制比较不纯）——
  登记为遗留验证项（复现方法：stash 本计划 P2 触点文件 → 重构前寻址回补 → 同配方重跑比对）。

### P3 octahedral 抽象 / P5 ShaderLib 公共库

- P3：**触发条件未触发**（无方向 bilinear/mip 消费端），按计划原文登记不立项；
- P5（伴随）：K-1 引 `ShaderLib/SkyAtmosphere.glsl`；新增 shader 代码零本地 SH/Chebyshev 副本；
  存量 4+ 处 SH 基副本的收敛仍为持续性目标（未批量替换，避免本轮风险面扩大）。

---

## 7. frame 780 统一配方 vs UE Lumen 参考图（对比分析）

对照物：用户提供的 UE Lumen 参考截图（Sponza 中庭，拱门机位）vs `SPONZA_REFERENCE=1` frame 780
交付截图（`/tmp/e0_final3/*/frame.png`，回滚对照 `/tmp/e0_final_legacy`，改动前基线 `/tmp/e0_baseline`）。

**已对齐的结构**：
1. 直射结构与阴影走向（太阳合同统一后两侧一致）；受光立面/地面亮带的对比度关系成立；
2. 横幅色弹（绿/红/金）经间接光染墙——两侧都有，本引擎的 transport 通路（Debug B）承载；
3. 天空开口的低频 fill（Debug A）轮廓成立。

**残余差距（按 ROI 表量化）**：
1. **hierarchy 反向未解**：参考 floor(75.5) ≫ arch(18.9)；本引擎 arch(66) > floor(18.5)——拱腹（阴影面）
   相对偏亮、地面反弹仍不足 4×。机制上 [GAP] §2.1 的查询产出率 + K-4 显式 V(x,ω) 缺位是主因；
   本轮 floor +73%（对回滚）是方向性改善但未反转层次。
2. **受光立面读数偏低**（mlwall 61.7 vs 参考 165.4）：部分是 adapt 曝光口径差（参考图非本管线
   曝光链），部分是直射泵入撤出后的诚实读数——后续靠 B-2 产出率专项（**SPGI 侧先攻 orientFail
   47.9%**）把合法反弹补回，而非恢复泵入。
3. **天空色深**：参考图开口为饱和蓝天，本引擎发白（EvaluateSkyBase 档位 + ACES + bloom opt-in）——
   显示链 S3 范畴，与能量解耦。

**结论**：双通路框架下"fill 托轮廓、transport 给结构"已成立且有仪表闭环；下一轮杠杆排序：
①SPGI orientFail 治理（新发现）→ ②[根治] Phase 3 显式天空可见性（K-4）→ ③radiance 卫生
（NaN/3390）→ ④DDGI moments 接进探针球 confidence/variance 视图。

---

## 8. 文件触点索引

| 域 | 文件 |
|---|---|
| E0-I/E0-B shader | `Shaders/Lumen/ScreenProbeTraceFinalize.comp`（mode 4/5、膝点、fail_stats、SoA）、`SurfaceCacheLightEval.comp`（nodirect 写）、`SurfaceCacheIndirectResolve.comp`（K-1 + include 扩展声明） |
| E0-B CPU | `SurfaceCachePass.{h,cpp}`（副本分配/布局/绑定/RG 身份/稳定性修复）、`SurfaceCacheTypes.h`（Output 扩展）、`GICacheView.h`、`ScreenProbeGIPass.{h,cpp}`（绑定交换、fail_stats、膝点、单缓冲） |
| P0 | `DebugProbeSphereModule.cpp`、`ProbeSphere.vert`、`ProbeSphereWRC.frag`、`StandardRenderPipeline.{h,cpp}`（env 入口 + RWCB 注入修复）、`EngineTest/shaders/ProbeSphere.metal`（Metal 镜像） |
| P4 | `LumenWRCPass.{h,cpp}`（ANGVAR/PF_CHECK 标准化 + SoA 读回）、`ScreenProbeAverage.comp`（SH2 检查） |
| P2 | `WRCTypes.h`（布局 v2 契约）、`WRCCacheView.h`、`WRCUpdateRadiance/WRCPrefilterIrradiance/WRCCardInject/ReflSpecularTrace.comp` |
| 测试 | `TestVulkanSponzaRenderGraph.cpp`（漏斗/SH2/IRR_DUMP 读回打印） |
| spv | 8 个 .comp/.frag/.vert 重编（glslangValidator -ILumen -IShaderLib，与 build_spv.sh 同参数） |

## 9. 遗留与红线核查

- 遗留：P2 严格逐位 A/B（§6 方法登记）、DDGI moments→球视图接线、radiance 卫生专项、
  SPGI orientFail 治理、K-4/[根治] Phase 3、存量 SH 副本收敛（P5 持续）；
- 红线核查：未恢复 ambient floor（B12/B14）、未引入加性常量天空（§2.6 反模式核查仍然成立——
  nodirect 是查询侧过滤不是常量注入）、未全局抬档（meanY 持平）、未恢复 0.15 时代半档（K-2 实测）。
