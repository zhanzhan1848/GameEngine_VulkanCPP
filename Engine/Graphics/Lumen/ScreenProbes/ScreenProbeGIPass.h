#pragma once

#include "CommonHeaders.h"
#include "Graphics/RHI/Core/RHITypes.h"
#include "Graphics/RenderGraph/RenderGraphDefinitions.h"
#include "Graphics/Lumen/GICacheView.h"
#include "ScreenProbeCacheValidation.h"

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
    float hzb_max_distance  = 25.0f; // T5.1: near-field HZB trace range (world units; 25 lets second-floor probes reach the sunlit nave floor)
    // F5 (M7): per-frame jitter of probe placement + ray direction set. UE
    // Lumen jitters every frame and lets the temporal filter average over
    // frames; a fixed grid + fixed Fibonacci set leaves banding EMA cannot
    // smooth. World-space position amplitude is clamped (jitter_max_world)
    // so the Temporal pass's position gate (posThreshold = 0.5) never
    // rejects a probe solely because of jitter.
    bool  enable_jitter     = true;
    float jitter_max_world  = 0.2f;  // < posThreshold / 2
    // F1 (M7): far-field radiance-cache early-out. SDF rays march only this
    // far, then unresolved rays query the DDGI volume at the stop point
    // (UE Lumen's world-probe distance-threshold semantics). Should sit
    // above hzb_max_distance and below max_ray_distance. 0 disables.
    float far_cache_start_distance = 30.0f;
    // F2 (M7): importance resampling of the Fibonacci direction set against
    // the previous frame's final probe SH (UE lighting-pdf × BRDF-pdf
    // semantics without a radiance map). Rays are re-allocated by inverse-CDF
    // over cosine × SH-luminance; the SH projection weight carries the
    // 1/(N·q) correction so the estimator stays unbiased.
    bool  enable_importance = true;
    // F4 (M7): fine-tier adaptive activation threshold — UE interpolation-
    // failure test (bilinear reconstruction error from the 4 coarse
    // neighbors, world units). 0 falls back to the T5.3 depth/normal
    // variance heuristic.
    float adaptive_pos_threshold = 0.35f;
};

/// Per-frame camera data.
struct ScreenProbeCameraData {
    math::m4x4 view_matrix;
    math::m4x4 proj_matrix;
    math::v3   camera_position;
    u32        frame_index;
    // T5.2: previous-frame matrices for probe reprojection (velocity-free
    // camera-motion compensation; static-scene assumption matches the
    // probe-grid disocclusion checks).
    math::m4x4 prev_view_matrix{};
    math::m4x4 prev_proj_matrix{};
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

    // DDGI probe fallback (T2.2) — miss rays sample the DDGI probe grid
    // instead of a constant sky gradient (Lumen's world-probe fallback edge).
    math::v4   ddgi_fb[2];             // offset 352: [0]=probe origin.xyz + spacing, [1]=counts.xyz + enabled(0/1)

    // T5.1 hybrid trace: near-field HZB march parameters
    // (near/far match the SSGI/SSR convention: 0.1/1000)
    math::v4   hzb_params;             // offset 384: x=near, y=far, z=mip_levels, w=hzb_max_distance

    // F1 (M7): far-field cache query. x = far_cache_start_distance (world
    // units) — SDF rays stop there and query the DDGI radiance cache at the
    // stop point (UE world-probe semantics); 0 disables (legacy miss chain).
    // F2 (M7): y = importance resampling on/off (1/0).
    math::v4   cache_params;           // offset 400: x=far_cache_start, y=importance, z=adaptive_thr, w=cascade
    // P1（sponza-gap-reaudit §4.1/4.2 逐阶段台账）: trace 诊断模式。
    // x = mode: 0=off, 1=分支统计, 2=HZB hit 去亮度惩罚（raw 对照）,
    // 3=SC card 同亮度惩罚（两端一致对照）。yzw 保留。
    math::v4   diag_params;            // offset 416
    // G2 A/B（2026-09-16）: x = 三线性 SDF 梯度 hint 开关（TraceSDF 消费，
    // 供 SC 朝向门的命中法线）。仅声明尾扩；未读取该字段的 kernel 其
    // std140 前缀布局稳定，无需同步改声明。
    math::v4   hint_params;            // offset 432
};

static_assert(sizeof(ScreenProbeGlobalData) == 448, "ScreenProbe shader constants must stay 448 bytes");
static_assert(offsetof(ScreenProbeGlobalData, ddgi_fb) == 352, "DDGI constants offset mismatch");
static_assert(offsetof(ScreenProbeGlobalData, hzb_params) == 384, "HZB constants offset mismatch");
static_assert(offsetof(ScreenProbeGlobalData, cache_params) == 400, "Cache constants offset mismatch");
static_assert(offsetof(ScreenProbeGlobalData, diag_params) == 416, "Diag constants offset mismatch");

// ============================================================================
// RadianceCascadeParams — T3.5 cascade update constants (must match both
// shader dialects: std140 block in RadianceCascadeUpdate.comp, MSL struct in
// RadianceCascadeUpdate.metal). 4 × vec4 + 4 scalars = 80B in both layouts.
// ============================================================================

struct RadianceCascadeParams {
    math::v4 origin_spacing;        // xyz = cascade origin (world), w = texel spacing
    math::v4 counts_enabled;        // xyz = cascade resolution (32,32,32), w = enabled (0/1)
    math::v4 ddgi_origin_spacing;   // xyz = DDGI probe grid origin, w = probe spacing
    math::v4 ddgi_counts;           // xyz = DDGI probe counts per axis
    float    temporal_weight;       // EMA factor toward the previous cascade
    float    frame_index;           // EMA engages from frame 2
    float    pad[2];
};

static_assert(sizeof(RadianceCascadeParams) == 80, "RadianceCascade shader constants must stay 80 bytes");

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
        u32 current_frame_index,
        rendergraph::RGResourceHandle hzb_texture = rendergraph::RGResourceHandle{});  // T5.1 near-field

    bool IsInitialized() const { return initialized_; }

    /// Get the output GI texture for direct sampling in other passes.
    rhi::ResourceHandle GetOutputTexture() const { return output_texture_; }  // DEBUG: bypass denoise
    // 滤波后输出（fusion 实际消费的纹理）——诊断 readback 用持久物理句柄，
    // 绕开 RG 瞬态资源的 CopySource 布局迁移（Vulkan 下拷贝回零）。
    rhi::ResourceHandle GetFilteredOutputTexture() const { return output_texture_filtered_; }

    const ScreenProbeParams& GetParams() const { return params_; }

    // T5.1: HZB near-field input (nanite::HZBSystem R32F min-mip chain).
    // Call alongside AddPass's hzb RG handle; mip count feeds the march.
    void SetHZBData(rhi::ResourceHandle hzb_texture, u32 mip_levels) {
        hzb_texture_ = hzb_texture;
        hzb_mip_levels_ = mip_levels;
    }

    // Surface Cache integration
    // X2/A3: 视图签名（推荐）—— 从 SurfaceCachePass::GetCacheView() 直传。
    // 注意 cardCount 上传的是容量值（max_card_count == LumenConfig::
    // surface_cache_max_cards），与旧注入点逐字节等价；活跃卡片数在
    // view.card_count，供后续消费者使用。
    void SetSurfaceCacheData(const SurfaceCacheView& view) {
        surface_cache_lighting_atlas_ = view.lighting_atlas;
        surface_cache_depth_atlas_ = view.depth_atlas;
        surface_cache_card_data_buffer_ = view.card_data;
        surface_cache_card_lookup_buffer_ = view.card_lookup;
        surface_cache_atlas_size_ = view.atlas_size;
        surface_cache_card_count_ = view.max_card_count;
        surface_cache_grid_offsets_buffer_ = view.grid_offsets;
        surface_cache_grid_cards_buffer_ = view.grid_cards;
        surface_cache_available_ = IsScreenProbeSurfaceCacheValid(view);
    }

    // T1.1 (C2): single physical import identity — the pipeline imports the
    // SC lighting atlas ONCE ("SCLightingAtlas") and hands the RG handle
    // here; AddPass then skips its own "SurfaceCache_Lighting_ForProbes"
    // import, which aliased the same physical texture under a second
    // identity (cross-identity RAW deps invisible to the scheduler).
    void SetExternalLightingRG(rendergraph::RGResourceHandle rg) { external_lighting_rg_ = rg; }

    // --- T4.3 二批: per-mesh detail SDF slots (PRIMAL_DDGI_DETAIL_SDF=1) ---
    // Same contract as LumenDDGIPass::DetailSDFSlot (POD, 160 B, keep in
    // sync): the split SDF trace refines coarse GlobalSDF hit DISTANCES
    // against the per-mesh field — distance only, screen probes carry their
    // own GBuffer normals. count=0 → inert.
    static constexpr u32 kDetailSDFSlots = 4;
    struct DetailSDFSlot {
        math::m4x4 inv_world;
        math::v4   normal_matrix[3];   // unused by SPGI (kept for layout parity)
        math::v4   bounds_min;
        math::v4   bounds_max;
        math::v4   params;             // x=resolution, y=max_dim, z=signed, w=voxel_size
    };
    void SetDetailSDFSlots(const rhi::ResourceHandle* textures,
                           const DetailSDFSlot* slots, u32 count) {
        detail_sdf_slot_count_ = (count > kDetailSDFSlots) ? kDetailSDFSlots : count;
        for (u32 i = 0; i < detail_sdf_slot_count_; ++i) {
            detail_sdf_textures_[i] = textures[i];
            detail_sdf_slots_[i] = slots[i];
        }
    }

    // DEPRECATED (X2/A3): 旧签名保留为转发适配器（等价映射进视图签名）。
    void SetSurfaceCacheData(
        rhi::ResourceHandle lighting_atlas,
        rhi::ResourceHandle card_data,
        rhi::ResourceHandle card_lookup,
        u32 atlas_size = 2048,
        u32 card_count = 0,
        rhi::ResourceHandle grid_offsets = rhi::handles::INVALID_RESOURCE,
        rhi::ResourceHandle grid_cards = rhi::handles::INVALID_RESOURCE) {
        SurfaceCacheView view;
        view.lighting_atlas = lighting_atlas;
        view.card_data = card_data;
        view.card_lookup = card_lookup;
        view.grid_offsets = grid_offsets;
        view.grid_cards = grid_cards;
        view.atlas_size = atlas_size;
        view.max_card_count = card_count;   // 旧语义即容量值
        SetSurfaceCacheData(view);
    }

    // DDGI probe fallback (T2.2): miss rays evaluate the nearest DDGI probe's    // SH L0+L1 along the ray direction.
    // X2/A3: 视图签名（推荐）—— 从 LumenDDGIPass::GetVolumeView() 直传。
    void SetDDGIFallbackData(const DDGIVolumeView& view) {
        ddgi_irradiance_buffer_ = view.irradiance;
        ddgi_fb_origin_ = view.origin;
        ddgi_fb_counts_ = view.counts;
        ddgi_fb_spacing_ = view.spacing;
        ddgi_fallback_available_ = IsScreenProbeDDGIValid(view);
    }

    // Offline high-res SDF (single volume, e.g. OfflineSDFMerger's 384³ field).
    // When set, the SDF far-field trace uses it instead of the runtime
    // GlobalSDF cascades — same semantics as LumenDDGIPass::SetOfflineSDFSource
    // (single cascade, count=1; outside the volume sampleBestSDF returns the
    // 1e10 empty sentinel and the ray resolves as miss).
    void SetOfflineSDFSource(rhi::ResourceHandle texture,
                             math::v3 origin, math::v3 extent, u32 resolution) {
        offline_sdf_texture_ = texture;
        offline_sdf_origin_ = origin;
        offline_sdf_extent_ = extent;
        offline_sdf_resolution_ = resolution;
        has_offline_sdf_ = (texture != rhi::handles::INVALID_RESOURCE && resolution > 0);
    }
    bool HasOfflineSDF() const { return has_offline_sdf_; }

    // DEPRECATED (X2/A3): 旧签名保留为转发适配器。
    void SetDDGIFallbackData(
        rhi::ResourceHandle irradiance_buffer,
        const math::v3& probe_origin,
        const math::v3& probe_counts,
        float probe_spacing) {
        DDGIVolumeView view;
        view.irradiance = irradiance_buffer;
        view.origin = probe_origin;
        view.counts = probe_counts;
        view.spacing = probe_spacing;
        SetDDGIFallbackData(view);
    }

    // X2/A3: 时域策略映射 —— EMA 即现有 temporal accumulation alpha 路径
    // （默认，行为不变）；EMAVarianceClamp/Reprojection 暂存枚举（TODO X3）。
    void SetTemporalStrategy(GITemporalStrategy strategy);
    GITemporalStrategy GetTemporalStrategy() const { return temporal_strategy_; }

    // A11: skip history blending for exactly one temporal accumulation pass —
    // call on camera cut / scene switch so stale probe radiance never bleeds
    // into the new view.
    void RequestTemporalReset() { temporal_reset_pending_ = true; }

    // P1（reaudit §4.1/4.2）: trace 分支诊断。mode 见 diag_params 注释；
    // mode>=1 时 finalize 逐射线记录 hit 分支/亮度统计（Vulkan only），
    // 由测试在指定帧读回（仿 FROXEL_DUMP 的 SPGI_TRACE_DIAG 通道）。
    void SetTraceDiagMode(u32 mode) { trace_diag_mode_ = mode; }
    u32  GetTraceDiagMode() const { return trace_diag_mode_; }
    rhi::ResourceHandle GetTraceStatsBuffer() const { return trace_stats_buffer_; }
    u32 GetGridWidth() const { return grid_width_; }
    u32 GetGridHeight() const { return grid_height_; }

private:
    void CreateDescriptorSetLayouts();
    void CreatePipelines();
    void CreateConstantBuffers();
    // T3.5: world-space radiance cascade (32³ RGBA16F double-buffered)
    void CreateRadianceCascade();
    // writeTex/prevTex are the RG-resolved physical cascade textures of this
    // frame (writeIdx = cascade_update_frame_ % 2; prev = 1 - writeIdx).
    // SH-L1: 4 texture pairs per slot — [0] = L0, [1..3] = L1x/L1y/L1z.
    void DispatchCascadeUpdate(rhi::RHICommandBuffer* cmd, u32 frameIdx,
                               const rhi::ResourceHandle* writeTexs /*[4]*/,
                               const rhi::ResourceHandle* prevTexs /*[4]*/);
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
    rhi::PipelineHandle hzb_trace_pipeline_{ rhi::handles::INVALID_PIPELINE };   // T5.1 2a near-field
    rhi::PipelineHandle sdf_trace_pipeline_{ rhi::handles::INVALID_PIPELINE };   // T5.1 2b far-field
    rhi::PipelineHandle finalize_pipeline_{ rhi::handles::INVALID_PIPELINE };    // T5.1 2c radiance
    rhi::PipelineHandle avg_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineHandle temporal_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineHandle spatial_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineHandle gather_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineHandle denoise_pipeline_{ rhi::handles::INVALID_PIPELINE };

    // Pipeline layouts
    rhi::PipelineLayoutHandle place_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineLayoutHandle trace_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineLayoutHandle hzb_trace_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
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
    rhi::DescriptorSetLayoutHandle hzb_trace_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
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
    rhi::DescriptorSetHandle hzb_trace_ds_[3]{
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

    // T4.3 二批: detail-SDF slot table UBO (656B, mirror of LumenDDGIPass)
    u32  detail_sdf_slot_count_{ 0 };
    DetailSDFSlot detail_sdf_slots_[kDetailSDFSlots]{};
    rhi::ResourceHandle detail_sdf_textures_[kDetailSDFSlots]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };
    rhi::ResourceHandle detail_table_cb_[3]{
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
    // Temporal output must not alias current SH: variance clamp reads neighbors.
    rhi::ResourceHandle probe_temporal_radiance_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };
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
    // T3.5: world-space radiance cascade (32³ RGBA16F) — pre-integrated far-field
    // radiance from DDGI probes, sampled by far-cache to avoid 16-read probe queries.
    // 2026-09-10 SH-L1 upgrade: texel semantics are RGB = raw SH coefficient,
    // A = validity; radiance_cascade_ holds L0, radiance_cascade_l1_[slot][0..2]
    // hold the L1x/L1y/L1z coefficients. Far-cache evaluates the canonical DDGI
    // L0+L1 basis along the ray direction (the old DC-only cascade was isotropic).
    static constexpr u32 kRadianceCascadeRes = 32;
    rhi::ResourceHandle radiance_cascade_[2]{                                         // double-buffered for temporal EMA
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle radiance_cascade_l1_[2][3]{};                                 // [slot][L1x/L1y/L1z]
    rhi::PipelineHandle cascade_update_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineLayoutHandle cascade_update_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::DescriptorSetLayoutHandle cascade_update_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetHandle cascade_update_set_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET };
    u32 cascade_update_frame_ = 0;   // frame counter for temporal EMA gating
    // Cascade world extent — covers a larger volume than the DDGI probe grid.
    math::v3 cascade_origin_{0.0f};
    math::v3 cascade_extent_{128.0f};  // cubic extent (single scalar for simplicity)
    float cascade_spacing_ = 128.0f / 32.0f;  // = 4.0 world units per cascade texel
    // T3.5: per-frame RadianceCascadeParams upload (80B, triple-buffered;
    // Metal overrides buffer(3) with setBytes at dispatch time).
    rhi::ResourceHandle cascade_params_cb_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE };
    // 1³ RGBA16F dummy bound at the finalize cascade slot when the cascade
    // system is off (layout must stay complete for the recompiled shaders).
    rhi::ResourceHandle dummy_texture_3d_{ rhi::handles::INVALID_RESOURCE };

    // Dummy resources for Metal validation (bound when surface cache is unavailable)
    rhi::ResourceHandle dummy_buffer_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle dummy_texture_2d_{ rhi::handles::INVALID_RESOURCE };

    // Surface Cache integration
    rhi::ResourceHandle surface_cache_lighting_atlas_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle surface_cache_depth_atlas_{ rhi::handles::INVALID_RESOURCE };
    rendergraph::RGResourceHandle external_lighting_rg_{};   // T1.1 (C2)
    rhi::ResourceHandle surface_cache_card_data_buffer_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle surface_cache_card_lookup_buffer_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle surface_cache_grid_offsets_buffer_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle surface_cache_grid_cards_buffer_{ rhi::handles::INVALID_RESOURCE };
    u32  surface_cache_atlas_size_ = 0;
    u32  surface_cache_card_count_ = 0;
    bool surface_cache_available_ = false;

    // DDGI probe fallback (T2.2)
    rhi::ResourceHandle ddgi_irradiance_buffer_{ rhi::handles::INVALID_RESOURCE };
    math::v3 ddgi_fb_origin_{ 0.0f };
    math::v3 ddgi_fb_counts_{ 0.0f };
    float ddgi_fb_spacing_ = 0.0f;
    bool ddgi_fallback_available_ = false;

    // T5.1: HZB near-field input
    rhi::ResourceHandle hzb_texture_{ rhi::handles::INVALID_RESOURCE };
    u32 hzb_mip_levels_ = 0;

    // Offline high-res SDF override for the far-field trace
    bool has_offline_sdf_ = false;
    rhi::ResourceHandle offline_sdf_texture_{ rhi::handles::INVALID_RESOURCE };
    math::v3 offline_sdf_origin_{ 0.0f };
    math::v3 offline_sdf_extent_{ 0.0f };
    u32 offline_sdf_resolution_ = 0;

    // X2/A3 时域策略枚举存储（EMA = 现有 temporal accumulation，行为不变）
    GITemporalStrategy temporal_strategy_ = GITemporalStrategy::EMA;
    // A11: one-shot temporal reset flag (alpha forced to 1.0 for one pass)
    bool temporal_reset_pending_ = false;

    // P1（reaudit §4.1/4.2）: finalize 分支统计缓冲（uvec4[4][17]，host-visible
    // 便于读回；atomic 仅在 diag_params.x>0.5 时执行，默认路径零开销）。
    u32 trace_diag_mode_{ 0 };
    rhi::ResourceHandle trace_stats_buffer_{ rhi::handles::INVALID_RESOURCE };
};

} // namespace primal::graphics::lumen
