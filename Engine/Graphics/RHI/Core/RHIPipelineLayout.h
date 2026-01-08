#pragma once
#include "CommonHeaders.h"
#include "RHIResource.h"

namespace primal::graphics::rhi {

class RHIPipelineLayout : public RHIResource {
public:
    RHIPipelineLayout(RHIDeviceBase& device, const PipelineLayoutDesc& desc) 
        : RHIResource(device, ResourceDesc(ResourceType::PipelineLayout, ResourceUsage::None, GPUMemoryUsage::Unknown, 0, nullptr)),
          setLayouts_(desc.setLayouts, desc.setLayouts + desc.setLayoutCount),
          pushConstantRanges_(desc.pushConstantRanges, desc.pushConstantRanges + desc.pushConstantRangeCount) {
    }
    virtual ~RHIPipelineLayout() = default;
    
    const utl::vector<DescriptorSetLayoutHandle>& GetSetLayouts() const { return setLayouts_; }
    const utl::vector<PushConstantRange>& GetPushConstantRanges() const { return pushConstantRanges_; }

protected:
    utl::vector<DescriptorSetLayoutHandle> setLayouts_;
    utl::vector<PushConstantRange> pushConstantRanges_;
};

}
