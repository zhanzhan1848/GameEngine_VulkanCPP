#include "DawnQuery.h"

#if defined(ENABLE_WEBGPU) && ENABLE_WEBGPU

#include "DawnDevice.h"
#include <iostream>

namespace primal::graphics::rhi {

DawnQueryPool::DawnQueryPool(DawnDevice& device)
    : device_(device) {}

DawnQueryPool::~DawnQueryPool() {
    Destroy();
}

bool DawnQueryPool::Initialize(const QueryPoolDesc& desc) {
    type_ = desc.type;
    queryCount_ = desc.queryCount;

    WGPUQueryType wgpuType = WGPUQueryType_Occlusion;
    switch (desc.type) {
        case QueryType::Timestamp: {
            // Timestamp queries require device feature — check availability
            WGPUBool supported = wgpuDeviceHasFeature(device_.GetNativeDevice(), WGPUFeatureName_TimestampQuery);
            if (!supported) {
                // Not supported — create as occlusion instead, timestamps will be no-ops
                wgpuType = WGPUQueryType_Occlusion;
            } else {
                wgpuType = WGPUQueryType_Timestamp;
            }
            break;
        }
        case QueryType::Occlusion:
            wgpuType = WGPUQueryType_Occlusion;
            break;
        case QueryType::PipelineStatistics:
            wgpuType = WGPUQueryType_Occlusion;
            break;
    }

    WGPUQuerySetDescriptor wgpuDesc{};
    wgpuDesc.nextInChain = nullptr;
    wgpuDesc.label = ToWGPUStringView("DawnQuerySet");
    wgpuDesc.type = wgpuType;
    wgpuDesc.count = desc.queryCount;

    wgpuQuerySet_ = wgpuDeviceCreateQuerySet(device_.GetNativeDevice(), &wgpuDesc);
    if (!wgpuQuerySet_) {
        std::cerr << "[DawnQueryPool] Failed to create query set" << std::endl;
        return false;
    }
    return true;
}

void DawnQueryPool::Destroy() {
    if (wgpuQuerySet_) {
        wgpuQuerySetRelease(wgpuQuerySet_);
        wgpuQuerySet_ = nullptr;
    }
}

} // namespace primal::graphics::rhi

#endif // ENABLE_WEBGPU
