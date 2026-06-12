#include "DawnCommandBuffer.h"

#if defined(ENABLE_WEBGPU) && ENABLE_WEBGPU

#include "DawnDevice.h"
#include "DawnTexture.h"
#include "DawnPipeline.h"
#include <iostream>
#include <cstring>
#include <chrono>

namespace primal::graphics::rhi {

// === Helper: index format conversion ===

static WGPUIndexFormat ToWGPUIndexFormat(DataFormat format) {
    switch (format) {
        case DataFormat::R16_UNorm:
        case DataFormat::R16_UInt:  return WGPUIndexFormat_Uint16;
        case DataFormat::R32_UInt:  return WGPUIndexFormat_Uint32;
        default:                    return WGPUIndexFormat_Uint32;
    }
}

// === Constructor / Destructor ===

DawnCommandBuffer::DawnCommandBuffer(DawnDevice& device, CommandQueueType type)
    : RHICommandBuffer(static_cast<RHIDeviceBase&>(device), type)
    , device_(device) {
}

DawnCommandBuffer::~DawnCommandBuffer() {
    destroyImpl();
}

bool DawnCommandBuffer::Initialize() {
    // Nothing to do at init time. The encoder is created in Begin().
    return true;
}

// === Lifecycle (override the public API to manage state directly) ===

bool DawnCommandBuffer::Reset() {
    if (GetState() == CommandBufferState::Invalid) {
        return false;
    }

    bool result = resetImpl();
    if (result) {
        SetState(CommandBufferState::Reset);
    }
    return result;
}

bool DawnCommandBuffer::Begin() {
    auto state = GetState();
    if (state != CommandBufferState::Reset && state != CommandBufferState::Executed) {
        return false;
    }

    bool result = beginImpl();
    if (result) {
        SetState(CommandBufferState::Recording);
    }
    return result;
}

bool DawnCommandBuffer::End() {
    if (GetState() != CommandBufferState::Recording) {
        return false;
    }

    bool result = endImpl();
    if (result) {
        SetState(CommandBufferState::RecordingEnded);
    }
    return result;
}

bool DawnCommandBuffer::Submit(u32 waitFlags) {
    if (GetState() != CommandBufferState::RecordingEnded) {
        return false;
    }

    bool result = submitImpl(waitFlags);
    if (result) {
        SetState(CommandBufferState::Submitted);
    }
    return result;
}

bool DawnCommandBuffer::WaitForCompletion() {
    if (GetState() != CommandBufferState::Submitted) {
        return false;
    }

    bool result = waitForCompletionImpl();
    if (result) {
        SetState(CommandBufferState::Executed);
    }
    return result;
}

// === Lifecycle (private impl) ===

void DawnCommandBuffer::destroyImpl() {
    EndCurrentEncoder();

    if (wgpuCommandBuffer_) {
        wgpuCommandBufferRelease(wgpuCommandBuffer_);
        wgpuCommandBuffer_ = nullptr;
    }
    if (wgpuEncoder_) {
        wgpuCommandEncoderRelease(wgpuEncoder_);
        wgpuEncoder_ = nullptr;
    }
}

bool DawnCommandBuffer::resetImpl() {
    EndCurrentEncoder();

    if (wgpuCommandBuffer_) {
        wgpuCommandBufferRelease(wgpuCommandBuffer_);
        wgpuCommandBuffer_ = nullptr;
    }
    if (wgpuEncoder_) {
        wgpuCommandEncoderRelease(wgpuEncoder_);
        wgpuEncoder_ = nullptr;
    }

    currentEncoderType_ = EncoderType::None;
    return true;
}

bool DawnCommandBuffer::beginImpl() {
    // Release any previous encoder / command buffer
    if (wgpuCommandBuffer_) {
        wgpuCommandBufferRelease(wgpuCommandBuffer_);
        wgpuCommandBuffer_ = nullptr;
    }
    if (wgpuEncoder_) {
        wgpuCommandEncoderRelease(wgpuEncoder_);
        wgpuEncoder_ = nullptr;
    }

    WGPUCommandEncoderDescriptor encoderDesc{};
    encoderDesc.nextInChain = nullptr;
    encoderDesc.label = ToWGPUStringView("DawnCommandEncoder");

    wgpuEncoder_ = wgpuDeviceCreateCommandEncoder(device_.GetNativeDevice(), &encoderDesc);
    if (!wgpuEncoder_) {
        std::cerr << "[DawnCommandBuffer] Failed to create command encoder" << std::endl;
        return false;
    }

    currentEncoderType_ = EncoderType::None;
    return true;
}

bool DawnCommandBuffer::endImpl() {
    // End any active render/compute pass
    EndCurrentEncoder();

    if (!wgpuEncoder_) {
        std::cerr << "[DawnCommandBuffer] End() called without encoder" << std::endl;
        return false;
    }

    WGPUCommandBufferDescriptor cbDesc{};
    cbDesc.nextInChain = nullptr;
    cbDesc.label = ToWGPUStringView("DawnCommandBuffer");

    wgpuCommandBuffer_ = wgpuCommandEncoderFinish(wgpuEncoder_, &cbDesc);
    if (!wgpuCommandBuffer_) {
        std::cerr << "[DawnCommandBuffer] Failed to finish command encoder" << std::endl;
        return false;
    }

    // The encoder is consumed by Finish; release our reference.
    wgpuCommandEncoderRelease(wgpuEncoder_);
    wgpuEncoder_ = nullptr;

    return true;
}

bool DawnCommandBuffer::submitImpl(u32 waitFlags) {
    (void)waitFlags;

    if (!wgpuCommandBuffer_) {
        std::cerr << "[DawnCommandBuffer] Submit() called without command buffer" << std::endl;
        return false;
    }

    // Submit via the device queue
    WGPUCommandBuffer cmdBufs[] = { wgpuCommandBuffer_ };
    wgpuQueueSubmit(device_.GetQueue(), 1, cmdBufs);

    return true;
}

bool DawnCommandBuffer::waitForCompletionImpl() {
    // Process events on the WGPU instance until all submitted work is done
    WGPUInstance wgpuInstance = device_.GetInstance();
    if (!wgpuInstance) return false;

    auto startTime = std::chrono::steady_clock::now();
    const auto timeout = std::chrono::seconds(5);

    while (true) {
        wgpuInstanceProcessEvents(wgpuInstance);

        auto elapsed = std::chrono::steady_clock::now() - startTime;
        if (elapsed > timeout) {
            std::cerr << "[DawnCommandBuffer] WaitForCompletion timed out" << std::endl;
            return false;
        }

        // In WebGPU, wgpuInstanceProcessEvents processes all pending work.
        // After submission, a single call should suffice for synchronous completion.
        break;
    }

    return true;
}

// === Render Pass ===

void DawnCommandBuffer::BeginRenderPass(const RenderPassDesc& desc) {
    // End any currently active encoder/pass
    EndCurrentEncoder();

    // Ensure we have a command encoder
    EnsureCommandEncoder();
    if (!wgpuEncoder_) {
        std::cerr << "[DawnCommandBuffer] BeginRenderPass: no command encoder" << std::endl;
        return;
    }

    // Build color attachments
    WGPURenderPassColorAttachment colorAttachments[constants::MAX_RENDER_TARGETS]{};
    u32 colorCount = 0;

    for (u32 i = 0; i < desc.colorAttachments.size() && i < constants::MAX_RENDER_TARGETS; ++i) {
        const auto& src = desc.colorAttachments[i];
        if (src.texture == handles::INVALID_RESOURCE) continue;

        DawnTexture* tex = device_.GetTexture(src.texture);
        if (!tex) {
            std::cerr << "[DawnCommandBuffer] BeginRenderPass: invalid color attachment " << i << std::endl;
            continue;
        }

        WGPUTextureView view = tex->GetDefaultView();
        if (!view) {
            std::cerr << "[DawnCommandBuffer] BeginRenderPass: no texture view for attachment " << i << std::endl;
            continue;
        }

        auto& dst = colorAttachments[colorCount];
        dst.nextInChain = nullptr;
        dst.view = view;
        dst.resolveTarget = nullptr;
        dst.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
        dst.loadOp = ToWGPULoadOp(src.loadOp);
        dst.storeOp = ToWGPUStoreOp(src.storeOp);
        dst.clearValue = WGPUColor{
            static_cast<double>(src.clearValue.color.x),
            static_cast<double>(src.clearValue.color.y),
            static_cast<double>(src.clearValue.color.z),
            static_cast<double>(src.clearValue.color.w)
        };

        ++colorCount;
    }

    // Build depth-stencil attachment if present
    WGPURenderPassDepthStencilAttachment depthStencilAttachment{};
    bool hasDepthStencil = false;

    if (desc.depthAttachment.texture != handles::INVALID_RESOURCE) {
        DawnTexture* depthTex = device_.GetTexture(desc.depthAttachment.texture);
        if (depthTex) {
            WGPUTextureView depthView = depthTex->GetDefaultView();
            auto& texDesc = depthTex->GetTextureDesc();
            // For array textures, always create a single-layer 2D view
            // (default view covers all layers, which WebGPU rejects as depth attachment)
            if (texDesc.type == rhi::TextureType::Texture2DArray && texDesc.arraySize > 1) {
                rhi::TextureViewDesc viewDesc{};
                viewDesc.texture = desc.depthAttachment.texture;
                viewDesc.viewType = rhi::TextureType::Texture2D;
                viewDesc.firstArraySlice = desc.depthAttachment.arrayLayer;
                viewDesc.arraySize = 1;
                viewDesc.format = texDesc.format;
                depthView = depthTex->CreateView(viewDesc);
            } else if (desc.depthAttachment.arrayLayer > 0) {
                rhi::TextureViewDesc viewDesc{};
                viewDesc.texture = desc.depthAttachment.texture;
                viewDesc.viewType = rhi::TextureType::Texture2D;
                viewDesc.firstArraySlice = desc.depthAttachment.arrayLayer;
                viewDesc.arraySize = 1;
                viewDesc.format = texDesc.format;
                depthView = depthTex->CreateView(viewDesc);
            }

            depthStencilAttachment.nextInChain = nullptr;
            depthStencilAttachment.view = depthView;
            depthStencilAttachment.depthLoadOp = ToWGPULoadOp(desc.depthAttachment.loadOp);
            depthStencilAttachment.depthStoreOp = ToWGPUStoreOp(desc.depthAttachment.storeOp);
            depthStencilAttachment.depthClearValue = desc.depthAttachment.clearValue.depth;
            depthStencilAttachment.depthReadOnly = false;
            // Only set stencil fields if the format has a stencil aspect
            DataFormat dsFormat = depthTex->GetTextureDesc().format;
            bool hasStencil = (dsFormat == DataFormat::D24_UNorm_S8_UInt ||
                               dsFormat == DataFormat::D32_Float_S8X24_UInt);
            if (hasStencil) {
                depthStencilAttachment.stencilLoadOp = ToWGPULoadOp(desc.stencilAttachment.loadOp);
                depthStencilAttachment.stencilStoreOp = ToWGPUStoreOp(desc.stencilAttachment.storeOp);
                depthStencilAttachment.stencilClearValue = desc.stencilAttachment.clearValue.stencil;
                depthStencilAttachment.stencilReadOnly = false;
            } else {
                depthStencilAttachment.stencilLoadOp = WGPULoadOp_Undefined;
                depthStencilAttachment.stencilStoreOp = WGPUStoreOp_Undefined;
                depthStencilAttachment.stencilClearValue = 0;
                depthStencilAttachment.stencilReadOnly = true;
            }
            hasDepthStencil = true;
        }
    }

    // Assemble the render pass descriptor
    WGPURenderPassDescriptor rpDesc{};
    rpDesc.nextInChain = nullptr;
    rpDesc.colorAttachmentCount = colorCount;
    rpDesc.colorAttachments = colorAttachments;
    rpDesc.depthStencilAttachment = hasDepthStencil ? &depthStencilAttachment : nullptr;
    rpDesc.occlusionQuerySet = nullptr;
    rpDesc.timestampWrites = nullptr;

    wgpuRenderPass_ = wgpuCommandEncoderBeginRenderPass(wgpuEncoder_, &rpDesc);
    if (!wgpuRenderPass_) {
        std::cerr << "[DawnCommandBuffer] Failed to begin render pass" << std::endl;
        return;
    }

    currentEncoderType_ = EncoderType::Render;
    UpdateStats(CommandType::BeginRenderPass);

    // Apply viewport and scissor if specified in the desc
    if (desc.viewport.size.x > 0.0f && desc.viewport.size.y > 0.0f) {
        SetViewport(desc.viewport);
    }
    if (desc.scissor.extent.x > 0 && desc.scissor.extent.y > 0) {
        SetScissor(desc.scissor);
    }
}

void DawnCommandBuffer::BeginRenderPass(RenderPassHandle renderPass) {
    // This overload is for pre-built render pass objects.
    // For now, WebGPU render passes are built inline from RenderPassDesc,
    // so this path requires looking up the cached DawnRenderPass.
    (void)renderPass;
    std::cerr << "[DawnCommandBuffer] BeginRenderPass(handle) not yet implemented" << std::endl;
}

void DawnCommandBuffer::EndRenderPass() {
    if (wgpuRenderPass_) {
        wgpuRenderPassEncoderEnd(wgpuRenderPass_);
        wgpuRenderPassEncoderRelease(wgpuRenderPass_);
        wgpuRenderPass_ = nullptr;
    }
    currentEncoderType_ = EncoderType::None;
}

// === Viewport / Scissor ===

void DawnCommandBuffer::SetViewport(const ViewportDesc& viewport) {
    if (wgpuRenderPass_) {
        wgpuRenderPassEncoderSetViewport(
            wgpuRenderPass_,
            viewport.topLeft.x,
            viewport.topLeft.y,
            viewport.size.x,
            viewport.size.y,
            viewport.minDepth,
            viewport.maxDepth);
    }
}

void DawnCommandBuffer::SetScissor(const Rect& scissor) {
    if (wgpuRenderPass_) {
        wgpuRenderPassEncoderSetScissorRect(
            wgpuRenderPass_,
            static_cast<u32>(scissor.offset.x),
            static_cast<u32>(scissor.offset.y),
            scissor.extent.x,
            scissor.extent.y);
    }
}

// === Pipeline Binding ===

void DawnCommandBuffer::BindGraphicsPipeline(PipelineHandle pipeline) {
    if (!wgpuRenderPass_) {
        std::cerr << "[DawnCommandBuffer] BindGraphicsPipeline called outside render pass" << std::endl;
        return;
    }

    DawnPipeline* dawnPipeline = device_.GetPipeline(pipeline);
    if (!dawnPipeline) {
        std::cerr << "[DawnCommandBuffer] BindGraphicsPipeline: invalid pipeline handle" << std::endl;
        return;
    }

    WGPURenderPipeline renderPipeline = dawnPipeline->GetRenderPipeline();
    if (!renderPipeline) {
        std::cerr << "[DawnCommandBuffer] BindGraphicsPipeline: null render pipeline" << std::endl;
        return;
    }

    wgpuRenderPassEncoderSetPipeline(wgpuRenderPass_, renderPipeline);
    UpdateStats(CommandType::BindGraphicsPipeline);
}

void DawnCommandBuffer::BindComputePipeline(PipelineHandle pipeline) {
    DawnPipeline* dawnPipeline = device_.GetPipeline(pipeline);
    if (!dawnPipeline) {
        std::cerr << "[DawnCommandBuffer] BindComputePipeline: invalid pipeline handle" << std::endl;
        return;
    }

    WGPUComputePipeline computePipeline = dawnPipeline->GetComputePipeline();
    if (!computePipeline) {
        std::cerr << "[DawnCommandBuffer] BindComputePipeline: null compute pipeline" << std::endl;
        return;
    }

    // End any current pass and start a fresh compute pass for each pipeline
    // bind. This creates proper WebGPU sync scope boundaries so that buffer
    // usage validation doesn't flag cross-dispatch conflicts.
    EndCurrentEncoder();
    EnsureCommandEncoder();

    WGPUComputePassDescriptor cpDesc{};
    cpDesc.nextInChain = nullptr;
    cpDesc.label = ToWGPUStringView("DawnComputePass");
    cpDesc.timestampWrites = nullptr;

    wgpuComputePass_ = wgpuCommandEncoderBeginComputePass(wgpuEncoder_, &cpDesc);
    if (!wgpuComputePass_) {
        std::cerr << "[DawnCommandBuffer] Failed to begin compute pass" << std::endl;
        return;
    }
    currentEncoderType_ = EncoderType::Compute;

    wgpuComputePassEncoderSetPipeline(wgpuComputePass_, computePipeline);
    UpdateStats(CommandType::BindComputePipeline);
}

// === Resource Binding ===

void DawnCommandBuffer::BindVertexBuffers(u32 firstSlot, u32 slotCount,
                                           const ResourceHandle* buffers,
                                           const u64* offsets) {
    if (!wgpuRenderPass_) {
        std::cerr << "[DawnCommandBuffer] BindVertexBuffers called outside render pass" << std::endl;
        return;
    }

    for (u32 i = 0; i < slotCount; ++i) {
        if (buffers[i] == handles::INVALID_RESOURCE) continue;

        DawnBuffer* buf = device_.GetBuffer(buffers[i]);
        if (!buf) continue;

        WGPUBuffer wgpuBuf = buf->GetNativeBuffer();
        if (!wgpuBuf) continue;

        u64 offset = offsets ? offsets[i] : 0;
        wgpuRenderPassEncoderSetVertexBuffer(wgpuRenderPass_, firstSlot + i, wgpuBuf, offset, WGPU_WHOLE_SIZE);
    }

    UpdateStats(CommandType::BindVertexBuffers);
}

void DawnCommandBuffer::BindIndexBuffer(ResourceHandle buffer, DataFormat format, u64 offset) {
    if (!wgpuRenderPass_) {
        std::cerr << "[DawnCommandBuffer] BindIndexBuffer called outside render pass" << std::endl;
        return;
    }

    DawnBuffer* buf = device_.GetBuffer(buffer);
    if (!buf) {
        std::cerr << "[DawnCommandBuffer] BindIndexBuffer: invalid buffer handle" << std::endl;
        return;
    }

    WGPUBuffer wgpuBuf = buf->GetNativeBuffer();
    if (!wgpuBuf) return;

    WGPUIndexFormat indexFormat = ToWGPUIndexFormat(format);
    wgpuRenderPassEncoderSetIndexBuffer(wgpuRenderPass_, wgpuBuf, indexFormat, offset, WGPU_WHOLE_SIZE);

    UpdateStats(CommandType::BindIndexBuffer);
}

void DawnCommandBuffer::BindDescriptorSets(PipelineBindPoint bindPoint,
                                            PipelineLayoutHandle pipelineLayout,
                                            u32 firstSet,
                                            u32 setCount,
                                            const DescriptorSetHandle* descriptorSets,
                                            u32 dynamicOffsetCount,
                                            const u32* dynamicOffsets) {
    (void)pipelineLayout;

    u32 dynOffsetCursor = 0;

    for (u32 i = 0; i < setCount; ++i) {
        if (descriptorSets[i] == static_cast<DescriptorSetHandle>(handles::INVALID_RESOURCE)) {
            // Still advance cursor by this set's dynamic binding count
            if (dynamicOffsets && dynOffsetCursor < dynamicOffsetCount) {
                // Can't determine count without the set — skip (shouldn't happen in practice)
            }
            continue;
        }

        DawnDescriptorSet* ds = device_.GetDescriptorSet(descriptorSets[i]);
        if (!ds) continue;

        WGPUBindGroup bindGroup = ds->GetBindGroup();
        if (!bindGroup) continue;

        // Query this set's layout for its dynamic binding count
        u32 setDynCount = 0;
        const u32* setDynOffsets = nullptr;
        if (dynamicOffsets && dynamicOffsetCount > 0 && dynOffsetCursor < dynamicOffsetCount) {
            auto* layout = device_.GetDescriptorSetLayout(ds->GetLayout());
            setDynCount = layout ? layout->GetDynamicBindingCount() : 0;
            if (setDynCount > 0 && (dynOffsetCursor + setDynCount) <= dynamicOffsetCount) {
                setDynOffsets = dynamicOffsets + dynOffsetCursor;
            } else {
                setDynCount = 0;
            }
            dynOffsetCursor += setDynCount;
        }

        u32 groupIndex = firstSet + i;

        if (bindPoint == PipelineBindPoint::Graphics && wgpuRenderPass_) {
            wgpuRenderPassEncoderSetBindGroup(wgpuRenderPass_, groupIndex, bindGroup,
                                              setDynCount, setDynOffsets);
        } else if (bindPoint == PipelineBindPoint::Compute && wgpuComputePass_) {
            wgpuComputePassEncoderSetBindGroup(wgpuComputePass_, groupIndex, bindGroup,
                                               setDynCount, setDynOffsets);
        }
    }

    UpdateStats(CommandType::BindDescriptorSets);
}

void DawnCommandBuffer::PushConstants(PipelineLayoutHandle layout,
                                       ShaderStage stageFlags,
                                       u32 offset,
                                       u32 size,
                                       const void* pValues) {
    (void)stageFlags;
    (void)offset;

    if (!pValues || size == 0) return;

    DawnPipelineLayout* pipelineLayout = device_.GetPipelineLayout(layout);
    if (!pipelineLayout || !pipelineLayout->HasPushConstants()) return;

    WGPUBindGroupLayout pcBGL = pipelineLayout->GetPushConstantBindGroupLayout();
    if (!pcBGL) return;

    // WebGPU has no native push constants. We emulate via a ring buffer.
    // 1. Allocate a slot from the device's push constant ring buffer
    u32 ringOffset = device_.AllocatePushConstantSlot();

    // 2. Write data to the push constant buffer via the queue
    WGPUBuffer pushConstantBuf = device_.GetPushConstantBuffer();
    if (!pushConstantBuf) {
        std::cerr << "[DawnCommandBuffer] PushConstants: no push constant buffer" << std::endl;
        return;
    }

    wgpuQueueWriteBuffer(device_.GetQueue(), pushConstantBuf, ringOffset, pValues, size);

    // 3. Create a bind group pointing at the ring buffer slot
    WGPUBindGroupEntry entry{};
    entry.nextInChain = nullptr;
    entry.binding = 0;
    entry.buffer = pushConstantBuf;
    entry.offset = ringOffset;
    entry.size = constants::MAX_PUSH_CONSTANTS_SIZE;

    WGPUBindGroupDescriptor bgDesc{};
    bgDesc.nextInChain = nullptr;
    bgDesc.label = ToWGPUStringView("Push Constant Bind Group");
    bgDesc.layout = pcBGL;
    bgDesc.entryCount = 1;
    bgDesc.entries = &entry;

    WGPUBindGroup bg = wgpuDeviceCreateBindGroup(device_.GetNativeDevice(), &bgDesc);
    if (!bg) {
        std::cerr << "[DawnCommandBuffer] PushConstants: failed to create bind group" << std::endl;
        return;
    }

    // 4. Bind at the push constant group index (= setLayoutCount, last group)
    u32 groupIndex = pipelineLayout->GetPushConstantBindGroupIndex();
    if (wgpuRenderPass_) {
        wgpuRenderPassEncoderSetBindGroup(wgpuRenderPass_, groupIndex, bg, 0, nullptr);
    } else if (wgpuComputePass_) {
        wgpuComputePassEncoderSetBindGroup(wgpuComputePass_, groupIndex, bg, 0, nullptr);
    }

    // Release immediately — the bind group is referenced by the encoder until submit
    wgpuBindGroupRelease(bg);
}

// === Draw Commands ===

void DawnCommandBuffer::Draw(u32 vertexCount, u32 startVertex,
                              u32 instanceCount, u32 startInstance) {
    if (!wgpuRenderPass_) {
        std::cerr << "[DawnCommandBuffer] Draw called outside render pass" << std::endl;
        return;
    }

    wgpuRenderPassEncoderDraw(wgpuRenderPass_, vertexCount, instanceCount, startVertex, startInstance);
    UpdateStats(CommandType::Draw);
}

void DawnCommandBuffer::DrawIndexed(u32 indexCount, u32 startIndex,
                                     u32 baseVertex,
                                     u32 instanceCount, u32 startInstance) {
    if (!wgpuRenderPass_) {
        std::cerr << "[DawnCommandBuffer] DrawIndexed called outside render pass" << std::endl;
        return;
    }

    wgpuRenderPassEncoderDrawIndexed(wgpuRenderPass_, indexCount, instanceCount,
                                     startIndex, baseVertex, startInstance);
    UpdateStats(CommandType::DrawIndexed);
}

void DawnCommandBuffer::DrawIndirect(ResourceHandle buffer, u64 offset, u32 drawCount) {
    (void)drawCount;

    if (!wgpuRenderPass_) {
        std::cerr << "[DawnCommandBuffer] DrawIndirect called outside render pass" << std::endl;
        return;
    }

    DawnBuffer* buf = device_.GetBuffer(buffer);
    if (!buf) {
        std::cerr << "[DawnCommandBuffer] DrawIndirect: invalid buffer handle" << std::endl;
        return;
    }

    WGPUBuffer wgpuBuf = buf->GetNativeBuffer();
    if (!wgpuBuf) return;

    wgpuRenderPassEncoderDrawIndirect(wgpuRenderPass_, wgpuBuf, offset);
    UpdateStats(CommandType::DrawIndirect);
}

// === Compute ===

void DawnCommandBuffer::Dispatch(u32 groupCountX, u32 groupCountY, u32 groupCountZ) {
    if (currentEncoderType_ != EncoderType::Compute || !wgpuComputePass_) {
        // Auto-start a compute pass if needed
        EndCurrentEncoder();
        EnsureCommandEncoder();

        WGPUComputePassDescriptor cpDesc{};
        cpDesc.nextInChain = nullptr;
        cpDesc.label = ToWGPUStringView("DawnComputePass");
        cpDesc.timestampWrites = nullptr;

        wgpuComputePass_ = wgpuCommandEncoderBeginComputePass(wgpuEncoder_, &cpDesc);
        if (!wgpuComputePass_) {
            std::cerr << "[DawnCommandBuffer] Failed to begin compute pass for Dispatch" << std::endl;
            return;
        }
        currentEncoderType_ = EncoderType::Compute;
    }

    wgpuComputePassEncoderDispatchWorkgroups(wgpuComputePass_, groupCountX, groupCountY, groupCountZ);
    UpdateStats(CommandType::Dispatch);
}

void DawnCommandBuffer::DispatchIndirect(ResourceHandle buffer, u64 offset) {
    if (currentEncoderType_ != EncoderType::Compute || !wgpuComputePass_) {
        std::cerr << "[DawnCommandBuffer] DispatchIndirect called without compute pass" << std::endl;
        return;
    }

    DawnBuffer* buf = device_.GetBuffer(buffer);
    if (!buf) {
        std::cerr << "[DawnCommandBuffer] DispatchIndirect: invalid buffer handle" << std::endl;
        return;
    }

    WGPUBuffer wgpuBuf = buf->GetNativeBuffer();
    if (!wgpuBuf) return;

    wgpuComputePassEncoderDispatchWorkgroupsIndirect(wgpuComputePass_, wgpuBuf, offset);
    UpdateStats(CommandType::DispatchIndirect);
}

// === Barriers (NO-OP in WebGPU) ===

void DawnCommandBuffer::MemoryBarrier(PipelineStage srcStageMask,
                                       PipelineStage dstStageMask,
                                       AccessFlag srcAccessMask,
                                       AccessFlag dstAccessMask) {
    // WebGPU handles synchronization internally. No explicit barriers needed.
    (void)srcStageMask;
    (void)dstStageMask;
    (void)srcAccessMask;
    (void)dstAccessMask;
}

void DawnCommandBuffer::InsertBarrier(const ResourceBarrier* barriers, u32 barrierCount) {
    // WebGPU handles synchronization internally. No explicit barriers needed.
    (void)barriers;
    (void)barrierCount;
}

// === Resource Copies ===

void DawnCommandBuffer::CopyBuffer(ResourceHandle src, ResourceHandle dst,
                                    u64 srcOffset, u64 dstOffset, u64 size) {
    EnsureCommandEncoder();
    if (!wgpuEncoder_) return;

    DawnBuffer* srcBuf = device_.GetBuffer(src);
    DawnBuffer* dstBuf = device_.GetBuffer(dst);
    if (!srcBuf || !dstBuf) {
        std::cerr << "[DawnCommandBuffer] CopyBuffer: invalid buffer handles" << std::endl;
        return;
    }

    WGPUBuffer wgpuSrc = srcBuf->GetNativeBuffer();
    WGPUBuffer wgpuDst = dstBuf->GetNativeBuffer();
    if (!wgpuSrc || !wgpuDst) return;

    wgpuCommandEncoderCopyBufferToBuffer(wgpuEncoder_, wgpuSrc, srcOffset, wgpuDst, dstOffset, size);
    UpdateStats(CommandType::CopyBuffer);
}

void DawnCommandBuffer::CopyBufferToTexture(ResourceHandle srcBuffer,
                                             ResourceHandle dstTexture,
                                             const BufferTextureCopyRegion* regions,
                                             u32 regionCount) {
    EnsureCommandEncoder();
    if (!wgpuEncoder_) return;

    DawnBuffer* srcBuf = device_.GetBuffer(srcBuffer);
    DawnTexture* dstTex = device_.GetTexture(dstTexture);
    if (!srcBuf || !dstTex) {
        std::cerr << "[DawnCommandBuffer] CopyBufferToTexture: invalid handles" << std::endl;
        return;
    }

    WGPUBuffer wgpuSrc = srcBuf->GetNativeBuffer();
    WGPUTexture wgpuDst = dstTex->GetNativeTexture();
    if (!wgpuSrc || !wgpuDst) return;

    for (u32 i = 0; i < regionCount; ++i) {
        const auto& r = regions[i];

        WGPUTexelCopyBufferInfo srcCopy{};
        srcCopy.layout.offset = r.bufferOffset;
        srcCopy.layout.bytesPerRow = r.bufferRowLength;
        srcCopy.layout.rowsPerImage = r.bufferImageHeight;
        srcCopy.buffer = wgpuSrc;

        WGPUTexelCopyTextureInfo dstCopy{};
        dstCopy.texture = wgpuDst;
        dstCopy.mipLevel = r.imageSubresource.mipLevel;
        dstCopy.origin = WGPUOrigin3D{
            static_cast<u32>(r.imageOffset.x),
            static_cast<u32>(r.imageOffset.y),
            static_cast<u32>(r.imageOffset.z)
        };
        dstCopy.aspect = WGPUTextureAspect_All;

        WGPUExtent3D copySize{};
        copySize.width = r.imageExtent.width;
        copySize.height = r.imageExtent.height;
        copySize.depthOrArrayLayers = r.imageExtent.depth;

        wgpuCommandEncoderCopyBufferToTexture(wgpuEncoder_, &srcCopy, &dstCopy, &copySize);
    }

    UpdateStats(CommandType::CopyBufferToTexture);
}

void DawnCommandBuffer::CopyTextureToBuffer(ResourceHandle srcTexture,
                                             ResourceHandle dstBuffer,
                                             const BufferTextureCopyRegion* regions,
                                             u32 regionCount) {
    EnsureCommandEncoder();
    if (!wgpuEncoder_) return;

    DawnTexture* srcTex = device_.GetTexture(srcTexture);
    DawnBuffer* dstBuf = device_.GetBuffer(dstBuffer);
    if (!srcTex || !dstBuf) {
        std::cerr << "[DawnCommandBuffer] CopyTextureToBuffer: invalid handles" << std::endl;
        return;
    }

    WGPUTexture wgpuSrc = srcTex->GetNativeTexture();
    WGPUBuffer wgpuDst = dstBuf->GetNativeBuffer();
    if (!wgpuSrc || !wgpuDst) return;

    for (u32 i = 0; i < regionCount; ++i) {
        const auto& r = regions[i];

        WGPUTexelCopyTextureInfo srcCopy{};
        srcCopy.texture = wgpuSrc;
        srcCopy.mipLevel = r.imageSubresource.mipLevel;
        srcCopy.origin = WGPUOrigin3D{
            static_cast<u32>(r.imageOffset.x),
            static_cast<u32>(r.imageOffset.y),
            static_cast<u32>(r.imageOffset.z)
        };
        srcCopy.aspect = WGPUTextureAspect_All;

        WGPUTexelCopyBufferInfo dstCopy{};
        dstCopy.layout.offset = r.bufferOffset;
        dstCopy.layout.bytesPerRow = r.bufferRowLength;
        dstCopy.layout.rowsPerImage = r.bufferImageHeight;
        dstCopy.buffer = wgpuDst;

        WGPUExtent3D copySize{};
        copySize.width = r.imageExtent.width;
        copySize.height = r.imageExtent.height;
        copySize.depthOrArrayLayers = r.imageExtent.depth;

        wgpuCommandEncoderCopyTextureToBuffer(wgpuEncoder_, &srcCopy, &dstCopy, &copySize);
    }

    UpdateStats(CommandType::CopyTextureToBuffer);
}

void DawnCommandBuffer::BlitTexture(ResourceHandle src, ResourceHandle dst,
                                     const TextureBlitRegion* regions,
                                     u32 regionCount, FilterMode filter) {
    // WebGPU has no direct blit operation. For 1:1 copies we can use
    // CopyTextureToTexture, but scaled blits require a full-screen quad pass.
    // Log a warning and skip for now.
    (void)src;
    (void)dst;
    (void)regions;
    (void)regionCount;
    (void)filter;

    std::cerr << "[DawnCommandBuffer] BlitTexture: WebGPU has no native blit. "
              << "Skipping " << regionCount << " region(s)." << std::endl;
}

// GenerateMipmaps is implemented in DawnDevice.cpp to access blit pipeline internals

// === Queries ===

void DawnCommandBuffer::WriteTimestamp(QueryPoolHandle queryPool, u32 queryIndex) {
    // Close any open pass — WriteTimestamp is an encoder-level command and
    // Dawn locks the encoder while a pass is open.
    EndCurrentEncoder();
    EnsureCommandEncoder();
    if (!wgpuEncoder_) return;

    // WebGPU timestamp queries require feature + allow_unsafe_apis toggle.
    // Skip silently if not available to avoid invalidating the command buffer.
    if (!wgpuDeviceHasFeature(device_.GetNativeDevice(), WGPUFeatureName_TimestampQuery)) {
        return;
    }

    DawnQueryPool* pool = device_.GetQueryPool(queryPool);
    if (!pool) return;

    WGPUQuerySet querySet = pool->GetNativeQuerySet();
    if (!querySet) return;

    wgpuCommandEncoderWriteTimestamp(wgpuEncoder_, querySet, queryIndex);
}

// === Helper Methods ===

void DawnCommandBuffer::EnsureCommandEncoder() {
    if (!wgpuEncoder_) {
        // Re-create encoder if needed (e.g. after submit/reset).
        // This is a safety net; normally Begin() creates the encoder.
        WGPUCommandEncoderDescriptor encoderDesc{};
        encoderDesc.nextInChain = nullptr;
        encoderDesc.label = ToWGPUStringView("DawnCommandEncoder (auto)");
        wgpuEncoder_ = wgpuDeviceCreateCommandEncoder(device_.GetNativeDevice(), &encoderDesc);
    }
}

void DawnCommandBuffer::EndCurrentEncoder() {
    if (currentEncoderType_ == EncoderType::Render && wgpuRenderPass_) {
        wgpuRenderPassEncoderEnd(wgpuRenderPass_);
        wgpuRenderPassEncoderRelease(wgpuRenderPass_);
        wgpuRenderPass_ = nullptr;
    } else if (currentEncoderType_ == EncoderType::Compute && wgpuComputePass_) {
        wgpuComputePassEncoderEnd(wgpuComputePass_);
        wgpuComputePassEncoderRelease(wgpuComputePass_);
        wgpuComputePass_ = nullptr;
    }
    currentEncoderType_ = EncoderType::None;
}

} // namespace primal::graphics::rhi

#endif // ENABLE_WEBGPU
