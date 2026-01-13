#include "Graphics/MaterialInstance.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHIResource.h"
#include <cstring>
#include <fstream>
#include <iostream>

namespace primal::graphics {

MaterialInstance::MaterialInstance(Material* material)
    : material_(material) {
    uniformBuffers_.resize(MAX_FRAMES_IN_FLIGHT, rhi::handles::INVALID_RESOURCE);
    uniformBuffersMapped_.resize(MAX_FRAMES_IN_FLIGHT, nullptr);
    descriptorSets_.resize(MAX_FRAMES_IN_FLIGHT, rhi::handles::INVALID_RESOURCE);
}

MaterialInstance::MaterialInstance(MaterialInstance&& other) noexcept
    : material_(other.material_),
      descriptorSets_(std::move(other.descriptorSets_)),
      uniformBuffers_(std::move(other.uniformBuffers_)),
      uniformBuffersMapped_(std::move(other.uniformBuffersMapped_)),
      currentFrameIndex_(other.currentFrameIndex_),
      uniformDirty_(other.uniformDirty_),
      pendingTextures_(std::move(other.pendingTextures_)),
      pendingSamplers_(std::move(other.pendingSamplers_)),
      device_(other.device_) {
    
    other.descriptorSets_.clear();
    other.uniformBuffers_.clear();
    other.uniformBuffersMapped_.clear();
    other.currentFrameIndex_ = 0;
    other.uniformDirty_ = false;
    other.device_ = nullptr;
}

MaterialInstance& MaterialInstance::operator=(MaterialInstance&& other) noexcept {
    if (this != &other) {
        // Destroy current resources
        if (device_) {
            for (auto buf : uniformBuffers_) {
                if (buf != rhi::handles::INVALID_RESOURCE) {
                    device_->UnmapBuffer(buf);
                    device_->DestroyBuffer(buf);
                }
            }
            for (auto ds : descriptorSets_) {
                if (ds != rhi::handles::INVALID_RESOURCE) {
                    device_->DestroyDescriptorSet(ds);
                }
            }
        }
        
        // Move resources
        material_ = other.material_;
        descriptorSets_ = std::move(other.descriptorSets_);
        uniformBuffers_ = std::move(other.uniformBuffers_);
        uniformBuffersMapped_ = std::move(other.uniformBuffersMapped_);
        currentFrameIndex_ = other.currentFrameIndex_;
        uniformDirty_ = other.uniformDirty_;
        pendingTextures_ = std::move(other.pendingTextures_);
        pendingSamplers_ = std::move(other.pendingSamplers_);
        device_ = other.device_;
        
        // Invalidate other
        other.descriptorSets_.clear();
        other.uniformBuffers_.clear();
        other.uniformBuffersMapped_.clear();
        other.currentFrameIndex_ = 0;
        other.uniformDirty_ = false;
        other.device_ = nullptr;
    }
    return *this;
}

MaterialInstance::~MaterialInstance() {
    if (device_) {
        for (auto buf : uniformBuffers_) {
            if (buf != rhi::handles::INVALID_RESOURCE) {
                device_->UnmapBuffer(buf);
                device_->DestroyBuffer(buf);
            }
        }
        for (auto ds : descriptorSets_) {
            if (ds != rhi::handles::INVALID_RESOURCE) {
                device_->DestroyDescriptorSet(ds);
            }
        }
    }
}

bool MaterialInstance::Initialize(rhi::RHIDeviceBase* device) {
    if (!device || !material_) return false;
    device_ = device;

    // 1. Create Uniform Buffers if needed
    u32 blockSize = material_->GetUniformBlockSize();
    if (blockSize > 0) {
        rhi::BufferDesc bufferDesc;
        bufferDesc.size = blockSize;
        bufferDesc.type = rhi::BufferType::Constant;
        bufferDesc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
        bufferDesc.bindFlags = static_cast<uint32_t>(rhi::ResourceUsage::ConstantBuffer);
        
        for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
            uniformBuffers_[i] = device->CreateBuffer(bufferDesc);
            if (uniformBuffers_[i] == rhi::handles::INVALID_RESOURCE) {
                return false;
            }
            // Map buffer immediately for updates
            uniformBuffersMapped_[i] = device->MapBuffer(uniformBuffers_[i], 0, blockSize);
        }
    }

    // 2. Allocate Descriptor Sets
    rhi::DescriptorSetLayoutHandle layout = material_->GetDescriptorSetLayout();
    if (layout != rhi::handles::INVALID_RESOURCE) {
        rhi::DescriptorSetDesc setDesc;
        setDesc.layout = layout;
        
        for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
            descriptorSets_[i] = device->CreateDescriptorSet(setDesc);
            if (descriptorSets_[i] == rhi::handles::INVALID_RESOURCE) {
                return false;
            }

            // 3. Bind Corresponding Uniform Buffer to Descriptor Set
            if (blockSize > 0 && i < uniformBuffers_.size() && uniformBuffers_[i] != rhi::handles::INVALID_RESOURCE) {
                rhi::WriteDescriptorSet write;
                write.dstSet = descriptorSets_[i];
                write.dstBinding = material_->GetUniformBufferBinding();
                write.dstArrayElement = 0;
                write.descriptorCount = 1;
                write.descriptorType = rhi::DescriptorType::UniformBuffer;
                
                rhi::DescriptorBufferInfo bufferInfo;
                bufferInfo.buffer = uniformBuffers_[i];
                bufferInfo.offset = 0;
                bufferInfo.range = blockSize;
                
                write.bufferInfo = &bufferInfo;
                
                device->UpdateDescriptorSets(1, &write);
            }
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
    if (uniformBuffers_.empty() || currentFrameIndex_ >= uniformBuffers_.size() || !data || size == 0) {
        return;
    }
    
    rhi::ResourceHandle currentBufferHandle = uniformBuffers_[currentFrameIndex_];
    if (currentBufferHandle == rhi::handles::INVALID_RESOURCE) return;

    // Use mapped memory if available (faster)
    if (uniformBuffersMapped_[currentFrameIndex_]) {
        memcpy(static_cast<uint8_t*>(uniformBuffersMapped_[currentFrameIndex_]) + offset, data, size);
        uniformDirty_ = true;
    } else {
        // Fallback to RHI update
        rhi::RHIResource* buffer = rhi::ResourceManager::Instance().GetResource(currentBufferHandle);
        if (buffer) {
            buffer->UpdateData(data, size, offset);
            uniformDirty_ = true;
        }
    }
}

void MaterialInstance::SetCurrentFrame(u32 frameIndex) {
    if (frameIndex < MAX_FRAMES_IN_FLIGHT) {
        currentFrameIndex_ = frameIndex;
    }
}

void MaterialInstance::Update(rhi::RHIDeviceBase* device) {
    // Only update textures/samplers if needed. 
    // Uniform buffers are bound statically in Initialize, so we don't need to re-bind them here.
    
    bool needsUpdate = !pendingTextures_.empty() || !pendingSamplers_.empty();
    
    rhi::DescriptorSetHandle currentSet = GetDescriptorSet();
    if (!needsUpdate || currentSet == rhi::handles::INVALID_RESOURCE) return;

    std::vector<rhi::WriteDescriptorSet> writes;
    writes.reserve(pendingTextures_.size() + pendingSamplers_.size());
    
    std::vector<rhi::DescriptorImageInfo> imageInfos;
    imageInfos.reserve(pendingTextures_.size() + pendingSamplers_.size());

    for (const auto& tex : pendingTextures_) {
        rhi::DescriptorImageInfo info;
        info.imageView = tex.texture;
        info.imageLayout = rhi::ResourceState::Ready;
        info.sampler = rhi::handles::INVALID_SAMPLER;
        
        imageInfos.push_back(info);
        
        rhi::WriteDescriptorSet write;
        write.dstSet = currentSet;
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
        write.dstSet = currentSet;
        write.dstBinding = samp.binding;
        write.dstArrayElement = 0;
        write.descriptorCount = 1;
        write.descriptorType = rhi::DescriptorType::Sampler;
        write.imageInfo = &imageInfos.back(); 
        
        writes.push_back(write);
    }

    if (!writes.empty()) {
        device->UpdateDescriptorSets(static_cast<u32>(writes.size()), writes.data());
    }

    // Note: We clear pending updates. This means updates only apply to the CURRENT frame's descriptor set.
    // In a full engine, we might want to propagate to all sets or track state per set.
    pendingTextures_.clear();
    pendingSamplers_.clear();
    
    // We do NOT auto-increment currentFrameIndex_ anymore. 
    // It should be set via SetCurrentFrame() by the renderer.
}

} // namespace primal::graphics
