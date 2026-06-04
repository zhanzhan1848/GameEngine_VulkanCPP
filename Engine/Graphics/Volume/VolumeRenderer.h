#pragma once

#include "CommonHeaders.h"
#include "Graphics/Volume/VolumeTypes.h"
#include "Graphics/RHI/Core/RHITypes.h"

namespace primal::graphics::rhi {
    class RHIDeviceBase;
    class RHICommandBuffer;
}

namespace primal::graphics::volume {

/// Forward-rendered volume object using proxy cube mesh + fragment ray march.
/// Renders into its own RGBA16_Float scatter texture, then feeds into
/// FusionComposite via the volume_scatter path (pre-tone-map HDR blend).
class VolumeRenderer {
public:
    VolumeRenderer() = default;
    ~VolumeRenderer();

    bool Initialize(rhi::RHIDeviceBase* device, u32 render_width, u32 render_height);
    void Shutdown();

    /// Render proxy cube into internal scatter texture (call before render graph).
    void Render(rhi::RHICommandBuffer* cmd,
                const VolumeCameraData& camera_data,
                u32 frame_index,
                u32 width, u32 height);

    /// Get the scatter output texture for the given frame index.
    rhi::ResourceHandle GetScatterTexture(u32 frame_index) const;

    bool IsInitialized() const { return initialized_; }

private:
    void CreatePipeline();
    void CreateDescriptorSets();
    void CreateConstantBuffers();
    void CreateNoiseTexture();
    void CreateOutputTextures();

    bool              initialized_{false};
    rhi::RHIDeviceBase* device_{nullptr};
    u32               render_width_{0};
    u32               render_height_{0};

    // Graphics pipeline (forward, blended)
    rhi::PipelineHandle        pipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::PipelineLayoutHandle  pipeline_layout_{rhi::handles::INVALID_PIPELINE_LAYOUT};
    rhi::DescriptorSetLayoutHandle set_layout_{rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};

    // Triple-buffered descriptor sets + constant buffers
    rhi::DescriptorSetHandle descriptor_sets_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET
    };
    rhi::ResourceHandle params_cb_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };
    rhi::ResourceHandle transform_cb_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };

    // Persistent output textures (triple-buffered, RGBA16_Float)
    rhi::ResourceHandle scatter_texture_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };

    // Default noise texture (128³ Worley noise, created once)
    rhi::ResourceHandle noise_texture_{rhi::handles::INVALID_RESOURCE};

    // Test volume AABB — positioned above scene for debugging
    math::v3 test_volume_origin_{-1.5f, 5.0f, -1.5f};
    math::v3 test_volume_extent_{3.0f, 3.0f, 3.0f};
};

} // namespace primal::graphics::volume
