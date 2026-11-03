// Windows-only: requires ENABLE_D3D12 (_WIN32)
//
// D3D12CommandBuffer.cpp — 命令录制实现（M1 F1-6）。
#include "D3D12CommandBuffer.h"

#if defined(_WIN32) && defined(ENABLE_D3D12) && ENABLE_D3D12

#include "D3D12DescriptorHeaps.h"
#include "D3D12Device.h"
#include "D3D12Pipeline.h"
#include "D3D12Texture.h"
#include "D3D12Upload.h"

#include "Engine/Common/CoreDiag.h"

#ifndef PRIMAL_LOG_STREAM
#define PRIMAL_LOG_STREAM(level, channel) primal::diag::StreamProxy(level, channel)
#endif

namespace primal::graphics::rhi::d3d12 {

// ============================================================================
// 生命周期
// ============================================================================

D3D12CommandBufferImpl::D3D12CommandBufferImpl(D3D12Device& device, CommandQueueType type)
    : RHICommandBuffer(device, type) {}

D3D12CommandBufferImpl::D3D12CommandBufferImpl(D3D12CommandBufferImpl&& other) noexcept
    : RHICommandBuffer(std::move(other)),
      currentAllocator_(other.currentAllocator_),
      boundPipeline_(other.boundPipeline_),
      boundLayout_(other.boundLayout_),
      descriptorHeapsBound_(other.descriptorHeapsBound_),
      lastExecuteTarget_(other.lastExecuteTarget_),
      isRecording_(other.isRecording_) {
    for (u32 i = 0; i < kAllocatorSlots; ++i) {
        allocators_[i] = std::move(other.allocators_[i]);
        allocatorLastUseTarget_[i] = other.allocatorLastUseTarget_[i];
    }
    commandList_ = std::move(other.commandList_);
    other.boundPipeline_ = nullptr;
    other.boundLayout_ = nullptr;
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
        boundPipeline_ = other.boundPipeline_;
        boundLayout_ = other.boundLayout_;
        descriptorHeapsBound_ = other.descriptorHeapsBound_;
        lastExecuteTarget_ = other.lastExecuteTarget_;
        isRecording_ = other.isRecording_;
        other.boundPipeline_ = nullptr;
        other.boundLayout_ = nullptr;
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
    boundPipeline_ = nullptr;
    boundLayout_ = nullptr;
}

bool D3D12CommandBufferImpl::resetImpl() {
    // GPU 侧复用判定在 beginImpl（allocator 轮转处）统一处理；这里只清绑定态。
    boundPipeline_ = nullptr;
    boundLayout_ = nullptr;
    descriptorHeapsBound_ = false;
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

    isRecording_ = true;
    dev.IncrementRecording();
    descriptorHeapsBound_ = false;
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
        // 语义：rb.beforeState = 调用方事实注入（GPU 侧已处于该态——如
        // compute dispatch 的隐式 UAV 提升），tracker 与之不符时以 Track()
        // 采纳后按目标态 Transition（产出 op / 或 COMMON 提升零 op）。
        // 未注册 buffer（M5 前手工创建路径）保持 M1 no-op 行为。
        d3d12::D3D12Buffer* bufHandle = dev.GetBuffer(rb.resource);
        if (bufHandle) {
            D3D12SubresourceKey key{};
            key.resource = rb.resource;
            key.subresource = 0;
            key.isTexture = false;
            ResourceState cur{};
            if (dev.GetStateTracker().QueryState(key, cur) && cur != rb.beforeState
                && rb.beforeState != ResourceState::Unknown) {
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
    // §7.2-1 契约：负高度 {0, H, W, -H}——D3D12 官方 Y 翻转手段，
    // 使原始帧与 Vulkan 原始读回同向；cull 翻转在 PSO（D3D12Pipeline）。
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

void D3D12CommandBufferImpl::BindGraphicsPipeline(PipelineHandle pipeline) {
    if (!commandList_) return;
    UpdateStats(CommandType::BindGraphicsPipeline);
    D3D12Device& dev = static_cast<D3D12Device&>(device_);
    D3D12Pipeline* pipe = dev.GetPipeline(pipeline);
    if (!pipe || pipe->IsCompute()) return;
    boundPipeline_ = pipe;
    boundLayout_ = pipe->GetPipelineLayout();
    commandList_->SetPipelineState(pipe->GetPso());
    if (ID3D12RootSignature* rs = pipe->GetRootSignature()) {
        commandList_->SetGraphicsRootSignature(rs);
    }
    commandList_->IASetPrimitiveTopology(pipe->GetIaTopology());
}

void D3D12CommandBufferImpl::BindComputePipeline(PipelineHandle pipeline) {
    if (!commandList_) return;
    UpdateStats(CommandType::BindComputePipeline);
    D3D12Device& dev = static_cast<D3D12Device&>(device_);
    D3D12Pipeline* pipe = dev.GetPipeline(pipeline);
    if (!pipe || !pipe->IsCompute()) return;
    boundPipeline_ = pipe;
    boundLayout_ = pipe->GetPipelineLayout();
    commandList_->SetPipelineState(pipe->GetPso());
    if (ID3D12RootSignature* rs = pipe->GetRootSignature()) {
        commandList_->SetComputeRootSignature(rs);
    }
}

void D3D12CommandBufferImpl::EnsureDescriptorHeapsBound() {
    if (descriptorHeapsBound_ || !commandList_) return;
    D3D12Device& dev = static_cast<D3D12Device&>(device_);
    ID3D12DescriptorHeap* heaps[] = {dev.GetDescriptorHeaps().GetCbvSrvUavHeap(),
                                     dev.GetDescriptorHeaps().GetSamplerHeap()};
    commandList_->SetDescriptorHeaps(2, heaps);
    descriptorHeapsBound_ = true;
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
        // 声明该槽时回退 BufferDesc.vertex.vertexStride。
        UINT stride = buffer->GetBufferDesc().vertex.vertexStride;
        if (boundPipeline_ && !boundPipeline_->IsCompute()) {
            for (const auto& vb : boundPipeline_->GetGraphicsDesc().vertexBindings) {
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

    for (u32 i = 0; i < setCount; ++i) {
        const u32 setIndex = firstSet + i;
        D3D12DescriptorSet* set = dev.GetDescriptorSet(descriptorSets[i]);
        if (!set) continue;
        // 长寿命 set 跨帧护栏：区段属旧帧槽 → 迁移到当前槽（见设备声明）。
        dev.EnsureSetRegionCurrent(set);
        // root 参数索引由绑定管线布局的映射表提供（契约 §3）。
        const D3D12RootSignatureLayout* rootLayout = boundLayout_
            ? &boundLayout_->GetRootLayout() : nullptr;
        if (!rootLayout || setIndex >= rootLayout->sets.size()) continue;
        const D3D12SetRootParams& params = rootLayout->sets[setIndex];
        const bool compute = bindPoint == PipelineBindPoint::Compute;
        if (params.tableParam != D3D12SetRootParams::kInvalidRootParam
            && set->BaseSlot() != D3D12DescriptorHeapManager::kInvalidSlot) {
            const D3D12_GPU_DESCRIPTOR_HANDLE gpu =
                dev.GetDescriptorHeaps().CbvSrvUavGpuHandle(set->BaseSlot());
            if (compute) {
                commandList_->SetComputeRootDescriptorTable(params.tableParam, gpu);
            } else {
                commandList_->SetGraphicsRootDescriptorTable(params.tableParam, gpu);
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

void D3D12CommandBufferImpl::PushConstants(PipelineLayoutHandle /*layout*/,
                                           ShaderStage /*stageFlags*/,
                                           u32 offset, u32 size, const void* pValues) {
    if (!commandList_ || !pValues || size == 0) return;
    // root constants（绑定模型契约 §3 尾参）；offset/size 按 32 位字对齐。
    const D3D12RootSignatureLayout* rootLayout = boundLayout_
        ? &boundLayout_->GetRootLayout() : nullptr;
    if (!rootLayout
        || rootLayout->constantsParam == D3D12SetRootParams::kInvalidRootParam) {
        return;
    }
    const u32 wordOffset = offset / 4;
    const u32 wordCount = size / 4;
    if (boundPipeline_ && boundPipeline_->IsCompute()) {
        commandList_->SetComputeRoot32BitConstants(
            rootLayout->constantsParam, wordCount, pValues, wordOffset);
    } else {
        commandList_->SetGraphicsRoot32BitConstants(
            rootLayout->constantsParam, wordCount, pValues, wordOffset);
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
    UpdateStats(CommandType::Draw);
    commandList_->DrawInstanced(vertexCount, instanceCount, startVertex, startInstance);
}

void D3D12CommandBufferImpl::DrawIndexed(u32 indexCount, u32 startIndex,
                                         u32 baseVertex, u32 instanceCount,
                                         u32 startInstance) {
    if (!commandList_) return;
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
    UpdateStats(CommandType::Dispatch);
    commandList_->Dispatch(groupCountX, groupCountY, groupCountZ);
}

void D3D12CommandBufferImpl::DispatchIndirect(ResourceHandle buffer, u64 offset) {
    if (!commandList_) return;
    D3D12Device& dev = static_cast<D3D12Device&>(device_);
    D3D12Buffer* buf = dev.GetBuffer(buffer);
    if (!buf) return;
    UpdateStats(CommandType::DispatchIndirect);
    commandList_->DispatchIndirect(buf->GetNativeResource(), offset);
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
    // buffer 拷贝零 barrier（promotion + 衰减，见 D3D12Buffer.h）。
    commandList_->CopyBufferRegion(dstBuf->GetNativeResource(), dstOffset,
                                   srcBuf->GetNativeResource(), srcOffset, bytes);
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

    // 源 → COPY_SOURCE；读回后收敛回 ShaderResource（对齐 Vulkan P0-B 收尾）。
    TransitionTexture(tex, srcTexture, ResourceState::CopySource,
                      kD3D12ResourceBarrierAllSubresources);

    const D3D12_RESOURCE_DESC texDesc = tex->GetNativeDesc();
    const u64 texelBytes = tex->BytesPerTexel();
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

        commandList_->CopyTextureRegion(&srcLoc, r.imageOffset.x, r.imageOffset.y,
                                        r.imageOffset.z, &dstLoc, nullptr);
    }
    TransitionTexture(tex, srcTexture, ResourceState::ShaderResource,
                      kD3D12ResourceBarrierAllSubresources);
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

void D3D12CommandBufferImpl::GenerateMipmaps(ResourceHandle) {
    // M7 F7-3 接通评估结论：NOT-IMPLEMENTED 维持，移交 Windows 集成会话。
    // 原因：D3D12 无 vkCmdBlitImage 直译（Vulkan 侧为整链 blit 硬件过滤），
    // 等价物=M5Mipmap.comp（整数 box，工具 shader 已交付，Vulkan 侧
    // RHI_TEST_MIPGEN 逐字节验证）逐 mip dispatch。接通所需：
    //   ① M5Mipmap.cso 字节进后端（嵌入头或 blob 装载——后端无文件路径
    //      上下文，且 freshness gate 需覆盖嵌入物）；
    //   ② 设备级惰性 compute PSO + 逐 mip SRV/UAV 描述符（native 侧直接
    //      建 1-mip 视图，无 RHI 视图限制）+ mip 间全局 UAV barrier。
    // 成本与风险（无 macOS 编译验证的新机制）超 M7"成本可控"线，按 PhaseDoc
    // 移交协议登记（见 D3D12_Windows_Integration_Runbook.md §7）。
    PRIMAL_LOG_STREAM(Error, "RHI")
        << "[D3D12CommandBuffer] GenerateMipmaps NOT-IMPLEMENTED "
        << "(tool shader delivered; wiring deferred to Windows session)" << std::endl;
}

} // namespace primal::graphics::rhi::d3d12

#endif // defined(_WIN32) && defined(ENABLE_D3D12) && ENABLE_D3D12
