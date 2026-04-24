#pragma once

#include "CommonHeaders.h"
#include "Graphics/RHI/Core/RHITypes.h"
#include "Graphics/RenderGraph/RenderGraphDefinitions.h"

namespace primal::graphics::rhi {
    class RHIDeviceBase;
}

namespace primal::graphics::rendergraph {
    class RenderGraph;
}

namespace primal::graphics::lumen {

// ============================================================================
// DDGI Parameters
// ============================================================================

/// DDGI volume configuration (set once at initialization).
struct DDGIRuntimeParams {
    u32   probe_count_x = 16;
    u32   probe_count_y = 8;
    u32   probe_count_z = 16;
    u32   rays_per_probe = 64;
    float probe_spacing = 2.0f;                 // 16 probes * 2.0 = 30 units coverage (denser probes for quality)
    float irradiance_temporal_weight = 0.02f;   // EMA alpha for irradiance
    float depth_temporal_weight = 0.2f;         // EMA alpha for depth
    float ray_max_distance = 50.0f;             // Must reach geometry across probe grid
};

/// Per-frame camera data that the caller must provide.
struct DDGICameraData {
    math::v3   camera_position;
    math::m4x4 view_matrix;
    math::m4x4 proj_matrix;
    math::m4x4 prev_view_matrix;
    math::m4x4 prev_proj_matrix;
    math::v3   light_direction;   // normalized world-space light direction
    math::v3   light_color;       // linear HDR light color
    u32        frame_index;
    float      delta_time;
};

/// Output from DDGI AddPass — probe irradiance buffer/depth textures available for sampling.
struct LumenDDGIOutput {
    rendergraph::RGResourceHandle ddgi_irradiance;  ///< Storage buffer irradiance (current frame)
    rendergraph::RGResourceHandle ddgi_depth;        ///< Texture3D depth (current frame)
};

// ============================================================================
// DDGIVolumeData — GPU constant buffer struct (must match Metal shader)
// ============================================================================

struct DDGIVolumeData {
    // Probe grid definition — v4 matches Metal's float3 16-byte alignment
    math::v4 ProbeOrigin;             // offset 0, 16 bytes (w unused)
    float    ProbeSpacing;            // offset 16

    // Explicit padding: Metal's uint3 has 16-byte alignment → offset 32
    float    _pad_before_counts[3];   // offset 20, 12 bytes

    // Use u32[4] to match Metal's uint3 16-byte slot (last element = padding)
    u32      ProbeCounts[4];          // offset 32, 16 bytes ([3] unused)
    u32      RaysPerProbe;            // offset 48
    u32      ProbeCountTotal;         // offset 52

    // Temporal filtering
    float    IrradianceBlurSigma;     // offset 56
    float    DepthBlurSigma;          // offset 60

    // Frame info
    float    DeltaTime;               // offset 64
    u32      FrameIndex;              // offset 68
    float    RayMaxDistance;          // offset 72

    float    ProbeHysteresis;         // offset 76
    float    TemporalAlpha;           // offset 80

    // GlobalSDF cascade data — v4 matches Metal's float4 alignment
    // C++ inserts implicit padding 84→96 for v4 16-byte alignment
    math::v4 SdfOrigins[3];           // offset 96
    math::v4 SdfVoxelSizes[3];        // offset 144
    math::v4 SdfExtents[3];           // offset 192
    u32      SdfResolutions[3];       // offset 240
    u32      SdfCascadeCount;         // offset 252
    math::v4 LightDirection;          // offset 256: xyz = light dir, w unused
    math::v4 LightColor;              // offset 272: xyz = light color, w unused
};

// ============================================================================
// DDGIRayData — GPU storage buffer element (must match Metal shader)
// ============================================================================

struct DDGIRayData {
    math::v4 radiance_dist;   // xyz = radiance, w = hit_distance (negative = miss)
};

// ============================================================================
// LumenDDGIPass
// ============================================================================

/**
 * @brief Self-contained DDGI pass: TraceRays -> UpdateIrradiance -> UpdateDepth.
 *
 * Lifecycle:
 *   1. Initialize(device, params)       -- once
 *   2. AddPass(graph, ..., camera_data) -- every frame
 *   3. Shutdown()                        -- once
 *
 * The pass owns all persistent GPU resources:
 *   - 3 compute pipelines (trace, irradiance update, depth update)
 *   - Triple-buffered descriptor sets, constant buffers
 *   - Persistent probe irradiance buffer/depth Texture3D (triple-buffered)
 *   - Ray data storage buffer
 */
class LumenDDGIPass {
public:
    LumenDDGIPass() = default;
    ~LumenDDGIPass();

    bool Initialize(rhi::RHIDeviceBase* device, const DDGIRuntimeParams& params = {});
    void Shutdown();

    LumenDDGIOutput AddPass(
        rendergraph::RenderGraph& graph,
        rendergraph::RGResourceHandle prev_frame_color,
        const DDGICameraData& camera_data,
        u32 current_frame_index);

    bool IsInitialized() const { return initialized_; }

    // Accessors for probe data (for sampling in other passes)
    rhi::ResourceHandle GetIrradianceBuffer(u32 frame_idx) const {
        return irradiance_buffers_[frame_idx % 3];
    }
    rhi::ResourceHandle GetDepthTexture(u32 frame_idx) const {
        return depth_textures_[frame_idx % 3];
    }
    rhi::ResourceHandle GetDepthBuffer(u32 frame_idx) const {
        return depth_buffers_[frame_idx % 3];
    }
    const DDGIRuntimeParams& GetParams() const { return params_; }
    const DDGIVolumeData& GetVolumeData() const { return volume_data_; }

private:
    void CreateDescriptorSetLayouts();
    void CreatePipelines();
    void CreateConstantBuffers();
    void CreateProbeTextures();

    bool              initialized_{ false };
    rhi::RHIDeviceBase* device_{ nullptr };
    DDGIRuntimeParams params_{};
    DDGIVolumeData    volume_data_{};
    math::v3          probe_origin_{ 0.0f };

    // Compute pipelines (3 sub-passes)
    rhi::PipelineHandle trace_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineHandle irradiance_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineHandle depth_pipeline_{ rhi::handles::INVALID_PIPELINE };

    // Pipeline layouts
    rhi::PipelineLayoutHandle trace_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineLayoutHandle irradiance_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineLayoutHandle depth_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };

    // Descriptor set layouts
    rhi::DescriptorSetLayoutHandle trace_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetLayoutHandle irradiance_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetLayoutHandle depth_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };

    // Descriptor sets (triple-buffered)
    rhi::DescriptorSetHandle trace_ds_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET
    };
    rhi::DescriptorSetHandle irradiance_ds_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET
    };
    rhi::DescriptorSetHandle depth_ds_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET
    };

    // Constant buffers (triple-buffered)
    rhi::ResourceHandle global_cb_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };
    rhi::ResourceHandle volume_cb_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };

    // Probe irradiance buffers (persistent, triple-buffered for history)
    rhi::ResourceHandle irradiance_buffers_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };
    rhi::ResourceHandle depth_textures_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };
    // Probe depth buffers (buffer-based, replaces texture3D for Apple Silicon)
    rhi::ResourceHandle depth_buffers_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };

    // Ray data storage buffer (single, reused each frame)
    rhi::ResourceHandle ray_data_buffer_{ rhi::handles::INVALID_RESOURCE };
};

} // namespace primal::graphics::lumen
