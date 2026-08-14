/**
 * @file MetalStagingAllocator.cpp
 */

#include "MetalStagingAllocator.h"
#include <cassert>
#include <cstring>
#include <iostream>

namespace primal::graphics::rhi {

MetalStagingAllocator::~MetalStagingAllocator() {
    Shutdown();
}

void MetalStagingAllocator::Initialize(MTL::Device* device,
                                       MTL::CommandQueue* transferQueue,
                                       u64 poolSize) {
    assert(device != nullptr);
    assert(!initialized_ && "MetalStagingAllocator already initialized");

    device_ = device;
    device_->retain();
    transferQueue_ = transferQueue;  // Not retained — MetalDevice owns it

    for (u32 i = 0; i < FRAME_COUNT; ++i) {
        pools_[i].buffer = device_->newBuffer(poolSize, MTL::ResourceStorageModeShared);
        if (!pools_[i].buffer) {
            // Allocation failure — abort with partial state
            std::cerr << "[MetalStagingAllocator] Failed to allocate pool " << i
                      << " (size=" << poolSize << ")" << std::endl;
            Shutdown();
            return;
        }
        pools_[i].capacity = poolSize;
        pools_[i].offset = 0;
        pools_[i].pending.clear();
    }

    currentFrame_ = 0;
    initialized_ = true;
}

void MetalStagingAllocator::Shutdown() {
    if (!initialized_) return;

    for (u32 i = 0; i < FRAME_COUNT; ++i) {
        if (pools_[i].buffer) {
            pools_[i].buffer->release();
            pools_[i].buffer = nullptr;
        }
        pools_[i].pending.clear();
        pools_[i].offset = 0;
        pools_[i].capacity = 0;
    }

    if (device_) {
        device_->release();
        device_ = nullptr;
    }
    transferQueue_ = nullptr;
    initialized_ = false;
}

void MetalStagingAllocator::BeginFrame() {
    if (!initialized_) return;

    currentFrame_ = (currentFrame_ + 1) % FRAME_COUNT;
    pools_[currentFrame_].offset = 0;
    pools_[currentFrame_].pending.clear();
}

MetalStagingAllocator::Allocation
MetalStagingAllocator::Allocate(u64 size, u64 alignment) {
    Allocation alloc{};

    if (!initialized_) {
        alloc.overflow = true;
        return alloc;
    }

    FramePool& pool = pools_[currentFrame_];

    // Align offset
    u64 alignedOffset = (pool.offset + alignment - 1) & ~(alignment - 1);
    if (alignedOffset + size > pool.capacity) {
        std::cerr << "[MetalStagingAllocator] Frame " << currentFrame_
                  << " pool exhausted: requested=" << size
                  << " aligned=" << alignedOffset
                  << " capacity=" << pool.capacity << std::endl;
        alloc.overflow = true;
        return alloc;
    }

    alloc.stagingBuffer = pool.buffer;
    alloc.offset = alignedOffset;
    alloc.cpuPtr = static_cast<u8*>(pool.buffer->contents()) + alignedOffset;
    alloc.overflow = false;

    pool.offset = alignedOffset + size;
    return alloc;
}

void MetalStagingAllocator::QueueBlit_Buffer(Allocation alloc,
                                              MTL::Buffer* dstBuffer,
                                              u64 dstOffset, u64 size) {
    if (!initialized_ || alloc.overflow || !dstBuffer) return;

    PendingBlit p{};
    p.srcBuffer     = alloc.stagingBuffer;
    p.srcOffset     = alloc.offset;
    p.dstBuffer     = dstBuffer;
    p.dstBufferOffset = dstOffset;
    p.size          = size;
    p.isTexture     = false;

    pools_[currentFrame_].pending.push_back(p);
}

void MetalStagingAllocator::QueueBlit_Texture(Allocation alloc,
                                               MTL::Texture* dstTexture,
                                               u32 mipLevel, u32 slice,
                                               u32 originX, u32 originY, u32 originZ,
                                               u32 width, u32 height, u32 depth,
                                               u32 bytesPerRow, u32 bytesPerImage) {
    if (!initialized_ || alloc.overflow || !dstTexture) return;

    PendingBlit p{};
    p.srcBuffer     = alloc.stagingBuffer;
    p.srcOffset     = alloc.offset;
    p.dstTexture    = dstTexture;
    p.mipLevel      = mipLevel;
    p.slice         = slice;
    p.originX       = originX;
    p.originY       = originY;
    p.originZ       = originZ;
    p.width         = width;
    p.height        = height;
    p.depth         = depth;
    p.bytesPerRow   = bytesPerRow;
    p.bytesPerImage = bytesPerImage;
    p.isTexture     = true;

    pools_[currentFrame_].pending.push_back(p);
}

void MetalStagingAllocator::EncodePendingBlits(MTL::CommandBuffer* cmd) {
    if (!initialized_ || !cmd) return;

    FramePool& pool = pools_[currentFrame_];
    if (pool.pending.empty()) return;

    MTL::BlitCommandEncoder* enc = cmd->blitCommandEncoder();
    if (!enc) {
        std::cerr << "[MetalStagingAllocator] Failed to create blit encoder" << std::endl;
        return;
    }

    for (const auto& p : pool.pending) {
        if (p.isTexture) {
            MTL::Origin origin(p.originX, p.originY, p.originZ);
            MTL::Size  size(p.width, p.height, p.depth);
            // API: copyFromBuffer(srcBuffer, srcOffset, bytesPerRow, bytesPerImage, srcSize,
            //                     dstTexture, dstSlice, dstLevel, dstOrigin)
            enc->copyFromBuffer(p.srcBuffer, p.srcOffset,
                                p.bytesPerRow, p.bytesPerImage,
                                size,
                                p.dstTexture,
                                p.slice, p.mipLevel,
                                origin);
        } else {
            enc->copyFromBuffer(p.srcBuffer, p.srcOffset,
                                p.dstBuffer, p.dstBufferOffset,
                                p.size);
        }
    }

    enc->endEncoding();
    pool.pending.clear();
}

void MetalStagingAllocator::FlushBlocking() {
    if (!initialized_) return;
    if (!transferQueue_) {
        std::cerr << "[MetalStagingAllocator] FlushBlocking: no transfer queue" << std::endl;
        return;
    }

    FramePool& pool = pools_[currentFrame_];
    if (pool.pending.empty()) return;

    MTL::CommandBuffer* cmd = transferQueue_->commandBuffer();
    if (!cmd) {
        std::cerr << "[MetalStagingAllocator] FlushBlocking: commandBuffer() failed" << std::endl;
        return;
    }

    EncodePendingBlits(cmd);
    cmd->commit();
    cmd->waitUntilCompleted();
}

} // namespace primal::graphics::rhi
