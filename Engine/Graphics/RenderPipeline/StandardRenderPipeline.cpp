#include "StandardRenderPipeline.h"
#include "Graphics/RenderPipeline/RenderPasses/ForwardPass.h"
#include "Graphics/RenderPipeline/RenderPasses/PostProcess/SSAOPass.h"
#include "Graphics/RenderPipeline/RenderPasses/PostProcess/BloomPass.h"
#include "Graphics/RenderPipeline/RenderPasses/PostProcess/ToneMappingPass.h"
#include "Graphics/RenderPipeline/RenderPasses/PostProcess/HZBPass.h"
#include "Graphics/RenderPipeline/RenderPasses/PostProcess/VelocityPass.h"
#include "Graphics/RenderPipeline/RenderPasses/PostProcess/LumenSSGIDawnPass.h"
#include "Graphics/RenderScene.h"
#include "Graphics/RenderView.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Core/RHIMath.h"
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
#include "Graphics/Scene/CameraSyncSystem.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"
#include "Components/Entity.h"
#include "Components/Transform.h"
#include "Components/Light.h"
#include "EngineAPI/GameEntity.h"
#include "EngineAPI/GameEntity_impl.h"
#include "EngineAPI/LightComponent.h"
#include "Components/Material.h"
#include <iostream>
#include <chrono>

namespace primal::graphics {

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
        std::cerr << "[StandardPipeline] GPU Optimizer init failed (non-critical)" << std::endl;
    }

    // Create utility black texture
    // Apple Silicon zero-initializes new textures (RGBA=0,0,0,0)
    {
        TextureDesc desc{};
        desc.size = {1, 1, 1};
        desc.format = DataFormat::RGBA16_Float;
        desc.usage = TextureUsage::ShaderResource;
        black_texture_ = device->CreateTexture(desc);
    }

    return true;
}

void StandardRenderPipeline::Shutdown() {
    if (s_instance == this) s_instance = nullptr;
    ShutdownSubsystems();
    ShutdownLumenPasses();

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

void StandardRenderPipeline::SetLumenConfig(const lumen::LumenConfig& config) {
    settings_.lumen = config;
    settings_.quality = PipelineQualityConfig::FromPreset(config.quality);

    // Apply render scale: GPU resolution = logical window size × scale
    render_width_  = static_cast<u32>(logical_width_  * settings_.quality.render_scale);
    render_height_ = static_cast<u32>(logical_height_ * settings_.quality.render_scale);

    std::cout << "[Pipeline] Render scale=" << settings_.quality.render_scale
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
        std::cout << "[Pipeline] Pass '" << GetRenderPassInfo(pass).name
                  << "' " << (enabled ? "enabled" : "disabled") << std::endl;
    }
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
    render_width_  = static_cast<u32>(logical_width_  * settings_.quality.render_scale);
    render_height_ = static_cast<u32>(logical_height_ * settings_.quality.render_scale);
}

bool StandardRenderPipeline::ApplyConfigChanges() {
    if (!settings_dirty_) return false;
    settings_dirty_ = false;

    if (subsystems_initialized_) {
        ShutdownLumenPasses();
        InitializeLumenPasses();
    }
    return true;
}

// ============================================================================
// Subsystem Init / Shutdown
// ============================================================================

void StandardRenderPipeline::InitializeSubsystems() {
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

    // Global SDF
    auto& globalSDF = nanite::GlobalSDF::Get();
    if (!globalSDF.IsInitialized()) {
        globalSDF.Initialize(device_);
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

    deferred_module_ = std::make_unique<DeferredLightingModule>();
    deferred_module_->Initialize(device_, deferred_vs_, deferred_ps_, render_width_, render_height_);

    final_blit_module_ = std::make_unique<FinalBlitModule>();
    final_blit_module_->Initialize(device_, blit_vs_, blit_ps_);

    if (settings_.quality.enable_ddgi && gi_gather_shader_ != handles::INVALID_SHADER) {
        gi_gather_module_ = std::make_unique<GIGatherModule>();
        gi_gather_module_->Initialize(device_, gi_gather_shader_, render_width_, render_height_);
    }

    if (settings_.quality.enable_ssgi || settings_.quality.enable_ddgi) {
        fusion_module_ = std::make_unique<FusionCompositeModule>();
        fusion_module_->Initialize(device_, fusion_indirect_ps_, blit_vs_,
                                   fusion_composite_ps_, render_width_, render_height_);
    }

    if (settings_.quality.enable_surface_cache && settings_.quality.enable_ddgi) {
        sc_ddgi_module_ = std::make_unique<SCDDGIIntegrationModule>();
        sc_ddgi_module_->Initialize(device_, sc_card_radiance_shader_,
                                    sc_probe_irradiance_shader_, render_width_, render_height_);
    }

    // --- Lumen passes ---
    InitializeLumenPasses();

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
    if (fusion_module_) { fusion_module_->Shutdown(); fusion_module_.reset(); }
    if (gi_gather_module_) { gi_gather_module_->Shutdown(); gi_gather_module_.reset(); }
    if (final_blit_module_) { final_blit_module_->Shutdown(); final_blit_module_.reset(); }
    if (deferred_module_) { deferred_module_->Shutdown(); deferred_module_.reset(); }
    if (shadow_module_) { shadow_module_->Shutdown(); shadow_module_.reset(); }

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

// ============================================================================
// Lumen Pass Init / Shutdown
// ============================================================================

void StandardRenderPipeline::InitializeLumenPasses() {
    if (!device_) return;
    const auto& config = settings_.lumen;

    // DDGI
    if (settings_.quality.enable_ddgi) {
        ddgi_pass_ = std::make_unique<lumen::LumenDDGIPass>();
        lumen::DDGIRuntimeParams ddgiParams{};
        ddgiParams.probe_count_x = config.ddgi_probe_count_x;
        ddgiParams.probe_count_y = config.ddgi_probe_count_y;
        ddgiParams.probe_count_z = config.ddgi_probe_count_z;
        ddgiParams.rays_per_probe = config.ddgi_rays_per_probe;
        ddgiParams.probe_spacing = config.ddgi_probe_spacing;
        ddgiParams.irradiance_temporal_weight = config.ddgi_irradiance_temporal_weight;
        ddgiParams.depth_temporal_weight = config.ddgi_depth_temporal_weight;
        ddgiParams.ray_max_distance = config.ddgi_ray_max_distance;
        if (!ddgi_pass_->Initialize(device_, ddgiParams)) {
            std::cerr << "[Lumen] DDGI init failed" << std::endl;
            ddgi_pass_.reset();
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
            std::cerr << "[Lumen] SSAO init failed" << std::endl;
            ssao_pass_.reset();
        }
    }

    // SSGI
    if (settings_.quality.enable_ssgi) {
        ssgi_pass_ = std::make_unique<lumen::LumenSSGIPass>();
        lumen::SSGIParams ssgiParams{};
        if (!ssgi_pass_->Initialize(device_, render_width_, render_height_, ssgiParams)) {
            std::cerr << "[Lumen] SSGI init failed" << std::endl;
            ssgi_pass_.reset();
        }
    }

    // Surface Cache
    if (settings_.quality.enable_surface_cache) {
        surface_cache_pass_ = std::make_unique<lumen::SurfaceCachePass>();
        if (!surface_cache_pass_->Initialize(device_, config)) {
            std::cerr << "[Lumen] Surface Cache init failed" << std::endl;
            surface_cache_pass_.reset();
        }
    }

    // Screen Probes
    if (settings_.quality.enable_screen_probes) {
        screen_probe_pass_ = std::make_unique<lumen::ScreenProbeGIPass>();
        lumen::ScreenProbeParams probeParams{};
        probeParams.downsample_factor = config.screen_probes_spacing;
        probeParams.rays_per_probe = config.screen_probes_rays;
        if (!screen_probe_pass_->Initialize(device_, render_width_, render_height_, probeParams)) {
            std::cerr << "[Lumen] Screen Probes init failed" << std::endl;
            screen_probe_pass_.reset();
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
            std::cout << "[Lumen] Loaded static probe cache: " << probeCachePath << std::endl;
        }
    }

    // Volume Pass (post-process fog)
    if (settings_.quality.enable_volume_pass) {
        volume_pass_ = std::make_unique<volume::VolumePass>();
        volume::VolumeRuntimeParams volumeParams{};
        if (!volume_pass_->Initialize(device_, render_width_, render_height_, volumeParams)) {
            std::cerr << "[Volume] VolumePass init failed" << std::endl;
            volume_pass_.reset();
        }
    }

    // Volume Renderer (forward proxy cube + fragment ray march)
    if (settings_.quality.enable_volume_renderer) {
        volume_renderer_ = std::make_unique<volume::VolumeRenderer>();
        if (!volume_renderer_->Initialize(device_, render_width_, render_height_)) {
            std::cerr << "[Volume] VolumeRenderer init failed" << std::endl;
            volume_renderer_.reset();
        }
    }

    // Froxel Fog (frustum-aligned volumetric fog)
    if (settings_.quality.enable_froxel_fog) {
        froxel_fog_pass_ = std::make_unique<volume::FroxelFogPass>();
        if (!froxel_fog_pass_->Initialize(device_, render_width_, render_height_, settings_.froxel)) {
            std::cerr << "[FroxelFog] Init failed" << std::endl;
            froxel_fog_pass_.reset();
        }
    }

    // Fluid Render (splat-based fluid surface)
    if (settings_.quality.enable_fluid_render) {
        fluid_render_pass_ = std::make_unique<fluid::FluidRenderPass>();
        if (!fluid_render_pass_->Initialize(device_, render_width_, render_height_, settings_.fluid)) {
            std::cerr << "[FluidRender] Init failed" << std::endl;
            fluid_render_pass_.reset();
        }
    }
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
    if (ddgi_pass_) { ddgi_pass_->Shutdown(); ddgi_pass_.reset(); }
}

// ============================================================================
// Per-frame Update
// ============================================================================

void StandardRenderPipeline::UpdatePerFrame(RenderScene& scene, RenderView& view) {
    // === Phase 3: ECS Camera → RenderView 同步 ===
    // 在读 view.GetViewMatrix() 之前先让 ECS Camera 组件覆盖 RenderView 的矩阵。
    // 如果场景里没有 Camera entity，SyncCamerasFromECS no-op，view 保留上游 caller
    // 设置的矩阵（保持旧行为兼容）。
    scene_sync::SyncCamerasFromECS(view);

    view_matrix_ = view.GetViewMatrix();
    proj_matrix_ = view.GetProjectionMatrix();

    static u32 viewDiag = 0;
    if (viewDiag < 3) {
        std::cout << "[Diag] View: col0=(" << view_matrix_.columns[0][0] << "," << view_matrix_.columns[0][1] << "," << view_matrix_.columns[0][2] << ")"
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
        if (scene_snapshot_->NeedsFullRebuild()) {
            scene_snapshot_->Rebind(scene);
            scene_snapshot_->ClearFullRebuildFlag();
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

    // Extract texture IDs (with fallback to invalid_id)
    id::id_type albedo_id = (texture_count > 0) ? texture_content_ids[0] : id::invalid_id;
    id::id_type normal_id = (texture_count > 1) ? texture_content_ids[1] : id::invalid_id;
    id::id_type orm_id    = (texture_count > 2) ? texture_content_ids[2] : id::invalid_id;

    // Register mesh resource with ForwardSceneRenderer
    u32 slot = forward_renderer_->RegisterMeshResource(
        geometry_content_id, albedo_id, normal_id, orm_id);

    if (slot == (u32)-1) {
        std::cerr << "[StandardRenderPipeline] RegisterMeshEntity: failed for geometry "
                  << geometry_content_id << std::endl;
        return id::invalid_id;
    }

    // Create ECS entity with identity transform
    transform::init_info tf{};
    tf.position[0] = 0.f; tf.position[1] = 0.f; tf.position[2] = 0.f;
    tf.rotation[0] = 0.f; tf.rotation[1] = 0.f;
    tf.rotation[2] = 0.f; tf.rotation[3] = 1.f;
    tf.scale[0] = 1.f; tf.scale[1] = 1.f; tf.scale[2] = 1.f;

    game_entity::entity_info info{};
    info.transform = &tf;

    game_entity::entity entity = game_entity::create(info);
    if (!entity.is_valid()) {
        forward_renderer_->UnregisterMeshResource(slot);
        return id::invalid_id;
    }

    id::id_type eid = entity.get_id();
    static_entity_ids_.push_back(eid);
    static_mesh_slot_indices_.push_back(slot);

    return eid;
}

void StandardRenderPipeline::UnregisterMeshEntity(id::id_type entity_id) {
    // Find and remove from static entity lists
    for (size_t i = 0; i < static_entity_ids_.size(); i++) {
        if (static_entity_ids_[i] == entity_id) {
            forward_renderer_->UnregisterMeshResource(static_mesh_slot_indices_[i]);

            // Swap with last and pop
            static_entity_ids_[i] = static_entity_ids_.back();
            static_entity_ids_.pop_back();
            static_mesh_slot_indices_[i] = static_mesh_slot_indices_.back();
            static_mesh_slot_indices_.pop_back();

            // Destroy ECS entity
            game_entity::entity e{game_entity::entity_id{entity_id}};
            if (e.is_valid()) game_entity::remove(game_entity::entity_id{entity_id});
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

void StandardRenderPipeline::BuildRenderGraph(ResourceHandle backBuffer, u32 cbIdx) {
    auto& graph = *renderGraph_;
    auto& gpuDraw = nanite::GPUDrivenDrawPipeline::Get();

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
        graph.AddPass<NanitePassData>(
            "NaniteSceneRender",
            RGPassType::Graphics,
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
                auto& gpuDraw = nanite::GPUDrivenDrawPipeline::Get();
                auto* cmd = ctx.cmdBuffer;
                if (!gpuDraw.IsInitialized() || !data.scene_snapshot) return;

                auto cullingResults = data.culling_pipeline
                    ? data.culling_pipeline->GetResults()
                    : nanite::CullingResults{};

                gpuDraw.Execute(cmd, *data.scene_snapshot, data.view_matrix, data.proj_matrix,
                                cullingResults, static_cast<u32>(data.frame_count), data.buffer_index);
            }
        );

        // Pass 1c: HZB build
        if (hzb_system_ && hzb_system_->IsReady()) {
            graph.AddPass<NanitePassData>(
                "HZBBuild",
                RGPassType::Compute,
                [&](NanitePassData& data, RenderGraphBuilder& builder) {
                    builder.SideEffect();
                    data.buffer_index = cbIdx;
                },
                [this](const NanitePassData& data, RenderGraphContext& ctx) {
                    auto& gpuDraw = nanite::GPUDrivenDrawPipeline::Get();
                    if (!gpuDraw.IsInitialized()) return;
                    hzb_system_->BuildHZB(gpuDraw.GetGBufferDepthSampleable(), ctx.cmdBuffer);
                }
            );
        }
    }

    // ========================================================================
    // Step 2: Shadow Map Module
    // ========================================================================

    ShadowMapOutputs shadowOut;
    math::v3 lightDir = Normalize(settings_.lighting.light_direction);
    if (shadow_module_) {
        ShadowMapInputs shadowIn;
        shadowIn.gpu_draw_pipeline = &gpuDraw;
        shadowIn.scene_snapshot = scene_snapshot_.get();
        shadowIn.camera_position = camera_position_;
        shadowIn.view_matrix = view_matrix_;
        shadowIn.proj_matrix = proj_matrix_;
        shadowIn.light_direction = lightDir;
        shadowIn.current_buffer_index = cbIdx;
        shadowIn.shadow_quality = settings_.quality.shadow_quality;
        shadowOut = shadow_module_->AddPasses(graph, shadowIn);
    }

    // ========================================================================
    // Step 3: SSAO
    // ========================================================================

    lumen::LumenSSAOOutput ssaoOut{};
    if (ssao_pass_ && ssao_pass_->IsInitialized()) {
        // Import GBuffer under UNIQUE names for SSAO — the RG should NOT know
        // these are the same physical textures written by NaniteSceneRender.
        // Metal's encoder boundary synchronization handles transitions.
        auto gbufferNormalSSAO = graph.ImportResource("GBufferNormal_SSAO", gpuDraw.GetGBufferNormal());
        auto gbufferDepthSSAO = graph.ImportResource("GBufferDepth_SSAO", gpuDraw.GetGBufferDepthSampleable());
        lumen::SSAOCameraData camData{};
        camData.view_matrix = view_matrix_;
        camData.proj_matrix = proj_matrix_;
        camData.prev_view_matrix = view_matrix_;
        camData.prev_proj_matrix = proj_matrix_;
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
        auto gbufferAlbedoDL = graph.ImportResource("GBufferAlbedo_DL", gpuDraw.GetGBufferAlbedo());
        auto gbufferNormalDL = graph.ImportResource("GBufferNormal_DL", gpuDraw.GetGBufferNormal());
        auto gbufferORMDL = graph.ImportResource("GBufferORM_DL", gpuDraw.GetGBufferORM());
        auto gbufferDepthDL = graph.ImportResource("GBufferDepth_DL", gpuDraw.GetGBufferDepthSampleable());

        DeferredLightingInputs deferredIn;
        deferredIn.gpu_draw_pipeline = &gpuDraw;
        deferredIn.ssao_pass = nullptr;
        deferredIn.view_matrix = view_matrix_;
        deferredIn.proj_matrix = proj_matrix_;
        deferredIn.camera_position = camera_position_;
        deferredIn.light_pos = math::v4{-lightDir.x, -lightDir.y, -lightDir.z, 0.0f};
        deferredIn.light_color = settings_.lighting.light_color;
        deferredIn.cascade_splits = settings_.lighting.cascade_splits;
        deferredIn.shadow_matrix0 = shadowOut.shadow_matrix0;
        deferredIn.shadow_matrix1 = shadowOut.shadow_matrix1;
        deferredIn.current_buffer_index = cbIdx;
        deferredIn.shadow_visibility_rg = shadowOut.shadow_visibility_rg;
        deferredIn.shadow_visibility_tex = shadowOut.shadow_visibility_tex;
        deferredIn.gbuffer_albedo_rg = gbufferAlbedoDL;
        deferredIn.gbuffer_normal_rg = gbufferNormalDL;
        deferredIn.gbuffer_orm_rg = gbufferORMDL;
        deferredIn.gbuffer_depth_rg = gbufferDepthDL;
        deferredOut = deferred_module_->AddPasses(graph, deferredIn);
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
        froxelIn.gbuffer_depth = graph.ImportResource("GBufferDepth_Volume", gpuDraw.GetGBufferDepthSampleable());
        froxelIn.scene_color = deferredOut.deferred_output_rg;
        froxelIn.shadow_map = shadowOut.shadow_visibility_rg;
        froxelIn.shadow_matrix0 = shadowOut.shadow_matrix0;
        froxelIn.shadow_matrix1 = shadowOut.shadow_matrix1;
        collectSDFCascades(froxelIn);

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
        volIn.gbuffer_depth = graph.ImportResource("GBufferDepth_Volume", gpuDraw.GetGBufferDepthSampleable());
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
        fluidIn.gbuffer_depth = graph.ImportResource("GBufferDepth_Fluid", gpuDraw.GetGBufferDepthSampleable());
        fluidIn.width = render_width_;
        fluidIn.height = render_height_;
        fluidOut = fluid_render_pass_->AddPass(graph, fluidIn);
    }

    // ========================================================================
    // Step 5: Surface Cache
    // ========================================================================

    RGResourceHandle scLightingRG;
    if (surface_cache_pass_ && surface_cache_pass_->IsInitialized()) {
        lumen::SurfaceCacheFrameData frameData{};
        frameData.camera_position = {camera_position_.x, camera_position_.y, camera_position_.z};
        frameData.frame_index = static_cast<u32>(frameCount_);
        frameData.light_count = 1;

        ResourceHandle nullLightBuf{handles::INVALID_RESOURCE};
        surface_cache_pass_->AddPass(graph, RGResourceHandle{}, nullLightBuf,
                                     frameData, static_cast<u32>(frameCount_));

        scLightingRG = graph.ImportResource("SCLightingAtlas",
            surface_cache_pass_->GetLightingAtlas(static_cast<u32>(frameCount_)));

        if (screen_probe_pass_ && screen_probe_pass_->IsInitialized()) {
            screen_probe_pass_->SetSurfaceCacheData(
                surface_cache_pass_->GetLightingAtlas(static_cast<u32>(frameCount_)),
                surface_cache_pass_->GetCardDataBuffer(),
                surface_cache_pass_->GetCardLookupBuffer(),
                settings_.lumen.surface_cache_atlas_size,
                settings_.lumen.surface_cache_max_cards);
        }

        if (ddgi_pass_ && ddgi_pass_->IsInitialized()) {
            ddgi_pass_->SetSurfaceCacheResources(
                surface_cache_pass_->GetLightingAtlas(static_cast<u32>(frameCount_)),
                surface_cache_pass_->GetCardLookupBuffer(),
                surface_cache_pass_->GetCardDataBuffer(),
                settings_.lumen.surface_cache_atlas_size,
                settings_.lumen.surface_cache_max_cards);
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
        camData.prev_view_matrix = view_matrix_;
        camData.prev_proj_matrix = proj_matrix_;
        math::v3 lightFwd = Normalize(settings_.lighting.light_direction);
        camData.light_direction = lightFwd;
        camData.light_color = {settings_.lighting.ddgi_light_color.x, settings_.lighting.ddgi_light_color.y, settings_.lighting.ddgi_light_color.z};
        camData.frame_index = static_cast<u32>(frameCount_);
        camData.delta_time = settings_.advanced.delta_time_override;

        RGResourceHandle prevColor;
        if (deferredOut.deferred_output_tex != handles::INVALID_RESOURCE) {
            prevColor = graph.ImportResource("PrevFrameColor", deferredOut.deferred_output_tex);
        }

        ddgiOut = ddgi_pass_->AddPass(graph, prevColor, camData,
                                       static_cast<u32>(frameCount_), scLightingRG);
    }

    // ========================================================================
    // Step 7: SC→DDGI Integration
    // ========================================================================

    if (sc_ddgi_module_ && surface_cache_pass_ && ddgi_pass_) {
        SCDDGIInputs scDDGIIn;
        scDDGIIn.ddgi_pass = ddgi_pass_.get();
        scDDGIIn.surface_cache_pass = surface_cache_pass_.get();
        scDDGIIn.current_buffer_index = cbIdx;
        scDDGIIn.sc_lighting_rg = scLightingRG;
        sc_ddgi_module_->AddPass(graph, scDDGIIn);
    }

    // ========================================================================
    // Step 8: SSGI (Screen Space GI)
    // ========================================================================

    lumen::LumenSSGIOutput ssgiOut;
    if (ssgi_pass_ && ssgi_pass_->IsInitialized() && frameCount_ > 0) {
        auto gbufferNormalSSGI = graph.ImportResource("GBufferNormal_SSGI", gpuDraw.GetGBufferNormal());
        auto gbufferDepthSSGI = graph.ImportResource("GBufferDepth_SSGI", gpuDraw.GetGBufferDepthSampleable());
        auto gbufferVelocitySSGI = graph.ImportResource("GBufferVelocity_SSGI", gpuDraw.GetGBufferVelocity());
        auto hzbHandle = graph.ImportResource("HZBTexture_SSGI", hzb_system_->GetHZBTexture());

        RGResourceHandle prevColorSSGI;
        if (deferredOut.deferred_output_tex != handles::INVALID_RESOURCE) {
            prevColorSSGI = graph.ImportResource("PrevFrameColor_SSGI", deferredOut.deferred_output_tex);
        }

        lumen::SSGICameraData ssgiCam{};
        ssgiCam.view_matrix = view_matrix_;
        ssgiCam.proj_matrix = proj_matrix_;
        ssgiCam.prev_view_matrix = view_matrix_;
        ssgiCam.prev_proj_matrix = proj_matrix_;
        ssgiCam.frame_index = static_cast<u32>(frameCount_);
        ssgiCam.delta_time = 0.016f;

        ssgiOut = ssgi_pass_->AddPass(graph, gbufferNormalSSGI, gbufferDepthSSGI,
            gbufferVelocitySSGI, hzbHandle, prevColorSSGI,
            ssgiCam, static_cast<u32>(frameCount_),
            hzb_system_->GetMipLevels());
    }

    // ========================================================================
    // Step 9: Screen Probe GI
    // ========================================================================

    lumen::ScreenProbeGIOutput spgiOut;
    if (screen_probe_pass_ && screen_probe_pass_->IsInitialized() && frameCount_ > 0) {
        auto spDepth = graph.ImportResource("GBufferDepth_SP", gpuDraw.GetGBufferDepthSampleable());
        auto spNormal = graph.ImportResource("GBufferNormal_SP", gpuDraw.GetGBufferNormal());

        RGResourceHandle spRadiance;
        if (deferredOut.deferred_output_rg.IsValid()) {
            spRadiance = deferredOut.deferred_output_rg;
        } else {
            spRadiance = graph.ImportResource("SPBlackFallback", black_texture_);
        }

        lumen::ScreenProbeCameraData spCam{};
        spCam.view_matrix = view_matrix_;
        spCam.proj_matrix = proj_matrix_;
        spCam.camera_position = camera_position_;
        spCam.frame_index = static_cast<u32>(frameCount_);

        spgiOut = screen_probe_pass_->AddPass(graph, spDepth, spNormal,
            spRadiance, spCam, static_cast<u32>(frameCount_));
    }

    // ========================================================================
    // Step 9: GI Gather (DDGI → half-res screen texture)
    // ========================================================================

    GIGatherOutputs giOut;
    if (gi_gather_module_ && ddgi_pass_ && ddgi_pass_->IsInitialized()) {
        // Import GBuffer under UNIQUE names for GIGather — same physical textures,
        // different RG identities.
        auto gbufferDepthGI = graph.ImportResource("GBufferDepthGI", gpuDraw.GetGBufferDepthSampleable());
        auto gbufferNormalGI = graph.ImportResource("GBufferNormalGI", gpuDraw.GetGBufferNormal());
        GIGatherInputs giIn;
        giIn.ddgi_pass = ddgi_pass_.get();
        giIn.static_probe_volume = static_probe_volume_.get();
        giIn.gbuffer_depth = gpuDraw.GetGBufferDepthSampleable();
        giIn.gbuffer_normal = gpuDraw.GetGBufferNormal();
        giIn.gbuffer_depth_rg = gbufferDepthGI;
        giIn.gbuffer_normal_rg = gbufferNormalGI;
        giIn.view_matrix = view_matrix_;
        giIn.proj_matrix = proj_matrix_;
        giIn.current_buffer_index = cbIdx;
        giOut = gi_gather_module_->AddPasses(graph, giIn);
    }

    // ========================================================================
    // Step 10: Fusion Composite
    // ========================================================================

    FusionOutputs fusionOut;
    if (fusion_module_ && settings_.quality.enable_ssgi) {
        FusionInputs fusionIn;
        fusionIn.primary_input_rg = deferredOut.deferred_output_rg;
        fusionIn.primary_input_tex = deferredOut.deferred_output_tex;
        fusionIn.ssgi_rg = ssgiOut.ssgi_output;
        fusionIn.ssgi_tex = handles::INVALID_RESOURCE; // RG-resolved inside module
        fusionIn.ddgi_rg = giOut.gi_output_rg;
        fusionIn.ddgi_tex = giOut.gi_output_tex;
        fusionIn.spgi_rg = spgiOut.gi_output;
        fusionIn.spgi_tex = handles::INVALID_RESOURCE; // RG-resolved inside module
        fusionIn.gbuffer_albedo = gpuDraw.GetGBufferAlbedo();
        fusionIn.ssao_rg = ssaoOut.ssao_output;
        fusionIn.ssao_tex = ssao_pass_ ? ssao_pass_->GetFilterTexture() : black_texture_;
        fusionIn.volume_scatter_rg = volumeOut.volume_scatter;
        fusionIn.volume_scatter_tex = handles::INVALID_RESOURCE; // RG-resolved inside module
        fusionIn.current_buffer_index = cbIdx;
        fusionIn.render_width = render_width_;
        fusionIn.render_height = render_height_;
        fusionIn.black_texture = black_texture_;
        fusionOut = fusion_module_->AddPasses(graph, fusionIn);
    }

    // ========================================================================
    // Step 11: Final Blit → BackBuffer
    // ========================================================================

    if (final_blit_module_) {
        FinalBlitInputs blitIn;
        if (fusionOut.output_tex != handles::INVALID_RESOURCE) {
            blitIn.input_rg = fusionOut.output_rg;
            blitIn.input_tex = fusionOut.output_tex;
        } else {
            blitIn.input_rg = deferredOut.deferred_output_rg;
            blitIn.input_tex = deferredOut.deferred_output_tex;
        }
        blitIn.backbuffer_rg = graph.ImportResource("BackBuffer", backBuffer);
        blitIn.current_buffer_index = cbIdx;
        blitIn.render_width = target_width_ > 0 ? target_width_ : render_width_;
        blitIn.render_height = target_height_ > 0 ? target_height_ : render_height_;
        final_blit_module_->AddPass(graph, blitIn);
    }
}

// ============================================================================
// RenderWithCommandBuffer (uses caller's pre-allocated CB — matches working pattern)
// ============================================================================

void StandardRenderPipeline::RenderWithCommandBuffer(
    RenderScene& scene, RenderView& view,
    ResourceHandle target, const TextureDesc& targetDesc,
    RHICommandBuffer* cmd, u32 bufferIndex,
    CommandBufferHandle cmdHandle,
    SyncHandle signalFence)
{
    current_scene_ = &scene;
    auto startTime = std::chrono::high_resolution_clock::now();
    if (!device_ || !cmd || !renderGraph_) return;

    ApplyConfigChanges();

    u32 cbIdx = bufferIndex;

    if (targetDesc.size.x > 0 && targetDesc.size.y > 0) {
        target_width_ = targetDesc.size.x;
        target_height_ = targetDesc.size.y;
    }

    UpdatePerFrame(scene, view);

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
                                   target, cbIdx % 3);

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
    }

    // After draw: Shadow + Deferred + FinalBlit via RenderGraph
    if (deferred_module_ && final_blit_module_) {
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
            shadowIn.current_buffer_index = cbIdx;
            shadowIn.shadow_quality = settings_.quality.shadow_quality;
            shadowOut = shadow_module_->AddPasses(graph, shadowIn);
        }

        // Deferred Lighting — lightPos = direction TO light = -lightDir
        DeferredLightingInputs deferredIn;
        deferredIn.gpu_draw_pipeline = &gpuDraw;
        deferredIn.ssao_pass = nullptr;
        deferredIn.view_matrix = view_matrix_;
        deferredIn.proj_matrix = proj_matrix_;
        deferredIn.camera_position = camera_position_;
        deferredIn.light_pos = math::v4{-lightDir.x, -lightDir.y, -lightDir.z, 0.0f};
        deferredIn.light_color = settings_.lighting.light_color;
        deferredIn.cascade_splits = settings_.lighting.cascade_splits;
        deferredIn.shadow_matrix0 = shadowOut.shadow_matrix0;
        deferredIn.shadow_matrix1 = shadowOut.shadow_matrix1;
        deferredIn.current_buffer_index = cbIdx;
        deferredIn.shadow_visibility_rg = shadowOut.shadow_visibility_rg;
        deferredIn.shadow_visibility_tex = shadowOut.shadow_visibility_tex;
        deferredIn.gbuffer_albedo_rg = gbufferAlbedoDL;
        deferredIn.gbuffer_normal_rg = gbufferNormalDL;
        deferredIn.gbuffer_orm_rg = gbufferORMDL;
        deferredIn.gbuffer_depth_rg = gbufferDepthDL;
        auto deferredOut = deferred_module_->AddPasses(graph, deferredIn);

        // --- SSAO ---
        lumen::LumenSSAOOutput ssaoOut{};
        if (ssao_pass_ && ssao_pass_->IsInitialized()) {
            auto gbufferNormalSSAO = graph.ImportResource("GBufferNormal_SSAO", gpuDraw.GetGBufferNormal());
            auto gbufferDepthSSAO = graph.ImportResource("GBufferDepth_SSAO", gpuDraw.GetGBufferDepthSampleable());
            lumen::SSAOCameraData camData{};
            camData.view_matrix = view_matrix_;
            camData.proj_matrix = proj_matrix_;
            camData.prev_view_matrix = view_matrix_;
            camData.prev_proj_matrix = proj_matrix_;
            camData.frame_index = static_cast<u32>(frameCount_);
            camData.delta_time = settings_.advanced.delta_time_override;
            ssaoOut = ssao_pass_->AddPass(graph, gbufferNormalSSAO, gbufferDepthSSAO, camData, static_cast<u32>(frameCount_));
        }

        // --- Surface Cache ---
        RGResourceHandle scLightingRG;
        if (surface_cache_pass_ && surface_cache_pass_->IsInitialized()) {
            lumen::SurfaceCacheFrameData frameData{};
            frameData.camera_position = {camera_position_.x, camera_position_.y, camera_position_.z};
            frameData.frame_index = static_cast<u32>(frameCount_);
            frameData.light_count = 1;
            ResourceHandle nullLightBuf{handles::INVALID_RESOURCE};
            surface_cache_pass_->AddPass(graph, RGResourceHandle{}, nullLightBuf, frameData, static_cast<u32>(frameCount_));
            scLightingRG = graph.ImportResource("SCLightingAtlas", surface_cache_pass_->GetLightingAtlas(static_cast<u32>(frameCount_)));
            if (ddgi_pass_ && ddgi_pass_->IsInitialized()) {
                ddgi_pass_->SetSurfaceCacheResources(
                    surface_cache_pass_->GetLightingAtlas(static_cast<u32>(frameCount_)),
                    surface_cache_pass_->GetCardLookupBuffer(),
                    surface_cache_pass_->GetCardDataBuffer(),
                    settings_.lumen.surface_cache_atlas_size,
                    settings_.lumen.surface_cache_max_cards);
            }
            if (screen_probe_pass_ && screen_probe_pass_->IsInitialized()) {
                screen_probe_pass_->SetSurfaceCacheData(
                    surface_cache_pass_->GetLightingAtlas(static_cast<u32>(frameCount_)),
                    surface_cache_pass_->GetCardDataBuffer(),
                    surface_cache_pass_->GetCardLookupBuffer(),
                    settings_.lumen.surface_cache_atlas_size,
                    settings_.lumen.surface_cache_max_cards);
            }
        }

        // --- DDGI ---
        lumen::LumenDDGIOutput ddgiOut;
        if (ddgi_pass_ && ddgi_pass_->IsInitialized()) {
            lumen::DDGICameraData camData{};
            camData.camera_position = camera_position_;
            camData.view_matrix = view_matrix_;
            camData.proj_matrix = proj_matrix_;
            camData.prev_view_matrix = view_matrix_;
            camData.prev_proj_matrix = proj_matrix_;
            camData.light_direction = Normalize(settings_.lighting.light_direction);
            camData.light_color = {settings_.lighting.ddgi_light_color.x, settings_.lighting.ddgi_light_color.y, settings_.lighting.ddgi_light_color.z};
            camData.frame_index = static_cast<u32>(frameCount_);
            camData.delta_time = settings_.advanced.delta_time_override;
            RGResourceHandle prevColor;
            if (deferredOut.deferred_output_tex != handles::INVALID_RESOURCE)
                prevColor = graph.ImportResource("PrevFrameColor", deferredOut.deferred_output_tex);
            ddgiOut = ddgi_pass_->AddPass(graph, prevColor, camData, static_cast<u32>(frameCount_), scLightingRG);
        }

        // --- SC-DDGI Integration ---
        if (sc_ddgi_module_ && surface_cache_pass_ && ddgi_pass_) {
            SCDDGIInputs scDDGIIn;
            scDDGIIn.ddgi_pass = ddgi_pass_.get();
            scDDGIIn.surface_cache_pass = surface_cache_pass_.get();
            scDDGIIn.current_buffer_index = cbIdx;
            scDDGIIn.sc_lighting_rg = scLightingRG;
            sc_ddgi_module_->AddPass(graph, scDDGIIn);
        }

        // --- GIGather ---
        GIGatherOutputs giOut;
        if (gi_gather_module_ && ddgi_pass_ && ddgi_pass_->IsInitialized()) {
            auto gbufferDepthGI = graph.ImportResource("GBufferDepthGI", gpuDraw.GetGBufferDepthSampleable());
            auto gbufferNormalGI = graph.ImportResource("GBufferNormalGI", gpuDraw.GetGBufferNormal());
            GIGatherInputs giIn;
            giIn.ddgi_pass = ddgi_pass_.get();
            giIn.static_probe_volume = static_probe_volume_.get();
            giIn.gbuffer_depth = gpuDraw.GetGBufferDepthSampleable();
            giIn.gbuffer_normal = gpuDraw.GetGBufferNormal();
            giIn.gbuffer_depth_rg = gbufferDepthGI;
            giIn.gbuffer_normal_rg = gbufferNormalGI;
            giIn.view_matrix = view_matrix_;
            giIn.proj_matrix = proj_matrix_;
            giIn.current_buffer_index = cbIdx;
            giOut = gi_gather_module_->AddPasses(graph, giIn);
        }

        // --- SSGI (Screen Space GI) ---
        lumen::LumenSSGIOutput ssgiOut;
        if (ssgi_pass_ && ssgi_pass_->IsInitialized() && frameCount_ > 0) {
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
            ssgiCam.prev_view_matrix = view_matrix_;
            ssgiCam.prev_proj_matrix = proj_matrix_;
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
            auto spDepth = graph.ImportResource("GBufferDepth_SP", gpuDraw.GetGBufferDepthSampleable());
            auto spNormal = graph.ImportResource("GBufferNormal_SP", gpuDraw.GetGBufferNormal());

            RGResourceHandle spRadiance = deferredOut.deferred_output_rg.IsValid()
                ? deferredOut.deferred_output_rg
                : graph.ImportResource("SPBlackFallback", black_texture_);

            lumen::ScreenProbeCameraData spCam{};
            spCam.view_matrix = view_matrix_;
            spCam.proj_matrix = proj_matrix_;
            spCam.camera_position = camera_position_;
            spCam.frame_index = static_cast<u32>(frameCount_);

            spgiOut = screen_probe_pass_->AddPass(graph, spDepth, spNormal,
                spRadiance, spCam, static_cast<u32>(frameCount_));
        }

        // --- Volume (Froxel Fog or Ray March) ---
        volume::VolumeOutput volumeOut;
        if (froxel_fog_pass_ && froxel_fog_pass_->IsInitialized()) {
            volume::FroxelInputs froxelIn;
            froxelIn.gbuffer_depth = graph.ImportResource("GBufferDepth_Volume", gpuDraw.GetGBufferDepthSampleable());
            froxelIn.scene_color = deferredOut.deferred_output_rg;
            froxelIn.shadow_map = shadowOut.shadow_visibility_rg;
            froxelIn.shadow_matrix0 = shadowOut.shadow_matrix0;
            froxelIn.shadow_matrix1 = shadowOut.shadow_matrix1;

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
        if (fusion_module_ && settings_.quality.enable_ssgi) {
            FusionInputs fusionIn;
            fusionIn.primary_input_rg = deferredOut.deferred_output_rg;
            fusionIn.primary_input_tex = deferredOut.deferred_output_tex;
            fusionIn.ssgi_rg = ssgiOut.ssgi_output;
            fusionIn.ssgi_tex = handles::INVALID_RESOURCE;
            fusionIn.ddgi_rg = giOut.gi_output_rg;
            fusionIn.ddgi_tex = giOut.gi_output_tex;
            fusionIn.spgi_rg = spgiOut.gi_output;
            fusionIn.spgi_tex = handles::INVALID_RESOURCE;
            fusionIn.gbuffer_albedo = gpuDraw.GetGBufferAlbedo();
            fusionIn.ssao_rg = ssaoOut.ssao_output;
            fusionIn.ssao_tex = ssao_pass_ ? ssao_pass_->GetFilterTexture() : black_texture_;
            fusionIn.current_buffer_index = cbIdx;
            fusionIn.render_width = render_width_;
            fusionIn.render_height = render_height_;
            fusionIn.black_texture = black_texture_;
            fusionIn.volume_scatter_rg = volume_scatter_rg.IsValid()
                ? volume_scatter_rg : volumeOut.volume_scatter;
            fusionOut = fusion_module_->AddPasses(graph, fusionIn);
        }

        // --- Final Blit → backbuffer ---
        FinalBlitInputs blitIn;
        if (fusionOut.output_tex != handles::INVALID_RESOURCE) {
            blitIn.input_rg = fusionOut.output_rg;
            blitIn.input_tex = fusionOut.output_tex;
        } else {
            blitIn.input_rg = deferredOut.deferred_output_rg;
            blitIn.input_tex = deferredOut.deferred_output_tex;
        }
        blitIn.backbuffer_rg = graph.ImportResource("BackBuffer", target);
        blitIn.current_buffer_index = cbIdx;
        blitIn.render_width = target_width_ > 0 ? target_width_ : render_width_;
        blitIn.render_height = target_height_ > 0 ? target_height_ : render_height_;
        final_blit_module_->AddPass(graph, blitIn);

        graph.Compile();
        graph.Execute(cmd);
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
            imgInfo.imageView = gpuDraw.GetGBufferAlbedo();
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
    current_scene_ = &scene;
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
                                   target, cbIdx % 3);

        // PCG SDF: dispatch voxelization + issue readback after scene render
        auto& gsdf = nanite::GlobalSDF::Get();
        static int s_guard_checks = 0;
        if (s_guard_checks < 3) {
            std::cout << "[StdPipeline] DispatchVoxelization guard #" << s_guard_checks
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
        depth_history_->StoreCurrentFrameDepth(gpuDraw.GetGBufferDepthSampleable(), cmd, static_cast<u32>(frameCount_));
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
