# DDGI 重定位坐标契约修复（effective origin）与 GILadder 台账公式修正

> 2026-09-17 ｜ 依据 [2026-09-17-probe-selection-density-audit](render-comparisons/2026-09-17-probe-selection-density-audit/README.md)
> §5（P0 重定位坐标不一致）与 §8.1（P0 台账判定器公式失真）实施的代码修复。
> 状态：✅ 已完成并通过回归；中庭亮度的量化影响已记录（见 §4），后续测量见 §6。

## 1. 修了什么

### 1.1 P0-1：DDGI 统一 effective origin（Vulkan + Metal 孪生）

审计确认的缺陷：`DDGITraceSDF` 从重定位后的起点 P1 追踪并写出 `hit_distance = t`，
`DDGITraceFinalize` 却用原网格点 P0 重建 `hitPos = P0 + t·d`。只要 probe 被移动过
（位移上限 0.95×spacing，深埋借格最远 ≈5.2 m），命中位置、卡片查询、可见性比较
全部错位 ΔP = P1 − P0。

修复（审计 §5.3 推荐修法 1/2/3 的最小闭环）：新增**每 probe 一个 float4 的
relocated origins 缓冲**（xyz = 重定位后有效起点，绝对 probeIdx 索引），由 trace
发布、其余消费方读取，**同帧版本唯一事实源**。w 通道承载 §5.3-4 的分类：

- `1.0` = 本格自由 / 重定位成功（对原格位的独立测量）；
- `0.5` = 深埋借格（26 邻格搜索后从最开阔邻格中心起 trace——SH 描述的是借来格
  的环境，**不得当作原格位的独立完整测量**）；
- `0` = 未 trace 过（回退逻辑格点，首帧防御）。

| 端 | 文件 | 变更 |
|---|---|---|
| Vulkan | `Vulkan/Shaders/Lumen/DDGITraceSDF.comp` | 新增 binding 12（writeonly），relocation 带 `out state`，`tid==0` 写 `vec4(P1,state)` |
| Vulkan | `Vulkan/Shaders/Lumen/DDGITraceFinalize.comp` | 新增 binding 17（ro）；hitPos 重建与 miss 卡片查询（scGridCellIndex / worldToCardUV）改用有效起点 |
| Vulkan | `Vulkan/Shaders/Lumen/DDGIGIGather.comp` | 新增 binding 19（ro）；近场可见性 distToProbe 锚点改为有效起点；**借格探针（w=0.5）可见性通过后打半权**，插值权重仍沿逻辑格 |
| Metal | `Metal/shaders/Lumen/DDGITraceRays.metal` | sdf 入口新增 `buffer(6)` 写（含 state）、finalize 入口新增 `buffer(12)` 读，语义与 Vulkan 对等 |
| Metal | `Metal/shaders/Lumen/DDGIGIGather.metal`（+ EngineTest 同名拷贝） | 新增 `buffer(12)`，物理锚点 + 借格半权同 Vulkan |
| C++ | `Lumen/DDGI/LumenDDGIPass.{h,cpp}` | `relocated_origins_` 缓冲（totalProbes×16B，初始化显式清零）；Vulkan flat 12/17 与 Metal 6/12 布局及描述符写入；`GetVolumeView` 携带句柄 |
| C++ | `Lumen/GICacheView.h` | `DDGIVolumeView::relocated_origins`（可缺省） |
| C++ | `RenderPipeline/Modules/GIGatherModule.cpp` | Vulkan binding 19 / Metal buffer(12)；绑定近场 volume 的 relocated origins（INVALID 时绑 dummy）；barrier 列表补该缓冲 |

### 1.2 P1 脚枪：SPGI rays 上限钳制（审计 §7）

`ScreenProbeAverage` 只有 64 lanes、每 lane 读一条；`SCREEN_PROBE_RAYS` override
原只有 ≥8 下限——>64 时会只积分前 64 条却按更大 raysPerProbe 归一化，能量反而
变暗。`StandardRenderPipeline.cpp` 对 env 路径与 config 路径统一钳到 64 并输出
日志；更高值需先实现 lane-strided accumulation + 同步归一化。

### 1.3 调度实测：DDGI_AGE_STATS（审计 §3.3/§9-P2 口径）

`LumenDDGIPass` 更新列表构建后，env `DDGI_AGE_STATS=1` 一次性输出
`update_ratio / age_med / age_P95 / age_max`（age 来自 CPU 侧
`probe_states_.last_update_frame`）。Sponza 实测：`update_ratio=8192/16384`、
age 全为 1——governor 以半预算运行但无饥饿（P95==30 才是 §3.3 担心的饥饿
信号）。台账 source 列（逐 probe 辐射来源）仍需 GPU 状态读回，留待 P1。

### 1.4 测有效覆盖：DDGI_ORIGIN_STATS 读回（审计 §3.1 缺口）

`DDGI_ORIGIN_STATS=1` 时（第 60 帧触发，独立命令缓冲同步读回——先于本帧
render graph 执行，读到的是上一帧稳态数据；per-instance 守卫，近/远各一次），
输出 relocated origins 的分类分布与位移分位数。Sponza 实测：

- 近场 16384 probes：**self=16384（100%）**，displaced（位移>0 的 self）=484
  （2.95%），位移 med 0.6 m / P95 2.1 m / max 2.7 m（=0.95×3 m 上限 ✓）；
- 远场 2048 probes：self=2048（100%），displaced=19，max 7 m（0.95×8 m ✓）；
- **borrowed=0**：深埋 26 邻格借格回退在 Sponza 从未触发——§1.1 的借格半权
  在本场景是零成本的正确性保险（A/B 亦证实：半权前后地面 38.86 vs 37.91，
  运行间方差内）。

### 1.5 归因 A/B：DDGI_LEGACY_CONSUMERS（地面变化的最终归因）

env 门控的编译期变体（Vulkan；build_spv.sh 以 `-DDDGI_LEGACY_CONSUMERS` 编出
`DDGITraceFinalize_legacy` / `DDGIGIGather_legacy` sibling bytecode，测试入口
按 env 加载）：消费端（Finalize hitPos + Gather 可见性锚点）回到旧原格点行为，
缓冲仍照常写。frame 360 同协议采集：

| 指标 | 审计基线（修复前） | 契约修复 | LEGACY_CONSUMERS |
|---|---:|---:|---:|
| 全图均值 | 40.57 | 36.89 | 40.60 |
| 暗像素 <32 | 66.90% | 72.29% | 64.84% |
| 中央地面 | 65.26 | 39.08 | **64.46** |

**结论**：地面变化（65→39）100% 来自坐标契约修复的消费端；结合 §1.5，其
载体是 484 枚真实重定位的贴面探针（位移中位 0.6 m——地板/墙表面探针被抬起）。
旧值 65 包含"hitPos 用原格点重建（差 0.6 m）"的错误采样能量；新路径对重定位
探针几何自洽（trace 起点＝hitPos 重建起点＝可见性锚点＝深度图出处）。39 vs
参考 75 的剩余差距属探针供给/覆盖问题（审计下一阶段），不再与坐标错误混淆。

**未动**：UpdateDepth / UpdateIrradiance（纯八面体空间，不消费世界坐标）；
Dawn（unified pass 单 invocation 内完成 relocate+shade，契约本就一致）；
审计 §5.3-4 的 invalid/classified 状态与 §5.3-5 的 offset 变化历史失效
（scrolling 当前恒 return false，无失效场景；volume scroll 启用时必须补）。

### 1.6 调试可视化修复：probe 球体（审计 §8.3"完善调试网格"）

审计观察到的"大量三角碎片和异常大小形状"，根因有三（均已修）：

1. **全量绘制不可读**：16384+2048 枚球全部绘制（含墙内、任意深度），画面
   糊成碎片墙。修复：切片过滤——默认只画相机高度 ±2.5 m 的一层
   （`SPGI_PROBE_SLICE=0` 关、`SPGI_PROBE_SLICE_HALF` 调半宽）。
2. **逐顶点相机隐藏撕裂**：旧 VS 对 `distance(vWorld,cam)<2` 的顶点逐个塌缩
   到裁剪域外——球心在 2 m 边界的探针部分顶点塌缩、部分保留，产生横跨
   屏幕的拉伸三角形（"异常大小形状"）。修复：顶点流加探针中心属性
   （stride 28→44B），按中心整球判定；隐藏半径经 `cam_pos.w` 下发
   （`SPGI_PROBE_HIDE`，默认 4 m——切片带在相机高度，2 m 挡不住近景球）。
3. **无状态语义**：球体只有稳定色相，读不出覆盖/重定位。修复：新增
   `GetRelocatedOriginsCpu()`（第 60 帧一次性同步读回 relocated origins），
   VB 烘焙 per-probe 状态，FS 按色着色——**绿=本格自由、黄=重定位、
   红=借格、灰=未 trace**，half-lambert 提亮；无状态数据时保持 legacy 色相。

验收（frame 360，锁定机位）：切片带清晰可读——黄色重定位探针聚在墙柱
周边、绿色自由探针沿地面排布；状态直方图 `relocated=126 self=898` 与
DDGI_ORIGIN_STATS 一致。附带发现并修复两处引入过程中的顺序/键值 bug
（状态指针晚于构建赋值、缓存键含切片后顶点数导致每帧重建与越界读）。
Vulkan/Metal 孪生同步（ProbeSphere.metal 同结构改动）。

### 1.7 P0-2：GILadder 台账判定器改为逐像素镜像真实融合式

审计 §8.1：旧判定器 `expected=(medianDDGI+medianSPGI)×0.25/π` 与 shader 的
实际公式差了 mix/加法、实际 albedo、spgi.a 权重、SSGI 项与 gain，`C_indirect_ok`
只证明"非零输出"。

`RenderPipeline/Modules/FusionCompositeModule.cpp::RunGiLadderDump`：

- 台账源 5→7：新增 **ssgi**（半分辨率 RGBA16F）与 **albedo**（GBuffer
  BGRA8_UNorm，4B/px 专路解码——按 8B 半浮点读会得到垃圾）。
- 逐像素期望 = `albedo × (mix(ddgi,spgi,w) + ssgi×conf×mix(.25,.15,w)) × gain/π`，
  其中 `w=clamp(spgi.a,0,.9)`、`conf=clamp(1-clamp(ssgi.a,0,30)/30×0.7,.3,1)`、
  gain 读 `PRIMAL_GI_INDIRECT_GAIN`（与 pass 同源，默认 3）。
- CSV 追加 `ssgi_rgb, albedo_rgb, spgi_a, expected_lum, residual_lum` 列；
  新增 `[GILadderResidual]`（median/P95/max）行。
- 判定 C 档 `C_indirect_ok` → `C_fused_matches_model`，并明示其语义边界：
  合成一致 ≠ 供给正确（探针供给错误时模型会同样一致地复现错误）。

## 2. 验证与证据

- **SPIR-V**（与审计同方法，spirv-dis）：TraceSDF 含 `%robuf Binding 12`、
  Finalize 含 `Binding 17`、Gather 含 `Binding 19`；修复前 Finalize 无 relocation
  函数、也无该绑定。
- **编译**：`build_spv.sh` 全绿（含 pathdbg / SDFCone 变体）；Metal shader 验证
  两个改动文件 OK；两棵 CMake 树 100% 构建（DebugVulkan 首次失败为构建元数据
  落后于 7d784498 的存量问题，reconfigure 解决，与本修复无关）。
- **回归**：Unit 测试两树 4/4；Vulkan Sponza / Metal TestModularPipeline 各
  100s 无 fault / validation ERROR = 0，`[DDGI-TracePath] SPLIT` 正常触发。
- **shader 拷贝门禁**：同步 EngineTest 的 DDGIGIGather.metal 与
  SurfaceCacheIndirectResolve.metal 拷贝后 `check_shader_copies.sh` OK。
- **台账端到端**：`GI_LADDER_DUMP=1` 采集正常；目标带 expected(model)=0.228 vs
  indirect=0.416（residual med/P95/max = 0.170/0.324/0.410），CSV 新列齐全。

## 3. 复现

```bash
cmake -S . -B Darwin/DebugVulkan -DCMAKE_BUILD_TYPE=Debug -DENABLE_VULKAN=ON && cmake --build Darwin/DebugVulkan -j8
ctest --test-dir Darwin/DebugVulkan -R "Unit" --output-on-failure
./Darwin/Debug/TestVulkanSponzaRenderGraph 2>&1 | tee /tmp/regress_vulkan.log   # 100s 后 kill
python3 scripts/sponza_reference.py vulkan --output /tmp/probe-fix --frame 360
GI_LADDER_DUMP=1 python3 scripts/sponza_reference.py vulkan --output /tmp/probe-ladder --frame 400
DDGI_AGE_STATS=1 python3 scripts/sponza_reference.py vulkan --output /tmp/probe-age --frame 60
DDGI_ORIGIN_STATS=1 python3 scripts/sponza_reference.py vulkan --output /tmp/probe-origin --frame 90
DDGI_LEGACY_CONSUMERS=1 python3 scripts/sponza_reference.py vulkan --output /tmp/probe-legacy --frame 360   # 归因 A/B
SPGI_PROBE_SPHERES=1 python3 scripts/sponza_reference.py vulkan --output /tmp/probe-viz --frame 360         # 探针状态切片可视化
./scripts/check_shader_copies.sh
```

## 4. 行为影响（诚实记录，不作"修复即变好"声明）

锁定参考机位、frame 360、与审计同协议采集（1280×720 LANCZOS，Y′ 加权）：

| 指标 | 审计基线（修复前复采） | 契约修复 run1 / run2 | + 借格半权 + P1 解耦 | LEGACY_CONSUMERS |
|---|---:|---:|---:|---:|
| 全图均值 | 40.57 | 36.83 / 36.47 | 36.89 | 40.60 |
| 暗像素 <32 | 66.90% | 72.43% / 72.84% | 72.29% | 64.84% |
| 中央地面 | 65.26 | 38.91 / 37.91 | 39.08 | **64.46** |
| 台账目标带 DDGI 中位 Y | 2.208 | 0.947 | 0.948 | — |

**归因链闭合（§1.4/§1.5）**：
1. 借格半权 A/B 排除借格假说（38.86 vs 37.91，方差内；且 ORIGIN_STATS 证实
   Sponza 借格数为 0）。
2. DDGI_ORIGIN_STATS 定位载体：484/16384 枚真实重定位探针（位移中位 0.6 m，
   即被抬离地板/墙表面的贴面探针）。
3. DDGI_LEGACY_CONSUMERS A/B 收口：消费端回到旧原格点行为 → 地面 64.46 ≈
   基线 65.26。地面变化 100% 来自坐标契约修复本身；SPGI confidence 解耦与
   台账重写不参与（SPGI 只影响自身通道；台账只影响读数）。

物理含义：旧地面值 65 含"hitPos 以原格点重建（对抬起 0.6 m 的探针偏差
0.6 m）"的错误采样能量；新值 39 来自几何自洽的重定位探针。39 vs 参考 75 的
剩余差距属探针供给/覆盖问题（审计下一阶段），不再与坐标错误混淆。

**这正是审计规定的顺序**：修坐标契约与诊断（已完成）→ 测有效 probe/接收点
覆盖（ORIGIN_STATS 迈出第一步，覆盖率细分仍缺）→ 修 fallback/confidence
（ScreenProbeAverage 解耦已落地，效果待评估）→ 局部密度实验 → 标定显示增益。

**调度实测附带结论**（DDGI_AGE_STATS，直接回答审计 §3.3 的悬问）：
Sponza 下 governor 实际以半预算运行（8192/16384），但 age_med=P95=max=1
——半预算下每 probe 仍每两帧更新一次，无 §3.3 担心的饥饿；该开关留作
紧预算场景的回归检查。

## 5. 已知边界与后续

1. ScreenProbeAverage valid/几何命中解耦（§6.3）已落地：fallback 底 0.35 为
   保守占位，来源感知的完整分级（覆盖/可见性/年龄）属 fallback/confidence
   阶段；"合法采样数能量均值一致"的常量场最小场景验收未搭（需天空平面
   测试场景）。借格低频保底分量未降权（防黑格优先）。
2. volume scroll 一旦启用，必须在 shift 时清空 relocated origins 的 w 位
   （审计 §5.3-5），当前滚动函数恒 false，暂无触发路径。
3. 台账 source 列（逐 probe 辐射来源 SC/prev/analytic/sky）需 GPU 状态读回，
   未做；age 已有 DDGI_AGE_STATS，origin/state 已有 DDGI_ORIGIN_STATS。
4. Metal 侧需独立的基线采集（审计只采了 Vulkan）；本轮仅验证功能与稳定。
   DDGI_LEGACY_CONSUMERS 变体仅覆盖 Vulkan bytecode（Metal 运行时源编译
   无此 #ifdef 门）。
5. probe 球体可视化已修（§1.7）：切片 + 中心隐藏 + 状态色落地；剩余可选项
   ——逻辑格位/有效位双显（连线偏移）、Vulkan 侧 irradiance 采样模式未接。
6. 审计 §9 P1 剩余（Gather 平面滤波/几何法线、SPGI fallback 合同统一）、
   P2（局部加密/scheduler 饥饿保障）、P3（增益分离）未动。SPGI ray 上限已
   钳制（§1.2），lane-strided accumulation 仍是完整解。
