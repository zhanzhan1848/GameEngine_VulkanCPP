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
#include "Engine/Common/CoreLogStream.h"
#include "VulkanDDGIAuditPrefix.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

#include <iostream>
#include <cstdio>
#include <vector>
#include <fstream>
#include <cstdlib>
#include <cstring>

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
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanDescriptorSet] Invalid layout handle" << std::endl;
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
    // R10 taskC（UB 加固）：原 reserve(writeCount*4) 是启发式——一旦写入的
    // descriptorCount 总和超过该值（数组型绑定），后续 push_back 触发向量
    // 重分配，先前 vw.pBufferInfo/pImageInfo 存进的指针全部悬垂，
    // vkUpdateDescriptorSets 读已释放内存（UAF）。精确预计算总数后 reserve，
    // 任何 count 组合都不可能再重分配。当前所有调用方 count=1，容量只会
    // 等于或小于旧值，写入行为零变化（Mac 参照零影响）。
    {
        u64 totalBuf = 0, totalImg = 0;
        for (u32 i = 0; i < writeCount; ++i) {
            if (writes[i].dstSet != handle_) continue;
            const u32 n = std::max<u32>(1u, writes[i].descriptorCount);
            switch (writes[i].descriptorType) {
                case DescriptorType::UniformBuffer:
                case DescriptorType::StorageBuffer:
                case DescriptorType::UniformBufferDynamic:
                case DescriptorType::StorageBufferDynamic:
                    if (writes[i].bufferInfo) totalBuf += n;
                    break;
                case DescriptorType::Sampler:
                case DescriptorType::CombinedImageSampler:
                case DescriptorType::SampledImage:
                case DescriptorType::InputAttachment:
                case DescriptorType::SampledDepthImage:
                case DescriptorType::StorageImage:
                    if (writes[i].imageInfo) totalImg += n;
                    break;
                default: break;
            }
        }
        bufInfos.reserve(totalBuf);
        imgInfos.reserve(totalImg);
    }

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
                    // P0-B 定位（PRIMAL_RG_LAYOUT_TRACE=1）: sampled 绑定追踪
                    if (tex && std::getenv("PRIMAL_RG_LAYOUT_TRACE")) {
                        fprintf(stderr, "[DSSAMPLED] set=%p binding=%u img=0x%llx name='%s'\n",
                                (void*)set_, w.dstBinding,
                                (unsigned long long)(uintptr_t)tex->GetNativeImage(),
                                tex->GetTextureDesc().name.c_str());
                    }
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
                    // P0-B 定位（PRIMAL_RG_LAYOUT_TRACE=1）: storage image 绑定
                    // 追踪——与 [RGTRACE] 的 img= 句柄同键，用于把"无 barrier 的
                    // storage 写"归属到具体 set/binding。
                    if (tex && std::getenv("PRIMAL_RG_LAYOUT_TRACE")) {
                        fprintf(stderr, "[DSWRITE] set=%p binding=%u img=0x%llx\n",
                                (void*)set_, w.dstBinding,
                                (unsigned long long)(uintptr_t)tex->GetNativeImage());
                    }
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
        // Explicit rho diagnostic only: record the exact native writes supplied
        // to Vulkan, for joining to actual compute descriptor-set bindings.
        if (const char* prefix = ddgiRecordedComputeAuditPrefix(); prefix && *prefix && std::getenv("DDGI_DUMP") &&
            std::strcmp(std::getenv("DDGI_DUMP"), "2") == 0) {
            std::ofstream trace(std::string(prefix) + ".native-descriptor-writes.jsonl", std::ios::app);
            for (const auto& w : vkWrites) {
                const WriteDescriptorSet* original = nullptr;
                for (u32 i = 0; i < writeCount; ++i) {
                    if (writes[i].dstSet == handle_ && writes[i].dstBinding == w.dstBinding &&
                        writes[i].dstArrayElement == w.dstArrayElement) {
                        original = &writes[i];
                        break;
                    }
                }
                for (u32 j = 0; j < w.descriptorCount; ++j) {
                    trace << "{\"set_handle\":" << handle_
                          << ",\"native_set\":" << (u64)(uintptr_t)w.dstSet
                          << ",\"binding\":" << w.dstBinding << ",\"array_element\":" << w.dstArrayElement+j
                          << ",\"type\":" << w.descriptorType;
                    if (original && original->bufferInfo)
                        trace << ",\"resource_handle\":" << original->bufferInfo[j].buffer;
                    if (original && original->imageInfo)
                        trace << ",\"resource_handle\":" << original->imageInfo[j].imageView
                              << ",\"sampler_handle\":" << original->imageInfo[j].sampler;
                    if (w.pBufferInfo) {
                        const auto& b = w.pBufferInfo[j];
                        trace << ",\"native_buffer\":" << (u64)(uintptr_t)b.buffer
                              << ",\"offset\":" << b.offset << ",\"range\":" << b.range;
                    }
                    if (w.pImageInfo) {
                        const auto& im = w.pImageInfo[j];
                        trace << ",\"native_view\":" << (u64)(uintptr_t)im.imageView
                              << ",\"native_sampler\":" << (u64)(uintptr_t)im.sampler
                              << ",\"image_layout\":" << im.imageLayout;
                    }
                    trace << "}\n";
                }
            }
            trace.close();
            if (!trace) PRIMAL_LOG_STREAM(Error,"RHI") << "[FirstRhoNativeAudit] native descriptor write evidence failed" << std::endl;
        }
    }
}

} // namespace primal::graphics::rhi

#endif // ENABLE_VULKAN
