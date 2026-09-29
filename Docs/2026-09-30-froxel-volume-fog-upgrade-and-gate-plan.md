# Froxel 体积雾升级与集成门禁计划（light march 自阴影 / SPGI 环境项 / 时域重投影 / TestVulkanFroxelRenderGraph）

> 日期：2026-09-30
> 状态：✅ **已完工**——Phase 0-3 + Metal 移植全部落地并过门禁（TestVulkanFroxelRenderGraph RESULT PASS 5/5，exit 0），完工记录见 `Docs/completed/2026-09-30-froxel-volume-fog-upgrade-and-gate-completion.md`（阈值回填、性能实测、排障记录、截图档案）。WP4（抖动累积）按计划未排期。
> 范围：`Engine/Graphics/Volume/`（FroxelFogPass 三 pass 管线）+ `EngineTest/IntegrationTests/`（新增独立门禁测试）
> 关联：`Docs/ARCHIVE.md`（登记）、`Docs/2026-09-16-volume-gi-repair-audit`（V-Test-A/V7/P2-B 契约，本计划在其上叠加）、`Docs/RHI/GraphicsAPI_Parity_Matrix.md`、AGENTS.md 构建纪律

---

## 1. 背景与调研结论（摘要）

### 1.1 技术路线定位

Disney 系（Hyperion / Deep Scattering）的体积渲染是离线路径追踪路线：参与介质辐射传输方程用 null-collision 采样 + 逐 shadow ray 求解，多重散射靠路径反弹收敛；Moana 云的 Deep Scattering（Kallweit et al., TOG 2017）用神经网络预测 in-scattered radiance 替代暴力追踪。**这些都不能实时**，但它们与实时方案共享同一套数学内核：

- 辐射传输方程：`dL = -σt·L·ds + σa·Le + σs·∫p(ω,ω')·L·dω'`，σs/σa/σt 与相位函数（双叶 Henyey-Greenstein，本引擎 `phase_g` 已实现单叶 HG）
- Beer-Lambert 透射 `T = exp(-∫σt ds)` 是一切自阴影计算的核心
- 相机上屏 = emission-absorption 前向积分 + premultiplied-over 合成

### 1.2 实时等价物全部是光栅操作（无 RT 依赖）

本计划涉及的全部技术点**不需要任何光线追踪硬件**：

| 电影线做法（需路径追踪） | 实时无 RT 替代 | 成本性质 |
|---|---|---|
| shadow ray 求可见性 | shadow map 投影采样（光栅化一次，之后全是纹理查找） | 一次矩阵乘 + 一次 2D 采样 |
| 雾对雾自阴影（透射积分） | light march：沿光方向步进 4~8 步，逐步采 shadow map / SDF，累乘 Beer 衰减 | 循环纹理采样，非 RT |
| 多重散射收敛 | 单次散射 + 环境项能量补偿（本引擎已有 `ambient_intensity`） | 免费 |
| GI 照亮雾 | 逐 froxel 采 SPGI 探针体积（SPGI 本身就是屏幕空间/探针方案） | 一次 3D 采样 |

标准出处：Wronski "Volumetric Fog"（SIGGRAPH 2014，froxel 方案原始形态，UE 沿用至今）、Hillaire "A Physical Approach to Volumetric Fog"（SIGGRAPH 2020，ESM ≈ 常数消光 Beer 律的解析自阴影）、Frostbite "Physically Based and Unified Volumetric Rendering"（SIGGRAPH 2015）。

---

## 2. 引擎现状盘点（2026-09-30 实测）

### 2.1 FroxelFogPass 管线（已落地，Vulkan 先行）

代码：`Engine/Graphics/Volume/FroxelFogPass.{h,cpp}`、`FroxelTypes.h`、shader 在 `Engine/Graphics/Vulkan/shaders/Volume/`（GLSL 源）与 `Engine/Graphics/Metal/shaders/Volume/`（MSL 镜像）。

| Pass | Shader | 现状 |
|---|---|---|
| 密度注入 | `FroxelDensityInject.comp` | 噪声 + 高度雾 + GlobalSDF 三级联 carve（`FroxelDensityInject.comp:63`）；常量介质 debug 模式（`density_uniform_debug`，V-Test-A 基准路径） |
| 光照积分 | `FroxelLightIntegrate.comp` | 160×90 线程 × 64 层循环；单方向光 + HG 相位（`:189-197`）+ **每 slice 一次** `froxelShadowQuery`（`:178`）；环境项为**硬编码天空色** `vec3(0.4,0.45,0.6)*ambient`（`:201`）；front-to-back Beer 递推，逐 froxel 写 (scatter_rgb, transmittance) |
| Resolve | `FroxelResolve.comp` | 按像素深度采体积，输出半分辨率 RGBA16F 屏幕散射图（RGB=scatter, A=transmittance） |

关键契约：

- `FroxelParams`（`FroxelTypes.h`）GPU 常量缓冲 **656 字节，`static_assert(sizeof)==656`**，CPU/GLSL/MSL 三方逐字节镜像（MSL 注释明示"布局逐字节一致"）——**任何布局改动必须三方同步**
- 阴影模式 `VolumeParams3.w = shadow_mode`：`0`=无阴影输入恒 lit（明确定义的 fallback）、`1`=4 级联 light-space VSM moments（RG32F z,z²）、`2`=PCSS 链 light-space 深度图逐 froxel 比较（**Vulkan only，Metal MSL 未支持，运行时门控回退恒 lit**，见 `FroxelFogPass.h:22-34` 注释）
- 阴影数据源自 `ShadowMapModule`（`ShadowMapOutputs`：`shadow_moments_rg[4]` / `shadow_depth_rg[4]` / `shadow_matrix[4]`，4 级联 12/30/90/270 半宽）
- 坐标契约已修复（P2-B/volume-gi-repair-audit）：froxel 逐世界坐标投影到 light clip space，不再采样屏幕空间可见性图；相机 near/far 与雾体 near/far 已分离（`FrustumParams2`，2026-09-16）
- 纹理 triple-buffer（`froxel_density_[3]` / `froxel_scatter_[3]`）——但**每帧全量重算，历史未被利用**

### 2.2 验证能力现状（缺口）

- `TestVulkanSponzaRenderGraph` / `TestVulkanDLSSRenderGraph`：`FROXEL=1` env 开启 pass（默认关，`TestVulkanSponzaRenderGraph.cpp:576`），A/B 旋钮齐全：`FROXEL_UNIFORM` / `FROXEL_PHASE_G` / `FROXEL_NO_SHADOW=1`（恒 lit 对照，`:2546`）/ `FROXEL_DUMP=1/2`（frame 350 手动读回：半分辨率 scatter 分带统计 + ×60 提亮 PPM；`=2` 额外读回 3D 密度按 z 切片打印，`:2590` 起）
- `TestModularPipeline`（Metal）：显式 `SetPassEnabled(RenderPassID::FroxelFog, false)`（`:588`），Metal 集成零覆盖
- **没有自动断言**：`FROXEL_DUMP` 只打印 stderr 供人判读；Sponza/DLSS 测试对 FG/NR 有 `_Exit 0/1` 自动门禁，froxel 没有
- 不在 ctest 体系；只能窗口式跑（60-120s kill）
- `EnvEnabled` 语义已修复（`"0"/"false"/"off"` 判关，`TestVulkanSponzaRenderGraph.cpp:153`；早期 `FROXEL=0` 语义混乱已留注释）

### 2.3 与标准做法（Wronski/UE）的差距表

| 能力 | 标准 froxel 方案 | 本引擎 | 差距 |
|---|---|---|---|
| 几何挡光（god rays） | froxel 投影采 shadow map | ✅ mode 1/2 已实现 | 无 |
| 雾对雾自阴影 | light march 4~8 步 Beer | ❌ 每 slice 仅一次阴影查询 | **P0 缺口（WP1）** |
| 环境项 | 天空/环境光/GI 喂 in-scattering | ❌ 硬编码 `vec3(0.4,0.45,0.6)` | **P1 缺口（WP2）** |
| 体积时间重投影 | 世界位置反投影 + 邻域钳制 | ❌ 每帧全量重算 | **P1 缺口（WP3）** |
| 抖动采样 + 时域累积 | 可选进阶 | ❌ | P2（WP4，依赖 WP3） |
| 多重散射近似 | ambient/能量补偿 | ✅ ambient 项 | 无 |
| 自动门禁 | —— | ❌ 手动读回无断言 | **本计划 Phase 0 补齐** |

---

## 3. 全程约束（不变量）

1. **无 RT 硬件**：所有方案必须是 raster/compute + 纹理采样，禁止任何依赖 ray tracing 的设计。
2. **Apple Silicon 读预算**（沿用 GI 计划硬约束）：≤32 次 device 读/线程、texture3D ~4 次/线程（约束以现行管线实测为准，超标需拆 dispatch）。WP3 的 27 读邻域钳制必须做成独立 pass（见 §4.3）。
3. **FPS ≥ 30 硬底线**：每个 WP 的效果增量必须带性能测量（§5 预算表）与回滚 env。
4. **FroxelParams 三方镜像**：CB 布局改动必须同步 GLSL / MSL / `static_assert(sizeof)`，即便 Metal 暂不启用新功能（布局不一致 = MSL 端未定义行为）。
5. **Vulkan 先行，Metal 明确不跟**：本计划所有新功能在 Metal 端保持现状（恒 lit / 无 march），与 shadow_mode 2 的现行门控一致；门禁测试 Vulkan-only。
6. **仓库构建坑**（AGENTS.md + 既有教训）：
   - spv 大小写路径：源在小写 `shaders/`，产物 `.spv` 经 `build_spv.sh` 生成，大小写混用会静默丢文件；测试目标有 `VulkanShaderFreshnessGate` 把关
   - 改 `Engine/Graphics` 源码后编辑器路径必须重链 EngineDLL（黑屏教训）
   - Metal/Vulkan 两棵构建树交叉构建会混入对方对象集：切树后先 `rm Darwin/Debug/libEngine.a`
   - HEAD 存在不可独立编译的中间态：分阶段验证以**工作区全量构建**为准，不假设部分提交可独立编译

---

## 4. 工作包

### WP1 — light march 体积自阴影（P0，画质主缺口）

**现状**：`FroxelLightIntegrate.comp:178` 每 froxel slice 只做一次 `froxelShadowQuery`——雾能被几何挡光（god rays），但雾自身不能给雾投影（山影/柱影打在雾体上的"体积阴影的自阴影"分量缺失，暗部雾均匀发亮）。

**方案**：阴影查询升级为沿 `LightDirection` 的 N 步 march（默认 N=4）：

```
每 slice（世界坐标 x，视线段透射 T）：
  vis = 1.0
  for s in 0..N-1:
    p_s   = x + LightDir * (s + jitter0) * stepLen      // stepLen = 雾体深度/N 起步，标定调
    d_s   = SampleFroxelDensity(p_s)                     // 沿路密度从 froxel_density 3D 采样
    vis  *= exp(-d_s * extinction_scale * stepLen)       // Beer 累乘
    vis  *= ShadowQuery(p_s)                             // mode1 VSM moments / mode2 深度比较，复用现查询
  shadow = vis
```

- 每步成本 = 一次 3D 密度采样 + 一次 shadow map 投影查询（2D），N=4 时积分 pass shadow tap 从 92 万 → ~370 万/帧，估算增量 +0.3~0.5ms（§5）
- **GlobalSDF 变体（级联外大尺度遮挡，可选项）**：级联范围外用 SDF march 补充。注意每步需 8~16 次 3D 采样 ×3 级联选择，成本约为 shadow map 变体 10 倍——若引入，减到 2~4 步并考虑隔 slice 计算/独立低频 pass。先不做，作为 G5 通过后的增强项
- **修改点**：`FroxelLightIntegrate.comp`（march 循环）、`FroxelTypes.h`（`FroxelParams` 追加 `march_steps/step_len` 字段 → 新 `math::v4` 对齐 656→672 字节，同步 MSL 镜像与 `static_assert`）、`FroxelFogPass.cpp`（CB 填充）、`RenderSettings`/测试 env（`FROXEL_MARCH=<N>`）
- **A/B 与回滚**：`FROXEL_MARCH=0` 完全回退现行单查询路径（默认先 0，G5 基线跑完再翻默认）
- **验收**：§6 门禁 G5 + 视觉验收（Sponza 拱廊 god rays 出现雾内投影层次；`FROXEL_NO_SHADOW` 对照恒亮）

### WP2 — SPGI 环境项接线（P1，雾被 GI 照亮）

**现状**：环境项硬编码 `vec3(0.4,0.45,0.6)*ambient`（`FroxelLightIntegrate.comp:201`）——雾色与场景光照完全脱钩，夜室内雾不发暗、灯下雾不吃反弹。

**方案**：`FroxelInputs` 增加 SPGI 辐照体积句柄（以 SPGI/Fusion 对外暴露的世界空间探针/辐照资源为准，接线时确认渲染图依赖顺序——雾积分必须排在 SPGI 收敛输出之后）；integrate pass 逐 slice 采一次世界空间辐照替换硬编码项：

```
ambientTerm = SampleSPGIIrradiance(worldPos) * ambient_intensity   // fallback: 现硬编码色
```

- 成本：+92 万次 3D 采样/帧，估算 +0.05~0.1ms
- **修改点**：`FroxelFogPass.h`（`FroxelInputs`）、`FroxelLightIntegrate.comp`（采样分支）、管线侧 AddPass 接线（`StandardRenderPipeline` / `ForwardSceneRenderer` 中 FroxelFog 的调用点）、env `FROXEL_SPGI_AMBIENT=0` 回退
- **验收**：视觉 A/B（Sponza 中庭开/关对比：雾色跟随天空与灯光反弹；GI_ALL_OFF 时回退项无感）+ SPGI 未初始化/句柄 INVALID 时确定性行为（走 fallback，不崩）
- **风险**：SPGI 数据的渲染图时序（若 SPGI 输出晚于雾积分，需调 pass 序或用上一帧辐照——体积雾低频，上一帧辐照可接受，与 WP3 的历史思路一致）

### WP3 — 体积时间重投影（P1，买质量预算 + 抗噪）

**现状**：triple buffer 只做帧轮转，每帧全量重算；密度噪声动画逐帧重采样，低显卡顿/抖动时散射场闪烁。

**方案**：新增第 4 个 pass `reproject_clamp`（排在 integrate 之后、resolve 之前）：

- `FroxelParams` 追加 prev 帧 `InvViewProj`（+64B，布局同步三方）
- 每 froxel 一线程（92 万线程）：世界坐标 → 投影到上一帧 froxel 网格 → 采上一帧 `froxel_scatter` → **3×3×3 邻域 min/max 钳制**（27 次读 < 32 读/线程约束 ✓，因此必须独立 pass，不能并进 integrate 的 64 层循环线程）→ 混合 `new = mix(current, history_clamped, 0.35)`（雾低频，混合系数可到 0.3~0.5）
- 视轴快速平移时 froxel 深度重映射是主要 ghost 源 → 钳制兜底；不引入 confidence 通道（第一期）
- **修改点**：新 `FroxelReproject.comp` + `FroxelFogPass.cpp`（第 4 管线/描述符集）、`FroxelTypes.h`（prev camera 字段）、env `FROXEL_TEMPORAL=0` 回退
- **验收**：视觉（快速旋转/平移无拖尾鬼影、密度噪声闪烁显著降低）+ 可选断言 G6（连续两帧散射场差分方差下降阈值）
- **注意**：WP4（抖动累积）依赖本 WP 的重投影基础设施

### WP4 — 抖动 light march + 时域累积（P2，可选，依赖 WP1+WP3）

单帧 march 起点用 IGN/blue-noise 按 (slice index, frame index) 抖动，经 WP3 重投影在时间上累积，等效步数 ×4~8 而单帧只走 1~2 步。仅当 WP1 的 N=4 诚实版不满足软阴影观感、且 WP3 稳定时启动。默认不排期。

---

## 5. 性能预算（160×90×64 = 92 万 froxel，估算量级，落地时实测校准）

| 项 | 估算增量 | 依据 |
|---|---|---|
| 现行三 pass 基线 | ~0.5-1ms | inject 92 万线程轻量；integrate 1.44 万线程×64 层+1 shadow tap；resolve 半分辨率 |
| WP1（march N=4） | +0.3~0.5ms | shadow tap ×4 ≈ 370 万次 2D 采样 + 密度 3D 采样 |
| WP1 SDF 变体（若引入） | +3~5ms 级 | 每步 8~16×3D 采样×级联——**必须减步/隔 slice/低频 pass，默认不引入** |
| WP2 | +0.05~0.1ms | +92 万次 3D 采样 |
| WP3 | +0.2~0.4ms | 独立 pass，92 万线程×27 读（<32 约束 ✓） |
| 合计目标 | 峰值 <2.5ms | FPS≥30 硬底线下有充足余量；每 WP 落地时在 DebugVulkan 实测帧时间入档 |

---

## 6. 集成门禁：TestVulkanFroxelRenderGraph（照 DLSS 模式）

### 6.1 为什么独立成测试

- froxel 现在寄生在 Sponza/DLSS 测试的 env 门后，无自动断言；本计划四个 WP 都改 `Engine/Graphics/Volume` 核心契约，**必须先有基线门禁再动代码**（"改动前先有基线"）
- 独立可执行避免污染 Sponza/DLSS 测试语义（CMake 注释明示 DLSS 与 Sponza 必须完全独立，同理由适用于 froxel）

### 6.2 结构（完全照抄 DLSS 门禁模式）

`TestVulkanDLSSRenderGraph` 的模式：复制 Sponza 脚手架 + 独立 cpp + **Initialize 内 setenv 自带配置**（不依赖外部 env，`TestVulkanDLSSRenderGraph.cpp:218` 同款）+ 到 gate frame 执行进程内断言 + `[XxxGate] PASS/FAIL <item>` 逐项打印 + `RESULT PASS/FAIL` + `std::_Exit(ok ? 0 : 1)`（静态析构安全，`:1514` 注释的理由同样适用——先显式 Shutdown 再 _Exit）。

**CMake 注册**（`EngineTest/IntegrationTests/CMakeLists.txt`，紧随 DLSS 块之后，模板照抄 DLSS）：

```cmake
# 10c. TestVulkanFroxelRenderGraph：Froxel 体积雾集成门禁（G1-G5，_Exit 0/1）。
# 与 Sponza/DLSS 完全独立——本测试 Initialize 内 setenv(FROXEL=1...)，
# 运行：Darwin/DebugVulkan/TestVulkanFroxelRenderGraph; echo $?
add_executable(TestVulkanFroxelRenderGraph
    "Main.cpp"
    "TestVulkanFroxelRenderGraph.cpp"
    "MacKeyboard.mm"
    ${COMMON_HEADERS}
    "${CMAKE_CURRENT_SOURCE_DIR}/../../Engine/Input/InputMac.mm"
)
setup_test_target(TestVulkanFroxelRenderGraph)
target_compile_definitions(TestVulkanFroxelRenderGraph PRIVATE "TEST_VULKAN_FROXEL_RENDERGRAPH=1" "TEST_RENDERER=0" "ENABLE_VULKAN=1")
add_dependencies(TestVulkanFroxelRenderGraph VulkanShaderFreshnessGate)
add_custom_command(TARGET TestVulkanFroxelRenderGraph POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/Engine/Graphics/Vulkan/shaders
    $<TARGET_FILE_DIR:TestVulkanFroxelRenderGraph>/Engine/Graphics/Vulkan/shaders
)
```

**门禁 env**（沿用 DLSS 命名惯例）：

| env | 默认 | 语义 |
|---|---|---|
| `FROXEL_GATE` | 开 | 门禁开关；`=0` 退回窗口长跑（观景模式） |
| `FROXEL_GATE_FRAME` | 300 | 断言执行帧 |
| `FROXEL_GATE_AB` | 开 | G5 双相位开关（WP1 后生效，见 §6.4） |

### 6.3 场景策略

- **首选**：程序化最小场景（地面 + 3 根遮挡柱 + 背墙），常量介质 `FROXEL_UNIFORM` 基准 + 遮挡几何清零——断言区域世界坐标解析可知，基线不受 Sponza 资产波动影响
- **Phase 0 首个验证点**：GlobalSDF/管线模块对小场景有无尺度隐含假设。**退路**：直接复用 Sponza 场景跑门禁，断言区域改用已知遮挡位置（如柱廊）——代价是基线与资产耦合，需在文档记录基线帧
- 场景需含 shadow pass + GlobalSDF（FroxelFog 的上游依赖），Sponza 脚手架现成提供

### 6.4 门禁项定义

读回方式复用 `FROXEL_DUMP=2` 已验证的 staging copy 路径（3D `CopyTextureToBuffer` + half-float 解码，`TestVulkanSponzaRenderGraph.cpp:2590` 起）——实施时抽成公共函数供测试与 dump 共用。**阈值均为占位，Phase 0 实测标定后回填本表**。

| # | 断言 | 读回对象 | 判据（占位） | Phase |
|---|---|---|---|---|
| G1 | 积分健康 | 3D `froxel_scatter`（RGBA16F） | 开阔区 (x,y) 列 transmittance 沿 z 单调不增（容差 1e-3）；RGB 无 NaN/Inf；远端 scatter > 0 | 0 |
| G2 | 常量介质均匀性 | 3D `froxel_density`（R16F） | `FROXEL_UNIFORM` 下几何外非零占比 ≥ 99% | 0 |
| G3 | 体积阴影生效（occluder/god rays） | 3D `froxel_scatter` | 柱后方与柱侧开阔区同深度 slice scatter 均值比 ≤ 0.7；`FROXEL_NO_SHADOW` 内部对照跑比值 ≈ 1.0 | 0 |
| G4 | SDF carve | 3D `froxel_density` | 柱体内部 froxel 密度 < `density_threshold` | 0 |
| G5 | light march 自阴影贡献（WP1 后） | 3D `froxel_scatter` 双相位 | `FROXEL_GATE_AB`：相位 A（march=0）与相位 B（march=4）各跑到 gate frame 读回，柱影区对比度 `（lit区-影区）/lit区` 满足 `B > A + 阈值` | 1 |
| G6 | 时域稳定性（WP3 后，可选） | 3D `froxel_scatter` 连续帧 | 相邻帧差分能量下降 ≥ 阈值 | 3 |

G5 双相位在**同一进程内**完成：gate frame 前后切换 `renderSettings.froxel` 的 march 参数再跑一个相位窗口（参照 GI_HALFLIFE 门在进程内切换配置的先例，`TestVulkanDLSSRenderGraph.cpp:3168`），切换后需若干帧让散射场收敛再读回。

### 6.5 输出与运行

```
[FroxelGate] PASS G1 integrate-health transmittance-monotonic
[FroxelGate] PASS G2 uniform-medium coverage=99.7%
[FroxelGate] FAIL G3 pillar-shadow-contrast ratio=0.82 want<=0.70
[FroxelGate] RESULT FAIL
$ echo $?   # 1
```

- 运行：`cmake --build Darwin/DebugVulkan --target TestVulkanFroxelRenderGraph && Darwin/DebugVulkan/TestVulkanFroxelRenderGraph; echo $?`
- **不注册 ctest**（窗口式，同 DLSS/Sponza 惯例）；后续若 ci_local.sh 恢复 Vulkan 作业可挂接
- Metal/Dawn 不覆盖（与 §3.5 一致），本节即记录该边界

---

## 7. 实施顺序与 DoD

| Phase | 内容 | DoD | 依赖 |
|---|---|---|---|
| 0 | TestVulkanFroxelRenderGraph 落地（G1-G4） | 门禁跑通：现状代码 G1/G2/G3/G4 全 PASS（若 G3 不过说明现行 mode 2 在测试场景有 bug——那本身就是有价值的发现）；阈值标定值回填 §6.4；基线帧时间实测入 §5 | 无（纯测试） |
| 1 | WP1 light march | G5 PASS；`FROXEL_MARCH` 默认翻 4 前完成 Sponza 视觉 A/B 与帧时间实测；march=0 回归 G1-G4 不破 | Phase 0 |
| 2 | WP2 SPGI ambient | 视觉 A/B 入档（Sponza 中庭开/关）；fallback 行为验证（无 SPGI 时 G1-G5 不破）；帧时间实测 | Phase 0 |
| 3 | WP3 时间重投影 | 视觉验收（快速运动无鬼影/闪烁下降）；G1-G5 回归不破（重投影关闭路径）；可选 G6 | Phase 1（顺序建议，非硬依赖） |
| 4 | WP4 抖动累积（可选） | 视观感与预算另行立项 | WP1+WP3 |

每 Phase 验证命令（工作区全量构建，见 §3.6 构建坑）：

```bash
cmake --build Darwin/DebugVulkan -j8 --target TestVulkanFroxelRenderGraph TestVulkanSponzaRenderGraph
Darwin/DebugVulkan/TestVulkanFroxelRenderGraph; echo $?
Darwin/DebugVulkan/TestVulkanSponzaRenderGraph 2>&1 | tee /tmp/regress_vulkan.log   # 无 fault/异常退出
```

## 8. 风险清单

1. 程序化小场景对 GlobalSDF/管线的尺度假设未知 → §6.3 退路（Sponza 场景）
2. `FroxelParams` 布局改动漏同步 MSL 镜像 → static_assert + 代码评审 checklist 双保险（§3.4）
3. spv 新鲜度/大小写坑 → `VulkanShaderFreshnessGate` 依赖 + build_spv.sh（§3.6）
4. G5 双相位的相位间收敛不充分导致假阳/假阴 → 切换后固定收敛帧数 + 阈值留裕量，Phase 1 标定
5. WP2 的 SPGI 渲染图时序冲突 → 退路上帧辐照（§4 WP2 风险段）
6. 邻域钳制误平滑真实动画（密度噪声被历史拖平）→ 混合系数标定 + `FROXEL_TEMPORAL=0` 快速对照

## 9. 参考

**论文/课程**：
- Kallweit et al., [Deep Scattering: Rendering Atmospheric Clouds with Radiance-Predicting Neural Networks](https://la.disneyresearch.com/publication/deep-scattering)（TOG 2017，Moana 云）
- Huang & Burley et al., [Unbiased Emission and Scattering Importance Sampling for Heterogeneous Volumes](https://media.disneyanimation.com/technology/publications/2021/unbiased-emission-and-scattering-importance-sampling-for-heterogeneous-volumes.pdf)（Hyperion 体积采样）
- Novák, Georgiev, Hanika, Jarosz, Kulla, [Monte Carlo Methods for Volumetric Light Transport Simulation](https://la.disneyresearch.com/wp-content/uploads/Monte-Carlo-Methods-for-Volumetric-Light-Transport-Simulation-Paper.pdf)（null-collision 综述）
- Burley, [Extending the Disney BRDF to a BSDF with Integrated Subsurface Scattering](https://www.scribd.com/file/26422232/pbs2015-course)（SIGGRAPH 2015 course，双叶 HG）
- Wronski, [Volumetric Fog / Physically Based and Unified Volumetric Rendering in Frostbite](https://www.slideshare.net/slideshow/physically-based-and-unified-volumetric-rendering-in-frostbite/51840934)（SIGGRAPH 2014/2015，froxel 原始方案）
- Hillaire, [A Physical Approach to Volumetric Fog](https://blog.selfshadow.com/publications/s2020-shading-course/hillaire/s2020_pbs_hillaire_slides.pdf)（SIGGRAPH 2020，ESM≈Beer 解析自阴影）

**引擎内代码**：
- `Engine/Graphics/Volume/FroxelFogPass.{h,cpp}`、`FroxelTypes.h`（`FroxelParams` 656B 契约、shadow_mode）
- `Engine/Graphics/Vulkan/shaders/Volume/Froxel{DensityInject,LightIntegrate,Resolve}.comp`（march/ambient 修改点）
- `Engine/Graphics/RenderPipeline/Modules/ShadowMapModule.h`（阴影数据源契约）
- `EngineTest/IntegrationTests/TestVulkanDLSSRenderGraph.cpp`（门禁模式范本：`DLSS_GATE`/`GI_VERIFY`/`GI_HALFLIFE` 三族 `_Exit 0/1` 先例）
- `EngineTest/IntegrationTests/TestVulkanSponzaRenderGraph.cpp:576,2546,2590`（FROXEL env 门 + FROXEL_DUMP 读回代码路径）
- `EngineTest/IntegrationTests/CMakeLists.txt:310-366`（测试目标注册模板）
