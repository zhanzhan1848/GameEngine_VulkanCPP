/**
 * @file MetalCommandBuffer.cpp
 * @brief Metal 命令缓冲区实现
 * @author GameEngine VulkanCPP Team
 * @date 2026-01-07
 * @version 0.1.0
 */

#include "MetalCommandBuffer.h"
#include "MetalDevice.h"
#include "MetalBuffer.h"
#include "MetalSync.h"
#include <iostream>

namespace primal::graphics::rhi {

MetalCommandBuffer::MetalCommandBuffer(MetalDevice& device, CommandQueueType type)
    : RHICommandBuffer(device, type) {}

MetalCommandBuffer::MetalCommandBuffer(MetalDevice& device, CommandQueueType type, MTL::RenderCommandEncoder* encoder)
    : RHICommandBuffer(device, type), isSecondary_(true) {
    currentEncoder_ = encoder;
    currentEncoderType_ = EncoderType::Render;
    // Secondary buffer starts in Recording state
    state_ = CommandBufferState::Recording;
}

MetalCommandBuffer::~MetalCommandBuffer() {
    Destroy();
}

bool MetalCommandBuffer::Initialize() {
    if (state_ != CommandBufferState::Invalid) return true;
    
    if (isSecondary_) return true;

    // Create Guard Event for synchronization
    MetalDevice& metalDevice = static_cast<MetalDevice&>(device_);
    guardEventHandle_ = metalDevice.CreateSync();
    if (guardEventHandle_ == handles::INVALID_SYNC) return false;
    
    state_ = CommandBufferState::Reset;
    return true;
}

void MetalCommandBuffer::destroyImpl() {
    if (isSecondary_) {
        // Secondary buffers don't own the encoder or command buffer
        currentEncoder_ = nullptr;
        currentEncoderType_ = EncoderType::None;
        return;
    }

    endCurrentEncoder();

    if (mtlCommandBuffer_) {
        mtlCommandBuffer_->release();
        mtlCommandBuffer_ = nullptr;
    }
    
    if (pool_) {
        pool_->release();
        pool_ = nullptr;
    }

    if (guardEventHandle_ != handles::INVALID_SYNC) {
        MetalDevice& metalDevice = static_cast<MetalDevice&>(device_);
        metalDevice.DestroySync(guardEventHandle_);
        guardEventHandle_ = handles::INVALID_SYNC;
    }
}

bool MetalCommandBuffer::resetImpl() {
    // Release existing resources but keep the object ready for Begin()
    destroyImpl();
    return true;
}

bool MetalCommandBuffer::beginImpl() {
    if (mtlCommandBuffer_) {
        return false; // Already recording or recorded
    }

    // Create autorelease pool for the recording session
    pool_ = NS::AutoreleasePool::alloc()->init();

    MetalDevice& metalDevice = static_cast<MetalDevice&>(device_);
    MTL::CommandQueue* queue = nullptr;
    
    switch (type_) {
        case CommandQueueType::Graphics:
            queue = metalDevice.GetGraphicsQueue();
            break;
        case CommandQueueType::Compute:
            queue = metalDevice.GetComputeQueue();
            break;
        case CommandQueueType::Transfer:
            queue = metalDevice.GetTransferQueue();
            break;
        default:
            queue = metalDevice.GetGraphicsQueue();
            break;
    }
    
    if (!queue) {
        std::cerr << "[MetalCommandBuffer] Queue not found for type " << (int)type_ << std::endl;
        return false;
    }

    mtlCommandBuffer_ = queue->commandBuffer();
    if (!mtlCommandBuffer_) {
        std::cerr << "[MetalCommandBuffer] Failed to create command buffer" << std::endl;
        return false;
    }

    // Keep reference
    mtlCommandBuffer_->retain();
    
    return true;
}

bool MetalCommandBuffer::endImpl() {
    endCurrentEncoder();
    
    // Release the autorelease pool created in Begin()
    // This MUST be done on the same thread that called Begin()
    if (pool_) {
        pool_->release();
        pool_ = nullptr;
    }
    
    return true;
}

bool MetalCommandBuffer::submitImpl(uint32_t waitFlags) {
    if (!mtlCommandBuffer_) return false;

    // Process wait semaphores
    for (const auto& semInfo : waitSemaphores_) {
        MetalDevice& metalDevice = static_cast<MetalDevice&>(device_);
        MetalSync* sync = metalDevice.GetSync(semInfo.semaphore);
        if (sync && sync->GetNativeEvent()) {
            mtlCommandBuffer_->encodeSignalEvent(sync->GetNativeEvent(), semInfo.value);
        }
    }

    // Process signal semaphores
    for (const auto& semInfo : signalSemaphores_) {
        MetalDevice& metalDevice = static_cast<MetalDevice&>(device_);
        MetalSync* sync = metalDevice.GetSync(semInfo.semaphore);
        if (sync && sync->GetNativeEvent()) {
            mtlCommandBuffer_->encodeSignalEvent(sync->GetNativeEvent(), semInfo.value);
        }
    }

    mtlCommandBuffer_->commit();
    
    return true;
}

bool MetalCommandBuffer::waitForCompletionImpl() {
    if (!mtlCommandBuffer_) return false;
    
    mtlCommandBuffer_->waitUntilCompleted();
    return true;
}

void MetalCommandBuffer::endCurrentEncoder() {
    if (currentEncoder_) {
        currentEncoder_->endEncoding();
        currentEncoder_ = nullptr;
    }
    currentEncoderType_ = EncoderType::None;
}

MTL::BlitCommandEncoder* MetalCommandBuffer::getBlitEncoder() {
    if (currentEncoderType_ == EncoderType::Blit) {
        return static_cast<MTL::BlitCommandEncoder*>(currentEncoder_);
    }
    
    endCurrentEncoder();
    
    if (!mtlCommandBuffer_) return nullptr;
    
    MTL::BlitCommandEncoder* encoder = mtlCommandBuffer_->blitCommandEncoder();
    currentEncoder_ = encoder;
    currentEncoderType_ = EncoderType::Blit;
    return encoder;
}

MTL::ComputeCommandEncoder* MetalCommandBuffer::getComputeEncoder() {
    if (currentEncoderType_ == EncoderType::Compute) {
        return static_cast<MTL::ComputeCommandEncoder*>(currentEncoder_);
    }
    
    endCurrentEncoder();
    
    if (!mtlCommandBuffer_) return nullptr;
    
    MTL::ComputeCommandEncoder* encoder = mtlCommandBuffer_->computeCommandEncoder();
    currentEncoder_ = encoder;
    currentEncoderType_ = EncoderType::Compute;
    return encoder;
}

// === Render Commands ===

void MetalCommandBuffer::BeginRenderPass(const RenderPassDesc& desc) {
    if (isSecondary_) return;

    endCurrentEncoder();
    
    // Construct MTLRenderPassDescriptor
    MTL::RenderPassDescriptor* passDesc = MTL::RenderPassDescriptor::renderPassDescriptor();
    
    // Setup color attachments
    for (size_t i = 0; i < desc.colorAttachments.size(); ++i) {
        const auto& attachment = desc.colorAttachments[i];
        if (attachment.texture != handles::INVALID_RESOURCE) {
            MetalDevice& metalDevice = static_cast<MetalDevice&>(device_);
            MetalTexture* texture = metalDevice.GetTexture(attachment.texture);
            
            if (texture && texture->GetNativeTexture()) {
                MTL::RenderPassColorAttachmentDescriptor* ca = passDesc->colorAttachments()->object(i);
                ca->setTexture(texture->GetNativeTexture());
                MTL::LoadAction metalLoadAction = MTL::LoadActionDontCare;
                switch (attachment.loadOp) {
                    case LoadAction::Load: metalLoadAction = MTL::LoadActionLoad; break;
                    case LoadAction::Clear: metalLoadAction = MTL::LoadActionClear; break;
                    case LoadAction::DontCare: metalLoadAction = MTL::LoadActionDontCare; break;
                }
                ca->setLoadAction(metalLoadAction);

                MTL::StoreAction metalStoreAction = MTL::StoreActionDontCare;
                switch (attachment.storeOp) {
                    case StoreAction::Store: metalStoreAction = MTL::StoreActionStore; break;
                    case StoreAction::DontCare: metalStoreAction = MTL::StoreActionDontCare; break;
                }
                ca->setStoreAction(metalStoreAction);
                
                if (attachment.loadOp == LoadAction::Clear) {
                    ca->setClearColor(MTL::ClearColor(
                        attachment.clearValue.color.r,
                        attachment.clearValue.color.g,
                        attachment.clearValue.color.b,
                        attachment.clearValue.color.a
                    ));
                }
            }
        }
    }
    
    // Setup depth/stencil attachments
    if (desc.depthAttachment.texture != handles::INVALID_RESOURCE) {
        MetalDevice& metalDevice = static_cast<MetalDevice&>(device_);
        MetalTexture* texture = metalDevice.GetTexture(desc.depthAttachment.texture);
        
        if (texture && texture->GetNativeTexture()) {
            MTL::RenderPassDepthAttachmentDescriptor* da = passDesc->depthAttachment();
            da->setTexture(texture->GetNativeTexture());
            
            MTL::LoadAction metalLoadAction = MTL::LoadActionDontCare;
            switch (desc.depthAttachment.loadOp) {
                case LoadAction::Load: metalLoadAction = MTL::LoadActionLoad; break;
                case LoadAction::Clear: metalLoadAction = MTL::LoadActionClear; break;
                case LoadAction::DontCare: metalLoadAction = MTL::LoadActionDontCare; break;
            }
            da->setLoadAction(metalLoadAction);
            da->setClearDepth(desc.depthAttachment.clearValue.depth);
            
            MTL::StoreAction metalStoreAction = MTL::StoreActionDontCare;
            switch (desc.depthAttachment.storeOp) {
                case StoreAction::Store: metalStoreAction = MTL::StoreActionStore; break;
                case StoreAction::DontCare: metalStoreAction = MTL::StoreActionDontCare; break;
            }
            da->setStoreAction(metalStoreAction);
        }
    }
    
    if (mtlCommandBuffer_) {
        MTL::RenderCommandEncoder* encoder = mtlCommandBuffer_->renderCommandEncoder(passDesc);
        currentEncoder_ = encoder;
        currentEncoderType_ = EncoderType::Render;
        
        // Initial state setup (viewport, scissor)
        SetViewport(desc.viewport);
        SetScissor(desc.scissor);
    }
    
    // Descriptor is autoreleased
}

void MetalCommandBuffer::BeginRenderPass(RenderPassHandle renderPass) {
    if (isSecondary_) return;

    endCurrentEncoder();
    
    MetalDevice& metalDevice = static_cast<MetalDevice&>(device_);
    MetalRenderPass* pass = metalDevice.GetRenderPass(renderPass);
    if (!pass) {
        std::cerr << "[MetalCommandBuffer] Invalid RenderPass handle" << std::endl;
        return;
    }

    MTL::RenderPassDescriptor* passDesc = pass->GetNativeRenderPassDescriptor();
    if (!passDesc) {
        std::cerr << "[MetalCommandBuffer] Failed to get native RenderPass descriptor" << std::endl;
        return;
    }

    if (mtlCommandBuffer_) {
        MTL::RenderCommandEncoder* encoder = mtlCommandBuffer_->renderCommandEncoder(passDesc);
        currentEncoder_ = encoder;
        currentEncoderType_ = EncoderType::Render;
        
        // Initial state setup (viewport, scissor)
        const auto& desc = pass->GetDesc();
        SetViewport(desc.viewport);
        SetScissor(desc.scissor);
    }
}

void MetalCommandBuffer::BeginParallelRenderPass(const RenderPassDesc& desc) {
    if (isSecondary_) return;
    
    endCurrentEncoder();
    
    // Construct MTLRenderPassDescriptor
    MTL::RenderPassDescriptor* passDesc = MTL::RenderPassDescriptor::renderPassDescriptor();
    
    // Setup color attachments
    for (size_t i = 0; i < desc.colorAttachments.size(); ++i) {
        const auto& attachment = desc.colorAttachments[i];
        if (attachment.texture != handles::INVALID_RESOURCE) {
            MetalDevice& metalDevice = static_cast<MetalDevice&>(device_);
            MetalTexture* texture = metalDevice.GetTexture(attachment.texture);
            
            if (texture && texture->GetNativeTexture()) {
                MTL::RenderPassColorAttachmentDescriptor* ca = passDesc->colorAttachments()->object(i);
                ca->setTexture(texture->GetNativeTexture());
                MTL::LoadAction metalLoadAction = MTL::LoadActionDontCare;
                switch (attachment.loadOp) {
                    case LoadAction::Load: metalLoadAction = MTL::LoadActionLoad; break;
                    case LoadAction::Clear: metalLoadAction = MTL::LoadActionClear; break;
                    case LoadAction::DontCare: metalLoadAction = MTL::LoadActionDontCare; break;
                }
                ca->setLoadAction(metalLoadAction);

                MTL::StoreAction metalStoreAction = MTL::StoreActionDontCare;
                switch (attachment.storeOp) {
                    case StoreAction::Store: metalStoreAction = MTL::StoreActionStore; break;
                    case StoreAction::DontCare: metalStoreAction = MTL::StoreActionDontCare; break;
                }
                ca->setStoreAction(metalStoreAction);
                
                if (attachment.loadOp == LoadAction::Clear) {
                    ca->setClearColor(MTL::ClearColor(
                        attachment.clearValue.color.r,
                        attachment.clearValue.color.g,
                        attachment.clearValue.color.b,
                        attachment.clearValue.color.a
                    ));
                }
                
                // Mip/Layer setup if needed
                ca->setLevel(attachment.mipLevel);
                ca->setSlice(attachment.arrayLayer);
            }
        }
    }
    
    // Setup depth/stencil attachments
    if (desc.depthAttachment.texture != handles::INVALID_RESOURCE) {
        MetalDevice& metalDevice = static_cast<MetalDevice&>(device_);
        MetalTexture* texture = metalDevice.GetTexture(desc.depthAttachment.texture);
        
        if (texture && texture->GetNativeTexture()) {
            MTL::RenderPassDepthAttachmentDescriptor* da = passDesc->depthAttachment();
            da->setTexture(texture->GetNativeTexture());
            
            MTL::LoadAction metalLoadAction = MTL::LoadActionDontCare;
            switch (desc.depthAttachment.loadOp) {
                case LoadAction::Load: metalLoadAction = MTL::LoadActionLoad; break;
                case LoadAction::Clear: metalLoadAction = MTL::LoadActionClear; break;
                case LoadAction::DontCare: metalLoadAction = MTL::LoadActionDontCare; break;
            }
            da->setLoadAction(metalLoadAction);
            da->setClearDepth(desc.depthAttachment.clearValue.depth);
            
            MTL::StoreAction metalStoreAction = MTL::StoreActionDontCare;
            switch (desc.depthAttachment.storeOp) {
                case StoreAction::Store: metalStoreAction = MTL::StoreActionStore; break;
                case StoreAction::DontCare: metalStoreAction = MTL::StoreActionDontCare; break;
            }
            da->setStoreAction(metalStoreAction);
        }
    }

    if (mtlCommandBuffer_) {
        parallelRenderEncoder_ = mtlCommandBuffer_->parallelRenderCommandEncoder(passDesc);
        currentEncoderType_ = EncoderType::Render;
        currentEncoder_ = nullptr;
    }
}

void MetalCommandBuffer::BeginParallelRenderPass(RenderPassHandle renderPass) {
    if (isSecondary_) return;

    endCurrentEncoder();
    
    MetalDevice& metalDevice = static_cast<MetalDevice&>(device_);
    MetalRenderPass* pass = metalDevice.GetRenderPass(renderPass);
    if (!pass) {
        std::cerr << "[MetalCommandBuffer] Invalid RenderPass handle" << std::endl;
        return;
    }

    MTL::RenderPassDescriptor* passDesc = pass->GetNativeRenderPassDescriptor();
    if (!passDesc) {
        std::cerr << "[MetalCommandBuffer] Failed to get native RenderPass descriptor" << std::endl;
        return;
    }

    if (mtlCommandBuffer_) {
        parallelRenderEncoder_ = mtlCommandBuffer_->parallelRenderCommandEncoder(passDesc);
        currentEncoderType_ = EncoderType::Render;
        currentEncoder_ = nullptr;
    }
}

MetalCommandBuffer* MetalCommandBuffer::CreateSecondaryCommandBuffer() {
    if (!parallelRenderEncoder_) return nullptr;
    
    MTL::RenderCommandEncoder* subEncoder = parallelRenderEncoder_->renderCommandEncoder();
    if (!subEncoder) return nullptr;
    
    return new MetalCommandBuffer(static_cast<MetalDevice&>(device_), type_, subEncoder);
}

void MetalCommandBuffer::EndRenderPass() {
    if (isSecondary_) {
        if (currentEncoder_) {
            currentEncoder_->endEncoding();
            currentEncoder_ = nullptr;
        }
        return;
    }

    if (parallelRenderEncoder_) {
        parallelRenderEncoder_->endEncoding();
        parallelRenderEncoder_ = nullptr;
        currentEncoderType_ = EncoderType::None;
    } else if (currentEncoderType_ == EncoderType::Render) {
        endCurrentEncoder();
    }
}

void MetalCommandBuffer::SetViewport(const ViewportDesc& viewport) {
    if (currentEncoderType_ == EncoderType::Render) {
        MTL::Viewport vp;
        vp.originX = viewport.topLeft.x;
        vp.originY = viewport.topLeft.y;
        vp.width = viewport.size.x;
        vp.height = viewport.size.y;
        vp.znear = viewport.minDepth;
        vp.zfar = viewport.maxDepth;
        
        static_cast<MTL::RenderCommandEncoder*>(currentEncoder_)->setViewport(vp);
    }
}

void MetalCommandBuffer::SetScissor(const Rect& scissor) {
    if (currentEncoderType_ == EncoderType::Render) {
        MTL::ScissorRect rect;
        rect.x = static_cast<NS::UInteger>(scissor.offset.x);
        rect.y = static_cast<NS::UInteger>(scissor.offset.y);
        rect.width = static_cast<NS::UInteger>(scissor.extent.x);
        rect.height = static_cast<NS::UInteger>(scissor.extent.y);
        
        static_cast<MTL::RenderCommandEncoder*>(currentEncoder_)->setScissorRect(rect);
    }
}

void MetalCommandBuffer::BindGraphicsPipeline(PipelineHandle pipeline) {
    if (currentEncoderType_ != EncoderType::Render) {
        std::cerr << "BindGraphicsPipeline: Not in render pass!" << std::endl;
        return;
    }
    
    MetalDevice& metalDevice = static_cast<MetalDevice&>(device_);
    MetalPipeline* mtlPipeline = metalDevice.GetPipeline(pipeline);
    
    if (mtlPipeline && mtlPipeline->GetRenderPipelineState()) {
        MTL::RenderCommandEncoder* encoder = static_cast<MTL::RenderCommandEncoder*>(currentEncoder_);
        encoder->setRenderPipelineState(mtlPipeline->GetRenderPipelineState());
        
        // 更新图元类型
        currentPrimitiveType_ = MetalPipeline::ToMTLPrimitiveType(mtlPipeline->GetGraphicsDesc().topology);
        
        // 设置深度模板状态
        if (mtlPipeline->GetDepthStencilState()) {
            encoder->setDepthStencilState(mtlPipeline->GetDepthStencilState());
        }
        
        // 设置光栅化状态
        const auto& desc = mtlPipeline->GetGraphicsDesc();
        
        MTL::CullMode cullMode = MTL::CullModeNone;
        if (desc.cullMode == CullMode::Front) cullMode = MTL::CullModeFront;
        else if (desc.cullMode == CullMode::Back) cullMode = MTL::CullModeBack;
        encoder->setCullMode(cullMode);
        
        // 默认顺时针，RHI目前没有暴露FrontFace
        encoder->setFrontFacingWinding(MTL::WindingClockwise);
        
        MTL::TriangleFillMode fillMode = (desc.fillMode == FillMode::Wireframe) ? MTL::TriangleFillModeLines : MTL::TriangleFillModeFill;
        encoder->setTriangleFillMode(fillMode);

        // 深度偏差和深度裁剪目前未在 GraphicsPipelineDesc 中暴露
        // encoder->setDepthBias(0, 0, 0);
        // encoder->setDepthClipMode(MTL::DepthClipModeClip);
    } else {
        std::cerr << "BindGraphicsPipeline: Failed to get PSO! Pipeline: " << pipeline << " Ptr: " << mtlPipeline << std::endl;
    }
}

void MetalCommandBuffer::BindVertexBuffers(uint32_t firstSlot, uint32_t slotCount, const ResourceHandle* buffers, const uint64_t* offsets) {
    if (currentEncoderType_ != EncoderType::Render) return;
    
    MTL::RenderCommandEncoder* encoder = static_cast<MTL::RenderCommandEncoder*>(currentEncoder_);
    MetalDevice& metalDevice = static_cast<MetalDevice&>(device_);
    
    for (uint32_t i = 0; i < slotCount; ++i) {
        if (buffers[i] != handles::INVALID_RESOURCE) {
            MetalBuffer* buffer = metalDevice.GetBuffer(buffers[i]);
            if (buffer && buffer->GetNativeBuffer()) {
                NS::UInteger offset = offsets ? offsets[i] : 0;
                encoder->setVertexBuffer(buffer->GetNativeBuffer(), offset, firstSlot + i);
            }
        }
    }
}

void MetalCommandBuffer::BindIndexBuffer(ResourceHandle buffer, DataFormat format, uint64_t offset) {
    MetalDevice& metalDevice = static_cast<MetalDevice&>(device_);
    MetalBuffer* mtlBuffer = metalDevice.GetBuffer(buffer);
    
    if (mtlBuffer && mtlBuffer->GetNativeBuffer()) {
        currentIndexBuffer_ = mtlBuffer->GetNativeBuffer();
        currentIndexBufferOffset_ = offset;
        currentIndexType_ = (format == DataFormat::R16_UInt) ? MTL::IndexTypeUInt16 : MTL::IndexTypeUInt32;
    }
}

void MetalCommandBuffer::BindDescriptorSets(PipelineBindPoint bindPoint,
                                           PipelineLayoutHandle pipelineLayout,
                                           uint32_t firstSet,
                                           uint32_t setCount,
                                           const DescriptorSetHandle* descriptorSets,
                                           uint32_t dynamicOffsetCount,
                                           const uint32_t* dynamicOffsets) {
    if (setCount == 0 || !descriptorSets) return;

    MetalDevice& metalDevice = static_cast<MetalDevice&>(device_);
    
    // Determine encoder based on bindPoint
    MTL::RenderCommandEncoder* renderEncoder = nullptr;
    MTL::ComputeCommandEncoder* computeEncoder = nullptr;
    
    if (bindPoint == PipelineBindPoint::Graphics) {
        if (currentEncoderType_ != EncoderType::Render) return;
        renderEncoder = static_cast<MTL::RenderCommandEncoder*>(currentEncoder_);
    } else {
        if (currentEncoderType_ != EncoderType::Compute) return;
        computeEncoder = static_cast<MTL::ComputeCommandEncoder*>(currentEncoder_);
    }

    uint32_t currentDynamicOffsetIndex = 0;

    for (uint32_t i = 0; i < setCount; ++i) {
        MetalDescriptorSet* set = metalDevice.GetDescriptorSet(descriptorSets[i]);
        if (!set) continue;
        
        const auto& bindings = set->GetBindings();
        for (const auto& binding : bindings) {
            // Simple mapping: binding index = slot index
            // In a real engine, we might remap this based on PipelineLayout or SPIR-V reflection
            uint32_t slot = binding.binding; 
            
            // Apply dynamic offset if needed
            uint32_t dynamicOffset = 0;
             if ((binding.type == DescriptorType::UniformBufferDynamic || 
                  binding.type == DescriptorType::StorageBufferDynamic) && 
                 currentDynamicOffsetIndex < dynamicOffsetCount) {
                 if (dynamicOffsets) {
                    dynamicOffset = dynamicOffsets[currentDynamicOffsetIndex];
                 }
                 currentDynamicOffsetIndex++;
             }

            switch (binding.type) {
                case DescriptorType::UniformBuffer:
                case DescriptorType::UniformBufferDynamic:
                case DescriptorType::StorageBuffer:
                case DescriptorType::StorageBufferDynamic: {
                    if (binding.resources.empty()) break;
                    MetalBuffer* buffer = metalDevice.GetBuffer(binding.resources[0]);
                    if (buffer && buffer->GetNativeBuffer()) {
                        uint64_t offset = (binding.bufferOffsets.empty() ? 0 : binding.bufferOffsets[0]) + dynamicOffset;
                        if (renderEncoder) {
                            renderEncoder->setVertexBuffer(buffer->GetNativeBuffer(), offset, slot);
                            renderEncoder->setFragmentBuffer(buffer->GetNativeBuffer(), offset, slot);
                        } else if (computeEncoder) {
                            computeEncoder->setBuffer(buffer->GetNativeBuffer(), offset, slot);
                        }
                    }
                    break;
                }
                case DescriptorType::SampledImage:
                case DescriptorType::StorageImage: 
                case DescriptorType::CombinedImageSampler: 
                case DescriptorType::InputAttachment: {
                    if (binding.resources.empty()) break;
                    MetalTexture* texture = metalDevice.GetTexture(binding.resources[0]);
                    if (texture && texture->GetNativeTexture()) {
                        if (renderEncoder) {
                            renderEncoder->setFragmentTexture(texture->GetNativeTexture(), slot);
                            renderEncoder->setVertexTexture(texture->GetNativeTexture(), slot);
                        } else if (computeEncoder) {
                            computeEncoder->setTexture(texture->GetNativeTexture(), slot);
                        }
                    }
                    
                    if (binding.type == DescriptorType::CombinedImageSampler && !binding.samplers.empty()) {
                        MetalSampler* sampler = metalDevice.GetSampler(binding.samplers[0]);
                        if (sampler && sampler->GetSamplerState()) {
                             if (renderEncoder) {
                                renderEncoder->setFragmentSamplerState(sampler->GetSamplerState(), slot);
                                renderEncoder->setVertexSamplerState(sampler->GetSamplerState(), slot);
                            } else if (computeEncoder) {
                                computeEncoder->setSamplerState(sampler->GetSamplerState(), slot);
                            }
                        }
                    }
                    break;
                }
                case DescriptorType::Sampler: {
                     if (binding.samplers.empty()) break;
                     MetalSampler* sampler = metalDevice.GetSampler(binding.samplers[0]);
                     if (sampler && sampler->GetSamplerState()) {
                         if (renderEncoder) {
                            renderEncoder->setFragmentSamplerState(sampler->GetSamplerState(), slot);
                            renderEncoder->setVertexSamplerState(sampler->GetSamplerState(), slot);
                        } else if (computeEncoder) {
                            computeEncoder->setSamplerState(sampler->GetSamplerState(), slot);
                        }
                    }
                    break;
                }
                default:
                    break;
            }
        }
    }
}

void MetalCommandBuffer::Draw(uint32_t vertexCount, uint32_t startVertex, uint32_t instanceCount, uint32_t startInstance) {
    if (currentEncoderType_ == EncoderType::Render) {
        static_cast<MTL::RenderCommandEncoder*>(currentEncoder_)->drawPrimitives(
            currentPrimitiveType_,
            (NS::UInteger)startVertex,
            (NS::UInteger)vertexCount,
            (NS::UInteger)instanceCount,
            (NS::UInteger)startInstance
        );
    } else {
        std::cerr << "MetalCommandBuffer::Draw: Not in render pass! Type: " << (int)currentEncoderType_ << std::endl;
    }
}

void MetalCommandBuffer::DrawIndexed(uint32_t indexCount, uint32_t startIndex, uint32_t baseVertex, uint32_t instanceCount, uint32_t startInstance) {
    if (currentEncoderType_ == EncoderType::Render && currentIndexBuffer_) {
        NS::UInteger indexStride = (currentIndexType_ == MTL::IndexTypeUInt16) ? 2 : 4;
        NS::UInteger offset = currentIndexBufferOffset_ + startIndex * indexStride;
        
        static_cast<MTL::RenderCommandEncoder*>(currentEncoder_)->drawIndexedPrimitives(
            currentPrimitiveType_,
            (NS::UInteger)indexCount,
            currentIndexType_,
            currentIndexBuffer_,
            offset,
            (NS::UInteger)instanceCount,
            (NS::UInteger)baseVertex,
            (NS::UInteger)startInstance
        );
    }
}

void MetalCommandBuffer::DrawIndirect(ResourceHandle buffer, uint64_t offset, uint32_t drawCount) {
    if (currentEncoderType_ == EncoderType::Render) {
        MetalDevice& metalDevice = static_cast<MetalDevice&>(device_);
        MetalBuffer* mtlBuffer = metalDevice.GetBuffer(buffer);
        
        if (mtlBuffer && mtlBuffer->GetNativeBuffer()) {
            MTL::RenderCommandEncoder* encoder = static_cast<MTL::RenderCommandEncoder*>(currentEncoder_);
            MTL::Buffer* nativeBuffer = mtlBuffer->GetNativeBuffer();
            
            for (uint32_t i = 0; i < drawCount; ++i) {
                // Metal indirect buffer layout matches RHI
                // MTLDrawPrimitivesIndirectArguments
                encoder->drawPrimitives(
                    currentPrimitiveType_,
                    nativeBuffer,
                    offset + i * sizeof(MTL::DrawPrimitivesIndirectArguments)
                );
            }
        }
    }
}

// === Compute Commands ===

void MetalCommandBuffer::BindComputePipeline(PipelineHandle pipeline) {
    MTL::ComputeCommandEncoder* encoder = getComputeEncoder();
    if (!encoder) return;
    
    MetalDevice& metalDevice = static_cast<MetalDevice&>(device_);
    MetalPipeline* mtlPipeline = metalDevice.GetPipeline(pipeline);
    
    if (mtlPipeline && mtlPipeline->GetComputePipelineState()) {
        encoder->setComputePipelineState(mtlPipeline->GetComputePipelineState());
        currentThreadGroupSize_ = mtlPipeline->GetThreadGroupSize();
    }
}

void MetalCommandBuffer::Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) {
    MTL::ComputeCommandEncoder* encoder = getComputeEncoder();
    if (encoder) {
        encoder->dispatchThreadgroups(
            MTL::Size::Make(groupCountX, groupCountY, groupCountZ),
            currentThreadGroupSize_
        );
    }
}

void MetalCommandBuffer::DispatchIndirect(ResourceHandle buffer, uint64_t offset) {
    MTL::ComputeCommandEncoder* encoder = getComputeEncoder();
    if (!encoder) return;
    
    MetalDevice& metalDevice = static_cast<MetalDevice&>(device_);
    MetalBuffer* mtlBuffer = metalDevice.GetBuffer(buffer);
    
    if (mtlBuffer && mtlBuffer->GetNativeBuffer()) {
        encoder->dispatchThreadgroups(
            mtlBuffer->GetNativeBuffer(),
            offset,
            currentThreadGroupSize_
        );
    }
}

void MetalCommandBuffer::BindComputeBuffers(uint32_t firstSlot, uint32_t slotCount, const ResourceHandle* buffers, const uint64_t* offsets) {
    MTL::ComputeCommandEncoder* encoder = getComputeEncoder();
    if (!encoder) return;
    
    MetalDevice& metalDevice = static_cast<MetalDevice&>(device_);
    
    for (uint32_t i = 0; i < slotCount; ++i) {
        if (buffers[i] != handles::INVALID_RESOURCE) {
            MetalBuffer* buffer = metalDevice.GetBuffer(buffers[i]);
            if (buffer && buffer->GetNativeBuffer()) {
                NS::UInteger offset = offsets ? offsets[i] : 0;
                encoder->setBuffer(buffer->GetNativeBuffer(), offset, firstSlot + i);
            }
        }
    }
}

// === Resource Commands ===

void MetalCommandBuffer::CopyBuffer(ResourceHandle src, ResourceHandle dst, uint64_t srcOffset, uint64_t dstOffset, uint64_t size) {
    MTL::BlitCommandEncoder* encoder = getBlitEncoder();
    if (!encoder) return;
    
    MetalDevice& metalDevice = static_cast<MetalDevice&>(device_);
    MetalBuffer* srcBuf = metalDevice.GetBuffer(src);
    MetalBuffer* dstBuf = metalDevice.GetBuffer(dst);
    
    if (srcBuf && dstBuf && srcBuf->GetNativeBuffer() && dstBuf->GetNativeBuffer()) {
        encoder->copyFromBuffer(
            srcBuf->GetNativeBuffer(),
            srcOffset,
            dstBuf->GetNativeBuffer(),
            dstOffset,
            size
        );
    }
}

void MetalCommandBuffer::InsertBarrier(const ResourceBarrier* barriers, uint32_t barrierCount) {
    // Metal handles many barriers implicitly, but fences/events might be needed for specific synchronization.
    // For now, empty.
}

} // namespace primal::graphics::rhi
