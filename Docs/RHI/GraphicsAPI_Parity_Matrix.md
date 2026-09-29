# 图形 API 核心对象对照矩阵（Vulkan / Metal / D3D12 / Dawn-WebGPU）

> 归属：`Docs/RHI/` ｜ 建立日期：2026-09-14 ｜ 配套文档：[rhi-barrier-semantics.md](../rhi-barrier-semantics.md)（同步语义三端规范）、[2026-08-19-rhi-vulkan-metal-parity-plan.md](../2026-08-19-rhi-vulkan-metal-parity-plan.md)（双端对等计划）
>
> Vulkan↔Metal 对照基表来自知乎答主 **Meteroa Van**（[知乎回答](https://www.zhihu.com/question/1958242900426068747/answer/2082415000082297981)），本文在其基础上补全 **D3D12** 与 **Dawn(WebGPU)** 两列，并追加本仓库 RHI 相关的补充对照行。商业引用请联系原作者；本文内部使用已注明出处。
>
> 标记：✅ 对等｜⚠️ 功能近似但模型不同｜❌ 无直接对应

---

## 1. 核心对象对照总表

| 模块 | Vulkan | Metal | D3D12 | Dawn / WebGPU | 差异说明 |
|---|---|---|---|---|---|
| 设备抽象 | ✅ `VkPhysicalDevice` / `VkDevice` | ✅ `MTLDevice` | ✅ `IDXGIAdapter1` / `ID3D12Device` | ✅ `Adapter` / `wgpu::Device` | Vulkan 与 D3D12 都分离物理/逻辑设备；Metal 合并为 `MTLDevice`；WebGPU 恢复 Adapter→Device 两级，但一个逻辑设备就是唯一 GPU 入口 |
| 命令队列 | ✅ `VkQueue`（queue family 分能力） | ✅ `MTLCommandQueue` | ✅ `ID3D12CommandQueue`（DIRECT/COMPUTE/COPY 类型） | ⚠️ `wgpu::Queue`（每设备一个） | Vulkan/D3D12 支持多队列按能力分工；WebGPU 只暴露单设备队列（实现内部仍可映射多后端队列） |
| 命令缓冲区 | ⚠️ `VkCommandBuffer`（主/从二级） | ✅ `MTLCommandBuffer` | ✅ `ID3D12GraphicsCommandList`（Direct/Bundle） | ⚠️ `CommandEncoder` → `CommandBuffer` | Vulkan 主/从、D3D12 Direct/Bundle 两级录制；Metal 与 WebGPU 都没有通用二级录制，D3D12 Bundle 是最接近的近似（状态受限）；WebGPU CommandBuffer 是一次性提交单元 |
| 命令池 | ✅ `VkCommandPool` | ❌ 无 | ✅ `ID3D12CommandAllocator` | ❌ 无 | D3D12 CommandAllocator 是 `VkCommandPool` 的真实对应物（reset 语义也类似）；Metal/Dawn 由 Queue/Device 直接创建，无池对象 |
| 渲染通道 | ✅ `VkRenderPass` + `VkFramebuffer`（子通道+依赖系统） | ✅ `MTLRenderPassDescriptor`（仅附件描述） | ⚠️ 无一等对象：`OMSetRenderTargets` + 资源状态；RS5 起 `BeginRenderPass` 仅为 tile 优化提示 | ✅ `RenderPassEncoder`（`BeginRenderPass` 描述附件） | Vulkan 有子通道/依赖系统；Metal/Dawn/传统 D3D12 都是"附件描述 + 编码器"模型；D3D12 的 `BeginRenderPass/EndRenderPass` 只是 hint，不承载依赖语义 |
| 管线对象 | ✅ `VkPipeline`（图形/计算，可拆动态状态） | ✅ `MTLRenderPipelineState` / `MTLComputePipelineState` | ✅ `ID3D12PipelineState`（PSO） | ✅ `wgpu::RenderPipeline` / `wgpu::ComputePipeline` | 四家都是"编译好的完整管线状态"对象；动态化程度 Vulkan 最多，WebGPU 最少 |
| 管线布局 | ✅ `VkPipelineLayout`（描述符集布局集合） | ⚠️ `MTLArgumentEncoder`（Argument Buffer 模型） | ✅ `ID3D12RootSignature`（根参数模型） | ✅ `wgpu::PipelineLayout`（`BindGroupLayout` 数组） | 四种绑定模型形态各异：描述符集 / Argument Buffer / 根签名 / BindGroupLayout；RHI 层需以"布局=绑定槽契约"做统一抽象 |
| 描述符集 | ✅ `VkDescriptorSet` + `VkDescriptorPool` | ❌ 无（Argument Buffer 为近似物） | ⚠️ `ID3D12DescriptorHeap` + 描述符句柄（CBV/SRV/UAV/Sampler） | ✅ `wgpu::BindGroup`（按 Layout 严格校验） | D3D12 有描述符堆但没有"成组 set"对象，靠根描述符表索引；WebGPU 的 BindGroup 是四家中最接近"类型化描述符集"的一等公民 |
| 缓冲区 | ✅ `VkBuffer` | ✅ `MTLBuffer` | ✅ `ID3D12Resource`（BUFFER 维度） | ✅ `wgpu::Buffer` | 对等；差异在堆/放置策略（见"内存"行） |
| 图像/纹理 | ✅ `VkImage` | ✅ `MTLTexture` | ✅ `ID3D12Resource`（TEXTURE1D/2D/3D 维度） | ✅ `wgpu::Texture` | 对等；D3D12 与 Vulkan 一样区分缓冲/纹理但共用 Resource 类型 |
| 图像视图 | ✅ `VkImageView` | ✅ texture view（`createView`） | ⚠️ 无独立对象：视图信息折入 SRV/UAV/RTV/DSV 描述符 | ✅ `wgpu::TextureView`（`createView`） | D3D12 把"视图"折叠进描述符创建；Metal 与 WebGPU 都有显式 view 对象 |
| 采样器 | ✅ `VkSampler` | ✅ `MTLSamplerState` | ✅ `CreateSampler`（堆内描述符） | ✅ `wgpu::Sampler` | 对等 |
| 内存 | ✅ `VkDeviceMemory`（显式类型选择/分配/绑定） | ⚠️ runtime 托管 + 存储模式（Shared/Managed/Private） | ⚠️ `ID3D12Heap`：Committed/Placed/Reserved 三种放置 + UPLOAD/DEFAULT/READBACK 堆类型 | ❌ 完全托管（仅 buffer 的 map/unmap 映射区间） | 显式性光谱：**Vulkan ≈ D3D12（显式堆/放置）＞ Metal（存储模式）＞ WebGPU（全托管）**；RHI 内存抽象按 Vulkan 建模时，Dawn 后端只能整体降级为托管语义 |
| 同步：信号量（GPU↔GPU） | ✅ `VkSemaphore`（含 timeline） | ⚠️ `MTLSharedEvent`（跨队列/进程；队列内还有 `MTLFence`） | ⚠️ 无独立对象：`ID3D12Fence` 兼任 GPU↔GPU 信号 | ❌ 无用户级信号量（单队列模型；互操作走 SharedFence 扩展） | Vulkan 语义最全；D3D12 用 64 位单调 fence 统一两种场景；WebGPU 的单队列模型基本消除了显式 GPU↔GPU 信号需求 |
| 同步：围栏（CPU↔GPU） | ✅ `VkFence` | ⚠️ completion handler / `MTLSharedEvent` 等待 | ✅ `ID3D12Fence` + 系统事件（`SetEventOnCompletion`） | ⚠️ `queue.onSubmittedWorkDone` 回调 | Vulkan/D3D12 原生 fence 对象可查询可等待；Metal/Dawn 偏回调风格 |
| 同步：管线屏障 | ✅ `VkPipelineBarrier`（阶段+访问掩码全表达） | ⚠️ encoder 边界隐式屏障 + `memoryBarrierWithScope`（粒度粗） | ✅ `ResourceBarrier`（TRANSITION/ALIASING/UAV 三类，按状态迁移） | ❌ 无显式屏障——由 usage 声明自动推导 | 见配套 [rhi-barrier-semantics.md](../rhi-barrier-semantics.md)：Dawn 的 no-op 是三端约定里已知坑之一；D3D12 表达力强但按"状态机迁移"而非"阶段掩码"建模 |
| 着色器模块 | ✅ `VkShaderModule`（SPIR-V） | ✅ `MTLFunction`（MSL 源码 / metallib） | ⚠️ 无独立对象：DXIL 字节数组直接进 PSO 描述（root signature 单独 `CreateRootSignature`） | ✅ `wgpu::ShaderModule`（WGSL 源码，内部经 Tint 转译到各后端） | 本仓库着色器单源策略：GLSL → SPIR-V（Vulkan）/ MSL（Metal）/ WGSL（Dawn），见 `build_spv` 与 PlatformProfile |
| 推送常量 | ✅ Push Constants（典型 128–256B 预算） | ❌ 无原生：`setVertexBytes`/`setFragmentBytes` 内联（≤4KB）或 Argument Buffer | ✅ 根常量（Root Constants；与根描述符共享 64 DWORD 根签名预算） | ❌ 无标准等价物：uniform buffer 模拟（Dawn 有非标准实验特性） | 本仓库对等计划 F6 的 ">128B push constant 回退" 即针对此断层；Dawn/Vulkan 对齐时统一走 uniform blob |
| 动态管线状态 | ✅ `VkDynamicState`（大量状态可动态） | ⚠️ 子集（viewport/scissor/blendColor/stencilRef/depthBias/per-draw 图元类型） | ⚠️ 子集（viewport/scissor/blendFactor/stencilRef/顶点-索引缓冲/per-draw 拓扑；随版本扩展） | ❌ 最受限（viewport/scissor/blendConstant/stencilRef/绑定组；深度偏移、格式等编译进管线） | RHI 抽象若以 Vulkan 动态态为上限，Dawn 后端需要把这些状态编译进 `RenderPipeline` 并在变更时重建管线 |

---

## 2. 补充对照（本仓库 RHI 相关）

| 模块 | Vulkan | Metal | D3D12 | Dawn / WebGPU | 说明 |
|---|---|---|---|---|---|
| 交换链 | ✅ `VkSwapchainKHR` | ✅ `CAMetalLayer` | ✅ `IDXGISwapChain`（DXGI） | ✅ `wgpu::Surface` + `Surface::configure` | 对等计划 F2 的"swapchain 自动重建"（resize/丢失）需在 RHI 层做统一事件模型；WebGPU 的 `getCurrentTexture` 语义与另外三家不同 |
| 纹理诊断元数据（2026-09-28） | ✅ `GetTextureSize`（含 view base mip）/ `GetTextureFormat` | ⚠️ size 已实现；format 返回基类 `Unknown` | ⚠️ 基类 false/Unknown | ⚠️ 基类 false/Unknown | 可选只读接口，非渲染能力门；Vulkan 同帧 gather 回放按实际格式分配 readback，未知格式拒绝采集，不猜 BGRA8/浮点法线大小 |
| 二级命令录制 | ✅ secondary command buffer | ❌ 无 | ⚠️ Bundle（受限状态集，只能 ChangeState 少数 API） | ❌ 无标准（Dawn `Render Bundles` 为实验扩展） | 对等计划 F7 在 Metal/Dawn 无落点；RHI 的"录制复用"抽象在两后端需降级为重录或扩展特性 |
| 间接绘制 | ✅ `vkCmdDraw*Indirect`（含 count buffer） | ✅ `drawIndexedPrimitives` indirect buffer | ✅ `ExecuteIndirect` | ❌ 无标准（Dawn 有实验性 indirect commands 扩展） | GPU-Driven/Nanite 路径的关键依赖；Dawn 后端当前需绕行（本仓库 Dawn 侧 GPU-driven 尚未走 indirect） |
| 时间线同步 | ✅ `VkTimelineSemaphore` | ⚠️ `MTLSharedEvent` 值等待（天然时间线形状） | ✅ `ID3D12Fence` 本身即 64 位单调值 | ⚠️ 单队列 work-done 状态 | WebGPU 无跨队列时间线；多队列渲染图在 Dawn 上退化为单队列串行 |

---

## 3. RHI 层设计含义（小结）

1. **显式性光谱**：Vulkan ≈ D3D12 ≫ Metal > Dawn/WebGPU。以 Vulkan 为建模上限的 RHI 抽象，在 Dawn 后端必须接受"整段能力降级为托管语义"（内存、屏障、信号量、间接绘制、二级录制）；这与本仓库"机制进 RHI、算法进 pass"的分层原则一致。
2. **绑定模型四象限**：DescriptorSet / Argument Buffer / RootSignature / BindGroup。统一抽象应落在"管线布局=槽位契约、绑定=按布局校验的组"这一公共子集上。
3. **推送常量断层**：仅 Vulkan/D3D12 有真硬件概念；Metal 有内联 bytes 近似；Dawn 只能 buffer 模拟 → 平台分支点是 `PlatformProfile` 的能力位，而非 shader 重写。
4. **屏障语义**：Dawn 自动同步是"抽象的福利"，也是排查跨端差异时的盲区（后端 no-op，问题常被别的后端掩盖），参见 [rhi-barrier-semantics.md](../rhi-barrier-semantics.md) 的三条约定。

## 4. GI 能量所有权修复（2026-09-18）Vulkan 领先项（Metal/Dawn 后置清单）

> 依据：`Docs/2026-09-18-gi-energy-ownership-remediation-plan.md`（Vulkan 优先策略，对齐 GI 计划 §2①）。
> 下列各项已在 Vulkan 链落地；Metal 侧对齐时按本清单逐项补齐。

| # | 项 | Vulkan 状态 | Metal 缺口 | 涉及文件 |
|---|---|---|---|---|
| 1 | SH 评基单源（EvalLib.glsl：evalProbe{Radiance,Irradiance}SH9） | ✅ 三消费端单源 | ⚠️ 复制实现已同步注释互指（DDGIGIGather.metal / SurfaceCacheLightEval.metal）——Metal 收敛到共享头需 MSL include 机制 | ShaderLib/EvalLib.glsl |
| 2 | DDGI 槽位单一事实源（DDGISlotMap.h，Fusion cbIdx 时钟废除） | ✅ 全链 | ✅ CPU 端共享（无缺口）；Metal 专属 mirror-to-idle 路径（SCDDGIIntegrationModule）沿用 BySlot 语义 | Engine/Graphics/Lumen/DDGISlotMap.h |
| 3 | SC 注入置信门（binding 6 = confidence，settled 档位 0.45→0.15） | ✅ | ❌ DDGIProbeIrradianceFromCards.metal 未接 confidence 绑定与 settled 门 | SCDDGIIntegrationModule / DDGIProbeIrradianceFromCards.* |
| 4 | openSky V2（float2 {escapeRatio, skyEnergyRatio} + E_geo/E_sky 拆分） | ✅ | ❌ SurfaceCacheLightEval.metal 未读 sER（stride-2）、DDGIGIGather.metal far-path 未接 atten 拆分 | DDGIUpdateIrradiance / GIGatherModule / LightEval |
| 5 | 远场天空 ×0.5 退场（SetFarSkyScale 1.0，env A/B） | ✅ | ✅ 共享 CPU 路径（无缺口） | StandardRenderPipeline |
| 6 | E4 断边（FusionIndirect→DDGI 默认断开，env=1 回接） | ✅ | —（MakeProbeRadianceSource 本就 Vulkan 门控，Metal 无此边） | StandardRenderPipeline |
| 7 | GBuffer 进 RG（producer identity 单一导入 + 删手工 no-op barrier） | ✅（BuildRenderGraph 站点） | ❌ Metal 依赖 encoder 边界语义（另案）；RenderWithCommandBuffer twin 路径两侧均保持旧 per-site 导入 | StandardRenderPipeline / SurfaceCachePass |
| 8 | GIHistoryValidity 最小版（灯光事件→DDGI 提权/SC tile 清零/SPGI reset） | ✅ | ✅ 共享 CPU 路径（无缺口） | StandardRenderPipeline / LumenDDGIPass / SurfaceCachePass |
| 9 | SC tile radiosity 默认消费权重（2026-09-25 Sponza 对照） | Vulkan 0.5（右廊/近景地面 A/B 收益；`PRIMAL_SC_RADIOSITY_W=1.0` 回滚） | Metal/Dawn 保持 1.0；未做对应画面校准，属有意暂时分叉 | SurfaceCachePass |
| 10 | DDGI scalar 步进下界 / 网格支持命中证据分离（2026-09-27） | ⚠️ 仅 `SDF_GPU_EVIDENCE` 实验 RG16F + DDGI split mode；实际 GPU 虚假命中减少，但画面退化，默认关闭。SPGI/重定位/WRC 未迁移 | Metal/Dawn 不设置证据模式，仍旧 scalar 语义；尚非待同步生产功能 | OfflineSDFMerger / StandardRenderPipeline / LumenDDGIPass / DDGITraceSDF |
| 11 | DDGI 下半球逃逸环境边界（2026-09-28） | ⚠️ `DDGI_LOWER_ENV_SCALE` 仅 Vulkan split-finalize 消融，默认全球环境不变；`0` 测试下半球零入射，非完整地面反弹模型。复用 `SdfOrigins[0].w` 空槽，无布局/同步变更 | Metal/Dawn 不设置该空槽，维持旧环境；实验尚未晋升，不同步生产默认 | LumenDDGIPass / DDGITraceFinalize |
| 12 | DDGI 方向测量来源诊断（2026-09-28） | ⚠️ `DDGI_DEPTH_PROVENANCE` 仅 Vulkan opt-in；独立三槽计数/时间戳缓冲，诊断 depth 变体增加 bindings7/8；默认布局、矩和消费权重不变。采样状态不等于几何可靠性 | Metal/Dawn 保持原6绑定，不分配/不读取诊断字段；非生产遮挡规则 | LumenDDGIPass / DDGIUpdateDepth / GIGatherModule |
| 13 | DDGI 深度/辐照追踪策略分离（2026-09-28） | ⚠️ `DDGI_DEPTH_ONLY_EVIDENCE` 仅 Vulkan mode5/RG证据实验；额外深度读G，普通辐照保持scalar规则读R。复用第二cascade voxel.w，无布局变更；R逐位一致需独立预检，非遮挡有效性证明 | Metal/Dawn 不设置该空槽，维持旧路径；实验默认关闭 | LumenDDGIPass / DDGITraceSDF |
| 14 | 已核验地板区域消费端消融（2026-09-28） | ⚠️ `DDGI_GATHER_FLOOR_PATCH` 仅 Vulkan dual-policy mode5诊断；debugVis空槽携带平面/半宽；仅额外方向格且深度充分支持的跨地板连线切断可见性和低频贡献，置信度保留未切分母。场景特定三角形证书需独立预检，无绑定/布局变化 | Metal/Dawn不设置空槽、不改消费；不是通用遮挡修复或生产默认 | GIGatherModule / DDGIGIGather |
| 15 | SC CardFill 起点/轴向消融与写入证据（2026-09-28） | ⚠️ `SC_FILL_REJECT_ORIGIN_HIT` / `SC_FILL_AXIS_LIMIT` 两个独立默认关闭实验，复用CardFillCB原padding；`SC_FILL_AUDIT_PREFIX` 同命令前后复制深度/alpha，延迟3帧读回，不改shader数据/绑定 | Metal/Dawn无CardFill消融或诊断分配；不改原路径，尚非生产修复 | SurfaceCachePass / SurfaceCacheCardFill / TestVulkanSponzaRenderGraph |
| 16 | SC CardFill 离线距离场源对照（2026-09-28） | ⚠️ `SC_FILL_OFFLINE_SDF=1` 默认关闭，仅以已有离线SDF替换CardFill纹理/坐标/分辨率，单场且不静默回退；可选`SC_FILL_SDF_AUDIT`读取实际source0，ShaderResource↔CopySource恢复，3帧退休后Readback。无新shader绑定/布局 | Metal/Dawn不替换源、不分配源审计；尚非生产修复，默认不变 | SurfaceCachePass / TestVulkanSponzaRenderGraph |
| 17 | 离线SDF精确体素来源重放（2026-09-28） | ⚠️ 测试宿主`SDF_VOXEL_PROVENANCE`显式指定整数体素CSV及输出，CPU只读重放缓存/模型的实际合并候选；与可选source0 GPU读回对照，无着色器/布局/资源状态新增 | CPU方法共享，不自动调用；Metal/Dawn测试不启动该诊断，生产渲染不变 | OfflineSDFMerger / TestVulkanSponzaRenderGraph |
| 18 | 离线SDF几何编码跨度对照（2026-09-28） | ⚠️ 测试宿主`SDF_GEOMETRY_DECODE=1`默认关闭，读取当前mesh几何认证20% padding，以未padding跨度解码后再传播；独立scalar `PGSDFG01`缓存，不允许覆盖其他格式/混合RG证据。全部离线消费者反馈对照，不是CardFill-only修复，无shader/布局变化 | CPU方法可选共享，Metal/Dawn宿主不启用；暂不改变默认编码/缓存 | OfflineSDFMerger / TestVulkanSponzaRenderGraph |
| 19 | SC 几何支持 GPU 只读证明（2026-09-28） | ⚠️ `SC_GPU_GEOMETRY_PROOF_PREFIX` + queries 显式启用测试宿主独立 4-SSBO compute；实际场景紧凑 BVH，显式 Host/Transfer→Compute→Transfer→Host 屏障、Readback/fence、全部输入逐位读回。94 查询通过：45 伪命中拒绝、4 capture 保留、4 阈值未决，强制溢出全部未决；echo/32节点trace仅诊断。不改 SC/DDGI radiance 或生产命中，未证明射线首交点/alpha | Metal/Dawn不分配或运行该诊断；不更改其 shader/资源布局，不是待同步生产功能 | SCGeometrySupportProof.comp / SCGeometrySupportProof.inl / TestVulkanSponzaRenderGraph |
| 20 | SC 段内几何首交点诊断（2026-09-28） | ⚠️ 同一独立 4-SSBO 诊断增加 `SC_GPU_GEOMETRY_RAYS=1` 变体及严格 `PGRAYS01` 输入；115 查询（已退休卡片 CPU 参数重建射线，非 shader 内采集），62 几何命中/49 段内几何未命中/4 端点未决；强制栈1时112未决、3场景外对照直接排除。交点 mesh/local triangle 与实际加载模型逐位对照，不证明 UV/alpha/材质或天空，不改变生产 radiance | Metal/Dawn 不启动独立射线诊断，不改生产绑定或材质语义；非生产同步项 | SCGeometrySupportProof_ray.comp.spv / SCGeometrySupportProof.inl / audit_sc_ray_support.py |

| 21 | CardFill 实际绑定材质 POD 只读核验/位解释对照（2026-09-28） | ⚠️ `SC_FILL_AUDIT_PREFIX` + `SC_FILL_MATERIAL_AUDIT=1` 复制同命令实际 bindings10/11/7，按实际 buffer 字节数分配 Readback，显式 Host/AllCommands→Transfer→Host，退休3帧后读取；393条material与CPU逐位一致、393 instance身份匹配。`SC_FILL_MATERIAL_BITS=1` 默认关闭，仅pad0 bit1切整数原始位读取；原起点拒绝使用bit0，布局/命中深度/有效性不变。右廊/地面改善但350/480整图门禁失败，不晋升默认。Vulkan仅新增只读 `GetBufferByteSize` 元数据方法，未新增EngineDLL ABI | Metal/Dawn不分配读回、不启用位解释实验；当前无对应CardFill生产路径，不改变其材质规则 | SurfaceCachePass / SurfaceCacheCardFill.comp / VulkanDevice / audit_sc_material_bits.py |

| 22 | SC 实际 GPU UV/纹理取样只读核验（2026-09-28） | ⚠️ `SC_GPU_MATERIAL_PROOF_PREFIX` + queries 默认不启用；测试宿主独立7绑定compute，用实际全局元素/instance/material/albedo SRV，显式 Host/AllCommands→Compute→Transfer→Host 屏障、同步提交等待后 Readback。161407顶点UV逐位匹配运行资产；62几何交点UV/颜色通过、3越界全零拒绝。实际 RGBA8 sRGB→GPU线性色、mip0独立linear-clamp采样，四邻域复核；不写atlas/radiance，不证明opacity或生产shader射线 | Metal/Dawn不启动诊断、不增生产布局；GPUDriven仅增现有元素buffer的只读getter，非新增后端渲染能力 | SCMaterialSampleProof.comp / SCMaterialSampleProof.inl / audit_sc_gpu_material.py |
| 23 | DDGI 实际 shader 射线与 native graphics PSO 只读诊断（2026-09-29） | ⚠️ `DDGI_RAY_SOURCE_AUDIT=1` 默认关闭；复用既有 hit-normal SSBO 的独立尾部，TraceSDF/Finalize 各写实际 origin/direction、16位frame/probe身份及ray身份。测试宿主仅 `DDGI_DUMP=2` 显式同步、检查提交完成并按实际update list读取。`VK_NATIVE_PIPELINE_AUDIT=<prefix>` 从成功native创建采集module持有SPIR-V与状态，并记录首次真实graphics绑定；当前Sponza cull/alpha-discard/coverage/blend关闭，不认证物理透射。384实际射线独立BVH：383可认证首交点、1精度未决，栈溢出fail closed；无生产算法候选晋升 | Metal/Dawn不分配该尾部或native诊断；不改变通用RHI同步、生产layout或材质规则。Metal仅有界API启动验证，有library fallback/零draw/fence timeout，非视觉认证 | LumenDDGIPass / DDGITraceSDF / DDGITraceFinalize / VulkanPipeline / VulkanCommandBuffer / audit_ddgi_ray_source.py；证据 `autoresearch/classic-260929-0106` |
| 24 | DDGI 第一表面距离单变量候选（2026-09-29） | ⚠️ `DDGI_FIRST_SURFACE=1` 精确值、scalar mode0、默认关闭；独立16绑定TraceSDF变体，测试宿主按实际scene/material构建BVH，配置重建后给near/far实例接线。仅有限、非透射、非端点且保守重构误差上界≤2e−5的第一几何距离可替换；未知/溢出/segment miss保留旧值，既有normal有效时跳过。3vec4/ray诊断按新鲜list及frame/probe/ray身份核验；384目标中331接受、37透射保留、16精度保留。350/480六ROI均失败，不晋升默认；不认证物理opacity/transport | Metal/Dawn不创建变体或新增绑定，保留默认算法；这是Vulkan测试宿主限定的实验，不是后端对等生产能力。Metal仅有界API冒烟，非视觉证书 | LumenDDGIPass / StandardRenderPipeline / DDGIFirstSurface.glsl / DDGIFirstSurfaceCandidate.inl / audit_ddgi_first_surface.py；证据 `autoresearch/classic-260929-0145` |
| 25 | DDGI 实际查询源与 gather 读回诊断（2026-09-29） | ⚠️ `DDGI_QUERY_SOURCE_AUDIT=1` 依赖 ray-source audit、scalar0、默认关闭；27N 尾部记录实际 Finalize hitPos、严格最小 key、16候选顺序和实际 texel，不改选卡规则。DDGI_GATHER_AUDIT 在快照后显式 Transfer→Host，读回前检查 graphics command Begin/End/Submit/Wait；同迭代完成命令及日志顺序覆盖 Fusion 旧槽复制。350/480 同帧 FusionIndirect 闭合，地面 diffuse Lo 下降而 Composite Lo 上升；8输入尚不独立归因 BRDF/glossy/反射/volume | Metal/Dawn 不启用查询尾部；gather 完成检查使用公共 RHI，仅 opt-in 诊断。Metal 有界冒烟仍受 shader library fallback/Draws0 限制，不是视觉对等证书 | LumenDDGIPass / DDGITraceFinalize / GIGatherModule / TestVulkanSponzaRenderGraph / audit_ddgi_query_source.py；证据 `autoresearch/classic-260929-0220` |
| 26 | FusionComposite 十三源实际绑定诊断（2026-09-29） | ⚠️ GI_ROI_AUDIT 默认关闭；实际 Indirect/Composite descriptor 数组记录来源，13源按运行时 GetTextureFormat/Size 导出（包含真实黑与volume identity），拷贝后 Transfer→Host，Map前独立graphics Begin/End/Submit/Wait检查。完整生产Composite回放四组六ROI误差<.037%；地面候选glossy增加抵消diffuse下降，材质/NoV/primary/SSR/offscreen/volume采样逐位相同。未认证glossy生产端SH/visibility根因，FIRST_SURFACE仍默认关闭、350480完整门禁失败 | 诊断仅Vulkan；不改生产bindings、shader、合成系数或其它后端算法。Metal启动仍受libraryfallback/Draws0限制 | FusionCompositeModule / analyze_fusion_composite_audit.py；证据 `autoresearch/classic-260929-0253` |
| 27 | DDGI glossy 实际消费来源诊断（2026-09-29） | ⚠️ `DDGI_GLOSSY_SOURCE_AUDIT=1` 要求 gather audit 前缀，默认关闭；保留原生产19-entry layout/SPIR-V，独立20-entry辅助layout追加binding21、9vec4/halfres-pixel实际receiver/N/R/NoV/texel/权重/global probe ID。相同输入写独立shadow输出，四组350/480原Main与aux全部逐字节一致；同帧生产spec资源/原始字节另与Fusion binding6核对。仅辅助记录显式Compute/Transfer/Host→Transfer→Host与checked Begin/End/Submit/Wait。六ROI glossy回放误差<1%，350严格SH/visibility算术分解可用；480完整ORM两字节不一致，分解SKIP，CPU diffuse墙面仍UNKNOWN；非物理opacity/transport证书，无质量候选晋升 | 仅Vulkan诊断新增binding；Metal/Dawn生产layout与算法不变。Metal有界API冒烟仍受libraryfallback/Draws0限制，非视觉对等 | GIGatherModule / DDGIGIGather / StandardRenderPipeline / analyze_ddgi_glossy_audit.py；证据 `autoresearch/classic-260929-0310` |

| 28 | DDGI 首命中法线来源只读诊断（2026-09-29） | ⚠️ `DDGI_FIRST_NORMAL_PROOF_PREFIX` 要求 FIRST_SURFACE=1、DUMP=2、ray source audit；默认不启用。独立8绑定compute消费同帧接受的 first triangle/bary/ray 与实际 native positions/elements/instances/materials/normal texture/sampler，48B query、20vec4 result、上限16384 queries；显式 Host/AllCommands→Compute→Transfer→Host，检查 Reset/Begin/End/Submit/Wait 后 Map，先保存 raw 再派生核验。GPUDrivenDraw.wgsl 临时编译与实际 primary module 逐字节相等；使用 probe-ray facing geometry 替代屏幕导数方向，仅源法线反事实。独立数值门限2e−5、actualUV四texel插值门限.002，未闭合样本UNKNOWN；不同mesh/depth的SC normal不认证首表面 | Metal/Dawn不启动诊断，不增生产布局或改算法。只增加既有native normal texture/sampler只读getter；Metal60s仍140library fail/1FenceTimeout/16Draws0，非视觉对等 | DDGIFirstNormalProof.inl/.comp / GPUDrivenDrawPipeline.h / audit_ddgi_first_normal.py；证据 `autoresearch/classic-260929-0433` |
| 29 | DDGI 同首交点 native mapped-N 候选（2026-09-29） | ⚠️ `DDGI_FIRST_NATIVE_NORMAL=1` 要求 FIRST_SURFACE=1、Vulkan scalar；默认关闭。独立24绑定 Trace child 借用实际 GPUDrivenDraw positions/elements/instances/materials/normal texture/sampler，原始 triangle→vertex/instance 映射与保守误差/材质/旧HN/首点 guards 决定仅写 HN；距离、rho、Finalize 的 −rayDir SC lookup 保持原规则。实际 draw 后两种入口刷新 near/far sources。DUMP=2 可选24vec4/ray witness，固定≤64MiB，显式 Transfer→Host 和提交完成检查，保存 raw 后独立核验；2e−5/.002 不放宽，拒绝项保持原 HN。源法线使用 ray-facing 几何，仍是反事实，未认证 primary pixel/physical opacity/transport。350/480完整六ROI决定 keep/discard，未通过不得改默认 | Metal/Dawn不分配或启用该候选，默认布局/算法不变；非待同步生产功能。仅 DebugVulkan 构建树生成的 Metal 冒烟仍有既有 library/fence 限制，不作视觉对等证明 | DDGIFirstNativeNormalCandidate.inl / DDGIFirstNativeNormal.glsl / LumenDDGIPass / StandardRenderPipeline / audit_ddgi_first_native_normal.py；证据 `autoresearch/classic-260929-0530` |


| 30 | SC capture-time 几何来源只读诊断（2026-09-29） | ⚠️ `SC_CAPTURE_SOURCE_AUDIT=1` 的独立4MRT writer/16binding CardFill记录实际frame/kind/cluster/triangle或owner/material；`SC_CAPTURE_GEOMETRY_AUDIT=1`再要求Vulkan/card-view及8MRT，独立同12binding管线增加四个RGBA32_UInt world-point/三顶点persistent witness。实际设备8MRT/16384，2048 atlas总320MiB≤384MiB，默认无资源；显式附件/Compute/Host→Transfer→Host与Reset/Begin/End/Submit/Wait后Map。215目标raster插值点在writer-time triangle≤2e−5，捕获顶点与dump native replay一致；Finalize default0/78、parent40/137在该triangle，严格同点0。材料与历史CardParams/opacity未认证，未实施画质候选，350/480完整六ROI诊断回归PASS | Metal/Dawn不创建variant/witness，不改默认三MRT、CardFill14binding或RHI同步；Metal60s仍libraryfallback/FenceTimeout/Draws0，非视觉对等 | SurfaceCachePass / GPUDrivenDrawPipeline / CardCapture / TestVulkanSponzaRenderGraph / audit_sc_capture_source；证据 `autoresearch/classic-260929-0632`、`classic-260929-0742` |

| 31 | 默认 DDGI 同帧首三角形只读 oracle（2026-09-29） | ⚠️ `DDGI_DEFAULT_FIRST_ORACLE_PREFIX` 要求测试宿主 DUMP2、ray-source audit、默认 scalar，并拒绝 FIRST_SURFACE/FIRST_NATIVE_NORMAL。独立6绑定compute只读实际 source tail/update list及诊断BVH，32768 fresh×12vec4≤6MiB，生产资源只读；UNKNOWN/端点/溢出/segment miss/nonzero-flags guard单列，固定2e−5。显式Host/AllCommands→Compute→Transfer→Host及checked completion后Map，dispatch前后生产buffer复制核对；精确独立entrypoint的成功native compute创建/实际Bind与module持有SPV另存。此行只描述诊断契约，实测/验证结果以本阶段证据为准 | Metal/Dawn不创建oracle或资源，无生产binding/算法/RHI同步语义/EngineDLL ABI变化；独立CB镜像不认证历史生产CB，几何不认证opacity/transport，默认关闭 | DDGIDefaultFirstOracle.inl/.comp / VulkanPipeline / VulkanCommandBuffer / audit_ddgi_default_first_oracle；证据 `autoresearch/classic-260929-0916` |

## 5. 相关文档

- [rhi-barrier-semantics.md](../rhi-barrier-semantics.md) — 三端 MemoryBarrier/fence 语义规范（本文"同步"三行的落地约定）
- [2026-08-19-rhi-vulkan-metal-parity-plan.md](../2026-08-19-rhi-vulkan-metal-parity-plan.md) — Vulkan↔Metal 双端对等计划（F2 swapchain / F6 push constant / F7 二级命令缓冲）
- [Dawn_WebGPU_Implementation_Status.md](./Dawn_WebGPU_Implementation_Status.md) — Dawn 后端实施状态
- [Dawn_WebGPU_Lessons.md](./Dawn_WebGPU_Lessons.md) — Dawn 实践坑复盘
- [RenderGraphDesign.md](./RenderGraphDesign.md) — 渲染图架构（屏障/生命周期的上层消费者）
- [../ARCHIVE.md](../ARCHIVE.md) — 全仓文档索引

## 6. Pass Debug View（2026-09-20）三端对等

| 项 | Vulkan | Metal | Dawn/其它 | 涉及文件 |
|---|---|---|---|---|
| PassDebugViewModule（全屏 pass 查看器，14 视图） | ✅（cluster map SSBO + usampler2D 组合采样） | ✅（MSL 源直编，constexpr sampler） | ⚠️ 降级边界：字节码加载失败 → 模块禁用、正常出图（`LoadPassDebugBytecode` 仅 Metal/Vulkan 分支） | Modules/PassDebugViewModule.* / PassDebugView.h |
| 着色器解码（ID 哈希/深度线性化/ACES） | ✅ PassDebugView.frag.spv | ✅ PassDebugView.metal | — | EngineTest/shaders / Vulkan/shaders/Debug |
| WRC 探针球 overlay（64 方向 radiance 球） | ✅ ProbeSphereWRC.frag.spv | ✅ ProbeSphere.metal `probe_sphere_wrc_fs` | — | DebugProbeSphereModule（Source=DDGI\|WRC 管线族） |
