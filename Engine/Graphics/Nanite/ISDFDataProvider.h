#pragma once

#include "CommonHeaders.h"
#include "../RHI/Core/RHIDevice.h"

namespace primal::graphics::nanite {

struct SDFCascade;

class ISDFDataProvider {
public:
    virtual ~ISDFDataProvider() = default;

    virtual bool Initialize(rhi::RHIDeviceBase* device) = 0;
    virtual bool IsReady() const = 0;

    virtual void DispatchCascade(rhi::RHICommandBuffer* cmd,
                                  u32 frame_index,
                                  const SDFCascade& cascade) = 0;
};

} // namespace primal::graphics::nanite
