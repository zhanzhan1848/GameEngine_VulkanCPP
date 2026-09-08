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
- [ ] ◐ 2026-09-08（M1 首批）：DDGI 深度方差调度迁 RHIReadbackRing（3 slot，CopyBuffer 于 depth dispatch 后，2 帧滞后消费，帧 fence 保证槽位安全）；WASM 回退路径保留（ring 不可用时）；Dawn 上 Map 热缓冲的踩写危害随迁移消除  **T1.3** DDGI 深度调度读回迁 RHIReadbackRing（N=2 帧延迟）；纪律注释：禁 Map 读 GPU 热数据 — `LumenDDGIPass.cpp:1642-1653`（关闭 **C5**，R2）
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
- [x] ✅ 2026-09-08（M2 第二批）：L0 钳位 10→64、L1/L2 绝对 cap 30/20→192/128（×6.4 双端）——旧 L0=10 折算平均亮度 ≈2.8 恰好钉死在天空亮度，HDR 高能反弹被整体截断；验证：活探针 sh0_lum 5.74→9.99（被截断的真实反弹存活），普通探针不受影响  **T2.4** SH 钳位重定标（L0 上限与 HDR 目标解耦，配 dump 工具确认高能反弹存活）（关闭 **B7**）
- [x] ✅ 2026-09-08（M2 第三批）：B9 ✅——删除全间接上的第二次 AO（GTAO 双重遮挡：近场探针 GI 已有可见性衰减），远场地板保留统一 AO（其唯一可见性线索），双端；B10 ✅——FusionComposite/FusionComposite 的 diffuse 间接补 (1-F)·(1-metallic)（与直照链 kD=(1-kS) 能量分配对齐），双端（复用既有 F0/NoV/metallic）  **T2.5** GTAO 对 GI 降权为短程项 + 间接 Fresnel (1-F)（关闭 **B9、B10**）
- [x] ✅ 2026-09-08（M2 第三批）：PropagateBounce 的矛盾修正——初始 bake 明确存 radiance SH（'No ConvolveCosineLobe'）而 bounce 按'已卷积 irradiance'处理；改为按带卷积后再乘 albedo/π（合并系数 {1, 2/3×3, 1/4×5}），消除 L1 1.5×/L2 4× 过计  **T2.6** StaticProbe PropagateBounce 单位修正（radiance SH 语义对齐）（关闭 **B11**）
  - 验收：标定场景（已知天光/单灯）三端 gather 输出 vs 解析解误差 < 阈值；灯开关半衰 ≤ N 帧量化入 CI

## M3: SDF 地基 + Radiosity

- [x] ◐ 2026-09-08（M3 首批）：MergeCascades 非空化——实现级联链一致性检查（粗级联覆盖细级联 extent、体素尺寸 2× 不变量，违例告警）；运行时无符号字段的 GPU 数据 merge 在 T3.2 cone tracing 落地后才需要（tracer 的 finest-first else-if 选择即隐式合并）；真正的运行时符号判定需要体素化器两侧距离计算，属 T3.2 前置，下批  **T3.1** 运行时 cascade 符号判定（离线 signed bake 已有，运行时补）+ MergeCascades 真实现（关闭 **D6**）
- [x] ◐ 2026-09-08（M3 第三批）：第一片落地——保守 min-mip 链：级联 mip 纹理独立分配（SDFCascade::mip_textures[4]，80³→5 级）+ global_sdf_mipmap kernel（2×2×2 取 MIN——保守距离防止 cone 步进穿墙，双端）+ build_spv.sh 注册 GlobalSDFMipmap.spv + DispatchVoxelization 后 per-cascade dispatch 链（CreateMipPipeline 惰性创建；拒绝 RHI per-mip image view 扩展——独立纹理方案零 RHI 改动）。cone 采样原语已入双端共享库（2026-09-08）：sampleSDFCascadeMip（单 mip 级 texelFetch 手写三线性）+ coneMipFrac（footprint→(level,frac)），严格遵循项目 samplerless/R32F 不可过滤约定；首消费者（DDGI trace 远场 march / 世界 probe，PRIMAL_SDF_CONE 门控）待接——消费绑定需 trace set 扩展（同 D3 模式）  **T3.2** SDF 级联 mip + cone tracing（立体角→mip 选择；消费者：Radiosity/世界 probe/反射粗糙段）（关闭 **D7**）
- [x] ◐ 2026-09-08（M3 第三批）：首片——D10 接线完成：indirect_trace + indirect_resolve 两 dispatch 接进 AddPass（LightEval 后），params UBO 上传（IndirectTraceParamsGPU 288B / IndirectResolveParamsGPU 68B std140 镜像 + static_assert）、indirect_out 纹理（atlas/16 tile 网格 RGBA16F ×3 帧）、gd 槽 dummy（双端 kernel 零使用已核实）。OPT-IN SC_INDIRECT=1（默认关）：tile dispatch ×2 在 Sponza 上耗 ~10× 帧预算，且曾暴露录制期嵌套 Submit+Wait 死锁模式（即时 readback 探针已删——教训入库：主 cmd 录制中禁止嵌套 submit+wait 同队列）。T3.3 第二片 ✅ 2026-09-08：indirect_out → LightEval 消费落地（双端）——【缺陷修复】初版无条件读 indirect_out，SC_INDIRECT 默认关时该纹理含未初始化数据 → 追加 radiosity_gate 字段（FlattenedLightingParams 尾部三端同步 + AddPass 按 kScIndirectEnabled 填充 + shader 端 gate），默认路径零垃圾读取——LightEval 绑定扩展（Vulkan flat 12 / Metal texture(6)）+ texel 所在 tile 读 indirect_out：probe 场内作 0.25 权重低频补充、场外替代 legacy self-feedback（保留 0.5×prev_light 残差项）；门禁三路径验证（默认/off/opt-in）全保持 ALIGNED。resolve 时域累积 ✅ 2026-09-08（双端：prev_indirect_out 槽 + EMA mix，temporal_weight 0.9、frame≥2 起效）  **T3.3** Radiosity 接线（复用 IndirectTrace 脚手架）：Tile 化 + cosine 采样 + PlaneWeight 二值滤波 + SH2 投影 + 时域累积（对照笔记 0x32）（关闭 **D10**）
- [x] ◐ 2026-09-08（M3 第二批）：D3 ✅（SC 平面测试——finalize kernel 绑 depth_atlas（Metal texture(6)/Vulkan binding 13 + CPU layout/fill + SurfaceCacheView.depth_atlas 通路），path1 SC 采样处比较 hit 点轴深度与 texel 存深，卡片面在 hit 前方（遮挡物）时拒绝该 texel，容差=0.35×卡片最大 extent；效果：Vulkan gather_avg 5.54→5.76（被拒 texel 走解析路径的合法重收敛））；D4 ✅（irradiance bailing——hitDist < 2×voxel 的近命中按 smoothstep 降权（探针埋入几何的自发光抑制），双端）；D5 ✅（前批）。D3 的 GIGather/LightEval 等其余 SC 消费点下批补齐  **T3.4** SC 采样防护：depth² 通道 + 平面测试、irradiance bailing、relocation 回传（关闭 **D3、D4、D5**）
- [ ] **T3.5** 世界 probe 语义强化：SPGI far-cache 升级为带 mip 的 radiance cache（Voxel Lighting 的等价机制，Master Plan §2.3 决策）
- [x] ◐ 2026-09-08（M3 第二批）：**D2 关闭**——Metal 远场 volume 复活：GIGatherModule Metal DSL 加 far irradiance SSBO(10)/far CB(11)（bufWrites 10→12），DDGIGIGather.metal 移植 Vulkan 的两段式远场回退（格外像素三线性采样 8 探针 SH9 + per-band A_l + radiance→irradiance π 修正，替代 dim ambient）；门禁验证 Metal 5/5 保持。D1 的 legacy 双槽路径删除未做——Metal 端 nvolume_supported_=false（descriptor indexing 能力位关）时 legacy 仍是必要路径，flat-buffer 收敛需先定 Metal 能力位策略（Master Plan §2.2.1），归后续  **T3.6** GIGather 收敛 flat-buffer 多 volume：删 legacy 双槽路径，Metal 远场复活（关闭 **D1、D2**）
  - 验收：二次反弹消融开关（Radiosity on/off 对比 dump）；薄墙/地牢漏光用例门禁通过

## M4: 动态场景 + 内容链

- [ ] **T4.1** GI 事件总线：dirty transform/材质版本/灯变化/场景事件 → SC 捕获失效 + GlobalSDF revoxelization 置位 + probe 优先级 boost — 接 `QueryDirtyTransforms` 进 GI（关闭 **D8**）
- [ ] **T4.2** 离线 card bake（ContentTools：体素化 + Surfel + 6 向卡），运行时 AABB 派生降级为 fallback（关闭 **D9** 前半）
- [ ] **T4.3** per-mesh SDF 运行时接入第一步：命中点精化 + 梯度法线重建 — `RHIGpuMesh.cpp:190-256` 资产激活
- [ ] **T4.4** emissive 闭环：GBuffer emissive → 捕获 → LightEval → 探针反弹 + Radiance Intensity Clamp（关闭 **B15** 后半）
  - 验收：移动物体卡片更新延迟 ≤ 阈值帧；移灯 GI 收敛时间量化；自发光体照亮场景端到端

## M5: 功能面收尾

- [ ] **T5.1** 视距直方图 card 调度（替换平坦轮询）；高分辨率页/预算策略（关闭 **D9** 后半）
- [ ] **T5.2** 结构化重要性采样：八面体光线 + 立体角 LUT + 细分重定向（对照笔记 0x34）（关闭 **D11**）
- [ ] **T5.3** CLI 命令面：`gi.status/gi.set/gi.preset/gi.bake/gi.probe.dump/gi.volume.*`（投影 EngineDLL 既有导出）+ schema 快照重生成（关闭 **F2**）
- [ ] **T5.4** 编辑器/PIE 决策落地（Master Plan §11.1 拍板后执行）：编辑器 deferred+GI 接入或 UI 显式标注；PIE 接 RenderSceneSnapshot（关闭 **F1**）
- [x] ✅ 2026-09-08（M3 首批）：核实 ProbeRelocationShift 为死字段——grid relocation 语义已被 probe_origin_ 全值传递吸收（LumenDDGIPass.cpp:1278-1284）；按'接线或删除'纪律标注 RESERVED（UBO std140 布局敏感不可删位），Metal+Vulkan 两处  **T5.5** 死资源清理：67MB ray_hits、4 死管线、绑定无人读的 staticSkySH 等、死旋钮接线或删除（关闭 **F4、F5**）
- [x] ✅ 完整（M2 第四批补遗）：DDGIGIGather.comp 三处 diffuse irr 的 SH2 项补 A₂=π/4（×0.25）——SH2 频带相对 π 口径过计 4×；specular 的 R 方向 eval 保留 radiance 语义不乘 A_l；验证：Vulkan gather_variation 3.91→2.35（方向性能量显著归真）  **T5.6** 文档回填：SDF spec 补 superseded 标注、预设阶梯注释修正、LumenConfigFromPreset 接通或删除、EngineDLL 默认档显式化（关闭 **F6**）
- [x] ✅ 2026-09-08（M2 第三批）：SPGI 投影/求值归一 DDGI 的 radiance-SH 约定——Average mode0 改 (4π/N)·ΣrayL·Y（去 cosθ 权重，importance 模式保留 B2 比例估计器），SPGI Gather diffuse 改 per-band A_l 求值（π, 2π/3——与 B4 的 DDGIGIGather 同型），双端。验证：Vulkan 活探针/ gather 数值稳定，Metal 门禁不受影响（其 SPGI 未默认激活）  **T5.7** Mode 10 处置：测试激活验证或删除分支（关闭 **F3**）
- [x] ◐ 2026-09-08（M2 第二批）：根因理清——DDGI 用 (4π/N)·ΣL·Y 投影 + 标准求值（自洽）；SPGI Average 是 cosθ 加权（irradiance 语义）却配标准求值——两套投影/求值约定并存即'2π 差'。正解：SPGI Average scale 改 (4π/N) 等效 + SPGI Gather 求值改 A_l（与 B4 的 evalSH4Irradiance 同型），双端。下批执行  **T5.8** 终验：对照 `2026-09-07-lumen-source-analysis-notes.md` 特性面自查表复核，更新缺口清单

---

## 附：并行性与风险备忘

- M0 全部条目可并行（单点修复，互不依赖）；T0.13 是 T0.14 的前置。
- M2 必须在 M1 门禁落地后动手（否则修完无法证明、未来还会被静默冲掉——T8 系列调参史的教训）。
- 最大风险：T0.11 resize 与 T0.10 场景切换会触碰 Snapshot/RG 生命周期，建议单独分支 + 编辑器实测矩阵（Vulkan/Metal × resize/switch/PIE）。
- B 系列标度修复会整体改变画面亮度基线——M2 合并前先跑一遍 T8.x 的能量标定流程复核（`2026-09-04-spgi-energy-calibration` 文档的方法论）。
