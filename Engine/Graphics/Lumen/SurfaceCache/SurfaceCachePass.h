#pragma once
#include "BoundaryRadianceCache.h"
#include "BoundaryMixedIndex.h"

#include "CommonHeaders.h"
#include "Graphics/RHI/Core/RHITypes.h"
#include "Graphics/RenderGraph/RenderGraphDefinitions.h"
#include "SurfaceCacheTypes.h"
#include "CardGenerator.h"
#include "SurfaceStaticPages.h"
#include "BoundaryStaticSamples.h"
#include <memory>
#include "Graphics/Utilities/GeometryFirstHit.h"
#include "../LumenTypes.h"
#include "../GICacheView.h"

namespace primal::graphics::rhi {
    class RHIDeviceBase;
}

namespace primal::graphics::rendergraph {
    class RenderGraph;
}

namespace primal::graphics {
class RenderSceneSnapshot;
}

namespace primal::graphics::lumen {

// ============================================================================
// SurfaceCachePass — orchestrates all surface cache sub-passes
// ============================================================================

/**
 * @brief Self-contained surface cache pass: CardCapture -> DepthDilate -> LightCull -> LightEval -> IndirectTrace -> IndirectResolve.
 *
 * Lifecycle:
 *   1. Initialize(device, config)       -- once
 *   2. AddPass(graph, ..., frame_data)  -- every frame
 *   3. Shutdown()                        -- once
 *
 * The pass owns all persistent GPU resources:
 *   - Atlas textures (albedo, normal, depth, emissive, lighting)
 *   - Triple-buffered constant buffers and lighting atlas
 *   - Compute pipelines (stubs until later tasks)
 *   - Runtime buffers for light assignment, light info, ray hits
 */
class SurfaceCachePass {
public:
    SurfaceCachePass() = default;
    ~SurfaceCachePass();

    // TEMP diagnosis (T8.13 Metal sync): direct CPU read of the SC shadow
    // visibility buffer (Dynamic/shared memory). Returns -1 when unavailable.
    float DebugShadowVisibilityAt(u32 index) const;
    // T3.3 诊断：可见性缓冲统计（均值/零值占比）——直射项归零排查。
    void DebugShadowVisibilityStats() const;
    bool DebugShadowPipelineReady() const { return shadow_pipeline_ != rhi::handles::INVALID_PIPELINE; }

    // Vulkan opt-in, one-shot pre/post CardFill readback. Four R32/rgba8
    // atlas-sized segments: pre-depth, pre-albedo, post-depth, post-albedo.
    struct CardFillAuditView {
        rhi::ResourceHandle staging{rhi::handles::INVALID_RESOURCE};
        u32 frame = 0;
        u32 atlas_size = 0;
        rhi::ResourceHandle sdf_staging{rhi::handles::INVALID_RESOURCE};
        math::v3 sdf_origin{}, sdf_extent{};
        u32 sdf_resolution = 0, sdf_stride = 0, sdf_count = 0;
        bool offline_source = false;
        bool queued = false;
        // Optional actual binding10/11/7 snapshot, not CPU upload surrogates.
        rhi::ResourceHandle tables_staging{rhi::handles::INVALID_RESOURCE};
        rhi::ResourceHandle instance_source{rhi::handles::INVALID_RESOURCE};
        rhi::ResourceHandle material_source{rhi::handles::INVALID_RESOURCE};
        rhi::ResourceHandle albedo_source{rhi::handles::INVALID_RESOURCE};
        u64 instance_bytes=0, material_bytes=0;
        u32 albedo_width=0, albedo_height=0, albedo_layers=0;
    };
    CardFillAuditView GetCardFillAuditView() const { return card_fill_audit_; }
    const std::vector<SurfaceCacheCard>& GetCardFillAuditCards() const { return card_fill_audit_cards_; }

    // T8.13: offline merged SDF (384^3) for the per-texel shadow march —
    // the runtime cascades (1m/2m/4m) misplace SC shadow hits the same way
    // they do for the DDGI trace.
    void SetOfflineSDFSource(rhi::ResourceHandle texture,
                             math::v3 origin, math::v3 extent, u32 resolution) {
        sc_offline_sdf_texture_ = texture;
        sc_offline_sdf_origin_ = origin;
        sc_offline_sdf_extent_ = extent;
        sc_offline_sdf_resolution_ = resolution;
    }

    bool Initialize(rhi::RHIDeviceBase* device, const LumenConfig& config);
    void Shutdown();

    SurfaceCacheOutput AddPass(
        rendergraph::RenderGraph& graph,
        rendergraph::RGResourceHandle prev_frame_color,
        rhi::ResourceHandle light_data_buffer,
        const SurfaceCacheFrameData& frame_data,
        u32 current_frame_index);

    // T8.12: card-view capture — renders a rotating batch of cards from their
    // own orthographic views straight into the atlas pages (albedo/normal/
    // axis-depth), replacing the camera-bound screen-space re-projection.
    // Enabled via env SC_CARDVIEW=1; when on, AddPass skips the screen-space
    // capture_gather dispatch (the two paths are mutually exclusive).
    bool IsCardViewEnabled() const { return card_view_enabled_; }
    // Vulkan-only, exact opt-in SC_CAPTURE_SOURCE_AUDIT=1.  These accessors
    // expose the actual persistent writer image for the DUMP2 readback; no
    // writer allocation exists when the gate is off.
    bool IsSourceWriterAuditEnabled() const { return source_writer_audit_enabled_; }
    bool IsSourceWriterAuditFailed() const { return source_writer_audit_failed_; }
    rhi::ResourceHandle GetSourceWriterAtlas() const { return source_writer_atlas_; }
    bool IsSourceWriterAtlasInUAV() const { return source_writer_atlas_in_uav_; }
    bool IsGeometryAuditEnabled() const { return geometry_audit_enabled_; }
    bool IsGeometryAuditFailed() const { return geometry_audit_failed_; }
    rhi::ResourceHandle GetGeometryWitness(u32 index) const {
        return index < 4 ? geometry_witness_[index] : rhi::handles::INVALID_RESOURCE;
    }
    bool IsGeometryWitnessInUAV() const { return geometry_witness_in_uav_; }
    void AddCardCapturePasses(rendergraph::RenderGraph& graph,
                              const RenderSceneSnapshot& scene_snapshot,
                              u32 current_frame_index);

    rhi::ResourceHandle GetLightingAtlas(u32 frame_index) const {
        return lighting_atlas_[frame_index % 3];
    }
    // [E0-B 2026-09-21] 直射剥离副本（LightEval 另存：result − direct·ρ/π）。
    // 消费者（SPGI SC 查询 / SC tile 反弹查询源）用它把探针可见链路收敛到
    // indirect-only；未分配时 INVALID。
    rhi::ResourceHandle GetLightingNoDirectAtlas(u32 frame_index) const {
        return lighting_nodirect_atlas_[frame_index % 3];
    }
    rhi::ResourceHandle GetAlbedoAtlas() const { return albedo_atlas_; }
    rhi::ResourceHandle GetNormalAtlas() const { return normal_atlas_; }
    rhi::ResourceHandle GetDepthAtlas() const { return depth_atlas_; }
    rhi::ResourceHandle GetEmissiveAtlas() const { return emissive_atlas_; }
    // [F3 扩展 2026-09-20] 剩余存储的调试视图 accessor（"SC 所有纹理可视化"）。
    rhi::ResourceHandle GetShadowVisibilityBuffer() const { return shadow_visibility_buffer_; }
    rhi::ResourceHandle GetIndirectOutTexture(u32 slot) const { return indirect_out_tex_[slot % 3]; }
    rhi::ResourceHandle GetTileRadianceTexture(u32 slot) const { return indirect_tile_tex_[slot % 3]; }
    rhi::ResourceHandle GetTileAuxTexture() const { return indirect_tile_aux_; }
    // Diagnostic readback only; 32 ProbeRayHit records per tile, 32 B each.
    rhi::ResourceHandle GetIndirectRayHitsBuffer() const { return ray_hits_buffer_; }
    rhi::ResourceHandle GetLightInfoBuffer() const { return light_info_buffer_; }
    // Read-only test-host source diagnostics; no additional production binding.
    rhi::ResourceHandle GetLightingParamsBuffer(u32 slot) const { return params_cb_[slot % 3]; }
    rhi::ResourceHandle GetShadowParamsBuffer(u32 slot) const { return shadow_cb_[slot % 3]; }
    struct LightWriterSnapshot {
        rhi::ResourceHandle staging{rhi::handles::INVALID_RESOURCE};
        rhi::ResourceHandle sources[12]{};
        u64 offsets[12]{}, sizes[12]{}, bytes = 0;
        u32 frame = 0, render_slot = 0, output_slot = 0, dispatch_cards = 0;
        bool recorded = false, failed = false;
    };
    const LightWriterSnapshot& GetLightWriterSnapshot() const { return light_writer_snapshot_; }
    static bool BoundaryLightingProofFrame(u32 frame);
    static bool BoundaryContinuousFrame(u32 frame);
    bool ReadbackBoundaryLightingProof(const char* prefix);
    u32 GetAtlasSize() const { return atlas_size_; }
    u32 GetPageSize() const { return page_size_; }

    // T4.1: runtime LightEval page budget (GIAdaptiveGovernor). 0 keeps the
    // legacy full-update path; takes effect at the next AddPass.
    void SetPageBudget(u32 pages) { config_.surface_cache_capture_budget = pages; }
    // GI 能量所有权修复 Phase 5a: 灯光修改事件 → 下帧 tile validity 清零。
    void RequestTileValidityReset() { tile_validity_reset_pending_ = true; boundary_radiance_cache_.Clear(); }
    rhi::ResourceHandle GetCardDataBuffer() const {
        return card_generator_.GetCardDataBuffer();
    }
    rhi::ResourceHandle GetCardLookupBuffer() const {
        return card_generator_.GetCardLookupBuffer();
    }
    rhi::ResourceHandle GetGridOffsetsBuffer() const {
        return card_generator_.GetGridOffsetsBuffer();
    }
    rhi::ResourceHandle GetGridCardsBuffer() const {
        return card_generator_.GetGridCardsBuffer();
    }

    // X2/A3: 统一 Cache View —— 消费者（DDGI finalize、SPGI）从这里取 SC 数据
    // 快照，不再逐个摸 pass 内部 buffer getter。纯数据搬运，逐字段等价于上方
    // 现有 getter 的组合；lighting_rg 由管线侧按需回填。
    SurfaceCacheView GetCacheView(u32 atlas_idx) const {
        SurfaceCacheView view;
        view.albedo_atlas = albedo_atlas_;
        view.lighting_atlas = GetLightingAtlas(atlas_idx);
        view.lighting_nodirect_atlas = GetLightingNoDirectAtlas(atlas_idx);  // E0-B
        view.card_data = card_generator_.GetCardDataBuffer();
        view.card_lookup = card_generator_.GetCardLookupBuffer();
        view.grid_offsets = card_generator_.GetGridOffsetsBuffer();
        view.grid_cards = card_generator_.GetGridCardsBuffer();
        view.depth_atlas = depth_atlas_;               // T3.4 (D3)
        view.atlas_size = atlas_size_;                 // == LumenConfig::surface_cache_atlas_size
        view.card_count = card_generator_.GetCardCount();
        view.max_card_count = card_generator_.GetMaxCardCount();
        view.lookup_count = card_generator_.GetLookupCount();
        return view;
    }

    // X2/A3: 时域策略枚举存储（SC 自身无 probe 时域链；X3 归并入五轴配置）。
    void SetTemporalStrategy(GITemporalStrategy strategy);
    GITemporalStrategy GetTemporalStrategy() const { return temporal_strategy_; }

    // SC owns the fenced frame slots; expose only its current scene epoch.
    rhi::ResourceHandle GetGeometryFirstHitBuffer(u32 frame,u32 i) const {
        const auto& s=geometry_buffers_[frame%3];
        return i<4 && s.ready && s.epoch==geometry_epoch_ ? s.buffers[i] : rhi::handles::INVALID_RESOURCE;
    }
    bool WantsGeometryFirstHit() const { return geometry_first_requested_; }
    bool SetGeometryScene(std::span<const ray_geometry::InstanceInput> instances);
    void InvalidateGeometryScene() { geometry_scene_={}; ++geometry_epoch_; RequestTileValidityReset(); }

    bool IsInitialized() const { return initialized_; }

    CardGenerator& GetCardGenerator() { return card_generator_; }
    const CardGenerator& GetCardGenerator() const { return card_generator_; }

    // T4.4: per-instance emissive（rgb = radiance）。下一帧 capture 写入
    // emissive_atlas，LightEval 项随之进入 SC↔probe 反弹（≤8.0 钳制）。
    void SetInstanceEmissive(uint32_t instance_id, const math::v3& rgb);

    // ============================================================================
    // [SC per-mesh 计划 P2] 静态表面页（PRIMAL_SC_STATIC_PAGES=<scsurf 路径>，
    // 默认关——计划门禁纪律：候选初始默认关闭，P6 完整门禁通过才考虑翻默认）。
    //   AttachStaticPages  — 读 sidecar header/records（页按需 fread）。
    //   ResolveStaticSlot  — 注册循环用双摘要键控；INVALID = 无页（保持 capture
    //                        路径）。合成管线不可用（Metal v1）时恒 INVALID，
    //                        保证旗标与合成能力同生同灭。
    // ============================================================================
    // Diagnostic caller invokes after the production frame submission.
    bool ReadbackRayLookupStats(uint32_t frame);
    bool AttachStaticPages(const char* path);
    uint32_t ResolveStaticSlot(uint64_t geometry_digest, uint64_t layout_digest, uint64_t material_digest) const;
    bool StaticPagesEnabled() const { return static_pages_ && static_pages_->IsAttached(); }
    bool AttachBoundarySamples(const char* path);
    const rhi::card_boundary::Mesh* BoundarySamplesForStaticSlot(u32 slot) const {
        return static_pages_ ? boundary_samples_.Find(slot,*static_pages_) : nullptr;
    }

private:
    void CreateAtlasTextures();
    void CreateConstantBuffers();
    void CreateDescriptorSetLayouts();
    void CreatePipelines();
    void CreateRayLookupStats();   // [SC per-mesh P4]

    bool              initialized_{ false };
    bool boundary_continuous_requested_{false};
    bool boundary_consumer_requested_{false};
    bool geometry_first_requested_ = false;
    ray_geometry::SceneBVH geometry_scene_;
    u64 geometry_epoch_ = 0;
    struct GeometryBuffers {
        rhi::ResourceHandle buffers[4]{rhi::handles::INVALID_RESOURCE,rhi::handles::INVALID_RESOURCE,
            rhi::handles::INVALID_RESOURCE,rhi::handles::INVALID_RESOURCE};
        u64 sizes[4]{}, epoch{~u64(0)};
        bool ready = false;
    } geometry_buffers_[3];
    bool UploadGeometryScene(u32 frame);
    rhi::ResourceHandle triangle_support_buffer_{rhi::handles::INVALID_RESOURCE};
    u32 triangle_support_word_count_{0};
    uint32_t triangle_support_epoch_{0xffffffffu};
    rhi::ResourceHandle triangle_atlas_buffer_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle ray_triangle_buffer_{rhi::handles::INVALID_RESOURCE};
    u64 ray_triangle_capacity_ = 0;

    rhi::RHIDeviceBase* device_{ nullptr };
    LumenConfig       config_{};

    CardGenerator     card_generator_;

    // Atlas textures (persistent)
    rhi::ResourceHandle albedo_atlas_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle normal_atlas_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle depth_atlas_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle emissive_atlas_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle lighting_atlas_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };
    // [E0-B 2026-09-21] nodirect 副本（三缓冲，与 lighting_atlas_ 同步轮转）：
    // LightEval 每帧同时写 result_nodirect = result − direct_light·ρ/π。
    rhi::ResourceHandle lighting_nodirect_atlas_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };
    // UNITS: lighting_atlas_ = DiffuseOutgoingRadiance（Lo = ρ(E_direct +
    // E_probe_floor + E_sc_bounce)/π + emissive，LightEval 产出；审计 §5：
    // DDGI trace 反向采样它是合法 radiance；审计 §3.2：探针项是冷启动地板，
    // 收敛后应由 E2 逐射线采样承担）。

    // T8.12 card-view capture state
    bool card_view_enabled_{ false };
    u32  card_view_cursor_{ 0 };          // face cursor over card indices
    bool card_view_seeded_{ false };      // all faces captured at least once
    u32  card_view_faces_total_{ 0 };
    rhi::ResourceHandle card_view_depth_{ rhi::handles::INVALID_RESOURCE };  // scratch D32 (atlas-sized)

    // SC_CAPTURE_SOURCE_AUDIT=1 diagnostics.  The requested/failed flags are
    // kept separate so an unavailable Vulkan writer can never silently select
    // the production card capture or the non-card-view GBuffer path.
    bool source_writer_audit_requested_{ false };
    bool source_writer_audit_enabled_{ false };
    bool source_writer_audit_failed_{ false };
    bool source_writer_atlas_in_uav_{ false };
    rhi::ResourceHandle source_writer_atlas_{ rhi::handles::INVALID_RESOURCE };

    // SC_CAPTURE_GEOMETRY_AUDIT=1: four persistent RGBA32_UInt MRT witnesses
    // appended after the existing writer RT.  They are allocated only when the
    // source writer audit is explicitly enabled and device caps pass.
    bool geometry_audit_requested_{ false };
    bool geometry_audit_enabled_{ false };
    bool geometry_audit_failed_{ false };
    bool geometry_witness_in_uav_{ false };
    rhi::ResourceHandle geometry_witness_[4]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE };

    // Constant buffers (triple-buffered)
    rhi::ResourceHandle global_cb_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };
    rhi::ResourceHandle params_cb_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };

    // Descriptor set layouts
    rhi::DescriptorSetLayoutHandle dilate_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetLayoutHandle light_cull_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetLayoutHandle light_eval_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetLayoutHandle capture_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetLayoutHandle indirect_trace_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetLayoutHandle indirect_resolve_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetLayoutHandle atlas_init_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetLayoutHandle capture_gather_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetLayoutHandle card_fill_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetLayoutHandle shadow_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };   // T3.3

    // Pipelines
    rhi::PipelineHandle capture_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineHandle dilate_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineHandle light_cull_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineHandle light_eval_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineHandle indirect_trace_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineHandle indirect_resolve_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineHandle atlas_init_pipeline_{ rhi::handles::INVALID_PIPELINE };
    // [SC per-mesh P2] 静态页合成（Vulkan-only；Metal 走原 capture 路径）。
    rhi::PipelineHandle static_composite_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineLayoutHandle static_composite_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::DescriptorSetLayoutHandle static_composite_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetHandle static_composite_set_{ rhi::handles::INVALID_DESCRIPTOR_SET };
    rhi::ResourceHandle static_composite_info_buffer_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle static_composite_pool_buffer_{ rhi::handles::INVALID_RESOURCE };
    std::unique_ptr<SurfaceStaticPages> static_pages_;
    BoundaryStaticSamples boundary_samples_;
    uint32_t surface_seed_epoch_{ 0xFFFFFFFFu };
    uint32_t static_composite_epoch_{ 0xFFFFFFFFu };   // 已合成的分配纪元
    uint32_t static_composite_card_count_{ 0 };        // 上次合成的静态卡数（诊断）
    uint64_t static_composite_pool_bytes_{ 0 };        // 上次合成的页字节（诊断）
    rhi::PipelineHandle capture_gather_pipeline_{ rhi::handles::INVALID_PIPELINE };
    // A7: write/clear phases of the three-dispatch capture (same comp file)
    rhi::PipelineHandle capture_write_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineHandle capture_clear_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineHandle card_fill_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineHandle shadow_pipeline_{ rhi::handles::INVALID_PIPELINE };   // T3.3

    // Pipeline layouts
    rhi::PipelineLayoutHandle capture_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineLayoutHandle dilate_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineLayoutHandle light_cull_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineLayoutHandle light_eval_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineLayoutHandle indirect_trace_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineLayoutHandle indirect_resolve_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineLayoutHandle atlas_init_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineLayoutHandle capture_gather_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineLayoutHandle card_fill_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineLayoutHandle shadow_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };   // T3.3

    // Descriptor sets (triple-buffered per pass)
    rhi::DescriptorSetHandle atlas_init_set_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET
    };
    rhi::DescriptorSetHandle capture_gather_set_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET
    };
    rhi::DescriptorSetHandle card_fill_set_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET
    };
    rhi::DescriptorSetHandle shadow_set_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET
    };   // T3.3
    rhi::DescriptorSetHandle capture_set_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET
    };
    rhi::DescriptorSetHandle dilate_set_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET
    };
    rhi::DescriptorSetHandle light_cull_set_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET
    };
    rhi::DescriptorSetHandle light_eval_set_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET
    };
    rhi::DescriptorSetHandle indirect_trace_set_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET
    };
    rhi::DescriptorSetHandle indirect_resolve_set_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET
    };

    // Runtime buffers
    rhi::ResourceHandle light_info_buffer_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle ray_hits_buffer_{ rhi::handles::INVALID_RESOURCE };
    // G (plan §三.G): actual hit capacity of ray_hits_buffer_ in ProbeRayHit
    // entries (tilesX×tilesY×raysPerProbe) — sized from the runtime tile
    // config, guarded before every trace/resolve dispatch.
    u64 ray_hits_capacity_{ 0 };
    // B (plan §三.B): per-tile validity bitmask (1 bit per tile, packed u32).
    // A set bit means "ray_hits for this tile were consumed while current" —
    // resolve may carry that tile's history; a clear bit means never-traced
    // (or post-reset) and its texels must be explicit zeros, never stale
    // hits reinterpreted as measurements. Recreated (zeroed) when the tile
    // grid or the card set changes.
    rhi::ResourceHandle indirect_tile_valid_buffer_{ rhi::handles::INVALID_RESOURCE };
    // GI 能量所有权修复 Phase 5a: 灯光修改事件 → 下帧 tile validity 位图
    // 强制清零（indirect tile EMA 历史失效，radiosity 重新积累）。
    bool tile_validity_reset_pending_{ false };
    u64 indirect_tile_config_sig_{ 0 };
    // A (plan §三.A): per-tile INCIDENT radiance + validity (triple-buffered,
    // tile-domain EMA) and the compat-evidence tile texture (oct normal /
    // capture depth / owning card id, single-buffered — rewritten per frame).
    rhi::ResourceHandle indirect_tile_tex_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle indirect_tile_aux_{ rhi::handles::INVALID_RESOURCE };
    // A: reconstruct pass (tile-grid → validated continuous full atlas).
    rhi::PipelineHandle indirect_reconstruct_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineLayoutHandle indirect_reconstruct_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::DescriptorSetLayoutHandle indirect_reconstruct_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetHandle indirect_reconstruct_set_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET
    };
    rhi::ResourceHandle card_dispatch_buffer_{ rhi::handles::INVALID_RESOURCE };
    // A7: nearest-depth scratch (u32 floatBits per atlas texel) for the
    // three-dispatch GBuffer-gather capture (clear → atomicMin → write).
    rhi::ResourceHandle capture_min_depth_buffer_{ rhi::handles::INVALID_RESOURCE };
    // T3.3 (D10): indirect trace/resolve wiring — params UBOs, per-frame
    // indirect_out target and a 16B dummy for the unused GlobalShaderData slot.
    rhi::ResourceHandle indirect_tile_owner_buffers_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE};
    u64 indirect_tile_owner_bytes_[3]{};
    u64 indirect_tile_owner_signatures_[3]{~u64(0),~u64(0),~u64(0)};
    rhi::ResourceHandle indirect_trace_params_cb_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle indirect_resolve_params_cb_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE };
    // [SC per-mesh P4] 射线 lookup 分类统计（15×u32，host-visible，常驻绑定）。
    rhi::ResourceHandle ray_lookup_stats_buffer_{ rhi::handles::INVALID_RESOURCE };
    uint32_t ray_lookup_stats_dumped_{ 0 };
    // [SC per-mesh P2] 页复用拒绝旧引用：挤页 rect 清零（Vulkan-only）。
    rhi::PipelineHandle page_clear_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineLayoutHandle page_clear_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::DescriptorSetLayoutHandle page_clear_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetHandle page_clear_set_{ rhi::handles::INVALID_DESCRIPTOR_SET };
    rhi::ResourceHandle page_clear_rects_buffer_{ rhi::handles::INVALID_RESOURCE };
    uint32_t page_clear_epoch_{ 0xFFFFFFFFu };
    rhi::ResourceHandle indirect_out_tex_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle indirect_dummy_cb_{ rhi::handles::INVALID_RESOURCE };
    // 1x1 stand-in for shader-layout SampledImage slots that the resolve
    // pass never reads — binding a real atlas there forces the descriptor's
    // SHADER_READ_ONLY_OPTIMAL expectation onto a texture that is parked in
    // GENERAL for storage writes (VUID-vkCmdDraw-None-09600 every dispatch).
    rhi::ResourceHandle indirect_dummy_tex_{ rhi::handles::INVALID_RESOURCE };
    // T3.3: atlas-space directional visibility (float per atlas texel,
    // CPU-initialized to 1.0) + triple-buffered shadow params CB.
    rhi::ResourceHandle shadow_visibility_buffer_{ rhi::handles::INVALID_RESOURCE };
    bool s_ab_readback_done_ = false;
    bool s_albedo_readback_done_ = false;
    rhi::ResourceHandle sc_offline_sdf_texture_{ rhi::handles::INVALID_RESOURCE };
    math::v3 sc_offline_sdf_origin_{};
    math::v3 sc_offline_sdf_extent_{};
    u32 sc_offline_sdf_resolution_ = 0;
    rhi::ResourceHandle shadow_cb_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };

    u32 atlas_size_ = 0;
    u32 page_size_ = 0;
    u32 light_eval_rr_cursor_ = 0;   // T1.4: round-robin card cursor for page-budget LightEval
    u32 lighting_atlas_seed_mask_ = 0; // one bit per triple-buffered lighting slot
    GITemporalStrategy temporal_strategy_ = GITemporalStrategy::EMA;   // X2/A3 枚举存储（暂不改变行为）

    // [WRC 计划 Stream D1]（PRIMAL_SC_PRIORITY=1）：卡片重要性打分的新鲜度
    // 状态——上次被 LightEval / card-view capture 服务的帧号。打分 =
    // 屏幕占比代理(4·ex·ey) × 1/dist² × mix(1, freshness, importance_weight)，
    // d > surface_cache_update_distance 的卡片 score=0（两个死配置参数复活）。
    std::vector<u32> card_light_last_frame_;
    std::vector<u32> card_capture_last_frame_;

    // GBuffer-gather capture state
    rhi::ResourceHandle capture_cb_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };
    // T4.4 v1: capture emissive 源（场景级 uniform；PRIMAL_SC_EMISSIVE="r,g,b"，
    // 默认 0 = 行为不变）。per-material 源随 GBuffer-emissive 批次落地。
    rhi::ResourceHandle capture_emissive_cb_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };
    // T4.4: per-instance emissive 表（SetInstanceEmissive 写入；a>0.5 覆盖
    // 场景级源）。容量 kMaxEmissiveInstances，索引 = mesh_instance_id。
    static constexpr u32 kMaxEmissiveInstances = 1024;
    rhi::ResourceHandle instance_emissive_buffer_{
        rhi::handles::INVALID_RESOURCE };
    bool instance_emissive_dirty_ = false;
    std::array<math::v4, kMaxEmissiveInstances> instance_emissive_cpu_{};
    // CardFill (SDF-based fill) state
    rhi::ResourceHandle card_fill_cb_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };
    rhi::ResourceHandle card_fill_writer_cb_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };
    rhi::DescriptorSetLayoutHandle card_fill_writer_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::PipelineHandle card_fill_writer_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineLayoutHandle card_fill_writer_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::DescriptorSetHandle card_fill_writer_set_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET
    };
    bool atlas_seeded_ = false;   // AtlasInit runs once; Capture overwrites per frame
    LightWriterSnapshot light_writer_snapshot_{};
    // One-shot Vulkan diagnostic; these results never enter production lookup.
    struct BoundaryLightingProof {
        rhi::ResourceHandle points{rhi::handles::INVALID_RESOURCE},visibility{rhi::handles::INVALID_RESOURCE},
            results{rhi::handles::INVALID_RESOURCE},staging{rhi::handles::INVALID_RESOURCE};
        rhi::PipelineHandle pipelines[5]{rhi::handles::INVALID_PIPELINE,rhi::handles::INVALID_PIPELINE,rhi::handles::INVALID_PIPELINE,rhi::handles::INVALID_PIPELINE,rhi::handles::INVALID_PIPELINE};
        rhi::PipelineLayoutHandle layouts[5]{rhi::handles::INVALID_PIPELINE_LAYOUT,rhi::handles::INVALID_PIPELINE_LAYOUT,rhi::handles::INVALID_PIPELINE_LAYOUT,rhi::handles::INVALID_PIPELINE_LAYOUT,rhi::handles::INVALID_PIPELINE_LAYOUT};
        rhi::DescriptorSetLayoutHandle set_layouts[5]{rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT,rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT,rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT,rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT,rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};
        rhi::DescriptorSetHandle sets[5]{rhi::handles::INVALID_DESCRIPTOR_SET,rhi::handles::INVALID_DESCRIPTOR_SET,rhi::handles::INVALID_DESCRIPTOR_SET,rhi::handles::INVALID_DESCRIPTOR_SET,rhi::handles::INVALID_DESCRIPTOR_SET};
        rhi::ResourceHandle radio_buffers[6]{rhi::handles::INVALID_RESOURCE,rhi::handles::INVALID_RESOURCE,rhi::handles::INVALID_RESOURCE,rhi::handles::INVALID_RESOURCE,rhi::handles::INVALID_RESOURCE,rhi::handles::INVALID_RESOURCE};
        rhi::ResourceHandle cache{rhi::handles::INVALID_RESOURCE},cache_staging{rhi::handles::INVALID_RESOURCE};
        BoundaryRadianceCache::Key cache_key{};
        bool cache_recorded=false,reused=false,consumer_recorded=false,readback_requested=false;
        u64 completion_serial=0;
        rhi::QueryPoolHandle timings{rhi::handles::INVALID_QUERY_POOL};
        u32 timing_mask=0,timing_frame=UINT32_MAX;
        std::vector<BoundaryLightPoint> cpu_points;
        rhi::ResourceHandle mixed_index{rhi::handles::INVALID_RESOURCE};
        std::vector<u32> mixed_words;
        rhi::ResourceHandle lookup_lighting{rhi::handles::INVALID_RESOURCE};
        u64 radio_offsets[8]{},radio_sizes[8]{},radio_bytes=0,radio_staging_capacity=0;
        bool radio_traced=false,radio_recorded=false;
        u64 offsets[10]{},sizes[10]{},bytes=0,staging_capacity=0;
        u32 count=0,frame=0,epoch=0;
        bool failed=false,shadowed=false,recorded=false,dumped=false;
    } boundary_light_proof_[3];
    u32 boundary_proof_slot_=0;
    void BoundaryTimestamp(rhi::RHICommandBuffer* cmd,u32 query);
    BoundaryLightingProof& BoundaryProof(){return boundary_light_proof_[boundary_proof_slot_];}
    std::vector<BoundaryPointIndex::Group> boundary_atlas_groups_;
    u32 boundary_atlas_epoch_=UINT32_MAX;
    BoundaryRadianceCache boundary_radiance_cache_;
    bool ReadbackBoundaryLookupProof(const char* prefix);
    bool RecordBoundaryRadianceCache(rhi::RHICommandBuffer* cmd,rhi::ResourceHandle params);
    bool PrepareBoundaryLightingProof(u32 frame,u32 slot);
    void ReleaseBoundaryLightingProof();
    bool PrepareBoundaryRadiosityProof(u32 pass,const std::vector<rhi::DescriptorSetLayoutBinding>& bindings);
    CardFillAuditView card_fill_audit_{};
    std::vector<SurfaceCacheCard> card_fill_audit_cards_;
};

} // namespace primal::graphics::lumen
