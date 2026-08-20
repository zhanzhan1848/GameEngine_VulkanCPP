# RHI 双后端功能对等计划（feat/vulkan-rhi-backend → dev 合并准备）

- 日期：2026-08-19
- 范围：**仅 RHI 层**（`Engine/Graphics/RHI/Core/` + `Platforms/Metal/` + `Platforms/Vulkan/`）
- 对比基线：dev 的 `Platforms/Metal/` 与 feat/vulkan-rhi-backend 的 `Platforms/Vulkan/`（两分支 Metal 平台代码自分化点 `6a58cd4` 起零改动，等效于同一 API 下两后端对比）
- 目标：补齐 Vulkan 后端相对 Metal 后端的全部功能缺口，使 feat/vulkan-rhi-backend 可安全合入 dev

---

## 0. 现状基线

Vulkan 后端已达到的状态（作为本计划的起点，不是差距）：

- 15 对平台文件与 Metal 一一对应；核心命令面（Draw/DrawIndexed/DrawIndirect/Dispatch/DispatchIndirect/Blit/Copy/Mipmap/Barrier/PushConstants/动态偏移描述符）完整
- 23 个测试二进制（≥50 用例）全部通过，**零 validation error**
- SimplePBR 与 Metal 参照帧 SSIM = 0.99533；ForwardRenderer 集成测试帧耗时与 Metal 持平（±5%）
- 时间戳查询、PipelineStatistics、几何/曲面细分接线、wireframe、跨平台 surface、PresentMode 协商已**反超** Metal

对等缺口集中在 7 项（见 §1），其中 3 项为合并阻塞（P0）。

---

## 1. 差距清单与优先级（功能路径总览）

| ID | 缺口 | 优先级 | 预估 | 一句话描述 |
|----|------|--------|------|-----------|
| F1 | DataFormat 映射补全 | **P0** | 0.5–1 天 | D16_UNorm 映射到 UNDEFINED（创建即失败）+ 16/32 位 norm/int 变体 + BC 变体缺失 |
| F2 | Swapchain 失效自动重建 | **P0** | 1 天 | OUT_OF_DATE/SUBOPTIMAL 只打日志直接 return false，窗口 resize 后渲染永久中断 |
| F3 | 纹理 updateData/Map 通道 | **P0** | 1–2 天 | `updateDataImpl` 恒返回 false，依赖该通道的上层资产路径静默失败 |
| F4 | StagingAllocator 上传队列 | P1 | 2–3 天 | 只有 Allocate；QueueBlit_*/EncodePendingBlits/FlushBlocking 四方法整体缺失（Phase 3 注释遗留） |
| F5 | Layered 渲染 | P1 | 2–3 天 | 无 renderTargetArrayLength 等价物，CSM/立方体阴影只能逐层渲染 |
| F6 | SetComputeBytes >128B 回退 | P1 | 0.5 天 | push constant 模拟超 128B 未走 UBO 回退（Phase 6 注释遗留） |
| F7 | Secondary CommandBuffer / 并行录制 | P1 | 3–5 天 | Metal 的 BeginParallelRenderPass + CreateSecondaryCommandBuffer 无对应 |
| F8 | Immutable Sampler | P2 | 0.5 天 | 恒 nullptr（"Phase 4 暂不支持"） |
| F9 | Cube Storage Image | P2 | 1 天 | 有意限制，引擎侧已用 2D-array 绕过；补齐或正式记录为不对等 |
| F10 | Linux XCB Surface | P2 | 0.5 天 | connection 硬编码 nullptr，Linux 上屏路径断；macOS 为主开发平台，不阻塞合并 |

**明确不在本计划范围**（合并不要求）：

- 双向对等的 Metal 反向项：Metal 缺 BC1–BC7 全系、24 位 RGB、BGRA8 非 UNorm 变体、RGBA32_sRGB；Metal 的裸 `WriteTimestamp` 在 Apple Silicon 上是 no-op。若引擎资产管线未来采用 BC 压缩需另行立项。
- 双方共同缺失（无对等问题）：MSAA、bindless/`VK_EXT_descriptor_indexing`、ray tracing、mesh shader、VRS、debug label、`DrawIndexedIndirect`/`ResolveTexture`/`CopyTextureToTexture`、timeline semaphore。这些属于共同天花板，列入 §4 合并后 backlog。
- 4b-T4 引擎集成（ForwardRenderer 的 Vulkan 路径接线）：属引擎层而非 RHI 层，以 README §10 为准另行推进。

---

## 2. 逐项实现方案

通用约定（每项都适用）：

- 所有路径/行号以 feat/vulkan-rhi-backend 为准
- 每项交付 = 代码 + 单元测试 + 视觉 parity 测试 + 全量回归
- **零 validation error 是所有项的硬门槛**（见 `Platforms/Vulkan/README.md` §4）
- 视觉验收统一复用 `EngineTest/Utils/ImageCompare`（Wang-Bovik SSIM、RGBA16F→RGBA8、depth→RGBA8、FlipYInPlace）与 `EngineTest/Assets/ReferenceImages/` 参照帧机制；参照帧生成方式沿用 `TestMetalSimplePBR.cpp` 模式（Metal 离屏渲染 → 提交 PNG 入库）

### F1 — DataFormat 映射补全（P0）

**现状**：`Platforms/Vulkan/VulkanMath.h:30 ToVkFormat` 约 45 个 case；`Platforms/Metal/MetalDevice.cpp:130 ToMTLPixelFormat` 约 52 个 case。

**精确缺口表**（左列 = `RHITypes.h DataFormat` 枚举值）：

| 分组 | 缺失枚举值 | 目标 VkFormat | 备注 |
|------|-----------|--------------|------|
| 深度 | `D16_UNorm` | `VK_FORMAT_D16_UNORM` | **最高优先**：当前返回 UNDEFINED，vkCreateImage 直接报错 |
| R16 | `R16_UNorm` / `R16_SNorm` / `R16_SInt` | `R16_UNORM` / `R16_SNORM` / `R16_SINT` | |
| RG16 | `RG16_UNorm` / `RG16_SNorm` / `RG16_SInt` | `R16G16_UNORM` / `R16G16_SNORM` / `R16G16_SINT` | |
| RGBA16 | `RGBA16_UNorm` / `RGBA16_SNorm` / `RGBA16_UInt` / `RGBA16_SInt` | `R16G16B16A16_UNORM` / `_SNORM` / `_UINT` / `_SINT` | |
| R32 | `R32_UNorm` / `R32_SNorm` / `R32_SInt` | `_SFLOAT` / `_SFLOAT` / `R32_SINT` | UNorm/SNorm 按 Metal 先例 fallback 到 Float（Metal `R32_UNorm→R32Float`） |
| RG32 | `RG32_UNorm` / `RG32_SNorm` / `RG32_UInt` / `RG32_SInt` | 同上规则（`R32G32_*`） | |
| RGB32 | `RGB32_UNorm` / `RGB32_SNorm` / `RGB32_UInt` / `RGB32_SInt` | 同上规则（`R32G32B32_*`） | 注意 RGB32 是 3 通道 12 字节，Vulkan 支持但部分驱动对 storage/blit 受限，仅作采样/RT |
| RGBA32 | `RGBA32_UNorm` / `RGBA32_SNorm` / `RGBA32_UInt` / `RGBA32_SInt` | 同上规则（`R32G32B32A32_*`） | |
| BC 压缩 | `BC1_sRGB` `BC2_UNorm` `BC2_sRGB` `BC3_sRGB` `BC4_UNorm` `BC4_SNorm` `BC5_SNorm` `BC6H_UF16` `BC6H_SF16` `BC7_sRGB` | 对应 `VK_FORMAT_BC*_BLOCK` | Metal 侧无 BC（反向缺口），macOS 路径上 BC 纹理本来就不可用，此项为 Linux/Win 对齐 |

**实现路径**：

1. `VulkanMath.h ToVkFormat` 按上表补 case；映射后逐项跑 `vkGetPhysicalDeviceFormatProperties` 确认 optimalTilingFeatures 含 `SAMPLED_BIT`（RT 用途另查 `COLOR_ATTACHMENT_BIT`），不支持的组合在 `VulkanTexture` 创建处打 warn 并降级/失败——不静默。
2. `VulkanTexture::GetAspectMask()` 无需改动（depth 组已按格式判断）。
3. 新增测试二进制 `TestVulkanFormatParity`（见验收）。
4. 同步在 `VulkanMath.h` 头注释里删掉"仅映射当前测试子集"的过期说明。

**功能验收**：

- [ ] `ToVkFormat` 对 `RHITypes.h` 全部 79 个枚举值除双方共同不支持项（`RGBA32_sRGB` 两边都缺、Metal-only 反向项除外）外无 UNDEFINED 返回；用一个静态遍历单测锁死
- [ ] 每个新增格式：创建 64×64 采样纹理 + 创建 RT 纹理（用途允许时）均成功且零 validation error
- [ ] D16_UNorm 深度纹理可作为 DepthPrePass 深度附件完整跑通

**视觉验收**：

- [ ] `TestVulkanFormatParity::RenderGradientPerFormat`：每个新增颜色格式渲染同一渐变+几何场景到该格式 RT → readback → RGBA8 → 与 Metal 同场景参照 PNG（`ReferenceImages/P4c-F1/format_<name>.png`）SSIM ≥ **0.98**
- [ ] `TestVulkanFormatParity::Depth16Parity`：D16 深度预 pass → depth readback 经 ImageCompare 的 depth→RGBA8 转换 → 与 Metal D16 参照 SSIM ≥ **0.98**
- [ ] UInt/SInt 格式做**像素精确**断言（roundtrip 整数值逐一相等，不经 SSIM）
- [ ] 全部产物 PNG 提交入库，测试在参照帧缺失时 skip 而非 fail（沿用 SimplePBR 约定）

### F2 — Swapchain 失效自动重建（P0）

**现状**：`VulkanSwapChain.cpp:298`（acquire）与 `:332`（present）对 `VK_ERROR_OUT_OF_DATE_KHR / VK_SUBOPTIMAL_KHR` 仅 `std::cerr` 后放弃；显式 `Resize()` 能重建 swapchain，但窗口拖拽/DPI 变化触发的隐式失效没有恢复路径——一旦发生渲染永久中断。

**实现路径**：

1. `VulkanSwapChain` 增加私有 `Recreate()`：`vkDeviceWaitIdle` → 释放旧 backbuffer 包装纹理（经 GC 延迟销毁，不能立刻 destroy，`RenderSystem` 可能仍持有句柄）→ 重新查询 surface capability → 重建 swapchain + backbuffer 包装 → 更新对外尺寸。
2. `AcquireNextImage`：遇 `VK_ERROR_OUT_OF_DATE_KHR` → `Recreate()` → **循环重试 acquire（上限 8 次）**；遇 `VK_SUBOPTIMAL_KHR` → 本次放行（仍可呈现），置 `needsRecreate_` 标志在下一帧 `BeginFrame` 时重建。
3. `Present`：遇两者 → 置 `needsRecreate_`，下一帧重建；`VK_ERROR_SURFACE_LOST_KHR` → 销毁并重建 `VkSurfaceKHR`（保留原始窗口指针）。
4. 重建后旧 image 必须处理 layout：新 swapchain image 初始为 UNDEFINED，首个 acquire 后的 pass 需允许 discard load（现有 `BeginRenderPass` 已按 UNDEFINED 处理，验证即可）。
5. `TestVulkanSwapChain` 扩展（见验收）。

**功能验收**：

- [ ] acquire/present 任一环节返回 OUT_OF_DATE 后，无需调用方干预，下一帧自动恢复呈现
- [ ] 重建期间不产生 use-after-free validation error（GC 延迟销毁生效）
- [ ] 现有 3 个 `TestVulkanSwapChain` 用例不回归

**视觉验收**：

- [ ] `TestVulkanSwapChain::ResizeRecovery`：开窗渲染动画渐变 → 连续触发 10 次尺寸变化（每次间隔 ≥10 帧）→ 每次重建后截取一帧，断言：帧非全黑/全白（直方图方差 > 阈值）、内容与同帧号的无 resize 基线渲染 SSIM ≥ **0.95**（同分辨率离屏 RT 渲染 + blit 到 backbuffer 的对照路径）
- [ ] CI 无显示环境时该用例自动 skip，并保留**人工验收清单**入库：本机开窗手动拖拽 resize 20 次 + 全屏切换 2 次，目视无黑帧/撕裂/卡死，截图存 `ReferenceImages/P4c-F2/manual/`

### F3 — 纹理 updateData / Map 通道（P0）

**现状**：`VulkanTexture.h:107-109` — `mapImpl` 返回 nullptr、`updateDataImpl` 返回 false。Metal 侧 `MetalTexture.cpp:331-347` 有完整实现（ReplaceRegion 类路径）。

**实现路径**：

1. `updateDataImpl(data, size, offset)`：设备级实现——从 `VulkanStagingAllocator::Allocate` 取 CPU 指针 → memcpy → 立即模式（无帧上下文，资产加载期）走一次性 transfer cmdbuf + fence 等待；帧内模式（有活跃 cmdbuf）走 `QueueBlit_Texture`（依赖 F4，先落立即模式）。需要 `TextureDesc` 几何信息计算 bytesPerRow/bytesPerImage（`GetTextureDesc()` 已提供）。
2. `mapImpl/unmapImpl`：限定语义——仅对 `GPUMemoryUsage::Staging/Readback` 的纹理（VMA HOST_VISIBLE）返回持久映射指针；DEVICE_LOCAL 纹理 map 返回 nullptr 并打一次 warn（与 Metal 行为差异写入 README 限制表：Metal ReplaceRegion 本质也是内核 staging，Vulkan 暴露为 updateData 而非 map）。
3. 更新后布局：`updateDataImpl` 结束时把 texture 置回 `currentLayout_`（copy 后 barrier，参照 `CopyBufferToTexture` 调用方契约——这次由 RHI 内部负责，不再要求调用方手动 barrier，与 Metal 语义对齐）。
4. 新增 `TestVulkanTextureUpdate`。

**功能验收**：

- [ ] 整张与子矩形（subrect offset）更新均生效；连续两次部分更新无互踩
- [ ] updateData 后直接采样，无 validation error（布局/屏障由 RHI 内部闭合）
- [ ] DEVICE_LOCAL 纹理 map 返回 nullptr + 一次性 warn，不 crash

**视觉验收**：

- [ ] `TestVulkanTextureUpdate::CheckerboardExact`：8×8 棋盘格纹理 updateData 上传 → 全屏采样绘制 → readback 与 Metal ReplaceRegion 参照**像素精确**（max-abs-diff ≤ 1/255；确定性内容不用 SSIM）
- [ ] `TestVulkanTextureUpdate::ProgressiveSubrect`：分 16 次子矩形更新拼出渐变图 → 最终帧与一次性整图更新帧 SSIM = **1.0**（同后端自参照）
- [ ] 与 Metal 参照帧 SSIM ≥ **0.98**，参照 PNG 入库

### F4 — StagingAllocator 上传队列（P1）

**现状**：`VulkanStagingAllocator.h` 明注 "Phase 2 范围：只实现 Allocate"；`QueueBlit_Buffer` / `QueueBlit_Texture` / `EncodePendingBlits` / `FlushBlocking` 四方法整体缺失。Metal 侧 `MetalStagingAllocator.h` 为完整参照（含同步论证注释）。

**实现路径**：

1. 头文件补 `PendingBlit` 结构 + 四方法声明，签名对齐 Metal（`VkBuffer/VkImage` 替换 MTL 类型；纹理参数含 mipLevel/slice/origin/extent/bytesPerRow/bytesPerImage）。
2. `QueueBlit_Buffer` → `vkCmdCopyBuffer`；`QueueBlit_Texture` → `vkCmdCopyBufferToImage`（布局转换到 TRANSFER_DST_OPTIMAL 由 Encode 时统一处理）。
3. `EncodePendingBlits(VkCommandBuffer)`：帧首调用——对涉及的纹理先统一 barrier 到 TRANSFER_DST，编码全部拷贝，再 barrier 回 `currentLayout_`（或 SHADER_READ_ONLY），清空队列。语义对齐 Metal 的"单 blit encoder + 用户 pass 之前"。
4. `FlushBlocking()`：立即模式——一次性 cmdbuf + fence 等待（复用 F3 的立即传输路径）。
5. 接线：`RenderSystem::BeginFrame`（或 `VulkanCommandBuffer::Begin`）处调用 `EncodePendingBlits`；`VulkanBuffer/VulkanTexture` 慢路径改走队列化上传（消除逐次 fence 等待）。
6. overflow 处理：pool 耗尽时 fallback 到 FlushBlocking 一次性分配并在 README 记录（Metal 侧同款 `overflow=true` 约定）。

**功能验收**：

- [ ] 300 帧连续 staged 上传（每帧 1–2 个子矩形）零 validation error、零泄漏（`TestVulkanStress` 模式的 churn 检查）
- [ ] 单帧 >16MB 上传触发 overflow fallback 仍正确
- [ ] FlushBlocking 在无帧上下文时语义正确（资产加载期调用）

**视觉验收**：

- [ ] `TestVulkanStagingUpload::RollingGradient300Frames`：滚动渐变每帧经 staged 通道更新 → 第 300 帧 readback 与 CPU 同步计算的期望帧 max-abs-diff ≤ **2/255**
- [ ] 与 Metal staged 路径参照帧 SSIM ≥ **0.95**
- [ ] 第 1 帧 vs 第 300 帧的"未更新区域"像素精确一致（验证无 pool 踩踏）

### F5 — Layered 渲染（P1）

**现状**：Metal 消费 `RenderPassDesc::renderTargetArrayLength`（`MetalCommandBuffer.cpp:390` `setRenderTargetArrayLength`）；Vulkan `BeginRenderPass` 只按 `GetLayerView(arrayLayer)` 绑单层视图（`VulkanCommandBuffer.cpp:778,815`）。

**实现路径**：

1. `VulkanCommandBuffer::BeginRenderPass`：`desc` 带 `renderTargetArrayLength > 0` 时，framebuffer 的 attachment 视图改绑 **2D_ARRAY 视图**（`VulkanTexture` 增加 `GetArrayView()`，lazy 缓存，复用 `GetLayerView` 的模式）。
2. 管线无需改动（多层渲染走普通 2D_ARRAY RT + shader 内 `gl_Layer`）；`GraphicsPipelineDesc` 无需扩展。
3. `VulkanRenderPass` 的 desc hash 已含 attachment 格式，无需变更；确认 depth attachment 同样走 array 视图。
4. 新增 `TestVulkanLayeredRendering`。
5. 依赖：F1（D32 array、RGBA16 array 格式映射）先行。

**功能验收**：

- [ ] 单 pass 渲染到 4 层 2D_ARRAY RT，逐层 readback 内容正确
- [ ] 零 validation error（重点： framebuffer 与 pipeline 的 view 类型一致）

**视觉验收**：

- [ ] `TestVulkanLayeredRendering::CascadeSelfParity`：同一 4-cascade CSM 场景，单 pass layered 渲染 vs 4 次单层渲染（同后端自参照），逐层 SSIM ≥ **0.99**
- [ ] `TestVulkanLayeredRendering::LayerIsolation`：每层绘制独有图案（层 i 仅层 i 出现），逐层断言其他层无该图案（像素级：其他层对应区域与背景精确相等）
- [ ] 与 Metal `setRenderTargetArrayLength` 参照帧逐层 SSIM ≥ **0.95**，参照 PNG 入库

### F6 — SetComputeBytes >128B 回退（P1）

**现状**：`VulkanCommandBuffer.cpp:1067-1079` push constant 模拟（`index*16` 分槽），注释标注超 `maxPushConstantsSize`（通常 128B）需走 UBO 为 Phase 6。

**实现路径**：

1. 设备级维护一个 per-frame ring 的隐式 UBO（复用 StagingAllocator，依赖 F4）+ 统一的 trivial descriptor set layout（一个 `UNIFORM_BUFFER` binding，compute stage）。
2. `SetComputeBytes` 判定 `index*16 + size > maxPushConstantsSize_`（Initialize 时从设备属性查询）→ 改写隐式 UBO 并绑定该 descriptor set（binding 号用布局约定，写入 `RHIShaderCommon` 文档）；shader 侧以 `layout(set=0, binding=N)` 读取——需要配套 GLSL 约定说明，不改 Metal 行为（Metal 继续 `setBytes`）。
3. 新增 `TestVulkanComputeBytesLarge`。

**功能验收**：

- [ ] ≤128B 路径行为不变（现有 `TestVulkanPushConstants` 不回归）
- [ ] >128B（测试 256B、512B 两档）经 UBO 路径零 validation error

**视觉验收**：

- [ ] `TestVulkanComputeBytesLarge`：512B 常量（含位于 128B 之后的扰动参数）驱动 compute 写 storage image → 与 CPU 参照逐像素 max-abs-diff ≤ **2/255**；与 Metal `setBytes` 参照 SSIM ≥ **0.95**

### F7 — Secondary CommandBuffer / 并行录制（P1）

**现状**：Metal `MetalCommandBuffer.cpp:495,605` 有 `BeginParallelRenderPass` + `CreateSecondaryCommandBuffer`（MTLParallelRenderCommandEncoder）；Vulkan 未暴露 secondary command buffer，也无 RHICommand 层的等价入口。

**实现路径**：

1. **Core 层**（新 API，双方语义统一）：
   - `RHICommand.h` 增 `virtual CommandBufferHandle BeginSecondaryCommandBuffer(const SecondaryCommandBufferDesc&)` 与 `virtual void ExecuteSecondaryCommandBuffers(u32 count, CommandBufferHandle* secondaries)`；`SecondaryCommandBufferDesc` 含继承的 render pass 目标（供 Vulkan `VkCommandBufferInheritanceInfo`），Metal 侧映射为 parallel encoder 的子 encoder。
   - Metal 侧已有 parallel 路径，补齐新虚函数的对接即可。
2. **Vulkan 侧**：`VulkanCommandPool` 按 `VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT`（现有）分配 `VK_COMMAND_BUFFER_LEVEL_SECONDARY`；`Begin` 时带 `VkCommandBufferInheritanceInfo`（renderpass + subpass + framebuffer，兼容 framebuffers 二级缓冲的 `VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT`）；primary 的 `End` 前插入 `vkCmdExecuteCommands`。
3. 同步语义：secondary 提交前不拥有 fence，`WaitForCompletion` 仅对 primary 有意义——写入头文件契约。
4. GC：secondary cmdbuf 生命周期挂到 primary 的提交帧，随 primary 一起回收。
5. 新增 `TestVulkanSecondaryCommandBuffer`。
6. 依赖：F4（帧结构与 EncodePendingBlits 时序需先稳定）。

**功能验收**：

- [ ] N 个 secondary（N=1,4,16）经 primary 执行，渲染结果与直接录制一致，零 validation error
- [ ] secondary 中 BeginRenderPass 被拒绝或正确继承（按设计选定，写契约测试）
- [ ] GC 压力下（1000 次 create/execute/destroy）无泄漏（并入 `TestVulkanStress`）

**视觉验收**：

- [ ] `TestVulkanSecondaryCommandBuffer::SameCommandsExact`：同一组 32 个 draw 经 secondary 录制执行 vs 直接录制，同后端 SSIM = **1.0**
- [ ] 与 Metal parallel encoder 参照帧 SSIM ≥ **0.95**，参照 PNG 入库

### F8–F10（P2，处置方案）

| ID | 处置 |
|----|------|
| F8 Immutable Sampler | 0.5 天：`VulkanDescriptorSetLayout.cpp:67` 消费 `ImmutableSampler` 数组（`VkSampler` 于 layout 创建时绑定，pImmutableSamplers）。验收：单测断言绑定后 descriptor write 不再需要 sampler；视觉上与可变 sampler 渲染帧像素精确一致。**若排期不下，正式记录为"已知不对等"并写入 README 限制表即可合并** |
| F9 Cube Storage Image | 1 天：`VulkanTexture.cpp:350` 放开 cube + storage 组合（`VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT` 已支持，需补 storage view per-face 的 `VkImageViewType_2D` 别名）。引擎已有 2D-array 绕过，**推荐仅记录，不实现** |
| F10 Linux XCB | 0.5 天：`VulkanSurface_Linux.cpp:35` 从窗口句柄推导/由调用方传入 connection。macOS 为主平台，**不阻塞合并**；Linux 成为构建目标前必须完成 |

---

## 3. 实现过程路径

### 3.1 实施顺序（按依赖与风险排序）

```
F1 格式映射 ──→ F5 Layered（依赖 D32/RGBA16 array 格式）
F3 纹理 updateData ──→ F4 Staging 队列（共用立即传输路径）──→ F6 SetComputeBytes（复用 ring UBO）──→ F7 Secondary（依赖帧结构稳定）
F2 Swapchain 重建（独立，可并行）
```

建议分四个 PR/提交序列：

1. **PR-1（P0 收口）**：F1 → F2 → F3。合并阻塞项全部清零。
2. **PR-2**：F4（staging 队列，F6/F7 的地基）。
3. **PR-3**：F5 + F6（两项独立小中件）。
4. **PR-4**：F7（最大件，Core API 扩展，需要 Metal 侧同步对接新虚函数）。

F8–F10 在任意间隙插入或记录处置。

### 3.2 每项的标准工作流（沿用仓库 T-phase 惯例）

1. **实现**：代码 + 头文件契约注释（中文，标注 Phase/依赖关系，风格对齐现有 `T4.6.5 part XX` 注释）。
2. **单元测试**：功能验收清单逐条落为 `TestFramework` 用例（函数式，非 catch2），新增测试二进制注册进 CMake（`ENABLE_VULKAN=OFF` 时编为 no-op main）。
3. **视觉 parity**：
   - 先在 Metal 侧生成参照帧（嵌入式 MSL，离屏渲染 → PNG 提交 `EngineTest/Assets/ReferenceImages/P4c-<Fx>/`，CMake `POST_BUILD copy_directory` 拷贝到构建目录——沿用 `P4b-T2` 机制）；
   - Vulkan 测试渲染 → readback → 必要时 `FlipYInPlace` → SSIM/max-diff 断言；
   - 参照帧缺失（非 macOS 构建）时 skip 不 fail。
4. **回归**：全量 Vulkan 套件 + 全量 Metal 套件双绿（Metal 套件必须保持绿——本计划不允许任何 Metal 行为回退）；Debug 构建零 validation error。
5. **记录**：`Platforms/Vulkan/README.md` §10 表格更新该项状态；遇到的驱动/规范陷阱按仓库惯例写入 `memory/`。
6. **提交**：`feat(rhi/vulkan): F<x> <一句话>` 一个逻辑改动一 commit；Core API 扩展（F7）单独 commit 并同步 Metal 对接。

### 3.3 视觉验收基建补强（一次性，随 PR-1 落地）

- `ImageCompare` 增加 `MaxAbsDiff`（已存在则确认导出）作为 SSIM 之外的确定性断言原语。
- 参照帧目录约定：`ReferenceImages/P4c-F1/ … P4c-F7/`，文件名含格式/用例名。
- 建立**视觉回归报告**脚本：跑完套件输出 `build/VisualParityReport.md`（每用例 SSIM/max-diff/参照帧路径），合并 PR 附该报告。

---

## 4. 合并门槛（最终验收清单）

合并 feat/vulkan-rhi-backend → dev 前逐项打勾：

**功能完整性**

- [ ] F1、F2、F3（P0）全部完成并通过验收
- [ ] F4–F7（P1）全部完成，或每项有书面处置决策（完成/延后+理由）记入 README §10
- [ ] F8–F10 有处置记录（实现或"已知不对等"声明）

**测试与稳定性**

- [ ] 全量 Vulkan 套件（现有 23 二进制 + 新增 6–7 个）零 validation error 通过
- [ ] 全量 Metal 套件零回退（证明 Core 层改动无害）
- [ ] `ENABLE_VULKAN=OFF` 构建产物与合并前 bit-identical（Vulkan 代码完全惰性）
- [ ] `TestVulkanStress` churn 用例（含 F4/F7 新增压力项）无泄漏

**视觉验收（汇总）**

- [ ] §2 各项视觉验收全部通过，参照帧 PNG 全部入库
- [ ] `VisualParityReport.md` 附于合并 PR：P0 项 SSIM ≥ 0.98、P1 项 ≥ 0.95、确定性用例 max-abs-diff 达标（≤1/255 或 ≤2/255 按项）
- [ ] SimplePBR parity 不回退（SSIM ≥ 0.95，预期 ≈0.995）
- [ ] swapchain 人工验收清单执行完毕（resize 20 次 + 全屏 2 次，截图入库）

**性能**

- [ ] ForwardRenderer 集成测试帧耗时相对 Metal 偏差 ≤ ±5%（F4 落地后 staged 上传路径应优于现状）

**文档**

- [ ] `Platforms/Vulkan/README.md` §10 状态表、限制表更新至终态
- [ ] 本文档标记完成项

**合并后 backlog（记录，不在本次范围）**

- 双向对等：Metal 侧 BC 压缩格式缺失（资产管线采用 BC 前必须解决，候选方案 ASTC 转码）、Metal 裸 WriteTimestamp no-op
- 共同新增：MSAA、bindless（Linux/Win only）、`DrawIndexedIndirect`/`ResolveTexture`/`CopyTextureToTexture`、debug label（Core 已有枚举无实现）、timeline semaphore、ray tracing/mesh shader/VRS
- 4b-T4 引擎集成（ForwardRenderer Vulkan 路径、ShadowMapModule R8→D32、IBLPrecomputer SPIR-V 分支）
