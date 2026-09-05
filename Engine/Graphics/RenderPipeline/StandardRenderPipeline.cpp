#include "StandardRenderPipeline.h"
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
#include "Engine/Common/CoreLogStream.h"

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
    render_width_  = static_cast<u32>(logical_width_  * settings_.quality.render_scale);
    render_height_ = static_cast<u32>(logical_height_ * settings_.quality.render_scale);

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
    editor_mode_ = enabled;
    if (forward_renderer_) forward_renderer_->SetEditorMode(enabled);
}

void StandardRenderPipeline::InitializeSubsystems() {
#ifndef __EMSCRIPTEN__
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

    // SDF visualization module (toggled at runtime via SetSDFVisualization).
    // Reuses the fullscreen triangle vertex shader (blit_vs_).
    if (sdf_viz_ps_ != handles::INVALID_SHADER) {
        sdf_viz_module_ = std::make_unique<SDFVisualizationModule>();
        sdf_viz_module_->Initialize(device_, blit_vs_, sdf_viz_ps_);
    }

    if (settings_.quality.enable_ddgi && gi_gather_shader_ != handles::INVALID_SHADER) {
        gi_gather_module_ = std::make_unique<GIGatherModule>();
        gi_gather_module_->Initialize(device_, gi_gather_shader_, render_width_, render_height_);
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
    if (fusion_module_) { fusion_module_->Shutdown(); fusion_module_.reset(); }
    if (gi_gather_module_) { gi_gather_module_->Shutdown(); gi_gather_module_.reset(); }
    if (final_blit_module_) { final_blit_module_->Shutdown(); final_blit_module_.reset(); }
    if (sdf_viz_module_) { sdf_viz_module_->Shutdown(); sdf_viz_module_.reset(); }
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

// ============================================================================
// Lumen Pass Init / Shutdown
// ============================================================================

void StandardRenderPipeline::InitializeLumenPasses() {
    if (!device_) return;
    const auto& config = settings_.lumen;

    // T4.1: capture the base GI budgets before the governor starts scaling them.
    gi_base_probe_budget_ = config.ddgi_probe_count_x * config.ddgi_probe_count_y *
                            config.ddgi_probe_count_z;
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
            vols[0].probe_count_x = config.ddgi_probe_count_x;
            vols[0].probe_count_y = config.ddgi_probe_count_y;
            vols[0].probe_count_z = config.ddgi_probe_count_z;
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
            if (vi > 0 && vc.update_budget_divisor > 1)
                vp.max_probes_per_frame = 16384u / vc.update_budget_divisor;
            auto vol = std::make_unique<lumen::LumenDDGIPass>();
            if (!vol->Initialize(device_, vp)) {
                PRIMAL_LOG_STREAM(Error, "Render") << "[Lumen] DDGI volume " << vi << " init failed (skipping)" << std::endl;
                continue;
            }
            vol->SetDynamicMode(true);   // Mode 11: runtime SDF trace
            if (vi > 0) vol->SetRGNamePrefix(kPrefixes[vi]);
            ddgi_volumes_[ddgi_volume_count_++] = std::move(vol);
            PRIMAL_LOG_STREAM(Info, "Render") << "[Lumen] DDGI volume " << vi << " initialized ("
                      << vp.probe_count_x << "x" << vp.probe_count_y
                      << "x" << vp.probe_count_z << " @ spacing "
                      << vp.probe_spacing << ")" << std::endl;
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
        PRIMAL_LOG_STREAM(Error, "Render") << "[Lumen] Offline SDF re-applied to DDGI trace ("
                  << offline_sdf_resolution_ << "³)" << std::endl;
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
        // T8.13: re-apply the offline SDF to the SC shadow march after a
        // Lumen rebuild (surface_cache was just re-created).
        if (has_offline_sdf_) {
            surface_cache_pass_->SetOfflineSDFSource(offline_sdf_texture_,
                offline_sdf_origin_, offline_sdf_extent_, offline_sdf_resolution_);
        }
            surface_cache_pass_.reset();
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
            probeParams.rays_per_probe = std::max(8u, (u32)atoi(e));
        }
        probeParams.enable_jitter = config.screen_probes_jitter;   // F5 (M7)
        probeParams.far_cache_start_distance = config.screen_probes_far_cache_start;  // F1 (M7)
        probeParams.enable_importance = config.screen_probes_importance;  // F2 (M7)
        probeParams.adaptive_pos_threshold = config.screen_probes_adaptive_pos_error;  // F4 (M7)
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
        if (scene_snapshot_->NeedsFullRebuild()) {
            scene_snapshot_->Rebind(scene);
            scene_snapshot_->ClearFullRebuildFlag();

            // Phase 1 (Lumen architecture): register real mesh bounds with
            // the Surface Cache CardGenerator. Each scene instance gets its
            // own cards at appropriate resolution — replacing the 2 large
            // test AABBs from Initialize. This is the single biggest
            // improvement to SC atlas coverage and per-mesh detail.
            if (surface_cache_pass_ && surface_cache_pass_->IsInitialized()) {
                auto& cardGen = surface_cache_pass_->GetCardGenerator();
                const auto& instances = scene_snapshot_->GetInstanceData();
                for (u32 i = 0; i < instances.size(); ++i) {
                    const auto& inst = instances[i];
                    math::v3 aabbMin = inst.bounds_center - math::v3{inst.bounds_radius, inst.bounds_radius, inst.bounds_radius};
                    math::v3 aabbMax = inst.bounds_center + math::v3{inst.bounds_radius, inst.bounds_radius, inst.bounds_radius};
                    cardGen.RegisterMesh(aabbMin, aabbMax, i);
                }
                cardGen.RebuildCardAllocation();
                PRIMAL_LOG_STREAM(Info, "Render") << "[LumenSC] Registered " << instances.size()
                          << " scene instances with CardGenerator → "
                          << cardGen.GetCardCount() << " cards" << std::endl;
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

    u32 slot = forward_renderer_->RegisterMeshResource(
        geometry_content_id, albedo_id, normal_id, orm_id, eid);

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

                // A7: forward the editor view mode (0=shaded, 1=wireframe).
                gpuDraw.SetViewMode(view_mode_);

                auto cullingResults = data.culling_pipeline
                    ? data.culling_pipeline->GetResults()
                    : nanite::CullingResults{};

                gpuDraw.Execute(cmd, *data.scene_snapshot, data.view_matrix, data.proj_matrix,
                                cullingResults, static_cast<u32>(data.frame_count), data.buffer_index);

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
        // T8.13: shadow softness knobs (Hard=0 / PCF=1 / PCSS=2).
        if (const char* e = std::getenv("SHADOW_QUALITY")) {
            shadowIn.shadow_quality = (lumen::ShadowQuality)atoi(e);
        }
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
        deferredIn.ssao_pass = ssao_pass_.get();
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
        // VSM: blurred moments + mode flag (shader falls back to the R8
        // visibility path when vsm_enabled is false).
        deferredIn.vsm_enabled = shadowOut.vsm_enabled;
        deferredIn.shadow_moments_rg[0] = shadowOut.shadow_moments_rg[0];
        deferredIn.shadow_moments_rg[1] = shadowOut.shadow_moments_rg[1];
        deferredIn.shadow_moments_tex[0] = shadowOut.shadow_moments_tex[0];
        deferredIn.shadow_moments_tex[1] = shadowOut.shadow_moments_tex[1];
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
        // T3.2: real light from settings instead of the SC-internal hardcode
        math::v3 scLightDir = Normalize(settings_.lighting.light_direction);
        frameData.light_direction = scLightDir;
        // T3.2+: SC texels see the REAL sun (light_color). The old
        // ddgi_light_color (≈half intensity, tuned when the trace chain was
        // broken) made every card's direct term 2× dimmer than the main pass
        // — the whole GI pyramid inherited the deficit.
        frameData.light_color = {settings_.lighting.light_color.x,
                                 settings_.lighting.light_color.y,
                                 settings_.lighting.light_color.z};

        // GBuffer-gather capture: real rendered surfaces → card atlas.
        frameData.inv_view_projection = Inverse(proj_matrix_ * view_matrix_);
        frameData.render_width  = render_width_;
        frameData.render_height = render_height_;
        frameData.gbuffer_depth  = gpuDraw.GetGBufferDepthSampleable();
        frameData.gbuffer_albedo = gpuDraw.GetGBufferAlbedo();
        frameData.gbuffer_normal = gpuDraw.GetGBufferNormal();

        // Probe-fed indirect (Lumen hierarchy): previous-frame near-volume
        // irradiance — SC runs before the DDGI update, so (frame+2)%3 is the
        // last fully integrated result. Shadowed texels gather their bounce
        // from these probes instead of collapsing to black.
        if (ddgi_pass_ && ddgi_pass_->IsInitialized()) {
            const auto& vol = ddgi_pass_->GetVolumeData();
            frameData.ddgi_irradiance = ddgi_pass_->GetIrradianceBuffer(
                (static_cast<u32>(frameCount_) + 2) % 3);
            frameData.ddgi_origin = {vol.ProbeOrigin.x, vol.ProbeOrigin.y, vol.ProbeOrigin.z};
            frameData.ddgi_spacing = vol.ProbeSpacing;
            frameData.ddgi_counts[0] = vol.ProbeCounts[0];
            frameData.ddgi_counts[1] = vol.ProbeCounts[1];
            frameData.ddgi_counts[2] = vol.ProbeCounts[2];
            frameData.ddgi_valid = 1;
        }

        ResourceHandle nullLightBuf{handles::INVALID_RESOURCE};
        // T8.12: card-view capture (SC_CARDVIEW=1) — runs instead of the
        // screen-space gather inside AddPass.
        if (surface_cache_pass_->IsCardViewEnabled() && scene_snapshot_) {
            surface_cache_pass_->AddCardCapturePasses(graph, *scene_snapshot_,
                                                      static_cast<u32>(frameCount_));
        }
        surface_cache_pass_->AddPass(graph, RGResourceHandle{}, nullLightBuf,
                                     frameData, static_cast<u32>(frameCount_));

        scLightingRG = graph.ImportResource("SCLightingAtlas",
            surface_cache_pass_->GetLightingAtlas(static_cast<u32>(frameCount_)));

        if (screen_probe_pass_ && screen_probe_pass_->IsInitialized()) {
            // X2/A3: SC 数据走统一 Cache View（atlas_size/cardCount 与旧
            // settings 传值同源，上传逐字节等价）
            screen_probe_pass_->SetSurfaceCacheData(
                surface_cache_pass_->GetCacheView(static_cast<u32>(frameCount_)));
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
        auto depthSSR = graph.ImportResource("GBufferDepth_SSR", gpuDraw.GetGBufferDepthSampleable());
        auto hzbSSR   = graph.ImportResource("HZBTexture_SSR", hzb_system_->GetHZBTexture());
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
        // T2.2: DDGI probe fallback for miss rays (hist slot — one frame
        // behind, no read/write race with this frame's DDGI update).
        if (ddgi_pass_ && ddgi_pass_->IsInitialized()) {
            if (settings_.lumen.axes.fallback_spgi_to_ddgi) {
                // X2/A3: volume 数据走统一 Cache View（ddgi_pass_ 别名 ddgi_volumes_[0]）
                screen_probe_pass_->SetDDGIFallbackData(
                    ddgi_pass_->GetVolumeView(static_cast<u32>(frameCount_ + 2) % 3));
            } else {
                // X3/A6 轴4: fallback 边关断 → Clear 语义（空 view 置
                // ddgi_fallback_available_=false，miss 回落天空常量）
                screen_probe_pass_->SetDDGIFallbackData(lumen::DDGIVolumeView{});
            }
        }
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
        spCam.prev_view_matrix = prev_view_matrix_;
        spCam.prev_proj_matrix = prev_proj_matrix_;
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
    }

    // ========================================================================
    // Step 9: GI Gather (DDGI → half-res screen texture)
    // ========================================================================

    GIGatherOutputs giOut;
    if (gi_gather_module_ && ddgi_pass_ && ddgi_pass_->IsInitialized() && lumen::GIDebug().gi_gather) {
        // Import GBuffer under UNIQUE names for GIGather — same physical textures,
        // different RG identities.
        auto gbufferDepthGI = graph.ImportResource("GBufferDepthGI", gpuDraw.GetGBufferDepthSampleable());
        auto gbufferNormalGI = graph.ImportResource("GBufferNormalGI", gpuDraw.GetGBufferNormal());
        GIGatherInputs giIn;
        giIn.ddgi_pass = ddgi_pass_.get();
        giIn.ddgi_far_pass = (ddgi_volume_count_ > 1) ? ddgi_volumes_[1].get() : nullptr;
        // X2/A3: 远场 volume 走 Cache View 直传（槽位与模块内 ddgiReadIdx 一致）
        giIn.far_volume_view = (ddgi_volume_count_ > 1 && ddgi_volumes_[1])
            ? ddgi_volumes_[1]->GetVolumeView(static_cast<u32>(frameCount_) % 3) : lumen::DDGIVolumeView{};
        // T6.3 sub-B: 全 volume 视图直传（模块内按 descriptor indexing 能力位
        // 门控 N-volume 路径；能力不可用/有效 volume < 2 时零行为变化）。
        giIn.volume_count = ddgi_volume_count_;
        for (u32 vi = 0; vi < ddgi_volume_count_ && vi < GIGatherInputs::kMaxVolumes; ++vi)
            if (ddgi_volumes_[vi]) giIn.volume_views[vi] = ddgi_volumes_[vi]->GetVolumeView(static_cast<u32>(frameCount_) % 3);
        giIn.static_probe_volume = static_probe_volume_.get();
        giIn.gbuffer_depth = gpuDraw.GetGBufferDepthSampleable();
        giIn.gbuffer_normal = gpuDraw.GetGBufferNormal();
        giIn.gbuffer_orm = gpuDraw.GetGBufferORM();
        giIn.gbuffer_depth_rg = gbufferDepthGI;
        giIn.gbuffer_normal_rg = gbufferNormalGI;
        auto gbufferOrmGI = graph.ImportResource("GBufferOrmGI", gpuDraw.GetGBufferORM());
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
        fusionIn.current_buffer_index = cbIdx;
        fusionIn.render_width = render_width_;
        fusionIn.render_height = render_height_;
        fusionIn.black_texture = black_texture_;
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

        // BLOOM_OFF=1 诊断开关：跳过 bright/blur，ToneMapping 走 dummy 黑纹理。
        const bool bloomOff = std::getenv("BLOOM_OFF") != nullptr;
        auto bloomOut = bloomOff
            ? PostProcess::BloomPassData{}
            : PostProcess::AddBloomPass(graph, hdrClean, cbIdx);

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
                cbIdx);
    }

    // ========================================================================
    // Step 10.6: Toon post process (optional, both platforms)
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

    if (sdf_visualization_enabled_ && sdf_viz_module_ && sdf_viz_module_->IsInitialized()) {
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
        blitIn.backbuffer_rg = graph.ImportResource("BackBuffer", backBuffer);
        blitIn.current_buffer_index = cbIdx;
        blitIn.render_width = target_width_ > 0 ? target_width_ : render_width_;
        blitIn.render_height = target_height_ > 0 ? target_height_ : render_height_;
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
            psIn.irradiance_buffer = ddgi_pass_->GetIrradianceBuffer(cbIdx);
            // B1 L3 遮挡：GBuffer 深度导入（唯一名——物理纹理与 Nanite 写入
            // 的同一张，RG 不感知复用；Metal 编码器边界同步处理转移）。
            auto& gpuDrawForDepth = nanite::GPUDrivenDrawPipeline::Get();
            psIn.depth_rg = graph.ImportResource("GBufferDepth_ProbeSpheres",
                                                 gpuDrawForDepth.GetGBufferDepthSampleable());
            psIn.depth_width = render_width_;
            psIn.depth_height = render_height_;
            probe_sphere_module_->AddPass(graph, psIn);
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
    static u32 frames = 0;

    for (const auto& [name, ms] : graph.GetPassExecutionTimes()) {
        sumMs[name] += ms;
        cnt[name] += 1;
    }
    if (++frames < 120) return;

    std::vector<std::pair<std::string, double>> rows;
    double total = 0.0;
    for (const auto& [n, s] : sumMs) {
        rows.push_back({n, s / frames});
        total += s / frames;
    }
    std::sort(rows.begin(), rows.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    PRIMAL_LOG_STREAM(Error, "Render") << "[RG-Perf] === GPU ms/frame over last " << frames << " frames ===" << std::endl;
    u32 shown = 0;
    for (const auto& [n, avg] : rows) {
        if (shown++ >= 24) break;
        PRIMAL_LOG_STREAM(Error, "Render") << "  " << avg << "  " << n << std::endl;
    }
    PRIMAL_LOG_STREAM(Error, "Render") << "[RG-Perf] TOTAL(sum of passes) = " << total << " ms/frame" << std::endl;

    sumMs.clear();
    cnt.clear();
    frames = 0;
}

void StandardRenderPipeline::RenderWithCommandBuffer(
    RenderScene& scene, RenderView& view,
    ResourceHandle target, const TextureDesc& targetDesc,
    RHICommandBuffer* cmd, u32 bufferIndex,
    CommandBufferHandle cmdHandle,
    SyncHandle signalFence)
{
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
        hzb_system_->BuildHZB(gpuDraw.GetGBufferDepthSampleable(), cmd);
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

    // After draw: Shadow + Deferred + FinalBlit via RenderGraph.
    // T4.6.5 part 39: GBuffer path requires Stage3 to have populated the
    // GBuffer RTs. In VisibilityBufferOnly mode, skip the entire block and
    // fall through to the resolve-output blit instead.
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
            shadowIn.current_buffer_index = cbIdx;
            shadowIn.shadow_quality = settings_.quality.shadow_quality;
        // T8.13: shadow softness knobs (Hard=0 / PCF=1 / PCSS=2).
        if (const char* e = std::getenv("SHADOW_QUALITY")) {
            shadowIn.shadow_quality = (lumen::ShadowQuality)atoi(e);
        }
            shadowOut = shadow_module_->AddPasses(graph, shadowIn);
        }

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
        deferredIn.shadow_matrix0 = shadowOut.shadow_matrix0;
        deferredIn.shadow_matrix1 = shadowOut.shadow_matrix1;
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
            // T3.2: real light from settings instead of the SC-internal hardcode
            math::v3 scLightDir2 = Normalize(settings_.lighting.light_direction);
            frameData.light_direction = scLightDir2;
            frameData.light_color = {settings_.lighting.ddgi_light_color.x,
                                     settings_.lighting.ddgi_light_color.y,
                                     settings_.lighting.ddgi_light_color.z};
            frameData.inv_view_projection = Inverse(proj_matrix_ * view_matrix_);
            frameData.render_width  = render_width_;
            frameData.render_height = render_height_;
            frameData.gbuffer_depth  = gpuDraw.GetGBufferDepthSampleable();
            frameData.gbuffer_albedo = gpuDraw.GetGBufferAlbedo();
            frameData.gbuffer_normal = gpuDraw.GetGBufferNormal();
            // Probe-fed indirect — see the BuildRenderGraph twin site.
            if (ddgi_pass_ && ddgi_pass_->IsInitialized()) {
                const auto& vol = ddgi_pass_->GetVolumeData();
                frameData.ddgi_irradiance = ddgi_pass_->GetIrradianceBuffer(
                    (static_cast<u32>(frameCount_) + 2) % 3);
                frameData.ddgi_origin = {vol.ProbeOrigin.x, vol.ProbeOrigin.y, vol.ProbeOrigin.z};
                frameData.ddgi_spacing = vol.ProbeSpacing;
                frameData.ddgi_counts[0] = vol.ProbeCounts[0];
                frameData.ddgi_counts[1] = vol.ProbeCounts[1];
                frameData.ddgi_counts[2] = vol.ProbeCounts[2];
                frameData.ddgi_valid = 1;
            }
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
            surface_cache_pass_->AddPass(graph, RGResourceHandle{}, nullLightBuf, frameData, static_cast<u32>(frameCount_));
            scLightingRG = graph.ImportResource("SCLightingAtlas", surface_cache_pass_->GetLightingAtlas(static_cast<u32>(frameCount_)));
            if (ddgi_pass_ && ddgi_pass_->IsInitialized()) {
                // X2/A3: SC→DDGI trace 注入走统一 Cache View
                ddgi_pass_->SetSurfaceCacheResources(
                    surface_cache_pass_->GetCacheView(static_cast<u32>(frameCount_)));
            }
            if (screen_probe_pass_ && screen_probe_pass_->IsInitialized()) {
                // X2/A3: SC 数据走统一 Cache View（atlas_size/cardCount 与旧
                // settings 传值同源，上传逐字节等价）
                screen_probe_pass_->SetSurfaceCacheData(
                    surface_cache_pass_->GetCacheView(static_cast<u32>(frameCount_)));
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
                ? ddgi_volumes_[1]->GetVolumeView(static_cast<u32>(frameCount_) % 3) : lumen::DDGIVolumeView{};
            // T6.3 sub-B: 全 volume 视图直传（同 Render 路径；能力门控在模块内）。
            giIn.volume_count = ddgi_volume_count_;
            for (u32 vi = 0; vi < ddgi_volume_count_ && vi < GIGatherInputs::kMaxVolumes; ++vi)
                if (ddgi_volumes_[vi]) giIn.volume_views[vi] = ddgi_volumes_[vi]->GetVolumeView(static_cast<u32>(frameCount_) % 3);
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
            // T2.2: DDGI probe fallback for miss rays
            if (ddgi_pass_ && ddgi_pass_->IsInitialized()) {
                if (settings_.lumen.axes.fallback_spgi_to_ddgi) {
                    // X2/A3: volume 数据走统一 Cache View（ddgi_pass_ 别名 ddgi_volumes_[0]）
                    screen_probe_pass_->SetDDGIFallbackData(
                        ddgi_pass_->GetVolumeView(static_cast<u32>(frameCount_ + 2) % 3));
                } else {
                    // X3/A6 轴4: fallback 边关断 → Clear 语义（空 view 置
                    // ddgi_fallback_available_=false，miss 回落天空常量）
                    screen_probe_pass_->SetDDGIFallbackData(lumen::DDGIVolumeView{});
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
            fusionIn.current_buffer_index = cbIdx;
            fusionIn.render_width = render_width_;
            fusionIn.render_height = render_height_;
            fusionIn.black_texture = black_texture_;
            fusionIn.volume_scatter_rg = volume_scatter_rg.IsValid()
                ? volume_scatter_rg : volumeOut.volume_scatter;
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

            auto bloomRWCB = std::getenv("BLOOM_OFF")
                ? PostProcess::BloomPassData{}
                : PostProcess::AddBloomPass(graph, hdrCleanRWCB, cbIdx);
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
                    velRWCB, cbIdx);
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

        // --- Final Blit → backbuffer ---
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
        blitIn.backbuffer_rg = graph.ImportResource("BackBuffer", target);
        blitIn.current_buffer_index = cbIdx;
        blitIn.render_width = target_width_ > 0 ? target_width_ : render_width_;
        blitIn.render_height = target_height_ > 0 ? target_height_ : render_height_;
        final_blit_module_->AddPass(graph, blitIn);

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
                psIn.blit_output_rg = blitIn.backbuffer_rg;
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
                probe_sphere_module_->AddPass(graph, psIn);
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
        final_blit_module_->AddPass(graph, blitIn);

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
