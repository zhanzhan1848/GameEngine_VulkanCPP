#include "MetalDescriptorSet.h"
#include "MetalDevice.h"
#include "MetalDescriptorSetLayout.h"

namespace primal::graphics::rhi {

MetalDescriptorSet::MetalDescriptorSet(MetalDevice& device, const DescriptorSetDesc& desc)
    : RHIDescriptorSet(device, desc) {
}

MetalDescriptorSet::~MetalDescriptorSet() {
    Destroy();
}

bool MetalDescriptorSet::Initialize() {
    // Resize bindings based on layout
    MetalDevice& metalDevice = static_cast<MetalDevice&>(device_);
    MetalDescriptorSetLayout* layout = metalDevice.GetDescriptorSetLayout(layoutHandle_);
    if (!layout) return false;
    
    // We need to find the maximum binding index to resize the vector
    // Or we can just iterate the layout bindings and add them?
    // The layout has a list of bindings.
    const auto& layoutBindings = layout->GetBindings();
    u32 maxBinding = 0;
    for (const auto& b : layoutBindings) {
        if (b.binding > maxBinding) {
            maxBinding = b.binding;
        }
    }
    
    // Resize to fit all bindings
    // We use maxBinding + 1 size, but we only populate the ones that exist
    // However, bindings_ is a vector of MetalDescriptorBinding.
    // We should probably just store the active bindings.
    // But Update uses index.
    // Let's populate bindings_ with all bindings from layout.
    
    bindings_.resize(layoutBindings.size());
    for (size_t i = 0; i < layoutBindings.size(); ++i) {
        const auto& lb = layoutBindings[i];
        bindings_[i].binding = lb.binding;
        bindings_[i].type = lb.descriptorType;
        bindings_[i].count = lb.descriptorCount;
        bindings_[i].stageFlags = lb.stageFlags;
        
        // Resize resources vectors
        bindings_[i].resources.resize(lb.descriptorCount, handles::INVALID_RESOURCE);
        bindings_[i].samplers.resize(lb.descriptorCount, handles::INVALID_SAMPLER);
        bindings_[i].bufferOffsets.resize(lb.descriptorCount, 0);
    }
    
    return true;
}

void MetalDescriptorSet::Destroy() {
    bindings_.clear();
}

void MetalDescriptorSet::Update(const WriteDescriptorSet* writes, u32 writeCount) {
    for (u32 i = 0; i < writeCount; ++i) {
        const WriteDescriptorSet& write = writes[i];
        
        // Find the binding in our vector
        // Since we didn't index by binding number directly (to save space if bindings are sparse),
        // we search for it.
        MetalDescriptorBinding* targetBinding = nullptr;
        for (auto& b : bindings_) {
            if (b.binding == write.dstBinding) {
                targetBinding = &b;
                break;
            }
        }
        
        if (!targetBinding) {
            // Binding not found in layout?
            continue;
        }
        
        // Update resources
        u32 count = write.descriptorCount;
        u32 dstArrayElement = write.dstArrayElement;
        
        if (dstArrayElement + count > targetBinding->resources.size()) {
             // Out of bounds
             count = (u32)targetBinding->resources.size() - dstArrayElement;
        }
        
        for (u32 j = 0; j < count; ++j) {
            u32 idx = dstArrayElement + j;
            
            if (write.descriptorType == DescriptorType::UniformBuffer ||
                write.descriptorType == DescriptorType::StorageBuffer ||
                write.descriptorType == DescriptorType::UniformBufferDynamic ||
                write.descriptorType == DescriptorType::StorageBufferDynamic) {
                
                if (write.bufferInfo) {
                    targetBinding->resources[idx] = write.bufferInfo[j].buffer;
                    targetBinding->bufferOffsets[idx] = write.bufferInfo[j].offset;
                    // Range is not stored in MetalDescriptorBinding currently
                }
            } else if (write.descriptorType == DescriptorType::SampledImage ||
                       write.descriptorType == DescriptorType::StorageImage ||
                       write.descriptorType == DescriptorType::CombinedImageSampler) {
                
                if (write.imageInfo) {
                    targetBinding->resources[idx] = write.imageInfo[j].imageView; // Use view as resource
                    targetBinding->samplers[idx] = write.imageInfo[j].sampler;
                }
            } else if (write.descriptorType == DescriptorType::Sampler) {
                if (write.imageInfo) {
                    targetBinding->samplers[idx] = write.imageInfo[j].sampler;
                }
            }
        }
    }
}

bool MetalDescriptorSet::updateDataImpl(const void* data, u64 size, u64 offset) {
    // Descriptor sets are updated via Update() method, not generic updateDataImpl
    return false;
}

void* MetalDescriptorSet::mapImpl(u64 offset, u64 size) {
    return nullptr;
}

void MetalDescriptorSet::unmapImpl() {
}

}
