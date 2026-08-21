# Virtual Texture 系统设计（完整版）

- 日期：2026-08-21
- 状态：设计稿（待评审）
- 范围：SVT（磁盘流送虚拟纹理）+ RVT（运行时生成虚拟纹理）+ 完整工具链
- 关联：`Docs/2026-08-19-rhi-vulkan-metal-parity-plan.md`、`openspec/changes/nanite-lumen-v71-architecture/`

---

## 1. 目标与范围

### 1.1 目标

按 UE 级完整度实现软件页表路线的 Virtual Texture 系统：

1. **SVT**：大纹理按 tile 从磁盘按需流送，显存占用与纹理总大小解耦，与"屏幕可见页数"挂钩；
2. **RVT**：GPU 运行时生成 tile（着色缓存），服务 PCG/SurfaceNets 地形混合、decal 类工作流；
3. **Layer Stack**：同一 UV 的多层（albedo/normal/ORM）共享一次页表查找；
4. **混合尺寸**：任意分辨率纹理统一进一个虚拟地址空间（UE UDIM 等价能力），解掉 Nanite bindless 数组的单尺寸限制；
5. **混合流送**：GPU feedback 被动流送 + CPU 距离/速度主动预取（UE 5.6 方案）；
6. **工具链**：离线烘焙、调试视图、统计面板、编辑器支持。

### 1.2 非目标（本期不做，预留架构）

- 硬件 sparse 路线（Vulkan sparse binding / D3D12 tiled resource；Metal 无等价物，且与本引擎三后端 + MoltenVK 现状冲突）；
- Sparse Volume Textures（3D 体数据虚拟化，SDF/Fluid 后续复用本架构时再做）；
- Virtual Shadow Maps（同架构的屏幕空间变体，后续独立立项）。

### 1.3 成功标准

| 指标 | 现状 | 目标 |
|---|---|---|
| Sponza（4K 资产、全材质 VT 化）显存 | 1024² 全量 BCn ≈ 164MB，只增不减 | 物理池总量 ≤ 128MB 可配置，LRU 稳定 |
| 首帧可见 | 阻塞加载全部纹理 | root 页（平均色）即时呈现，可见 tile 2 帧内开始渐进 |
| 纹理分辨率上限 | 离线支持 8K，运行时不可用 | 8K 资产可用且显存不随之增长 |
| 纹理尺寸混排 | Texture2DArray 跳过不匹配尺寸 | 任意尺寸/混合分辨率共存 |
| 淘汰 | 无 | LRU + pin 最低 mip + 空间回收 |

### 1.4 设计原则

- **核心层只提供能力**（CLAUDE.md 原则）：VT 是 `Engine/Graphics` 的能力模块，通过稳定接口暴露给渲染管线、编辑器、脚本层；
- **一次实现，三后端通用**：全部逻辑在 RHI 之上，RHI 仅补齐第 7 节能力清单；
- **抄 UE 的成熟参数与机制**，不自行发明：tile 128、border 4、feedback 编码、页表 layer 打包、LRU 二叉堆、向低 mip 扩散更新、5.6 混合流送、5.7 空间回收。

---

## 2. 背景结论（调研摘要）

- 路线决策：**软件页表**（UE 全平台同款）。物理页池 = 普通采样纹理，页表 = 整数格式纹理，feedback = 片元着色器写 SSBO + 异步回读，tile 上传 = staging 局部拷贝。不需要任何特殊 RHI 特性，Metal/MoltenVK 全功能可用。
- UE 源码架构（`FVirtualTextureSystem` / `FVirtualTextureSpace` / 物理页池 / Producer-Finalizer）自 4.23 至 5.8 未重写，证明该架构的长期稳定性；UE5 的演进（5.2 UAV aliasing 上传、5.6 CPU 距离流送 + 平均色兜底、5.7 空间回收）作为已知弱点修复清单直接纳入本设计。
- 本引擎现状：纹理子系统为"全量驻留、无 mip 采样（legacy 路径 maxLod=0）、无淘汰"；已有两套可复用的分页基础设施——`NaniteStreamingManager`（256KB 页 / 256MB 池 / LRU / prefetch，几何流送）与 Lumen Surface Cache（2048² 页分配 atlas + 三重缓冲）。

---

## 3. 总体架构

```
┌─────────────────────────────────────────────────────────────────┐
│ 离线（ContentTools）                                              │
│  TextureImporter（已有：mip + BCn）                               │
│  + VTBaker（新）：tile 切分 / 页索引 / 双格式(BCn+ASTC) / root 页  │
│  → .vt 资产（TEXR-v2 容器）                                       │
└──────────────────────────┬──────────────────────────────────────┘
                           │ 异步 IO
┌──────────────────────────▼──────────────────────────────────────┐
│ Engine/Graphics/VirtualTexture（新模块，RHI 之上）                 │
│                                                                  │
│  VirtualTextureSystem ── 每帧 Update() 总调度（JobSystem 任务化）  │
│   ├─ FeedbackSystem      写入(渲染pass) / 回读 / 异步分析去重       │
│   ├─ VirtualTextureSpace 页表：layer打包 / buddy分配 / Morton地址   │
│   │                      / 低mip扩散更新 / 空间回收                 │
│   ├─ PhysicalSpacePool   物理页池：按格式分组 / LRU二叉堆 / pin      │
│   ├─ StreamingManager    IO线程 / 解压转码 / staging上传 / 节流     │
│   ├─ HybridPrefetcher    CPU距离+相机速度主动预取（UE 5.6 方案）     │
│   └─ Producer/Finalizer  接口层：                                  │
│        ├─ StreamingVTProducer   （磁盘 tile 生产）                 │
│        └─ RuntimeVTProducer      （GPU 渲染生产，RVT 阶段接入）      │
└──────────────────────────┬──────────────────────────────────────┘
                           │ RHI 抽象接口（第 7 节清单）
┌──────────────────────────▼──────────────────────────────────────┐
│ Engine/Graphics/RHI → Vulkan / Metal / Direct3D12 后端            │
└─────────────────────────────────────────────────────────────────┘

Shader 侧：Engine/Graphics/RHI/Shaders/VirtualTexture/ 共享函数库一份
（页表查找 + 物理 UV + feedback 写入三合一），经 ShaderIR 交叉编译到各后端。
```

### 每帧流程（稳态）

1. 渲染 pass（GBuffer/Forward/SurfaceCacheCapture…）中材质采样 VT → 共享函数顺带写 feedback（降采样因子 N×N 一像素）；
2. 帧末：feedback buffer 经 fence 异步回读（多帧 in flight，结果天然延迟 ≥1 帧）；
3. `VirtualTextureSystem::Update()`：回读完成的前帧 feedback 交给异步分析任务 → 去重哈希 → 页请求合并分类（需加载数据 / 仅需改映射）；
4. `StreamingManager`：请求排序（优先级 = 层级 + 距离 + 是否预取）→ IO 线程读 tile → 解压/转码 → staging 上传物理池；
5. `VirtualTextureSpace`：页表 CS 更新（含向低 mip 扩散），root 页缺失时用平均色兜底，不阻塞渲染线程；
6. 淘汰：LRU 堆 + PageFreeThreshold + 最低 mip pin。

---

## 4. 数据格式设计

### 4.1 离线资产（.vt，TEXR-v2 容器）

```
Header:
  magic 'VTEX' / version
  virtualSize[2]          // 虚拟分辨率（任意 2^n，非正方形）
  layerCount              // 1-4（albedo/normal/ORM…，同 UV 层组）
  layerFormats[4]         // 每层物理格式（BC1/BC5/ASTC…，按平台选择）
  tileSize                // 128（默认）
  borderSize              // 4（默认）
  mipCount                // log2(virtualSize/tileSize)+1，最低 mip ≤ tile
  udimRegions[]           // 混合尺寸区域表（UDIM 等价，可选）
  rootPageColor[4]        // 编译期平均色（UE 5.6 兜底方案）
  pageIndexOffset         // → 页索引表
TileIndex (每虚拟页一条):
  vPageX/vPageY/vLevel    // 虚拟页坐标
  streamOffset/streamSize // 在数据段的偏移（按 mip 就近聚类存放）
  flags                   // compressed / transcodeNeeded
TileData:
  zlib(tile+border 数据, 格式为 layerFormats 的双格式之一)
```

- **双格式烘焙**：Windows/Linux 用 BCn，Apple/mobile 用 ASTC。同一 tile 索引指向两份数据段，运行时按 RHI 能力选择；离线烘两份而不是运行时转码（转码作为 MoltenVK 兜底路径保留，见 §10）。
- 沿用 `ContentTools/TextureImporter.cpp` 已有的 DirectXTex mip/BC 能力，VTBaker 是其下游新增步骤；容器挂接 `ContentToEngine.cpp` 的资产管线（现 `create_texture_resource()` 全 mip 一次性上传路径保留给非 VT 纹理）。

### 4.2 页表纹理（VirtualTextureSpace）

- **多 VT 共享一个 space**（页表纹理），2D buddy 分配器划分子矩形，不够则翻倍 Grow；
- **Layer 打包**：1 层 `R16_UINT`、2 层 `R16G16_UINT`、3-4 层 `R16G16B16A16_UINT`——采样一次页表取所有层物理地址（Stack 的基础）；
- 页 ≤ 64×64 用 16bit 编码（4bit vLevel + 2×6bit pPage），更大用 32bit（4bit + 2×8bit）；
- 虚拟地址 **Morton 码**组织（mip 四叉树父子 O(1) 定位）；
- **向低 mip 扩散更新**：写入某页时同时确保父链页表项有效（父未驻留则指向更粗 mip），这是防 pop-in 的关键机制（UE `ExpandPageTableUpdateMasked/Painters` 的 GPU/CPU 两种实现，我们选 GPU 剔除式）；
- **空间回收**：闲置 space（默认 150 帧，`r.VT.SpaceReleaseFrames` 同款 cvar，-1 关闭）归还页表纹理，加载期防抖；
- 自适应（稀疏）页表（tile 数超页表上限时层级化）列为扩展，本期固定上限。

### 4.3 物理页池（PhysicalSpacePool）

- 按 `(格式组, tileSize+border)` 分池；每池一张物理纹理，2048² 起步、`r.VT.PoolSizeScale` 可缩放；
- 页布局 `tileSize + 2×border`，UV 换算：
  `pUV = pPage × pPageSize + frac(vUV × 2^-vLevel) × vPageSize + border`；
- 管理结构：`FTexturePagePool` 同款——LRU **二叉堆**（堆顶最久未用）+ 页表项数组 + 反查哈希；pAddress 一维展开 X 优先；
- pin 策略：每个 VT 的最低 mip 页常驻（安全网，防全黑采样）；过度 pin 会退化缓存命中率，仅 root 页强制；
- 淘汰节流：`r.VT.MaxUploadsPerFrame`（游戏 8 / 编辑器 64）与 `r.VT.PageFreeThreshold`（默认 15，UE 5.6 调优值）。

---

## 5. 运行时子系统

### 5.1 VirtualTextureSystem

- 全局单例（对齐 UE），持有个体 VT 注册表、space 池、物理池、feedback、流送；
- `Update()` 在渲染帧调度中调用，内部任务全部走 JobSystem（异步分析/合并阶段），同步等待点压缩到提交上传前；
- 接口面向能力提供：`RegisterStreamingVT(asset)` / `RegisterRuntimeVT(desc)` / `SampleHandle` 给材质系统。

### 5.2 Feedback

- **写入**：材质共享采样函数顺带输出（不做独立 pre-pass——与 UE 同款，避免一遍几何重画）；每 N×N 像素写一个（`r.VT.FeedbackFactor`，默认 2），半透明/多页冲突用像素坐标+深度+帧号伪随机选一；
- **编码**：32bit = `|4bit vtId | 4bit vLevel | 12bit vPageX | 12bit vPageY|`；
- **回读**：专用 readback buffer 池按帧轮转，fence 信号后 CPU 才读；
- **分析**：异步任务扫描 → run-length 合并 → `UniquePageList`（开放寻址哈希，value=请求计数）→ 多任务结果 merge → 分类（LoadRequest / MappingRequest）。

### 5.3 流送与 IO

- IO 线程（复用并改造 `AsyncResourceLoader`：其"写死 mip=1、空 `ProcessPendingUploads()`"的现状由 VT 管线接管，原路径保留服务非 VT 纹理）；
- 管线：读 tile → zlib 解压（按需）→ 格式校验 → staging buffer → `CopyBufferToImage` 到物理池页区域 → 通知页表更新；
- **上传批合并**：同帧多个小 tile 合并进一次 staging/拷贝命令，减少 command buffer 抖动；
- 优先级：可见性（feedback 命中）> 主动预取 > 淘汰回收；IO 深度受限可取消（页淘汰时撤销未完成请求）。

### 5.4 混合流送（HybridPrefetcher，UE 5.6 方案）

- 动机：纯 feedback 被动响应在镜头快速移动时跟不上（UE 六年后的官方补丁，我们第一天就做）；
- VT 可 opt-in：mip 级 CPU 距离流送（复用传统纹理流放的"包围盒+屏幕尺寸"估算），预算 `r.Streaming.PoolSizeForVirtualTextures` 等价 cvar，优先级字段挂在 VT 资产与纹理组上；
- 相机速度外推：按当前速度预取运动方向前方 tile（进阶，M3 后评估）。

### 5.5 Producer / Finalizer 接口层

```
class IVTProducer {
    // 返回句柄；完成后由 Finalizer 落位
    RequestPageData(vPage, callback);
    CancelRequest(handle);
};
class IVTFinalizer {
    // 把生产出的页数据写进物理池并触发页表更新
    FinalizePages(producer);
};
```

- `StreamingVTProducer`：磁盘 tile 生产（M2）；
- `RuntimeVTProducer`：GPU 渲染生产（M5，RVT）；
- `UploadFinalizer`：staging 拷贝落位（通用）；
- `GPUFinalizer`：RVT 压缩页直接写物理池（M5，对应 UE 5.2 的 UAV aliasing 优化路径）。

### 5.6 RVT（运行时虚拟纹理）

- `RuntimeVTVolume`：世界空间 AABB，正交 -Z 投影，尺寸 = tileSize × tileCount（上限 4096² 页）；
- 页生产三 pass（对齐 UE）：
  1. **Draw**：收集 Volume 内静态网格，材质 MRT 输出到 RGBA8 RT（层分离）；
  2. **Compress**：CS 现场压缩 BC1/BC5/ASTC（Apple 上 ASTC）；
  3. **Finalize**：`GPUFinalizer` 把压缩页写进物理池（storage image 直写，跳过回读）；
- 页驻留后不重渲，被 LRU 淘汰后再次请求才重生产——本质是着色缓存，只适合静态物体；
- **本引擎的落点**：PCG/SurfaceNets 生成的地形网格目前逐像素跑完整材质求值；RVT 可把地形混合（多层 splat + decal）烘焙进缓存，主 pass 单次采样。远期与 Lumen Surface Cache 的关系：RVT 是"内容侧"缓存（材质结果），Surface Cache 是"光照侧"缓存，二者互补不合并；
- 内容打包沿 UE 方案：BaseColor→BC1/ASTC；Normal 世界空间存 BC5；ORM 打包；WorldHeight R16（PCG 地形高度查询用）；
- 低 mip 可选烘焙为 SVT 磁盘数据（大世界组合技），列为 M5 扩展。

---

## 6. Shader 端设计

### 6.1 共享函数库（一份，交叉编译）

```
// 采样 + feedback 三合一（UE FinalizeVirtualTextureFeedback 同构）
float4 VTSample(VTParams vt, SamplerState s, float2 vUV, int2 pixelPos)
{
    // 1. 手动 mip：dUV×virtualSize → footprint → 0.5*log2，clamp [0, maxLevel]
    //    + MipBias + 帧随机抖动（trilinear 经 TAA 积分，UE 同款）
    // 2. 页表查找：texelFetch(pageTable, int3(vPage, vLevel)) → 解码 pPage（多层一次取齐）
    // 3. 物理 UV 换算（§4.3 公式）→ PhysicalSpace.SampleGrad(...)
    // 4. feedback：本像素命中采样点 → 按 N×N 因子与伪随机阈值写 SSBO
}
// 各向异性：MipLevelAniso2D 计算 footprint 椭圆 → SampleGrad 交给硬件
```

- 位置：`Engine/Graphics/RHI/Shaders/VirtualTexture/VTCommon.*`，进 ShaderIR 现有编译矩阵（.spv/.air/后端变体）；
- **Stack**：`vt.layers` 相同 UV 的多材质槽共享第 2 步结果（GBuffer 的 albedo/normal/ORM 三层 = 1 次页表 fetch + 3 次物理 fetch，对比现状 3 次普通 fetch，成本近似持平）。

### 6.2 过滤策略

| 过滤 | 方案 | 参数 |
|---|---|---|
| 双线性 | tile border padding | border=2 即无接缝 |
| 各向异性 | 手动 footprint（MipLevelAniso2D）+ SampleGrad | border=4（上限） |
| 三线性 | mip level ±0.5 随机抖动 + TAA 积分 | 无 TAA 路径退化为最近 mip（接受噪点，列为已知限制） |

### 6.3 材质系统集成

- `MaterialInstance`（现 bindings 0=albedo/1=normal/2=ORM，`MaterialInstance.cpp:127-139`）：新增 VT 绑定模式——三个槽指向同一 VT 的三个 layer，descriptor 只绑页表+物理池（**全场景共享**，per-material descriptor 大幅减少）；
- MaterialGraph：`SampleTextureNode`（现为返回白色的 stub）新增 `SampleVirtualTextureNode`（uv → float4，带 layer 选择与 MipValueMode 枚举：None/MipLevel/MipBias）；
- **Nanite 集成**：`GPUMaterialRegistry::MaterialData` 的 `albedo/normal/orm_texture_idx`（现指向 Texture2DArray layer）改为 `vtId + uvScale`；`CreateTextureArray()` 的单尺寸限制随 VT 化消失；GBuffer/SurfaceCacheCapture shader 换 `VTSample`；
- 兼容：非 VT 纹理与 VT 采样在材质里可混用（`VirtualTextureFeatureSwitch` 等价物：RHI 不支持时的 fallback 路径）。

---

## 7. RHI 层需求（能力清单与缺口）

| # | 能力 | 用途 | 现状判断 | 工作量 |
|---|---|---|---|---|
| 1 | 整数格式纹理（R16/G16/R32_UINT）+ texelFetch 采样 | 页表 | RHI 有格式系统，需补 texelFetch 采样路径三后端 | 小 |
| 2 | fragment 阶段 SSBO 随机写 | feedback 写入 | Vulkan 支持；Metal 对应 MSL buffer+device fragment writes；D3D12 UAV | 中 |
| 3 | 带 fence 的异步回读 buffer（多帧轮转） | feedback 回读 | 现无通用 readback 路径，需新增 RHI 接口 | 中 |
| 4 | staging → 纹理**局部区域**上传（带 offset 的 CopyBufferToImage） | tile 上传 | VulkanTexture 已有块压缩上传，需暴露带 offset 的页粒度版本 | 小 |
| 5 | storage image 写（format qualified） | RVT GPUFinalizer / 页表 CS 更新 | 需三后端补齐 | 中 |
| 6 | ASTC 格式支持 | Apple tile | 现无 ASTC，MoltenVK 原生支持，RHI 格式表补条目 | 小 |
| 7 | CS 写任意 mip level 的纹理（页表更新） | 页表 | Vulkan/Metal/D3D12 均可，走 storage image 或 CS+image barrier | 中 |

- 页表更新选 **CS 直写**（比 UE 的"图形管线 instanced quad 渲染进页表"简单直接，Vulkan/Metal 均自然支持）；
- 上传统一走传输队列可选项（M6 性能调优时评估专用 transfer queue）。

---

## 8. 与现有系统的集成与改造点

| 系统 | 改造 |
|---|---|
| `ContentToEngine.cpp` | TEXR-v2/`.vt` 解析与注册；`create_texture_resource()` 原路径保留（非 VT） |
| `AsyncResourceLoader.cpp` | 保留服务普通纹理；VT 的 IO/解压/上传走新 StreamingManager（其空壳 `ProcessPendingUploads` 由 VT 管线取代职责） |
| `VulkanContent.cpp` legacy 路径 | 不动（兼容），新资产不再走此路径 |
| `ForwardSceneRenderer` | `LoadTextureFromFile` 场景初始化阻塞加载 → VT 资产注册（渐进呈现）；GBuffer 材质采样换 VTSample |
| RenderGraph | 新增 pass 类型声明：FeedbackAnalyze（async）、PageTableUpdate（CS）、VTUpload（transfer）；页表/物理池作为持久资源跨 pass 声明 |
| Nanite 管线 | GPUMaterialRegistry 改 VT 采样（§6.3）；VisibilityBuffer 材质 pass 同步 |
| Lumen | SurfaceCacheCapture.metal 采样换 VTSample（可选开关）；DDGI 不动 |
| PrimalEditor | VT 资产检查器（池占用/页表可视化/mip 驻留着色） |
| cvar 体系 | `r.VT.*` 命名空间对齐 UE（Flush/MaxUploadsPerFrame/PageFreeThreshold/PoolSizeScale/FeedbackFactor/SpaceReleaseFrames/MaxAnisotropy/Borders） |

---

## 9. 调试、工具与可观测性

- **统计面板**（对齐 `stat virtualtexturing` / `stat virtualtexturememory`）：VT 系统耗时（分析/IO/上传/页表更新 ms）、页表计数（驻留/请求/命中/淘汰）、物理池占用与命中率、feedback 处理页数；
- **调试视图**：`r.VT.Borders 1` 等价物（材质上显示 tile 网格与驻留 mip 颜色编码）、feedback 请求热力图、页表空间分配图（space 内各 VT 的 buddy 块）；
- **日志/自检**：页表项不一致检测（驻留页无页表项）、池泄漏检测、空间回收事件日志；
- 编辑器 VT 资产检查器：预览、格式/尺寸信息、池占用估算（对齐 UE RVT 资产编辑器的 PageTable/Physical 内存预估显示）。

---

## 10. 平台策略

| 平台 | tile 格式 | 说明 |
|---|---|---|
| Windows / Linux（D3D12/Vulkan） | BCn | TextureImporter 已有压缩能力直接复用 |
| macOS / iOS（Metal/MoltenVK） | ASTC（离线烘焙） | Apple GPU 无原生 BCn 采样；MoltenVK 载入转 BCn→ASTC 作为未双烘资产的兜底路径（页粒度小，转码成本可控） |

- 内存预算默认：物理池每格式组 2048²（≈ BCn 128×128 tile + border 下 240 页/池），全局上限可配；
- 移动端降级：`r.VT.PoolSizeScale 0.5`、FeedbackFactor 放宽、tile 256 减少 bookkeeping（后评估）。

---

## 11. 分阶段实施计划

> 每阶段以可运行验收为准；阶段间保持主干可编译、Sponza 全程作为基准场景。

### M0 — RHI 能力补齐 + 原型验证（先证可行）
- RHI 清单 §7 的 #1/#3/#4（整数纹理采样、异步回读、局部上传）；
- 手工驱动：单张 Sponza 纹理离线手工切 tile → 最小页表 + 物理 atlas + 两段式采样 shader；
- **验收**：静态相机下与原图逐像素对比（误差 ≤ 压缩格式固有损失）；三后端编译通过。

### M1 — ContentTools VT 烘焙管线
- VTBaker：tile 切分、页索引、双格式（BCn+ASTC）、root 平均色、zlib；
- TEXR-v2 容器 + `ContentToEngine` 解析；
- **验收**：Sponza 全套 75 张纹理烘焙为 .vt；资产尺寸/加载校验通过。

### M2 — 完整 SVT 运行时
- VirtualTextureSystem / Feedback（写入+回读+异步分析去重）/ 页表 space（buddy+Morton+低 mip 扩散）/ 物理池（LRU+pin）/ StreamingManager（IO 线程+staging 批量上传+节流）；
- GBuffer 三层 Stack 采样接入（MaterialInstance VT 绑定模式）；
- **验收**：Sponza 全材质 VT 化，飞行相机无可见 pop-in（含快速移动）；显存 ≤ 目标；淘汰/重加载循环 30 分钟无泄漏（池页/页表项/IO 请求计数归零校验）。

### M3 — 质量与响应性
- 过滤完整化（border 策略、aniso 手动 footprint、TAA 抖动 trilinear）；
- HybridPrefetcher（CPU 距离 opt-in + 优先级）+ root 平均色兜底（不阻塞渲染线程）；
- 调试视图与 stat 面板第一版；
- **验收**：快速移动 pop-in 显著低于 M2（录屏对比）；aniso 8x 下无接缝伪影；无 TAA 时退化可接受。

### M4 — Nanite 集成与混合尺寸
- GPUMaterialRegistry 改 VT 采样；CreateTextureArray 单尺寸限制移除；
- udimRegions 混合尺寸支持 + 材质多 VT 参数化；
- `r.VT.*` cvar 体系与纹理组优先级；
- **验收**：Nanite 管线下 4K/8K 资产 + 混合尺寸材质正常；Sponza + 4K 替换资产压测。

### M5 — RVT（运行时虚拟纹理）
- Producer/Finalizer 接口抽取（若 M2 时已按 §5.5 实现则此步仅接入）；RuntimeVTVolume、三 pass 页生产（Draw/Compress CS/GPUFinalizer）；
- 应用：PCG/SurfaceNets 地形材质混合缓存 + WorldHeight 层；
- **验收**：地形区域材质求值成本下降（GPU timing 对比）；静态场景 RVT 命中率 > 95%；动态物体正确走主 pass。

### M6 — 打磨与工程化
- 空间回收（SpaceReleaseFrames）、PageFreeThreshold 调优、上传批合并、专用 transfer queue 评估；
- PrimalEditor VT 检查器；崩溃/泄漏长跑；移动端降级配置；
- **验收**：加载-卸载循环页表空间可回收；30 分钟压测稳定；文档（使用手册 + 调参指南）。

---

## 12. 风险与开放问题

| 风险 | 缓解 |
|---|---|
| 采样成本上升（每 VT 采样 ≥2 fetch + 数学） | Stack 摊薄（本引擎材质即同 UV 三层形态，成本近似持平）；材质层数多时收益反超 |
| 无 TAA 路径的 trilinear 抖动噪点 | 提供 MipLevel 显式模式与"最近 mip"退化开关 |
| Apple BCn 转码 CPU 成本 | 离线双烘为主，转码仅兜底；页粒度小天然适合 |
| RenderGraph 持久资源/异步 pass 模型不匹配 | M0 先验证 readback fence 与 RG barrier 语义（与 RHI parity 计划对齐推进） |
| feedback 分析与流送和渲染线程竞争 | 全异步任务化 + 明确的所有权交接点（回读 fence 后只读） |
| 页表 CS 更新的屏障正确性（三后端差异） | M0 写一致性测试（写后立即采样验证）；RenderDoc/Xcode GPU debugger 抓帧 |
| 与 bindless 材质索引体系的描述符布局冲突 | M4 前置设计评审：页表/物理池走全局 set（类似现 Surface Cache 全局 atlas 绑定） |
| 开放：物理池格式分组粒度（BC1 与 BC5 是否共池） | 参考 UE pool config，按 tileSize 分组起步，M6 依据命中率调 |
| 开放：M5 RVT 与 Lumen Surface Cache 的资源复用边界 | 设计上互补不合并；若相机相关性强再评估统一页池 |

---

## 13. 参考

- UE 官方：[Virtual Texturing](https://dev.epicgames.com/documentation/unreal-engine/virtual-texturing-in-unreal-engine) / [RVT](https://dev.epicgames.com/documentation/unreal-engine/runtime-virtual-texturing-in-unreal-engine) / [SVT](https://dev.epicgames.com/documentation/unreal-engine/streaming-virtual-texturing-in-unreal-engine) / [VT Reference](https://dev.epicgames.com/documentation/en-us/unreal-engine/virtual-texturing-reference?application_version=4.27)
- UE 源码解析：[游戏引擎随笔 0x14（丛越）](https://zhuanlan.zhihu.com/p/143709152) · [Runtime Virtual Texture 基本流程（李兵）](https://testerhome.com/topics/24827/show_wechat)
- UE5 演进：[Tom Looman 5.6 要点](https://tomlooman.com/unreal-engine-5-6-performance-highlights/) · [Tom Looman 5.7 要点](https://tomlooman.com/unreal-engine-5-7-performance-highlights/) · [UE 5.8 Mesh Terrain](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-5-8-release-notes?lang=en-US)
- 原理与实现：[shlom.dev 深度解析](https://www.shlom.dev/articles/how-virtual-textures-really-work/) · [Sean Barrett 经典 SVT](https://silverspaceship.com/src/svt/) · [PLAYERUNKNOWN 生产实践](https://playerunknownproductions.net/news/virtual-texturing) · [SIGGRAPH 2023 COD VT](https://advances.realtimerendering.com/s2023/index.html)
- 引擎内资源：`Engine/Graphics/Nanite/NaniteStreamingManager.*`（页池/LRU 模板）、`Engine/Graphics/Lumen/SurfaceCache/SurfaceCachePass.cpp`（页分配 atlas 模板）、`ContentTools/TextureImporter.cpp`（mip/BCn）、`Engine/Graphics/MaterialInstance.cpp`（材质绑定）、`Engine/Graphics/Nanite/GPUMaterialRegistry.h`（bindless 材质）
