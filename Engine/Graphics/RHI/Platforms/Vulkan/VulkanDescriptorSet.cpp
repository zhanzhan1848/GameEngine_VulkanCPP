/**
 * @file VulkanDescriptorSet.cpp
 * @brief VulkanDescriptorSet 实现
 * @author GameEngine VulkanCPP Team
 * @date 2026-07-26
 */

#include "VulkanDescriptorSet.h"
#include "VulkanDevice.h"
#include "VulkanDescriptorSetLayout.h"
#include "VulkanBuffer.h"
#include "VulkanTexture.h"
#include "VulkanSampler.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

#include <iostream>
#include <vector>

namespace primal::graphics::rhi {

namespace {

VkDescriptorType ToVkDescriptorType(DescriptorType t) {
    switch (t) {
        case DescriptorType::Sampler:               return VK_DESCRIPTOR_TYPE_SAMPLER;
        case DescriptorType::CombinedImageSampler:  return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        case DescriptorType::SampledImage:          return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        case DescriptorType::StorageImage:          return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        case DescriptorType::UniformTexelBuffer:    return VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
        case DescriptorType::StorageTexelBuffer:    return VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
        case DescriptorType::UniformBuffer:         return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        case DescriptorType::StorageBuffer:         return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        case DescriptorType::UniformBufferDynamic:  return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        case DescriptorType::StorageBufferDynamic:  return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
        case DescriptorType::InputAttachment:       return VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
        case DescriptorType::SampledDepthImage:     return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        default:                                    return VK_DESCRIPTOR_TYPE_MAX_ENUM;
    }
}

} // anonymous namespace

VulkanDescriptorSet::VulkanDescriptorSet(VulkanDevice& device, const DescriptorSetDesc& desc)
    : RHIDescriptorSet(device, desc) {}

bool VulkanDescriptorSet::Initialize() {
    VulkanDevice& vk = static_cast<VulkanDevice&>(device_);
    VulkanDescriptorSetLayout* layout = vk.GetDescriptorSetLayout(layoutHandle_);
    if (!layout) {
        std::cerr << "[VulkanDescriptorSet] Invalid layout handle" << std::endl;
        return false;
    }
    set_ = layout->AllocateSet();
    if (set_ == VK_NULL_HANDLE) return false;
    state_ = ResourceState::Ready;
    return true;
}

void VulkanDescriptorSet::destroyImpl() {
    if (set_ == VK_NULL_HANDLE) return;
    // VkDescriptorSet must outlive any command buffer that references it.
    // HZBSystem + similar per-call creators destroy the set before the command
    // buffer is submitted — validation flags this as UAF. Defer vkFreeDescriptorSets
    // to GC purge so the set stays alive until the device is satisfied the GPU
    // work has completed (or Shutdown flushes the queue).
    VkDevice dev = static_cast<VulkanDevice&>(device_).GetNativeDevice();
    VulkanDescriptorSetLayout* layout = static_cast<VulkanDevice&>(device_).GetDescriptorSetLayout(layoutHandle_);
    VkDescriptorSet set = set_;
    VkDescriptorPool pool = layout ? layout->GetNativePool() : VK_NULL_HANDLE;
    if (dev != VK_NULL_HANDLE && pool != VK_NULL_HANDLE) {
        device_.GetGarbageCollector().DeferredDestroy([dev, pool, set]() {
            vkFreeDescriptorSets(dev, pool, 1, &set);
        });
    }
    set_ = VK_NULL_HANDLE;
}

void VulkanDescriptorSet::Update(const WriteDescriptorSet* writes, u32 writeCount) {
    if (!writes || writeCount == 0 || set_ == VK_NULL_HANDLE) return;
    VkDevice dev = static_cast<VulkanDevice&>(device_).GetNativeDevice();
    VulkanDevice& vk = static_cast<VulkanDevice&>(device_);

    std::vector<VkWriteDescriptorSet> vkWrites;
    std::vector<VkDescriptorBufferInfo> bufInfos;
    std::vector<VkDescriptorImageInfo>  imgInfos;
    bufInfos.reserve(writeCount * 4);
    imgInfos.reserve(writeCount * 4);

    for (u32 i = 0; i < writeCount; ++i) {
        const WriteDescriptorSet& w = writes[i];
        if (w.dstSet != handle_) continue;  // 只处理指向自己的 write

        VkWriteDescriptorSet vw{};
        vw.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        vw.dstSet = set_;
        vw.dstBinding = w.dstBinding;
        vw.dstArrayElement = w.dstArrayElement;
        vw.descriptorCount = std::max<u32>(1u, w.descriptorCount);
        vw.descriptorType = ToVkDescriptorType(w.descriptorType);

        switch (w.descriptorType) {
            case DescriptorType::UniformBuffer:
            case DescriptorType::StorageBuffer:
            case DescriptorType::UniformBufferDynamic:
            case DescriptorType::StorageBufferDynamic: {
                if (!w.bufferInfo) break;
                for (u32 j = 0; j < vw.descriptorCount; ++j) {
                    VkDescriptorBufferInfo bi{};
                    VulkanBuffer* buf = vk.GetBuffer(w.bufferInfo[j].buffer);
                    bi.buffer = buf ? buf->GetNativeBuffer() : VK_NULL_HANDLE;
                    bi.offset = w.bufferInfo[j].offset;
                    bi.range  = (w.bufferInfo[j].range == 0) ? VK_WHOLE_SIZE : w.bufferInfo[j].range;
                    bufInfos.push_back(bi);
                }
                vw.pBufferInfo = bufInfos.data() + (bufInfos.size() - vw.descriptorCount);
                break;
            }
            case DescriptorType::Sampler:
            case DescriptorType::CombinedImageSampler:
            case DescriptorType::SampledImage:
            case DescriptorType::InputAttachment:
            case DescriptorType::SampledDepthImage: {
                if (!w.imageInfo) break;
                for (u32 j = 0; j < vw.descriptorCount; ++j) {
                    VkDescriptorImageInfo ii{};
                    VulkanTexture* tex = vk.GetTexture(w.imageInfo[j].imageView);
                    ii.imageView = tex ? tex->GetNativeView() : VK_NULL_HANDLE;
                    VulkanSampler* samp = vk.GetSampler(w.imageInfo[j].sampler);
                    ii.sampler = samp ? samp->GetNativeSampler() : VK_NULL_HANDLE;
                    ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    imgInfos.push_back(ii);
                }
                vw.pImageInfo = imgInfos.data() + (imgInfos.size() - vw.descriptorCount);
                break;
            }
            case DescriptorType::StorageImage: {
                if (!w.imageInfo) break;
                for (u32 j = 0; j < vw.descriptorCount; ++j) {
                    VkDescriptorImageInfo ii{};
                    VulkanTexture* tex = vk.GetTexture(w.imageInfo[j].imageView);
                    ii.imageView = tex ? tex->GetNativeView() : VK_NULL_HANDLE;
                    VulkanSampler* samp = vk.GetSampler(w.imageInfo[j].sampler);
                    ii.sampler = samp ? samp->GetNativeSampler() : VK_NULL_HANDLE;
                    // StorageImage requires GENERAL layout (read+write access).
                    // The hardcoded SHADER_READ_ONLY_OPTIMAL triggers
                    // VUID-VkWriteDescriptorSet-descriptorType-04152 on strict drivers.
                    ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                    imgInfos.push_back(ii);
                }
                vw.pImageInfo = imgInfos.data() + (imgInfos.size() - vw.descriptorCount);
                break;
            }
            default:
                // Texel buffer 等 Phase 5+ 再补
                break;
        }
        vkWrites.push_back(vw);
    }

    if (!vkWrites.empty()) {
        vkUpdateDescriptorSets(dev, static_cast<u32>(vkWrites.size()), vkWrites.data(), 0, nullptr);
    }
}

} // namespace primal::graphics::rhi

#endif // ENABLE_VULKAN
