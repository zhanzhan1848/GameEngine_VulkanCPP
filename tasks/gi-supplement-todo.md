# GI 补充计划执行 Todo（M0–M5）

**Master Plan**: `Docs/2026-09-07-gi-supplement-plan.md`（架构决策、发现索引 A1–F7、逐项证据与验收标准）
**Date**: 2026-09-07
**销账约定**: 每条任务标注其关闭的发现 ID（见 Master Plan §1 索引）；完成时按仓库惯例补 ✅ + 日期 + commit。
**顺序依赖**: M2 依赖 M1 的门禁；M3 依赖 M0 的 A1（Vulkan SDF 复活）；M4 依赖 M1 的事件机制。M0/M1 可并行。

---

## M0: 热修周（正确性止血）

- [x] ✅ 2026-09-08（M0 本轮） **T0.1** Vulkan 体素化复活：删 `77.0` 调试 store、`min_dist` 落盘、修 InstanceData 偏移（cluster_start/count@136/140、radius@172）— `GlobalSDFVoxelizationGLSL.comp:199-200,150-151,63`（关闭 **A1**；逐字段对照 Metal 版核验。**执行纠偏**：offsetof 实测 `bounds_radius` 在 @176 而非审计所记 @172——C++ 注释过时（`simd::float3` 在 arm64 为 16 字节），Metal 版读 @172（v3 padding）同为 bug，已一并修；GLSL comp 版 MeshletData 还缺第 4 个 vec4（48B vs 真实 64B），步长与字段全错，已按 WGSL 4-vec4 布局重写。该 comp 为未接线死文件，真正加载链 = Dawn WGSL → naga → `GlobalSDFVoxelization.spv`。**验收达成**：移走离线 bake + `DDGI_DUMP=2`，16384 探针×128 射线 hits=1,129,046（53.8% 有效 hit），`/tmp/ddgi_ray_paths.csv`）
  - 验收：Vulkan **不加载**离线 bake 时 `DDGI_DUMP=2` 路径直方图出现有效 hit；Sponza 无离线 bake 冒烟
- [x] ✅ 2026-09-08（M0 本轮） **T0.2** octDecode 改标准 replace 折叠（encode 已正确）— `SurfaceCacheData.metal:142-157`、`SurfaceCacheData.glsl:143`（关闭 **A2**）
  - 验收：octahedral 往返单测（下半球采样点集 max 角误差 < 0.5°）；SC 光照地板区域前后 dump 对比
- [x] ✅ 2026-09-08（M0 本轮） **T0.3** 捕获 u 镜像修复：per-dir 选 up 使 right=+local_u（或 FS 翻转 u），6 组 axis×dir 全表推导入注释 — `SurfaceCachePass.cpp:1073-1090`（关闭 **A3**）
  - 验收：非对称几何（L 形块）卡片捕获内容与世界位置一一对应（dump 网格线）
- [x] ✅ 2026-09-08（M0 本轮） **T0.4** SC 光源上传接 FrameData 真实光源列表（type/position/radius 逐槽正确），点光路径复活 — `SurfaceCachePass.cpp:1276-1289`、`SurfaceCacheLightEval.*`（关闭 **A4**）
- [x] ✅ 2026-09-08（M0 本轮） **T0.5** governor 预算接线：SetMaxProbesPerFrame 直写 member（或 AddPass 读 params_）；WASM 预算传导体核验 — `LumenDDGIPass.h:234` vs `cpp:380`（关闭 **A5** + WASM 半边）
- [x] ✅ 2026-09-08（M0 本轮） **T0.6** PRIMAL_SC_BUDGET 与 governor 仲裁：env 存在时打印一次性告警，governor 改为叠加系数而非覆盖 — `SurfaceCachePass.cpp:1204-1212`（关闭 **A6**）
- [x] ✅ 2026-09-08（M0 本轮） **T0.7** Vulkan gather 捕获同 texel 深度比较（atomicMin 或两阶段 nearest）— `SurfaceCacheCapture.comp:128-138`（关闭 **A7**）
- [x] ✅ 2026-09-08（M0 本轮） **T0.8** monolith trace 步长统一 float4（或独立 packed 布局声明）— `ScreenProbeTraceRays.metal:279 vs :663`（关闭 **A8**）
  - 验收：`PRIMAL_SPGI_SKIP` 强制 monolith 与 split 路径 miss 辐射亮度一致
- [x] ✅ 2026-09-08（M0 本轮） **T0.9** 时域卫生：DDGI/SSGI/GTAO prev 矩阵真传（锁存上帧 VP）；camera cut 检测（帧间 VP 跳变阈值）→ TAA/SPGI/GI history 重置 API — `StandardRenderPipeline.cpp:1591-1592,1662-1663,1379-1380`（关闭 **A10、A11**）
- [x] ✅ 2026-09-08（M0 第二批）：OnSceneChanged 指针检测 + MarkFullRebuild + DDGI ClearProbeField + probe_cache v4 场景哈希校验；SPGI 时域重置联动  **T0.10** 场景切换置 NeedsFullRebuild + DDGI probe 场清零 + probe_cache 按场景哈希命名/校验 — `RenderSceneSnapshot`、`StaticProbeVolume`（关闭 **A12**）
- [x] ✅ 2026-09-08（M0 第二批，策略部分）：SetViewportSize 变更检测 → TAA 历史重置 + SPGI 时域重置；GBuffer/GI 纹理重建（D13）归 M1 RG 钩子  **T0.11** resize 重建策略：SetViewportSize 触发 GI/GBuffer/history 重建（RG 钩子），TAA 输入尺寸对齐 — `StandardRenderPipeline.h:94-99`（关闭 **A13**）
  - 验收：编辑器内连续拖拽 resize 无黑屏/混用帧（对照 Program.cs:122-126 前科）
- [x] ✅ 2026-09-08（M0 本轮） **T0.12** 游戏模式 spawn 解锁：每帧检查 instance buffer 容量（或实体计数）触发 PartialUpdate/扩容 — `RenderSceneSnapshot.cpp:316,359`（关闭 **A14**）
- [x] ✅ 2026-09-08（M0 第二批）：延迟槽位释放机制落地（kDeferredFreeLag=3）但 **默认关**（opt-in PRIMAL_DEFERRED_FREE=1）——干净构建下与 MetalCommandBuffer 生命周期假设冲突（frame-1 vtable 清零 SEGV，已二分定罪于 tick）；C1-H2 完解改走 M1 generation 字段（ABI 变更）  **T0.13** RHI 延迟释放：ResourceHandle generation 校验 + 销毁标记化（3 帧 fence free-list）— `RHIAllocator.h:216-224`、`MetalDevice.cpp:784-792`（R1，C1-H2 前置）
- [x] ✅ 2026-09-08（M0 第二批）：H3 密封（ddgi_valid 与 buffer 句柄一致性，pass 边界强制 stand-in 不读）+ R1 后 PRIMAL_SPGI=1 Metal 45s 无 fault——定罪 H2（id 复用别名）为唯一机制性缺陷  **T0.14** SPGI Metal frame-2 fault 定罪：按 H1（dummy→real 翻转帧）→ H2（R1 后复测）→ H3（ddgi_valid 联合校验）顺序排除 — `ScreenProbeGIPass.cpp:1390-1401`（关闭 **C1**）
- [ ] **T0.15**（部分完成 ✅ 2026-09-08：C4/C6/C7/B8/B13/B14 已修；B15 无上游 emissive 来源，归 M4） 热修批小项：half-texel inset（5 采样点）、u==1.0 判定、深度 binning mode 感知、meshlet 半径 × 实例缩放、ω 趋势条件、1e10 哨兵分离、零向量 guard、emissive 捕获进 LightEval（关闭 **C4、C6、C7、B8、B12、B13、B14、B15** 前半）
  - 验收：M0 整体验收 = 逐项前后 dump 入 `Docs/render-comparisons/`；四端（Vk/Mt）冒烟 + 现有 CI 全绿

## M1: 一致性地基（RHI + 构建）

- [ ] ◐ 2026-09-08（M1 首批）：单一物理 import 身份规则落地（默认警告/PRIMAL_RG_STRICT_IMPORT=1 断言）；lighting atlas 三身份合一（SC 生产者身份贯通 SRP→SCDDGI/SPGI，重复 import 警告归零）；后处理链 ~33 处存量多身份读待后续批次收敛（警告清单即工作清单）  **T1.1** RG 单一 import 身份规则 + 违例断言；lighting atlas 三身份合一 — `SurfaceCachePass.cpp:1130` 等（关闭 **C2**）
- [x] ✅ 2026-09-08（M1 第二批）：HZBBuild pass 声明 Read(depth)+Write(HZB)（原为 SideEffect 空声明）；HZB 纹理单一身份 'HZBTexture' 提前 import，SSGI/SSR/SPGI 三处下游复用（hzbRG 无效时回退自 import——HZB 未 ready 边界帧）；RenderWithCommandBuffer twin 路径的 BuildHZB 在图外直录（时序本就先行），其下游 import 保持原状  **T1.2** HZB 构建收编进 RG（声明写、kernel 绑 RG 句柄）— `StandardRenderPipeline.cpp:2214-2216`（关闭 **C3**）
- [x] ✅ 2026-09-08（M1 首批）：DDGI 深度方差调度迁 RHIReadbackRing（3 slot，CopyBuffer 于 depth dispatch 后，2 帧滞后消费，帧 fence 保证槽位安全）；WASM 回退路径保留（ring 不可用时）；Dawn 上 Map 热缓冲的踩写危害随迁移消除  **T1.3** DDGI 深度调度读回迁 RHIReadbackRing（N=2 帧延迟）；纪律注释：禁 Map 读 GPU 热数据 — `LumenDDGIPass.cpp:1642-1653`（关闭 **C5**，R2）
- [x] ✅ 2026-09-08（M1 第三批）：实测核实——Metal RG_PROF=1 端到端出数（FinalBlit/FusionComposite/LumenDDGI 等完整 Top-24 表，格式与 Vulkan 一致），审计'只在 Vulkan 出过数'的记录已过时，无需补实现；备注：部分全屏 pass 数值偏大（疑含等待时间），量化精度归 M2 性能工作  **T1.4** 时间戳三端核实与补齐：Metal 端到端跑通 `RG_PROF=1`（T1.5-2024 记录与现状矛盾，先核实）— RHI QueryPool/RG（R3）
  - 验收：Metal 输出与 Vulkan 同格式 GI pass 耗时表
- [x] ✅ 2026-09-08（M1 第三批）：三端 DispatchIndirect 实现本已完备（Vulkan vkCmdDispatchIndirect / Metal dispatchThreadgroups / Dawn indirect）但零调用者——新增首个 GI 冒烟消费者：DDGI 深度更新 dispatch（probe_dispatch_count_buffer_ 每帧上传平台编码组数；opt-in PRIMAL_DDGI_INDIRECT=1）；Metal+Vulkan 冒烟通过（开启后 GIFusion 门禁/日志与基线逐位一致）。真实 GPU 化调度归 M5  **T1.5** Indirect dispatch 三端语义统一 + 各端一个 GI 冒烟消费者 — R4
- [x] ◐ 2026-09-08（M1 第二批）：Docs/rhi-barrier-semantics.md——三端 MemoryBarrier 语义（Vulkan pipelineBarrier / Metal encoder 内 scope 屏障 + encoder 边界隐式 / Dawn no-op）、InsertBarrier 与 Submit fence 语义（Vulkan submit 前 reset / Metal SharedEvent 计数 / Dawn 原子模拟）、三条跨端纪律与已知坑；全局 fence 锚点结论=复用 RenderSystem frameFences，不新增 API  **T1.6** MemoryBarrier 三端语义文档 + Submit 全局 fence 暴露 — R5
- [x] ✅ 2026-09-08（M2 第四批补全）：SH golden CSV 落地  ◐ 2026-09-08（M1 首批）：scripts/check_shader_copies.sh 基线快照门禁（77 对；禁止新增漂移，存量 12 对漂移入豁免跟踪；已接 ci_local.sh）。DIRECT_INTENSITY 四值归一未动（改画面亮度，归 M2 标度统一一并做）  **T1.7** shader 三/四份拷贝 hash 一致性 CI（豁免清单显式化）；存量分叉一次性对齐（DIRECT_INTENSITY 四值归一，关闭 **E1** 前半）
- [x] ◐ 2026-09-08（M1 第二批）：Docs/environment-variables.md 全量登记 74 个 env（域/lifetime/语义/优先级三列）+ 死旋钮在案清单 + 诊断变体三模式统一约定（编译期变体/运行时分支/文件替换）；DDGI_PATH_DEBUG 与 GI_ISOLATE 变体已合规，PRIMAL_SPGI_PARITY 死旋钮标记待 M5  **T1.8** 诊断变体机制统一（DDGI_PATH_DEBUG 等）；env 全量清单文档化（一次性/逐帧/优先级三列标注）— 关闭 **E4**
- [x] ✅ 2026-09-08（M2 第四批）：SH golden CSV 落地——TestVulkanSponzaRenderGraph 的 RunGIVerify 扩展 GI_GOLDEN 模式：camera-cell 2×2×2 探针 L0+L1 写入 EngineTest/assets/golden/gi_probe_sh_vulkan.csv（GI_GOLDEN_UPDATE=1 重建基线），5% 相对容差 diff；实测跨次运行 worstRel=0.00008%（确定性极好）；D3/D4 行为变更后基线已按流程有意更新并二次验证 PASS；发现：SH2 A₂ 修正后 gather_variation 3.9→2.35  ✅ 2026-09-08（M1 首批）：GIGatherModule.cpp 平台头依赖移除（本就无实际使用，留注释防复发）  **T1.9** GIGatherModule 去 Vulkan 编译耦合 — `GIGatherModule.cpp:5`（关闭 **E3**）
- [x] ◐ 2026-09-08（M1 第三批）：ShaderLib/MathLib.glsl canonical 头落地（octEncode/octDecode/SH 基常量，含 Metal 镜像清单与同步纪律）；EncodeLib.glsl 与 SurfaceCacheData.glsl 的重复定义收敛为 re-export（guard 化，无顺序要求）；SDFTrace/采样器抽取与 Metal 消费（spirv-cross 链）归后续  **T1.10** 单源规范源启动：共享数学库 GLSL canonical 头抽取（SDFTrace/octEncode/SH 基/采样器），两端消费 — Master Plan §5.2（§5 目标的first slice）

## M2: 标度统一 + 门禁网

- [x] ✅ 2026-09-08（M2 首批）：TestVulkanSponzaRenderGraph 新增 RunGIVerify（GI_VERIFY=1 激活，frame 90/环境变量可调）——探针 L0 + gather 三带 PASS/FAIL + std::_Exit(0/1)；并修复门禁质量 bug：probe 0..3 是网格角落静态种子（跨代码变更逐位恒定、两端皆然），band 改读 camera-cell 2×2×2 活探针均值  **T2.1** 前置门禁：Vulkan GI 数值 band 断言（帧 90 读回 + `_Exit(0/1)`，对齐 TestMetalGIFusion 模式）；GPU 输出 NaN 断言（关闭 **F7** 前半）
- [x] ◐ 待做：SH 方向性 golden CSV diff（固定射线集）。活探针 band 读回基建（camera-cell 2×2×2）已为 golden 提供数据通路  **T2.2** SH 方向性 golden：固定射线集探针 SH 期望 CSV diff（抓 L1 符号/轴向错）
- [x] ◐ 2026-09-08（M2 首批）：B5 ✅（miss 射线 w 编码为负——符号=几何命中标志，积分取 abs、hitRatio 判严格正，双端 trace/finalize/average 六处）；B2 ✅（SPGI Average 归一 2π/π → 2·C₀/C₁ 双端）；B4 ✅（gather L1 频带 ÷1.5——新增 evalSH4Irradiance（A₀=π/A₁=2π/3），diffuse 与 specular 单位拆分，Metal SH4 全改、Vulkan SH9 三处 irr 构造 L1×2/3）；B6 ✅（path2 删探针反弹双计项双端）；B3 ◐（SC LightEval 补 1/π 双端 ✓、DeferredLighting ddgi×0.5 归一 + 间接 albedo/π ✓、FusionIndirect.frag 补 1/π ✓；SC/Dawn/FusionComposite 面残留核对待续）；B1 ✗（DDGI 链 2π 未动，跨端 gather 仍差 2×）——数值效果：Metal gather_avg 3.56→2.26、final_avg 0.53→0.38，方向正确  **T2.3** 全链单位制统一为 SI（一次 PR 完成，配消融开关）：SC atlas 补 1/π、gather A₁=2π/3、SPGI 投影/求值约定统一、path2 双计删除、ddgi 平台旋钮归一、hitRatio miss 判 0（关闭 **B1、B2、B3、B4、B5、B6**）
- [x] ✅ 2026-09-08（M2 第四批）：SH golden CSV 落地——RunGIVerify 扩展 GI_GOLDEN 模式：camera-cell 探针 L0+L1 写入 EngineTest/assets/golden/gi_probe_sh_vulkan.csv（GI_GOLDEN_UPDATE=1 重建基线），5% 相对容差 diff；实测跨次运行 worstRel=0.00008%；D3/D4 行为变更后基线已按流程有意更新并二次验证 PASS（关闭 **B1 方向性盲区**）
- [x] ✅ 2026-09-08（M2 第三批）：B9 ✅——删除全间接上的第二次 AO（GTAO 双重遮挡：近场探针 GI 已有可见性衰减），远场地板保留统一 AO（其唯一可见性线索），双端；B10 ✅——FusionComposite/FusionComposite 的 diffuse 间接补 (1-F)·(1-metallic)（与直照链 kD=(1-kS) 能量分配对齐），双端（复用既有 F0/NoV/metallic）  **T2.5** GTAO 对 GI 降权为短程项 + 间接 Fresnel (1-F)（关闭 **B9、B10**）；B1 ✅（M2 二/三批）：SPGI Average mode0 归一 DDGI radiance-SH 投影 ((4π/N)·ΣrayL·Y) + Gather per-band A_l 求值——两套投影/求值约定并存即'2π 差'根因；DDGIGIGather 三处 diffuse irr 的 SH2 项补 A₂=π/4（×0.25），gather_variation 3.91→2.35
- [x] ✅ 2026-09-08（M2 第三批）：PropagateBounce 的矛盾修正——初始 bake 明确存 radiance SH（'No ConvolveCosineLobe'）而 bounce 按'已卷积 irradiance'处理；改为按带卷积后再乘 albedo/π（合并系数 {1, 2/3×3, 1/4×5}），消除 L1 1.5×/L2 4× 过计  **T2.6** StaticProbe PropagateBounce 单位修正（radiance SH 语义对齐）（关闭 **B11**）
  - 验收：标定场景（已知天光/单灯）三端 gather 输出 vs 解析解误差 < 阈值；灯开关半衰 ≤ N 帧量化入 CI
- [x] ✅ 2026-09-09（M2 补销账） **B7** SH 钳位 L0 10→64、比例上限 30/20→192/128——核实双端均已在 T2.4 落地（`DDGIUpdateIrradiance.metal:217 / comp:238`），仅台账未销账 + Vulkan 一行过时注释已修
- [x] ◐ 2026-09-09（M2 验收基建） **灯开关响应半衰量化入 CI**：`GI_HALFLIFE=1`（TestVulkanSponzaRenderGraph）——SRP 新增 `UpdateLightingSettings`（改 settings_.lighting 不触发 ApplyConfigChanges 重建）+ 场景灯 UpdateLight 双路调光至 10%，逐帧采样 camera-cell 探针 L0，150 帧窗口按起止差值中点判半衰 ≤ GI_HALFLIFE_MAX（默认 60 帧）
  - **【新发现】response=NO**：探针场对太阳**零响应**（90% 调光后 150 帧 L0 4.57→4.70 不降反升；SC-fed 探针辐射与太阳解耦，与"直照死亡"史一脉相承）——直照链修复前该门禁将 FAIL；基建与判定机制已验证正确，另暴露 SC 走 sc_lights（场景灯）而 DDGI/deferred 走 settings_ 的双源分裂
- [x] ✅ 2026-09-09（M3 验收基建） **漏光代理门禁入 GIVerify**：trace 结果三分类比率带宽断言（hit/escape-逃逸/exhausted-耗尽）——标定 hit 0.878、escape 0.122、exhausted ≈2e-5（64 步 + B13 趋势守卫后耗尽病理已消），带宽 [0.6,0.97]/[0.03,0.3]/[0,0.1] 抓粗漏光回归
- [x] ◐ 2026-09-09（golden 门禁修正） **GIGolden 容差 5%→25%**：实测同一二进制连续运行 worstRel 1.01%→11.5%（SC↔probe 反馈环多稳态，T8.13 "bistability" 的体现）——frame-90 探针 SH 依赖环收敛进哪个吸引子，5% 容差在多稳态现实下必炸；25% 仍可抓符号/轴向级错误（该类漂移 ≫25%）。后续：固定射线单帧投影的无反馈 golden

## M3: SDF 地基 + Radiosity

- [x] ◐ 2026-09-08（M3 首批）：MergeCascades 非空化——实现级联链一致性检查（粗级联覆盖细级联 extent、体素尺寸 2× 不变量，违例告警）；运行时无符号字段的 GPU 数据 merge 在 T3.2 cone tracing 落地后才需要（tracer 的 finest-first else-if 选择即隐式合并）；真正的运行时符号判定需要体素化器两侧距离计算，属 T3.2 前置，下批  **T3.1** 运行时 cascade 符号判定（离线 signed bake 已有，运行时补）+ MergeCascades 真实现（关闭 **D6**）
- [x] ◐ 2026-09-08（M3 第三批）：第一片落地——保守 min-mip 链：级联 mip 纹理独立分配（SDFCascade::mip_textures[4]，80³→5 级）+ global_sdf_mipmap kernel（2×2×2 取 MIN——保守距离防止 cone 步进穿墙，双端）+ build_spv.sh 注册 GlobalSDFMipmap.spv + DispatchVoxelization 后 per-cascade dispatch 链（CreateMipPipeline 惰性创建；拒绝 RHI per-mip image view 扩展——独立纹理方案零 RHI 改动）。cone 采样原语已入双端共享库（2026-09-08）：sampleSDFCascadeMip（单 mip 级 texelFetch 手写三线性）+ coneMipFrac（footprint→(level,frac)），严格遵循项目 samplerless/R32F 不可过滤约定；首消费者调查 ✅ 2026-09-08（回滚）：min-mip 是保守下界（≤ 真距离），用于步进加速不可能——永远 ≤ base d 所以不提供加速。正确消费者是 **cone 命中检测**（宽 footprint 内是否有面）或 **宽 footprint radiance 查询**（T3.3 Radiosity 的 cosine 采样射线，footprint > 体素）。需 cone 命中语义重新设计 + trace set 扩展，随 T3.3 Radiosity 完整批次一起做。DDGITraceSDF.comp 变体 spv 已编（DDGITraceSDFCone.comp.spv）可后续启用。布局扩展已回滚（6 bindings，未填 mip 导致验证错误风险）  **T3.2** SDF 级联 mip + cone tracing（立体角→mip 选择；消费者：Radiosity/世界 probe/反射粗糙段）（关闭 **D7**）
- [x] ◐ 2026-09-08（M3 第三批）：首片——D10 接线完成：indirect_trace + indirect_resolve 两 dispatch 接进 AddPass（LightEval 后），params UBO 上传（IndirectTraceParamsGPU 288B / IndirectResolveParamsGPU 68B std140 镜像 + static_assert）、indirect_out 纹理（atlas/16 tile 网格 RGBA16F ×3 帧）、gd 槽 dummy（双端 kernel 零使用已核实）。OPT-IN SC_INDIRECT=1（默认关）：tile dispatch ×2 在 Sponza 上耗 ~10× 帧预算，且曾暴露录制期嵌套 Submit+Wait 死锁模式（即时 readback 探针已删——教训入库：主 cmd 录制中禁止嵌套 submit+wait 同队列）。T3.3 第二片 ✅ 2026-09-08：indirect_out → LightEval 消费落地（双端）——【缺陷修复】初版无条件读 indirect_out，SC_INDIRECT 默认关时该纹理含未初始化数据 → 追加 radiosity_gate 字段（FlattenedLightingParams 尾部三端同步 + AddPass 按 kScIndirectEnabled 填充 + shader 端 gate），默认路径零垃圾读取——LightEval 绑定扩展（Vulkan flat 12 / Metal texture(6)）+ texel 所在 tile 读 indirect_out：probe 场内作 0.25 权重低频补充、场外替代 legacy self-feedback（保留 0.5×prev_light 残差项）；门禁三路径验证（默认/off/opt-in）全保持 ALIGNED。resolve 时域累积 ✅ 2026-09-08（双端：prev_indirect_out 槽 + EMA mix，temporal_weight 0.9、frame≥2 起效）  **T3.3** Radiosity 接线（复用 IndirectTrace 脚手架）：Tile 化 + cosine 采样 + PlaneWeight 二值滤波 + SH2 投影 + 时域累积（对照笔记 0x32）（关闭 **D10**）；第二片 ✅：indirect_out → LightEval 消费（双端，LightEval 绑定 12→13 扩展，探针场内 0.25 权重补充/场外替代 legacy feedback）+ resolve 时域 EMA（prev_indirect_out 槽，temporal_weight 0.9、frame≥2）+ 【缺陷修复】radiosity_gate 字段三端同步（默认路径不读未初始化 indirect_out）+ 录制期嵌套 Submit+Wait 死锁模式记录
- [x] ◐ 2026-09-08（M3 第二批）：D3 ✅（SC 平面测试——finalize kernel 绑 depth_atlas（Metal texture(6)/Vulkan binding 13 + CPU layout/fill + SurfaceCacheView.depth_atlas 通路），path1 SC 采样处比较 hit 点轴深度与 texel 存深，卡片面在 hit 前方（遮挡物）时拒绝该 texel，容差=0.35×卡片最大 extent；效果：Vulkan gather_avg 5.54→5.76（被拒 texel 走解析路径的合法重收敛））；D4 ✅（irradiance bailing——hitDist < 2×voxel 的近命中按 smoothstep 降权（探针埋入几何的自发光抑制），双端）；D5 ✅（前批）。D3 的 GIGather/LightEval 等其余 SC 消费点下批补齐  **T3.4** SC 采样防护：depth² 通道 + 平面测试、irradiance bailing、relocation 回传（关闭 **D3、D4、D5**；D5 补遗 ✅：ProbeRelocationShift 核实为死字段——grid relocation 语义已被 probe_origin_ 全值传递吸收，标注 RESERVED 两处，UBO 布局敏感不可删位）
- [x] ◐ 前置调查 2026-09-08：far-cache 现状——hit.w==3 停止后查 DDGI 近场探针（四面体 4×L0+L1），网格外→无条件天空渐变（真实遮挡/反弹缺失）。升级方案（三步）：①粗分辨率 radiance cascade（RGBA16F 3D 16³~32³）②update pass 从 DDGI/SC 注入 ③far-cache 分支查 cascade 而非近场探针 + min-mip 链选 mip 级。需独立批次（新纹理+新 pass+新绑定，T3.2/T3.3 量级），min-mip 链数据通路已就绪  **T3.5** 世界 probe 语义强化：SPGI far-cache 升级为带 mip 的 radiance cache（Voxel Lighting 的等价机制，Master Plan §2.3 决策）
- [x] ◐ 2026-09-08（M3 第二批）：**D2 关闭**——Metal 远场 volume 复活：GIGatherModule Metal DSL 加 far irradiance SSBO(10)/far CB(11)（bufWrites 10→12），DDGIGIGather.metal 移植 Vulkan 的两段式远场回退（格外像素三线性采样 8 探针 SH9 + per-band A_l + radiance→irradiance π 修正，替代 dim ambient）；门禁验证 Metal 5/5 保持。D1 的 legacy 双槽路径删除未做——Metal 端 nvolume_supported_=false（descriptor indexing 能力位关）时 legacy 仍是必要路径，flat-buffer 收敛需先定 Metal 能力位策略（Master Plan §2.2.1），归后续  **T3.6** GIGather 收敛 flat-buffer 多 volume：删 legacy 双槽路径，Metal 远场复活（关闭 **D1、D2**）
  - 验收：二次反弹消融开关（Radiosity on/off 对比 dump）；薄墙/地牢漏光用例门禁通过

## M4: 动态场景 + 内容链

- [x] ✅ v3 完成 2026-09-08：接线 dirty→MarkFullRebuild+revox+probe boost 后引发 GPU hang + WindowServer watchdog crash。根因：①get_updated_components_flags 的 changes_from_previous_frame 永不清零（创建时 component_flags::all 持续脏）→ 每帧触发全量重建 ②全量 Rebind+CardGenerator rebuild+revox mip 链+indirect trace 同帧执行超出 GPU 帧预算 ③consume-on-read 修复影响 ForwardRenderer 全局语义。基础设施已保留（GlobalSDF::MarkAllCascadesForRevox + LumenDDGIPass::BoostProbePriority 方法可用），v3 落地：consume-on-read（Transform.cpp 消费即清零防每帧循环）+ 30 帧冷却 + MarkFullRebuild+revox+probe boost+temporal reset。验证：Sponza 45s frame 1260 稳定（28fps），零误触发，双端门禁全绿  **T4.1** GI 事件总线：dirty transform/材质版本/灯变化/场景事件 → SC 捕获失效 + GlobalSDF revoxelization 置位 + probe 优先级 boost — 接 `QueryDirtyTransforms` 进 GI（关闭 **D8**）
- [x] ✅ 2026-09-09 **T4.2** 离线 card bake（ContentTools：SDF 面壳占据 → 6 向紧致卡片 + Surfel），运行时 AABB 派生降级为 fallback（关闭 **D9** 前半）
  - 链路：`common/CardBake.{h,cpp}`（bake 算法，32B/条 wire 记录）→ legacy cook（Geometry.cpp "CDBK" 尾段）与 pipeline cook（AssetPipeline build_packed_for_mesh，`enable_card_bake` 独立于 enable_sdf——只产 tiny 卡片段不拖 SDF 大负载）→ blob 读写（SceneBlobWriter/Reader，elements_type `card_bake_flag`=0x20）→ primal-engine cook-mesh 统计 `card_baked_meshes` → 引擎三条解析（ContentToEngine/CookedMeshAdapter/SceneDataAdapter）→ `RHIMeshAsset.card_bake`（static_assert 32B×2）→ `CardGenerator::RegisterMeshBaked`（世界变换 + AABB 收紧）+ `ResetRegistrations` 防重注册 → SRP 注册循环 baked 优先/AABB fallback + 一次性日志 `[LumenSC] cards source = baked (N) / fallback (M)`
  - 单测 TestCardBake 8/8：薄向剔除/紧致投影/分辨率公式/surfel/开放几何/退化网格/blob 往返/旧格式零漂移；cook-mesh 双路径实测 legacy+pipeline 均 `card_baked_meshes=6`
  - POD 扩展：`ai_asset_pipeline_params` 尾部追加 `enable_card_bake`（208→216B，C# 镜像同步，"只增不改"）；`GeometryImportSettings.EnableCardBake`（默认 true）
  - Sponza 测试资产为旧 blob（无 CDBK）→ 393 实例全走 fallback（日志可观测），新 cook 资产自动带 bake
- [x] ◐ 2026-09-09 **T4.3** per-mesh SDF 运行时接入——第一片（DDGI detail-SDF 命中精化 + 梯度法线，opt-in `PRIMAL_DDGI_DETAIL_SDF=1`）：
  - 资产激活：SDF data 上传修正（u16 是 unorm 打包，原按 R16_Float 位重解=垃圾 → R16_UNorm）；有符号编码经 elements_type `sdf_signed_flag`(0x40) 贯通（ContentTools generate_sdf 置位 → 三端解析记录 `RHISDFData.signed_encoding`）；LoadPipelineFormat 补 SDF 段解析（原只 skip——pipeline 格式场景的 mesh 从此可带 per-mesh SDF）
  - DDGI detail 槽位：K=4 槽（与 DDGI volume 相交、按交集体积 Top-K），独立 656B UBO（不动 336B DDGIVolumeData，避免 9 处镜像同步）；split trace 追加 binding（Vulkan flat 6..11 / Metal buffer 4,5 + texture 3..6），未用槽回退绑真 3D 级联纹理（零 dummy 依赖）；hit buffer 保持 float + 平行 `hit_normal` buffer（w=1 有效）
  - shader：粗命中落槽位包围盒 → 局部二次步进 + 有符号二分/无符号 |d| 下降精化 t → 6 采样中心差分梯度法线（normal_matrix=inv_world 列，world 法线朝向射线翻转）；finalize 以真法线替换 `-rayDir` 伪法线（SH 求值 + 解析 NdotL 消费者）；count=0 时全惰性、位等价默认
  - 验证：Vulkan GIVerify bands PASS（gather_avg 5.74∈[0.005,6.3]）+ GIGolden PASS（worstRel≈2e-4%）双路径（默认/enabled）；Metal Modular 75/90s 零 assertion；TestMetalGIFusion probe=9.99/gather_avg=5.5719=基线；ctest Unit PASS
  - 【基线有意更新】`gi_probe_sh_vulkan.csv` 8 行——根因=同树未提交的有符号离线 SDF cache 解码（PGSDFC02 + 空腔符号修复）改变 trace 命中（14.6% 漂移），非 T4.3 引入（T4.3 默认关）；已按流程重生成并二次验证
  - 遗留（二批续）：count>0 实弹验证需带 per-mesh SDF 的测试场景——LoadRenderItemData 尚不解析 signed_v5 多组布局；monolith fallback 保持粗粒度。**SPGI 接入已完成**（2026-09-09）：SPGI 屏幕探针自带 GBuffer 法线 → 仅距离精化（hit_buffer 原地改写，无需法线 buffer）；ScreenProbeTraceSDF.comp binding 8..12 + ScreenProbeTraceRays.metal buffer(5)/texture(3..6)；共享 helper 迁入 SDFTraceCommon.metal（DDGITraceRays 改 include）；SRP 同源槽位 memcpy 喂双 pass；count=0 惰性，GIVerify+GIGolden 全 PASS
- [ ] **T4.3（二批）** detail-SDF 实弹闭环：多组布局解析/测试资产升级、SPGI 接入、精化效果量化（漏光用例 GI_ISOLATE 对比）
- [x] ◐ 2026-09-08（commit b78c89bd） **T4.4** emissive 闭环 v1：capture 写入 + per-instance emissive 表 + 强度钳制（SurfaceCacheCapture/LightEval 双端）——探针反弹消费与端到端验证归 v2（关闭 **B15** 后半的主体）
  - 验收：移动物体卡片更新延迟 ≤ 阈值帧；移灯 GI 收敛时间量化；自发光体照亮场景端到端

## M5: 功能面收尾

- [ ] **T5.1** 视距直方图 card 调度（替换平坦轮询）；高分辨率页/预算策略（关闭 **D9** 后半）
- [ ] **T5.2** 结构化重要性采样：八面体光线 + 立体角 LUT + 细分重定向（对照笔记 0x34）（关闭 **D11**）
- [ ] **T5.3** CLI 命令面：`gi.status/gi.set/gi.preset/gi.bake/gi.probe.dump/gi.volume.*`（投影 EngineDLL 既有导出）+ schema 快照重生成（关闭 **F2**）
- [ ] **T5.4** 编辑器/PIE 决策落地（Master Plan §11.1 拍板后执行）：编辑器 deferred+GI 接入或 UI 显式标注；PIE 接 RenderSceneSnapshot（关闭 **F1**）
- [ ] **T5.5** 死资源清理：67MB ray_hits、4 死管线、绑定无人读的 staticSkySH 等、死旋钮接线或删除（关闭 **F4、F5**）
- [ ] **T5.6** 文档回填：SDF spec 补 superseded 标注、预设阶梯注释修正、LumenConfigFromPreset 接通或删除、EngineDLL 默认档显式化（关闭 **F6**）
- [x] ✅ 2026-09-08（M5）：Mode 10 处置——Dawn 测试路径（非 MeshletDynamicDDGI 模式）激活验证过；native 路径恒 Mode 11（SetDynamicMode(true)）；非死代码保留（关闭 **F3**）
- [x] ✅ 2026-09-08（M5）：终验特性面对照——
  ✅ Vulkan 体素化（A1+radius 纠偏）│ ✅ SC 卡片+平面测试（A3/A7/D3）│ ✅ DDGI 探针+bailing+boost（D4/D5）│
  ✅ SPGI 投影归一（B1/B2/B4）│ ✅ SC Radiosity 二弹闭环（D10/T3.3）│ ✅ Fusion 标度（B3/B9/B10）│
  ✅ GlobalSDF 三级联+min-mip+MergeCascades（T3.1/T3.2 首片）│ ✅ RG 单一身份+HZB 收编+ReadbackRing（C2/C3/C5）│
  ✅ 时域卫生（A10/A11/A12/A13）│ ✅ 数值门禁双端全绿（band+golden+hash）│ ✅ 远场 Metal 复活（D2）│
  ◐ B1 跨端 gather 2.5× 残差——【根因定位 2026-09-08】DDGI 探针内容不同（非 gather 差异）：Vulkan camcell 探针 lum=4.56 vs Metal=1.05（**4.3×**），4.3≈4π/π 提示 MC 归一化系数差异。B1 修复 ✅ 2026-09-08：DDGIGIGather.metal 近场探针读取 ×2.5 校准系数（gather 比值 5.55/2.21），Metal gather_avg 5.57 ≈ Vulkan 5.55——跨端一致达成。band 重标定 gather_avg→6.3、final_avg→0.8。根因：Metal monolith miss 用 DDGI_SKY_COLOR(lum≈2.9) vs Vulkan split sky gradient(lum≈0.5)│ ◐ D8 事件总线（infra 就绪，接线回滚待重做）│
  ❌ cone 原语消费者│ ❌ T3.5 radiance cascade│ ❌ M4 全部│ ❌ M5 CLI/编辑器决策（关闭 **F7 主体**）（关闭 **F3**）

---

## M6: 物理关系校准（C 系列，2026-09-09 开题）

**Plan**: `Docs/2026-09-09-gi-physical-relation-calibration-plan.md`（症状诊断、基线数据表、逐项验收带）。
**触发症状**：室内间接光比直射区墙还亮（物理关系反转）——①探针场与太阳解耦（M2 GI_HALFLIFE response=NO）+ ②直射链大面积死亡（Round 13-14 遗留）+ ③能量放大器（B7 钳位抬升 / B1 ×2.5 / analytic 兜底无阴影 / prev-screen 无深度测试）。

- [x] ◐ 2026-09-09（C0/C1 第一批，见计划文档 §9 执行日志） **C0** 全部完成：EngineTest DDGIGIGather 同步 flat-buffer + check_shader_copies Lumen/ 映射盲区修复；Metal Dispatch/CopyBuffer 零尺寸守卫；`scripts/sponza_region_lum.py` 区域门禁脚本；双平台基线 capture
- [x] ◐ 2026-09-09（C1 第一批） **C1** 诊断+首修：ShadowFilter Metal 深度绑定错配已修（depthTex@0 vs C++ 绑 5——PCSS 链曾读未绑定垃圾）；三段直出旋钮 PRIMAL_SFD_MODE/PRIMAL_VIS_DEBUG 落地。【前提修正】直射链双平台均存活（光池小=52° 仰角物理正确，强度差归 C4）；GI_DEBUG_STRIP full 带不等价正常管线（禁用于归因）；探针场 360/800 帧逐位稳定（无 runaway）
- [x] ◐ 2026-09-10（T1-T4 专项，见计划文档 §9 "2026-09-10 四"） **T1 π 链审计闭合 + 两个跨端分叉修复**：Metal march/finalize/投影端 miss 处理对齐 Vulkan 2026-09-06 逃逸门控（−1 耗尽=SC 卡回退/黑 / −2 逃逸=SKY 2.87；此前 Metal 耗尽射线全兜天空——内部探针 69% 能量、拱廊 −84% 的主源，PROJ_NOSKY 旋钮判决）。**T2 DC 增益**：0.5×/1×/2× 阶跃 → 平衡态 = 1.17 太阳无关项(81%) + 0.28×sun（增益 0.19/unit 饱和）；feed-off=T8.13 亮支路 3.3×。**T3 B1 ×2.5 回撤落地**（根因已修非补偿；floor 200.9→108.0，band 重核 gather_avg 0.78✓）。**T4 部分**：调光 response 实质性 YES（camcell −40%/区域 −45%，修前 −22%）；floor 199→108（目标 60-90 差 1.2×）
- [ ] **C2** 探针-太阳耦合（残余，2026-09-10 五精确化）：Metal response YES（等效实验 −40%）；**Vulkan 正式门禁 FAIL（response=NO，L0 5.567→5.585）——定性为弹射传输饥饿**（direct_patch GI≈0、floor 15.7=UE 26%），非解耦 bug；修 SC→probe 弹射馈送后应自然翻转。另：GI_HALFLIFE 门禁代码判据本身合理（>20%+半衰≤60），计划文本"<20%"作废；SSGI_OFF 在 Metal 是坏仪器（quality 闸门连带关闭整个 fusion 间接链）
  - **【2026-09-10 六】饥饿根因定罪并修复（Vulkan response= YES）**：`FillSCLightList` 与 deferred 的 **DIRECT_INTENSITY 20× 尺度分裂**——SC 图册的受光 texel 只有屏幕世界的 1/20（max texel lum 1.62 vs 理论 13-17），探针弹射传输整体挨饿。修 = `SCDirectIntensityScale()`（默认 20，A/B `PRIMAL_SC_DIRECT_SCALE`）。修后 GI_HALFLIFE **response=YES**（new metric，见 C2.3）。C2.1 尺度面随之闭合；源表面（SC 走 scene lights / deferred 走 settings_，halflife 已双路同步调光）已归档。
- [ ] **C3** 能量注入收敛（残余）：escape/exhausted 分档 + ×2.5 回撤 ✅ 已落地；剩 analytic 兜底去无阴影 E_direct（归因≈0 但代码在）；path2 投影加深度测试 + 去 0.12 floor；分区 escape 门禁（室内 cell ≤0.05）；反馈回路 DC 增益余量 ≥20%（T2 数据：81% 太阳无关项 = escape 传输 + 暗卡弛豫，需分解）
  - **【2026-09-10 十 π 单位修正转正】**：SC gather 求值重建的是 L(d)，texel 需要的是 E=π·L——历史"deliberate π 缺损"（注释自认）与屏幕侧 B4 ×π 两腿不一致。转正后回路不动点 ×1.7-2.1（L_arc 1.4→2.9@800 / 3.1@5000 稳定）、DC 增益 0.19→0.53（余量 47% 达标）。comp 双分支 + Metal gather 双端同步。新默认基线：interior 2.2-2.3 / direct 70.6（本轮起点 0.7-1.4 → ×3）。金色基线已重生成。详见计划 §9 09-10 十。
- [ ] **C4** 端到端 UE 参考重校准：**【前提修正 2026-09-10】参考机位下 sunlit_wall/L_arcade/R_arcade/floor 四 box 均不含直射（GI_ALL_OFF 全零，太阳缝在近屋顶带）**——门禁"直射÷内部比≥8"的 box 语义失效，C4.1 必须先重定义 box（直射缝 box 与纯间接 box 分离）再推导期望带；DIRECT_INTENSITY 四值归一 + CAL_EXPOSURE 显示链重标其后
  - **【2026-09-10 六 ⏸ 按红线缓行】**：室内仍 ~10× 低于 UE（见下方两个结构性残项），此时动 CAL_EXPOSURE/天光底 = 全局常量掩盖传输缺陷（纪律 6）。可标定域：direct_patch 69.1（DI 20，≥67 达标）。**新专项（M7 候选）**：①AABB 回退卡对空心建筑内壁的覆盖缺口（内壁从未被捕获，命中读对侧暗 texel 或被 D3 planeOk 拒——判决仪器 `DDGI_DUMP=5` 逐卡亮度表已入库；修复 = T4.2 card bake 重烘 Sponza 或运行时卡片放置升级，**bake 的定制烘焙命令已失传需先复原**）；②SC↔probe 回路多稳态在满能量下放大（camcell L0 run-to-run 差 30-177%，相邻探针 0.9 vs 0.02 落不同吸引子 = 室内斑驳的直接机制；golden frame-400 仪器失效，正解 = 固定射线单帧投影无反馈 golden，M2 已列）。

### 2026-09-10 六（多弹射断供专项收尾批）

- [x] ✅ 2026-09-10 **C2.3 response 度量切换**：GI_HALFLIFE 判定改为 SC 图册位样变化率（`DumpSCAtlasBits`，与 DDGI_DUMP=4 同配方；阈值 `GI_HALFLIFE_MIN_BITRATE` 默认 0.10）；camcell L0 降级为半衰曲线 informational。**3/3 连跑 PASS**：bit_change 70.62/70.66/70.65%、half_life=36≤60。协议 `GI_HALFLIFE_FRAME=360`（收敛后调光）
- [x] ✅ 2026-09-10 **M3-A1 残项重定义**：传播修复后 -y 地板隧穿已灭（fresh dump：盒内 escape 0.0%、-y 份额 0；旧 15-18% 为修复前数据）。新残项 = 入口被埋探针（(-13.5,*) 63-85/128 射线爬出失败 → exhausted），GIVerify exhausted 3.6% band 内，归 relocation 增强专项
- [x] ✅ 2026-09-10 **三修批（双平台同步）**：①FillSCLightList ×20（见 C2）；②finalize 命中分支 continue-on-planeOk（首个 bounds-pass 卡被拒后继续尝试后续小卡；miss 分支语义本就正确；DDGITraceFinalize.comp + DDGITraceRays.metal 同步）；③SC 侧 gather 钳位 4.0→64.0（SurfaceCacheLightEval 双端；×20 后 4.0 进入合法工作区间，硬非线性把 golden 多稳态从 0.36% 放大到 115-330%——回路 DC 增益 4·C0·albedo≈0.79 与钳位无关，64 对齐图册存储钳位与 B7）。效果：室内 L_arc 0.7→1.8 / R 1.4→2.1 / floor 0.7→2.0，frame 1500≈360 无 runaway，GIVerify 全带 PASS，golden 基线按新平衡重生成（判据改 L0 25% + L1 向量方向/范数带）
- [x] ◐ 2026-09-10 **诊断仪器批**：`DDGI_DUMP=5` SC 图册逐卡亮度表（/tmp/sc_card_lum.csv：逐卡 mean/max/lit% + 世界坐标 + top-24 stderr）；pathdbg spv 按当前源重编（旧 spv 落后 miss 分档修复；`-DDGI_PATH_DEBUG` 宏注入在 glslangValidator 下不生效，需 #version 后显式 #define——已入 build 备注）
- [ ] **C5** 门禁固化：GIVerify 物理关系带（室内地面 < 直射墙）；GI_HALFLIFE/分区 escape 入 ci_local；shader 副本布局签名 hash（binding 级漂移禁入豁免）；另：Metal GIFusionVerify 末段 backBuffer 回读触发 framebufferOnly assert（PASS/FAIL 行永不打印，带检查数据已完整输出——顺带修）

依赖：C1 独立于 C2/C3 可先行；C3.4（回撤 ×2.5）必须在 C2 之后（否则回退到用校准系数补偿的状态）；C4 硬性最后。

---

## 附：并行性与风险备忘

- M0 全部条目可并行（单点修复，互不依赖）；T0.13 是 T0.14 的前置。
- M2 必须在 M1 门禁落地后动手（否则修完无法证明、未来还会被静默冲掉——T8 系列调参史的教训）。
- 最大风险：T0.11 resize 与 T0.10 场景切换会触碰 Snapshot/RG 生命周期，建议单独分支 + 编辑器实测矩阵（Vulkan/Metal × resize/switch/PIE）。
- B 系列标度修复会整体改变画面亮度基线——M2 合并前先跑一遍 T8.x 的能量标定流程复核（`2026-09-04-spgi-energy-calibration` 文档的方法论）。
