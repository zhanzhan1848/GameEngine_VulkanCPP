/**
 * @file MetalSync.cpp
 * @brief Metal 同步原语实现
 * @author GameEngine VulkanCPP Team
 * @date 2026-01-07
 * @version 0.1.0
 */

#include "MetalSync.h"

#include <dispatch/dispatch.h>
#include "Engine/Common/CoreLogStream.h"

namespace primal::graphics::rhi {

MetalSync::MetalSync(MTL::Device* device) {
    if (device) {
        event_ = device->newSharedEvent();
    }
}

MetalSync::~MetalSync() {
    if (fenceSubmission_) fenceSubmission_->release();
    if (event_) {
        event_->release();
        event_ = nullptr;
    }
}

void MetalSync::TrackFenceSubmission(MTL::CommandBuffer* buffer) {
    if (buffer) buffer->retain();
    if (fenceSubmission_) fenceSubmission_->release();
    fenceSubmission_ = buffer;
}

u64 MetalSync::GetValue() const {
    return GetSignaledValue();
}

void MetalSync::SetValue(u64 value) {
    Signal(value);
}

u64 MetalSync::GetSignaledValue() const {
    if (event_) {
        return event_->signaledValue();
    }
    return 0;
}

void MetalSync::Signal(u64 value) {
    if (event_) {
        event_->setSignaledValue(value);
    }
}

bool MetalSync::Wait(u64 value, u32 timeoutMs) {
    if (!event_) return false;

    // Fast path: already signaled at or above the target value.
    if (event_->signaledValue() >= value) return true;

    dispatch_semaphore_t sema = dispatch_semaphore_create(0);
    dispatch_retain(sema); // Retain for the block (block may fire synchronously)

    MTL::SharedEventListener* listener = MTL::SharedEventListener::alloc()->init();
    if (!listener) {
        dispatch_release(sema);
        dispatch_release(sema);
        return false;
    }

    event_->notifyListener(listener, value,
        ^(MTL::SharedEvent*, u64) {
            dispatch_semaphore_signal(sema);
            dispatch_release(sema);
        });

    long result;
    if (timeoutMs == 0xFFFFFFFFu) {
        result = dispatch_semaphore_wait(sema, DISPATCH_TIME_FOREVER);
    } else {
        result = dispatch_semaphore_wait(sema,
            dispatch_time(DISPATCH_TIME_NOW, (int64_t)timeoutMs * NSEC_PER_MSEC));
    }

    listener->release();

    dispatch_release(sema); // release the caller's ownership on every wait

    if (result != 0 && fenceSubmission_ && reportedTimeoutTarget_ != value) {
        reportedTimeoutTarget_ = value;
        const auto status = fenceSubmission_->status();
        const char* name = "unknown";
        switch (status) {
            case MTL::CommandBufferStatusNotEnqueued: name = "not-enqueued"; break;
            case MTL::CommandBufferStatusEnqueued: name = "enqueued"; break;
            case MTL::CommandBufferStatusCommitted: name = "committed"; break;
            case MTL::CommandBufferStatusScheduled: name = "scheduled"; break;
            case MTL::CommandBufferStatusCompleted: name = "completed"; break;
            case MTL::CommandBufferStatusError: name = "error"; break;
        }
        auto* error = fenceSubmission_->error();
        PRIMAL_LOG_STREAM(Error, "RHI") << "[MetalFenceTimeout] target=" << value
            << " signaled=" << event_->signaledValue() << " commandBuffer=" << name
            << " error=" << (error ? error->localizedDescription()->utf8String() : "none")
            << std::endl;
    }

    // If we timed out, the block is still pending and will fire later — but the
    // semaphore was retained by both dispatch_semaphore_create (our handle) and
    // the block. We released our handle via dispatch_release above; the block
    // retains its own reference and will release on fire. Safe to return.
    return result == 0;
}

MTL::SharedEvent* MetalSync::DetachNativeEvent() {
    MTL::SharedEvent* temp = event_;
    event_ = nullptr;
    return temp;
}

} // namespace primal::graphics::rhi
