#pragma once

#include "DawnCommon.h"

#if defined(ENABLE_WEBGPU) && ENABLE_WEBGPU

#include "../../Core/RHITypes.h"

namespace primal::graphics::rhi {

class DawnDevice;

/// Per-binding pending data, populated by updateDescriptorSetsImpl.
struct DawnPendingBinding {
    DescriptorType type{ DescriptorType::Unknown };
    u32 engineBinding{ 0 };

    // Buffer binding
    WGPUBuffer buffer{ nullptr };
    u64 offset{ 0 };
    u64 size{ 0 };

    // Texture binding
    WGPUTextureView textureView{ nullptr };

    // Sampler binding
    WGPUSampler sampler{ nullptr };

    bool populated{ false };
};

class DawnDescriptorSet {
    friend class DawnDevice;
public:
    ~DawnDescriptorSet();

    /// Get the WGPUBindGroup, lazily building it if dirty.
    WGPUBindGroup GetBindGroup();

    /// Get the layout handle used to create this set.
    DescriptorSetLayoutHandle GetLayout() const { return layoutHandle_; }

    /// Check if rebuild is needed.
    bool NeedsRebuild() const { return needsRebuild_; }

public:
    DawnDescriptorSet(DawnDevice& device);

private:
    bool Initialize(const DescriptorSetDesc& desc);
    void Destroy();

    /// Build (or rebuild) the WGPUBindGroup from pending bindings.
    bool BuildBindGroup();

    /// Mark dirty so next GetBindGroup() will rebuild.
    void MarkDirty() { needsRebuild_ = true; }

    /// Get mutable pending binding for a given engine binding slot.
    DawnPendingBinding* GetOrCreatePending(u32 engineBinding, DescriptorType type);

    // Diagnostic accessors
    u32 PendingCount() const { return static_cast<u32>(pendingBindings_.size()); }
    DescriptorType PendingType(u32 idx) const {
        return idx < pendingBindings_.size() ? pendingBindings_[idx].type : DescriptorType::Unknown;
    }

    DawnDevice& device_;
    DescriptorSetLayoutHandle layoutHandle_{ handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    WGPUBindGroup wgpuGroup_ = nullptr;
    bool needsRebuild_ = true;

    utl::vector<DawnPendingBinding> pendingBindings_;
};

} // namespace primal::graphics::rhi

#endif // ENABLE_WEBGPU
