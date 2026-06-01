#include "StandardRenderPipeline.h"
#include "Graphics/RenderScene.h"
#include "Graphics/RenderView.h"
#include "Graphics/Nanite/GPUDrivenDrawPipeline.h"
#include "Graphics/Nanite/GPUCullingPipeline.h"
#include "Graphics/Nanite/NaniteStreamingManager.h"
#include "Graphics/Nanite/NaniteResourceManager.h"
#include "Graphics/Nanite/DepthHistoryManager.h"
#include "Graphics/Nanite/ColorHistoryManager.h"
#include "Graphics/Nanite/HZBSystem.h"
#include "Graphics/Nanite/GlobalSDF.h"
#include "Graphics/Scene/RenderSceneSnapshot.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"
#include <iostream>
#include <chrono>

namespace primal::graphics {

using namespace rhi;
using namespace rhi::math;
using namespace rendergraph;

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

void StandardRenderPipeline::SetLumenConfig(const lumen::LumenConfig& config) {
    lumen_config_ = config;
    quality_config_ = PipelineQualityConfig::FromPreset(config.quality);

    // Apply render scale: GPU resolution = logical window size × scale
    render_width_  = static_cast<u32>(logical_width_  * quality_config_.render_scale);
    render_height_ = static_cast<u32>(logical_height_ * quality_config_.render_scale);

    std::cout << "[Pipeline] Render scale=" << quality_config_.render_scale
              << " logical=" << logical_width_ << "x" << logical_height_
              << " render=" << render_width_ << "x" << render_height_ << std::endl;

    if (device_ && !subsystems_initialized_) {
        InitializeSubsystems();
        subsystems_initialized_ = true;
    }
}

void StandardRenderPipeline::SetQualityOverride(bool enable_screen_probes, bool enable_surface_cache) {
    quality_config_.enable_screen_probes = enable_screen_probes;
    quality_config_.enable_surface_cache = enable_surface_cache;
    // Re-initialize Lumen passes with updated flags
    if (subsystems_initialized_) {
        ShutdownLumenPasses();
        InitializeLumenPasses();
    }
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

    if (quality_config_.enable_ddgi && gi_gather_shader_ != handles::INVALID_SHADER) {
        gi_gather_module_ = std::make_unique<GIGatherModule>();
        gi_gather_module_->Initialize(device_, gi_gather_shader_, render_width_, render_height_);
    }

    if (quality_config_.enable_ssgi || quality_config_.enable_ddgi) {
        fusion_module_ = std::make_unique<FusionCompositeModule>();
        fusion_module_->Initialize(device_, fusion_indirect_ps_, blit_vs_,
                                   fusion_composite_ps_, render_width_, render_height_);
    }

    if (quality_config_.enable_surface_cache && quality_config_.enable_ddgi) {
        sc_ddgi_module_ = std::make_unique<SCDDGIIntegrationModule>();
        sc_ddgi_module_->Initialize(device_, sc_card_radiance_shader_,
                                    sc_probe_irradiance_shader_, render_width_, render_height_);
    }

    // --- Lumen passes ---
    InitializeLumenPasses();
}

void StandardRenderPipeline::ShutdownSubsystems() {
    if (sc_ddgi_module_) { sc_ddgi_module_->Shutdown(); sc_ddgi_module_.reset(); }
    if (fusion_module_) { fusion_module_->Shutdown(); fusion_module_.reset(); }
    if (gi_gather_module_) { gi_gather_module_->Shutdown(); gi_gather_module_.reset(); }
    if (final_blit_module_) { final_blit_module_->Shutdown(); final_blit_module_.reset(); }
    if (deferred_module_) { deferred_module_->Shutdown(); deferred_module_.reset(); }
    if (shadow_module_) { shadow_module_->Shutdown(); shadow_module_.reset(); }

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
    const auto& config = lumen_config_;

    // DDGI
    if (quality_config_.enable_ddgi) {
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
    if (quality_config_.enable_ssao) {
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
    if (quality_config_.enable_ssgi) {
        ssgi_pass_ = std::make_unique<lumen::LumenSSGIPass>();
        lumen::SSGIParams ssgiParams{};
        if (!ssgi_pass_->Initialize(device_, render_width_, render_height_, ssgiParams)) {
            std::cerr << "[Lumen] SSGI init failed" << std::endl;
            ssgi_pass_.reset();
        }
    }

    // Surface Cache
    if (quality_config_.enable_surface_cache) {
        surface_cache_pass_ = std::make_unique<lumen::SurfaceCachePass>();
        if (!surface_cache_pass_->Initialize(device_, config)) {
            std::cerr << "[Lumen] Surface Cache init failed" << std::endl;
            surface_cache_pass_.reset();
        }
    }

    // Screen Probes
    if (quality_config_.enable_screen_probes) {
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
}

void StandardRenderPipeline::ShutdownLumenPasses() {
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

    // Update GlobalSDF cascade origins
    auto& globalSDF = nanite::GlobalSDF::Get();
    if (globalSDF.IsInitialized() && scene_snapshot_) {
        globalSDF.Update(*scene_snapshot_, frameCount_, camera_position_);
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
    math::v3 lightDir = Normalize(math::v3{0.707f, -1.0f, 0.408f});
    if (shadow_module_) {
        ShadowMapInputs shadowIn;
        shadowIn.gpu_draw_pipeline = &gpuDraw;
        shadowIn.scene_snapshot = scene_snapshot_.get();
        shadowIn.camera_position = camera_position_;
        shadowIn.view_matrix = view_matrix_;
        shadowIn.proj_matrix = proj_matrix_;
        shadowIn.light_direction = lightDir;
        shadowIn.current_buffer_index = cbIdx;
        shadowIn.shadow_quality = quality_config_.shadow_quality;
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
        camData.delta_time = 0.016f;
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
        deferredIn.light_color = light_color_;
        deferredIn.cascade_splits = {600.0f, 2000.0f, 0.0f, 0.0f};
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
                lumen_config_.surface_cache_atlas_size,
                lumen_config_.surface_cache_max_cards);
        }

        if (ddgi_pass_ && ddgi_pass_->IsInitialized()) {
            ddgi_pass_->SetSurfaceCacheResources(
                surface_cache_pass_->GetLightingAtlas(static_cast<u32>(frameCount_)),
                surface_cache_pass_->GetCardLookupBuffer(),
                surface_cache_pass_->GetCardDataBuffer(),
                lumen_config_.surface_cache_atlas_size,
                lumen_config_.surface_cache_max_cards);
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
        math::v3 lightFwd = Normalize(math::v3{0.707f, -1.0f, 0.408f});
        camData.light_direction = lightFwd;
        camData.light_color = {20.0f, 20.0f, 20.0f};
        camData.frame_index = static_cast<u32>(frameCount_);
        camData.delta_time = 0.016f;

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
    if (fusion_module_ && quality_config_.enable_ssgi) {
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
    auto startTime = std::chrono::high_resolution_clock::now();
    if (!device_ || !cmd || !renderGraph_) return;

    u32 cbIdx = bufferIndex;

    if (targetDesc.size.x > 0 && targetDesc.size.y > 0) {
        target_width_ = targetDesc.size.x;
        target_height_ = targetDesc.size.y;
    }

    UpdatePerFrame(scene, view);

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
        math::v3 lightDir = Normalize(math::v3{0.707f, -1.0f, 0.408f});

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
            shadowIn.shadow_quality = quality_config_.shadow_quality;
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
        deferredIn.light_color = light_color_;
        deferredIn.cascade_splits = {600.0f, 2000.0f, 0.0f, 0.0f};
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
            camData.delta_time = 0.016f;
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
                    lumen_config_.surface_cache_atlas_size,
                    lumen_config_.surface_cache_max_cards);
            }
            if (screen_probe_pass_ && screen_probe_pass_->IsInitialized()) {
                screen_probe_pass_->SetSurfaceCacheData(
                    surface_cache_pass_->GetLightingAtlas(static_cast<u32>(frameCount_)),
                    surface_cache_pass_->GetCardDataBuffer(),
                    surface_cache_pass_->GetCardLookupBuffer(),
                    lumen_config_.surface_cache_atlas_size,
                    lumen_config_.surface_cache_max_cards);
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
            camData.light_direction = Normalize(math::v3{0.707f, -1.0f, 0.408f});
            camData.light_color = {20.0f, 20.0f, 20.0f};
            camData.frame_index = static_cast<u32>(frameCount_);
            camData.delta_time = 0.016f;
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
            ssgiCam.delta_time = 0.016f;

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

        // --- FusionComposite ---
        FusionOutputs fusionOut;
        if (fusion_module_ && quality_config_.enable_ssgi) {
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

    auto endTime = std::chrono::high_resolution_clock::now();
    stats_.cpuFrameTimeMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();
}

// ============================================================================
// Render (legacy — creates/destroys CB per frame)
// ============================================================================

void StandardRenderPipeline::Render(RenderScene& scene, RenderView& view,
                                     ResourceHandle target, const TextureDesc& targetDesc,
                                     SyncHandle signalFence) {
    auto startTime = std::chrono::high_resolution_clock::now();
    if (!device_ || !renderGraph_) return;

    u32 cbIdx = static_cast<u32>(frameCount_ % 3);

    if (targetDesc.size.x > 0 && targetDesc.size.y > 0) {
        target_width_ = targetDesc.size.x;
        target_height_ = targetDesc.size.y;
    }

    UpdatePerFrame(scene, view);

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

    auto endTime = std::chrono::high_resolution_clock::now();
    stats_.cpuFrameTimeMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();
}

} // namespace primal::graphics
