#include "DawnDescriptorSet.h"
#include "DawnDevice.h"
#include "DawnDescriptorSetLayout.h"
#include "DawnBuffer.h"
#include "DawnTexture.h"
#include "DawnSampler.h"

#if defined(ENABLE_WEBGPU) && ENABLE_WEBGPU

#include <iostream>
#include <cstring>

namespace primal::graphics::rhi {

DawnDescriptorSet::DawnDescriptorSet(DawnDevice& device)
    : device_(device) {}

DawnDescriptorSet::~DawnDescriptorSet() {
    Destroy();
}

bool DawnDescriptorSet::Initialize(const DescriptorSetDesc& desc) {
    if (desc.layout == handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
        std::cerr << "[DawnDescriptorSet] Invalid layout handle" << std::endl;
        return false;
    }

    layoutHandle_ = desc.layout;

    // Pre-allocate pending bindings from the layout's bindings
    DawnDescriptorSetLayout* layout = device_.GetDescriptorSetLayout(desc.layout);
    if (!layout) {
        std::cerr << "[DawnDescriptorSet] Failed to resolve layout handle" << std::endl;
        return false;
    }

    const auto& layoutBindings = layout->GetBindings();
    for (const auto& lb : layoutBindings) {
        DawnPendingBinding pb{};
        pb.type = lb.descriptorType;
        pb.engineBinding = lb.binding;
        pb.populated = false;
        pendingBindings_.emplace_back(pb);

        // For CombinedImageSampler, add a second pending slot for the sampler
        if (lb.descriptorType == DescriptorType::CombinedImageSampler) {
            DawnPendingBinding sampPB{};
            sampPB.type = DescriptorType::Sampler;
            sampPB.engineBinding = lb.binding;
            sampPB.populated = false;
            pendingBindings_.emplace_back(sampPB);
        }
    }

    // Don't create the WGPUBindGroup yet — it will be built lazily on first use.
    needsRebuild_ = true;
    return true;
}

void DawnDescriptorSet::Destroy() {
    if (wgpuGroup_) {
        wgpuBindGroupRelease(wgpuGroup_);
        wgpuGroup_ = nullptr;
    }
    pendingBindings_.clear();
    needsRebuild_ = true;
}

DawnPendingBinding* DawnDescriptorSet::GetOrCreatePending(u32 engineBinding, DescriptorType type) {
    // First try to find an exact match
    for (auto& pb : pendingBindings_) {
        if (pb.engineBinding == engineBinding && pb.type == type) {
            return &pb;
        }
    }
    // If looking for a buffer type and no exact match, try any buffer type
    if (type == DescriptorType::UniformBuffer || type == DescriptorType::UniformBufferDynamic ||
        type == DescriptorType::StorageBuffer || type == DescriptorType::StorageBufferDynamic) {
        for (auto& pb : pendingBindings_) {
            if (pb.engineBinding == engineBinding &&
                (pb.type == DescriptorType::UniformBuffer || pb.type == DescriptorType::UniformBufferDynamic ||
                 pb.type == DescriptorType::StorageBuffer || pb.type == DescriptorType::StorageBufferDynamic)) {
                return &pb;
            }
        }
    }
    // For texture/sampler types, try compatible types at same binding
    if (type == DescriptorType::SampledImage || type == DescriptorType::StorageImage ||
        type == DescriptorType::InputAttachment || type == DescriptorType::SampledDepthImage ||
        type == DescriptorType::CombinedImageSampler) {
        for (auto& pb : pendingBindings_) {
            if (pb.engineBinding == engineBinding &&
                (pb.type == DescriptorType::SampledImage || pb.type == DescriptorType::StorageImage ||
                 pb.type == DescriptorType::InputAttachment || pb.type == DescriptorType::SampledDepthImage ||
                 pb.type == DescriptorType::CombinedImageSampler)) {
                return &pb;
            }
        }
    }
    // Not found — do NOT grow. The layout's bindings were pre-populated during
    // Initialize(), so a miss means the write type is incompatible with the layout.
    // Returning nullptr is safe: the caller checks for nullptr and skips the write.
    return nullptr;
}

WGPUBindGroup DawnDescriptorSet::GetBindGroup() {
    if (needsRebuild_) {
        if (!BuildBindGroup()) {
            return nullptr;
        }
    }
    return wgpuGroup_;
}

bool DawnDescriptorSet::BuildBindGroup() {
    DawnDescriptorSetLayout* layout = device_.GetDescriptorSetLayout(layoutHandle_);
    if (!layout || !layout->GetNativeLayout()) {
        std::cerr << "[DawnDS] Cannot build: invalid layout" << std::endl;
        return false;
    }

    // Count populated entries
    u32 populatedCount = 0;
    for (const auto& pb : pendingBindings_) {
        if (pb.populated) ++populatedCount;
    }

    if (populatedCount == 0) {
        std::cerr << "[DawnDS] No populated bindings — nothing to build (pending count=" << pendingBindings_.size() << ")" << std::endl;
        return false;
    }

    // Build WGPUBindGroupEntry array
    utl::vector<WGPUBindGroupEntry> entries;
    entries.reserve(populatedCount);

    for (auto& pb : pendingBindings_) {
        if (!pb.populated) continue;

        DawnDescriptorSetLayout* layout2 = device_.GetDescriptorSetLayout(layoutHandle_);
        u32 wgpuBinding = layout2->GetWGPUBinding(pb.engineBinding, pb.type);

        switch (pb.type) {
        case DescriptorType::UniformBufferDynamic:
        case DescriptorType::UniformBuffer:
        case DescriptorType::StorageBuffer:
        case DescriptorType::StorageBufferDynamic:
        case DescriptorType::UniformTexelBuffer:
        case DescriptorType::StorageTexelBuffer: {
            if (!pb.buffer) continue;
            auto& entry = entries.emplace_back();
            std::memset(&entry, 0, sizeof(entry));
            entry.nextInChain = nullptr;
            entry.binding = wgpuBinding;
            entry.buffer = pb.buffer;
            entry.offset = pb.offset;
            entry.size = pb.size;
            break;
        }
        case DescriptorType::SampledDepthImage:
        case DescriptorType::SampledImage:
        case DescriptorType::InputAttachment: {
            if (!pb.textureView) {
                pb.textureView = device_.GetDummyTextureView();
                if (!pb.textureView) continue;
            }
            auto& entry = entries.emplace_back();
            std::memset(&entry, 0, sizeof(entry));
            entry.nextInChain = nullptr;
            entry.binding = wgpuBinding;
            entry.textureView = pb.textureView;
            break;
        }
        case DescriptorType::StorageImage: {
            if (!pb.textureView) {
                pb.textureView = device_.GetDummyTextureView();
                if (!pb.textureView) continue;
            }
            auto& entry = entries.emplace_back();
            std::memset(&entry, 0, sizeof(entry));
            entry.nextInChain = nullptr;
            entry.binding = wgpuBinding;
            entry.textureView = pb.textureView;
            break;
        }
        case DescriptorType::Sampler: {
            if (!pb.sampler) continue;
            // Skip samplers that belong to a CombinedImageSampler — handled in second pass
            bool belongsToCombined = false;
            for (const auto& other : pendingBindings_) {
                if (other.engineBinding == pb.engineBinding &&
                    other.type == DescriptorType::CombinedImageSampler &&
                    other.populated) {
                    belongsToCombined = true;
                    break;
                }
            }
            if (belongsToCombined) continue;
            auto& entry = entries.emplace_back();
            std::memset(&entry, 0, sizeof(entry));
            entry.nextInChain = nullptr;
            entry.binding = wgpuBinding;
            entry.sampler = pb.sampler;
            break;
        }
        case DescriptorType::CombinedImageSampler: {
            // Texture part
            if (pb.textureView) {
                auto& entry = entries.emplace_back();
                std::memset(&entry, 0, sizeof(entry));
                entry.nextInChain = nullptr;
                entry.binding = wgpuBinding;
                entry.textureView = pb.textureView;
            }
            // Sampler part is handled by the separate Sampler pending binding
            break;
        }
        default:
            break;
        }
    }

    // Also handle sampler parts of CombinedImageSampler
    for (const auto& pb : pendingBindings_) {
        if (!pb.populated || pb.type != DescriptorType::Sampler) continue;
        // Check if this sampler belongs to a CombinedImageSampler
        bool isCombinedSampler = false;
        for (const auto& other : pendingBindings_) {
            if (other.engineBinding == pb.engineBinding &&
                other.type == DescriptorType::CombinedImageSampler) {
                isCombinedSampler = true;
                break;
            }
        }
        if (isCombinedSampler && pb.sampler) {
            u32 samplerBinding = layout->GetCombinedSamplerWGPUBinding(pb.engineBinding);
            auto& entry = entries.emplace_back();
            std::memset(&entry, 0, sizeof(entry));
            entry.nextInChain = nullptr;
            entry.binding = samplerBinding;
            entry.sampler = pb.sampler;
        }
    }

    // Release previous bind group
    if (wgpuGroup_) {
        wgpuBindGroupRelease(wgpuGroup_);
        wgpuGroup_ = nullptr;
    }

    if (entries.empty()) {
        return false;
    }

    WGPUBindGroupDescriptor groupDesc{};
    groupDesc.nextInChain = nullptr;
    groupDesc.label = ToWGPUStringView("Dawn Descriptor Set");
    groupDesc.layout = layout->GetNativeLayout();
    groupDesc.entryCount = static_cast<u32>(entries.size());
    groupDesc.entries = entries.data();

    wgpuGroup_ = wgpuDeviceCreateBindGroup(device_.GetNativeDevice(), &groupDesc);
    if (!wgpuGroup_) {
        std::cerr << "[DawnDescriptorSet] Failed to create WGPUBindGroup" << std::endl;
        return false;
    }

    needsRebuild_ = false;
    return true;
}

} // namespace primal::graphics::rhi

#endif // ENABLE_WEBGPU
