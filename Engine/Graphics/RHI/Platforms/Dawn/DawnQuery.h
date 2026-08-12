#pragma once

#include "DawnCommon.h"

#if defined(ENABLE_WEBGPU) && ENABLE_WEBGPU

#include "../../Core/RHIDevice.h"

namespace primal::graphics::rhi {

class DawnDevice;

class DawnQueryPool {
    friend class DawnDevice;
public:
    ~DawnQueryPool();
    WGPUQuerySet GetNativeQuerySet() const { return wgpuQuerySet_; }

public:
    DawnQueryPool(DawnDevice& device);

private:
    bool Initialize(const QueryPoolDesc& desc);
    void Destroy();

    DawnDevice& device_;
    WGPUQuerySet wgpuQuerySet_ = nullptr;
    QueryType type_;
    u32 queryCount_ = 0;
};

} // namespace primal::graphics::rhi

#endif // ENABLE_WEBGPU
