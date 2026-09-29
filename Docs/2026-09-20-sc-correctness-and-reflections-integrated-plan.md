# Surface Cache 正确性 × Lumen Reflections 整合：缺陷清单与分阶段实施计划

> 日期：2026-09-20 ｜ 状态：✅ 全部阶段实施完成（R0-R5；完工记录
> `Docs/completed/2026-09-20-sc-correctness-and-reflections-completion.md`）
> 标签：**[SC]** **[REFL]** ｜ 关联：**[WRC]** [SSR]
> 上游：
> - `Docs/completed/2026-09-20-sponza-gap-actions-completion.md`（SC 审计五问 + P0 有效性修复，已完成基线）
> - `Docs/2026-09-20-sponza-vs-lumen-visual-gap-analysis.md`（观感差距分析）
> - `Docs/2026-09-20-lumen-reflections-feasibility.md`（P6 立项评估，本文档吸收其选项 A/B/C）
> - `Docs/completed/render-comparisons/2026-09-20-gap-actions-and-sc-audit/README.md`（A/B 数据）

---

## 0. TL;DR

GI 反弹（色彩弹射）与 Lumen Reflections **消费同一张卡片真值层**
（`scQueryCardCandidate` / 卡片 atlas / 网格加速）。09-20 SC 审计已修复有效性语义
（伪测量清除），但 **boundsFail 50.6% 的卡片覆盖缺口**同时卡住两条链：漫反射 GI
拿不到反弹，反射拿不到屏幕外辐射源。因此本计划把 SC 正确性收口与 Reflections
实施编排为一条依赖链：

```
R0 工程地基（build_spv 清单/QUERY_TOL 接线/漏斗细分）
  → R1 卡片覆盖缺口专项（boundsFail 50.6% → <25%，诚实 found ≥40%）
    → R2 捕获一致性收口（own-surface / 近远平面 / CardFill 近似质量）
      → R3 Reflections v1：SSR 默认开 + DDGI relay 兜底（不依赖 R1/R2，可并行）
        → R4 Reflections v2 原型：WRC 探针 specular lobe 采样
          → R5 Reflections v3：SC 卡片 specular trace 全量（强依赖 R1/R2）
```

**核心论点**：R3 可以先行（纯默认值/接线，立即改善屏幕内反射）；但 R5 的
"卡片 specular trace"若在覆盖缺口未收口前实施，**缺失的卡片会直接成像为反射黑洞**
——这是把 R1/R2 编排在 R5 之前的原因。

---

## 1. 已完成基线（2026-09-20，本计划的起点）

SC 审计五问结论与 P0 修复已全部落地（详见完工记录，此处只列与后续阶段相关的状态）：

| 已修复 | 位置 | 效果 |
|---|---|---|
| AtlasInit 幽灵种子（depth 0.5/alpha1 → −1/0） | `SurfaceCacheAtlasInit.comp` + `.metal` | 查询不再接受 0.9 容差带内的假表面 |
| CardFill miss 假平面 + 硬编码天空光照 → 无效化 | `SurfaceCacheCardFill.comp` | 伪能量源清除（depth=0 假平面曾赢过真候选） |
| CardFill 跳过谓词 is_gray → 仅 alpha | 同上 | 灰色石材真捕获不再每帧被 tint 近似覆盖 |
| 查询 1-texel UV margin（`worldToCardUVMargin`） | `SurfaceCacheData.glsl` | boundsFail 53.2%→50.6%，贴边合法候选获救 |
| 漏斗预分类与查询路径对齐 | `WRCTraceFinalize.comp` | bounds 计数与接受路径一致 |
| 有效性语义确立 | 全链 | **albedo.alpha = 有效性（0 无效）；depth 符号 = 可查性（<0 拒绝）**——后续一切消费方遵循 |

诚实漏斗现状（GI 线标准配方，frame780）：`found 24.7% | boundsFail 50.6% | orientFail 20.8% | queryFail 0 | hitLum 0.434`。

---

## 2. 遗留缺陷与专项总清单（按消费方分组）

### A. 传输链（GI / 色弹消费）

| # | 缺陷/专项 | 现状与证据 | 位置 |
|---|---|---|---|
| A1 | **卡片覆盖缺口**（boundsFail 50.6%）：未卡化几何、掠射带、SDF 命中带横向 smear 超 1 texel | 漏斗 boundsFail 158462→150838/297849；09-19 曾实测 33% 命中在卡片网格外（v2 重烘后未复测该口径） | 候选卡 AABB（bake 侧 extent + 运行时 fallback AABB） |
| A2 | orientFail 20.8%（双面墙/邻面朝向） | 57629→61814/297849（margin 让更多候选到达朝向门） | `scQueryCardCandidate` G2 门 + bake 方向位 |
| A3 | trace 扫描路径 lookup 上限：Metal `kMaxLookups=128`（threadgroup 数组）< Sponza 393 lookup，超出部分仅 grid 路径覆盖 | `DDGITraceRays.metal:200-206`；Vulkan 侧为每查询 `kMaxReads=16` 候选读上限（`WRCTraceFinalize.comp:156`） | 两端口径不一致，需统一度量后再动 |

### B. 捕获链（卡片内容质量）

| # | 缺陷/专项 | 现状与证据 | 位置 |
|---|---|---|---|
| B1 | **捕获近/远平面与卡片自身表面的包含关系**：贴面卡理论上全表面在 near=0.05 之前，但 4 个退化 mesh fallback 卡与薄几何残余 among queryFail 机制；near=0 实验曾致探针场崩溃（0.42→0.03，机制未明） | `CardCapture.vert` UBO `misc.x/y = depth_near/far`（CPU 供给）；疑点在 `CreateOrthographicMatrix` near=0 退化 | `GPUDrivenDrawPipeline.cpp` 捕获视图构建 |
| B2 | own-surface capture 实验 `PRIMAL_SC_CAPTURE_OWNER`（默认关）：对抗"近邻遮挡物比贴面卡更近"残余 | 已实现未 A/B；以漏斗裁决 | `ExecuteCardCapturePass` |
| B3 | CardFill SDF 命中 texel 的近似质量：albedo=纹理**中心点**采样（整 mesh 一色）、normal=SDF 梯度（粗）、depth=SDF hit_dist（体素级误差） | 卡视角未覆盖区域的兜底数据；影响屏幕外反射的成像正确性（R5 前置） | `SurfaceCacheCardFill.comp` `getMeshTint` |
| B4 | 4 个 4 顶点退化面片无 SDF 壳 → AABB fallback 卡 | [SC-完工] §7；content-ready 门禁因此必走 600 帧超时 | bake 侧壳生成 |

### C. 反射链（新增需求，来自 P6 立项）

| # | 缺口 | 现状 | 位置 |
|---|---|---|---|
| C1 | 间接 specular 仅低频 DDGI 球谐 relay（×0.75） | `FusionComposite.frag:59-63`；无形状正确性 | `GIGatherModule.cpp:165-168` |
| C2 | SSR 已实现但默认关，屏幕外无兜底接力语义未默认化 | `PipelineQualityConfig.h:119`；ssrW 接力公式已存在 | `FusionComposite.frag:51-55` |
| C3 | 无卡片 specular trace / 探针 specular 采样（Lumen Reflections 对应物） | P6 立项评估选项 A/C | 新 pass |

### D. 工程链

| # | 项 | 现状 |
|---|---|---|
| D1 | `build_spv.sh` 缺 SurfaceCacheCardFill / WRC 系（WRCTrace*/WRCUpdate*/WRCPrefilter/WRCCardInject）清单——本轮手编（inline_includes + `--source-entrypoint <entry> -e <entry>` + 手动 copy_directory） | 手编漂移风险 |
| D2 | `PRIMAL_WRC_QUERY_TOL` env 未接线（编译期常量 `max(voxel*1.25, 0.9)`，注释已纠正） | 需 CPU uniform 通道或维持常量 |
| D3 | Metal parity：CardFill 无 .metal（M5）、WRC 八件套、C1 fusion mode | 既定后置决策 |

---

## 3. 分阶段实施计划

### R0 —— 工程地基（0.5 天，无观感风险）

| 项 | 内容 | 门禁 |
|---|---|---|
| build_spv.sh 补清单 | CardFill + WRC 五件套按 LUMEN_GLSL 块模式入清单（实名入口分支） | 全量重编后 spirv-val + 入口名核验；shader 拷贝门禁 |
| QUERY_TOL 接线 | 经 `WRCVolumeData` 空闲字段下发（std140 尾部，静态断言护尺寸）；缺省 = 现常量 | env 0.45/0.9/1.5 三档漏斗 A/B 逐位可复现 |
| 漏斗细分补全 | queryFail 拆 along/perp 两计数器（现合一个）；boundsFail 拆"卡外/贴边"两段 | 计数器自洽：cands = bounds + orient + query 接受 + 拒绝 |

**验收**：零行为变化（spv 字节级等价或 env 缺省逐位）；仪表可信度提升。

### R1 —— 卡片覆盖缺口专项（A1，主战场，2-3 天）

目标：把诚实漏斗从 `found 24.7% / boundsFail 50.6%` 推进到 **found ≥40% / boundsFail <25%**。

1. **候选卡 AABB 扩张**：查询侧把卡片 UV 接受域从几何 extent 扩一个 SDF 命中带
   （±0.45m 投影），与 1-texel margin 互补（margin 管贴边 texel，扩张管整段掠射带）。
   实现：`SurfaceCacheCard` 增加 query-side extent pad（或 `worldToCardUVMargin` 传入
   米制 pad），**捕获侧 extent 不动**（避免摊薄 texel 密度）。
2. **bake v2 覆盖补测**：对 boundsFail 拒绝位置做空间分布 dump（新 dbg 通道），
   区分"未卡化几何"（bake 补卡/调整 5% 过滤器豁免集）vs"掠射带"（第 1 项解决）。
3. **fallback AABB 质量**：4 个退化 mesh + 无 bake 实例的球转立方体 AABB 收紧。
4. （评估项）`kMaxReads=16` 每查询候选读上限的饱和度统计。

| 门禁 | 指标 |
|---|---|
| 漏斗 | boundsFail <25%、诚实 found ≥40%、queryFail 保持 0 |
| 能量 | hitLum 不降（≥0.43）；GI 场去材质 P90/P10 从 2.1× → ≥3× |
| 观感 | 色彩弹射 ROI：直射旗帜→邻面通道比可测提升；室内不回弹（tier0.15 锚点） |
| 回归 | 默认路径 0 fault；室内 ROI 不升（防伪测量回潮） |

**回滚**：全部改动 env 门控（`PRIMAL_SC_QPAD_M`、dump 通道），逐项可关。

### R2 —— 捕获一致性收口（B1/B2/B4，与 R1 可并行，2 天）

1. **near=0 崩溃机制 RHI 排查**（历史悬案）：`CreateOrthographicMatrix` near=0 的
   深度映射退化验证（NDC z 映射、reversed-Z 假设）→ 决定捕获近平面能否安全下调；
   产出：机制文档 + 卡片近平面参数化方案。
2. **own-surface capture A/B**：`PRIMAL_SC_CAPTURE_OWNER=1` × 漏斗四段 + 暗拱 G4
   分层，裁决是否默认化。
3. **退化面片 bake 补壳**（B4）：4 mesh 的壳生成退化路径修复或显式降级标记，
   解除 content-ready 门禁 600 帧超时的必然触发（预期：注册提前 ~500 帧）。

| 门禁 | 指标 |
|---|---|
| 捕获 | 卡片 texel 与所属表面 along 误差分布 P95 < 0.2m（新 dbg 直方图） |
| 注册 | content-ready 门禁不再走超时（4 退化 mesh 修复后）；首注册帧数日志 |
| 漏斗 | orientFail 占比下降；暗拱 G4 欠亮改善 |

### R3 —— Reflections v1：SSR 默认开 + relay 兜底（C2，可立即开工，1 天）

不依赖 R1/R2。`enable_ssr` 默认 true（env 回退），`FusionComposite.frag:51-55` 的
ssrW 粗糙度接力参数整定（ssr 强度 0.7 / max_roughness 0.85 复核），
DDGI glossy relay 兜底权重随 adapt/key 新显示基调重标。

| 门禁 | 指标 |
|---|---|
| 观感 | 光滑石材（roughness<0.3）屏幕内反射可见；frame 700-900 三帧差分 <2/255 |
| 性能 | 1080p ≤1.5ms（SSRPass 已有 HZB trace，测增量） |
| 回归 | 室内阴影不引入噪声（时间累积参数） |

### R4 —— Reflections v2 原型：WRC 探针 specular 采样（C3 选项 C，2 天）

反射向量 → WRC 近场探针（K=64 Fibonacci 方向格）cos-lobe 加权插值——比球谐 relay
高频，复用 WRC 基建（探针场 5175 探针 @2m 默认在役）。`PRIMAL_REFL_PROBE=1` 门控原型，
与 ssrW 接力合成。

| 门禁 | 指标 |
|---|---|
| 正确性 | 屏幕外反射至少明暗结构对（参考图 A/B）；视线相关闪烁 <3/255（时间滤波后） |
| 性能 | ≤3ms 增量 |
| 裁决 | 与 R5 对比后二选一量产化 |

### R5 —— Reflections v3：SC 卡片 specular trace 全量（C3 选项 A，3-5 天，强依赖 R1/R2）

反射未命中屏幕 → 沿射线走与 WRC finalize 同构的卡片查询（复用
`scQueryCardCandidate` + 网格加速 + R0 漏斗细分）；specular lobe 窄 →
边界/深度误差直接成像，**这就是必须排在 R1/R2 之后的原因**。

| 门禁 | 指标 |
|---|---|
| 正确性 | 形状近似正确（屏幕外反射体轮廓可辨）；伪反射（无效 texel 泄漏）= 0（有效性语义守护） |
| 性能 | ≤3ms |
| 总验收 | `PRIMAL_REFL` 三档（off/v1/v2-v3）对比图集 + GILadder 扩展 spec 列 |

**统一验收配方**：§4 之 frame780 协议 + 反射专项三帧差分 + 性能帧统计；
所有阶段每步抓帧入 `Docs/completed/render-comparisons/`（README 数据入库）。

---

## 4. 统一验收配方（全部阶段共用）

```bash
SPONZA_REFERENCE=1 PRIMAL_LIGHT_DIR="0.45,-0.8,0.35" SPGI_CAM_LOCK=1 \
PRIMAL_WRC_TRACE_STATS=1 \
SPONZA_CAPTURE=/tmp/x.ppm SPONZA_CAPTURE_FRAME=780 \
  ./Darwin/Debug/TestVulkanSponzaRenderGraph 2>&1 | tee /tmp/x.log
```

- frame780 = content-ready 门禁 600 帧 + 暖启动收敛（frame360 协议已失效）。
- 量化口径：漏斗四段（R0 后六段）、hitLum/meanLum、GI 场去材质 P90/P10、
  lit/shadow ROI 比、色彩弹射通道比、反射三帧差分、性能 ms。
- 回归红线：室内 ROI 不回升（防伪测量/天空泄漏回潮）、DC 增益 amp<0.05、
  默认路径 0 validation/fault。

---

## 5. 风险与依赖

| 风险 | 缓解 |
|---|---|
| R1 AABB 扩张引入跨面误采样 | margin/pad 全部小步 + 漏斗逐段计数 + orient/along/perp 门保持；env 逐项可回退 |
| near=0 排查再次证伪 | R2 只承诺机制文档与参数化方案，不承诺 near=0 落地；贴面卡已保证所属表面在 near 前 |
| R4/R5 视线闪烁 | 时间滤波先行（复用 SSR 累积器）；原型档位 `PRIMAL_REFL_PROBE` 隔离 |
| Metal parity | 全程 Vulkan 领衔；R3 的 SSR Metal 状态在 parity matrix 复核后决定是否跟进 |
| 显示基调耦合 | R3 relay 重标以 adapt on + key0.55 新基调为准（旧标定作废） |

## 6. 参考

- 完工基线：`Docs/completed/2026-09-20-sponza-gap-actions-completion.md`
- A/B 数据：`Docs/completed/render-comparisons/2026-09-20-gap-actions-and-sc-audit/README.md`
- P0 前置：`Docs/completed/2026-09-19-world-space-radiance-cache-completion.md`（§3.1 capture/query 一致性方向）
- 反射调研：`Docs/2026-09-20-lumen-reflections-feasibility.md`（选项 A/B/C 原文）
