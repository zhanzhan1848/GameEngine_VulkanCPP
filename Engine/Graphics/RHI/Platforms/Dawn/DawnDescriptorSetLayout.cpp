#include "DawnDescriptorSetLayout.h"
#include "DawnDevice.h"
#include "DawnCommon.h"

#if defined(ENABLE_WEBGPU) && ENABLE_WEBGPU

#include <iostream>
#include <cstring>
#include <algorithm>

namespace primal::graphics::rhi {

namespace {

WGPUShaderStage ToWGPUShaderStageFlags(ShaderStage stage) {
    WGPUShaderStage flags = WGPUShaderStage_None;
    if ((static_cast<u8>(stage) & static_cast<u8>(ShaderStage::Vertex)) != 0) {
        flags |= WGPUShaderStage_Vertex;
    }
    if ((static_cast<u8>(stage) & static_cast<u8>(ShaderStage::Pixel)) != 0) {
        flags |= WGPUShaderStage_Fragment;
    }
    if ((static_cast<u8>(stage) & static_cast<u8>(ShaderStage::Compute)) != 0) {
        flags |= WGPUShaderStage_Compute;
    }
    return flags;
}

bool IsBufferType(DescriptorType t) {
    return t == DescriptorType::UniformBuffer || t == DescriptorType::StorageBuffer ||
           t == DescriptorType::UniformBufferDynamic || t == DescriptorType::StorageBufferDynamic ||
           t == DescriptorType::UniformTexelBuffer || t == DescriptorType::StorageTexelBuffer;
}

bool IsTextureType(DescriptorType t) {
    return t == DescriptorType::SampledImage || t == DescriptorType::StorageImage ||
           t == DescriptorType::InputAttachment || t == DescriptorType::CombinedImageSampler ||
           t == DescriptorType::SampledDepthImage;
}

} // anonymous namespace

DawnDescriptorSetLayout::DawnDescriptorSetLayout(DawnDevice& device)
    : device_(device) {}

DawnDescriptorSetLayout::~DawnDescriptorSetLayout() {
    Destroy();
}

u32 DawnDescriptorSetLayout::GetWGPUBinding(u32 engineBinding, DescriptorType type) const {
    for (const auto& m : bindingMappings_) {
        if (m.engineBinding == engineBinding && m.type == type) {
            return m.wgpuBinding;
        }
    }
    // Fallback: no remapping found, use engine binding directly
    return engineBinding;
}

u32 DawnDescriptorSetLayout::GetCombinedSamplerWGPUBinding(u32 engineBinding) const {
    for (const auto& m : combinedSamplerMappings_) {
        if (m.engineBinding == engineBinding) {
            return m.samplerBinding;
        }
    }
    return engineBinding;
}

bool DawnDescriptorSetLayout::Initialize(const DescriptorSetLayoutDesc& desc) {
    if (!desc.bindings || desc.bindingCount == 0) {
        std::cerr << "[DawnDescriptorSetLayout] No bindings provided" << std::endl;
        return false;
    }

    // Store original bindings
    bindings_.resize(desc.bindingCount);
    for (u32 i = 0; i < desc.bindingCount; ++i) {
        bindings_[i] = desc.bindings[i];
    }

    // Detect Metal dual-namespace collisions:
    // Metal allows texture(0) and buffer(0) to coexist.
    // WebGPU requires unique binding numbers per bind group.
    // If collision detected, remap buffer bindings to higher slots.

    // Collect all (engineBinding, type) pairs and detect collisions on engineBinding
    struct SlotUsage {
        u32 engineBinding;
        bool hasBuffer;
        bool hasTexture;
        bool hasSampler;
    };

    utl::vector<SlotUsage> slots;
    for (u32 i = 0; i < desc.bindingCount; ++i) {
        const auto& b = desc.bindings[i];
        SlotUsage* slot = nullptr;
        for (auto& s : slots) {
            if (s.engineBinding == b.binding) { slot = &s; break; }
        }
        if (!slot) {
            slot = &slots.emplace_back();
            slot->engineBinding = b.binding;
            slot->hasBuffer = false;
            slot->hasTexture = false;
            slot->hasSampler = false;
        }
        if (IsBufferType(b.descriptorType)) slot->hasBuffer = true;
        else if (IsTextureType(b.descriptorType)) slot->hasTexture = true;
        else if (b.descriptorType == DescriptorType::Sampler) slot->hasSampler = true;
    }

    // Find max engine binding to use as offset for remapped bindings
    u32 maxBinding = 0;
    for (u32 i = 0; i < desc.bindingCount; ++i) {
        if (desc.bindings[i].binding > maxBinding) maxBinding = desc.bindings[i].binding;
    }
    u32 remapOffset = maxBinding + 1;

    // Build binding mappings — if a slot has both texture and buffer, remap buffer
    for (u32 i = 0; i < desc.bindingCount; ++i) {
        const auto& b = desc.bindings[i];
        bool needsRemap = false;

        for (const auto& s : slots) {
            if (s.engineBinding == b.binding && s.hasBuffer && s.hasTexture) {
                needsRemap = true;
                break;
            }
        }

        BindingMapping mapping;
        mapping.engineBinding = b.binding;
        mapping.type = b.descriptorType;

        if (needsRemap && IsBufferType(b.descriptorType)) {
            // Find next available remap slot
            u32 remappedBinding = remapOffset;
            bool found = false;
            while (!found) {
                found = true;
                for (const auto& m : bindingMappings_) {
                    if (m.wgpuBinding == remappedBinding) {
                        remappedBinding++;
                        found = false;
                        break;
                    }
                }
                // Also check unremapped bindings
                for (u32 j = 0; j < desc.bindingCount; ++j) {
                    if (desc.bindings[j].binding == remappedBinding && !IsBufferType(desc.bindings[j].descriptorType)) {
                        remappedBinding++;
                        found = false;
                        break;
                    }
                }
            }
            mapping.wgpuBinding = remappedBinding;
        } else {
            mapping.wgpuBinding = b.binding;
        }
        bindingMappings_.emplace_back(mapping);
    }

    // Count total WGPU entries (CombinedImageSampler splits into 2)
    u32 totalEntries = 0;
    for (u32 i = 0; i < desc.bindingCount; ++i) {
        if (desc.bindings[i].descriptorType == DescriptorType::CombinedImageSampler) {
            totalEntries += 2;
        } else {
            totalEntries += 1;
        }
    }

    // Build WGPU layout entries using remapped binding numbers
    utl::vector<WGPUBindGroupLayoutEntry> entries;
    entries.reserve(totalEntries);

    for (u32 i = 0; i < desc.bindingCount; ++i) {
        const auto& binding = desc.bindings[i];
        WGPUShaderStage visibility = ToWGPUShaderStageFlags(binding.stageFlags);
        u32 wgpuBinding = bindingMappings_[i].wgpuBinding;

        switch (binding.descriptorType) {
        case DescriptorType::UniformBuffer: {
            auto& entry = entries.emplace_back();
            std::memset(&entry, 0, sizeof(entry));
            entry.nextInChain = nullptr;
            entry.binding = wgpuBinding;
            entry.visibility = visibility;
            entry.buffer.type = WGPUBufferBindingType_Uniform;
            entry.buffer.hasDynamicOffset = false;
            entry.buffer.minBindingSize = binding.minBindingSize;
            break;
        }
        case DescriptorType::UniformBufferDynamic: {
            auto& entry = entries.emplace_back();
            std::memset(&entry, 0, sizeof(entry));
            entry.nextInChain = nullptr;
            entry.binding = wgpuBinding;
            entry.visibility = visibility;
            entry.buffer.type = WGPUBufferBindingType_Uniform;
            entry.buffer.hasDynamicOffset = true;
            entry.buffer.minBindingSize = binding.minBindingSize;
            dynamicBindingCount_++;
            break;
        }
        case DescriptorType::StorageBuffer: {
            auto& entry = entries.emplace_back();
            std::memset(&entry, 0, sizeof(entry));
            entry.nextInChain = nullptr;
            entry.binding = wgpuBinding;
            entry.visibility = visibility;
            // WebGPU forbids read-write storage buffers in vertex shaders. Vertex
            // pulling / meshlet vertex fetches are read-only by design, so coerce
            // to ReadOnlyStorage when the binding is visible to the vertex stage.
            // Also honor an explicit `readonly` request from non-vertex stages.
            bool vertexVisible = (visibility & WGPUShaderStage_Vertex) != 0;
            entry.buffer.type = (vertexVisible || binding.readonly)
                ? WGPUBufferBindingType_ReadOnlyStorage
                : WGPUBufferBindingType_Storage;
            entry.buffer.hasDynamicOffset = false;
            entry.buffer.minBindingSize = binding.minBindingSize;
            break;
        }
        case DescriptorType::StorageBufferDynamic: {
            auto& entry = entries.emplace_back();
            std::memset(&entry, 0, sizeof(entry));
            entry.nextInChain = nullptr;
            entry.binding = wgpuBinding;
            entry.visibility = visibility;
            entry.buffer.type = WGPUBufferBindingType_Storage;
            entry.buffer.hasDynamicOffset = true;
            entry.buffer.minBindingSize = binding.minBindingSize;
            dynamicBindingCount_++;
            break;
        }
        case DescriptorType::SampledImage: {
            auto& entry = entries.emplace_back();
            std::memset(&entry, 0, sizeof(entry));
            entry.nextInChain = nullptr;
            entry.binding = wgpuBinding;
            entry.visibility = visibility;
            // R32Float and a few other formats are UnfilterableFloat in WebGPU —
            // declaring Float here triggers validation when such a texture is bound.
            // Callers set binding.unfilterableFloat = true to opt in.
            entry.texture.sampleType = binding.unfilterableFloat
                ? WGPUTextureSampleType_UnfilterableFloat
                : WGPUTextureSampleType_Float;
            // N0a: honor isArray for texture_2d_array bindings (meshlet material arrays).
            // is3D takes precedence over both for texture_3d bindings (DDGI SDF cascades).
            // isCube takes precedence over 2D since cube arrays aren't requested here.
            entry.texture.viewDimension = binding.is3D
                ? WGPUTextureViewDimension_3D
                : (binding.isArray
                    ? WGPUTextureViewDimension_2DArray
                    : (binding.isCube ? WGPUTextureViewDimension_Cube : WGPUTextureViewDimension_2D));
            entry.texture.multisampled = false;
            break;
        }
        case DescriptorType::StorageImage: {
            auto& entry = entries.emplace_back();
            std::memset(&entry, 0, sizeof(entry));
            entry.nextInChain = nullptr;
            entry.binding = wgpuBinding;
            entry.visibility = visibility;
            entry.storageTexture.access = binding.readonly
                ? WGPUStorageTextureAccess_ReadOnly
                : WGPUStorageTextureAccess_WriteOnly;
            entry.storageTexture.format = ToWGPUTextureFormat(binding.format);
            entry.storageTexture.viewDimension = binding.is3D
                ? WGPUTextureViewDimension_3D
                : (binding.isArray
                    ? WGPUTextureViewDimension_2DArray : WGPUTextureViewDimension_2D);
            break;
        }
        case DescriptorType::Sampler: {
            auto& entry = entries.emplace_back();
            std::memset(&entry, 0, sizeof(entry));
            entry.nextInChain = nullptr;
            entry.binding = wgpuBinding;
            entry.visibility = visibility;
            entry.sampler.type = binding.isNonFiltering
                ? WGPUSamplerBindingType_NonFiltering
                : WGPUSamplerBindingType_Filtering;
            break;
        }
        case DescriptorType::CombinedImageSampler: {
            // Texture entry at remapped binding
            auto& texEntry = entries.emplace_back();
            std::memset(&texEntry, 0, sizeof(texEntry));
            texEntry.nextInChain = nullptr;
            texEntry.binding = wgpuBinding;
            texEntry.visibility = visibility;
            texEntry.texture.sampleType = WGPUTextureSampleType_Float;
            texEntry.texture.viewDimension = WGPUTextureViewDimension_2D;
            texEntry.texture.multisampled = false;

            // Sampler entry at next available binding after remapOffset
            u32 samplerBinding = remapOffset;
            bool foundSlot = false;
            while (!foundSlot) {
                foundSlot = true;
                for (const auto& e : entries) {
                    if (e.binding == samplerBinding) {
                        samplerBinding++;
                        foundSlot = false;
                        break;
                    }
                }
            }
            auto& sampEntry = entries.emplace_back();
            std::memset(&sampEntry, 0, sizeof(sampEntry));
            sampEntry.nextInChain = nullptr;
            sampEntry.binding = samplerBinding;
            sampEntry.visibility = visibility;
            sampEntry.sampler.type = WGPUSamplerBindingType_Filtering;

            // Record the sampler binding for this CombinedImageSampler
            CombinedSamplerMapping csm;
            csm.engineBinding = binding.binding;
            csm.samplerBinding = samplerBinding;
            combinedSamplerMappings_.emplace_back(csm);
            break;
        }
        case DescriptorType::UniformTexelBuffer:
        case DescriptorType::StorageTexelBuffer: {
            auto& entry = entries.emplace_back();
            std::memset(&entry, 0, sizeof(entry));
            entry.nextInChain = nullptr;
            entry.binding = wgpuBinding;
            entry.visibility = visibility;
            entry.buffer.type = (binding.descriptorType == DescriptorType::UniformTexelBuffer)
                                ? WGPUBufferBindingType_Uniform
                                : WGPUBufferBindingType_Storage;
            entry.buffer.hasDynamicOffset = false;
            entry.buffer.minBindingSize = 0;
            break;
        }
        case DescriptorType::InputAttachment: {
            auto& entry = entries.emplace_back();
            std::memset(&entry, 0, sizeof(entry));
            entry.nextInChain = nullptr;
            entry.binding = wgpuBinding;
            entry.visibility = visibility;
            entry.texture.sampleType = WGPUTextureSampleType_Float;
            entry.texture.viewDimension = WGPUTextureViewDimension_2D;
            entry.texture.multisampled = false;
            break;
        }
        case DescriptorType::SampledDepthImage: {
            auto& entry = entries.emplace_back();
            std::memset(&entry, 0, sizeof(entry));
            entry.nextInChain = nullptr;
            entry.binding = wgpuBinding;
            entry.visibility = visibility;
            entry.texture.sampleType = WGPUTextureSampleType_Depth;
            entry.texture.viewDimension = binding.isArray
                ? WGPUTextureViewDimension_2DArray : WGPUTextureViewDimension_2D;
            entry.texture.multisampled = false;
            break;
        }
        default:
            std::cerr << "[DawnDescriptorSetLayout] Unsupported descriptor type: "
                      << static_cast<u32>(binding.descriptorType) << std::endl;
            break;
        }
    }

    // Create the WGPU bind group layout
    WGPUBindGroupLayoutDescriptor layoutDesc{};
    layoutDesc.nextInChain = nullptr;
    layoutDesc.label = ToWGPUStringView("Dawn Descriptor Set Layout");
    layoutDesc.entryCount = static_cast<u32>(entries.size());
    layoutDesc.entries = entries.data();

    wgpuLayout_ = wgpuDeviceCreateBindGroupLayout(device_.GetNativeDevice(), &layoutDesc);
    if (!wgpuLayout_) {
        std::cerr << "[DawnDescriptorSetLayout] Failed to create WGPUBindGroupLayout" << std::endl;
        return false;
    }

    bindingCount_ = desc.bindingCount;
    return true;
}

void DawnDescriptorSetLayout::Destroy() {
    if (wgpuLayout_) {
        wgpuBindGroupLayoutRelease(wgpuLayout_);
        wgpuLayout_ = nullptr;
    }
    bindingCount_ = 0;
    bindings_.clear();
    bindingMappings_.clear();
}

} // namespace primal::graphics::rhi

#endif // ENABLE_WEBGPU
