// Engine/Graphics/WFC/WFCStepBuffer.cpp
//
// Task 11 (Phase A.1): Producer/consumer queue for solver steps.
#include "WFCStepBuffer.h"

namespace primal::graphics::wfc {

void WFCStepBuffer::Push(const WFCStep& step) {
    std::unique_lock<std::shared_mutex> lock(mtx_);
    queue_.push_back(step);
}

u32 WFCStepBuffer::Consume(WFCStep* out, u32 max_count) {
    std::unique_lock<std::shared_mutex> lock(mtx_);
    u32 consumed = 0;
    while (consumed < max_count && !queue_.empty()) {
        out[consumed] = queue_.front();
        queue_.pop_front();
        ++consumed;
    }
    return consumed;
}

bool WFCStepBuffer::Empty() const {
    std::shared_lock<std::shared_mutex> lock(mtx_);
    return queue_.empty();
}

} // namespace primal::graphics::wfc
