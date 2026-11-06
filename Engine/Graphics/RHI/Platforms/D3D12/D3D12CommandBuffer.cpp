// Windows-only: requires ENABLE_D3D12 (_WIN32)
//
// D3D12CommandBuffer.cpp — 命令录制实现（M1 F1-6）。
#include "D3D12CommandBuffer.h"

#if defined(_WIN32) && defined(ENABLE_D3D12) && ENABLE_D3D12

#include "D3D12DescriptorHeaps.h"
#include "D3D12Device.h"
#include "D3D12Format.h"
#include "D3D12MipGen.h"
#include "D3D12Pipeline.h"
#include "D3D12Texture.h"
#include "D3D12Upload.h"

#include "Engine/Common/CoreLogStream.h"

#include <cstdlib>
#include <mutex>
#include <unordered_set>

namespace primal::graphics::rhi::d3d12 {

namespace {
// R21-B 绑定状态机诊断埋点总门（对齐 R20-B PRIMAL_D3D12_UPLOAD_TRACE 的
// env 惯例）：PRIMAL_D3D12_BIND_TRACE=1 时输出每次 list Reset（世代+分配
// 器槽位）、SetPipelineState/Set{Graphics,Compute}RootSignature/
// SetDescriptorHeaps 真实下发、Ensure 自愈/跳过事件。帧号 ≈ 世代-1（宿主
// 每帧恰好一次 cmd Reset+Begin，ClearBindingMirrors 每次 Begin 递增世代；
// 跨会话辅助对账时以 Reset 行的 gen 序列为准）。默认关闭 = 零日志零额外
// API 调用（仅本函数一次 getenv）。
bool BindTraceEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("PRIMAL_D3D12_BIND_TRACE");
        return v && v[0] == '1';
    }();
    return enabled;
}
} // namespace

// ============================================================================
// 生命周期
// ============================================================================

D3D12CommandBufferImpl::D3D12CommandBufferImpl(D3D12Device& device, CommandQueueType type)
    : RHICommandBuffer(device, type) {}

D3D12CommandBufferImpl::D3D12CommandBufferImpl(D3D12CommandBufferImpl&& other) noexcept
    : RHICommandBuffer(std::move(other)),
      currentAllocator_(other.currentAllocator_),
      boundGraphicsPipeline_(other.boundGraphicsPipeline_),
      boundGraphicsLayout_(other.boundGraphicsLayout_),
      boundComputePipeline_(other.boundComputePipeline_),
      boundComputeLayout_(other.boundComputeLayout_),
      effectivePso_(other.effectivePso_),
      graphicsRootSigSet_(other.graphicsRootSigSet_),
      computeRootSigSet_(other.computeRootSigSet_),
      descriptorHeapsBound_(other.descriptorHeapsBound_),
      listGeneration_(other.listGeneration_),
      lastExecuteTarget_(other.lastExecuteTarget_),
      isRecording_(other.isRecording_) {
    for (u32 i = 0; i < kAllocatorSlots; ++i) {
        allocators_[i] = std::move(other.allocators_[i]);
        allocatorLastUseTarget_[i] = other.allocatorLastUseTarget_[i];
    }
    commandList_ = std::move(other.commandList_);
    // R21-B：三槽镜像按"指针+世代"整体迁移（世代随镜像走，move 后的
    // Matches 语义不变）；源端全清（ClearBindingMirrors 内含世代递增）。
    other.ClearBindingMirrors();
    other.lastExecuteTarget_ = 0;
    other.isRecording_ = false;
}

D3D12CommandBufferImpl& D3D12CommandBufferImpl::operator=(D3D12CommandBufferImpl&& other) noexcept {
    if (this != &other) {
        RHICommandBuffer::operator=(std::move(other));
        for (u32 i = 0; i < kAllocatorSlots; ++i) {
            allocators_[i] = std::move(other.allocators_[i]);
            allocatorLastUseTarget_[i] = other.allocatorLastUseTarget_[i];
        }
        commandList_ = std::move(other.commandList_);
        currentAllocator_ = other.currentAllocator_;
        boundGraphicsPipeline_ = other.boundGraphicsPipeline_;
        boundGraphicsLayout_ = other.boundGraphicsLayout_;
        boundComputePipeline_ = other.boundComputePipeline_;
        boundComputeLayout_ = other.boundComputeLayout_;
        effectivePso_ = other.effectivePso_;
        graphicsRootSigSet_ = other.graphicsRootSigSet_;
        computeRootSigSet_ = other.computeRootSigSet_;
        descriptorHeapsBound_ = other.descriptorHeapsBound_;
        listGeneration_ = other.listGeneration_;
        lastExecuteTarget_ = other.lastExecuteTarget_;
        isRecording_ = other.isRecording_;
        other.ClearBindingMirrors();
        other.lastExecuteTarget_ = 0;
        other.isRecording_ = false;
    }
    return *this;
}

D3D12CommandBufferImpl::~D3D12CommandBufferImpl() { destroyImpl(); }

bool D3D12CommandBufferImpl::Initialize() {
    D3D12Device& dev = static_cast<D3D12Device&>(device_);
    ID3D12Device* native = dev.GetNativeDevice();
    if (!native) return false;

    for (u32 i = 0; i < kAllocatorSlots; ++i) {
        if (FAILED(native->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  IID_PPV_ARGS(&allocators_[i])))) {
            PRIMAL_LOG_STREAM(Error, "RHI")
                << "[D3D12CommandBuffer] allocator " << i << " creation failed" << std::endl;
            return false;
        }
        allocatorLastUseTarget_[i] = 0;
    }
    if (FAILED(native->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                         allocators_[0].Get(), nullptr,
                                         IID_PPV_ARGS(&commandList_)))) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[D3D12CommandBuffer] list creation failed" << std::endl;
        return false;
    }
    // D3D12 创建即 recording——立即 Close 归入 Reset 态（Open 状态机契约）。
    commandList_->Close();
    currentAllocator_ = 0;
    return true;
}

void D3D12CommandBufferImpl::destroyImpl() {
    commandList_.Reset();
    for (u32 i = 0; i < kAllocatorSlots; ++i) allocators_[i].Reset();
    // R21-B：清空收口到 ClearBindingMirrors（含世代递增）。
    ClearBindingMirrors();
}

// R21-B：绑定镜像全清 + 命令列表世代递增——beginImpl/resetImpl/move/
// destroy 的单一落点。commandList_->Reset 清空命令列表全部绑定态（PSO
// 单槽 + graphics/compute 两根签名槽 + descriptor heap 绑定），本函数与
// 之同步；世代递增让一切按旧世代置入的槽位镜像在 Matches 中失效——即使
// 未来出现遗漏调用本函数的 list Reset 路径，幂等跳过也会因世代不匹配而
// 退化为真实重下发（防御 Windows 二十测黑帧类的"镜像说已绑/实际已清"）。
void D3D12CommandBufferImpl::ClearBindingMirrors() {
    ++listGeneration_;
    boundGraphicsPipeline_ = nullptr;
    boundGraphicsLayout_ = handles::INVALID_PIPELINE_LAYOUT;
    boundComputePipeline_ = nullptr;
    boundComputeLayout_ = handles::INVALID_PIPELINE_LAYOUT;
    effectivePso_.Invalidate();
    graphicsRootSigSet_.Invalidate();
    computeRootSigSet_.Invalidate();
    descriptorHeapsBound_ = false;
}

bool D3D12CommandBufferImpl::resetImpl() {
    // GPU 侧复用判定在 beginImpl（allocator 轮转处）统一处理；这里只清绑定态。
    ClearBindingMirrors();
    return true;
}

bool D3D12CommandBufferImpl::beginImpl() {
    D3D12Device& dev = static_cast<D3D12Device&>(device_);
    if (!commandList_) return false;

    // allocator 帧槽轮转：fence 完成值判定复用（契约见文件头）。
    currentAllocator_ = (currentAllocator_ + 1) % kAllocatorSlots;
    const u64 guard = allocatorLastUseTarget_[currentAllocator_];
    if (guard != 0) {
        D3D12FencePool& fence = dev.GetFencePool();
        if (fence.GetCompletedValue() < guard) {
            PRIMAL_LOG_STREAM(Warn, "RHI")
                << "[D3D12CommandBuffer] allocator slot " << currentAllocator_
                << " still in flight (fence target " << guard
                << ") — CPU waiting before reset" << std::endl;
            fence.Wait(guard, 10000);
        }
    }
    if (FAILED(allocators_[currentAllocator_]->Reset())) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[D3D12CommandBuffer] allocator Reset failed" << std::endl;
        return false;
    }
    if (FAILED(commandList_->Reset(allocators_[currentAllocator_].Get(), nullptr))) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[D3D12CommandBuffer] list Reset failed" << std::endl;
        return false;
    }

    // R20-A：commandList_->Reset 清空命令列表全部绑定态（PSO 单槽 +
    // graphics/compute 两根签名槽）。镜像必须同步全清——Begin 可从
    // Executed 态直入（宿主无显式 Reset 时，如 TestRHISponzaShared 纹理
    // 上传 helper 的逐次 Begin），残留镜像会让 PushConstants/Dispatch 按
    // 上一会话的布局/管线路由，对着已被清空的命令列表槽位下发
    // （Windows 十九测 #709 SET_ROOT_CONSTANT_INVALID 的「签名未设」
    // 子句机制；镜像≠实际的一切分歧入口由此闭合）。
    // R21-B：清空收口 ClearBindingMirrors（含世代递增 + heap 绑定镜像），
    // 并在 BIND_TRACE 下输出 Reset 事件（黑帧诊断时可见"帧 N 从空绑定
    // 起步"——后续每帧应跟 SetDescriptorHeaps/SetPipelineState 记录）。
    ClearBindingMirrors();
    if (BindTraceEnabled()) {
        PRIMAL_LOG_STREAM(Info, "RHI.D3D12")
            << "[D3D12Bind] list Reset: gen=" << listGeneration_
            << " allocatorSlot=" << currentAllocator_ << std::endl;
    }

    isRecording_ = true;
    dev.IncrementRecording();
    EnsureDescriptorHeapsBound();
    // 帧首：上传环挂起拷贝编码（用户 pass 之前——对齐 Vulkan
    // VulkanCommandBuffer::beginImpl 的 EncodePendingBlits 接线）。
    dev.GetUploadRing().EncodePendingCopies(commandList_.Get());
    return true;
}

bool D3D12CommandBufferImpl::endImpl() {
    if (FAILED(commandList_->Close())) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[D3D12CommandBuffer] Close failed" << std::endl;
        return false;
    }
    D3D12Device& dev = static_cast<D3D12Device&>(device_);
    if (isRecording_) {
        isRecording_ = false;
        dev.DecrementRecording();
    }
    return true;
}

bool D3D12CommandBufferImpl::submitImpl(u32 /*waitFlags*/) {
    // 直连 Submit 路径（RenderSystem 走 device Submit → 设备层统一
    // ExecuteCommandLists + fence Signal；两条路径共享 ExecuteAndSignal）。
    D3D12Device& dev = static_cast<D3D12Device&>(device_);
    if (!commandList_) return false;
    // R21-A（M5 帧 2+ 新暴露面）：本直连路径此前不推进 tracker 提交边界
    // 衰减——只有设备层 submitImpl/EndFrame 与 upload flush 调
    // OnSubmitBoundary，而集成测试逐帧 cmd->Submit + WaitForCompletion 恰
    // 好绕开三者 → buffer 残留上一帧 CopySource/UAV 等假态，下一帧
    // Transition 产出 before≠GPU 实际（已按 D3D12 衰减语义回 COMMON）的
    // barrier。与设备层同款：提交时点推进建模（幂等——已 COMMON 的再衰
    // 减为零记录）。
    (void)dev.GetStateTracker().OnSubmitBoundary();
    const u64 target = dev.ExecuteAndSignal(commandList_.Get());
    if (target == 0) return false;
    allocatorLastUseTarget_[currentAllocator_] = target;
    lastExecuteTarget_ = target;
    return true;
}

bool D3D12CommandBufferImpl::waitForCompletionImpl() {
    D3D12Device& dev = static_cast<D3D12Device&>(device_);
    if (lastExecuteTarget_ == 0) return true;  // 无挂起提交
    return dev.GetFencePool().Wait(lastExecuteTarget_, 30000);
}

// ============================================================================
// barrier 集成（M0 D3D12StateTracker）
// ============================================================================

void D3D12CommandBufferImpl::EmitTrackerOps(const utl::vector<D3D12ResourceBarrierOp>& ops,
                                            ID3D12Resource* resource) {
    if (ops.empty() || !resource || !commandList_) return;
    D3D12_RESOURCE_BARRIER barriers[8];
    u32 count = 0;
    for (const D3D12ResourceBarrierOp& op : ops) {
        if (op.kind == D3D12ResourceBarrierOp::Kind::Transition && count < 8) {
            D3D12_RESOURCE_BARRIER& b = barriers[count++];
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            b.Transition.pResource = resource;
            b.Transition.StateBefore = static_cast<D3D12_RESOURCE_STATES>(op.stateBefore);
            b.Transition.StateAfter = static_cast<D3D12_RESOURCE_STATES>(op.stateAfter);
            b.Transition.Subresource = op.subresource;
        } else if (op.kind == D3D12ResourceBarrierOp::Kind::UAV && count < 8) {
            D3D12_RESOURCE_BARRIER& b = barriers[count++];
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            b.UAV.pResource = resource;
        }
        // Aliasing：M1 无 placed 别名资源——跳过（tracker 不产出该类 op）。
        if (count == 8) {
            commandList_->ResourceBarrier(count, barriers);
            count = 0;
        }
    }
    if (count > 0) commandList_->ResourceBarrier(count, barriers);
}

void D3D12CommandBufferImpl::TransitionTexture(D3D12Texture* texture, ResourceHandle handle,
                                               ResourceState target, u32 subresource) {
    if (!texture || !commandList_) return;
    D3D12Device& dev = static_cast<D3D12Device&>(device_);
    D3D12SubresourceKey key{};
    key.resource = handle;
    key.subresource = subresource;
    key.isTexture = true;
    EmitTrackerOps(dev.GetStateTracker().Transition(key, target),
                   texture->GetNativeResource());
}

namespace {
// R21-A：InsertBarrier buffer 分支"声明态与 tracker 不一致"Warn 的一次性
// 去重（key=resource id；命令录制单线程，mutex 仅作跨线程防御）。
std::mutex g_barrierDeclWarnMutex;
std::unordered_set<u64> g_barrierDeclWarned;
bool WarnBarrierDeclMismatchOnce(u64 resource) {
    std::lock_guard<std::mutex> lock(g_barrierDeclWarnMutex);
    return g_barrierDeclWarned.insert(resource).second;
}
} // namespace

void D3D12CommandBufferImpl::InsertBarrier(const ResourceBarrier* barriers, u32 barrierCount) {
    if (!barriers || barrierCount == 0 || !commandList_) return;
    D3D12Device& dev = static_cast<D3D12Device&>(device_);
    UpdateStats(CommandType::Barrier);

    for (u32 i = 0; i < barrierCount; ++i) {
        const ResourceBarrier& rb = barriers[i];
        D3D12Texture* texture = dev.GetTexture(rb.resource);
        if (texture) {
            // 子资源语义：RHI_ALL_SUBRESOURCES → 全子资源；否则按 D3D12 线性
            // 子资源索引直通（RenderGraph 侧 M1 以单 mip/全量为主）。
            const u32 sub = rb.subresource == RHI_ALL_SUBRESOURCES
                                ? kD3D12ResourceBarrierAllSubresources
                                : rb.subresource;
            D3D12SubresourceKey key{};
            key.resource = rb.resource;
            key.subresource = sub;
            key.isTexture = true;
            EmitTrackerOps(dev.GetStateTracker().Transition(key, rb.afterState),
                           texture->GetNativeResource());
            continue;
        }
        // M5 F5-2/F5-4：buffer 走 tracker（M1 为 no-op——COMMON 提升覆盖
        // 读路径；但 compute UAV 写→INDIRECT_ARGUMENT 读需显式转换，即
        // D3D12ExecuteIndirect 官方样例的 UAV→INDIRECT_ARGUMENT 模式）。
        // R21-A（M5 #538 修复）：tracker 权威，rb.beforeState 声明仅参考。
        // tracker 已有条目时不一致不得 Track 覆盖——旧实现把 tracker 改成
        // 调用方声明假态，Transition 于假态起点产出零 op、GPU 停留实际态
        // = Windows 二十测 #538 CopyBufferRegion 源态错（argsBuf 实际
        // INDIRECT_ARGUMENT、声明 CopySource → 零 barrier 直拷非法）。
        // 不一致时 Warn 一次性留痕（防刷屏，按 resource 去重）；无条目且
        // 声明非 Unknown 才 Track 注入（保留 GPU 事实注入语义；未注册
        // buffer 的 Track 被跟踪器拒绝，自然维持 M1 手工路径 no-op）。
        d3d12::D3D12Buffer* bufHandle = dev.GetBuffer(rb.resource);
        if (bufHandle) {
            D3D12SubresourceKey key{};
            key.resource = rb.resource;
            key.subresource = 0;
            key.isTexture = false;
            ResourceState cur{};
            if (dev.GetStateTracker().QueryState(key, cur)) {
                if (cur != rb.beforeState
                    && rb.beforeState != ResourceState::Unknown
                    && WarnBarrierDeclMismatchOnce(rb.resource)) {
                    PRIMAL_LOG_STREAM(Warn, "RHI")
                        << "[D3D12CommandBuffer] InsertBarrier buffer declaration "
                           "differs from tracker (tracker authoritative, decl "
                           "ignored): resource="
                        << rb.resource
                        << " declared=" << static_cast<u32>(rb.beforeState)
                        << " tracked=" << static_cast<u32>(cur) << std::endl;
                }
            } else if (rb.beforeState != ResourceState::Unknown) {
                dev.GetStateTracker().Track(key, rb.beforeState);
            }
            EmitTrackerOps(dev.GetStateTracker().Transition(key, rb.afterState),
                           bufHandle->GetNativeResource());
        }
    }
}

void D3D12CommandBufferImpl::MemoryBarrier(PipelineStage /*srcStageMask*/,
                                            PipelineStage /*dstStageMask*/,
                                            AccessFlag srcAccessMask,
                                            AccessFlag /*dstAccessMask*/) {
    if (!commandList_) return;
    UpdateStats(CommandType::Barrier);
    // 实现策略（文件头契约）：src 含任一写位 → 一条全局 UAV barrier
    // （pResource=nullptr = 等待全部在飞 UAV 读写；覆盖 compute→compute
    // WAW/WAR 与通用写序）；纯读依赖 → no-op（经典模型里读依赖由
    // transition 的执行/内存序携带）。M5+ Enhanced Barriers 改精确表达。
    const u32 writeBits =
        static_cast<u32>(AccessFlag::ShaderWrite) | static_cast<u32>(AccessFlag::TransferWrite)
        | static_cast<u32>(AccessFlag::ColorAttachmentWrite)
        | static_cast<u32>(AccessFlag::DepthStencilAttachmentWrite)
        | static_cast<u32>(AccessFlag::MemoryWrite) | static_cast<u32>(AccessFlag::HostWrite);
    if ((static_cast<u32>(srcAccessMask) & writeBits) != 0) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.UAV.pResource = nullptr;  // 全局 UAV barrier
        commandList_->ResourceBarrier(1, &barrier);
    }
}

// ============================================================================
// 渲染通道
// ============================================================================

void D3D12CommandBufferImpl::BeginRenderPass(const RenderPassDesc& desc) {
    if (!commandList_) return;
    D3D12Device& dev = static_cast<D3D12Device&>(device_);
    EnsureDescriptorHeapsBound();
    UpdateStats(CommandType::BeginRenderPass);

    // RTV/DSV 集合 + 附件状态收敛（tracker 幂等转换：同状态零 op）。
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[constants::MAX_RENDER_TARGETS];
    BOOL hasDepth = FALSE;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv{};

    u32 rtCount = 0;
    for (const RenderPassDesc::Attachment& attachment : desc.colorAttachments) {
        D3D12Texture* tex = dev.GetTexture(attachment.texture);
        if (!tex || tex->RtvSlot() == D3D12Texture::kInvalidDescriptorSlot) continue;
        rtvs[rtCount] = dev.GetDescriptorHeaps().RtvCpuHandle(tex->RtvSlot());
        // 线性子资源索引（mip + mipLevels * arrayLayer；tracker 契约）。
        const u32 subresource = attachment.mipLevel
            + tex->GetTextureDesc().mipLevels * attachment.arrayLayer;
        TransitionTexture(tex, attachment.texture, ResourceState::RenderTarget,
                          subresource);
        if (attachment.loadOp == LoadAction::Clear) {
            const float color[4] = {attachment.clearValue.color.x,
                                    attachment.clearValue.color.y,
                                    attachment.clearValue.color.z,
                                    attachment.clearValue.color.w};
            commandList_->ClearRenderTargetView(rtvs[rtCount], color, 0, nullptr);
        }
        ++rtCount;
    }
    if (desc.depthAttachment.texture != handles::INVALID_RESOURCE) {
        D3D12Texture* tex = dev.GetTexture(desc.depthAttachment.texture);
        if (tex && tex->DsvSlot() != D3D12Texture::kInvalidDescriptorSlot) {
            dsv = dev.GetDescriptorHeaps().DsvCpuHandle(tex->DsvSlot());
            hasDepth = TRUE;
            const u32 subresource = desc.depthAttachment.mipLevel
                + tex->GetTextureDesc().mipLevels * desc.depthAttachment.arrayLayer;
            TransitionTexture(tex, desc.depthAttachment.texture,
                              ResourceState::DepthStencil, subresource);
            if (desc.depthAttachment.loadOp == LoadAction::Clear) {
                // §7.2-2：[0,1] 正向 Z——清 1.0（模板 0）。
                commandList_->ClearDepthStencilView(
                    dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
            }
        }
    }

    commandList_->OMSetRenderTargets(rtCount, rtvs, FALSE, hasDepth ? &dsv : nullptr);

    // 视口/裁剪：RenderPass 携带时直接应用（负高度契约见 SetViewport）。
    if (desc.viewport.size.x > 0 && desc.viewport.size.y > 0) {
        SetViewport(desc.viewport);
    }
    if (desc.scissor.extent.x > 0 && desc.scissor.extent.y > 0) {
        SetScissor(desc.scissor);
    }
}

void D3D12CommandBufferImpl::BeginRenderPass(RenderPassHandle renderPass) {
    // 句柄变体：取设备侧登记的 RenderPassDesc 重入 desc 路径。
    D3D12Device& dev = static_cast<D3D12Device&>(device_);
    const RenderPassDesc* stored = dev.GetRenderPassDesc(renderPass);
    if (stored) {
        BeginRenderPass(*stored);
    } else {
        PRIMAL_LOG_STREAM(Warn, "RHI")
            << "[D3D12CommandBuffer] BeginRenderPass(handle) unresolved handle" << std::endl;
    }
}

void D3D12CommandBufferImpl::EndRenderPass() {
    UpdateStats(CommandType::EndRenderPass);
    // 附件结束态由调用方 InsertBarrier 驱动（tracker 已记录当前态）；
    // D3D12 无 render pass 对象边界。
}

void D3D12CommandBufferImpl::SetViewport(const ViewportDesc& viewport) {
    if (!commandList_) return;
    UpdateStats(CommandType::SetViewport);
    // §7.2-1 契约（H2 修订）：负高度 {0, H, W, -H}——D3D12 官方 Y 翻转
    // 手段，补偿 NDC Y 语义差后绕序与 Vulkan 一致；PSO cull 直译
    //（D3D12Pipeline::ToCullMode，不再二次翻转）。
    D3D12_VIEWPORT vp{};
    vp.TopLeftX = viewport.topLeft.x;
    vp.TopLeftY = viewport.topLeft.y + viewport.size.y;  // 翻转锚点=底边
    vp.Width = viewport.size.x;
    vp.Height = -viewport.size.y;                        // 负高度
    vp.MinDepth = viewport.minDepth;
    vp.MaxDepth = viewport.maxDepth;
    commandList_->RSSetViewports(1, &vp);
}

void D3D12CommandBufferImpl::SetScissor(const Rect& scissor) {
    if (!commandList_) return;
    UpdateStats(CommandType::SetScissor);
    // Scissor 是屏幕空间矩形（不随负高度视口翻转）。
    D3D12_RECT rect{};
    rect.left = static_cast<LONG>(scissor.offset.x);
    rect.top = static_cast<LONG>(scissor.offset.y);
    rect.right = rect.left + static_cast<LONG>(scissor.extent.x);
    rect.bottom = rect.top + static_cast<LONG>(scissor.extent.y);
    commandList_->RSSetScissorRects(1, &rect);
}

// ============================================================================
// 管线 / 绑定
// ============================================================================

// R20-A：Bind{Graphics,Compute}Pipeline 共用落点。原实现的两个缺口：
//   ①单一 boundPipeline_/boundLayout_ 镜像——跨管线类型切换后
//     PushConstants/BindDescriptorSets 的布局来源与目标 bind point 脱钩；
//   ②pipe->GetRootSignature() 为空时静默跳过 Set*RootSignature——
//     创建期异常态被吞，缺陷延后到 Dispatch（#952）/PushConstants
//     （#709）处才在 debug layer 显形。现为按 bind point 独立镜像 +
//     空签名 Error 显式记录；PSO/根签名下发按槽位镜像幂等跳过。
// R21-B：①幂等判定从裸指针等值升级为 D3D12BindSlotMirror「指针+世代」
//   Matches——跨 list Reset（ClearBindingMirrors 世代递增）后同指针不再
//   命中，即使镜像清空与 Reset 失同步也只会多下发冗余 Set（安全侧失败），
//   不可能再漏发（黑帧侧失败）；②原 PSO 缺失（GetPso()==null）为静默
//   跳过、且 Ensure 侧会以 null 调 SetPipelineState——现 Error 显式。
void D3D12CommandBufferImpl::ApplyPipelineBind(D3D12Pipeline* pipe) {
    if (pipe->IsCompute()) {
        boundComputePipeline_ = pipe;
        // 布局镜像存句柄（pool 扩容搬迁槽位后旧地址悬垂，见成员声明处
        // 注释）；使用点经 device 现查。自建空布局管线句柄为 INVALID，
        // 与原实现 GetPipelineLayout()==nullptr 的镜像置空语义一致。
        boundComputeLayout_ = pipe->GetLayoutHandle();
        if (ID3D12RootSignature* rs = pipe->GetRootSignature()) {
            if (!computeRootSigSet_.Matches(rs, listGeneration_)) {
                commandList_->SetComputeRootSignature(rs);
                computeRootSigSet_.Set(rs, listGeneration_);
                if (BindTraceEnabled()) {
                    PRIMAL_LOG_STREAM(Info, "RHI.D3D12")
                        << "[D3D12Bind] gen=" << listGeneration_
                        << " SetComputeRootSignature rs=" << rs << std::endl;
                }
            }
        } else {
            PRIMAL_LOG_STREAM(Error, "RHI")
                << "[D3D12CommandBuffer] BindComputePipeline: pipeline has null "
                << "root signature — Dispatch would raise debug-layer "
                << "#952 (DISPATCH_ROOT_SIGNATURE_NOT_SET)" << std::endl;
        }
    } else {
        boundGraphicsPipeline_ = pipe;
        // 同 compute 侧：布局镜像存句柄，使用点现查。
        boundGraphicsLayout_ = pipe->GetLayoutHandle();
        if (ID3D12RootSignature* rs = pipe->GetRootSignature()) {
            if (!graphicsRootSigSet_.Matches(rs, listGeneration_)) {
                commandList_->SetGraphicsRootSignature(rs);
                graphicsRootSigSet_.Set(rs, listGeneration_);
                if (BindTraceEnabled()) {
                    PRIMAL_LOG_STREAM(Info, "RHI.D3D12")
                        << "[D3D12Bind] gen=" << listGeneration_
                        << " SetGraphicsRootSignature rs=" << rs << std::endl;
                }
            }
        } else {
            PRIMAL_LOG_STREAM(Error, "RHI")
                << "[D3D12CommandBuffer] BindGraphicsPipeline: pipeline has "
                << "null root signature — draw/push would raise debug-layer "
                << "#709 (SET_ROOT_CONSTANT_INVALID) family" << std::endl;
        }
        commandList_->IASetPrimitiveTopology(pipe->GetIaTopology());
    }
    // PSO 槽 graphics/compute 共用：按 effectivePso_ 镜像（指针+世代）幂等下发。
    if (ID3D12PipelineState* pso = pipe->GetPso()) {
        if (!effectivePso_.Matches(pso, listGeneration_)) {
            commandList_->SetPipelineState(pso);
            effectivePso_.Set(pso, listGeneration_);
            if (BindTraceEnabled()) {
                PRIMAL_LOG_STREAM(Info, "RHI.D3D12")
                    << "[D3D12Bind] gen=" << listGeneration_
                    << " SetPipelineState pso=" << pso << std::endl;
            }
        }
    } else {
        // R21-B：PSO 缺失原为静默跳过（Ensure 侧会以 null 下发
        // SetPipelineState(nullptr)——debug-layer 必错）。创建期异常态显式化。
        PRIMAL_LOG_STREAM(Error, "RHI")
            << "[D3D12CommandBuffer] ApplyPipelineBind: pipeline has null "
            << "PSO (creation-time anomaly) — draws/dispatches would record "
            << "without a pipeline state" << std::endl;
    }
}

// 消费方前置自愈：compute 槽（PSO+根签名）。镜像在会话内从未绑定 →
// false（调用方跳过录制——把必错命令发给 debug layer 只会产生
// #952/#882 族噪声，Error 日志已给出现场）。
// R21-B：原 null RS / null PSO 分支静默 return false——若管线创建期即缺
// RS/PSO，每个 Dispatch/Draw 全部无声跳过 = 整程黑帧且消费点零日志
// （二十测黑帧排查要求 Ensure 失败路径全部显式）；现一次性 Error。
bool D3D12CommandBufferImpl::EnsureComputeState() {
    if (!commandList_) return false;
    if (!boundComputePipeline_) {
        PRIMAL_LOG_STREAM(Error, "RHI")
            << "[D3D12CommandBuffer] Dispatch without BindComputePipeline in "
            << "this recording session — command skipped (would raise "
            << "debug-layer #952 DISPATCH_ROOT_SIGNATURE_NOT_SET)" << std::endl;
        return false;
    }
    if (ID3D12RootSignature* rs = boundComputePipeline_->GetRootSignature()) {
        if (!computeRootSigSet_.Matches(rs, listGeneration_)) {
            commandList_->SetComputeRootSignature(rs);
            computeRootSigSet_.Set(rs, listGeneration_);
            if (BindTraceEnabled()) {
                PRIMAL_LOG_STREAM(Info, "RHI.D3D12")
                    << "[D3D12Bind] gen=" << listGeneration_
                    << " SetComputeRootSignature rs=" << rs
                    << " (EnsureCompute heal)" << std::endl;
            }
        }
    } else {
        if (!warnedEnsureNullRs_) {
            warnedEnsureNullRs_ = true;
            PRIMAL_LOG_STREAM(Error, "RHI")
                << "[D3D12CommandBuffer] EnsureComputeState: bound compute "
                << "pipeline has null root signature — all Dispatches skipped "
                << "(creation-time anomaly; see BindComputePipeline Error "
                << "above)" << std::endl;
        }
        return false;
    }
    if (ID3D12PipelineState* pso = boundComputePipeline_->GetPso()) {
        if (!effectivePso_.Matches(pso, listGeneration_)) {
            commandList_->SetPipelineState(pso);
            effectivePso_.Set(pso, listGeneration_);
            if (BindTraceEnabled()) {
                PRIMAL_LOG_STREAM(Info, "RHI.D3D12")
                    << "[D3D12Bind] gen=" << listGeneration_
                    << " SetPipelineState pso=" << pso
                    << " (EnsureCompute heal)" << std::endl;
            }
        }
    } else {
        // R21-B：原实现以 null 调 SetPipelineState（必错）——跳过并显式。
        if (!warnedEnsureNullPso_) {
            warnedEnsureNullPso_ = true;
            PRIMAL_LOG_STREAM(Error, "RHI")
                << "[D3D12CommandBuffer] EnsureComputeState: bound compute "
                << "pipeline has null PSO — all Dispatches skipped" << std::endl;
        }
        return false;
    }
    return true;
}

// 消费方前置自愈：graphics 槽（PSO+根签名），对称 EnsureComputeState。
bool D3D12CommandBufferImpl::EnsureGraphicsState() {
    if (!commandList_) return false;
    if (!boundGraphicsPipeline_) {
        PRIMAL_LOG_STREAM(Error, "RHI")
            << "[D3D12CommandBuffer] Draw/ExecuteIndirect without "
            << "BindGraphicsPipeline in this recording session — command "
            << "skipped (would raise debug-layer #110 DRAW_ROOT_SIGNATURE_"
            << "NOT_SET family)" << std::endl;
        return false;
    }
    if (ID3D12RootSignature* rs = boundGraphicsPipeline_->GetRootSignature()) {
        if (!graphicsRootSigSet_.Matches(rs, listGeneration_)) {
            commandList_->SetGraphicsRootSignature(rs);
            graphicsRootSigSet_.Set(rs, listGeneration_);
            if (BindTraceEnabled()) {
                PRIMAL_LOG_STREAM(Info, "RHI.D3D12")
                    << "[D3D12Bind] gen=" << listGeneration_
                    << " SetGraphicsRootSignature rs=" << rs
                    << " (EnsureGraphics heal)" << std::endl;
            }
        }
    } else {
        if (!warnedEnsureNullRs_) {
            warnedEnsureNullRs_ = true;
            PRIMAL_LOG_STREAM(Error, "RHI")
                << "[D3D12CommandBuffer] EnsureGraphicsState: bound graphics "
                << "pipeline has null root signature — all draws skipped "
                << "(creation-time anomaly; see BindGraphicsPipeline Error "
                << "above)" << std::endl;
        }
        return false;
    }
    if (ID3D12PipelineState* pso = boundGraphicsPipeline_->GetPso()) {
        if (!effectivePso_.Matches(pso, listGeneration_)) {
            commandList_->SetPipelineState(pso);
            effectivePso_.Set(pso, listGeneration_);
            if (BindTraceEnabled()) {
                PRIMAL_LOG_STREAM(Info, "RHI.D3D12")
                    << "[D3D12Bind] gen=" << listGeneration_
                    << " SetPipelineState pso=" << pso
                    << " (EnsureGraphics heal)" << std::endl;
            }
        }
    } else {
        // R21-B：原实现以 null 调 SetPipelineState（必错）——跳过并显式。
        if (!warnedEnsureNullPso_) {
            warnedEnsureNullPso_ = true;
            PRIMAL_LOG_STREAM(Error, "RHI")
                << "[D3D12CommandBuffer] EnsureGraphicsState: bound graphics "
                << "pipeline has null PSO — all draws skipped" << std::endl;
        }
        return false;
    }
    return true;
}

void D3D12CommandBufferImpl::BindGraphicsPipeline(PipelineHandle pipeline) {
    if (!commandList_) return;
    UpdateStats(CommandType::BindGraphicsPipeline);
    D3D12Device& dev = static_cast<D3D12Device&>(device_);
    D3D12Pipeline* pipe = dev.GetPipeline(pipeline);
    if (!pipe || pipe->IsCompute()) return;
    ApplyPipelineBind(pipe);
}

void D3D12CommandBufferImpl::BindComputePipeline(PipelineHandle pipeline) {
    if (!commandList_) return;
    UpdateStats(CommandType::BindComputePipeline);
    D3D12Device& dev = static_cast<D3D12Device&>(device_);
    D3D12Pipeline* pipe = dev.GetPipeline(pipeline);
    if (!pipe || !pipe->IsCompute()) return;
    ApplyPipelineBind(pipe);
}

void D3D12CommandBufferImpl::EnsureDescriptorHeapsBound() {
    if (descriptorHeapsBound_ || !commandList_) return;
    D3D12Device& dev = static_cast<D3D12Device&>(device_);
    ID3D12DescriptorHeap* heaps[] = {dev.GetDescriptorHeaps().GetCbvSrvUavHeap(),
                                     dev.GetDescriptorHeaps().GetSamplerHeap()};
    commandList_->SetDescriptorHeaps(2, heaps);
    descriptorHeapsBound_ = true;
    // R21-B：heap 绑定属于命令列表状态（list Reset 清空，descriptorHeapsBound_
    // 镜像随 ClearBindingMirrors 同步失效）——BIND_TRACE 下帧首可见此行，
    // 黑帧诊断时"帧 N 无 SetDescriptorHeaps 记录"= heap 绑定镜像/生命周期缺陷。
    if (BindTraceEnabled()) {
        PRIMAL_LOG_STREAM(Info, "RHI.D3D12")
            << "[D3D12Bind] gen=" << listGeneration_
            << " SetDescriptorHeaps cbvSrvUav=" << heaps[0]
            << " sampler=" << heaps[1] << std::endl;
    }
}

void D3D12CommandBufferImpl::BindVertexBuffers(u32 firstSlot, u32 slotCount,
                                               const ResourceHandle* buffers,
                                               const u64* offsets) {
    if (!commandList_ || !buffers || slotCount == 0) return;
    UpdateStats(CommandType::BindVertexBuffers);
    D3D12Device& dev = static_cast<D3D12Device&>(device_);
    D3D12_VERTEX_BUFFER_VIEW views[constants::MAX_VERTEX_BUFFERS];
    u32 count = 0;
    for (u32 i = 0; i < slotCount && firstSlot + i < constants::MAX_VERTEX_BUFFERS; ++i) {
        D3D12Buffer* buffer = dev.GetBuffer(buffers[i]);
        if (!buffer) continue;
        const u64 offset = offsets ? offsets[i] : 0;
        // stride 优先取绑定管线的顶点布局声明（32B stride 先例）；管线未
        // 声明该槽时回退 BufferDesc.vertex.vertexStride。（R20-A：stride
        // 声明属 graphics 侧——改读 boundGraphicsPipeline_，compute 绑定
        // 不再参与该查找。）
        UINT stride = buffer->GetBufferDesc().vertex.vertexStride;
        if (boundGraphicsPipeline_) {
            for (const auto& vb : boundGraphicsPipeline_->GetGraphicsDesc().vertexBindings) {
                if (vb.binding == firstSlot + i) {
                    stride = vb.stride;
                    break;
                }
            }
        }
        D3D12_VERTEX_BUFFER_VIEW& view = views[count++];
        view.BufferLocation = buffer->GetGpuAddress() + offset;
        view.SizeInBytes = static_cast<UINT>(buffer->GetBufferDesc().size - offset);
        view.StrideInBytes = stride;
    }
    if (count > 0) {
        commandList_->IASetVertexBuffers(firstSlot, count, views);
    }
}

void D3D12CommandBufferImpl::BindIndexBuffer(ResourceHandle buffer, DataFormat format,
                                             u64 offset) {
    if (!commandList_) return;
    UpdateStats(CommandType::BindIndexBuffer);
    D3D12Device& dev = static_cast<D3D12Device&>(device_);
    D3D12Buffer* buf = dev.GetBuffer(buffer);
    if (!buf) return;
    D3D12_INDEX_BUFFER_VIEW view{};
    view.BufferLocation = buf->GetGpuAddress() + offset;
    view.SizeInBytes = static_cast<UINT>(buf->GetBufferDesc().size - offset);
    view.Format = format == DataFormat::R16_UInt
                      ? static_cast<DXGI_FORMAT>(kDXGIFormat_R16_UInt)
                      : static_cast<DXGI_FORMAT>(kDXGIFormat_R32_UInt);
    commandList_->IASetIndexBuffer(&view);
}

void D3D12CommandBufferImpl::BindDescriptorSets(PipelineBindPoint bindPoint,
                                                PipelineLayoutHandle /*layout*/,
                                                u32 firstSet, u32 setCount,
                                                const DescriptorSetHandle* descriptorSets,
                                                u32 dynamicOffsetCount,
                                                const u32* /*dynamicOffsets*/) {
    if (!commandList_ || !descriptorSets || setCount == 0) return;
    UpdateStats(CommandType::BindDescriptorSets);
    EnsureDescriptorHeapsBound();
    D3D12Device& dev = static_cast<D3D12Device&>(device_);

    // 动态偏移：M1 显式忽略（绑定模型契约 §6——帧槽化 UpdateDescriptorSets
    // 路径下烘焙偏移等价）；一次性提示。
    if (dynamicOffsetCount > 0 && !warnedDynamicOffsets_) {
        warnedDynamicOffsets_ = true;
        PRIMAL_LOG_STREAM(Warn, "RHI")
            << "[D3D12CommandBuffer] dynamic offsets accepted and ignored "
            << "(baked at UpdateDescriptorSets; per-frame set re-update path)" << std::endl;
    }

    const bool compute = bindPoint == PipelineBindPoint::Compute;
    // R20-A：root 参数索引按目标 bind point 取对应侧绑定布局（原实现用
    // 单一 boundLayout_——最近绑定是另一类型管线时，Graphics 绑定会拿
    // compute 布局的 tableParam 对 graphics 签名下发 = #708/#709 族的
    // 参数索引错位）。前置自愈：对应槽位缺/错时按镜像补设。
    // 布局镜像为句柄：pool 扩容搬迁槽位后旧地址悬垂，此处现查取新地址
    //（句柄失效 = 布局已销毁 → nullptr 走下方"无布局"早退）。
    const PipelineLayoutHandle boundLayoutHandle = compute ? boundComputeLayout_
                                                           : boundGraphicsLayout_;
    const D3D12PipelineLayout* boundLayout =
        boundLayoutHandle == handles::INVALID_PIPELINE_LAYOUT
            ? nullptr : dev.GetPipelineLayout(boundLayoutHandle);
    if (!boundLayout && !warnedSetsNoLayout_) {
        warnedSetsNoLayout_ = true;
        PRIMAL_LOG_STREAM(Warn, "RHI")
            << "[D3D12CommandBuffer] BindDescriptorSets("
            << (compute ? "Compute" : "Graphics")
            << ") with no pipeline bound on that bind point in this session "
            << "— sets ignored" << std::endl;
    }
    if (!boundLayout || !(compute ? EnsureComputeState() : EnsureGraphicsState())) return;

    for (u32 i = 0; i < setCount; ++i) {
        const u32 setIndex = firstSet + i;
        D3D12DescriptorSet* set = dev.GetDescriptorSet(descriptorSets[i]);
        if (!set) continue;
        // 长寿命 set 跨帧护栏：区段属旧帧槽 → 迁移到当前槽（见设备声明）。
        dev.EnsureSetRegionCurrent(set);
        // root 参数索引由绑定管线布局的映射表提供（契约 §3）。
        const D3D12RootSignatureLayout* rootLayout = &boundLayout->GetRootLayout();
        if (setIndex >= rootLayout->sets.size()) continue;
        const D3D12SetRootParams& params = rootLayout->sets[setIndex];
        if (params.tableParam != D3D12SetRootParams::kInvalidRootParam
            && set->BaseSlot() != D3D12DescriptorHeapManager::kInvalidSlot) {
            const D3D12_GPU_DESCRIPTOR_HANDLE gpu =
                dev.GetDescriptorHeaps().CbvSrvUavGpuHandle(set->BaseSlot());
            // H1 stage 拆组：main 表参数为连续区间 [tableParam,
            // tableParam+tableParamCount)，各组绑同一区段起点句柄（range
            // 偏移是区段内绝对偏移，见 D3D12Pipeline.h 契约 §4）。
            for (u32 p = 0; p < params.tableParamCount; ++p) {
                if (compute) {
                    commandList_->SetComputeRootDescriptorTable(
                        params.tableParam + p, gpu);
                } else {
                    commandList_->SetGraphicsRootDescriptorTable(
                        params.tableParam + p, gpu);
                }
            }
        }
        if (params.samplerTableParam != D3D12SetRootParams::kInvalidRootParam
            && set->SamplerBaseSlot() != D3D12DescriptorHeapManager::kInvalidSlot) {
            const D3D12_GPU_DESCRIPTOR_HANDLE gpu =
                dev.GetDescriptorHeaps().SamplerGpuHandle(set->SamplerBaseSlot());
            if (compute) {
                commandList_->SetComputeRootDescriptorTable(params.samplerTableParam, gpu);
            } else {
                commandList_->SetGraphicsRootDescriptorTable(params.samplerTableParam, gpu);
            }
        }
    }
}

// R20-A 重写（契约对齐 VulkanCommandBuffer::PushConstants）：
//   - 布局来源：layout 句柄优先解析（调用方传入的布局即常量归属——
//     constantsParam 与根签名同源，参数索引恒与命令列表当前签名一致，
//     #709 SET_ROOT_CONSTANT_INVALID 双子句「签名未设/索引不匹配」由此
//     根治）；句柄无效回退目标 bind point 的绑定布局。
//   - Graphics/Compute 前缀按 stageFlags 路由（原实现按单一镜像
//     boundPipeline_->IsCompute()——跨管线类型切换后路由与布局来源可能
//     脱钩）；歧义（无位/多位混合）回退：仅单侧已绑取该侧，双侧已绑取
//     graphics（M1-M6 语料全部为显式 Vertex/Compute 单位）。
//   - 下发前 Ensure*State：目标槽（PSO+根签名）按镜像自愈——若命令列表
//     签名与常量来源布局的签名不同步（layout 参数指向他者布局），以
//     布局的签名为准重设该槽（常量调用自身保持内部一致；后续 Draw 若
//     因此报 DRAW_ROOT_SIGNATURE_MISMATCH，则指向真实的宿主布局错配）。
void D3D12CommandBufferImpl::PushConstants(PipelineLayoutHandle layout,
                                           ShaderStage stageFlags,
                                           u32 offset, u32 size, const void* pValues) {
    if (!commandList_ || !pValues || size == 0) return;
    D3D12Device& dev = static_cast<D3D12Device&>(device_);
    D3D12PipelineLayout* targetLayout = nullptr;
    if (layout != handles::INVALID_PIPELINE_LAYOUT) {
        targetLayout = dev.GetPipelineLayout(layout);
    }
    const u32 stageBits = static_cast<u32>(stageFlags);
    const u32 computeBit = static_cast<u32>(ShaderStage::Compute);
    const u32 graphicsBits = static_cast<u32>(ShaderStage::Vertex)
        | static_cast<u32>(ShaderStage::Pixel)
        | static_cast<u32>(ShaderStage::Geometry)
        | static_cast<u32>(ShaderStage::Hull)
        | static_cast<u32>(ShaderStage::Domain);
    bool compute = false;
    if (stageBits == computeBit) {
        compute = true;
    } else if ((stageBits & graphicsBits) != 0 && (stageBits & computeBit) == 0) {
        compute = false;
    } else {
        compute = boundComputePipeline_ && !boundGraphicsPipeline_;
    }
    // 回退布局镜像为句柄（pool 扩容搬迁后现查取新地址；句柄失效 → 维持
    // nullptr 走下方"无可用布局"早退）。
    if (!targetLayout) {
        const PipelineLayoutHandle fallback = compute ? boundComputeLayout_
                                                      : boundGraphicsLayout_;
        if (fallback != handles::INVALID_PIPELINE_LAYOUT) {
            targetLayout = dev.GetPipelineLayout(fallback);
        }
    }
    if (!targetLayout) {
        if (!warnedPushNoLayout_) {
            warnedPushNoLayout_ = true;
            PRIMAL_LOG_STREAM(Warn, "RHI")
                << "[D3D12CommandBuffer] PushConstants with no usable "
                << "pipeline layout (layout handle unresolved and no "
                << (compute ? "compute" : "graphics")
                << " pipeline bound) — push ignored" << std::endl;
        }
        return;
    }
    // root constants（绑定模型契约 §3 尾参）；offset/size 按 32 位字对齐。
    const D3D12RootSignatureLayout& rootLayout = targetLayout->GetRootLayout();
    if (rootLayout.constantsParam == D3D12SetRootParams::kInvalidRootParam) {
        return;   // 该布局无 push constant range（合法：M5 剔除布局即如此）
    }
    // 目标槽自愈：布局的签名与该槽已设不同步时，以布局签名为准重设。
    // R21-B：①判定升级为「指针+世代」Matches（跨 Reset 的同指针不再命中）；
    // ②该路径 = 宿主传入布局与绑定管线布局不同源的显式信号——Set*RootSignature
    // 会作废该槽此前已绑的根参数（表/常量），且后续 Draw 的 Ensure 会切回
    // 管线自身签名（常量被换签名解读）。M1-M6 语料全部同源（handle 即管线
    // 布局）不应走到；一次性 Warn 留痕，二十测日志可直接 grep 定位宿主错配。
    if (ID3D12RootSignature* layoutRs = targetLayout->GetRootSignature()) {
        D3D12BindSlotMirror<ID3D12RootSignature*>& slotSet =
            compute ? computeRootSigSet_ : graphicsRootSigSet_;
        if (!slotSet.Matches(layoutRs, listGeneration_)) {
            if (!warnedPushRsDiverge_) {
                warnedPushRsDiverge_ = true;
                PRIMAL_LOG_STREAM(Warn, "RHI")
                    << "[D3D12CommandBuffer] PushConstants: layout root "
                    << "signature differs from the one currently set on the "
                    << (compute ? "compute" : "graphics")
                    << " slot — switching to the layout's signature (root "
                    << "tables/constants previously set under the old "
                    << "signature are invalidated; check that the layout "
                    << "handle passed here belongs to the bound pipeline)"
                    << std::endl;
            }
            if (compute) {
                commandList_->SetComputeRootSignature(layoutRs);
            } else {
                commandList_->SetGraphicsRootSignature(layoutRs);
            }
            slotSet.Set(layoutRs, listGeneration_);
        }
    }
    // PSO 槽：目标侧有绑定镜像且当前槽位不同步时补设（无镜像不强行切
    // PSO——常量语义不依赖 PSO，仅依赖签名；保持 Draw/Dispatch 各自的
    // Ensure 语义）。
    const u32 wordOffset = offset / 4;
    const u32 wordCount = size / 4;
    if (compute) {
        commandList_->SetComputeRoot32BitConstants(
            rootLayout.constantsParam, wordCount, pValues, wordOffset);
    } else {
        commandList_->SetGraphicsRoot32BitConstants(
            rootLayout.constantsParam, wordCount, pValues, wordOffset);
    }
}

void D3D12CommandBufferImpl::SetComputeBytes(u32 /*index*/, const void* /*data*/, u32 /*size*/) {
    // Metal setBytes 语义的 compute 小常量。M1：D3D12 侧统一走 PushConstants
    // （root constants）；本入口显式不支持（不静默）。
    if (!warnedComputeBytes_) {
        warnedComputeBytes_ = true;
        PRIMAL_LOG_STREAM(Warn, "RHI")
            << "[D3D12CommandBuffer] SetComputeBytes NOT-IMPLEMENTED in M1 — "
            << "use PushConstants (root constants)" << std::endl;
    }
}

void D3D12CommandBufferImpl::WriteTimestamp(QueryPoolHandle, u32) {
    // M1：query pool 未接通（F1 清单无 query 项；timestamp 观测走
    // RHIPassTimestampAudit 桩——D3D12BackendLinks）。M3+ 接管。
    PRIMAL_LOG_STREAM(Warn, "RHI")
        << "[D3D12CommandBuffer] WriteTimestamp NOT-IMPLEMENTED in M1" << std::endl;
}

// ============================================================================
// 绘制 / 计算
// ============================================================================

void D3D12CommandBufferImpl::Draw(u32 vertexCount, u32 startVertex,
                                  u32 instanceCount, u32 startInstance) {
    if (!commandList_) return;
    if (!EnsureGraphicsState()) return;   // R20-A：签名/PSO 自愈 + 守卫
    UpdateStats(CommandType::Draw);
    commandList_->DrawInstanced(vertexCount, instanceCount, startVertex, startInstance);
}

void D3D12CommandBufferImpl::DrawIndexed(u32 indexCount, u32 startIndex,
                                         u32 baseVertex, u32 instanceCount,
                                         u32 startInstance) {
    if (!commandList_) return;
    if (!EnsureGraphicsState()) return;   // R20-A：签名/PSO 自愈 + 守卫
    UpdateStats(CommandType::DrawIndexed);
    commandList_->DrawIndexedInstanced(indexCount, instanceCount, startIndex,
                                       baseVertex, startInstance);
}

// M5 F5-2 重写：M1 版为 legacy DrawInstancedIndirect 逐命令循环，且步长
// 注释断言 32B 有误（D3D12_DRAW_ARGUMENTS=VkDrawIndirectCommand=4×u32=16B）。
// M1-M4 宿主仅 drawCount=1 单命令路径，未触发错步长；本阶段改为正规
// ID3D12CommandSignature(DRAW) + ExecuteIndirect 单调用：
//   - arg 布局/VkDrawIndirectCommand 单一事实源，16B 步长（见 D3D12Device）；
//   - count 语义：RHI 无 countBuffer 参数 → MaxCommandCount=drawCount、
//     CountBuffer=nullptr（GPU 侧 count 缩放=无；Vulkan 非 count 变体等价）。
//     D3D12 ExecuteIndirect 的 count buffer 是"arg buffer 外的独立 u32 缩放
//     词"（CountBufferOffset 指向处），与 Vulkan DrawIndexedIndirectCount 的
//     独立 countBuffer+countOffset 语义同族但字节位置约定不同；RHI 接口
//     两者皆未暴露 → 直译零适配（登记：count 变体接入时需扩 RHI 接口）。
//   - arg buffer 状态：经 tracker Transition→INDIRECT_ARGUMENT（buffer 已
//     于 createBufferImpl 注册，COMMON 起点=读类提升零 barrier；compute 写
//     后经调用方 InsertBarrier(UAV→INDIRECT_ARGUMENT) 显式转换时 tracker
//     已处 INDIRECT_ARGUMENT，此处幂等零 op）。写入→读取的执行序由调用方
//     MemoryBarrier（全局 UAV barrier）保证。
void D3D12CommandBufferImpl::DrawIndirect(ResourceHandle buffer, u64 offset, u32 drawCount) {
    if (!commandList_) return;
    D3D12Device& dev = static_cast<D3D12Device&>(device_);
    D3D12Buffer* buf = dev.GetBuffer(buffer);
    if (!buf) return;
    ID3D12CommandSignature* sig = dev.GetDrawIndirectSignature();
    if (!sig) return;
    if (!EnsureGraphicsState()) return;   // R20-A：签名/PSO 自愈 + 守卫
    D3D12SubresourceKey key{};
    key.resource = buffer;
    key.subresource = 0;
    key.isTexture = false;
    EmitTrackerOps(dev.GetStateTracker().Transition(key, ResourceState::IndirectArgument),
                   buf->GetNativeResource());
    UpdateStats(CommandType::DrawIndirect);
    commandList_->ExecuteIndirect(sig, drawCount, buf->GetNativeResource(), offset,
                                  nullptr, 0);
}

// M5 F5-2 新增：索引间接绘制。CommandSignature(DRAW_INDEXED) 20B 步长 =
// VkDrawIndexedIndirectCommand 布局（indexCount/instanceCount/firstIndex/
// vertexOffset/firstInstance ≡ D3D12 字段序）——零翻译直译。count 语义与
// 状态流转说明同上 DrawIndirect 注释。
void D3D12CommandBufferImpl::DrawIndexedIndirect(ResourceHandle buffer, u64 offset,
                                                 u32 drawCount) {
    if (!commandList_) return;
    D3D12Device& dev = static_cast<D3D12Device&>(device_);
    D3D12Buffer* buf = dev.GetBuffer(buffer);
    if (!buf) return;
    ID3D12CommandSignature* sig = dev.GetDrawIndexedIndirectSignature();
    if (!sig) return;
    if (!EnsureGraphicsState()) return;   // R20-A：签名/PSO 自愈 + 守卫（M5 GPU-driven 直绘消费点）
    D3D12SubresourceKey key{};
    key.resource = buffer;
    key.subresource = 0;
    key.isTexture = false;
    EmitTrackerOps(dev.GetStateTracker().Transition(key, ResourceState::IndirectArgument),
                   buf->GetNativeResource());
    UpdateStats(CommandType::DrawIndirect);
    commandList_->ExecuteIndirect(sig, drawCount, buf->GetNativeResource(), offset,
                                  nullptr, 0);
}

void D3D12CommandBufferImpl::Dispatch(u32 groupCountX, u32 groupCountY, u32 groupCountZ) {
    if (!commandList_) return;
    // R20-A：Dispatch 前置——compute 槽（PSO+根签名）按镜像自愈；会话内
    // 未绑定 compute 管线则跳过录制（原实现直通 = debug layer #952
    // COMMAND_LIST_DISPATCH_ROOT_SIGNATURE_NOT_SET 的必错形态）。
    if (!EnsureComputeState()) return;
    UpdateStats(CommandType::Dispatch);
    commandList_->Dispatch(groupCountX, groupCountY, groupCountZ);
}

// [R6/Windows] 原实现误调不存在的 ID3D12GraphicsCommandList::DispatchIndirect
// （MSVC C2039）。改为与 DrawIndirect 同款的 DISPATCH CommandSignature +
// ExecuteIndirect 路径（签名惰性创建见 D3D12Device）。RHI 接口无 count 参数 →
// MaxCommandCount=1、CountBuffer=nullptr（单命令，Vulkan 非count变体等价）。
// arg buffer 状态流转与 DrawIndirect 注释相同：INDIRECT_ARGUMENT 提升幂等。
void D3D12CommandBufferImpl::DispatchIndirect(ResourceHandle buffer, u64 offset) {
    if (!commandList_) return;
    D3D12Device& dev = static_cast<D3D12Device&>(device_);
    D3D12Buffer* buf = dev.GetBuffer(buffer);
    if (!buf) return;
    ID3D12CommandSignature* sig = dev.GetDispatchIndirectSignature();
    if (!sig) return;
    if (!EnsureComputeState()) return;   // R20-A：签名/PSO 自愈 + 守卫
    D3D12SubresourceKey key{};
    key.resource = buffer;
    key.subresource = 0;
    key.isTexture = false;
    EmitTrackerOps(dev.GetStateTracker().Transition(key, ResourceState::IndirectArgument),
                   buf->GetNativeResource());
    UpdateStats(CommandType::DispatchIndirect);
    commandList_->ExecuteIndirect(sig, 1, buf->GetNativeResource(), offset, nullptr, 0);
}

// ============================================================================
// 拷贝
// ============================================================================

void D3D12CommandBufferImpl::CopyBuffer(ResourceHandle src, ResourceHandle dst,
                                        u64 srcOffset, u64 dstOffset, u64 size) {
    if (!commandList_) return;
    D3D12Device& dev = static_cast<D3D12Device&>(device_);
    D3D12Buffer* srcBuf = dev.GetBuffer(src);
    D3D12Buffer* dstBuf = dev.GetBuffer(dst);
    if (!srcBuf || !dstBuf) return;
    UpdateStats(CommandType::CopyBuffer);
    const u64 bytes = size != 0 ? size
                      : (srcBuf->GetBufferDesc().size > srcOffset
                             ? srcBuf->GetBufferDesc().size - srcOffset : 0);
    if (bytes == 0) return;
    // R21-A（M5 #538 修复）：tracker 驱动的拷贝态显式转换（对齐 R17-A
    // CopyTextureToBuffer 先例）。M1 的"零 barrier + COMMON 提升/提交衰减"
    // 只对 COMMON 起点成立；帧内经 UAV/INDIRECT_ARGUMENT 流转的 buffer
    //（M5 args/counter——R19-1 UAV 标志 DEFAULT 堆资源，compute dispatch
    // 写入后 tracker 处 UAV/INDIRECT_ARGUMENT）以当前态直拷 = Windows
    // 二十测 #538 CopyBufferRegion 源态错。规则：
    //   - tracker 有条目且现态≠COMMON（General）：pre Transition
    //     (CopySource/CopyDest) 产出显式 op（与现态任意合法对），拷贝后
    //     Transition 回 QueryState 存下的快照态（保留调用方既有态契约，
    //     对齐 R17-A post 回置）；
    //   - 现态=COMMON 或 tracker 无条目：保持 M1 零 op 行为——buffer 的
    //     COMMON→COPY_SOURCE/COPY_DEST 属可提升面（二十测 M5 帧 1 compute
    //     dispatch 的 COMMON→UAV 隐式提升先行成功即实证）；UPLOAD/READBACK
    //     堆 buffer 恒 GENERIC_READ/COPY_DEST 规定态，对其发显式 transition
    //     反而非法，快照=COMMON 的跳过分支同时覆盖该面。UAV 标志资源在
    //     tracker 无条目时（createBufferImpl 全量注册，理论不存在）无法
    //     产出 op，同样维持零 op。
    D3D12SubresourceKey srcKey{};
    srcKey.resource = src;
    srcKey.subresource = 0;
    srcKey.isTexture = false;
    D3D12SubresourceKey dstKey{};
    dstKey.resource = dst;
    dstKey.subresource = 0;
    dstKey.isTexture = false;
    ResourceState srcPrev = ResourceState::Unknown;
    ResourceState dstPrev = ResourceState::Unknown;
    const bool srcExplicit = dev.GetStateTracker().QueryState(srcKey, srcPrev)
        && srcPrev != ResourceState::General;   // General = COMMON（位值 0）
    const bool dstExplicit = dev.GetStateTracker().QueryState(dstKey, dstPrev)
        && dstPrev != ResourceState::General;
    if (srcExplicit) {
        EmitTrackerOps(dev.GetStateTracker().Transition(srcKey, ResourceState::CopySource),
                       srcBuf->GetNativeResource());
    }
    if (dstExplicit) {
        EmitTrackerOps(dev.GetStateTracker().Transition(dstKey, ResourceState::CopyDest),
                       dstBuf->GetNativeResource());
    }
    commandList_->CopyBufferRegion(dstBuf->GetNativeResource(), dstOffset,
                                   srcBuf->GetNativeResource(), srcOffset, bytes);
    if (srcExplicit) {
        EmitTrackerOps(dev.GetStateTracker().Transition(srcKey, srcPrev),
                       srcBuf->GetNativeResource());
    }
    if (dstExplicit) {
        EmitTrackerOps(dev.GetStateTracker().Transition(dstKey, dstPrev),
                       dstBuf->GetNativeResource());
    }
}

void D3D12CommandBufferImpl::CopyBufferToTexture(ResourceHandle srcBuffer,
                                                 ResourceHandle dstTexture,
                                                 const BufferTextureCopyRegion* regions,
                                                 u32 regionCount) {
    if (!commandList_ || !regions || regionCount == 0) return;
    D3D12Device& dev = static_cast<D3D12Device&>(device_);
    D3D12Buffer* buf = dev.GetBuffer(srcBuffer);
    D3D12Texture* tex = dev.GetTexture(dstTexture);
    if (!buf || !tex) return;
    UpdateStats(CommandType::CopyBufferToTexture);

    // 目标 → COPY_DEST（tracker 幂等）；拷贝后保持 COPY_DEST（对齐 Vulkan
    // 的 TRANSFER_DST 留置语义——后续状态由调用方 InsertBarrier 驱动）。
    TransitionTexture(tex, dstTexture, ResourceState::CopyDest,
                      kD3D12ResourceBarrierAllSubresources);

    const D3D12_RESOURCE_DESC texDesc = tex->GetNativeDesc();
    const u64 texelBytes = tex->BytesPerTexel();
    for (u32 i = 0; i < regionCount; ++i) {
        const BufferTextureCopyRegion& r = regions[i];
        // 源布局：bufferRowLength==0 = 紧密排列（按 extent 宽计算）。
        // footprint 宽度用源行宽（texel），RowPitch = align(宽×texel, 256)。
        const UINT srcRowTexels = r.bufferRowLength ? r.bufferRowLength
                                                    : r.imageExtent.width;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        footprint.Offset = r.bufferOffset;
        footprint.Footprint.Format = texDesc.Format;
        footprint.Footprint.Width = srcRowTexels;
        footprint.Footprint.Height = r.imageExtent.height;
        footprint.Footprint.Depth = r.imageExtent.depth;
        const u64 rowBytes = u64(srcRowTexels) * texelBytes;
        footprint.Footprint.RowPitch = static_cast<UINT>((rowBytes + 255) & ~u64(255));

        D3D12_TEXTURE_COPY_LOCATION srcLoc{};
        srcLoc.pResource = buf->GetNativeResource();
        srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcLoc.PlacedFootprint = footprint;

        D3D12_TEXTURE_COPY_LOCATION dstLoc{};
        dstLoc.pResource = tex->GetNativeResource();
        dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dstLoc.SubresourceIndex = r.imageSubresource.mipLevel
                                  + texDesc.MipLevels * r.imageSubresource.baseArrayLayer;

        D3D12_BOX box{};
        box.left = static_cast<UINT>(r.imageOffset.x);
        box.top = static_cast<UINT>(r.imageOffset.y);
        box.front = static_cast<UINT>(r.imageOffset.z);
        box.right = box.left + r.imageExtent.width;
        box.bottom = box.top + r.imageExtent.height;
        box.back = box.front + r.imageExtent.depth;
        commandList_->CopyTextureRegion(&dstLoc, box.left, box.top, box.front,
                                        &srcLoc, &box);
    }
}

void D3D12CommandBufferImpl::CopyTextureToBuffer(ResourceHandle srcTexture,
                                                 ResourceHandle dstBuffer,
                                                 const BufferTextureCopyRegion* regions,
                                                 u32 regionCount) {
    if (!commandList_ || !regions || regionCount == 0) return;
    D3D12Device& dev = static_cast<D3D12Device&>(device_);
    D3D12Texture* tex = dev.GetTexture(srcTexture);
    D3D12Buffer* buf = dev.GetBuffer(dstBuffer);
    if (!tex || !buf) return;
    UpdateStats(CommandType::CopyTextureToBuffer);

    // R17-A：pre/post 状态转换按 region 的线性子资源索引作用域化，post
    // 回置各子资源的"读前态"（不再无条件整资源收敛 ShaderResource）。
    // 原实现（ALL 键 pre→COPY_SOURCE / post→ShaderResource）是 Windows
    // 十六测 M1 #538 的 tracker/GPU 分歧源（macOS 探针 P3/P4 实证）：
    //   ① ALL 键 pre 把未被本次拷贝读取的子资源在 tracker 里零 barrier
    //      "提升"成 COPY_SOURCE——GPU 只在使用点提升实际被读的子资源，
    //      未读的仍 COMMON；post 的显式 COPY_SOURCE→SRV barrier 于是对
    //      GPU 实际 COMMON 的子资源携带错误 StateBefore（barrier 拒绝/
    //      tracker 与 GPU 各执一词）。
    //   ② 无条件回置 ShaderResource 破坏 RT/DS/UAV 纹理的既有态（调用方
    //      自管收尾 barrier 时与本函数内部转换打架——M1 帧尾读回的宿主
    //      RT→CS / CS→RT 三明治即该形态）。
    // 修法 = tracker 驱动的显式转换（任务核心原则）：pre 逐子资源
    // →COPY_SOURCE（tracker 记录态≠所需态即发显式 barrier，含
    // COPY_DEST→COPY_SOURCE / SRV→COPY_SOURCE 任意合法对）；post 逐子
    // 资源回置 QueryState 存下的读前态（未注册/查询失败回退 SRV 兼容
    // 旧尾态语义）。
    const D3D12_RESOURCE_DESC texDesc = tex->GetNativeDesc();
    const u64 texelBytes = tex->BytesPerTexel();

    utl::vector<u32> subs;              // 去重后的 region 线性子资源（录制序）
    utl::vector<ResourceState> prev;    // 对应读前态（post 回置目标）
    for (u32 i = 0; i < regionCount; ++i) {
        const u32 sub = regions[i].imageSubresource.mipLevel
                        + texDesc.MipLevels * regions[i].imageSubresource.baseArrayLayer;
        bool dup = false;
        for (u32 j = 0; j < subs.size(); ++j) dup = dup || (subs[j] == sub);
        if (dup) continue;
        subs.push_back(sub);
        ResourceState pre = ResourceState::ShaderResource;  // 查询失败回退（旧尾态）
        D3D12SubresourceKey key{};
        key.resource = srcTexture;
        key.subresource = sub;
        key.isTexture = true;
        // 注意 QueryState 在 bits 不可映射时会先覆写 out=Unknown 再返回
        // false——查询后仍需兜底（Unknown 不可作 Transition 目标）。
        if (!dev.GetStateTracker().QueryState(key, pre) || pre == ResourceState::Unknown) {
            pre = ResourceState::ShaderResource;
        }
        prev.push_back(pre);
    }
    for (u32 j = 0; j < subs.size(); ++j) {
        TransitionTexture(tex, srcTexture, ResourceState::CopySource, subs[j]);
    }

    for (u32 i = 0; i < regionCount; ++i) {
        const BufferTextureCopyRegion& r = regions[i];
        const UINT srcRowTexels = r.bufferRowLength ? r.bufferRowLength
                                                    : r.imageExtent.width;
        D3D12_TEXTURE_COPY_LOCATION srcLoc{};
        srcLoc.pResource = tex->GetNativeResource();
        srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        srcLoc.SubresourceIndex = r.imageSubresource.mipLevel
                                  + texDesc.MipLevels * r.imageSubresource.baseArrayLayer;

        D3D12_TEXTURE_COPY_LOCATION dstLoc{};
        dstLoc.pResource = buf->GetNativeResource();
        dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dstLoc.PlacedFootprint.Offset = r.bufferOffset;
        dstLoc.PlacedFootprint.Footprint.Format = texDesc.Format;
        dstLoc.PlacedFootprint.Footprint.Width = srcRowTexels;
        dstLoc.PlacedFootprint.Footprint.Height = r.imageExtent.height;
        dstLoc.PlacedFootprint.Footprint.Depth = r.imageExtent.depth;
        const u64 rowBytes = u64(srcRowTexels) * texelBytes;
        dstLoc.PlacedFootprint.Footprint.RowPitch =
            static_cast<UINT>((rowBytes + 255) & ~u64(255));

        // R18-A：CopyTextureRegion 签名 = (pDst, DstX/Y/Z, pSrc, pSrcBox)，
        // 第一参是【目标】。原调用 (&srcLoc, imageOffset…, &dstLoc, nullptr)
        // 把纹理 location 放进 pDst 槽——GPU 实际执行 buffer→texture（纹理
        // =目标，要求 COPY_DEST），而上方 pre 转换已按读回意图把纹理转进
        // COPY_SOURCE（纹理=源的语义本身正确）→ 合法 barrier 发出后拷贝
        // 即被 debug-layer 拦下：Windows 十七测 M1 #538（十六测同签名现象
        // 同源）。修正实参序：
        //   pDst = buffer footprint（buffer 目标无纹素坐标，DstX/Y/Z 恒 0，
        //   缓冲内偏移由 PlacedFootprint.Offset 携带）；
        //   pSrc = texture 子资源，选区 = 源空间 box（imageOffset..+extent，
        //   vkCmdCopyImageToBuffer 的 imageOffset/imageExtent 直译）。
        D3D12_BOX box{};
        box.left = static_cast<UINT>(r.imageOffset.x);
        box.top = static_cast<UINT>(r.imageOffset.y);
        box.front = static_cast<UINT>(r.imageOffset.z);
        box.right = box.left + r.imageExtent.width;
        box.bottom = box.top + r.imageExtent.height;
        box.back = box.front + r.imageExtent.depth;
        commandList_->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, &box);
    }

    for (u32 j = 0; j < subs.size(); ++j) {
        TransitionTexture(tex, srcTexture, prev[j], subs[j]);
    }
}

void D3D12CommandBufferImpl::BlitTexture(ResourceHandle src, ResourceHandle dst,
                                          const TextureBlitRegion* regions,
                                          u32 regionCount, FilterMode) {
    // M7 F7-3：同格式 1:1 blit = CopyTextureRegion 直译（Windows-only）。
    // 语义对齐 VulkanCommandBuffer::BlitTexture 的整资源收敛模型：
    //   整资源 src→COPY_SOURCE / dst→COPY_DEST → 逐 region 拷贝 →
    //   两端收敛回 ShaderResource（Vulkan P0-B 同款收尾）。
    // 显式不支持（NOT-IMPLEMENTED 登记，不静默）：
    //   ① 格式转换 blit（src/dst DXGI 不同——D3D12 CopyTextureRegion 仅
    //      支持同格式或同 typeless 家族；跨家族转换需 draw/compute 内核）；
    //   ② 缩放 blit（srcOffsets/dstOffsets 尺寸不同——vkCmdBlitImage 的
    //      硬件过滤能力在 D3D12 无直译；filter 参数因此被忽略并警告）。
    if (!commandList_ || !regions || regionCount == 0) return;
    D3D12Device& dev = static_cast<D3D12Device&>(device_);
    D3D12Texture* srcTex = dev.GetTexture(src);
    D3D12Texture* dstTex = dev.GetTexture(dst);
    if (!srcTex || !dstTex) return;
    UpdateStats(CommandType::BlitTexture);

    if (srcTex->GetTextureDesc().format != dstTex->GetTextureDesc().format) {
        PRIMAL_LOG_STREAM(Error, "RHI")
            << "[D3D12CommandBuffer] BlitTexture format conversion "
            << "NOT-IMPLEMENTED (src/dst formats differ) — copy skipped" << std::endl;
        return;
    }
    bool anyScale = false;
    for (u32 i = 0; i < regionCount; ++i) {
        const TextureBlitRegion& r = regions[i];
        const s64 sw = s64(r.srcOffsets[1].x) - r.srcOffsets[0].x;
        const s64 sh = s64(r.srcOffsets[1].y) - r.srcOffsets[0].y;
        const s64 dw = s64(r.dstOffsets[1].x) - r.dstOffsets[0].x;
        const s64 dh = s64(r.dstOffsets[1].y) - r.dstOffsets[0].y;
        if (sw != dw || sh != dh) { anyScale = true; break; }
    }
    if (anyScale) {
        PRIMAL_LOG_STREAM(Error, "RHI")
            << "[D3D12CommandBuffer] BlitTexture scaled/filter blit "
            << "NOT-IMPLEMENTED (D3D12 has no vkCmdBlitImage equivalent; "
            << "1:1 same-format copy only) — copy skipped" << std::endl;
        return;
    }

    // 整资源收敛（tracker 幂等；Vulkan 侧 TransitionImageLayout 整图同构）。
    TransitionTexture(srcTex, src, ResourceState::CopySource,
                      kD3D12ResourceBarrierAllSubresources);
    TransitionTexture(dstTex, dst, ResourceState::CopyDest,
                      kD3D12ResourceBarrierAllSubresources);

    const D3D12_RESOURCE_DESC srcDesc = srcTex->GetNativeDesc();
    for (u32 i = 0; i < regionCount; ++i) {
        const TextureBlitRegion& r = regions[i];
        D3D12_TEXTURE_COPY_LOCATION srcLoc{};
        srcLoc.pResource = srcTex->GetNativeResource();
        srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        srcLoc.SubresourceIndex = r.srcSubresource.mipLevel
            + srcDesc.MipLevels * r.srcSubresource.baseArrayLayer;

        D3D12_TEXTURE_COPY_LOCATION dstLoc{};
        dstLoc.pResource = dstTex->GetNativeResource();
        dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dstLoc.SubresourceIndex = r.dstSubresource.mipLevel
            + dstTex->GetNativeDesc().MipLevels * r.dstSubresource.baseArrayLayer;

        // 1:1 无缩放：BOX 以源区间为准（CopyTextureRegion 禁止同一 box
        // 同时约束 src/dst 两端——dst 由 DstX/Y/Z 定位）。
        D3D12_BOX box{};
        box.left = static_cast<UINT>(r.srcOffsets[0].x);
        box.top = static_cast<UINT>(r.srcOffsets[0].y);
        box.front = static_cast<UINT>(r.srcOffsets[0].z);
        box.right = static_cast<UINT>(r.srcOffsets[1].x);
        box.bottom = static_cast<UINT>(r.srcOffsets[1].y);
        box.back = static_cast<UINT>(r.srcOffsets[1].z);
        commandList_->CopyTextureRegion(
            &dstLoc, r.dstOffsets[0].x, r.dstOffsets[0].y, r.dstOffsets[0].z,
            &srcLoc, &box);
    }

    // 收敛回采样态（Vulkan P0-B 同款）。
    TransitionTexture(srcTex, src, ResourceState::ShaderResource,
                      kD3D12ResourceBarrierAllSubresources);
    TransitionTexture(dstTex, dst, ResourceState::ShaderResource,
                      kD3D12ResourceBarrierAllSubresources);
}

void D3D12CommandBufferImpl::GenerateMipmaps(ResourceHandle texture) {
    // R13/T2：canonical PS downsample（M7 F7-3 移交项的 Windows 侧接通）。
    // 方案：每级 mip 一次全屏三角形 draw——src mip（i）经 1-mip SRV 绑 t0、
    // dst mip（i+1）绑 RTV；PS 双线性采样在半尺寸目标纹素中心 = src 2x2
    // box 均值（与 Vulkan 侧 vkCmdBlitImage LINEAR blit 链同档滤波）。
    // shader 字节=构建期嵌入头（MipGen.hlsl → dxc/fxc → cso → 字节数组，
    // Engine/CMakeLists.txt 编译块；构建机无 HLSL 编译器 → 嵌入为空 → 此处
    // 保持 NOT-IMPLEMENTED Error，显式降级不静默）。
    if (!commandList_) return;
    D3D12Device& dev = static_cast<D3D12Device&>(device_);
    d3d12::D3D12Texture* tex = dev.GetTexture(texture);
    if (!tex || !tex->GetNativeResource()) return;

    const TextureDesc& td = tex->GetTextureDesc();
    const u32 mipCount = td.mipLevels;
    if (mipCount <= 1) return;  // Vulkan 版同款 no-op

    // 支持面（对齐 Vulkan"仅 2D、full-mipchain"简化）：Texture2D 单 slice +
    // 格式支持 RTV（CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT) 拦截
    // BC 压缩/深度/TYPELESS——BC 无 RTV 能力，mip 生成保持 NOT-IMPLEMENTED
    // Error 并登记后续）。
    if (td.type != TextureType::Texture2D) {
        PRIMAL_LOG_STREAM(Error, "RHI")
            << "[D3D12CommandBuffer] GenerateMipmaps NOT-IMPLEMENTED for "
               "texture type " << static_cast<u32>(td.type)
            << " (Texture2D only, Vulkan parity) — skipped" << std::endl;
        return;
    }
    const u32 dxgi = d3d12::ToDxgiFormat(td.format);
    // D3D12 无 ID3D12Device::CheckFormatSupport（那是 D3D11 API；R14-1 修
    // 正 MSVC C2039/C2065）——能力查询走 CheckFeatureSupport +
    // D3D12_FEATURE_DATA_FORMAT_SUPPORT（Support1 含 RENDER_TARGET 位）。
    D3D12_FEATURE_DATA_FORMAT_SUPPORT formatSupport{};
    formatSupport.Format = static_cast<DXGI_FORMAT>(dxgi);
    if (FAILED(dev.GetNativeDevice()->CheckFeatureSupport(
            D3D12_FEATURE_FORMAT_SUPPORT, &formatSupport, sizeof(formatSupport)))
        || (formatSupport.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET) == 0) {
        PRIMAL_LOG_STREAM(Error, "RHI")
            << "[D3D12CommandBuffer] GenerateMipmaps NOT-IMPLEMENTED for "
               "format 0x" << std::hex << dxgi << std::dec
            << " (no RTV support: BC-compressed/depth/TYPELESS family — "
               "registered as follow-up) — skipped" << std::endl;
        return;
    }
    // R15-1 层2（RTV 守卫）：格式能力门之上再核资源标志——资源无
    // ALLOW_RENDER_TARGET 时 CreateRenderTargetView 是 debug-layer 必错项
    //（#42 CREATERENDERTARGETVIEW_INVALIDRESOURCE；Windows 十四测唯一渲染
    // 阻塞，CDB 实证本函数 mip 循环内的 RTV 创建点）。层1 已在创建期为
    // mips>1 采样颜色纹理增补该标志；此处兜底残余入口（wrap/view 模式
    // 资源、usage 无 ShaderResource 位等），Warn 显式跳过不生成 mip——对齐
    // Vulkan 侧对不可服务纹理的跳过观感（其 blit 链因隐式 TRANSFER 标志
    // 从不缺能力，mips<=1 为静默 no-op）。
    if ((tex->GetNativeDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0) {
        PRIMAL_LOG_STREAM(Warn, "RHI")
            << "[D3D12CommandBuffer] GenerateMipmaps skipped: texture '"
            << td.name << "' resource lacks D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET "
               "(usage=" << static_cast<u32>(td.usage) << ", mips=" << mipCount
            << ") — RTV creation would raise debug-layer #42" << std::endl;
        return;
    }
    ID3D12PipelineState* pso = dev.GetMipGenerator().GetOrCreatePipeline(dxgi);
    if (!pso || !dev.GetMipGenerator().GetRootSignature()) {
        PRIMAL_LOG_STREAM(Error, "RHI")
            << "[D3D12CommandBuffer] GenerateMipmaps NOT-IMPLEMENTED "
               "(embedded shader bytes missing or PSO creation failed — "
               "build machine had no dxc/fxc) — skipped" << std::endl;
        return;
    }
    UpdateStats(CommandType::GenerateMipmaps);

    // SRV 描述符：每 mip 独占槽（同帧各 draw 引用不同槽，无覆写冒险），
    // 帧槽线性区分配（跨帧护栏与 set 实例区段同款）。R13/T1 双写：先落
    // shadow 再整段镜像 visible。
    d3d12::D3D12DescriptorHeapManager& heaps = dev.GetDescriptorHeaps();
    const u32 srvCount = mipCount - 1;
    u32 srvBase = d3d12::D3D12DescriptorHeapManager::kInvalidSlot;
    if (!heaps.AllocShaderVisible(srvCount, srvBase)) {
        PRIMAL_LOG_STREAM(Error, "RHI")
            << "[D3D12CommandBuffer] GenerateMipmaps frame-slot descriptor "
               "capacity exhausted — skipped" << std::endl;
        return;
    }
    ID3D12Device* native = dev.GetNativeDevice();
    for (u32 m = 0; m < srvCount; ++m) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = static_cast<DXGI_FORMAT>(dxgi);
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MostDetailedMip = m;   // src = 第 m 级
        srv.Texture2D.MipLevels = 1;
        srv.Texture2D.PlaneSlice = 0;
        srv.Texture2D.ResourceMinLODClamp = 0.0f;
        native->CreateShaderResourceView(tex->GetNativeResource(), &srv,
                                         heaps.CbvSrvUavShadowCpuHandle(srvBase + m));
    }
    heaps.CopyCbvSrvUavShadowToVisible(srvBase, srvCount);

    // RTV 槽复用：RTV/DSV 描述符在 OMSetRenderTargets 录制期即被解析进
    // 命令流（非 shader-visible 堆无法被 GPU 侧延迟读取），录制后立即改写
    // 安全——单槽逐 mip 重写即可。
    const u32 rtvSlot = heaps.AllocRtv();
    if (rtvSlot == d3d12::D3D12DescriptorHeapManager::kInvalidSlot) {
        PRIMAL_LOG_STREAM(Error, "RHI")
            << "[D3D12CommandBuffer] GenerateMipmaps RTV slot exhausted — "
               "skipped" << std::endl;
        return;
    }

    // 状态机（tracker 驱动，逐子资源）：mip i → PIXEL_SHADER_RESOURCE 读、
    // mip i+1 → RENDER_TARGET 写；画完 mip i+1 → PIXEL_SHADER_RESOURCE
    //（下一轮读源 + 终态）。上传路径后 tracker 已在 ShaderResource（上传环
    // 拷贝尾转换），首轮 transition 幂等零 op。终态=全 mip ShaderResource，
    // 与 Vulkan 版 SHADER_READ_ONLY 收敛一致。
    EnsureDescriptorHeapsBound();
    commandList_->SetGraphicsRootSignature(dev.GetMipGenerator().GetRootSignature());
    commandList_->SetPipelineState(pso);
    commandList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    // R20-A：此处直改命令列表 graphics 槽（RS/PSO/拓扑）——镜像必须同步
    // （mipgen 无 RHI 管线/布局 → graphics 侧绑定镜像置空）。否则后续
    // PushConstants/BindDescriptorSets(Graphics) 若未先重绑管线，会拿旧
    // 布局的 constantsParam/tableParam 对 mipgen 签名（仅 1 个描述符表
    // 参数）下发 = #709/#708 的参数索引错位子句；镜像置空后这些调用
    // 走「无布局」早退 + Warn，宿主缺重绑直接显形。
    // R21-B：槽位镜像经 Set(…, listGeneration_) 记入当前世代（不递增——
    // 本函数不重置命令列表，只是真实下发后的镜像记账）。
    boundGraphicsPipeline_ = nullptr;
    boundGraphicsLayout_ = handles::INVALID_PIPELINE_LAYOUT;
    graphicsRootSigSet_.Set(dev.GetMipGenerator().GetRootSignature(), listGeneration_);
    effectivePso_.Set(pso, listGeneration_);
    if (BindTraceEnabled()) {
        PRIMAL_LOG_STREAM(Info, "RHI.D3D12")
            << "[D3D12Bind] gen=" << listGeneration_
            << " mipgen direct set: rs=" << dev.GetMipGenerator().GetRootSignature()
            << " pso=" << pso << std::endl;
    }
    for (u32 m = 0; m < srvCount; ++m) {
        const u32 dstW = td.size.x > (1u << (m + 1)) ? td.size.x >> (m + 1) : 1u;
        const u32 dstH = td.size.y > (1u << (m + 1)) ? td.size.y >> (m + 1) : 1u;

        TransitionTexture(tex, texture, ResourceState::ShaderResource, m);
        TransitionTexture(tex, texture, ResourceState::RenderTarget, m + 1);

        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
        rtvDesc.Format = static_cast<DXGI_FORMAT>(dxgi);
        rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        rtvDesc.Texture2D.MipSlice = static_cast<UINT>(m + 1);
        rtvDesc.Texture2D.PlaneSlice = 0;
        native->CreateRenderTargetView(tex->GetNativeResource(), &rtvDesc,
                                       heaps.RtvCpuHandle(rtvSlot));

        // 正高度视口：mip gen 无呈现 Y 翻转参与（src/dst 同为纹理行主序，
        // UV 直映），与引擎交换链负高度视口契约无关。
        D3D12_VIEWPORT viewport{};
        viewport.Width = static_cast<FLOAT>(dstW);
        viewport.Height = static_cast<FLOAT>(dstH);
        viewport.MaxDepth = 1.0f;
        D3D12_RECT scissor{};
        scissor.right = static_cast<LONG>(dstW);
        scissor.bottom = static_cast<LONG>(dstH);
        commandList_->RSSetViewports(1, &viewport);
        commandList_->RSSetScissorRects(1, &scissor);
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = heaps.RtvCpuHandle(rtvSlot);
        commandList_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        commandList_->SetGraphicsRootDescriptorTable(
            0, heaps.CbvSrvUavGpuHandle(srvBase + m));
        commandList_->DrawInstanced(3, 1, 0, 0);

        TransitionTexture(tex, texture, ResourceState::ShaderResource, m + 1);
    }
    heaps.FreeRtv(rtvSlot);
}

} // namespace primal::graphics::rhi::d3d12

#endif // defined(_WIN32) && defined(ENABLE_D3D12) && ENABLE_D3D12
