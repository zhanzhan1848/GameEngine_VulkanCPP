#include "Graphics/MaterialInstance.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHIResource.h"
#include <cstring>
#include <algorithm>
#include <cmath>
#include "Graphics/RHI/Core/CardSurfaceIdentity.h"
#include <fstream>
#include <iostream>

namespace primal::graphics {

MaterialInstance::MaterialInstance(Material* material)
    : material_(material) {
    uniformBuffers_.resize(rhi::MAX_FRAMES_IN_FLIGHT, rhi::handles::INVALID_RESOURCE);
    uniformBuffersMapped_.resize(rhi::MAX_FRAMES_IN_FLIGHT, nullptr);
    descriptorSets_.resize(rhi::MAX_FRAMES_IN_FLIGHT, rhi::handles::INVALID_RESOURCE);
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
      pendingBuffers_(std::move(other.pendingBuffers_)),
      boundTextures_(std::move(other.boundTextures_)),
      transmission_factor_(other.transmission_factor_),
      thickness_factor_(other.thickness_factor_),
      device_(other.device_) {
    surface_bake_texture_digest_ = other.surface_bake_texture_digest_;
    other.surface_bake_texture_digest_ = 0;

    other.descriptorSets_.clear();
    other.uniformBuffers_.clear();
    other.uniformBuffersMapped_.clear();
    other.currentFrameIndex_ = 0;
    other.uniformDirty_ = false;
    other.transmission_factor_ = 0.0f;
    other.thickness_factor_ = 0.0f;
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
        surface_bake_texture_digest_ = other.surface_bake_texture_digest_;
        other.surface_bake_texture_digest_ = 0;
        material_ = other.material_;
        descriptorSets_ = std::move(other.descriptorSets_);
        uniformBuffers_ = std::move(other.uniformBuffers_);
        uniformBuffersMapped_ = std::move(other.uniformBuffersMapped_);
        currentFrameIndex_ = other.currentFrameIndex_;
        uniformDirty_ = other.uniformDirty_;
        pendingTextures_ = std::move(other.pendingTextures_);
        pendingSamplers_ = std::move(other.pendingSamplers_);
        pendingBuffers_ = std::move(other.pendingBuffers_);
        boundTextures_ = std::move(other.boundTextures_);
        transmission_factor_ = other.transmission_factor_;
        thickness_factor_ = other.thickness_factor_;
        device_ = other.device_;
        
        // Invalidate other
        other.descriptorSets_.clear();
        other.uniformBuffers_.clear();
        other.uniformBuffersMapped_.clear();
        other.currentFrameIndex_ = 0;
        other.uniformDirty_ = false;
        other.transmission_factor_ = 0.0f;
        other.thickness_factor_ = 0.0f;
        other.device_ = nullptr;
    }
    return *this;
}

MaterialInstance::~MaterialInstance() {
    surface_bake_lifetime_.reset();
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
        rhi::BufferDesc bufferDesc{
            blockSize,
            rhi::BufferType::Constant,
            rhi::GPUMemoryUsage::Dynamic,
            rhi::GPUMemoryUsage::Dynamic,
            
        };
        
        for (u32 i = 0; i < rhi::MAX_FRAMES_IN_FLIGHT; ++i) {
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

        for (u32 i = 0; i < rhi::MAX_FRAMES_IN_FLIGHT; ++i) {
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

u64 MaterialInstance::GetSurfaceBakeDigest() const {
    // SCS v2 supports opaque materials. Transport/unknown sources use owner capture.
    if (transmission_factor_ != 0.f || thickness_factor_ != 0.f) return 0;
    math::v3 tint; float metallic, roughness;
    GetMaterialFactors(tint, metallic, roughness);
    const float rgb[3] = {tint.x, tint.y, tint.z};
    for (float v : rgb) if (!std::isfinite(v)) return 0;
    if (!std::isfinite(roughness) || !std::isfinite(metallic)) return 0;
    return rhi::card_surface_identity::Material(surface_bake_texture_digest_, rgb,
        u8(std::clamp(roughness, 0.f, 1.f) * 255.f + .5f),
        u8(std::clamp(metallic, 0.f, 1.f) * 255.f + .5f));
}

void MaterialInstance::SetTexture(u32 binding, rhi::ResourceHandle texture, u32 arrayElement) {
    if (binding == 0) surface_bake_texture_digest_ = 0;
    TextureUpdate update;
    update.binding = binding;
    update.arrayElement = arrayElement;
    update.texture = texture;
    pendingTextures_.push_back(update);

    // Mirror into boundTextures_ so GetTextureHandle returns the most recent
    // value even after Update() clears pendingTextures_. GPU-driven paths
    // (GPUMaterialRegistry / MaterialDataBuilder) query handles long after
    // SetTexture + Update have run.
    if (binding >= boundTextures_.size()) {
        boundTextures_.resize(binding + 1, rhi::handles::INVALID_RESOURCE);
    }
    boundTextures_[binding] = texture;
}

void MaterialInstance::SetSampler(u32 binding, rhi::SamplerHandle sampler, u32 arrayElement) {
    surface_bake_texture_digest_ = 0; // Sampling changes invalidate the baked source certificate.
    SamplerUpdate update;
    update.binding = binding;
    update.arrayElement = arrayElement;
    update.sampler = sampler;
    pendingSamplers_.push_back(update);
}

void MaterialInstance::SetBuffer(u32 binding, rhi::ResourceHandle buffer, u32 size, u32 offset) {
    BufferUpdate update;
    update.binding = binding;
    update.buffer = buffer;
    update.size = size;
    update.offset = offset;
    pendingBuffers_.push_back(update);
}

void MaterialInstance::SetUniformData(u32 offset, const void* data, u32 size) {
    if (data && size) surface_bake_texture_digest_ = 0; // Arbitrary uniforms cannot be reproduced offline.
    if (uniformBuffers_.empty() || currentFrameIndex_ >= uniformBuffers_.size() || !data || size == 0) {
        return;
    }
    
    rhi::ResourceHandle currentBufferHandle = uniformBuffers_[currentFrameIndex_];
    if (currentBufferHandle == rhi::handles::INVALID_RESOURCE) return;

    // Use mapped memory if available (faster)
    if (uniformBuffersMapped_[currentFrameIndex_]) {
        memcpy(static_cast<u8*>(uniformBuffersMapped_[currentFrameIndex_]) + offset, data, size);
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
    if (frameIndex < rhi::MAX_FRAMES_IN_FLIGHT) {
        currentFrameIndex_ = frameIndex;
    }
}

void MaterialInstance::Update(rhi::RHIDeviceBase* device) {
    // Only update textures/samplers if needed. 
    // Uniform buffers are bound statically in Initialize, so we don't need to re-bind them here.
    
    bool needsUpdate = !pendingTextures_.empty() || !pendingSamplers_.empty() || !pendingBuffers_.empty();
    
    rhi::DescriptorSetHandle currentSet = GetDescriptorSet();
    if (!needsUpdate || currentSet == rhi::handles::INVALID_RESOURCE) return;

    utl::vector<rhi::WriteDescriptorSet> writes;
    writes.reserve(pendingTextures_.size() + pendingSamplers_.size() + pendingBuffers_.size());
    
    utl::vector<rhi::DescriptorImageInfo> imageInfos;
    imageInfos.reserve(pendingTextures_.size() + pendingSamplers_.size());

    utl::vector<rhi::DescriptorBufferInfo> bufferInfos;
    bufferInfos.reserve(pendingBuffers_.size());

    for (const auto& tex : pendingTextures_) {
        rhi::DescriptorImageInfo info;
        info.imageView = tex.texture;
        info.imageLayout = rhi::ResourceState::Ready;
        info.sampler = rhi::handles::INVALID_SAMPLER;
        
        imageInfos.push_back(info);
        
        rhi::WriteDescriptorSet write;
        write.dstSet = currentSet;
        write.dstBinding = tex.binding;
        write.dstArrayElement = tex.arrayElement;
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
        write.dstArrayElement = samp.arrayElement;
        write.descriptorCount = 1;
        write.descriptorType = rhi::DescriptorType::Sampler;
        write.imageInfo = &imageInfos.back(); 
        
        writes.push_back(write);
    }

    for (const auto& buf : pendingBuffers_) {
        rhi::DescriptorBufferInfo info;
        info.buffer = buf.buffer;
        info.offset = buf.offset;
        info.range = buf.size;
        
        bufferInfos.push_back(info);
        
        rhi::WriteDescriptorSet write;
        write.dstSet = currentSet;
        write.dstBinding = buf.binding;
        write.dstArrayElement = 0;
        write.descriptorCount = 1;
        write.descriptorType = rhi::DescriptorType::UniformBuffer;
        write.bufferInfo = &bufferInfos.back(); 
        
        writes.push_back(write);
    }

    if (!writes.empty()) {
        device->UpdateDescriptorSets(static_cast<u32>(writes.size()), writes.data());
    }

    // Note: We clear pending updates. This means updates only apply to the CURRENT frame's descriptor set.
    // In a full engine, we might want to propagate to all sets or track state per set.
    pendingTextures_.clear();
    pendingSamplers_.clear();
    pendingBuffers_.clear();
    
    // We do NOT auto-increment currentFrameIndex_ anymore.
    // It should be set via SetCurrentFrame() by the renderer.
}

// ============================================================================
// 🔥 NEW METHODS: GPU Material Registry Support (Task 4)
// ============================================================================

rhi::ResourceHandle MaterialInstance::GetTextureHandle(u32 binding) const {
    // Read committed state (kept current in SetTexture, persists across
    // Update() calls). Fall back to pendingTextures_ for any caller that
    // queries between SetTexture and the next Update — same handle value,
    // just defensive.
    if (binding < boundTextures_.size() && boundTextures_[binding] != rhi::handles::INVALID_RESOURCE) {
        return boundTextures_[binding];
    }
    for (const auto& update : pendingTextures_) {
        if (update.binding == binding) {
            return update.texture;
        }
    }
    return rhi::handles::INVALID_RESOURCE;
}

void MaterialInstance::GetMaterialFactors(
    math::v3& out_albedo_tint,
    float& out_metallic,
    float& out_roughness
) const {
    // Try to get from material template
    if (material_) {
        // Assuming material template has methods to get factors
        // For now, return default values
        // TODO: Implement proper material factor extraction
        out_albedo_tint = math::v3{1.0f, 1.0f, 1.0f};
        out_metallic = 0.0f;
        out_roughness = 0.5f;
    } else {
        // Default values
        out_albedo_tint = math::v3{1.0f, 1.0f, 1.0f};
        out_metallic = 0.0f;
        out_roughness = 0.5f;
    }
}

void MaterialInstance::SetTransmissionFactor(float transmission) {
    // Keep the source value normalized before MaterialDataBuilder quantizes it
    // to the ABI-stable flags word.
    transmission_factor_ = transmission > 1.0f ? 1.0f :
                           (transmission > 0.0f ? transmission : 0.0f);
}

void MaterialInstance::SetThicknessFactor(float thickness) {
    thickness_factor_ = thickness > 1.0f ? 1.0f :
                        (thickness > 0.0f ? thickness : 0.0f);
}

void MaterialInstance::GetBoundTextures(
    rhi::ResourceHandle& out_albedo,
    rhi::ResourceHandle& out_normal,
    rhi::ResourceHandle& out_orm
) const {
    // Standard binding indices: 0=albedo, 1=normal, 2=ORM
    out_albedo = GetTextureHandle(0);
    out_normal = GetTextureHandle(1);
    out_orm = GetTextureHandle(2);
}

// ============================================================================
// END NEW METHODS
// ============================================================================

} // namespace primal::graphics
