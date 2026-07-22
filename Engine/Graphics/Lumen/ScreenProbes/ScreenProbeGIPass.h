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
// Screen Probe GI Parameters
// ============================================================================

/// Screen Probe GI configuration (set once at initialization).
struct ScreenProbeParams {
    u32   downsample_factor = 16;    // Pixels per probe (16 = 16x16 grid, matched to temporal accumulation)
    u32   rays_per_probe    = 64;    // Rays per probe (64 for better directional coverage)
    float max_ray_distance  = 50.0f; // Maximum ray travel distance (world units)
    float gather_radius     = 2.0f;  // Bilateral filter radius in probe grid cells
};

/// Per-frame camera data.
struct ScreenProbeCameraData {
    math::m4x4 view_matrix;
    math::m4x4 proj_matrix;
    math::v3   camera_position;
    u32        frame_index;
};

/// Output from AddPass.
struct ScreenProbeGIOutput {
    rendergraph::RGResourceHandle gi_output;  ///< Full-resolution GI texture (RGBA16_Float)
};

// ============================================================================
// ScreenProbeGlobalData — GPU constant buffer (must match Metal shader)
// ============================================================================

struct ScreenProbeGlobalData {
    // Camera matrices
    math::m4x4 view_projection;        // offset 0
    math::m4x4 inv_view_projection;    // offset 64
    math::v4   camera_position;        // offset 128: xyz = camera pos

    // Screen probe grid parameters
    math::v4   grid_params;            // offset 144: x=gridW, y=gridH, z=downsample, w=raysPerProbe
    math::v4   trace_params;           // offset 160: x=maxRayDist, y=gatherRadius, z=renderWidth, w=renderHeight

    // GlobalSDF cascade data
    math::v4   sdf_origins[3];         // offset 176
    math::v4   sdf_voxel_sizes[3];     // offset 224
    math::v4   sdf_extents[3];         // offset 272
    math::v4   sdf_resolutions;        // offset 320: x=res0, y=res1, z=res2, w=cascadeCount

    // Surface Cache integration data
    math::v4   surface_cache_params;   // offset 336: x=atlasSize, y=cardCount, z=surfaceCacheAvailable(0/1), w=unused
};

// ============================================================================
// ScreenProbeGIPass
// ============================================================================

/**
 * @brief Self-contained Screen Probe GI pass: Place -> TraceRays -> Gather.
 *
 * Screen Probe GI places probes in screen space (e.g. every 16x16 pixels),
 * traces rays through GlobalSDF from each probe, and gathers the radiance
 * into a full-resolution GI texture via bilateral filtering.
 *
 * Lifecycle:
 *   1. Initialize(device, width, height, params)  -- once
 *   2. AddPass(graph, ..., camera_data)            -- every frame
 *   3. Shutdown()                                   -- once
 */
class ScreenProbeGIPass {
public:
    ScreenProbeGIPass() = default;
    ~ScreenProbeGIPass();

    bool Initialize(rhi::RHIDeviceBase* device,
                    u32 render_width, u32 render_height,
                    const ScreenProbeParams& params = {});
    void Shutdown();

    ScreenProbeGIOutput AddPass(
        rendergraph::RenderGraph& graph,
        rendergraph::RGResourceHandle gbuffer_depth,
        rendergraph::RGResourceHandle gbuffer_normal,
        rendergraph::RGResourceHandle prev_frame_color,
        const ScreenProbeCameraData& camera_data,
        u32 current_frame_index);

    bool IsInitialized() const { return initialized_; }

    /// Get the output GI texture for direct sampling in other passes.
    rhi::ResourceHandle GetOutputTexture() const { return output_texture_; }  // DEBUG: bypass denoise

    const ScreenProbeParams& GetParams() const { return params_; }

    // Surface Cache integration
    void SetSurfaceCacheData(
        rhi::ResourceHandle lighting_atlas,
        rhi::ResourceHandle card_data,
        rhi::ResourceHandle card_lookup,
        u32 atlas_size = 2048,
        u32 card_count = 0) {
        surface_cache_lighting_atlas_ = lighting_atlas;
        surface_cache_card_data_buffer_ = card_data;
        surface_cache_card_lookup_buffer_ = card_lookup;
        surface_cache_atlas_size_ = atlas_size;
        surface_cache_card_count_ = card_count;
        surface_cache_available_ = true;
    }

private:
    void CreateDescriptorSetLayouts();
    void CreatePipelines();
    void CreateConstantBuffers();
    void CreateBuffers();

    bool              initialized_{ false };
    rhi::RHIDeviceBase* device_{ nullptr };
    ScreenProbeParams params_{};
    u32               render_width_{ 0 };
    u32               render_height_{ 0 };
    u32               grid_width_{ 0 };
    u32               grid_height_{ 0 };

    // Compute pipelines (7 sub-passes + split trace)
    rhi::PipelineHandle place_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineHandle trace_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineHandle sdf_trace_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineHandle finalize_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineHandle avg_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineHandle temporal_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineHandle spatial_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineHandle gather_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineHandle denoise_pipeline_{ rhi::handles::INVALID_PIPELINE };

    // Pipeline layouts
    rhi::PipelineLayoutHandle place_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineLayoutHandle trace_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineLayoutHandle sdf_trace_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineLayoutHandle finalize_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineLayoutHandle avg_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineLayoutHandle temporal_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineLayoutHandle spatial_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineLayoutHandle gather_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineLayoutHandle denoise_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };

    // Descriptor set layouts
    rhi::DescriptorSetLayoutHandle place_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetLayoutHandle trace_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetLayoutHandle sdf_trace_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetLayoutHandle finalize_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetLayoutHandle avg_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetLayoutHandle temporal_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetLayoutHandle spatial_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetLayoutHandle gather_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetLayoutHandle denoise_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };

    // Descriptor sets (triple-buffered)
    rhi::DescriptorSetHandle place_ds_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET
    };
    rhi::DescriptorSetHandle trace_ds_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET
    };
    rhi::DescriptorSetHandle sdf_trace_ds_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET
    };
    rhi::DescriptorSetHandle finalize_ds_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET
    };
    rhi::DescriptorSetHandle avg_ds_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET
    };
    rhi::DescriptorSetHandle temporal_ds_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET
    };
    rhi::DescriptorSetHandle spatial_ds_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET
    };
    rhi::DescriptorSetHandle denoise_ds_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET
    };
    rhi::DescriptorSetHandle gather_ds_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET
    };

    // Constant buffers (triple-buffered)
    rhi::ResourceHandle global_cb_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };

    // Probe data buffers (triple-buffered to avoid GPU read-write race)
    rhi::ResourceHandle probe_positions_buffer_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };  // gridW * gridH * float4
    rhi::ResourceHandle probe_normals_buffer_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };  // gridW * gridH * float4
    rhi::ResourceHandle probe_radiance_buffer_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };  // gridW * gridH * raysPerProbe * float4
    rhi::ResourceHandle hit_distance_buffer_{ rhi::handles::INVALID_RESOURCE };  // GPU-only intermediate (single buffer)
    rhi::ResourceHandle probe_avg_radiance_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };  // gridW * gridH * 4 * float4 (SH2: L0+L1 coefficients per probe)
    rhi::ResourceHandle avg_constants_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };  // 2 * uint32 = totalProbes + raysPerProbe
    rhi::ResourceHandle temporal_cb_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };  // TemporalConstants: totalProbes, alpha, threshold, pad
    rhi::ResourceHandle spatial_cb_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };  // SpatialConstants: totalProbes, gridW, sigma, pad
    rhi::ResourceHandle denoise_cb_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };  // DenoiseParams: sigma_depth, sigma_normal, sigma_spatial, kernel_radius, width, height

    // Output textures (full-resolution RGBA16_Float)
    rhi::ResourceHandle output_texture_{ rhi::handles::INVALID_RESOURCE };           // Gather output (raw)
    rhi::ResourceHandle output_texture_filtered_{ rhi::handles::INVALID_RESOURCE };  // Denoise output (filtered)

    // Dummy resources for Metal validation (bound when surface cache is unavailable)
    rhi::ResourceHandle dummy_buffer_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle dummy_texture_2d_{ rhi::handles::INVALID_RESOURCE };

    // Surface Cache integration
    rhi::ResourceHandle surface_cache_lighting_atlas_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle surface_cache_card_data_buffer_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle surface_cache_card_lookup_buffer_{ rhi::handles::INVALID_RESOURCE };
    u32  surface_cache_atlas_size_ = 0;
    u32  surface_cache_card_count_ = 0;
    bool surface_cache_available_ = false;
};

} // namespace primal::graphics::lumen
