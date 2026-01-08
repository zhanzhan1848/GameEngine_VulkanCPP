#pragma once
#include "CommonHeaders.h"
#include "RHIResource.h"
#include "RHIDescriptorSetLayout.h"

namespace primal::graphics::rhi {

class RHIDescriptorSet : public RHIResource {
public:
    RHIDescriptorSet(RHIDeviceBase& device, const DescriptorSetDesc& desc)
        : RHIResource(device, ResourceDesc(ResourceType::DescriptorSet, ResourceUsage::None, GPUMemoryUsage::Unknown, 0, nullptr)), 
          layoutHandle_(desc.layout) {}
    virtual ~RHIDescriptorSet() = default;
    
    DescriptorSetLayoutHandle GetLayout() const { return layoutHandle_; }

protected:
    DescriptorSetLayoutHandle layoutHandle_;
};

}
