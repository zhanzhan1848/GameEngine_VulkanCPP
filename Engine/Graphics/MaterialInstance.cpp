#include "Graphics/MaterialInstance.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHIResource.h"
#include <cstring>

namespace primal::graphics {

MaterialInstance::MaterialInstance(Material* material)
    : material_(material) {
}

MaterialInstance::~MaterialInstance() {
    if (device_) {
        if (uniformBuffer_ != rhi::handles::INVALID_RESOURCE) {
            if (uniformBufferMapped_) {
                device_->UnmapBuffer(uniformBuffer_);
            }
            device_->DestroyBuffer(uniformBuffer_);
        }
        if (descriptorSet_ != rhi::handles::INVALID_RESOURCE) {
            device_->DestroyDescriptorSet(descriptorSet_);
        }
    }
}

bool MaterialInstance::Initialize(rhi::RHIDeviceBase* device) {
    if (!device || !material_) return false;
    device_ = device;

    // 1. Create Uniform Buffer if needed
    u32 blockSize = material_->GetUniformBlockSize();
    if (blockSize > 0) {
        rhi::BufferDesc bufferDesc;
        bufferDesc.size = blockSize;
        bufferDesc.type = rhi::BufferType::Constant;
        bufferDesc.usage = rhi::GPUMemoryUsage::Dynamic; // CPU write, GPU read
        bufferDesc.bindFlags = static_cast<uint32_t>(rhi::ResourceUsage::ConstantBuffer);
        
        uniformBuffer_ = device->CreateBuffer(bufferDesc);
        if (uniformBuffer_ == rhi::handles::INVALID_RESOURCE) {
            return false;
        }

        uniformBufferMapped_ = device->MapBuffer(uniformBuffer_, 0, blockSize);
    }

    // 2. Allocate Descriptor Set
    rhi::DescriptorSetLayoutHandle layout = material_->GetDescriptorSetLayout();
    if (layout != rhi::handles::INVALID_RESOURCE) {
        rhi::DescriptorSetDesc setDesc;
        setDesc.layout = layout;
        descriptorSet_ = device->CreateDescriptorSet(setDesc);
        
        if (descriptorSet_ == rhi::handles::INVALID_RESOURCE) {
            return false;
        }

        // 3. Bind Uniform Buffer to Descriptor Set
        if (blockSize > 0 && uniformBuffer_ != rhi::handles::INVALID_RESOURCE) {
            rhi::WriteDescriptorSet write;
            write.dstSet = descriptorSet_;
            write.dstBinding = material_->GetUniformBufferBinding();
            write.dstArrayElement = 0;
            write.descriptorCount = 1;
            write.descriptorType = rhi::DescriptorType::UniformBuffer;
            
            rhi::DescriptorBufferInfo bufferInfo;
            bufferInfo.buffer = uniformBuffer_;
            bufferInfo.offset = 0;
            bufferInfo.range = blockSize;
            
            write.bufferInfo = &bufferInfo;
            
            device->UpdateDescriptorSets(1, &write);
        }
    }

    return true;
}

void MaterialInstance::SetTexture(u32 binding, rhi::ResourceHandle texture) {
    TextureUpdate update;
    update.binding = binding;
    update.texture = texture;
    pendingTextures_.push_back(update);
}

void MaterialInstance::SetSampler(u32 binding, rhi::SamplerHandle sampler) {
    SamplerUpdate update;
    update.binding = binding;
    update.sampler = sampler;
    pendingSamplers_.push_back(update);
}

void MaterialInstance::SetUniformData(u32 offset, const void* data, u32 size) {
    if (!uniformBufferMapped_ || !data || size == 0) return;
    
    u32 blockSize = material_->GetUniformBlockSize();
    if (offset + size > blockSize) {
        // Handle error: out of bounds
        return;
    }

    memcpy(static_cast<u8*>(uniformBufferMapped_) + offset, data, size);
}

void MaterialInstance::Update(rhi::RHIDeviceBase* device) {
    if (pendingTextures_.empty() && pendingSamplers_.empty()) return;
    if (descriptorSet_ == rhi::handles::INVALID_RESOURCE) return;

    std::vector<rhi::WriteDescriptorSet> writes;
    writes.reserve(pendingTextures_.size() + pendingSamplers_.size());
    
    // We need to keep infos alive until UpdateDescriptorSets call
    std::vector<rhi::DescriptorImageInfo> imageInfos;
    imageInfos.reserve(pendingTextures_.size() + pendingSamplers_.size());

    for (const auto& tex : pendingTextures_) {
        rhi::DescriptorImageInfo info;
        info.imageView = tex.texture;
        info.imageLayout = rhi::ResourceState::Ready; // Use Ready as a reasonable default
        info.sampler = rhi::handles::INVALID_SAMPLER;
        
        imageInfos.push_back(info);
        
        rhi::WriteDescriptorSet write;
        write.dstSet = descriptorSet_;
        write.dstBinding = tex.binding;
        write.dstArrayElement = 0;
        write.descriptorCount = 1;
        write.descriptorType = rhi::DescriptorType::SampledImage;
        write.imageInfo = &imageInfos.back(); 
        
        writes.push_back(write);
    }

    for (const auto& samp : pendingSamplers_) {
        rhi::DescriptorImageInfo info;
        info.imageView = rhi::handles::INVALID_RESOURCE;
        info.imageLayout = rhi::ResourceState::Unknown;
        info.sampler = samp.sampler;
        
        imageInfos.push_back(info);
        
        rhi::WriteDescriptorSet write;
        write.dstSet = descriptorSet_;
        write.dstBinding = samp.binding;
        write.dstArrayElement = 0;
        write.descriptorCount = 1;
        write.descriptorType = rhi::DescriptorType::Sampler;
        write.imageInfo = &imageInfos.back(); 
        
        writes.push_back(write);
    }

    device->UpdateDescriptorSets(static_cast<u32>(writes.size()), writes.data());

    pendingTextures_.clear();
    pendingSamplers_.clear();
}

} // namespace primal::graphics
