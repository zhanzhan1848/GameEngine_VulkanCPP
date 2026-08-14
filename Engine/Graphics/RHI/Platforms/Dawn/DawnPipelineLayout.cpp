#include "DawnPipelineLayout.h"

#if defined(ENABLE_WEBGPU) && ENABLE_WEBGPU

#include "DawnDevice.h"
#include "DawnDescriptorSetLayout.h"
#include <iostream>
#include <vector>

namespace primal::graphics::rhi {

DawnPipelineLayout::DawnPipelineLayout(DawnDevice& device)
    : device_(device) {
}

DawnPipelineLayout::~DawnPipelineLayout() {
    Destroy();
}

bool DawnPipelineLayout::Initialize(const PipelineLayoutDesc& desc) {
    std::vector<WGPUBindGroupLayout> bindGroupLayouts;

    setLayoutCount_ = desc.setLayoutCount;

    // 1. For each descriptor set layout, get the WGPUBindGroupLayout from the allocator
    for (u32 i = 0; i < desc.setLayoutCount; ++i) {
        DawnDescriptorSetLayout* setLayout = device_.GetDescriptorSetLayout(desc.setLayouts[i]);
        if (setLayout) {
            bindGroupLayouts.push_back(setLayout->GetNativeLayout());
        } else {
            std::cerr << "[DawnPipelineLayout] Failed to get descriptor set layout at index " << i << std::endl;
            return false;
        }
    }

    // 2. Create push constant emulation bind group layout if push constant ranges exist
    if (desc.pushConstantRangeCount > 0 && desc.pushConstantRanges) {
        WGPUBindGroupLayoutEntry pushConstantEntry{};
        pushConstantEntry.nextInChain = nullptr;
        pushConstantEntry.binding = 0;
        pushConstantEntry.visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment | WGPUShaderStage_Compute;
        pushConstantEntry.buffer.type = WGPUBufferBindingType_Uniform;
        pushConstantEntry.buffer.hasDynamicOffset = false;
        pushConstantEntry.buffer.minBindingSize = constants::MAX_PUSH_CONSTANTS_SIZE;
        pushConstantEntry.sampler.type = WGPUSamplerBindingType_BindingNotUsed;
        pushConstantEntry.texture.sampleType = WGPUTextureSampleType_BindingNotUsed;
        pushConstantEntry.texture.viewDimension = WGPUTextureViewDimension_Undefined;
        pushConstantEntry.texture.multisampled = false;
        pushConstantEntry.storageTexture.access = WGPUStorageTextureAccess_BindingNotUsed;
        pushConstantEntry.storageTexture.format = WGPUTextureFormat_Undefined;
        pushConstantEntry.storageTexture.viewDimension = WGPUTextureViewDimension_Undefined;

        WGPUBindGroupLayoutDescriptor pushConstantBGLDesc{};
        pushConstantBGLDesc.nextInChain = nullptr;
        pushConstantBGLDesc.label = ToWGPUStringView("Push Constant Bind Group Layout");
        pushConstantBGLDesc.entryCount = 1;
        pushConstantBGLDesc.entries = &pushConstantEntry;

        pushConstantBindGroupLayout_ = wgpuDeviceCreateBindGroupLayout(
            device_.GetNativeDevice(), &pushConstantBGLDesc);

        if (!pushConstantBindGroupLayout_) {
            std::cerr << "[DawnPipelineLayout] Failed to create push constant bind group layout" << std::endl;
            return false;
        }

        bindGroupLayouts.push_back(pushConstantBindGroupLayout_);
    }

    // 3. Create WGPUPipelineLayoutDescriptor with all bind group layouts
    WGPUPipelineLayoutDescriptor layoutDesc{};
    layoutDesc.nextInChain = nullptr;
    layoutDesc.label = ToWGPUStringView("DawnPipelineLayout");
    layoutDesc.bindGroupLayoutCount = static_cast<u32>(bindGroupLayouts.size());
    layoutDesc.bindGroupLayouts = bindGroupLayouts.empty() ? nullptr : bindGroupLayouts.data();

    wgpuLayout_ = wgpuDeviceCreatePipelineLayout(device_.GetNativeDevice(), &layoutDesc);
    if (!wgpuLayout_) {
        std::cerr << "[DawnPipelineLayout] Failed to create pipeline layout" << std::endl;
        return false;
    }

    return true;
}

void DawnPipelineLayout::Destroy() {
    if (wgpuLayout_) {
        wgpuPipelineLayoutRelease(wgpuLayout_);
        wgpuLayout_ = nullptr;
    }
    if (pushConstantBindGroupLayout_) {
        wgpuBindGroupLayoutRelease(pushConstantBindGroupLayout_);
        pushConstantBindGroupLayout_ = nullptr;
    }
}

} // namespace primal::graphics::rhi

#endif // ENABLE_WEBGPU
