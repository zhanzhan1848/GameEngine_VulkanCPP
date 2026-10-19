#include "StandardRenderPipeline.h"
#include "Graphics/Lumen/DDGIFitGrid.h"
#include "Graphics/RenderPipeline/RenderPasses/ForwardPass.h"
#include "Graphics/RenderPipeline/RenderPasses/PostProcess/SSAOPass.h"
#include "Graphics/RenderPipeline/RenderPasses/PostProcess/BloomPass.h"
#include "Graphics/RenderPipeline/RenderPasses/PostProcess/ToneMappingPass.h"
#include "Graphics/RenderPipeline/RenderPasses/PostProcess/HZBPass.h"
#include "Graphics/RenderPipeline/RenderPasses/PostProcess/VelocityPass.h"
#include "Graphics/RenderPipeline/RenderPasses/PostProcess/LumenSSGIDawnPass.h"
#include "Graphics/RenderPipeline/RenderPasses/PostProcess/SSRPass.h"
#include "Graphics/RenderPipeline/RenderPasses/PostProcess/TAAPass.h"
#include "Graphics/RenderScene.h"
#include "Graphics/RenderView.h"

#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Core/RHIMath.h"
#include "Graphics/RHI/Core/RHIGpuMesh.h"  // T4.3: detail-SDF slot selection
#include "Graphics/Nanite/GPUDrivenDrawPipeline.h"
#include "Graphics/Nanite/GPUCullingPipeline.h"
#include "Graphics/Nanite/NaniteStreamingManager.h"
#include "Graphics/Nanite/NaniteResourceManager.h"
#include "Graphics/Nanite/DepthHistoryManager.h"
#include "Graphics/Nanite/ColorHistoryManager.h"
#include "Graphics/Nanite/HZBSystem.h"
#include "Graphics/Nanite/GlobalSDF.h"
#include "Graphics/PCG/GPU/GPUMesher.h"
#include "Graphics/Scene/RenderSceneSnapshot.h"
#include "Graphics/Baking/LightmapSceneCollector.h"
#include "Graphics/Scene/CameraSyncSystem.h"
#include "Content/ContentToEngine.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"
#include "Components/Entity.h"
#include "Components/Transform.h"
#include "Components/Light.h"
#include "Components/Cluster.h"
#include "EngineAPI/GameEntity.h"
#include "EngineAPI/GameEntity_impl.h"
#include "EngineAPI/LightComponent.h"
#include "Components/Material.h"
#include <iostream>
#include <chrono>
#include <cstring>
#include "Graphics/Utils/ShaderRegistry.h"
#include <fstream>
#include <set>
#include <sstream>
#include <unordered_map>
#include "Engine/Common/CoreLogStream.h"

namespace primal::graphics {

// A4 (T0.4): pack the real scene light list into SurfaceCacheFrameData so the
// Surface Cache LightEval uploads per-slot type/position/radius (keeps the
// point-light path alive). Leaves sc_light_count == 0 when the scene has no
// lights — the pass then falls back to its legacy single directional slot.
// C2.1 (2026-09-10): the deferred screen path multiplies direct light by
// DIRECT_INTENSITY (20.0 — DeferredLighting.frag / DeferredLighting.metal /
// EngineTest DeferredLighting.metal gpuDriven, kept in sync). The SC atlas
// must live in the SAME units, otherwise its sunlit texels sit 20× below the
// visible surface radiance and every probe bounce reads a 1/20-bright world —
// measured as the multi-bounce supply collapse (interior GI 0.7-1.4 vs UE
// 8.9-59.5, SC atlas max texel lum 1.6 vs expected ~13). A/B:
// PRIMAL_SC_DIRECT_SCALE (default follows the shader constant).
static f32 SCDirectIntensityScale() {
    static const f32 scale = [] {
        if (const char* e = std::getenv("PRIMAL_SC_DIRECT_SCALE")) {
            const f32 v = (f32)std::atof(e);
            if (v >= 0.0f && v <= 1000.0f) return v;
        }
        return 50.0f;   // 2026-09-10 十七: 同步 DIRECT_INTENSITY 20→50（C2.1 单位统一）
    }();
    return scale;
}

static void FillSCLightList(lumen::SurfaceCacheFrameData& fd, const RenderScene* scene) {
    fd.sc_light_count = 0;
    if (!scene) return;
    u32 n = 0;
    const f32 directScale = SCDirectIntensityScale();
    for (const auto& rl : scene->GetLights()) {
        if (n >= lumen::SurfaceCacheFrameData::kSCMaxLights) break;
        const f32 intensity = std::max(rl.intensity, 0.0f) * directScale;
        fd.sc_lights[n * 3 + 0] = math::v4{rl.position.x, rl.position.y, rl.position.z, rl.range};
        fd.sc_lights[n * 3 + 1] = math::v4{rl.color.x * intensity, rl.color.y * intensity,
                                           rl.color.z * intensity, 0.0f};
        fd.sc_lights[n * 3 + 2] = math::v4{rl.direction.x, rl.direction.y, rl.direction.z,
                                           rl.type == LightType::Directional ? 1.0f : 0.0f};
        ++n;
    }
    fd.sc_light_count = n;
}

// P1（2026-09-17 sun-audit §7）：单一太阳合同。BuildRenderGraph 与
// RenderWithCommandBuffer 两个 RenderGraph 站点必须从同一填充器取方向与
// 颜色——两处曾漂移（RenderWithCommandBuffer 的 SC 备用色错用
// ddgi_light_color，与主路径 light_color 相差 1.5×）。
//   - 屏幕直射 / SC 灯表与备用色：light_color（真实太阳）；
//   - DDGI 解析探针驱动：ddgi_light_color（历史 1.5× 档，审计 §7 P1：
//     移除前必须先追踪它在 SC hit/miss/fallback 各分支的消费）。
static void FillSurfaceCacheSunLight(lumen::SurfaceCacheFrameData& fd,
                                     const RenderPipelineSettings& settings,
                                     const RenderScene* scene) {
    fd.light_count = 1;
    fd.light_direction = rhi::math::Normalize(settings.lighting.light_direction);
    fd.light_color = {settings.lighting.light_color.x,
                      settings.lighting.light_color.y,
                      settings.lighting.light_color.z};
    FillSCLightList(fd, scene);
}

static void FillDDGISunLight(lumen::DDGICameraData& camData,
                             const RenderPipelineSettings& settings) {
    camData.light_direction = rhi::math::Normalize(settings.lighting.light_direction);
    camData.light_color = {settings.lighting.ddgi_light_color.x,
                           settings.lighting.ddgi_light_color.y,
                           settings.lighting.ddgi_light_color.z};
}

// P0（sun-audit §7）：解析后的有效太阳/相机/pass 尺寸参数一次性打到 stderr
// ——采集脚本捕获 render.log 后并入 capture.json（engine_reported），保证
// metadata 与实际 GPU UBO 数值对应（修复旧采集器环境白名单缺
// PRIMAL_DIRECT_LIGHT/PRIMAL_SC_DIRECT_SCALE 的 provenance 缺口）。
struct SunContractSizes {
    u32 render_w = 0, render_h = 0;
    u32 shadow_map_resolution = 0, shadow_cascades = 0;
    float shadow_extents[4] = {};
    u32 sc_atlas = 0, sc_cards = 0, sc_max_cards = 0;
    u32 ddgi_counts[3] = {};
    float ddgi_spacing = 0.0f;
    bool froxel_enabled = false;
    u32 froxel_grid[3] = {};
};

static void LogSunContractOnce(const RenderPipelineSettings& settings,
                               const math::m4x4& view, const math::m4x4& proj,
                               const SunContractSizes& s, u32 frame) {
    // 第 1 帧导出一次（保证最短采集也有 provenance）；第 30 帧再导出一次
    // 稳态值——首帧时 tone 的上游 HDR 标志与 DDGI volume 尺寸尚未初始化
    // （base 会误报 passthrough 0.26、probe counts 全 0）。脚本取最后一行。
    static bool loggedEarly = false;
    static bool loggedSteady = false;
    if (frame <= 1) {
        if (loggedEarly) return;
        loggedEarly = true;
    } else if (frame >= 30) {
        if (loggedSteady) return;
        loggedSteady = true;
    } else {
        return;
    }
    const auto& l = settings.lighting;
    const math::v3 D = rhi::math::Normalize(l.light_direction);
    // 0 = 显式关档（旧固定 lightSizeUV），如实上报 0 而非夹紧后的下限。
    const f32 diameter = l.sun_angular_diameter_deg > 0.0f
        ? std::min(std::max(l.sun_angular_diameter_deg, 0.05f), 10.0f)
        : 0.0f;
    const auto tone = PostProcess::GetToneProvenance();
    std::fprintf(stderr,
                 "[SunContract] {\"light_direction\":[%.6f,%.6f,%.6f],"
                 "\"to_light\":[%.6f,%.6f,%.6f],"
                 "\"direct_light_color\":[%.6f,%.6f,%.6f],"
                 "\"ddgi_probe_light_color\":[%.6f,%.6f,%.6f],"
                 "\"sc_direct_scale\":%.3f,"
                 "\"sun_angular_diameter_deg\":%.4f,"
                 "\"exposure\":{\"ev\":%.4f,\"base\":%.6f,\"effective\":%.6f,"
                 "\"adapt_enabled\":%s,\"force_aces\":%s,\"force_passthrough\":%s,"
                 "\"contrast\":%.4f,\"saturation\":%.4f,\"vignette\":%.4f,"
                 "\"key\":%.4f,\"bloom_intensity\":%.4f},"
                 "\"camera\":{\"view\":[[%.6f,%.6f,%.6f,%.6f],[%.6f,%.6f,%.6f,%.6f],"
                 "[%.6f,%.6f,%.6f,%.6f],[%.6f,%.6f,%.6f,%.6f]],"
                 "\"projection\":[[%.6f,%.6f,%.6f,%.6f],[%.6f,%.6f,%.6f,%.6f],"
                 "[%.6f,%.6f,%.6f,%.6f],[%.6f,%.6f,%.6f,%.6f]]},"
                 "\"passes\":{\"render\":[%u,%u],"
                 "\"shadow\":{\"map_resolution\":%u,\"cascades\":%u,"
                 "\"extents\":[%.1f,%.1f,%.1f,%.1f]},"
                 "\"sc_atlas\":{\"size\":%u,\"cards\":%u,\"max_cards\":%u},"
                 "\"ddgi\":{\"probe_counts\":[%u,%u,%u],\"probe_spacing\":%.4f},"
                 "\"froxel\":{\"enabled\":%s,\"grid\":[%u,%u,%u]}}}\n",
                 D.x, D.y, D.z, -D.x, -D.y, -D.z,
                 l.light_color.x, l.light_color.y, l.light_color.z,
                 l.ddgi_light_color.x, l.ddgi_light_color.y, l.ddgi_light_color.z,
                 SCDirectIntensityScale(), diameter,
                 tone.exposure_ev, tone.base_exposure, tone.exposure,
                 tone.adapt_enabled ? "true" : "false",
                 tone.force_aces ? "true" : "false",
                 tone.force_passthrough ? "true" : "false",
                 tone.contrast, tone.saturation, tone.vignette, tone.key,
                 tone.bloom_intensity,
                 view.columns[0].x, view.columns[0].y, view.columns[0].z, view.columns[0].w,
                 view.columns[1].x, view.columns[1].y, view.columns[1].z, view.columns[1].w,
                 view.columns[2].x, view.columns[2].y, view.columns[2].z, view.columns[2].w,
                 view.columns[3].x, view.columns[3].y, view.columns[3].z, view.columns[3].w,
                 proj.columns[0].x, proj.columns[0].y, proj.columns[0].z, proj.columns[0].w,
                 proj.columns[1].x, proj.columns[1].y, proj.columns[1].z, proj.columns[1].w,
                 proj.columns[2].x, proj.columns[2].y, proj.columns[2].z, proj.columns[2].w,
                 proj.columns[3].x, proj.columns[3].y, proj.columns[3].z, proj.columns[3].w,
                 s.render_w, s.render_h,
                 s.shadow_map_resolution, s.shadow_cascades,
                 s.shadow_extents[0], s.shadow_extents[1], s.shadow_extents[2], s.shadow_extents[3],
                 s.sc_atlas, s.sc_cards, s.sc_max_cards,
                 s.ddgi_counts[0], s.ddgi_counts[1], s.ddgi_counts[2], s.ddgi_spacing,
                 s.froxel_enabled ? "true" : "false",
                 s.froxel_grid[0], s.froxel_grid[1], s.froxel_grid[2]);
}

// 采集点收集 pass 尺寸真值（const 访问器，null 安全）。
static SunContractSizes GatherSunContractSizes(const RenderPipelineSettings& settings,
                                               u32 render_w, u32 render_h,
                                               const ShadowMapOutputs* shadowOut,
                                               const lumen::SurfaceCachePass* scPass, u32 frame,
                                               const lumen::LumenDDGIPass* ddgiPass) {
    SunContractSizes s{};
    s.render_w = render_w;
    s.render_h = render_h;
    if (shadowOut) {
        s.shadow_map_resolution = shadowOut->shadow_map_resolution;
        s.shadow_cascades = shadowOut->shadow_cascade_count;
        for (u32 c = 0; c < 4; ++c) s.shadow_extents[c] = shadowOut->shadow_cascade_extents[c];
    }
    if (scPass && scPass->IsInitialized()) {
        const auto view = scPass->GetCacheView(frame);
        s.sc_atlas = view.atlas_size;
        s.sc_cards = view.card_count;
        s.sc_max_cards = view.max_card_count;
    }
    if (ddgiPass && ddgiPass->IsInitialized()) {
        const auto& vol = ddgiPass->GetVolumeData();
        s.ddgi_counts[0] = vol.ProbeCounts[0];
        s.ddgi_counts[1] = vol.ProbeCounts[1];
        s.ddgi_counts[2] = vol.ProbeCounts[2];
        s.ddgi_spacing = vol.ProbeSpacing;
    }
    s.froxel_enabled = settings.quality.enable_froxel_fog;
    s.froxel_grid[0] = settings.froxel.grid_width;
    s.froxel_grid[1] = settings.froxel.grid_height;
    s.froxel_grid[2] = settings.froxel.grid_depth;
    return s;
}

// Bloom is a reference-scene presentation effect while the Vulkan HDR path is
// the only path whose input units are known to match the bright-pass threshold.
// 2026-09-17 用户反馈：bloom 观感过强（合成增益 4.0 的大范围光晕把受光区
// 洗白、掩盖间接光结构），默认关闭——PRIMAL_BLOOM=1 显式开启，0/未设=关。
static bool ShouldEnableBloom(const rhi::RHIDeviceBase& device) {
    if (const char* e = std::getenv("PRIMAL_BLOOM")) {
        if (std::strcmp(e, "0") == 0) return false;
        if (std::strcmp(e, "1") == 0) return true;
    }
    (void)device;
    return false;
}

// P3（sponza-gap-audit §4.9）: bright-pass 参数接线——Bloom.frag 的
// threshold/knee/energy 已改为 UBO，由这里从环境变量构造（默认 = 旧标定
// 常数 4/1/0.2，行为零变化）。合成强度见 ToneMappingPass 的
// PRIMAL_BLOOM_INTENSITY。
static PostProcess::BloomParams MakeBloomParams() {
    PostProcess::BloomParams p{};
    auto envF = [](const char* name, float def) {
        if (const char* e = std::getenv(name)) {
            char* end = nullptr;
            float v = std::strtof(e, &end);
            if (end && *end == '\0' && v >= 0.0f) return v;
        }
        return def;
    };
    p.threshold = envF("PRIMAL_BLOOM_THRESHOLD", p.threshold);
    p.knee      = envF("PRIMAL_BLOOM_KNEE", p.knee);
    p.energy    = envF("PRIMAL_BLOOM_ENERGY", p.energy);
    // 2026-09-16: 渐进宽模糊链旋钮（BloomParams 新字段）。passes=1 回到旧
    // 单次 σ2 行为；sigmaBase 缩放整条 σ={2,3,5,8} 链（默认 2.0=名义值）。
    if (const char* e = std::getenv("PRIMAL_BLOOM_PASSES")) {
        int v = (int)std::strtof(e, nullptr);
        if (v >= 1 && v <= 4) p.blurPasses = v;
    }
    p.blurSigmaBase = envF("PRIMAL_BLOOM_SIGMA", p.blurSigmaBase);
    return p;
}

// A12/T0.10: cheap scene identity hash — FNV-1a over each proxy's mesh/entity
// ids and world translation bits. Not cryptographic; enough to reject loading
// (or keep) a static probe cache that was baked for a different scene.
static u32 ComputeSceneHash(const RenderScene& scene) {
    u32 h = 2166136261u;
    auto mix = [&h](u32 v) {
        h ^= v;
        h *= 16777619u;
    };
    auto mixFloat = [&mix](float f) {
        u32 bits; std::memcpy(&bits, &f, sizeof(bits));   // simd elements have no address
        mix(bits);
    };
    const auto& proxies = scene.GetProxies();
    mix((u32)proxies.size());
    for (const auto& p : proxies) {
        mix((u32)p.meshId);
        mix((u32)p.entityId);
        mixFloat(p.transform.columns[3].x);
        mixFloat(p.transform.columns[3].y);
        mixFloat(p.transform.columns[3].z);
    }
    return h ? h : 1u;
}

void StandardRenderPipeline::OnSceneChanged(const RenderScene& scene, bool first_bind) {
    // Per-frame guards: identical scene pointer → nothing to do. The flag
    // args tell us WHY we may act (first ever bind vs live switch) but the
    // pointer identity is the actual switch test.
    if (!first_bind && last_bound_scene_ == &scene) return;

    // A12/T0.10: the three GI caches (snapshot / DDGI probes / static probe
    // cache) previously survived scene switches untouched — old probe fields
    // and other-scene caches polluted the new scene's first frames.
    if (scene_snapshot_) {
        scene_snapshot_->MarkFullRebuild();
    }
    snapshot_proxy_count_ = 0;   // re-arm the A14 growth check
    gi_content_ready_frame_ = 0xFFFFFFFFu;
    sc_registration_done_ = false;
    sc_registration_deferred_frames_ = 0;
    sc_gate_diag_logged_ = false;

    if (ddgi_pass_ && ddgi_pass_->IsInitialized()) {
        ddgi_pass_->ClearProbeField();
    }
    // [WRC] 场景切换同步清场（A12 同语义：不继承旧场景 GI）。
    for (u32 vi = 0; vi < wrc_volume_count_; ++vi) {
        if (wrc_volumes_[vi] && wrc_volumes_[vi]->IsInitialized()) {
            wrc_volumes_[vi]->ClearProbeField();
        }
    }

    // Static probe cache validation (A12): reject a cache baked for a
    // different scene. hash==0 on either side (legacy cache / empty scene)
    // skips validation.
    if (static_probe_volume_ && static_probe_volume_->IsLoaded()) {
        const u32 cacheHash = static_probe_volume_->SceneHash();
        const u32 sceneHash = ComputeSceneHash(scene);
        if (cacheHash != 0 && sceneHash != 0 && cacheHash != sceneHash) {
            static_probe_volume_->InvalidateLoaded();
            PRIMAL_LOG_STREAM(Info, "Render")
                << "[Lumen] Static probe cache rejected by scene hash (cache="
                << cacheHash << " scene=" << sceneHash
                << ") — DDGI re-seeds from trace." << std::endl;
        }
    }

    if (screen_probe_pass_ && screen_probe_pass_->IsInitialized()) {
        screen_probe_pass_->RequestTemporalReset();
    }
    camera_cut_this_frame_ = true;   // switch is a cut for TAA purposes

    PRIMAL_LOG_STREAM(Info, "Render")
        << "[Lumen] Scene " << (first_bind ? "bound" : "switch")
        << " — snapshot rebuild + GI probe reset queued." << std::endl;
    last_bound_scene_ = &scene;
}

void StandardRenderPipeline::TryLatchGIContentReady() {
    if (gi_content_ready_frame_ != 0xFFFFFFFFu) return;

    const bool sdfReady = (has_offline_sdf_ && offline_sdf_texture_ != rhi::handles::INVALID_RESOURCE)
        || nanite::GlobalSDF::Get().IsVoxelizationReady();
    const bool snapshotStable = !scene_snapshot_ || !scene_snapshot_->NeedsFullRebuild();
    const bool cardsReady = !surface_cache_pass_ || sc_registration_done_;
    if (!sdfReady || !snapshotStable || !cardsReady) return;

    const u32 readyFrame = static_cast<u32>(frameCount_);
    gi_content_ready_frame_ = readyFrame;

    // A late async card registration must start from the same temporal state
    // as a warm-cache frame-0 registration. Otherwise already-settled DDGI
    // confidence/open-sky data drives SC/DDGI into a different feedback basin.
    for (u32 vi = 0; vi < ddgi_volume_count_; ++vi) {
        if (ddgi_volumes_[vi] && ddgi_volumes_[vi]->IsInitialized()) {
            ddgi_volumes_[vi]->ClearProbeField();
            ddgi_volumes_[vi]->NotifyLightingChanged(readyFrame);
        }
    }
    for (u32 vi = 0; vi < wrc_volume_count_; ++vi) {
        if (wrc_volumes_[vi] && wrc_volumes_[vi]->IsInitialized())
            wrc_volumes_[vi]->ClearProbeField();
    }
    if (surface_cache_pass_ && surface_cache_pass_->IsInitialized())
        surface_cache_pass_->RequestTileValidityReset();
    if (screen_probe_pass_ && screen_probe_pass_->IsInitialized())
        screen_probe_pass_->RequestTemporalReset();

    PRIMAL_LOG_STREAM(Info, "Render")
        << "[Lumen] GI content-ready at frame " << readyFrame
        << " — temporal caches reset; GI-local warmup begins." << std::endl;
}



using namespace rhi;
using namespace rhi::math;
using namespace rendergraph;

RenderPipeline* RenderPipeline::s_instance = nullptr;

// Descriptor update helper (same pattern as module .cpp files)
struct DescData {
    u32 binding;
    DescriptorType type;
    ResourceHandle resource;
    u32 count = 1;
};

static void UpdateDesc(RHIDeviceBase* device, DescriptorSetHandle set, const DescData* params, u32 count) {
    std::vector<WriteDescriptorSet> writes(count);
    std::vector<DescriptorImageInfo> imageInfos(count);
    std::vector<DescriptorBufferInfo> bufferInfos(count);

    for (u32 i = 0; i < count; ++i) {
        writes[i].dstSet = set;
        writes[i].dstBinding = params[i].binding;
        writes[i].descriptorCount = params[i].count;
        writes[i].descriptorType = params[i].type;

        if (params[i].type == DescriptorType::UniformBuffer || params[i].type == DescriptorType::StorageBuffer) {
            bufferInfos[i].buffer = params[i].resource;
            bufferInfos[i].offset = 0;
            bufferInfos[i].range = ~0ull;
            writes[i].bufferInfo = &bufferInfos[i];
        } else if (params[i].type == DescriptorType::SampledImage || params[i].type == DescriptorType::StorageImage) {
            imageInfos[i].imageView = params[i].resource;
            imageInfos[i].imageLayout = ResourceState::ShaderResource;
            writes[i].imageInfo = &imageInfos[i];
        } else if (params[i].type == DescriptorType::Sampler) {
            imageInfos[i].sampler = static_cast<SamplerHandle>(params[i].resource);
            writes[i].imageInfo = &imageInfos[i];
        }
    }
    device->UpdateDescriptorSets(count, writes.data());
}

// ============================================================================
// X3/A6: GI 五轴配置（GIFiveAxisConfig）→ 现有运行时机制接线。
// 红线：零描述符/RG 结构变化；axes 默认值组合映射后与既有 enable_* 完全一致。
// ============================================================================

// 轴1（空间缓存组合）→ pass 开关覆写：axes 是轴视图单一真源。必须在模块创建
// 前生效——SetLumenConfig 内、InitializeSubsystems 之前调用（gi_gather/fusion/
// sc_ddgi 模块与 InitializeLumenPasses 均按 enable_* 创建）。晚于此的
// SetQualityOverride / SetPassEnabled / UpdateSettings 属用户显式覆盖，优先于
// 轴默认值且不回写 axes（TestModularPipeline 依赖此覆盖序：SetLumenConfig 后
// SetQualityOverride(true, true) 开 SPGI——若在 InitializeLumenPasses 内覆写会
// 把用户覆盖打回轴值）。
static void ApplyGIAxis1_SpaceCaches(RenderPipelineSettings& s) {
    s.quality.enable_surface_cache = s.lumen.axes.use_surface_cache;
    s.quality.enable_ddgi          = s.lumen.axes.use_ddgi_volume;
    s.quality.enable_screen_probes = s.lumen.axes.use_screen_probes;
}

// 轴5（时域策略）→ 三缓存模块 SetTemporalStrategy（X2 已立枚举存储；EMA =
// 现状默认，各模块内自带 early-return，默认档调用为纯 no-op；EMAVarianceClamp/
// Reprojection 的 kernel 分支随 M6/T6.3 CB 化）。
// 轴2（directional SH2|SH3）/轴3（dfg_path Analytic|PreintegratedLUT）：
// config 字段已在 LumenConfig.axes 落位，kernel 分支点随 M6/T6.3 CB 化
// （TODO）——本任务不做 shader 改动。
// 轴1 在 SetLumenConfig 覆写；轴4（传播拓扑边）在两处 Render 路径的
// SC→DDGI AddPass 与 SPGI SetDDGIFallbackData 按 axes 条件门控。
static void ApplyGIaxes(const lumen::GIFiveAxisConfig& axes,
                        const std::array<std::unique_ptr<lumen::LumenDDGIPass>, 4>& ddgi_volumes,
                        u32 ddgi_volume_count,
                        const std::unique_ptr<lumen::SurfaceCachePass>& surface_cache,
                        const std::unique_ptr<lumen::ScreenProbeGIPass>& screen_probes) {
    for (u32 vi = 0; vi < ddgi_volume_count; ++vi)
        if (ddgi_volumes[vi]) ddgi_volumes[vi]->SetTemporalStrategy(axes.temporal);
    if (surface_cache) surface_cache->SetTemporalStrategy(axes.temporal);
    if (screen_probes) screen_probes->SetTemporalStrategy(axes.temporal);
}

// ============================================================================
// Lifecycle
// ============================================================================

StandardRenderPipeline::~StandardRenderPipeline() {
    Shutdown();
}

bool StandardRenderPipeline::Initialize(RHIDeviceBase* device) {
    if (!device) return false;
    device_ = device;
    s_instance = this;
    renderGraph_ = std::make_unique<RenderGraph>(*device);

    gpuOptimizer_ = std::make_unique<rhi::RHIGPUOptimizer>(*device);
    if (!gpuOptimizer_->Initialize()) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[StandardPipeline] GPU Optimizer init failed (non-critical)" << std::endl;
    }

    // Create utility black texture
    // NOTE: Metal heap textures are NOT zero-initialized (contents undefined —
    // the allocator recycles freed memory). The "black" fallback MUST be
    // explicitly cleared or the SPGI/AO/SSGI fallback slots sample recycled
    // garbage. Same proven upload pattern as the FusionCompositeModule volume
    // identity texture.
    {
        TextureDesc desc{};
        desc.size = {1, 1, 1};
        desc.format = DataFormat::RGBA16_Float;
        desc.usage = TextureUsage::ShaderResource | TextureUsage::CopyDest;
        black_texture_ = device->CreateTexture(desc);

        if (black_texture_ != handles::INVALID_RESOURCE) {
            BufferDesc zDesc{};
            zDesc.size = 256;  // Metal minimum buffer allocation
            zDesc.memoryUsage = GPUMemoryUsage::Staging;
            auto staging = device->CreateBuffer(zDesc);
            if (staging != handles::INVALID_RESOURCE) {
                if (void* mapped = device->MapBuffer(staging)) {
                    memset(mapped, 0, 256);
                    device->UnmapBuffer(staging);
                }
                CommandBufferHandle cmdH = device->CreateCommandBuffer(CommandQueueType::Graphics);
                if (cmdH != handles::INVALID_COMMAND_BUFFER) {
                    RHICommandBuffer* cmd = GetCommandBuffer(cmdH);
                    if (cmd && cmd->Begin()) {
                        BufferTextureCopyRegion cr{};
                        cr.imageSubresource = {0, 0, 1};
                        cr.imageExtent = {1, 1, 1};
                        cmd->CopyBufferToTexture(staging, black_texture_, &cr, 1);
                        // One-time transition UNDEFINED → ShaderResource.
                        // FusionComposite samples black_texture_ as fallback
                        // when SSGI/DDGI/SPGI/AO aren't populated; without
                        // this, validation fires VUID-vkCmdDraw-None-09600.
                        ResourceBarrier b{};
                        b.resource = black_texture_;
                        b.beforeState = ResourceState::UnorderedAccess;
                        b.afterState = ResourceState::ShaderResource;
                        b.subresource = RHI_ALL_SUBRESOURCES;
                        b.queueFamily = 0xFFFFFFFF;
                        cmd->InsertBarrier(&b, 1);
                        cmd->End();
                        QueueSubmitInfo si{};
                        si.cmdBuffer = cmdH;
                        device->Submit(si);
                        cmd->WaitForCompletion();
                    }
                    device->DestroyCommandBuffer(cmdH);
                }
                device->DestroyBuffer(staging);
            }
        }
    }

    return true;
}

void StandardRenderPipeline::Shutdown() {
    if (s_instance == this) s_instance = nullptr;
    ShutdownSubsystems();
    ShutdownLumenPasses();

    // Release file-static PostProcess singletons (Bloom's BlurPass holds a
    // persistently-mapped param buffer) while device_ is still alive — their
    // static destructors run at program exit, after the device is destroyed.
    PostProcess::ShutdownBloomPass();

    if (black_texture_ != handles::INVALID_RESOURCE && device_) {
        device_->DestroyTexture(black_texture_);
        black_texture_ = handles::INVALID_RESOURCE;
    }
    if (gpuOptimizer_) {
        gpuOptimizer_->Shutdown();
        gpuOptimizer_.reset();
    }
    renderGraph_.reset();
    device_ = nullptr;
}

// ============================================================================
// Config
// ============================================================================

bool StandardRenderPipeline::ReloadShader(ShaderHandle shader, const void* data, u32 size) {
    if (!device_ || !data || size == 0) return false;
    return device_->ReloadShader(shader, data, size);
}

bool StandardRenderPipeline::SetDDGIFirstSurfaceGeometry(const void* nodes, u64 nodeBytes,
                                                        const void* triangles, u64 triangleBytes) {
    const char* requested=std::getenv("DDGI_FIRST_SURFACE");
    const char* zeroRequested=std::getenv("DDGI_FIRST_ZERO_CROSS");
    const bool fullRequest=requested && std::strcmp(requested,"1")==0;
    const bool zeroRequest=zeroRequested && std::strcmp(zeroRequested,"1")==0;
    // The pass validates mutual exclusion and scalar mode before enabling.
    // Stage the same bounded geometry for either independent opt-in variant.
    if (!device_ || device_->GetPlatform()!=RHIPlatform::Vulkan ||
        (!fullRequest && !zeroRequest) || (fullRequest && zeroRequest) ||
        !nodes || !triangles || !nodeBytes || !triangleBytes ||
        nodeBytes%48u || triangleBytes%48u || nodeBytes>48u*(1ull<<20) || triangleBytes>48u*(1ull<<22)) {
        PRIMAL_LOG_STREAM(Error,"Render") << "[DDGI_FIRST_SURFACE] pipeline staging rejected";
        return false;
    }
    // UpdateSettings normally rebuilds passes inside the first Render call.
    // Preserve normal initialization timing; do not install into a retired pass.
    const auto* n=static_cast<const u8*>(nodes);const auto* tr=static_cast<const u8*>(triangles);
    ddgi_first_surface_nodes_.assign(n,n+nodeBytes);
    ddgi_first_surface_triangles_.assign(tr,tr+triangleBytes);
    if (!settings_dirty_) {
        for (u32 vi=0;vi<ddgi_volume_count_;++vi)
            if (!ddgi_volumes_[vi] || !ddgi_volumes_[vi]->SetFirstSurfaceGeometry(n,nodeBytes,tr,triangleBytes)) return false;
    }
    PRIMAL_LOG_STREAM(Info,"Render") << "[DDGI_FIRST_SURFACE] current scene geometry staged for actual DDGI volumes";
    return true;
}

bool StandardRenderPipeline::SetDDGIFirstNativeNormalSources(
    rhi::ResourceHandle positions, rhi::ResourceHandle elements,
    rhi::ResourceHandle instances, rhi::ResourceHandle materials,
    rhi::ResourceHandle normalTexture, rhi::SamplerHandle normalSampler,
    const void* triangleMapping, u64 triangleMappingBytes) {
    const char* firstSurface = std::getenv("DDGI_FIRST_SURFACE");
    const char* nativeNormal = std::getenv("DDGI_FIRST_NATIVE_NORMAL");
    if (!device_ || device_->GetPlatform() != RHIPlatform::Vulkan ||
        !firstSurface || std::strcmp(firstSurface, "1") != 0 ||
        !nativeNormal || std::strcmp(nativeNormal, "1") != 0 ||
        normalTexture == handles::INVALID_RESOURCE ||
        normalSampler == handles::INVALID_SAMPLER || !triangleMapping ||
        triangleMappingBytes == 0 || (triangleMappingBytes % 16u) != 0u) {
        PRIMAL_LOG_STREAM(Error, "Render")
            << "[DDGI_FIRST_NATIVE_NORMAL] pipeline staging rejected" << std::endl;
        return false;
    }

    const auto* mapping = static_cast<const u8*>(triangleMapping);
    ddgi_first_native_triangle_mapping_.assign(mapping, mapping + triangleMappingBytes);
    ddgi_first_native_positions_ = positions;
    ddgi_first_native_elements_ = elements;
    ddgi_first_native_instances_ = instances;
    ddgi_first_native_materials_ = materials;
    ddgi_first_native_normal_texture_ = normalTexture;
    ddgi_first_native_normal_sampler_ = normalSampler;

    // UpdateSettings rebuilds actual DDGI instances during the next frame.
    // Keep the source identity staged in this pipeline and apply it to every
    // live volume only when no rebuild is pending.
    // GPUDrivenDraw creates the four global geometry buffers on its first
    // scene execution, after this LoadScene hook.  Keep the exact mapping and
    // normal-array identity staged when those borrowed handles are not live
    // yet; the NaniteSceneRender callback below refreshes them immediately
    // after the current scene's Execute().
    if (!settings_dirty_ && positions != handles::INVALID_RESOURCE &&
        elements != handles::INVALID_RESOURCE && instances != handles::INVALID_RESOURCE &&
        materials != handles::INVALID_RESOURCE) {
        for (u32 vi = 0; vi < ddgi_volume_count_; ++vi) {
            if (!ddgi_volumes_[vi] ||
                !ddgi_volumes_[vi]->SetFirstNativeNormalSources(
                    positions, elements, instances, materials, normalTexture,
                    normalSampler, triangleMapping, triangleMappingBytes)) {
                PRIMAL_LOG_STREAM(Error, "Render")
                    << "[DDGI_FIRST_NATIVE_NORMAL] actual volume setup failed vi="
                    << vi << std::endl;
                return false;
            }
        }
    }
    PRIMAL_LOG_STREAM(Info, "Render")
        << "[DDGI_FIRST_NATIVE_NORMAL] source identity staged for actual DDGI volumes"
        << (positions == handles::INVALID_RESOURCE ? " (awaiting GPU geometry)" : "")
        << " mapping_bytes=" << triangleMappingBytes << std::endl;
    return true;
}

bool StandardRenderPipeline::SetDDGIAnalyticFirstRhoSources(
    const void* nodes, u64 nodeBytes, const void* triangles, u64 triangleBytes,
    rhi::ResourceHandle positions, rhi::ResourceHandle elements,
    rhi::ResourceHandle instances, rhi::ResourceHandle materials,
    rhi::ResourceHandle albedoTexture, rhi::SamplerHandle albedoSampler,
    const void* triangleMapping, u64 triangleMappingBytes) {
    const char* apply = std::getenv("DDGI_ANALYTIC_FIRST_RHO");
    const char* position = std::getenv("DDGI_ANALYTIC_FIRST_POSITION");
    const char* prefix = std::getenv("DDGI_ANALYTIC_FIRST_RHO_PREFIX");
    const char* dump = std::getenv("DDGI_DUMP");
    const bool requested =
        (apply && std::strcmp(apply, "1") == 0) ||
        (position && (std::strcmp(position, "1") == 0 ||
                      std::strcmp(position, "0") == 0)) ||
        (prefix && prefix[0] && dump && std::strcmp(dump, "2") == 0);
    if (!device_ || device_->GetPlatform() != rhi::RHIPlatform::Vulkan || !requested ||
        !nodes || !triangles || nodeBytes == 0 || triangleBytes == 0 ||
        nodeBytes % 48u || triangleBytes % 48u ||
        nodeBytes > 48u * (1ull << 20) || triangleBytes > 48u * (1ull << 22) ||
        !triangleMapping || triangleMappingBytes == 0 || triangleMappingBytes % 32u ||
        positions == handles::INVALID_RESOURCE || elements == handles::INVALID_RESOURCE ||
        instances == handles::INVALID_RESOURCE || materials == handles::INVALID_RESOURCE ||
        albedoTexture == handles::INVALID_RESOURCE ||
        albedoSampler == handles::INVALID_SAMPLER) {
        PRIMAL_LOG_STREAM(Error, "Render")
            << "[DDGI_ANALYTIC_FIRST_RHO] pipeline staging rejected" << std::endl;
        return false;
    }
    const auto* n = static_cast<const u8*>(nodes);
    const auto* tr = static_cast<const u8*>(triangles);
    const auto* mapping = static_cast<const u8*>(triangleMapping);
    ddgi_analytic_first_rho_nodes_.assign(n, n + nodeBytes);
    ddgi_analytic_first_rho_triangles_.assign(tr, tr + triangleBytes);
    ddgi_analytic_first_rho_mapping_.assign(mapping, mapping + triangleMappingBytes);
    ddgi_analytic_first_rho_positions_ = positions;
    ddgi_analytic_first_rho_elements_ = elements;
    ddgi_analytic_first_rho_instances_ = instances;
    ddgi_analytic_first_rho_materials_ = materials;
    ddgi_analytic_first_rho_albedo_ = albedoTexture;
    ddgi_analytic_first_rho_sampler_ = albedoSampler;
    if (!settings_dirty_ && positions != handles::INVALID_RESOURCE &&
        elements != handles::INVALID_RESOURCE && instances != handles::INVALID_RESOURCE &&
        materials != handles::INVALID_RESOURCE) {
        for (u32 vi = 0; vi < ddgi_volume_count_; ++vi) {
            if (!ddgi_volumes_[vi] ||
                !ddgi_volumes_[vi]->SetAnalyticFirstRhoSources(
                    n, nodeBytes, tr, triangleBytes, positions, elements, instances,
                    materials, albedoTexture, albedoSampler, mapping, triangleMappingBytes)) {
                PRIMAL_LOG_STREAM(Error, "Render")
                    << "[DDGI_ANALYTIC_FIRST_RHO] actual volume setup failed vi="
                    << vi << std::endl;
                return false;
            }
        }
    }
    PRIMAL_LOG_STREAM(Info, "Render")
        << "[DDGI_ANALYTIC_FIRST_RHO] source identity staged for actual DDGI volumes"
        << " nodes=" << nodeBytes / 48u << " triangles=" << triangleBytes / 48u
        << " mapping_bytes=" << triangleMappingBytes << std::endl;
    return true;
}

bool StandardRenderPipeline::RefreshDDGIFirstNativeNormalSources() {
    const char* nativeNormal = std::getenv("DDGI_FIRST_NATIVE_NORMAL");
    if (!nativeNormal || std::strcmp(nativeNormal, "1") != 0)
        return true;
    if (ddgi_first_native_triangle_mapping_.empty()) {
        PRIMAL_LOG_STREAM(Error, "Render")
            << "[DDGI_FIRST_NATIVE_NORMAL] live refresh rejected: triangle mapping is empty"
            << std::endl;
        return false;
    }
    if (ddgi_volume_count_ == 0) {
        PRIMAL_LOG_STREAM(Error, "Render")
            << "[DDGI_FIRST_NATIVE_NORMAL] live refresh rejected: no DDGI volumes"
            << std::endl;
        return false;
    }

    auto& gpuDraw = nanite::GPUDrivenDrawPipeline::Get();
    const auto livePositions = gpuDraw.GetGlobalVertexBuffer();
    const auto liveElements = gpuDraw.GetGlobalElementBuffer();
    const auto liveInstances = gpuDraw.GetGlobalInstanceDataBuffer();
    const auto liveMaterials = gpuDraw.GetMaterialDataBuffer();
    const auto liveNormal = gpuDraw.GetNormalTextureArray();
    const auto liveSampler = gpuDraw.GetTextureSampler();
    if (livePositions == handles::INVALID_RESOURCE ||
        liveElements == handles::INVALID_RESOURCE ||
        liveInstances == handles::INVALID_RESOURCE ||
        liveMaterials == handles::INVALID_RESOURCE ||
        liveNormal == handles::INVALID_RESOURCE ||
        liveSampler == handles::INVALID_SAMPLER) {
        PRIMAL_LOG_STREAM(Error, "Render")
            << "[DDGI_FIRST_NATIVE_NORMAL] live refresh rejected: GPUDrivenDraw sources unavailable"
            << std::endl;
        return false;
    }

    const bool changed =
        ddgi_first_native_positions_ != livePositions ||
        ddgi_first_native_elements_ != liveElements ||
        ddgi_first_native_instances_ != liveInstances ||
        ddgi_first_native_materials_ != liveMaterials ||
        ddgi_first_native_normal_texture_ != liveNormal ||
        ddgi_first_native_normal_sampler_ != liveSampler;
    bool needsRebind = changed;
    for (u32 vi = 0; vi < ddgi_volume_count_; ++vi) {
        if (!ddgi_volumes_[vi] || !ddgi_volumes_[vi]->IsFirstNativeNormalEnabled()) {
            needsRebind = true;
            break;
        }
    }
    if (!needsRebind)
        return true;

    for (u32 vi = 0; vi < ddgi_volume_count_; ++vi) {
        if (!ddgi_volumes_[vi] ||
            !ddgi_volumes_[vi]->SetFirstNativeNormalSources(
                livePositions, liveElements, liveInstances, liveMaterials,
                liveNormal, liveSampler,
                ddgi_first_native_triangle_mapping_.data(),
                ddgi_first_native_triangle_mapping_.size())) {
            PRIMAL_LOG_STREAM(Error, "Render")
                << "[DDGI_FIRST_NATIVE_NORMAL] live source refresh failed vi="
                << vi << std::endl;
            return false;
        }
    }
    ddgi_first_native_positions_ = livePositions;
    ddgi_first_native_elements_ = liveElements;
    ddgi_first_native_instances_ = liveInstances;
    ddgi_first_native_materials_ = liveMaterials;
    ddgi_first_native_normal_texture_ = liveNormal;
    ddgi_first_native_normal_sampler_ = liveSampler;
    PRIMAL_LOG_STREAM(Info, "Render")
        << "[DDGI_FIRST_NATIVE_NORMAL] live sources refreshed after GPUDrivenDraw"
        << " volumes=" << ddgi_volume_count_ << std::endl;
    return true;
}

bool StandardRenderPipeline::RefreshDDGIAnalyticFirstRhoSources() {
    const char* apply = std::getenv("DDGI_ANALYTIC_FIRST_RHO");
    const char* position = std::getenv("DDGI_ANALYTIC_FIRST_POSITION");
    const char* prefix = std::getenv("DDGI_ANALYTIC_FIRST_RHO_PREFIX");
    const char* dump = std::getenv("DDGI_DUMP");
    const bool requested =
        (apply && std::strcmp(apply, "1") == 0) ||
        (position && (std::strcmp(position, "1") == 0 ||
                      std::strcmp(position, "0") == 0)) ||
        (prefix && prefix[0] && dump && std::strcmp(dump, "2") == 0);
    if (!requested) return true;
    if (ddgi_analytic_first_rho_nodes_.empty() ||
        ddgi_analytic_first_rho_triangles_.empty() ||
        ddgi_analytic_first_rho_mapping_.empty()) {
        PRIMAL_LOG_STREAM(Error, "Render")
            << "[DDGI_ANALYTIC_FIRST_RHO] live refresh rejected: candidate geometry/mapping is empty"
            << std::endl;
        return false;
    }
    if (ddgi_volume_count_ == 0) {
        PRIMAL_LOG_STREAM(Error, "Render")
            << "[DDGI_ANALYTIC_FIRST_RHO] live refresh rejected: no DDGI volumes"
            << std::endl;
        return false;
    }
    auto& gpuDraw = nanite::GPUDrivenDrawPipeline::Get();
    const auto livePositions = gpuDraw.GetGlobalVertexBuffer();
    const auto liveElements = gpuDraw.GetGlobalElementBuffer();
    const auto liveInstances = gpuDraw.GetGlobalInstanceDataBuffer();
    const auto liveMaterials = gpuDraw.GetMaterialDataBuffer();
    const auto liveAlbedo = gpuDraw.GetAlbedoTextureArray();
    const auto liveSampler = gpuDraw.GetTextureSampler();
    if (livePositions == handles::INVALID_RESOURCE ||
        liveElements == handles::INVALID_RESOURCE ||
        liveInstances == handles::INVALID_RESOURCE ||
        liveMaterials == handles::INVALID_RESOURCE ||
        liveAlbedo == handles::INVALID_RESOURCE ||
        liveSampler == handles::INVALID_SAMPLER) {
        PRIMAL_LOG_STREAM(Error, "Render")
            << "[DDGI_ANALYTIC_FIRST_RHO] live refresh rejected: GPUDrivenDraw sources unavailable"
            << std::endl;
        return false;
    }
    const bool changed =
        ddgi_analytic_first_rho_positions_ != livePositions ||
        ddgi_analytic_first_rho_elements_ != liveElements ||
        ddgi_analytic_first_rho_instances_ != liveInstances ||
        ddgi_analytic_first_rho_materials_ != liveMaterials ||
        ddgi_analytic_first_rho_albedo_ != liveAlbedo ||
        ddgi_analytic_first_rho_sampler_ != liveSampler;
    bool needsRebind = changed;
    for (u32 vi = 0; vi < ddgi_volume_count_; ++vi) {
        if (!ddgi_volumes_[vi] || !ddgi_volumes_[vi]->IsAnalyticFirstRhoEnabled()) {
            needsRebind = true;
            break;
        }
    }
    if (!needsRebind) return true;
    for (u32 vi = 0; vi < ddgi_volume_count_; ++vi) {
        if (!ddgi_volumes_[vi] ||
            !ddgi_volumes_[vi]->SetAnalyticFirstRhoSources(
                ddgi_analytic_first_rho_nodes_.data(), ddgi_analytic_first_rho_nodes_.size(),
                ddgi_analytic_first_rho_triangles_.data(), ddgi_analytic_first_rho_triangles_.size(),
                livePositions, liveElements, liveInstances, liveMaterials,
                liveAlbedo, liveSampler, ddgi_analytic_first_rho_mapping_.data(),
                ddgi_analytic_first_rho_mapping_.size())) {
            PRIMAL_LOG_STREAM(Error, "Render")
                << "[DDGI_ANALYTIC_FIRST_RHO] live source refresh failed vi="
                << vi << std::endl;
            return false;
        }
    }
    ddgi_analytic_first_rho_positions_ = livePositions;
    ddgi_analytic_first_rho_elements_ = liveElements;
    ddgi_analytic_first_rho_instances_ = liveInstances;
    ddgi_analytic_first_rho_materials_ = liveMaterials;
    ddgi_analytic_first_rho_albedo_ = liveAlbedo;
    ddgi_analytic_first_rho_sampler_ = liveSampler;
    PRIMAL_LOG_STREAM(Info, "Render")
        << "[DDGI_ANALYTIC_FIRST_RHO] live sources refreshed after GPUDrivenDraw"
        << " volumes=" << ddgi_volume_count_ << std::endl;
    return true;
}

void StandardRenderPipeline::SetLumenConfig(const lumen::LumenConfig& config) {
    settings_.lumen = config;
    settings_.quality = PipelineQualityConfig::FromPreset(config.quality);

    // X3/A6 轴1: axes 覆写 pass 开关（轴视图单一真源）——必须先于下方
    // InitializeSubsystems 的模块创建。注意 FromPreset 与 LumenConfigFromPreset
    // 两表按档位保持一致（见 RenderPipelineConfig.cpp 一致性契约注释），此处
    // 覆写在默认路径上是恒等操作；仅当调用方手工构造 LumenConfig 的 axes 与
    // quality 档位不一致时以 axes 为准。
    ApplyGIAxis1_SpaceCaches(settings_);

    // Apply render scale: GPU resolution = logical window size × scale
    // PRIMAL_RENDER_SCALE：离线对比实验用渲染比例覆盖（默认 1.0 零影响）。
    static const float s_renderScaleEnv = [] {
        if (const char* e = std::getenv("PRIMAL_RENDER_SCALE")) {
            const float v = static_cast<float>(std::atof(e));
            if (v > 0.1f && v <= 2.0f) return v;
        }
        return 1.0f;
    }();
    render_width_  = static_cast<u32>(logical_width_  * settings_.quality.render_scale * s_renderScaleEnv);
    render_height_ = static_cast<u32>(logical_height_ * settings_.quality.render_scale * s_renderScaleEnv);

    PRIMAL_LOG_STREAM(Info, "Render") << "[Pipeline] Render scale=" << settings_.quality.render_scale
              << " logical=" << logical_width_ << "x" << logical_height_
              << " render=" << render_width_ << "x" << render_height_ << std::endl;

    if (device_ && !subsystems_initialized_) {
        InitializeSubsystems();
        subsystems_initialized_ = true;
    }
}

void StandardRenderPipeline::SetQualityOverride(bool enable_screen_probes, bool enable_surface_cache) {
    settings_.quality.enable_screen_probes = enable_screen_probes;
    settings_.quality.enable_surface_cache = enable_surface_cache;
    settings_dirty_ = true;
}

// ============================================================================
// Runtime Pass Configuration
// ============================================================================

void StandardRenderPipeline::SetPassEnabled(RenderPassID pass, bool enabled) {
    bool was_enabled = settings_.quality.IsPassEnabled(pass);
    settings_.quality.SetPassEnabled(pass, enabled);
    if (was_enabled != settings_.quality.IsPassEnabled(pass)) {
        settings_dirty_ = true;
        PRIMAL_LOG_STREAM(Info, "Render") << "[Pipeline] Pass '" << GetRenderPassInfo(pass).name
                  << "' " << (enabled ? "enabled" : "disabled") << std::endl;
    }
}

void StandardRenderPipeline::SetIBLResources(rhi::ResourceHandle irradiance,
                                             rhi::ResourceHandle prefilter,
                                             rhi::ResourceHandle brdfLUT) {
    // T4.6.5 part 37: cache handles — apply to deferred module once it exists.
    // If subsystems are already initialized, forward immediately; otherwise
    // InitializeSubsystems will pick them up when it creates the deferred
    // module (call sequence: SetShaderHandles -> SetIBLResources -> SetLumenConfig
    // triggers InitializeSubsystems).
    if (deferred_module_) {
        deferred_module_->SetIBLResources(irradiance, prefilter, brdfLUT);
    }
    // Stash on members for late-init path.
    ibl_irradiance_ = irradiance;
    ibl_prefilter_ = prefilter;
    ibl_brdf_lut_ = brdfLUT;
}

bool StandardRenderPipeline::IsPassEnabled(RenderPassID pass) const {
    return settings_.quality.IsPassEnabled(pass);
}

bool StandardRenderPipeline::IsPassActive(RenderPassID pass) const {
    if (!settings_.quality.IsPassEnabled(pass)) return false;
    switch (pass) {
        case RenderPassID::Shadow:           return shadow_module_ != nullptr;
        case RenderPassID::DeferredLighting: return deferred_module_ != nullptr;
        case RenderPassID::SSAO:             return ssao_pass_ && ssao_pass_->IsInitialized();
        case RenderPassID::DDGI:             return ddgi_pass_ && ddgi_pass_->IsInitialized();
        case RenderPassID::SurfaceCache:     return surface_cache_pass_ && surface_cache_pass_->IsInitialized();
        case RenderPassID::SCDDGIIntegration:return sc_ddgi_module_ && surface_cache_pass_ && ddgi_pass_;
        case RenderPassID::SSGI:             return ssgi_pass_ && ssgi_pass_->IsInitialized();
        case RenderPassID::SSR:              return settings_.quality.enable_ssr;
        case RenderPassID::ScreenProbes:     return screen_probe_pass_ && screen_probe_pass_->IsInitialized();
        case RenderPassID::GIGather:         return gi_gather_module_ && ddgi_pass_ && ddgi_pass_->IsInitialized();
        case RenderPassID::VolumePass:       return (volume_pass_ && volume_pass_->IsInitialized()) ||
                                                     (froxel_fog_pass_ && froxel_fog_pass_->IsInitialized());
        case RenderPassID::VolumeRenderer:   return volume_renderer_ && volume_renderer_->IsInitialized();
        case RenderPassID::FroxelFog:       return froxel_fog_pass_ && froxel_fog_pass_->IsInitialized();
        case RenderPassID::FluidRender:     return fluid_render_pass_ && fluid_render_pass_->IsInitialized();
        case RenderPassID::FusionComposite:  return fusion_module_ != nullptr;
        case RenderPassID::FinalBlit:        return final_blit_module_ != nullptr;
        default: return false;
    }
}

void StandardRenderPipeline::UpdateSettings(const RenderPipelineSettings& settings) {
    settings_ = settings;
    settings_dirty_ = true;

    // Re-derive render dimensions if scale changed
    // PRIMAL_RENDER_SCALE：离线对比实验用渲染比例覆盖（默认 1.0 零影响）。
    static const float s_renderScaleEnv = [] {
        if (const char* e = std::getenv("PRIMAL_RENDER_SCALE")) {
            const float v = static_cast<float>(std::atof(e));
            if (v > 0.1f && v <= 2.0f) return v;
        }
        return 1.0f;
    }();
    render_width_  = static_cast<u32>(logical_width_  * settings_.quality.render_scale * s_renderScaleEnv);
    render_height_ = static_cast<u32>(logical_height_ * settings_.quality.render_scale * s_renderScaleEnv);
}

bool StandardRenderPipeline::ApplyConfigChanges() {
    if (!settings_dirty_) return false;
    settings_dirty_ = false;

    if (subsystems_initialized_) {
        ShutdownLumenPasses();
        InitializeLumenPasses();
    }

    // B2（plan §6.2）：froxel fog 运行时开关/生命周期。Initialize 只在创建时
    // 读一次 grid config——参数编辑后需开关一次（off→on）重建以生效；
    // Lumen-only 的 dirty 不重建已存在的 froxel pass（避免每 tick 重建）。
    if (subsystems_initialized_) {
        if (settings_.quality.enable_froxel_fog && !froxel_fog_pass_) {
            froxel_fog_pass_ = std::make_unique<volume::FroxelFogPass>();
            if (!froxel_fog_pass_->Initialize(device_, render_width_, render_height_, settings_.froxel)) {
                PRIMAL_LOG_STREAM(Error, "Render") << "[FroxelFog] runtime re-init failed" << std::endl;
                froxel_fog_pass_.reset();
            }
        } else if (!settings_.quality.enable_froxel_fog && froxel_fog_pass_) {
            froxel_fog_pass_->Shutdown();
            froxel_fog_pass_.reset();
        }
    }
    return true;
}

// ============================================================================
// Subsystem Init / Shutdown
// ============================================================================

void StandardRenderPipeline::SetEditorMode(bool enabled) {
    // PassDebugView 激活期间强制留在完整渲染图路径：轻量前向没有 GBuffer/
    // SSAO/GI 资源，回编辑器前向会让 debug 视图静默失效（典型场景：退出
    // Play 时 EngineViewport3D 重设 editor mode）。PASS=Off 后恢复正常。
    if (enabled && debug_view_ != static_cast<u32>(PassDebugView::Off)) {
        PRIMAL_LOG_STREAM(Info, "Render")
            << "[Pipeline] SetEditorMode(true) ignored — PassDebugView active ("
            << (GetPassDebugViewName(debug_view_) ? GetPassDebugViewName(debug_view_) : "?")
            << "); set PassDebugView=Off to restore editor forward mode" << std::endl;
        return;
    }
    editor_mode_ = enabled;
    if (forward_renderer_) forward_renderer_->SetEditorMode(enabled);
}

void StandardRenderPipeline::SetPassDebugView(u32 view) {
    if (view >= static_cast<u32>(PassDebugView::Count)) {
        PRIMAL_LOG_STREAM(Error, "Render")
            << "[Pipeline] SetPassDebugView: invalid view id " << view
            << " (0.." << static_cast<u32>(PassDebugView::Count) - 1 << ")" << std::endl;
        return;
    }
    if (debug_view_ == view) return;
    debug_view_ = view;
    debug_view_fallback_warned_ = false;  // 新视图重新允许一次回退提示
    if (view != static_cast<u32>(PassDebugView::Off)) {
        // debug 视图需要完整渲染图（GBuffer/Nanite/SSAO/GI）；轻量前向路径
        // 没有这些资源。SetEditorMode 在视图激活期间会被守卫忽略。
        editor_mode_ = false;
        if (forward_renderer_) forward_renderer_->SetEditorMode(false);
    } else {
        editor_mode_ = true;
        if (forward_renderer_) forward_renderer_->SetEditorMode(true);
    }
    PRIMAL_LOG_STREAM(Info, "Render")
        << "[Pipeline] PassDebugView → "
        << (GetPassDebugViewName(view) ? GetPassDebugViewName(view) : "?") << std::endl;
}

// ============================================================================
// 编辑器路径着色器自加载（无 SetShaderHandles 引导时的最小装配）
// ============================================================================
// 背景：编辑器（EngineDLL CreateStandardRenderPipeline）从不调用
// SetShaderHandles——blit/deferred/gi_gather/fusion 系句柄全部 INVALID，
// RG 输出链（含 FinalBlit）在编辑器 Render() 路径下从未真正装配过（运行时
// player 由构建系统注入，测试壳自装，均不受影响）。2026-09-20 pass view
// 功能首次暴露。此装配只补 INVALID 的句柄，显式注入仍优先。
// 模式仿 ForwardSceneRenderer::LoadShaderSource（Metal: MSL 源 + include
// 内联；Vulkan: 预编译 .spv）。刻意复制不共享——ForwardSceneRenderer 的
// helper 为文件内 static。

static std::string SelfLoadReadFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return {};
    std::stringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

static std::string SelfLoadResolve(const std::string& rel) {
    // 复用现成的 CWD 无关解析（PRIMAL_SHADER_ROOT → cwd 相对 → 自 cwd 与
    // exe 目录向上逐级）。
    return utils::ShaderRegistry::ResolveShaderPath(rel);
}

// 精简 include 内联（#include "X.metal" 递归展开，相对当前文件目录）。
static void SelfLoadInlineIncludes(std::string& source, const std::string& dir,
                                   std::set<std::string>& included, int depth) {
    if (depth > 8) return;
    std::string result;
    result.reserve(source.size() * 2);
    std::string::size_type pos = 0;
    while (true) {
        std::string::size_type lineEnd = source.find('\n', pos);
        std::string line = source.substr(pos,
            lineEnd == std::string::npos ? std::string::npos : lineEnd - pos);
        std::string trimmed = line;
        trimmed.erase(0, trimmed.find_first_not_of(" \t"));
        if (trimmed.rfind("#include", 0) != 0 || trimmed.find('<') != std::string::npos) {
            result += line;
            result += '\n';
        } else {
            auto q0 = trimmed.find('"');
            auto q1 = trimmed.rfind('"');
            if (q0 != std::string::npos && q1 > q0) {
                std::string name = trimmed.substr(q0 + 1, q1 - q0 - 1);
                if (included.insert(name).second) {
                    std::string inc = SelfLoadReadFile(dir + name);
                    SelfLoadInlineIncludes(inc, dir, included, depth + 1);
                    result += inc;
                    result += '\n';
                }
            }
        }
        if (lineEnd == std::string::npos) break;
        pos = lineEnd + 1;
    }
    source = std::move(result);
}

static std::vector<u8> SelfLoadShaderSource(const char* file, rhi::RHIPlatform platform,
                                            ShaderStage stage, const char* metalDir) {
    if (platform == rhi::RHIPlatform::Vulkan) {
        const char* suffix = (stage == ShaderStage::Vertex) ? ".vert.spv"
                           : (stage == ShaderStage::Pixel) ? ".frag.spv" : ".spv";
        std::string rel = std::string("Engine/Graphics/Vulkan/shaders/") + file + suffix;
        std::string path = SelfLoadResolve(rel);
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f.is_open()) return {};
        std::streamsize size = f.tellg();
        f.seekg(0);
        std::vector<u8> bytes(static_cast<size_t>(size));
        if (!f.read(reinterpret_cast<char*>(bytes.data()), size)) return {};
        return bytes;  // SPIR-V，entry "main"
    }
    // Metal：MSL 源 + include 内联。目录由装配表给（DeferredLighting 系在
    // EngineTest/shaders/——与测试壳同源的完整版；Lumen compute 在引擎
    // 部署目录 Engine/Graphics/Metal/shaders/Lumen/）。
    std::string rel = std::string("EngineTest/shaders/") + metalDir + file;
    std::string path = SelfLoadResolve(rel);
    if (std::ifstream probe(path); !probe.good()) {
        // EngineTest/shaders 缺该文件时回退引擎部署目录。
        path = SelfLoadResolve(std::string("Engine/Graphics/Metal/shaders/") + metalDir + file);
    }
    std::string source = SelfLoadReadFile(path);
    if (source.empty()) return {};
    std::string dir = path.substr(0, path.find_last_of("/\\") + 1);
    std::set<std::string> included;
    included.insert(file);
    SelfLoadInlineIncludes(source, dir, included, 0);
    return std::vector<u8>(source.begin(), source.end());
}

// The auxiliary gather shader is deliberately opt-in and exact-match.  A
// value such as "10" or "true" must not silently switch the receiver audit
// pipeline on or change the production shader selection.
static bool IsExactGlossySourceAuditRequest() {
    const char* e = std::getenv("DDGI_GLOSSY_SOURCE_AUDIT");
    return e && e[0] == '1' && e[1] == '\0';
}

void StandardRenderPipeline::SelfLoadPipelineShaders() {
    if (!device_) return;
    const rhi::RHIPlatform platform = device_->GetPlatform();
    const bool metal = (platform == rhi::RHIPlatform::Metal);
    auto load = [&](const char* vulkanFile, const char* metalFile, const char* metalDir,
                    const char* metalEntry, ShaderStage stage, ShaderHandle& out) {
        if (out != handles::INVALID_SHADER) return;
        auto bytes = SelfLoadShaderSource(metal ? metalFile : vulkanFile, platform,
                                          stage, metalDir);
        if (bytes.empty()) {
            PRIMAL_LOG_STREAM(Error, "Render")
                << "[Pipeline] self-load source not found: "
                << (metal ? metalFile : vulkanFile) << std::endl;
            return;
        }
        out = device_->CreateShader(bytes.data(), bytes.size(), stage,
                                    metal ? metalEntry : "main");
        if (out == handles::INVALID_SHADER) {
            PRIMAL_LOG_STREAM(Error, "Render")
                << "[Pipeline] CreateShader failed: " << metalFile << " / " << metalEntry
                << " (bytes=" << bytes.size() << ")" << std::endl;
        }
    };

    // 装配表与测试壳一致（TestModularPipeline Metal 表 / TestVulkanSponzaRenderGraph Vulkan 表）。
    if (metal) {
        load("Forward/DeferredLighting", "DeferredLighting.metal", "", "vertexMain",
             ShaderStage::Vertex, blit_vs_);
        deferred_vs_ = blit_vs_;
        load("Forward/DeferredLighting", "DeferredLighting.metal", "", "fragmentLighting_gpuDriven",
             ShaderStage::Pixel, deferred_ps_);
        load("Forward/Blit", "DeferredLighting.metal", "", "fragmentBlitFXAA",
             ShaderStage::Pixel, blit_ps_);
        load("Lumen/DDGIGIGather.comp", "DDGIGIGather.metal", "Lumen/", "ddgi_gi_gather",
             ShaderStage::Compute, gi_gather_shader_);
        load("Lumen/FusionIndirect", "DeferredLighting.metal", "", "fragmentFusionIndirect",
             ShaderStage::Pixel, fusion_indirect_ps_);
        load("Lumen/FusionComposite", "DeferredLighting.metal", "", "fragmentFusion",
             ShaderStage::Pixel, fusion_composite_ps_);
        load("Lumen/DDGICardRadianceAvg.comp", "DDGICardRadianceAvg.metal", "Lumen/",
             "ddgi_card_radiance_avg", ShaderStage::Compute, sc_card_radiance_shader_);
        load("Lumen/DDGIProbeIrradianceFromCards.comp", "DDGIProbeIrradianceFromCards.metal", "Lumen/",
             "ddgi_probe_irradiance_from_cards", ShaderStage::Compute, sc_probe_irradiance_shader_);
    } else if (platform == rhi::RHIPlatform::Vulkan) {
        load("Forward/DeferredLighting", nullptr, nullptr, "deferred_vs", ShaderStage::Vertex, deferred_vs_);
        load("Forward/DeferredLighting", nullptr, nullptr, "deferred_ps", ShaderStage::Pixel, deferred_ps_);
        load("Forward/Blit", nullptr, nullptr, "blit_vs", ShaderStage::Vertex, blit_vs_);
        load("Forward/Blit", nullptr, nullptr, "blit_ps", ShaderStage::Pixel, blit_ps_);
        load("Lumen/DDGIGIGather.comp", nullptr, nullptr, "gi_gather", ShaderStage::Compute, gi_gather_shader_);
        load("Lumen/FusionIndirect", nullptr, nullptr, "fusion_indirect", ShaderStage::Pixel, fusion_indirect_ps_);
        load("Lumen/FusionComposite", nullptr, nullptr, "fusion_composite", ShaderStage::Pixel, fusion_composite_ps_);
        load("Lumen/DDGICardRadianceAvg.comp", nullptr, nullptr, "sc_card_radiance",
             ShaderStage::Compute, sc_card_radiance_shader_);
        load("Lumen/DDGIProbeIrradianceFromCards.comp", nullptr, nullptr, "sc_probe_irradiance",
             ShaderStage::Compute, sc_probe_irradiance_shader_);
    }
    u32 loaded = 0;
    for (ShaderHandle h : {deferred_vs_, deferred_ps_, blit_vs_, blit_ps_,
                           gi_gather_shader_, fusion_indirect_ps_,
                           fusion_composite_ps_, sc_card_radiance_shader_,
                           sc_probe_irradiance_shader_}) {
        if (h != handles::INVALID_SHADER) ++loaded;
    }
    PRIMAL_LOG_STREAM(Info, "Render")
        << "[Pipeline] self-loaded pipeline shaders: " << loaded << "/9" << std::endl;
}

void StandardRenderPipeline::InitializeSubsystems() {
#ifndef __EMSCRIPTEN__
    // 编辑器路径（无 SetShaderHandles 引导）先自加载 RG 必需着色器，
    // 否则 blit/deferred/gi_gather/fusion 系模块全部 INVALID、RG 输出链空转。
    SelfLoadPipelineShaders();
    rhi::ShaderHandle glossySourceAuditShader = rhi::handles::INVALID_SHADER;
    if (device_->GetPlatform() == rhi::RHIPlatform::Vulkan &&
        IsExactGlossySourceAuditRequest()) {
        const char* gatherAuditPrefix = std::getenv("DDGI_GATHER_AUDIT");
        if (!gatherAuditPrefix || !gatherAuditPrefix[0]) {
            PRIMAL_LOG_STREAM(Error, "Render")
                << "[DDGI_GLOSSY_SOURCE_AUDIT] requested without DDGI_GATHER_AUDIT prefix"
                << std::endl;
        } else {
            // Keep the normal gather CS loaded above.  The source variant is
            // a separate opt-in module shader; a missing variant stays invalid
            // so GIGatherModule fails closed instead of silently using the
            // normal CS.
            auto bytes = SelfLoadShaderSource("Lumen/DDGIGIGather_source.comp",
                                              device_->GetPlatform(),
                                              ShaderStage::Compute, nullptr);
            if (!bytes.empty()) {
                glossySourceAuditShader = device_->CreateShader(
                    bytes.data(), bytes.size(), ShaderStage::Compute, "ddgi_gi_gather");
            }
            if (glossySourceAuditShader == rhi::handles::INVALID_SHADER) {
                PRIMAL_LOG_STREAM(Error, "Render")
                    << "[DDGI_GLOSSY_SOURCE_AUDIT] requested source shader is unavailable"
                    << std::endl;
            }
        }
    }
    // --- Meshlet/Nanite subsystems (not yet ported to WASM/Dawn WGSL) ---
    // The WFC demo runs in editor mode (ForwardSceneRenderer path) which is
    // self-contained and does not touch any of the subsystems below. Skipping
    // their init avoids cascading pipeline-creation failures from missing
    // meshlet WGSL shaders (ClusterBinning, VisibilityBuffer, etc.).
    auto& gpuDraw = nanite::GPUDrivenDrawPipeline::Get();
    if (!gpuDraw.IsInitialized()) {
        nanite::BinningConfig binningConfig{};
        nanite::VisibilityBufferConfig visConfig{};
        visConfig.width = render_width_;
        visConfig.height = render_height_;
        gpuDraw.Initialize(device_, binningConfig, visConfig);
    }

    // Culling pipeline (singleton)
    culling_pipeline_ = &nanite::GPUCullingPipeline::Get();
    if (!culling_pipeline_->IsInitialized()) {
        culling_pipeline_->Initialize(device_);
        culling_pipeline_->SetGPUDrawPipeline(&gpuDraw);
    }
    gpuDraw.SetCullingPipeline(culling_pipeline_);

    // HZB
    hzb_system_ = std::make_unique<nanite::HZBSystem>();
    nanite::HZBSystem::Config hzbCfg;
    hzbCfg.max_width = render_width_;
    hzbCfg.max_height = render_height_;
    hzbCfg.min_mip_size = 8;
    hzbCfg.generate_on_gpu = true;
    hzb_system_->Initialize(device_, hzbCfg);
    gpuDraw.SetHZBSystem(hzb_system_.get());
    culling_pipeline_->SetHZBSystem(hzb_system_.get());

    // Streaming manager
    streaming_manager_ = new nanite::NaniteStreamingManager();
    nanite::NaniteStreamingConfig streamCfg{};
    streaming_manager_->Initialize(device_, streamCfg);

    // Resource manager (needed by RenderSceneSnapshot::ExtractSceneData)
    auto& resourceManager = nanite::NaniteResourceManager::Get();
    resourceManager.Initialize(device_);
    streaming_manager_->SetNaniteResourceManager(&resourceManager);

    // Depth/Color history — use default config then override fields
    depth_history_ = std::make_unique<nanite::DepthHistoryManager>();
    {
        nanite::DepthHistoryManager::Config cfg;
        cfg.width = render_width_;
        cfg.height = render_height_;
        depth_history_->Initialize(device_, cfg);
    }

    color_history_ = std::make_unique<nanite::ColorHistoryManager>();
    {
        nanite::ColorHistoryManager::Config cfg;
        cfg.width = render_width_;
        cfg.height = render_height_;
        color_history_->Initialize(device_, cfg);
    }

    // Scene snapshot
    scene_snapshot_ = std::make_unique<RenderSceneSnapshot>();
    scene_snapshot_->Initialize(device_);

    // Global SDF — use larger cascades to cover the full Sponza scene.
    auto& globalSDF = nanite::GlobalSDF::Get();
    if (!globalSDF.IsInitialized()) {
        // SDF cascade tuned for Sponza: voxel 1.0 × res 80 = 80 units cascade 0
        nanite::GlobalSDFConfig sdfConfig;
        sdfConfig.cascade_count = 3;
        sdfConfig.base_resolution = 80;
        sdfConfig.cascade_scale_factor = 2;
        sdfConfig.voxel_size_base = 1.0f;
        globalSDF.Initialize(device_, sdfConfig);
    }

    // Initialize GlobalSDF voxelization pipeline from Nanite mesh buffers.
    // This enables DDGI SDF ray tracing — without it, SDF cascades are empty
    // and DDGI trace produces no directional indirect light.
    if (globalSDF.IsInitialized() && gpuDraw.IsInitialized()) {
        nanite::SDFVoxelizationResources voxResources;
        voxResources.vertex_buffer = gpuDraw.GetGlobalVertexBuffer();
        voxResources.meshlet_buffer = gpuDraw.GetGlobalMeshletBuffer();
        voxResources.meshlet_vertices_buffer = gpuDraw.GetGlobalMeshletVerticesBuffer();
        voxResources.meshlet_triangles_buffer = gpuDraw.GetGlobalMeshletTrianglesBuffer();
        voxResources.cluster_map_buffer = gpuDraw.GetClusterMapBuffer();
        voxResources.instance_data_buffer = gpuDraw.GetGlobalInstanceDataBuffer();
        voxResources.num_instances = 0;
        PRIMAL_LOG_STREAM(Error, "Render") << "[SDF_INIT] vb=" << voxResources.vertex_buffer
                  << " mb=" << voxResources.meshlet_buffer
                  << " cmb=" << voxResources.cluster_map_buffer
                  << " ready_before=" << globalSDF.IsVoxelizationReady() << std::endl;
        globalSDF.InitVoxelization(voxResources);
        PRIMAL_LOG_STREAM(Error, "Render") << "[SDF_INIT] ready_after=" << globalSDF.IsVoxelizationReady() << std::endl;
    }

    // PCG SDF readback — matches cascade 0 resolution
    if (globalSDF.IsInitialized()) {
        pcg_sdf_readback_initialized_ = pcg_sdf_readback_.Initialize(device_, globalSDF.GetCascade(0).resolution);
    }

    // GPU Mesher (Phase 9.3a) — singleton bound to device; PCG MarchingCubesNode
    // queries IsReady() and falls back to CPU if false.
    pcg::GPUMesher::Get().Initialize(device_);

    // --- Shaders are NOT compiled here ---
    // The caller must set shader handles via SetShaderHandles() before Render().
    // Modules that receive INVALID_SHADER handles will skip their passes gracefully.

    // --- Initialize pipeline modules ---
    shadow_module_ = std::make_unique<ShadowMapModule>();
    shadow_module_->Initialize(device_, &gpuDraw, render_width_, render_height_, 1000, 10000);
    if (shadow_filter_shader_ != handles::INVALID_SHADER) {
        shadow_module_->InitializeShadowFilter(shadow_filter_shader_);
    }

    // Planar reflection (Mirror) — fails soft: if shader loading fails the
    // module stays invalid and the frame simply skips the mirror passes.
    reflection_module_ = std::make_unique<PlanarReflectionModule>();
    if (!reflection_module_->Initialize(device_, &gpuDraw, 10000)) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[StandardRenderPipeline] PlanarReflection init failed — mirror disabled" << std::endl;
        reflection_module_.reset();
    }

    deferred_module_ = std::make_unique<DeferredLightingModule>();
    deferred_module_->Initialize(device_, deferred_vs_, deferred_ps_, render_width_, render_height_);
    // T4.6.5 part 37: forward cached IBL handles (if SetIBLResources was
    // called before SetLumenConfig).
    deferred_module_->SetIBLResources(ibl_irradiance_, ibl_prefilter_, ibl_brdf_lut_);

    final_blit_module_ = std::make_unique<FinalBlitModule>();
    final_blit_module_->Initialize(device_, blit_vs_, blit_ps_);

    // Pass Debug View：着色器自加载（编辑器路径无 SetShaderHandles 引导），
    // 加载失败软降级——debug 视图整体禁用，正常出图不受影响。
    pass_debug_view_module_ = std::make_unique<PassDebugViewModule>();
    if (!pass_debug_view_module_->Initialize(device_)) {
        PRIMAL_LOG_STREAM(Error, "Render")
            << "[StandardPipeline] PassDebugView module unavailable — pass debug views disabled" << std::endl;
        pass_debug_view_module_.reset();
    }

    // PRIMAL_PASS_DEBUG=<name>（off/albedo/normal/.../sc_atlas）：headless 测试
    // 与复现用启动旋钮（UI/CLI 走 PipelineSetPassDebugView）。未知名告警忽略。
    if (const char* e = std::getenv("PRIMAL_PASS_DEBUG")) {
        const s32 v = ParsePassDebugViewName(e);
        if (v > 0) {
            SetPassDebugView(static_cast<u32>(v));
        } else if (v < 0) {
            PRIMAL_LOG_STREAM(Error, "Render")
                << "[StandardPipeline] PRIMAL_PASS_DEBUG='" << e << "' is not a valid view name" << std::endl;
        }
    }

    // SDF visualization module (toggled at runtime via SetSDFVisualization).
    // Reuses the fullscreen triangle vertex shader (blit_vs_).
    if (sdf_viz_ps_ != handles::INVALID_SHADER) {
        sdf_viz_module_ = std::make_unique<SDFVisualizationModule>();
        sdf_viz_module_->Initialize(device_, blit_vs_, sdf_viz_ps_);
    }

    // M0 神经渲染 debug 链路：PRIMAL_NEURAL_DEBUG=1 时激活。模块自加载
    // shader 字节码与 .pnn 权重资产；env 未设置时零资源零开销。
    if (const char* nd = std::getenv("PRIMAL_NEURAL_DEBUG"); nd && nd[0] == '1') {
        auto module = std::make_unique<NeuralDebugModule>();
        if (module->Initialize(device_, render_width_, render_height_)) {
            neural_debug_module_ = std::move(module);
            neural_debug_enabled_ = true;
        }
    }

    // M1 神经降噪训练数据采集：PRIMAL_NEURAL_CAPTURE=1 时激活（自加载，
    // take 完成后自动 idle）。
    if (const char* nc = std::getenv("PRIMAL_NEURAL_CAPTURE"); nc && nc[0] == '1') {
        auto module = std::make_unique<NeuralCaptureModule>();
        if (module->Initialize(device_, render_width_, render_height_)) {
            neural_capture_module_ = std::move(module);
        }
    }

    // M1 神经降噪推理：PRIMAL_NEURAL_DENOISE=1 时激活（自加载，纯推理）。
    if (const char* ndn = std::getenv("PRIMAL_NEURAL_DENOISE"); ndn && ndn[0] == '1') {
        auto module = std::make_unique<NeuralDenoiseModule>();
        if (module->Initialize(device_, render_width_, render_height_)) {
            neural_denoise_module_ = std::move(module);
        }
    }

    // R2.1 神经帧生成：PRIMAL_FRAMEGEN=1 时激活（shim 自加载；shim 内部走
    // MLX/Metal，但数据面是 CPU 缓冲，引擎两个后端均可用）。
    if (const char* fg = std::getenv("PRIMAL_FRAMEGEN"); fg && fg[0] == '1') {
        auto module = std::make_unique<NeuralFrameGenModule>();
        if (module->Initialize(device_, render_width_, render_height_)) {
            neural_framegen_module_ = std::move(module);
            const char* fp = std::getenv("PRIMAL_FRAMEGEN_PRESENT");
            framegen_present_ = fp && fp[0] == '1';
        }
    }

    // NR transformer 推理：PRIMAL_NEURAL_NR=1 时激活（间隔门控；默认关）。
    if (const char* nr = std::getenv("PRIMAL_NEURAL_NR"); nr && nr[0] == '1') {
        auto module = std::make_unique<NeuralNRModule>();
        if (module->Initialize(device_, render_width_, render_height_)) {
            neural_nr_module_ = std::move(module);
        }
    }

    if (settings_.quality.enable_ddgi && gi_gather_shader_ != handles::INVALID_SHADER) {
        gi_gather_module_ = std::make_unique<GIGatherModule>();
        const bool gatherInitialized = gi_gather_module_->Initialize(
            device_, gi_gather_shader_, render_width_, render_height_,
            glossySourceAuditShader);
        if (!gatherInitialized) {
            PRIMAL_LOG_STREAM(Error, "Render")
                << "[GIGather] initialization failed; gather path disabled"
                << std::endl;
            gi_gather_module_->Shutdown();
            gi_gather_module_.reset();
        }
    }

    // M8/T8.5: real-mesh DDGI probe sphere overlay (SetProbeSphereDebug /
    // SPGI_PROBE_SPHERES=1 in the Sponza test).
    // B1 L3: probe 球 overlay——句柄未注入时模块自加载字节码（编辑器路径），
    // 测试注入路径不变。
    probe_sphere_module_ = std::make_unique<DebugProbeSphereModule>();
    probe_sphere_module_->Initialize(device_, probe_sphere_vs_, probe_sphere_ps_);

    if (settings_.quality.enable_ssgi || settings_.quality.enable_ddgi) {
        fusion_module_ = std::make_unique<FusionCompositeModule>();
        fusion_module_->Initialize(device_, fusion_indirect_ps_, blit_vs_,
                                   fusion_composite_ps_, render_width_, render_height_);
    }

    // Lightmap 旁路（2026-10-19 计划）：独立改写级，模块自装 shader；非
    // Vulkan 平台或 spv 未部署时 Initialize 返回 false、模块不存在——
    // 渲染图与既有路径零差异。仅在 fusion 存在时有意义（改写其间接信号）。
    if (fusion_module_) {
        lightmap_module_ = std::make_unique<LightmapModule>();
        if (!lightmap_module_->Initialize(device_, blit_vs_, render_width_, render_height_)) {
            lightmap_module_.reset();
        } else {
            fusion_module_->SetLightmapHook(lightmap_module_.get());
        }
    }

    if (settings_.quality.enable_surface_cache && settings_.quality.enable_ddgi) {
        sc_ddgi_module_ = std::make_unique<SCDDGIIntegrationModule>();
        sc_ddgi_module_->Initialize(device_, sc_card_radiance_shader_,
                                    sc_probe_irradiance_shader_, render_width_, render_height_);
    }

    // --- Lumen passes ---
    InitializeLumenPasses();
#endif // !__EMSCRIPTEN__

    // --- Forward renderer (editor mode) ---
    forward_renderer_ = std::make_unique<ForwardSceneRenderer>();
    forward_renderer_->Initialize(device_, render_width_, render_height_);
}

void StandardRenderPipeline::ShutdownSubsystems() {
    // GPUMesher shutdown before device goes away.
    pcg::GPUMesher::Get().Shutdown();

    // GlobalSDF holds a data_provider_ whose destructor issues device_->Destroy*
    // calls. Reset it here while device_ is still alive — the singleton's own
    // destructor runs at program exit, after the RHI device is gone, so leaving
    // the provider alive causes a use-after-free in AnalyticSDFProvider::~.
    nanite::GlobalSDF::Get().Shutdown();

    if (pcg_sdf_readback_initialized_) { pcg_sdf_readback_.Shutdown(); pcg_sdf_readback_initialized_ = false; }

    if (sc_ddgi_module_) { sc_ddgi_module_->Shutdown(); sc_ddgi_module_.reset(); }
    if (lightmap_module_) { lightmap_module_->Shutdown(); lightmap_module_.reset(); }
    if (fusion_module_) { fusion_module_->Shutdown(); fusion_module_.reset(); }
    if (gi_gather_module_) { gi_gather_module_->Shutdown(); gi_gather_module_.reset(); }
    if (final_blit_module_) { final_blit_module_->Shutdown(); final_blit_module_.reset(); }
    if (pass_debug_view_module_) { pass_debug_view_module_->Shutdown(); pass_debug_view_module_.reset(); }
    if (sdf_viz_module_) { sdf_viz_module_->Shutdown(); sdf_viz_module_.reset(); }
    if (neural_debug_module_) { neural_debug_module_->Shutdown(); neural_debug_module_.reset(); }
    if (neural_capture_module_) { neural_capture_module_->Shutdown(); neural_capture_module_.reset(); }
    if (neural_denoise_module_) { neural_denoise_module_->Shutdown(); neural_denoise_module_.reset(); }
    if (neural_framegen_module_) { neural_framegen_module_->Shutdown(); neural_framegen_module_.reset(); }
    if (neural_nr_module_) { neural_nr_module_->Shutdown(); neural_nr_module_.reset(); }
    framegen_present_ = false;
    if (probe_sphere_module_) { probe_sphere_module_->Shutdown(); probe_sphere_module_.reset(); }
    if (deferred_module_) { deferred_module_->Shutdown(); deferred_module_.reset(); }
    if (shadow_module_) { shadow_module_->Shutdown(); shadow_module_.reset(); }
    if (reflection_module_) { reflection_module_->Shutdown(); reflection_module_.reset(); }

    // Forward renderer (editor mode)
    if (forward_renderer_) { forward_renderer_->Shutdown(); forward_renderer_.reset(); }

    if (scene_snapshot_) { scene_snapshot_->Shutdown(); scene_snapshot_.reset(); }
    if (color_history_) { color_history_->Shutdown(); color_history_.reset(); }
    if (depth_history_) { depth_history_->Shutdown(); depth_history_.reset(); }
    if (hzb_system_) { hzb_system_->Shutdown(); hzb_system_.reset(); }
    if (streaming_manager_) { streaming_manager_->Shutdown(); delete streaming_manager_; streaming_manager_ = nullptr; }

    // Shutdown singletons (must happen before device shutdown)
    if (culling_pipeline_) { culling_pipeline_->Shutdown(); culling_pipeline_ = nullptr; }
    auto& gpuDraw = nanite::GPUDrivenDrawPipeline::Get();
    if (gpuDraw.IsInitialized()) { gpuDraw.Shutdown(); }

    subsystems_initialized_ = false;
}

void StandardRenderPipeline::RegisterDebugLightmap() {
    if (!lightmap_module_) return;
    lightmap_module_->RegisterDebugLightmap();
}

bool StandardRenderPipeline::BakeSceneLightmaps(const baking::LightmapBakeSettings& settings,
                                                std::string* out_path, std::string* out_err) {
    if (!lightmap_module_) {
        if (out_err) *out_err = "lightmap module unavailable (non-Vulkan or spv missing)";
        return false;
    }
    const RenderScene* scene = current_scene_ ? current_scene_ : &default_scene_;
    if (!scene) {
        if (out_err) *out_err = "no active scene";
        return false;
    }

    baking::CollectedBakeScene collected;
    std::string err;
    if (!baking::CollectBakeScene(*scene, collected, err)) {
        if (out_err) *out_err = "collect: " + err;
        return false;
    }
    const baking::LightmapBakeResult r =
        baking::BakeLightmaps(collected.scene, settings, nullptr, nullptr);
    if (!r.ok) {
        if (out_err) *out_err = "bake: " + r.error;
        return false;
    }
    if (!lightmap_module_->LoadLightmap(r.output_path)) {
        if (out_err) *out_err = "load: " + r.output_path;
        return false;
    }
    if (out_path) *out_path = r.output_path;
    PRIMAL_LOG_STREAM(Info, "Render")
        << "[Lightmap] scene baked -> " << r.output_path << " (" << r.entries.size()
        << " entries, " << r.atlas_width << "x" << r.atlas_height << ")" << std::endl;
    return true;
}

// ============================================================================
// Lumen Pass Init / Shutdown
// ============================================================================

void StandardRenderPipeline::InitializeLumenPasses() {
    if (!device_) return;
    const auto& config = settings_.lumen;

    // Scene-fit probe grid (Docs/completed/2026-09-18-ddgi-scene-fit-probe-grid-plan.md
    // §5.4): when ddgi_fit_to_bounds, the near volume's counts are derived
    // from the coverage box (ceil(extent/spacing)+1 per axis, clamp 4..64)
    // and probe[0] sits on the box's min corner. Pure arithmetic, shared with
    // the unit tests via DDGIFitGrid.h.
    const bool ddgiFit = config.ddgi_fit_to_bounds;
    const lumen::DDGIFitGrid fitGrid = ddgiFit
        ? lumen::DDGIFitGridFromBounds(config.ddgi_fit_bounds_min,
                                       config.ddgi_fit_bounds_max,
                                       config.ddgi_probe_spacing,
                                       config.ddgi_fit_margin)
        : lumen::DDGIFitGrid{};

    // T4.1: capture the base GI budgets before the governor starts scaling them.
    const u32 nearCountX = ddgiFit ? fitGrid.count_x : config.ddgi_probe_count_x;
    const u32 nearCountY = ddgiFit ? fitGrid.count_y : config.ddgi_probe_count_y;
    const u32 nearCountZ = ddgiFit ? fitGrid.count_z : config.ddgi_probe_count_z;
    gi_base_probe_budget_ = nearCountX * nearCountY * nearCountZ;
    gi_base_page_budget_ = config.surface_cache_capture_budget;
    gi_governor_ = {};   // reset to full quality on (re)initialization

    // T4.5/C5: platform capability ceiling — the profile clamps GI budgets so
    // no dispatch exceeds per-thread read limits regardless of config.
    gi_profile_ = lumen::GIPlatformProfile::Detect(device_);
    if (gi_base_probe_budget_ > gi_profile_.max_probes_per_frame)
        gi_base_probe_budget_ = gi_profile_.max_probes_per_frame;
    if (gi_base_page_budget_ > gi_profile_.page_budget)
        gi_base_page_budget_ = gi_profile_.page_budget;

    // T4.6.5 part 33: Lumen suite (DDGI/SSAO/SSGI/SurfaceCache) is deferred on
    // Vulkan — needs SPIR-V ports + non-overlapping descriptor bindings per
    // memory vulkan-rhi-t46-subsystems-probe-findings.md. Silently skip the
    // init call rather than let Initialize() return false + print alarming
    // "init failed" log. The bypassProd flag in ForwardRenderer already keeps
    // production code paths off Vulkan; Lumen modules remain nullptr here.
    const bool isVulkan = (device_->GetPlatform() == rhi::RHIPlatform::Vulkan);

    // DDGI — T5.5 multi-volume runtime (cap 4, config-driven).
    if (settings_.quality.enable_ddgi) {
        // Volume list: explicit overrides win; else derive near + far (T4.4).
        std::vector<lumen::LumenVolumeConfig> vols = config.volume_overrides;
        if (vols.empty()) {
            vols.push_back(lumen::LumenVolumeConfig{});   // near defaults filled below
            if (config.ddgi_enable_far_volume) {
                lumen::LumenVolumeConfig far{};
                far.probe_count_x = config.ddgi_far_probe_count_x;
                far.probe_count_y = config.ddgi_far_probe_count_y;
                far.probe_count_z = config.ddgi_far_probe_count_z;
                far.probe_spacing = config.ddgi_far_probe_spacing;
                far.rays_per_probe = config.ddgi_rays_per_probe;
                far.ray_max_distance = config.ddgi_far_ray_max_distance;
                far.update_budget_divisor = 4;
                vols.push_back(far);
            }
            if (ddgiFit) {
                // Scene-fit near volume: fitted counts + min-corner origin
                // (RTXGI (N-1)×S coverage convention). The origin rides the
                // runtime params — LumenVolumeConfig::origin_override stays
                // reserved for static bakes / gizmo editing (plan §5.1).
                vols[0].probe_count_x = fitGrid.count_x;
                vols[0].probe_count_y = fitGrid.count_y;
                vols[0].probe_count_z = fitGrid.count_z;
            } else {
                vols[0].probe_count_x = config.ddgi_probe_count_x;
                vols[0].probe_count_y = config.ddgi_probe_count_y;
                vols[0].probe_count_z = config.ddgi_probe_count_z;
            }
            vols[0].rays_per_probe = config.ddgi_rays_per_probe;
            vols[0].probe_spacing = config.ddgi_probe_spacing;
            vols[0].ray_max_distance = config.ddgi_ray_max_distance;
        }
        if (vols.size() > 4) vols.resize(4);

        static const char* kPrefixes[4] = {"", "Far", "Vol2", "Vol3"};
        for (u32 vi = 0; vi < vols.size(); ++vi) {
            const auto& vc = vols[vi];
            lumen::DDGIRuntimeParams vp{};
            vp.probe_count_x = vc.probe_count_x;
            vp.probe_count_y = vc.probe_count_y;
            vp.probe_count_z = vc.probe_count_z;
            vp.rays_per_probe = vc.rays_per_probe;
            vp.probe_spacing = vc.probe_spacing;
            vp.irradiance_temporal_weight = config.ddgi_irradiance_temporal_weight;
            vp.depth_temporal_weight = config.ddgi_depth_temporal_weight;
            vp.ray_max_distance = vc.ray_max_distance;
            if (ddgiFit && vi == 0) {
                vp.probe_origin = fitGrid.origin;
                vp.use_probe_origin = true;
            }
            if (vi > 0 && vc.update_budget_divisor > 1)
                vp.max_probes_per_frame = 16384u / vc.update_budget_divisor;
            auto vol = std::make_unique<lumen::LumenDDGIPass>();
            if (!vol->Initialize(device_, vp)) {
                PRIMAL_LOG_STREAM(Error, "Render") << "[Lumen] DDGI volume " << vi << " init failed (skipping)" << std::endl;
                continue;
            }
            vol->SetDynamicMode(true);   // Mode 11: runtime SDF trace
            if (vi > 0) vol->SetRGNamePrefix(kPrefixes[vi]);
            // 能量所有权 Phase 4b: 远场 ×0.5 退场（审计 §2.5——×0.5 不物理，
            // 是"远场只表达低频"的代理；远场天空改由解析天空路径统一供给，
            // 即 finalize 逃逸分支的 EvaluateSkyBase×SkyColor 单一事实源）。
            // env PRIMAL_GI_FAR_SKY 保留 A/B（=0.5 回到旧档）。
            if (vi > 0) {
                float farSky = 1.0f;
                if (const char* e = std::getenv("PRIMAL_GI_FAR_SKY")) {
                    const float v = (float)std::atof(e);
                    if (v > 0.0f && v <= 1.0f) farSky = v;
                }
                vol->SetFarSkyScale(farSky);
            }
            {
                // GI 根治 Phase 4: 有符号粗场旗标（PRIMAL_DDGI_UNSIGNED_TRACE=1 回滚）。
                bool signedField = offline_sdf_signed_;
                if (const char* e = std::getenv("PRIMAL_DDGI_UNSIGNED_TRACE")) {
                    if (e[0] != '0') signedField = false;
                }
                vol->SetCoarseSdfSigned(signedField);
            }
            ddgi_volumes_[ddgi_volume_count_++] = std::move(vol);
            PRIMAL_LOG_STREAM(Info, "Render") << "[Lumen] DDGI volume " << vi << " initialized ("
                      << vp.probe_count_x << "x" << vp.probe_count_y
                      << "x" << vp.probe_count_z << " @ spacing "
                      << vp.probe_spacing << ")" << std::endl;
        }
        if (ddgiFit && ddgi_volume_count_ > 0) {
            const math::v3 fmax{fitGrid.origin.x + fitGrid.covered.x,
                                fitGrid.origin.y + fitGrid.covered.y,
                                fitGrid.origin.z + fitGrid.covered.z};
            PRIMAL_LOG_STREAM(Info, "Render")
                << "[Lumen] DDGI scene-fit grid: fit_min (" << fitGrid.origin.x << ", "
                << fitGrid.origin.y << ", " << fitGrid.origin.z << ") covered "
                << fitGrid.covered.x << "x" << fitGrid.covered.y << "x" << fitGrid.covered.z
                << " (fit_max " << fmax.x << ", " << fmax.y << ", " << fmax.z << "), margin "
                << config.ddgi_fit_margin << ", probes "
                << fitGrid.count_x * fitGrid.count_y * fitGrid.count_z << std::endl;
        }
    }

    // Re-apply the offline SDF source to the (possibly rebuilt) near volume.
    // SetLumenConfig → ApplyConfigChanges tears down and recreates the DDGI
    // passes; without this the offline source set by the host during pipeline
    // Initialize dies with the old instance and the trace silently falls back
    // to the coarse runtime GlobalSDF.
    if (ddgi_pass_ && offline_sdf_texture_ != handles::INVALID_RESOURCE) {
        ddgi_pass_->SetOfflineSDFSource(offline_sdf_texture_, offline_sdf_origin_,
                                        offline_sdf_extent_, offline_sdf_resolution_);
        ddgi_pass_->SetCoarseSdfSigned(offline_sdf_signed_);   // GI 根治 Phase 4
        ddgi_pass_->SetOfflineHitEvidence(offline_sdf_hit_evidence_);
        PRIMAL_LOG_STREAM(Error, "Render") << "[Lumen] Offline SDF re-applied to DDGI trace ("
                  << offline_sdf_resolution_ << "³)" << std::endl;
    }

    if (!ddgi_first_surface_nodes_.empty()) {
        if (ddgi_volume_count_==0) {
            PRIMAL_LOG_STREAM(Error,"Render") << "[DDGI_FIRST_SURFACE] candidate has no active volume";
            std::abort(); // Explicit experiment failure; never silently run the incumbent.
        }
        for (u32 vi=0;vi<ddgi_volume_count_;++vi) {
            if (!ddgi_volumes_[vi] || !ddgi_volumes_[vi]->SetFirstSurfaceGeometry(
                    ddgi_first_surface_nodes_.data(),ddgi_first_surface_nodes_.size(),
                    ddgi_first_surface_triangles_.data(),ddgi_first_surface_triangles_.size())) {
                PRIMAL_LOG_STREAM(Error,"Render") << "[DDGI_FIRST_SURFACE] actual volume setup failed vi=" << vi;
                std::abort(); // Exceptions are disabled in Engine; fail the opt-in experiment.
            }
        }
    }
    if (!ddgi_first_native_triangle_mapping_.empty()) {
        if (ddgi_volume_count_ == 0) {
            PRIMAL_LOG_STREAM(Error, "Render")
                << "[DDGI_FIRST_NATIVE_NORMAL] candidate has no active volume";
            std::abort();
        }
        const bool sourcesReady =
            ddgi_first_native_positions_ != handles::INVALID_RESOURCE &&
            ddgi_first_native_elements_ != handles::INVALID_RESOURCE &&
            ddgi_first_native_instances_ != handles::INVALID_RESOURCE &&
            ddgi_first_native_materials_ != handles::INVALID_RESOURCE;
        if (sourcesReady) {
            for (u32 vi = 0; vi < ddgi_volume_count_; ++vi) {
                if (!ddgi_volumes_[vi] ||
                    !ddgi_volumes_[vi]->SetFirstNativeNormalSources(
                        ddgi_first_native_positions_, ddgi_first_native_elements_,
                        ddgi_first_native_instances_, ddgi_first_native_materials_,
                        ddgi_first_native_normal_texture_,
                        ddgi_first_native_normal_sampler_,
                        ddgi_first_native_triangle_mapping_.data(),
                        ddgi_first_native_triangle_mapping_.size())) {
                    PRIMAL_LOG_STREAM(Error, "Render")
                        << "[DDGI_FIRST_NATIVE_NORMAL] actual volume setup failed vi="
                        << vi << std::endl;
                    std::abort();
                }
            }
        } else {
            PRIMAL_LOG_STREAM(Info, "Render")
                << "[DDGI_FIRST_NATIVE_NORMAL] waiting for GPUDrivenDraw geometry buffers"
                << std::endl;
        }
    }
    if (!ddgi_analytic_first_rho_mapping_.empty()) {
        if (!RefreshDDGIAnalyticFirstRhoSources()) {
            std::abort();
        }
    }

    // ========================================================================
    // [WRC] World Space Radiance Cache（设计文档 §4 Stream A）。
    // [2026-09-20 P4 默认翻转] WRCEnabled() 默认 1（env=0 回退）；High/Ultra
    // 档（或显式 wrc_enable）创建 near + far 两实例。创建门加 Vulkan 平台
    // 守卫——WRC.metal 八件套未移植（parity 后置决策），默认翻转不得把
    // Metal 链推入 shader 缺失路径。独立代码域：创建/接线/销毁全部在本
    // block 内，禁止触碰 DDGI 数据面。
    // ========================================================================
    wrc_volume_count_ = 0;
    const bool wrcPlatformOk = device_ == nullptr ||
        device_->GetPlatform() == rhi::RHIPlatform::Vulkan;
    const bool wrcTierDefault = config.quality == lumen::LumenQualityPreset::High ||
                                config.quality == lumen::LumenQualityPreset::Ultra ||
                                config.quality == lumen::LumenQualityPreset::UltraRT;
    if (wrcPlatformOk && lumen::WRCEnabled() && (config.wrc_enable || wrcTierDefault)) {
        if (!settings_.quality.enable_surface_cache) {
            // 近场命中采样 SC（cache 填充的唯一合法路径）；无 SC 时 WRC 只剩
            // 天空契约，cache 身份不成立——拒绝创建并提示。
            PRIMAL_LOG_STREAM(Error, "Render")
                << "[WRC] Surface Cache disabled — WRC trace would have no SC "
                << "truth layer to cache; WRC not created." << std::endl;
        } else {
            lumen::WRCRuntimeParams nearParams{};
            nearParams.probe_count_x = config.wrc_probe_count_x;
            nearParams.probe_count_y = config.wrc_probe_count_y;
            nearParams.probe_count_z = config.wrc_probe_count_z;
            nearParams.probe_spacing = config.wrc_probe_spacing;
            nearParams.ray_max_distance = config.wrc_ray_max_distance;
            nearParams.update_budget_probes_per_frame = config.wrc_update_budget_probes_per_frame;
            auto nearVol = std::make_unique<lumen::LumenWRCPass>();
            // A5 placement：离线 SDF 已就位时，近场网格从 merger volume 贴合
            //（WRCFitGridFromBounds，probe[0] = fit_min）。SDF 晚于管线创建
            // 到位的场景不做延迟重建（网格容量冻结于 Initialize）——与 DDGI
            // fit+auto 的 settings_dirty_ 重建路径相比是已知收窄，文档记录。
            if (offline_sdf_texture_ != handles::INVALID_RESOURCE && offline_sdf_resolution_ > 0) {
                const math::v3 bmax{ offline_sdf_origin_.x + offline_sdf_extent_.x,
                                     offline_sdf_origin_.y + offline_sdf_extent_.y,
                                     offline_sdf_origin_.z + offline_sdf_extent_.z };
                const lumen::WRCFitGrid wrcFit = lumen::WRCFitGridFromBounds(
                    offline_sdf_origin_, bmax, config.wrc_probe_spacing, 0.0f);
                nearParams.probe_count_x = wrcFit.count_x;
                nearParams.probe_count_y = wrcFit.count_y;
                nearParams.probe_count_z = wrcFit.count_z;
                nearParams.update_budget_probes_per_frame =
                    std::min(nearParams.update_budget_probes_per_frame,
                             wrcFit.count_x * wrcFit.count_y * wrcFit.count_z);
            }
            if (nearVol->Initialize(device_, nearParams)) {
                if (offline_sdf_texture_ != handles::INVALID_RESOURCE && offline_sdf_resolution_ > 0) {
                    const math::v3 bmax{ offline_sdf_origin_.x + offline_sdf_extent_.x,
                                         offline_sdf_origin_.y + offline_sdf_extent_.y,
                                         offline_sdf_origin_.z + offline_sdf_extent_.z };
                    const lumen::WRCFitGrid wrcFit = lumen::WRCFitGridFromBounds(
                        offline_sdf_origin_, bmax, config.wrc_probe_spacing, 0.0f);
                    nearVol->SetVolumeOrigin(wrcFit.origin);
                }
                wrc_volumes_[wrc_volume_count_++] = std::move(nearVol);
            }
            if (config.wrc_enable_far_volume && wrc_volume_count_ > 0) {
                lumen::WRCRuntimeParams farParams{};
                farParams.probe_count_x = config.wrc_far_probe_count_x;
                farParams.probe_count_y = config.wrc_far_probe_count_y;
                farParams.probe_count_z = config.wrc_far_probe_count_z;
                farParams.probe_spacing = config.wrc_far_probe_spacing;
                farParams.ray_max_distance = config.wrc_far_ray_max_distance;
                farParams.update_budget_probes_per_frame =
                    config.wrc_far_probe_count_x * config.wrc_far_probe_count_y * config.wrc_far_probe_count_z;
                auto farVol = std::make_unique<lumen::LumenWRCPass>();
                if (farVol->Initialize(device_, farParams)) {
                    farVol->SetRGNamePrefix("Far");
                    // 远场不采 SC（同 DDGI far 语义：卡片数据是近场真值）。
                    farVol->ClearSurfaceCacheResources();
                    wrc_volumes_[wrc_volume_count_++] = std::move(farVol);
                }
            }
            if (wrc_volume_count_ > 0) {
                PRIMAL_LOG_STREAM(Info, "Render")
                    << "[WRC] enabled: " << wrc_volume_count_ << " volume(s), "
                    << "High/Ultra 世界空间缓存接管 SPGI 远场消费（A3）" << std::endl;
            }
        }
    }
    // 离线 SDF 转发（与 DDGI 同一 host 供给）。
    for (u32 vi = 0; vi < wrc_volume_count_; ++vi) {
        if (wrc_volumes_[vi] && offline_sdf_texture_ != handles::INVALID_RESOURCE) {
            wrc_volumes_[vi]->SetOfflineSDFSource(offline_sdf_texture_, offline_sdf_origin_,
                                                  offline_sdf_extent_, offline_sdf_resolution_);
            wrc_volumes_[vi]->SetCoarseSdfSigned(offline_sdf_signed_);
        }
        if (wrc_volumes_[vi]) {
            wrc_volumes_[vi]->SetPrefilterEnabled(std::getenv("PRIMAL_WRC_PREFILTER") != nullptr);
        }
    }

    // ========================================================================
    // [REFL R4/R5 2026-09-20] 屏幕外 specular 兜底（ReflSpecularTrace）：
    // PRIMAL_REFL=2 启用（v1 仅 SSR+relay；R4 探针档由 PRIMAL_REFL_PROBE=1
    // 叠加）。SC 查询产出率门禁（R1）通过前不建议量产该档——覆盖缺口会
    // 直接成像为反射黑洞。
    // ========================================================================
    refl_specular_pass_.reset();
    if (ReflectionLevel() >= 2u && device_ &&
        device_->GetPlatform() == rhi::RHIPlatform::Vulkan) {
        auto reflPass = std::make_unique<lumen::ReflSpecularTracePass>();
        if (reflPass->Initialize(device_, render_width_, render_height_)) {
            refl_specular_pass_ = std::move(reflPass);
        } else {
            PRIMAL_LOG_STREAM(Warn, "Render")
                << "[REFL] ReflSpecularTrace init failed — running v1 (SSR + relay)" << std::endl;
        }
    }

    // SSAO
    if (settings_.quality.enable_ssao) {
        ssao_pass_ = std::make_unique<lumen::LumenSSAOPass>();
        lumen::SSAOParams ssaoParams{};
        ssaoParams.radius = config.gtao_radius;
        ssaoParams.power = config.gtao_power;
        ssaoParams.direction_count = config.gtao_direction_count;
        ssaoParams.sample_count = config.gtao_sample_count;
        if (!ssao_pass_->Initialize(device_, render_width_, render_height_, ssaoParams)) {
            PRIMAL_LOG_STREAM(Error, "Render") << "[Lumen] SSAO init failed" << std::endl;
            ssao_pass_.reset();
        }
    }

    // SSGI
    if (settings_.quality.enable_ssgi) {
        ssgi_pass_ = std::make_unique<lumen::LumenSSGIPass>();
        lumen::SSGIParams ssgiParams{};
        if (!ssgi_pass_->Initialize(device_, render_width_, render_height_, ssgiParams)) {
            PRIMAL_LOG_STREAM(Error, "Render") << "[Lumen] SSGI init failed" << std::endl;
            ssgi_pass_.reset();
        }
    }

    // Surface Cache
    if (settings_.quality.enable_surface_cache) {
        surface_cache_pass_ = std::make_unique<lumen::SurfaceCachePass>();
        if (!surface_cache_pass_->Initialize(device_, config)) {
            PRIMAL_LOG_STREAM(Error, "Render") << "[Lumen] Surface Cache init failed" << std::endl;
            surface_cache_pass_.reset();
        } else if (has_offline_sdf_) {
            // A config rebuild creates a new SC pass after the host's SDF
            // setter ran. Rebind the same offline field on SUCCESS; the old
            // failure-only branch silently fell back to coarse GlobalSDF.
            surface_cache_pass_->SetOfflineSDFSource(offline_sdf_texture_,
                offline_sdf_origin_, offline_sdf_extent_, offline_sdf_resolution_);
        }
    }

    // Screen Probes
    // TEMP diagnosis: SPGI_OFF=1 disables the screen-probe chain for A/B
    // isolation of the two-state GI flip.
    if (std::getenv("SPGI_OFF")) {
        settings_.quality.enable_screen_probes = false;
    }
    // TEMP diagnosis: SSGI_OFF=1 disables SSGI the same way.
    if (std::getenv("SSGI_OFF")) {
        settings_.quality.enable_ssgi = false;
    }
    if (settings_.quality.enable_screen_probes) {
        screen_probe_pass_ = std::make_unique<lumen::ScreenProbeGIPass>();
        lumen::ScreenProbeParams probeParams{};
        probeParams.downsample_factor = config.screen_probes_spacing;
        probeParams.rays_per_probe = config.screen_probes_rays;
        // T8.13 60FPS: env override — SPGI trace is the top GPU pass (12.6ms);
        // 64→32 rays halves it (F2 importance + spatial filter + denoise
        // absorb the noise; adaptive/bilateral path per M7).
        if (const char* e = std::getenv("SCREEN_PROBE_RAYS")) {
            // 2026-09-17 P1（probe-selection-density-audit §7）：Average 只有
            // 64 lanes、每 lane 读一条——override >64 会只积分前 64 条却按更大
            // raysPerProbe 归一化，能量反而变暗（audit §7 实测警示）。上限钳到
            // 64；更高值需先实现 lane-strided accumulation + 同步归一化。
            u32 requested = std::max(8u, (u32)atoi(e));
            if (requested > 64u) {
                PRIMAL_LOG_STREAM(Error, "Render")
                    << "[Lumen] SCREEN_PROBE_RAYS=" << requested
                    << " clamped to 64 — ScreenProbeAverage integrates 64 lanes; "
                       "larger values would dim the result until lane-strided "
                       "accumulation lands" << std::endl;
                requested = 64u;
            }
            probeParams.rays_per_probe = requested;
        }
        probeParams.enable_jitter = config.screen_probes_jitter;   // F5 (M7)
        probeParams.far_cache_start_distance = config.screen_probes_far_cache_start;  // F1 (M7)
        probeParams.enable_importance = config.screen_probes_importance;  // F2 (M7)
        probeParams.adaptive_pos_threshold = config.screen_probes_adaptive_pos_error;  // F4 (M7)
        // 最终防线：config 路径同样钳 64（Average 64-lane 上限，见上）。
        if (probeParams.rays_per_probe > 64u) {
            PRIMAL_LOG_STREAM(Error, "Render")
                << "[Lumen] screen_probes_rays=" << probeParams.rays_per_probe
                << " clamped to 64 (ScreenProbeAverage 64-lane limit)" << std::endl;
            probeParams.rays_per_probe = 64u;
        }
        // P1（reaudit §4.1/4.2）: 重建后保持诊断模式（测试在 init 前设定）
        screen_probe_pass_->SetTraceDiagMode(spgi_trace_diag_mode_);
        if (!screen_probe_pass_->Initialize(device_, render_width_, render_height_, probeParams)) {
            PRIMAL_LOG_STREAM(Error, "Render") << "[Lumen] Screen Probes init failed" << std::endl;
            screen_probe_pass_.reset();
        } else if (offline_sdf_texture_ != handles::INVALID_RESOURCE) {
            // Same rebuild-survival concern as the DDGI forward above: the
            // SDF far-field trace must run on the high-res offline volume.
            screen_probe_pass_->SetOfflineSDFSource(offline_sdf_texture_,
                                                    offline_sdf_origin_,
                                                    offline_sdf_extent_,
                                                    offline_sdf_resolution_);
            PRIMAL_LOG_STREAM(Error, "Render") << "[Lumen] Offline SDF re-applied to SPGI trace ("
                      << offline_sdf_resolution_ << "³)" << std::endl;
        }
    }

    // Static Probe Volume
    static_probe_volume_ = std::make_unique<lumen::StaticProbeVolume>();
    lumen::StaticProbeParams spParams{};
    spParams.grid_dim_x = config.ddgi_probe_count_x;
    spParams.grid_dim_y = config.ddgi_probe_count_y;
    spParams.grid_dim_z = config.ddgi_probe_count_z;
    spParams.spacing = config.ddgi_probe_spacing;
    if (static_probe_volume_->Initialize(device_, spParams)) {
        const char* probeCachePath = "scene.probe_cache";
        if (static_probe_volume_->LoadFromFile(probeCachePath)) {
            static_probe_volume_->UploadToGPU();
            PRIMAL_LOG_STREAM(Info, "Render") << "[Lumen] Loaded static probe cache: " << probeCachePath << std::endl;
        }
    }

    // Volume Pass (post-process fog)
    if (settings_.quality.enable_volume_pass) {
        volume_pass_ = std::make_unique<volume::VolumePass>();
        volume::VolumeRuntimeParams volumeParams{};
        if (!volume_pass_->Initialize(device_, render_width_, render_height_, volumeParams)) {
            PRIMAL_LOG_STREAM(Error, "Render") << "[Volume] VolumePass init failed" << std::endl;
            volume_pass_.reset();
        }
    }

    // Volume Renderer (forward proxy cube + fragment ray march)
    if (settings_.quality.enable_volume_renderer) {
        volume_renderer_ = std::make_unique<volume::VolumeRenderer>();
        if (!volume_renderer_->Initialize(device_, render_width_, render_height_)) {
            PRIMAL_LOG_STREAM(Error, "Render") << "[Volume] VolumeRenderer init failed" << std::endl;
            volume_renderer_.reset();
        }
    }

    // Froxel Fog (frustum-aligned volumetric fog)
    if (settings_.quality.enable_froxel_fog) {
        froxel_fog_pass_ = std::make_unique<volume::FroxelFogPass>();
        if (!froxel_fog_pass_->Initialize(device_, render_width_, render_height_, settings_.froxel)) {
            PRIMAL_LOG_STREAM(Error, "Render") << "[FroxelFog] Init failed" << std::endl;
            froxel_fog_pass_.reset();
        }
    }

    // Fluid Render (splat-based fluid surface)
    if (settings_.quality.enable_fluid_render) {
        fluid_render_pass_ = std::make_unique<fluid::FluidRenderPass>();
        if (!fluid_render_pass_->Initialize(device_, render_width_, render_height_, settings_.fluid)) {
            PRIMAL_LOG_STREAM(Error, "Render") << "[FluidRender] Init failed" << std::endl;
            fluid_render_pass_.reset();
        }
    }

    // ========================================================================
    // X3/A6: 五轴接线收尾 —— 轴5 时域策略注入三缓存模块（EMA=现状默认，
    // 模块内 early-return，默认档零行为变化/零日志）；轴2（directional）/
    // 轴3（dfg_path）为 kernel 分支轴，字段已落位、分支点随 M6/T6.3 CB 化
    // （TODO，不做 shader 改动）；轴1 已在 SetLumenConfig 覆写 pass 开关；
    // 轴4 传播边在两处 Render 路径按 axes 门控（见 SC→DDGI AddPass 与
    // SPGI SetDDGIFallbackData 调用点）。
    // ========================================================================
    ApplyGIaxes(config.axes, ddgi_volumes_, ddgi_volume_count_,
                surface_cache_pass_, screen_probe_pass_);
}

void StandardRenderPipeline::ShutdownLumenPasses() {
    if (fluid_render_pass_) { fluid_render_pass_->Shutdown(); fluid_render_pass_.reset(); }
    if (froxel_fog_pass_) { froxel_fog_pass_->Shutdown(); froxel_fog_pass_.reset(); }
    if (volume_renderer_) { volume_renderer_->Shutdown(); volume_renderer_.reset(); }
    if (volume_pass_) { volume_pass_->Shutdown(); volume_pass_.reset(); }
    if (static_probe_volume_) { static_probe_volume_->Shutdown(); static_probe_volume_.reset(); }
    if (screen_probe_pass_) { screen_probe_pass_->Shutdown(); screen_probe_pass_.reset(); }
    if (surface_cache_pass_) { surface_cache_pass_->Shutdown(); surface_cache_pass_.reset(); }
    if (ssao_pass_) { ssao_pass_->Shutdown(); ssao_pass_.reset(); }
    for (u32 vi = 0; vi < ddgi_volume_count_; ++vi) {
        if (ddgi_volumes_[vi]) { ddgi_volumes_[vi]->Shutdown(); ddgi_volumes_[vi].reset(); }
    }
    ddgi_volume_count_ = 0;
    // [WRC] 独立销毁（§1.2-2 文件属地禁令：与 DDGI 各自独立 block）。
    for (u32 vi = 0; vi < wrc_volume_count_; ++vi) {
        if (wrc_volumes_[vi]) { wrc_volumes_[vi]->Shutdown(); wrc_volumes_[vi].reset(); }
    }
    wrc_volume_count_ = 0;
}

// ============================================================================
// Per-frame Update
// ============================================================================

void StandardRenderPipeline::UpdatePerFrame(RenderScene& scene, RenderView& view) {
    // === Phase 3: ECS Camera → RenderView 同步 ===
    // 在读 view.GetViewMatrix() 之前先让 ECS Camera 组件覆盖 RenderView 的矩阵。
    // 如果场景里没有 Camera entity，SyncCamerasFromECS no-op，view 保留上游 caller
    // 设置的矩阵（保持旧行为兼容）。
    //
    // Editor camera 旁路：当 view.IsEditorCamera() 为 true 时，跳过 ECS 同步，
    // 否则场景里的 Camera entity 会每帧把 Editor Camera 的 view/projection
    // 覆盖掉，导致 orbit/pan/zoom 在画面上完全不可见。
    if (!view.IsEditorCamera()) {
        scene_sync::SyncCamerasFromECS(view, view.GetSceneCameraEntityId());
    }

    // T5.2: retain last frame's matrices for SPGI probe reprojection before
    // overwriting with the current camera.
    prev_view_matrix_ = view_matrix_;
    prev_proj_matrix_ = proj_matrix_;
    view_matrix_ = view.GetViewMatrix();
    proj_matrix_ = view.GetProjectionMatrix();

    // M1 采集多机位：按当前 take 应用相机偏移（PRIMAL_NEURAL_CAPTURE_CAM_PATH）。
    // 偏移在切镜检测之前生效——take 间瞬移自然触发 camera_cut，采集统计与
    // SPGI/TAA 时序历史随之复位。
    if (neural_capture_module_ && neural_capture_module_->IsInitialized() &&
        neural_capture_module_->IsActive()) {
        const math::v3 off = neural_capture_module_->GetTakeCameraOffset();
        if (off.x != 0.0f || off.y != 0.0f || off.z != 0.0f) {
            // view' = T(-off)·view：平移列 -= R·off（R 的列即 view 前 3 列）。
            const v4& c0 = view_matrix_.columns[0];
            const v4& c1 = view_matrix_.columns[1];
            const v4& c2 = view_matrix_.columns[2];
            view_matrix_.columns[3].x -= c0.x * off.x + c1.x * off.y + c2.x * off.z;
            view_matrix_.columns[3].y -= c0.y * off.x + c1.y * off.y + c2.y * off.z;
            view_matrix_.columns[3].z -= c0.z * off.x + c1.z * off.y + c2.z * off.z;
            camera_position_.x += off.x;
            camera_position_.y += off.y;
            camera_position_.z += off.z;
        }
    }

    // A10/A11: camera-cut detection. A large inter-frame jump in the view
    // basis/translation makes every temporal reprojection (TAA/SPGI/DDGI/
    // SSGI/GTAO) read garbage history. On a cut: re-latch prev := current so
    // reprojection degenerates to identity for exactly one frame, flag the
    // frame, and drop TAA history (the worst smearer).
    camera_cut_this_frame_ = false;
    if (!prev_matrices_valid_) {
        prev_view_matrix_ = view_matrix_;
        prev_proj_matrix_ = proj_matrix_;
        prev_matrices_valid_ = true;
        camera_cut_this_frame_ = true;   // first frame: no history to reuse
    } else {
        const m4x4& a = prev_view_matrix_;
        const m4x4& b = view_matrix_;
        float maxDelta = 0.0f;
        for (u32 c = 0; c < 4; ++c) {
            const v4& ca = a.columns[c];
            const v4& cb = b.columns[c];
            maxDelta = std::max(maxDelta, std::abs(ca.x - cb.x));
            maxDelta = std::max(maxDelta, std::abs(ca.y - cb.y));
            maxDelta = std::max(maxDelta, std::abs(ca.z - cb.z));
        }
        // View translation is -R^T·eye, so a cut swings it by scene-scale
        // units while real per-frame motion (orbit/pan) stays well below.
        // A 180° turn flips rotation rows by up to 2 — threshold 1.0 sits
        // between real motion and any cut.
        if (maxDelta > 1.0f) {
            camera_cut_this_frame_ = true;
            prev_view_matrix_ = view_matrix_;
            prev_proj_matrix_ = proj_matrix_;
            PostProcess::ResetTAAHistory();
        }
    }

    static u32 viewDiag = 0;
    if (viewDiag < 3) {
        PRIMAL_LOG_STREAM(Info, "Render") << "[Diag] View: col0=(" << view_matrix_.columns[0][0] << "," << view_matrix_.columns[0][1] << "," << view_matrix_.columns[0][2] << ")"
                  << " col1=(" << view_matrix_.columns[1][0] << "," << view_matrix_.columns[1][1] << "," << view_matrix_.columns[1][2] << ")"
                  << " col2=(" << view_matrix_.columns[2][0] << "," << view_matrix_.columns[2][1] << "," << view_matrix_.columns[2][2] << ")"
                  << " col3=(" << view_matrix_.columns[3][0] << "," << view_matrix_.columns[3][1] << "," << view_matrix_.columns[3][2] << ")"
                  << " Proj11=" << proj_matrix_.columns[1][1] << " Proj22=" << proj_matrix_.columns[2][2]
                  << std::endl;
        viewDiag++;
    }

    // Extract camera position from inverse view matrix (column 3, xyz)
    {
        math::m4x4 invView = Inverse(view_matrix_);
        camera_position_ = math::v3{invView.columns[3][0], invView.columns[3][1], invView.columns[3][2]};
    }

    // Update scene snapshot
    if (scene_snapshot_) {
        // A14: game-mode spawn unlock — the snapshot previously refreshed only
        // on its internal NeedsFullRebuild flag, so entities spawned after the
        // first frame never got extracted. Compare the live proxy count with
        // the count at the last extraction (NOT instance_count_: extraction
        // legitimately skips proxies without cluster caches, so a permanent
        // gap there must not re-trigger rebuilds every frame).
        const u32 liveProxies = static_cast<u32>(scene.GetProxies().size());
        if (!scene_snapshot_->NeedsFullRebuild() &&
            liveProxies != snapshot_proxy_count_) {
            scene_snapshot_->MarkFullRebuild();
        }
        if (surface_cache_pass_ && surface_cache_pass_->StaticPagesEnabled()) {
            const auto& current = scene_snapshot_->GetInstanceData();
            bool changed = sc_material_signatures_.size() != current.size();
            for (u32 i = 0; i < current.size() && !changed; ++i)
                changed = sc_material_signatures_[i] != nanite::GPUDrivenDrawPipeline::Get().GetSurfaceBakeMaterialDigest(current[i].material_id);
            for (const auto& proxy : scene.GetProxies()) {
                u8 flag = 0; game_entity::entity_id eid{proxy.entityId};
                transform::get_updated_components_flags(&eid, 1, &flag);
                changed |= flag != 0;
            }
            if (changed) scene_snapshot_->MarkFullRebuild();
        }
        if (scene_snapshot_->NeedsFullRebuild()) {
            scene_snapshot_->Rebind(scene);
            sc_material_signatures_.clear();
            for (const auto& inst : scene_snapshot_->GetInstanceData())
                sc_material_signatures_.push_back(nanite::GPUDrivenDrawPipeline::Get().GetSurfaceBakeMaterialDigest(inst.material_id));
            scene_snapshot_->ClearFullRebuildFlag();
            snapshot_proxy_count_ = liveProxies;   // A14: re-arm the growth check

            // Phase 1 (Lumen architecture): register real mesh bounds with
            // the Surface Cache CardGenerator. Each scene instance gets its
            // own cards at appropriate resolution — replacing the 2 large
            // test AABBs from Initialize. This is the single biggest
            // improvement to SC atlas coverage and per-mesh detail.
            if (surface_cache_pass_ && surface_cache_pass_->IsInitialized()) {
                auto& cardGen = surface_cache_pass_->GetCardGenerator();
                const auto& instances = scene_snapshot_->GetInstanceData();

                // ==========================================================
                // [SC 卡片 Lumen 对齐 Phase 2]（2026-09-19 计划，WRC 遗留
                // #5）：content-ready 注册门禁。异步 mesh 加载完成前注册的
                // 实例拿不到紧致 AABB（回退球转立方体），松 AABB 使卡片投
                // 影面积过滤/分辨率公式产出漂移的卡片集合（374/256 运行间
                // 两态，256 卡态查询全灭）。首次注册推迟到快照内全部实例
                // 都有离线 bake 或有效 mesh 紧致 AABB；每帧重臂全量重建直
                // 至就绪。超时（PRIMAL_SC_REG_GATE_TIMEOUT，默认 600 帧）
                // 后按现状注册并告警，防几何永远缺 bounds 的资产卡死 SC。
                // ==========================================================
                bool doRegister = true;
                if (!sc_registration_done_) {
                    bool allContentReady = !instances.empty();
                    for (u32 i = 0; i < instances.size() && allContentReady; ++i) {
                        const auto& inst = instances[i];
                        if (primal::content::has_rhi_mesh_card_bake(inst.geometry_id))
                            continue;
                        rhi::RHIGpuMesh* gateMesh = primal::content::get_rhi_gpu_mesh(inst.geometry_id);
                        const f32* gbmin = gateMesh ? gateMesh->GetBoundsMin() : nullptr;
                        const f32* gbmax = gateMesh ? gateMesh->GetBoundsMax() : nullptr;
                        // [R2.3 2026-09-20] 门禁判据收口：只等"mesh 未加载"
                        //（gateMesh==null，异步内容真正的 pending 信号）。
                        // 4 个退化 mesh 实测 bounds ≈ (0,0,0)-(-1e-9,0,0)（4 顶
                        // 点面片加载器退化路径，含 -ε 负向浮点尾差）——退化视为
                        // 显式降级：RegisterMesh 后 view_area 过滤自然产出 0 张卡
                        //（优于球转立方体松 AABB 卡）。
                        // [2026-09-22 二次收口] bounds 倒置（max < min，如实例
                        // 336 的 (0,6.6e28,0)-(0,0,0) 垃圾 bounds）同样视为显式
                        // 退化而非 pending——此前被误判为 pending 使门禁无限推
                        // 迟（SPGI 仅天空供给，take 内容退化为 0/1 二值），只能
                        // 靠 600 帧超时兜底。倒置 bounds 的实例在 view_area 过滤
                        // 中自然产出 0 卡，注册侧无副作用。
                        if (!(gbmin && gbmax)) {
                            allContentReady = false;
                            // [R2.3] 一次性诊断：门禁阻塞实例的 mesh bounds 状态
                            //（无 mesh / 全零 bounds / 平面 bounds）。
                            if (!sc_gate_diag_logged_) {
                                sc_gate_diag_logged_ = true;
                                PRIMAL_LOG_STREAM(Info, "Render")
                                    << "[LumenSC] gate block: inst=" << i
                                    << " geo=" << inst.geometry_id
                                    << " mesh=" << (gateMesh ? "yes" : "null")
                                    << " bounds="
                                    << (gbmin && gbmax
                                        ? "(" + std::to_string(gbmin[0]) + "," + std::to_string(gbmin[1]) + "," + std::to_string(gbmin[2]) + ")-(" + std::to_string(gbmax[0]) + "," + std::to_string(gbmax[1]) + "," + std::to_string(gbmax[2]) + ")"
                                        : std::string("n/a"))
                                    << std::endl;
                            }
                        }
                    }
                    static const u32 kGateTimeout = [] {
                        const char* e = std::getenv("PRIMAL_SC_REG_GATE_TIMEOUT");
                        return e ? (u32)std::max(1, atoi(e)) : 600u;
                    }();
                    if (!allContentReady && sc_registration_deferred_frames_ < kGateTimeout) {
                        if (sc_registration_deferred_frames_ == 0) {
                            PRIMAL_LOG_STREAM(Info, "Render")
                                << "[LumenSC] Content-ready gate: deferring first card"
                                << " registration (async mesh content pending, timeout "
                                << kGateTimeout << " frames)." << std::endl;
                        }
                        ++sc_registration_deferred_frames_;
                        scene_snapshot_->MarkFullRebuild();   // 下一帧重试
                        doRegister = false;
                    }
                }

                if (doRegister) {
                    if (!sc_registration_done_ && sc_registration_deferred_frames_ > 0) {
                        PRIMAL_LOG_STREAM(Info, "Render")
                            << "[LumenSC] Content-ready gate passed after "
                            << sc_registration_deferred_frames_ << " deferred frames."
                            << std::endl;
                    }
                    sc_registration_done_ = true;

                    // T4.2: the loop runs on every full rebuild — drop the
                    // previous registration set first, otherwise instances
                    // accumulate and duplicate cards.
                    cardGen.ResetRegistrations();
                    // [F5 2026-09-20 能量不对称修复] 手工卡片 override 表——
                    // PRIMAL_SC_CARD_OVERRIDE="mesh_id:x0,y0,z0,x1,y1,z1[;...]"
                    //（世界系 AABB；多 mesh 用 ';' 分隔）。命中表项的实例
                    // 跳过离线 bake/回退推导，直接按 override AABB 走
                    // RegisterMesh 6 向发卡——服务单点实验（如单柱+墙+地面
                    // 强制高质量卡后 SC-only 观察梯度）。表为空 = 原行为；
                    // 非法条目告警忽略。
                    static const std::unordered_map<u32, std::pair<math::v3, math::v3>> scCardOverrides = [] {
                        std::unordered_map<u32, std::pair<math::v3, math::v3>> m;
                        if (const char* e = std::getenv("PRIMAL_SC_CARD_OVERRIDE")) {
                            std::stringstream ss(e);
                            std::string item;
                            while (std::getline(ss, item, ';')) {
                                if (item.empty()) continue;
                                float v[6] = {};
                                unsigned id = 0;
                                if (std::sscanf(item.c_str(), "%u:%f,%f,%f,%f,%f,%f",
                                                &id, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 7) {
                                    m[id] = {math::v3{v[0], v[1], v[2]}, math::v3{v[3], v[4], v[5]}};
                                } else {
                                    PRIMAL_LOG_STREAM(Error, "Render")
                                        << "[LumenSC] PRIMAL_SC_CARD_OVERRIDE: malformed entry '"
                                        << item << "' (ignored)" << std::endl;
                                }
                            }
                            if (!m.empty()) {
                                PRIMAL_LOG_STREAM(Info, "Render")
                                    << "[LumenSC] card override table active: " << m.size()
                                    << " mesh entrie(s)" << std::endl;
                            }
                        }
                        return m;
                    }();
                    for (u32 i = 0; i < instances.size(); ++i) {
                    const auto& inst = instances[i];
                    // [F5] override 表命中 → 手工 AABB 6 向发卡（优先级最高）。
                    if (auto it = scCardOverrides.find(inst.geometry_id); it != scCardOverrides.end()) {
                        cardGen.RegisterMesh(it->second.first, it->second.second, i);
                        continue;
                    }
                    // T4.2: prefer the mesh's offline card bake; fall back to
                    // the sphere-derived cube AABB when absent (procedural
                    // meshes, pre-T4.2 blobs, zero-occupancy geometry).
                    rhi::RHICardBakeData bake;
                    if (primal::content::get_rhi_mesh_card_bake(inst.geometry_id, bake)) {
                        // [SC per-mesh 计划 P2] 双摘要键控静态页槽位；无页/
                        // 无合成能力 = INVALID → 原 capture 路径。
                        const u32 staticSlot = surface_cache_pass_
                            ? surface_cache_pass_->ResolveStaticSlot(bake.geometry_digest, bake.layout_digest,
                                nanite::GPUDrivenDrawPipeline::Get().GetSurfaceBakeMaterialDigest(inst.material_id))
                            : 0xFFFFFFFFu;
                        cardGen.RegisterMeshBaked(bake, inst.world_matrix, i, staticSlot);
                    } else {
                        // [WRC 计划 Stream E1]: fallback AABB 从 bounds_radius
                        // 球转立方体换 mesh 紧致 AABB——本地 bounds 经 world
                        // 轴展开（与 RegisterMeshBaked / detail-SDF 同配方）。
                        // 球转立方体把平板/细长网格的空体积全数包进卡片 AABB，
                        // 卡片分辨率与覆盖都被球半径主导。无 mesh bounds 时
                        // 保留球转立方体回退。
                        bool tightApplied = false;
                        if (rhi::RHIGpuMesh* mesh = primal::content::get_rhi_gpu_mesh(inst.geometry_id)) {
                            const f32* bmin = mesh->GetBoundsMin();
                            const f32* bmax = mesh->GetBoundsMax();
                            // [R1.3/R2.3 2026-09-20] 与 content-ready 门禁同判据：
                            // mesh 已加载即用其紧致 AABB（含平面/全零退化——
                            // view_area 过滤自然丢弃零面积朝向），球转立方体
                            // 回退仅留给 mesh 缺失/bounds 倒置（损坏）情形。
                            if (bmin && bmax &&
                                bmax[0] >= bmin[0] - 1e-4f && bmax[1] >= bmin[1] - 1e-4f &&
                                bmax[2] >= bmin[2] - 1e-4f) {
                                const math::v3 ext{ (bmax[0] - bmin[0]) * 0.5f,
                                                    (bmax[1] - bmin[1]) * 0.5f,
                                                    (bmax[2] - bmin[2]) * 0.5f };
                                const math::v3 lc{ (bmax[0] + bmin[0]) * 0.5f,
                                                   (bmax[1] + bmin[1]) * 0.5f,
                                                   (bmax[2] + bmin[2]) * 0.5f };
                                const math::m4x4& w = inst.world_matrix;
                                f32 we[3] = {0.f, 0.f, 0.f};
                                for (u32 k = 0; k < 3; ++k) {
                                    const math::v4& col = w.columns[k];
                                    const f32 e = (k == 0) ? ext.x : (k == 1) ? ext.y : ext.z;
                                    we[0] += std::abs(col.x) * e;
                                    we[1] += std::abs(col.y) * e;
                                    we[2] += std::abs(col.z) * e;
                                }
                                const math::v4 cw = w * math::v4{lc.x, lc.y, lc.z, 1.0f};
                                const math::v3 wmin{cw.x - we[0], cw.y - we[1], cw.z - we[2]};
                                const math::v3 wmax{cw.x + we[0], cw.y + we[1], cw.z + we[2]};
                                cardGen.RegisterMesh(wmin, wmax, i);
                                tightApplied = true;
                            }
                        }
                        if (!tightApplied) {
                            math::v3 aabbMin = inst.bounds_center - math::v3{inst.bounds_radius, inst.bounds_radius, inst.bounds_radius};
                            math::v3 aabbMax = inst.bounds_center + math::v3{inst.bounds_radius, inst.bounds_radius, inst.bounds_radius};
                            cardGen.RegisterMesh(aabbMin, aabbMax, i);
                        }
                    }
                }
                    if(surface_cache_pass_ && surface_cache_pass_->WantsGeometryFirstHit()) {
                        std::vector<ray_geometry::InstanceInput> geometryInstances;
                        geometryInstances.reserve(instances.size());
                        for(u32 id=0;id<instances.size();++id) {
                            const auto& item=instances[id];
                            geometryInstances.push_back({primal::content::get_rhi_mesh_blas(item.geometry_id),item.world_matrix,id});
                        }
                        surface_cache_pass_->SetGeometryScene(geometryInstances);
                    }
                    cardGen.RebuildCardAllocation();
                    PRIMAL_LOG_STREAM(Info, "Render") << "[LumenSC] Registered " << instances.size()
                              << " scene instances with CardGenerator → "
                              << cardGen.GetCardCount() << " cards"
                              << " | cards source = baked (" << cardGen.GetBakedMeshCount()
                              << " meshes) / fallback (" << cardGen.GetFallbackMeshCount()
                              << " meshes)"
                              << " | truncated_by_max_cards (" << cardGen.GetCapacityTruncatedMeshCount()
                              << " meshes)" << std::endl;

                    // ======================================================
                    // [SC 卡片 Lumen 对齐 Phase 4] fallback 政策：cook 必带
                    // bake。无 CDBK 的 blob 走 AABB fallback——把计数升级为
                    // Warn（首次注册各告警一次），便于发现漏烘资产。
                    // ======================================================
                    if (cardGen.GetFallbackMeshCount() > 0 && !sc_registration_warned_) {
                        sc_registration_warned_ = true;
                        PRIMAL_LOG_STREAM(Warn, "Render")
                            << "[LumenSC] " << cardGen.GetFallbackMeshCount() << "/"
                            << instances.size() << " meshes have no offline card bake"
                            << " (CDBK) — AABB fallback in use. Policy: cook must"
                            << " carry bake; repair via `primal-engine bake-cards`."
                            << std::endl;
                    }
                }

        // [WRC 计划 Stream E2]: dirty-transform GI invalidation 改事件驱动
        // （每帧查询变换脏旗标——旗标即事件源，30 帧冷却废除），增量路径：
        // 脏实体的 SC 卡片经 CardGenerator::RebuildInstance 单实例重建，
        // snapshot 全量 Rebind 与 GlobalSDF revox / probe boost 保持原语义。
        {
            static u32 lastDirtyRebuildFrame = 0;
            bool anyDirty = false;
            const auto& proxies = scene.GetProxies();
            for (u32 pi = 0; pi < proxies.size(); ++pi) {
                u8 flag = 0;
                game_entity::entity_id eid{ proxies[pi].entityId };
                transform::get_updated_components_flags(&eid, 1, &flag);
                if (flag == 0) continue;
                anyDirty = true;
                // E2 增量：脏实例的注册 AABB 立即更新（紧致 AABB 配方同上，
                // 失败回退球转立方体）。allocation_dirty_ 触发下一次
                // RebuildCardAllocation（Rebind 重建帧内先于注册循环执行）。
                if (surface_cache_pass_ && surface_cache_pass_->IsInitialized()) {
                    if (surface_cache_pass_->StaticPagesEnabled()) continue;
                    auto& cardGen = surface_cache_pass_->GetCardGenerator();
                    const auto& instances = scene_snapshot_->GetInstanceData();
                    if (pi < instances.size()) {
                        const auto& inst = instances[pi];
                        math::v3 mn = inst.bounds_center - math::v3{inst.bounds_radius, inst.bounds_radius, inst.bounds_radius};
                        math::v3 mx = inst.bounds_center + math::v3{inst.bounds_radius, inst.bounds_radius, inst.bounds_radius};
                        if (rhi::RHIGpuMesh* mesh = primal::content::get_rhi_gpu_mesh(inst.geometry_id)) {
                            const f32* bmin = mesh->GetBoundsMin();
                            const f32* bmax = mesh->GetBoundsMax();
                            if (bmin && bmax && bmax[0] > bmin[0] && bmax[1] > bmin[1] && bmax[2] > bmin[2]) {
                                const math::v3 ext{ (bmax[0] - bmin[0]) * 0.5f,
                                                    (bmax[1] - bmin[1]) * 0.5f,
                                                    (bmax[2] - bmin[2]) * 0.5f };
                                const math::v3 lc{ (bmax[0] + bmin[0]) * 0.5f,
                                                   (bmax[1] + bmin[1]) * 0.5f,
                                                   (bmax[2] + bmin[2]) * 0.5f };
                                const math::m4x4& w = inst.world_matrix;
                                f32 we[3] = {0.f, 0.f, 0.f};
                                for (u32 k = 0; k < 3; ++k) {
                                    const math::v4& col = w.columns[k];
                                    const f32 e = (k == 0) ? ext.x : (k == 1) ? ext.y : ext.z;
                                    we[0] += std::abs(col.x) * e;
                                    we[1] += std::abs(col.y) * e;
                                    we[2] += std::abs(col.z) * e;
                                }
                                const math::v4 cw = w * math::v4{lc.x, lc.y, lc.z, 1.0f};
                                mn = {cw.x - we[0], cw.y - we[1], cw.z - we[2]};
                                mx = {cw.x + we[0], cw.y + we[1], cw.z + we[2]};
                            }
                        }
                        cardGen.RebuildInstance(pi, mn, mx);
                    }
                }
            }
            if (anyDirty && frameCount_ != lastDirtyRebuildFrame) {
                if(surface_cache_pass_ && surface_cache_pass_->WantsGeometryFirstHit())
                    surface_cache_pass_->InvalidateGeometryScene();
                lastDirtyRebuildFrame = static_cast<u32>(frameCount_);
                scene_snapshot_->MarkFullRebuild();
                nanite::GlobalSDF::Get().MarkAllCascadesForRevox();
                if (ddgi_pass_ && ddgi_pass_->IsInitialized())
                    ddgi_pass_->BoostProbePriority();
                if (screen_probe_pass_ && screen_probe_pass_->IsInitialized())
                    screen_probe_pass_->RequestTemporalReset();
                PRIMAL_LOG_STREAM(Info, "Render")
                    << "[Lumen] Dirty transform (event-driven) — incremental "
                    << "card rebuild + revox+probe boost." << std::endl;
            }
        }            }
        }
    }

    // T4.3: per-mesh detail-SDF slot selection (first slice of detail
    // tracing, PRIMAL_DDGI_DETAIL_SDF=1). Up to 4 instances whose per-mesh
    // SDF textures intersect the DDGI volume, scored by intersection volume;
    // the split SDF trace refines coarse hits against them and derives
    // gradient normals consumed by finalize.
    {
        static const bool detailSDFEnabled =
            (std::getenv("PRIMAL_DDGI_DETAIL_SDF") != nullptr);
        if (ddgi_pass_ && ddgi_pass_->IsInitialized() && detailSDFEnabled && scene_snapshot_) {
            constexpr u32 kMaxSlots = lumen::LumenDDGIPass::kDetailSDFSlots;
            math::v3 volMin, volMax;
            ddgi_pass_->GetVolumeAABB(volMin, volMax);

            struct Candidate { f32 score; u32 index; };
            Candidate cands[kMaxSlots] = {};
            u32 candCount = 0;
            u32 worstSlot = 0;

            const auto& instances = scene_snapshot_->GetInstanceData();
            for (u32 i = 0; i < instances.size(); ++i) {
                const auto& inst = instances[i];
                rhi::RHIGpuMesh* mesh = primal::content::get_rhi_gpu_mesh(inst.geometry_id);
                if (!mesh || mesh->GetSDFTexture() == handles::INVALID_RESOURCE) continue;

                // Mesh-local SDF bounds → world AABB (axis-expanded extents
                // under rotation+scale, same recipe as RegisterMeshBaked).
                const f32* bmin = mesh->GetBoundsMin();
                const f32* bmax = mesh->GetBoundsMax();
                const math::v3 ext{ (bmax[0] - bmin[0]) * 0.5f,
                                    (bmax[1] - bmin[1]) * 0.5f,
                                    (bmax[2] - bmin[2]) * 0.5f };
                const math::v3 lc{ (bmax[0] + bmin[0]) * 0.5f,
                                   (bmax[1] + bmin[1]) * 0.5f,
                                   (bmax[2] + bmin[2]) * 0.5f };
                const math::m4x4& w = inst.world_matrix;
                f32 we[3] = {0.f, 0.f, 0.f};
                for (u32 k = 0; k < 3; ++k) {
                    const math::v4& col = w.columns[k];
                    const f32 e = (k == 0) ? ext.x : (k == 1) ? ext.y : ext.z;
                    we[0] += std::abs(col.x) * e;
                    we[1] += std::abs(col.y) * e;
                    we[2] += std::abs(col.z) * e;
                }
                const math::v4 cw = w * math::v4{lc.x, lc.y, lc.z, 1.0f};
                const math::v3 wmin{cw.x - we[0], cw.y - we[1], cw.z - we[2]};
                const math::v3 wmax{cw.x + we[0], cw.y + we[1], cw.z + we[2]};

                // Score = world-AABB ∩ DDGI-volume intersection volume.
                const math::v3 imin{std::max(wmin.x, volMin.x), std::max(wmin.y, volMin.y), std::max(wmin.z, volMin.z)};
                const math::v3 imax{std::min(wmax.x, volMax.x), std::min(wmax.y, volMax.y), std::min(wmax.z, volMax.z)};
                const f32 vx = imax.x - imin.x, vy = imax.y - imin.y, vz = imax.z - imin.z;
                if (vx <= 0.f || vy <= 0.f || vz <= 0.f) continue;
                const f32 score = vx * vy * vz;

                if (candCount < kMaxSlots) {
                    cands[candCount++] = {score, i};
                    if (candCount == kMaxSlots) {
                        worstSlot = 0;
                        for (u32 s = 1; s < kMaxSlots; ++s)
                            if (cands[s].score < cands[worstSlot].score) worstSlot = s;
                    }
                } else if (score > cands[worstSlot].score) {
                    cands[worstSlot] = {score, i};
                    worstSlot = 0;
                    for (u32 s = 1; s < kMaxSlots; ++s)
                        if (cands[s].score < cands[worstSlot].score) worstSlot = s;
                }
            }

            if (candCount > 0) {
                lumen::LumenDDGIPass::DetailSDFSlot slots[kMaxSlots] = {};
                rhi::ResourceHandle texs[kMaxSlots] = {
                    handles::INVALID_RESOURCE, handles::INVALID_RESOURCE,
                    handles::INVALID_RESOURCE, handles::INVALID_RESOURCE };
                for (u32 s = 0; s < candCount; ++s) {
                    const auto& inst = instances[cands[s].index];
                    rhi::RHIGpuMesh* mesh = primal::content::get_rhi_gpu_mesh(inst.geometry_id);
                    if (!mesh) continue;

                    slots[s].inv_world = Inverse(inst.world_matrix);
                    // Normal-matrix rows = columns of inv_world: for
                    // column-major M, n_world = (M₃ₓ₃⁻¹)ᵀ n_local, applied as
                    // Σ_j inv.columns[j] * g_j — exactly the shader's row form.
                    slots[s].normal_matrix[0] = slots[s].inv_world.columns[0];
                    slots[s].normal_matrix[1] = slots[s].inv_world.columns[1];
                    slots[s].normal_matrix[2] = slots[s].inv_world.columns[2];

                    const f32* bmin = mesh->GetBoundsMin();
                    const f32* bmax = mesh->GetBoundsMax();
                    slots[s].bounds_min = math::v4{bmin[0], bmin[1], bmin[2], 0.f};
                    slots[s].bounds_max = math::v4{bmax[0], bmax[1], bmax[2], 0.f};
                    // Decode params: max_dim = padded bounds max extent / 1.4
                    // (ContentTools uniform 20% padding); voxel = extent/res.
                    const f32 ex = bmax[0] - bmin[0], ey = bmax[1] - bmin[1], ez = bmax[2] - bmin[2];
                    const f32 maxExt = std::max({ex, ey, ez});
                    const u32 res = mesh->GetSDFResolution()[0];
                    slots[s].params = math::v4{
                        (f32)res, maxExt / 1.4f,
                        mesh->IsSDFSigned() ? 1.0f : 0.0f,
                        maxExt / (f32)std::max(res, 1u)};
                    texs[s] = mesh->GetSDFTexture();
                }
                        ddgi_pass_->SetDetailSDFSlots(texs, slots, candCount);
                        // T4.3 二批: mirror into the SPGI pass (POD-identical
                        // slot layout; distance-only refinement on its side).
                        if (screen_probe_pass_ && screen_probe_pass_->IsInitialized()) {
                            static_assert(sizeof(lumen::ScreenProbeGIPass::DetailSDFSlot) ==
                                          sizeof(lumen::LumenDDGIPass::DetailSDFSlot),
                                          "detail slot POD drift");
                            lumen::ScreenProbeGIPass::DetailSDFSlot spgiSlots[kMaxSlots];
                            memcpy(spgiSlots, slots, sizeof(spgiSlots));
                            screen_probe_pass_->SetDetailSDFSlots(texs, spgiSlots, candCount);
                        }
                        static u32 lastLogged = 0xFFFFFFFFu;
                        if (candCount != lastLogged) {
                            lastLogged = candCount;
                            PRIMAL_LOG_STREAM(Info, "Render")
                                << "[LumenDDGI] T4.3 detail-SDF slots active = " << candCount
                                << "/" << kMaxSlots << std::endl;
                        }
                    } else {
                        ddgi_pass_->SetDetailSDFSlots(nullptr, nullptr, 0);
                        if (screen_probe_pass_ && screen_probe_pass_->IsInitialized())
                            screen_probe_pass_->SetDetailSDFSlots(nullptr, nullptr, 0);
                // One-shot diagnostics: why no slot activated.
                static bool diagLogged = false;
                if (!diagLogged && !scene_snapshot_->GetInstanceData().empty()) {
                    diagLogged = true;
                    u32 meshNull = 0, texInvalid = 0, total = 0;
                    const auto& insts = scene_snapshot_->GetInstanceData();
                    for (u32 i = 0; i < insts.size(); ++i) {
                        ++total;
                        rhi::RHIGpuMesh* mesh = primal::content::get_rhi_gpu_mesh(insts[i].geometry_id);
                        if (!mesh) { ++meshNull; continue; }
                        if (mesh->GetSDFTexture() == handles::INVALID_RESOURCE) ++texInvalid;
                    }
                    PRIMAL_LOG_STREAM(Info, "Render")
                        << "[LumenDDGI] T4.3 detail-SDF: " << total << " instances, "
                        << meshNull << " no GPU mesh, " << texInvalid << " mesh w/o SDF texture"
                        << std::endl;
                }
            }
        }
    }

    // Phase 9.3b: Give GPUDrivenDrawPipeline access to the RenderScene so
    // DrawStreamingMeshes can iterate GetStreamingMeshes() during Execute.
    auto& gpuDraw = nanite::GPUDrivenDrawPipeline::Get();
    gpuDraw.SetRenderScene(&scene);

    // Update GlobalSDF cascade origins
    auto& globalSDF = nanite::GlobalSDF::Get();
    if (globalSDF.IsInitialized() && scene_snapshot_) {
        globalSDF.Update(*scene_snapshot_, frameCount_, camera_position_);
    }

    // PCG SDF readback: consume previous frame's GPU data
    if (pcg_sdf_readback_initialized_) {
        pcg_sdf_readback_.ReadbackData();
    }

    // DDGI probe origin stays fixed at world origin (matches TestNaniteStreamingPipeline).
    // UpdateProbeOrigin causes relocation shifts that reset irradiance data, producing
    // "light turning off" artifacts in shadowed areas. The static grid (32x16x32 = 124
    // units coverage) is large enough for Sponza (~30 units).
    // if (ddgi_pass_ && ddgi_pass_->IsInitialized()) {
    //     ddgi_pass_->UpdateProbeOrigin(camera_position_);
    // }

    // Streaming LRU update
    if (streaming_manager_) {
        streaming_manager_->UpdateLRU(frameCount_, camera_position_);
    }
}

// ============================================================================
// PCG Entity → RenderScene Sync (Phase 3c)
// ============================================================================

void StandardRenderPipeline::SyncEntitiesToRenderScene(RenderScene& scene) {
    if (!forward_renderer_) return;
    if (static_entity_ids_.empty() && pcg_entity_ids_.empty()) return;

    // Lambda to sync a group of entity IDs
    auto syncGroup = [&](const std::vector<id::id_type>& ids,
                         const std::vector<u32>& slots) {
        for (size_t i = 0; i < ids.size(); i++) {
            id::id_type eid = ids[i];

            if (!game_entity::is_alive(game_entity::entity_id{eid})) {
                scene.RemoveProxy(eid);
                continue;
            }

            math::m4x4 world, inv_world;
            transform::get_transform_matrices(game_entity::entity_id{eid}, world, inv_world);

            u32 slot = slots[i];
            auto* meshInfo = forward_renderer_->GetMeshInfo(slot);
            if (!meshInfo || !meshInfo->mesh) continue;

            id::id_type meshId = meshInfo->mesh->GetEntityId();
            id::id_type materialId = meshInfo->gpuMaterialId;

            RenderProxy proxy = RenderProxy::Create(eid, meshId, materialId);
            proxy.UpdateTransform(world);

            game_entity::entity entity{game_entity::entity_id{eid}};
            if (entity.Has<component::Material>()) {
                auto mc = entity.Get<component::Material>();
                proxy.technique = material::get_technique(mc);
                proxy.roughness = material::get_roughness(mc);
                proxy.metallic  = material::get_metallic(mc);
                material::get_base_color(mc, proxy.base_color);
            }
            // Editor world-material path: entities with a successfully
            // registered compiled graph render through the generated shader.
            proxy.graphVariant = forward_renderer_->GetEntityGraphVariant(eid);

            scene.UpdateProxy(eid, proxy);
        }
    };

    syncGroup(static_entity_ids_, static_mesh_slot_indices_);
    syncGroup(pcg_entity_ids_, pcg_mesh_slot_indices_);
}

id::id_type StandardRenderPipeline::RegisterMeshEntity(id::id_type geometry_content_id,
                                                         const id::id_type* texture_content_ids,
                                                         u32 texture_count) {
    if (!forward_renderer_ || geometry_content_id == id::invalid_id) return id::invalid_id;

    // Create ECS entity FIRST so we have a unique id to pass to
    // RegisterMeshResource. The mesh's entity_id is what mesh_id_to_index_
    // is keyed by — if multiple entities share the same geometry asset we
    // must NOT collide, so each entity gets its own mesh registration.
    transform::init_info tf{};
    tf.position[0] = 0.f; tf.position[1] = 0.f; tf.position[2] = 0.f;
    tf.rotation[0] = 0.f; tf.rotation[1] = 0.f;
    tf.rotation[2] = 0.f; tf.rotation[3] = 1.f;
    tf.scale[0] = 1.f; tf.scale[1] = 1.f; tf.scale[2] = 1.f;

    game_entity::entity_info info{};
    info.transform = &tf;

    id::id_type albedo_id = (texture_count > 0) ? texture_content_ids[0] : id::invalid_id;
    id::id_type normal_id = (texture_count > 1) ? texture_content_ids[1] : id::invalid_id;
    id::id_type orm_id    = (texture_count > 2) ? texture_content_ids[2] : id::invalid_id;
    material::init_info material_info{};
    material_info.albedo_texture = albedo_id;
    material_info.normal_texture = normal_id;
    material_info.orm_texture = orm_id;
    info.material = &material_info;

    game_entity::entity entity = game_entity::create(info);
    if (!entity.is_valid()) {
        return id::invalid_id;
    }
    const id::id_type eid = entity.get_id();

    // [PassDebugView/RG] 补挂 cluster 组件：RenderSceneSnapshot::ExtractSceneData
    // 以 cluster::get(proxy.meshId) 命中为唯一收录门禁（proxy.meshId 即本实体
    // id，SyncEntitiesToRenderScene :2145）。缺此组件的 mesh 实体不进快照 →
    // 编辑器 RG 路径（Runtime 预览 / pass view）场景几何全空。对齐测试壳
    // （TestVulkanSponzaRenderGraph :856-858）。前提：NaniteResourceManager 已
    // 初始化（InitializeSubsystems 先于场景实体注册；AddGeometryRef 对未注册
    // geometry 静默 no-op，异步 mesh 就绪后再由快照提取补建）。
    {
        cluster::init_info cluster_info{};
        cluster_info.geometry_content_id = geometry_content_id;
        if (cluster::create(cluster_info, entity) == id::invalid_id) {
            PRIMAL_LOG_STREAM(Error, "Render")
                << "[StandardRenderPipeline] cluster::create failed for geometry "
                << geometry_content_id << " — entity won't enter RG snapshots" << std::endl;
        }
    }

    u32 slot = forward_renderer_->RegisterMeshResource(
        geometry_content_id, albedo_id, normal_id, orm_id, eid);

    // [PassDebugView 排查] 编辑器材质接线：有 albedo 纹理的实体注册进
    // Nanite 材质数组（记录+层），cluster map materialID 随之生效 ——
    // 否则 pass 视图 albedo 只能显示白模。失败安全：返回 0 = 白模。
    // 纹理句柄取 forward 路径已解析的同一句柄，保证 off/albedo 两路一致。
    u32 gpu_material_id = 0;
    if (slot != (u32)-1) {
        rhi::ResourceHandle ah, nh, oh;
        forward_renderer_->GetMeshTextureHandles(slot, ah, nh, oh);
        if (ah != rhi::handles::INVALID_RESOURCE) {
            gpu_material_id = nanite::GPUDrivenDrawPipeline::Get()
                                  .RegisterEditorMaterial(ah, nh, oh);
        }
        if (auto* mi = forward_renderer_->GetMeshInfoMutable(slot)) {
            mi->gpuMaterialId = gpu_material_id;
        }
    }

    if (slot == (u32)-1) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[StandardRenderPipeline] RegisterMeshEntity: failed for geometry "
                  << geometry_content_id << std::endl;
        game_entity::remove(game_entity::entity_id{eid});
        return id::invalid_id;
    }

    static_entity_ids_.push_back(eid);
    static_mesh_slot_indices_.push_back(slot);

    return eid;
}

void StandardRenderPipeline::UnregisterMeshEntity(id::id_type entity_id) {
    if (forward_renderer_) forward_renderer_->ClearEntityGraphMaterial(entity_id);

    // Find and remove from static entity lists
    for (size_t i = 0; i < static_entity_ids_.size(); i++) {
        if (static_entity_ids_[i] == entity_id) {
            forward_renderer_->UnregisterMeshResource(static_mesh_slot_indices_[i]);

            // Swap with last and pop
            static_entity_ids_[i] = static_entity_ids_.back();
            static_entity_ids_.pop_back();
            static_mesh_slot_indices_[i] = static_mesh_slot_indices_.back();
            static_mesh_slot_indices_.pop_back();

            // Destroy ECS entity（cluster 组件随实体释放；显式 remove 释放
            // NaniteResourceManager 引用计数）。
            game_entity::entity e{game_entity::entity_id{entity_id}};
            if (e.is_valid()) {
                cluster::remove(entity_id);
                game_entity::remove(game_entity::entity_id{entity_id});
            }
            return;
        }
    }
}

// ============================================================================
// Light Entity Registration (Path B C ABI support)
// ============================================================================
// REVISED contract: info.entity_id is IGNORED. Pipeline creates a new ECS
// entity internally with Transform + Light components. LightSyncSystem reads
// from Light component (via LightComponent setters) and position from the
// Transform world matrix. Returns new entity_id or invalid_id on failure.

id::id_type StandardRenderPipeline::RegisterLightEntity(const light_init_info& info) {
    // Create identity transform for the light entity.
    transform::init_info tf{};
    tf.position[0] = 0.f; tf.position[1] = 0.f; tf.position[2] = 0.f;
    tf.rotation[0] = 0.f; tf.rotation[1] = 0.f;
    tf.rotation[2] = 0.f; tf.rotation[3] = 1.f;  // identity quaternion
    tf.scale[0] = 1.f; tf.scale[1] = 1.f; tf.scale[2] = 1.f;

    // Map graphics::light_init_info → light::init_info (component).
    primal::light::init_info linfo{};
    linfo.light_set_key = info.light_set_key;
    linfo.type = info.type;
    linfo.intensity = info.intensity;
    linfo.color = info.color;
    linfo.is_enabled = info.is_enabled;
    // Pull type-specific params from the union.
    if (info.type == graphics::light::point) {
        linfo.attenuation = info.point_param.attenuation;
        linfo.range = info.point_param.range;
    } else if (info.type == graphics::light::spot) {
        linfo.attenuation = info.spot_param.attenuation;
        linfo.range = info.spot_param.range;
        linfo.umbra = info.spot_param.umbra;
        linfo.penumbra = info.spot_param.penumbra;
    }

    // Create entity with Transform + Light components.
    game_entity::entity_info einfo{};
    einfo.transform = &tf;
    einfo.light = &linfo;

    game_entity::entity ent = game_entity::create(einfo);
    if (!ent.is_valid()) return id::invalid_id;

    return ent.get_id();
}

void StandardRenderPipeline::UnregisterLightEntity(id::id_type entity_id) {
    if (entity_id == id::invalid_id) return;
    game_entity::entity_id eid{ entity_id };
    if (!game_entity::is_alive(eid)) return;
    game_entity::remove(eid);
}

bool StandardRenderPipeline::UpdateLightEntity(id::id_type entity_id, const light_init_info& info) {
    if (entity_id == id::invalid_id) return false;
    game_entity::entity_id eid{ entity_id };
    if (!game_entity::is_alive(eid)) return false;
    component_mask mask = game_entity::get_component_mask(eid);
    if ((mask & bit_mask(component_bit::Light)) == 0) return false;

    // Route through Light component setters (EngineAPI/LightComponent.h).
    primal::light::component lc{ primal::light::light_component_id{ entity_id } };
    if (!lc.is_valid()) return false;

    lc.set_color(info.color);
    lc.set_intensity(info.intensity);
    lc.set_enabled(info.is_enabled);
    lc.set_light_type(info.type);

    // Type-specific params — ALWAYS set every field based on info.type so that
    // type changes (e.g. spot → directional) clear stale state from the
    // type-erased SoA backing store. LightSyncSystem reads range/umbra/penumbra/
    // attenuation unconditionally, so leftover spot cone angles on a directional
    // light would otherwise feed garbage into RenderLight.
    // Defaults mirror light::create() initial SoA values (Light.cpp:30-33):
    //   attenuation = {1,0,0}, range = 10, umbra/penumbra = 0.
    if (info.type == graphics::light::directional) {
        lc.set_attenuation(math::v3{1.0f, 0.0f, 0.0f});
        lc.set_range(10.0f);
        lc.set_cone_angles(0.0f, 0.0f);
    } else if (info.type == graphics::light::point) {
        lc.set_attenuation(info.point_param.attenuation);
        lc.set_range(info.point_param.range);
        lc.set_cone_angles(0.0f, 0.0f);  // point lights have no cone
    } else if (info.type == graphics::light::spot) {
        lc.set_attenuation(info.spot_param.attenuation);
        lc.set_range(info.spot_param.range);
        lc.set_cone_angles(info.spot_param.umbra, info.spot_param.penumbra);
    }

    return true;
}

// ============================================================================
// Build Render Graph
// ============================================================================

// B11 (2026-09-11) 探针辐射度供给：DDGI finalize 的 prev-screen 档（binding 8）
// 改读上一帧 Fusion HDR 输出（含 GI 的完整 Lo）。旧绑定 DeferredLighting 输出
// 只有 ×50 直射（IBL_INTENSITY=0）——射线命中屏幕内阴影面时供给 ≈0，fallback
// materialBounce≈0.1×albedo 比物理阴影墙辐射低 ~30×，室内探针能量只剩逃逸
// 天空档 + fusion floor 三个漏光代理在撑，color bleeding 也随供给链断在半路。
// 上一帧槽 (cbIdx+2)%3 GPU-complete（三缓冲，与 DDGI irradiance history 同
// 惯例）；本帧 Fusion 在 RG 中位于 DDGI 之后且写的是 cbIdx 槽，读写异槽无环。
// 门控：Vulkan（Metal 的 fusion 上游单位制另案审计）；frameCount_≥8 避开
// 未初始化槽（Vulkan heap 纹理非零初始化，见 SurfaceCacheLightEval 的
// prev_light sanitize 注释）；fusion 需真实在管线中；Phase 2a 起默认断开，
// PRIMAL_PROBE_FUSED_SUPPLY=1 回接（A/B 门，语义已反转）。
rendergraph::RGResourceHandle StandardRenderPipeline::MakeProbeRadianceSource(
    rendergraph::RenderGraph& graph, u32 cbIdx) {
    // GI 能量所有权修复 Phase 2a（审计 P1）: E4 边（FusionIndirect→DDGI）
    // 默认断开——语义反转，env =1 才回接（A/B 保留）。理由：Fusion 输出含
    // DDGI(t−2) 的 gather 回波，回灌探针场 = 层级自反馈（E1/E2/E3 携带的
    // 能量两帧后经屏幕再入账），稳态偏亮且无法归因；既有稳定手段保证收敛
    // 但不保证能量归属唯一。断边后 bounce 由 SC（E2 逐射线采样 + E6 自反弹）
    // 与 DDGI 自身多反弹承担——观测窗若能量塌陷，禁止加 gain 回接，按
    // 修复计划 §2b 修供给归属后复测。
    static const bool fusedSupply = [] {
        const char* e = std::getenv("PRIMAL_PROBE_FUSED_SUPPLY");
        return e && std::atoi(e) != 0;
    }();
    if (!fusedSupply || !fusion_module_ || frameCount_ < 8) return {};
    if (!device_ || device_->GetPlatform() != rhi::RHIPlatform::Vulkan) return {};
    if (!(settings_.quality.enable_ssgi || settings_.quality.enable_ssr ||
          settings_.quality.enable_screen_probes) || !lumen::GIDebug().fusion) {
        return {};
    }
    // GI 能量根治方案 Phase 2.3（R4/B11）: 探针供给改回灌 **FusionIndirect 的
    // pre-gain GI 缓冲**——不再回灌含直射与融合增益的最终 Fusion HDR。
    // giIndirectGain 永久移出回路：未来任何显示增益调整都不会被 B11 回灌
    // 再放大（旧的"含 ×3 增益的融合结果回喂探针"是整体偏亮放大器）。
    // 直射供给由 SC 卡片（带 PCSS 可见性）负责，prev-screen 分支的过亮
    // 惩罚（Phase 1.4）继续兜住残余热点。PRIMAL_PROBE_FUSED_SUPPLY=0 仍可
    // 断电回退到直射-only 供给。
    // Phase 1d: 读槽 = DDGISlotMap.h previousIndex(frameCount_)（原
    // (cbIdx+2)%3 裸算随 cbIdx 时钟一并废除）。
    const ResourceHandle tex = fusion_module_->GetIndirectOutput(static_cast<u32>(frameCount_));
    if (tex == handles::INVALID_RESOURCE) return {};
    return graph.ImportResource("PrevFrameColor_GIIndirect", tex);
}

// ============================================================================
// Pass Debug View 源解析
// ============================================================================

bool StandardRenderPipeline::ResolvePassDebugViewSource(
    RenderGraph& graph, u32 cbIdx,
    const DeferredLightingOutputs& deferredOut,
    const FusionOutputs& fusionOut,
    rendergraph::RGResourceHandle& out_source,
    rendergraph::RGResourceHandle& out_id,
    rendergraph::RGResourceHandle& out_depth,
    rhi::ResourceHandle& out_cluster_map,
    rhi::ResourceHandle& out_aux_buffer,
    u32& out_aux_count) {
    auto& gpuDraw = nanite::GPUDrivenDrawPipeline::Get();
    // 副输入（所有视图静态引用）：GBuffer 深度 + 可见性缓冲 + cluster map
    // storage buffer。缺失视为不可用。
    if (!gpuDraw.IsInitialized()) {
        PRIMAL_LOG_STREAM(Info, "Render") << "[PassDebugView] unavailable: gpuDraw not initialized" << std::endl;
        return false;
    }
    const ResourceHandle depth = gpuDraw.GetGBufferDepthSampleable();
    const ResourceHandle visBuf = gpuDraw.GetVisibilityBuffer();
    const ResourceHandle clusterMap = gpuDraw.GetClusterMapBuffer();
    // depth/vis 是着色器静态引用的硬要求；cluster map 允许缺席（流式场景
    // 可能暂缺）——模块用哑缓冲兜底，ClusterID/MeshID 视图退化为偏红提示。
    if (depth == handles::INVALID_RESOURCE || visBuf == handles::INVALID_RESOURCE) {
        PRIMAL_LOG_STREAM(Info, "Render")
            << "[PassDebugView] unavailable: side input invalid (depth="
            << (depth == handles::INVALID_RESOURCE ? 0 : 1)
            << " vis=" << (visBuf == handles::INVALID_RESOURCE ? 0 : 1) << ")" << std::endl;
        return false;
    }
    out_depth = graph.ImportResource("PassDebug_GBufferDepth", depth);
    out_id = graph.ImportResource("PassDebug_VisibilityBuffer", visBuf);
    out_cluster_map = clusterMap;

    const auto view = static_cast<PassDebugView>(debug_view_);
    switch (view) {
        case PassDebugView::Albedo:
            out_source = graph.ImportResource("PassDebug_GBufferAlbedo", gpuDraw.GetGBufferAlbedo());
            break;
        case PassDebugView::Normal:
            out_source = graph.ImportResource("PassDebug_GBufferNormal", gpuDraw.GetGBufferNormal());
            break;
        case PassDebugView::ORM:
            out_source = graph.ImportResource("PassDebug_GBufferORM", gpuDraw.GetGBufferORM());
            break;
        case PassDebugView::Depth:
            out_source = out_depth;  // 深度视图：源 = 副输入深度
            break;
        case PassDebugView::Velocity:
            out_source = graph.ImportResource("PassDebug_GBufferVelocity", gpuDraw.GetGBufferVelocity());
            break;
        case PassDebugView::Visibility:
        case PassDebugView::MeshletID:
        case PassDebugView::ClusterID:
        case PassDebugView::MeshID:
            // ID 数据走 uint 槽（binding 5）；float 槽（binding 0）静态引用
            // 要求 FLOAT 组件类型，绑 albedo 作合法占位（ID 分支不采样它）。
            out_source = graph.ImportResource("PassDebug_GBufferAlbedo", gpuDraw.GetGBufferAlbedo());
            break;
        case PassDebugView::SSAO:
            if (!ssao_pass_ || !ssao_pass_->IsInitialized() ||
                ssao_pass_->GetFilterTexture() == handles::INVALID_RESOURCE) return false;
            out_source = graph.ImportResource("PassDebug_LumenSSAO", ssao_pass_->GetFilterTexture());
            break;
        case PassDebugView::DeferredLighting:
            if (!deferredOut.deferred_output_rg.IsValid()) return false;
            out_source = deferredOut.deferred_output_rg;
            break;
        case PassDebugView::GIIndirect:
            if (!fusionOut.output_rg.IsValid()) return false;
            out_source = fusionOut.output_rg;
            break;
        case PassDebugView::SPGI:
            if (!screen_probe_pass_ || !screen_probe_pass_->IsInitialized() ||
                screen_probe_pass_->GetFilteredOutputTexture() == handles::INVALID_RESOURCE) return false;
            out_source = graph.ImportResource("PassDebug_SPGI", screen_probe_pass_->GetFilteredOutputTexture());
            break;
        case PassDebugView::SCLightingAtlas: {
            if (!surface_cache_pass_ || !surface_cache_pass_->IsInitialized()) return false;
            // Surface Cache rotates persistent atlases from the absolute
            // frame clock (DDGISlotMap), not the swapchain callback slot.
            // Keep the debug view on the same clock as the producer so a
            // swapchain/frame-count phase difference cannot show another slot.
            const auto scView = surface_cache_pass_->GetCacheView(
                static_cast<u32>(frameCount_));
            if (scView.lighting_atlas == handles::INVALID_RESOURCE) return false;
            out_source = graph.ImportResource("PassDebug_SCLightingAtlas", scView.lighting_atlas);
            break;
        }
        // [F3-V1 2026-09-20] SC 三 atlas 视图（accessor 已存在，纯增量接线）。
        case PassDebugView::SCAlbedoAtlas: {
            if (!surface_cache_pass_ || !surface_cache_pass_->IsInitialized()) return false;
            const ResourceHandle tex = surface_cache_pass_->GetAlbedoAtlas();
            if (tex == handles::INVALID_RESOURCE) return false;
            out_source = graph.ImportResource("PassDebug_SCAlbedoAtlas", tex);
            break;
        }
        case PassDebugView::SCNormalAtlas: {
            if (!surface_cache_pass_ || !surface_cache_pass_->IsInitialized()) return false;
            const ResourceHandle tex = surface_cache_pass_->GetNormalAtlas();
            if (tex == handles::INVALID_RESOURCE) return false;
            out_source = graph.ImportResource("PassDebug_SCNormalAtlas", tex);
            break;
        }
        case PassDebugView::SCDepthAtlas: {
            if (!surface_cache_pass_ || !surface_cache_pass_->IsInitialized()) return false;
            const ResourceHandle tex = surface_cache_pass_->GetDepthAtlas();
            if (tex == handles::INVALID_RESOURCE) return false;
            out_source = graph.ImportResource("PassDebug_SCDepthAtlas", tex);
            break;
        }
        // [F3-V2 2026-09-20] SPGI 降噪前原始 gather 输出（诊断 denoise 归因）。
        case PassDebugView::SPGIRaw: {
            if (!screen_probe_pass_ || !screen_probe_pass_->IsInitialized() ||
                screen_probe_pass_->GetOutputTexture() == handles::INVALID_RESOURCE) return false;
            out_source = graph.ImportResource("PassDebug_SPGIRaw", screen_probe_pass_->GetOutputTexture());
            break;
        }
        // [F3 扩展 2026-09-20] SC 剩余存储全可视化。
        case PassDebugView::SCEmissive: {
            if (!surface_cache_pass_ || !surface_cache_pass_->IsInitialized()) return false;
            const ResourceHandle tex = surface_cache_pass_->GetEmissiveAtlas();
            if (tex == handles::INVALID_RESOURCE) return false;
            out_source = graph.ImportResource("PassDebug_SCEmissive", tex);
            break;
        }
        case PassDebugView::SCShadowVis: {
            // 数据源是 buffer（binding 6），float 槽照 ID 类视图惯例绑 albedo 占位。
            if (!surface_cache_pass_ || !surface_cache_pass_->IsInitialized()) return false;
            out_aux_buffer = surface_cache_pass_->GetShadowVisibilityBuffer();
            out_aux_count = surface_cache_pass_->GetAtlasSize();
            if (out_aux_buffer == handles::INVALID_RESOURCE || out_aux_count == 0) return false;
            out_source = graph.ImportResource("PassDebug_GBufferAlbedo", gpuDraw.GetGBufferAlbedo());
            break;
        }
        case PassDebugView::SCIndirect: {
            if (!surface_cache_pass_ || !surface_cache_pass_->IsInitialized()) return false;
            // 槽位时钟必须与写入侧一致（DDGISlotMap frame%3）——cbIdx 是
            // swapchain 槽时钟，与 GI 三缓冲时钟无同步不变量（Phase 1d
            // 审计结论①）。用 cbIdx 会让视图轮显三个年龄不同的槽。
            const ResourceHandle tex =
                surface_cache_pass_->GetIndirectOutTexture(static_cast<u32>(frameCount_));
            if (tex == handles::INVALID_RESOURCE) return false;
            out_source = graph.ImportResource("PassDebug_SCIndirect", tex);
            break;
        }
        case PassDebugView::SCTileRadiance: {
            if (!surface_cache_pass_ || !surface_cache_pass_->IsInitialized()) return false;
            const ResourceHandle tex =
                surface_cache_pass_->GetTileRadianceTexture(static_cast<u32>(frameCount_));
            if (tex == handles::INVALID_RESOURCE) return false;
            out_source = graph.ImportResource("PassDebug_SCTileRadiance", tex);
            break;
        }
        case PassDebugView::SCTileAux: {
            if (!surface_cache_pass_ || !surface_cache_pass_->IsInitialized()) return false;
            const ResourceHandle tex = surface_cache_pass_->GetTileAuxTexture();
            if (tex == handles::INVALID_RESOURCE) return false;
            out_source = graph.ImportResource("PassDebug_SCTileAux", tex);
            break;
        }
        default:
            return false;
    }
    return out_source.IsValid();
}

void StandardRenderPipeline::BuildRenderGraph(ResourceHandle backBuffer, u32 cbIdx) {
    auto& graph = *renderGraph_;
    auto& gpuDraw = nanite::GPUDrivenDrawPipeline::Get();
    // T1.2 (C3): the HZB texture's single RG identity, imported by the
    // HZBBuild pass below and reused by SSGI/SSR/SPGI imports.
    RGResourceHandle hzbRG{};

    // ======================================================================
    // GI 能量所有权修复 Phase 5b（P5，Vulkan 领先）: GBuffer 进 RG——
    // 单一规范身份（producer identity），NaniteSceneRender 以 Write 声明
    // 产出，下游 SC capture/GIGather/SPGI/SSR/Fusion 全部复用本身份，
    // 不再各自 ImportResource 造第二身份。消除的旧模式：~12 处
    // "GBuffer*_XXX" 重复导入让 RG 看不到跨消费者依赖，逼出
    // SurfaceCachePass 里的手工 GBuffer no-op barrier（本阶段删除）。
    // ======================================================================
    // env 回滚开关（Phase 5b 二分用）：PRIMAL_GBUFFER_RG=0 → 旧 per-site 导入。
    const bool gbufferInRG = device_ && device_->GetPlatform() == rhi::RHIPlatform::Vulkan &&
                             [](const char* e) { return !(e && e[0] == '0'); }(std::getenv("PRIMAL_GBUFFER_RG"));
    RGResourceHandle gbufferDepthRG{}, gbufferAlbedoRG{}, gbufferNormalRG{};
    if (gbufferInRG) {
        gbufferDepthRG = graph.ImportResource("GBufferDepth", gpuDraw.GetGBufferDepthSampleable());
        gbufferAlbedoRG = graph.ImportResource("GBufferAlbedo", gpuDraw.GetGBufferAlbedo());
        gbufferNormalRG = graph.ImportResource("GBufferNormal", gpuDraw.GetGBufferNormal());
    }

    // A13/T0.11: on a viewport size change, drop all screen-space history
    // before this frame records — TAA re-inits from the current frame instead
    // of blending a different-size history (black/mixed-frame regression),
    // SPGI drops its probe temporal accumulator.
    if (viewport_size_changed_) {
        viewport_size_changed_ = false;
        PostProcess::ResetTAAHistory();
        if (screen_probe_pass_ && screen_probe_pass_->IsInitialized()) {
            screen_probe_pass_->RequestTemporalReset();
        }
        PRIMAL_LOG_STREAM(Info, "Render")
            << "[Pipeline] Viewport resized to " << render_width_ << "x" << render_height_
            << " — screen-space history reset." << std::endl;
    }

    // T4.1: governor tick — scale the GI budgets for this frame (FPS ≥ 30 floor).
    gi_governor_.RecordFrame();
    if (ddgi_pass_ && ddgi_pass_->IsInitialized()) {
        ddgi_pass_->SetMaxProbesPerFrame(
            GIAdaptiveGovernor::ScaleBudget(gi_base_probe_budget_, gi_governor_.Scale()));
    }
    if (surface_cache_pass_ && surface_cache_pass_->IsInitialized()) {
        surface_cache_pass_->SetPageBudget(
            GIAdaptiveGovernor::ScaleBudget(gi_base_page_budget_, gi_governor_.Scale()));
    }

    // ========================================================================
    // GIHistoryValidity 最小版（GI 能量所有权修复 Phase 5a）：灯光修改事件
    // 消费。DDGI：BoostProbePriority + 30 帧 alpha=0.35 抬升窗；SC：tile
    // validity 位图清零（下一帧 AddPass 生效）+ 本帧页预算拉满（0=全量，
    // 下一帧 governor 恢复缩放）；SPGI：temporal reset。
    // ========================================================================
    if (gi_history_dirty_) {
        gi_history_dirty_ = false;
        const u32 now = static_cast<u32>(frameCount_);
        if (ddgi_pass_ && ddgi_pass_->IsInitialized()) {
            ddgi_pass_->NotifyLightingChanged(now);
        }
        if (surface_cache_pass_ && surface_cache_pass_->IsInitialized()) {
            surface_cache_pass_->RequestTileValidityReset();
            surface_cache_pass_->SetPageBudget(0u);   // 一帧页预算拉满
        }
        if (screen_probe_pass_ && screen_probe_pass_->IsInitialized()) {
            screen_probe_pass_->RequestTemporalReset();
        }
        PRIMAL_LOG_STREAM(Info, "Render")
            << "[GIHistory] lighting changed — DDGI boost+alpha-hold, SC tile"
               " reset+full page budget, SPGI temporal reset at frame " << now
            << std::endl;
    }

    // NOTE: GBuffer and culling buffers are NOT tracked through the render graph.
    // Matching TestNaniteStreamingPipeline's approach: each downstream pass imports
    // GBuffer textures under UNIQUE names so the RG sees them as unrelated resources.
    // Metal's implicit encoder boundary synchronization handles all transitions.
    // Tracking shared resources through the RG causes incorrect barrier insertion
    // that conflicts with the GPU draw pipeline's internal encoder management.

    // ========================================================================
    // Step 1: Nanite GBuffer + Shadow Culling + Raster (as render graph passes)
    // ========================================================================

    struct NanitePassData {
        nanite::GPUCullingPipeline* culling_pipeline;
        nanite::NaniteStreamingManager* streaming_manager;
        RenderSceneSnapshot* scene_snapshot;
        math::m4x4 view_matrix;
        math::m4x4 proj_matrix;
        u32 buffer_index;
        u64 frame_count;
        nanite::CullingResults culling_results;
    };

    // Pass 1a: HZB + Culling
    if (culling_pipeline_ && scene_snapshot_ && gpuDraw.IsInitialized()) {
        graph.AddPass<NanitePassData>(
            "NaniteCulling",
            RGPassType::Compute,
            [&](NanitePassData& data, RenderGraphBuilder& builder) {
                builder.SideEffect();
                data.culling_pipeline = culling_pipeline_;
                data.streaming_manager = streaming_manager_;
                data.scene_snapshot = scene_snapshot_.get();
                data.view_matrix = view_matrix_;
                data.proj_matrix = proj_matrix_;
                data.buffer_index = cbIdx;
                data.frame_count = frameCount_;
            },
            [this](const NanitePassData& data, RenderGraphContext& ctx) {
                auto* cmd = ctx.cmdBuffer;

                // GPU Culling
                if (data.culling_pipeline && data.scene_snapshot) {
                    data.culling_pipeline->Execute(
                        cmd, *data.scene_snapshot, data.view_matrix, data.proj_matrix,
                        data.streaming_manager, data.buffer_index);
                }
            }
        );

        // Pass 1b: GPU Draw — declare Read on culling buffers to force a
        // Compute→Graphics barrier, and Write on GBuffer textures for downstream passes.
        // Phase 5b: GBuffer Write 声明实际落地（Vulkan）——下游全部经
        // producer identity 读取，RG 插入 Write→Read 转换，SC 手工 barrier
        // 随之删除。Metal 依赖 encoder 边界语义，暂不声明（parity 矩阵另案）。
        graph.AddPass<NanitePassData>(
            "NaniteSceneRender",
            RGPassType::Graphics,
            [&](NanitePassData& data, RenderGraphBuilder& builder) {
                builder.SideEffect();
                if (gbufferDepthRG.IsValid()) builder.Write(gbufferDepthRG, ResourceState::ShaderResource);
                if (gbufferAlbedoRG.IsValid()) builder.Write(gbufferAlbedoRG, ResourceState::ShaderResource);
                if (gbufferNormalRG.IsValid()) builder.Write(gbufferNormalRG, ResourceState::ShaderResource);
                data.culling_pipeline = culling_pipeline_;
                data.streaming_manager = streaming_manager_;
                data.scene_snapshot = scene_snapshot_.get();
                data.view_matrix = view_matrix_;
                data.proj_matrix = proj_matrix_;
                data.buffer_index = cbIdx;
                data.frame_count = frameCount_;
            },
            [this](const NanitePassData& data, RenderGraphContext& ctx) {
                auto& gpuDraw = nanite::GPUDrivenDrawPipeline::Get();
                auto* cmd = ctx.cmdBuffer;
                if (!gpuDraw.IsInitialized() || !data.scene_snapshot) return;

                // A7: forward the editor view mode (0=shaded, 1=wireframe).
                gpuDraw.SetViewMode(view_mode_);

                auto cullingResults = data.culling_pipeline
                    ? data.culling_pipeline->GetResults()
                    : nanite::CullingResults{};

                gpuDraw.Execute(cmd, *data.scene_snapshot, data.view_matrix, data.proj_matrix,
                                cullingResults, static_cast<u32>(data.frame_count), data.buffer_index);

                // GPUDrivenDraw creates/rebuilds its borrowed geometry here;
                // rebind every actual DDGI volume before any DDGI graph pass
                // records descriptors.  This helper is also called by the
                // RenderWithCommandBuffer entry point used by the Vulkan test.
                if (!RefreshDDGIFirstNativeNormalSources())
                    std::abort();
                if (!RefreshDDGIAnalyticFirstRhoSources())
                    std::abort();

                // Step 1d: GlobalSDF voxelization — inline after NaniteSceneRender
                // (mesh buffers are now populated for this frame).
                {
                    static int s_vox_check = 0;
                    if (s_vox_check < 3) {
                        auto& gsdf = nanite::GlobalSDF::Get();
                        PRIMAL_LOG_STREAM(Info, "Render") << "[VOX_CHECK] sceneRender done, sdfInit=" << gsdf.IsInitialized()
                                  << " voxReady=" << gsdf.IsVoxelizationReady()
                                  << " cascade0Needs=" << (gsdf.GetConfig().cascade_count > 0 ? "check" : "n/a")
                                  << std::endl;
                        s_vox_check++;
                    }
                }
                auto& globalSDF = nanite::GlobalSDF::Get();
                if (globalSDF.IsInitialized()) {
                    nanite::SDFVoxelizationResources voxResources;
                    voxResources.vertex_buffer = gpuDraw.GetGlobalVertexBuffer();
                    voxResources.meshlet_buffer = gpuDraw.GetGlobalMeshletBuffer();
                    voxResources.meshlet_vertices_buffer = gpuDraw.GetGlobalMeshletVerticesBuffer();
                    voxResources.meshlet_triangles_buffer = gpuDraw.GetGlobalMeshletTrianglesBuffer();
                    voxResources.cluster_map_buffer = gpuDraw.GetClusterMapBuffer();
                    voxResources.instance_data_buffer = gpuDraw.GetGlobalInstanceDataBuffer();
                    voxResources.num_instances = data.scene_snapshot ? data.scene_snapshot->GetInstanceCount() : 0;
                    globalSDF.SetVoxelizationResources(voxResources);

                    for (u32 c = 0; c < globalSDF.GetConfig().cascade_count; ++c) {
                        if (globalSDF.CascadeNeedsVoxelization(c)) {
                            globalSDF.DispatchVoxelization(cmd, c);
                        }
                    }
                }
            }
        );

        // Pass 1c: HZB build
        // T1.2 (C3): declare the HZB write (and depth read) so the mip chain
        // gets real barriers and downstream readers (SSGI/SSR/SPGI) order
        // against this pass instead of racing it. hzbRG (declared at function
        // scope) is imported HERE once and reused downstream; it stays
        // invalid when HZB is not ready — consumers fall back then.
        if (hzb_system_ && hzb_system_->IsReady()) {
            hzbRG = graph.ImportResource("HZBTexture", hzb_system_->GetHZBTexture());
            auto depthHZB = gbufferInRG ? gbufferDepthRG : graph.ImportResource("GBufferDepth_HZB", gpuDraw.GetGBufferDepthSampleable());
            graph.AddPass<NanitePassData>(
                "HZBBuild",
                RGPassType::Compute,
                [hzbRG, depthHZB, cbIdx](NanitePassData& data, RenderGraphBuilder& builder) {
                    builder.Read(depthHZB, ResourceState::ShaderResource);
                    builder.Write(hzbRG, ResourceState::UnorderedAccess);
                    data.buffer_index = cbIdx;
                },
                [this, cbIdx](const NanitePassData& data, RenderGraphContext& ctx) {
                    auto& gpuDraw = nanite::GPUDrivenDrawPipeline::Get();
                    if (!gpuDraw.IsInitialized()) return;
                    hzb_system_->BuildHZB(gpuDraw.GetGBufferDepthSampleable(), ctx.cmdBuffer, cbIdx);
                }
            );
        }

        // Step 1d: GlobalSDF voxelization
        {
            auto& globalSDF = nanite::GlobalSDF::Get();
            // Always register the pass if SDF is initialized — buffer handles
            // are refreshed inside the execute lambda.
            if (globalSDF.IsInitialized()) {
                graph.AddPass<NanitePassData>(
                    "GlobalSDF_Voxelization",
                    RGPassType::Compute,
                    [&](NanitePassData& data, RenderGraphBuilder& builder) {
                        builder.SideEffect();
                        data.buffer_index = cbIdx;
                    },
                    [this](const NanitePassData& data, RenderGraphContext& ctx) {
                        auto& gpuDraw = nanite::GPUDrivenDrawPipeline::Get();
                        auto& globalSDF = nanite::GlobalSDF::Get();
                        if (!gpuDraw.IsInitialized()) return;

                        static int s_vox_diag = 0;
                        if (s_vox_diag < 5) {
                            PRIMAL_LOG_STREAM(Error, "Render") << "[SDF_VOX] pass executing, ready=" << globalSDF.IsVoxelizationReady()
                                      << " instances=" << (scene_snapshot_ ? scene_snapshot_->GetInstanceCount() : 0)
                                      << std::endl;
                            s_vox_diag++;
                        }

                        // Refresh buffer handles each frame (may change with streaming)
                        nanite::SDFVoxelizationResources voxResources;
                        voxResources.vertex_buffer = gpuDraw.GetGlobalVertexBuffer();
                        voxResources.meshlet_buffer = gpuDraw.GetGlobalMeshletBuffer();
                        voxResources.meshlet_vertices_buffer = gpuDraw.GetGlobalMeshletVerticesBuffer();
                        voxResources.meshlet_triangles_buffer = gpuDraw.GetGlobalMeshletTrianglesBuffer();
                        voxResources.cluster_map_buffer = gpuDraw.GetClusterMapBuffer();
                        voxResources.instance_data_buffer = gpuDraw.GetGlobalInstanceDataBuffer();
                        voxResources.num_instances = scene_snapshot_ ? scene_snapshot_->GetInstanceCount() : 0;
                        globalSDF.SetVoxelizationResources(voxResources);

                        for (u32 c = 0; c < globalSDF.GetConfig().cascade_count; ++c) {
                            if (globalSDF.CascadeNeedsVoxelization(c)) {
                                globalSDF.DispatchVoxelization(ctx.cmdBuffer, c);
                            }
                        }
                    }
                );
            }
        }
    }

    // ========================================================================
    // Step 2: Shadow Map Module
    // ========================================================================

    ShadowMapOutputs shadowOut;
    math::v3 lightDir = Normalize(settings_.lighting.light_direction);
    {
        ShadowMapInputs shadowIn;
        shadowIn.gpu_draw_pipeline = &gpuDraw;
        shadowIn.scene_snapshot = scene_snapshot_.get();
        shadowIn.camera_position = camera_position_;
        shadowIn.view_matrix = view_matrix_;
        shadowIn.proj_matrix = proj_matrix_;
        shadowIn.light_direction = lightDir;
        shadowIn.sun_angular_diameter_deg = settings_.lighting.sun_angular_diameter_deg;
        shadowIn.current_buffer_index = cbIdx;
        shadowIn.shadow_quality = settings_.quality.shadow_quality;
        // T8.13: shadow softness knobs (Hard=0 / PCF=1 / PCSS=2).
        if (const char* e = std::getenv("SHADOW_QUALITY")) {
            shadowIn.shadow_quality = (lumen::ShadowQuality)atoi(e);
        }
        shadowOut = shadow_module_->AddPasses(graph, shadowIn);
    }
    // P0: shadow 段之后输出——ShadowMapOutputs 的尺寸 provenance 已填充。
    LogSunContractOnce(settings_, view_matrix_, proj_matrix_,
                       GatherSunContractSizes(settings_, render_width_, render_height_,
                                              &shadowOut, surface_cache_pass_.get(),
                                              static_cast<u32>(frameCount_),
                                              ddgi_pass_.get()),
                       static_cast<u32>(frameCount_));

    // ========================================================================
    // Step 3: SSAO
    // ========================================================================

    lumen::LumenSSAOOutput ssaoOut{};
    if (ssao_pass_ && ssao_pass_->IsInitialized()) {
        // Import GBuffer under UNIQUE names for SSAO — the RG should NOT know
        // these are the same physical textures written by NaniteSceneRender.
        // Metal's encoder boundary synchronization handles transitions.
        auto gbufferNormalSSAO = gbufferInRG ? gbufferNormalRG : graph.ImportResource("GBufferNormal_SSAO", gpuDraw.GetGBufferNormal());
        auto gbufferDepthSSAO = gbufferInRG ? gbufferDepthRG : graph.ImportResource("GBufferDepth_SSAO", gpuDraw.GetGBufferDepthSampleable());
        lumen::SSAOCameraData camData{};
        camData.view_matrix = view_matrix_;
        camData.proj_matrix = proj_matrix_;
        camData.prev_view_matrix = prev_view_matrix_;   // A10: true previous-frame VP
        camData.prev_proj_matrix = prev_proj_matrix_;
        camData.frame_index = static_cast<u32>(frameCount_);
        camData.delta_time = settings_.advanced.delta_time_override;
        ssaoOut = ssao_pass_->AddPass(graph, gbufferNormalSSAO, gbufferDepthSSAO, camData, static_cast<u32>(frameCount_));
    }

    // ========================================================================
    // Step 4: Deferred Lighting
    // ========================================================================

    DeferredLightingOutputs deferredOut;
    if (deferred_module_) {
        // Import GBuffer under UNIQUE names for Deferred — same physical textures,
        // different RG identities.  No dependency chain with NaniteSceneRender.
        auto gbufferAlbedoDL = gbufferInRG ? gbufferAlbedoRG : graph.ImportResource("GBufferAlbedo_DL", gpuDraw.GetGBufferAlbedo());
        auto gbufferNormalDL = gbufferInRG ? gbufferNormalRG : graph.ImportResource("GBufferNormal_DL", gpuDraw.GetGBufferNormal());
        auto gbufferORMDL = graph.ImportResource("GBufferORM_DL", gpuDraw.GetGBufferORM());
        auto gbufferDepthDL = gbufferInRG ? gbufferDepthRG : graph.ImportResource("GBufferDepth_DL", gpuDraw.GetGBufferDepthSampleable());

        DeferredLightingInputs deferredIn;
        deferredIn.gpu_draw_pipeline = &gpuDraw;
        deferredIn.ssao_pass = ssao_pass_.get();
        deferredIn.view_matrix = view_matrix_;
        deferredIn.proj_matrix = proj_matrix_;
        deferredIn.camera_position = camera_position_;
        deferredIn.light_pos = math::v4{-lightDir.x, -lightDir.y, -lightDir.z, 0.0f};
        deferredIn.light_color = settings_.lighting.light_color;
        deferredIn.cascade_splits = settings_.lighting.cascade_splits;
        deferredIn.shadow_matrix0 = shadowOut.shadow_matrix[0];
        deferredIn.shadow_matrix1 = shadowOut.shadow_matrix[1];
        deferredIn.shadow_matrix2 = shadowOut.shadow_matrix[2];   // P4.2
        deferredIn.shadow_matrix3 = shadowOut.shadow_matrix[3];   // P4.2
        deferredIn.current_buffer_index = cbIdx;
        deferredIn.shadow_visibility_rg = shadowOut.shadow_visibility_rg;
        deferredIn.shadow_visibility_tex = shadowOut.shadow_visibility_tex;
        // VSM: blurred moments + mode flag (shader falls back to the R8
        // visibility path when vsm_enabled is false).
        deferredIn.vsm_enabled = shadowOut.vsm_enabled;
        deferredIn.shadow_moments_rg[0] = shadowOut.shadow_moments_rg[0];
        deferredIn.shadow_moments_rg[1] = shadowOut.shadow_moments_rg[1];
        deferredIn.shadow_moments_tex[0] = shadowOut.shadow_moments_tex[0];
        deferredIn.shadow_moments_tex[1] = shadowOut.shadow_moments_tex[1];
        deferredIn.shadow_moments_tex[2] = shadowOut.shadow_moments_tex[2];   // P4.2
        deferredIn.shadow_moments_tex[3] = shadowOut.shadow_moments_tex[3];   // P4.2
        deferredIn.shadow_translucent_rg = shadowOut.shadow_translucent_rg;   // P4.1
        deferredIn.shadow_translucent_tex = shadowOut.shadow_translucent_tex; // P4.1
        // sun-audit P2 第二步: 探针深度可见性输入（PRIMAL_PROBE_OCC 变体）
        if (ddgi_pass_ && ddgi_pass_->IsInitialized()) {
            deferredIn.probe_depth_buf = ddgi_pass_->GetDepthBufferBySlot(cbIdx);  // 槽号（swapchain 环），诊断变体
            const auto& vol = ddgi_pass_->GetVolumeData();
            deferredIn.probe_origin_spacing = math::v4{
                vol.ProbeOrigin.x, vol.ProbeOrigin.y, vol.ProbeOrigin.z, vol.ProbeSpacing};
            deferredIn.probe_counts = math::v4{
                float(vol.ProbeCounts[0]), float(vol.ProbeCounts[1]),
                float(vol.ProbeCounts[2]),
                float(vol.ProbeCounts[0] * vol.ProbeCounts[1] * vol.ProbeCounts[2])};
        }
        deferredIn.gbuffer_albedo_rg = gbufferAlbedoDL;
        deferredIn.gbuffer_normal_rg = gbufferNormalDL;
        deferredIn.gbuffer_orm_rg = gbufferORMDL;
        deferredIn.gbuffer_depth_rg = gbufferDepthDL;
        deferredOut = deferred_module_->AddPasses(graph, deferredIn);

        // P1-B（sponza-gap-audit §4.2）: 世界空间定点 HDR 探针。参考模式
        // 相机锁定 ⇒ 固定屏幕点即固定世界点。PRIMAL_GI_PROBE=1 时把融合后
        // HDR 输出的三个探针点（左上日照拱 / 右暗拱 / 中央地面，audit
        // measurements ROI 中心 ×2 render scale）拷入 3 槽 staging ring，
        // 滞后 2 帧 Map 并打 log（线性 HDR，非显示编码）。
        if (gi_probe_enabled_ && deferredOut.deferred_output_tex != handles::INVALID_RESOURCE) {
            static ResourceHandle s_probeStaging[3] = {
                handles::INVALID_RESOURCE, handles::INVALID_RESOURCE, handles::INVALID_RESOURCE};
            static bool s_probeStagingInit = false;
            const u32 probeSlot = static_cast<u32>(frameCount_) % 3;
            if (!s_probeStagingInit) {
                PRIMAL_LOG_STREAM(Info, "Render")
                    << "[GI-PROBE] staging init, deferredTex="
                    << deferredOut.deferred_output_tex << std::endl;
                BufferDesc pb{};
                pb.size = 64;  // 3 × RGBA16F 像素（8B）对齐 16
                pb.type = BufferType::Raw;
                pb.usage = GPUMemoryUsage::Readback;
                pb.memoryUsage = GPUMemoryUsage::Readback;
                for (u32 i = 0; i < 3; ++i) s_probeStaging[i] = graph.GetDevice().CreateBuffer(pb);
                s_probeStagingInit = true;
            }
            struct GIProbeData { rendergraph::RGResourceHandle rgTex; u32 slot; };
            graph.AddPass<GIProbeData>("GIProbeDump", RGPassType::Copy, RGPassCategory::Copy,
                [rgTex = deferredOut.deferred_output_rg, slot = probeSlot](GIProbeData& d, RenderGraphBuilder& builder) {
                    d.rgTex = rgTex; d.slot = slot;
                    builder.Read(rgTex, ResourceState::ShaderResource);
                },
                [this](const GIProbeData& d, RenderGraphContext& context) {
                    auto* res = context.graph->GetResource(d.rgTex);
                    ResourceHandle phys = res ? res->GetPhysicalHandle() : handles::INVALID_RESOURCE;
                    if (phys == handles::INVALID_RESOURCE ||
                        s_probeStaging[d.slot] == handles::INVALID_RESOURCE) return;
                    rhi::BufferTextureCopyRegion regions[3];
                    // 点位标定（display 720p → render ×2）：
                    //  arch_sunlit  (150,20)  显示亮度 ~166（日照拱顶亮带）
                    //  floor_sunlit (760,655) 显示亮度 ~239（日照地面亮带）
                    //  right_dark   (982,297) 显示亮度 ~56（暗拱间接光区）
                    const u32 px[3][2] = { {300, 40}, {1520, 1310}, {1940, 570} };  // right_dark (940,285)×2 亮度 ~5（纯间接光区）  // arch / right-arch / floor
                    for (u32 i = 0; i < 3; ++i) {
                        regions[i].bufferOffset = i * 16;
                        regions[i].bufferRowLength = 0;
                        regions[i].bufferImageHeight = 0;
                        regions[i].imageSubresource.mipLevel = 0;
                        regions[i].imageSubresource.baseArrayLayer = 0;
                        regions[i].imageSubresource.layerCount = 1;
                        regions[i].imageOffset = {static_cast<s32>(px[i][0]), static_cast<s32>(px[i][1]), 0};
                        regions[i].imageExtent = {1, 1, 1};
                    }
                    context.cmdBuffer->CopyTextureToBuffer(phys, s_probeStaging[d.slot], regions, 3);
                });
            // 滞后 2 帧 readback（同队列顺序提交，2 帧前的拷贝早已完成）
            {
                const u32 lagSlot = (probeSlot + 1) % 3;
                const u32 lagFrame = static_cast<u32>(frameCount_) - 2;
                if (static_cast<u32>(frameCount_) >= 2 &&
                    (lagFrame == 120 || lagFrame == 360 || lagFrame == 1200)) {
                    static bool s_probeMapLogged = false;
                    if (!s_probeMapLogged) {
                        s_probeMapLogged = true;
                        PRIMAL_LOG_STREAM(Info, "Render")
                            << "[GI-PROBE] lag readback attempt at frame " << frameCount_
                            << " lagFrame=" << lagFrame << std::endl;
                    }
                    if (void* p = graph.GetDevice().MapBuffer(s_probeStaging[lagSlot], 0, 64)) {
                        const u16* v = static_cast<const u16*>(p);
                        auto f16 = [](u16 h) {
                            u32 sign = (h & 0x8000u) << 16, expo = (h & 0x7C00u) >> 10, man = h & 0x03FFu;
                            u32 bits;
                            if (expo == 0) bits = sign;
                            else if (expo == 31) bits = sign | 0x7F800000u | (man << 13);
                            else bits = sign | ((expo - 15u + 127u) << 23) | (man << 13);
                            float f; std::memcpy(&f, &bits, 4); return f;
                        };
                        PRIMAL_LOG_STREAM(Info, "Render")
                            << "[GI-PROBE] frame=" << lagFrame
                            << " archHDR=(" << f16(v[0]) << "," << f16(v[1]) << "," << f16(v[2]) << ")"
                            << " rightArchHDR=(" << f16(v[4]) << "," << f16(v[5]) << "," << f16(v[6]) << ")"
                            << " floorHDR=(" << f16(v[8]) << "," << f16(v[9]) << "," << f16(v[10]) << ")"
                            << std::endl;
                        graph.GetDevice().UnmapBuffer(s_probeStaging[lagSlot]);
                    }
                }
            }
        }
    }

    // ========================================================================
    // Step 4.5: Volume (Froxel Fog or Ray March)
    // ========================================================================

    // Helper: collect SDF cascade RG handles (shared by both VolumePass and FroxelFogPass)
    auto collectSDFCascades = [&](auto& volIn) {
        auto& globalSDF = nanite::GlobalSDF::Get();
        if (globalSDF.IsInitialized()) {
            for (u32 c = 0; c < std::min(3u, globalSDF.GetConfig().cascade_count); ++c) {
                const auto& cascade = globalSDF.GetCascade(c);
                if (cascade.sdf_texture != handles::INVALID_RESOURCE) {
                    std::string name = "SDFCascade" + std::to_string(c) + "_Volume";
                    auto sdfRG = graph.ImportResource(name, cascade.sdf_texture);
                    if (c == 0) volIn.sdf_cascade_0 = sdfRG;
                    else if (c == 1) volIn.sdf_cascade_1 = sdfRG;
                    else if (c == 2) volIn.sdf_cascade_2 = sdfRG;
                }
            }
        }
    };

    volume::VolumeOutput volumeOut;
    if (froxel_fog_pass_ && froxel_fog_pass_->IsInitialized()) {
        volume::FroxelInputs froxelIn;
        froxelIn.gbuffer_depth = gbufferInRG ? gbufferDepthRG : graph.ImportResource("GBufferDepth_Volume", gpuDraw.GetGBufferDepthSampleable());
        froxelIn.scene_color = deferredOut.deferred_output_rg;
        froxelIn.shadow_map = shadowOut.shadow_visibility_rg;
        froxelIn.shadow_matrix0 = shadowOut.shadow_matrix[0];
        froxelIn.shadow_matrix1 = shadowOut.shadow_matrix[1];
        // P2-B（sponza-gap-audit §4.6）: 雾的阴影输入换成 light-space VSM
        // moments + 全 4 级联矩阵。屏幕空间表面可见性图不再被雾采样（屏幕 UV
        // 与 light VP UV 不对应，表面可见性不描述空间点遮挡）。
        // P1（sponza-gap-reaudit §4.4）: 同时传入 PCSS 深度图；无 moments
        // （默认 PCSS）时 FroxelFogPass 自动切换 shadow_mode=2 深度比较，
        // 不再恒 lit。级联数按 4 传入，模式由纹理有效性在 pass 内决定。
        // SetFroxelShadowsEnabled(false)（FROXEL_NO_SHADOW）→ 恒 lit 对照组。
        if (froxel_shadows_enabled_) {
            for (u32 c = 0; c < 4; ++c) {
                froxelIn.shadow_moments[c] = shadowOut.shadow_moments_rg[c];
                froxelIn.shadow_matrices[c] = shadowOut.shadow_matrix[c];
                froxelIn.shadow_depth[c] = shadowOut.shadow_depth_rg[c];
            }
            froxelIn.shadow_cascade_count = 4u;
        }
        collectSDFCascades(froxelIn);

        // WP2（froxel-upgrade 计划 §4）: 上一帧 SPGI 过滤输出作为雾环境项。
        // 雾积分（Step 4.5）先于本帧 SPGI 重写该纹理执行，读到的是最近完成
        // 帧——体积雾低频，上一帧辐照可接受（计划认可的退路）。SPGI 关闭时
        // 句柄 INVALID，integrate 走硬编码天空色 fallback（G1-G5 基准行为）。
        // 有意不走 RG 跟踪：SPGI pass 自管理该纹理，跨帧直读与 froxel
        // 模块自有纹理同模式。
        if (screen_probe_pass_)
            froxelIn.spgi_ambient_tex = screen_probe_pass_->GetFilteredOutputTexture();

        volume::VolumeCameraData volCam{};
        volCam.camera_position = camera_position_;
        volCam.view_matrix = view_matrix_;
        volCam.proj_matrix = proj_matrix_;
        volCam.light_direction = Normalize(settings_.lighting.light_direction);
        volCam.light_color = {settings_.lighting.light_color.x, settings_.lighting.light_color.y, settings_.lighting.light_color.z};
        volCam.frame_index = static_cast<u32>(frameCount_);
        froxelIn.camera_data = volCam;
        froxelIn.width = render_width_;
        froxelIn.height = render_height_;

        auto froxelOut = froxel_fog_pass_->AddPass(graph, froxelIn);
        volumeOut.volume_scatter = froxelOut.volume_scatter;
        volumeOut.valid = froxelOut.valid;
    } else if (volume_pass_ && volume_pass_->IsInitialized()) {
        volume::VolumeInputs volIn;
        volIn.gbuffer_depth = gbufferInRG ? gbufferDepthRG : graph.ImportResource("GBufferDepth_Volume", gpuDraw.GetGBufferDepthSampleable());
        volIn.scene_color = deferredOut.deferred_output_rg;
        volIn.shadow_map = shadowOut.shadow_visibility_rg;
        collectSDFCascades(volIn);

        volume::VolumeCameraData volCam{};
        volCam.camera_position = camera_position_;
        volCam.view_matrix = view_matrix_;
        volCam.proj_matrix = proj_matrix_;
        volCam.light_direction = Normalize(settings_.lighting.light_direction);
        volCam.light_color = {settings_.lighting.light_color.x, settings_.lighting.light_color.y, settings_.lighting.light_color.z};
        volCam.frame_index = static_cast<u32>(frameCount_);
        volIn.camera_data = volCam;
        volIn.width = render_width_;
        volIn.height = render_height_;
        volumeOut = volume_pass_->AddPass(graph, volIn);
    }

    // ========================================================================
    // Step 4.6: Fluid Render
    // ========================================================================

    fluid::FluidOutput fluidOut;
    if (fluid_render_pass_ && fluid_render_pass_->IsInitialized()) {
        fluid::FluidInputs fluidIn;
        fluidIn.view_matrix = view_matrix_;
        fluidIn.proj_matrix = proj_matrix_;
        fluidIn.camera_position = camera_position_;
        fluidIn.scene_color = deferredOut.deferred_output_rg;
        fluidIn.gbuffer_depth = gbufferInRG ? gbufferDepthRG : graph.ImportResource("GBufferDepth_Fluid", gpuDraw.GetGBufferDepthSampleable());
        fluidIn.width = render_width_;
        fluidIn.height = render_height_;
        fluidOut = fluid_render_pass_->AddPass(graph, fluidIn);
    }

    // ========================================================================
    // Step 5: Surface Cache
    // ========================================================================

    // [双稳态修复] GI 内容就绪闩锁：全部异步内容（离线 SDF / GlobalSDF
    // 体素化 / 场景快照）就位的首帧。GI 暖启动时钟 gi_frame 从此帧计数，
    // 消除异步加载时序导致的 SC↔probe 双稳态分支选择。
    TryLatchGIContentReady();
    const u32 giFrame = GIFrame(static_cast<u32>(frameCount_));

    RGResourceHandle scLightingRG;
    RGResourceHandle scNodirectRG;   // [E0-B]
    if (surface_cache_pass_ && surface_cache_pass_->IsInitialized()) {
        lumen::SurfaceCacheFrameData frameData{};
        frameData.camera_position = {camera_position_.x, camera_position_.y, camera_position_.z};
        frameData.frame_index = static_cast<u32>(frameCount_);
        // T3.2/T3.2+: 方向与颜色来自单一太阳合同（真实太阳 light_color 进
        // SC 备用色；灯表经 FillSCLightList × SC_DIRECT_SCALE）。
        FillSurfaceCacheSunLight(frameData, settings_, current_scene_);

        // GBuffer-gather capture: real rendered surfaces → card atlas.
        frameData.inv_view_projection = Inverse(proj_matrix_ * view_matrix_);
        frameData.render_width  = render_width_;
        frameData.render_height = render_height_;
        frameData.gbuffer_depth  = gpuDraw.GetGBufferDepthSampleable();
        frameData.gbuffer_albedo = gpuDraw.GetGBufferAlbedo();
        frameData.gbuffer_normal = gpuDraw.GetGBufferNormal();

        // Probe-fed indirect (Lumen hierarchy): previous-frame near-volume
        // irradiance — SC runs before the DDGI update, so previousIndex is
        // the last fully integrated result. Shadowed texels gather their
        // bounce from these probes instead of collapsing to black.
        // Phase 1d: 槽位单一事实源 DDGISlotMap.h。
        if (ddgi_pass_ && ddgi_pass_->IsInitialized()) {
            const auto ddgiSlots = lumen::DDGISlotsForFrame(static_cast<u32>(frameCount_));
            const auto& vol = ddgi_pass_->GetVolumeData();
            frameData.ddgi_irradiance = ddgi_pass_->GetIrradianceBufferBySlot(ddgiSlots.previousIndex);
            // C 系列：同槽位探针深度——LightEval gather 的 Chebyshev 可见性。
            frameData.ddgi_depth = ddgi_pass_->GetDepthBufferBySlot(ddgiSlots.previousIndex);
            // GI 根治 Phase 3.2: 同源 openSky 统计（天空分类）。
            frameData.ddgi_opensky = ddgi_pass_->GetOpenSkyBuffer();
            frameData.ddgi_origin = {vol.ProbeOrigin.x, vol.ProbeOrigin.y, vol.ProbeOrigin.z};
            frameData.ddgi_spacing = vol.ProbeSpacing;
            frameData.ddgi_counts[0] = vol.ProbeCounts[0];
            frameData.ddgi_counts[1] = vol.ProbeCounts[1];
            frameData.ddgi_counts[2] = vol.ProbeCounts[2];
            // C1-H3 hardening: only flag the feed valid when the buffer handle
            // is real — ddgi_valid=1 with the card_dispatch stand-in bound made
            // LightEval walk a 48B-struct array as a 9×float4-per-probe buffer.
            frameData.ddgi_valid =
                (frameData.ddgi_irradiance != handles::INVALID_RESOURCE) ? 1u : 0u;
        }

        // G2 capture 侧（2026-09-16）: 卡片直射可见性消费与表面同源的 PCSS
        // 深度（薄墙不再被粗 SDF 漏光烘焙成假直射）
        for (u32 c = 0; c < 4; ++c) {
            frameData.shadow_depth[c] = shadowOut.shadow_depth_rg[c];
            frameData.shadow_matrices[c] = shadowOut.shadow_matrix[c];
        }
        frameData.shadow_cascade_count = 4u;

        ResourceHandle nullLightBuf{handles::INVALID_RESOURCE};
        // T8.12: card-view capture (SC_CARDVIEW=1) — runs instead of the
        // screen-space gather inside AddPass.
        if (surface_cache_pass_->IsCardViewEnabled() && scene_snapshot_) {
            surface_cache_pass_->AddCardCapturePasses(graph, *scene_snapshot_,
                                                      static_cast<u32>(frameCount_));
        }
        {
            lumen::SurfaceCacheFrameData frameDataGi = frameData;
            frameDataGi.gi_frame = giFrame;
            const auto scOut = surface_cache_pass_->AddPass(graph, RGResourceHandle{}, nullLightBuf,
                                                            frameDataGi, static_cast<u32>(frameCount_));
            // T1.1 (C2): reuse the producer's own RG identity instead of
            // re-importing the same physical atlas as "SCLightingAtlas".
            scLightingRG = scOut.lighting_atlas;
            scNodirectRG = scOut.lighting_nodirect_atlas;   // [E0-B]
        }

        if (screen_probe_pass_ && screen_probe_pass_->IsInitialized()) {
            // X2/A3: SC 数据走统一 Cache View（atlas_size/cardCount 与旧
            // settings 传值同源，上传逐字节等价）
            const auto scView = surface_cache_pass_->GetCacheView(static_cast<u32>(frameCount_));
            // 2026-09-22 排障：scAvail=NO（Vulkan sponza）一次性打印各字段
            {
                static bool scDbg = false;
                if (!scDbg) { scDbg = true;
                    PRIMAL_LOG_STREAM(Error, "Render")
                        << "[SC-DIAG] atlas=" << scView.atlas_size
                        << " lighting=" << (scView.lighting_atlas != rhi::handles::INVALID_RESOURCE)
                        << " card_data=" << (scView.card_data != rhi::handles::INVALID_RESOURCE)
                        << " card_lookup=" << (scView.card_lookup != rhi::handles::INVALID_RESOURCE)
                        << " grid_offsets=" << (scView.grid_offsets != rhi::handles::INVALID_RESOURCE)
                        << " grid_cards=" << (scView.grid_cards != rhi::handles::INVALID_RESOURCE)
                        << " max_cards=" << scView.max_card_count
                        << " card_count=" << scView.card_count << std::endl; }
            }
            screen_probe_pass_->SetSurfaceCacheData(scView);
            // Opt-in validated first-surface owner qualification. SC owner IDs
            // are exactly snapshot order; ambiguous proxy contracts stay unknown.
            std::vector<math::u32v4> zeroOwners;
            const char* firstEnv=std::getenv("PRIMAL_SPGI_ZERO_HIT_GEOMETRY");
            if(scene_snapshot_ && device_->GetPlatform()==rhi::RHIPlatform::Vulkan && firstEnv &&
               (std::strcmp(firstEnv,"1")==0 || std::strcmp(firstEnv,"2")==0)) {
                for(const auto& inst:scene_snapshot_->GetInstanceData()) {
                    bool found=false,supported=true;
                    for(const auto& proxy:GetCurrentScene()->GetProxies()) {
                        const auto* cache=cluster::get(proxy.meshId);
                        if(!cache || !cache->exists || cache->geometry_content_id!=inst.geometry_id || proxy.materialId!=inst.material_id ||
                           std::memcmp(&proxy.transform,&inst.world_matrix,sizeof(inst.world_matrix))!=0) continue;
                        found=true;
                        supported &= proxy.technique==ShaderTechnique::Opaque && proxy.base_color[3]==1.0f && proxy.graphVariant==0;
                    }
                    zeroOwners.push_back(math::u32v4{inst.material_id,found&&supported?1u:0u,u32(zeroOwners.size()),u32(frameCount_)});
                }
            }
            screen_probe_pass_->SetFirstSurfaceInputs(surface_cache_pass_.get(),std::move(zeroOwners),
                nanite::GPUDrivenDrawPipeline::Get().GetMaterialDataBuffer());
        }

        if (ddgi_pass_ && ddgi_pass_->IsInitialized()) {
            // X2/A3: SC→DDGI trace 注入走统一 Cache View
            ddgi_pass_->SetSurfaceCacheResources(
                surface_cache_pass_->GetCacheView(static_cast<u32>(frameCount_)));
        }
    }

    // ========================================================================
    // Step 6: DDGI
    // ========================================================================

    lumen::LumenDDGIOutput ddgiOut;
    if (ddgi_pass_ && ddgi_pass_->IsInitialized()) {
        lumen::DDGICameraData camData{};
        camData.camera_position = camera_position_;
        camData.view_matrix = view_matrix_;
        camData.proj_matrix = proj_matrix_;
        camData.prev_view_matrix = prev_view_matrix_;   // A10: true previous-frame VP
        camData.prev_proj_matrix = prev_proj_matrix_;
        FillDDGISunLight(camData, settings_);
        camData.frame_index = static_cast<u32>(frameCount_);
        camData.gi_frame_index = giFrame;
        camData.delta_time = settings_.advanced.delta_time_override;

        RGResourceHandle prevColor;
        if (deferredOut.deferred_output_tex != handles::INVALID_RESOURCE) {
            prevColor = graph.ImportResource("PrevFrameColor", deferredOut.deferred_output_tex);
        }
        // B11/Phase 2.3: 回灌 pre-gain GI 缓冲（FusionIndirect 输出）——
        // 融合增益与直射不再进探针回路（直射供给由 SC 卡片负责）。
        // Phase 2a: 默认断边（env =1 回接）。断边态 radiance_depth 置无效
        // → finalize validity.y=0 → samplePrevColor 短路，prev-screen 分支
        // 落 analytic——**不**绑 deferred 直射输出当 radiance（防"直射绕过
        // 遮挡二次入账"复发，审计 §2.2/计划 §2a 决策）。
        if (RGResourceHandle fused = MakeProbeRadianceSource(graph, cbIdx); fused.IsValid()) {
            prevColor = fused;
            camData.radiance_is_previous_frame = true;
        }
        if (device_->GetPlatform() == rhi::RHIPlatform::Vulkan) {
            camData.screen_depth = gbufferInRG ? gbufferDepthRG : graph.ImportResource("DDGI_CurrentScreenDepth", gpuDraw.GetGBufferDepthSampleable());
            if (camData.radiance_is_previous_frame) {
                if (depth_history_ && !camera_cut_this_frame_) {
                    const auto previous = depth_history_->GetPreviousFrameDepth(static_cast<u32>(frameCount_));
                    if (previous.is_valid && previous.texture != handles::INVALID_RESOURCE)
                        camData.radiance_depth = graph.ImportResource("DDGI_RadianceDepth", previous.texture);
                }
            }
            // 断边默认态：radiance_depth 保持无效 = 屏域 radiance 档关断
            // （省一次历史深度解析）；screen_depth 仍供 albedo/遮挡查询。
        }

        ddgiOut = ddgi_pass_->AddPass(graph, prevColor, camData,
                                       static_cast<u32>(frameCount_), scLightingRG);

        // T5.5: secondary volumes (no SC atlas — card data is near-field).
        if (lumen::GIDebug().ddgi_far_volume) {
            for (u32 vi = 1; vi < ddgi_volume_count_; ++vi) {
                if (ddgi_volumes_[vi] && ddgi_volumes_[vi]->IsInitialized()) {
                    ddgi_volumes_[vi]->AddPass(graph, prevColor, camData,
                                               static_cast<u32>(frameCount_), RGResourceHandle{});
                }
            }
        }
    }

    // ========================================================================
    // Step 6.7: [WRC] World Space Radiance Cache（设计文档 §4 Stream A）。
    // PRIMAL_WRC_ENABLE=0 时 wrc_volume_count_==0，本块整体跳过（零行为
    // 变化）。能量单向律：WRC 只在此被 SC 填充（trace 采样），消费在 SPGI
    // 远场（fallback_spgi_to_wrc 边，A3）——绝不回灌 SC/DDGI。
    // ========================================================================
    if (wrc_volume_count_ > 0) {
        for (u32 vi = 0; vi < wrc_volume_count_; ++vi) {
            if (!wrc_volumes_[vi] || !wrc_volumes_[vi]->IsInitialized()) continue;
            if (vi == 0 && surface_cache_pass_) {
                // X2/A3 同款视图签名：SC 数据走统一 Cache View（独立接线
                // block，§1.2-2）。SC 首帧完成前强制绑 dummy 2D（占位在
                // WRC dispatch 内一次性转换到 SRO）——避免 SC atlas 首帧
                // 布局/内容未定时进入采样描述符。
                if (frameCount_ >= 1) {
                    wrc_volumes_[vi]->SetSurfaceCacheResources(
                        surface_cache_pass_->GetCacheView(static_cast<u32>(frameCount_)));
                } else {
                    wrc_volumes_[vi]->ClearSurfaceCacheResources();
                }
                // [反弹修复] 卡片→探针直投影的邻域表供给（卡片数变化时
                // 内部自动重建）。
                auto& cardGen = surface_cache_pass_->GetCardGenerator();
                wrc_volumes_[vi]->UpdateProbeCardList(
                    cardGen.GetCards().data(), (u32)cardGen.GetCards().size());
            }
            lumen::WRCCameraData wrcCam{};
            wrcCam.light_direction = rhi::math::Normalize(settings_.lighting.light_direction);
            wrcCam.light_color = {settings_.lighting.ddgi_light_color.x,
                                  settings_.lighting.ddgi_light_color.y,
                                  settings_.lighting.ddgi_light_color.z};
            // 天空单一事实源：与 DDGI 同一 GIDebug 档位（中性色调，T8.13 P4）。
            // [2026-09-20 观感轮] 天空色改中性 1.0：旧 {tier,tier,tier} 与
            // SkyColor.w 档位相乘成 tier²（0.15² = 0.0225），WRC 逃逸射线的
            // 天空供给被压死——二层中庭的天光梯度（Lumen 参考的主要来源）
            // 因此消失。档位语义由 w 单次承载。
            wrcCam.sky_color = {1.0f, 1.0f, 1.0f};
            wrcCam.frame_index = static_cast<u32>(frameCount_);
            const RGResourceHandle scRG = (vi == 0) ? scLightingRG : RGResourceHandle{};
            wrc_volumes_[vi]->AddPass(graph, wrcCam, static_cast<u32>(frameCount_), scRG);
        }
    }

    // ========================================================================
    // Step 7: SC→DDGI Integration
    // ========================================================================

    // X3/A6 轴4: propagate_sc_to_ddgi=false 跳过喂食边（RG 不声明该 pass，
    // DDGI 回落自身 trace 链）；默认 true 保持 SC→DDGI AddPass 不变。
    if (settings_.lumen.axes.propagate_sc_to_ddgi &&
        sc_ddgi_module_ && surface_cache_pass_ && ddgi_pass_) {
        SCDDGIInputs scDDGIIn;
        scDDGIIn.ddgi_pass = ddgi_pass_.get();
        scDDGIIn.surface_cache_pass = surface_cache_pass_.get();
        scDDGIIn.frame_index = static_cast<u32>(frameCount_);
        scDDGIIn.current_buffer_index = cbIdx;
        scDDGIIn.sc_lighting_rg = scLightingRG;
        scDDGIIn.ddgi_irradiance = ddgiOut.ddgi_irradiance;
        scDDGIIn.ddgi_irradiance_hist = ddgiOut.ddgi_irradiance_hist;
        sc_ddgi_module_->AddPass(graph, scDDGIIn);
    }

    // ========================================================================
    // Step 8: SSGI (Screen Space GI)
    // ========================================================================

    lumen::LumenSSGIOutput ssgiOut;
    // T8.13: SSGI/SPGI are alternative screen-space GI solutions — running
    // both costs 31ms GPU (18.3+12.6, 85% of frame) with overlapping
    // contribution (T8.7 A/B: SSGI ≈ 0 where SPGI exists). Policy: SPGI
    // active ⇒ SSGI passes skipped; SSGI_FORCE=1 keeps both for A/B.
    const bool spgiActive = screen_probe_pass_ && screen_probe_pass_->IsInitialized();
    const bool ssgiSuppressed = spgiActive && !std::getenv("SSGI_FORCE");
    if (ssgi_pass_ && ssgi_pass_->IsInitialized() && !ssgiSuppressed) {
        auto gbufferNormalSSGI = gbufferInRG ? gbufferNormalRG : graph.ImportResource("GBufferNormal_SSGI", gpuDraw.GetGBufferNormal());
        auto gbufferDepthSSGI = gbufferInRG ? gbufferDepthRG : graph.ImportResource("GBufferDepth_SSGI", gpuDraw.GetGBufferDepthSampleable());
        auto gbufferVelocitySSGI = graph.ImportResource("GBufferVelocity_SSGI", gpuDraw.GetGBufferVelocity());
        // T1.2 (C3): reuse the HZBBuild producer identity when it exists
        auto hzbHandle = hzbRG.IsValid()
            ? hzbRG
            : graph.ImportResource("HZBTexture_SSGI", hzb_system_->GetHZBTexture());

        RGResourceHandle prevColorSSGI;
        if (deferredOut.deferred_output_tex != handles::INVALID_RESOURCE) {
            prevColorSSGI = graph.ImportResource("PrevFrameColor_SSGI", deferredOut.deferred_output_tex);
        }

        lumen::SSGICameraData ssgiCam{};
        ssgiCam.view_matrix = view_matrix_;
        ssgiCam.proj_matrix = proj_matrix_;
        ssgiCam.prev_view_matrix = prev_view_matrix_;   // A10: true previous-frame VP
        ssgiCam.prev_proj_matrix = prev_proj_matrix_;
        ssgiCam.frame_index = static_cast<u32>(frameCount_);
        ssgiCam.delta_time = 0.016f;

        ssgiOut = ssgi_pass_->AddPass(graph, gbufferNormalSSGI, gbufferDepthSSGI,
            gbufferVelocitySSGI, hzbHandle, prevColorSSGI,
            ssgiCam, static_cast<u32>(frameCount_),
            hzb_system_->GetMipLevels());
    }

    // ========================================================================
    // Step 8.5: SSR (Screen-Space Reflections)
    // ========================================================================
    // SSR traces reflection rays through the HZB, temporally accumulates, then
    // upsamples to full-res with roughness attenuation. The output is a pure
    // reflection-color texture that FusionComposite blends additively into the
    // scene. GBuffer ORM (.g=roughness, .b=metallic) gates reflection strength.
    PostProcess::SSRPassData ssrOut;
    if (settings_.quality.enable_ssr && frameCount_ > 0) {
        RGResourceHandle hdrSSR;
        if (deferredOut.deferred_output_rg.IsValid()) {
            hdrSSR = deferredOut.deferred_output_rg;
        } else if (deferredOut.deferred_output_tex != handles::INVALID_RESOURCE) {
            hdrSSR = graph.ImportResource("SSR_HdrSource", deferredOut.deferred_output_tex);
        }
        auto depthSSR = gbufferInRG ? gbufferDepthRG : graph.ImportResource("GBufferDepth_SSR", gpuDraw.GetGBufferDepthSampleable());
        // T1.2 (C3): reuse the HZBBuild producer identity when it exists
        auto hzbSSR = hzbRG.IsValid()
            ? hzbRG
            : graph.ImportResource("HZBTexture_SSR", hzb_system_->GetHZBTexture());
        auto velSSR   = graph.ImportResource("GBufferVelocity_SSR", gpuDraw.GetGBufferVelocity());
        auto ormSSR   = graph.ImportResource("GBufferORM_SSR", gpuDraw.GetGBufferORM());

        math::m4x4 invProj = Inverse(proj_matrix_);
        ssrOut = PostProcess::AddSSRPass(graph, hdrSSR, depthSSR, hzbSSR, velSSR, ormSSR,
            render_width_, render_height_, static_cast<u32>(frameCount_),
            proj_matrix_, invProj, settings_.ssr);
    }

    // ========================================================================
    // Step 9: Screen Probe GI
    // ========================================================================

    lumen::ScreenProbeGIOutput spgiOut;
    if (screen_probe_pass_ && screen_probe_pass_->IsInitialized() && frameCount_ > 0) {
        // [WRC] A3（fallback_spgi_to_wrc 边）：远场段优先消费 WRC。near volume
        // 的 cache view + 消费计数缓冲传入；边关断/WRC 未启用 → 空视图
        // （shader 走冻结的 DDGI 回退边，行为与 A3 前一致）。
        {
            bool wrcEdge = settings_.lumen.wrc_spgi_consume &&
                           wrc_volume_count_ > 0 &&
                           wrc_volumes_[0] && wrc_volumes_[0]->IsInitialized();
            if (wrcEdge) {
                screen_probe_pass_->SetWRCCacheData(
                    wrc_volumes_[0]->GetCacheView(static_cast<u32>(frameCount_)),
                    wrc_volumes_[0]->GetConsumeCounterBuffer(), true);
            } else {
                screen_probe_pass_->SetWRCCacheData(lumen::WRCCacheView{},
                                                    rhi::handles::INVALID_RESOURCE, false);
            }
        }
        // T2.2: DDGI probe fallback for miss rays (hist slot — one frame
        // behind, no read/write race with this frame's DDGI update).
        if (ddgi_pass_ && ddgi_pass_->IsInitialized()) {
            if (settings_.lumen.axes.fallback_spgi_to_ddgi) {
                // X2/A3: volume 数据走统一 Cache View（ddgi_pass_ 别名 ddgi_volumes_[0]）。
                // Phase 1d: previousIndex = 上一完成帧（原 frame+2 裸算）。
                screen_probe_pass_->SetDDGIFallbackData(
                    ddgi_pass_->GetVolumeViewBySlot(
                        lumen::DDGISlotsForFrame(static_cast<u32>(frameCount_)).previousIndex));
            } else {
                // X3/A6 轴4: fallback 边关断 → Clear 语义（空 view 置
                // ddgi_fallback_available_=false，miss 回落天空常量）
                screen_probe_pass_->SetDDGIFallbackData(lumen::DDGIVolumeView{});
            }
        }
        auto spDepth = gbufferInRG ? gbufferDepthRG : graph.ImportResource("GBufferDepth_SP", gpuDraw.GetGBufferDepthSampleable());
        auto spNormal = gbufferInRG ? gbufferNormalRG : graph.ImportResource("GBufferNormal_SP", gpuDraw.GetGBufferNormal());

        RGResourceHandle spRadiance;
        if (deferredOut.deferred_output_rg.IsValid()) {
            spRadiance = deferredOut.deferred_output_rg;
        } else {
            spRadiance = graph.ImportResource("SPBlackFallback", black_texture_);
        }

        lumen::ScreenProbeCameraData spCam{};
        spCam.view_matrix = view_matrix_;
        spCam.proj_matrix = proj_matrix_;
        spCam.prev_view_matrix = prev_view_matrix_;
        spCam.prev_proj_matrix = prev_proj_matrix_;
        if (camera_cut_this_frame_ && screen_probe_pass_) {
            screen_probe_pass_->RequestTemporalReset();   // A11
        }
        // T1.1 (C2): single RG identity for the SC lighting atlas
        if (screen_probe_pass_ && scLightingRG.IsValid()) {
            screen_probe_pass_->SetExternalLightingRG(scLightingRG);
            screen_probe_pass_->SetExternalNoDirectLightingRG(scNodirectRG);   // [E0-B]
        }
        spCam.camera_position = camera_position_;
        spCam.frame_index = static_cast<u32>(frameCount_);

        // T5.1: HZB near-field input for the hybrid split trace.
        RGResourceHandle spHzb;
        if (hzb_system_) {
            // T1.2 (C3): reuse the HZBBuild producer identity when it exists
            spHzb = hzbRG.IsValid()
                ? hzbRG
                : graph.ImportResource("HZBTexture_SPGI", hzb_system_->GetHZBTexture());
            screen_probe_pass_->SetHZBData(hzb_system_->GetHZBTexture(),
                                           hzb_system_->GetMipLevels());
        }
        spgiOut = screen_probe_pass_->AddPass(graph, spDepth, spNormal,
            spRadiance, spCam, static_cast<u32>(frameCount_), spHzb);

        // M1 采集：SPGI gather 输出（Denoise 前）+ GBuffer 特征 → Welford
        // 累积（PRIMAL_NEURAL_CAPTURE=1 门控；未激活时零足迹）。
        if (neural_capture_module_ && neural_capture_module_->IsInitialized()) {
            NeuralCaptureInputs capIn;
            capIn.spgi_gi_output = spgiOut.gi_output;
            capIn.gbuffer_depth = spDepth;
            capIn.gbuffer_normal = spNormal;
            capIn.render_width = target_width_ > 0 ? target_width_ : render_width_;
            capIn.render_height = target_height_ > 0 ? target_height_ : render_height_;
            capIn.current_buffer_index = static_cast<u32>(frameCount_);
            capIn.camera_cut = camera_cut_this_frame_;
            neural_capture_module_->AddPass(graph, capIn);
        }
 
        // M1 神经降噪推理（env 门控；与捕获共用同一 SPGI 输出口）。
        if (neural_denoise_module_ && neural_denoise_module_->IsInitialized()) {
            NeuralDenoiseInputs dnIn;
            dnIn.spgi_gi_output = spgiOut.gi_output;
            dnIn.gbuffer_depth = spDepth;
            dnIn.gbuffer_normal = spNormal;
            dnIn.render_width = target_width_ > 0 ? target_width_ : render_width_;
            dnIn.render_height = target_height_ > 0 ? target_height_ : render_height_;
            dnIn.current_buffer_index = static_cast<u32>(frameCount_);
            neural_denoise_module_->AddPass(graph, dnIn);
        }
        // NR transformer 推理（PRIMAL_NEURAL_NR 门控；与降噪共用 SPGI 输出口）。
        if (neural_nr_module_ && neural_nr_module_->IsInitialized()) {
            NeuralNRInputs nrIn;
            nrIn.spgi_gi_output = spgiOut.gi_output;
            nrIn.gbuffer_depth = spDepth;
            nrIn.gbuffer_normal = spNormal;
            nrIn.render_width = target_width_ > 0 ? target_width_ : render_width_;
            nrIn.render_height = target_height_ > 0 ? target_height_ : render_height_;
            nrIn.current_buffer_index = static_cast<u32>(frameCount_);
            neural_nr_module_->AddPass(graph, nrIn);
        }
    }

    // ========================================================================
    // Step 9: GI Gather (DDGI → half-res screen texture)
    // ========================================================================

    GIGatherOutputs giOut;
    if (gi_gather_module_ && ddgi_pass_ && ddgi_pass_->IsInitialized() && lumen::GIDebug().gi_gather) {
        // Import GBuffer under UNIQUE names for GIGather — same physical textures,
        // different RG identities.
        auto gbufferDepthGI = gbufferInRG ? gbufferDepthRG : graph.ImportResource("GBufferDepthGI", gpuDraw.GetGBufferDepthSampleable());
        auto gbufferNormalGI = gbufferInRG ? gbufferNormalRG : graph.ImportResource("GBufferNormalGI", gpuDraw.GetGBufferNormal());
        GIGatherInputs giIn;
        giIn.ddgi_pass = ddgi_pass_.get();
        giIn.ddgi_far_pass = (ddgi_volume_count_ > 1) ? ddgi_volumes_[1].get() : nullptr;
        // X2/A3: 远场 volume 走 Cache View 直传（槽位与模块内 ddgiReadIdx 一致）。
        // Phase 1d: GetVolumeView(frame) 内部派生 currentIndex（本帧完成槽）。
        giIn.far_volume_view = (ddgi_volume_count_ > 1 && ddgi_volumes_[1])
            ? ddgi_volumes_[1]->GetVolumeView(static_cast<u32>(frameCount_)) : lumen::DDGIVolumeView{};
        // T6.3 sub-B: 全 volume 视图直传（模块内按 descriptor indexing 能力位
        // 门控 N-volume 路径；能力不可用/有效 volume < 2 时零行为变化）。
        giIn.volume_count = ddgi_volume_count_;
        for (u32 vi = 0; vi < ddgi_volume_count_ && vi < GIGatherInputs::kMaxVolumes; ++vi)
            if (ddgi_volumes_[vi]) giIn.volume_views[vi] = ddgi_volumes_[vi]->GetVolumeView(static_cast<u32>(frameCount_));
        giIn.static_probe_volume = static_probe_volume_.get();
        giIn.gbuffer_depth = gpuDraw.GetGBufferDepthSampleable();
        giIn.gbuffer_normal = gpuDraw.GetGBufferNormal();
        giIn.gbuffer_orm = gpuDraw.GetGBufferORM();
        giIn.gbuffer_depth_rg = gbufferDepthGI;
        giIn.gbuffer_normal_rg = gbufferNormalGI;
        auto gbufferOrmGI = gbufferInRG ? graph.ImportResource("GBufferOrm", gpuDraw.GetGBufferORM()) : graph.ImportResource("GBufferOrmGI", gpuDraw.GetGBufferORM());
        giIn.gbuffer_orm_rg = gbufferOrmGI;
        giIn.view_matrix = view_matrix_;
        giIn.proj_matrix = proj_matrix_;
        giIn.current_buffer_index = cbIdx;
        giIn.frame_index = static_cast<u32>(frameCount_);
        giIn.fallback_texture = black_texture_;
        giOut = gi_gather_module_->AddPasses(graph, giIn);
    }

    // ========================================================================
    // Step 10: GI Fusion Composite
    // ========================================================================

    FusionOutputs fusionOut;
    if (fusion_module_ && (settings_.quality.enable_ssgi || settings_.quality.enable_ssr ||
                           settings_.quality.enable_screen_probes) && lumen::GIDebug().fusion) {
        FusionInputs fusionIn;
        fusionIn.primary_input_rg = deferredOut.deferred_output_rg;
        fusionIn.primary_input_tex = deferredOut.deferred_output_tex;
        fusionIn.ssgi_rg = ssgiOut.ssgi_output;
        fusionIn.ssgi_tex = handles::INVALID_RESOURCE; // RG-resolved inside module
        fusionIn.ddgi_rg = giOut.gi_output_rg;
        fusionIn.ddgi_tex = giOut.gi_output_tex;
        fusionIn.spgi_rg = spgiOut.gi_output;
        fusionIn.spgi_tex = handles::INVALID_RESOURCE; // RG-resolved inside module
        fusionIn.spgi_pass = screen_probe_pass_.get();
        fusionIn.gbuffer_albedo = gpuDraw.GetGBufferAlbedo();
        fusionIn.ssao_rg = ssaoOut.ssao_output;
        fusionIn.ssao_tex = ssao_pass_ ? ssao_pass_->GetFilterTexture() : black_texture_;
        fusionIn.ssr_rg = ssrOut.outputColor;
        fusionIn.ssr_tex = handles::INVALID_RESOURCE;  // RG-resolved inside module
        fusionIn.gbuffer_orm = gpuDraw.GetGBufferORM();
        fusionIn.ddgi_specular_rg = giOut.gi_specular_rg;
        fusionIn.ddgi_specular_tex = giOut.gi_specular_tex;
        fusionIn.volume_scatter_rg = volumeOut.volume_scatter;
        fusionIn.volume_scatter_tex = handles::INVALID_RESOURCE; // RG-resolved inside module
        // sun-audit P1: GI_LADDER_DUMP 能量台账输入（vis 半分辨率 R8 + invVP）
        fusionIn.shadow_vis_tex = shadowOut.shadow_visibility_tex;
        fusionIn.inv_view_projection = Inverse(proj_matrix_ * view_matrix_);
        fusionIn.current_buffer_index = cbIdx;
        fusionIn.frame_index = static_cast<u32>(frameCount_);
        fusionIn.render_width = render_width_;
        fusionIn.render_height = render_height_;
        fusionIn.black_texture = black_texture_;
        // WRC 计划 Stream C1（Fusion 档位化）：High/Ultra（use_screen_probes）
        // 像素只读 SPGI，Medium 只读 DDGI——档位结构不变，见 FusionInputs 注释。
        fusionIn.pixel_gi_spgi_only = settings_.lumen.axes.use_screen_probes;

        // [REFL R4/R5] 屏幕外 specular 兜底：SSR composite 之后、fusion 之前。
        if (refl_specular_pass_ && refl_specular_pass_->IsInitialized()) {
            lumen::ReflSpecularTracePass::FrameData reflIn{};
            reflIn.depth_rg = gbufferDepthRG;
            reflIn.albedo_rg = gbufferAlbedoRG;
            reflIn.depth = gpuDraw.GetGBufferDepthSampleable();
            reflIn.albedo = gpuDraw.GetGBufferAlbedo();
            reflIn.orm = gpuDraw.GetGBufferORM();
            reflIn.ssr_rg = ssrOut.outputColor;
            reflIn.normal = gpuDraw.GetGBufferNormal();
            reflIn.inv_view_proj = Inverse(proj_matrix_ * view_matrix_);
            reflIn.camera_pos = camera_position_;
            reflIn.width = render_width_;
            reflIn.height = render_height_;
            if (surface_cache_pass_ && surface_cache_pass_->IsInitialized()) {
                reflIn.sc = surface_cache_pass_->GetCacheView(static_cast<u32>(frameCount_));
            }
            if (wrc_volume_count_ > 0 && wrc_volumes_[0] && wrc_volumes_[0]->IsInitialized()) {
                reflIn.wrc = wrc_volumes_[0]->GetCacheView(static_cast<u32>(frameCount_));
                reflIn.ray_max_distance = wrc_volumes_[0]->GetParams().ray_max_distance;
            }
            if (offline_sdf_texture_ != handles::INVALID_RESOURCE && offline_sdf_resolution_ > 0) {
                reflIn.offline_sdf = offline_sdf_texture_;
                reflIn.offline_sdf_origin = offline_sdf_origin_;
                reflIn.offline_sdf_extent = offline_sdf_extent_;
                reflIn.offline_sdf_resolution = offline_sdf_resolution_;
                reflIn.signed_sdf = offline_sdf_signed_;
            }
            reflIn.sky_color = {1.0f, 1.0f, 1.0f};   // 同 WRC：去 tier 双重压缩
            reflIn.sky_tier = lumen::WRCSkyTier();
            reflIn.probe_mode = ReflProbeEnabled();
            fusionIn.refl_specular_rg = refl_specular_pass_->AddPass(
                graph, reflIn, static_cast<u32>(frameCount_));
            fusionIn.refl_specular_tex = refl_specular_pass_->GetOutputTex(
                static_cast<u32>(frameCount_));
        }

        // Lightmap 旁路（2026-10-19 计划，M3）：帧注册 + UV pre-pass，在融合
        // 链内部（fusion AddPasses 的两 pass 之间由插针追加 composite）。
        // fusion 未激活时 lightmap 一并休眠——它改写的是 fusion 的间接信号。
        if (lightmap_module_) {
            if (current_scene_) lightmap_module_->UpdateFrame(*current_scene_);
            LightmapModule::FrameInputs lmFrame;
            lmFrame.frame_index = static_cast<u32>(frameCount_);
            lmFrame.render_width = render_width_;
            lmFrame.render_height = render_height_;
            lmFrame.view_projection = proj_matrix_ * view_matrix_;
            lmFrame.gbuffer_depth_rg = gbufferDepthRG;
            lightmap_module_->AddUVPrePass(graph, lmFrame);
        }

        fusionOut = fusion_module_->AddPasses(graph, fusionIn);
    }

    // ========================================================================
    // Step 10.5: PostProcess — TAA + Bloom + ToneMapping
    // ========================================================================
    // TAA resolves the jittered HDR (from GPUDrivenDraw vertex shader jitter)
    // into clean HDR using velocity-based reprojection + YCoCg variance clip.
    // Bloom extracts bright areas and blurs them for soft glow. ToneMapping
    // converts HDR→LDR (ACES) and composites Bloom + AO + SSGI.
    // Order: TAA (on jittered HDR) → Bloom → ToneMapping.
    RGResourceHandle postProcessInputRG;
    if (fusionOut.output_tex != handles::INVALID_RESOURCE) {
        postProcessInputRG = fusionOut.output_rg;
    } else if (deferredOut.deferred_output_rg.IsValid()) {
        postProcessInputRG = deferredOut.deferred_output_rg;
    }

    // ========================================================================
    // Step 10.45: Planar reflection composite (optional, both platforms)
    // ========================================================================
    // Mirror quad blended over the HDR frame BEFORE TAA so the reflection is
    // temporally resolved together with the scene.
    if (reflection_module_ && reflection_module_->IsEnabled() && postProcessInputRG.IsValid() &&
        scene_snapshot_) {
        PlanarReflectionInputs reflIn;
        reflIn.scene_snapshot = scene_snapshot_.get();
        reflIn.view_matrix = view_matrix_;
        reflIn.proj_matrix = proj_matrix_;
        reflIn.camera_position = camera_position_;
        math::v3 mLDir = Normalize(settings_.lighting.light_direction);
        reflIn.light_dir = math::v4{-mLDir.x, -mLDir.y, -mLDir.z, 0.0f};  // TO light
        reflIn.light_color = settings_.lighting.light_color;
        reflIn.current_buffer_index = cbIdx;
        reflection_module_->AddPasses(graph, postProcessInputRG, reflIn);
    }

    PostProcess::ToneMappingPassData toneOut;
    RGResourceHandle taaResolvedColor; // TONE_OFF 时 FinalBlit 的回退输入
    if (postProcessInputRG.IsValid()) {
        // TAA will resolve the frame — enable the sub-pixel jitter injection.
        // T4.6.5 followup: TAAPass now has working SPIR-V (Vulkan) + MSL (Metal)
        // shaders, triple-buffered history with resize handling, so jitter is
        // safe to enable on both platforms.
        gpuDraw.SetJitterEnabled(taa_jitter_enabled_);
        // TAA — velocity from GBuffer MRT (NDC-space, matches TAA.comp expectation).
        auto velTAA = graph.ImportResource("GBufferVelocity_TAA", gpuDraw.GetGBufferVelocity());
        // Depth reprojection: curr/prev depth + view-projection pair. Active on
        // Vulkan/Metal once the first depth-history store has completed; until
        // then (or when history is invalid) AddTAAPass keeps the velocity path.
        PostProcess::TAADepthInputs taaDepth;
        PostProcess::TAADepthInputs* taaDepthPtr = nullptr;
        {
            const auto prevDepth = depth_history_
                ? depth_history_->GetPreviousFrameDepth(static_cast<u32>(frameCount_))
                : nanite::DepthHistoryManager::DepthBuffer{};
            if (has_prev_taa_view_proj_ && prevDepth.is_valid &&
                prevDepth.texture != handles::INVALID_RESOURCE &&
                gpuDraw.GetGBufferDepthSampleable() != handles::INVALID_RESOURCE) {
                const math::m4x4 currVP = proj_matrix_ * view_matrix_;
                taaDepth.currDepth = gpuDraw.GetGBufferDepthSampleable();
                taaDepth.prevDepth = prevDepth.texture;
                taaDepth.currInvViewProj = Inverse(currVP);
                taaDepth.prevViewProj = prev_taa_view_proj_;
                taaDepthPtr = &taaDepth;
            }
        }
        auto taaOut = PostProcess::AddTAAPass(graph, postProcessInputRG, velTAA,
            render_width_, render_height_, static_cast<u32>(frameCount_), taaDepthPtr);
        RGResourceHandle hdrClean = taaOut.output;
        taaResolvedColor = hdrClean;

        // Bloom owns independent per-frame images (BloomPass.cpp), so it can
        // be enabled for the reference Vulkan HDR path without relying on the
        // transient pool's aliasing decisions.  Explicit env values remain an
        // A/B switch for captures and diagnostics.
        auto bloomOut = ShouldEnableBloom(graph.GetDevice())
            ? PostProcess::AddBloomPass(graph, hdrClean, cbIdx, MakeBloomParams())
            : PostProcess::BloomPassData{};

        // FusionIndirect has already applied AO and blended SSGI/DDGI/SPGI.
        // Do not feed AO/SSGI to ToneMapping again or the legacy post-process
        // composition path will count them a second time.
        const bool giAlreadyComposited = fusionOut.output_tex != handles::INVALID_RESOURCE;
        // TONE_OFF=1 诊断开关（同 TAA_OFF/BLOOM_OFF）：跳过 ToneMapping，
        // FinalBlit 回退直读 hdrClean。
        toneOut = std::getenv("TONE_OFF")
            ? PostProcess::ToneMappingPassData{}
            : PostProcess::AddToneMappingPass(
                graph,
                hdrClean,                 // TAA-resolved HDR scene color
                bloomOut.bloomOutput,     // Bloom blur result
                giAlreadyComposited ? RGResourceHandle{} : ssaoOut.ssao_output,
                giAlreadyComposited ? RGResourceHandle{} : ssgiOut.ssgi_output,
                velTAA,                   // velocity
                static_cast<u32>(frameCount_));  // sun-audit P3: 绝对帧号（fi 取模不变），[ToneAdapt] 采样用
    }

    // ========================================================================
    // Step 10.55 (sun-audit P1 第二层): SC atlas card-space 定点读回
    // ========================================================================
    {
        struct ScLadderHit {
            u32 res = 0, ox = 0, oy = 0, axis = 0, dir = 0;
            float cx = 0, cy = 0, cz = 0;
            float u = 0, v = 0;   // 查询点在卡片内的归一坐标
            rhi::ResourceHandle buf{handles::INVALID_RESOURCE};
        };
        static std::vector<ScLadderHit> scLadderHits;
        static bool scLadderDone = false;

        // ② frame 382：映射打印（fence 已由 BeginFrame 等待，2 帧前拷贝完成）
        if (scLadderDone && !scLadderHits.empty() && frameCount_ == 382) {
            auto f16 = [](u16 h) {
                const u32 sign = (h >> 15) & 1u, ex = (h >> 10) & 0x1Fu, mant = h & 0x3FFu;
                if (ex == 0u) return 0.0f;
                u32 bits = (sign << 31) | ((ex + 112u) << 23) | (mant << 13);
                float f; std::memcpy(&f, &bits, 4); return f;
            };
            double bestMax = 0.0;
            for (auto& h : scLadderHits) {
                if (h.buf == handles::INVALID_RESOURCE) continue;
                const u64 bytes = u64(h.res) * h.res * 8;
                const u16* t = static_cast<const u16*>(device_->MapBuffer(h.buf, 0, bytes));
                if (!t) { device_->DestroyBuffer(h.buf); h.buf = handles::INVALID_RESOURCE; continue; }
                double sum = 0.0; float mx = 0.0f; u64 n = 0;
                float mr = 0, mg = 0, mb = 0;
                for (u32 y = 0; y < h.res; y += 2) {
                    for (u32 x = 0; x < h.res; x += 2) {
                        const u16* p = t + ((size_t)y * h.res + x) * 4;
                        const float r = f16(p[0]), g = f16(p[1]), b = f16(p[2]);
                        if (std::isnan(r)) continue;
                        const float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                        sum += lum; ++n;
                        if (lum > mx) { mx = lum; mr = r; mg = g; mb = b; }
                    }
                }
                // 查询点 (u,v) 处的定点纹素（3×3 邻域均值）
                const u32 tx = std::min(h.res - 1u, u32(h.u * h.res));
                const u32 ty = std::min(h.res - 1u, u32(h.v * h.res));
                double tsum = 0.0; float tmax = 0.0f; u32 tn = 0;
                float tr = 0, tg = 0, tb = 0;
                for (int oy2 = -1; oy2 <= 1; ++oy2) {
                    for (int ox2 = -1; ox2 <= 1; ++ox2) {
                        const int px = int(tx) + ox2, py = int(ty) + oy2;
                        if (px < 0 || py < 0 || px >= int(h.res) || py >= int(h.res)) continue;
                        const u16* p = t + ((size_t)py * h.res + px) * 4;
                        const float r = f16(p[0]), g = f16(p[1]), b = f16(p[2]);
                        if (std::isnan(r)) continue;
                        const float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                        tsum += lum; ++tn;
                        if (lum > tmax) { tmax = lum; tr = r; tg = g; tb = b; }
                    }
                }
                device_->UnmapBuffer(h.buf);
                device_->DestroyBuffer(h.buf);
                h.buf = handles::INVALID_RESOURCE;
                const double mean = n ? sum / n : 0.0;
                const double texelLum = tn ? tsum / tn : 0.0;
                bestMax = std::max(bestMax, double(mx));
                const char* axn = h.axis == 0 ? "X" : (h.axis == 1 ? "Y" : "Z");
                std::fprintf(stderr,
                    "[SCLadder] card axis=%s dir=%u res=%u atlas=(%u,%u) world=(%.2f,%.2f,%.2f) "
                    "queryUV=(%.2f,%.2f) texel_lum=%.4f maxTexel=(%.3f,%.3f,%.3f) "
                    "card_mean_lum=%.4f card_max_lum=%.4f\n",
                    axn, h.dir, h.res, h.ox, h.oy, h.cx, h.cy, h.cz,
                    h.u, h.v, texelLum, tr, tg, tb, mean, mx);
            }
            const char* verdict = bestMax >= 0.5 ? "sc_source_lit(A-excluded)"
                                : (bestMax < 0.1 ? "sc_source_dark" : "sc_source_dim");
            std::fprintf(stderr,
                "[SCLadderVerdict] best_card_max_lum=%.4f -> %s | "
                "若 sc_source_lit 且 GI_LADDER 判 A：命中/方向错（查卡片深度、SDF 覆盖）；"
                "若 sc_source_dark：查太阳方向/阴影/材质（SC 源面已暗）\n",
                bestMax, verdict);
            scLadderHits.clear();
        }

        // ① frame 380：CPU 卡片查找（与 SurfaceCacheCapture/LightEval 同一 uv
        // 约定）+ 记录卡片 rect 拷贝 pass。世界点 = 中央地面 ROI 中心视线 ∩
        // y=0 平面。
        if (std::getenv("GI_LADDER_DUMP") && scLightingRG.IsValid() &&
            surface_cache_pass_ && frameCount_ == 380 && !scLadderDone) {
            const math::m4x4 invVP = Inverse(proj_matrix_ * view_matrix_);
            auto xform = [&invVP](float x, float y, float z, float out[4]) {
                const float xs[4] = {x, y, z, 1.0f};
                for (int rr = 0; rr < 4; ++rr) out[rr] = 0.0f;
                for (int cc = 0; cc < 4; ++cc)
                    for (int rr = 0; rr < 4; ++rr) out[rr] += invVP.columns[cc][rr] * xs[cc];
            };
            float n0[4], n1[4];
            xform(0.4727f * 2.0f - 1.0f, 1.0f - 2.0f * 0.9167f, 0.0f, n0);
            xform(0.4727f * 2.0f - 1.0f, 1.0f - 2.0f * 0.9167f, 1.0f, n1);
            const math::v3 p0{n0[0] / std::max(n0[3], 1e-6f), n0[1] / std::max(n0[3], 1e-6f), n0[2] / std::max(n0[3], 1e-6f)};
            const math::v3 p1{n1[0] / std::max(n1[3], 1e-6f), n1[1] / std::max(n1[3], 1e-6f), n1[2] / std::max(n1[3], 1e-6f)};
            math::v3 query{0.0f, 0.0f, 0.0f};
            if (std::abs(p1.y - p0.y) > 1e-5f) {
                const float t = (0.0f - p0.y) / (p1.y - p0.y);
                if (t > 0.0f) {
                    query.x = p0.x + (p1.x - p0.x) * t;
                    query.y = 0.0f;
                    query.z = p0.z + (p1.z - p0.z) * t;
                }
            }
            // GI_LADDER_QUERY=x,y,z：覆盖查询点（如直射光区旁的墙面/布料，
            // 审计分支"靠近直射区的表面是否收到 bounce"的定点测量）。
            if (const char* q = std::getenv("GI_LADDER_QUERY")) {
                float qx, qy, qz;
                if (std::sscanf(q, "%f,%f,%f", &qx, &qy, &qz) == 3) {
                    query = math::v3{qx, qy, qz};
                    std::fprintf(stderr, "[SCLadder] query overridden by GI_LADDER_QUERY=%s\n", q);
                }
            }
            // GI_LADDER_QUERY=x,y,z：覆盖查询点（如直射光区旁的墙面/布料，
            // 审计分支"靠近直射区的表面是否收到 bounce"的定点测量）。
            if (const char* q = std::getenv("GI_LADDER_QUERY")) {
                float qx, qy, qz;
                if (std::sscanf(q, "%f,%f,%f", &qx, &qy, &qz) == 3) {
                    query = math::v3{qx, qy, qz};
                    std::fprintf(stderr, "[SCLadder] query overridden by GI_LADDER_QUERY=%s\n", q);
                }
            }
            std::fprintf(stderr,
                "[SCLadder] query world point = (%.3f, %.3f, %.3f) (ROI 中心视线 ∩ y=0)\n",
                query.x, query.y, query.z);

            const auto& cards = surface_cache_pass_->GetCardGenerator().GetCards();
            // 容差匹配：AABB 外 0.75u 内也算命中（墙面/布面是薄板，空中
            // 查询点贴面偏移可命中），按 AABB 距离升序取最近 4 张。
            constexpr float kMatchTol = 0.75f;
            struct RankedCard { float dist; float vol; const lumen::SurfaceCacheCard* p; };
            std::vector<RankedCard> ranked;
            for (const auto& card : cards) {
                const float cx = card.center.x, cy = card.center.y, cz = card.center.z;
                const float ex = card.extent.x, ey = card.extent.y, ez = card.extent.z;
                const float dx = std::max(std::abs(query.x - cx) - ex, 0.0f);
                const float dy = std::max(std::abs(query.y - cy) - ey, 0.0f);
                const float dz = std::max(std::abs(query.z - cz) - ez, 0.0f);
                const float dist = std::sqrt(dx*dx + dy*dy + dz*dz);
                if (dist <= kMatchTol) ranked.push_back({dist, ex * ey * ez, &card});
            }
            // 同距离时优先更小体积的卡片（贴墙/贴面薄板优先于庭院级巨卡）。
            // 比较器只用元素自身字段（std::sort 的枢轴临时副本上 &e-data()
            // 会越界——上一版UB）。
            std::sort(ranked.begin(), ranked.end(), [](const RankedCard& a, const RankedCard& b) {
                if (a.dist != b.dist) return a.dist < b.dist;
                return a.vol < b.vol;
            });
            for (const auto& rc : ranked) {
                const auto& [d, vol, pc] = rc;
                if (scLadderHits.size() >= 4) break;
                const auto& card = *pc;
                if (card.resolution == 0) continue;
                const float cx = card.center.x, cy = card.center.y, cz = card.center.z;
                const float ex = card.extent.x, ey = card.extent.y, ez = card.extent.z;
                const u32 axis = card.axis_direction & 0xFFu;
                const u32 dir = (card.axis_direction >> 8) & 0xFFu;
                float u = 0.0f, v = 0.0f;
                if (axis == 0) {
                    u = (query.y - cy) / (2.0f * ey) + 0.5f;
                    v = (query.z - cz) / (2.0f * ez) + 0.5f;
                } else if (axis == 1) {
                    u = (query.x - cx) / (2.0f * ex) + 0.5f;
                    v = (query.z - cz) / (2.0f * ez) + 0.5f;
                } else {
                    u = (query.x - cx) / (2.0f * ex) + 0.5f;
                    v = (query.y - cy) / (2.0f * ey) + 0.5f;
                }
                if (u <= 0.0f || u >= 1.0f || v <= 0.0f || v >= 1.0f) continue;
                ScLadderHit h;
                h.res = card.resolution; h.ox = card.atlas_offset_x; h.oy = card.atlas_offset_y;
                h.axis = axis; h.dir = dir; h.cx = cx; h.cy = cy; h.cz = cz;
                h.u = u; h.v = v;
                BufferDesc bd{};
                bd.size = u64(h.res) * h.res * 8;
                bd.bindFlags = (u32)BufferUsageFlags::TransferDst;
                bd.memoryUsage = GPUMemoryUsage::Readback;
                h.buf = device_->CreateBuffer(bd);
                if (h.buf != handles::INVALID_RESOURCE) scLadderHits.push_back(h);
            }
            scLadderDone = true;

            if (scLadderHits.empty()) {
                std::fprintf(stderr, "[SCLadder] no card AABB contains the query point\n");
            } else {
                struct SCLadderData { rhi::ResourceHandle atlas; };
                auto* hitsPtr = &scLadderHits;
                // atlas 物理句柄（三缓冲当前槽）；不通过 RG Read(CopySource)
                // 声明——与同帧其它 ShaderResource 读冲突会让 RG 状态解析
                // 死循环（实测 906% CPU 挂死）。改为 SideEffect pass + 主 cmd
                // 手动 SR→CopySource→SR 屏障（跟踪器在同一 CB 内解析）。
                const rhi::ResourceHandle atlasTex =
                    surface_cache_pass_->GetCacheView(static_cast<u32>(frameCount_)).lighting_atlas;
                graph.AddPass<SCLadderData>("SCLadderReadback",
                    rendergraph::RGPassType::Compute, rendergraph::RGPassCategory::Lighting,
                    [atlasTex](SCLadderData&, rendergraph::RenderGraphBuilder& builder) {
                        builder.SideEffect();
                    },
                    [atlasTex, hitsPtr](const SCLadderData&, rendergraph::RenderGraphContext& context) {
                        if (atlasTex == handles::INVALID_RESOURCE) return;
                        auto* cmd = context.cmdBuffer;
                        std::fprintf(stderr, "[SCLadder] readback pass executing, hits=%zu\n", hitsPtr->size());
                        ResourceBarrier toCopy{};
                        toCopy.resource = atlasTex;
                        toCopy.beforeState = ResourceState::ShaderResource;
                        toCopy.afterState = ResourceState::CopySource;
                        toCopy.subresource = 0xFFFFFFFF;
                        toCopy.queueFamily = 0xFFFFFFFF;
                        cmd->InsertBarrier(&toCopy, 1);
                        for (const auto& h : *hitsPtr) {
                            BufferTextureCopyRegion rr{};
                            rr.imageSubresource.layerCount = 1;
                            rr.imageOffset = {(int)h.ox, (int)h.oy, 0};
                            rr.imageExtent = {h.res, h.res, 1};
                            cmd->CopyTextureToBuffer(atlasTex, h.buf, &rr, 1);
                        }
                        ResourceBarrier toSR{};
                        toSR.resource = atlasTex;
                        toSR.beforeState = ResourceState::CopySource;
                        toSR.afterState = ResourceState::ShaderResource;
                        toSR.subresource = 0xFFFFFFFF;
                        toSR.queueFamily = 0xFFFFFFFF;
                        cmd->InsertBarrier(&toSR, 1);
                    });
            }
            }
        }

    // ========================================================================
    // Cel shading on the tone-mapped LDR frame: color quantization + depth
    // Sobel edges. Runs before the GeometryDebug overlay so debug colors stay
    // unquantized.
    RGResourceHandle postToonColor;
    if (toneOut.output.IsValid()) postToonColor = toneOut.output;
    if (toon_enabled_ && postToonColor.IsValid()) {
        auto toonDepth = graph.ImportResource("GBufferDepth_Toon", gpuDraw.GetGBufferDepthSampleable());
        auto toonNormal = graph.ImportResource("GBufferNormal_Toon", gpuDraw.GetGBufferNormal());
        auto toonOut = renderpass::AddToonPass(graph, postToonColor, toonDepth, toonNormal,
                                               toon_params_, cbIdx, render_width_, render_height_);
        if (toonOut.toonOutput.IsValid()) postToonColor = toonOut.toonOutput;
    }

    // ========================================================================
    // Step 10.7: Geometry debug overlay (optional, both platforms)
    // ========================================================================
    // Meshlet / SDF-slice / vector-field / voxel visualizations blended onto
    // the tone-mapped frame. Debug pipelines lazy-init on the first enabled
    // frame; view comes from the Render()/RenderWithCommandBuffer() argument.
    if (geometry_debug_settings_.enable && frame_view_ && postToonColor.IsValid()) {
        auto gbufferDepthGD = graph.ImportResource("GBufferDepth_GeometryDebug",
                                                   gpuDraw.GetGBufferDepthSampleable());
        AddGeometryDebugPass(graph, postToonColor, gbufferDepthGD,
                             *frame_view_, &geometry_debug_settings_);
    }

    // ========================================================================
    // Step 11: Final Blit → BackBuffer
    //         (or SDF Visualization if toggled on)
    // ========================================================================

    // Output node of whatever produced the final LDR image (blit or SDF viz);
    // the probe-sphere overlay must chain after it for RG ordering.
    RGResourceHandle finalOutputNode{};

    // ---- Pass Debug View：源解析（视图激活且目标可用时替换 Step 11 输出）----
    // 解析失败（pass 未启用/内容未就绪）回退正常链并只提示一次。
    bool passDebugActive = false;
    RGResourceHandle pdSource{}, pdId{}, pdDepth{};
    ResourceHandle pdClusterMap{handles::INVALID_RESOURCE};
    ResourceHandle pdAuxBuffer{handles::INVALID_RESOURCE};
    u32 pdAuxCount = 0;
    if (debug_view_ != static_cast<u32>(PassDebugView::Off) &&
        pass_debug_view_module_ && pass_debug_view_module_->IsInitialized()) {
        passDebugActive = ResolvePassDebugViewSource(graph, cbIdx, deferredOut, fusionOut,
                                                     pdSource, pdId, pdDepth, pdClusterMap,
                                                     pdAuxBuffer, pdAuxCount);
        if (!passDebugActive && !debug_view_fallback_warned_) {
            debug_view_fallback_warned_ = true;
            PRIMAL_LOG_STREAM(Info, "Render")
                << "[Pipeline] PassDebugView '" << GetPassDebugViewName(debug_view_)
                << "' target unavailable this frame — falling back to FinalBlit" << std::endl;
        }
    }

    if (passDebugActive) {
        PassDebugViewInputs dvIn;
        dvIn.source_rg = pdSource;
        dvIn.id_rg = pdId;
        dvIn.depth_rg = pdDepth;
        dvIn.cluster_map_buffer = pdClusterMap;
        dvIn.aux_buffer = pdAuxBuffer;
        dvIn.aux_count = pdAuxCount;
        dvIn.backbuffer_rg = graph.ImportResource("BackBuffer", backBuffer);
        dvIn.current_buffer_index = cbIdx;
        dvIn.render_width = target_width_ > 0 ? target_width_ : render_width_;
        dvIn.render_height = target_height_ > 0 ? target_height_ : render_height_;
        dvIn.view = static_cast<PassDebugView>(debug_view_);
        dvIn.inv_view_proj = proj_matrix_ * view_matrix_;  // 保留（反投影诊断用）
        dvIn.proj = proj_matrix_;
        // 近远平面：引擎约定 0.1/1000（SSAOFilter.metal 同值）； Depth 视图线性化用。
        dvIn.near_plane = 0.1f;
        dvIn.far_plane = 1000.0f;
                dvIn.cluster_count = nanite::GPUDrivenDrawPipeline::Get().GetClusterMapEntryCount();
                dvIn.output_format = current_output_format_;
                pass_debug_view_module_->AddPass(graph, dvIn);
        finalOutputNode = dvIn.backbuffer_rg;
    } else if (sdf_visualization_enabled_ && sdf_viz_module_ && sdf_viz_module_->IsInitialized()) {
        // SDF visualization replaces the normal scene output.
        SDFVisualizationInputs vizIn;
        vizIn.backbuffer_rg = graph.ImportResource("BackBuffer", backBuffer);
        vizIn.current_buffer_index = cbIdx;
        vizIn.render_width = target_width_ > 0 ? target_width_ : render_width_;
        vizIn.render_height = target_height_ > 0 ? target_height_ : render_height_;
        vizIn.camera_position = { camera_position_.x, camera_position_.y,
                                  camera_position_.z, 0.0f };
        vizIn.view_matrix = view_matrix_;
        vizIn.proj_matrix = proj_matrix_;
        vizIn.light_direction = { settings_.lighting.light_direction.x,
                                  settings_.lighting.light_direction.y,
                                  settings_.lighting.light_direction.z, 0.0f };
        vizIn.light_intensity = 3.0f;
        // Use offline SDF if available; otherwise fall back to GlobalSDF cascades.
        if (has_offline_sdf_) {
            vizIn.use_offline_sdf = true;
            vizIn.offline_sdf_texture = offline_sdf_texture_;
            vizIn.offline_sdf_origin = offline_sdf_origin_;
            vizIn.offline_sdf_extent = offline_sdf_extent_;
            vizIn.offline_sdf_resolution = offline_sdf_resolution_;
        }
        sdf_viz_module_->AddPass(graph, vizIn);
    } else if (neural_debug_enabled_ && neural_debug_module_ &&
               neural_debug_module_->IsInitialized()) {
        // M0 神经渲染 debug 视图：左半屏 MLP 推理重建，右半屏 GT 参考函数。
        NeuralDebugInputs ndIn;
        ndIn.backbuffer_rg = graph.ImportResource("BackBuffer", backBuffer);
        ndIn.current_buffer_index = cbIdx;
        ndIn.render_width = target_width_ > 0 ? target_width_ : render_width_;
        ndIn.render_height = target_height_ > 0 ? target_height_ : render_height_;
        neural_debug_module_->AddPass(graph, ndIn);
    } else if (final_blit_module_) {
        FinalBlitInputs blitIn;
        // Prefer the tone-mapped LDR output (Toon-processed when enabled);
        // fall back to HDR composite if PostProcess was skipped (e.g. no valid
        // input texture this frame). 只决定 RG 句柄，物理句柄由 FinalBlit 在
        // exec 时解析（同 RWCB 路径的说明）。
        if (postToonColor.IsValid()) {
            blitIn.input_rg = postToonColor;
        }
        // TONE_OFF 诊断路径：ToneMapping 被跳过时直读 TAA 解析后的 HDR。
        if (!blitIn.input_rg.IsValid() && taaResolvedColor.IsValid()) {
            blitIn.input_rg = taaResolvedColor;
        }
        if (!blitIn.input_rg.IsValid() &&
            fusionOut.output_tex != handles::INVALID_RESOURCE) {
            blitIn.input_rg = fusionOut.output_rg;
            blitIn.input_tex = fusionOut.output_tex;
        }
        if (!blitIn.input_rg.IsValid() &&
            deferredOut.deferred_output_tex != handles::INVALID_RESOURCE) {
            blitIn.input_rg = deferredOut.deferred_output_rg;
            blitIn.input_tex = deferredOut.deferred_output_tex;
        }
        if (const char* debug = std::getenv("PRIMAL_LIGHTING_DEBUG");
            debug && device_->GetPlatform() == rhi::RHIPlatform::Vulkan) {
            if (std::strcmp(debug, "normal") == 0 || std::strcmp(debug, "ndotl") == 0 ||
                std::strcmp(debug, "shadow") == 0 || std::strcmp(debug, "albedo") == 0) {
                blitIn.input_rg = deferredOut.deferred_output_rg;
                blitIn.input_tex = deferredOut.deferred_output_tex;
            }
        }
        blitIn.backbuffer_rg = graph.ImportResource("BackBuffer", backBuffer);
        blitIn.current_buffer_index = cbIdx;
        blitIn.render_width = target_width_ > 0 ? target_width_ : render_width_;
        blitIn.render_height = target_height_ > 0 ? target_height_ : render_height_;
        // R2.1 神经帧生成：回读当帧 blit 输入 + 上传上轮插值结果；
        // present 开启且插值帧就绪时 FinalBlit 换源为插值纹理。
        if (neural_framegen_module_ && neural_framegen_module_->IsInitialized()) {
            NeuralFrameGenInputs fgIn;
            fgIn.blit_input = blitIn.input_rg;
            fgIn.render_width = blitIn.render_width;
            fgIn.render_height = blitIn.render_height;
            fgIn.current_buffer_index = cbIdx;
            fgIn.input_is_hdr = false;
            fgIn.present_interpolated = framegen_present_;
            neural_framegen_module_->AddPass(graph, fgIn);
            if (framegen_present_ && neural_framegen_module_->PresentReady()) {
                blitIn.input_rg = graph.ImportTexture(
                    "FrameGenPresent", neural_framegen_module_->OutputTexture(),
                    neural_framegen_module_->OutputDesc(),
                    rhi::ResourceState::ShaderResource);
                blitIn.input_tex = handles::INVALID_RESOURCE;
            }
        }
        finalOutputNode = blitIn.backbuffer_rg;
        final_blit_module_->AddPass(graph, blitIn);
    }

    // M8/T8.5: probe sphere overlay — after the tonemapped blit so spheres
    // composite over the final LDR image, depth-occluded by the gbuffer.
    if (probe_sphere_debug_ && probe_sphere_module_ && probe_sphere_module_->IsInitialized() &&
        ddgi_pass_ && ddgi_pass_->IsInitialized()) {
        const auto& vol = ddgi_pass_->GetVolumeData();
        const u32 probeCount = vol.ProbeCountTotal;
        if (probeCount > 0) {
            DebugProbeSphereInputs psIn;
            psIn.backbuffer_rg = graph.ImportResource("BackBuffer_ProbeSphere", backBuffer);
            psIn.blit_output_rg = finalOutputNode;
            psIn.current_buffer_index = cbIdx;
            psIn.render_width = target_width_ > 0 ? target_width_ : render_width_;
            psIn.render_height = target_height_ > 0 ? target_height_ : render_height_;
            psIn.view_matrix = view_matrix_;
            psIn.proj_matrix = proj_matrix_;
            psIn.camera_position = {camera_position_.x, camera_position_.y,
                                    camera_position_.z, 0.0f};
            math::v3 toLight = -Normalize(settings_.lighting.light_direction);
            psIn.sun_dir = {toLight.x, toLight.y, toLight.z, 0.0f};
            psIn.probe_origin = {vol.ProbeOrigin.x, vol.ProbeOrigin.y,
                                 vol.ProbeOrigin.z};
            psIn.probe_counts[0] = ddgi_pass_->GetParams().probe_count_x;
            psIn.probe_counts[1] = ddgi_pass_->GetParams().probe_count_y;
            psIn.probe_counts[2] = ddgi_pass_->GetParams().probe_count_z;
            psIn.probe_count = probeCount;
            psIn.probe_spacing = ddgi_pass_->GetParams().probe_spacing;
            psIn.irradiance_buffer = ddgi_pass_->GetIrradianceBufferBySlot(cbIdx);  // 槽号（诊断可视化）
            // [④ 2026-09-21] moments（confidence/variance 数据源），同槽轮转。
            psIn.ddgi_depth_buffer = ddgi_pass_->GetDepthBufferBySlot(cbIdx);
            {   // [P0 调试] 一次性打印注入句柄
                static bool irrDumped = false;
                if (!irrDumped) {
                    irrDumped = true;
                    PRIMAL_LOG_STREAM(Error, "Render")
                        << "[ProbeSpheres] inject cbIdx=" << cbIdx
                        << " irrHandle=" << psIn.irradiance_buffer
                        << " slot0=" << ddgi_pass_->GetIrradianceBufferBySlot(0)
                        << " slot1=" << ddgi_pass_->GetIrradianceBufferBySlot(1) << std::endl;
                }
            }
            // B1 L3 遮挡：GBuffer 深度导入（唯一名——物理纹理与 Nanite 写入
            // 的同一张，RG 不感知复用；Metal 编码器边界同步处理转移）。
            auto& gpuDrawForDepth = nanite::GPUDrivenDrawPipeline::Get();
            psIn.depth_rg = graph.ImportResource("GBufferDepth_ProbeSpheres",
                                                 gpuDrawForDepth.GetGBufferDepthSampleable());
            psIn.depth_width = render_width_;
            psIn.depth_height = render_height_;
            // 2026-09-17 审计 §8.3：relocated origins CPU 读回（60 帧后到达，
            // 版本变化触发 VB 重建）→ 球体按 绿/黄/红/灰 状态着色。
            u32 originStateCount = 0, originStateVersion = 0;
            if (const math::v4* originStates =
                    ddgi_pass_->GetRelocatedOriginsCpu(originStateCount, originStateVersion)) {
                psIn.origin_states = originStates;
                psIn.origin_states_count = originStateCount;
                psIn.origin_states_version = originStateVersion;
            }
            probe_sphere_module_->AddPass(graph, psIn);
        }
        // NR transformer 段已移入 SPGI 块作用域内（spgiOut/spDepth/spNormal
        // 为块局部句柄，见上方 Step 9 收尾）。
    }

    // [WRC] WRC 探针球 overlay（PassDebugView WRC Probes 开关）：与 DDGI
    // variant 同构——球体复合在最终 LDR 之上、GBuffer 深度遮挡；数据源换
    // wrc_volumes_[0]（规则格 + 单缓冲 probe data，槽位无关）。
    if (wrc_probe_debug_ && probe_sphere_module_ && probe_sphere_module_->IsInitialized() &&
        probe_sphere_module_->HasWrcPipeline() &&
        wrc_volumes_[0] && wrc_volumes_[0]->IsInitialized()) {
        const auto& wrcParams = wrc_volumes_[0]->GetParams();
        const u32 wrcProbeCount = wrc_volumes_[0]->GetProbeCountTotal();
        if (wrcProbeCount > 0) {
            DebugProbeSphereInputs wpIn;
            wpIn.backbuffer_rg = graph.ImportResource("BackBuffer_WRCProbeSphere", backBuffer);
            wpIn.blit_output_rg = finalOutputNode;
            wpIn.current_buffer_index = cbIdx;
            wpIn.render_width = target_width_ > 0 ? target_width_ : render_width_;
            wpIn.render_height = target_height_ > 0 ? target_height_ : render_height_;
            wpIn.view_matrix = view_matrix_;
            wpIn.proj_matrix = proj_matrix_;
            wpIn.camera_position = {camera_position_.x, camera_position_.y,
                                    camera_position_.z, 0.0f};
            math::v3 toLightW = -Normalize(settings_.lighting.light_direction);
            wpIn.sun_dir = {toLightW.x, toLightW.y, toLightW.z, 0.0f};
            const math::v3 wrcOrigin = wrc_volumes_[0]->GetVolumeOrigin();
            wpIn.probe_origin = wrcOrigin;
            wpIn.probe_counts[0] = wrcParams.probe_count_x;
            wpIn.probe_counts[1] = wrcParams.probe_count_y;
            wpIn.probe_counts[2] = wrcParams.probe_count_z;
            wpIn.probe_count = wrcProbeCount;
            wpIn.probe_spacing = wrcParams.probe_spacing;
            wpIn.wrc_data_buffer = wrc_volumes_[0]->GetProbeDataBuffer();
            auto& gpuDrawForWrcDepth = nanite::GPUDrivenDrawPipeline::Get();
            wpIn.depth_rg = graph.ImportResource("GBufferDepth_WRCProbeSpheres",
                                                 gpuDrawForWrcDepth.GetGBufferDepthSampleable());
            wpIn.depth_width = render_width_;
            wpIn.depth_height = render_height_;
            probe_sphere_module_->AddPass(graph, wpIn);
        }
    }
}

// ============================================================================
// RenderWithCommandBuffer (uses caller's pre-allocated CB — matches working pattern)
// ============================================================================


// ============================================================================
// T8.13 P0: GPU per-pass profiler (RG_PROF=1). The RenderGraph already records
// per-pass GPU timestamps (RenderPass desc bracketing for graphics passes,
// WriteTimestamp for compute) and resolves them at the next Execute entry;
// this only aggregates passExecutionTimes_ over a window and prints the
// Top-N by average GPU ms.
// ============================================================================
static void ReportGraphPassTimes(RenderGraph& graph) {
    static bool enabled = std::getenv("RG_PROF") != nullptr;
    static bool checked = false;
    if (!checked) { checked = true; enabled = enabled || false; }
    if (!enabled) return;

    static std::unordered_map<std::string, double> sumMs;
    static std::unordered_map<std::string, u32> cnt;
    // sun-audit P3: P50/P95 帧时统计——每帧各 pass 样本与帧总 GPU ms
    //（120 帧窗口，报告后清空）。
    static std::unordered_map<std::string, std::vector<double>> valsMs;
    static std::vector<double> frameTotals;
    static u32 frames = 0;

    double thisFrameTotal = 0.0;
    for (const auto& [name, ms] : graph.GetPassExecutionTimes()) {
        sumMs[name] += ms;
        cnt[name] += 1;
        valsMs[name].push_back(ms);
        thisFrameTotal += ms;
    }
    frameTotals.push_back(thisFrameTotal);
    if (++frames < 120) return;

    auto percentile = [](std::vector<double> v, double p) -> double {
        if (v.empty()) return 0.0;
        std::sort(v.begin(), v.end());
        const size_t idx = std::min((size_t)(p * (v.size() - 1)), v.size() - 1);
        return v[idx];
    };

    const double totalP50 = percentile(frameTotals, 0.50);
    const double totalP95 = percentile(frameTotals, 0.95);
    double totalMean = 0.0;
    for (double t : frameTotals) totalMean += t;
    totalMean /= frameTotals.size();

    std::vector<std::pair<std::string, double>> rows;
    double total = 0.0;
    for (const auto& [n, s] : sumMs) {
        rows.push_back({n, s / frames});
        total += s / frames;
    }
    std::sort(rows.begin(), rows.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    PRIMAL_LOG_STREAM(Error, "Render") << "[RG-Perf] === GPU ms/frame over last " << frames << " frames ===" << std::endl;
    // sun-audit P3: 帧总 GPU 时间 P50/P95（切镜尖峰在此窗口内可见）
    PRIMAL_LOG_STREAM(Error, "Render") << "[RG-Perf] FRAME_TOTAL p50=" << totalP50
                                       << " p95=" << totalP95
                                       << " mean=" << totalMean << " ms" << std::endl;
    u32 shown = 0;
    for (const auto& [n, avg] : rows) {
        if (shown++ >= 24) break;
        const double p95 = percentile(valsMs[n], 0.95);
        PRIMAL_LOG_STREAM(Error, "Render") << "  " << avg << "  p95=" << p95 << "  " << n << std::endl;
    }
    PRIMAL_LOG_STREAM(Error, "Render") << "[RG-Perf] TOTAL(sum of passes) = " << total << " ms/frame" << std::endl;

    sumMs.clear();
    cnt.clear();
    valsMs.clear();
    frameTotals.clear();
    frames = 0;
}

void StandardRenderPipeline::RenderWithCommandBuffer(
    RenderScene& scene, RenderView& view,
    ResourceHandle target, const TextureDesc& targetDesc,
    RHICommandBuffer* cmd, u32 bufferIndex,
    CommandBufferHandle cmdHandle,
    SyncHandle signalFence)
{
    // Phase 5b: 本 twin 路径的 RG 在函数内多次 Clear/重建，规范身份的导入
    // 时机与图生命周期耦合——保持下方消费点的 IsValid() 回退（= 旧 per-site
    // 导入），待本路径图生命周期梳理后再接入 producer identity。
    RGResourceHandle gbufferDepthRG{}, gbufferAlbedoRG{}, gbufferNormalRG{};
    // A13/T0.11: on a viewport size change, drop all screen-space history
    // before this frame records — TAA re-inits from the current frame instead
    // of blending a different-size history (black/mixed-frame regression),
    // SPGI drops its probe temporal accumulator.
    if (viewport_size_changed_) {
        viewport_size_changed_ = false;
        PostProcess::ResetTAAHistory();
        if (screen_probe_pass_ && screen_probe_pass_->IsInitialized()) {
            screen_probe_pass_->RequestTemporalReset();
        }
        PRIMAL_LOG_STREAM(Info, "Render")
            << "[Pipeline] Viewport resized to " << render_width_ << "x" << render_height_
            << " — screen-space history reset." << std::endl;
    }

    // T4.1: governor tick — scale the GI budgets for this frame (FPS ≥ 30 floor).
    gi_governor_.RecordFrame();
    if (ddgi_pass_ && ddgi_pass_->IsInitialized()) {
        ddgi_pass_->SetMaxProbesPerFrame(
            GIAdaptiveGovernor::ScaleBudget(gi_base_probe_budget_, gi_governor_.Scale()));
    }
    if (surface_cache_pass_ && surface_cache_pass_->IsInitialized()) {
        surface_cache_pass_->SetPageBudget(
            GIAdaptiveGovernor::ScaleBudget(gi_base_page_budget_, gi_governor_.Scale()));
    }

    // ========================================================================
    // GIHistoryValidity 最小版（GI 能量所有权修复 Phase 5a）：灯光修改事件
    // 消费。DDGI：BoostProbePriority + 30 帧 alpha=0.35 抬升窗；SC：tile
    // validity 位图清零（下一帧 AddPass 生效）+ 本帧页预算拉满（0=全量，
    // 下一帧 governor 恢复缩放）；SPGI：temporal reset。
    // ========================================================================
    if (gi_history_dirty_) {
        gi_history_dirty_ = false;
        const u32 now = static_cast<u32>(frameCount_);
        if (ddgi_pass_ && ddgi_pass_->IsInitialized()) {
            ddgi_pass_->NotifyLightingChanged(now);
        }
        if (surface_cache_pass_ && surface_cache_pass_->IsInitialized()) {
            surface_cache_pass_->RequestTileValidityReset();
            surface_cache_pass_->SetPageBudget(0u);   // 一帧页预算拉满
        }
        if (screen_probe_pass_ && screen_probe_pass_->IsInitialized()) {
            screen_probe_pass_->RequestTemporalReset();
        }
        PRIMAL_LOG_STREAM(Info, "Render")
            << "[GIHistory] lighting changed — DDGI boost+alpha-hold, SC tile"
               " reset+full page budget, SPGI temporal reset at frame " << now
            << std::endl;
    }
    OnSceneChanged(scene, current_scene_ == nullptr);
    current_scene_ = &scene;
    frame_view_ = &view;  // consumed by the optional GeometryDebug overlay
    auto startTime = std::chrono::high_resolution_clock::now();
    if (!device_ || !cmd || !renderGraph_) return;

    ApplyConfigChanges();

    u32 cbIdx = bufferIndex;

    if (targetDesc.size.x > 0 && targetDesc.size.y > 0) {
        target_width_ = targetDesc.size.x;
        target_height_ = targetDesc.size.y;
    }

    UpdatePerFrame(scene, view);

    // RG 路径（Nanite 快照）的实例来源：RegisterMeshEntity 注册的 static
    // entities → RenderScene proxies。编辑器前向分支在下方自行同步；非前向
    // 分支（deferred/pass view/截屏）必须在此同步，否则快照 0 实例、
    // GBuffer 系 pass 视图全空（2026-09-20 pass view 排查）。
    if (!editor_mode_ && (!static_entity_ids_.empty() || !pcg_entity_ids_.empty())) {
        SyncEntitiesToRenderScene(scene);
        forward_renderer_->SetRenderScene(&scene);
    }

    // --- Editor mode: lightweight forward rendering ---
    if (editor_mode_ && forward_renderer_) {
        // Sync entities to RenderScene
        if (!static_entity_ids_.empty() || !pcg_entity_ids_.empty()) {
            SyncEntitiesToRenderScene(scene);
            forward_renderer_->SetRenderScene(&scene);
        }
        // Sync geometry entities for line overlay
        if (!geometry_entity_ids_.empty()) {
            forward_renderer_->SetGeometryEntities(geometry_entity_ids_);
        }
        forward_renderer_->Render(cmd, view_matrix_, proj_matrix_, camera_position_,
                                   target, cbIdx % 3, targetDesc.format);

        // PCG SDF: dispatch voxelization via data_provider_ when set (Strategy
        // pattern). This mirrors the path in Render() at line ~1644 but for
        // the RenderWithCommandBuffer entrypoint used by TestPCGScatter.
        auto& gsdf = nanite::GlobalSDF::Get();
        if (pcg_sdf_readback_initialized_ && gsdf.IsInitialized() && gsdf.IsVoxelizationReady()) {
            gsdf.DispatchVoxelization(cmd, 0);
        }

        frameCount_++;
        // Frame boundary: drain deferred-destroy queue from GPUMesher.
        pcg::GPUMesher::Get().DrainDeferredDestroys();
        scene.ClearTombstonedStreamingMeshes();
        return;
    }

    auto& gpuDraw = nanite::GPUDrivenDrawPipeline::Get();

    // Culling
    if (culling_pipeline_ && scene_snapshot_ && gpuDraw.IsInitialized()) {
        culling_pipeline_->Execute(cmd, *scene_snapshot_, view_matrix_, proj_matrix_,
                                    streaming_manager_, cbIdx);
    }

    // GPU Draw (GBuffer)
    if (gpuDraw.IsInitialized() && scene_snapshot_) {
        auto cullingResults = culling_pipeline_
            ? culling_pipeline_->GetResults()
            : nanite::CullingResults{};
        gpuDraw.Execute(cmd, *scene_snapshot_, view_matrix_, proj_matrix_,
                        cullingResults, static_cast<u32>(frameCount_), cbIdx);
        // RenderWithCommandBuffer bypasses BuildRenderGraph's NaniteSceneRender
        // callback.  Refresh the opt-in child at the same point here, after
        // GPUDrivenDraw has created/rebuilt its real borrowed buffers and before
        // the DDGI AddPass calls below record their descriptors.
        if (!RefreshDDGIFirstNativeNormalSources())
            std::abort();
        if (!RefreshDDGIAnalyticFirstRhoSources())
            std::abort();
    }

    // GlobalSDF voxelization — fill SDF cascades from Nanite mesh data so
    // DDGI can do SDF ray tracing. Must run AFTER gpuDraw.Execute (mesh
    // buffers populated) and BEFORE DDGI trace.
    {
        auto& globalSDF = nanite::GlobalSDF::Get();
        if (globalSDF.IsInitialized() && globalSDF.IsVoxelizationReady()) {
            nanite::SDFVoxelizationResources voxResources;
            voxResources.vertex_buffer = gpuDraw.GetGlobalVertexBuffer();
            voxResources.meshlet_buffer = gpuDraw.GetGlobalMeshletBuffer();
            voxResources.meshlet_vertices_buffer = gpuDraw.GetGlobalMeshletVerticesBuffer();
            voxResources.meshlet_triangles_buffer = gpuDraw.GetGlobalMeshletTrianglesBuffer();
            voxResources.cluster_map_buffer = gpuDraw.GetClusterMapBuffer();
            voxResources.instance_data_buffer = gpuDraw.GetGlobalInstanceDataBuffer();
            voxResources.num_instances = scene_snapshot_ ? scene_snapshot_->GetInstanceCount() : 0;
            globalSDF.SetVoxelizationResources(voxResources);

            for (u32 c = 0; c < globalSDF.GetConfig().cascade_count; ++c) {
                if (globalSDF.CascadeNeedsVoxelization(c)) {
                    globalSDF.DispatchVoxelization(cmd, c);
                }
            }
        }
    }

    // T4.6.5 part 39: Stage2/Stage3 mutual exclusion. In VisibilityBufferOnly
    // mode, Stage3 is skipped (gpuDraw.Execute gates internally). Downstream
    // passes that read GBuffer RTs must also be skipped — their input handles
    // are INVALID_RESOURCE which would cause validation errors.
    const bool wants_gbuffer = (gpuDraw.GetRenderMode() != nanite::GPURenderMode::VisibilityBufferOnly);

    // T4.6.5 part 35.5: Build HZB from this frame's GBuffer depth. The
    // non-Editor path was missing this — Culling samples hzb_texture_ in
    // Stage5 occlusion, but without BuildHZB the texture contained only
    // the initialization-time allocation (uninitialized/stale data).
    // Result: false-occlusion of mid-frustum geometry (pillars, cloth) that
    // varied frame-to-frame because the stale HZB never matched the live
    // scene. BuildHZB writes nearest-depth mip chain via MIN filter so
    // next frame's Stage5 has correct occluder depths.
    if (hzb_system_ && hzb_system_->IsReady() && gpuDraw.IsInitialized()) {
        hzb_system_->BuildHZB(gpuDraw.GetGBufferDepthSampleable(), cmd, cbIdx % 3);
    }

    // ========================================================================
    // SDF Visualization early-out: when toggled on, skip the entire
    // GBuffer/Deferred/Fusion pipeline and render ONLY the fullscreen SDF
    // ray-march pass. This avoids resource-state conflicts with passes whose
    // outputs would otherwise have no downstream consumer.
    // ========================================================================
    if (sdf_visualization_enabled_ && sdf_viz_module_ && sdf_viz_module_->IsInitialized()) {
        renderGraph_->Clear();
        auto& graph = *renderGraph_;

        SDFVisualizationInputs vizIn;
        vizIn.backbuffer_rg = graph.ImportResource("BackBuffer", target);
        vizIn.current_buffer_index = cbIdx;
        vizIn.render_width = target_width_ > 0 ? target_width_ : render_width_;
        vizIn.render_height = target_height_ > 0 ? target_height_ : render_height_;
        vizIn.camera_position = { camera_position_.x, camera_position_.y,
                                  camera_position_.z, 0.0f };
        vizIn.view_matrix = view_matrix_;
        vizIn.proj_matrix = proj_matrix_;
        vizIn.light_direction = { settings_.lighting.light_direction.x,
                                  settings_.lighting.light_direction.y,
                                  settings_.lighting.light_direction.z, 0.0f };
        vizIn.light_intensity = 3.0f;
        // Use offline SDF if available; otherwise fall back to GlobalSDF cascades.
        if (has_offline_sdf_) {
            vizIn.use_offline_sdf = true;
            vizIn.offline_sdf_texture = offline_sdf_texture_;
            vizIn.offline_sdf_origin = offline_sdf_origin_;
            vizIn.offline_sdf_extent = offline_sdf_extent_;
            vizIn.offline_sdf_resolution = offline_sdf_resolution_;
        }
        sdf_viz_module_->AddPass(graph, vizIn);

        graph.Compile();
        graph.Execute(cmd);
        ReportGraphPassTimes(graph);
        return;
    }

    // ========================================================================
    // Neural debug early-out (PRIMAL_NEURAL_DEBUG=1): fullscreen hash-encode +
    // MLP inference view, same early-out shape as the SDF visualization above.
    // ========================================================================
    if (neural_debug_enabled_ && neural_debug_module_ &&
        neural_debug_module_->IsInitialized()) {
        renderGraph_->Clear();
        auto& graph = *renderGraph_;

        NeuralDebugInputs ndIn;
        ndIn.backbuffer_rg = graph.ImportResource("BackBuffer", target);
        ndIn.current_buffer_index = cbIdx;
        ndIn.render_width = target_width_ > 0 ? target_width_ : render_width_;
        ndIn.render_height = target_height_ > 0 ? target_height_ : render_height_;
        neural_debug_module_->AddPass(graph, ndIn);

        graph.Compile();
        graph.Execute(cmd);
        ReportGraphPassTimes(graph);
        return;
    }

    // After draw: Shadow + Deferred + FinalBlit via RenderGraph.
    // T4.6.5 part 39: GBuffer path requires Stage3 to have populated the
    // GBuffer RTs. In VisibilityBufferOnly mode, skip the entire block and
    // fall through to the resolve-output blit instead.
    // [PassDebugView/RG] blitBackbufferRG 跨 if/else 两分支共享（探针球 overlay
    // 与 pass view 都依赖），声明提升到分支选择之前。
    RGResourceHandle blitBackbufferRG{};
    if (wants_gbuffer && deferred_module_ && final_blit_module_) {
        renderGraph_->Clear();
        auto& graph = *renderGraph_;

        // Import GBuffer under unique names — same pattern as BuildRenderGraph
        auto gbufferAlbedoDL = graph.ImportResource("GBufferAlbedo_DL", gpuDraw.GetGBufferAlbedo());
        auto gbufferNormalDL = graph.ImportResource("GBufferNormal_DL", gpuDraw.GetGBufferNormal());
        auto gbufferORMDL = graph.ImportResource("GBufferORM_DL", gpuDraw.GetGBufferORM());
        auto gbufferDepthDL = graph.ImportResource("GBufferDepth_DL", gpuDraw.GetGBufferDepthSampleable());

        // Unified light direction (shines in this direction)
        math::v3 lightDir = Normalize(settings_.lighting.light_direction);

        // Shadow Map
        ShadowMapOutputs shadowOut;
        if (shadow_module_) {
            ShadowMapInputs shadowIn;
            shadowIn.gpu_draw_pipeline = &gpuDraw;
            shadowIn.scene_snapshot = scene_snapshot_.get();
            shadowIn.camera_position = camera_position_;
            shadowIn.view_matrix = view_matrix_;
            shadowIn.proj_matrix = proj_matrix_;
            shadowIn.light_direction = lightDir;
            shadowIn.sun_angular_diameter_deg = settings_.lighting.sun_angular_diameter_deg;
            shadowIn.current_buffer_index = cbIdx;
            shadowIn.shadow_quality = settings_.quality.shadow_quality;
        // T8.13: shadow softness knobs (Hard=0 / PCF=1 / PCSS=2).
        if (const char* e = std::getenv("SHADOW_QUALITY")) {
            shadowIn.shadow_quality = (lumen::ShadowQuality)atoi(e);
        }
            shadowOut = shadow_module_->AddPasses(graph, shadowIn);
        }
        // P0: shadow 段之后输出——ShadowMapOutputs 的尺寸 provenance 已填充。
        LogSunContractOnce(settings_, view_matrix_, proj_matrix_,
                           GatherSunContractSizes(settings_, render_width_, render_height_,
                                                  &shadowOut, surface_cache_pass_.get(),
                                                  static_cast<u32>(frameCount_),
                                                  ddgi_pass_.get()),
                           static_cast<u32>(frameCount_));

        // Deferred Lighting — lightPos = direction TO light = -lightDir
        DeferredLightingInputs deferredIn;
        deferredIn.gpu_draw_pipeline = &gpuDraw;
        deferredIn.ssao_pass = ssao_pass_.get();
        deferredIn.view_matrix = view_matrix_;
        deferredIn.proj_matrix = proj_matrix_;
        deferredIn.camera_position = camera_position_;
        deferredIn.light_pos = math::v4{-lightDir.x, -lightDir.y, -lightDir.z, 0.0f};
        deferredIn.light_color = settings_.lighting.light_color;
        deferredIn.cascade_splits = settings_.lighting.cascade_splits;
        deferredIn.shadow_matrix0 = shadowOut.shadow_matrix[0];
        deferredIn.shadow_matrix1 = shadowOut.shadow_matrix[1];
        deferredIn.shadow_matrix2 = shadowOut.shadow_matrix[2];   // P4.2
        deferredIn.shadow_matrix3 = shadowOut.shadow_matrix[3];   // P4.2
        deferredIn.current_buffer_index = cbIdx;
        deferredIn.shadow_visibility_rg = shadowOut.shadow_visibility_rg;
        deferredIn.shadow_visibility_tex = shadowOut.shadow_visibility_tex;
        // VSM: blurred moments + mode flag (shader falls back to the R8
        // visibility path when vsm_enabled is false).
        deferredIn.vsm_enabled = shadowOut.vsm_enabled;
        deferredIn.shadow_moments_rg[0] = shadowOut.shadow_moments_rg[0];
        deferredIn.shadow_moments_rg[1] = shadowOut.shadow_moments_rg[1];
        deferredIn.shadow_moments_tex[0] = shadowOut.shadow_moments_tex[0];
        deferredIn.shadow_moments_tex[1] = shadowOut.shadow_moments_tex[1];
        deferredIn.shadow_moments_tex[2] = shadowOut.shadow_moments_tex[2];   // P4.2
        deferredIn.shadow_moments_tex[3] = shadowOut.shadow_moments_tex[3];   // P4.2
        deferredIn.shadow_translucent_rg = shadowOut.shadow_translucent_rg;   // P4.1
        deferredIn.shadow_translucent_tex = shadowOut.shadow_translucent_tex; // P4.1
        deferredIn.gbuffer_albedo_rg = gbufferAlbedoDL;
        deferredIn.gbuffer_normal_rg = gbufferNormalDL;
        deferredIn.gbuffer_orm_rg = gbufferORMDL;
        deferredIn.gbuffer_depth_rg = gbufferDepthDL;
        // sun-audit P2 第二步: 探针深度可见性输入（PRIMAL_PROBE_OCC 变体）
        if (ddgi_pass_ && ddgi_pass_->IsInitialized()) {
            deferredIn.probe_depth_buf = ddgi_pass_->GetDepthBufferBySlot(cbIdx);  // 槽号（swapchain 环），诊断变体
            const auto& vol = ddgi_pass_->GetVolumeData();
            deferredIn.probe_origin_spacing = math::v4{
                vol.ProbeOrigin.x, vol.ProbeOrigin.y, vol.ProbeOrigin.z, vol.ProbeSpacing};
            deferredIn.probe_counts = math::v4{
                float(vol.ProbeCounts[0]), float(vol.ProbeCounts[1]),
                float(vol.ProbeCounts[2]),
                float(vol.ProbeCounts[0] * vol.ProbeCounts[1] * vol.ProbeCounts[2])};
        }
        auto deferredOut = deferred_module_->AddPasses(graph, deferredIn);

        // P1-B（sponza-gap-audit §4.2）: 世界空间定点 HDR 探针（RWCB 路径，
        // P3 修订：接入点改挂 TAA 解析后的最终 HDR（hdrCleanRWCB），
        // 逐级能量链审计以 GI_ALL_OFF / GI_ISOLATE 消融驱动）。
        // 相机锁定 ⇒ 固定屏幕点即固定世界点。PRIMAL_GI_PROBE=1 时在
        // frame 120/360/1202 用一次性命令缓冲把融合后 HDR 输出的三个探针点
        // （audit measurements ROI 中心 ×2 render scale）读回并打 log
        // （线性 HDR，非显示编码）。SC_DUMP_CARDS 同款 one-shot 模式。
        if (gi_probe_enabled_) {
            const u32 probeFrame = static_cast<u32>(frameCount_);
            if (probeFrame == 122 || probeFrame == 362 || probeFrame == 1202) {
                // P3 修订: 源 = 上一帧 TAA 解析后的最终 HDR（锁定相机下等价）。
                ResourceHandle physTex = probe_taa_output_phys_;

                rhi::RHIDeviceBase* dev = &graph.GetDevice();
                auto ch = dev->CreateCommandBuffer(rhi::CommandQueueType::Graphics);
                auto* c2 = rhi::GetCommandBuffer(ch);
                if (c2) {
                        BufferDesc pb{};
                        pb.size = 64;
                        pb.type = BufferType::Raw;
                        pb.bindFlags = (u32)rhi::BufferUsageFlags::TransferDst;
                        pb.memoryUsage = GPUMemoryUsage::Readback;
                        ResourceHandle stage = dev->CreateBuffer(pb);
                        if (stage == handles::INVALID_RESOURCE) { dev->DestroyCommandBuffer(ch); }
                        else {
                            c2->Begin();
                            rhi::BufferTextureCopyRegion regions[3];
                            // 点位标定（display 720p → render ×2）：
                            //  arch_sunlit  (150,20)  显示亮度 ~166（日照拱顶亮带）
                            //  floor_sunlit (760,655) 显示亮度 ~239（日照地面亮带）
                            //  right_dark   (982,297) 显示亮度 ~56（暗拱间接光区）
                            const u32 px[3][2] = { {300, 40}, {1520, 1310}, {1964, 594} };
                            for (u32 i = 0; i < 3; ++i) {
                                regions[i].bufferOffset = i * 16;
                                regions[i].imageSubresource.mipLevel = 0;
                                regions[i].imageSubresource.layerCount = 1;
                                regions[i].imageOffset = {static_cast<s32>(px[i][0]), static_cast<s32>(px[i][1]), 0};
                                regions[i].imageExtent = {1, 1, 1};
                            }
                            c2->CopyTextureToBuffer(physTex, stage, regions, 3);
                            c2->End(); c2->Submit(); c2->WaitForCompletion();
                            if (void* p = dev->MapBuffer(stage, 0, 64)) {
                                const u16* v = static_cast<const u16*>(p);
                                auto f16 = [](u16 h) {
                                    u32 sign = (h & 0x8000u) << 16, expo = (h & 0x7C00u) >> 10, man = h & 0x03FFu;
                                    u32 bits;
                                    if (expo == 0) bits = sign;
                                    else if (expo == 31) bits = sign | 0x7F800000u | (man << 13);
                                    else bits = sign | ((expo - 15u + 127u) << 23) | (man << 13);
                                    float f; std::memcpy(&f, &bits, 4); return f;
                                };
                                PRIMAL_LOG_STREAM(Info, "Render")
                                    << "[GI-PROBE] frame=" << probeFrame
                                    << " archHDR=(" << f16(v[0]) << "," << f16(v[1]) << "," << f16(v[2]) << ")"
                                    << " rightArchHDR=(" << f16(v[4]) << "," << f16(v[5]) << "," << f16(v[6]) << ")"
                                    << " floorHDR=(" << f16(v[8]) << "," << f16(v[9]) << "," << f16(v[10]) << ")"
                                    << std::endl;
                                dev->UnmapBuffer(stage);
                            }
                            dev->DestroyBuffer(stage);
                        }
                    dev->DestroyCommandBuffer(ch);
                }
            }
        }

        // --- SSAO ---
        lumen::LumenSSAOOutput ssaoOut{};
        if (ssao_pass_ && ssao_pass_->IsInitialized()) {
            auto gbufferNormalSSAO = graph.ImportResource("GBufferNormal_SSAO", gpuDraw.GetGBufferNormal());
            auto gbufferDepthSSAO = graph.ImportResource("GBufferDepth_SSAO", gpuDraw.GetGBufferDepthSampleable());
            lumen::SSAOCameraData camData{};
            camData.view_matrix = view_matrix_;
            camData.proj_matrix = proj_matrix_;
            camData.prev_view_matrix = prev_view_matrix_;   // A10: true previous-frame VP
            camData.prev_proj_matrix = prev_proj_matrix_;
            camData.frame_index = static_cast<u32>(frameCount_);
            camData.delta_time = settings_.advanced.delta_time_override;
            ssaoOut = ssao_pass_->AddPass(graph, gbufferNormalSSAO, gbufferDepthSSAO, camData, static_cast<u32>(frameCount_));
        }

        // --- Surface Cache ---
        RGResourceHandle scLightingRG;
        RGResourceHandle scNodirectRG;   // [E0-B]
        u32 giFrameT = GIFrame(static_cast<u32>(frameCount_));
        if (surface_cache_pass_ && surface_cache_pass_->IsInitialized()) {
            lumen::SurfaceCacheFrameData frameData{};
            frameData.camera_position = {camera_position_.x, camera_position_.y, camera_position_.z};
            frameData.frame_index = static_cast<u32>(frameCount_);
            // sun-audit §7 P1: 与 BuildRenderGraph 站点统一——旧代码在这里把
            // 备用 SC 阳光写成 ddgi_light_color（比主路径 light_color 高
            // 1.5×），是两条 RenderGraph 站点漂移的实锤。现在两个站点都走
            // FillSurfaceCacheSunLight。
            FillSurfaceCacheSunLight(frameData, settings_, current_scene_);
            frameData.inv_view_projection = Inverse(proj_matrix_ * view_matrix_);
            frameData.render_width  = render_width_;
            frameData.render_height = render_height_;
            frameData.gbuffer_depth  = gpuDraw.GetGBufferDepthSampleable();
            frameData.gbuffer_albedo = gpuDraw.GetGBufferAlbedo();
            frameData.gbuffer_normal = gpuDraw.GetGBufferNormal();
            // Probe-fed indirect — see the BuildRenderGraph twin site.
            // Phase 1d: 槽位单一事实源 DDGISlotMap.h。
            if (ddgi_pass_ && ddgi_pass_->IsInitialized()) {
                const auto ddgiSlots = lumen::DDGISlotsForFrame(static_cast<u32>(frameCount_));
                const auto& vol = ddgi_pass_->GetVolumeData();
                frameData.ddgi_irradiance = ddgi_pass_->GetIrradianceBufferBySlot(ddgiSlots.previousIndex);
                // C 系列：同槽位探针深度（twin 站点同步）。
                frameData.ddgi_depth = ddgi_pass_->GetDepthBufferBySlot(ddgiSlots.previousIndex);
                // GI 根治 Phase 3.2: 同源 openSky 统计（twin 站点同步）。
                frameData.ddgi_opensky = ddgi_pass_->GetOpenSkyBuffer();
                frameData.ddgi_origin = {vol.ProbeOrigin.x, vol.ProbeOrigin.y, vol.ProbeOrigin.z};
                frameData.ddgi_spacing = vol.ProbeSpacing;
                frameData.ddgi_counts[0] = vol.ProbeCounts[0];
                frameData.ddgi_counts[1] = vol.ProbeCounts[1];
                frameData.ddgi_counts[2] = vol.ProbeCounts[2];
                // C1-H3 hardening — see the BuildRenderGraph twin site.
                frameData.ddgi_valid =
                    (frameData.ddgi_irradiance != handles::INVALID_RESOURCE) ? 1u : 0u;
            }
            // G2 capture 侧（2026-09-16）: 同 site 1——PCSS 深度供卡片
            // 直射可见性
            for (u32 c = 0; c < 4; ++c) {
                frameData.shadow_depth[c] = shadowOut.shadow_depth_rg[c];
                frameData.shadow_matrices[c] = shadowOut.shadow_matrix[c];
            }
            frameData.shadow_cascade_count = 4u;

            ResourceHandle nullLightBuf{handles::INVALID_RESOURCE};
            // T8.12: card-view capture — this path never had the wiring (the
            // BuildRenderGraph twin site has it). With card view enabled the
            // screen-space gather inside AddPass is disabled, so without this
            // the atlas keeps AtlasInit seed values (flat gray albedo / +Z
            // normals) forever.
            if (surface_cache_pass_->IsCardViewEnabled() && scene_snapshot_) {
                surface_cache_pass_->AddCardCapturePasses(graph, *scene_snapshot_,
                                                          static_cast<u32>(frameCount_));
            }
            // [双稳态修复] twin 站点同步闩锁（幂等——先到先锁）。
            TryLatchGIContentReady();
            giFrameT = GIFrame(static_cast<u32>(frameCount_));
            {
                lumen::SurfaceCacheFrameData frameDataGi = frameData;
                frameDataGi.gi_frame = giFrameT;
                const auto scOut = surface_cache_pass_->AddPass(graph, RGResourceHandle{}, nullLightBuf, frameDataGi, static_cast<u32>(frameCount_));
                // T1.1 (C2): producer's RG identity — see BuildRenderGraph twin.
                scLightingRG = scOut.lighting_atlas;
                scNodirectRG = scOut.lighting_nodirect_atlas;   // [E0-B]
            }
            if (ddgi_pass_ && ddgi_pass_->IsInitialized()) {
                // X2/A3: SC→DDGI trace 注入走统一 Cache View
                ddgi_pass_->SetSurfaceCacheResources(
                    surface_cache_pass_->GetCacheView(static_cast<u32>(frameCount_)));
            }
            if (screen_probe_pass_ && screen_probe_pass_->IsInitialized()) {
                // X2/A3: SC 数据走统一 Cache View（atlas_size/cardCount 与旧
                // settings 传值同源，上传逐字节等价）
                const auto scView = surface_cache_pass_->GetCacheView(static_cast<u32>(frameCount_));
                // 2026-09-22 排障：scAvail=NO（Vulkan sponza）一次性打印各字段
                {
                    static bool scDbg = false;
                    if (!scDbg) { scDbg = true;
                        PRIMAL_LOG_STREAM(Error, "Render")
                            << "[SC-DIAG] atlas=" << scView.atlas_size
                            << " lighting=" << (scView.lighting_atlas != rhi::handles::INVALID_RESOURCE)
                            << " card_data=" << (scView.card_data != rhi::handles::INVALID_RESOURCE)
                            << " card_lookup=" << (scView.card_lookup != rhi::handles::INVALID_RESOURCE)
                            << " grid_offsets=" << (scView.grid_offsets != rhi::handles::INVALID_RESOURCE)
                            << " grid_cards=" << (scView.grid_cards != rhi::handles::INVALID_RESOURCE)
                            << " max_cards=" << scView.max_card_count
                            << " card_count=" << scView.card_count << std::endl; }
                }
                screen_probe_pass_->SetSurfaceCacheData(scView);
            // Opt-in validated first-surface owner qualification. SC owner IDs
            // are exactly snapshot order; ambiguous proxy contracts stay unknown.
            std::vector<math::u32v4> zeroOwners;
            const char* firstEnv=std::getenv("PRIMAL_SPGI_ZERO_HIT_GEOMETRY");
            if(scene_snapshot_ && device_->GetPlatform()==rhi::RHIPlatform::Vulkan && firstEnv &&
               (std::strcmp(firstEnv,"1")==0 || std::strcmp(firstEnv,"2")==0)) {
                for(const auto& inst:scene_snapshot_->GetInstanceData()) {
                    bool found=false,supported=true;
                    for(const auto& proxy:GetCurrentScene()->GetProxies()) {
                        const auto* cache=cluster::get(proxy.meshId);
                        if(!cache || !cache->exists || cache->geometry_content_id!=inst.geometry_id || proxy.materialId!=inst.material_id ||
                           std::memcmp(&proxy.transform,&inst.world_matrix,sizeof(inst.world_matrix))!=0) continue;
                        found=true;
                        supported &= proxy.technique==ShaderTechnique::Opaque && proxy.base_color[3]==1.0f && proxy.graphVariant==0;
                    }
                    zeroOwners.push_back(math::u32v4{inst.material_id,found&&supported?1u:0u,u32(zeroOwners.size()),u32(frameCount_)});
                }
            }
            screen_probe_pass_->SetFirstSurfaceInputs(surface_cache_pass_.get(),std::move(zeroOwners),
                nanite::GPUDrivenDrawPipeline::Get().GetMaterialDataBuffer());
            }
        }

        // --- DDGI ---
        lumen::LumenDDGIOutput ddgiOut;
        if (ddgi_pass_ && ddgi_pass_->IsInitialized()) {
            lumen::DDGICameraData camData{};
            camData.camera_position = camera_position_;
            camData.view_matrix = view_matrix_;
            camData.proj_matrix = proj_matrix_;
            camData.prev_view_matrix = prev_view_matrix_;   // A10: true previous-frame VP
            camData.prev_proj_matrix = prev_proj_matrix_;
            FillDDGISunLight(camData, settings_);
            camData.frame_index = static_cast<u32>(frameCount_);
            camData.gi_frame_index = giFrameT;
            camData.delta_time = settings_.advanced.delta_time_override;
            RGResourceHandle prevColor;
            if (deferredOut.deferred_output_tex != handles::INVALID_RESOURCE)
                prevColor = graph.ImportResource("PrevFrameColor", deferredOut.deferred_output_tex);
            // B11/Phase 2.3: 回灌 pre-gain GI 缓冲（FusionIndirect 输出）——
            // 融合增益与直射不再进探针回路（直射供给由 SC 卡片负责）。
            // Phase 2a: 默认断边（twin 站点与主路径同步，见上方注释）。
            if (RGResourceHandle fused = MakeProbeRadianceSource(graph, cbIdx); fused.IsValid()) {
                prevColor = fused;
                camData.radiance_is_previous_frame = true;
            }
            if (device_->GetPlatform() == rhi::RHIPlatform::Vulkan) {
                camData.screen_depth = graph.ImportResource("DDGI_CurrentScreenDepth", gpuDraw.GetGBufferDepthSampleable());
                if (camData.radiance_is_previous_frame) {
                    if (depth_history_ && !camera_cut_this_frame_) {
                        const auto previous = depth_history_->GetPreviousFrameDepth(static_cast<u32>(frameCount_));
                        if (previous.is_valid && previous.texture != handles::INVALID_RESOURCE)
                            camData.radiance_depth = graph.ImportResource("DDGI_RadianceDepth", previous.texture);
                    }
                }
                // 断边默认态：radiance_depth 保持无效 = 屏域 radiance 档关断。
            }
            ddgiOut = ddgi_pass_->AddPass(graph, prevColor, camData, static_cast<u32>(frameCount_), scLightingRG);

            // T5.5: secondary volumes (no SC atlas — near-field data only).
            if (lumen::GIDebug().ddgi_far_volume) {
                for (u32 vi = 1; vi < ddgi_volume_count_; ++vi) {
                    if (ddgi_volumes_[vi] && ddgi_volumes_[vi]->IsInitialized()) {
                        ddgi_volumes_[vi]->AddPass(graph, prevColor, camData,
                                                   static_cast<u32>(frameCount_), RGResourceHandle{});
                    }
                }
            }
        }

        // --- [WRC] twin 站点同步（Step 6.7 的 RWCB 路径镜像，独立接线 block）。
        if (wrc_volume_count_ > 0) {
            for (u32 vi = 0; vi < wrc_volume_count_; ++vi) {
                if (!wrc_volumes_[vi] || !wrc_volumes_[vi]->IsInitialized()) continue;
                if (vi == 0 && surface_cache_pass_) {
                    // SC 首帧完成前强制绑 dummy 2D（同 Step 6.7 门控）。
                    if (frameCount_ >= 1) {
                        wrc_volumes_[vi]->SetSurfaceCacheResources(
                            surface_cache_pass_->GetCacheView(static_cast<u32>(frameCount_)));
                    } else {
                        wrc_volumes_[vi]->ClearSurfaceCacheResources();
                    }
                    auto& cardGen = surface_cache_pass_->GetCardGenerator();
                    wrc_volumes_[vi]->UpdateProbeCardList(
                        cardGen.GetCards().data(), (u32)cardGen.GetCards().size());
                }
                lumen::WRCCameraData wrcCam{};
                wrcCam.light_direction = rhi::math::Normalize(settings_.lighting.light_direction);
                wrcCam.light_color = {settings_.lighting.ddgi_light_color.x,
                                      settings_.lighting.ddgi_light_color.y,
                                      settings_.lighting.ddgi_light_color.z};
                wrcCam.sky_color = {1.0f, 1.0f, 1.0f};   // 去 tier 双重压缩（见 3054 注）
                wrcCam.frame_index = static_cast<u32>(frameCount_);
                const RGResourceHandle scRG = (vi == 0) ? scLightingRG : RGResourceHandle{};
                wrc_volumes_[vi]->AddPass(graph, wrcCam, static_cast<u32>(frameCount_), scRG);
            }
        }

        // --- SC-DDGI Integration ---
        // X3/A6 轴4: propagate_sc_to_ddgi=false 跳过喂食边（同 Step 7 路径）
        if (settings_.lumen.axes.propagate_sc_to_ddgi &&
            sc_ddgi_module_ && surface_cache_pass_ && ddgi_pass_) {
            SCDDGIInputs scDDGIIn;
            scDDGIIn.ddgi_pass = ddgi_pass_.get();
            scDDGIIn.surface_cache_pass = surface_cache_pass_.get();
            scDDGIIn.frame_index = static_cast<u32>(frameCount_);
            scDDGIIn.current_buffer_index = cbIdx;
            scDDGIIn.sc_lighting_rg = scLightingRG;
            scDDGIIn.ddgi_irradiance = ddgiOut.ddgi_irradiance;
            scDDGIIn.ddgi_irradiance_hist = ddgiOut.ddgi_irradiance_hist;
            sc_ddgi_module_->AddPass(graph, scDDGIIn);
        }

        // --- GIGather ---
        GIGatherOutputs giOut;
        if (gi_gather_module_ && ddgi_pass_ && ddgi_pass_->IsInitialized() && lumen::GIDebug().gi_gather) {
            auto gbufferDepthGI = graph.ImportResource("GBufferDepthGI", gpuDraw.GetGBufferDepthSampleable());
            auto gbufferNormalGI = graph.ImportResource("GBufferNormalGI", gpuDraw.GetGBufferNormal());
            auto gbufferOrmGI = graph.ImportResource("GBufferOrmGI", gpuDraw.GetGBufferORM());
            GIGatherInputs giIn;
            giIn.ddgi_pass = ddgi_pass_.get();
            giIn.ddgi_far_pass = (ddgi_volume_count_ > 1) ? ddgi_volumes_[1].get() : nullptr;
            // X2/A3: 远场 volume 走 Cache View 直传（槽位与模块内 ddgiReadIdx 一致）
            giIn.far_volume_view = (ddgi_volume_count_ > 1 && ddgi_volumes_[1])
                ? ddgi_volumes_[1]->GetVolumeView(static_cast<u32>(frameCount_)) : lumen::DDGIVolumeView{};
            // T6.3 sub-B: 全 volume 视图直传（同 Render 路径；能力门控在模块内）。
            giIn.volume_count = ddgi_volume_count_;
            for (u32 vi = 0; vi < ddgi_volume_count_ && vi < GIGatherInputs::kMaxVolumes; ++vi)
                if (ddgi_volumes_[vi]) giIn.volume_views[vi] = ddgi_volumes_[vi]->GetVolumeView(static_cast<u32>(frameCount_));
            giIn.static_probe_volume = static_probe_volume_.get();
            giIn.gbuffer_depth = gpuDraw.GetGBufferDepthSampleable();
            giIn.gbuffer_normal = gpuDraw.GetGBufferNormal();
            giIn.gbuffer_orm = gpuDraw.GetGBufferORM();
            giIn.gbuffer_depth_rg = gbufferDepthGI;
            giIn.gbuffer_normal_rg = gbufferNormalGI;
            giIn.gbuffer_orm_rg = gbufferOrmGI;
            giIn.view_matrix = view_matrix_;
            giIn.proj_matrix = proj_matrix_;
            giIn.current_buffer_index = cbIdx;
            giIn.frame_index = static_cast<u32>(frameCount_);
            giIn.fallback_texture = black_texture_;
            giOut = gi_gather_module_->AddPasses(graph, giIn);
        }

        // --- SSGI (Screen Space GI) ---
        lumen::LumenSSGIOutput ssgiOut;
        // T8.13: SSGI/SPGI are alternative screen-space GI solutions — running
    // both costs 31ms GPU (18.3+12.6, 85% of frame) with overlapping
    // contribution (T8.7 A/B: SSGI ≈ 0 where SPGI exists). Policy: SPGI
    // active ⇒ SSGI passes skipped; SSGI_FORCE=1 keeps both for A/B.
    const bool spgiActive = screen_probe_pass_ && screen_probe_pass_->IsInitialized();
    const bool ssgiSuppressed = spgiActive && !std::getenv("SSGI_FORCE");
    if (ssgi_pass_ && ssgi_pass_->IsInitialized() && !ssgiSuppressed) {
            auto gbufferNormalSSGI = graph.ImportResource("GBufferNormal_SSGI", gpuDraw.GetGBufferNormal());
            auto gbufferDepthSSGI = graph.ImportResource("GBufferDepth_SSGI", gpuDraw.GetGBufferDepthSampleable());
            auto gbufferVelocitySSGI = graph.ImportResource("GBufferVelocity_SSGI", gpuDraw.GetGBufferVelocity());
            auto hzbHandle = graph.ImportResource("HZBTexture_SSGI", hzb_system_->GetHZBTexture());

            RGResourceHandle prevColorSSGI;
            if (deferredOut.deferred_output_tex != handles::INVALID_RESOURCE)
                prevColorSSGI = graph.ImportResource("PrevFrameColor_SSGI", deferredOut.deferred_output_tex);

            lumen::SSGICameraData ssgiCam{};
            ssgiCam.view_matrix = view_matrix_;
            ssgiCam.proj_matrix = proj_matrix_;
            ssgiCam.prev_view_matrix = prev_view_matrix_;   // A10: true previous-frame VP
            ssgiCam.prev_proj_matrix = prev_proj_matrix_;
            ssgiCam.frame_index = static_cast<u32>(frameCount_);
            ssgiCam.delta_time = settings_.advanced.delta_time_override;

            ssgiOut = ssgi_pass_->AddPass(graph, gbufferNormalSSGI, gbufferDepthSSGI,
                gbufferVelocitySSGI, hzbHandle, prevColorSSGI,
                ssgiCam, static_cast<u32>(frameCount_),
                hzb_system_->GetMipLevels());
        }

        // --- SPGI (Screen Probe GI) ---
        lumen::ScreenProbeGIOutput spgiOut;
        if (screen_probe_pass_ && screen_probe_pass_->IsInitialized() && frameCount_ > 0) {
            // T2.2: DDGI probe fallback for miss rays
            if (ddgi_pass_ && ddgi_pass_->IsInitialized()) {
                if (settings_.lumen.axes.fallback_spgi_to_ddgi) {
                    // X2/A3: volume 数据走统一 Cache View（ddgi_pass_ 别名 ddgi_volumes_[0]）。
                    // Phase 1d: previousIndex = 上一完成帧（twin 站点同步）。
                    screen_probe_pass_->SetDDGIFallbackData(
                        ddgi_pass_->GetVolumeViewBySlot(
                            lumen::DDGISlotsForFrame(static_cast<u32>(frameCount_)).previousIndex));
                } else {
                    // X3/A6 轴4: fallback 边关断 → Clear 语义（空 view 置
                    // ddgi_fallback_available_=false，miss 回落天空常量）
                    screen_probe_pass_->SetDDGIFallbackData(lumen::DDGIVolumeView{});
                }
            }
            // [WRC] A3 twin 站点同步（fallback_spgi_to_wrc 边，见 Step 9 注释）。
            {
                bool wrcEdge = settings_.lumen.wrc_spgi_consume &&
                               wrc_volume_count_ > 0 &&
                               wrc_volumes_[0] && wrc_volumes_[0]->IsInitialized();
                if (wrcEdge) {
                    screen_probe_pass_->SetWRCCacheData(
                        wrc_volumes_[0]->GetCacheView(static_cast<u32>(frameCount_)),
                        wrc_volumes_[0]->GetConsumeCounterBuffer(), true);
                } else {
                    screen_probe_pass_->SetWRCCacheData(lumen::WRCCacheView{},
                                                        rhi::handles::INVALID_RESOURCE, false);
                }
            }
            auto spDepth = graph.ImportResource("GBufferDepth_SP", gpuDraw.GetGBufferDepthSampleable());
            auto spNormal = graph.ImportResource("GBufferNormal_SP", gpuDraw.GetGBufferNormal());

            RGResourceHandle spRadiance = deferredOut.deferred_output_rg.IsValid()
                ? deferredOut.deferred_output_rg
                : graph.ImportResource("SPBlackFallback", black_texture_);

            lumen::ScreenProbeCameraData spCam{};
            spCam.view_matrix = view_matrix_;
            spCam.proj_matrix = proj_matrix_;
            spCam.prev_view_matrix = prev_view_matrix_;
            spCam.prev_proj_matrix = prev_proj_matrix_;
            if (camera_cut_this_frame_ && screen_probe_pass_) {
                screen_probe_pass_->RequestTemporalReset();   // A11
            }
            // T1.1 (C2): single RG identity for the SC lighting atlas
            if (screen_probe_pass_ && scLightingRG.IsValid()) {
                screen_probe_pass_->SetExternalLightingRG(scLightingRG);
                screen_probe_pass_->SetExternalNoDirectLightingRG(scNodirectRG);   // [E0-B]
            }
            spCam.camera_position = camera_position_;
            spCam.frame_index = static_cast<u32>(frameCount_);

            // T5.1: HZB near-field input for the hybrid split trace.
            RGResourceHandle spHzb;
            if (hzb_system_) {
                spHzb = graph.ImportResource("HZBTexture_SPGI", hzb_system_->GetHZBTexture());
                screen_probe_pass_->SetHZBData(hzb_system_->GetHZBTexture(),
                                               hzb_system_->GetMipLevels());
            }
            spgiOut = screen_probe_pass_->AddPass(graph, spDepth, spNormal,
                spRadiance, spCam, static_cast<u32>(frameCount_), spHzb);

            // M1 采集（第二渲染路径，同上）。
            if (neural_capture_module_ && neural_capture_module_->IsInitialized()) {
                NeuralCaptureInputs capIn;
                capIn.spgi_gi_output = spgiOut.gi_output;
                capIn.gbuffer_depth = spDepth;
                capIn.gbuffer_normal = spNormal;
                capIn.render_width = target_width_ > 0 ? target_width_ : render_width_;
                capIn.render_height = target_height_ > 0 ? target_height_ : render_height_;
                capIn.current_buffer_index = cbIdx;
                capIn.camera_cut = camera_cut_this_frame_;
                neural_capture_module_->AddPass(graph, capIn);
            }
 
            // M1 神经降噪推理（第二渲染路径，同上）。
            if (neural_denoise_module_ && neural_denoise_module_->IsInitialized()) {
                NeuralDenoiseInputs dnIn;
                dnIn.spgi_gi_output = spgiOut.gi_output;
                dnIn.gbuffer_depth = spDepth;
                dnIn.gbuffer_normal = spNormal;
                dnIn.render_width = target_width_ > 0 ? target_width_ : render_width_;
                dnIn.render_height = target_height_ > 0 ? target_height_ : render_height_;
                dnIn.current_buffer_index = cbIdx;
                neural_denoise_module_->AddPass(graph, dnIn);
            }
            // NR transformer 推理（第二渲染路径，同上）。
            if (neural_nr_module_ && neural_nr_module_->IsInitialized()) {
                NeuralNRInputs nrIn;
                nrIn.spgi_gi_output = spgiOut.gi_output;
                nrIn.gbuffer_depth = spDepth;
                nrIn.gbuffer_normal = spNormal;
                nrIn.render_width = target_width_ > 0 ? target_width_ : render_width_;
                nrIn.render_height = target_height_ > 0 ? target_height_ : render_height_;
                nrIn.current_buffer_index = cbIdx;
                neural_nr_module_->AddPass(graph, nrIn);
            }
        }

        // --- Volume (Froxel Fog or Ray March) ---
        volume::VolumeOutput volumeOut;
        if (froxel_fog_pass_ && froxel_fog_pass_->IsInitialized()) {
            volume::FroxelInputs froxelIn;
            froxelIn.gbuffer_depth = graph.ImportResource("GBufferDepth_Volume", gpuDraw.GetGBufferDepthSampleable());
            froxelIn.scene_color = deferredOut.deferred_output_rg;
            froxelIn.shadow_map = shadowOut.shadow_visibility_rg;
            froxelIn.shadow_matrix0 = shadowOut.shadow_matrix[0];
            froxelIn.shadow_matrix1 = shadowOut.shadow_matrix[1];
            // P2-B（sponza-gap-audit §4.6）: 雾的阴影输入换成 light-space VSM
            // moments + 全 4 级联矩阵。屏幕空间表面可见性图不再被雾采样（屏幕 UV
            // 与 light VP UV 不对应，表面可见性不描述空间点遮挡）。
            // P1（sponza-gap-reaudit §4.4）: 同时传入 PCSS 深度图；无 moments
            // （默认 PCSS）时 FroxelFogPass 自动切换 shadow_mode=2 深度比较，
            // 不再恒 lit。级联数按 4 传入，模式由纹理有效性在 pass 内决定。
            if (froxel_shadows_enabled_) {
                for (u32 c = 0; c < 4; ++c) {
                    froxelIn.shadow_moments[c] = shadowOut.shadow_moments_rg[c];
                    froxelIn.shadow_matrices[c] = shadowOut.shadow_matrix[c];
                    froxelIn.shadow_depth[c] = shadowOut.shadow_depth_rg[c];
                }
                froxelIn.shadow_cascade_count = 4u;
            }

            auto& globalSDF = nanite::GlobalSDF::Get();
            if (globalSDF.IsInitialized()) {
                for (u32 c = 0; c < std::min(3u, globalSDF.GetConfig().cascade_count); ++c) {
                    const auto& cascade = globalSDF.GetCascade(c);
                    if (cascade.sdf_texture != handles::INVALID_RESOURCE) {
                        std::string name = "SDFCascade" + std::to_string(c) + "_Volume";
                        auto sdfRG = graph.ImportResource(name, cascade.sdf_texture);
                        if (c == 0) froxelIn.sdf_cascade_0 = sdfRG;
                        else if (c == 1) froxelIn.sdf_cascade_1 = sdfRG;
                        else if (c == 2) froxelIn.sdf_cascade_2 = sdfRG;
                    }
                }
            }

            volume::VolumeCameraData volCam{};
            volCam.camera_position = camera_position_;
            volCam.view_matrix = view_matrix_;
            volCam.proj_matrix = proj_matrix_;
            math::v3 lightDir = Normalize(settings_.lighting.light_direction);
            volCam.light_direction = lightDir;
            volCam.light_color = {settings_.lighting.light_color.x, settings_.lighting.light_color.y, settings_.lighting.light_color.z};
            volCam.frame_index = static_cast<u32>(frameCount_);
            froxelIn.camera_data = volCam;
            froxelIn.width = render_width_;
            froxelIn.height = render_height_;

            auto froxelOut = froxel_fog_pass_->AddPass(graph, froxelIn);
            volumeOut.volume_scatter = froxelOut.volume_scatter;
            volumeOut.valid = froxelOut.valid;
        } else if (volume_pass_ && volume_pass_->IsInitialized()) {
            volume::VolumeInputs volIn;
            volIn.gbuffer_depth = graph.ImportResource("GBufferDepth_Volume", gpuDraw.GetGBufferDepthSampleable());
            volIn.scene_color = deferredOut.deferred_output_rg;
            volIn.shadow_map = shadowOut.shadow_visibility_rg;

            auto& globalSDF = nanite::GlobalSDF::Get();
            if (globalSDF.IsInitialized()) {
                for (u32 c = 0; c < std::min(3u, globalSDF.GetConfig().cascade_count); ++c) {
                    const auto& cascade = globalSDF.GetCascade(c);
                    if (cascade.sdf_texture != handles::INVALID_RESOURCE) {
                        std::string name = "SDFCascade" + std::to_string(c) + "_Volume";
                        auto sdfRG = graph.ImportResource(name, cascade.sdf_texture);
                        if (c == 0) volIn.sdf_cascade_0 = sdfRG;
                        else if (c == 1) volIn.sdf_cascade_1 = sdfRG;
                        else if (c == 2) volIn.sdf_cascade_2 = sdfRG;
                    }
                }
            }

            volume::VolumeCameraData volCam{};
            volCam.camera_position = camera_position_;
            volCam.view_matrix = view_matrix_;
            volCam.proj_matrix = proj_matrix_;
            math::v3 lightDir = Normalize(settings_.lighting.light_direction);
            volCam.light_direction = lightDir;
            volCam.light_color = {settings_.lighting.light_color.x, settings_.lighting.light_color.y, settings_.lighting.light_color.z};
            volCam.frame_index = static_cast<u32>(frameCount_);
            volIn.camera_data = volCam;
            volIn.width = render_width_;
            volIn.height = render_height_;
            volumeOut = volume_pass_->AddPass(graph, volIn);
        }

        // --- SSR (Screen-Space Reflections) ---
        // Trace → Temporal → Upsample. Output is a pure reflection-color texture
        // (RGBA16F: RGB=reflection, A=strength). FusionComposite blends it in.
        PostProcess::SSRPassData ssrOutRWCB;
        if (settings_.quality.enable_ssr && frameCount_ > 0) {
            RGResourceHandle hdrSSR;
            if (deferredOut.deferred_output_rg.IsValid()) {
                hdrSSR = deferredOut.deferred_output_rg;
            } else if (deferredOut.deferred_output_tex != handles::INVALID_RESOURCE) {
                hdrSSR = graph.ImportResource("SSR_HdrSource", deferredOut.deferred_output_tex);
            }
            auto depthSSR = graph.ImportResource("GBufferDepth_SSR", gpuDraw.GetGBufferDepthSampleable());
            auto hzbSSR   = graph.ImportResource("HZBTexture_SSR", hzb_system_->GetHZBTexture());
            auto velSSR   = graph.ImportResource("GBufferVelocity_SSR", gpuDraw.GetGBufferVelocity());
            auto ormSSR   = graph.ImportResource("GBufferORM_SSR", gpuDraw.GetGBufferORM());

            math::m4x4 invProjRWCB = Inverse(proj_matrix_);
            ssrOutRWCB = PostProcess::AddSSRPass(graph, hdrSSR, depthSSR, hzbSSR, velSSR, ormSSR,
                render_width_, render_height_, static_cast<u32>(frameCount_),
                proj_matrix_, invProjRWCB, settings_.ssr);
        }

        // --- Fluid Render ---
        fluid::FluidOutput fluidOut;
        if (fluid_render_pass_ && fluid_render_pass_->IsInitialized()) {
            fluid::FluidInputs fluidIn;
            fluidIn.view_matrix = view_matrix_;
            fluidIn.proj_matrix = proj_matrix_;
            fluidIn.camera_position = camera_position_;
            fluidIn.scene_color = deferredOut.deferred_output_rg;
            fluidIn.gbuffer_depth = graph.ImportResource("GBufferDepth_Fluid", gpuDraw.GetGBufferDepthSampleable());
            fluidIn.width = render_width_;
            fluidIn.height = render_height_;
            fluidOut = fluid_render_pass_->AddPass(graph, fluidIn);
        }

        // --- Volume Object Rendering (forward, into scatter texture) ---
        // Must run BEFORE graph.Execute() so scatter texture can be imported into FusionComposite.
        rendergraph::RGResourceHandle volume_scatter_rg;
        if (volume_renderer_ && volume_renderer_->IsInitialized()) {
            volume::VolumeCameraData volCam{};
            volCam.camera_position = camera_position_;
            volCam.view_matrix = view_matrix_;
            volCam.proj_matrix = proj_matrix_;
            math::v3 lightDir = Normalize(settings_.lighting.light_direction);
            volCam.light_direction = lightDir;
            volCam.light_color = {settings_.lighting.light_color.x, settings_.lighting.light_color.y, settings_.lighting.light_color.z};
            volCam.frame_index = static_cast<u32>(frameCount_);

            volume_renderer_->Render(cmd,
                volCam,
                static_cast<u32>(frameCount_),
                render_width_,
                render_height_);

            volume_scatter_rg = graph.ImportResource("VolumeScatter",
                volume_renderer_->GetScatterTexture(static_cast<u32>(frameCount_)));
        }

        // --- FusionComposite ---
        FusionOutputs fusionOut;
        if (fusion_module_ && (settings_.quality.enable_ssgi || settings_.quality.enable_ssr ||
                               settings_.quality.enable_screen_probes) && lumen::GIDebug().fusion) {
            FusionInputs fusionIn;
            fusionIn.primary_input_rg = deferredOut.deferred_output_rg;
            fusionIn.primary_input_tex = deferredOut.deferred_output_tex;
            // TEMP diagnosis: SSGI_SILENCE=1 mutes SSGI into fusion (black)
            // while keeping the fusion path itself alive — isolates the SSGI
            // prevColor feedback loop without disabling fusion.
            // 伪影1（楔形加性亮斑）二分：SSAO_SILENCE / DDGI_SILENCE /
            // SPGI_SILENCE 逐项静音（与 SSGI_SILENCE 同模式）。
            fusionIn.ssgi_rg = std::getenv("SSGI_SILENCE")
                ? rendergraph::RGResourceHandle{} : ssgiOut.ssgi_output;
            fusionIn.ssgi_tex = handles::INVALID_RESOURCE;
            fusionIn.ddgi_rg = std::getenv("DDGI_SILENCE")
                ? rendergraph::RGResourceHandle{} : giOut.gi_output_rg;
            // 注意：ddgi_tex 承载真实物理句柄（与 ssgi 不同，DDGI 的 tex/rg
            // 双路径都是活的）——曾误置 INVALID 掐断 DDGI 纹理路径，GI 退化
            // 为解析 sky fallback（2026-09-02 回归，同日修复）。
            fusionIn.ddgi_tex = std::getenv("DDGI_SILENCE")
                ? handles::INVALID_RESOURCE : giOut.gi_output_tex;
            fusionIn.ddgi_specular_rg = std::getenv("DDGI_SILENCE")
                ? rendergraph::RGResourceHandle{} : giOut.gi_specular_rg;
            fusionIn.ddgi_specular_tex = std::getenv("DDGI_SILENCE")
                ? handles::INVALID_RESOURCE : giOut.gi_specular_tex;
            fusionIn.spgi_rg = std::getenv("SPGI_SILENCE")
                ? rendergraph::RGResourceHandle{} : spgiOut.gi_output;
            fusionIn.spgi_tex = handles::INVALID_RESOURCE;
            fusionIn.spgi_pass = screen_probe_pass_.get();
            fusionIn.gbuffer_albedo = gpuDraw.GetGBufferAlbedo();
            fusionIn.ssao_rg = std::getenv("SSAO_SILENCE")
                ? rendergraph::RGResourceHandle{} : ssaoOut.ssao_output;
            fusionIn.ssao_tex = ssao_pass_ ? ssao_pass_->GetFilterTexture() : black_texture_;
            fusionIn.ssr_rg = ssrOutRWCB.outputColor;
            fusionIn.ssr_tex = handles::INVALID_RESOURCE;
            fusionIn.gbuffer_orm = gpuDraw.GetGBufferORM();
            // sun-audit P1: GI_LADDER_DUMP 能量台账输入（vis 半分辨率 R8 + invVP）
            fusionIn.shadow_vis_tex = shadowOut.shadow_visibility_tex;
            fusionIn.inv_view_projection = Inverse(proj_matrix_ * view_matrix_);
            fusionIn.current_buffer_index = cbIdx;
            // Phase 1d: twin 站点同步——frame_index 是三缓冲槽位单一事实源
            // （漏设会钉死 slot0/prevIdx=2，仪表与下游读到未写槽 = NaN）。
            fusionIn.frame_index = static_cast<u32>(frameCount_);
            fusionIn.render_width = render_width_;
            fusionIn.render_height = render_height_;
            fusionIn.black_texture = black_texture_;
            fusionIn.volume_scatter_rg = volume_scatter_rg.IsValid()
                ? volume_scatter_rg : volumeOut.volume_scatter;
            // WRC 计划 Stream C1（Fusion 档位化）twin 站点同步（见 2790 处注释）。
            fusionIn.pixel_gi_spgi_only = settings_.lumen.axes.use_screen_probes;

            // [REFL R4/R5] twin 站点同步：SSR composite 之后、fusion 之前。
            if (refl_specular_pass_ && refl_specular_pass_->IsInitialized()) {
                lumen::ReflSpecularTracePass::FrameData reflIn{};
                reflIn.depth_rg = gbufferDepthDL;
                reflIn.depth = gpuDraw.GetGBufferDepthSampleable();
                reflIn.albedo = gpuDraw.GetGBufferAlbedo();
                reflIn.orm = gpuDraw.GetGBufferORM();
                reflIn.ssr_rg = ssrOutRWCB.outputColor;
                reflIn.normal = gpuDraw.GetGBufferNormal();
                reflIn.inv_view_proj = Inverse(proj_matrix_ * view_matrix_);
                reflIn.camera_pos = camera_position_;
                reflIn.width = render_width_;
                reflIn.height = render_height_;
                if (surface_cache_pass_ && surface_cache_pass_->IsInitialized()) {
                    reflIn.sc = surface_cache_pass_->GetCacheView(static_cast<u32>(frameCount_));
                }
                if (wrc_volume_count_ > 0 && wrc_volumes_[0] && wrc_volumes_[0]->IsInitialized()) {
                    reflIn.wrc = wrc_volumes_[0]->GetCacheView(static_cast<u32>(frameCount_));
                    reflIn.ray_max_distance = wrc_volumes_[0]->GetParams().ray_max_distance;
                }
                if (offline_sdf_texture_ != handles::INVALID_RESOURCE && offline_sdf_resolution_ > 0) {
                    reflIn.offline_sdf = offline_sdf_texture_;
                    reflIn.offline_sdf_origin = offline_sdf_origin_;
                    reflIn.offline_sdf_extent = offline_sdf_extent_;
                    reflIn.offline_sdf_resolution = offline_sdf_resolution_;
                    reflIn.signed_sdf = offline_sdf_signed_;
                }
                reflIn.sky_color = {1.0f, 1.0f, 1.0f};   // 同 WRC：去 tier 双重压缩
                reflIn.sky_tier = lumen::WRCSkyTier();
                reflIn.probe_mode = ReflProbeEnabled();
                fusionIn.refl_specular_rg = refl_specular_pass_->AddPass(
                    graph, reflIn, static_cast<u32>(frameCount_));
                fusionIn.refl_specular_tex = refl_specular_pass_->GetOutputTex(
                    static_cast<u32>(frameCount_));
            }

            // Lightmap 旁路（RenderWithCommandBuffer 站点，与 BuildRenderGraph
            // 站点镜像——见另一处同注释；深度在此站点按需导入，RG 同物理
            // 资源自动合并身份）。
            if (lightmap_module_) {
                if (current_scene_) lightmap_module_->UpdateFrame(*current_scene_);
                LightmapModule::FrameInputs lmFrame;
                lmFrame.frame_index = static_cast<u32>(frameCount_);
                lmFrame.render_width = render_width_;
                lmFrame.render_height = render_height_;
                lmFrame.view_projection = proj_matrix_ * view_matrix_;
                lmFrame.gbuffer_depth_rg =
                    graph.ImportResource("GBufferDepth_LM", gpuDraw.GetGBufferDepthSampleable());
                lightmap_module_->AddUVPrePass(graph, lmFrame);
            }

            fusionOut = fusion_module_->AddPasses(graph, fusionIn);
        }

        // --- PostProcess: TAA → Bloom → ToneMapping ---
        // Mirrors BuildRenderGraph Step 10.5. TAA resolves the sub-pixel jitter
        // injected by GPUDrivenDraw (jitter gate is enabled below when TAA runs).
        RGResourceHandle ppInputRG;
        if (fusionOut.output_tex != handles::INVALID_RESOURCE) {
            ppInputRG = fusionOut.output_rg;
        } else if (deferredOut.deferred_output_rg.IsValid()) {
            ppInputRG = deferredOut.deferred_output_rg;
        }
        // Planar reflection composite — mirrors BuildRenderGraph Step 10.45.
        if (reflection_module_ && reflection_module_->IsEnabled() && ppInputRG.IsValid() &&
            scene_snapshot_) {
            PlanarReflectionInputs reflInRWCB;
            reflInRWCB.scene_snapshot = scene_snapshot_.get();
            reflInRWCB.view_matrix = view_matrix_;
            reflInRWCB.proj_matrix = proj_matrix_;
            reflInRWCB.camera_position = camera_position_;
            math::v3 mLDirRWCB = Normalize(settings_.lighting.light_direction);
            reflInRWCB.light_dir = math::v4{-mLDirRWCB.x, -mLDirRWCB.y, -mLDirRWCB.z, 0.0f};
            reflInRWCB.light_color = settings_.lighting.light_color;
            reflInRWCB.current_buffer_index = cbIdx;
            reflection_module_->AddPasses(graph, ppInputRG, reflInRWCB);
        }

        PostProcess::ToneMappingPassData toneOutRWCB;
        RGResourceHandle taaResolvedColorRWCB; // TONE_OFF 时 FinalBlit 的回退输入
        // 后处理链启用策略（2026-09-05 三次修正）：legacy（FinalBlit 直读
        // fusion 线性 HDR）使 ≥1.0 的像素全部贴白——同机位实测 Metal 贴白率
        // 27.5%/均值 131，Vulkan 0.7%/40（用户判定 Metal 过曝、Vulkan 偏暗，
        // 两者都是 raw 线性直出显示器的伪像）。2026-09-02 的全链楔死已无法
        // 复现（本日绑定守卫/围栏追踪/SceneData 对齐等修复后，全链 60s+
        // 稳定×2，贴白率 0.0%/2.2%）→ 默认启用全链（TAA+Bloom+ACES），
        // PRIMAL_LEGACY_POST=1 保留旧直出路径作逃生门。
        const bool legacyPostRWCB = std::getenv("PRIMAL_LEGACY_POST") != nullptr;
        if (ppInputRG.IsValid() && !legacyPostRWCB) {
            // TAA resolve active on Vulkan + Metal — jitter is safe (see
            // BuildRenderGraph Step 10.5).
            gpuDraw.SetJitterEnabled(taa_jitter_enabled_);
            auto velRWCB = graph.ImportResource("GBufferVelocity_TAA_RWCB", gpuDraw.GetGBufferVelocity());
            // Depth reprojection inputs — same gating as BuildRenderGraph.
            PostProcess::TAADepthInputs taaDepthRWCB;
            PostProcess::TAADepthInputs* taaDepthRWCBPtr = nullptr;
            {
                const auto prevDepthRWCB = depth_history_
                    ? depth_history_->GetPreviousFrameDepth(static_cast<u32>(frameCount_))
                    : nanite::DepthHistoryManager::DepthBuffer{};
                if (has_prev_taa_view_proj_ && prevDepthRWCB.is_valid &&
                    prevDepthRWCB.texture != handles::INVALID_RESOURCE &&
                    gpuDraw.GetGBufferDepthSampleable() != handles::INVALID_RESOURCE) {
                    const math::m4x4 currVP_RWCB = proj_matrix_ * view_matrix_;
                    taaDepthRWCB.currDepth = gpuDraw.GetGBufferDepthSampleable();
                    taaDepthRWCB.prevDepth = prevDepthRWCB.texture;
                    taaDepthRWCB.currInvViewProj = Inverse(currVP_RWCB);
                    taaDepthRWCB.prevViewProj = prev_taa_view_proj_;
                    taaDepthRWCBPtr = &taaDepthRWCB;
                }
            }
            // TEMP diagnosis: TAA_OFF=1 bypasses the temporal accumulation.
            RGResourceHandle hdrCleanRWCB;
            if (std::getenv("TAA_OFF")) {
                hdrCleanRWCB = ppInputRG;
            } else {
                auto taaRWCB = PostProcess::AddTAAPass(graph, ppInputRG, velRWCB,
                    render_width_, render_height_, static_cast<u32>(frameCount_), taaDepthRWCBPtr);
                hdrCleanRWCB = taaRWCB.output;
            }
            taaResolvedColorRWCB = hdrCleanRWCB;
            // P1-B/P3: 解析 TAA 输出物理纹理供下一帧 GI-PROBE 一次性读回。
            if (gi_probe_enabled_) {
                auto* res = graph.GetResource(taaResolvedColorRWCB);
                if (res) probe_taa_output_phys_ = res->GetPhysicalHandle();
            }

            auto bloomRWCB = ShouldEnableBloom(graph.GetDevice())
                ? PostProcess::AddBloomPass(graph, hdrCleanRWCB, cbIdx, MakeBloomParams())
                : PostProcess::BloomPassData{};
            // FusionIndirect already owns AO/GI composition. ToneMapping only
            // receives those legacy inputs when Fusion was unavailable.
            const bool giAlreadyCompositedRWCB =
                fusionOut.output_tex != handles::INVALID_RESOURCE;
            // TONE_OFF=1 诊断开关（同 TAA_OFF/BLOOM_OFF）。
            toneOutRWCB = std::getenv("TONE_OFF")
                ? PostProcess::ToneMappingPassData{}
                : PostProcess::AddToneMappingPass(
                    graph, hdrCleanRWCB, bloomRWCB.bloomOutput,
                    giAlreadyCompositedRWCB ? RGResourceHandle{} : ssaoOut.ssao_output,
                    giAlreadyCompositedRWCB ? RGResourceHandle{} : ssgiOut.ssgi_output,
                    velRWCB, static_cast<u32>(frameCount_));  // sun-audit P3: 绝对帧号
    // ========================================================================
    // Step 10.55 (sun-audit P1 第二层): SC atlas card-space 定点读回
    // ========================================================================
    {
        struct ScLadderHit {
            u32 res = 0, ox = 0, oy = 0, axis = 0, dir = 0;
            float cx = 0, cy = 0, cz = 0;
            float u = 0, v = 0;   // 查询点在卡片内的归一坐标
            rhi::ResourceHandle buf{handles::INVALID_RESOURCE};
        };
        static std::vector<ScLadderHit> scLadderHits;
        static bool scLadderDone = false;

        // ② frame 382：映射打印（fence 已由 BeginFrame 等待，2 帧前拷贝完成）
        if (scLadderDone && !scLadderHits.empty() && frameCount_ == 382) {
            auto f16 = [](u16 h) {
                const u32 sign = (h >> 15) & 1u, ex = (h >> 10) & 0x1Fu, mant = h & 0x3FFu;
                if (ex == 0u) return 0.0f;
                u32 bits = (sign << 31) | ((ex + 112u) << 23) | (mant << 13);
                float f; std::memcpy(&f, &bits, 4); return f;
            };
            double bestMax = 0.0;
            for (auto& h : scLadderHits) {
                if (h.buf == handles::INVALID_RESOURCE) continue;
                const u64 bytes = u64(h.res) * h.res * 8;
                const u16* t = static_cast<const u16*>(device_->MapBuffer(h.buf, 0, bytes));
                if (!t) { device_->DestroyBuffer(h.buf); h.buf = handles::INVALID_RESOURCE; continue; }
                double sum = 0.0; float mx = 0.0f; u64 n = 0;
                float mr = 0, mg = 0, mb = 0;
                for (u32 y = 0; y < h.res; y += 2) {
                    for (u32 x = 0; x < h.res; x += 2) {
                        const u16* p = t + ((size_t)y * h.res + x) * 4;
                        const float r = f16(p[0]), g = f16(p[1]), b = f16(p[2]);
                        if (std::isnan(r)) continue;
                        const float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                        sum += lum; ++n;
                        if (lum > mx) { mx = lum; mr = r; mg = g; mb = b; }
                    }
                }
                // 查询点 (u,v) 处的定点纹素（3×3 邻域均值）
                const u32 tx = std::min(h.res - 1u, u32(h.u * h.res));
                const u32 ty = std::min(h.res - 1u, u32(h.v * h.res));
                double tsum = 0.0; float tmax = 0.0f; u32 tn = 0;
                float tr = 0, tg = 0, tb = 0;
                for (int oy2 = -1; oy2 <= 1; ++oy2) {
                    for (int ox2 = -1; ox2 <= 1; ++ox2) {
                        const int px = int(tx) + ox2, py = int(ty) + oy2;
                        if (px < 0 || py < 0 || px >= int(h.res) || py >= int(h.res)) continue;
                        const u16* p = t + ((size_t)py * h.res + px) * 4;
                        const float r = f16(p[0]), g = f16(p[1]), b = f16(p[2]);
                        if (std::isnan(r)) continue;
                        const float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                        tsum += lum; ++tn;
                        if (lum > tmax) { tmax = lum; tr = r; tg = g; tb = b; }
                    }
                }
                device_->UnmapBuffer(h.buf);
                device_->DestroyBuffer(h.buf);
                h.buf = handles::INVALID_RESOURCE;
                const double mean = n ? sum / n : 0.0;
                const double texelLum = tn ? tsum / tn : 0.0;
                bestMax = std::max(bestMax, double(mx));
                const char* axn = h.axis == 0 ? "X" : (h.axis == 1 ? "Y" : "Z");
                std::fprintf(stderr,
                    "[SCLadder] card axis=%s dir=%u res=%u atlas=(%u,%u) world=(%.2f,%.2f,%.2f) "
                    "queryUV=(%.2f,%.2f) texel_lum=%.4f maxTexel=(%.3f,%.3f,%.3f) "
                    "card_mean_lum=%.4f card_max_lum=%.4f\n",
                    axn, h.dir, h.res, h.ox, h.oy, h.cx, h.cy, h.cz,
                    h.u, h.v, texelLum, tr, tg, tb, mean, mx);
            }
            const char* verdict = bestMax >= 0.5 ? "sc_source_lit(A-excluded)"
                                : (bestMax < 0.1 ? "sc_source_dark" : "sc_source_dim");
            std::fprintf(stderr,
                "[SCLadderVerdict] best_card_max_lum=%.4f -> %s | "
                "若 sc_source_lit 且 GI_LADDER 判 A：命中/方向错（查卡片深度、SDF 覆盖）；"
                "若 sc_source_dark：查太阳方向/阴影/材质（SC 源面已暗）\n",
                bestMax, verdict);
            scLadderHits.clear();
        }

        // ① frame 380：CPU 卡片查找（与 SurfaceCacheCapture/LightEval 同一 uv
        // 约定）+ 记录卡片 rect 拷贝 pass。世界点 = 中央地面 ROI 中心视线 ∩
        // y=0 平面。
        if (std::getenv("GI_LADDER_DUMP") && scLightingRG.IsValid() &&
            surface_cache_pass_ && frameCount_ == 380 && !scLadderDone) {
            const math::m4x4 invVP = Inverse(proj_matrix_ * view_matrix_);
            auto xform = [&invVP](float x, float y, float z, float out[4]) {
                const float xs[4] = {x, y, z, 1.0f};
                for (int rr = 0; rr < 4; ++rr) out[rr] = 0.0f;
                for (int cc = 0; cc < 4; ++cc)
                    for (int rr = 0; rr < 4; ++rr) out[rr] += invVP.columns[cc][rr] * xs[cc];
            };
            float n0[4], n1[4];
            xform(0.4727f * 2.0f - 1.0f, 1.0f - 2.0f * 0.9167f, 0.0f, n0);
            xform(0.4727f * 2.0f - 1.0f, 1.0f - 2.0f * 0.9167f, 1.0f, n1);
            const math::v3 p0{n0[0] / std::max(n0[3], 1e-6f), n0[1] / std::max(n0[3], 1e-6f), n0[2] / std::max(n0[3], 1e-6f)};
            const math::v3 p1{n1[0] / std::max(n1[3], 1e-6f), n1[1] / std::max(n1[3], 1e-6f), n1[2] / std::max(n1[3], 1e-6f)};
            math::v3 query{0.0f, 0.0f, 0.0f};
            if (std::abs(p1.y - p0.y) > 1e-5f) {
                const float t = (0.0f - p0.y) / (p1.y - p0.y);
                if (t > 0.0f) {
                    query.x = p0.x + (p1.x - p0.x) * t;
                    query.y = 0.0f;
                    query.z = p0.z + (p1.z - p0.z) * t;
                }
            }
            // GI_LADDER_QUERY=x,y,z：覆盖查询点（如直射光区旁的墙面/布料，
            // 审计分支"靠近直射区的表面是否收到 bounce"的定点测量）。
            if (const char* q = std::getenv("GI_LADDER_QUERY")) {
                float qx, qy, qz;
                if (std::sscanf(q, "%f,%f,%f", &qx, &qy, &qz) == 3) {
                    query = math::v3{qx, qy, qz};
                    std::fprintf(stderr, "[SCLadder] query overridden by GI_LADDER_QUERY=%s\n", q);
                }
            }
            std::fprintf(stderr,
                "[SCLadder] query world point = (%.3f, %.3f, %.3f) (ROI 中心视线 ∩ y=0)\n",
                query.x, query.y, query.z);

            const auto& cards = surface_cache_pass_->GetCardGenerator().GetCards();
            // 容差匹配：AABB 外 0.75u 内也算命中（墙面/布面是薄板，空中
            // 查询点贴面偏移可命中），按 AABB 距离升序取最近 4 张。
            constexpr float kMatchTol = 0.75f;
            struct RankedCard { float dist; float vol; const lumen::SurfaceCacheCard* p; };
            std::vector<RankedCard> ranked;
            for (const auto& card : cards) {
                const float cx = card.center.x, cy = card.center.y, cz = card.center.z;
                const float ex = card.extent.x, ey = card.extent.y, ez = card.extent.z;
                const float dx = std::max(std::abs(query.x - cx) - ex, 0.0f);
                const float dy = std::max(std::abs(query.y - cy) - ey, 0.0f);
                const float dz = std::max(std::abs(query.z - cz) - ez, 0.0f);
                const float dist = std::sqrt(dx*dx + dy*dy + dz*dz);
                if (dist <= kMatchTol) ranked.push_back({dist, ex * ey * ez, &card});
            }
            // 同距离时优先更小体积的卡片（贴墙/贴面薄板优先于庭院级巨卡）。
            // 比较器只用元素自身字段（std::sort 的枢轴临时副本上 &e-data()
            // 会越界——上一版UB）。
            std::sort(ranked.begin(), ranked.end(), [](const RankedCard& a, const RankedCard& b) {
                if (a.dist != b.dist) return a.dist < b.dist;
                return a.vol < b.vol;
            });
            for (const auto& rc : ranked) {
                const auto& [d, vol, pc] = rc;
                if (scLadderHits.size() >= 4) break;
                const auto& card = *pc;
                if (card.resolution == 0) continue;
                const float cx = card.center.x, cy = card.center.y, cz = card.center.z;
                const float ex = card.extent.x, ey = card.extent.y, ez = card.extent.z;
                const u32 axis = card.axis_direction & 0xFFu;
                const u32 dir = (card.axis_direction >> 8) & 0xFFu;
                float u = 0.0f, v = 0.0f;
                if (axis == 0) {
                    u = (query.y - cy) / (2.0f * ey) + 0.5f;
                    v = (query.z - cz) / (2.0f * ez) + 0.5f;
                } else if (axis == 1) {
                    u = (query.x - cx) / (2.0f * ex) + 0.5f;
                    v = (query.z - cz) / (2.0f * ez) + 0.5f;
                } else {
                    u = (query.x - cx) / (2.0f * ex) + 0.5f;
                    v = (query.y - cy) / (2.0f * ey) + 0.5f;
                }
                if (u <= 0.0f || u >= 1.0f || v <= 0.0f || v >= 1.0f) continue;
                ScLadderHit h;
                h.res = card.resolution; h.ox = card.atlas_offset_x; h.oy = card.atlas_offset_y;
                h.axis = axis; h.dir = dir; h.cx = cx; h.cy = cy; h.cz = cz;
                h.u = u; h.v = v;
                BufferDesc bd{};
                bd.size = u64(h.res) * h.res * 8;
                bd.bindFlags = (u32)BufferUsageFlags::TransferDst;
                bd.memoryUsage = GPUMemoryUsage::Readback;
                h.buf = device_->CreateBuffer(bd);
                if (h.buf != handles::INVALID_RESOURCE) scLadderHits.push_back(h);
            }
            scLadderDone = true;

            if (scLadderHits.empty()) {
                std::fprintf(stderr, "[SCLadder] no card AABB contains the query point\n");
            } else {
                struct SCLadderData { rhi::ResourceHandle atlas; };
                auto* hitsPtr = &scLadderHits;
                // atlas 物理句柄（三缓冲当前槽）；不通过 RG Read(CopySource)
                // 声明——与同帧其它 ShaderResource 读冲突会让 RG 状态解析
                // 死循环（实测 906% CPU 挂死）。改为 SideEffect pass + 主 cmd
                // 手动 SR→CopySource→SR 屏障（跟踪器在同一 CB 内解析）。
                const rhi::ResourceHandle atlasTex =
                    surface_cache_pass_->GetCacheView(static_cast<u32>(frameCount_)).lighting_atlas;
                graph.AddPass<SCLadderData>("SCLadderReadback",
                    rendergraph::RGPassType::Compute, rendergraph::RGPassCategory::Lighting,
                    [atlasTex](SCLadderData&, rendergraph::RenderGraphBuilder& builder) {
                        builder.SideEffect();
                    },
                    [atlasTex, hitsPtr](const SCLadderData&, rendergraph::RenderGraphContext& context) {
                        if (atlasTex == handles::INVALID_RESOURCE) return;
                        auto* cmd = context.cmdBuffer;
                        std::fprintf(stderr, "[SCLadder] readback pass executing, hits=%zu\n", hitsPtr->size());
                        ResourceBarrier toCopy{};
                        toCopy.resource = atlasTex;
                        toCopy.beforeState = ResourceState::ShaderResource;
                        toCopy.afterState = ResourceState::CopySource;
                        toCopy.subresource = 0xFFFFFFFF;
                        toCopy.queueFamily = 0xFFFFFFFF;
                        cmd->InsertBarrier(&toCopy, 1);
                        for (const auto& h : *hitsPtr) {
                            BufferTextureCopyRegion rr{};
                            rr.imageSubresource.layerCount = 1;
                            rr.imageOffset = {(int)h.ox, (int)h.oy, 0};
                            rr.imageExtent = {h.res, h.res, 1};
                            cmd->CopyTextureToBuffer(atlasTex, h.buf, &rr, 1);
                        }
                        ResourceBarrier toSR{};
                        toSR.resource = atlasTex;
                        toSR.beforeState = ResourceState::CopySource;
                        toSR.afterState = ResourceState::ShaderResource;
                        toSR.subresource = 0xFFFFFFFF;
                        toSR.queueFamily = 0xFFFFFFFF;
                        cmd->InsertBarrier(&toSR, 1);
                    });
            }
            }
        }
        }

        // Toon post process — mirrors BuildRenderGraph Step 10.6.
        RGResourceHandle postToonColorRWCB;
        if (toneOutRWCB.output.IsValid()) postToonColorRWCB = toneOutRWCB.output;
        if (toon_enabled_ && postToonColorRWCB.IsValid()) {
            auto toonDepthRWCB = graph.ImportResource("GBufferDepth_Toon_RWCB",
                                                      gpuDraw.GetGBufferDepthSampleable());
            auto toonNormalRWCB = graph.ImportResource("GBufferNormal_Toon_RWCB",
                                                       gpuDraw.GetGBufferNormal());
            auto toonOutRWCB = renderpass::AddToonPass(graph, postToonColorRWCB,
                                                       toonDepthRWCB, toonNormalRWCB,
                                                       toon_params_, cbIdx,
                                                       render_width_, render_height_);
            if (toonOutRWCB.toonOutput.IsValid()) postToonColorRWCB = toonOutRWCB.toonOutput;
        }

        // Geometry debug overlay — mirrors BuildRenderGraph Step 10.7.
        if (geometry_debug_settings_.enable && frame_view_ && postToonColorRWCB.IsValid()) {
            auto gbufferDepthGDRWCB = graph.ImportResource("GBufferDepth_GeometryDebug_RWCB",
                                                           gpuDraw.GetGBufferDepthSampleable());
            AddGeometryDebugPass(graph, postToonColorRWCB, gbufferDepthGDRWCB,
                                 *frame_view_, &geometry_debug_settings_);
        }

        // --- Pass Debug View（镜像 BuildRenderGraph Step 11 分支）---
        // blitBackbufferRG：本帧 BackBuffer 的 RG 节点（PassDebugView 与
        // FinalBlit 二选一产出；探针球 overlay 依赖它做 RG 排序）。
        // （声明已提升到分支选择前——跨 if/else 共享。）
        bool rwcbPassDebug = false;
        if (debug_view_ != static_cast<u32>(PassDebugView::Off) &&
            pass_debug_view_module_ && pass_debug_view_module_->IsInitialized()) {
            RGResourceHandle pdSource{}, pdId{}, pdDepth{};
            ResourceHandle pdClusterMap{handles::INVALID_RESOURCE};
            ResourceHandle pdAuxBuffer{handles::INVALID_RESOURCE};
            u32 pdAuxCount = 0;
            if (ResolvePassDebugViewSource(graph, cbIdx, deferredOut, fusionOut,
                                           pdSource, pdId, pdDepth, pdClusterMap,
                                           pdAuxBuffer, pdAuxCount)) {
                PassDebugViewInputs dvIn;
                dvIn.source_rg = pdSource;
                dvIn.id_rg = pdId;
                dvIn.depth_rg = pdDepth;
                dvIn.cluster_map_buffer = pdClusterMap;
                dvIn.aux_buffer = pdAuxBuffer;
                dvIn.aux_count = pdAuxCount;
                dvIn.backbuffer_rg = graph.ImportResource("BackBuffer", target);
                dvIn.current_buffer_index = cbIdx;
                dvIn.render_width = target_width_ > 0 ? target_width_ : render_width_;
                dvIn.render_height = target_height_ > 0 ? target_height_ : render_height_;
                dvIn.view = static_cast<PassDebugView>(debug_view_);
                dvIn.inv_view_proj = proj_matrix_ * view_matrix_;
                dvIn.proj = proj_matrix_;
                dvIn.near_plane = 0.1f;
                dvIn.far_plane = 1000.0f;
                dvIn.cluster_count = gpuDraw.GetClusterMapEntryCount();
                // BackBuffer 实际格式（RGBA16F 离屏 RT 需要对应管线变体）。
                dvIn.output_format = targetDesc.format;
                pass_debug_view_module_->AddPass(graph, dvIn);
                blitBackbufferRG = dvIn.backbuffer_rg;
                rwcbPassDebug = true;
            } else if (!debug_view_fallback_warned_) {
                debug_view_fallback_warned_ = true;
                PRIMAL_LOG_STREAM(Info, "Render")
                    << "[Pipeline] PassDebugView '" << GetPassDebugViewName(debug_view_)
                    << "' target unavailable this frame — falling back to FinalBlit" << std::endl;
            }
        }

        // --- Final Blit → backbuffer ---
        if (!rwcbPassDebug) {
        FinalBlitInputs blitIn;
        // Prefer the Toon-processed frame (== toneOutRWCB.output when toon is off);
        // fall back to HDR composite if PostProcess was skipped. 注意：这里只决定
        // RG 句柄——物理句柄由 FinalBlit 在 exec 时解析（瞬态纹理 AddPass 时
        // 尚未分配，预解析拿到 INVALID 曾令后处理链整体被裁剪，见 FinalBlitModule）。
        if (postToonColorRWCB.IsValid()) {
            blitIn.input_rg = postToonColorRWCB;
        }
        // TONE_OFF 诊断路径：ToneMapping 被跳过时直读 TAA 解析后的 HDR。
        if (!blitIn.input_rg.IsValid() && taaResolvedColorRWCB.IsValid()) {
            blitIn.input_rg = taaResolvedColorRWCB;
        }
        if (!blitIn.input_rg.IsValid() &&
            fusionOut.output_tex != handles::INVALID_RESOURCE) {
            blitIn.input_rg = fusionOut.output_rg;
            blitIn.input_tex = fusionOut.output_tex;
        }
        if (!blitIn.input_rg.IsValid() &&
            deferredOut.deferred_output_tex != handles::INVALID_RESOURCE) {
            blitIn.input_rg = deferredOut.deferred_output_rg;
            blitIn.input_tex = deferredOut.deferred_output_tex;
        }
        // Vulkan numerical debug views must reach the target unchanged.
        // In particular, GI and ACES can make a zero shadow mask look nonzero.
        if (const char* debug = std::getenv("PRIMAL_LIGHTING_DEBUG");
            debug && device_->GetPlatform() == rhi::RHIPlatform::Vulkan) {
            if (std::strcmp(debug, "normal") == 0 || std::strcmp(debug, "ndotl") == 0 ||
                std::strcmp(debug, "shadow") == 0 || std::strcmp(debug, "albedo") == 0) {
                blitIn.input_rg = deferredOut.deferred_output_rg;
                blitIn.input_tex = deferredOut.deferred_output_tex;
            }
        }
        blitIn.backbuffer_rg = graph.ImportResource("BackBuffer", target);
        blitIn.current_buffer_index = cbIdx;
        blitIn.render_width = target_width_ > 0 ? target_width_ : render_width_;
        blitIn.render_height = target_height_ > 0 ? target_height_ : render_height_;
        // R2.1 神经帧生成（RenderWithCommandBuffer 路径，同 BuildRenderGraph）。
        if (neural_framegen_module_ && neural_framegen_module_->IsInitialized()) {
            NeuralFrameGenInputs fgIn;
            fgIn.blit_input = blitIn.input_rg;
            fgIn.render_width = blitIn.render_width;
            fgIn.render_height = blitIn.render_height;
            fgIn.current_buffer_index = cbIdx;
            fgIn.input_is_hdr = false;
            fgIn.present_interpolated = framegen_present_;
            neural_framegen_module_->AddPass(graph, fgIn);
            if (framegen_present_ && neural_framegen_module_->PresentReady()) {
                blitIn.input_rg = graph.ImportTexture(
                    "FrameGenPresent", neural_framegen_module_->OutputTexture(),
                    neural_framegen_module_->OutputDesc(),
                    rhi::ResourceState::ShaderResource);
                blitIn.input_tex = handles::INVALID_RESOURCE;
            }
        }
        final_blit_module_->AddPass(graph, blitIn);
        blitBackbufferRG = blitIn.backbuffer_rg;
        } // !rwcbPassDebug

        // M8/T8.5: probe sphere overlay (RenderWithCommandBuffer path — the
        // Sponza test drives THIS entrypoint; mirrors the BuildRenderGraph
        // injection at the end of its Step 11).
        {
            static bool psLogged = false;
            if (!psLogged) {
                psLogged = true;
                const bool modOk = probe_sphere_module_ && probe_sphere_module_->IsInitialized();
                const bool ddgiOk = ddgi_pass_ && ddgi_pass_->IsInitialized();
                PRIMAL_LOG_STREAM(Error, "Render") << "[ProbeSpheres] RWCB injection: enabled=" << probe_sphere_debug_
                          << " module=" << (modOk ? "ok" : "missing")
                          << " ddgi=" << (ddgiOk ? "ok" : "missing")
                          << " probes=" << (ddgiOk ? ddgi_pass_->GetVolumeData().ProbeCountTotal : 0u)
                          << std::endl;
            }
        }
        if (probe_sphere_debug_ && probe_sphere_module_ && probe_sphere_module_->IsInitialized() &&
            ddgi_pass_ && ddgi_pass_->IsInitialized()) {
            const auto& volPS = ddgi_pass_->GetVolumeData();
            if (volPS.ProbeCountTotal > 0) {
                DebugProbeSphereInputs psIn;
                // WRITE a fresh alias of the swapchain image; declare READ on
                // the blit's output node so the RG serializes after FinalBlit
                // (a bare alias has no edge and the blit would overwrite us).
                psIn.backbuffer_rg = graph.ImportResource("BackBuffer_ProbeSphere", target);
                psIn.blit_output_rg = blitBackbufferRG;
                psIn.current_buffer_index = cbIdx;
                psIn.render_width = target_width_ > 0 ? target_width_ : render_width_;
                psIn.render_height = target_height_ > 0 ? target_height_ : render_height_;
                psIn.view_matrix = view_matrix_;
                psIn.proj_matrix = proj_matrix_;
                psIn.camera_position = {camera_position_.x, camera_position_.y,
                                        camera_position_.z, 0.0f};
                math::v3 toLightPS = -Normalize(settings_.lighting.light_direction);
                psIn.sun_dir = {toLightPS.x, toLightPS.y, toLightPS.z, 0.0f};
                psIn.probe_origin = {volPS.ProbeOrigin.x, volPS.ProbeOrigin.y,
                                     volPS.ProbeOrigin.z};
                psIn.probe_counts[0] = ddgi_pass_->GetParams().probe_count_x;
                psIn.probe_counts[1] = ddgi_pass_->GetParams().probe_count_y;
                psIn.probe_counts[2] = ddgi_pass_->GetParams().probe_count_z;
                psIn.probe_count = volPS.ProbeCountTotal;
                psIn.probe_spacing = ddgi_pass_->GetParams().probe_spacing;
            // [P0 2026-09-21] RWCB 注入点补 SH9 句柄——radiance/confidence/
            // variance 模式的 DDGI 腿需要它切换 irradiance 管线。绑上一完成
            // 帧槽（(cbIdx+2)%3 ≡ cbIdx-2：DDGI 当帧槽在 overlay 采样时点
            // 尚未落笔，实测绑当帧槽得全黑球）。
            psIn.irradiance_buffer =
                ddgi_pass_->GetIrradianceBufferBySlot((cbIdx + 2u) % 3u);
            // [④ 2026-09-21] moments（confidence/variance 模式数据源）——
            // 同上一完成帧槽。
            psIn.ddgi_depth_buffer =
                ddgi_pass_->GetDepthBufferBySlot((cbIdx + 2u) % 3u);
                // 2026-09-17 审计 §8.3：relocated origins CPU 读回（60 帧后到
                // 达，版本变化触发 VB 重建）→ 球体按 绿/黄/红/灰 状态着色。
                u32 originStateCount = 0, originStateVersion = 0;
                if (const math::v4* originStates =
                        ddgi_pass_->GetRelocatedOriginsCpu(originStateCount, originStateVersion)) {
                    psIn.origin_states = originStates;
                    psIn.origin_states_count = originStateCount;
                    psIn.origin_states_version = originStateVersion;
                }
                probe_sphere_module_->AddPass(graph, psIn);
            }
        }

        // [WRC] WRC 探针球 overlay（RWCB 镜像——Sponza 测试驱动本入口）。
        if (wrc_probe_debug_ && probe_sphere_module_ && probe_sphere_module_->IsInitialized() &&
            probe_sphere_module_->HasWrcPipeline() &&
            wrc_volumes_[0] && wrc_volumes_[0]->IsInitialized()) {
            const auto& wrcParams = wrc_volumes_[0]->GetParams();
            const u32 wrcProbeCount = wrc_volumes_[0]->GetProbeCountTotal();
            if (wrcProbeCount > 0) {
                DebugProbeSphereInputs wpIn;
                wpIn.backbuffer_rg = graph.ImportResource("BackBuffer_WRCProbeSphere", target);
                wpIn.blit_output_rg = blitBackbufferRG;
                wpIn.current_buffer_index = cbIdx;
                wpIn.render_width = target_width_ > 0 ? target_width_ : render_width_;
                wpIn.render_height = target_height_ > 0 ? target_height_ : render_height_;
                wpIn.view_matrix = view_matrix_;
                wpIn.proj_matrix = proj_matrix_;
                wpIn.camera_position = {camera_position_.x, camera_position_.y,
                                        camera_position_.z, 0.0f};
                math::v3 toLightW = -Normalize(settings_.lighting.light_direction);
                wpIn.sun_dir = {toLightW.x, toLightW.y, toLightW.z, 0.0f};
                const math::v3 wrcOrigin = wrc_volumes_[0]->GetVolumeOrigin();
                wpIn.probe_origin = wrcOrigin;
                wpIn.probe_counts[0] = wrcParams.probe_count_x;
                wpIn.probe_counts[1] = wrcParams.probe_count_y;
                wpIn.probe_counts[2] = wrcParams.probe_count_z;
                wpIn.probe_count = wrcProbeCount;
                wpIn.probe_spacing = wrcParams.probe_spacing;
                wpIn.wrc_data_buffer = wrc_volumes_[0]->GetProbeDataBuffer();
                wpIn.depth_rg = graph.ImportResource("GBufferDepth_WRCProbeSpheres",
                                                     gpuDraw.GetGBufferDepthSampleable());
                wpIn.depth_width = render_width_;
                wpIn.depth_height = render_height_;
                probe_sphere_module_->AddPass(graph, wpIn);
            }
        }

        // DDGI dispatch（trace/finalize/update，裸 compute）→ render graph
        // （GIGather/Fusion 等读 DDGI 输出）：必须隔 encoder 边界保证
        // 写读有序——同一 encoder 内无 barrier 则 Metal 可能并发调度，
        // 表现为 GI 闪烁。MoltenVK 自动插 barrier 所以 Vulkan 不闪。
        cmd->MemoryBarrier(
            PipelineStage::ComputeShader, PipelineStage::ComputeShader,
            AccessFlag::ShaderWrite, AccessFlag::ShaderRead);

        graph.Compile();
        graph.Execute(cmd);
        ReportGraphPassTimes(graph);

        // Depth history for TAA reprojection — mirrors Render()'s end-of-frame
        // store. RWCB's command buffer is caller-owned; the store only records
        // into it, submission stays with the caller.
        if (depth_history_ && gpuDraw.IsInitialized()) {
            if (depth_history_->StoreCurrentFrameDepth(gpuDraw.GetGBufferDepthSampleable(), cmd, static_cast<u32>(frameCount_))) {
                prev_taa_view_proj_ = proj_matrix_ * view_matrix_;
                has_prev_taa_view_proj_ = true;
            }
        }
    } else if (!wants_gbuffer && final_blit_module_) {
        // T4.6.5 part 39: VisibilityBufferOnly path. Stage2 rasterized into
        // visibility_buffer_, ResolveVisibilityBuffer (auto-called in
        // gpuDraw.Execute) wrote resolve_output_texture_. Skip Shadow +
        // Deferred + SSAO + Fusion entirely; FinalBlit reads resolve_output_texture_
        // directly as the backbuffer source.
        renderGraph_->Clear();
        auto& graph = *renderGraph_;

        FinalBlitInputs blitIn;
        blitIn.input_rg = graph.ImportResource("ResolveOutput_DL", gpuDraw.GetResolveOutputTexture());
        blitIn.input_tex = gpuDraw.GetResolveOutputTexture();
        blitIn.backbuffer_rg = graph.ImportResource("BackBuffer", target);
        blitIn.current_buffer_index = cbIdx;
        blitIn.render_width = target_width_ > 0 ? target_width_ : render_width_;
        blitIn.render_height = target_height_ > 0 ? target_height_ : render_height_;
        // R2.1 神经帧生成（RenderWithCommandBuffer 路径，同 BuildRenderGraph）。
        if (neural_framegen_module_ && neural_framegen_module_->IsInitialized()) {
            NeuralFrameGenInputs fgIn;
            fgIn.blit_input = blitIn.input_rg;
            fgIn.render_width = blitIn.render_width;
            fgIn.render_height = blitIn.render_height;
            fgIn.current_buffer_index = cbIdx;
            fgIn.input_is_hdr = false;
            fgIn.present_interpolated = framegen_present_;
            neural_framegen_module_->AddPass(graph, fgIn);
            if (framegen_present_ && neural_framegen_module_->PresentReady()) {
                blitIn.input_rg = graph.ImportTexture(
                    "FrameGenPresent", neural_framegen_module_->OutputTexture(),
                    neural_framegen_module_->OutputDesc(),
                    rhi::ResourceState::ShaderResource);
                blitIn.input_tex = handles::INVALID_RESOURCE;
            }
        }
        final_blit_module_->AddPass(graph, blitIn);
        blitBackbufferRG = blitIn.backbuffer_rg;

        graph.Compile();
        graph.Execute(cmd);
        ReportGraphPassTimes(graph);

        // Depth history for TAA reprojection — mirrors Render()'s end-of-frame
        // store. RWCB's command buffer is caller-owned; the store only records
        // into it, submission stays with the caller.
        if (depth_history_ && gpuDraw.IsInitialized()) {
            if (depth_history_->StoreCurrentFrameDepth(gpuDraw.GetGBufferDepthSampleable(), cmd, static_cast<u32>(frameCount_))) {
                prev_taa_view_proj_ = proj_matrix_ * view_matrix_;
                has_prev_taa_view_proj_ = true;
            }
            // NR transformer 推理已移入上方 wants_gbuffer 分支的 RG 建图段
            //（:5206 附近）——Execute 之后 AddPass 不进图，勿在此重复。
        }
    } else {
        // Fallback: manual blit GBuffer albedo to backbuffer
        RenderPassDesc rpDesc{};
        rpDesc.colorAttachments.resize(1);
        rpDesc.colorAttachments[0].texture = target;
        rpDesc.colorAttachments[0].loadOp = LoadAction::Clear;
        rpDesc.colorAttachments[0].clearValue.color = math::v4{0.0f, 0.0f, 0.0f, 1.0f};
        rpDesc.colorAttachments[0].storeOp = StoreAction::Store;
        cmd->BeginRenderPass(rpDesc);

        auto blitPipeline = final_blit_module_ ? final_blit_module_->GetPipeline() : handles::INVALID_PIPELINE;
        auto blitLayout = final_blit_module_ ? final_blit_module_->GetPipelineLayout() : handles::INVALID_PIPELINE_LAYOUT;
        auto blitDescSet = final_blit_module_ ? final_blit_module_->GetDescriptorSet(cbIdx) : handles::INVALID_DESCRIPTOR_SET;

        if (blitPipeline != handles::INVALID_PIPELINE) {
            WriteDescriptorSet write{};
            DescriptorImageInfo imgInfo{};
            write.dstSet = blitDescSet;
            write.dstBinding = 0;
            write.dstArrayElement = 0;
            write.descriptorCount = 1;
            write.descriptorType = DescriptorType::SampledImage;
            // T4.6.5 part 39: fallback path fires only when final_blit_module_
            // is null (subsystems not initialized). In that case render_mode_
            // is at its default GBuffer; pick the right source texture anyway
            // for defensive correctness.
            imgInfo.imageView = wants_gbuffer ? gpuDraw.GetGBufferAlbedo()
                                              : gpuDraw.GetResolveOutputTexture();
            imgInfo.imageLayout = ResourceState::ShaderResource;
            write.imageInfo = &imgInfo;
            device_->UpdateDescriptorSets(1, &write);

            u32 blitW = target_width_ > 0 ? target_width_ : render_width_;
            u32 blitH = target_height_ > 0 ? target_height_ : render_height_;
            cmd->SetViewport({{0, 0}, {static_cast<float>(blitW), static_cast<float>(blitH)}, 0, 1});
            cmd->SetScissor({{0, 0}, {blitW, blitH}});
            cmd->BindGraphicsPipeline(blitPipeline);
            const DescriptorSetHandle sets[] = { blitDescSet };
            cmd->BindDescriptorSets(PipelineBindPoint::Graphics, blitLayout, 0, 1, sets, 0, nullptr);
            cmd->Draw(3, 0, 1, 0);
        }

        cmd->EndRenderPass();
    }

    const auto& cmdStats = cmd->GetStats();
    stats_.drawCallCount = cmdStats.drawCallCount;
    stats_.gpuFrameTimeMs = cmdStats.commandExecutionTime;

    frameCount_++;

    // Drain queued streaming-mesh destroys. Safe on Metal due to in-flight
    // resource retention (the command buffer for this frame has not yet been
    // submitted — submission happens in the caller, e.g. TestPCGScatter).
    pcg::GPUMesher::Get().DrainDeferredDestroys();
    scene.ClearTombstonedStreamingMeshes();

    auto endTime = std::chrono::high_resolution_clock::now();
    stats_.cpuFrameTimeMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();
}

// ============================================================================
// Render (legacy — creates/destroys CB per frame)
// ============================================================================

void StandardRenderPipeline::Render(RenderScene& scene, RenderView& view,
                                     ResourceHandle target, const TextureDesc& targetDesc,
                                     SyncHandle signalFence) {
    OnSceneChanged(scene, current_scene_ == nullptr);
    current_scene_ = &scene;
    frame_view_ = &view;  // consumed by the optional GeometryDebug overlay
    auto startTime = std::chrono::high_resolution_clock::now();
    if (!device_ || !renderGraph_) return;

    u32 cbIdx = static_cast<u32>(frameCount_ % 3);

    if (targetDesc.size.x > 0 && targetDesc.size.y > 0) {
        target_width_ = targetDesc.size.x;
        target_height_ = targetDesc.size.y;
    }

    ApplyConfigChanges();
    UpdatePerFrame(scene, view);

    // --- Editor mode: lightweight forward rendering ---
    if (editor_mode_ && forward_renderer_) {
        CommandBufferHandle cmdHandle = device_->CreateCommandBuffer(CommandQueueType::Graphics);
        if (cmdHandle == handles::INVALID_COMMAND_BUFFER) return;

        RHICommandBuffer* cmd = GetCommandBuffer(cmdHandle);
        if (!cmd || !cmd->Initialize() || !cmd->Begin()) {
            if (cmd) device_->DestroyCommandBuffer(cmdHandle);
            return;
        }

        // Sync entities to RenderScene
        if (!static_entity_ids_.empty() || !pcg_entity_ids_.empty()) {
            SyncEntitiesToRenderScene(scene);
            forward_renderer_->SetRenderScene(&scene);
        }
        // Sync geometry entities for line overlay
        if (!geometry_entity_ids_.empty()) {
            forward_renderer_->SetGeometryEntities(geometry_entity_ids_);
        }

        forward_renderer_->Render(cmd, view_matrix_, proj_matrix_, camera_position_,
                                   target, cbIdx % 3, targetDesc.format);

        // PCG SDF: dispatch voxelization + issue readback after scene render
        auto& gsdf = nanite::GlobalSDF::Get();
        static int s_guard_checks = 0;
        if (s_guard_checks < 3) {
            PRIMAL_LOG_STREAM(Info, "Render") << "[StdPipeline] DispatchVoxelization guard #" << s_guard_checks
                      << " readback_init=" << pcg_sdf_readback_initialized_
                      << " sdf_init=" << gsdf.IsInitialized()
                      << " vox_ready=" << gsdf.IsVoxelizationReady()
                      << " editor_mode=" << editor_mode_
                      << std::endl;
            ++s_guard_checks;
        }
        if (pcg_sdf_readback_initialized_ && gsdf.IsInitialized() && gsdf.IsVoxelizationReady()) {
            gsdf.DispatchVoxelization(cmd, 0);
            pcg_sdf_readback_.IssueReadback(cmd, gsdf.GetCascade(0).sdf_texture);
        }

        cmd->End();
        QueueSubmitInfo submitInfo{};
        submitInfo.cmdBuffer = cmdHandle;
        submitInfo.signalFence = signalFence;
        device_->Submit(submitInfo);
        device_->DestroyCommandBuffer(cmdHandle);

        frameCount_++;

        // Frame boundary: drain deferred-destroy queue from GPUMesher.
        pcg::GPUMesher::Get().DrainDeferredDestroys();
        scene.ClearTombstonedStreamingMeshes();

        auto endTime = std::chrono::high_resolution_clock::now();
        stats_.cpuFrameTimeMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();
        return;
    }

    renderGraph_->Clear();
    current_output_format_ = targetDesc.format;  // PassDebugView 管线变体选择用
    BuildRenderGraph(target, cbIdx);
    renderGraph_->Compile();

    CommandBufferHandle cmdHandle = device_->CreateCommandBuffer(CommandQueueType::Graphics);
    if (cmdHandle == handles::INVALID_COMMAND_BUFFER) return;

    RHICommandBuffer* cmd = GetCommandBuffer(cmdHandle);
    if (!cmd || !cmd->Initialize() || !cmd->Begin()) {
        if (cmd) device_->DestroyCommandBuffer(cmdHandle);
        return;
    }

    renderGraph_->Execute(cmd);

    auto& gpuDraw = nanite::GPUDrivenDrawPipeline::Get();
    if (depth_history_ && gpuDraw.IsInitialized()) {
        if (depth_history_->StoreCurrentFrameDepth(gpuDraw.GetGBufferDepthSampleable(), cmd, static_cast<u32>(frameCount_))) {
            // Depth copy for frame N landed — cache frame N's view-projection so
            // next frame's TAA can reproject against this depth (the buffer the
            // manager returns for frame N+1 is exactly the one stored here).
            prev_taa_view_proj_ = proj_matrix_ * view_matrix_;
            has_prev_taa_view_proj_ = true;
        }
    }
    if (color_history_ && deferred_module_) {
        color_history_->StoreCurrentFrameColor(
            deferred_module_->GetOutputTexture(cbIdx), cmd, static_cast<u32>(frameCount_));
    }

    cmd->End();

    QueueSubmitInfo submitInfo{};
    submitInfo.cmdBuffer = cmdHandle;
    submitInfo.signalFence = signalFence;
    device_->Submit(submitInfo);

    // T4.6.5 part 24.6 (B8 fix): wait for cmd buffer completion before
    // destroying. Submit returns immediately while the GPU is still
    // processing; destroying the cmd buffer (which owns the VkFence) before
    // completion triggers VUID-vkFreeCommandBuffers-pCommandBuffers-00047 +
    // cascades into GPU lost. WaitForCompletion is a no-op on Metal/Dawn
    // (their cmd buffers don't expose per-buffer fences this way) but is
    // required on Vulkan.
    cmd->WaitForCompletion();

    const auto& cmdStats = cmd->GetStats();
    stats_.drawCallCount = cmdStats.drawCallCount;
    stats_.gpuFrameTimeMs = cmdStats.commandExecutionTime;
    stats_.passExecutionTimes = renderGraph_->GetPassExecutionTimes();

    if (gpuOptimizer_) {
        for (const auto& [passName, timeMs] : stats_.passExecutionTimes) {
            gpuOptimizer_->RecordPassExecutionTime(passName, timeMs);
        }
        gpuOptimizer_->Update(frameCount_, 16.6f / 1000.0f);
    }

    device_->DestroyCommandBuffer(cmdHandle);

    frameCount_++;

    // Frame boundary: drain deferred-destroy queue from GPUMesher.
    pcg::GPUMesher::Get().DrainDeferredDestroys();
    scene.ClearTombstonedStreamingMeshes();

    auto endTime = std::chrono::high_resolution_clock::now();
    stats_.cpuFrameTimeMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();
}

} // namespace primal::graphics
