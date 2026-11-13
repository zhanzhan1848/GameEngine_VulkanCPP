#pragma once
#include "Graphics/Lumen/DDGI/VarianceFenceTicket.h"

#include "CommonHeaders.h"
#include "Graphics/RHI/Core/RHITypes.h"
#include "Graphics/RenderGraph/RenderGraphDefinitions.h"
#include "Graphics/Dawn/DawnDebugParams.h"
#include "Graphics/Lumen/GIDebugParams.h"
#include "Graphics/Lumen/GICacheView.h"
#include "Graphics/Lumen/DDGISlotMap.h"
#include "FarUpdatePolicy.h"
#include "Graphics/RHI/Core/RHIReadbackRing.h"
#include <vector>
#include <memory>

namespace primal::graphics::rhi {
    class RHIDeviceBase;
    class RHICommandBuffer;
}

namespace primal::graphics::rendergraph {
    class RenderGraph;
}

namespace primal::graphics::lumen {

// Forward declaration
class StaticProbeVolume;
class SurfaceCachePass;
struct DDGIShProducerAudit;

// ============================================================================
// DDGI Probe State (for importance-based partial update scheduling)
// ============================================================================

struct DDGIProbeState {
    float max_depth_variance = 0.0f;
    u32 last_update_frame = 0;
};

// ============================================================================
// DDGI Parameters
// ============================================================================

/// DDGI volume configuration (set once at initialization).
/// Note: these struct defaults are NOT the runtime config — presets in
/// RenderPipelineConfig::LumenConfigFromPreset() + LumenConfig defaults
/// (LumenTypes.h) override them. Probe density/spacing reference:
/// Docs/2026-09-16-ddgi-world-space-probe-grid-reference.md
struct DDGIRuntimeParams {
    u32   probe_count_x = 16;
    u32   probe_count_y = 8;
    u32   probe_count_z = 16;
    u32   rays_per_probe = 64;
    float probe_spacing = 4.0f;                 // 16 probes * 4.0 = 64 units coverage
    float irradiance_temporal_weight = 0.05f;   // EMA alpha for irradiance
    float depth_temporal_weight = 0.2f;         // EMA alpha for depth
    float ray_max_distance = 50.0f;             // Must reach geometry across probe grid
    bool deferred_field_reset = false;        // Vulkan far experiment, all volumes
    u32 far_update_budget = 0;                 // 0=legacy; explicit 512/2048 far only
    u32   max_probes_per_frame = 16384;          // Update ALL probes every frame (no flicker)
    // Scene-fit grids (Docs/completed/2026-09-18-ddgi-scene-fit-probe-grid-plan.md §5.2):
    // false = legacy centered origin (default), true = probe_origin is the
    // grid's min corner (probe[0] world position).
    math::v3 probe_origin = math::splat3(0.0f);
    bool     use_probe_origin = false;
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
    // GI-local startup clock. Unlike frame_index (absolute frame / readback
    // ring identity), this starts at zero only after cards + SDF are ready.
    u32        gi_frame_index = 0;
    bool       gi_content_ready = false; // Explicit source latch, independent of warmup length
    float      delta_time;
    // Vulkan screen-hit validation. Missing depth rejects screen samples;
    // Surface Cache / world probes remain available as fallback.
    rendergraph::RGResourceHandle screen_depth{};
    rendergraph::RGResourceHandle radiance_depth{};
    bool radiance_is_previous_frame = false;
};

/// Output from DDGI AddPass — probe irradiance buffer available for sampling.
struct LumenDDGIOutput {
    rendergraph::RGResourceHandle ddgi_irradiance;       ///< Storage buffer irradiance (current frame)
    rendergraph::RGResourceHandle ddgi_irradiance_hist;   ///< History irradiance buffer (for downstream reads)
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

    // Partial update scheduling
    u32      ProbeUpdateCount;        // offset 84 — number of probes to update this frame
    u32      SCLookupCount;           // offset 88 — Surface Cache card lookup count (was _pad)
    u32      SCAtlasSize;             // offset 92 — Surface Cache lighting atlas size

    // Probe grid relocation (camera-following grid shift, in probe cells)
    int      ProbeRelocationShift[3]; // offset 96, 12 bytes — fills padding before SdfOrigins
    // Probe ray sampler mode (offset 108 — the LAST padding slot before
    // SdfOrigins; struct size unchanged, so the unaligned Metal/Dawn mirrors,
    // which never read this slot, stay prefix-compatible).
    // 0 = legacy fixed Fibonacci (bit-identical), 1 = per-frame golden-angle
    // jitter, 2 = cosine-lobe mixture around the light. Semantics live in the
    // canonical Vulkan shader include ShaderLib/ProbeSampleLib.glsl; selected
    // via the DDGI_RAY_SAMPLE env var (see LumenDDGIPass.cpp Initialize).
    int      RaySampleMode = 0;

    // GlobalSDF cascade data — v4 matches Metal's float4 alignment
    // C++ inserts implicit padding 104→112 for v4 16-byte alignment
    math::v4 SdfOrigins[3];           // offset 112
    math::v4 SdfVoxelSizes[3];        // offset 160
    math::v4 SdfExtents[3];           // offset 208
    u32      SdfResolutions[3];       // offset 256
    u32      SdfCascadeCount;         // offset 268
    math::v4 LightDirection;          // offset 272: xyz = light dir, w unused
    math::v4 LightColor;              // offset 288: xyz = light color, w = intensity
    // Mode 11 (MeshletDynamicDDGI) canonical radiance inputs — match bake's ProbeBakingScene
    math::v4 SkyColor;                // offset 304: xyz = sky color, w = analytic history (legacy0/off1/Lambert2)
    math::v4 Albedo;                  // offset 320: xyz = uniform albedo, w unused
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
    LumenDDGIPass();
    ~LumenDDGIPass();

    bool Initialize(rhi::RHIDeviceBase* device, const DDGIRuntimeParams& params = {});
    void Shutdown();
    // Optional DUMP2-only producer readback after the frame has been submitted.
    bool FlushSHProducerAudit(u32 absoluteFrame);

    LumenDDGIOutput AddPass(
        rendergraph::RenderGraph& graph,
        rendergraph::RGResourceHandle prev_frame_color,
        const DDGICameraData& camera_data,
        u32 current_frame_index,
        rendergraph::RGResourceHandle sc_lighting_atlas = {});

    bool IsInitialized() const { return initialized_; }
    // Explicit host submission handshake; other callers never authorize a read.
    void BeginVarianceFenceFrame(u32 slot,rhi::CommandBufferHandle command,rhi::SyncHandle fence,u64 nativeFrame,u64 ticket);
    void CommitVarianceFenceFrame(u32 slot,rhi::CommandBufferHandle command,rhi::SyncHandle fence,u64 nativeFrame,u64 ticket,bool success);
    void ValidateVarianceFenceCommand(u32 slot,rhi::CommandBufferHandle command);
    struct VarianceFenceAuditView {
        bool enabled=false,accepted_frame=false;
        u64 accepted_count=0,submitted_count=0,source_frame=~u64(0),source_native=~u64(0),source_ticket=0,completion_serial=0;
        u64 source_epoch=0,source_digest=0,source_generation=0,current_generation=0,current_epoch=0,current_digest=0;
        u64 skip_counts[7]{};
    };
    VarianceFenceAuditView GetVarianceFenceAudit() const;
    void SyncVarianceFenceScene(u64 sceneEpoch,u64 sourceDigest);
    void SetVarianceFenceSceneProvider(const SurfaceCachePass* provider);
    bool VarianceFenceSceneStillCurrent();


    // Accessors for probe data (for sampling in other passes)
    // frame_idx = 绝对帧号（不是槽号）：内部经 DDGISlotMap.h 派生槽位。
    // 调用端语义约定：读"最近完成帧"传 frame+2（previousIndex），GIGather
    // 读"本帧 DDGI 完成后的数据"传 frame（currentIndex，RG 定序保证写先
    // 于读）。裸传槽号已废除（Phase 1d）。
    rhi::ResourceHandle GetIrradianceBuffer(u32 frame_idx) const {
        return irradiance_buffers_[DDGISlotsForFrame(frame_idx).currentIndex];
    }
    // 槽位显式版（Phase 1d）：调用端先取 DDGISlotsForFrame(frame)，再把
    // .previousIndex / .currentIndex 传入——读侧语义自证，禁止裸算槽号。
    rhi::ResourceHandle GetIrradianceBufferBySlot(u32 slot) const {
        return irradiance_buffers_[slot];
    }

    // A12/T0.10: zero the probe field (all 3 frame slots' irradiance + depth
    // history) so a scene switch can never inherit the previous scene's GI.
    // The normal trace/update loop re-seeds within a few frames.
    void ClearProbeField();
    struct FarUpdateAudit {
        u32 native_frame=0, count=0, rotation=0, max_age=0;
        u64 epoch=0, list_hash=0;
        bool bootstrap=false, source_ready=false, warmup=false, uploaded=false, recorded=false;
    };
    const FarUpdateAudit& GetFarUpdateAudit() const { return far_update_audit_; }
    bool UsesFarUpdatePolicy() const { return params_.far_update_budget!=0; }

    // T4.1 (D8): reset convergence ages so ALL probes become high-priority
    // in the partial-update scheduler — used on scene/dirty-transform events.
    void BoostProbePriority();
    // GI 能量所有权修复 Phase 5a（GIHistoryValidity 最小版）: 灯光修改事件 →
    // BoostProbePriority + 事件驱动 TemporalAlpha 抬升窗（0.35，30 帧——
    // 复用 PRIMAL_DDGI_ALPHA_HOLD 机制改为事件驱动）。
    void NotifyLightingChanged(u32 frame) {
        if (params_.far_update_budget) { ClearProbeField(); return; }
        BoostProbePriority();
        alpha_hold_until_frame_ = frame + 30u;
    }
    rhi::ResourceHandle GetDepthBuffer(u32 frame_idx) const {
        return depth_buffers_[DDGISlotsForFrame(frame_idx).currentIndex];
    }
    // 槽位显式版（Phase 1d，语义同 GetIrradianceBufferBySlot）。
    rhi::ResourceHandle GetDepthBufferBySlot(u32 slot) const {
        return depth_buffers_[slot];
    }
    // Optional measurement provenance, paired with the SAME moments slot.
    // 64 cells/probe,8 uint32/cell: H/E/X counts, observation stamp,
    // last H/E/X/sample stamps. Stamps=UBO FrameIndex+1; zero=never.
    rhi::ResourceHandle GetDepthProvenanceBufferBySlot(u32 slot) const {
        return depth_provenance_buffers_[slot];
    }
    const DDGIRuntimeParams& GetParams() const { return params_; }
    const DDGIVolumeData& GetVolumeData() const { return volume_data_; }
    // Read-only diagnostic source. Absolute frame clock, same slot as
    // Finalize binding1; callers must copy to Readback and wait before Map.
    rhi::ResourceHandle GetVolumeUniformBuffer(u32 frame) const {
        return volume_cb_[DDGISlotsForFrame(frame).currentIndex];
    }
    // CPU scheduler diagnostic: a zero value also represents a probe that
    // has not been selected since initialization. No GPU synchronization.
    u32 GetProbeLastUpdateFrame(u32 probe_index) const {
        return probe_index < probe_states_.size()
            ? probe_states_[probe_index].last_update_frame : 0u;
    }
    // First-class probe position buffer (float4 xyz = world pos, one per
    // probe, grid lattice order) — filled ONCE in Initialize, before the
    // render loop (the grid is static; UpdateProbeOrigin early-returns).
    // Single source for the debug sphere visualization; per-probe relocation
    // results will be cached here instead of re-derived per ray.
    rhi::ResourceHandle GetProbePositionBuffer() const { return probe_positions_; }
    // B1 L2: confidence buffer (per probe float4 = rayHitRatio, temporalStability,
    // visibilityConf, convergenceAge) — CPU readback for the probe inspector.
    rhi::RHIDeviceBase* GetDevice() const { return device_; }
    rhi::ResourceHandle GetIrradianceBufferLatest() const {
        return irradiance_buffers_[volume_data_.FrameIndex % 3];
    }

    // T8.9b diagnostic: full probe×ray radiance buffer for CPU-side A/B of
    // which hit-radiance path (SC atlas / prev-screen / analytic) actually
    // fires per ray (DDGI_PATH_DEBUG build tags the path in the distance
    // fraction). Read-only handle; consumers stage-copy off the render queue.
    rhi::ResourceHandle GetRayDataBuffer() const { return ray_data_buffer_; }
    // Same update-slot × ray indexing as DDGITraceFinalize's hit-normal input.
    // Diagnostic consumers must stage-copy it after the trace has completed.
    rhi::ResourceHandle GetHitNormalBuffer() const { return hit_normal_buffer_; }
    // Optional Vulkan DDGI_FIRST_SURFACE=1 candidate.  Its geometry and audit
    // buffers are deliberately independent of the existing hit-normal/source
    // audit allocation, so the default 13-binding trace remains unchanged.
    bool SetFirstSurfaceGeometry(const void* nodes, u64 nodeBytes,
                                 const void* triangles, u64 triangleBytes);
    bool IsFirstSurfaceEnabled() const { return first_surface_enabled_; }
    rhi::ResourceHandle GetFirstSurfaceNodesBuffer() const {
        return first_surface_nodes_buffer_;
    }
    rhi::ResourceHandle GetFirstSurfaceTrianglesBuffer() const {
        return first_surface_triangles_buffer_;
    }
    rhi::ResourceHandle GetFirstSurfaceAuditBuffer() const {
        return first_surface_audit_buffer_;
    }
    // Vulkan-only DDGI_FIRST_ZERO_CROSS=1 candidate.  It reuses the bounded
    // first-surface BVH/audit buffers, but its TraceSDF module can replace a
    // distance only for an incumbent signed zero crossing.  The default path
    // and the full DDGI_FIRST_SURFACE candidate remain separate.
    bool IsFirstZeroCrossRequested() const { return first_zero_cross_requested_; }
    bool IsFirstZeroCrossEnabled() const { return first_zero_cross_enabled_; }
    // Vulkan-only DDGI_FIRST_NATIVE_NORMAL=1 candidate.  The four geometry
    // buffers and the normal texture/sampler are borrowed from the native
    // GPUDrivenDraw path; only the original-triangle mapping and per-ray audit
    // buffers are owned here.  This setter is deliberately separate from
    // SetFirstSurfaceGeometry so a deferred Lumen rebuild can rebind the exact
    // live native resources without changing the incumbent 13-binding trace.
    bool SetFirstNativeNormalSources(
        rhi::ResourceHandle positions,
        rhi::ResourceHandle elements,
        rhi::ResourceHandle instances,
        rhi::ResourceHandle materials,
        rhi::ResourceHandle normalTexture,
        rhi::SamplerHandle normalSampler,
        const void* triangleMapping,
        u64 triangleMappingBytes);
    bool IsFirstNativeNormalEnabled() const { return first_native_normal_enabled_; }
    rhi::ResourceHandle GetFirstNativeNormalAuditBuffer() const {
        return first_native_normal_audit_buffer_;
    }
    u64 GetFirstNativeNormalAuditBytes() const {
        return first_native_normal_audit_bytes_;
    }

    // Vulkan-only analytic fallback rho candidate.  This is deliberately
    // independent of DDGI_FIRST_SURFACE/FIRST_NATIVE_NORMAL: it reuses the
    // same finalized hit ray, but only overrides the material rho for the
    // existing analytic source-11/source-14 fallback.  Source handles are
    // borrowed from GPUDrivenDraw; the pass owns the BVH/mapping/audit SSBOs.
    bool SetAnalyticFirstRhoSources(
        const void* nodes, u64 nodeBytes,
        const void* triangles, u64 triangleBytes,
        rhi::ResourceHandle positions,
        rhi::ResourceHandle elements,
        rhi::ResourceHandle instances,
        rhi::ResourceHandle materials,
        rhi::ResourceHandle albedoTexture,
        rhi::SamplerHandle albedoSampler,
        const void* triangleMapping, u64 triangleMappingBytes);
    bool IsAnalyticFirstRhoRequested() const { return analytic_first_rho_requested_; }
    bool IsAnalyticFirstRhoEnabled() const { return analytic_first_rho_enabled_; }
    bool IsAnalyticFirstRhoApplied() const { return analytic_first_rho_apply_; }
    bool IsAnalyticFirstPositionApplied() const { return analytic_first_position_apply_; }
    rhi::ResourceHandle GetAnalyticFirstRhoNodesBuffer() const {
        return analytic_first_rho_nodes_buffer_;
    }
    rhi::ResourceHandle GetAnalyticFirstRhoTrianglesBuffer() const {
        return analytic_first_rho_triangles_buffer_;
    }
    rhi::ResourceHandle GetAnalyticFirstRhoMappingBuffer() const {
        return analytic_first_rho_mapping_buffer_;
    }
    rhi::ResourceHandle GetAnalyticFirstRhoAuditBuffer() const {
        return analytic_first_rho_audit_buffer_;
    }
    u64 GetAnalyticFirstRhoAuditBytes() const {
        return analytic_first_rho_audit_bytes_;
    }
    rhi::ResourceHandle GetAnalyticFirstRhoPositions() const {
        return analytic_first_rho_positions_;
    }
    rhi::ResourceHandle GetAnalyticFirstRhoElements() const {
        return analytic_first_rho_elements_;
    }
    rhi::ResourceHandle GetAnalyticFirstRhoInstances() const {
        return analytic_first_rho_instances_;
    }
    rhi::ResourceHandle GetAnalyticFirstRhoMaterials() const {
        return analytic_first_rho_materials_;
    }
    rhi::ResourceHandle GetAnalyticFirstRhoAlbedoTexture() const {
        return analytic_first_rho_albedo_texture_;
    }
    rhi::SamplerHandle GetAnalyticFirstRhoAlbedoSampler() const {
        return analytic_first_rho_albedo_sampler_;
    }
    rhi::ResourceHandle GetFirstNativeNormalTriangleMappingBuffer() const {
        return first_native_normal_triangle_mapping_buffer_;
    }
    rhi::ResourceHandle GetFirstNativeNormalPositions() const {
        return first_native_normal_positions_;
    }
    rhi::ResourceHandle GetFirstNativeNormalElements() const {
        return first_native_normal_elements_;
    }
    rhi::ResourceHandle GetFirstNativeNormalInstances() const {
        return first_native_normal_instances_;
    }
    rhi::ResourceHandle GetFirstNativeNormalMaterials() const {
        return first_native_normal_materials_;
    }
    rhi::ResourceHandle GetFirstNativeNormalTexture() const {
        return first_native_normal_texture_;
    }
    rhi::SamplerHandle GetFirstNativeNormalSampler() const {
        return first_native_normal_sampler_;
    }
    // Vulkan-only DDGI_QUERY_SOURCE_AUDIT=1.  This is a read-only diagnostic
    // tail appended after the existing 9N hit-normal/source-audit records.
    bool IsQuerySourceAuditEnabled() const { return query_source_audit_enabled_; }
    // 诊断：hit_distance_buffer 直读（ray0/1 被 ddgi_trace_sdf 写入
    // 探针原点/重定位后的原始 SDF 距离——级联外为 5555 哨兵）。
    rhi::ResourceHandle GetHitDistanceBuffer() const { return hit_distance_buffer_; }
    // 2026-09-17 P0：重定位后有效起点缓冲（契约见成员注释）。调试可视化
    // 与 gather 经此取同一份数据。
    rhi::ResourceHandle GetRelocatedOriginsBuffer() const { return relocated_origins_; }
    // 审计 §8.3 调试可视化用：一次性同步读回 relocated origins 到 CPU
    //（probeIdx 索引，xyz=有效起点，w=分类）。第 60 帧前返回 nullptr（缓冲
    // 尚未被稳态写入）。version 随读回次数递增，供消费方触发重建。
    const math::v4* GetRelocatedOriginsCpu(u32& count, u32& version);
    // DDGI_ORIGIN_STATS 一次性读回的 per-instance 守卫（近/远两个 pass 实例
    // 各自 dump 一次；函数级 static 会被先到 60 帧的实例独占）。
    u32  origin_stats_tick_{ 0 };
    bool origin_stats_dumped_{ false };
    // 诊断：SC grid header 读回（验证 grid 覆盖范围/卡片数）
    rhi::ResourceHandle GetSCGridOffsetsBuffer() const { return sc_grid_offsets_buffer_; }
    rhi::ResourceHandle GetSCGridCardsBuffer() const { return sc_grid_cards_buffer_; }
    rhi::ResourceHandle GetSCCardDataBuffer() const { return sc_card_data_buffer_; }

    // T8.9b diagnostic: probes refreshed by the latest trace dispatch, so
    // CPU-side ray-buffer reads can skip stale/unwritten slots.
    void SetMeasuredGeometrySource(rhi::ResourceHandle top, rhi::ResourceHandle bottom,
                                   rhi::ResourceHandle triangles, rhi::ResourceHandle instances, bool ownerDirectoryReady=false,
                                   u64 sceneEpoch=0, u64 sourceDigest=0) {
        measured_owner_directory_ready_=ownerDirectoryReady;
        measured_geometry_epoch_=sceneEpoch; measured_geometry_digest_=sourceDigest;
        measured_geometry_[0]=top; measured_geometry_[1]=bottom;
        measured_geometry_[2]=triangles; measured_geometry_[3]=instances;
    }
    rhi::ResourceHandle GetMeasuredGeometrySource(u32 i) const {
        return i<4?measured_geometry_[i]:rhi::handles::INVALID_RESOURCE;
    }
    bool UsesMeasuredVisibility() const { return ray_sample_mode_ == 7u; }

    u32 GetLastUpdateCount() const { return last_recorded_update_count_; }

    // X2/A3: 统一 Cache View —— 消费者（SPGI 回退、GIGather 远场）从这里取
    // volume 数据快照，不再逐个摸 pass 内部 getter。irradiance 取
    // frame_idx 派生槽位（DDGISlotMap.h，语义见 GetIrradianceBuffer）；
    // origin/counts/spacing 与旧上传字段逐一对应。
    DDGIVolumeView GetVolumeView(u32 frame_idx) const {
        DDGIVolumeView view;
        view.measured_visibility = UsesMeasuredVisibility();
        view.irradiance = irradiance_buffers_[DDGISlotsForFrame(frame_idx).currentIndex];
        view.depth = depth_buffers_[DDGISlotsForFrame(frame_idx).currentIndex];
        view.origin = math::v3{ volume_data_.ProbeOrigin.x,
                                volume_data_.ProbeOrigin.y,
                                volume_data_.ProbeOrigin.z };
        view.counts = math::v3{ static_cast<float>(params_.probe_count_x),
                                static_cast<float>(params_.probe_count_y),
                                static_cast<float>(params_.probe_count_z) };
        view.spacing = params_.probe_spacing;
        view.relocated_origins = relocated_origins_;
        // GI 能量根治方案 Phase 3.1: 每探针 openSky 统计（逃逸占比，EMA）。
        view.opensky = opensky_buffer_;
        return view;
    }

    // GI 能量根治方案 Phase 3.2: SC 卡片 texel gather 的天空分类数据源。
    rhi::ResourceHandle GetOpenSkyBuffer() const { return opensky_buffer_; }

    // 槽位显式版（Phase 1d）：与 GetVolumeView 相同，irradiance 槽由调用端
    // 传入 DDGISlotsForFrame(frame) 的具名槽（previousIndex/currentIndex）。
    DDGIVolumeView GetVolumeViewBySlot(u32 slot) const {
        DDGIVolumeView view;
        view.measured_visibility = UsesMeasuredVisibility();
        view.irradiance = irradiance_buffers_[slot];
        view.depth = depth_buffers_[slot];
        view.origin = math::v3{ volume_data_.ProbeOrigin.x,
                                volume_data_.ProbeOrigin.y,
                                volume_data_.ProbeOrigin.z };
        view.counts = math::v3{ static_cast<float>(params_.probe_count_x),
                                static_cast<float>(params_.probe_count_y),
                                static_cast<float>(params_.probe_count_z) };
        view.spacing = params_.probe_spacing;
        view.relocated_origins = relocated_origins_;
        view.opensky = opensky_buffer_;
        return view;
    }

    // X2/A3: 时域策略映射 —— EMA 即现有 irradiance/depth EMA（默认，行为
    // 不变）；EMAVarianceClamp/Reprojection 暂等同 EMA（TODO X3 落地完整实现）。
    void SetTemporalStrategy(GITemporalStrategy strategy);
    GITemporalStrategy GetTemporalStrategy() const { return temporal_strategy_; }

    // T4.1: runtime probe-update budget (GIAdaptiveGovernor). Applied at the
    // next AddPass — CPU-side only, no reallocation.
    // A5: write the update-loop member directly — the loop reads
    // max_probes_per_frame_ (frozen from params_ at Initialize), so a
    // params_-only write never reached it.
    void SetMaxProbesPerFrame(u32 n) {
        if (n > 0) {
            params_.max_probes_per_frame = n;
            max_probes_per_frame_ = n;
        }
    }

    // Accessors for Surface Cache → DDGI integration
    rhi::ResourceHandle GetProbeUpdateListBuffer(u32 frame_idx = 0) const {
        return probe_update_list_buffers_[DDGISlotsForFrame(frame_idx).writeIndex];
    }

    // Update probe origin to follow camera (grid-snapped).
    // Returns true if the grid actually shifted this frame.
    bool UpdateProbeOrigin(const math::v3& camera_position);

    // Set static probe volume for initialization from bake data
    void SetStaticProbeVolume(StaticProbeVolume* volume) { static_volume_ = volume; }

    // Mode 11 (MeshletDynamicDDGI): when true, runtime TraceRays/UpdateIrradiance/
    // UpdateDepth run every frame even when a static_volume_ is loaded. The static
    // data still seeds the history buffers (InitializeProbesFromStatic copies it
    // into all 3 triple-buffered irradiance_buffers_), then runtime traces EMA-
    // blend toward canonical dynamic DDGI. Mode 10 leaves this false to preserve
    // the static bake indefinitely.
    void SetDynamicMode(bool enabled) { dynamic_mode_ = enabled; }

    // T4.4: tag prepended to all RenderGraph import names and the pass name.
    // Required when two volumes live in one graph (near + far coarse layer) —
    // without it both instances import different physical buffers under the
    // same "LumenDDGI_Irradiance_N" name and the graph aliases them.
    void SetRGNamePrefix(const std::string& prefix) { rg_name_prefix_ = prefix; }

    // GI 能量根治方案 Phase 3.3: 远场 volume 天空档位（vi>0 时设 0.5——
    // 远场只表达低频环境，逃逸比例天然高，全额天空会成为封闭区隐性泵浦）。
    void SetFarSkyScale(float s) { far_sky_scale_ = s; }

    // GI 能量根治方案 Phase 4: 离线场为有符号编码时置 true——trace 端启用
    // 零穿越命中判定（sign change + 5 次二分），根治无符号场 thin-wall
    // tunneling（R6/T8.10f 正解的运行时半）。
    void SetCoarseSdfSigned(bool s) { coarse_sdf_signed_ = s; }
    void SetOfflineHitEvidence(u32 mode) { offline_hit_evidence_ = std::min(mode,3u); }

    // Live-tunable debug params (WASM sidebar). Indices 4..6 of DawnDebugParams
    // map to SkyColor intensity, Albedo intensity, ProbeHysteresis. Values are
    // consumed in AddPass when uploading DDGIVolumeData to the GPU.
    // T4.5: legacy WASM sidebar bridge — routes into the unified GIDebug()
    // knobs (three-backend single source). Kept for API compatibility.
    void SetDawnDebugParams(const DawnDebugParams& params) {
        dawnDebugParams_ = params;
        GIDebug().ApplyLegacy(params);
    }

    // Accessor for confidence buffer (for Task 5 later).
    // Single buffer: convergenceAge is a monotonic counter accumulated by
    // read-modify-write in DDGIUpdateIrradiance. The old triple-buffered
    // array rotated the written slot per frame, so each slot only advanced
    // every 3rd frame (3x slower ramp) while GIGather read frame%3 —
    // effectively never seeing a settled probe.
    rhi::ResourceHandle GetConfidenceBuffer(u32 frame_idx = 0) const {
        (void)frame_idx;
        return confidence_buffer_;
    }

    // Set Surface Cache resources for view-independent radiance in TraceRays.
    // Must be called before AddPass when Surface Cache is active. The grid
    // pair (T1.3) is optional — Vulkan finalize uses it for O(1) card lookup;
    // without it the buffer handles stay INVALID and shaders fall back to the
    // 0xFFFFFFFF cell sentinel (no SC card sampling).
    //
    // X2/A3: 视图签名（推荐）—— 从 SurfaceCachePass::GetCacheView() 直传。
    void SetSurfaceCacheResources(const SurfaceCacheView& view);

    // DEPRECATED (X2/A3): 旧 7 参签名保留为转发适配器，等价映射进视图签名，
    // 避免破坏未知调用点（TestNaniteStreamingPipeline 等）。
    void SetSurfaceCacheResources(
        rhi::ResourceHandle lighting_atlas,
        rhi::ResourceHandle card_lookup_buffer,
        rhi::ResourceHandle card_data_buffer,
        u32 atlas_size,
        u32 lookup_count,
        rhi::ResourceHandle grid_offsets_buffer = rhi::handles::INVALID_RESOURCE,
        rhi::ResourceHandle grid_cards_buffer = rhi::handles::INVALID_RESOURCE) {
        SurfaceCacheView view;
        view.lighting_atlas = lighting_atlas;
        view.card_lookup = card_lookup_buffer;
        view.card_data = card_data_buffer;
        view.grid_offsets = grid_offsets_buffer;
        view.grid_cards = grid_cards_buffer;
        view.atlas_size = atlas_size;
        view.lookup_count = lookup_count;
        SetSurfaceCacheResources(view);
    }

    // Clear Surface Cache resources — call when leaving SC mode to prevent
    // DDGI finalize from reading stale SC card data and racing with the
    // irradiance update pass on irradiance_buffers_.
    void ClearSurfaceCacheResources();

    // --- Offline SDF data source ---
    // When set, DDGI trace uses this pre-built high-resolution SDF volume
    // instead of the runtime GlobalSDF cascades. The volume stores unsigned
    // distances; traceSDF uses adaptive first-hit marching.
    void SetOfflineSDFSource(rhi::ResourceHandle texture,
                             math::v3 origin, math::v3 extent, u32 resolution) {
        offline_sdf_texture_ = texture;
        offline_sdf_origin_ = origin;
        offline_sdf_extent_ = extent;
        offline_sdf_resolution_ = resolution;
        has_offline_sdf_ = (texture != rhi::handles::INVALID_RESOURCE && resolution > 0);
    }

    // --- T4.3 per-mesh detail SDF slots (PRIMAL_DDGI_DETAIL_SDF=1) ---
    // First slice of detail tracing: up to 4 instances whose per-mesh SDF
    // textures (RHIGpuMesh, R16_UNorm) intersect the DDGI volume get bound
    // to the split SDF trace. At a coarse GlobalSDF hit inside a slot's
    // world AABB the trace refines t against the finer mesh field and
    // derives a gradient normal (6-tap central difference) that finalize
    // uses instead of the -rayDir fake. count=0 → trace writes zero
    // normals and finalize keeps current behavior (bit-identical default).
    //
    // Layout contract (std140 UBO / Metal constant buffer, mirror in
    // DDGITraceSDF.comp + DDGITraceRays.metal):
    //   header      : vec4 (x = active slot count, yzw pad)
    //   inv_world[4]: mat4 world → mesh-local
    //   normal_mtx[4]: vec4[3] rows of transpose(inverse(world 3x3))
    //   bounds_min[4]/bounds_max[4]: vec4 world AABB of the mesh SDF bounds
    //   params[4]   : vec4 (resolution, max_dim, signed, voxel_size)
    static constexpr u32 kDetailSDFSlots = 4;
    struct DetailSDFSlot {
        math::m4x4 inv_world;
        math::v4   normal_matrix[3];
        math::v4   bounds_min;
        math::v4   bounds_max;
        math::v4   params;          // x=resolution, y=max_dim, z=signed, w=voxel_size
    };
    void SetDetailSDFSlots(const rhi::ResourceHandle* textures,
                           const DetailSDFSlot* slots, u32 count) {
        detail_sdf_slot_count_ = (count > kDetailSDFSlots) ? kDetailSDFSlots : count;
        for (u32 i = 0; i < detail_sdf_slot_count_; ++i) {
            detail_sdf_textures_[i] = textures[i];
            detail_sdf_slots_[i] = slots[i];
        }
    }
    // World AABB of the probe volume (probe origin/counts/spacing + 1 cell
    // margin) — the region detail-SDF refinement can affect.
    void GetVolumeAABB(math::v3& out_min, math::v3& out_max) const;

    // M2 acceptance (light-response half-life): override the trace light
    // color the probes see, WITHOUT the UpdateSettings pass rebuild — the
    // rebuild resets the probe field and would measure re-init, not the
    // SC↔probe convergence the gate wants. Cleared explicitly.
    void SetProbeLightOverride(const math::v4& color) {
        probe_light_override_ = color;
        probe_light_override_active_ = true;
    }
    void ClearProbeLightOverride() { probe_light_override_active_ = false; }

private:
    void CreateDescriptorSetLayouts();
    void CreatePipelines();
    void CreateConstantBuffers();
    // T1.3 (C5): async variance readback ring (3 slots x probeCount*128 floats)
    void CreateDepthReadbackRing();
    bool CreateFieldResetTemplate();
    void RecordFieldReset(rhi::RHICommandBuffer* cmd);
    void InheritProbeHistory(rhi::RHICommandBuffer* cmd, u32 out, u32 history);
    void PublishFarUpdateAudit(rhi::RHICommandBuffer* cmd, u32 slot);
    FarUpdatePolicy far_update_policy_;
    FarUpdateAudit far_update_audit_{};
    u32 last_recorded_update_count_=0;
    rhi::ResourceHandle field_reset_template_=rhi::handles::INVALID_RESOURCE;
    u64 reset_zero_bytes_=0, reset_depth_offset_=0, reset_confidence_offset_=0;
    bool field_reset_pending_=false;

    void CreateProbeTextures();
    void InitializeProbesFromStatic();

    bool              initialized_{ false };
    rhi::RHIDeviceBase* device_{ nullptr };
    DDGIRuntimeParams params_{};
    DDGIVolumeData    volume_data_{};
    math::v3          probe_origin_ = math::splat3(0.0f);
    math::v3          last_snapped_cam_ = math::splat3(0.0f);  // last camera position snapped to grid
    int               relocation_shift_[3]{ 0, 0, 0 };  // current frame grid shift (probe cells)
    u32               ray_sample_mode_{ 0 };            // DDGI_RAY_SAMPLE env: 0 uniform / 1 jitter / 2 cosine
    rhi::ResourceHandle probe_positions_{ rhi::handles::INVALID_RESOURCE };  // float4 per probe, filled once at Initialize
    GITemporalStrategy temporal_strategy_ = GITemporalStrategy::EMA;   // X2/A3 枚举存储（EMA=现状）

    // Compute pipelines (3 sub-passes)
    rhi::PipelineHandle trace_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineHandle irradiance_pipeline_{ rhi::handles::INVALID_PIPELINE };
    std::unique_ptr<DDGIShProducerAudit> sh_producer_audit_;
    rhi::PipelineHandle depth_pipeline_{ rhi::handles::INVALID_PIPELINE };

    // Split trace pipelines (Apple Silicon: separate texture3D from buffer reads)
    rhi::PipelineHandle sdf_trace_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineHandle finalize_pipeline_{ rhi::handles::INVALID_PIPELINE };
    // Vulkan-only first-surface variant.  It has a separate descriptor layout
    // because the incumbent DDGITraceSDF contract remains 13 bindings.
    rhi::PipelineHandle first_surface_sdf_trace_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::ShaderHandle first_surface_sdf_trace_shader_{ rhi::handles::INVALID_SHADER };
    // Vulkan-only zero-cross distance child.  It shares the 16-binding
    // first-surface descriptor contract, but has an independent SPIR-V module
    // and pipeline so the incumbent FIRST_SURFACE path is not mutated.
    rhi::PipelineHandle first_zero_cross_sdf_trace_pipeline_{
        rhi::handles::INVALID_PIPELINE };
    rhi::ShaderHandle first_zero_cross_sdf_trace_shader_{
        rhi::handles::INVALID_SHADER };
    // Vulkan-only native first-hit normal candidate.  It extends the
    // first-surface descriptor contract to bindings 16..23 and is never
    // selected when the exact opt-in flag is absent.
    rhi::PipelineHandle first_native_normal_sdf_trace_pipeline_{
        rhi::handles::INVALID_PIPELINE };
    rhi::ShaderHandle first_native_normal_sdf_trace_shader_{
        rhi::handles::INVALID_SHADER };
    // Vulkan-only analytic fallback rho variant.  The incumbent Finalize
    // module/layout remain untouched; this variant is created only when the
    // exact candidate flag or diagnostic prefix is present.
    rhi::PipelineHandle analytic_first_rho_finalize_pipeline_{
        rhi::handles::INVALID_PIPELINE };
    rhi::ShaderHandle analytic_first_rho_finalize_shader_{
        rhi::handles::INVALID_SHADER };

    // Pipeline layouts
    rhi::PipelineLayoutHandle trace_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineLayoutHandle irradiance_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineLayoutHandle depth_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineLayoutHandle sdf_trace_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineLayoutHandle finalize_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineLayoutHandle first_surface_sdf_trace_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineLayoutHandle first_native_normal_sdf_trace_layout_{
        rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineLayoutHandle analytic_first_rho_finalize_layout_{
        rhi::handles::INVALID_PIPELINE_LAYOUT };

    // Descriptor set layouts
    rhi::DescriptorSetLayoutHandle trace_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetLayoutHandle irradiance_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetLayoutHandle depth_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetLayoutHandle sdf_trace_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetLayoutHandle finalize_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetLayoutHandle first_surface_sdf_trace_set_layout_{
        rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetLayoutHandle first_native_normal_sdf_trace_set_layout_{
        rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetLayoutHandle analytic_first_rho_finalize_set_layout_{
        rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };

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
    rhi::DescriptorSetHandle sdf_trace_ds_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET
    };
    rhi::DescriptorSetHandle first_surface_sdf_trace_ds_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET
    };
    rhi::DescriptorSetHandle first_native_normal_sdf_trace_ds_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET
    };
    rhi::DescriptorSetHandle finalize_ds_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET
    };
    rhi::DescriptorSetHandle analytic_first_rho_finalize_ds_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET
    };

    // Constant buffers (triple-buffered)
    rhi::ResourceHandle global_cb_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };
    rhi::ResourceHandle volume_cb_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };

    // Probe irradiance buffers (persistent, triple-buffered for history)
    // UNITS: Radiance-SH（9×float4 per probe，radiance 投影，mcWeight=4π/N；
    // 消费端评基单源 EvalLib.glsl——评出 E/π 后须 ×π 才是 E，见审计文档 §5）。
    // Concurrency contract (audited 2026-09-10): with outIdx = frame % 3 and
    // histIdx = (frame + 2) % 3, frame N writes slot N%3 while frames N-1/N-2
    // read slots (N-1)%3/(N-2)%3 and frame N-2 reads slot N%3 as history.
    // This is race-free because MAX_FRAMES_IN_FLIGHT = 3 (RHITypes.h) plus the
    // BeginFrame fence wait on frame N-3 (RenderSystem) guarantees the last
    // same-slot WRITER (frame N-3) is complete, and single-queue submission
    // order guarantees the in-flight readers' commands execute before this
    // frame's writes. Do NOT shrink the fence wait without re-auditing.
    rhi::ResourceHandle irradiance_buffers_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };
    // Probe depth buffers (buffer-based, 16 floats per probe: 8 mean + 8 variance)
    rhi::ResourceHandle depth_buffers_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };
    bool depth_provenance_enabled_{false}; // Vulkan diagnostic, allocation latched.
    rhi::ResourceHandle depth_provenance_buffers_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };

    // Ray data storage buffer (single, reused each frame)
    // UNITS: Radiance（逐射线命中 radiance L；SC 采样/prev-screen/解析回退
    // 三源在此汇合——三者均为 radiance 量纲，直用合法，见审计文档 §5 铁律 2）。
    rhi::ResourceHandle ray_data_buffer_{ rhi::handles::INVALID_RESOURCE };

    // Hit distance buffer (intermediate between split trace dispatches)
    rhi::ResourceHandle hit_distance_buffer_{ rhi::handles::INVALID_RESOURCE };

    // T4.3: per-ray refined hit normals (xyz, w>0.5 = valid), written by the
    // split SDF trace (zeros when no detail slots), consumed by finalize.
    bool analytic_audit_enabled_{false}; // Latched with allocation; never enable an unallocated tail.
    // Vulkan-only DDGI_RAY_SOURCE_AUDIT=1.  When latched this also enables the
    // existing analytic audit records; the source records live after the
    // unchanged 5*full-ray-capacity normal/analytic region.
    bool ray_source_audit_enabled_{false};
    // Vulkan-only card-query source audit.  Allocation is latched together
    // with ray_source_audit_enabled_; the extra 18N records are appended after
    // the unchanged N/5N/9N regions and are never bound in the default path.
    bool query_source_audit_enabled_{false};
    bool material_projection_enabled_{false}; // Vulkan evidence experiment, allocation-latched.
    bool measured_geometry_requested_{false};
    u64 measured_geometry_epoch_{0}, measured_geometry_digest_{0}; // CPU publication identity for diagnostics.
    bool measured_owner_directory_ready_{false};
    rhi::ResourceHandle measured_geometry_[4]{rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
                                             rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE};
    bool first_surface_requested_{false};
    bool first_surface_enabled_{false};
    bool first_zero_cross_requested_{false};
    bool first_zero_cross_enabled_{false};
    bool first_native_normal_requested_{false};
    bool first_native_normal_enabled_{false};
    rhi::ResourceHandle first_surface_nodes_buffer_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle first_surface_triangles_buffer_{ rhi::handles::INVALID_RESOURCE };
    // Three vec4 records per full-capacity ray: old/new/first+status,
    // triangle/flags/residual/frame+probe stamp, GPU barycentric+ray id.
    rhi::ResourceHandle first_surface_audit_buffer_{ rhi::handles::INVALID_RESOURCE };
    // Borrowed native sources.  These are refreshed by the pipeline when the
    // current scene's GPUDrivenDraw resources are ready and are never destroyed
    // by this pass.
    rhi::ResourceHandle first_native_normal_positions_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle first_native_normal_elements_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle first_native_normal_instances_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle first_native_normal_materials_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle first_native_normal_texture_{ rhi::handles::INVALID_RESOURCE };
    rhi::SamplerHandle first_native_normal_sampler_{ rhi::handles::INVALID_SAMPLER };
    rhi::ResourceHandle first_native_normal_triangle_mapping_buffer_{
        rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle first_native_normal_audit_buffer_{
        rhi::handles::INVALID_RESOURCE };
    u64 first_native_normal_audit_bytes_{ 0 };
    // Candidate request is latched before descriptor creation.  A nonempty
    // PREFIX creates/binds the exact module with application disabled, which
    // gives the control run the same descriptor/variant proof.
    bool analytic_first_rho_requested_{false};
    bool analytic_first_rho_enabled_{false};
    bool analytic_first_rho_apply_{false};
    bool analytic_first_position_apply_{false};
    bool analytic_first_rho_audit_{false};
    bool analytic_sc_first_audit_{false};
    bool analytic_first_rho_prefix_{false};
    rhi::ResourceHandle analytic_first_rho_nodes_buffer_{
        rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle analytic_first_rho_triangles_buffer_{
        rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle analytic_first_rho_mapping_buffer_{
        rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle analytic_first_rho_audit_buffer_{
        rhi::handles::INVALID_RESOURCE };
    u64 analytic_first_rho_audit_bytes_{0};
    rhi::ResourceHandle analytic_first_rho_positions_{
        rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle analytic_first_rho_elements_{
        rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle analytic_first_rho_instances_{
        rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle analytic_first_rho_materials_{
        rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle analytic_first_rho_albedo_texture_{
        rhi::handles::INVALID_RESOURCE };
    rhi::SamplerHandle analytic_first_rho_albedo_sampler_{
        rhi::handles::INVALID_SAMPLER };
    rhi::ResourceHandle hit_normal_buffer_{
        rhi::handles::INVALID_RESOURCE
    };
    // 2026-09-17 P0：重定位后有效射线起点（float4 per probe，绝对 probeIdx 索引）
    rhi::ResourceHandle relocated_origins_{
        rhi::handles::INVALID_RESOURCE
    };
    // §8.3 调试可视化：一次性 CPU 读回产物（GetRelocatedOriginsCpu 填充）。
    std::vector<math::v4> relocated_origins_cpu_;
    bool relocated_origins_cpu_done_{ false };
    u32  relocated_origins_cpu_version_{ 0 };
    u32  exec_frame_index_{ 0 };   // AddPass 参数帧号（CPU 读回帧门用）
    // T4.3: detail-SDF slot table UBO (triple-buffered) + slot textures +
    // dummy resources that keep the appended bindings valid when the feature
    // is off (PRIMAL_DDGI_DETAIL_SDF unset → slot count 0 → shader no-op).
    bool detail_sdf_enabled_{ false };
    u32  detail_sdf_slot_count_{ 0 };
    DetailSDFSlot detail_sdf_slots_[kDetailSDFSlots]{};
    rhi::ResourceHandle detail_sdf_textures_[kDetailSDFSlots]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };
    rhi::ResourceHandle detail_table_cb_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };
    // Unused detail slots bind the (always-valid) SDF cascade texture — no
    // dummy texture needed.

    // M2: probe-light override (light-response half-life gate).
    math::v4 probe_light_override_{};
    bool probe_light_override_active_{ false };
    // Phase 5a: 事件驱动的年轻档 α 窗（灯光修改后 30 帧 TemporalAlpha=0.35）。
    u32 alpha_hold_until_frame_{ 0 };

    // Finalize screen-space sampling: current/radiance VP, inverse matrices
    // and depth validity (272B per slot). Metal reads the leading current VP.
    rhi::ResourceHandle finalize_vp_cb_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };
    math::m4x4 current_view_projection_{};

    // Probe state tracking (importance-based partial update)
    std::vector<DDGIProbeState> probe_states_;
    // Triple-buffered: CPU writes frameIdx's copy while GPU reads previous frames' copies.
    // Single-buffer caused an intermittent CPU-GPU race — CPU overwrote the update list
    // while GPU threads were still reading it from the prior frame's dispatch, corrupting
    // probe index lookups and producing screen-filling solid color in Mode 10.
    rhi::ResourceHandle probe_update_list_buffers_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };
    u32 max_probes_per_frame_ = 256;
    u32 probe_update_offset_ = 0;  // round-robin offset for partial updates

    // T1.3 (C5): async readback ring for probe-depth variance scheduling —
    // replaces the per-frame MapBuffer of the GPU-hot depth history.
    VarianceFenceTicket variance_fence_ticket_;
    VarianceSceneIdentity variance_identity_;
    VarianceFenceAuditView variance_audit_{};
    u64 variance_generation_=0;

    const SurfaceCachePass* variance_scene_provider_=nullptr;
    bool variance_cpu_confirmed_=false,variance_identity_valid_=false;
    u64 variance_scene_epoch_=0,variance_source_digest_=0;
    rhi_readback_ring depth_readback_ring_;
    u32 depth_ring_copies_ = 0;    // copies issued; consumer waits for > 2
    u32 depth_ring_recorded_frame_[3]{~u32(0),~u32(0),~u32(0)}; // command recording only, not completion
    u64 depth_ring_recorded_native_[3]{~u64(0),~u64(0),~u64(0)};
    // T1.5 (R4): DispatchIndirect smoke consumer — probe-update dispatch count
    // (group count, platform-encoded) uploaded per frame; opt-in via
    // PRIMAL_DDGI_INDIRECT=1 so the default path stays CPU-computed dispatch.
    rhi::ResourceHandle probe_dispatch_count_buffer_{ rhi::handles::INVALID_RESOURCE };

    // Static probe volume reference (for initialization from bake data)
    StaticProbeVolume* static_volume_{nullptr};

    // Mode 11 flag — see SetDynamicMode comment. False = Mode 10 (static-bake only).
    bool dynamic_mode_{false};

    // T4.4: RenderGraph import-name tag for multi-volume graphs ("" = legacy names).
    std::string rg_name_prefix_;

    // Live-tunable debug params (WASM sidebar). Only indices 4..6 are read
    // here: SkyColor intensity, Albedo intensity, ProbeHysteresis. Applied
    // in AddPass when uploading DDGIVolumeData.
    DawnDebugParams dawnDebugParams_{};

    // Confidence buffer (single-buffered — see GetConfidenceBuffer note).
    // Written read-modify-write by DDGIUpdateIrradiance, read by GIGather
    // later the same frame; the pre-existing same-frame write→read ordering
    // through separate passes applies unchanged.
    rhi::ResourceHandle confidence_buffer_{
        rhi::handles::INVALID_RESOURCE
    };

    // GI 能量根治方案 Phase 3.1: 每探针 openSky 统计（float/probe，逃逸射线
    // 占比，EMA 平滑）。单缓冲持久（探针逐批更新，未更新帧保留旧值），
    // 写入 DDGIUpdateIrradiance，读取 GIGather / SurfaceCacheLightEval 的
    // 天空可见性分类。
    rhi::ResourceHandle opensky_buffer_{
        rhi::handles::INVALID_RESOURCE
    };
    // 远场天空档位（Phase 3.3），默认 1.0 = 近场行为。
    float far_sky_scale_{ 1.0f };
    // 有符号粗场旗标（Phase 4），经 ProbeCounts[3]（原 padding）进 shader。
    bool coarse_sdf_signed_{ false };

    // Surface Cache resources (set externally before AddPass)
    rhi::ResourceHandle sc_lighting_atlas_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle sc_card_lookup_buffer_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle sc_card_data_buffer_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle sc_depth_atlas_{ rhi::handles::INVALID_RESOURCE };   // T3.4 (D3)
    rhi::ResourceHandle sc_grid_offsets_buffer_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle sc_grid_cards_buffer_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle sc_params_buffer_{ rhi::handles::INVALID_RESOURCE };
    u32                 sc_atlas_size_ = 0;
    u32                 sc_lookup_count_ = 0;
    bool                sc_enabled_ = false;

    // Offline SDF data source (replaces GlobalSDF cascades when active)
    bool                has_offline_sdf_{ false };
    u32                 offline_hit_evidence_{0}; // Vulkan split trace only, explicit RG16F contract
    rhi::ResourceHandle offline_sdf_texture_{ rhi::handles::INVALID_RESOURCE };
    math::v3            offline_sdf_origin_{ 0, 0, 0 };
    math::v3            offline_sdf_extent_{ 0, 0, 0 };
    u32                 offline_sdf_resolution_{ 0 };
};

} // namespace primal::graphics::lumen
