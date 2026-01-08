#pragma once
#include "CommonHeaders.h"
#include "RHIResource.h"

namespace primal::graphics::rhi {

class RHIDescriptorSetLayout : public RHIResource {
public:
    RHIDescriptorSetLayout(RHIDeviceBase& device, const DescriptorSetLayoutDesc& desc)
        : RHIResource(device, ResourceDesc(ResourceType::DescriptorSetLayout, ResourceUsage::None, GPUMemoryUsage::Unknown, 0, nullptr)),
          bindings_(desc.bindings, desc.bindings + desc.bindingCount) {
    }
    
    virtual ~RHIDescriptorSetLayout() = default;
    
    const utl::vector<DescriptorSetLayoutBinding>& GetBindings() const { return bindings_; }

protected:
    utl::vector<DescriptorSetLayoutBinding> bindings_;
};

}
