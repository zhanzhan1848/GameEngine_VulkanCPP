# Vulkan 黑块与块状光照：审计和修复方案

2026-09-14。目标来自用户最新红框：不仅消除异常暗色，也要消除平滑墙面、拱顶、柱子附近的离散块状边界。当前尚未完成视觉验收。

## 一、修复在哪里，Swift 做什么

实际修复在引擎文件，不在 Swift 运行时：

| 已落地改动 | 实际文件 |
|---|---|
| Vulkan Card capture 转换 clip Y，使 atlas 与 worldToCardUV 一致 | Engine/Graphics/Vulkan/shaders/Nanite/CardCapture.vert:137 |
| Card 镜像不再决定双面法线；按 Card 观察方向定向 | Engine/Graphics/Vulkan/shaders/Nanite/CardCapture.frag:78；EngineTest/shaders/CardCapture.metal:212 |
| SC radiosity 分配完整 atlas，匹配 Resolve 的展开写入 | Engine/Graphics/Lumen/SurfaceCache/SurfaceCachePass.cpp:463 附近 |
| LightEval 按完整 atlas texel 读取 radiosity | Engine/Graphics/Vulkan/shaders/Lumen/SurfaceCacheLightEval.comp:444；对应 Metal 与测试副本 |
| Screen Probe 连续 tent 重建和接收点平面约束 | Engine/Graphics/Vulkan/shaders/Lumen/ScreenProbeGather.comp；对应 Metal |
| DDGI 诊断和数值门禁按实际纹理大小读取 | EngineTest/IntegrationTests/TestVulkanSponzaRenderGraph.cpp:1633、2463 |

Vulkan SPIR-V 已重新编译。此次审计确认 CardCapture.vert.spv、SurfaceCacheLightEval.comp.spv 的源码侧产物与 Darwin/Debug 运行目录副本 SHA-256 一致。不能据此认为已打开进程会热更新；启动时加载的 shader 需要重启才改变。

`scripts/test_card_capture.swift` / `test_probe_gather.swift` / `test_sc_radiosity.swift` 是独立的 Metal GPU 测试驱动。Swift 用来创建 GPU 设备、准备纹理/缓冲区和检查结果，未接入引擎渲染流程。部分测试提取生产 GLSL 表达式并放入 MSL fixture，另一些运行生产 Metal kernel：它们验证局部数学和数据契约，不能证明 Vulkan 的真实绑定、同步和整帧视觉正确。后续验收必须补实际 Vulkan readback。

工作区有其他并行改动；git diff 的全部行数不等于本任务的改动量，不应将这些文件的所有修改都归于本次修复。

## 二、把两个问题分开验收

- **亮度/颜色异常：** 相邻同材质、同一平面上出现没有遮挡依据的黑斑、亮斑或颜色跳变。可能涉及错误命中、错误 Card、无效历史被当成有效黑色等。
- **块状边界：** 光照按 tile、probe cell、Card texel 或 SDF voxel 分段常量/突然跳变。整体乘亮度系数只会把黑块变成灰块；模糊可以减轻边界，却不能纠正错误表面数据。

已证明的引擎缺陷不等于已证明某个红框的唯一成因。以下审计区分这两类结论。

## 三、进一步确认的问题

### A. Radiosity 输出仍是 tile 常量展开（确定的块状来源机制）

证据：SurfaceCacheIndirectTrace.comp:249 在 tile 中心取深度和法线，只追踪这一点；SurfaceCacheIndirectResolve.comp:165 将同一个 indirect 值乘每个 texel 的 albedo 后铺满 tile。LightEval 现正确逐 texel 读取，但没有重建连续的入射光场。

同一平面、同材质的 tile 内是相同光照估计，tile 之间独立取值，天然会出现阶梯。修复分配越界并没有修复这种采样表示。当前默认 tile=8；atlas texel 的世界尺寸随 Card 尺寸/分辨率变化，不能直接称为固定的屏幕 8 像素块。

处理：保存每个有效 tile 的入射辐射度/irradiance（明确单位），按 tile 中心坐标连续重建，再乘接收 texel 的 albedo。邻居必须经过 Card 身份、捕获位置、几何法线/深度验证。相邻纹理格子不一定是相邻物理表面。不能直接恢复旧的固定 3×3 权重。

### B. 跳过的 tile 没有有效性契约（确定的数据流缺陷，红框贡献待 readback）

证据：SurfaceCacheIndirectTrace.comp:245–261 对刷新 phase 不匹配、depth<=0、无 owning card 直接 return；Resolve 仍读取每个 tile 的全部 ray_hits。没有伴随的 tile-valid、last-updated、card-generation 判定。Resolve 的历史有效性只看 frame_index>=2（:163），不代表该 tile 已写过。

后果：首次半速刷新、空洞、Card 重捕获/重用后，未初始化或旧表面的射线可能继续参与合成；即便底层恰好清零，hit_type=0 也会被解释成有效命中，而非无效。

处理：为 tile 建立 valid/generation/lastUpdated；未刷新但同代有效的 tile 才复用历史；失效 tile 显式标无效，不冒充天空/黑色命中。Resolve 在读取 ray_hits 前检查；历史在 Card 变化时重置。不要每帧无条件清除所有历史，破坏分帧刷新。

### C. 多条 SC 查询使用不同且不完整的表面验证（确定）

- SurfaceCacheIndirectResolve.comp:127–146：first worldToCardUV 命中即接受，既无 captured depth 校验，也无法线/朝向校验。
- DDGITraceFinalize.comp:449 附近：深度容差与 Card extent 成比例且为单侧比较；接受第一个满足条件且非黑的 Card，有效黑色又被亮度门槛拒绝。
- ScreenProbeTraceFinalize.comp:194–229：有 captured position 检查，但按最近 texel 的误差选 Card，没有捕获法线/表面朝向约束；误差最小的 Card 随 texel 改变时可能跳选。

这会让同一个世界位置沿不同路径收到不同颜色、不同侧面的光照。只修 Screen Probe 查询，其他路径仍会把错误能量送回缓存。

处理：统一查询契约：输入命中位置、射线方向、可用的命中法线及误差范围；输出 radiance 和独立 validity、Card ID、位置误差。检查捕获有效性、沿射线/垂直射线误差、表面朝向，并使用稳定的候选选择。双线性 tap 限于同 Card/兼容表面，失败回退世界缓存。不要仅靠亮度判断有效，也不要只增加容差。

### D. Cone 遮挡证明被当成精确表面命中（确定，需优先隔离）

证据：SurfaceCacheIndirectTrace.comp:277 的主追踪从带偏移的 ray_origin 出发；:311–316 的 cone 分支却从 probe_pos 检查并构造 hit_pos。距离 t 与构造位置不采用相同原点。更重要的是，cone/min-mip 只说明宽 footprint 内可能有遮挡，代码将其设为普通 hit，Resolve 随后用这个构造点去取 SC 颜色。

处理：区分真实 hit、cone-occluded、escaped、exhausted、invalid。只有有表面定位依据的 hit 可进行 SC 精确查询。cone 遮挡走保守缓存/可见性回退，不生成伪造的精确 hit_pos；所有路径统一 ray origin。用细柱、拱边和斜墙回归，检查漏光与误遮挡。

### E. SDF 最近邻与硬阈值产生阶梯（确定的采样方式，具体贡献待隔离）

SurfaceCacheShadow.comp:74–80 使用最近邻 SDF，后续输出硬 0/1 阴影；SurfaceCacheIndirectTrace.comp:127 也使用最近邻，命中阈值为 max(voxel*1.25,0.30)。世界位置跨 voxel 或 cascade 时会改变命中/可见性。关闭 SC 阴影会大幅改变画面，但不能据此宣布所有黑块都是阴影。

处理：先输出 SDF 距离、voxel/cascade ID、命中类型确认边界关联；基础距离场采用正确 texel-center 的连续采样和合理步进。min-mip 的保守遮挡语义与基础距离场不能混用；插值不应未经验证就用于不安全的大步进。阴影过滤只在几何采样正确后按光源角尺寸/接收 footprint 进行。

另有确定边界漏洞：IndirectTrace 的 sampleSDFNearest 在 uvw==1 时得到 coord==size，应将整数坐标夹至 size-1 或统一使用半开域。Shadow 的对应函数已做 size-1 夹取，不能误列为同一越界。

### F. 上游 probe 过滤仍可能混入不同表面（确定机制，伪影贡献待测）

ScreenProbeSpatialFilter.comp:119–143 只按世界距离过滤 SH，没有法线或接收平面一致性；CPU sigma=2.0。最后 Gather 加几何约束只能限制使用哪些 probe，无法撤销一个 probe 的 SH 已经混入邻近另一面墙的光照。

ScreenProbeDenoise 使用原始非线性 depth 差，当前 radius=1 内部像素。它既不能稳定消除大尺度 tile 块，也可能在远处给深度不连续位置过高权重。

处理：在 SH 空间过滤阶段就加入表面兼容性；最终重建使用一致的世界/线性深度指标。不要把扩大后处理半径作为主修复。

### G. 可配置 tile 的 ray buffer 容量不跟参数走（确定的独立越界）

SurfaceCachePass.cpp:2214 按 atlas_size²×16 字节分配 ray_hits。实际需求是 tilesX×tilesY×raysPerProbe×32 字节（当前 GLSL ProbeRayHit stride 32）。2048 atlas、32 rays：tile=8 恰为64 MiB；代码允许 tile=4，此时需要256 MiB，原分配不足。任意 4–128 整数都被接受，非整除 atlas 的 tile 还会遗留边缘。

处理：按统一 runtime config 算容量，溢出检查，尺寸变化时重建/失效；明确允许的 tile 大小或 ceil dispatch+边界处理。该问题不能解释默认 tile=8 的全部黑块，但必须先修，才能安全开展 tile 尺寸 A/B。

## 四、执行顺序与每阶段验收

### 阶段 1：稳定复现和数据证据

固定相机、曝光、灯光、内部 render scale，保存二进制/关键 shader 哈希和实际尺寸。选上墙、中心柱后墙、拱顶侧墙、狮头背墙、右侧墙五组 ROI。

实际 Vulkan 输出：DDGI、SPGI raw/filtered、Fusion 前线性间接光、SC lighting、SC shadow、tile ID、Card ID、validity/age/generation、SDF hit type；针对少量指定 probe 保存逐射线命中点、选中 Card、深度误差和 radiance。避免用整帧平均亮度代替局部诊断，也避免用改变 Fusion 反馈源的调试画面作为唯一证据。

### 阶段 2：P0 数据正确性

先修 B/G 和 D 的伪精确命中，再统一 C。补首次刷新、stride=1/2/4、空 tile、有效黑面、Card 重用、相机切换的实际 Vulkan 回归。验证缓冲区 sentinel、没有无效样本被 Resolve 消费。历史 slot 的两帧延迟可以是合法设计，但必须有生产完成和代际有效性的证据；不盲改帧序。

### 阶段 3：P1 连续重建

修 A/F；优先实现同表面的 tile 中心连续重建，分离入射光和材质乘法。再处理 E 的 SDF/阴影连续性。tile=4/8/16、内部 render scale=1/2 分别对比：分辨率改变只能改变细节与噪声，不能把大块阴影移动到别的墙面。

### 阶段 4：验收与代价

1. 常量/线性渐变照明的平面：合法 tile 边界应无额外跳变；单独标记几何、材质、真实阴影边界，不能一律抹平。
2. 黑白相邻表面、平行墙、薄柱：有效黑色保留，不串色、不依赖 Card 列表顺序。
3. 用户五组 ROI：固定曝光记录线性亮度剖面与块边界梯度；与各自邻域/参考采样比较，阈值由固定基线定义，失败不临时放宽。
4. 静止长时间、旋转与位移：没有随相机滑动的光照块、历史残留；后端 shader/data layout 一致。
5. 实际 Vulkan GPU readback + GPU fixtures + Engine Unit + 60–120 秒窗口回归；记录 GPU 时间与内存。现有370条 validation日志单独分类，不能把所有错误都称为无关；新增/关联描述符和同步错误必须清除。

每阶段独立保存前后图、数据和测试结果，再进行下一阶段。源代码修复、Swift 局部测试通过、Vulkan 图像通过是三个不同的完成条件。

## 五、当前状态

本轮追加的是源码审计和修复方案。已落地的引擎修复继续保留；不能说最新红框已经修好。当前资料足以按上述顺序执行，无需再通过提高整体 GI 强度来试错。

### 2026-09-14 进展（阶段 1 完成 + 阶段 2 部分完成）

先决：graphics 模块的既有未提交改动已按功能拆成 10 个 commit（13cebe6c..804c45e2）。

**阶段 1（稳定复现与数据证据）已建立**，证据与复现命令存于
`test-output/block-artifacts/stage1-before/`（README 含 ROI 线性数据、
validation VUID 直方图、dump1/2/4/5 产物）：

- 参考机位基线 360 帧；五组 ROI 的 DDGI gather 线性统计（ROI 内 min/max
  差 4-27×，块状边界的量化证据）；
- 发现并修正两个诊断假警报：dump5 top-24 排序越界读（"卡片 #1 lum=1e26"
  系测试 bug，atlas 实际干净）；dump2 的 Vulkan 路径统计不可信（生产 spv
  不含 dbgPath，需要 pathdbg spv 装载开关——遗留）。

**阶段 2 P0 已落地两项**（证据存 `test-output/block-artifacts/stage2-bg*/`）：

- **G（ray buffer 容量）**：ray_hits 按 tilesX×tilesY×rays×32B 分配、
  pow2 tile 校验、溢出上限（超限清零 dispatch 而非越界写）。tile=4
  （256MiB，旧代码必越界）120 帧回归无新增 validation。
- **B（tile 有效性契约）**：resolve 仅消费本帧 refresh phase 内 tile 的
  ray_hits；相位外携带有效历史或显式零（indirect_out.A=validity）；
  hit_type≥3 = INVALID 哨兵；tile 网格/卡片集变更清零。LightEval 按 A 门控
  （Vulkan+Metal 同步）。区域门 sunlit/int 1.19→1.37，R_arcade 23.0→34.3。
- 顺带修复 §三.E 记录的 sampleSDFNearest uvw==1 越界（夹至 size-1）。

**下一步**：阶段 2 剩余 = D（cone 伪精确命中隔离）→ C（SC 查询契约统一）；
之后进入阶段 3（A/F 连续重建 → E）。370 条 validation 的分类清除仍留在阶段 4。
