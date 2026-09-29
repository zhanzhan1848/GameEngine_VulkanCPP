# Sponza 观感差距行动轮：SC 审计 + P0-P6 实施完工

> 日期：2026-09-20 ｜ 状态：✅ 完成（A4 三场景周环比与 boundsFail 覆盖缺口为会话外遗留）
> 上游：`Docs/2026-09-20-sponza-vs-lumen-visual-gap-analysis.md`（差距分析，§5 行动清单）
> 图集：`Docs/completed/render-comparisons/2026-09-20-gap-actions-and-sc-audit/README.md`（全部 A/B 量化）
> 关联：`Docs/completed/2026-09-19-world-space-radiance-cache-completion.md`（遗留专项 #1 本轮部分收口）、
> `Docs/completed/2026-09-19-sc-card-lumen-parity-implementation.md`（§9 不可测项本轮补测）

---

## 0. TL;DR

用户要求"先审计 Surface Cache（五问），再完成差距分析全部阶段"。本轮：

- **SC 审计（五问）**：结构与调度健康；**数据正确性三个实锤缺陷**（AtlasInit 幽灵种子、
  CardFill miss 假平面 + 硬编码天空、CardFill 灰面覆盖谓词）——修复（P0）后漏斗诚实化。
- **P0**：有效性语义三修复 + 查询 1-texel UV margin + 漏斗预分类对齐。
- **P1**：统一配方复拍；**发现抓帧协议失效**（frame360 落在 600 帧注册门禁前）→ 改 780。
- **P2**：天空档阶梯 → **0.25 即饱和点**，0.4 无增益；adapt 吸收档位 → 默认保持 0.15。
- **P3**：eye adaptation 默认**开** + 适应 key 默认 **0.55**（bloom 遵循 09-17 用户反馈维持关）。
- **P4**：`PRIMAL_WRC_ENABLE` 默认翻 **1** + Vulkan 平台守卫；回退与默认路径双验证。
- **P5**：tile radiosity w 默认 **2.0→1.0**（物理衰减，阴影泵住解除）。
- **P6**：Lumen Reflections 立项评估（`Docs/2026-09-20-lumen-reflections-feasibility.md`）。

门禁：Unit 5/5 ✅、shader 拷贝门禁 OK ✅、默认路径 3691 帧 0 fault ✅、
回退路径（WRC=0）零创建 ✅、PRIMAL_WRC_TRACE_STATS 配方稳态窗口逐位一致 ✅。

---

## 1. SC 审计（用户五问的结论）

| 问题 | 结论 | 证据 |
|---|---|---|
| ① 是否符合要求 | **结构与调度符合；数据正确性部分不符合**（→P0 修复） | CDB2 389 段/436 卡注册/T8.12 capture 启用/LightEval 436 卡×3.17M texel 每帧/漏斗确定性稳态 |
| ② 是否需要修改结果 | **需要**：查询侧接受伪测量；真捕获遭覆盖 | 三缺陷（§2）；found 中 ~7000/窗口为伪测量 |
| ③ 是否需要增加新的数据 | **不需要新数据源**；需要**有效性语义**（现有通道表达：alpha=有效性、depth 符号=可查性） | 修复零新增纹理/缓冲 |
| ④ 现有可否正常更新 | **可以**：capture 轮转、LightEval 全量、radiosity 每帧，120 帧窗口逐位一致（锁机位确定性） | 审计运行日志 |
| ⑤ 更新的数据是否正确 | **部分不正确→已修复**：AtlasInit depth=0.5/alpha=1 幽灵、CardFill depth=0 假平面+硬编码天空光照、灰面真捕获被谓词覆盖 | `SurfaceCacheAtlasInit.comp/.metal`、`SurfaceCacheCardFill.comp` |

关键机制（修复前）：未捕获 texel 的 depth=0.5/0 落在 0.9 查询容差带内被当合法测量，
以低 key（perp+0.5|along| 小）**赢过真候选**——"均匀灰填充"的直接来源之一；
hitLum 0.409 里混有假能量。

## 2. P0 修复内容（shader 三件 + 一处计数对齐）

1. **AtlasInit 有效性种子**（`SurfaceCacheAtlasInit.comp` + `.metal` 镜像）：
   depth 0.5→**−1**（查询契约 `depth<0` 拒绝）、albedo alpha 1→**0**（LightEval `:454` 门跳过）。
   CardFill 每帧以 alpha=1 回填 SDF 命中 texel，故无"永久黑"回归。
2. **CardFill miss 无效化 + 跳过谓词修正**（`SurfaceCacheCardFill.comp`）：
   miss texel depth=0→**−1**、alpha→0、删除硬编码天空渐变光照写；
   跳过谓词从"非灰且 alpha>0.5"改为**仅 alpha>0.5**（灰色石材真捕获不再每帧被
   纹理中心均色 tint + SDF 梯度覆盖）。
3. **查询 UV margin**（`SurfaceCacheData.glsl`）：新增 `worldToCardUVMargin`
   （GLSL 先声明后使用顺序注意），`scQueryCardCandidate` 用 1 texel 余量钳边采样；
   捕获侧 `projectToCard` 严格边界不变；正确性仍由朝向门 + along/perp 检查把关。
4. **漏斗预分类对齐**（`WRCTraceFinalize.comp`）：bounds 预分类改用同一 margin 变体，
   计数与接受路径一致；顺带修正"PRIMAL_WRC_QUERY_TOL env 可调"的失实注释（实为编译期常量，env 未接线）。

### P0 前后（GI 线标准配方稳态窗口）

| 指标 | 前 | 后 |
|---|---|---|
| found/hits | 60101/214871（28.0%） | 53110/214871（24.7%）——伪测量清除 |
| boundsFail/cands | 53.2% | **50.6%**（margin 生效） |
| hitLum（near） | 0.409 | **0.434** |
| 探针 meanLum | 0.358 | **0.376** |
| queryFail | 0（容差 0.9 已消除） | 0 |

**found>40% 目标的修正**：该目标制定时把伪测量计入产出。诚实漏斗下剩余
boundsFail 50.6% 是真实的卡片覆盖缺口（未卡化几何/掠射带）——继续专项
（[WRC-完工] §3.1 方向③ AABB 扩张 + 近/远平面包含关系仍是主攻方向）。

## 3. 默认值翻转（全部带 A/B 数据与 env 回退）

| 项 | 旧→新 | 数据 | 回退 |
|---|---|---|---|
| eye adaptation（`ToneMappingPass.cpp`） | 关→**开** | 固定曝光把室内外压同档（差距④主单项） | `PRIMAL_TONE_ADAPT=0` |
| 适应 key（同文件） | 1.0→**0.55** | key1.0 洗白（lit/shadow 2.4×）；0.55 恢复 3.8× 且阴影 43.7/255 非死黑 | `PRIMAL_TONE_KEY=1.0` |
| WRC 总闸（`WRCTypes.h`） | 0→**1** | 近场 2m/注入门控/远场方差 −31% 默认生效；创建门加 Vulkan 平台守卫（Metal 无 WRC.metal 八件套） | `PRIMAL_WRC_ENABLE=0` |
| SC radiosity w（`SurfaceCachePass.cpp`） | 2.0→**1.0** | w=2.0 稳态放大 2.5×（物理 1.43×）泵住阴影；w=1.0 室内 −13%、对比 4.4→5.1× | `PRIMAL_SC_RADIOSITY_W=2.0` |
| 天空档 | 0.15（**不变**） | 0.25 即饱和（0.4 逐像素差 0.3/255）；adapt 吸收档位差异 | — |
| bloom | 关（**不变**） | 09-17 用户反馈增益 4.0 洗白受光区——维持 opt-in | — |

## 4. 协议变更（影响所有后续 GI A/B）

- **抓帧 frame360→780**：content-ready 注册门禁（600 帧）使旧协议抓到"无卡片缓存"形态。
- 旧"found 14-20%"表述更新为：诚实漏斗 24.7%（伪测量清除后）；>40% 目标待覆盖缺口专项后再评。

## 5. 门禁与验证

| 门禁 | 结果 |
|---|---|
| Vulkan Debug 树编译（三轮增量） | ✅ 0 error |
| Metal 树编译 | ⚠️ 未重跑（本工作区无 Metal-only 构建目录；本轮 C++ 改动均为平台中立代码或带 `rhi::RHIPlatform::Vulkan` 守卫，该文件原有同类用法先行存在；Metal shader 未触碰） |
| Unit 单测 | ✅ 5/5 |
| shader 同语言拷贝门禁 | ✅ OK（0 对待更新） |
| 默认路径（无 env，全新默认） | ✅ 3691 帧 0 fault/validation；WRC 2 volumes 默认创建、card-inject 436 卡 |
| 回退路径（`PRIMAL_WRC_ENABLE=0`） | ✅ WRC 零创建、无 fault |
| 漏斗稳态 | ✅ 120 帧窗口逐位一致（锁机位确定性） |
| spv 手动编译入口 | ✅ CardFill/WRCTraceFinalize 不在 build_spv.sh 清单，已按同 pattern 手编 + spirv-val + 入口名核验（**遗留：build_spv.sh 未含 CardFill/WRC 清单，后续补**） |

## 6. 遗留（会话外）

1. boundsFail 50.6% 卡片覆盖缺口专项（AABB 扩张 + 捕获近/远平面包含关系；
   near=0 与深度编码耦合的 RHI 层排查）。
2. A4 三场景基线采集（Sponza 之外的 arcade/TestProject_01）+ 7 天故障计数周环比。
3. build_spv.sh 补 CardFill/WRC shader 清单（防手编漂移）。
4. `PRIMAL_WRC_QUERY_TOL` env 真正接线（当前仅编译期常量，注释已纠正）。

## 7. 回滚

| 项 | 回退 |
|---|---|
| eye adaptation | `PRIMAL_TONE_ADAPT=0` |
| 适应 key | `PRIMAL_TONE_KEY=1.0` |
| WRC 默认 | `PRIMAL_WRC_ENABLE=0` |
| radiosity w | `PRIMAL_SC_RADIOSITY_W=2.0` |
| P0 shader 修复 | revert 三 shader + 重编 spv（`build_spv.sh` + 手编两件） |
