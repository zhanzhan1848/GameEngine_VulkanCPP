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
#include "MetalDescriptorSet.h"
#include "MetalTexture.h"
#include "MetalQuery.h"
#include <iostream>
#include <thread>

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
        auto mtlCmdBuf = mtlCommandBuffer_;
        device_.GetGarbageCollector().DeferredDestroy([mtlCmdBuf]() {
            mtlCmdBuf->release();
        });
        mtlCommandBuffer_ = nullptr;
    }
    
    if (pool_) {
        if (std::this_thread::get_id() == recordingThreadId_) {
            pool_->release();
        } else {
            std::cerr << "[MetalCommandBuffer] Warning: destroyImpl called on wrong thread (" 
                      << std::this_thread::get_id() << "), expected " << recordingThreadId_ 
                      << ". Leaking pool." << std::endl;
        }
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
    // Do NOT call destroyImpl() as it destroys the guardEventHandle_ which is needed for reuse
    
    // Reset state tracking
    currentPrimitiveType_ = MTL::PrimitiveTypeTriangle;
    currentIndexType_ = MTL::IndexTypeUInt32;
    currentIndexBuffer_ = nullptr;
    currentIndexBufferOffset_ = 0;
    currentThreadGroupSize_ = MTL::Size::Make(1, 1, 1);
    
    if (isSecondary_) {
        // Secondary buffers don't own the encoder or command buffer
        currentEncoder_ = nullptr;
        currentEncoderType_ = EncoderType::None;
        return true;
    }

    endCurrentEncoder();

    if (mtlCommandBuffer_) {
        auto mtlCmdBuf = mtlCommandBuffer_;
        device_.GetGarbageCollector().DeferredDestroy([mtlCmdBuf]() {
            mtlCmdBuf->release();
        });
        mtlCommandBuffer_ = nullptr;
    }
    
    if (pool_) {
        if (std::this_thread::get_id() == recordingThreadId_) {
            pool_->release();
        } else {
            std::cerr << "[MetalCommandBuffer] Warning: resetImpl called on wrong thread (" 
                      << std::this_thread::get_id() << "), expected " << recordingThreadId_ 
                      << ". Leaking pool." << std::endl;
        }
        pool_ = nullptr;
    }

    return true;
}

bool MetalCommandBuffer::beginImpl() {
    if (mtlCommandBuffer_) {
        return false; // Already recording or recorded
    }

    recordingThreadId_ = std::this_thread::get_id();
    // std::cout << "[MetalCommandBuffer] BeginImpl: " << this << " Thread: " << recordingThreadId_ << std::endl;

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
    
    // std::cout << "[MetalCommandBuffer] EndImpl: " << this << " Thread: " << std::this_thread::get_id() << std::endl;

    // Release the autorelease pool created in Begin()
    // This MUST be done on the same thread that called Begin()
    if (pool_) {
        if (std::this_thread::get_id() == recordingThreadId_) {
            pool_->release();
        } else {
            std::cerr << "[MetalCommandBuffer] Error: EndImpl called on wrong thread (" 
                      << std::this_thread::get_id() << "), expected " << recordingThreadId_ 
                      << ". Leaking pool." << std::endl;
        }
        pool_ = nullptr;
    }
    
    return true;
}

bool MetalCommandBuffer::submitImpl(u32 waitFlags) {
    if (!mtlCommandBuffer_) return false;

    // Defensive: ensure no encoder is active before commit
    endCurrentEncoder();

    // Process wait semaphores
    for (const auto& semInfo : waitSemaphores_) {
        MetalDevice& metalDevice = static_cast<MetalDevice&>(device_);
        MetalSync* sync = metalDevice.GetSync(semInfo.semaphore);
        if (sync && sync->GetNativeEvent()) {
            mtlCommandBuffer_->encodeWait(sync->GetNativeEvent(), semInfo.value);
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

    // Add completion handler for GPU timing - REMOVED due to Use-After-Free risk
    // Accessing 'this' in the completion handler is unsafe because the CommandBuffer object
    // might be destroyed/pooled before the GPU finishes execution.
    // Timing stats will be updated in WaitForCompletion() instead.
    /*
    if (mtlCommandBuffer_) {
        MetalCommandBuffer* self = this;
         mtlCommandBuffer_->addCompletedHandler([self](MTL::CommandBuffer* buffer) {
             CFTimeInterval start = buffer->GPUStartTime();
             CFTimeInterval end = buffer->GPUEndTime();
             if (start != 0 && end != 0) {
                  self->stats_.commandExecutionTime = (float)((end - start) * 1000.0);
             }
         });
     }
    */

    mtlCommandBuffer_->commit();
    
    return true;
}

bool MetalCommandBuffer::waitForCompletionImpl() {
    if (!mtlCommandBuffer_) return false;
    
    mtlCommandBuffer_->waitUntilCompleted();
    
    // Update execution time stats safely here
    CFTimeInterval start = mtlCommandBuffer_->GPUStartTime();
    CFTimeInterval end = mtlCommandBuffer_->GPUEndTime();
    if (start != 0 && end != 0) {
        stats_.commandExecutionTime = (float)((end - start) * 1000.0);
    }
    
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
    static int passCount = 0;
    passCount++;
    
    // Log first 5 render passes with texture details
    if (passCount <= 5) {
        std::cout << "[Metal] BeginRenderPass #" << passCount << ": colorAttachments=" << desc.colorAttachments.size();
        if (!desc.colorAttachments.empty()) {
            std::cout << " texture[0]=" << desc.colorAttachments[0].texture
                      << " loadOp=" << (int)desc.colorAttachments[0].loadOp;
        }
        std::cout << std::endl;
    }
    
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
                // std::cout << "[MetalCommandBuffer] Bound color attachment " << i << " to texture " << texture->GetNativeTexture() << std::endl;
                ca->setSlice(attachment.arrayLayer);
                ca->setLevel(attachment.mipLevel);

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
            } else {
                 std::cerr << "[MetalCommandBuffer] Failed to bind color attachment " << i << ": Texture or NativeTexture is null" << std::endl;
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
            da->setSlice(desc.depthAttachment.arrayLayer);
            da->setLevel(desc.depthAttachment.mipLevel);
            
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

    // Set Render Target Array Length (For Layered Rendering)
    if (desc.renderTargetArrayLength > 1) {
        passDesc->setRenderTargetArrayLength(desc.renderTargetArrayLength);
    }

    // Setup Timestamp Query
    if (desc.enableTimestamp && desc.timestampQueryPool != handles::INVALID_QUERY_POOL) {
        MetalDevice& metalDevice = static_cast<MetalDevice&>(device_);
        MetalQueryPool* pool = metalDevice.GetQueryPool(desc.timestampQueryPool);
        
        if (pool && pool->GetType() == MetalQueryType::Timestamp) {
            MTL::CounterSampleBuffer* buffer = pool->GetNativeBuffer();
            if (buffer) {
                MTL::RenderPassSampleBufferAttachmentDescriptor* attachment = passDesc->sampleBufferAttachments()->object(0);
                attachment->setSampleBuffer(buffer);
                attachment->setStartOfVertexSampleIndex(desc.beginTimestampIndex);
                attachment->setEndOfFragmentSampleIndex(desc.endTimestampIndex);
            }
        }
    }
    
    if (mtlCommandBuffer_) {
                MTL::RenderCommandEncoder* encoder = mtlCommandBuffer_->renderCommandEncoder(passDesc);
                if (!encoder) {
                    std::cerr << "[MetalCommandBuffer] Failed to create render command encoder!" << std::endl;
                    if (passDesc->colorAttachments()->object(0)->texture() == nullptr) {
                         std::cerr << "  Color Attachment 0 texture is null" << std::endl;
                    }
                }
                currentEncoder_ = encoder;
                currentEncoderType_ = EncoderType::Render;
                
                // Initial state setup (viewport, scissor)
                SetViewport(desc.viewport);
                SetScissor(desc.scissor);
            } else {
                std::cerr << "[MetalCommandBuffer] mtlCommandBuffer_ is null in BeginRenderPass!" << std::endl;
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

        if (!encoder) {
            static int nullEncoderCount = 0;
            nullEncoderCount++;
            if (nullEncoderCount <= 3) {
                std::cerr << "[MetalCommandBuffer] CRITICAL: renderCommandEncoder returned nilULL!" << std::endl;
                std::cerr << "  This means the render pass will be completely silent (no draws, no depth writes)." << std::endl;
            }
        } else {
            // DIAGNOSTIC: Check ALL attachments in the descriptor
            static int okCount = 0;
            okCount++;
            if (okCount <= 3) {
                auto* colorAttachments = passDesc->colorAttachments();
                int colorCount = 0;
                for (int i = 0; i < 8; ++i) {
                    auto* ca = colorAttachments->object(i);
                    if (ca && ca->texture()) colorCount++;
                }
                auto* depthDesc = passDesc->depthAttachment();
                std::cout << "[MetalCMD] BeginRenderPass OK: colorAttachments=" << colorCount
                          << " depthTex=" << (void*)(depthDesc ? depthDesc->texture() : nullptr)
                          << " depthLoad=" << (depthDesc ? (int)depthDesc->loadAction() : -1)
                          << " descColors=" << pass->GetDesc().colorAttachments.size()
                          << std::endl;
                // Print each color attachment texture pointer
                for (size_t i = 0; i < pass->GetDesc().colorAttachments.size() && i < 4; ++i) {
                    auto* ca = colorAttachments->object(i);
                    std::cout << "  color[" << i << "] tex=" << (void*)(ca ? ca->texture() : nullptr)
                              << " loadAction=" << (ca ? (int)ca->loadAction() : -1)
                              << " storeAction=" << (ca ? (int)ca->storeAction() : -1) << std::endl;
                }
            }
        }

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
            da->setSlice(desc.depthAttachment.arrayLayer);
            da->setLevel(desc.depthAttachment.mipLevel);
            
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
    // DEBUG: Log viewport settings for debug pass
    static int viewportCount = 0;
    viewportCount++;
    if (viewportCount > 100 && viewportCount <= 105) {
        std::cout << "[Metal] SetViewport #" << viewportCount 
                  << " x=" << viewport.topLeft.x 
                  << " y=" << viewport.topLeft.y
                  << " w=" << viewport.size.x 
                  << " h=" << viewport.size.y << std::endl;
    }
    
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
    
    // DEBUG: Log pipeline binding for meshlet debug
    static int bindCount = 0;
    bindCount++;
    if (bindCount > 10000 && bindCount <= 10010) {
        std::cout << "[Metal] BindGraphicsPipeline #" << bindCount 
                  << " pipeline=" << (mtlPipeline ? "valid" : "NULL")
                  << " encoder=" << (currentEncoder_ ? "valid" : "NULL") << std::endl;
    }
    
    if (mtlPipeline && mtlPipeline->GetRenderPipelineState()) {
        MTL::RenderCommandEncoder* encoder = static_cast<MTL::RenderCommandEncoder*>(currentEncoder_);
        encoder->setRenderPipelineState(mtlPipeline->GetRenderPipelineState());
        
        // 更新图元类型
        currentPrimitiveType_ = MetalPipeline::ToMTLPrimitiveType(mtlPipeline->GetGraphicsDesc().topology);
        
        // 设置深度模板状态
        if (mtlPipeline->GetDepthStencilState()) {
            encoder->setDepthStencilState(mtlPipeline->GetDepthStencilState());
        }

        // Set Depth Bias
        encoder->setDepthBias(mtlPipeline->GetDepthBias(), mtlPipeline->GetSlopeScaledDepthBias(), mtlPipeline->GetDepthBiasClamp());
        
        // 设置光栅化状态
        const auto& desc = mtlPipeline->GetGraphicsDesc();
        
        MTL::CullMode cullMode = MTL::CullModeNone;
        if (desc.cullMode == CullMode::Front) cullMode = MTL::CullModeFront;
        else if (desc.cullMode == CullMode::Back) cullMode = MTL::CullModeBack;
        encoder->setCullMode(cullMode);
        
        // 默认逆时针为正面，符合Vulkan/OpenGL习惯
        encoder->setFrontFacingWinding(MTL::WindingCounterClockwise);
        
        MTL::TriangleFillMode fillMode = (desc.fillMode == FillMode::Wireframe) ? MTL::TriangleFillModeLines : MTL::TriangleFillModeFill;
        encoder->setTriangleFillMode(fillMode);

        // Depth Clip Mode (Default to Clip)
        // encoder->setDepthClipMode(MTL::DepthClipModeClip);
    } else {
        std::cerr << "BindGraphicsPipeline: Failed to get PSO! Pipeline: " << pipeline << " Ptr: " << mtlPipeline << std::endl;
    }
}

void MetalCommandBuffer::BindVertexBuffers(u32 firstSlot, u32 slotCount, const ResourceHandle* buffers, const u64* offsets) {
    if (currentEncoderType_ != EncoderType::Render) return;
    
    // std::cout << "[MetalCommandBuffer] BindVertexBuffers: first=" << firstSlot << " count=" << slotCount << std::endl;

    MetalDevice& metalDevice = static_cast<MetalDevice&>(device_);

    for (u32 i = 0; i < slotCount; ++i) {
        if (buffers[i] != handles::INVALID_RESOURCE) {
            MetalBuffer* buffer = metalDevice.GetBuffer(buffers[i]);
            if (buffer && buffer->GetNativeBuffer()) {
                u64 offset = offsets ? offsets[i] : 0;
                static_cast<MTL::RenderCommandEncoder*>(currentEncoder_)->setVertexBuffer(
                    buffer->GetNativeBuffer(),
                    offset,
                    firstSlot + i
                );
    // std::cout << "[MetalCommandBuffer] Bound vertex buffer " << (firstSlot + i) << " (Size: " << buffer->GetDesc().size << ")" << std::endl;
            } else {
                 std::cerr << "[MetalCommandBuffer] Failed to bind vertex buffer " << (firstSlot + i) << ": Buffer is null" << std::endl;
            }
        }
    }
}

void MetalCommandBuffer::BindIndexBuffer(ResourceHandle buffer, DataFormat format, u64 offset) {
    if (currentEncoderType_ != EncoderType::Render) return;
    
    MetalDevice& metalDevice = static_cast<MetalDevice&>(device_);
    MetalBuffer* metalBuffer = metalDevice.GetBuffer(buffer);
    
    if (metalBuffer && metalBuffer->GetNativeBuffer()) {
        currentIndexBuffer_ = metalBuffer->GetNativeBuffer();
        currentIndexBufferOffset_ = offset;
        currentIndexType_ = (format == DataFormat::R16_UInt) ? MTL::IndexTypeUInt16 : MTL::IndexTypeUInt32;
    // std::cout << "[MetalCommandBuffer] Bound index buffer (Offset: " << offset << ")" << std::endl;
    } else {
         std::cerr << "[MetalCommandBuffer] Failed to bind index buffer: Buffer is null" << std::endl;
         currentIndexBuffer_ = nullptr;
    }
}

void MetalCommandBuffer::BindDescriptorSets(PipelineBindPoint bindPoint,
                                           PipelineLayoutHandle pipelineLayout,
                                           u32 firstSet,
                                           u32 setCount,
                                           const DescriptorSetHandle* descriptorSets,
                                           u32 dynamicOffsetCount,
                                           const u32* dynamicOffsets) {
    if (setCount == 0 || !descriptorSets) return;

    if (std::this_thread::get_id() != recordingThreadId_) {
         std::cerr << "[MetalCommandBuffer] Error: BindDescriptorSets called on wrong thread (" 
                   << std::this_thread::get_id() << "), expected " << recordingThreadId_ << std::endl;
    }

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

    u32 currentDynamicOffsetIndex = 0;

    for (u32 i = 0; i < setCount; ++i) {
        MetalDescriptorSet* set = metalDevice.GetDescriptorSet(descriptorSets[i]);
        if (!set) continue;
        
        const auto& bindings = set->GetBindings();
        for (const auto& binding : bindings) {
            // Simple mapping: binding index = slot index
            // In a real engine, we might remap this based on PipelineLayout or SPIR-V reflection
            u32 slot = binding.binding; 
            
            // Apply dynamic offset if needed
            u32 dynamicOffset = 0;
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
                                u64 offset = (binding.bufferOffsets.empty() ? 0 : binding.bufferOffsets[0]) + dynamicOffset;

                                // DEBUG: Log uniform buffer binding
                                if (binding.type == DescriptorType::UniformBuffer && slot == 3) {
                                    // std::cout << "[MetalCommandBuffer] DEBUG - Binding uniform buffer to slot 3:" << std::endl;
                                    // std::cout << "  Buffer native pointer: " << buffer->GetNativeBuffer() << std::endl;
                                    // std::cout << "  Offset: " << offset << std::endl;
                                    // std::cout << "  Buffer contents address: " << buffer->GetNativeBuffer()->contents() << std::endl;

                                    // Debug: Check data at different offsets
                                    float* testData = static_cast<float*>(buffer->GetNativeBuffer()->contents());
                                    // std::cout << "  Offset 0 (view_matrix[0]): " << testData[0] << ", " << testData[1] << ", " << testData[2] << ", " << testData[3] << std::endl;
                                    // std::cout << "  Offset 16 (view_matrix[1]): " << testData[4] << ", " << testData[5] << ", " << testData[6] << ", " << testData[7] << std::endl;

                                    // Check camera_position at offset 192 bytes = 48 floats
                                    // std::cout << "  Offset 192 (camera_position): " << testData[48] << ", " << testData[49] << ", " << testData[50] << std::endl;

                                    // Check frame_index at offset 216 bytes = 54 floats
                                    u32* uintData = reinterpret_cast<u32*>(testData);
                                    // std::cout << "  Offset 216 (frame_index): " << uintData[54] << std::endl;
                                }

                                if (renderEncoder) {
                                    if (static_cast<u8>(binding.stageFlags) & static_cast<u8>(ShaderStage::Vertex))
                                        renderEncoder->setVertexBuffer(buffer->GetNativeBuffer(), offset, slot);
                            if (static_cast<u8>(binding.stageFlags) & static_cast<u8>(ShaderStage::Pixel))
                                renderEncoder->setFragmentBuffer(buffer->GetNativeBuffer(), offset, slot);
                        } else if (computeEncoder) {
                            if (static_cast<u8>(binding.stageFlags) & static_cast<u8>(ShaderStage::Compute))
                                computeEncoder->setBuffer(buffer->GetNativeBuffer(), offset, slot);
                        }
                    } else {
                        // std::cerr << "[MetalCommandBuffer] ERROR - Failed to bind buffer at slot " << slot
                        //           << " (binding type: " << (int)binding.type << ")" << std::endl;
                        if (!buffer) {
                            std::cerr << "  Buffer is null" << std::endl;
                        } else if (!buffer->GetNativeBuffer()) {
                            std::cerr << "  Native buffer is null" << std::endl;
                        }
                    }
                    break;
                }
                case DescriptorType::SampledImage:
                case DescriptorType::StorageImage: 
                case DescriptorType::CombinedImageSampler: 
                case DescriptorType::InputAttachment: {
                    if (binding.resources.empty()) break;

                    // Support for Descriptor Arrays
                    utl::vector<MTL::Texture*> mtlTextures;
                    mtlTextures.reserve(binding.resources.size());
                    
                    for (auto handle : binding.resources) {
                         MetalTexture* texture = metalDevice.GetTexture(handle);
                         mtlTextures.push_back((texture && texture->GetNativeTexture()) ? texture->GetNativeTexture() : nullptr);
                    }
                    
                    utl::vector<MTL::SamplerState*> mtlSamplers;
                    if (binding.type == DescriptorType::CombinedImageSampler && !binding.samplers.empty()) {
                        mtlSamplers.reserve(binding.samplers.size());
                        for (auto handle : binding.samplers) {
                             MetalSampler* sampler = metalDevice.GetSampler(handle);
                             mtlSamplers.push_back((sampler && sampler->GetSamplerState()) ? sampler->GetSamplerState() : nullptr);
                        }
                    }
                    
                    NS::Range range(slot, mtlTextures.size());
                    
                    if (renderEncoder) {
                        if (static_cast<u8>(binding.stageFlags) & static_cast<u8>(ShaderStage::Vertex))
                            renderEncoder->setVertexTextures(mtlTextures.data(), range);
                        if (static_cast<u8>(binding.stageFlags) & static_cast<u8>(ShaderStage::Pixel))
                            renderEncoder->setFragmentTextures(mtlTextures.data(), range);
                    } else if (computeEncoder) {
                        if (static_cast<u8>(binding.stageFlags) & static_cast<u8>(ShaderStage::Compute))
                            computeEncoder->setTextures(mtlTextures.data(), range);
                    }
                    
                    // Bind Samplers (Array)
                    if (!mtlSamplers.empty()) {
                        NS::Range samplerRange(slot, mtlSamplers.size());
                        if (renderEncoder) {
                             if (static_cast<u8>(binding.stageFlags) & static_cast<u8>(ShaderStage::Vertex))
                                renderEncoder->setVertexSamplerStates(mtlSamplers.data(), samplerRange);
                             if (static_cast<u8>(binding.stageFlags) & static_cast<u8>(ShaderStage::Pixel))
                                renderEncoder->setFragmentSamplerStates(mtlSamplers.data(), samplerRange);
                        } else if (computeEncoder) {
                             if (static_cast<u8>(binding.stageFlags) & static_cast<u8>(ShaderStage::Compute))
                                computeEncoder->setSamplerStates(mtlSamplers.data(), samplerRange);
                        }
                    }
                    
                    break;
                }
                case DescriptorType::Sampler: {
                     if (binding.samplers.empty()) break;
                     
                     utl::vector<MTL::SamplerState*> mtlSamplers;
                     mtlSamplers.reserve(binding.samplers.size());
                     
                     for (auto handle : binding.samplers) {
                         MetalSampler* sampler = metalDevice.GetSampler(handle);
                         mtlSamplers.push_back((sampler && sampler->GetSamplerState()) ? sampler->GetSamplerState() : nullptr);
                     }
                     
                     if (!mtlSamplers.empty()) {
                         NS::Range range(slot, mtlSamplers.size());
                         if (renderEncoder) {
                            if (static_cast<u8>(binding.stageFlags) & static_cast<u8>(ShaderStage::Vertex))
                                renderEncoder->setVertexSamplerStates(mtlSamplers.data(), range);
                            if (static_cast<u8>(binding.stageFlags) & static_cast<u8>(ShaderStage::Pixel))
                                renderEncoder->setFragmentSamplerStates(mtlSamplers.data(), range);
                        } else if (computeEncoder) {
                            if (static_cast<u8>(binding.stageFlags) & static_cast<u8>(ShaderStage::Compute))
                                computeEncoder->setSamplerStates(mtlSamplers.data(), range);
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

void MetalCommandBuffer::WriteTimestamp(QueryPoolHandle queryPool, u32 queryIndex) {
    MetalDevice& metalDevice = static_cast<MetalDevice&>(device_);
    MetalQueryPool* pool = metalDevice.GetQueryPool(queryPool);
    if (!pool || pool->GetType() != MetalQueryType::Timestamp) return;
    
    MTL::CounterSampleBuffer* buffer = pool->GetNativeBuffer();
    if (!buffer) return;
    
    if (currentEncoderType_ == EncoderType::None) {
        getBlitEncoder();
    }
    
    switch (currentEncoderType_) {
        case EncoderType::Render: {
            auto encoder = static_cast<MTL::RenderCommandEncoder*>(currentEncoder_);
            // Apple Silicon devices do not support sampleCountersInBuffer in Render Encoder.
            // Use RenderPassDesc sampleBufferAttachments instead.
            // encoder->sampleCountersInBuffer(buffer, queryIndex, true);
            break;
        }
        case EncoderType::Compute: {
            auto encoder = static_cast<MTL::ComputeCommandEncoder*>(currentEncoder_);
            // Similarly for Compute
            // encoder->sampleCountersInBuffer(buffer, queryIndex, true);
            break;
        }
        case EncoderType::Blit: {
            auto encoder = static_cast<MTL::BlitCommandEncoder*>(currentEncoder_);
            // Similarly for Blit
            // encoder->sampleCountersInBuffer(buffer, queryIndex, true);
            break;
        }
        default:
            break;
    }
}

void MetalCommandBuffer::Draw(u32 vertexCount, u32 startVertex, u32 instanceCount, u32 startInstance) {
    // DEBUG: Log ALL draw calls for first 10 calls
    static int totalDrawCount = 0;
    totalDrawCount++;
    if (totalDrawCount <= 10) {
        std::cout << "[Metal] DRAW #" << totalDrawCount << ": vertexCount=" << vertexCount 
                  << " instanceCount=" << instanceCount 
                  << " encoderType=" << (int)currentEncoderType_ << std::endl;
    }
    
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

void MetalCommandBuffer::DrawIndexed(u32 indexCount, u32 startIndex, u32 baseVertex, u32 instanceCount, u32 startInstance) {
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
    } else {
        if (currentEncoderType_ != EncoderType::Render) {
             std::cerr << "MetalCommandBuffer::DrawIndexed: Not in render pass! Type: " << (int)currentEncoderType_ << std::endl;
        } else if (!currentIndexBuffer_) {
             std::cerr << "MetalCommandBuffer::DrawIndexed: No Index Buffer Bound!" << std::endl;
        }
    }
}

void MetalCommandBuffer::DrawIndirect(ResourceHandle buffer, u64 offset, u32 drawCount) {
    if (currentEncoderType_ == EncoderType::Render) {
        MetalDevice& metalDevice = static_cast<MetalDevice&>(device_);
        MetalBuffer* mtlBuffer = metalDevice.GetBuffer(buffer);

        if (mtlBuffer && mtlBuffer->GetNativeBuffer()) {
            MTL::RenderCommandEncoder* encoder = static_cast<MTL::RenderCommandEncoder*>(currentEncoder_);

            // DIAGNOSTIC: Check if encoder is valid
            if (!encoder) {
                static int nullEncoderDrawCount = 0;
                nullEncoderDrawCount++;
                if (nullEncoderDrawCount <= 3) {
                    std::cerr << "[MetalCMD] DrawIndirect ERROR: encoder is NULL! Draws will be silently dropped." << std::endl;
                }
                return;
            }

            MTL::Buffer* nativeBuffer = mtlBuffer->GetNativeBuffer();

            // Use Metal's proper indirect drawing API
            for (u32 i = 0; i < drawCount; ++i) {
                encoder->drawPrimitives(
                    currentPrimitiveType_,
                    nativeBuffer,
                    offset + i * sizeof(MTL::DrawPrimitivesIndirectArguments)
                );
            }

            // DIAGNOSTIC: Verify draw call on first few frames
            {
                static u32 drawDiagCount = 0;
                drawDiagCount++;
                if (drawDiagCount <= 5) {
                    // Read indirect args from CPU (this is the buffer from 2 frames ago)
                    void* mapped = nativeBuffer->contents();
                    if (mapped) {
                        u32* args = reinterpret_cast<u32*>((u8*)mapped + offset);
                        std::cout << "[MetalCMD] DrawIndirect #" << drawDiagCount
                                  << " vertexStart=" << args[0]
                                  << " vertexCount=" << args[1]
                                  << " instanceCount=" << args[2]
                                  << " instanceStart=" << args[3] << std::endl;
                    }
                }
            }
        } else {
            // std::cerr << "[Metal] DrawIndirect ERROR: Invalid buffer or native buffer is null!" << std::endl;
            if (!mtlBuffer) {
                std::cerr << "  MetalBuffer is null" << std::endl;
            } else {
                std::cerr << "  NativeBuffer is null" << std::endl;
            }
        }
    } else {
        std::cerr << "[Metal] DrawIndirect ERROR: Not in render encoder! Current encoder type: " << (int)currentEncoderType_ << std::endl;
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

void MetalCommandBuffer::PushConstants(PipelineLayoutHandle layout, ShaderStage stageFlags,
                                     u32 offset, u32 size, const void* pValues) {
    // Metal assumes push constants are passed as bytes at a reserved index (e.g. 0 or based on reflection)
    // Since MetalPipelineLayout doesn't currently store this info, we'll try a convention or just warn.
    // For now, let's assume index 0 for push constants if they are small enough (< 4KB).
    // Note: This might conflict with other buffers if not managed carefully.
    
    // Convention: Push constants often mapped to buffer index 0 or a high index. 
    // Without reflection data, we can't be sure.
    // However, to satisfy the linker and provide basic functionality:
    
    // Only support max 4KB as per Metal set*Bytes limit
    if (size > 4096) {
        std::cerr << "MetalCommandBuffer::PushConstants: Size too large (" << size << ")" << std::endl;
        return;
    }
    
    if (currentEncoderType_ == EncoderType::Render) {
        auto encoder = static_cast<MTL::RenderCommandEncoder*>(currentEncoder_);
        if (static_cast<u32>(stageFlags) & static_cast<u32>(ShaderStage::Vertex)) {
            // Assuming index 2 for now to avoid conflict with SceneData(0) and ViewData(1)
             encoder->setVertexBytes(pValues, size, 2); 
        }
        if (static_cast<u32>(stageFlags) & static_cast<u32>(ShaderStage::Pixel)) {
             encoder->setFragmentBytes(pValues, size, 2);
        }
    } else if (currentEncoderType_ == EncoderType::Compute) {
        auto encoder = static_cast<MTL::ComputeCommandEncoder*>(currentEncoder_);
        encoder->setBytes(pValues, size, 2);
    }
}

void MetalCommandBuffer::SetComputeBytes(u32 index, const void* data, u32 size) {
    auto encoder = getComputeEncoder();
    if (encoder) {
        encoder->setBytes(data, size, index);
    }
}

void MetalCommandBuffer::Dispatch(u32 groupCountX, u32 groupCountY, u32 groupCountZ) {
    MTL::ComputeCommandEncoder* encoder = getComputeEncoder();
    if (encoder) {
        encoder->dispatchThreadgroups(
            MTL::Size::Make(groupCountX, groupCountY, groupCountZ),
            currentThreadGroupSize_
        );
    }
}

void MetalCommandBuffer::DispatchIndirect(ResourceHandle buffer, u64 offset) {
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

void MetalCommandBuffer::BindComputeBuffers(u32 firstSlot, u32 slotCount, const ResourceHandle* buffers, const u64* offsets) {
    MTL::ComputeCommandEncoder* encoder = getComputeEncoder();
    if (!encoder) return;
    
    MetalDevice& metalDevice = static_cast<MetalDevice&>(device_);
    
    for (u32 i = 0; i < slotCount; ++i) {
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

void MetalCommandBuffer::CopyBuffer(ResourceHandle src, ResourceHandle dst, u64 srcOffset, u64 dstOffset, u64 size) {
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

void MetalCommandBuffer::InsertBarrier(const ResourceBarrier* barriers, u32 barrierCount) {
    if (!currentEncoder_ || barrierCount == 0) return;

    switch (currentEncoderType_) {
        case EncoderType::Compute: {
            auto* computeEncoder = static_cast<MTL::ComputeCommandEncoder*>(currentEncoder_);

            // Accumulate barrier scope based on actual resource types
            MetalDevice& metalDevice = static_cast<MetalDevice&>(device_);
            MTL::BarrierScope scope = 0;
            for (u32 i = 0; i < barrierCount; ++i) {
                auto handle = barriers[i].resource;
                if (handle == handles::INVALID_RESOURCE) continue;
                // Distinguish texture vs buffer by trying both allocators
                if (metalDevice.GetTexture(handle) != nullptr) {
                    scope |= MTL::BarrierScopeTextures;
                } else if (metalDevice.GetBuffer(handle) != nullptr) {
                    scope |= MTL::BarrierScopeBuffers;
                }
            }

            // Fallback: if no specific resources matched, barrier both scopes
            if (scope == 0) {
                scope = MTL::BarrierScopeTextures | MTL::BarrierScopeBuffers;
            }

            computeEncoder->memoryBarrier(scope);
            break;
        }
        case EncoderType::Render: {
            // Render encoder doesn't have memoryBarrier; handled implicitly by Metal.
            break;
        }
        case EncoderType::Blit:
            // Blit operations are serialized within the encoder; no explicit barrier needed.
            break;
        default:
            break;
    }
}

void MetalCommandBuffer::MemoryBarrier(PipelineStage srcStageMask, PipelineStage dstStageMask,
                                      AccessFlag srcAccessMask, AccessFlag dstAccessMask) {
    // SPIKE-VALIDATED (2026-07-02, TestMetalEncoderBarrierSpike):
    //   For pure Compute→Compute transitions on the same compute encoder,
    //   `computeEncoder->memoryBarrier(scope)` ALONE is sufficient — the
    //   subsequent dispatch sees all prior writes. No encoder end needed.
    //
    //   We still must endCurrentEncoder() when:
    //     - dst includes non-Compute bits (DrawIndirect / VertexInput / Transfer / Host)
    //       → next call needs a different encoder type
    //     - src includes Host/Transfer → producer lived in a different encoder type
    //     - current encoder is not Compute (Render/Blit barriers handled by Metal implicitly
    //       via encoder boundaries, but we still need to end to switch types)

    if (currentEncoder_ == nullptr) return;

    if (currentEncoderType_ == EncoderType::Compute) {
        auto computeEncoder = static_cast<MTL::ComputeCommandEncoder*>(currentEncoder_);
        computeEncoder->memoryBarrier(MTL::BarrierScopeBuffers);

        static int barrierCount = 0;
        if (barrierCount < 30) {
            std::cerr << "[MetalBarrier] Compute memoryBarrier(scope) #" << barrierCount
                      << std::endl;
            ++barrierCount;
        }

        // Exact Compute→Compute (no other bits)? Keep encoder open.
        constexpr u32 kComputeOnly = static_cast<u32>(PipelineStage::ComputeShader);
        bool srcIsComputeOnly = static_cast<u32>(srcStageMask) == kComputeOnly;
        bool dstIsComputeOnly = static_cast<u32>(dstStageMask) == kComputeOnly;

        // src includes Host/Transfer? Producer was a different encoder type — end.
        u32 srcNonComputeBits = static_cast<u32>(srcStageMask) &
            (static_cast<u32>(PipelineStage::Host) |
             static_cast<u32>(PipelineStage::Transfer));
        bool srcFromOtherEncoder = srcNonComputeBits != 0;

        if (!srcIsComputeOnly || !dstIsComputeOnly || srcFromOtherEncoder) {
            if (barrierCount <= 30) {
                std::cerr << "[MetalBarrier] END encoder: src=0x" << std::hex
                          << static_cast<u32>(srcStageMask)
                          << " dst=0x" << static_cast<u32>(dstStageMask)
                          << std::dec << std::endl;
            }
            endCurrentEncoder();
        }
        // else: pure Compute→Compute on same encoder — keep open (spike validated)
    }
    else {
        // Render / Blit / other: end encoder to switch types
        endCurrentEncoder();
    }
}

    // Helper to get pixel format info for block-based calculation
    struct PixelFormatInfo {
        u32 bytesPerBlock;
        u32 blockWidth;
        u32 blockHeight;
    };

    static PixelFormatInfo GetPixelFormatInfo(MTL::PixelFormat format) {
        // Debug print to ensure we are running the new version
        u64 fmt = (u64)format;
        // std::cout << "DEBUG: GetPixelFormatInfo(" << fmt << ")" << std::endl;
        switch (fmt) {
            // Uncompressed formats
            case 10: // MTL::PixelFormatR8Unorm
            case 13: // MTL::PixelFormatR8Uint
            case 14: // MTL::PixelFormatR8Sint
                // std::cout << "DEBUG: Hit case 10/13/14" << std::endl;
                return {1, 1, 1};
            case 25: // MTL::PixelFormatR16Float
            case 26: // MTL::PixelFormatR16Uint
            case 27: // MTL::PixelFormatR16Sint
            case 20: // MTL::PixelFormatR16Unorm
                // std::cout << "DEBUG: Hit case 25/26/27/20" << std::endl;
                return {2, 1, 1};
            case 65: // MTL::PixelFormatRG16Float
            case 66: // MTL::PixelFormatRG16Uint
            case 67: // MTL::PixelFormatRG16Sint
            case 60: // MTL::PixelFormatRG16Unorm
                return {4, 1, 1};
            case MTL::PixelFormatRGBA8Unorm:
            case MTL::PixelFormatRGBA8Unorm_sRGB:
            case MTL::PixelFormatBGRA8Unorm:
            case MTL::PixelFormatBGRA8Unorm_sRGB:
                return {4, 1, 1};
            case MTL::PixelFormatRGBA16Float:
                return {8, 1, 1};
            case MTL::PixelFormatRGBA32Float:
                return {16, 1, 1};
            case MTL::PixelFormatR32Float:
                return {4, 1, 1};
            
            // Compressed formats (BC/DXT)
            case MTL::PixelFormatBC1_RGBA:
            case MTL::PixelFormatBC1_RGBA_sRGB:
            case MTL::PixelFormatBC4_RUnorm:
            case MTL::PixelFormatBC4_RSnorm:
                return {8, 4, 4};
            case MTL::PixelFormatBC2_RGBA:
            case MTL::PixelFormatBC2_RGBA_sRGB:
            case MTL::PixelFormatBC3_RGBA:
            case MTL::PixelFormatBC3_RGBA_sRGB:
            case MTL::PixelFormatBC5_RGUnorm:
            case MTL::PixelFormatBC5_RGSnorm:
            case MTL::PixelFormatBC6H_RGBFloat:
            case MTL::PixelFormatBC6H_RGBUfloat:
            case MTL::PixelFormatBC7_RGBAUnorm:
            case MTL::PixelFormatBC7_RGBAUnorm_sRGB:
                return {16, 4, 4};
            
            // ETC2 / EAC
            case MTL::PixelFormatEAC_R11Unorm:
            case MTL::PixelFormatEAC_R11Snorm:
            case MTL::PixelFormatETC2_RGB8:
            case MTL::PixelFormatETC2_RGB8_sRGB:
            case MTL::PixelFormatETC2_RGB8A1:
            case MTL::PixelFormatETC2_RGB8A1_sRGB:
                return {8, 4, 4};
            case MTL::PixelFormatEAC_RG11Unorm:
            case MTL::PixelFormatEAC_RG11Snorm:
            case MTL::PixelFormatEAC_RGBA8:
            case MTL::PixelFormatEAC_RGBA8_sRGB:
                return {16, 4, 4};
            
            // ASTC (4x4)
            case MTL::PixelFormatASTC_4x4_sRGB:
            case MTL::PixelFormatASTC_4x4_LDR:
                return {16, 4, 4};
            
            // Add more as needed
            default:
                return {0, 0, 0};
        }
    }

    void MetalCommandBuffer::CopyBufferToTexture(ResourceHandle srcBuffer, ResourceHandle dstTexture, const BufferTextureCopyRegion* regions, u32 regionCount) {
        MTL::BlitCommandEncoder* encoder = getBlitEncoder();
        if (!encoder) return;

        MetalDevice& metalDevice = static_cast<MetalDevice&>(device_);
        MetalBuffer* buffer = metalDevice.GetBuffer(srcBuffer);
        MetalTexture* texture = metalDevice.GetTexture(dstTexture);

        if (!buffer || !texture) return;

        PixelFormatInfo info = GetPixelFormatInfo(texture->GetNativeTexture()->pixelFormat());
        if (info.bytesPerBlock == 0) {
            std::cerr << "[MetalCommandBuffer] SUPER UNIQUE ERROR: Unsupported pixel format for copy: " 
                      << (u64)texture->GetNativeTexture()->pixelFormat() << std::endl;
            return;
        }

        for (u32 i = 0; i < regionCount; ++i) {
            const auto& region = regions[i];
            
            MTL::Origin origin(region.imageOffset.x, region.imageOffset.y, region.imageOffset.z);
            MTL::Size size(region.imageExtent.width, region.imageExtent.height, region.imageExtent.depth);

            // Calculate bytesPerRow and bytesPerImage using block-based logic
            NS::UInteger rowLength = region.bufferRowLength ? region.bufferRowLength : region.imageExtent.width;
            NS::UInteger imageHeight = region.bufferImageHeight ? region.bufferImageHeight : region.imageExtent.height;

            NS::UInteger rowLengthInBlocks = (rowLength + info.blockWidth - 1) / info.blockWidth;
            NS::UInteger bytesPerRow = rowLengthInBlocks * info.bytesPerBlock;
            
            NS::UInteger imageHeightInBlocks = (imageHeight + info.blockHeight - 1) / info.blockHeight;
            NS::UInteger bytesPerImage = imageHeightInBlocks * bytesPerRow;

            for (u32 layer = 0; layer < region.imageSubresource.layerCount; ++layer) {
                encoder->copyFromBuffer(
                    buffer->GetNativeBuffer(),
                    region.bufferOffset + layer * bytesPerImage,
                    bytesPerRow,
                    bytesPerImage,
                    size,
                    texture->GetNativeTexture(),
                    region.imageSubresource.baseArrayLayer + layer,
                    region.imageSubresource.mipLevel,
                    origin
                );
            }
        }
    }

    void MetalCommandBuffer::CopyTextureToBuffer(ResourceHandle srcTexture, ResourceHandle dstBuffer, const BufferTextureCopyRegion* regions, u32 regionCount) {
        MTL::BlitCommandEncoder* encoder = getBlitEncoder();
        if (!encoder) return;

        MetalDevice& metalDevice = static_cast<MetalDevice&>(device_);
        MetalTexture* texture = metalDevice.GetTexture(srcTexture);
        MetalBuffer* buffer = metalDevice.GetBuffer(dstBuffer);

        if (!texture || !buffer) return;

        PixelFormatInfo info = GetPixelFormatInfo(texture->GetNativeTexture()->pixelFormat());
        if (info.bytesPerBlock == 0) {
            std::cerr << "[MetalCommandBuffer] Unsupported pixel format for copy: " 
                      << (u64)texture->GetNativeTexture()->pixelFormat() << std::endl;
            return;
        }

        for (u32 i = 0; i < regionCount; ++i) {
            const auto& region = regions[i];
            
            MTL::Origin origin(region.imageOffset.x, region.imageOffset.y, region.imageOffset.z);
            MTL::Size size(region.imageExtent.width, region.imageExtent.height, region.imageExtent.depth);

            // Calculate bytesPerRow and bytesPerImage using block-based logic
            NS::UInteger rowLength = region.bufferRowLength ? region.bufferRowLength : region.imageExtent.width;
            NS::UInteger imageHeight = region.bufferImageHeight ? region.bufferImageHeight : region.imageExtent.height;

            NS::UInteger rowLengthInBlocks = (rowLength + info.blockWidth - 1) / info.blockWidth;
            NS::UInteger bytesPerRow = rowLengthInBlocks * info.bytesPerBlock;
            
            NS::UInteger imageHeightInBlocks = (imageHeight + info.blockHeight - 1) / info.blockHeight;
            NS::UInteger bytesPerImage = imageHeightInBlocks * bytesPerRow;

            for (u32 layer = 0; layer < region.imageSubresource.layerCount; ++layer) {
                encoder->copyFromTexture(
                    texture->GetNativeTexture(),
                    region.imageSubresource.baseArrayLayer + layer,
                    region.imageSubresource.mipLevel,
                    origin,
                    size,
                    buffer->GetNativeBuffer(),
                    region.bufferOffset + layer * bytesPerImage,
                    bytesPerRow,
                    bytesPerImage
                );
            }
        }
    }

void MetalCommandBuffer::BlitTexture(ResourceHandle src, ResourceHandle dst,
                                     const TextureBlitRegion* regions, u32 regionCount,
                                     FilterMode filter) {
    // Metal's BlitCommandEncoder has limitations - use render pass for proper blitting
    // For now, use a simple copy approach which should work better

    MetalDevice& metalDevice = static_cast<MetalDevice&>(device_);
    MetalTexture* srcTex = metalDevice.GetTexture(src);
    MetalTexture* dstTex = metalDevice.GetTexture(dst);

    if (!srcTex || !dstTex || !srcTex->GetNativeTexture() || !dstTex->GetNativeTexture()) {
        std::cerr << "[MetalCommandBuffer] BlitTexture: Invalid textures" << std::endl;
        return;
    }

    // For Metal, we need to use a render pass approach for proper blitting
    // But for now, let's try using the blit encoder with proper format checking
    MTL::BlitCommandEncoder* encoder = getBlitEncoder();
    if (!encoder) return;

    for (u32 i = 0; i < regionCount; ++i) {
        const auto& region = regions[i];

        MTL::Origin srcOrigin(region.srcOffsets[0].x, region.srcOffsets[0].y, region.srcOffsets[0].z);
        MTL::Size srcSize(
            region.srcOffsets[1].x - region.srcOffsets[0].x,
            region.srcOffsets[1].y - region.srcOffsets[0].y,
            region.srcOffsets[1].z - region.srcOffsets[0].z
        );

        MTL::Origin dstOrigin(region.dstOffsets[0].x, region.dstOffsets[0].y, region.dstOffsets[0].z);

        // Metal's blit encoder can copy between formats as long as the block size matches.
        // Depth32Float (32-bit) <-> R32Float (32-bit) is a common cross-format copy needed
        // for sampling depth in shaders (Metal TBDR cannot sample D32 directly).
        auto srcFmt = srcTex->GetNativeTexture()->pixelFormat();
        auto dstFmt = dstTex->GetNativeTexture()->pixelFormat();

        // Metal docs: "Depth and stencil pixel formats are not compatible with color pixel formats."
        // D32_Float -> R32_Float requires a compute shader (e.g. ShadowBlit.metal), NOT the blit encoder.
        if (srcFmt == dstFmt) {
            encoder->copyFromTexture(
                srcTex->GetNativeTexture(),
                region.srcSubresource.baseArrayLayer,
                region.srcSubresource.mipLevel,
                srcOrigin,
                srcSize,
                dstTex->GetNativeTexture(),
                region.dstSubresource.baseArrayLayer,
                region.dstSubresource.mipLevel,
                dstOrigin
            );
        } else {
            // CRITICAL: Metal blit encoder does NOT support depth<->color format copies
            // even when byte sizes match (D32_Float <-> R32_Float).
            // Use ExecuteGBufferDepthBlit() compute shader instead.
            std::cerr << "[MetalCommandBuffer] BlitTexture: Incompatible formats src="
                      << (int)srcFmt << " dst=" << (int)dstFmt
                      << " — Metal does not support depth<->color copies via blit encoder. "
                      << "Use compute shader (ShadowBlit.metal) instead." << std::endl;
        }
    }

}

void MetalCommandBuffer::GenerateMipmaps(ResourceHandle texture) {
    MTL::BlitCommandEncoder* encoder = getBlitEncoder();
    if (!encoder) return;
    
    MetalDevice& metalDevice = static_cast<MetalDevice&>(device_);
    MetalTexture* tex = metalDevice.GetTexture(texture);
    
    if (tex && tex->GetNativeTexture()) {
        encoder->generateMipmaps(tex->GetNativeTexture());
    }
}

} // namespace primal::graphics::rhi
