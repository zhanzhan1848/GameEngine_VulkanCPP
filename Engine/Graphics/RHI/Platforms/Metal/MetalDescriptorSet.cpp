#include "MetalDescriptorSet.h"
#include "MetalDevice.h"

namespace primal::graphics::rhi {

// === MetalDescriptorSetLayout ===

MetalDescriptorSetLayout::MetalDescriptorSetLayout(MetalDevice& device, const DescriptorSetLayoutDesc& desc)
    : RHIResource(device, ResourceDesc(ResourceType::Unknown, ResourceUsage::None, GPUMemoryUsage::Unknown, 0)),
      device_(device), desc_(desc) {
    // Deep copy bindings
    if (desc.bindingCount > 0 && desc.bindings) {
        bindings_.assign(desc.bindings, desc.bindings + desc.bindingCount);
        // Update pointer in stored desc to point to local vector data
        desc_.bindings = bindings_.data();
    } else {
        desc_.bindings = nullptr;
    }
}

MetalDescriptorSetLayout::~MetalDescriptorSetLayout() {
}

// === MetalDescriptorSet ===

MetalDescriptorSet::MetalDescriptorSet(MetalDevice& device, const DescriptorSetDesc& desc)
    : RHIResource(device, ResourceDesc(ResourceType::Unknown, ResourceUsage::None, GPUMemoryUsage::Unknown, 0)),
      device_(device), desc_(desc) {
}

MetalDescriptorSet::~MetalDescriptorSet() {
}

void MetalDescriptorSet::Update(uint32_t writeCount, const WriteDescriptorSet* writes) {
    for (uint32_t i = 0; i < writeCount; ++i) {
        const WriteDescriptorSet& write = writes[i];
        
        MetalDescriptorBinding bindingInfo;
        bindingInfo.type = write.descriptorType;
        
        // Handle different descriptor types
        if (write.descriptorType == DescriptorType::UniformBuffer ||
            write.descriptorType == DescriptorType::StorageBuffer ||
            write.descriptorType == DescriptorType::UniformBufferDynamic ||
            write.descriptorType == DescriptorType::StorageBufferDynamic) {
            
            if (write.bufferInfo) {
                bindingInfo.resource = write.bufferInfo->buffer;
                bindingInfo.offset = write.bufferInfo->offset;
                bindingInfo.range = write.bufferInfo->range;
            }
        }
        else if (write.descriptorType == DescriptorType::SampledImage ||
                 write.descriptorType == DescriptorType::StorageImage ||
                 write.descriptorType == DescriptorType::CombinedImageSampler ||
                 write.descriptorType == DescriptorType::InputAttachment) {
            
            if (write.imageInfo) {
                bindingInfo.resource = write.imageInfo->imageView;
                bindingInfo.sampler = write.imageInfo->sampler;
                bindingInfo.imageLayout = write.imageInfo->imageLayout;
            }
        }
        else if (write.descriptorType == DescriptorType::Sampler) {
            if (write.imageInfo) {
                bindingInfo.sampler = write.imageInfo->sampler;
            }
        }
        
        // Update binding map
        // Note: Currently handling single descriptor per binding. 
        // For arrays, we would need to handle array elements.
        bindings_[write.dstBinding] = bindingInfo;
    }
}

} // namespace primal::graphics::rhi
