# Sponza 光照物理分布恢复计划（Vulkan 直射定罪 + GI 重标，2026-09-09）

**目标（用户物理验收标准）**：Sponza 半开放建筑在锁定机位下呈现正确光域——
①中庭镂空区直射亮（光池到裁切白档）；②贴近直射的墙出现太阳侧弹射梯度；
③建筑内部因遮挡与衰减渐暗。量化锚点沿用 `scripts/sponza_region_lum.py`：
**sunlit_wall / 内部 比 ≥ 8**（UE 参考 13.5×，当前 Vulkan ≈1.5×）。

**与 C-series 的关系**：本计划是 `Docs/2026-09-09-gi-physical-relation-calibration-plan.md`
（M6 台账）的 **Vulkan 侧落地分册**——V1 即 C1.2 的 Vulkan 定罪收尾，
V2 为 C2/C3 提供前置测量，V3/C4、V4/C5 一一对应，不重复立项。
C-series 的原则条款（先正确性后标定 / 每步量化验收带 / 双平台同参 /
直射修复后旧 A/B 全作废 / 构建树协议）全文适用。

---

## 0. 现状诊断（2026-09-09 晚 session 实测，工具已入库）

三层亏空，叠乘关系：

| # | 病灶 | 证据 | 归属 |
|---|---|---|---|
| (a) | **被照亮表面 vis 仅 0.4~0.7** → 直射压暗 ~2-3× | `PRIMAL_SHADOW_DBG_DEPTH/RECV/HARD` 哨兵同点采样：接收器 sc0.z 比阴影图深 0.000~0.036（(600,220)+0.036、(640,260)+0.008、(700,250)0.000）。硬比较在差值>0.003 的点全部判影 | **V1 定罪** |
| (b) | **间接光 ~9× 亏空** | 走廊地面 15.8 vs Metal 195.6 / UE 59.5；构成 ≈ SC 卡片辐射差 2.7×（第十五轮锁定，待重测）× M2 屏幕 ÷π（FusionIndirect.frag:138）× L1 波段 ×2/3（B4） | **C2/C3/C4** |
| (c) | 内部直射 = 0 | `GI_ALL_OFF` 纯直射帧内部 lum=0 | ✅ 物理正确，无需修 |

关键事实修正（防回归）：
- `ShadowFilter.spv` **不陈旧**（重编后标准路径逐像素一致，max diff 7/255）。
- 内部被遮挡、光柱位置正确（52° 仰角几何，见 C-series §9.2）——阴影方向链无罪，
  争议只在**受光表面的 vis 数值**。
- 首轮"vis 全黑"抓帧系调试模式误入 VSM 分支的假象；调试布线已修复
  （`DeferredLighting.frag` mode 0.25/0.5，VSM 判据收紧 `>0.75`）。

---

## Phase V1 — 直射 vis 缺口定罪与根修（C1.2 Vulkan 侧，约 0.5 天）

偏移 0.000~0.036 随采样点变化，三个候选假设，先判决后动手：

- [ ] **V1.1 三剖面采样**：沿（i）光柱中心（已知 vis=1、偏移 0）、（ii）光池墙带
  （vis 0.4~0.7）、（iii）内部墙（vis≈0）各取 5 点，用 `DBG_DEPTH/DBG_RECV` 建
  深度剖面。结合 Sponza 实际几何（檐口/壁柱/悬挂 banner）判别偏移来源：
  - H1 系统性 z 偏移（光栅 z 变换 vs 接收重构约定差）→ 偏移应全场景同号、幅度随
    光深/相机深单调；
  - H2 小遮挡器半影（ legitimately correct——2048 图上 banner/檐口边缘的真实
    penumbra）→ sc0.xy 邻域 map patch 应可见近距 occluder；
  - H3 slope acne → 偏移应与表面斜率相关、幅度 ≈ texelWorldSize×tanθ
    （≈1e-4 量级，与实测 2e-3~4e-2 不符，基本可排除）。
- [ ] **V1.2 occluder patch 直查**：新哨兵模式（或复用 DBG_DEPTH）输出 sc0.xy 邻域
  ±8 texel 的 map 深度微图，H1 应看到"无 occluder 但判影"，H2 应看到真实遮挡器。
- [ ] **V1.3 按判决修复**：
  - H1 → 定位偏移侧（候选：meshlet 阴影光栅顶点路径的 z 约定、naga 变换、
    invVP 深度假设），改约定不改数值；
  - H2 → 提高阴影图分辨率档 / PCSS lightSizeUV 收敛 / normal-offset；
  - H3 → slope-scaled bias。
  - **禁令**：未定罪前不得用加大常量 bias 掩盖——薄遮挡器深度差仅 ~0.008，
    bias ≥0.03 会连带抹掉全部内部阴影（漏光反转）。
- [ ] **V1.4 验收（C1.2/C1.3 原 band）**：光池带 vis ≥0.9；1F/2F 内部 vis ≤0.05；
  `GI_ALL_OFF` 太阳池 patch lum Vulkan/Metal 比 1.0±15%；薄墙/柱接触阴影保留；
  360 帧无 fault。

## Phase V2 — 重标前置测量（C2/C3 并行，约 0.5 天）

- [ ] **V2.1 卡片辐射重测**：V1 落地后 `SC_DUMP_CARDS` 双平台重测（直射复活 +
  vis 修复后历史 A/B 全作废条款），验证"2.7× 差"是否收敛。
- [ ] **V2.2 分源归因**：GI_ISOLATE 移植 Metal 后（C-series 下一步①），对 Vulkan
  室内亮度做逐源拆解（escape 天空 / analytic 兜底 / prev-screen / SC 反馈），
  为 C3.1-C3.4 排优先级。
- [ ] **V2.3 response 复测**：`GI_HALFLIFE=1` 判 response（C2.3 门禁，目标 YES）。

## Phase V3 — 能量注入收敛 + 显示链重标（C3 尾 + C4，约 1 天）

- 按 C3.1-C3.5 执行（analytic 无阴影 E_direct 去除、prev-screen 深度一致性、
  escape 分区门禁、B1 Metal ×2.5 回撤、反馈回路增益审计）。
- **Vulkan 专项核对**：M2 屏幕端 ÷π（`FusionIndirect.frag:138`）与 gather B4 系数
  的净等效增益，与 Metal 逐字比对；平台残差用判决实验定位，禁止校准系数吸收。
- C4.2 显示链一组旋钮一次 PR（CAL_EXPOSURE / DIRECT_INTENSITY 归一 / AO mix /
  ddgiBase / 天光底），对照 `sponza_region_lum.py` 全表。

## Phase V4 — 门禁固化（C5，约 0.5 天）

- [ ] GIVerify 追加物理关系带（直射墙/内部比 ∈ [8,100]、室内地面 < 直射墙）。
- [ ] `GI_GOLDEN_UPDATE=1` 重建 golden 基线（有意更新，随 PR 提交）。
- [ ] 本轮哨兵验收后随 PR 入库：`ShadowFilter.comp` quality 3-6、
  `DeferredLighting.frag` mode 0.25/0.5、两个 env 开关（零 env 时生产行为逐位
  不变，GIVerify 已全 PASS 确认）。
- [ ] 文档回填：C-series §9 日志 + `2026-09-04` 文档第十六轮。

---

## 执行注意

1. **并行 session 协调**：C-series session 正在同一工作树推进（C0 修复已在树：
   `DDGIGIGather.metal` flat-buffer、Metal ShadowFilter 绑定分支、零 dispatch 守卫）。
   动 `ShadowMapModule.cpp` / `DeferredLightingModule.cpp` / Lumen shaders 前先
   `git status` 对表，避免覆盖。
2. **构建树协议**：Vulkan 用 `Darwin/DebugVulkan`，切树先 `rm Darwin/Debug/libEngine.a`。
3. 每步验收 = 区域亮度表 + GIVerify 全带 + 360 帧无 fault；改 shader 后重编对应 spv
   （`build_spv.sh` / glslangValidator 命令见各脚本头注释）。
4. Metal 侧对照一律用 `TestMetalGIFusion` 替代路径（C-series §8），或等
   `TestModularPipeline` 恢复后回归。

---

## 10. 执行日志

### 2026-09-09 深夜（V1 完成：直射 vis 缺口定罪）

**V1.1 剖面判决（H1 系统偏移 ✗ / H3 slope acne ✗ / H2 真实遮挡 + PCSS 盘径 ✓）**：
全图 offset（接收器 sc0.z − 阴影图深度）按 vis 分桶：vis≥0.9 → mean −0.0007（无偏移）；
vis 0.5-0.9 → +0.0036（贴邻遮挡器）；vis<0.1（内部）→ **+0.27**（前方整栋建筑的真实
遮挡深度差）。偏移剖面 = 真实遮挡结构，**阴影比较链无罪**——首轮"系统性 z 偏移"
结论系 6 采样点恰落在檐口/banner 边缘的误判，于此撤回（与 C-series §9.2
"直射链存活"结论汇合）。

**工具修正**：`ShadowFilter.comp` 哨兵门控 `==3u||==4u` 挡住了 5/6 档
（HARD/BLOCKER 抓帧此前实际跑的是普通 PCSS vis）——已修 `>=3u && <=6u`。

**V1.3 判决实验 + 落地**：`pcssShadow` lightSizeUV 0.05 → **0.012**（T8.13 文档值；
0.05 系 9526281c 提交的注释/代码不一致产物——当时该分支是死代码，9/7 切 PCSS
默认后被激活；0.05 UV ≈ 3wu"太阳"，物理太阳 ≈0.0087 UV）。效果（受光带实测）：
vis≥0.9 占比 19% → **40%**，带内直射 lum 21.0 → 24.8；vis 图受光带边缘干净、
内部 vis≈0 保持、全帧无 artifacts。

**V1.4 验收**：`sponza_region_lum.py` 标准表与修复前逐位一致（标准区域盒不覆盖
近屋顶受光带——与 C-series §9.2 "光池在近屋顶墙带"一致）；GIVerify 全带 PASS；
360 帧无 fault。**C1.2 "光池 vis ≥0.9" 在真实受光带达成**；sunlit_wall §8 盒的
Vulkan/Metal 差异（25.8 vs 153.1）确认为盒位置 + Metal VSM 软半影/GI 注入差，
归 C4 跨端标定，非 Vulkan 正确性问题。

**遗留**：PCSS 半影带（vis 0.4-0.7 环绕受光带）是真实檐口遮挡 + 0.012 软化档的
物理结果；V2 起转入 C2/C3（探针-太阳耦合 + 能量注入收敛）。

### 2026-09-10 凌晨（V2 执行——两个根因 bug + 一次世界观修正）

**V2.3 response 复测 → 顺藤摸出本计划最大的 bug**：

1. **【已修·根因级】DDGI 探针索引错误**（`7a644b19` 引入，DDGITraceSDF.comp +
   DDGITraceFinalize.comp）：`group_id = gl_GlobalInvocationID.x` 在 8×8 局部
   布局 + Dispatch(updateCount)（一组一探针）下，把射线 lane x 折进探针号——
   每探针只有 16/128 条射线（≡ slot mod 8）被写，其余 112 条永远停留在初始化
   值。Metal 孪生 kernel 用 `[[threadgroup_position_in_grid]]` 是对的。修复 =
   `gl_WorkGroupID.x`（两文件）。**Vulkan 的 DDGI 探针从未真正完成过一次
   128 射线追踪**——这极可能是历史上"Vulkan GI 扁平/方向能量减半/卡片辐射
   3×/response=NO"全部悬案的共同根因。
2. **【测量语义崩塌】M3 漏光代理门禁的 hit=0.8776/escape=0.1223 ≈ 112/128 与
   16/128**——旧门禁度量的是"未写槽位的初始化零值"，不是射线结果。修复后
   真实 trace outcome = hit ~2-46%/escape ~54-98%（分探针位置），两旧 band
   全部作废，需按"盒内探针"重定义（见下）。
3. **【新暴露的主病灶 → C3 确认】探针场 = 天空填充**：修复后全量射线真实
   追踪，探针射线平均辐射 = (2.256,2.313,2.353) ≈ DDGI_SKY_COLOR，且调光
   （sun→10%，151 帧收敛）前后**逐位不变**——场能量来自逃逸射线吃天空，
   与太阳解耦的机制坐实。区域表：L_arcade 10.7→26.0、R_arcade 16.8→30.1
   （天空填充经修复的射线落地），sunlit/int 1.88→0.61 暂时恶化——符合
   C3 预判，下一修复项 = **C3.1/C3.3（analytic 无阴影直射删除 + escape 天空
   与可见天空统一/门控）**。
4. **逃逸的构成**：探针网格 ±46.5/±22.5 而离线 SDF 盒 (origin -21.6,-7.5,-14.1,
   extent 42×27.8×27.2) 只覆盖建筑本体——**~91.5% 探针在盒外**，100% 逃逸是
   预期行为；盒内探针（相机格）实测 46-82% hit。in-box 的剩余逃逸待查
   （无符号浅场阈值带 0.19-0.38 vs 阈值——阈值 A/B 0.30→0.45 仅 97.9→97.1，
   说明 in-box 逃逸另有原因，M3-A1 专项需以真实测量重做）。
5. 顺手修复：`DDGITraceFinalize_pathdbg.comp.spv` 陈旧（8/27）且 build_spv.sh
   无编译规则——已手编补齐，但 `-DDGI_PATH_DEBUG` 的 tag 常量仍未出现在 spv
   （工具链待查）；DDGI_DUMP=2 的 CSV 增加 rad_r/g/b 平均辐射列（本次判定
   的关键测量）。
6. `LumenDDGIPass.cpp` 新增 `[DDGI_SDF_CB]` 一次性日志（origin/extent/res/
   rayMax 真值）。

**当前门禁状态**：GIVerify probe/gather band PASS（probe_sh0 3.57、gather
4.83），trace 两 band FAIL（如实反映新语义）——**不回滚、不放宽**，band 重定义
随 C3 修复一起做（盒内探针分类统计）。GI_HALFLIFE response=NO（天空主导，
C3 后复测）。360 帧无 fault。

**下一步（优先序）**：
1. C3.1/C3.3：escape/analytic 天空注入门控（本诊断的直接后续）；
2. in-box 逃逸根修（M3-A1 以真实 hd 重做：SDF 采样 vs 阈值 vs 步进）；
3. GIVerify trace band 重定义（盒内/盒外分类）+ GIGolden 重生成；
4. C2 双源统一 + response 复测；
5. 之后才轮到 C4 跨端显示标定。

### 2026-09-10 凌晨二（C3.1 落地 + 门禁重定义 + 调光链路终局判定）

**C3.1 ✅**：`DDGITraceFinalize.comp` analytic 兜底删除无阴影 `E_direct`（穿墙阳光
注入），`E_sky` 降档 ×0.3 作环境底。效果：L0 7.50→6.14（halflife 起点）、
拱廊 R 30.1→26.8——"只兜黑不兜亮"达成。

**C3.3（部分）✅**：`DDGI_SKY_COLOR=(2.75,2.88,3.00)` 与 deferred 天顶色逐位
一致——逃逸天空"档位统一"本已达成；剩余问题 = 逃逸**份额**（in-box 67%，
其中大量为盒外探针的合法逃逸 + in-box 隧穿），归 M3-A1 与可见性根修。

**调光链路终局判定**（halflife 期间抓屏对比）：屏幕直射随调光正常下降
（courtyard_sunwall 48→22，deferred 走 settings_ ✓），**室内 GI 纹丝不动**
（arcade 11-13 → 12-14）——sun→GI 的解耦点确认为"逃逸天空 + 无可见性
SC↔probe 回路的自维持平衡态"，即 C-series 深夜二/三的判决。response 转 YES
依赖：SC 侧 gather 可见性（并行 session 车道）+ 逃逸份额压降（M3-A1）。

**门禁重定义 ✅（GIVerify 全绿）**：
- 发现更新列表非恒等映射（frame 1 后按优先级 partial_sort）——hd[slot] 存
  update_list[slot] 的射线，第一版门禁按 slot 位置分类探针得到 in-box 0.5%
  的假数字；修正为读 `GetProbeUpdateListBuffer` 映射后 in-box = 1134 探针、
  hit 32.9% / escape 67.1%。
- trace band 重定义为**盒内探针**统计：`trace_inbox_hit_ratio [0.10,0.90]`、
  `trace_inbox_escape_ratio [0.05,0.90]`、`trace_inbox_exhausted [0,0.10]`；
  全局数字降级为 informational（含 91.5% 盒外探针的合法逃逸）。
- 区域表（C3.1 后）：L_arcade 25.4 / R_arcade 26.8 / sunlit_wall 16.0 /
  floor_near 16.3——sunlit/int 0.61，物理关系门禁仍 FAIL（预期内，等
  可见性根修 + C4 标定）。
- 360 帧无 fault；validation 372 条与存量基线同类。

**注意**：跨树污染复发一次（并行 session 以 Metal 树重建了共享 archive，
链接期 VulkanDevice 符号丢失）——按协议 `rm Darwin/Debug/libEngine.a` 后
DebugVulkan 全量重链解决。并行编译会话期间构建前先核对 archive 归属。

**区域表（步长封顶后）**：L_arcade 23.1 / R_arcade 25.6 / sunlit_wall 15.8 /
floor_near 15.5 / sunlit-int 0.65；370 条 validation 存量同类，360 帧无 fault。
GIVerify 全带 PASS（含新盒内 band）。

### 2026-09-10 凌晨四（GIGolden 重生成 + 门禁闭环）

- GIGolden 旧基线 worstRel 544%（col 5 = L1 方向分量变化最大——方向性激活的
  直接证据）；按流程 `GI_GOLDEN_UPDATE=1` 重生成 8 探针基线并复验：
  **GIVerify + GIGolden 全绿**（run-to-run worstRel 0.36%，多稳态收敛良好）。
- 最终门禁态：GIVerify（含新盒内 band）PASS + GIGolden PASS + 360 帧无 fault。
- C2.1 备注：halflife harness 已双路调光（scene light + settings_），response=NO
  的阻塞项不是双源，而是天空锁死的平衡态（见凌晨二/三条目）；生产侧双源
  统一（编辑器改灯 → SC 捕获传导）仍列 C2 后续。
- halflife 门禁设计备注：camcell 2×2×2 采样落在部分被埋的中庭上空 cell
  （(1.5,7.5,1.5) 128 射线全 d=0）——response 门禁的采样 cell 选择需在
  被埋探针修复后重新评估（受光 cell / 多 cell 平均）。

**本轮（索引修复 + 步长封顶 + C3.1 + lightSize + 门禁重构）后的差异总表**：

| 量 | 修复前 | 修复后 |
|---|---|---|
| 探针每帧真实写入射线 | 16/128 | 128/128 |
| 盒内探针命中距离 p50 | 14-20 m（穿墙） | 0.5-1 m（近场） |
| trace outcome（全量） | 假象（init 零值 87.8% "hit"） | hit 3.8% / escape 96.2%（真实） |
| trace outcome（盒内） | 不可信 | hit 39.1% / escape 60.0% |
| GIVerify | 旧 band 假 PASS | 新 band 真 PASS |
| GIGolden | 旧基线（死场） | 重生成（活场，run-to-run 0.36%） |
| analytic 兜底 | 无阴影穿墙直射 | 仅环境底（×0.3）+ 自循环 |
| PCSS lightSize | 0.05（3wu"太阳"） | 0.012（≈1.4× 物理） |
| response | NO | NO（等 SC 可见性 + 被埋探针修复） |

### 2026-09-10 凌晨五（被埋探针修复：buried-start skin crawl）

**修复**：`DDGITraceSDF.comp` march 加 buried-start 爬出——拓扑歧义空气
（开放式中庭顶部）的合并 winding 退化会产生**精确 0 值体素**，起始于此的
探针此前 128 射线全部 t=0"命中"、拿投影位置的画面色当辐射。现在射线先爬出
命中皮肤（`d ≥ hitThreshold` 前不登记命中，`t += max(d, minStep)`），爬不出
（厚墙体）按 exhausted 走 miss 兜底（暗 SC 回退）——墙内探针的正确答案。

效果：in-box exhausted 0.87→1.96%（被埋份额，band 内）；halflife L0
6.14→5.57（垃圾亮色清除）；GIVerify 全带 PASS；区域表 L_arcade 23.9 /
R_arcade 27.2 / sunlit 19.2 / sunlit-int **0.65→0.75**（受光墙回升 = 垃圾
暗填充减少的连带效应）；370 validation 存量同类、360 帧无 fault。

**response 仍 NO**：剩余阻塞 = ①SC gather 可见性（并行 session 车道，
Chebyshev 根修）②in-box 60% 逃逸中的隧穿残量 + 合法长轴份额的进一步
分region量化。两者落地后 response 复测 → C2 → C4。

### 2026-09-10 上午（Metal 孪生同步 + M3-A1 逃逸成分量化收尾）

**Metal 同步 ✅**：`Engine/Graphics/Metal/shaders/Lumen/DDGITraceRays.metal`
（ddgi_trace_sdf kernel）同步三项——阈值下限 `max(voxel×1.25, 0.45)`、
buried-start skin crawl、近场步长封顶 ≈2 voxel（Metal 用同一份离线 SDF，
同样的 min 合成高估 → 同样的隧穿暴露面；Metal 索引本正确
`[[threadgroup_position_in_grid]]` 无需改）。运行时编译免构建；
`check_shader_copies.sh` gate OK（0 对待更新）。

**M3-A1 逃逸成分量化（步长封顶后，arcade 1F 盒内探针）**：
- 逃逸方向分解：±x 走廊长轴 28% + ±z 拱廊开口 25% + +y 天顶 24% +
  -y 地板 15% + 斜向 8%——**主体为合法长轴/开口射线**；-y 地板 15% 为
  隧穿残量疑点（地板距探针仅 1-3m 却逃逸，疑似接缝高估残量），列
  M3-A1 残项；
- 拱廊命中 p50 0.44 m——近场几何已被采到。
- 结论：in-box 逃逸的主体是**几何合法的远距离射线**，其天空注入属于
  "可见天空"而非隧穿泄漏；sky 份额的最终压制靠可见性权重（SC 车道）
  而非 march 修复。

**门禁态**：GIVerify + GIGolden 全绿；360 帧无 fault；validation 存量同类。

### 2026-09-10 上午三（DIRECT 15→20 + SC 可见性链路端到端验证 + response 门禁设计提案）

**直射二连加强至 20.0**（双平台同步；基线 7.5 的 2.67×）。direct_patch
56.7→**72.2**，**direct/interior 比值 1.88→6.83**（目标 ≥8，UE 13.5——
已接近）。

**SC 可见性链路端到端验证 ✅**（接手并行 session 落地的 Chebyshev 绑定）：
- `SC_DEPTH_STATS` 仪器原被放在 Metal-only else 分支（Vulkan 永不打印），
  已移出至平台无关位置；
- **Vulkan 深度数据已活**：camcell 7947 八方向 = (2.79,1.72,1.53,50,50,
  1.53,50,50)——真实墙距 1.5-2.8m + 走廊长轴开放 50，不再是全哨兵
  （探针索引修复喂活了深度数据）；
- `DDGI_DUMP=4` 新增：SC 图册位样转储（原始 u16 + nonzero/sum）。

**调光传导全链路判定（10× 调光）**：
- SC 图册：**43.5% 位样变化** ✅（LightEval 正确响应）；
- 探针射线（中庭/相机格）：~0.3-1.4% ✅ 有传导但份额小；
- 注意：旧"辐射逐位相同"结论系**时序伪影**（verdict@349 先于 dump@350
  退出，cp 复制了基线残留）——dim 实验必须 FRAME+WINDOW < dump 帧。

**response=NO 定量解释**：camcell 平衡态被"逃逸天空（已降档 2.5×）+
无可见性回路残量"支配，太阳信号 ~1-5% 被淹没；且 camcell 部分探针
被埋（能量=SC 回退=投影位置画面色）。**门禁设计提案**：response 度量
从 camcell L0 切换为 SC 图册响应（DDGI_DUMP=4 位样，10× 调光 → 43.5%
位样变化，SNR 高两个数量级）或改采样真正受光 cell——待 C-series owner
拍板。

**最终区域表（DI 20 + 天空档 0.4 + 全部修复）**：direct_patch 72.2 /
L_arcade 9.9 / R_arcade 11.3 / sunlit_wall 8.3 / floor_near 6.4 /
**direct/interior 6.83**（基线 1.88，+263%）。GIVerify + GIGolden
（重生成后 run-to-run 0.0003%）全绿；370 validation 存量同类。

**剩余**：①response 度量切换拍板 + 被埋探针 relocation 增强（M3 残）；
②-y 地板 15% 隧穿残量（单射线 SDF dump）；③C2 双源统一（生产侧）；
④C4 剩余旋钮（CAL_EXPOSURE / 天空底，visibility 收口后统一标定）。

### 2026-09-10 上午五（M3-A1 终局：隧穿源 = 离线缓存的合成结构；运行时 GlobalSDF 对照）

**离线缓存修复无效的定位**：`SampleMeshSDF` 的 AABB 修复后 GIVerify 逐位
不变——测试走的是 `LoadFromCache`（108MB 预合成 v3.cache），per-mesh 合成
路径根本未被行使。隧穿是**烘死在缓存里的**，运行时无法修复。

**缓存合成三层结构缺陷**（BuildAndUpload/ContentTools 同构）：
1. per-mesh AABB 扫描边距 ±0.01——地板网格（y ±0.35m）上方 0.36m 起
   min() 即盲（地板不可见 → d 被远处墙面支配 → 大步穿透）；
2. 空区填充 = voxel×4 ≈ **0.4375 ≈ 命中阈值 0.45**——自由空间贴着
   阈值，命中判定近乎随机；
3. 哨兵 min 盲区（1e10）叠加。

**运行时 GlobalSDF 对照（SPGI_NO_OFFLINE_SDF=1，零代码）**：

| 量 | 离线缓存 | 运行时 GlobalSDF |
|---|---|---|
| 盒内 hit / escape | 29.1% / 68.8% | **44.4% / 55.5%** |
| response（10× 调光） | +0.4%（无响应） | **−3.8%（方向正确）** |
| 区域表 | interior 6.4-11.3 | **2.3-8.8（遮挡正确、更暗）** |
| 目视 | 均匀灰亮填充 | 直射强、内部正确地暗 |

结论：离线缓存对 trace 结构性不适配；运行时 GlobalSDF 的遮挡/响应物理
正确，代价是室内更暗（合法多弹射积累仍缺——C3.5 回路增益专项）。
**决策待拍板**：默认 SDF 源切运行时 GlobalSDF（建议，物理正确 + 已验证）
vs 重烘缓存（需 ContentTools 修合成三缺陷后重跑，成本高）。切源后室内
变暗属物理正确，多弹射专项补回暖色积累。

### 2026-09-10 上午六（Plan B 实施第一步：generate_sdf 距离传播修复已就位）

**根因终极定位**（用户提示"注意离线 SDF 的符号"后沿链深挖）：解析
`Sponza_global_sdf.v3.cache`（PGSDFC02 = R16F 有符号半浮点，无解码）逐
体素采样——空气值 0.24/26.16/0.82/6.46/18.06 之间 100× 跳变，**根本不是
距离场**。烘焙端根因：`ContentTools/common/SDF.cpp generate_sdf` 的最近
三角形搜索只覆盖 **3³ 体素邻域**，超出 ~1 体素的空气直接钳位 max_dim
（代码 `dist > max_dim → dist = max_dim`）——per-mesh 场 = 表面皮肤 +
max_dim 钳位的混合物，min 合成后即为观测到的病态场（隧穿/被埋/平填充/
天空灌入的全部载体）。

**修复已实施**（`ContentTools/common/SDF.cpp`，构建通过）：generate_sdf
主循环改为保留原始量（sdist/cpts/inside_flag），随后 **6 方向符号感知
距离传播 sweep**（2 轮 × 3 轴 × 正反向，relax = |p − 邻域最近点|，符号随
本体素 winding），最后统一打包 data + vector_field。经典近似 SDF 传播，
全网格得到真实距离。

**重烘链路（待执行，需定制命令）**：Sponza_pipeline_signed_v5.model 由
此前 session 的定制工具烘焙（标准 `cook-mesh` CLI 明确不持久化 SDF 大负
载）。重烘步骤：①用该定制命令（或等价管线）以修复后的 generate_sdf 重
烘 → `Sponza_pipeline_signed_v6.model`；②改名/移走 v3.cache → 测试回退
pipeline-model 路径 → OfflineSDFMerger 重合成（含 AABB 修复 + SaveCache）
→ v3.cache 自动再生。**需要知道 signed_v5 当时的烘焙命令/参数。**

### 2026-09-10 上午四（均匀常量压降验证 + 物理分布达成）

**"albedo×0.15-0.25 均匀贴回"签名分析**（用户目测 + 逐区域 lum/albedo 比）：
天空档位门控（2.88→0.4 档）后，室内 lum 降至 albedo×~1.0（display 口径）——
均匀分量仍存在但绝对值已低（interior 6.4-11.3 display，此前 15.8-27.2）。
直射/内部比 **0.75→0.79→（DI 20）6.83→0.79 口径差**……最终态：
direct_patch 72.2、interior 6.4-11.3、比率 6.83（基线 1.88，+263%）。

**当前画面物理结构**（目视确认）：中庭直射光池明亮清晰 + 斜光束可见 +
室内拱廊正确地暗——用户要求的物理分布已呈现。

**剩余差距（真间接光的"质量"而非"有无"）**：UE 参考的入口地面有暖色
多次弹射积累（floor_near 59.5 vs 我们 6.4）——即合法的多弹射传输仍弱于
UE。这需要：SC↔probe 回路的真实多弹射积累（可见性已落地，回路 DC 增益
审计 C3.5）+ 被埋/隧穿残量清零。属下一阶段（M3-A1 残项 + C3.5），
不再是"错误常量"问题。

### 2026-09-10 凌晨二（传播修复全链生效；直接≈物理，多弹射断供定性）

**传播修复全链生效后的实测**（DI 20 + 天空档 0.4 + 传播 + 可见性）：
- **response 转 YES**：half_life=45 ≤ 60（C2.3 门禁通过）——调光后
  camcell L0 0.1557→0.1102（−29%），探针场首次真实耦合太阳；
- 盒内 trace：hit **85.6%** / escape 10.9%（真实近场几何，此前 29/69）；
- probe_sh0 0.497 / gather 0.414——探针场回落到物理量级（天空灌入坍缩）；
- GIVerify + GIGolden 全绿（golden 已按新平衡重生成）。

**残留问题（C3.5/C4 范畴）**：室内近黑（arcade 0.7-1.4 / floor 0.7，
UE 8.9-22.5 / 59.5）。frame 900 vs 360 逐位一致——非收敛问题，是
**多弹射传输链在可见性门控下断供**：合法弹射（走廊轴/拱廊开口的真实
透光方向）与隧穿残量一起被压制。修复方向 = ①C3.5 回路 DC 增益与
accVis/accLow 45/55 权重审计；②-y 地板隧穿残量清零后合法份额自然回升；
③必要时 escape 天空按开口立体角分档。

**直射侧已达标**：DIRECT_INTENSITY 20.0（双平台），direct_patch 67-72、
光带核心近裁切——用户要求的直射观感成立，物理关系（直射 ≫ 内部）成立。

**本轮全部改动文件**：DDGITraceSDF.comp（索引+步长封顶+爬出）、
DDGITraceFinalize.comp（C3.1/C3.3）、ShadowFilter.comp（哨兵+lightSize）、
DeferredLighting.frag（DI 20+调试模式）、OfflineSDFMerger.cpp/.h（传播
+RebakePerMeshSDF+SC_DEPTH_STATS 移正）、SurfaceCachePass.cpp（SC_DEPTH_
STATS 原位）、LumenDDGIPass.cpp（SDF_CB 日志）、ShadowMapModule.cpp/
DeferredLightingModule.cpp（哨兵）、TestVulkanSponzaRenderGraph.cpp
（门禁重定义+辐射/逐射线转储+SDF_REBAKE_V6 钩子）、golden CSV 重生成、
generate_sdf 传播修复（ContentTools/common/SDF.cpp，供下次重烘）。

### 2026-09-10 上午二（C4 首旋钮：DIRECT_INTENSITY 7.5 → 12.5，用户目测"直射偏弱"）

GI 修复串（探针索引/步长封顶/C3.1）落地后室内拾亮，直射相对冲击力下降。
按 T8.4 教训选 `DIRECT_INTENSITY`（只缩放屏幕直射，不进探针回路、不移动
SC↔probe 平衡）；双平台同步（Vulkan GLSL + EngineTest gpuDriven MSL）。

受光区实测（7.5 → 12.5）：

| 区域 | DI 7.5 | DI 12.5 |
|---|---|---|
| lit_band_R mean / p90 / max | 59.5 / 126 / 237 | **72.5 / 176 / 254（近裁切）** |
| lit_patch_c mean / p90 | 69.0 / 175 | **84.4 / 175** |
| beam 中庭斜光束 mean / p90 | 34.1 / 72 | **41.4 / 103** |
| 室内（arcade/sunlit_wall/floor） | 不变 ✓ | 不变 ✓（直射不进探针回路） |

光池核心进入裁切白（254）、斜光束 p90 +43%；受光面积仍由 52° 太阳仰角决定
（几何约束，UE 参考同构）。室内不受影响——物理关系（直射 > 内部）未破坏。
后续若需整体提亮，C4.2 的 CAL_EXPOSURE 旋钮待 visibility 收口后统一标定
（一次 PR 一组旋钮）。

**用户复检后继续加强：12.5 → 15.0**。lit_band_R mean 72.5→**78.4**（p90 176→189、
max 254→255 满裁切）、beam p90 103→**118**（max 176）、lit_patch_c mean 84.4→
**92.1**；室内逐位不变。当前档 15.0 = 历史基线（7.5）的 2×。

### 2026-09-10 凌晨三（M3-A1 破案：march 隧穿 + 步长封顶修复）

**工具**：DDGI_DUMP=2 新增 `/tmp/ddgi_ray_detail.csv`（盒内探针逐射线
rayId+dist，145K 行），配合 ProbeSampleLib 的 Fibonacci 方向集离线重算，
做方向聚类。

**判决**：盒内探针 0% 逃逸、命中**全部落在 14-20 m**（相机探针 p50=15.2），
而最近墙体 1-6 m——march 系统性穿透近场几何，只在远处表面注册。机制：
min 合成场在 mesh 网格接缝处**高估**距离（出格哨兵 1e10 → min() 取远处
mesh 的距离），保守分支 `advance = max(d, minStep)` 恰按虚高 d 迈步 →
一步跨过 0.19-0.38 浅带。探针场因此只收到"远处表面"的辐射 = 平填充的
第二根因（第一根 = 逃逸天空，见凌晨一条目 3）。

**修复**：近场步长封顶 ≈2 体素（`advance = clamp(d, minStep, 2×voxel)`，
远场增长向保持 1.5× 过冲以保 50 m 射程）。效果：
- 相机探针命中 p50 15.2 → **1.75 m**（逃逸 54%→0）；
- 盒内命中距离直方图回归近场（0.5-1 m 档 95K 条为主，≥10 m 仅 1.5K）；
- GIVerify 盒内 hit 32.9→39.1%、escape 67→60%、exhausted 0.87%（band 内），
  **ALL BANDS PASS**。

**response 仍 NO 的余因（下一 M3 项）**：halflife 读数的 camcell 探针
(1.5,7.5,1.5) 128 射线全部 d=0.00 命中——**被埋探针**（中庭空气处场值读 0
= min 合成接缝毒化）。被埋探针的平衡态天然与太阳无关；relocation 质量 +
场接缝去毒是 response 转 YES 前的最后一个 trace 侧修复。SC 可见性
（并行 session）并行推进。

**区域表（步长封顶后）**：L_arcade 23.1 / R_arcade 25.6 / sunlit_wall 15.8 /
floor_near 15.5 / sunlit-int 0.65；370 条 validation 存量同类，360 帧无 fault。
GIVerify 全带 PASS（含新盒内 band）。
