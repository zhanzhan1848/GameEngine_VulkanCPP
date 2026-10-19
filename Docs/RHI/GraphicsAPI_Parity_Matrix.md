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

## 2026-10-01 SC 软件两层 BVH 候选边界

`PRIMAL_SC_GEOMETRY_FIRST=1` 默认关闭，仅 Vulkan SC tile radiosity 创建独立17-binding trace variant（默认13不变）；CPU BLAS复用现有BVH构建器，在mesh资产首次请求时缓存共享，实例TLAS由scene snapshot发布。四个几何SSBO三帧槽更新，显式Host→Compute及trace Compute→resolve Compute同步；无完整场景/上传失败/栈溢出不得认证天空。命中身份限制同实例缓存，0.05m严格表面一致校验，miss保持no-cache。Metal/Dawn保留现有算法，不创建该GPU候选；CPU数据结构跨端可编译，非GPU生产对等声明。独立实际射线1497/1497首命中在2e−5m内，GPU仿射变换合成7/7，但350/480视觉门禁FAIL，未迁移DDGI/SPGI/WRC或认证alpha/透射。详见per-mesh实施文档的2026-10-01后续段。

2026-10-01持久化补充：PBVH0001 CPU缓存保存mesh BLAS及完整实例TLAS/共享BLAS快照；little-endian＋source摘要＋checksum＋树/范围/数值校验，坏缓存重建、写失败保留有效内存树。默认`.primal-cache/geometry-bvh`，可用`PRIMAL_SC_GEOMETRY_CACHE_DIR`指定持久目录。SC/BVH共享geometry摘要，实例GPU镜像64B末8B使用既有padding保存摘要，无GPU binding/镜像尺寸变更；发布前SC实例身份核验。393BLAS＋1场景快照冷/热1497射线逐字节一致，389SC记录几何身份全对应；不等于所有texel覆盖。CPU缓存不要求后端光追扩展，GPU候选范围仍仅Vulkan SC tile；Metal默认路径保持。

2026-10-01 TriangleID补充：SCS1 v4每有效texel保存原始mesh三角形序号，48B页条目＋第五R32_UINT平面，v3拒绝需重烘焙。Vulkan初始化/静态合成binding6增加uint atlas SSBO（2048²为16MiB），分配纪元种子为0xffffffff，所有属性共享ptex最近邻采样；trace几何变体18绑定，resolve几何变体20绑定，32B ProbeRayHit及默认trace13/resolve18保持。ray TriangleID单独uint SSBO，显式compute写→compute读；身份atlas先初始化再合成使用ShaderRead|ShaderWrite依赖。同实例＋同原始TriangleID，3×3邻域且点误差≤.05m，禁止跨primitive取光。Metal/Dawn保留原capture/算法，没有此GPU身份消费；共享v4读取器/CPU构建可编译。

2026-10-01后续：Vulkan静态页depth pool按float元素数完整上传（旧错误仅上传1/4），不改wire或GPU binding；CPU helper四分区回归。CoverageCardBudget以32²最小页保留全部候选、按rank/面积精化、对齐页格消除混合货架空洞。Sponza2532/2532放置、2444静态合成，8436射线首命中独立一致、947 SC接受且≤.05m，350/480六ROI均PASS；覆盖策略在已显式启用几何首命中＋有效静态页的Vulkan路径默认，PRIMAL_SC_COVERAGE_PACK=0可回退，全局GEOMETRY_FIRST仍默认关。预算不满足则原分配，不宣称所有场景画质或完整primitive覆盖。Metal/Dawn不自动开启此分配；CPU布局/helper跨端编译，Metal65s有界回归正常。完整SC miss/局部或多层card仍待推进。

2026-10-01实际atlas审计/查询范围补充：Vulkan geometry resolve用0.05m投影的texel矩形替代固定3×3，仍保持同实例/原始TriangleID及完整三维距离校验；同8436射线947→955且无旧命中丢失，350/480门禁均PASS，GPUhelper13/13。只读atlas诊断在非页表模式下独立提交读回depth/uint身份/CPUcard，显式AllCommands→Transfer、Transfer→Host与depth CopySource往返，不改GPU资源/binding/ABI。Metal/Dawn算法保持，默认Metal65秒回归通过；实际atlas仍有重采样平面漂移/身份丢失，未声称完整SC覆盖。

2026-10-01静态页几何重采样：已启用Vulkan几何首命中的有效静态页在分配纪元按目标texel中心查询owner-only CPU BLAS，原始TriangleID严格匹配源footprint属性，缺首层属性保持无效；输出page_res=card_res，仍沿用静态合成7binding及全部既有同步，不增wire/ABI/资源。实际GPU atlas最大三角形距离4.881e−7m，旧4699个>0.05m漂移降为0；新8201射线独立首命中一致、977正确缓存，350/480门禁PASS。PRIMAL_SC_GEOMETRY_RESAMPLE=0回退；全局几何首命中仍默认关闭。CPU helper跨端编译，Metal/Dawn不运行该重建，Metal65s默认回归通过；密度/可见层/非静态身份仍有缺口。

2026-10-01世界密度预算实验：CPU-only最长世界投影边/当前resolution优先精化，全部card最低32²保留；PRIMAL_SC_DENSITY_PACK=1仅显式Vulkan coverage路径，默认继续rank/面积。Sponza同8201已认证ray几何可用候选977→1749，完整atlas三角形距离最大3.767e−5m；新6434实际ray独立GPU仅6432严格认证（浮点t误差/端点未知），350/480原始门禁虽PASS但比已推广方案地面退步，未推广。GPU shader/binding/同步及wire不变；Metal/Dawn默认不执行该策略，Metal65s回归通过。

2026-10-01同原始三角形插值：Vulkan geometry resolve在全部旧点样本拒绝后，才对同owner card内四个同TriangleID有效depth/radiance做凸组合；保持0.05m完整点距、跨primitive/分配边界/缺角拒绝。沿用params.ray_lookup bit16、既有20绑定及全部同步，不增加wire/ABI/资源。相同8201实际ray与GPU depth/ID/card镜像，正确SC977→1871、丢失0，独立GPU+float64首命中8201一致，GPUhelper25/25、几何8/8、350/480门禁PASS；PRIMAL_SC_TRIANGLE_INTERPOLATION=0回退，默认仅显式几何首命中＋有效静态页＋实际静态card时启用，全局几何首命中仍关。生产GPU细长三角形用补偿法线/平面t修复数值抵消，独立flattened proof用普通平面t并保留端点UNKNOWN，旧超差ray误差2.946e−5→1.059e−7m。Metal/Dawn不启动新GPU查询，Metal65s默认回归通过；默认350/480复验PASS，samples/atlas与显式候选逐字节一致。DDGI/SPGI/WRC未迁移，局部/多层身份仍有缺口。

2026-10-01同TriangleID线段：仅全部旧点/四点/三点候选拒绝后尝试同owner card内有效同原始ID两角点的有限线段凸组合，权重钳位[0,1]、退化/非有限拒绝、完整3D距离≤0.05m，禁止外推。既有params mask64，无新增资源/binding/barrier/wire/ABI；相同8190实际query＋三份GPU atlas正确SC2770→3013、+243无丢失，旧card/cacheT全保留，同输入独立首命中8190一致，350/480门禁PASS。GPUhelper55/55与几何8/8，默认350全部实际samples/atlas与显式候选一致，SEGMENT=0完整350回退8190输入恢复2770且全读回字节一致，Metal65s通过；地面缺口91→68；全局geometry-first仍关，仅既有Vulkan有效静态页插值路径默认，PRIMAL_SC_TRIANGLE_SEGMENT=0回退、INTERPOLATION=0关闭全部插值；Metal/Dawn/DDGI/SPGI/WRC保持原查询。

2026-10-01地面材质变体：离线CLI与参考场景共用RGBA8 sRGB→linear×scale→sRGB/u8量化函数，三摘要选择保留原页与0.5变体。SCS1仍v4，同几何/布局允许多个材质记录，无RHI绑定/资源/barrier变化，PBVH几何身份不变。正式scale=0.5下五卡全部绑定，静态2444→2449；共同8190实际输入正确缓存1955→2770（+815无丢失），新批次独立CPU/GPU首命中8190全一致、2770全部≤0.05m，350/480门禁PASS。GPU atlas所有有效身份距原三角形最大4.881e−7m，CPU页68/68；906地面hit余91覆盖缺口、584透射cloth仍拒绝。正式资产路径2帧复验card/ID atlas及静态depth一致，Metal65s回归通过；全局geometry-first仍关，未改变透明/透射契约或推广其它API查询。

2026-10-01三点凸包补充：仅全部点/四点候选拒绝后尝试3个有效同TriangleID角点的非负重心插值，凸包外/缺两角拒绝、未使用角点NaN不读取，完整0.05m校验保持；既有params bit32，无新增binding/资源/wire/同步。相同8201真实ray、GPU atlas镜像，正确SC1871→1959、旧bestCard/cacheT全部保留，独立首命中8201一致；GPUhelper37/37、几何8/8、350/480门禁PASS。PRIMAL_SC_TRIANGLE_SIMPLEX=0回退三点，INTERPOLATION=0关闭全部插值；仅既有Vulkan几何首命中＋有效静态页插值路径默认，全局geometry-first仍关。两帧readonly诊断证实参考场景floor纹理运行时×0.5导致材质摘要拒绝两owner的5静态card（906条真实hit）；诊断scale=1时2444→2449静态card，正式scale=0.5保持，不能用放宽身份匹配解决。默认350/480复验PASS且实际samples/atlas与显式候选一致，Metal65s回归通过；Metal/Dawn未启用新查询，透明/透射及动态capture原始身份仍未迁移。

2026-10-01扩展同TriangleID支持域：原严格2×2查询footprint不变，源支持扩大为owner矩形内裁剪的4×4，最多16点/120对，仅全部旧阶段拒绝后尝试同原始ID有限线段非负凸组合，完整3D≤0.05m，禁止外推。既有params mask128，无新资源/binding/barrier/wire/ABI；同8190实际query＋三份atlas正确SC3013→3112，+99无丢失，旧card/cacheT全保留，独立首命中8190一致，350/480门禁PASS；GPUhelper72/72、几何8/8，默认350全部读回与显式候选一致。WIDE_SEGMENT=0完整350回退全部读回与上一轮一致、恢复3013，Metal65s有界回归通过。地面缺口68→37；仅既有Vulkan几何首命中＋有效静态页插值路径默认，WIDE_SEGMENT=0回退扩展阶段、INTERPOLATION=0关闭全部插值，全局geometry-first仍关闭；其他API与DDGI/SPGI/WRC未迁移，额外GPU耗时未独立认证。

2026-10-01有界同TriangleID凸包：原严格2×2查询域保持，源支持裁剪owner内8×8（最多64点），x/y顺序收集同原始ID有限depth，整数单调链凸包，最多62个非负重心三角形及64条有限边，完整3D≤0.05m；仅全部旧阶段拒绝后运行，旧选择全部保留。既有params mask256，无新资源/binding/barrier/wire/ABI；同8190实际query＋三份atlas正确SC3112→3194，+82无丢失，独立首命中8190一致，独立凸组合支持验证通过，GPUhelper96/96、几何8/8、350/480门禁PASS。默认350全部samples/atlas与候选一致，HULL=0完整350回退全部读回与上一轮一致、恢复3112，Metal65s有界回归通过。地面37→28缺口；只在既有Vulkan几何首命中＋有效静态页插值路径默认，HULL=0单独回退、INTERPOLATION=0关闭全部插值，全局geometry-first仍关；其他API及DDGI/SPGI/WRC未迁移，额外GPU耗时未独立认证。

2026-10-01紧凑三角形支持索引：Vulkan geometry-first resolve新增只读binding20、布局20→21；普通Vulkan仍18、其他API原布局。CPU在atlas分配纪元从同分辨率正向对角变换静态页按(card,TriangleID)构建有界凸包索引，2–64顶点/组、最多15步二分、16MiB序列化上限，Sponza实际2,446,716B；沿用static composite已有Host→Compute屏障，GPU只读，无新GPU写者。消费前要求索引/分配/成功静态合成纪元一致，每点重新核对实际GPU ID/depth及owner域；仅所有旧查询拒绝后进行非负凸组合、完整3D≤0.05m。mask512，SCS1/PBVH/ABI不变；同8190实际输入及atlas正确SC3194→3236、+42无丢失，独立首命中8190一致，CPU77/77、GPU112/112及几何8/8、350/480门禁PASS。默认350全部读回与候选一致，SUPPORT=0完整350全部读回恢复3194，Metal65s有界回归通过；页表/WRC物理复用路径保守禁用该索引阶段。地面28→20缺口，整card现有同ID支持不再可恢复；仅既有Vulkan静态页路径默认，SUPPORT=0单独回退、INTERPOLATION=0关闭全部插值，全局geometry-first仍关。Metal/Dawn/DDGI/SPGI/WRC未迁移；构建和查询耗时未独立认证。

2026-10-02密度重验与补点诊断：仅既有Vulkan DENSITY_PACK=1显式实验，生产资源/binding/同步/wire/ABI未改；当前几何/材质/索引路径下新6111实际ray独立首命中全一致，但同原8190输入几何支持新增1088/丢202，地面906输入新增18/丢2，350/480相对当前地面明显退步，不推广。新同输入CPU凸包审计不认证radiance；3577个真实边界位置联合旧点、每query最多32点的CPU原型覆盖剩20/20，仅几何可行性，尚无离线属性、GPU光照或新RHI资源实现，不将其登记为生产后端能力。

2026-10-02 补点持久化：新增共享 CPU SCB1 codec，支持有界文件读取与真实局部位置/原始 TriangleID/属性的三摘要键控，SCS1 v4 不变。没有新增 GPU resource/binding/barrier 或任何后端 shader；Vulkan/Metal/D3D12/Dawn 的生产查询能力不变。97/97 CPU 格式检查与离线 22/22 不代表 GPU 补点光照或视觉门禁通过。

2026-10-02 实际补点资产加载：SurfaceCachePass 可选 SCB1 CPU 挂载和静态页三摘要关联已接通，换页/Shutdown 清空，Vulkan 真实地面 4 记录/11054 点加载通过，Metal 65 秒冒烟无故障。仍无 GPU 补点 buffer/binding/dispatch/光照，其他后端生产查询能力不变；350 帧旧样本、depth、TriangleID、card 四文件逐字节一致，CPU 20/20 补点支持不能算成新增 GPU 命中。

2026-10-02 补点检索诊断：新增 epoch/card/original TriangleID 的混合点 KD 索引及 opt-in Vulkan compute/readback 测试（40/40，真实 20/20 最近点选择 + CPU 三维凸组合支持）。缓冲显式 Host/Transfer→Compute→Transfer→Host，完成提交后读回。生产 SC 补点光照/查询仍未接入，Metal/D3D12/Dawn 不增加此 GPU 能力；不能将诊断结果当成生产覆盖或跨端对等。

2026-10-02 补点真实位置光照诊断：Vulkan 可选单次 private SSBO shadow/eval/readback（14438 点），共享当前 PCF/SDF/8-probe gather，位置仿射/法线逆转置由 CPU pack；显式上传→shadow→eval→transfer→host，完成提交后 Map。独立 CPU PCF 全一致、开启旧 probe feed 时 DDGI 误差3.899e−6，默认 feed 输出0。生产仍缺补点 radiosity/缓存刷新/lookup，其他后端无此 GPU 功能；20/20 几何支持不代表生产命中恢复。

2026-10-02 补点 radiosity 诊断：Vulkan opt-in 私有 trace/resolve SSBO（真实点每点32射线，最多16384点），复用两层BVH及旧同owner/原始TriangleID的SC查询；输出实际射线/首命中/贡献/计数和单帧incident/outgoing。显式Host/Transfer→Compute→Compute→Transfer→Host，提交完成后读回；14438点462016射线，float64首命中抽查2048一致，所有积分与天空数值核验。无生产atlas写入/新增命中/时间缓存；剔除无效点后原20缺口GPU最近点与CPU三维支持仍20/20。Metal/D3D12/Dawn未增加对应GPU能力；不能将诊断登记为生产对等。

2026-10-02 补点完整radiance/刷新诊断：Vulkan新增private compose pipeline与64B cache SSBO/readback，复用同帧base＋radio、当前weight和clamp后nodirect；三帧刷新含数百有效→无效转换已独立核对。显式compute→compute→transfer→host，提交后读回/CPU事务发布；帧内不WaitIdle（避免回收recording引用的RG临时资源）。current-frame cache的分配/几何/三摘要/变换内容键与失效钩子CPU共享，生产无新增binding或命中；Metal/D3D12/Dawn未增加对应GPU功能，多帧proof重建不等于常驻高效刷新能力。

2026-10-02 补点GPU光照查询诊断：Vulkan共享最近32＋有界凸包radiance helper，实际绑定同帧cache及原resolve的真实混合lighting/depth/IDs/cards、监督first-hit fixture，三帧20/20返回光照、每帧5类非法/过期query全拒绝；实际atlas读回独立重算误差1.229e−9。buffer shader→transfer→host，纹理copy显式转换后恢复，提交完成后Map；内容键不变的监督point/cache等资源与5管线复用，但更新之前上一目标已WaitIdle/dump。生产默认/绑定/命中不变，未迁移异步frame-slot生命周期与自动混合索引。Metal/D3D12/Dawn未增加此GPU查询能力，诊断不计入生产对等。

2026-10-02 自动混合索引/诊断三槽：Vulkan显式SC_BOUNDARY_AUTO_INDEX=1按实际静态合成数据和当前SCB1 bank构建有界常驻只读SSBO，fixture仅提供query。私有point/cache/五组管线描述符/index按既有DDGI frame%3隔离，连续六帧验证跨槽句柄互斥和同槽复用；全量58组24692点与GPU ID/depth/world核对，20/20 query和每帧5负例通过。上传到compute仍沿用原显式屏障，提交/WaitIdle后读回的监督机制未改；主resolve资源绑定和生产命中不变，无阻塞异步调度未接通。默认不构建新增CPU镜像；Metal65秒回归通过，其他API无新增辅助GPU消费能力。

2026-10-02 可选主resolve辅助消费：Vulkan SC_BOUNDARY_CONSUMER=1新增主geometry resolve变体，布局21→24，只读21/index、22/points、23/cache。仅当前目标完成compose、epoch/frame/geometry匹配时开启bit1024，主resolve顺序延至辅助compose之后，compute/transfer/host→compute屏障发布；辅助resolve不使用消费者变体，避免同帧自反馈。主查询保留旧选择，仅所有旧支持失败才按owner/原TriangleID和card朝向/域消费真实radiance。349/350监督帧实际原8190射线/首命中/atlas不变，3236→3256、+20无丢失，原20地面缺口恢复；默认仍关闭，仍依赖有界监督刷新/阻塞读回，不登记为连续异步生产或跨端对等能力。Metal共享代码65秒回归通过，其他API无新增GPU补点消费者。

同阶段视觉复查：消费者开启的350/480历史六ROI门禁FAIL，关闭消费者同配置也FAIL；开关ROI最大差0.1284/0.04971。大幅历史偏差在关闭消费者时已存在，具体来源未定位；保留FAIL，不把主resolve恢复20命中认证成当前视觉晋升或连续帧性能认证。

2026-10-02 视觉验收范围纠正：曝光配置及阶段baseline混用，不是新同步或后端能力变化。固定PRIMAL_TONE_ADAPT=0/EV=1.8546后，消费者350/480对原SC阶段control门禁PASS，原8190输入+20无丢失再次确认；与前阶段通过图像ROI差≤0.05584/0.20310。最早P0视觉门禁仍FAIL、前阶段图像对P0也FAIL，不能用SC阶段PASS替代P0目标。脚本拒绝缺来源/不匹配配置（INCOMPARABLE，退出2），未改渲染参数或阈值；持续无阻塞刷新仍未实现。

2026-10-02 可选连续补点刷新：Vulkan SC_BOUNDARY_CONTINUOUS=1自动索引/完整radiance刷新/主resolve消费。共享RenderSystem仅实际槽fence成功后发布内部完成序号，SC同槽新序号才复用Dynamic输入及描述符；超时/缺信号拒绝。普通帧不分配/复制补点Readback、不调用补点诊断WaitIdle；compose用compute→compute发布，指定snapshot帧才走transfer→host读回。无fixture/prefix六帧原8190输入仍新增20、旧命中无损；三槽互斥/复用与单snapshot数值审计通过。默认关闭，长期性能/全场景与其他API辅助GPU消费未认证；内部RHIDeviceBase布局改变要求重建全部宿主，无C导出变化；最终Metal65秒共享代码回归通过，连续模式350/480既有SC阶段视觉均PASS、最早P0仍FAIL。

2026-10-02 SC诊断计时/owner范围：Vulkan SC_BOUNDARY_PROFILE=1每SC帧槽独立12timestamp，仅完整packet＋宿主槽fence完成＋GetQueryPoolResults成功才读；失败拒绝、不信任旧RG两槽readOk=false。诊断query pool同步销毁前WaitIdle，默认不创建。PRIMAL_SC_OWNER_CARD_RANGES=1仅geometry-first且CPU验证lookup完整单owner分区时设bit2048；shader限制升序card扫描范围，保留owner/tri/face/距离所有规则，无新绑定/ABI/持久化格式。两组231帧同shader调试对照wall−67.1%，lookup/radiance快照及350真实ray/atlas等价；350/480既有SC阶段视觉PASS。最终Metal65秒共享代码回归通过；最早P0另存门禁仍FAIL。两开关默认关闭，其他API无新增GPU计时/范围能力，未认证全场景或Release预算。

2026-10-03 SC直接owner目录：Vulkan显式PRIMAL_SC_OWNER_CARD_DIRECTORY=1仅GPU lookup副本的既有_pad[2]记录owner→card span，配合已验证bit2048启用bit4096，稀疏/超界owner回退旧范围检索；无新descriptor/ABI/磁盘格式或同步原语。监督快照增加实际lookup表读回，仅显式snapshot执行，普通连续帧路径不变。2532活跃card/393行全量GPU目录核对、231帧开关数值等价、20缺口恢复及350/480 SC阶段视觉通过；最终Metal65秒共享代码回归通过；单轮wall改善约1.35%，默认关闭，其他API无新增GPU目录能力，最早P0与Release性能预算未闭合。


2026-10-03 SC完整card成员过滤/owner TriangleID mask：Vulkan可选bit8192/32768，二者默认关闭；内部binding20的既有SCH1只读buffer追加所有raw ID、complete标记、owner排序mask及footer，C ABI/SCS1/descriptor/同步原语不变，沿当前allocation/composite epoch发布、现有Host→Compute依赖。未知card、异常或过大owner范围保留逐card路径；不声明Metal/Dawn/D3D12实现这些GPU查询，Metal保持capture。全量GPU读回整数审计、严格8190旧选择与20缺口、125十光照快照、Debug同binary/SPIR-V ABBA见最新实现记录；最新SC213/213、GPU112/112＋8/8及Metal65秒已验证。阶段PASS不能替代最早P0/全覆盖/Release预算。过滤后phase控制流诊断仅临时使用既有oracle尾部，采集后完整移除，无新增生产读回或全局等待。

2026-10-03 两层BVH控制流/links试验：临时Vulkan当前主trace每37射线计数使用原ray-ID buffer尾部及显式shader→transfer→host监督读回，采集后完整移除；延迟links首轮同binary/SPIR-V未加速，已discard。当前3生产源/8相关SPIR-V恢复，无新descriptor、C ABI、PBVH/SCS1或同步能力，各API生产边界不变；新GPU112/112＋8/8、Metal65秒通过。节点/拒绝/峰值计数不计硬件访存、独占时间、全覆盖或缩栈证明。

2026-10-03 BVH非零方向分类：Vulkan可选PRIMAL_SC_BVH_NONZERO_AXIS=1默认关闭，仅既有trace params reserved w mask0x1（bit0）；方向有限/非零才用等价逐轴AABB数学，零/非有限保持原路径，64栈/三角形/identity顺序不变。无新descriptor、C ABI、PBVH/SCS1或barrier/readback/wait。Debug同binary/SPIR-V ABBA收益见实现记录，非Release认证；GPU13解析/回归查询两路径逐位一致，surface112/112、Metal65秒共享代码回归通过，其他API无新增GPU路径能力。SC阶段PASS不能替代最早P0/全覆盖。

2026-10-03 原源点支持/共享预算：BoundaryMaxLightPoints=131072统一点pack、mixed index、current cache与Vulkan radiosity准备，原16K三处与pack上限不一致已修复，group8192/index4M words/32ray每点/越界事务失败不变。输入未扩充时不增加分配；独立SCB1实验不改默认资产。无新descriptor/C ABI/SCB1/SCS1/barrier/普通readback/WaitIdle，三槽实际fence仍同路。新110979点/3551328射线数值及2048首命中抽查、GPU112＋13、Metal65秒通过；source350/480 SC阶段PASS/P0FAIL。源扩充单对慢104.270%，不是优化或Release认证；其他API无新增补点GPU消费能力。

## 补充：Lightmap 旁路消费链（2026-10-19）

| 能力 | Vulkan | Metal | Dawn/WebGPU | 说明 |
| :-- | :-- | :-- | :-- | :-- |
| Lightmap UV pre-pass / composite 替换间接光 | ✅ | ✅ | ❌ 降级（模块 inert，渲染图零变化） | 与 Lumen fusion 链未移植 Dawn 的现状一致；PRIMAL_LIGHTMAP=0 三端统一零影响开关 |
