/**
 * @file MetalSync.cpp
 * @brief Metal 同步原语实现
 * @author GameEngine VulkanCPP Team
 * @date 2026-01-07
 * @version 0.1.0
 */

#include "MetalSync.h"

namespace primal::graphics::rhi {

MetalSync::MetalSync(MTL::Device* device) {
    if (device) {
        event_ = device->newSharedEvent();
    }
}

MetalSync::~MetalSync() {
    if (event_) {
        event_->release();
        event_ = nullptr;
    }
}

uint64_t MetalSync::GetValue() const {
    if (event_) {
        return event_->signaledValue();
    }
    return 0;
}

void MetalSync::SetValue(uint64_t value) {
    if (event_) {
        event_->setSignaledValue(value);
    }
}

} // namespace primal::graphics::rhi
