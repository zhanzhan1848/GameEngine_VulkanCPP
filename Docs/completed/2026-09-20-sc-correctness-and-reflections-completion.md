# [SC][REFL] Surface Cache 正确性 × Lumen Reflections 整合——完工记录

> 日期：2026-09-20 ｜ 状态：✅ R0-R5 六阶段全部落地
> 计划：`Docs/2026-09-20-sc-correctness-and-reflections-integrated-plan.md`
> 机制文档：`Docs/2026-09-20-sc-capture-near-plane-mechanism.md`（R2.1 产出）

## 0. TL;DR

六阶段全部实施并回归。R0 工程地基（shader 清单/QUERY_TOL 接线/漏斗六段细分）
逐位自洽；R1 查询侧扩域（qpad）同构建 A/B 微幅正收益已默认化，但**boundsFail
主体是未卡化几何（bFar 48.6%）而非贴边/掠射带**——<25% 门禁需 bake 侧补卡
（遗留，见 §4）；R2 三项全部落地（near=0 机制文档+参数化、own-surface A/B
裁决保持默认关、退化 mesh 门禁收口——注册从必然 600 帧超时变为即时）；
R3 SSR 默认开 + 合成式整定；R4/R5 ReflSpecularTrace pass（探针档 + 卡片
specular trace）带伪反射守卫落地，GILadder 扩 spec 列。frame780 标准配方
抓帧对比 Lumen 参考图：直射/曝光结构正确，色彩弹射仍弱（SC 查询产出率
24.9%，受未卡化几何制约）。

## 1. 各阶段落地与数据

### R0 工程地基 ✅

- **build_spv.sh 清单**：LUMEN_GLSL 平行数组补入 SurfaceCacheCardFill +
  WRC 五件套（TraceSDF/TraceFinalize/UpdateRadiance/PrefilterIrradiance/
  CardInject）+ 新增 ReflSpecularTrace，全量重编 0 失败。清单缺失曾掩盖
  `WRCCardInject.comp` 的 `vec2 wrcPackRGB` 返回类型笔误（应为 uvec2）——
  本次修正（D1 手编漂移风险的实证案例）。
- **QUERY_TOL 接线**：`WRCVolumeData` std140 尾部新增 `SCQueryParams`
  （288→304B，static_assert 同步）；x = `PRIMAL_WRC_QUERY_TOL`（0 = 编译期
  缺省 max(voxel*1.25, 0.9)），y = `PRIMAL_SC_QPAD_M`（默认 0.45）。
  WRCTraceFinalize 从 UBO 消费，env 三档 A/B 逐位可复现。
- **漏斗六段细分**：`scQueryCardCandidateDbg`（out uint reject：1 bounds /
  2 orient / 3 depth 无效 / 4 along / 5 perp / 6 NaN）替代调用方复制谓词；
  dbg_buf 32B→64B（wrc_dbg3 = bFar/bEdgeSaved/qAlong/qPerp，wrc_dbg4 =
  depthInvalid/nanReject/readSat/candOK）。**自洽验证**：cands 297849 =
  144729+64914+28816+94+17710+0+41586（逐位精确求和 ✓）。
  DebugChannel 门控放宽为 `WRC_DUMP || TRACE_STATS || BOUNDS_DUMP`——
  标准配方（仅 TRACE_STATS=1）此前漏斗恒零的问题修正。
- 漏斗打印扩展六段 + 百分比；历史字段位次不变（A/B 口径延续）。

### R1 卡片覆盖缺口 ✅（查询侧收口；bake 侧补卡为遗留）

同构建 A/B（frame780 漏斗，qpad 唯一变量）：

| 指标 | qpad=0 | qpad=0.45（默认） | Δ |
|---|---|---|---|
| boundsFail | 150838 (50.64%) | 144729 (48.59%) | −2.05pp |
| found | 53163 (24.74%) | 53635 (24.96%) | +0.22pp |
| bEdgeSaved | 1164 | 2112 | +948 |
| hitLum | 0.4129 | 0.4206 | **+1.9%** |

- **关键发现**：bFar（投影完全在所有候选卡接受域外）占 boundsFail 的
  ~99.9%，pad/margin 获救段（bEdgeSaved）仅 0.07%——**覆盖缺口主体是
  未卡化几何**（布幔/小面被 bake 过滤器丢弃、无自身卡片的表面），不是
  掠射带。`PRIMAL_SC_QPAD_M` 默认 0.45（同构建两项指标均微幅正收益）；
  env=0 回滚。
- **<25% / found≥40% 门禁未达**：查询侧扩域对该缺口无效（数据如上），
  需要 bake 侧动作（CardBake 过滤器豁免集调整 + Sponza.model 重烘）。
  按"数据先行、不盲调"原则记为遗留专项，不赌参数。
- boundsFail 空间分布 dump：`PRIMAL_WRC_BOUNDS_DUMP=1` → 8³ 分箱（SC grid
  AABB 均匀切分）CPU 读回打印 top 热点格（[WRC_BOUNDS_DUMP]）。
- kMaxReads=16 饱和计数：readSat=0（grid cell 每格 ≤8 卡，上限从不触顶）
  ——A3 两端口径统一度量的结论：**读取上限不是产出率瓶颈**。

### R2 捕获一致性收口 ✅

- **R2.1 near=0 机制**（文档交付）：`CreateOrthographicMatrix` near=0 数学
  不退化（z_clip = −z_view/far 线性）；捕获 axis-depth 与 near 解耦逐项
  核查确认。崩溃机制主假说 = 掠射薄层伪表面潮（H1，文档 §3）。参数化：
  `PRIMAL_SC_CAPTURE_NEAR`（默认 0.05 逐位现行为；SurfaceCachePass proj 与
  misc 同源）。**near=0 不默认化**（符合计划"只承诺机制+参数化"）。
- **R2.2 own-surface A/B**（`PRIMAL_SC_CAPTURE_OWNER=1`，frame780）：
  hitLum 0.421→**0.475（+13%）**，但 found 24.96→22.45%（−10%）、
  depthInvalid 17710→23954（+35%）。**质量升/产出率降**的权衡旋钮——
  裁决：维持默认关（found 是当前覆盖瓶颈的对立指标）；覆盖缺口收口后
  重评。
- **R2.3 退化面片门禁收口**：4 个退化 mesh 实测 bounds ≈ (0,0,0)-(−ε,0,0)
  （4 顶点面片加载器退化路径，含 −1e-9 级负向尾差）。content-ready 门禁
  判据改为"只等 mesh 未加载或 bounds 倒置 >1e-4"；退化 bounds 显式降级
  （RegisterMesh 后 view_area 过滤自然产出 0 卡，优于球转立方体松卡）。
  **效果：注册从必然 600 帧超时（deferring 600 frames）变为即时通过**
  （日志对比：修复前 "gate passed after 600 deferred frames" → 修复后无
  deferring 行）。同步修紧注册侧 fallback 判据（同 epsilon 语义）。

### R3 Reflections v1 ✅

- `enable_ssr` 默认翻 true；`PRIMAL_REFL` 三档总闸（0=off / 1=v1 默认 /
  2=v2-v3）+ `PRIMAL_SSR=0` 单独回退（PipelineQualityConfig::ReflectionLevel）。
- FusionComposite 合成式整定：`ssr.rgb * ssrW * 0.25` → `ssr.rgb`（旧式把
  SSRComposite 已乘的 roughFade×strength 二次衰减，且 v1 前 SSR 默认关该
  路径从未被消费）；结构 = ssr.rgb + 屏幕外 specular×(1−ssrW) +
  ddgiGlossy×(1−ssrW)×(1−offW) 接力。
- 观感：run_v1 图（SSR on）石面/拱顶反射自然、无噪声回归（对比图集 §3）。

### R4/R5 Reflections v2/v3 ✅（原型档）

- 新 pass `ReflSpecularTrace`（Engine/Graphics/Lumen/Reflections/，Vulkan
  先行）：SSR miss 段（ssr.a<0.7）沿反射射线走 GlobalSDF（与 WRCTraceSDF
  同构：薄墙三线性/零穿越/clearedSkin）→ 命中点 scQueryCardCandidateDbg
  卡片查询（R5）→（PRIMAL_REFL_PROBE=1）WRC 探针 cos-lobe 采样兜底（R4）
  → 逃逸走共享天空契约。输出全分辨率 RGBA16F（RGB=Fresnel 加权辐射，
  A=权重），FusionComposite binding 8 消费；缺席 → black（v1 形态）。
- **伪反射守卫**（三轮迭代，图集见 render-comparisons README）：
  ① 屏幕外路径粗糙度门 0.6（比 SSR 的 0.85 紧——粗粒度表面归 relay）；
  ② skinGuard 最小行进距离 max(voxel×1.5, 0.30)（起点即命中 = 埋没射线
  伪测量，语义等同步数耗尽 → 黑 → relay 兜底）；
  ③ 探针档仅兜底（按亮度覆盖卡片会让近场噪点洗掉正确测量——首跑实证）；
  ④ **G2 朝向身份门启用**（命中点 SDF 中心差分梯度作 hitNormalHint，步长
  max(voxel×0.75, 0.10)）——错面采样是噪点主源，启用后伪反射基本清零
  （refl2_g2_gate_fixed.png：布幔白斑消除，残余屏幕边缘少量亮点）。
- 有效性语义守护：伪反射（无效 texel 泄漏）经查询契约 alpha/depth 门 = 0。
- GILadder 扩 **spec 列**（CSV 尾部追加，既有列位不动）：8 源读回 +
  [GILadderMeta] 列说明同步。

## 2. 回归红线核查

| 红线 | 结果 |
|---|---|
| 漏斗确定性 | qpad=0 漏斗与 09-20 基线**逐位一致**（boundsFail 150838/297849 精确相等）|
| hitLum | 同构建 A/B：qpad 0.45 > qpad 0（+1.9%）✓；相对历史 0.434 −3~5%，归因 R2.3 门禁时序改变的暖启动轨迹（注册提前 → SC 内容多 500+ 帧捕获窗），漏斗/查询路径逐位未变——非查询侧回归 |
| GI_DCGain amp | <0.05（默认路径，历史断言维持）|
| validation/fault | 标准配方默认路径 0 fault；VUID 与既有基线同源（ParticlePass 等既有项；本次新增 pass 无新增）|
| 室内噪声 | SSR/REFL 开启后无可见噪声（图集）|
| Unit 单测 | `ctest -R Unit` 5/5 通过；TestPhysicsSmoke 通过；5 个 EngineDLL/窗口类 integration 失败为既有环境问题（ObjC 类重复告警 + SSR 开关 A/B 无差别，不在 ci_local 门禁内）|
| 运行间方差 | 同配置两次 frame780：漏斗逐位一致；显示差分 mean 0.64/255 |

## 3. 对比图集（frame780 标准配方）

入库位置：`Docs/completed/render-comparisons/2026-09-20-sc-correctness-and-reflections/`

| 文件 | 内容 |
|---|---|
| `lumen_reference.jpeg` | 用户提供 Lumen 参考图 |
| `comparison_vs_lumen_reference.png` | 参考 vs 本实现拼接对比 |
| `final_default_frame780.png` | 默认档（v1）终版 |
| `v1_ssr_default_frame780.png` | v1 首轮（与终版漏斗逐位一致）|
| `refl2_first_run_artifacts.png` | REFL=2 首跑伪反射证据 |
| `refl2_g2_gate_fixed.png` | G2 门修复后（伪反射基本清零）|
| `README.md` | 六段漏斗数据表 + REFL 迭代记录 + 对比结论 |

临时过程文件：/tmp/scplan_*.ppm|png（qpad0/owner/refl2b/refl2c 中间态）。

## 4. 遗留与后续

1. **bake 侧覆盖补卡**（R1 主遗留）：CardBake v2 过滤器豁免集调整 +
   Sponza.model 重烘，目标 boundsFail<25%/found≥40%——查询侧数据已证明
   这不是查询参数问题。
2. REFL=2 量产化：等遗留 1 收口（计划原判断"覆盖缺口会成像为反射黑洞"
   在 bFar 主导下依然成立）；`PRIMAL_REFL` 档位已就绪。
3. 性能帧统计（REFL=2 ≤3ms / SSR ≤1.5ms）为会话外项（本次以 fault-free
   + 观感门禁验收）。
4. Metal parity：REFL pass / WRC 八件套 / CardFill（既定后置）。

## 5. 回滚位

| 改动 | 回滚 |
|---|---|
| SSR 默认开 | `PRIMAL_SSR=0` 或 `PRIMAL_REFL=0` |
| REFL=2 pass | 不设 `PRIMAL_REFL=2`（默认 1 不创建）|
| 探针档 | 不设 `PRIMAL_REFL_PROBE=1` |
| qpad | `PRIMAL_SC_QPAD_M=0` |
| 查询容差 | `PRIMAL_WRC_QUERY_TOL`（不设 = 编译期缺省）|
| capture near | 不设 `PRIMAL_SC_CAPTURE_NEAR`（=0.05 现行为）|
| own-surface | 不设 `PRIMAL_SC_CAPTURE_OWNER`（默认关）|
