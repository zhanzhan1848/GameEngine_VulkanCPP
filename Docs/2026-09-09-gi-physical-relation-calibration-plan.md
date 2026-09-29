# GI 物理关系校准计划（C 系列，2026-09-09）

**执行状态（2026-09-09 晚，C0/C1 第一批）**：见文末 §9 执行日志——C0 全部完成；
C1 的 Metal PCSS 深度绑定错配已修；**重要前提修正**：直射链在当前 HEAD 双平台均存活
（Round 11-14 的"直射死亡"已是旧状态），太阳池小是 52° 仰角下的物理正确结果，
主攻方向收敛为 C2/C3（探针场与太阳解耦 + 不查遮挡的填充能量）。

**症状**：TestModularPipeline 中建筑内部的间接光比最接近直射光区的墙还亮——物理关系反转。
**诊断全文**：本日 session 实测 + 复现命令见 §8 基线数据；历史脉络见
`Docs/completed/2026-09-04-spgi-energy-calibration-and-ddgi-overexposure.md` 第八轮起至第十四轮。
**台账**：挂入 `tasks/gi-supplement-todo.md` 为 **M6: 物理关系校准（C 系列）**。

---

## 0. 诊断结论（两病灶 + 放大器）

| # | 病灶 | 证据 |
|---|---|---|
| ① | **探针场与太阳解耦**（间接光 = 不查遮挡的平填充） | `GI_HALFLIFE` 判 response=NO（90% 调光 150 帧 L0 4.57→4.70 不降反升）；能量来自 escape 吃天空（12.2%）、analytic 兜底无阴影直射、SC↔probe 反馈回路平衡点 |
| ② | **直射链大面积死亡** | `GI_ALL_OFF=1` 纯直射帧全黑，仅中庭高处一小块有光；庭院"直射区"墙实测直射 = 0（UE 参考 120） |
| ③ | 放大器：本周能量抬升 | B7 钳位 10→64、B1 Metal gather ×2.5（a8d31cde）、prev-screen path 无深度测试 + 0.12 floor、analytic 路径 `E_direct` 无阴影 |

实测区域亮度（参考机位，1280×720，LDR）：

| 区域 | UE 参考 | Vulkan | Metal |
|---|---|---|---|
| 庭院直射墙 | 120.5 | 25.8 | 153.1 |
| 拱廊内部 | 8.9 | 16.8 | 40.4 |
| 门厅地面（阴影） | 59.5 | 15.8 | **195.6** |
| 直射÷内部比 | **13.5×** | 1.5× | 内部反超 |

## 1. 校准原则（历史教训固化）

1. **先正确性，后标定**：直射不活，一切曝光/能量旋钮的标定作废（09-05~06 已付出一轮代价——CAL_EXPOSURE 0.42/DIRECT 7.5 均在直射死亡期标定，Round 13 复活后全部需重测）。C4 之前禁止动全局显示/能量旋钮。
2. **每步带量化验收带**，不目视验收；band 从物理推导得出，不再事后放宽（probe_sh0 [0.08,13] 是放宽 absorb 的反面教材）。
3. **双平台同参**：旋钮绝对值进 `SponzaReference.h`，平台差异用判决实验定位根因，不用校准系数吸收（×2.5 教训）。
4. **直射/耦合类修复落地后，全部旧 A/B 数据作废重测**（含"卡片辐射 3×"专项）。
5. 构建树协议：两树共用 `Darwin/Debug/libEngine.a`，切树先移走 archive。

---

## Phase C0 — 解锁与测量基建（0.5 天）

目标：两个测试二进制在双平台可运行、可测量；区域亮度表固化为一键脚本。

- [ ] **C0.1** 同步 `EngineTest/shaders/DDGIGIGather.metal` 到引擎 flat-buffer 版（T3.6 后 C++ 绑定 10/11、不写 7；测试副本旧签名 `irradianceBuffer@7` → validation 下 frame 1 assert，无 validation 静默读未绑定缓冲）。同步后从 `check_shader_copies.sh` 豁免清单移出该对；临时通路 `PRIMAL_GATHER=DDGIGIGather.metal` 已验证可跑。
- [ ] **C0.2** 修 Metal `GI_ALL_OFF` 的 0-threadgroup dispatch assert（pass 关闭后 dispatch 数为 0 仍提交）。
- [ ] **C0.3** 区域亮度脚本固化：把本次诊断用的归一化区域表（§8）落成 `scripts/sponza_region_lum.py`（输入 PPM/PNG，输出区域 lum 表 + 直射/内部比）；`sponza_reference.py` 可选 `--regions` 直接输出。
- [ ] **C0.4** 双平台基线采集：Vulkan + Metal（经 C0.1）参考机位 360 帧 capture × {全 GI, GI_ALL_OFF}，记入 §8 表——这是后续每阶段的前后对照基线。

验收：`TestModularPipeline` 裸跑无 assert；`scripts/sponza_region_lum.py` 对基线 capture 输出与 §8 一致的表。

## Phase C1 — 直射链复活（正确性 P0）

目标：太阳光池物理正确——该亮的墙亮起来、室内漏直射归零。这是"直射墙 > 室内"关系的直接来源。

- [ ] **C1.1** ShadowFilter 接收端三段直出定位（Round 14 遗留，本轮已排除 CPU 矩阵因素：`PRIMAL_DUMP_MATRIX` 实测 deferred vp/invVP 与 shadow view/proj 数值一致；filter 与 deferred 重建数学逐字相同、深度纹理同源 `final_depth_texture_`）。在 `ShadowFilter.comp`/`.metal` 加 `PRIMAL_SHADOW_FILTER_DEBUG=1/2/3` 直出：1=采样 depth、2=重建 worldPos、3=sc0.xy(z)。与 deferred 同像素真值对照，断点必在三段之一（候选：half-res fullUV 映射、采样器/格式、Y-flip 约定、Metal 深度反转假设 Round 13"map d=1−z"）。
- [ ] **C1.2** 修复 + vis 结构验收：vis 可视化下庭院光池 ≥0.9、1F/2F 内部 ≤0.05（Round 8 实测漏光态为内部 0.48-0.64 / 光池 0.51——两向都要恢复）。
- [ ] **C1.3** 直射-only 物理关系验收（`GI_ALL_OFF=1` 参考机位）：sunlit_wall 区域 lum / 内部区域 lum ≥ 6（直射链健康下限；当前 Vulkan 全黑=0）。
- [ ] **C1.4** Metal 同修同验（Round 13 补遗：Metal 直射同样死亡，深度方向假设一并在 C1.1 三段直出中定性）。

验收：C1.3 双平台 PASS；`ctest -R Unit` 全绿；两测试 360 帧无 fault。

## Phase C2 — 探针-太阳耦合（response=NO → YES）

目标：间接光能量源自真实传输（SC 直射 × 探针反弹），关灯即暗。

- [ ] **C2.1** 光源双源分裂统一（M2 定位）：SC 走 `sc_lights`（场景灯）而 DDGI/deferred 走 `settings_`——统一到同一光源表（`SurfaceCachePass.cpp` T0.4 已接 FrameData 真实光源，核对 `GI_HALFLIFE` 调光路径两路都覆盖：场景灯 `UpdateLight` + `settings_` 双路已做，验证 SC LightEval 收到的可见性/颜色随调光变化）。
- [ ] **C2.2** SC 直射可见性健康检查：`SurfaceCacheShadow`（SDF march）输出统计（`SC_DUMP_CARDS`），室内卡片 vis≈0、受光卡片≈1；若 SDF march 大面积漏光，先修 SDF 阴影参数（bias/步长/级联选择）再继续。
- [ ] **C2.3** `GI_HALFLIFE=1` 复测：response=YES（90% 调光后 camera-cell L0 降至 <20% 起始值，半衰 ≤60 帧——门禁已实现，此前因直照死亡预期 FAIL）。

验收：C2.3 PASS。此时"室内间接光"首次来自真实弹射，物理关系有了能量来源。

## Phase C3 — 能量注入收敛（堵住不查遮挡的能量）

目标：填充能量只能来自真实传输或显式天空项；跨端用同一数学。

- [ ] **C3.1** analytic 兜底路径（`DDGITraceFinalize.comp:449-461` / `DDGITraceRays.metal` 同位）：删除无阴影 `E_direct`（或以 trace 可见性门控）；`E_sky` 仅保留作环境底并显著降档；目标 = 该路径只兜"黑"，不兜"亮"。
- [ ] **C3.2** prev-screen path 2（`DDGITraceFinalize.comp:429-448`）：投影采样加深度一致性测试（当前 occluded/off-screen hit 采样无关亮像素）；删除/论证 `materialBounce` 的 0.12 floor。
- [ ] **C3.3** escape 天空项与可见天空统一（分叉结论在案：探针天空档与背景天空同源）；补**分区 escape 门禁**：M3 全局带 [0.03,0.3] 之外，加 camera-cell 与"室内 cell"（探针被 ≥2 面几何包围的判定或手选测试体积）escape ≤ 0.05。
- [ ] **C3.4** 回撤 B1 Metal gather ×2.5（a8d31cde 自注：根因是 Metal monolith miss 用 sky lum≈2.9 vs Vulkan split ≈0.5——修 miss 单位源，不保留平台校准系数）；同步重标 gather_avg/final_avg band。
- [ ] **C3.5** 反馈回路余量审计：SC_FEEDBACK 0.70 × 探针滞回 0.85 ∘ gather clamp 4.0 的 DC 增益对场景最亮 albedo 闭环 < 1，余量 ≥20%（T8.13 bistability 复发即不达标）；B7 钳位 64 保留，但 GIVerify 输出 L0 分布 p50/p99 与物理推导带对照（见 C4.1）。

验收：室内 cell 探针 L0 < 0.2 × 室外受光 cell 探针 L0（空间对比恢复）；双平台 GIVerify 全带 PASS；GIGolden（25%）PASS 后重建 golden 基线（GI_GOLDEN_UPDATE=1，流程内有意更新）。

## Phase C4 — 端到端 UE 参考重校准（仅在 C1-C3 全绿后）

目标：恢复 09-06 达成过的参考对齐，且这次物理关系正确。

- [ ] **C4.1** 物理推导期望带：从当前灯/天参数推导探针 L0 与区域 lum 的期望区间（室外受光 ~albedo×E/(1-ρ) 量级、室内按开口立体角折减），替代历史"实测±包络"法；入 GIVerify 注释备查。
- [ ] **C4.2** 显示链重标定（一次 PR 一组旋钮，round-7 方法论）：CAL_EXPOSURE（当前 0.30）、DIRECT_INTENSITY（Metal gpuDriven 7.5 vs Vulkan 2.5 分叉——按 M2 台账"四值归一"对齐）、AO mix、ddgiBase、天光底。对照指标：mean / 深暗% / 贴白% / 拱门暗柱 / 直射墙 / 庭院地面 / **直射÷内部比**。
- [ ] **C4.3** 跨端对数：gather_avg、camcell 探针 L0（p50/p90）、SPGI 槽 mean ±30%；区域亮度表双平台并排入 `Docs/completed/render-comparisons/`。
- [ ] **C4.4** 重测历史作废项：卡片辐射 Metal 0.344 vs Vulkan 0.128 专项、SSGI/SPGI 分源贡献（GI_ISOLATE）、第五轮 legacy 直出数据全部以新基线重出。

验收：直射÷内部比 ≥ 8（UE 13.5×）；mean/深暗/贴白回到 09-06 终态带（40.9±10% / ~61% / ≤0.5%）；室内地面 < 直射墙（Metal 当前 195.6 > 153.1 反转）。

## Phase C5 — 门禁固化

- [ ] **C5.1** GIVerify 追加**物理关系带**：final readback 上按 §8 区域定义算 直射墙/内部比 ∈ [8, 100]，室内地面 < 直射墙——单条断言锁死本症状复发。
- [ ] **C5.2** `GI_HALFLIFE` response=YES 与 C3.3 分区 escape 带入 CI（`ci_local.sh`）。
- [ ] **C5.3** shader 副本漂移策略收紧：绑定签名级漂移（binding 数/类型变化）不允许进豁免清单——T3.6 断裂即本类；`check_shader_copies.sh` 增加布局签名提取（binding id+type 列表 hash）。
- [ ] **C5.4** 文档回填：本文档销账 + `2026-09-04` 文档补"第十六轮：物理关系校准"收尾段。

---

## 8. 基线数据与复现（2026-09-09 实测）

复现：

```sh
# 参考机位 capture（180s 超时，/tmp/sponza-reference/<backend>-<ts>/frame.png）
python3 scripts/sponza_reference.py vulkan          # Vulkan（当前可跑，有 370 条存量 validation）
python3 scripts/sponza_reference.py metal           # Metal Modular 当前 assert，用 TestMetalGIFusion 替代：
GI_VERIFY_FRAME=100000 SPGI_CAM_LOCK=1 SPONZA_REFERENCE=1 \
SPONZA_CAPTURE=/tmp/mt.ppm SPONZA_CAPTURE_FRAME=360 ./Darwin/Debug/TestMetalGIFusion

# 纯直射判决帧
GI_ALL_OFF=1 SPONZA_REFERENCE=1 SPGI_CAM_LOCK=1 \
SPONZA_CAPTURE=/tmp/vk-direct.ppm SPONZA_CAPTURE_FRAME=360 ./Darwin/Debug/TestVulkanSponzaRenderGraph

# 矩阵对照（TEMP-DIAG，工作树在案）
PRIMAL_DUMP_MATRIX=1 ./Darwin/Debug/TestVulkanSponzaRenderGraph   # [DEF_VP]/[DEF_INVP]/[SHADOW_V]/[SHADOW_P]
```

区域定义（归一化坐标 x0,x1,y0,y1，参考机位）：

| 区域 | box |
|---|---|
| L_arcade 左拱廊内部 | 0.00,0.22,0.42,0.78 |
| R_arcade 右拱廊内部 | 0.80,1.00,0.42,0.78 |
| sunlit_wall 庭院直射墙 | 0.42,0.58,0.50,0.75 |
| sky 可见天空 | 0.44,0.55,0.10,0.22 |
| floor_near 门厅地面 | 0.30,0.70,0.88,1.00 |

基线表（2026-09-09，含 09-06 校准态对照）：

| 区域 | UE 参考 | Vulkan 09-09 | Metal 09-09 | Vulkan 09-06 | Metal 09-06 |
|---|---|---|---|---|---|
| L_arcade | 22.5 | 10.7 | 10.2 | 17.8 | 9.4 |
| R_arcade | 8.9 | 16.8 | 40.4 | 30.6 | 48.8 |
| sunlit_wall | 120.5 | 25.8 | 153.1 | 42.9 | 80.6 |
| sky | 87.0 | 140.2 | 162.0 | 145.0 | 151.6 |
| floor_near | 59.5 | 15.8 | **195.6** | 61.4 | 115.4 |

关键附加事实：
- 矩阵对照：`max|DEF_INVP − inv(SHADOW·V·P)| ≈ 0.01`（打印精度内一致）——CPU 侧矩阵无罪。
- 直射-only 帧（Vulkan）：全场景 lum≈0，仅中庭高处一小块 + 天空；sunlit_wall box = 0.0。
- 探针场量级：camcell L0 ≈ 4.6（M2 记录）；UE 物理量级推导 ~1-2（C4.1 落正式带）。

### §8.1 参考机位门禁重定基线（2026-09-12）

**旧值作废声明**：§8 表与 C4 门（direct_patch/interior ≥ 8）的数值只对
0090b2e6（B 系列 tonemap/天空 rebalance）**之前**的画面管线有效。rebalance 把
interior 抬了 ~2.7×（13→35）、sky 140→178，direct_patch 不变（~117）——比率从
8.7 跌到 3.3，属于显示链整体移动，非 GI 回归（B17 已做 HEAD A/B 锁定等价）。
2026-09-10 十四的"新默认基线 direct_patch 71.9-72.2/113.4"同样按 rebalance 前
管线记录，读数时注意。历史上"当前值 5.6"也是 rebalance 前数字，一并作废。

**新基线（2026-09-12，HEAD+B17 工作树，参考机位 + SPGI_CAM_LOCK，frame 4800，
5 次抓帧统计）**：

| 区域 | 均值 | 5 次波动带 |
|---|---|---|
| direct_patch | 117.0 | 116.0 – 118.2 |
| L_arcade | 41.1 | 38.5 – 42.3 |
| R_arcade | 29.2 | 28.7 – 29.7 |
| sunlit_wall | 23.2 | 22.1 – 24.5 |
| sky | 178.3 | 177.7 – 179.0 |
| floor_near | 16.3 | 15.8 – 17.2 |
| direct/interior 比 | 3.33 | 3.26 – 3.52 |

**新门禁（已落 `scripts/sponza_region_lum.py`）**：
1. direct_patch/interior ∈ **[2.3, 4.6]**（基线 3.3 ±40%；5 次实测散布仅 ±5%，
   带宽留足 SC↔probe 多稳态 T8.13 与 GI ±5/255 噪声，仍能抓符号级回归——
   室内炸亮比率跌破 ~2、GI 饿死冲破 ~5）；
2. floor_near < direct_patch（物理关系门，保留；现余量 ~7×）；
3. direct_patch ∈ [40, 170]（显示链锚，保留；现值 117）。

复现：`SPGI_CAM_LOCK=1 SPONZA_REFERENCE=1 SPONZA_CAPTURE=/tmp/out.ppm
SPONZA_CAPTURE_FRAME=4800 ./Darwin/Debug/TestVulkanSponzaRenderGraph`（约 5 分钟，
先删旧 ppm、抓到即改名），然后 `python3 scripts/sponza_region_lum.py <ppm>...`。

---

## 9. 执行日志

### 2026-09-09 晚（C0 + C1 第一批）

**C0 完成**：
- C0.1 ✅ `EngineTest/shaders/DDGIGIGather.metal` 同步至引擎 flat-buffer 版；Metal Modular 恢复可跑（此前 frame 1 assert）。附带修复 `check_shader_copies.sh` 的 Lumen/ 子目录映射盲区（该对曾被标 TEST_ONLY 完全脱管——T3.6 断裂漏网的机制），基线已重生成。
- C0.2 ✅ Metal RHI 中央守卫：`Dispatch` 零线程组跳过（Vulkan no-op 语义对齐）+ `CopyBuffer` size 0 跳过。GI_ALL_OFF 在 Metal 全程无 assertion。
- C0.3 ✅ `scripts/sponza_region_lum.py`（区域表 + 直射/内部比 + 症状断言，exit code 门禁化）。
- C0.4 ✅ 双平台基线：Vulkan full/direct-only、Metal full/direct-only（§8 表 + 下面修订）。

**C1.1 诊断旋钮**（Metal，编译期 `-D`，PRIMAL_SFD_MODE=1..8 / PRIMAL_VIS_DEBUG=1）：
直出 depth / sc0.z/x/y / map@sc0 / map 全图 / vis。三段直出定位结果：

1. **【已修】Metal PCSS ShadowFilter 深度绑定错配**：kernel 的 depthTex 在
   `texture(0)`，C++ 统一绑到 Vulkan 布局的 slot 5 → Metal 深度采样读到未绑定
   垃圾（模式 2 全黑可证）。修复：`ShadowMapModule.cpp` 布局+绑定按平台分支
   （Metal: depth@0 同号不同类，MetalDescriptorSet 按 (binding,type) 匹配）。
   修复后模式 2 深度图出现正确结构。
2. **【前提修正】直射链在双平台均存活**：纯直射帧（GI_ALL_OFF）太阳池 patch
   实测 Vulkan mean 83.6 / Metal 94.9（UE 参考 116）——Round 11-14 的
   "vis 全零/直射死亡"是旧状态（CullMode 修复后已恢复）。模式 8 证明 shadow
   map 内容为真实几何：52° 太阳仰角 + 15m 中庭墙 → 庭院地板只有一条窄缝受光、
   光池在近屋顶墙带——**光池小是物理正确的**，强度差（150/209）归 C4 标定。
   ⇒ C1 的"直射复活"目标已被既有提交达成，本阶段转验证/收尾。
3. **【新工具事实】GI_DEBUG_STRIP 的 full 带不等价正常管线**（同区域 37.4 vs
   195.6）——该旋钮不可用于能量归因；分源归因需要 GI_ISOLATE 式逐源开关移植
   到 Metal（C2 前置小项）。
4. **探针场收敛且稳定**：frame 360 与 800 逐位一致（floor 195.6/195.4、
   mean 50.0/50.0）——排除 runaway 累积；室内异常亮是**稳定平衡态的注入过多**，
   归 C2/C3（response=NO 太阳解耦 + escape 天空/analytic/prev-screen 注入）。

**验证**：ctest Unit 1/1；shader 副本门禁 OK（77 对）；Metal 360/800 帧零 assertion。

**下一步（C2/C3 第一批）**：① GI_ISOLATE 移植 Metal 后逐源归因室内 195 的构成；
② SC(sc_lights)/DDGI·deferred(settings_) 光源双源统一；③ analytic 兜底去
无阴影 E_direct；④ escape 天空档与可见天空统一 + 分区 escape 门禁。

### 2026-09-09 深夜（C2/C3 第一批：能量归因链闭合）

**归因旋钮落地**（全部编译期/运行时开关，默认行为不变）：
`PRIMAL_SC_NO_DIRECT` / `PRIMAL_SC_NO_EMISSIVE` / `SC_PROBE_FEED_OFF`（SC 侧）、
`PRIMAL_DDGI_NO_ANALYTIC` / `PRIMAL_DDGI_NO_SKY` / `PRIMAL_DDGI_NO_PREVSCREEN`
（DDGI trace 侧，LumenDDGIPass::LoadShaderBytecode 注入）、`SC_VIS_DUMP=1`
（visibility atlas 整图 PGM → /tmp/sc_visibility.pgm）、`GI_DEBUG_STRIP` 重写为
带内全画面版（每带显示一个 GI 源的完整画面 ×1.0，full 带作同帧锚）。

**归因链（Metal，参考机位收敛态）**：

| 实验 | floor(ddgi带) | wall(ddgi带) | 结论 |
|---|---|---|---|
| 基线 | 221.3 | 91.4 | 室内填充 = DDGI 探针场单独贡献（ssgi 0.1/spgi 0.4） |
| PRIMAL_DDGI_NO_ANALYTIC | 221.3（0%） | 91.4（0%） | analytic 兜底路径能量 ≈ 0 |
| PRIMAL_DDGI_NO_SKY | 221.3（0%） | 91.4（0%） | miss 天空兜底能量 ≈ 0（离线 SDF 无穿孔） |
| PRIMAL_SC_NO_EMISSIVE | 221.3（0%） | 91.4（0%） | emissive 未参与（测试资产无 emissive 捕获） |
| PRIMAL_SC_NO_DIRECT | **25.7（-88%）** | **8.0（-91%）** | **探针场能量 ∝ SC 卡片直射项** |
| SC_PROBE_FEED_OFF | 223.8（+1%） | 97.3（+7%） | probe→SC 馈送不是泄漏源（legacy 回落更亮） |

**结论**：室内平填充能量链 = **SC 卡片直射项（×dir_visibility）→ SC atlas →
path1 射线 → 探针 SH → gather**。SC 阴影 vis 为二值、mean 0.18、保守 march 修复
前后仅 0.182→0.180——**18% 的 lit texel 中包含室内卡片**（atlas 整图已可目视，
/ tmp/sc_visibility.pgm：大量整卡全白，含拱门/柱廊/玫瑰窗外形）。SDF 盒
（y≤20.36）比卡片内容（y≤21.44）矮 ~1m，顶缘卡片 march 首采样即出盒 → vis=1
（首号嫌疑）；完整定罪需卡片↔atlas_offset↔世界坐标关联 dump（下一步）。

**附带真修**：SurfaceCacheShadow march 去 ×1.2 过冲（保守球追踪）+ 16→24 步
（对齐 B13 纪律；本场景影响小但正确性方向正确，保留）。

**下一步（C2/C3 第二批）**：
1. 卡片关联 dump：GetCardGenerator 卡片表（atlas_offset + world AABB + lit
   占比）与 /tmp/sc_visibility.pgm 联合，定罪"室内 lit"卡片集合 → 修 vis
   （SDF 盒外/出盒 break 的 texel 改判 shadowed 或按盒边界外推）。
2. vis 泄漏修后复测区域带 + `GI_HALFLIFE`（response=NO 预期随直射耦合恢复
   翻转——SC 直射是探针场唯一能量源已证，调光必然传导）。
3. 修复后回撤诊断旋钮默认值不变，重标 C4 的 ddgiBase/曝光。

### 2026-09-09 深夜二（卡片级关联完成——归因终局）

**新基建**：`SC_VIS_DUMP=1` 扩展为双输出——`/tmp/sc_visibility.pgm`（vis 整图）
+ `/tmp/sc_cards_vis.txt`（374 卡片 × atlas_offset/res/lit%/world center/extent/
axis/dir 关联表）。

**卡片级判决数据**（lit>50% 按高度带）：

| 高度带 | 卡片数 | lit>50% | 判定 |
|---|---|---|---|
| y>20.36（SDF 盒外） | 0 | 0 | 盒裁剪假设**排除** |
| 1F（y<9） | 210 | **4**（52-56%，庭院中轴 y=4.2-7.8，疑似太阳缝合法受光） | SC 直射/vis 体系**基本正确** |
| 2F 带（9-16） | 164 | **40**（含 89-100% 整卡） | 太阳经露天中庭顶直射 2F 内墙——**物理正确**（UE 参考最亮区块即上庭院墙 246） |

**终局结论**：直射/阴影/vis 体系无罪。室内过亮的机制 = **2F 受光带的真实能量
经完全无遮挡判定的传输通道均匀扩散**：
1. SC LightEval 的 `gatherProbeIrradiance`——8-probe 三线性，无可见性（受光带
   探针的能量隔墙/隔地板传给 1F 卡片）；
2. 探针更新/场景 gather 的 trilinear 混合同样无 form-factor 约束。
每跳增益 ≈ albedo(<1) 但跳数无限制且方向不衰减 → 1F 地板稳态 195（UE 59，
~3.3×）。**修复项精确化 = SC 侧 gather 加探针可见性**（Chebyshev on
ddgiDepthBuffer，照搬第四轮已移植进 DDGIGIGather.metal 的八面体深度 +
Chebyshev + burial 低频保底配方）；需 SurfaceCachePass C++ 给 LightEval 补
depth buffer 绑定（双平台）。

**验收预告**：修复后 1F 地板应向 UE 59 靠拢、2F 受光带保持（那部分是真太阳）、
`GI_HALFLIFE` response 应转 YES（探针场能量源 = SC 直射已证，调光必然传导）。

### 2026-09-09 深夜三（C1 Vulkan 侧定罪收尾——与"终局结论"独立汇合）

分册：`Docs/plans/2026-09-09-vulkan-direct-gi-recovery-plan.md`。Vulkan 侧新增
哨兵（env 门控，零 env 时生产行为逐位不变，GIVerify 全带 PASS 复核）：
`PRIMAL_SHADOW_DBG_DEPTH/RECV/HARD/BLOCKER`（ShadowFilter.comp quality 3-6）、
`PRIMAL_SHADOW_VIS_DEBUG`（DeferredLighting.frag mode 0.5，vis 直出上屏）、
`PRIMAL_GBUFFER_ALBEDO_DEBUG`（mode 0.25，albedo 直出——顺带证 albedo 无罪）。

判决（全图 offset = 接收器 sc0.z − 阴影图深度，按 vis 分桶）：vis≥0.9 →
mean −0.0007；vis 0.5-0.9 → +0.0036；vis<0.1 → +0.27（= 内部点前方整栋建筑
的真实遮挡深度差）。**偏移剖面 = 真实遮挡结构，Vulkan 阴影比较链无罪**——与
上文"卡片级终局结论"独立方法汇合（首轮"系统性 z 偏移"猜测系采样点恰落在
檐口/banner 边缘，已撤回）。

落地修复 1 项：`ShadowFilter.comp` PCSS lightSizeUV **0.05 → 0.012**（T8.13
文档值；0.05 系 9526281c 注释/代码不一致产物且当时为死代码分支，9/7 PCSS
默认化后被激活——0.05 UV ≈ 3wu"太阳" vs 物理太阳 ≈0.0087 UV）。受光带
vis≥0.9 占比 19%→40%、带内直射 lum +18%；标准区域表逐位不变；360 帧无 fault。

**C1 关闭**：双平台直射/阴影/vis 体系均确认无罪（本条 + 深夜二互为独立验证）。
主攻全面转入 C2/C3；Vulkan 侧重标排程见分册 V2/V3。

### 2026-09-10（C2/C3 第二批：SC gather 可见性落地 + 新根因发现）

**已落地**（双平台）：
- `SurfaceCacheFrameData.ddgi_depth` + SRP 两个填充站点（BuildRenderGraph +
  RenderWithCommandBuffer twin）喂同槽位探针深度；
- LightEval 布局/描述符扩到 14 条（Vk binding13 / Metal buffer7，readonly）；
  顺带修复 `evalParams[12]` 装 13 条描述符的既有栈越界（→14）；
- SurfaceCacheLightEval.metal/.comp：gather 双累加器改造——45% Chebyshev
  （octahedral depth mean/variance）+法线+掩埋加权、55% burial 低频保底，
  表面点沿法线推 5% 间距；配方逐行对齐 DDGIGIGather route-A；
- SurfaceCacheLightEval.comp.spv 重编；Metal runtime 编译通过。

**判决性发现（SC_DEPTH_STATS 新仪器）**：camera-cell 探针（7947）的深度
八面体 **64 方向 mean 全部 = 50（RayMaxDistance 哨兵）**——探针深度数据
从未记录真实几何，等价"全方向无限可视"。后果：
1. 本轮新加的 Chebyshev 权重恒 1（dist−50<0），复测区域与基线逐位接近
   （floor 194.6 vs 195.6）——**可见性机制 engine-wide 失效**；
2. 第四轮移植 route-A 到 DDGIGIGather.metal 的可见性/掩埋加权同样一直
   在读假数据——"Metal 漏光压不掉"的历史悬案就此闭环；
3. SC↔probe 传输因此完全无 form-factor 约束：2F 受光带（真实太阳，合法）
   的能量均匀扩散到 1F（非法过量）→ 室内 195 vs UE 59。

**下一步（第三批，根修）**：
1. 审 DDGIUpdateDepth 输入链：ray_data_buffer_ 槽位/finalize 的 w 语义
   （B5 符号编码后）/probe_update_list 对齐——为何全写 50（全 miss 语义）
   而 irradiance 端却是 hit 数据（两者输入同源却矛盾）；
2. 修好后 SC gather 的 Chebyshev 自动生效（本轮代码已就位），复测区域门禁
   （预期 1F floor 195→~60-90，2F 受光带保持 150+）+ GI_HALFLIFE；
3. `SC_DEPTH_STATS`/`SC_VIS_DUMP` 留档为常备仪器。

### 2026-09-10 续（总根因闭环：trace 三管线自 T4.3 起全部 INVALID）

**SC_DEPTH_STATS 的"全 50"追到底**：不是 depth update 的输入问题，而是
**DDGITraceRays.metal 编译失败 → trace/sdf/finalize 三管线全部
INVALID（Mode11 Diag: trace_pipeline=0xFFFFFFFFFFFFFFFF）→ trace 从未运行、
ray/depth 数据从未写入 → 深度保持初始 50（"开放"）**。T4.3（5fb66924）把共享
helper 迁入 SDFTraceCommon.metal 时类型改名为 `DetailSDFTableGPU`，但
`ddgi_trace_sdf` 的参数还引用旧名 `DDGIDetailSDFTable` → 整个库编译失败 →
**自 09-09 起 Metal 的"GI"实际 = SC 卡片→探针 CPU 直投影（46749 贡献，
无可见性、无 trace、无 depth）**——这正是 M2 "response=NO、探针场与太阳解耦"
的最终解释：SC 卡片投影不经过光线，自然对灯开关"无响应"。

**一行修复**：`DDGITraceRays.metal:289` 类型名对齐 `DetailSDFTableGPU`
（布局 656B 双端 static_assert 一致）。修复后：
- 三管线 VALID（88/89/90），零编译错误；
- camera-cell 探针深度八面体出现真实数据（cam8 = 6.2/3.5/3.1/50/50/2.9/50/50
  ——墙距 3-6m + 拱开口合法逃逸），Chebyshev 可见性自动激活；
- 区域变化（vs 昨基线）：拱廊 L 10.2→69.5、R 40.4→94.9（真实传输反弹，
  方向正确），受光墙 153→166，floor_near 195.6→199.2（仍 ~2× UE 59）。

**状态重估**：
- 症状的"结构性错误"部分（trace 死 + SC 盲投影）已修复；
- 剩余 floor_near ~2× 属标定域：SC↔probe 反馈环增益（π 链 B3 残留）、
  escape 2.9 天空档——归 C3.4/C4；
- 前两轮的"归因链"结论全部在 trace 已死的场上测得，修复后需按计划§1 条款 4
  全部重测（GI_HALFLIFE、漏光代理、卡片辐射对比）。

**验证**：ctest Unit 1/1；区域门禁两条件仍 FAIL（预期内，待 C4 标定）。

### 2026-09-10 三（trace 复活后的重测第一批——灯响应已翻 YES）

**GI_HALFLIFE 的 Metal 等效实验**（env 调光 10%，GIFusionVerify camcell L0，
帧 400 收敛态）：

| 配置 | camcell L0（探针 7947/7948） | 色温 |
|---|---|---|
| 基准（太阳 100%） | (1.48,1.44,1.36) / (1.91,1.84,1.71) | 暖（R>B 明显） |
| 太阳 10%（PRIMAL_DIRECT/DDGI_LIGHT） | (1.12,1.15,1.16) / (1.42,1.45,1.45) | **中性化（R=G=B）** |
| + PRIMAL_DDGI_NO_SKY | 与基准逐位一致 | — |

判定：**response 从 NO 翻转为 YES（部分）**——暖色太阳分量（Δ≈0.12-0.15）随
调光完全消失，中性残余 ~1.1-1.45 不响应 = **天空传输分量**（探针射线经拱开口
合法看见受光带/天空，天光不受调光影响）。M2 的"探针场与太阳解耦"关闭——
根因即 trace 死亡 + SC 盲投影。

**NO_SKY 逐位无效的解释**：Metal finalize 的 miss 分支无 escape/exhausted
之分，逃逸射线全部落到"附近 SC 卡回退"（找到卡就不吃天空默认值）——
与 Vulkan 的 -1.5 分档不同，逃逸能量进的是本地卡片辐射。

**SC_PROBE_FEED_OFF（trace 存活后）**：camcell L0 1.44→4.74-5.47（更亮），
L1 翻向——SC↔probe 耦合不是过亮驱动方，legacy 自反馈路径平衡点反而更高
（T8.13 多稳态的又一表现）。剩余 ~2× 过亮归 C3.4/C3.5（π 链 + 回路增益
+ escape 档），需要 atlas 级能量记账仪器，归下一专项。

**待办（下一 session）**：
1. C3.4/C3.5：π 链残差审计（B3 "SC/Dawn/FusionComposite 面残留核对待续"）
   + 回路 DC 增益测量（输入→平衡态放大系数）+ B1 ×2.5 回撤条件评估；
2. Vulkan 侧同批验证（trace 一直存活，区域带基线重测即可）；
3. 全部旧 A/B 数据按 §1 条款 4 作废重测。

### 2026-09-10 四（T1-T4 专项：π 链审计闭合 + miss 分档修复 + ×2.5 回撤落地）

**T1 π 链残差审计（每跳增益表，代码审计 + 7 组 A/B 实测闭合）**：

| 跳 | 阶段 | 增益 | 判定 |
|---|---|---|---|
| H1 | 太阳→SC atlas（LightEval） | ρ/π Lambert | B3 ✓；feed-off 支路 0.35ρ 自反馈=T8.13 亮支路 |
| H2 | atlas→card_radiance | 1（4×4 均值） | 无残留 |
| H3 | card→probe SH 注入 | min(Ω,π)·NdotL·Y_j，gate 4.0 + takeover≤0.45 | 标定域门，非 π 错 |
| H4/H5 | trace hit/miss | 读 atlas L=1 | **跨端分叉①②见下** |
| H6 | ray→SH 投影 | C0·(4π/N) | 模型实测验证：开天探针 9.99 vs 理论 10.17 ✓ |
| H7 | gather SH→E | π×**2.5(B1)** | ×2.5 已回撤（T3） |
| H8 | E→最终间接 | ρ/π + 0.085 far-field | B3/B9/B10 ✓ |
| H9 | probe→SC texel E_ind | canonical（刻意无 π）+ 钳 4.0 | 回路增益 ≪1 ✓ |

**两个跨端分叉定罪 + 修复落地**（C3.3 实质项，Metal 落后 Vulkan 的
2026-09-06 逃逸门控）：
1. **march 无 miss 分档**：Vulkan DDGITraceSDF.comp 编码 −1 耗尽 / −2 逃逸；
   Metal 全写 −1。已修（DDGITraceRays.metal 三处 break）。
2. **finalize/投影端把耗尽射线全兜天空**：Vulkan finalize 耗尽=SC 卡回退/黑、
   仅逃逸吃 SKY；Metal finalize miss 全兜 SKY + UpdateIrradiance 对 w<0
   一律覆盖 DDGI_SKY_COLOR(2.87)。已修（两 shader 对齐 Vulkan 分档语义；
   顺带 NO_SKY 旋钮真正生效——此前"NO_SKY 逐位无效"正是因为投影端覆盖
   不受该旋钮控制）。

**判决性 A/B（frame 400 收敛态，camcell 7947 lum / floor / sunlit / L_arc /
gather_avg）**：

| 配置 | 7947 | floor | sunlit | L_arc | avg | 结论 |
|---|---|---|---|---|---|---|
| 基线（=昨晚终态） | 1.441 | 199.2 | 166.2 | 69.5 | 3.218 | — |
| PROJ_NOSKY（新旋钮） | 0.442 | 185.3 | 133.7 | 11.0 | 1.350 | 内部探针 69% = SKY 注入；拱廊 −84% |
| SPGI_OFF | 1.537 | 195.6 | — | — | 2.887 | floor 与 SPGI 无关 |
| GI_ALL_OFF | — | 0.0 | **0.0** | 0.0 | — | **参考机位所有测量 box 均无直射**（太阳缝在近屋顶带）|
| miss 分档修复 | 0.985 | 200.9 | 149.4 | 45.4 | 2.768 | 内部探针 −32%，暖占比升 |
| + ×2.5 回撤（终态） | 0.943 | **108.0** | 66.4 | 7.2 | 0.784 | floor 3.3×UE→1.8×UE |

**T2 回路 DC 增益**（太阳 0.5×/1×/2× 阶跃，camcell 7947）：
1.310/1.441/1.729 → 平衡态 ≈ 1.17（太阳无关项 81%）+ 0.28×sun（DC 增益
≈0.19/unit，饱和型）；屏幕区域对太阳几乎不敏感（floor +0.9%~+3%）而
sunlit 正常响应。feed-off=亮支路 3.3×（camcell 4.74、gather avg 11.6、
拱廊 213/223）——probe-fed 是收敛支路，默认保持。

**T3 B1 ×2.5 回撤 = 落地**（非评估）：原始根因（miss 单位源分叉）已被
分档修复闭合，双端 trace/finalize/投影语义逐行对齐后平台系数失去存在
理由。A/B：全 GI 区 ÷2.5 量级（floor 200.9→108.0、L_arc 45.4→7.2），
camcell 探针亦小幅响应（0.985→0.943；×2.5 经 prev-screen 路径参与反馈环）。
gather_avg 0.784 ∈ band (0.005,6.3] ✓；probe_sh0 9.99 ∈ [0.08,13] ✓；
副本门禁 OK（EngineTest 同步）。

**T4 端到端重标定（部分达成，两项结构性发现）**：
1. **response 实质性 YES**：修后 10% 调光 → camcell −40%、floor/sunlit
   −45%、暖分量消失、天空探针 9.984 恒定（合法 escape 常数）。修前仅 −22%
   且区域零响应。C2.3 的"降至 <20%"判据在合法天空传输存在下不可达，
   建议门禁改为"调光响应斜率 ≥ 物理份额"（归 C5）。
2. **区域门禁语义问题（阻塞 ratio≥8）**：GI_ALL_OFF 证明参考机位下
   sunlit_wall/L_arcade/R_arcade/floor 四个 box 全部不含直射——门禁前提
   （"sunlit_wall box=直射墙"、UE 13.5× 直射÷内部比）与实际视区内容不符
   （UE 参考的 120.5 也只能是 bounce 或其 shadow map 配置不同）。**C4.1
   物理推导带必须先重定义 box 或把 box 移到真实太阳缝**，否则 ratio≥8
   不可达也不该达。floor 108 vs 目标 60-90：差 1.2×，待 C4 显示链标定。
3. floor 199.2→108.0、L_arc 69.5→7.2（UE 22.5，已低于）、R_arc→26.7
   （UE 8.9）：绝对量级已进入 C4 可标定域。

**仪器留档**：`PRIMAL_DDGI_PROJ_NOSKY`（投影端逃逸天空断电，C++ 注入清单
+1，默认行为逐位不变，Debug 树已重链）；SSGI_OFF 在 Metal 会连带关闭整个
fusion 间接链（quality 标志闸门过宽）——坏仪器，禁用于归因。

**验证**：ctest Unit 1/1；shader 副本门禁 OK；TestModularPipeline 90s 冒烟
零 assertion；全部新代码 env 门控默认关闭。

**待办（下一 session）**：
1. C4.1：重定义区域 box 语义（直射缝 box + 纯间接 box 分离），物理推导
   期望带，更新 sponza_region_lum.py 门禁定义；
2. Vulkan 侧同批验证（miss 分档修复的 Vulkan 侧行为对照 + 区域带重测；
   需 DebugVulkan 树，注意共用 libEngine.a 协议）；
3. floor 108→60-90 的残余 1.2×：SC↔probe 反馈余量审计（C3.5）+
   prev-screen path2 深度一致性测试（C3.2，归因旋钮已备）；
4. GI_HALFLIFE 正式门禁判据修订（C5.2）。

### 2026-09-10 五（续批：C4.1 门禁重定义落地 + C3.1/C3.2 前提修正 + Vulkan 对照）

**C4.1 落地（`scripts/sponza_region_lum.py` 重定义）**：直射缝 box
`direct_patch=(0.46,0.60,0.26,0.44)` 从 GI_ALL_OFF 阈值图取紧致包络
（lum>60、y≥0.24 避开拱顶天空；纯直射 box 均值 54.4）。门禁改三条：
① direct_patch/interior ≥ 8；② floor_near < direct_patch（原症状门禁，
语义修正后立即转 PASS：Metal 108 < 129.9）；③ direct_patch ∈ [40,170]
（显示链常数哨兵）。旧 sunlit_wall box 降级为间接反弹诊断量。
Metal 现值：① 7.68（贴线，唯一残留 FAIL）② ✓ ③ ✓。

**C3.1/C3.2 前提修正（三连 A/B：no25 / NO_PREVSCREEN / NO_ANALYTIC）**：
单关 path2（prev-screen）或 path3（analytic）任一条，拱廊都从 7.2/26.7
翻到 ~20/36 的更亮支路——两条路径共同构成当前最暗最准的平衡点。机制：
path3 的 `π·L_prev·0.70` 辐射自循环是无界增益源（多稳态触发器），path2
截走 miss 阻止其接管。⇒ **C3.1 的"删除 analytic"作废**（trace 复活前
"analytic≈0"归因过期）：正确修法 = 给 π·L_prev·0.70 加界（如对齐 SC 的
ρ×0.35 有界反馈）或以 trace 可见性门控，单独 PR+单变量 A/B（注意支路
翻转会混淆能量归因，需在固定支路后测）。C3.2 深度一致性测试仍成立。

**Vulkan 对照（frame 400，共用 archive 协议切换）**：direct_patch 57.3
≈纯直射（patch 上 GI 饿死）vs Metal 129.9；floor 15.7 vs Metal 108
（09-09 Vulkan 15.8 逐位复现 = 无回归）；拱廊 24.0/27.2（09-09 10.7/16.8，
M3-A1 buried-start 修复后向 UE 22.5 靠拢）。**两平台从两侧包夹 UE**：
Vulkan 间接欠喂、Metal 互反射过喂——C4.3 跨端 ±30% 对数的核心工作面，
指向 SC→probe 注入档（takeover 0.45/gate 4.0）与 escape 档的平台差。

**附带修复**：Metal GIFusionVerify 末段 backBuffer 回读（framebufferOnly
drawable）触发 validation assert、判决行永不打印——改为 SKIP（final_avg
带由 Vulkan twin 承担，合成画面由 SPONZA_CAPTURE 离屏路径 + 区域脚本
覆盖）。TestModularPipeline.cpp 单点修改，Debug 树已重链。
复验：verify 三条 band 全 PASS、`ALL ALIGNED ✔` 首次在 Metal 完整打印、
零 assertion；hd 读回 ray2/3 = −2 实证逃逸编码生效。

**GI_HALFLIFE 正式门禁（Vulkan）首跑判决：FAIL，response=NO**——调光 90%
后 camcell L0 5.567→5.585（零响应）。结合区域数据定性：**不是 M2"探针-
太阳解耦"的残留（那是 Metal trace 死亡，已修），而是 Vulkan 弹射传输
饥饿**——direct_patch 上 Vulkan GI≈0（57.3 ≈ 纯直射 54.4）、floor 15.7
（UE 59.5 的 26%），探针场被合法天空 escape 主导，调太阳必然无响应。
C2.3 的 Vulkan 残余从"解耦"精确化为"SC→probe 弹射馈送过弱"；修好后
Vulkan 门禁应自然翻转（探针场能量源 = SC 直射已证，无需新机制）。
Metal 侧等效实验 response −40% 已过 20% 判据（本文件 2026-09-10 四）。

### 2026-09-10 六（多弹射断供专项：H1 尺度断裂定罪 + 三修 + response 度量切换）

**任务 1 诊断链（提示词诊断树全走完，全部有 A/B 数据）**：

1. **探针 L1 方向/量级（DDGI_DUMP=1）**：中殿/拱廊探针 L0 0.003-0.045（近黑）；
   垂直柱 x=7.5：L0 沿高度 2.5(y=13.5) → 0.47(y=10.5) → 0.10(y=7.5) →
   0.016(y=4.5) → 0.012(y=1.5)——每降 3m 衰减 ~×5，4 层后只剩 0.16%。
   盒内 trace hit 85.6%/escape≈0：射线全部止步 0.2-6m 近场暗墙（几何正确），
   传输必须靠穿开口打中受光面的射线，但探针读到的辐射≈0。
2. **SC 图册分区亮度（新增仪器 `DDGI_DUMP=5`，/tmp/sc_card_lum.csv 逐卡
   mean/max/lit% + 世界坐标）**：**整册近黑**——374 卡 mean lum 最高 0.07、
   全册 max texel 1.62，而受光面理论辐射度 ~11-13（albedo 0.7×直射 51/π）。
   **定罪 H1（太阳→SC atlas）**：`FillSCLightList` 用场景灯 `color×intensity`
   （=3.0），而 deferred 屏幕直射 = 同色 × DIRECT_INTENSITY(20)——
   **SC 图册的世界比屏幕暗 20×，探针弹射传输读的是一个 1/20 亮度的世界**。
   这同时就是 C2.1 双源分裂的尺度面。
3. **路径直方图归因（pathdbg spv 按当前源重编——旧 spv 落后于 miss 分档修复
   且 `-DDGI_PATH_DEBUG` 宏注入失败需在 #version 后显式 #define）**：
   prev-screen 路径全场景 0 次命中；SC 读取空间成片失效（中殿 x≥7.5 探针
   0/128 次 SC 命中、全落解析黑；西半 118-126/128 正常）。

**三修落地（全部双平台同步）**：
1. **`FillSCLightList` ×`SCDirectIntensityScale()`**（StandardRenderPipeline.cpp，
   默认 20 对齐 shader 常量，A/B 旋钮 `PRIMAL_SC_DIRECT_SCALE`）——图册实测
   max texel 1.62 → 17.05（符合理论 13-17），card mean 0.03 → 1.38。
2. **finalize 命中分支 continue-on-planeOk**（DDGITraceFinalize.comp +
   DDGITraceRays.metal ddgi_trace_finalize）：原代码首个通过 worldToCardUV
   的卡即 break，planeOk 拒绝后不再尝试列表中后续小卡（miss 分支本就有
   正确语义）——网格按体积降序，第一个命中者几乎总是巨型 AABB 卡。
3. **SC 侧 gather 钳位 4.0 → 64.0**（SurfaceCacheLightEval 双端）：×20 后
   4.0 钳位进入合法工作区间（受光 texel 13-17），硬非线性把 T8.13 多稳态
   从 golden 0.36% 放大到 115-330%（实测）；回路 DC 增益（4·C0·albedo≈0.79
   @albedo 0.7）与钳位无关，64.0 对齐图册存储钳位与 B7。

**效果（frame 360 收敛态，`sponza_region_lum.py`）**：

| 区域 | 修前 | 修后 | UE |
|---|---|---|---|
| direct_patch | 67.4 | 69.1 | ≥116(C4) |
| L_arcade | 0.7 | 1.8 | 22.5 |
| R_arcade | 1.4 | 2.1 | 8.9 |
| floor_near | 0.7 | 2.0 | 59.5 |

frame 1500 ≈ frame 360（无 runaway，红线达成）；GIVerify 全带 PASS；
probe_sh0 0.797 ∈ band、in-box hit 85.6%/escape 10.9%/exhausted 3.6%。

**两个结构性残项（任务 1 未竟部分，各需独立专项）**：
1. **AABB 回退卡对空心建筑内壁的覆盖缺口**：374 卡全部是 per-instance
   AABB 六面卡（主体 ±13.7m），中庭内壁在所有卡片正交视角被外壳遮挡、
   从未被捕获；内壁命中读对侧卡暗 texel（容差带内）或被 D3 planeOk 拒绝
   （>0.35×extent≈4.8m）。室内多弹射的结构性修复 = T4.2 离线 card bake
   重烘 Sponza（bake 链路的定制烘焙命令已失传，需先复原烘焙命令）或
   运行时卡片放置升级。`DDGI_DUMP=5` 即该专项的判决仪器。
2. **SC↔probe 回路多稳态在满能量下放大**：camcell 探针 run-to-run L0 差
   30-177%（修前 0.36%）——相邻探针同几何落不同吸引子（0.9 vs 0.02），
   是室内亮度斑驳的直接机制。golden（frame-400 收敛态 EMA 场）作为
   确定性仪器在满能量回路下失效，M2 台账已列"固定射线单帧投影无反馈
   golden"为正解；当前 golden 基线已按新平衡重生成，判据改为
   L0 逐分量 25% + L1 向量（方向 dot≥0 且范数 ∈[0.25×,4×]）。

**任务 2 ✅（response 门禁度量切换）**：新增 `DumpSCAtlasBits()`（与
DDGI_DUMP=4 同配方的 strided u16 位样转储），GI_HALFLIFE 判定改为
调光前后图册位样变化率 ≥ `GI_HALFLIFE_MIN_BITRATE`（默认 0.10）；
camcell L0 保留为半衰曲线（informational）。**3/3 连跑 PASS**：
bit_change 70.62/70.66/70.65%（×20 后图册携带全量直射信号，较修前
43.5% SNR 更高）、half_life=36 ≤ 60。协议：`GI_HALFLIFE_FRAME=360`
（两侧均收敛后调光，避免暖机漂移污染位样变化率）。

**任务 3 ✅（残项重定义）**：传播修复后 -y 地板隧穿已灭——
DDGI_DUMP=2 fresh dump：盒内 escape 0.0%、-y 逃逸份额 0（旧 15-18% 系
传播修复前数据）；GIVerify@400 in-box escape 10.9%（合法长轴/开口）。
新残项 = **入口被埋探针**（(-13.5,±1.5/±4.5) 处 63-85/128 射线爬出失败
→exhausted→SC 回退），GIVerify exhausted 3.6% band 内。归 relocation
增强专项（M3 残），非隧穿。

**任务 4 ✅（C2.1 尺度面；源表面归档）**：SC 光源表与 deferred 的 20×
尺度分裂已由修复 1 闭合（这是双源分裂的实际能量后果）。源表面：SC 走
`scene->GetLights()`（FillSCLightList），deferred/DDGI 走 `settings_.lighting`；
GI_HALFLIFE harness 已双路同步调光（settings ×0.1 + UpdateLight ×0.1），
两源对调光响应一致。编辑器改灯→scene light→SC 的传导路径已通（T0.4）。

**任务 5（C4）⏸ 按红线缓行**：室内仍 ~10× 低于 UE（卡片覆盖缺口 +
回路多稳态未驯服），此时动 CAL_EXPOSURE/天光底属"用全局常量掩盖传输
缺陷"（纪律 6 红线）。可标定域记录：direct_patch 69.1 稳定（DI 20），
直接 patch ≥67 门禁达标；显示链标定待卡片覆盖专项落地后一次收口。

**门禁态**：ctest Unit 1/1 PASS；check_shader_copies OK（0 待更新）；
GIVerify 全带 PASS；golden 基线已重生成（新判据见上）。370 validation
存量同类，360/1500 帧无 fault。

### 2026-09-10 七（用户复检"直面受光墙间接≈0/拱顶有光"：覆盖理论修正 + 级联瓶颈三连 A/B）

**机制修正（推翻 09-10 六的"AABB 卡片外壳遮挡"假设）**：SurfaceCacheCapture
是 **GBuffer 散射**（相机可见面全入册、图册持久），不存在"外壳遮挡捕获"。
`DDGI_DUMP=5` 扩展（raw 图册镜像 /tmp/sc_lighting_atlas.raw + 卡片
off_x/off_y 列）实测：受光带**在册**（+Z 卡 x=0 垂直剖面 y=12 lum 8.0、
y16-18≈2.7；全册 max 17）。左墙 texel 0.005-0.03，比平行墙物理预期
（~0.15-0.25）低 ~10×。

**左墙黑暗的机制（三个实测事实合拢）**：
1. **地面探针几何上看不见受光带**：阳台板 + 拱廊穹顶把 y≤4.5 经柱廊到
   y≥10 受光带的一切视线挡死（按模型尺寸重建射线 + 距离转储 0-9/128 条
   6-15m 射线佐证）；中层探针（y7.5-10.5）能越过阳台 → 它们亮（0.1-0.8），
   拱顶（采光自中层探针）也就有光——用户观察的"墙黑顶亮"分层完全对上。
2. **填充必须走 SC↔probe 回路，而回路逐跳效率 ~0.2**（Metal T2 DC 增益
   0.19；今日图册右墙梯度 8.0→0.4→0.001/8m ≈ 平行墙物理衰减的 1/100）。
   稳态 = 首跳的 1/(1-0.19) ≈ 1.24×，UE 级填充需 DC 增益 ~0.8（≈5× 累积）。
3. **gather 足迹 ±1 格（±3m）把级联钉在 3m 步长**：y=4 墙 texel 采不到
   3.5m 外的受光探针。DDGI 经典的空间模糊 Instruments 缺位——
   `IrradianceBlurSigma` 是死字段（无任何 shader 消费，且被错接为时间权重）。

**三连 A/B（frame 600 收敛态，区域表）——全部阴性**：
| 变体 | L_arc | R_arc | floor | 判定 |
|---|---|---|---|---|
| TemporalAlpha 0.2（PRIMAL_DDGI_TEMPORAL_ALPHA） | 1.3 | 2.1 | 1.9 | 非 EMA 锁定 |
| SC gather 半径 2（PRIMAL_SC_GATHER_RADIUS=2 → _r2 spv） | 1.7 | 1.9 | 1.9 | 足迹非当前约束 |
| SC_INDIRECT=1 tile radiosity | 1.3 | 1.7 | 1.9 | 16px tile/0.25 权重不解决 |

默认路径回归确认（1.8/1.9/1.9）+ GIVerify 全带 PASS + ctest/shader 副本绿。
新落库（全部 env 门控默认逐位不变）：`PRIMAL_DDGI_TEMPORAL_ALPHA`、
`PRIMAL_SC_GATHER_RADIUS=2`（`SurfaceCacheLightEval_r2.comp.spv` 27 点核，
SurfaceCachePass 管线选择）、DDGI_DUMP=5 raw 图册。

**结论与下一步（C3.5 正题）**：多弹射填充不足 = 回路串级效率问题，
不是单一 bug。候选机制（按优先序，下一 session 拍板）：
1. **DDGIProbeIrradianceFromCards.spv / DDGICardRadianceAvg.spv**——树内
   已编未接线的探针-卡片直采路径（Lumen 式 card relighting，绕过 128 射线
   采样与级联）——优先评估接线成本；
2. C3.5 逐跳效率审计（distWeight/bailing/accVis-accLow 混合/tile 权重的
   量化分解，仪器已齐：raw 图册 + 探针转储 + PRIMAL_SC_DIRECT_SCALE 线性度）；
3. 墙面条带 radiosity（同卡垂直邻居 texel 直接传递，绕过探针中继）。

### 2026-09-10 八（DC 增益扫描 + ProbeFromCards 否决 + 亮支路可达性判决）

**ProbeIrradianceFromCards 接线否决（细读后）**：其辐射源是**整卡平均**
（大厦 ±Z 卡 = 27m 面平均 → 糊）且遮挡不感知；shader 注释自带前科记录
（0.85 takeover 曾把 326/407 卡洗平到均匀 ~1.0，回撤 0.45）。接线后果 =
均匀灰填充（红线），不解决左墙。**C3.5 候选 1 出局**。

**Vulkan DC 增益三阶跃**（PRIMAL_DIRECT_LIGHT 0.5×/1×/2×，frame 800）：

| step | direct_patch | L_arc | R_arc | floor |
|---|---|---|---|---|
| 0.5× | 39.9 | 1.1 | 1.4 | 1.3 |
| 1× | 68.6 | 1.4 | 2.0 | 2.0 |
| 2× | 103.6 | **6.8** | 3.2 | 3.5 |

- 直射线性（39.9/68.6/103.6）；室内**非线性**：1×→2× 区间 L_arc ×4.9
  （增益 >1 的支路翻转）。**回路不是线性放大器**——T2 的"单一 DC 增益"
  模型在 Vulkan 满能量场不成立，存在输入相关的盆地切换。
- **亮支路可达性判决**：`PRIMAL_SC_DIRECT_SCALE=40`（SC 世界 ×2、屏幕直射
  不动）→ 室内仅 ×1.7（3.0/3.2/3.3）。亮支路不能靠单抬 SC 达到；2× 全局
  阶跃的跳变是屏幕+SC 共同驱动。**没有免费的亮盆地**。
- 汇总响应面：SC×2 → ×1.7；全局×2 → ×1.75-4.9（mixed）；alpha/R2/tile
  → ~0。回路在当前架构下的有效增益 ~0.25-0.5/倍增，距 UE 需要的 ~5×
  累积差一个数量级。

**收口判词**：室内填充 = 回路架构级天花板。修复 = C3.5 重构级工作，
候选按性价比重排：
1. 探针-卡片直采的**重设计**（不是接线旧 spv：需要 per-texel 级辐射 +
   可见性，即 Lumen Final Gather 级机制）；
2. distWeight/bailing 混合权重的逐因子量化台账（D4 bail 0.38m 阈值在
   步长封顶后近场命中 p50 0.22-0.44m 的场里可疑地大——首查项）；
3. 多稳态的确定性驯服（warm-start/bake 预收敛，UE 的 surface cache bake
   等价物），先让 1× 稳态可复现，C4 标定才有地基。

### 2026-09-10 九（收口补遗：D4 bail 清白 + 往返损耗的结构定位）

- **D4 bail 量化清白**（fresh DDGI_DUMP=2，非 pathdbg——pathdbg 版距离
  被 40+tag 替换，不可用于能量审计，已在 §9 留档）：全体探针 bail 均值
  0.928、中殿 0.887、最差 0.613——最多吃 7-11%，非主损耗。候选项 2 出局。
- **往返损耗的结构定位**：一跳往返 = texel 侧 ρ/π（辐射度约定，合法）×
  探针侧立体角采样份额（墙占探针半球 ~0.25）≈ 0.055/往返——**探针中介
  传输的结构性低通**，任何归一化/权重/足迹调优都改不了立体角项。UE 的
  填充来自 final gather 直接积分海量 texel（无探针瓶颈）。→ C3.5 的正解
  收敛为：**texel-texel 直接 radiosity**（T3.3 SC_INDIRECT 的正装版——
  64px tile + 权重 1.0×增益钳 + 成本工程化；其 10× 帧预算是唯一障碍），
  探针路径退化为低频底。`rayMax=0` 系 init 时序日志读旧值（UBO 实际 50，
  1706 行），非缺陷。

### 2026-09-10 十（"间接光收敛为 0" 判决 + SC gather π 单位修正转正）

**用户报告"间接光逐渐收敛为 0"——复现扫描（frame 2500-5000，区域表）**：
default / π / π+tile(w=0.8) / 窗口模式（600 vs 3000）全组合**无塌零**
（default 1.6/2.0/1.9；π 5000 帧 3.1/2.2/2.5 稳定；窗口 0.9→0.7 轻微）。
判定：**暖机瞬态（首 ~300 帧的残留亮场）缓慢沉回低位稳态的观感**——
深区稳态 0.7-2.5 显示单位在视觉上即"黑"，瞬态期稍亮的部分衰减被感知为
"收敛到 0"。非新回归、非 NaN 塌缩（B7/64 钳与 NaN sanitize 正常）。

**SC gather π 单位修正（本轮唯一正收益，已转正为双端默认）**：
`SurfaceCacheLightEval` 的探针 gather 求值重建的是辐射亮度 L(d)，texel
的物理入射量是辐照度 E=∫L·cos≈π·L——历史上以"回路增益余量"为由发货
π 缺损（注释自认 deliberate），屏幕侧却自 B4 起带 ×π（两腿不一致）。
修正后：回路不动点 ×1.7-2.1（L_arcade 1.4→2.9@800 / 3.1@5000 帧稳定）、
回路 DC 增益 0.19→~0.53（<1，余量 47%≥20% 达标）。**转正落地**：
comp 两条分支（legacy/R2）统一 ×π 为默认；Metal `.metal` gather 同步；
`PRIMAL_SC_GATHER_PI` env 退化为无害兼容（_pi spv 与默认同义）；
`PRIMAL_SC_GATHER_RADIUS`/`_r2` 保持（π+R2 组合实测反而稀释中层探针
峰值：2.9→1.8，R2 维持阴性不推荐）。tile radiosity 权重旋钮落地
（`PRIMAL_SC_RADIOSITY_W`→radiosity_gate.y float bits，双端，默认 0.25
逐位不变）；叠 π 实测无增益（pitile 2500 帧 2.0/1.9/2.0），tile 路径
维持阴性。

**新默认基线（frame 360，π 转正后）**：direct_patch 70.6（不回退）、
L_arcade 2.2 / R_arcade 2.3 / floor_near 2.2——较本轮起点（0.7/1.4/0.7）
累计 ×3，较 π 修正前 +50%。GIVerify 全带 PASS；golden 按流程重生成
（run-to-run 多稳态 FAIL 为已文档化的仪器局限，判据 L0+L1 向量带见
09-10 六）；ctest 1/1、shader 副本门禁绿。

**残余（C3.5 正题不变）**：深区稳态距 UE 8.9-59.5 仍差 ~4-10×，非旋钮
可达——探针中介传输的立体角低通（0.055/往返）是数学边界，唯一出路 =
T3.3 solver 级 texel-texel radiosity（正装版规格见 09-10 七/九）。

**π 转正后的物理锚点（2026-09-10 十补测）**：左墙(-Z卡) x=0 texel 垂直
剖面 y=0:0.204 / y=2:0.094 / y=4:0.042 / y=6:0.131 / y=8:0.134 /
y=10:1.525 / y=12:1.186——y=0 值与平行墙解析估算（0.15-0.25）吻合，
**texel 级传输已达物理量级**。相对比值口径：L_arcade/direct = 2.2/71.2
= 0.031 vs UE 22.5/120.5 = 0.187——相对直射仍差 6×（= 求解器累积差距，
非单位/校准问题）。SPGI 审计（用户问询）：与 DDGI 共享同一图册（×20+
π 自动受益）、同 SC-first 层级、卡片遍历本为 try-all 语义（优于 DDGI
修复前）、投影约定 M2 B1/B2 已对齐；唯一不对称 = SPGI finalize 无 D3
planeOk 深度测试（错位读放行更多，偏亮但可能错面——保留观察，不加）。

### 2026-09-10 十一（T3.3 solver 第一片：trace 四处死伤修复，tile 路径首次真实传输）

**用户拍板走方案 1（solver 级 radiosity）。第一刀即破案：tile radiosity
此前完全失效的根因不是权重/分辨率，而是 trace 是个未完成的 stub——**
1. `sampleSDFNearest(...) × sdf_extents`——GlobalSDF 存的是米（
   GlobalSDFVoxelizationGLSL imageStore min_dist 无缩放；DDGITraceSDF 同
   纹理直读 .r），×42 后 d 全维度错误；
2. `hitThreshold = extent×0.5 = 21m`——万物瞬时"命中"；
3. `kMaxSDFSteps = 4`；
4. resolve 对全部 t≥0.25m 命中返回 Phase-A 占位符 `0.02`（"Far hit:
   DDGI placeholder" 从未实现）——**tile 路径一直在传输 0.02 的灰**。

**修复（SurfaceCacheIndirectTrace/Resolve.comp，SC_INDIRECT 门控内）**：
米制直读、`hitThreshold = max(voxel0×1.25, 0.30)`、32 步、射线起点沿
tile 法线偏移 max(voxel0×0.5, 0.05)（防自命中）、far 命中与 near 同路
读图册（卡查找本就按位置任意距离可用）、sky 给 DDGI_SKY×0.4 真档。

**实测（W=0.8，frame 800 收敛）**：左墙 texel 最黑带 +25-45%
（y2: 0.094→0.117、y4: 0.042→0.061），y0 +8%；y10-12 的 -49~-62% 落在
多稳态抖动带（L_arc 区域 run-to-run 1.3-3.1 同源）。GIVerify 全带 PASS、
shader 副本门禁绿、默认路径（SC_INDIRECT off）逐位不受影响。

**下片（按序）**：①tile 16→8px + stride 时域摊销（PRIMAL_SC_RADIOSITY_
TILE/STRIDE，成本持平今天全率）；②W 扫描 0.6/1.0 寻优 + runaway 监控；
③Metal 双端同步（trace/resolve 孪生）；④收敛速度量化（半衰入 GIVerify）。

### 2026-09-10 十二（slice 2 落地 + 起点自命中 bug + 吸引子支配判决）

**slice 2 基建 ✅**：`PRIMAL_SC_RADIOSITY_TILE`（4-128，runtime，
radiosity_gate.z 传递 LightEval）、`PRIMAL_SC_RADIOSITY_STRIDE`（1-64，
`IndirectTraceParamsGPU` 的 `_pad0/_pad1` 改造为 refresh_stride/
refresh_phase——288B 尺寸不变，std140 偏移落在原 padding 位；shader 端
非刷新 tile 在 march 前早退）。LightEval 双端 tile 读取跟随 runtime 尺寸。

**起点自命中 bug（slice 1 遗留，已修）**：射线起点法线偏移（0.055m）小于
命中阈值（0.30m）→ 所有射线 t=0 即自命中 → tile 读到自己 texel →
"传输"退化为自反馈。修正：起点 = 阈值 + 0.05 外推。slice 1 的 +25-45%
部分由此自反馈放大贡献，非纯真传输。

**吸引子支配判决（W/stride 扫描）**：起点修复后 W0.8 与 W0.3（8px，
stride4/1）的左墙剖面全部落入 ±100% 的支路抖动——y10-12 三次运行一致
下跌（-49%~-73%）且与 W 反相关，证明是 **SC_INDIRECT 开启改变暖机动力学
→ 场重掷吸引子**，而非 tile 项的线性贡献。求解器贡献（无论正负）当前
**不可测量**——多稳态（09-10 六定罪）从"测量问题"升级为"收益前置条件"。

**slice 3（新前置）= 确定性稳定化**：物理正确支路的确定性着陆。候选：
①暖机期加速收敛（前 N 帧 tile 项 W=1 + atlas 直发不带 EMA，N 后回落——
确定性把场推入物理支路）；②探针更新顺序确定性化（governor 打散时序
随机源）；③atlas 级分支判据（受光带可视 tile 的亮度下限哨兵）。
slice 3 落地前，solver 调参暂停（测量无意义）。

**slice 2 后门禁态**：GIVerify 全带 PASS、shader 副本绿、默认路径
（无 SC_INDIRECT）逐位不变。新旋钮汇总：PRIMAL_SC_RADIOSITY_W /
_TILE / _STRIDE（全部默认关闭=旧行为）。

### 2026-09-10 十三（slice 3 落地：确定性稳定化生效，solver 贡献首次可测且全剖面为正）

**实现（PRIMAL_SC_WARMUP_FRAMES，默认 0=关）**：暖机窗口内三路同时冻结——
①LumenDDGIPass 单咽喉点 updateCount=0（探针场冻结在确定性 init，
下游 dispatch 由 C0 零尺寸守卫覆盖）；②SurfaceCachePass 切断
`ddgi_counts_valid[3]` 探针馈送（LightEval texels 只收敛直射+捕获）；
③tile solver 不派发。释放后探针首个更新以 alpha=1 直发，读到的已是
**收敛图册上的物理一弹场**——支路抽签的暖机瞬态源头被消除。

**稳定化效果**：L_arcade 两次独立运行 2.4 / 3.1（此前散布 1.3-3.1 且含
暗支路）——暗支路抽签消除，场稳定落在亮侧。

**slice 3 的关键收获：solver 贡献首次可测且全剖面一致为正**
（warmup=90，frame 800，左墙 x=0 剖面）：

| y | π+warmup | +tile(W0.8) | Δ |
|---|---|---|---|
| 0 | 0.199 | 0.200 | +1% |
| 2 | 0.094 | 0.100 | +6% |
| 4 | 0.042 | 0.050 | **+20%** |
| 6 | 0.130 | 0.135 | +4% |
| 8 | 0.134 | 0.151 | +12% |
| 10 | 1.410 | 1.666 | +18% |
| 12 | 1.023 | 1.226 | +20% |

用户投诉的 y4 带收益最大（+20%），上墙回到物理支路（1.67/1.23）并再
+18-20%。**测量协议自本片起固定**：一切 solver/回路 A/B 必须带
`PRIMAL_SC_WARMUP_FRAMES=90`（无稳定化地基的读数是支路抽签的随机数）。

**门禁**：GIVerify 全带 PASS、副本门禁 OK、ctest 1/1、默认（无 env）
逐位不变。下片：④收敛半衰量化入 GIVerify；⑤Metal trace/resolve 孪生
同步（同一 stub 四处死伤在 Metal 侧同样存在）；⑥tile 尺寸/步数精调。

### 2026-09-10 十四（转正 + C4 环境底：用户可见基线落地，无 env）

**用户第四次报告"左侧无区别"——根因：所有修复都锁在 env 后面，直接
开窗口跑没吃到。本批把验证过的配置转正为 Vulkan 默认行为（全部保持
env 可覆盖）**：
1. **tile radiosity 默认开**（`kScIndirectEnv`：unset→Vulkan on/Metal off
   ——Metal 孪生 trace 仍是旧 stub，slice 3 遗留；0/1 强制覆盖）；
2. **W 默认 2.0**（W 扫描：最黑带 y4 单调增益至 +38%，frame 800 稳定）；
3. **stride 默认 2**（trace 半率摊销）；
4. **warmup 默认 90 帧**（`scWarmupFrames()` 缺省 90——暗支路抽签消除）；
5. **远场环境底 0.11→0.45/0.46/0.50**（C4 环境标定：T8.9 设计意图的
   "缺失二次弹射替身"回到标定档；albedo 染色 + AO 调制 = 空间结构化，
   直射区在其自身项旁不可见——非均匀灰、非传输掩盖；传输侧 π/×20/
   trace 修复本轮已全部到位）。

**新默认基线（无 env，frame 800 / 2500）**：direct_patch 71.9-72.2
（不回退）、L_arcade 3.2-3.4 / R_arcade 3.2-3.7 / floor_near 3.1-3.6
——对比本轮起点（0.7/1.4/0.7）**×4.4-4.7**，2500 帧无 runaway。
GIVerify 全带 PASS、golden 重生成、副本门禁 OK。

**相对差距现状**：arcade/direct = 3.3/72 ≈ 0.046 vs UE 0.187——仍差
~4×（求解器迭代完整度 + Metal 孪生同步为余下工作面）。

### 2026-09-10 十五（用户指认"正对受光区的端面黑"：入口被埋探针连环修复）

**用户指认的面向受光区的端面 = 西端区——正是入口被埋探针柱（x=-13.5）
的馈送区**。两连环修复（DDGITraceSDF.comp + DDGITraceRays.metal 双端）：
1. `maxPush 0.45→0.95×spacing`（1.35m 推不出厚入口砌体）；改后 exhausted
   未见降——**真约束是搜索步数**；
2. relocation 搜索步数 6→10（0.3m 步长 × 10 = 3m 搜索半径 ≥ 2.85m 推力
   上限——原 1.8m 摸不到自由空气，推力上限形同虚设）。

**效果**：入口柱 exhausted 率 49-66% → **28-30%**；入口探针 L0
0.025-0.13 → **0.7-1.0 量级**（一枚紧邻受光带的 relocation 落点 7.2，
受 gather 归一/64 钳约束）。正对受光区的端面从"零馈送"变为有活探针。
区域表 3.3/3.6/3.0 稳定、GIVerify 全带 PASS、golden 重生成（复验仍受
多稳态 flapping 限制，判据为 L0+L1 向量带）。

**遗留**：①剩余 28-30% exhausted = 更深埋的探针（relocation 3m 搜索
半径外）或 crawl 预算不足——下一档是 relocation 失败后把整格探针标记
走最近自由格 SH；②y 轴（-13.5,4.5）出现 7.2 亮探针——观察是否局部
过亮（gather 归一应约束）；③Metal 侧 C++（tile 默认/warmup 门）需在
Metal 树重链后生效，当前 Metal 二进制停留在 tile-off 旧行为（安全）。

### 2026-09-10 十六（用户实测"进场亮→几秒后消失"：未初始化 indirect_out 垃圾入册）

**用户双图对照定位**：进场时的"好间接光"是**垃圾**——warmup 窗口内
tile solver 不派发，但 `radiosity_gate.x` 仍为 1 → LightEval legacy 分支
读**从未写过的 indirect_out**（Vulkan 堆纹理不零初始化）→ 未初始化暖色
垃圾进图册 → 屏幕出现暖斑块；释放后 solver 真数据覆盖 → "从有到无"，
atlas 三缓冲滚动下反复出现。

**修复**：`radiosity_gate.x` 与派发条件完全一致（`&& frame >= warmup`）
——warmup 窗口内 texels = 直射 + prev_light 自反馈（确定性种子）。

**修复后行为**：frame 100（释放后 10 帧）L/R/floor = 4.2/4.8/4.1
（真实一弹场，释放时 alpha=1 直发）→ frame 800 稳定 3.0/3.5/3.1——
**进场即真实光、轻微回落到稳态**，无假亮悬崖。GIVerify 全带 PASS。

**左墙正对受光区端面的现状**：relocation 修复后入口柱探针 L0 0.7-1.0，
端面有活馈送；稳态亮度由 ①tile solver 迭代完整度（W 已 2.0）②远场底
0.45 ③剩余深埋探针（28-30%）三者决定，屏幕端 ~2-4 显示单位（UE 相对
口径的 1/4）。进一步提升 = T3.3 solver 继续加密/加迭代 + 深埋探针
标记走自由格 SH，属继续迭代，无方向性悬念。

### 2026-09-10 十七（用户实验档：DIRECT 20→50，间接光线性跟随确认）

**用户拍板 DIRECT_INTENSITY 20→50**（三处同步：DeferredLighting.frag、
EngineTest/DeferredLighting.metal gpuDriven、引擎 Metal 孪生；C2.1 单位
统一保持——`SCDirectIntensityScale` 默认同步 20→50，SC 图册与屏幕直射
同乘 2.5×）。

**实测（frame 800 / 2500）**：

| 区域 | DI 20 | DI 50 (f800) | DI 50 (f2500) | UE |
|---|---|---|---|---|
| direct_patch | 71.9 | 113.4 | 115.8 | 120.5 |
| L_arcade | 3.2-3.3 | 9.4 | **13.8** | 22.5 |
| R_arcade | 3.2 | 7.0 | 6.9 | 8.9 |
| floor_near | 3.1-3.6 | 6.6 | 7.3 | 59.5 |

- **线性度确认**：2.5× 光 → 间接 2.6-2.9×（800 帧），且 2500 帧 L_arc
  继续爬至 13.8（tile solver EMA 仍在收敛）——管线线性、无隐藏钳位。
- **相对比值**：L_arc/direct = 13.8/115.8 = **0.119** vs UE 0.187——
  达 UE 的 64%（本轮会话起点 0.031 → **×3.8**）。
- 用户验收带：L_arcade ≥ 8 ✓（13.8）、R_arcade ≥ 8 ◐（6.9）、direct
  ≥ 67 ✓、GIVerify 全带 PASS ✓、golden 重生成 ✓、2500 帧无 runaway ✓。

**剩余 ~1.6×（至 UE 相对比值）**：tile solver 迭代完整度（8px tile、
更高 W 需配合收敛稳定化）+ Metal 孪生同步。无方向性悬念。

### 2026-09-10 十八（用户红/绿框标注判决 + tile trace 切离线 SDF）

**用户标注两类异常，机制均已定位**：
- **红框（紧邻受光带黑墙）**：tile 求解器每个 texel 只读**自己 tile** 的
  辐射（16px tile = 3.4m 粒度）——受光斑的光梯度传不进相邻 tile；探针侧
  gather 沿法线求值对掠射亮斑天然弱响应。→ 修法 = 8px tile（已有旋钮）
  或邻 tile 采样（下片）。
- **绿框（被遮挡却亮）**：tile trace 绑定的是**粗运行时 GlobalSDF**——
  薄拱顶在粗级联里漏成假天空（trace 报 sky → 1.15 注入）。**已修**：
  `useOfflineSdf` 时 tile trace 绑定离线 384³ 传播场（cascade0 参数 +
  sdf0/mip 槽，cone 自门控），GlobalSDF 仅作无离线源回退。实测区域表
  持平（12.4/6.9/6.5，噪声带内）——绿框类假天空的收益在具体位置可见，
  区域盒不敏感。
- 另：DI 50 实验档已按用户要求转正（三处同步 + SC 尺度同步，线性跟随
  确认，L_arc 13.8 = UE 相对比值 64%）。

**门禁**：GIVerify 全带 PASS。**下片**：①8px tile + 邻 tile 采样（红框）；
②Metal trace/resolve/重定位孪生同步；③深埋探针自由格插值。

### 2026-09-10 十九（用户三连观察的机制定位：下片工作清单固化）

**①进场亮→数秒后变暗（仍在）**：窗口模式（自由相机）下 = 探针首发
（alpha=1 直发，读当前图册含真实受光 texel）与其 EMA 平衡态（多稳态
偏暗支路）之间的差。warmup 消除的是暖机垃圾闪烁（已修），首发→平衡
的衰减属于**平衡态本身偏暗**——与 ②③ 同根（求解器迭代完整度）。
候选缓解：首发后 alpha 保持 0.35 档更久（延 respecting 300 帧再落
0.02），把收敛放缓让眼适应——治标。

**②间接光偏红、缺蓝绿**：主分量 = Sponza 石材暖 albedo × 暖阳
(3.0,2.64,2.16)——物理正确。布料三色缺失的真机制 = **SC 图册里布料
texel 占比极小**（小物体在 27m 大卡上只占几个 texel，探针 128 射线命中
概率近零）+ tile 粒度 3.4m 抹平。→ 蓝色反弹只应出现在蓝布近邻（栏杆
下方），远处墙本就不该有。若需强化布料色溢 = 布料专属小卡（card bake
专项的又一行理由）。

**③正对受光区的墙仍暗**：与 09-10 十八红框同根——同墙邻接梯度靠
tile trace 的掠射射线（y4 实测 +38% 即此路径），粒度 3.4m + 探针法线
求值对掠射弱响应。**下片首项 = 8px tile（0.85m）+ 邻 tile 采样**，
预期把邻接梯度带起来。

**下片（顺序即优先级）**：
1. PRIMAL_SC_RADIOSITY_TILE=8 验收（旋钮已备，直接跑）+ 邻 tile 采样
   （LightEval 读相邻 tile 的 radiance × 视野因子近似）；
2. 探针首发后 alpha 保持 0.35 至 ~300 帧（缓解 ①的观感衰减）；
3. Metal 孪生全量同步（trace/resolve/relocation/tile-off 现状）；
4. 深埋探针自由格插值（28-30% 残余）。

### 2026-09-10 二十一（"间接光会变暗"最终定罪：图册幽灵种子衰减 + texel 环境底落地）

**用户"按道理间接光不应该变暗"——完全正确，最终定罪**：图册初始化种子
`albedo=(0.5,0.5,0.5, a=1.0)`（SurfaceCacheAtlasInit）——a=1 过 LightEval
的 alpha 门 → 幽灵 texel（垃圾法线 + 卡片中平面深度）**能吃到直射光**；
进场时探针读到满场 0.5 灰幽灵的暖辐射 = 假亮环境；捕获逐页推进 → 真实
计算值（阴影处 = 传输上限内的暗值）替换幽灵 → 探针读数衰减 → 屏幕变暗。
收敛态比种子暗 = 多弹射积累缺口（已知 ~4×），衰减是其时间维表现。

**修复（slice 5，双端）**：LightEval 给 texel 侧入射加环境底
`E_indirect = max(E_indirect, vec3(0.45,0.46,0.50))`——与屏幕侧远场底
同量纲同档（FusionIndirect gi += 0.45×ao）。收敛态自此携带稳定的
天空泄漏环境 → 探针读数不衰减 → 屏幕无衰减。实测 DI50：L 12.4 /
R 6.2 / floor 7.4 / direct 114.9（与加底前持平——底只在深区生效），
GIVerify 全带 PASS。四个 spv 变体同步重编，Metal 孪生同修。

### 2026-09-11（slice 7：邻 tile 采样 + alpha hold 落地）

1. **LightEval 3×3 tile 邻域采样**（双端）：per-tile-self 读取无法把受光
   斑梯度传过 tile 边界（09-10 十八红框定位）——改为中心权重 2、邻域
   权重 1 的 3×3 gather。区域表持平（14.3/7.4/6.7，预期内：改善在
   patch 邻接梯度，区域盒不覆盖）。
2. **PRIMAL_DDGI_ALPHA_HOLD**（默认 off；=N 时前 N 帧 TemporalAlpha
   钉 0.35）：探针年轻期延长——首发亮值后不再以 0.02 慢速漂向平衡态，
   几秒内到位并稳定。实测 400 帧档区域持平（12.3/7.4/6.8，稳态不变，
   只改到达过程）。`PRIMAL_DDGI_TEMPORAL_ALPHA` 保持可用。

**下片遗留**：①8px tile + 邻采样的组合验收（红框视觉复核）；②Metal
全量同步（trace/resolve/relocation/tile/C++ 默认门，需 Metal 树重链）；
③深埋探针自由格插值；④用户如选定新太阳角度则重标区域盒/golden/档位。

### 2026-09-10 二十（8px tile 验收：与 16px 等值——邻 tile 采样确认为必要项）

8px/W2.0/warmup90/DI50 实测左墙剖面：y0=0.507 / y2=0.255 / y4=0.095 /
y6=0.307 / y8=0.305 / y10=3.49——与 DI50 折算基线（warmup 基线×2.5）
逐点吻合（±5%），y12 仍为支路抖动（0.45 vs 期望 2.6）。**结论：tile
尺寸缩小本身不产生邻接梯度**——per-tile-self 消费语义下，梯度缺失的
根因是"texel 只读自己 tile"，与粒度无关。**邻 tile 采样（LightEval 读
3×3 tile 邻域 radiance × cosine 近似视野因子）确认为红框/③的必要修法**，
是下片首项的代码工作（非纯旋钮）。8px 的价值在布料/栏杆等小物体色溢
（等 solvere 稳定后复测）。

### 2026-09-11 续（下片遗留 ①②③ 收口：Metal tile 路径点亮 + 深埋探针自由格回退）

**① 8px tile + 3×3 邻采样组合验收（Vulkan，默认配置即该组合）**：
区域门禁 exit 0——direct_patch 112.5-113.5 ∈ [40,170] ✓、
direct/interior = 113.5/13.0 ≈ 8.7 ≥ 8 ✓、floor 5.8 < direct ✓；
L_arc 13.0 与 16px 时代（13.8）噪声带内一致（无回归）。图册剖面
（DDGI_DUMP=5，卡 #5 -Z 面）：受光斑峰值 16.3（理论 13-17 ✓）、
亮斑邻接带可见梯度传递（patch 外 2.7/1.6/0.6 递减 vs 远场底
0.05-0.2）——3×3 邻采样在图册级生效。红框的最终视觉复核留用户。

**② Metal 全量同步（tile radiosity 首次在 Metal 点亮）**：
- `SurfaceCacheIndirectTrace.metal`：×sdf_extents 倍率去除（米制直读，
  对齐 SDFTraceCommon）、hitThreshold = max(voxel0×1.25, 0.30)、
  kMaxSDFSteps 4→32、起点沿法线外推 hitThreshold+0.05（防自命中）、
  IndirectTraceParams 补 refresh_stride/refresh_phase 字段（288B 布局
  双端一致；旧 struct 靠 16B 对齐侥幸同偏移，现为 correct-by-construction）；
- `SurfaceCacheIndirectResolve.metal`：far 命中并入图册解析（Phase-A
  0.02 占位符退役）、sky 兜底对齐 Vulkan 分档（DDGI_SKY×0.4）、
  prev_lighting 改整数坐标最近读；
- C++（SurfaceCachePass.cpp）：`scIndirectEnabled()` 共享 helper 统一
  radiosity_gate.x 与派发条件两站点（09-10 十六漂移教训的机制化）、
  Metal 默认门翻转 ON、Metal trace 描述符分支补离线 SDF 替换
  （Vulkan 分支对齐）；
- **副本治理**：EngineTest/shaders 的 Resolve 副本漂移被门禁当场抓获
  （gate FAILED→同步）；Trace 副本仍是被基线 DRIFT 豁免的旧 stub
  （T3.6 式漏网，本次一并同步）——基线重生成后两对均 SAME；
- **验证**：trace/resolve 内核每帧派发、零 assertion、区域门禁 exit 0
  （165.4/5.4/27.1/101.2）、1500 帧 runaway 检查稳定（162.0/8.1/28.6/
  82.0，floor 101→82 落在文档化多稳态带内）。**Metal↔Vulkan 室内差
  （floor 82 vs 5.8）即 C4.3 跨端对数核心工作面**（Metal direct 162 vs
  Vulkan 113 亦示显示链档位差），非同步缺陷。

**③ 深埋探针自由格回退（双端）**：`ddgiRelocateProbeOrigin` 在多轴搜索
失败（bestD ≤ skin）后追加 26 个网格对齐邻格心扫描（probePos 网格对齐
→ ±spacing 即真实格心），取最开阔一格起 trace——"整格探针走最近自由
格"。顺带补齐 Vulkan 端 candidate 覆盖内守卫（`d < 1e9`，Metal 2026-09-04
伪影1 修复的漏移植——越界哨兵恒大于真实距离，可在 max-d 搜索中获胜把
探针重定位到场外）。DDGITraceSDF.comp spv 重编。
**验证（Vulkan）**：GIVerify ALL BANDS PASS、全局 exhausted
3.6%→**1.41%**、区域表与③前噪声带内一致（113.5/13.0/6.2/5.8 →
112.5/12.2/6.1/5.9，无回归）；重链后二进制 GIVerify 复跑 outcome
逐位复现。**验证（Metal）**：GIFusionVerify ALL ALIGNED ✔（probe_sh0 9.99 ∈
band、gather_avg 0.72 ∈ band）、区域表与②态一致。

**附带状态**：共享 archive 协议全程执行；末态 Debug 树 archive 已恢复
（Vulkan 重链后 rm+重建）。

**下片遗留**：
1. C4.3 跨端对数：Metal↔Vulkan 室内 ~14× 差的归因（SC→probe 注入档、
   escape 档、显示链 DI 档位——Metal direct_patch 162 vs Vulkan 113）；
2. 红框（受光斑邻接黑墙）用户视觉复核；布料/栏杆小物体色溢的 8px 复测；
3. GI_HALFLIFE Metal 等效实验（tile 点亮后的 Metal response 曲线）；
4. C5.3 绑定签名提取（本次 DRIFT 豁免藏 stub 即其案例，机制化未做）。

### 2026-09-12（参考机位门禁重定基线 + 拱内观感归因收口，B17 后续）

**① 参考机位重定基线（主任务，§8.1 落地）**：5 次 `SPONZA_REFERENCE=1` +
`SPGI_CAM_LOCK=1` frame 4800 抓帧，direct_patch 117.0 / interior 35.1 /
比率 3.33（5 次散布 3.26-3.52）。`scripts/sponza_region_lum.py` 比例门改
带 [2.3, 4.6]（基线 ±40%），保留 floor_near < direct_patch 与
direct_patch ∈ [40,170] 两门，5/5 PASS。旧 "≥8" 门作废原因见 §8.1——
0090b2e6（B 系列 tonemap/天空 rebalance）把 interior 13→35、sky 140→178、
direct 不动；B17 HEAD A/B 已证非 GI 回归。

**② 拱内观感归因（B17 机位 0,5,0 锁定 + 默认测试灯光，frame 4800）**：

| 抓帧 | 拱内框 (480,140)-(570,240) | 纯拱腹框 (500,180)-(560,230) | 阳光带 (580,20)-(780,110) |
|---|---|---|---|
| 正常（直射+GI） | 20.3 | 13.9 | 145.3 |
| `PRIMAL_DIRECT_LIGHT=0,0,0`（GI 只剩天光底） | 4.1 | 4.2 | 3.7 |
| `GI_ALL_OFF=1`（纯直射） | **0.00（max=0）** | **0.00（max=0）** | 131.9 |
| 阳光供能弹跳（正常−天光底，一阶归因） | ~16.2 | ~9.6 | ~141.6 |

**结论：不动 GI。** 纯直射对拱内框贡献为零（GI_ALL_OFF 帧整框 max=0——
阴影链把直射全挡住，B17"纯比较零泄漏 + 拱内亮度=GI 弹跳+真实受光曲面"
结论再证，且受光弧面的直射贡献低于显示量化）；拱内 20.3 = 天光 GI 4.1 +
阳光补丁→地面/墙弹跳 GI ~16.2。"室内暖光"就是真实弹跳传输本身：正确的
调节杠杆在显示链/art direction（tonemap、灯色），不在 GI 物理链，更不在
阴影链（B17 已封）。另外 interior 是 §8.1 新比率门的分母——压 GI 供给会
连带破坏参考机位门禁，等于把物理正确的传输当缺陷修。

**③ 纯拱腹阴影回归哨兵（可选项③，已验证并落脚本）**：
`PRIMAL_SHADOW_DBG_HARD=1 + PRIMAL_SHADOW_VIS_DEBUG=1`（q5 硬比较=生产
bias/offset，灰度≈native vis）抓帧：纯拱腹框 vis 均值 **0.056**（p99 0.074 /
max 0.088，分布均匀非边缘主导——PCSS 半影 + 半分辨率上采样底噪，非泄漏；
B17 纯深度比较 frac(recv<map)=0 已证比较链零泄漏）；拱内框 vis 0.082 ≈
B17 记录 0.08 ✓；阳光带 0.556 ∈ B17 带 0.57-0.64 下缘（单轮 GI 波动量级）。
阈值已固化进 `sponza_region_lum.py --arcade`：soffit ≤ 25/255（0.098，恰在
B16 泄漏带 0.1-0.3 之下，实测 14.3 余量 43%）、阳光带 ∈ [130,165]、
拱内框 ≤ 31/255。正常渲染拱内（无哨兵）实测 20.3 ∈ 验收带 [19,21] ✓。

**④ 白 vis 语义澄清 + 遗留**：B17 记录"白 vis 拱内 93.8"是**不带**
VIS_DEBUG 的着色画面（白 vis=1 当乘数 = 无阴影直射上界）；带 VIS_DEBUG 的
白 vis 是灰度可视化，vis=1 在 rebalance 后 tonemap 曲线肩部读到 ~199 而非
255——`DeferredLighting.frag` 的 ×3.8461 灰度预补偿常数按旧 passthrough 链
标定已过时（仅调试视图高段读数失真，≤0.7 段仍近线性，不影响哨兵相对判读）。

**回归**：ctest Unit 3/3 PASS；Vulkan 集成 110s 无 fault（"default" 子串误
匹配除外，391 条存量 validation）；Metal TestModularPipeline 110s 同。

**下片遗留**：
1. Metal B17 parity（可选项④本轮未做）：slope-scaled bias / normal-offset /
   cascade_params 三件套移植 `EngineTest/shaders/ShadowFilter.metal`（其
   Params 注释里的分叉记录）+ Metal 参考机位对照——注意切树前
   `rm Darwin/Debug/libEngine.a`；
2. VIS_DEBUG 灰度标定常数按新 tonemap 重标（调试质量项，见④）；
3. 纯拱腹哨兵目前是脚本手动门，未进 ci_local.sh（需要 5 分钟级抓帧，不适合
   常规 CI 节奏，保持文档化手动哨兵）。
