#pragma once

#include "DawnCommon.h"

#if defined(ENABLE_WEBGPU) && ENABLE_WEBGPU

#include "../../Core/RHITypes.h"

namespace primal::graphics::rhi {

class DawnDevice;

class DawnDescriptorSetLayout {
    friend class DawnDevice;
public:
    ~DawnDescriptorSetLayout();
    WGPUBindGroupLayout GetNativeLayout() const { return wgpuLayout_; }
    u32 GetBindingCount() const { return bindingCount_; }

    /// Returns the WGPU binding number for a given engine binding index.
    /// Handles Metal dual-namespace remapping (texture/buffer same slot).
    u32 GetWGPUBinding(u32 engineBinding, DescriptorType type) const;

    /// Returns the WGPU sampler binding number for a CombinedImageSampler at the given engine binding.
    u32 GetCombinedSamplerWGPUBinding(u32 engineBinding) const;

    const utl::vector<DescriptorSetLayoutBinding>& GetBindings() const { return bindings_; }
    u32 GetDynamicBindingCount() const { return dynamicBindingCount_; }

public:
    DawnDescriptorSetLayout(DawnDevice& device);

private:
    bool Initialize(const DescriptorSetLayoutDesc& desc);
    void Destroy();

    DawnDevice& device_;
    WGPUBindGroupLayout wgpuLayout_ = nullptr;
    u32 bindingCount_ = 0;
    u32 dynamicBindingCount_ = 0;
    utl::vector<DescriptorSetLayoutBinding> bindings_;

    // Maps engine binding index → WGPU binding number.
    // Handles collisions where Metal uses the same binding slot for different types.
    struct BindingMapping {
        u32 engineBinding;
        DescriptorType type;
        u32 wgpuBinding;
    };
    utl::vector<BindingMapping> bindingMappings_;

    // CombinedImageSampler: maps engine binding → WGPU sampler binding
    struct CombinedSamplerMapping {
        u32 engineBinding;
        u32 samplerBinding;
    };
    utl::vector<CombinedSamplerMapping> combinedSamplerMappings_;
};

} // namespace primal::graphics::rhi

#endif // ENABLE_WEBGPU
