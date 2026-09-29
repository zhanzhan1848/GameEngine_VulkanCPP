# Sponza vs UE Lumen 观感差距：根因分析与缺口清单

> 日期：2026-09-20 ｜ 状态：🚧 诊断分析（非完工文档；行动项落地后另行出完工记录）
> 触发：用户对照截图——图一 UE Lumen（Sponza 中庭）、图二 `TestVulkanSponzaRenderGraph`（Vulkan Sponza 侧廊望中庭）
> 关联：`Docs/2026-08-24-gi-optimization-plan.md`、
> `Docs/completed/2026-09-19-world-space-radiance-cache-completion.md`（下称 [WRC-完工]）、
> `Docs/completed/2026-09-19-sc-card-lumen-parity-implementation.md`（下称 [SC-完工]）、
> `Docs/completed/render-comparisons/2026-09-16-sponza-gap-audit/`（历史差距审计）

---

## 0. TL;DR

四个观感差距各有明确主根因，其中**只有一个是参数问题，其余全部是结构性缺口**：

| 观感差距 | 主根因 | 性质 |
|---|---|---|
| ① 中庭间接光不亮 | 天空间接档位被压到 0.15 + SC 反弹饥饿 + 后处理不放大亮部（bloom/雾默认关、固定曝光） | 结构（传输）+ 显示链 |
| ② 无色彩弹射 | SC 查询产出率仅 ~14-20%（capture/query 不一致，queryFail 86%）→ 反弹传输饥饿 | **传输链专项（最大瓶颈）** |
| ③ 间接光"平"、无空间结构 | GI 入射场对比度仅 P90/P10≈2.1×；DDGI 近场 3m 粗端（室内 ~1 探针/间）；WRC 近场 2m/注入门控收益**默认未启用** | 密度 + 默认开关 |
| ④ 室内外亮度差不足 | 固定曝光（无 eye adaptation）+ tile radiosity w=2.0 非物理泵阴影 + SSGI/IBL 残留填充 | 显示链 + 非物理放大器 |

另有**对照方法**层面的前提：两张图的机位、太阳方向、场景资产（布幔 vs 旗帜）都不同，
任何修复的前后对比必须走锁定配方（§4），否则 A/B 不可比——这是历史上多轮"改了还是平"结论反复的教训之一。

---

## 1. 对照观察（两图差异现象分解）

| # | 现象 | UE 图一 | 本引擎图二 |
|---|---|---|---|
| A | 中庭区域间接光 | 对面墙整体被阳光洗亮，布幔透光，开放区明亮 | 中庭墙面灰暗，只有地面近处直射光斑亮 |
| B | 色彩弹射 | 橙/青布幔在邻接石面上留下暖色/青色反弹 | 布幔自身饱和，但周围光不含布幔颜色 |
| C | 间接光空间结构 | 明暗交界锐利，反弹有方向性和衰减层次 | 阴影区近乎均匀灰填充 |
| D | 室内外亮度对比 | 拱廊内接近黑 vs 中庭接近白（≥3 EV） | 室内中灰 vs 室外中亮（<2 EV），整体压在一档 |
| E | 空气感 | 太阳开口有 bloom 溢出、雾中光柱 | 无 bloom、无雾、无 god rays |

注：两图分辨率差异不影响上述判断；图一的布幔是 Crytek Sponza 原版资产，图二是旗帜贴图变体，
材质基线不同（见 §3.4）。

---

## 2. 引擎内已证实的根因（按机制分解，含量化证据）

### 2.1 SC 查询产出率 ~14-20% —— 色彩弹射的最大瓶颈（现象 B/C 主根因）

表面缓存（SC）是反弹能量的唯一供给方（bootstrap 翻转后探针不再回馈 SC，tile radiosity 单轨）。
但 SC 查询漏斗在源头大量漏光：

```
[WRC_SC_FUNNEL] 近场单帧 69742 命中 → gridOk 100% → cellFound 67% → found 仅 ~20%
360 帧累计 111 万次候选评估的拒绝分解：
  boundsFail 51%（hitPos 投影不到卡片 UV）
  orientFail 24%（双面墙/邻面朝向不符）
  queryFail  86%（通过前两级者：卡片捕获深度 ≠ SDF 命中面，along 误差数米 ≫ 0.48m 容差）
```

三重衰减（命中 20% × 产出 ~20% × 命中亮度 0.10 vs 逃逸天空 0.756）使**几何反弹 ≈ 0**——
亮面（阳光直射的布幔/墙面）的能量进不了探针场，色弹与反弹层次自然消失。
这与 A3 验收"室内间接 92% 是天空填充"互为印证。

> **[2026-09-20 状态更新]** 上表为本节诊断时的历史数据。行动轮后：
> queryFail 已被查询容差 0.9（`WRCTraceFinalize.comp:181` 编译期常量）消除；
> found 中 ~7000/窗口被证实为伪测量（AtlasInit 幽灵种子/CardFill miss 假平面）并已修复——
> 诚实漏斗 found 24.7%、hitLum 0.434；剩余 boundsFail 50.6% 为真实卡片覆盖缺口（见完工记录 §2）。

机制根源（capture/query 一致性专项，[WRC-完工] §3 遗留 #1）：
- 卡片 face 平面捕获的是"视图内最近可见表面"而非卡片自身表面（自身在 near=0.05 内被裁剪或被遮挡），
  捕获深度=远处几何 → along 一致性检查整批拒绝；
- near=0.05 与深度编码耦合不可直接改（near=0 实验曾致探针场崩溃 0.42→0.03，已回滚）。

修复方向（已在案）：① 捕获近平面/远平面与卡片自身表面的包含关系；② 捕获深度有效性掩码
（未捕获 texel 显式 A=0）；③ 卡片 AABB 扩张（+SDF 命中带）。

### 2.2 卡片覆盖缺口 —— 已大幅改善，残余待验收（现象 B 前置条件）

- 旧状态：33% 命中在卡片网格 AABB 外（织物/小面被"最大投影 5%"过滤器丢弃——恰是色弹源头），
  393 mesh 全走 AABB fallback，运行间卡片集合两态（374 vs 256 张）。
- 已落地（[SC-完工]）：CardBake v2 贴面卡重烘 **389/393**（coverage 均值 0.891、2532 卡）、
  秩轮转预装箱后 atlas 436 卡 75.6%、content-ready 门禁消除两态（两遍 436/436 逐位一致）。
- 残余：4 个 4 顶点退化面片无壳 fallback；**产出率验收在该测试配方下不可测**
  （WRC funnel baked/pre-baked 双向 found=0/cands=0，与卡片烘焙无因果——需 GI 线标准配方复测，[SC-完工] §9）。
  即 §2.1 的瓶颈量化与 v2 覆盖收益**尚未在同一配方下对齐验证**。

### 2.3 天空档 0.15 的结构性代价 —— 中庭不亮的 GI 侧成因（现象 A）

历史修复为压制"室内均匀灰"把天空能量档位从 0.4 压到 0.15
（`GIDebugParams.h:39 sky_color_intensity=0.15`；消费点 DDGI `LumenDDGIPass.cpp:2019`、
SPGI `ScreenProbeGIPass.cpp:1752`、WRC `StandardRenderPipeline.cpp:2855`）：

- 0.4 档：室内 ROI 0.506（均匀灰亮，E1 回路放大主源）；
- 0.15 档：室内 ROI 0.040×gain（暗部反弹层次恢复）——室内问题治好了，
  **代价是中庭这类合法见天的开放区域朝上探针拿到的天空能量也被同档压死**。

UE 的对应机制是 SkyLight 提供强室外环境光 + 探针遮挡保证室内不见天——即"空间化的天空可见性 +
足量天空能量"。我们当前是"无差别压档"：室内外一起挨饿。回路的三个放大根源
（探针馈送常开 → `GIWarmup.h:43` 已默认切断；运行间两态 → content-ready 门禁已修；
隔墙注入 → WRC 注入可见性门控已落地）均已治理，**现在具备"抬档不回弹"的重评条件**，
但需要 §4 配方下量化验证（预期：室内不回升、开放区显著变亮）。

另注意：显示层背景天空（zenith 5.5 线性、×2.0 重标）**不进 GI 回路**
（`DeferredLighting.frag:246-264`）——图二天空开口看着尚可、但中庭墙面几乎不吃到天空 GI，即此分裂所致。

### 2.4 探针密度粗端 + WRC 收益默认未启用（现象 C）

- DDGI 近场场景贴合 **882 探针 @3m**（RTXGI 参考粗端），3-5m 宽室内仅 ~1 探针/间，
  房间内变化被抹平（[WRC-完工] §3 Q1）——"平"的第二成因。
- 反弹传输专项 v2 已把 **WRC 近场密度降到 2m（5175 探针）**并实测改善：
  逃逸射线 34.2%→22.8%、天空主导探针 45%→31%、命中率 56%→65%。
- 但 **`PRIMAL_WRC_ENABLE` 默认 0**（`WRCTypes.h:23-33`；A4 效果验收未结）——
  默认运行（含用户截图的典型配方）**完全没吃到**近场 2m 密度、注入可见性门控、
  远场时间方差 −31% 这些已验证的收益，仍走 DDGI 3m + 冻结边 fallback。
- 量化实锤：GI 入射场（去材质）**P90/P10 仅 2.1×**、corr(indirect, albedo)=0.888——
  间接光几乎处处跟随 albedo 而非入射光分布，即反弹场没有空间结构。

### 2.5 tile radiosity w=2.0 非物理放大器（现象 D 的"室内不够暗"侧）

`PRIMAL_SC_RADIOSITY_W` 默认 2.0：每跳有效增益 w×ρ 达 0.6-1.0（高反照率面逼近 1），
稳态放大 2.5×（物理 1.43×）——**阴影被人为泵住**，压低室内外对比。
双稳态已由 gi_frame 闩锁根治（三次运行 11.8/11.8/11.8，方差 0），但 w=2.0 的非物理增益保留默认。
物理衰减（w=1.0）曾测室内 32.7 vs 2.0 的 15.0/37.3（该 A/B 先于闩锁修复，仅作方向参考）。

### 2.6 直射光向未对齐参考（现象 A/B 的前提条件）

- 测试默认太阳 `{0.5,-0.7,0.3}`（`TestVulkanSponzaRenderGraph.cpp:533`）；
  参考配方 `SPONZA_REFERENCE=1` 用 `{0.12,-0.97,0.22}`（`SponzaReference.h:39`，更接近水平的低仰角）；
  `PRIMAL_LIGHT_DIR` 可覆盖（历史实测 `0.45,-0.8,0.35` 出 Lumen 参考同构光斑结构）。
- 色彩弹射的物理前提是**彩色面被直射照亮**——垂直顶光下旗帜无直射=无反弹色。
  图一 Lumen 的"颜色反射"观感主要由光向驱动（斜射光照亮布幔的直射项 + 随光向分布的反弹）。
- 图二机位（`{0,5,-10}` yaw=π pitch=−0.291）与参考机位（`(-12.5,1.7,-0.7)`）不同构图，
  观感差异部分来自取景而非光照——严格对照必须同机位（`SPGI_CAM_LOCK=1`）。

### 2.7 直射能量本身充足（排除项）

`DIRECT_INTENSITY=50` × `light_color={3,3,3}`（`DeferredLighting.frag:65`、TEST:534），
4 级联 CSM + PCF_16/PCSS（`ShadowMapModule.h:51-52`）。图二地面光斑已过曝白——
直射链不是瓶颈；差距全部在间接/环境与显示链。

---

## 3. 显示链缺口清单（"还有什么缺少的"）

| 能力 | UE（Lumen 参考观感的来源） | 本引擎现状 | 缺口定性 |
|---|---|---|---|
| 自动曝光 / Eye Adaptation | 默认开，场景 EV 自适应 → 室内被大幅压暗、外景保持亮，形成强对比 | **存在但默认关**（`ToneMappingPass.cpp:76`，`PRIMAL_TONE_ADAPT`）；固定曝光 ACES×0.10 | **差距④主源**，开关即有 |
| Bloom | 默认卷积 bloom，天空开口/直射溢出 | **存在但默认关**（`StandardRenderPipeline.cpp:237-244`，`PRIMAL_BLOOM`；threshold 3.0/knee 1.5/合成 4.0） | 差距①/E 的显示侧，开关+调参 |
| 体积雾 / God rays | 高度雾 + 体积散射，中庭光柱与空气感 | Froxel 雾**存在但默认关**（`PipelineQualityConfig.h:126-128`，`FROXEL` env；extinction 0.35/height fog 8）；**god rays 无实现** | 雾是开关；god rays 缺功能 |
| 间接镜面（Lumen Reflections） | 表面缓存卡片 specular trace，屏幕内反射+离屏卡片兜底 | **无对应物**：仅低频 DDGI 球谐 glossy×0.75（`FusionComposite.frag:59-63`）+ SSR **默认关**（`PipelineQualityConfig.h:119`） | 缺功能（P5 级） |
| 天空光 | SkyLight 组件（大气/HDRI 捕获），足量室外环境光 | 程序化梯度常量（zenith≈5.5）× 0.15 档进 GI；IBL diffuse **0** / specular 0.35（`DeferredLighting.frag:73-74`，sunset.hdr） | 档位策略问题（§2.3）+ 无真实大气 |
| 多跳反弹 | SC radiosity 迭代 + final gather 直读卡片 | tile radiosity 递推存在且多跳衰减正确，但查询产出饥饿（§2.1）+ w=2.0 非物理 | 传输修复后可达 |
| 探针密度 | Lumen 屏幕探针逐屏重建 | DDGI 3m 粗端；WRC 2m 已做但默认关（§2.4） | 开关/预算 |

### 3.4 资产基线差异（非引擎问题，但影响观感评判）

图一为 Crytek Sponza 布幔版本（高分辨率 PBR + AO/roughness 贴图），图二为旗帜贴图变体。
布幔的透光感、饱和度和表面细节差异会放大"平"的观感。**观感结论应以同资产对照为准**；
历史上四轮 render-comparisons 均以同资产配方对比，本轮两张用户截图不满足该前提。

---

## 4. 对照方法学（修复验收的统一配方）

任何"改观感"的提交必须先量化再报告（历史纪律），配方：

```bash
# 参考机位 + 参考光向 + 锁定帧 + 抓帧
# [2026-09-20 协议变更] 抓帧 frame360→780：content-ready 注册门禁（600 帧，
# 4 个退化 mesh 永不满足内容检查必走超时）使 frame360 落在 SC 注册之前——
# 旧协议抓到的是"无卡片缓存"形态。
SPONZA_REFERENCE=1 SPGI_CAM_LOCK=1 GI_LADDER_FRAME=780 \
PRIMAL_WRC_ENABLE=1 PRIMAL_WRC_TRACE_STATS=1 \
SPONZA_CAPTURE=/tmp/gap.ppm SPONZA_CAPTURE_FRAME=780 \
  Darwin/DebugVulkan/TestVulkanSponzaRenderGraph 2>&1 | tee /tmp/gap.log
# PPM → PNG 后与 Lumen 参考同机位对比，入 Docs/completed/render-comparisons/<日期>-<主题>/
```

量化指标（沿用既有仪表）：
- **反弹结构**：GI 场去材质 P90/P10（目标 >3×）、lit-vs-shadow 对比比、WRC_SC_FUNNEL found 率（目标 >40%）；
- **色彩弹射**：GILadder ROI 弹射区色度（弹射面 vs 邻面通道比）；
- **室内外对比**：室内 ROI vs 中庭 ROI 显示亮度比（目标向 UE 的 ≥3 EV 靠拢）；
- **回归红线**：室内暗部不得回升（0.15 档成果），DC 增益 amp<0.05 线（GI_DCGAIN）。

---

## 5. 行动建议（按收益/依赖排序）

> **[2026-09-20 行动轮完成]** P0-P6 已全部处理，实施与验收数据见
> `Docs/completed/2026-09-20-sponza-gap-actions-completion.md` 与
> `Docs/completed/render-comparisons/2026-09-20-gap-actions-and-sc-audit/README.md`。
> 下表保留原始计划，括注结果。

| 优先级 | 行动 | 针对现象 | 说明 |
|---|---|---|---|
| **P0** | **capture/query 一致性专项**（近/远平面包含关系、深度有效性掩码、AABB 扩张） | ②③ | ✅ 已实施有效性语义三修复（AtlasInit 幽灵种子 / CardFill miss 假平面 / 查询 UV margin）+ 漏斗预分类对齐；审计发现 queryFail 已被容差 0.9 消除，漏斗诚实化：found 28.0%→24.7%（清除伪测量），hitLum 0.409→0.434，boundsFail 53.2%→50.6%（margin 生效）；剩余 boundsFail = 真实卡片覆盖缺口，继续专项 |
| **P1** | **统一对照配方复拍对比图集**（§4 env 矩阵），产出率与 GI 场指标在 GI 线标准配方下复测（补 [SC-完工] §9 不可测项） | 全部 | ✅ 已复拍入 `render-comparisons/2026-09-20-gap-actions-and-sc-audit/`；**协议变更：抓帧 frame360→780**（content-ready 门禁 600 帧，旧 frame360 落在 SC 注册前） |
| **P2** | **天空能量策略复评**：0.15→0.25/0.4 档阶梯 A/B（WRC 开、注入门控下），验证室内不回弹 | ① | ✅ 已测：0.25 为**有效饱和点**（0.4 逐像素差 0.3/255）；adapt 开启后档位差异被适应曝光吸收（R1≈R2）——**默认保持 0.15**，差异化提亮走探针遮挡/传输而非抬档 |
| **P3** | **显示链默认值翻转**：`PRIMAL_TONE_ADAPT` 开（或翻默认）、`PRIMAL_BLOOM` 开（threshold/强度重调）、`FROXEL` 开（接 CSM 阴影输入已有） | ④①E | ✅ 部分：eye adaptation 默认**开** + 适应 key 默认 **0.55**（key1.0 洗白 2.4×→0.55 恢复 3.8×，阴影有细节）；**bloom 维持默认关**（09-17 用户反馈增益 4.0 洗白受光区，opt-in 不变）；froxel 维持 opt-in（场景相关，不在本轮默认化） |
| **P4** | **`PRIMAL_WRC_ENABLE` 默认翻 1**（A4 三场景基线验收收尾） | ③ | ✅ 已翻 1 + 创建门加 Vulkan 平台守卫（Metal 无 WRC shader，parity 后置）；回退路径验证 ✓（env=0 零创建）；默认路径 3691 帧 0 fault；A4 三场景周环比仍为会话外项 |
| **P5** | tile radiosity w=2.0→物理衰减评估（闩锁已消双稳态，重跑 w=1.0 A/B） | ④ | ✅ 已测：w=1.0 室内 32.6→28.5（−13%）、对比 4.4→5.1×，直射不变；默认翻 **1.0**（回退 env=2.0） |
| **P6** | Lumen Reflections 对应物立项（SC 卡片 specular trace 或 SSR 默认开+卡片兜底） | 整体质感 | ✅ 立项评估已出：`Docs/2026-09-20-lumen-reflections-feasibility.md`（建议先 SSR 默认开+relay 兜底、后 WRC 探针 specular 原型） |

依赖关系：P0 是②的唯一解；P1 是其余各项的度量前提；P2/P3/P4 相互独立可并行；
P5 在 P0/P2 之后做（避免多变量叠加）；P6 独立立项。
**遗留（会话外）**：① boundsFail 50.6% 的卡片覆盖缺口（未卡化几何/掠射带）；
② A4 三场景基线采集（arcade/TestProject_01）；③ 卡片捕获近/远平面包含关系
（near=0 耦合问题的 RHI 层排查，[WRC-完工] §3.1 方向①）。

---

## 6. 证据索引

- SC 漏斗/拒绝分解、探针逃逸与天空占比：[WRC-完工] §3.3/§3.4；仪表 `PRIMAL_WRC_TRACE_STATS=1`、`PRIMAL_WRC_DUMP=1`、`GI_LADDER_DUMP=1`
- 卡片烘焙 v2 与两态消除：[SC-完工] §1/§3/§7/§8
- bootstrap 回路（室内 0.49→0.10、对比度 2.4×→7.6×）：`Engine/Graphics/Lumen/GIWarmup.h:39-56`
- 天空档 0.15 与消费点：`Engine/Graphics/Lumen/GIDebugParams.h:39`、`LumenDDGIPass.cpp:2019`、`ScreenProbeGIPass.cpp:1752`
- 后处理默认值：`ToneMappingPass.cpp:31-76`（曝光/适应）、`StandardRenderPipeline.cpp:237-244`（bloom）、`PipelineQualityConfig.h:101-128`（SSR/雾）、`FusionComposite.frag:59-63`（glossy）
- 直射/光向/机位：`TestVulkanSponzaRenderGraph.cpp:533-541,1066-1078`、`SponzaReference.h:17-54`、`DeferredLighting.frag:65`
- GI 合成式：`FusionIndirect.frag:99-120`（indirect = albedo × gi × gain × 1/π，无 ambient floor）
