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
//   - GenerateMipmaps（R13/T2）：Texture2D 单 slice + RTV 能力格式的
//     PS downsample 全 mip 链（每 mip：src SRV + dst RTV + 全屏三角形
//     双线性=2x2 box；shader=MipGen.hlsl 构建期嵌入，见 D3D12MipGen.h）。
//     BC 压缩/深度/TYPELESS 与非 2D 类型保持 NOT-IMPLEMENTED Error（登记）。
//
// 视口契约（阶段文档 §7.2-1，H2 后真实契约）：SetViewport 以负高度
// {0, H, W, -H} 下发——D3D12 官方 Y 翻转手段，使 NDC y=-1 映到 RT 第 0
// 行，与 Vulkan 正高度视口同映射、原始读回同向（共享对比免 FlipYInPlace）。
// PSO cull 直译（D3D12Pipeline::ToCullMode；FrontCounterClockwise=FALSE 与
// Vulkan frontFace=CW 同义，负视口已补偿 NDC Y 语义差）。Y 朝向由调用方
// 投影负责（harness F2：投影 y 行取反，RT 本身正立）。
// Scissor 不翻转：它是屏幕空间矩形，与光栅化绕序无关。
//
// 绑定状态机（R20-A，Windows 十九测 #709/#952）：
//   - D3D12 的根签名是命令列表状态（graphics/compute 各一槽，PSO 单槽
//     共用），且 commandList_->Reset（beginImpl）全清——不像 Vulkan 绑
//     管线自动携带 layout。本类维护与命令列表对齐的镜像：
//       bound{Graphics,Compute}{Pipeline,Layout}_ —— 各 bind point 最近
//       Bind*Pipeline 的 RHI 侧镜像（GenerateMipmaps 直设原生态时置空
//       该侧布局镜像——mipgen 非 RHI 管线，无映射布局）；
//       effectivePso_ / {graphics,compute}RootSigSet_ —— 命令列表三槽
//     （PSO、graphics RS、compute RS）当前值镜像；begin/reset 全清。
//   - 消费方前置自愈（Ensure{Graphics,Compute}State）：Draw/Dispatch/
//     ExecuteIndirect/PushConstants/BindDescriptorSets 下发前按镜像核对
//     对应槽位，缺/错则补设（SetPipelineState/Set*RootSignature）；
//     会话内从未 Bind* 的 bind point → Error 日志 + 跳过录制（不把必错
//     命令发给 debug layer）。
//   - PushConstants 契约对齐 Vulkan（VulkanCommandBuffer::PushConstants
//     L1538-1556）：layout 句柄优先解析（constantsParam 与 RS 同源自此，
//     保证参数索引恒与命令列表当前签名一致——#709 双子句根治），
//     Graphics/Compute 前缀按 stageFlags 路由（歧义回退单侧已绑侧）。
//   - R21-B（Windows 二十测黑帧回归）：三槽镜像从裸指针升级为"指针+命令
//     列表世代"（D3D12BindSlotMirror，本文件 Windows 守卫之外——纯 C++，
//     macOS 单测可直接包含建模验证）。幂等跳过必须指针与世代同时相等：
//     跨 Reset/Begin 后同指针不再被判"已绑定"，强制真实重下发——D3D12 的
//     SetPipelineState/Set{Graphics,Compute}RootSignature/SetDescriptorHeaps
//     均为命令列表状态、list Reset 全清；裸指针等值判定缺世代参与时，任何
//     "镜像未与 Reset 同步清空"的路径都会把空绑定误判为已绑定 → 黑帧
//     （本类缺陷的机械化防御，不再依赖各清空点手工不漏）。Ensure 失败
//     路径全部显式 Error（原 null RS/PSO 静默 return false = 无日志黑帧）。
#pragma once
#include <cstdint>

namespace primal::graphics::rhi {
namespace d3d12 {

/// 命令列表槽位绑定镜像小模型（R21-B；纯 C++，无 D3D12 类型——macOS 单测
/// 直接包含本文件验证生命周期语义，见 TestD3D12BindSlotMirror.cpp）。
/// 语义：ptr = 槽位当前值镜像；generation = ptr 置入时的命令列表世代。
/// Matches 要求指针与世代同时相等（且非空）——跨 Reset（世代递增）后
/// 同指针不再命中，幂等跳过被强制退化为真实重下发。
template <typename PtrT>
struct D3D12BindSlotMirror {
    PtrT ptr = nullptr;            ///< 槽位当前值镜像（非拥有）
    std::uint64_t generation = 0;  ///< ptr 置入时的命令列表世代（0 = 未置入）

    /// 幂等跳过判定：候选指针与当前世代一致且非空才视为"命令列表已持有"。
    bool Matches(const PtrT& candidate, std::uint64_t currentGeneration) const {
        return ptr != nullptr && ptr == candidate && generation == currentGeneration;
    }
    /// 记录一次真实下发后的槽位值。
    void Set(const PtrT& candidate, std::uint64_t currentGeneration) {
        ptr = candidate;
        generation = currentGeneration;
    }
    /// 列表 Reset/Begin：镜像与置入世代一起失效。
    void Invalidate() {
        ptr = nullptr;
        generation = 0;
    }
};

} // namespace d3d12
} // namespace primal::graphics::rhi

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
    /// Bind{Graphics,Compute}Pipeline 共用落点：更新对应 bind point 的
    /// RHI 镜像 + 幂等下发 PSO/根签名（槽位镜像指针比对跳过冗余 Set；
    /// 管线无根签名 = 创建期异常态，Error 显式记录不静默）。graphics 侧
    /// 附带 IASetPrimitiveTopology（原行为）。
    void ApplyPipelineBind(D3D12Pipeline* pipe);
    /// Dispatch 族前置：compute 槽（PSO+根签名）按镜像自愈；本会话从未
    /// BindComputePipeline → false（调用方跳过录制并已记 Error）。
    bool EnsureComputeState();
    /// Draw 族 / ExecuteIndirect(DRAW*) / PushConstants(graphics) /
    /// BindDescriptorSets(Graphics) 前置：graphics 槽同款自愈。
    bool EnsureGraphicsState();
    /// R21-B：绑定镜像全清 + listGeneration_ 递增——beginImpl/resetImpl/
    /// move/destroy 的单一落点（原三处手工清空列表的漂移面收口；任何未来
    /// 新增的 list Reset 路径必须经由本函数，世代递增保证旧镜像全部失效）。
    void ClearBindingMirrors();

    static constexpr u32 kAllocatorSlots = MAX_FRAMES_IN_FLIGHT + 1;  // 4 池轮转
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocators_[kAllocatorSlots];
    u64 allocatorLastUseTarget_[kAllocatorSlots] = {};
    u32 currentAllocator_ = 0;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList_;

    // === 绑定状态机镜像（R20-A；与命令列表三槽对齐，begin/reset 全清。
    //     R21-B：三槽镜像升级为 D3D12BindSlotMirror"指针+世代"——清空与
    //     幂等判定都以 listGeneration_ 为锚，单一函数收口）===
    D3D12Pipeline* boundGraphicsPipeline_ = nullptr;      // 非拥有
    D3D12PipelineLayout* boundGraphicsLayout_ = nullptr;  // 非拥有
    D3D12Pipeline* boundComputePipeline_ = nullptr;       // 非拥有
    D3D12PipelineLayout* boundComputeLayout_ = nullptr;   // 非拥有
    D3D12BindSlotMirror<ID3D12PipelineState*> effectivePso_;    // PSO 槽镜像（gfx/compute 共用）
    D3D12BindSlotMirror<ID3D12RootSignature*> graphicsRootSigSet_;  // graphics 根签名槽镜像
    D3D12BindSlotMirror<ID3D12RootSignature*> computeRootSigSet_;   // compute 根签名槽镜像

    bool descriptorHeapsBound_ = false;
    /// 命令列表世代：ClearBindingMirrors（Reset/Begin/move/destroy 落点）
    /// 每次递增；三槽镜像的幂等跳过必须在本世代内成立。
    std::uint64_t listGeneration_ = 0;
    u64 lastExecuteTarget_ = 0;                      // submitImpl/waitForSync 依据
    bool warnedDynamicOffsets_ = false;
    bool warnedComputeBytes_ = false;
    bool warnedPushNoLayout_ = false;      // PushConstants 无可用布局（一次性）
    bool warnedSetsNoLayout_ = false;      // BindDescriptorSets 目标侧无布局（一次性）
    bool warnedEnsureNullRs_ = false;      // Ensure* 撞空根签名管线（一次性，R21-B）
    bool warnedEnsureNullPso_ = false;     // Ensure* 撞空 PSO 管线（一次性，R21-B）
    bool warnedPushRsDiverge_ = false;     // PushConstants 布局签名与槽位分歧（一次性，R21-B）
    bool isRecording_ = false;                       // 设备 IsFrameRecording 镜像
};

} // namespace primal::graphics::rhi::d3d12
} // namespace primal::graphics::rhi

#endif // defined(_WIN32) && defined(ENABLE_D3D12) && ENABLE_D3D12
