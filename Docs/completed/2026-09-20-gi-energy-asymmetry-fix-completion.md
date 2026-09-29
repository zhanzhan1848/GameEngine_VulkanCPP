# [GI] 能量不对称修复 F0-F6 完工报告

> 标签：**[GI] [SC] [SPGI] [DDGI] [WRC]** ｜ 日期：2026-09-20 ｜ 状态：✅ 落地（F4 读取链一处遗留在案）
> 计划：`Docs/2026-09-20-gi-energy-asymmetry-fix-plan.md`（状态已同步为落地）
> 对比图：`Docs/completed/render-comparisons/2026-09-20-gi-energy-asymmetry-fix/`
> 验证：统一配方（SPONZA_REFERENCE=1 SPGI_CAM_LOCK=1，抓帧 780），单测 5/5 通过，
> 全部 12 次配方跑退出码 0。

---

## 落地清单

| 项 | 状态 | 实现要点 |
|---|---|---|
| **F0** SC tile 天空档接线 | ✅ | `IndirectResolveParamsGPU` 末尾追加 `tile_sky_color`（92→112B，`static_assert` 同步）；CPU 填 `GIDebug().sky_color_intensity × (2.75,2.88,3.00)`；shader 删硬编码 `*0.4`（化石注释一并删除）。回滚：`PRIMAL_SC_TILE_SKY_OVERRIDE="1.1,1.152,1.2"`。Metal 孪生顺带补齐既有 ABI 漂移（recon/card_count/page_table 三字段） |
| **F1-a** 失败语义限幅 fallback | ✅ | SPGI `ScreenProbeTraceFinalize.comp` 两处 `!ddgiOk` 天空门：下行射线 → RadianceCascade L0 低频回退，`min(级联值, 天空×k)`；WRC `WRCTraceFinalize.comp` 查询失败 → 周边卡片双线性借用 + 同式限幅。契约保持：不编造卡片辐射，只做真实测量的低频近似。k 默认 0.3，`PRIMAL_GI_FALLBACK_CLAMP` / `PRIMAL_WRC_FALLBACK_CLAMP=0` 回黑语义 |
| **F2** SC 命中惩罚对称性 | ✅ 默认翻转 | 三跑数据：SC 分支 meanLum 1.212（无惩罚，mode2）→ 0.182（惩罚，mode3），max 47.6→0.98，与 HZB 惩罚后 0.141 对称；全桶均匀 ~5-10× 压制（无选择性）。`diag_params.w` 作回滚位：`PRIMAL_SPGI_SC_NOPENALTY=1` |
| **F3-V1** SC 三 atlas 视图 | ✅ | `PassDebugView` 枚举 +3（sc_albedo/sc_normal/sc_depth）+ 名字表 + `ResolvePassDebugViewSource` 接线（accessor 已有）+ frag/Metal 孪生解码（normal 八面体、depth 0..16m 灰度）。**注意：`PassDebugView.frag.spv` 不在 build_spv.sh，需手动 glslangValidator** |
| **F3-V2** spgi_raw 视图 | ✅ | 枚举 +1 绑 `GetOutputTexture()`（bypass denoise）。**显示映射二次修正**：gather 输出是 HDR irradiance（sky 2.0 档下露天 6-12），ACES 直读全白（既有 filtered spgi 视图同病）——13/18 两视图改为乘显示曝光再 tonemap，默认 0.05，`PRIMAL_PASS_DEBUG_GI_SCALE` 可调。raw 视图的黑洞 = gather 无覆盖（无 GBuffer 表面/weightSum=0），正是 denoise 归因要看的 |
| **F3-V3** 逐射线 buffer getter | ✅ | `GetRayRadianceBuffer(frameIdx)/GetHitDistanceBuffer()`（DDGI 先例）；测试壳 `SPONZA_SPGI_RAY_DUMP=<frame>` 输出 16bin 直方图 + `/tmp/spgi_rays.bin`（float4 线性 HDR） |
| **F3-V4** 死旋钮处置 | ✅ 方案(a) | `SurfaceCachePass` capture/CardFill/LightEval/shadow 四处 dispatch 前接 `GIDebug()` 门控（false=跳过 dispatch，atlas 保持上帧）；`GI_ALL_OFF` 的 direct-only A/B 语义自此生效；env 文档死旋钮清单已更新 |
| **F4** 逐卡 boundsFail 计数 | ⚠️ 基建落地 | `WRCTraceFinalize` binding 12 `card_fail[4096]`（reject==1 原子累加）+ 布局/清零/`[WRC_CARD_FAIL_DUMP]` top-8 读回全部就绪；**当前输出全零**——与既有 `[WRC_BOUNDS_DUMP]` 分箱在同配方下同为空，疑 GPU 原子写与每帧 CPU memset 清零竞态（既有问题同象限，非本次回归）。后续：GPU 侧清零/专用 copy-clear pass |
| **F5** 手工卡片 override | ✅ | `PRIMAL_SC_CARD_OVERRIDE="mesh_id:x0,y0,z0,x1,y1,z1[;...]"`（世界系 AABB，6 向发卡，优先级高于离线 bake）；空表=原行为；非法条目告警忽略。验证：解析/告警/注册链路 5 帧冒烟通过 |
| **F6** π 注释冲突归档 + 复验 | ✅ | `DDGIGIGather.comp` "NEVER ×π" 化石注释修订（前提=已取代的 CPU /π 平衡方案）；`SurfaceCacheLightEval` 留两腿同步回滚警示。`GI_DCGAIN=1` 复验：amp=0.088（<0.05 断言线的已知偏差入档——观感轮 sky 2.0 档即 0.08，本次 +0.008），dc_gain 0.96<1 有界，无发散 |

---

## A/B 量化（before = 修复前基线，用 F0 回滚 env 复现；after = 全修复）

截图对比：`AllFix_compare.jpg`（参考图 | 修复前 | 修复后）。

### GI 台账（frame 780，target=中央地面 ROI，reference=右立面参考带）

| 指标 | before | after | 变化 |
|---|---|---|---|
| target ddgi 腿 | 1.675 | 2.586 | **+54%**（F0 tile 天空→SC→探针反弹链） |
| target spgi 腿 | 9.234 | 2.814 | **−70%**（F2 惩罚压 SC 泵入，对称化） |
| target 融合间接 | 0.338 | 0.426 | +26% |
| reference ddgi 腿 | 3.871 | 4.649 | +20% |
| reference spgi 腿 | 5.359 | 5.354 | 持平（惩罚不伤远场反弹带 ✓） |
| reference 融合间接 | 0.326 | 0.356 | +9% |

### 屏幕 ROI 亮度（8-bit，固定曝光）

| ROI | before | after | 变化 | 判读 |
|---|---|---|---|---|
| 中庭地面 | 89.2 | 99.0 | **+11%** | 中庭反弹变亮（验收 ✓） |
| 右柱背光面 | 84.7 | 82.7 | −2% | 持平 |
| 左内廊暗部 | 5.2 | 5.4 | +3% | **红线：不回升**（噪声级，F1 限幅封顶 ~0.3 天空） |
| 拱顶暗面 | 139.1 | 137.0 | −1.5% | 持平 |

### SPGI 分支统计（25 帧窗 ×15，frame 24..350）

| 分支 | mode2（无惩罚对照） | mode3（SC 同惩罚） |
|---|---|---|
| HZB meanLum | 0.646（去惩罚） | 0.141 |
| SC meanLum | **1.212** | **0.182** |
| SC maxLum | 47.6 | 0.98 |

### 回归红线核对

- 室内暗部 ROI 不回升 ✓（+3%，8-bit 噪声级）
- `GI_DCGAIN` amp=0.088 有界（历史已知偏差 0.08 之上 +0.008，dc_gain<1 无发散）⚠️ 入档
- `[WRC_SC_FUNNEL]` found/hitLum 不回退 ✓（修复前后逐字节同值：found=53659 等）

---

## 追加（同日第二轮）：SC 存储全可视化

用户需求"SC 所有纹理可看"——补齐剩余 5 个视图（枚举 19-23，名字表/管线/frag+Metal 孪生同步）：

| 视图 | 数据源 | 显示 | 截图验证 |
|---|---|---|---|
| `sc_emissive` | emissive_atlas（RGBA16F） | ×GI 曝光→ACES | Sponza 无自发光→全黑（正确） |
| `sc_shadow` | shadow_visibility **buffer**（binding 6 新增，恒绑哑缓冲兜底） | 灰度 白=lit | ✓ 卡片/柱影结构清晰 |
| `sc_indirect` | indirect_out_tex[3]（radiosity 逐 texel） | ×GI 曝光×16→ACES | ✓ 窗光/布幔结构 |
| `sc_tile` | indirect_tile_tex[3]（tile 入射+validity） | 同上 | ✓ tile 域低频图 |
| `sc_tile_aux` | indirect_tile_aux（rg=oct, b=深度, a=卡 id） | R/G 编码 + B 深度 0..16m | ✓ |

配套改动：`PassDebugViewInputs` +aux_buffer/aux_count；模块布局 binding 6（StorageBuffer）；
`misc2.w`=atlas_size（buffer 索引）；radiosity 两视图额外 ×16（量级比探针 irradiance 低
~两个量级）。CLI 帮助名单已同步，schema 快照重生成（Cli 57/57）。
SPGI filtered/raw 直读 ACES 全白的显示缺陷同轮修正（见 F3-V2 行）。

## 过程发现（重要，已复核）

1. **content-ready 门竞态（双稳态家族新成员）**：`SPGI_TRACE_DIAG=1` 的四次跑
   （mode1/mode1b/AllFix/probe）全部出现 `gate block: inst=336 geo=336
   bounds=(0, 4.4e12, 0)-(7.1e28, 0, 0)` ——异步加载中的 mesh 给出垃圾 bounds，
   content-ready 门把它当"内容未就绪"，注册延迟 600 帧超时；同期 SC-at-SPGI/探针
   SC 分支/DDGI 腿全灭（SC n=0、ddgi=0）。不带该 env 的跑内容先到、无此现象。
   根因不是 diag 本身，是 **env 解析等微小初始化扰动翻转了内容加载与门检查的竞速**——
   与 [sc-probe 双稳态]（gi_frame 闩锁修复）同族。后续：门判据应剔除
   非 finite/超巨 bounds 的实例（inst=336 的 mesh 在 820 帧内 bounds 一直是垃圾，
   值得单独查该资产）。
2. **`PassDebugView.frag.spv` 不在 build_spv.sh 编译清单**：检出的 spv 是手工编译的，
   本次改 frag 后旧 spv 静默生效（三视图全走旧 passthrough 分支：sc_depth 显示纯红
   即 R-only passthrough 的特征）。已手动重编并验证。建议把 Debug/ 纳入 build_spv.sh。
3. **AllFix 首跑作废原因**：即发现 1——该跑的 frame 780 抓图是 SC 死亡态，不可作
   A/B；AllFix2（无 diag env）健康后重拍。

## 回滚位汇总

| 项 | env |
|---|---|
| F0 | `PRIMAL_SC_TILE_SKY_OVERRIDE="1.1,1.152,1.2"` |
| F1 SPGI | `PRIMAL_GI_FALLBACK_CLAMP=0` |
| F1 WRC | `PRIMAL_WRC_FALLBACK_CLAMP=0` |
| F2 | `PRIMAL_SPGI_SC_NOPENALTY=1` |
| F5 | 表空即原行为 |
| F3 | 纯增量，无需回滚 |

## 后续（不在本计划内）

- F4 读取链竞态（GPU 侧清零）+ inst=336 垃圾 bounds 资产排查 + content-ready 门判据加固；
- Debug/ 目录 shader 纳入 build_spv.sh（防 spv 漂移）；
- F2 惩罚为全桶均匀压制，若需"只压直射泵入"可探索亮度分段惩罚（当前保持与 HZB 同式）；
- 新旋钮已登记 `Docs/environment-variables.md` §1/§2/§6。
