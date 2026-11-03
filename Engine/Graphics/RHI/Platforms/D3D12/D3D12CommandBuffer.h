// Windows-only: requires ENABLE_D3D12 (_WIN32)
//
// D3D12CommandBuffer.h — 命令录制（M1 F1-6）。
//
// 形态：ID3D12GraphicsCommandList（DIRECT）+ ID3D12CommandAllocator 池
// （帧槽轮转，fence 完成值 reset 判定）：
//   - Begin：轮转至下一 allocator；其上次消费提交的 fence 目标值已完成
//     （D3D12FencePool::GetCompletedValue >= lastUseTarget）才 Reset 复用，
//     未完成则 CPU 等待（防御旁路提交顺序）。之后 Reset(allocator)、
//     SetDescriptorHeaps（两个 shader-visible 堆）、编码上传环挂起拷贝。
//   - barrier：RHI InsertBarrier → 设备级 D3D12StateTracker::Transition
//     产出 op 列表 → 翻译成 D3D12_RESOURCE_BARRIER[] 一次 ResourceBarrier
//     下发；buffer 目标跳过（COMMON→任意状态 promotion + 提交边界衰减，
//     零显式 barrier——见 D3D12Buffer.h 文件头）。
//   - MemoryBarrier（全局障碍）实现策略（经典 barrier 模型的近似，M5+
//     Enhanced Barriers 换精确 SYNC/ACCESS）：src 访问含任一写位 → 发一条
//     pResource=nullptr 的全局 UAV barrier（D3D12 规范语义=等待全部在飞
//     UAV 读写；覆盖 compute→compute WAW 与 WAR）；纯读依赖 → no-op
//     （经典模型里读依赖由状态转换携带的执行/内存序保证）。
//   - bundle：不使用（M1 阶段文档 NOT-IMPLEMENTED 登记）。
//   - BlitTexture：M7 F7-3 起支持同格式 1:1 拷贝（CopyTextureRegion 直译，
//     整资源 CopySource/CopyDest 收敛模型与 Vulkan BlitTexture 同构）；
//     格式转换与缩放 blit 显式 NOT-IMPLEMENTED（登记）。
//   - GenerateMipmaps：维持 NOT-IMPLEMENTED——M5 工具 shader（M5Mipmap.comp，
//     整数 box）已交付，接线（cso 嵌入 + 逐 mip dispatch + UAV barrier）
//     按 M7 移交协议登记至 Windows 集成会话（runbook §7）。
//
// 视口契约（阶段文档 §7.2-1）：SetViewport 以负高度 {0, H, W, -H} 下发——
// D3D12 官方 Y 翻转手段；使原始帧与 Vulkan 原始读回同向（共享对比免
// FlipYInPlace）。绕序翻转由 PSO 的 cull 取反补偿（D3D12Pipeline.h）。
// Scissor 不翻转：它是屏幕空间矩形，与光栅化绕序无关。
#pragma once
#if defined(_WIN32) && defined(ENABLE_D3D12) && ENABLE_D3D12

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>

#include "../../Core/RHICommand.h"
#include "D3D12StateTracker.h"

namespace primal::graphics::rhi {

class D3D12Device;

namespace d3d12 {

// [R6/Windows] 真类在 rhi::d3d12（D3D12Pipeline.h）。此前这两个前置声明
// 误放在外层 rhi 命名空间，制造了与真类无关的影子不完整类型
// rhi::D3D12Pipeline / rhi::D3D12PipelineLayout，导致 CommandBuffer 成员
// boundPipeline_/boundLayout_ 与 Device::GetPipeline 返回的
// rhi::d3d12::D3D12Pipeline* 之间 C2440/C2027 双向断裂。
class D3D12Pipeline;
class D3D12PipelineLayout;

class D3D12Texture;

class D3D12CommandBufferImpl : public RHICommandBuffer {
    friend class primal::graphics::rhi::D3D12Device;
public:
    D3D12CommandBufferImpl(D3D12Device& device, CommandQueueType type);
    D3D12CommandBufferImpl(D3D12CommandBufferImpl&& other) noexcept;
    D3D12CommandBufferImpl& operator=(D3D12CommandBufferImpl&& other) noexcept;
    D3D12CommandBufferImpl(const D3D12CommandBufferImpl&) = delete;
    D3D12CommandBufferImpl& operator=(const D3D12CommandBufferImpl&) = delete;
    virtual ~D3D12CommandBufferImpl();

    bool Initialize() override;

    ID3D12GraphicsCommandList* GetCommandList() const { return commandList_.Get(); }

    /// 设备 Submit（RenderSystem 路径）回执：登记本帧 allocator 的 fence
    /// 护栏与 waitForCompletionImpl 等待依据。
    void OnDeviceSubmit(u64 fenceTarget) {
        allocatorLastUseTarget_[currentAllocator_] = fenceTarget;
        lastExecuteTarget_ = fenceTarget;
    }

    // === 渲染命令 ===
    void BeginRenderPass(const RenderPassDesc& desc) override;
    void BeginRenderPass(RenderPassHandle renderPass) override;
    void EndRenderPass() override;
    void SetViewport(const ViewportDesc& viewport) override;
    void SetScissor(const Rect& scissor) override;
    void BindGraphicsPipeline(PipelineHandle pipeline) override;
    void BindVertexBuffers(u32 firstSlot, u32 slotCount,
                           const ResourceHandle* buffers, const u64* offsets) override;
    void BindIndexBuffer(ResourceHandle buffer, DataFormat format, u64 offset = 0) override;
    void BindDescriptorSets(PipelineBindPoint bindPoint, PipelineLayoutHandle layout,
                            u32 firstSet, u32 setCount,
                            const DescriptorSetHandle* descriptorSets,
                            u32 dynamicOffsetCount, const u32* dynamicOffsets) override;
    void PushConstants(PipelineLayoutHandle layout, ShaderStage stageFlags,
                       u32 offset, u32 size, const void* pValues) override;
    void SetComputeBytes(u32 index, const void* data, u32 size) override;
    void WriteTimestamp(QueryPoolHandle queryPool, u32 queryIndex) override;
    void Draw(u32 vertexCount, u32 startVertex = 0,
              u32 instanceCount = 1, u32 startInstance = 0) override;
    void DrawIndexed(u32 indexCount, u32 startIndex = 0, u32 baseVertex = 0,
                     u32 instanceCount = 1, u32 startInstance = 0) override;
    void DrawIndirect(ResourceHandle buffer, u64 offset = 0, u32 drawCount = 1) override;
    // M5 F5-2：ExecuteIndirect(DRAW_INDEXED CommandSignature, 20B Vk 布局)。
    void DrawIndexedIndirect(ResourceHandle buffer, u64 offset = 0,
                             u32 drawCount = 1) override;
    void BindComputePipeline(PipelineHandle pipeline) override;
    void Dispatch(u32 groupCountX, u32 groupCountY, u32 groupCountZ) override;
    void DispatchIndirect(ResourceHandle buffer, u64 offset = 0) override;
    void MemoryBarrier(PipelineStage srcStageMask, PipelineStage dstStageMask,
                       AccessFlag srcAccessMask, AccessFlag dstAccessMask) override;

    // === 资源操作命令 ===
    void CopyBuffer(ResourceHandle src, ResourceHandle dst,
                    u64 srcOffset = 0, u64 dstOffset = 0, u64 size = 0) override;
    void CopyBufferToTexture(ResourceHandle srcBuffer, ResourceHandle dstTexture,
                             const BufferTextureCopyRegion* regions, u32 regionCount) override;
    void CopyTextureToBuffer(ResourceHandle srcTexture, ResourceHandle dstBuffer,
                             const BufferTextureCopyRegion* regions, u32 regionCount) override;
    void BlitTexture(ResourceHandle src, ResourceHandle dst,
                     const TextureBlitRegion* regions, u32 regionCount,
                     FilterMode filter) override;
    void GenerateMipmaps(ResourceHandle texture) override;
    void InsertBarrier(const ResourceBarrier* barriers, u32 barrierCount) override;

protected:
    void destroyImpl() override;
    bool resetImpl() override;
    bool beginImpl() override;
    bool endImpl() override;
    bool submitImpl(u32 waitFlags) override;
    bool waitForCompletionImpl() override;

private:
    /// tracker op → D3D12_RESOURCE_BARRIER[]（一次下发）；resource 为该 op 的
    /// 原生资源指针。
    void EmitTrackerOps(const utl::vector<D3D12ResourceBarrierOp>& ops, ID3D12Resource* resource);
    /// 纹理 → target 的隐式转换（拷贝路径用；tracker 驱动）。
    void TransitionTexture(D3D12Texture* texture, ResourceHandle handle,
                           ResourceState target, u32 subresource);
    /// 帧首一次 SetDescriptorHeaps。
    void EnsureDescriptorHeapsBound();

    static constexpr u32 kAllocatorSlots = MAX_FRAMES_IN_FLIGHT + 1;  // 4 池轮转
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocators_[kAllocatorSlots];
    u64 allocatorLastUseTarget_[kAllocatorSlots] = {};
    u32 currentAllocator_ = 0;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList_;

    D3D12Pipeline* boundPipeline_ = nullptr;         // 非拥有
    D3D12PipelineLayout* boundLayout_ = nullptr;     // 非拥有
    bool descriptorHeapsBound_ = false;
    u64 lastExecuteTarget_ = 0;                      // submitImpl/waitForSync 依据
    bool warnedDynamicOffsets_ = false;
    bool warnedComputeBytes_ = false;
    bool isRecording_ = false;                       // 设备 IsFrameRecording 镜像
};

} // namespace primal::graphics::rhi::d3d12
} // namespace primal::graphics::rhi

#endif // defined(_WIN32) && defined(ENABLE_D3D12) && ENABLE_D3D12
