# 新 Session 提示词：GI 能量/单位审计 + UE 参考重标定（C3.4/C3.5 → C4）

> 用法：新 session 直接把下面的提示词粘进去（或让它先读本文档）。

---

## 提示词（粘贴用）

背景：读 `Docs/2026-09-09-gi-physical-relation-calibration-plan.md`（C 系列计划 +
§9 执行日志，含全部已证结论与仪器清单）和
`Docs/completed/2026-09-04-spgi-energy-calibration-and-ddgi-overexposure.md`（历史脉络）。

当前状态（2026-09-10，已验证）：
1. Metal trace 三管线曾因 `DDGITraceRays.metal:289` 类型名过期
   （DDGIDetailSDFTable → DetailSDFTableGPU）整体 INVALID，已修复——
   trace/depth/可见性全部复活，camcell 探针深度八面体已有真实数据
   （SC_DEPTH_STATS 验证）；
2. 灯响应已翻 YES：太阳调光 10% → camcell L0 暖色分量完全消失
   （1.44-1.91 → 1.12-1.45），响应的是太阳传输；中性残余 ~1.1-1.45 是
   天光传输，不随太阳变化；
3. 当前参考机位区域基线（frame 400 收敛）：L_arcade 69.5 / R_arcade 94.9 /
   sunlit_wall 166.2 / floor_near 199.2 / sky 178.7。UE 参考：22.5/8.9/120.5/
   59.5/87。floor_near 仍 ~2× 过亮，属标定域。

本 session 任务（按序）：

**T1 π 链残差审计（C3.4/C3.5 前半）**——B3 台账遗留"SC/Dawn/FusionComposite
面残留核对待续"。逐段核对 SC atlas → 探针注入（mcWeight=4π/N radiance 约定）
→ GIGather（ deliberately 不补 π 的 SC 侧 vs ×π 的屏幕侧）→
FusionIndirect/Composite（1/π 与 albedo/π）→ DeferredLighting（ddgi×0.5）的
全链单位表，产出一张"每跳增益系数表"，标出乘积 ≠ 物理约定（irradiance =
π×平均 radiance）的段。

**T2 回路 DC 增益实测（C3.5）**——SC↔probe 反馈环的平衡态放大系数：
用 `PRIMAL_DDGI_LIGHT` env 做输入阶跃（1×/2×/0.5×），测 camcell L0 收敛值之比；
同场用 `SC_PROBE_FEED_OFF=1` 对比 legacy 自反馈路径平衡点（已知 feed-off 反而
更亮 4.7-5.5 vs 1.4-1.9，T8.13 多稳态）。产出：回路增益是否 ≥1 的判定与
定位（哪一跳的增益系数超标就修哪一跳，禁止用全局乘法吸收）。

**T3 B1 ×2.5 回撤评估**——`DDGIGIGather.metal`（引擎版 ~L289）的
`* 2.5f` 校准系数：trace 复活后 Metal/Vulkan 同机位重测 gather_avg 与
camcell L0，若比值回到 ~1 则删系数重标 band；若仍差，定位到 miss 单位源
（Metal split finalize miss 落本地卡片 vs Vulkan 天空渐变）并修单位。

**T4 C4 端到端重标定**——T1-T3 后按计划 §C4：物理推导期望带、
DIRECT_INTENSITY 四值归一、CAL_EXPOSURE/天光底/ddgiBase 重标，
验收 = `python3 scripts/sponza_region_lum.py`（floor_near→60-90、
sunlit/int ≥8、室内地面 < 直射墙）双平台 + `GI_HALFLIFE` response=YES。

仪器与操作纪律（务必遵守）：
- 诊断旋钮全部在计划文档 §9：`SC_DEPTH_STATS`（探针深度真值）、`SC_VIS_DUMP`
  （vis 整图+卡片关联表）、`GI_DEBUG_STRIP`（带内全画面分源）、
  `PRIMAL_DDGI_NO_ANALYTIC/NO_SKY/NO_PREVSCREEN`、`PRIMAL_SC_NO_DIRECT/
  NO_EMISSIVE`、`SC_PROBE_FEED_OFF`。
- Metal shader 运行时编译（引擎目录权威）；**引擎 C++ 改动后必须重链二进制**
  （旧二进制 + 新 shader = 绑定 assert，如 ddgi_depth_buffer binding 7）；
  Vulkan 改 comp 后 `cd Engine/Graphics/Vulkan/Shaders && ./build_spv.sh`。
- 测量纪律：`env -i` 干净环境 + `SPGI_CAM_LOCK=1` + capture/verify ≥ frame 400
  （收敛需 ~300 帧）；两树共用 Darwin/Debug/libEngine.a，切树先移 archive；
  TestRendererMac 编译失败是存量问题，忽略。
- 每个结论必须有 A/B 数据；禁止目视验收；旧结论若与我的记录矛盾，以重测为准。

验收门禁：`ctest --test-dir Darwin/Debug -R Unit`、
`./scripts/check_shader_copies.sh`、`scripts/sponza_region_lum.py` 双 FAIL 条
（floor>sunlit、ratio<8）转 PASS；完成后按惯例回写计划文档 §9 与
tasks/gi-supplement-todo.md M6 台账。
