# GI 多弹射恢复 session 提示词（2026-09-10，接续 09-09/10 修复链）

把下面整段作为新 session 的第一条消息发送。

---

## 任务背景（先读，30 秒）

仓库：GameEngine_VulkanCPP（macOS，Vulkan=MoltenVK 测试路径）。TestVulkanSponzaRenderGraph
的 GI 链路刚完成一轮根因修复（2026-09-09/10，见
`Docs/plans/2026-09-09-vulkan-direct-gi-recovery-plan.md` §10 与
`Docs/2026-09-09-gi-physical-relation-calibration-plan.md` §9）：

已修复并验证：①DDGI 探针索引错误（16/128 射线 → 128/128，
`gl_GlobalInvocationID.x` → `gl_WorkGroupID.x`）；②march 隧穿（近场步长
封顶 2 voxel + 封顶区扩到 6m）；③被埋探针（buried-start skin crawl）；
④PCSS lightSize 0.05→0.012；⑤C3.1 analytic 兜底去无阴影直射、逃逸天空
统一到 0.4 档（`DDGI_SKY_COLOR * volume.SkyColor.x`）；⑥SC 侧 gather
Chebyshev 可见性落地（SurfaceCacheLightEval.comp accVis/accLow，深度绑定
已接 `frame_data.ddgi_depth`）；⑦M3 门禁重定义为盒内探针统计；⑧
`generate_sdf` 传播修复（ContentTools/common/SDF.cpp，供下次重烘 model）。

**当前实测态**（SPONZA_REFERENCE=1 SPGI_CAM_LOCK=1，frame 360）：
- GIVerify 全带 PASS：盒内 hit 85.6%/escape 10.9%、probe_sh0 0.497、
  gather 0.414；
- **GI_HALFLIFE response=YES，half_life=45 ≤ 60**（sun→探针场耦合已通）；
- DIRECT_INTENSITY=20.0（双平台）、逃逸天空 0.4 档——当前平衡态，C4 会重标；
- **残留问题**：室内近黑——L_arcade 0.7 / R_arcade 1.4 / floor_near 0.7
  （UE 参考 22.5 / 8.9 / 59.5）。frame 900 vs 360 逐位一致 = 非收敛问题，
  是**多弹射传输链在可见性门控下断供**（合法弹射与隧穿残量一起被压）。

## 本次 session 的任务（按优先序，全部有量化验收）

### 任务 1（最高优先）：多弹射断供修复——让"真间接光"回到室内

物理目标：直射明亮（已达成，DI 20）、室内由**合法弹射**照亮（UE：
L_arcade 22.5 / R_arcade 8.9 / floor_near 59.5——暖色多次弹射），而不是
现在的近黑或历史上的均匀灰填充。

诊断树（按序执行，工具全部现成）：
1. **走廊轴探针亮度验证**：`DDGI_DUMP=1`（frame 350）读 camcell 探针
   SH——L1 是否指向中庭方向、量级多少？亮 → 断点在下游；暗 → 断点在
   探针 trace 的命中辐射（SC 图册采样）。
2. **SC 图册分区亮度**：SC_DUMP_CARDS（frame 0 top-24）+ 卡片世界坐标
   关联表（SC_VIS_DUMP 的 /tmp/sc_cards_vis.txt）——中庭受光卡（2F 带）
   亮度、走廊卡亮度、以及 SC 侧 gatherProbeIrradiance 的 accVis/accLow
   45/55 权重是否把跨区弹射全部杀掉。
3. **SC↔probe 回路增益审计（C3.5）**：SC_FEEDBACK 0.70 × 探针 EMA
   0.02-0.05 × gather clamp 的 DC 增益对最亮 albedo 闭环 < 1 且余量
   ≥20%（T8.13 bistability 复发即不达标）。
4. 修复候选（按诊断结果）：accVis/accLow 权重再平衡；Chebyshev 的
   variance 噪声下限；掩埋降权对"健康但深度被埋"探针的误杀；必要时
   escape 天空按开口立体角分档（而非全局档位）。

**红线**：不得让室内回到"均匀灰填充"（监控：arcade 内部方差、
lum/albedo 比的空间稳定性——健康态下该比值应随遮挡变化而非恒定）。

验收：floor_near ≥ 30（向 UE 59.5 收敛）、L_arcade ≥ 8 且 R_arcade ≥ 8、
直射 patch ≥ 67 不回退、GIVerify 全带 PASS、360/900 帧无 fault。

### 任务 2：response 门禁度量切换（C2.3 收口）

现门禁读 camcell L0——信噪比太低（10× 调光仅 ~1-5% 漂移，部分 camcell
探针被埋走 SC 回退）。切到 **SC 图册响应**：`DDGI_DUMP=4`（本 session
新增，/tmp/sc_atlas_bits.bin 位样转储 + nonzero/raw-sum）在调光前后各采
一次，判定"位样变化率 ≥ 阈值"。10× 调光实测 43.5% 位样变化——SNR 高两
个数量级。备选：受光 cell（中庭 2×2×2）采样。二选一实现入
GI_HALFLIFE 门禁。

验收：GI_HALFLIFE=1 下新度量稳定判定 YES（连跑 3 次）。

### 任务 3：-y 地板隧穿残量（M3-A1 残项）

arcade 盒内逃逸的 -y 份额 15-18%（地板距探针仅 1-3m 却逃逸）。工具：
`DDGI_DUMP=2` 逐射线转储（/tmp/ddgi_ray_detail.csv，rayId+dist）+
ProbeSampleLib 的 Fibonacci 方向集离线重算，锁定逃逸射线的中途采样
d 值剖面。嫌疑：地板网格薄（±0.35m）+ 接缝高估 + 1.5× 过冲步进在
0.19-0.38 浅带上的组合穿透。修复后 -y 逃逸应归零或转为近场命中。

验收：arcade 探针 -y 逃逸份额 < 5%。

### 任务 4（独立）：C2.1 生产侧双源统一

SC 捕获走 `sc_lights`（场景灯）、DDGI/deferred 走 `settings_`——编辑器
改灯时 SC 侧不传导。统一到同一光源表（SurfaceCachePass.cpp T0.4 已接
FrameData；核对 FillSCLightList 与 settings 的优先级/覆盖关系）。

### 任务 5：C4 最终标定（在 1-4 全绿后）

一次 PR 一组旋钮：CAL_EXPOSURE（现 0.26）、DIRECT_INTENSITY（现 20.0）、
天光底、accVis/accLow 权重。对照带：全图 mean 42.8±10%、深暗 ~61%、
贴白 ≤0.5%、直射 patch ≥ 116、直射/内部比 ≥ 8。**双平台同参**（Metal
侧 DIRECT 同步在 EngineTest/shaders/DeferredLighting.metal gpuDriven）。

## 工具与开关清单（全部 env 门控，零 env 时行为不变）

Vulkan（本 session 新增/扩展）：
- `DDGI_DUMP=1` 探针 SH 转储（/tmp/ddgi_probe_dump.csv）
- `DDGI_DUMP=2` 射线路径 + 探针平均辐射 CSV（/tmp/ddgi_ray_paths.csv，
  含 rad_r/g/b）+ `DDGI_DUMP=2` 逐射线明细（/tmp/ddgi_ray_detail.csv，
  盒内探针 rayId+dist）
- `DDGI_DUMP=4` SC 图册位样转储（/tmp/sc_atlas_bits.bin + [SC_ATLAS]
  统计行）——**调光传导/灯响应判定的首选仪器**
- `PRIMAL_SHADOW_DBG_DEPTH/RECV/HARD/BLOCKER`（quality 3-6，配
  PRIMAL_SHADOW_VIS_DEBUG 上屏）
- `PRIMAL_GBUFFER_ALBEDO_DEBUG`（GBuffer albedo 直出）
- `SDF_NO_PROPAGATE=1` 关闭 per-mesh 场传播（A/B）
- `SDF_REBAKE_V6=1` 重烘 per-mesh 场 → v6 model + 强制重合成
- `SC_DEPTH_STATS=1` 探针深度真值统计（注意：现已平台无关）

既有：`SPGI_NO_OFFLINE_SDF=1`（切运行时 GlobalSDF 对照）、
`GI_ALL_OFF=1`（纯直射）、`GI_ISOLATE=ssgi|ddgi|spgi|all`、
`SC_DUMP_CARDS`、`SPGI_PROBE_VIS`、`PRIMAL_DUMP_MATRIX`。

抓帧与量化：`python3 scripts/sponza_reference.py vulkan`（锁定机位 360
帧）+ `python3 scripts/sponza_region_lum.py <png>`（区域表 + 直射/内部
比门禁）。

## 纪律与坑（前车之鉴，勿重蹈）

1. **构建树协议**：两树共用 Darwin/Debug/libEngine.a，切树先
   `rm Darwin/Debug/libEngine.a`；并行会话期间构建前核对 archive 归属
   （本轮跨树污染复发一次，链接期 VulkanDevice 符号丢失）。
2. **门禁度量陷阱**：hd/ray_buffer 曾被索引 bug 污染（hit 0.8776 =
   112/128 init 零值假象）；halflife 的 dim→dump 时序必须
   FRAME + WINDOW < dump 帧（否则 cp 复制基线残留，"逐位相同"假象）。
3. **双平台同步**：改 march/trace 数学时同步
   `Engine/Graphics/Metal/shaders/Lumen/DDGITraceRays.metal`（ddgi_trace_
   sdf/finalize）与 `EngineTest/shaders/DeferredLighting.metal`
   （DIRECT_INTENSITY 现已 20.0 双平台）；改后跑
   `scripts/check_shader_copies.sh`。
4. **spv 重编**：GLSL 改动后必须重编（Forward/build_spv.sh、
   Shaders/build_spv.sh、Lumen build_spv.sh 各管各的）；测试从 CWD 加载
   源码树 spv。
5. **golden**：探针场语义变更后 `GI_GOLDEN_UPDATE=1` 重生成并随 PR 提交
   （本轮已按新平衡重生成一次）。
6. **红线**：不得用全局常量（天光底/曝光）掩盖传输缺陷；不得回退
   M3 盒内 band 的诚实语义。
