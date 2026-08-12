// Engine/Graphics/WFC/WFCStepBuffer.h
//
// Task 11 (Phase A.1): Producer/consumer queue for solver steps.
//
// The WFC solver will run on a background thread (Phase A.2) and emit a stream
// of WFCStep records (Collapse / Propagate / Restart). The main thread consumes
// them once per frame to drive PCG-side bookkeeping (mesh staging, debug viz,
// snapshot checkpoints).
//
// Threading contract:
//   * Push    — solver thread (single producer in Phase A.2).
//   * Consume — main thread, once per frame.
//   * Empty   — any thread (read-only).
//
// Storage is std::deque<WFCStep> for O(1) push_back / pop_front. We use
// std::shared_mutex so Empty() can take a shared lock while Push/Consume take
// unique locks. The Phase A.1 test suite exercises this single-threaded; the
// concurrency stress test lives in Task 12.
#pragma once

#include "../../Common/CommonHeaders.h"
#include "WFCTypes.h"
#include <deque>
#include <shared_mutex>

namespace primal::graphics::wfc {

class WFCStepBuffer {
public:
    // Producer side (solver background thread).
    void Push(const WFCStep& step);

    // Consumer side (main thread, called once per frame).
    // Copies up to max_count steps into out (FIFO order) and removes them from
    // the queue. Returns the number of steps actually consumed.
    u32  Consume(WFCStep* out, u32 max_count);

    // True if the queue is currently empty. Safe to call from any thread.
    bool Empty() const;

private:
    mutable std::shared_mutex      mtx_;
    std::deque<WFCStep>            queue_;
};

} // namespace primal::graphics::wfc
