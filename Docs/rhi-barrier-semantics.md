# RHI Barrier / 同步语义（三端）— T1.6 / R5

**Date**: 2026-09-08
**对象**: `RHICommandBuffer::MemoryBarrier`（全局内存障碍）、`InsertBarrier`（按资源障碍）、
`Submit` 的 fence 语义。对应审计 R5 与规划 §4-R5。

## 1. MemoryBarrier（全局，无资源参数）

| 后端 | 实现 | 语义要点 |
|---|---|---|
| **Vulkan** | `vkCmdPipelineBarrier`（1×VkMemoryBarrier，stage/access 直传） | 真·执行依赖 + 内存依赖。srcStage→dstStage 必须覆盖真实的读写阶段，否则 validation 报错或竞争。 |
| **Metal** | 有活跃 compute encoder 时：`[encoder memoryBarrierWithScope: Buffers\|Textures]`；**encoder 边界（end/切类型）本身是全量隐式屏障**；`currentEncoder_ == nullptr` 时 no-op（帧首/编码器之间的调用安全）。 | ① 隐式屏障只在**同一 encoder 内的 dispatch 间**由显式调用产生；跨 encoder 一律靠"结束当前 encoder"。② `dstStage` 含非 Compute 位（DrawIndirect/VertexInput/Transfer/Host）时实现会 end 当前 encoder（见 MetalCommandBuffer.cpp:1457 注释）。③ render/blit encoder 的依赖由 Metal 驱动在 encoder 边界隐式保证。 |
| **Dawn** | no-op | WebGPU 内部同步（dispatch 间自动屏障 + use-order 校验）。 |

**约定（写跨端代码时）**：
1. compute→compute 同 encoder 读后写：三端都调 `MemoryBarrier(ComputeShader→ComputeShader, ShaderWrite→ShaderRead)`。Metal 走 scope 屏障，Vulkan 走 pipelineBarrier，Dawn no-op。
2. 跨 encoder 类型切换：不要依赖 MemoryBarrier —— Metal 实现会 end encoder；显式 `InsertBarrier`（按资源状态）才是跨类型正确姿势。
3. Host 写 → GPU 读（Map 填充后）：`MemoryBarrier(Host→ComputeShader, HostWrite→ShaderRead)`。Metal 上 Map/Unmap 是 Shared 内存直写，进入同一 compute encoder 前的 scope 屏障覆盖它；若填充发生在 encoder 之外（常见），依靠 encoder 边界顺序。

## 2. InsertBarrier（按资源，状态转换）

- Vulkan：当前 `InsertBarrier` 实际仅处理 image barrier（oldLayout/newLayout、
  src/dst queueFamily 由资源跟踪层推断）；buffer 被跳过，尚未接入 buffer barrier。
  缓冲区依赖不能只靠该接口声明，显式同步诊断实例见 §4.4。
- Metal：`MTLCommandEncoder` 的 `useResource`/`memoryBarrier(scope)` 组合 +
  纹理 layout 跟踪（Metal 无显式 layout，追踪层保证状态机不产生错误假设）。
- Dawn：no-op（use-order 由 GPU 自动）。

**纪律**：同一物理资源在同一 encoder 内从"读"转"写"（或反向）必须 InsertBarrier；
RG 生成的 barrier 走同一路径。

## 3. Submit / fence

- `QueueSubmitInfo.signalFence`：三端语义一致 —— submit 完成时 fence 置位；
  `WaitForSync(fence, timeout)` CPU 等待。
- **Vulkan**：fence 在 submitImpl 内部 submit 前 reset —— 同一 fence 可循环使用
  （每帧 fence 池模式，见 RenderSystem frameFences_）。
- **Metal**：SharedEvent 计数语义 —— 每次 submit 递增目标值，WaitForSync 等待
  "最近一次"信号；重复使用安全但**不可跨帧比较数值**。
- **Dawn**：CPU 原子模拟（queue.onSubmittedWorkDone 的封装）。
- 全局 fence 锚点（resize/场景切换等 CPU 侧需要"GPU 全安静"的场景）：
  `RenderSystem::EndFrame` 后对 frameFences_[current] WaitForSync —— 不引入新的
  全局 API；rhi_readback_ring 的 attach_fence 提供按 slot 的细粒度版本。

## 4. 已知边界/坑

1. Metal encoder 末尾隐式同步**只覆盖 encoder 内的资源访问**；跨 encoder、跨
   command buffer 的 WAW/WAR 必须靠资源状态跟踪层（InsertBarrier）或帧 fence。
2. Vulkan 的 stage mask 不含 `Host` 时，Host 写不被 `MemoryBarrier` 覆盖 ——
   Host 写后必须用 `Host→<消费 stage>`。
3. Dawn 端 `MapBuffer(GPU-only storage)` 返回零页且 Unmap 回写（历史踩写事故，
   见 LumenDDGIPass C5 注释）—— CPU 读 GPU 数据一律走 Readback buffer +
   rhi_readback_ring。
4. 2026-09-28 实码核验：当前 `VulkanCommandBuffer::InsertBarrier` 仅生成纹理屏障，
   buffer 会跳过（实现注释仍标记 phase 5 待接）。缓冲区的 `UnorderedAccess→CopySource`
   声明不能替代真实依赖。同命令 compute 写后立即复制，必须显式
   `MemoryBarrier(ComputeShader→Transfer, ShaderWrite→TransferRead)`；读回再接
   `Transfer→Host, TransferWrite→HostRead`，并提交等待后 Map Readback。
   fence 只保证提交完成，不能修复提交内部已经抢先执行的 CopyBuffer。
   SC GPU 几何只读诊断使用这条显式路径；本轮未改通用 RHI buffer 屏障实现。
