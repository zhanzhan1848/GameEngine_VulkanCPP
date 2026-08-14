#pragma once

#include "DawnCommon.h"

#if defined(ENABLE_WEBGPU) && ENABLE_WEBGPU

#include "../../Core/RHITypes.h"
#include <atomic>
#include <mutex>
#include <condition_variable>

namespace primal::graphics::rhi {

class DawnDevice;

class DawnSync {
    friend class DawnDevice;
public:
    ~DawnSync() = default;
    u64 GetValue() const { return signalValue_.load(); }
    void Signal(u64 value);
    bool Wait(u64 value, u32 timeoutMs);

public:
    explicit DawnSync(DawnDevice& device);

    DawnDevice& device_;
    std::atomic<u64> signalValue_{0};
    std::mutex mutex_;
    std::condition_variable cv_;
};

} // namespace primal::graphics::rhi

#endif // ENABLE_WEBGPU
