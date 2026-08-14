#include "DawnSync.h"

#if defined(ENABLE_WEBGPU) && ENABLE_WEBGPU

#include "DawnDevice.h"
#include <chrono>
#include <iostream>

namespace primal::graphics::rhi {

DawnSync::DawnSync(DawnDevice& device)
    : device_(device) {}

void DawnSync::Signal(u64 value) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        signalValue_.store(value);
    }
    cv_.notify_all();
}

bool DawnSync::Wait(u64 value, u32 timeoutMs) {
    auto startTime = std::chrono::steady_clock::now();
    auto timeout = std::chrono::milliseconds(timeoutMs);

    while (signalValue_.load() < value) {
        auto elapsed = std::chrono::steady_clock::now() - startTime;
        if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed) >= timeout) {
            return false;
        }
        // Process WebGPU events to advance GPU work
        WGPUInstance instance = device_.GetInstance();
        if (instance) {
            wgpuInstanceProcessEvents(instance);
        }
    }
    return true;
}

} // namespace primal::graphics::rhi

#endif // ENABLE_WEBGPU
