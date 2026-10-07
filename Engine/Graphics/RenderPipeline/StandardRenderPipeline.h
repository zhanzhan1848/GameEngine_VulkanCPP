#pragma once

#include "RenderPipeline.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RHI/Core/RHIGPUOptimizer.h"
#include "Graphics/RenderPipeline/RenderPasses/Debug/GeometryDebugPass.h"
#include "Graphics/RenderPipeline/RenderPasses/PostProcess/ToonPass.h"
#include "Graphics/Lumen/SurfaceCache/SurfaceCachePass.h"
#include "Graphics/Lumen/ScreenProbes/ScreenProbeGIPass.h"
#include "Graphics/Lumen/StaticProbe/StaticProbeVolume.h"
#include "Graphics/Lumen/DDGI/LumenDDGIPass.h"
#include "Graphics/Lumen/WRC/LumenWRCPass.h"
#include "Graphics/Lumen/WRC/WRCFitGrid.h"
#include "Graphics/Lumen/Reflections/ReflSpecularTrace.h"
#include "Graphics/Lumen/SSAO/LumenSSAOPass.h"
#include "Graphics/Lumen/SSGI/LumenSSGIPass.h"
#include <array>
#include "Graphics/Lumen/LumenTypes.h"
#include "Graphics/Lumen/GIPlatformProfile.h"
#include "Engine/Common/CoreLogStream.h"   // SetOfflineSDFSource 的 auto-fit 日志（P1-A）
#include "Graphics/Volume/VolumePass.h"
#include "Graphics/RenderPipeline/Modules/GIAdaptiveGovernor.h"
#include "Graphics/Volume/VolumeRenderer.h"
#include "Graphics/Volume/FroxelFogPass.h"
#include "Graphics/Fluid/FluidRenderPass.h"
#include "Graphics/RenderPipeline/PipelineQualityConfig.h"
#include "Graphics/RenderPipeline/Modules/ForwardSceneRenderer.h"
#include "Graphics/RenderPipeline/Modules/ShadowMapModule.h"
#include "Graphics/RenderPipeline/Modules/PlanarReflectionModule.h"
#include "Graphics/RenderPipeline/Modules/DeferredLightingModule.h"
#include "Graphics/RenderPipeline/Modules/FinalBlitModule.h"
#include "Graphics/RenderPipeline/Modules/PassDebugViewModule.h"
#include "Graphics/RenderPipeline/PassDebugView.h"
#include "Graphics/RenderPipeline/Modules/SDFVisualizationModule.h"
#include "Graphics/RenderPipeline/Modules/NeuralDebugModule.h"
#include "Graphics/RenderPipeline/Modules/NeuralCaptureModule.h"
#include "Graphics/RenderPipeline/Modules/NeuralDenoiseModule.h"
#include "Graphics/RenderPipeline/Modules/NeuralFrameGenModule.h"
#include "Graphics/RenderPipeline/Modules/NeuralNRModule.h"
#include "Graphics/RenderPipeline/Modules/DebugProbeSphereModule.h"
#include "Graphics/RenderPipeline/Modules/GIGatherModule.h"
#include "Graphics/RenderPipeline/Modules/FusionCompositeModule.h"
#include "Graphics/RenderPipeline/Modules/SCDDGIIntegrationModule.h"
#include "Graphics/Nanite/DepthHistoryManager.h"
#include "Graphics/Nanite/ColorHistoryManager.h"
#include "Graphics/Nanite/HZBSystem.h"
#include "Graphics/PCG/PCGSDFReadbackManager.h"
#include "Graphics/Renderer.h"  // for light_init_info
#include <memory>

namespace primal::graphics {

namespace nanite {
class GPUCullingPipeline;
class NaniteStreamingManager;
}

class StandardRenderPipeline : public RenderPipeline {
public:
    StandardRenderPipeline() = default;
    ~StandardRenderPipeline() override;

    bool Initialize(rhi::RHIDeviceBase* device) override;
    void Shutdown() override;
    void Render(RenderScene& scene, RenderView& view, rhi::ResourceHandle target, const rhi::TextureDesc& targetDesc, rhi::SyncHandle signalFence = rhi::handles::INVALID_SYNC) override;

    /// Render using a pre-allocated command buffer (matches TestNaniteStreamingPipeline pattern).
    /// The caller manages CB lifecycle: Reset() + Begin() before, End() + Submit() after.
    void RenderWithCommandBuffer(RenderScene& scene, RenderView& view,
                                  rhi::ResourceHandle target, const rhi::TextureDesc& targetDesc,
                                  rhi::RHICommandBuffer* cmd, u32 bufferIndex,
                                  rhi::CommandBufferHandle cmdHandle,
                                  rhi::SyncHandle signalFence = rhi::handles::INVALID_SYNC);

    void SetOutputResource(rhi::ResourceHandle handle, const rhi::TextureDesc& desc) {
        outputResource_ = handle;
        outputDesc_ = desc;
    }

    void SetLumenConfig(const lumen::LumenConfig& config);

    const PipelineStatistics& GetStats() const { return stats_; }

    /// Override specific quality flags. Call AFTER SetLumenConfig — triggers re-init of Lumen passes.
    void SetQualityOverride(bool enable_screen_probes, bool enable_surface_cache = false);

    // --- Runtime pass configuration ---
    void SetPassEnabled(RenderPassID pass, bool enabled);
    bool IsPassEnabled(RenderPassID pass) const;
    bool IsPassActive(RenderPassID pass) const;
    static constexpr u32 GetPassCount() { return static_cast<u32>(RenderPassID::Count); }

    // --- Runtime settings ---
    const RenderPipelineSettings& GetSettings() const { return settings_; }
    void UpdateSettings(const RenderPipelineSettings& settings);
    // M2/M4: light-only mutation — deferred direct, SC LightEval and the DDGI
    // trace all read settings_.lighting per frame, so this toggles the real
    // sun WITHOUT the ApplyConfigChanges Lumen rebuild (which resets the
    // probe field and would measure re-init, not light response).
    // GI 能量所有权修复 Phase 5a（GIHistoryValidity 最小版）: 灯光 color/
    // direction 变化检测 → 置 gi_history_dirty_，渲染帧起点触发 DDGI 提权
    // + α 抬升窗 / SC tile validity 清零 / SPGI temporal reset。与编辑器
    // GI 面板、M2 半衰期门共用本入口（单一事实源，不新增第二路径）。
    void UpdateLightingSettings(const RenderPipelineSettings& settings) {
        const auto& a = settings_.lighting;
        const auto& b = settings.lighting;
        const bool changed =
            a.light_color.x != b.light_color.x || a.light_color.y != b.light_color.y ||
            a.light_color.z != b.light_color.z || a.light_color.w != b.light_color.w ||
            a.light_direction.x != b.light_direction.x ||
            a.light_direction.y != b.light_direction.y ||
            a.light_direction.z != b.light_direction.z;
        settings_.lighting = settings.lighting;
        if (changed) gi_history_dirty_ = true;
    }

    lumen::StaticProbeVolume* GetStaticProbeVolume() const { return static_probe_volume_.get(); }

    // PCG SDF readback — provides real GlobalSDF data for PCG scatter
    pcg::PCGSDFReadbackManager* GetPCGSDFReadback() { return &pcg_sdf_readback_; }
    const pcg::PCGSDFReadbackManager* GetPCGSDFReadback() const { return &pcg_sdf_readback_; }

    /// Set logical viewport dimensions.
    /// Actual render resolution = logical × quality_config_.render_scale.
    /// A13/T0.11: a size change arms viewport_size_changed_, consumed at the
    /// top of the next frame — TAA/SPGI history reset so no pass blends a
    /// stale different-size history into the resized output. (Full GBuffer/GI
    /// texture recreation remains engine-policy work, see the A13 notes.)
    void SetViewportSize(u32 width, u32 height) {
        const u32 newW = static_cast<u32>(width  * settings_.quality.render_scale);
        const u32 newH = static_cast<u32>(height * settings_.quality.render_scale);
        if (newW != render_width_ || newH != render_height_) {
            viewport_size_changed_ = true;
        }
        logical_width_ = width;
        logical_height_ = height;
        render_width_  = newW;
        render_height_ = newH;
    }

    /// Inject pre-compiled shader handles. Call before SetLumenConfig.
    struct ShaderHandles {
        rhi::ShaderHandle deferred_vs{rhi::handles::INVALID_SHADER};
        rhi::ShaderHandle deferred_ps{rhi::handles::INVALID_SHADER};
        rhi::ShaderHandle blit_vs{rhi::handles::INVALID_SHADER};
        rhi::ShaderHandle blit_ps{rhi::handles::INVALID_SHADER};
        rhi::ShaderHandle shadow_filter{rhi::handles::INVALID_SHADER};
        rhi::ShaderHandle gi_gather{rhi::handles::INVALID_SHADER};
        rhi::ShaderHandle fusion_indirect_ps{rhi::handles::INVALID_SHADER};
        rhi::ShaderHandle fusion_composite_ps{rhi::handles::INVALID_SHADER};
        rhi::ShaderHandle sc_card_radiance{rhi::handles::INVALID_SHADER};
        rhi::ShaderHandle sc_probe_irradiance{rhi::handles::INVALID_SHADER};
        rhi::ShaderHandle sdf_viz_ps{rhi::handles::INVALID_SHADER};
        rhi::ShaderHandle probe_sphere_vs{rhi::handles::INVALID_SHADER};
        rhi::ShaderHandle probe_sphere_ps{rhi::handles::INVALID_SHADER};
    };

    void SetShaderHandles(const ShaderHandles& handles) {
        deferred_vs_ = handles.deferred_vs;
        deferred_ps_ = handles.deferred_ps;
        blit_vs_ = handles.blit_vs;
        blit_ps_ = handles.blit_ps;
        shadow_filter_shader_ = handles.shadow_filter;
        gi_gather_shader_ = handles.gi_gather;
        fusion_indirect_ps_ = handles.fusion_indirect_ps;
        fusion_composite_ps_ = handles.fusion_composite_ps;
        sc_card_radiance_shader_ = handles.sc_card_radiance;
        sc_probe_irradiance_shader_ = handles.sc_probe_irradiance;
        sdf_viz_ps_ = handles.sdf_viz_ps;
        probe_sphere_vs_ = handles.probe_sphere_vs;
        probe_sphere_ps_ = handles.probe_sphere_ps;
    }

    /// T4.6.5 part 37: IBL resources (Tier 5 visual fidelity). Pass output of
    /// IBLPrecomputer::ComputeIrradianceMap / ComputePrefilteredEnvironmentMap /
    /// ComputeBRDFIntegrationMap. Safe to call before or after SetLumenConfig —
    /// the deferred module is consulted lazily on each AddPasses. Pass
    /// INVALID_RESOURCE for any handle to disable that IBL component (shader
    /// samples a 1x1 white fallback — effectively no IBL).
    void SetIBLResources(rhi::ResourceHandle irradiance,
                         rhi::ResourceHandle prefilter,
                         rhi::ResourceHandle brdfLUT);

    lumen::SurfaceCachePass* GetSurfaceCachePass() const {
        return surface_cache_pass_ ? surface_cache_pass_.get() : nullptr;
    }

    // Debug/testing: SSGI output texture access.
    lumen::LumenSSGIPass* GetSSGIPass() const {
        return ssgi_pass_ ? ssgi_pass_.get() : nullptr;
    }

    // Debug/testing: deferred lighting output texture access.
    DeferredLightingModule* GetDeferredLightingModule() const {
        return deferred_module_ ? deferred_module_.get() : nullptr;
    }

    // Debug/testing: DDGI half-res gather output texture access.
    GIGatherModule* GetGIGatherModule() const {
        return gi_gather_module_ ? gi_gather_module_.get() : nullptr;
    }

    // Debug/testing: shadow visibility texture access.
    ShadowMapModule* GetShadowMapModule() const {
        return shadow_module_ ? shadow_module_.get() : nullptr;
    }

    // Debug/testing: froxel fog scatter texture access.
    volume::FroxelFogPass* GetFroxelFogPass() const {
        return froxel_fog_pass_ ? froxel_fog_pass_.get() : nullptr;
    }

    // DLSS 集成门禁（TestVulkanDLSSRenderGraph）用模块访问器；env 未开时返回空。
    NeuralFrameGenModule* GetNeuralFrameGenModule() const {
        return neural_framegen_module_.get();
    }
    NeuralNRModule* GetNeuralNRModule() const { return neural_nr_module_.get(); }

    // Debug/testing: fusion composite module access.
    FusionCompositeModule* GetFusionCompositeModule() const {
        return fusion_module_ ? fusion_module_.get() : nullptr;
    }

    // Debug/testing: primary DDGI volume access.
    // Default-off Vulkan source-distance candidate. CPU geometry survives the
    // deferred Lumen rebuild and is uploaded to each actual DDGI volume.
    bool SetDDGIFirstSurfaceGeometry(const void* nodes, u64 nodeBytes,
                                    const void* triangles, u64 triangleBytes);
    // Default-off child candidate.  The source handles are borrowed from the
    // live GPUDrivenDraw scene; the DDGI pass owns only its triangle mapping
    // and audit buffers.  This is staged across deferred Lumen rebuilds.
    bool SetDDGIFirstNativeNormalSources(
        rhi::ResourceHandle positions,
        rhi::ResourceHandle elements,
        rhi::ResourceHandle instances,
        rhi::ResourceHandle materials,
        rhi::ResourceHandle normalTexture,
        rhi::SamplerHandle normalSampler,
        const void* triangleMapping,
        u64 triangleMappingBytes);
    // Default-off Vulkan analytic fallback rho candidate.  Geometry/mapping
    // are staged in CPU memory and borrowed native sources are refreshed after
    // GPUDrivenDraw creates the current scene buffers.
    bool SetDDGIAnalyticFirstRhoSources(
        const void* nodes, u64 nodeBytes,
        const void* triangles, u64 triangleBytes,
        rhi::ResourceHandle positions,
        rhi::ResourceHandle elements,
        rhi::ResourceHandle instances,
        rhi::ResourceHandle materials,
        rhi::ResourceHandle albedoTexture,
        rhi::SamplerHandle albedoSampler,
        const void* triangleMapping,
        u64 triangleMappingBytes);

    lumen::LumenDDGIPass* GetDDGIPass() const {
        return ddgi_volumes_[0] ? ddgi_volumes_[0].get() : nullptr;
    }

    // Debug/testing: screen-probe GI access（SPGI_TRACE_DIAG 读回用）.
    lumen::ScreenProbeGIPass* GetScreenProbePass() const {
        return screen_probe_pass_ ? screen_probe_pass_.get() : nullptr;
    }
    // Debug/testing: logical GI age after SDF + snapshot + SC cards become
    // ready. Diagnostics must not use the absolute render-frame clock.
    u32 GetGIFrameIndex() const {
        return GIFrame(static_cast<u32>(frameCount_));
    }
    // P1（reaudit §4.1/4.2）: trace 分支诊断模式（0=off，1/2/3 见 diag_params）。
    // 重建 screen_probe_pass_ 时保留（创建点读取成员）。
    void SetScreenProbeDiagMode(u32 mode) {
        spgi_trace_diag_mode_ = mode;
        if (screen_probe_pass_) screen_probe_pass_->SetTraceDiagMode(mode);
    }
    // P1（reaudit §4.4 验收）: 雾的体积阴影开关（FROXEL_NO_SHADOW A/B 用）。
    // false 时不给 FroxelFogPass 传任何阴影输入（shadowMode=0 恒 lit）。
    void SetFroxelShadowsEnabled(bool enabled) { froxel_shadows_enabled_ = enabled; }
    // WP1/WP2/WP3（froxel-upgrade 计划）: 运行时旋钮透传（免 pass 重建，
    // 门禁 G5 双相位 / FROXEL_TEMPORAL / FROXEL_SPGI_AMBIENT A-B 用）。
    // pass 未初始化时安全 no-op。
    void SetFroxelMarchSteps(u32 steps) {
        if (froxel_fog_pass_) froxel_fog_pass_->SetMarchSteps(steps);
    }
    void SetFroxelTemporalEnabled(bool on) {
        if (froxel_fog_pass_) froxel_fog_pass_->SetTemporalEnabled(on);
    }
    void SetFroxelAmbientSPGI(bool on) {
        if (froxel_fog_pass_) froxel_fog_pass_->SetAmbientSourceSPGI(on);
    }

    rhi::RHIDeviceBase* GetDevice() const { return device_; }

    // --- Editor mode (lightweight forward rendering) ---
    // PassDebugView 激活期间 SetEditorMode(true) 被忽略（debug 视图需要完整
    // 渲染图路径；PASS=Off 后恢复正常切换语义，见 SetPassDebugView）。
    void SetEditorMode(bool enabled);
    bool IsEditorMode() const { return editor_mode_; }
    bool LoadEditorScene(const std::string& model_path) {
        if (!forward_renderer_) return false;
        return forward_renderer_->LoadScene(model_path);
    }
    ForwardSceneRenderer* GetForwardRenderer() const { return forward_renderer_.get(); }

    // --- Pass Debug View（视口实时 pass 可视化，编辑器 PASS 下拉 / CLI 共用）---
    // view 取 PassDebugView 枚举值（0=Off）。非 Off 时 Step 11 用
    // PassDebugViewModule 整屏显示目标中间资源；目标 pass 本帧未产出则回退
    // 正常 FinalBlit。切换是 per-frame 值，无管线重建。
    void SetPassDebugView(u32 view);
    u32 GetPassDebugView() const { return debug_view_; }
    // WRC 探针球 overlay（对齐 SetProbeSphereDebug 的 DDGI 先例）。
    // [P0 2026-09-21] env PRIMAL_WRC_PROBE_SPHERES=1 = 测试/离线入口（此前
    // 仅编辑器 C ABI 可达）；显式 setter 仍可覆盖。
    void SetWRCProbeDebug(bool enabled) { wrc_probe_debug_ = enabled; }
    bool IsWRCProbeDebugEnabled() const {
        static const bool envOn = [] {
            const char* e = std::getenv("PRIMAL_WRC_PROBE_SPHERES");
            return e && e[0] == '1';
        }();
        return wrc_probe_debug_ || envOn;
    }

    /// Toggle fullscreen GlobalSDF visualization (replaces FinalBlit output).
    /// Cheap per-frame bool — does NOT trigger pipeline re-initialization.
    void SetSDFVisualization(bool enabled) { sdf_visualization_enabled_ = enabled; }
    bool IsSDFVisualizationEnabled() const { return sdf_visualization_enabled_; }
    // M8/T8.5: real-mesh DDGI probe spheres overlaid on the final LDR image,
    // depth-occluded by the scene. Requires probe_sphere_vs/ps shader handles
    // and an initialized DDGI pass (SPGI_PROBE_SPHERES=1 in the Sponza test).
    void SetProbeSphereDebug(bool enabled) { probe_sphere_debug_ = enabled; }
    bool IsProbeSphereDebugEnabled() const { return probe_sphere_debug_; }

    /// A7 视图模式: 0=Shaded, 1=Wireframe (deferred GBuffer path). Cheap
    /// per-frame field — the wireframe twin pipeline is created at GPUDriven
    /// init; switching only changes which handle the draw pass binds.
    /// (2=Unlit reserved; needs a deferred-shader variant, see plan A7.)
    void SetViewMode(u32 mode) { view_mode_ = mode; }
    u32 GetViewMode() const { return view_mode_; }

    /// Pass Debug View 当前视图（PassDebugView 枚举值）+ 回退提示去重。
    u32 debug_view_{0};
    bool debug_view_fallback_warned_{false};
    /// 本帧 BackBuffer 实际格式（PassDebugView 管线变体选择用——Metal 对
    /// 管线 pixel format 与 attach 格式不一致会静默丢 draw）。
    rhi::DataFormat current_output_format_{ rhi::DataFormat::BGRA8_UNorm };
    /// WRC 探针球 overlay 开关。
    bool wrc_probe_debug_{false};

    /// Geometry debug overlay (meshlets / SDF slices / vector fields / voxels).
    /// Drawn on the tone-mapped frame before FinalBlit. Cheap per-frame
    /// settings copy — the debug pipelines lazy-init on first enabled frame.
    void SetGeometryDebugSettings(const GeometryDebugSettings& settings) {
        geometry_debug_settings_ = settings;
    }
    const GeometryDebugSettings& GetGeometryDebugSettings() const {
        return geometry_debug_settings_;
    }

    /// Toon/cel-shading post process (quantize + depth Sobel edges) applied to
    /// the tone-mapped frame. Cheap per-frame toggle + params.
    void SetToonEnabled(bool enabled,
                        const renderpass::ToonParams& params = renderpass::ToonParams{}) {
        toon_enabled_ = enabled;
        toon_params_ = params;
    }
    bool IsToonEnabled() const { return toon_enabled_; }

    /// VSM shadows (moments + Chebyshev) vs PCSS (R8 visibility). Forwards to
    /// the ShadowMapModule; affects the next frame's shadow chain.
    void SetVSMShadows(bool enabled) {
        if (shadow_module_) shadow_module_->SetVSMEnabled(enabled);
    }
    bool IsVSMShadows() const { return shadow_module_ && shadow_module_->IsVSMEnabled(); }

    /// TAA sub-pixel jitter gate (A/B testing the resolve; TAA pass itself
    /// keeps running — with jitter off it converges to a passthrough).
    void SetTAAJitter(bool on) { taa_jitter_enabled_ = on; }
    bool IsTAAJitter() const { return taa_jitter_enabled_; }

    /// Planar reflection (mirror). The module renders the scene from the
    /// mirrored camera and composites the mirror quad over the HDR frame
    /// (before TAA). Plane defaults to a 10×10 floor mirror at the origin.
    void SetMirrorEnabled(bool enabled) {
        if (reflection_module_) reflection_module_->SetEnabled(enabled);
    }
    bool IsMirrorEnabled() const {
        return reflection_module_ && reflection_module_->IsEnabled();
    }
    void SetMirrorPlane(const PlanarReflectionPlane& plane) {
        if (reflection_module_) reflection_module_->SetPlane(plane);
    }

    // --- Offline SDF data source for SDF visualization ---
    // When set, SDF visualization uses this pre-built global SDF texture
    // instead of the runtime GlobalSDF cascades.
    // [双稳态修复] 场景卡片注册（从 NeedsFullRebuild 分支提取——
    // 内容就绪重注册路径复用同一实现）。
    void RegisterSceneCardsForCache(const RenderScene& scene);
    void TryLatchGIContentReady();
    // Persist across lazy GI-volume creation / auto-fit rebuilds.
    // Caller certifies offline texture is experimental RG16F evidence.
    void SetOfflineSDFHitEvidence(u32 mode) {
        offline_sdf_hit_evidence_=std::min(mode,3u);
        if (ddgi_pass_) ddgi_pass_->SetOfflineHitEvidence(offline_sdf_hit_evidence_);
    }

    void SetOfflineSDFSource(rhi::ResourceHandle texture,
                             math::v3 origin, math::v3 extent, u32 resolution,
                             bool signed_field = false) {
        offline_sdf_texture_ = texture;
        offline_sdf_origin_ = origin;
        offline_sdf_extent_ = extent;
        offline_sdf_resolution_ = resolution;
        has_offline_sdf_ = (texture != rhi::handles::INVALID_RESOURCE);
        // GI 根治 Phase 4: 场符号性旗标——转发 DDGI 探针 trace 的零穿越命中
        // 判定（ProbeCounts[3] padding 槽复用，无布局变化）。host 在管线
        // 初始化后才调用本 setter，必须同步转发到已创建的 volume 实例。
        offline_sdf_signed_ = signed_field;
        for (u32 vi = 0; vi < ddgi_volume_count_; ++vi) {
            if (ddgi_volumes_[vi]) ddgi_volumes_[vi]->SetCoarseSdfSigned(signed_field);
        }
        if (ddgi_pass_) ddgi_pass_->SetCoarseSdfSigned(signed_field);
        // [WRC] 同旗标转发（零穿越命中判定；独立代码域内的同一 host 契约）。
        for (u32 vi = 0; vi < wrc_volume_count_; ++vi) {
            if (wrc_volumes_[vi]) wrc_volumes_[vi]->SetCoarseSdfSigned(signed_field);
        }
        // P1-A (Docs/completed/2026-09-18-ddgi-scene-fit-probe-grid-plan.md §8): with
        // fit+auto, adopt the merger volume as the DDGI fit box. Flagging
        // settings_dirty_ defers the DDGI teardown/rebuild to the next frame's
        // ApplyConfigChanges (the §5.5 timing hazard); InitializeLumenPasses
        // re-applies this SDF source to the rebuilt pass afterwards. Re-entry
        // is safe — the second call sees equal bounds and skips.
        if (settings_.lumen.ddgi_fit_to_bounds &&
            settings_.lumen.ddgi_fit_bounds_auto &&
            has_offline_sdf_) {
            const math::v3 bmin = origin;
            const math::v3 bmax{origin.x + extent.x, origin.y + extent.y,
                                origin.z + extent.z};
            const math::v3 curMax = settings_.lumen.ddgi_fit_bounds_max;
            const math::v3 curMin = settings_.lumen.ddgi_fit_bounds_min;
            if (bmin.x != curMin.x || bmin.y != curMin.y || bmin.z != curMin.z ||
                bmax.x != curMax.x || bmax.y != curMax.y || bmax.z != curMax.z) {
                settings_.lumen.ddgi_fit_bounds_min = bmin;
                settings_.lumen.ddgi_fit_bounds_max = bmax;
                settings_dirty_ = true;
                PRIMAL_LOG_STREAM(Info, "Render")
                    << "[Lumen] DDGI fit bounds auto-filled from offline SDF volume ("
                    << extent.x << "x" << extent.y << "x" << extent.z
                    << ") — DDGI volumes rebuild next frame" << std::endl;
            }
        }
        // Forward to DDGI pass so it uses the same high-res SDF for ray tracing.
        if (ddgi_pass_) {
            ddgi_pass_->SetOfflineSDFSource(texture, origin, extent, resolution);
            ddgi_pass_->SetOfflineHitEvidence(offline_sdf_hit_evidence_);
        }
        // T8.13: forward to the SC shadow march as well (runtime cascades
        // misplace SC shadow hits just like they did for the DDGI trace).
        if (surface_cache_pass_) {
            surface_cache_pass_->SetOfflineSDFSource(texture, origin, extent, resolution);
        }
    }

    // --- PCG Entity integration (Phase 3c/3d) ---
    void SetPCGEntities(std::vector<id::id_type> entity_ids,
                        std::vector<u32> mesh_slot_indices) {
        pcg_entity_ids_ = std::move(entity_ids);
        pcg_mesh_slot_indices_ = std::move(mesh_slot_indices);
    }
    void ClearPCGEntities() {
        pcg_entity_ids_.clear();
        pcg_mesh_slot_indices_.clear();
    }

    // --- Static scene Entity integration (Phase 3e) ---
    void SetStaticEntities(std::vector<id::id_type> entity_ids,
                           std::vector<u32> mesh_slot_indices) {
        static_entity_ids_ = std::move(entity_ids);
        static_mesh_slot_indices_ = std::move(mesh_slot_indices);
    }
    void ClearStaticEntities() {
        static_entity_ids_.clear();
        static_mesh_slot_indices_.clear();
    }

    // --- Geometry entity integration (Phase 3.5) ---
    void SetGeometryEntities(std::vector<id::id_type> entity_ids) {
        geometry_entity_ids_ = std::move(entity_ids);
    }
    void ClearGeometryEntities() {
        geometry_entity_ids_.clear();
    }

    // --- Content system entity registration (Phase 4) ---
    // Register a mesh resource + create ECS entity → enters render pipeline
    // geometry_content_id: content system mesh ID (from create_resource)
    // texture_content_ids: [albedo, normal, ORM] content IDs (invalid_id for fallback)
    // Returns entity ID, or invalid_id on failure
    id::id_type RegisterMeshEntity(id::id_type geometry_content_id,
                                   const id::id_type* texture_content_ids,
                                   u32 texture_count);
    void UnregisterMeshEntity(id::id_type entity_id);

    // --- Light entity registration (Path B C ABI support) ---
    // Creates an ECS entity with Transform + Light components.
    // info.entity_id is IGNORED — pipeline creates a new entity internally.
    // Returns invalid_id on failure. Note: 0 is a valid entity id.
    id::id_type RegisterLightEntity(const light_init_info& info);
    bool UpdateLightEntity(id::id_type entity_id, const light_init_info& info);
    void UnregisterLightEntity(id::id_type entity_id);

    /// Hot-reload a shader. Returns true on success.
    bool ReloadShader(rhi::ShaderHandle shader, const void* data, u32 size) override;

    // Access the RenderScene currently being rendered. Returns nullptr if
    // called outside Render(). Used by C ABI (EngineDLL/RenderPipelineAPI.cpp)
    // entry points that need to mutate the scene outside the render call.
    // Spec §4.1 step 3: returns non-null after Initialize (default_scene_
    // backs it when no Render() has set current_scene_ yet, so callers don't
    // need to own a fallback RenderScene).
    RenderScene* GetCurrentScene() {
        return current_scene_ ? current_scene_ : &default_scene_;
    }

    // Frame counter (incremented at the end of each Render call). At Execute
    // time for frame N, returns N — matches the cbIdx % 3 the upcoming Render
    // will pass to ForwardSceneRenderer. Used by GlobalSDFMeshNode to pick
    // the matching triple-buffer slot for SurfaceNets writes.
    u64 GetFrameCount() const { return frameCount_; }

private:
    void InitializeSubsystems();
    void InitializeLumenPasses();
    void ShutdownSubsystems();
    void ShutdownLumenPasses();

    void UpdatePerFrame(RenderScene& scene, RenderView& view);
    void BuildRenderGraph(rhi::ResourceHandle backBuffer, u32 currentBufferIndex);

    // 编辑器路径着色器自加载：SetShaderHandles 未被调用（编辑器 EngineDLL
    // 路径）时补齐 blit/deferred/gi_gather/fusion 系句柄，装配表对齐测试壳。
    void SelfLoadPipelineShaders();

    // Pass Debug View：解析当前 debug_view_ 的源资源为 RG 节点（经
    // ImportResource 声明读 barrier）。全部 out 有效返回 true；目标 pass
    // 本帧未产出返回 false（调用方回退正常 FinalBlit）。深度 + 可见性缓冲
    // + cluster map 是所有视图的副输入（着色器静态引用），缺失同样按不可
    // 用处理。
    bool ResolvePassDebugViewSource(rendergraph::RenderGraph& graph, u32 cbIdx,
                                    const DeferredLightingOutputs& deferredOut,
                                    const FusionOutputs& fusionOut,
                                    rendergraph::RGResourceHandle& out_source,
                                    rendergraph::RGResourceHandle& out_id,
                                    rendergraph::RGResourceHandle& out_depth,
                                    rhi::ResourceHandle& out_cluster_map,
                                    rhi::ResourceHandle& out_aux_buffer,
                                    u32& out_aux_count);
    // B11: DDGI/探针射线的 prev-screen 辐射度供给源（上一帧 Fusion HDR，
    // 含 GI；不可用时返回 invalid，调用方回退 DeferredLighting 直射输出）。
    rendergraph::RGResourceHandle MakeProbeRadianceSource(rendergraph::RenderGraph& graph,
                                                          u32 currentBufferIndex);
    bool ApplyConfigChanges();
    // Rebind the opt-in native-normal child after GPUDrivenDraw has created or
    // rebuilt its borrowed geometry buffers.  Both render entry points call
    // this immediately before recording DDGI passes.
    bool RefreshDDGIFirstNativeNormalSources();
    bool RefreshDDGIAnalyticFirstRhoSources();

    rhi::RHIDeviceBase* device_{nullptr};
    std::unique_ptr<rendergraph::RenderGraph> renderGraph_;
    std::unique_ptr<rhi::RHIGPUOptimizer> gpuOptimizer_;

    rhi::ResourceHandle outputResource_{rhi::handles::INVALID_RESOURCE};
    rhi::TextureDesc outputDesc_;

    PipelineStatistics stats_;

    // Viewport
    u32 logical_width_ = 1280;   // window logical size (set via SetViewportSize)
    u32 logical_height_ = 720;
    u32 render_width_ = 1280;    // actual GPU render resolution (= logical × render_scale)
    u32 render_height_ = 720;
    u32 target_width_ = 0;       // backbuffer size (may differ on Retina/HiDPI)
    u32 target_height_ = 0;

    // Unified settings (replaces lumen_config_ + quality_config_ + hardcoded values)
    RenderPipelineSettings settings_{};
    bool settings_dirty_ = false;

    // --- Nanite subsystems ---
    nanite::GPUCullingPipeline* culling_pipeline_ = nullptr;
    nanite::NaniteStreamingManager* streaming_manager_ = nullptr;
    std::unique_ptr<nanite::DepthHistoryManager> depth_history_;
    std::unique_ptr<nanite::ColorHistoryManager> color_history_;
    std::unique_ptr<nanite::HZBSystem> hzb_system_;

    // --- TAA depth reprojection ---
    // Previous frame's view-projection (cached after each Render() that stores
    // depth history). Together with DepthHistoryManager's prev depth copy it
    // drives TAAPass's matrix reprojection + disocclusion rejection.
    math::m4x4 prev_taa_view_proj_{};
    bool has_prev_taa_view_proj_ = false;

    // --- Scene snapshot ---
    std::unique_ptr<RenderSceneSnapshot> scene_snapshot_;

    // Cached RenderScene pointer — valid between Render() entry and the next
    // Render() call. Set at the top of Render() / RenderWithCommandBuffer().
    // Used by C ABI entry points (PipelineRegisterStreamingMeshEntity etc.)
    // that need to mutate the scene outside the render call.
    RenderScene* current_scene_{nullptr};

    // Default RenderScene owned by the pipeline. Backs GetCurrentScene() when
    // no Render() has run yet (e.g., headless integration tests, Editor's
    // first PipelineRenderFrame). Eliminates the spec §4.1 step 3 violation
    // where callers had to supply their own fallback scene. Lazily filled by
    // ECS sync from PipelineRenderFrame / RegisterMeshEntity.
    RenderScene default_scene_{};

    // --- Lumen GI ---
    // T5.5 multi-volume runtime (cap 4). ddgi_pass_ aliases volumes[0] — all
    // legacy consumers (SC integration, SPGI fallback, pass gating) keep
    // working against the near volume.
    std::array<std::unique_ptr<lumen::LumenDDGIPass>, 4> ddgi_volumes_{};
    u32 ddgi_volume_count_ = 0;
    std::unique_ptr<lumen::LumenDDGIPass>& ddgi_pass_ = ddgi_volumes_[0];
    // [WRC] World Space Radiance Cache（设计文档 §1 独立代码域；PRIMAL_WRC_
    // ENABLE=1 时创建，默认 0 = 零行为变化）。near + far 两实例（cap 2），
    // 与 DDGI 逐项同构、互不包含——禁止从 WRC 读 DDGI 内部（反之亦然）。
    std::array<std::unique_ptr<lumen::LumenWRCPass>, 2> wrc_volumes_{};
    u32 wrc_volume_count_ = 0;
    // [REFL R4/R5] 屏幕外 specular 兜底（PRIMAL_REFL=2 创建；Vulkan 先行）。
    std::unique_ptr<lumen::ReflSpecularTracePass> refl_specular_pass_{};
    // [双稳态修复] GI 内容就绪闩锁——首次满足（离线 SDF 就位 + GlobalSDF
    // 体素化就绪 + 快照无待处理重建）的帧号。GI 暖启动时钟从此帧计数
    // （gi_frame = frame - ready_frame），消除异步内容加载时序导致的
    // SC↔probe 双稳态分支选择。
    u32 gi_content_ready_frame_ = 0xFFFFFFFFu;
    u32 GIFrame(u32 frame) const {
        return gi_content_ready_frame_ == 0xFFFFFFFFu
            ? 0u : (frame - gi_content_ready_frame_);
    }
    // [SC 卡片 Lumen 对齐 Phase 2]（2026-09-19 计划，WRC 遗留 #5）：
    // content-ready 注册门禁——首次 SC 卡片注册推迟到"快照内全部实例都
    // 携带离线 bake 或有效 mesh 紧致 AABB"为止，消除异步加载时序导致的
    // 注册 AABB 质量漂移（374/256 运行间两态）。超时 PRIMAL_SC_REG_GATE_
    // TIMEOUT（默认 600 帧）后按现状注册并告警，防几何永远缺 bounds 的
    // 资产卡死 SC。
    std::vector<u64> sc_material_signatures_;
    bool sc_registration_done_ = false;
    u32 sc_registration_deferred_frames_ = 0;
    bool sc_registration_warned_ = false;
    bool sc_gate_diag_logged_ = false;   // [R2.3] 门禁阻塞实例一次性诊断
    lumen::GIPlatformProfile gi_profile_{};                // T4.5/C5 capability ceiling
    std::unique_ptr<lumen::LumenSSAOPass> ssao_pass_;
    std::unique_ptr<lumen::LumenSSGIPass> ssgi_pass_;
    std::unique_ptr<lumen::SurfaceCachePass> surface_cache_pass_;
    std::unique_ptr<lumen::ScreenProbeGIPass> screen_probe_pass_;
    std::unique_ptr<lumen::StaticProbeVolume> static_probe_volume_;
    // P1（reaudit §4.1/4.2）: SPGI trace 诊断模式（诊断重建保留）+ 雾阴影开关
    u32 spgi_trace_diag_mode_{ 0 };
    bool froxel_shadows_enabled_{ true };

    // --- Volume Rendering ---
    std::unique_ptr<volume::VolumePass> volume_pass_;
    std::unique_ptr<volume::VolumeRenderer> volume_renderer_;
    std::unique_ptr<volume::FroxelFogPass> froxel_fog_pass_;

    // --- Fluid Rendering ---
    std::unique_ptr<fluid::FluidRenderPass> fluid_render_pass_;

    // --- Forward Renderer (Editor mode) ---
    std::unique_ptr<ForwardSceneRenderer> forward_renderer_;
    bool editor_mode_{false};

    // --- Pipeline modules ---
    std::unique_ptr<ShadowMapModule> shadow_module_;
    std::unique_ptr<PlanarReflectionModule> reflection_module_;
    std::unique_ptr<DeferredLightingModule> deferred_module_;
    std::unique_ptr<FinalBlitModule> final_blit_module_;
    std::unique_ptr<GIGatherModule> gi_gather_module_;

    // T4.1: frame-rate-driven GI quality governor (FPS ≥ 30 floor).
    GIAdaptiveGovernor gi_governor_;
    u32 gi_base_probe_budget_ = 0;   // full-quality DDGI probe update count
    u32 gi_base_page_budget_ = 0;    // full-quality LightEval page budget
    // GI 能量所有权修复 Phase 5a: GIHistoryValidity 最小版——灯光修改事件
    // 旗标（UpdateLightingSettings 置位，BuildRenderGraph 帧起点消费）。
    bool gi_history_dirty_ = false;
    std::unique_ptr<FusionCompositeModule> fusion_module_;
    std::unique_ptr<SCDDGIIntegrationModule> sc_ddgi_module_;
    std::unique_ptr<SDFVisualizationModule> sdf_viz_module_;
    // M0 神经渲染 debug 链路：PRIMAL_NEURAL_DEBUG=1 时激活（模块自加载字节码
    // 与 .pnn 权重；未设置 env 时零资源零开销）。
    std::unique_ptr<NeuralDebugModule> neural_debug_module_;
    bool neural_debug_enabled_{false};
    // M1 神经降噪训练数据采集：PRIMAL_NEURAL_CAPTURE=1 时激活（模块自加载）。
    std::unique_ptr<NeuralCaptureModule> neural_capture_module_;
    // M1 神经降噪推理：PRIMAL_NEURAL_DENOISE=1 时激活（模块自加载）。
    std::unique_ptr<NeuralDenoiseModule> neural_denoise_module_;
    // R2.1 神经帧生成：PRIMAL_FRAMEGEN=1 时激活（延迟回读插值；默认关）。
    std::unique_ptr<NeuralFrameGenModule> neural_framegen_module_;
    bool framegen_present_{false};
    // NR transformer 推理：PRIMAL_NEURAL_NR=1 时激活（间隔门控；默认关）。
    std::unique_ptr<NeuralNRModule> neural_nr_module_;
    std::unique_ptr<DebugProbeSphereModule> probe_sphere_module_;
    // Pass Debug View：整屏中间资源查看（自加载着色器，加载失败软降级）。
    std::unique_ptr<PassDebugViewModule> pass_debug_view_module_;

    // --- Shader compilation cache ---
    rhi::ShaderHandle shadow_filter_shader_{rhi::handles::INVALID_SHADER};
    rhi::ShaderHandle deferred_vs_{rhi::handles::INVALID_SHADER};
    rhi::ShaderHandle deferred_ps_{rhi::handles::INVALID_SHADER};
    rhi::ShaderHandle blit_vs_{rhi::handles::INVALID_SHADER};
    rhi::ShaderHandle blit_ps_{rhi::handles::INVALID_SHADER};
    rhi::ShaderHandle gi_gather_shader_{rhi::handles::INVALID_SHADER};
    rhi::ShaderHandle fusion_indirect_ps_{rhi::handles::INVALID_SHADER};
    rhi::ShaderHandle fusion_composite_ps_{rhi::handles::INVALID_SHADER};
    rhi::ShaderHandle sc_card_radiance_shader_{rhi::handles::INVALID_SHADER};
    rhi::ShaderHandle sc_probe_irradiance_shader_{rhi::handles::INVALID_SHADER};
    rhi::ShaderHandle sdf_viz_ps_{rhi::handles::INVALID_SHADER};
    rhi::ShaderHandle probe_sphere_vs_{rhi::handles::INVALID_SHADER};
    rhi::ShaderHandle probe_sphere_ps_{rhi::handles::INVALID_SHADER};

    /// When true, the pipeline outputs a fullscreen GlobalSDF ray-march view
    /// instead of the normal FinalBlit scene.  Cheap per-frame toggle.
    bool sdf_visualization_enabled_{false};
    /// M8/T8.5: overlay real-mesh DDGI probe spheres on the final image.
    bool probe_sphere_debug_{false};
    /// A7 视图模式: 0=Shaded, 1=Wireframe (forwarded to GPUDrivenDrawPipeline
    /// in the NaniteSceneRender pass; 2=Unlit reserved).
    u32 view_mode_{0};

    // Geometry debug overlay settings + the frame's view (needed by
    // AddGeometryDebugPass; set at the top of Render/RenderWithCommandBuffer).
    GeometryDebugSettings geometry_debug_settings_{};
    RenderView* frame_view_{nullptr};

    // Toon post-process toggle + params (Step 10.6).
    bool toon_enabled_{false};
    renderpass::ToonParams toon_params_{};

    // TAA sub-pixel jitter gate (default on; A/B togglable at runtime).
    bool taa_jitter_enabled_{true};

    // Offline SDF data source for SDF visualization.
    bool has_offline_sdf_{false};
    // GI 根治 Phase 4: 离线场是否为有符号编码（DDGI 零穿越命中判定）。
    bool offline_sdf_signed_{false};
    u32 offline_sdf_hit_evidence_{0};
    rhi::ResourceHandle offline_sdf_texture_{rhi::handles::INVALID_RESOURCE};
    math::v3 offline_sdf_origin_{0, 0, 0};
    math::v3 offline_sdf_extent_{0, 0, 0};
    u32 offline_sdf_resolution_{0};

    // --- Utility textures ---
    rhi::ResourceHandle black_texture_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle white_texture_{rhi::handles::INVALID_RESOURCE};

    // Explicit candidate CPU geometry retained across deferred pass rebuilds.
    std::vector<u8> ddgi_first_surface_nodes_;
    std::vector<u8> ddgi_first_surface_triangles_;
    std::vector<u8> ddgi_first_native_triangle_mapping_;
    rhi::ResourceHandle ddgi_first_native_positions_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle ddgi_first_native_elements_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle ddgi_first_native_instances_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle ddgi_first_native_materials_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle ddgi_first_native_normal_texture_{rhi::handles::INVALID_RESOURCE};
    rhi::SamplerHandle ddgi_first_native_normal_sampler_{rhi::handles::INVALID_SAMPLER};
    std::vector<u8> ddgi_analytic_first_rho_nodes_;
    std::vector<u8> ddgi_analytic_first_rho_triangles_;
    std::vector<u8> ddgi_analytic_first_rho_mapping_;
    rhi::ResourceHandle ddgi_analytic_first_rho_positions_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle ddgi_analytic_first_rho_elements_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle ddgi_analytic_first_rho_instances_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle ddgi_analytic_first_rho_materials_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle ddgi_analytic_first_rho_albedo_{rhi::handles::INVALID_RESOURCE};
    rhi::SamplerHandle ddgi_analytic_first_rho_sampler_{rhi::handles::INVALID_SAMPLER};

    // T4.6.5 part 37: IBL cached handles. Forwarded to deferred_module_ when
    // it's created by InitializeSubsystems. Allows SetIBLResources to be
    // called before SetLumenConfig.
    rhi::ResourceHandle ibl_irradiance_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle ibl_prefilter_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle ibl_brdf_lut_{rhi::handles::INVALID_RESOURCE};

    // --- PCG SDF readback ---
    pcg::PCGSDFReadbackManager pcg_sdf_readback_;
    bool pcg_sdf_readback_initialized_{false};

    // --- PCG Entity → RenderScene bridge (Phase 3c) ---
    std::vector<id::id_type> pcg_entity_ids_;
    std::vector<u32> pcg_mesh_slot_indices_;

    // --- Static Entity → RenderScene bridge (Phase 3e) ---
    std::vector<id::id_type> static_entity_ids_;
    std::vector<u32> static_mesh_slot_indices_;

    // --- Geometry Entity bridge (Phase 3.5) ---
    std::vector<id::id_type> geometry_entity_ids_;

    void SyncEntitiesToRenderScene(RenderScene& scene);

    // --- Persistent render state ---
    math::m4x4 view_matrix_{};
    math::m4x4 proj_matrix_{};
    // T5.2: previous-frame matrices for SPGI probe reprojection.
    // A10/A11: now consumed by ALL temporal passes (DDGI/SSGI/GTAO too) and
    // guarded by prev_matrices_valid_ + camera-cut detection.
    math::m4x4 prev_view_matrix_{};
    math::m4x4 prev_proj_matrix_{};
    bool prev_matrices_valid_{false};
    bool camera_cut_this_frame_{false};
    bool viewport_size_changed_{false};   // A13/T0.11: consumed next frame
    // P1-B: PRIMAL_GI_PROBE=1 → 世界空间定点 HDR 探针（锁定相机下固定屏幕点）
    bool gi_probe_enabled_{std::getenv("PRIMAL_GI_PROBE") != nullptr};
    // P3 修订: 探针源 = 上一帧 TAA 解析后的最终 HDR 物理纹理（RWCB 链）。
    rhi::ResourceHandle probe_taa_output_phys_{};
    // A14: proxy count at the last successful snapshot extraction — differs
    // from GetInstanceCount() because extraction skips proxies without
    // cluster caches (a permanent gap that must not re-trigger rebuilds).
    u32 snapshot_proxy_count_{0};
    math::v3 camera_position_{};

  public:
    /// A11: true exactly on the frame a camera cut was detected (large
    /// inter-frame view/VP jump) — temporal consumers reset their history.
    bool WasCameraCutThisFrame() const { return camera_cut_this_frame_; }

  private:
    /// A12/T0.10: scene-switch handling — snapshot full rebuild, DDGI probe
    /// field clear, temporal reset, and static probe cache hash validation.
    /// Called by both Render() and BuildRenderGraph() before current_scene_
    /// rebinds; internally no-ops unless the scene pointer actually changed.
    void OnSceneChanged(const RenderScene& scene, bool first_bind);
    const RenderScene* last_bound_scene_{nullptr};

    // Per-frame data
    u64 frameCount_{0};
    bool subsystems_initialized_{false};

    // Convenience accessors for frequently used settings fields
    const PipelineQualityConfig& qc() const { return settings_.quality; }
    const lumen::LumenConfig& lc() const { return settings_.lumen; }
};

} // namespace primal::graphics
