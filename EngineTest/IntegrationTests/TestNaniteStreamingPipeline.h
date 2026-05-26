#pragma once

#include "RenderTestFramework.h"
#include "Engine/Graphics/RHI/Core/RHIDevice.h"
#include "Engine/Graphics/RHI/Systems/RenderSystem.h"
#include "Engine/Graphics/RHI/Components/RHICamera.h"
#include "Engine/Graphics/RenderGraph/RenderGraph.h"
#include "Engine/Graphics/Nanite/GPUCullingPipeline.h"
#include "Engine/Graphics/Nanite/GPUDrivenDrawPipeline.h"
#include "Engine/Graphics/Nanite/NaniteStreamingManager.h"
#include "Engine/Graphics/Nanite/ColorHistoryManager.h"
#include "Engine/Graphics/Nanite/NaniteResourceManager.h"
#include "Engine/Graphics/Nanite/HZBSystem.h"
#include "Engine/Graphics/Nanite/DepthHistoryManager.h"
#include "Engine/Graphics/Nanite/VisibilityBufferSystem.h"
#include "Engine/Graphics/Nanite/GPUMaterialRegistry.h"
#include "Engine/Graphics/Lumen/SSGI/LumenSSGIPass.h"
#include "Engine/Graphics/Lumen/SSAO/LumenSSAOPass.h"
#include "Engine/Graphics/Lumen/DDGI/LumenDDGIPass.h"
#include "Engine/Graphics/Lumen/StaticProbe/StaticProbeVolume.h"
#include "Engine/Graphics/Lumen/ScreenProbes/ScreenProbeGIPass.h"
#include "Engine/Graphics/Lumen/SurfaceCache/SurfaceCachePass.h"
#include "Engine/Graphics/Nanite/GlobalSDF.h"
#include "Engine/Graphics/Scene/RenderSceneSnapshot.h"
#include "Engine/Components/Cluster.h"
#include "Engine/Graphics/Scene/SceneExtractionSystem.h"
#include "Engine/Graphics/SceneDataAdapter.h"
#include "Engine/Graphics/RenderScene.h"
#include "Engine/Graphics/RenderView.h"
#include "Engine/Platform/Platform.h"
#include "Engine/Platform/Window.h"
#include "Engine/Content/ContentToEngine.h"
#include "Engine/Content/AsyncResourceLoader.h"
#include "Engine/JobSystem/JobSystem.h"
#include "ShaderCompilation.h"
#include <memory>
#include <vector>
#include <atomic>
#include <unordered_map>
#include <string>

class TestNaniteStreamingPipeline;

class Engine_Test : public primal::test::RenderTestRunner {
public:
    Engine_Test();
};

class TestNaniteStreamingPipeline : public primal::test::RenderTestCase {
public:
    TestNaniteStreamingPipeline() = default;
    ~TestNaniteStreamingPipeline() override;

    // RenderTestCase interface
    bool Initialize() override;
    void Run() override;
    void Shutdown() override;
    void Resize(u32 width, u32 height) override;

    // Test-specific methods
    void TestStreamingInitialization();
    void TestGPURequestGeneration();
    void TestLRUEviction();
    void TestResidencyBuffer();
    void TestEndToEndStreaming();
    void TestPerformance();
    void TestStaticProbeSerialization();
    void TestDDGIInitFromStatic();

private:
    static TestNaniteStreamingPipeline* instance;
    // Core rendering components
    primal::graphics::rhi::RHIDeviceBase* device_{ nullptr };
    std::unique_ptr<primal::graphics::rhi::RHIDeviceBase> device_ownership_;
    primal::platform::window window_;
    primal::graphics::rhi::ResourceHandle sceneDepthTexture_{ primal::graphics::rhi::handles::INVALID_RESOURCE };  // Actual rendered depth for HZB
    primal::graphics::RenderSystem renderSystem_;
    std::unique_ptr<primal::graphics::rendergraph::RenderGraph> renderGraph_;

    // Nanite streaming components
    primal::graphics::nanite::GPUCullingPipeline* cullingPipeline_{ nullptr };
    primal::graphics::nanite::GPUDrivenDrawPipeline* gpuDrawPipeline_{ nullptr };
    primal::graphics::nanite::NaniteStreamingManager* streamingManager_{ nullptr };
    primal::graphics::nanite::NaniteResourceManager* resourceManager_{ nullptr };
    std::unique_ptr<primal::graphics::SceneExtractionSystem> extractionSystem_;

    // GPU Material Registry
    std::unique_ptr<primal::graphics::nanite::GPUMaterialRegistry> gpuMaterialRegistry_;
    primal::jobsystem::JobHandle materialBuildJob_;
    bool useProceduralUV_{ false };

    // HZB and Visibility Buffer components
    std::unique_ptr<primal::graphics::nanite::HZBSystem> hzbSystem_;
    std::unique_ptr<primal::graphics::nanite::DepthHistoryManager> depthHistoryManager_;
    std::unique_ptr<primal::graphics::nanite::VisibilityBufferSystem> visibilityBufferSystem_;

    // Blit pipeline for final presentation
    primal::graphics::rhi::PipelineHandle blit_pipeline_{ primal::graphics::rhi::handles::INVALID_PIPELINE };
    primal::graphics::rhi::PipelineLayoutHandle blit_layout_{ primal::graphics::rhi::handles::INVALID_PIPELINE_LAYOUT };
    primal::graphics::rhi::DescriptorSetLayoutHandle blit_set_layout_{ primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    primal::graphics::rhi::DescriptorSetHandle blit_descriptor_set_[3]{
        primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET,
        primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET,
        primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET
    };

    // Composite blit pipeline (scene + SSGI)
    primal::graphics::rhi::PipelineHandle blit_composite_pipeline_{ primal::graphics::rhi::handles::INVALID_PIPELINE };
    primal::graphics::rhi::PipelineLayoutHandle blit_composite_layout_{ primal::graphics::rhi::handles::INVALID_PIPELINE_LAYOUT };
    primal::graphics::rhi::DescriptorSetLayoutHandle blit_composite_set_layout_{ primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    primal::graphics::rhi::DescriptorSetHandle blit_composite_descriptor_set_[3]{
        primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET,
        primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET,
        primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET
    };

    // Fusion pipeline: 2-pass fragment for Apple Silicon TBDR compatibility
    // Pass 1 (half-res): 5 reads — SSGI+DDGI+SPGI+albedo×ssao → indirect contribution
    // Pass 2 (full-res): 2 reads — scene + indirect → tonemapped output
    primal::graphics::rhi::PipelineHandle fusion_indirect_pipeline_{ primal::graphics::rhi::handles::INVALID_PIPELINE };
    primal::graphics::rhi::PipelineLayoutHandle fusion_indirect_layout_{ primal::graphics::rhi::handles::INVALID_PIPELINE_LAYOUT };
    primal::graphics::rhi::DescriptorSetLayoutHandle fusion_indirect_set_layout_{ primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    primal::graphics::rhi::DescriptorSetHandle fusion_indirect_descriptor_set_[3]{
        primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET,
        primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET,
        primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET
    };
    primal::graphics::rhi::ResourceHandle fusion_indirect_output_[3]{
        primal::graphics::rhi::handles::INVALID_RESOURCE,
        primal::graphics::rhi::handles::INVALID_RESOURCE,
        primal::graphics::rhi::handles::INVALID_RESOURCE
    };
    primal::graphics::rhi::PipelineHandle fusion_fragment_pipeline_{ primal::graphics::rhi::handles::INVALID_PIPELINE };
    primal::graphics::rhi::PipelineLayoutHandle fusion_fragment_layout_{ primal::graphics::rhi::handles::INVALID_PIPELINE_LAYOUT };
    primal::graphics::rhi::DescriptorSetLayoutHandle fusion_fragment_set_layout_{ primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    primal::graphics::rhi::DescriptorSetHandle fusion_fragment_descriptor_set_[3]{
        primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET,
        primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET,
        primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET
    };
    primal::graphics::rhi::ResourceHandle fusion_output_[3]{
        primal::graphics::rhi::handles::INVALID_RESOURCE,
        primal::graphics::rhi::handles::INVALID_RESOURCE,
        primal::graphics::rhi::handles::INVALID_RESOURCE
    };

    // === Lumen SSGI ===
    std::unique_ptr<primal::graphics::lumen::LumenSSGIPass> ssgiPass_;
    primal::graphics::rhi::ResourceHandle ssgi_black_texture_{ primal::graphics::rhi::handles::INVALID_RESOURCE };

    // === Lumen SSAO ===
    std::unique_ptr<primal::graphics::lumen::LumenSSAOPass> ssaoPass_;

    // === Lumen DDGI ===
    std::unique_ptr<primal::graphics::lumen::LumenDDGIPass> ddgiPass_;

    // === Lumen Screen Probe GI ===
    std::unique_ptr<primal::graphics::lumen::ScreenProbeGIPass> screenProbeGIPass_;

    // === Lumen Surface Cache ===
    std::unique_ptr<primal::graphics::lumen::SurfaceCachePass> surfaceCachePass_;

    // Surface Cache test pipelines (compute-only for Phase A verification)
    primal::graphics::rhi::PipelineHandle sc_fill_test_pipeline_{ primal::graphics::rhi::handles::INVALID_PIPELINE };
    primal::graphics::rhi::PipelineLayoutHandle sc_fill_test_layout_{ primal::graphics::rhi::handles::INVALID_PIPELINE_LAYOUT };
    primal::graphics::rhi::DescriptorSetLayoutHandle sc_fill_test_set_layout_{ primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    primal::graphics::rhi::DescriptorSetHandle sc_fill_test_descriptor_set_{ primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET };

    primal::graphics::rhi::PipelineHandle sc_light_cull_pipeline_{ primal::graphics::rhi::handles::INVALID_PIPELINE };
    primal::graphics::rhi::PipelineLayoutHandle sc_light_cull_layout_{ primal::graphics::rhi::handles::INVALID_PIPELINE_LAYOUT };
    primal::graphics::rhi::DescriptorSetLayoutHandle sc_light_cull_set_layout_{ primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    primal::graphics::rhi::DescriptorSetHandle sc_light_cull_descriptor_sets_[3]{
        primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET,
        primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET,
        primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET
    };

    primal::graphics::rhi::PipelineHandle sc_light_eval_pipeline_{ primal::graphics::rhi::handles::INVALID_PIPELINE };
    primal::graphics::rhi::PipelineLayoutHandle sc_light_eval_layout_{ primal::graphics::rhi::handles::INVALID_PIPELINE_LAYOUT };
    primal::graphics::rhi::DescriptorSetLayoutHandle sc_light_eval_set_layout_{ primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    primal::graphics::rhi::DescriptorSetHandle sc_light_eval_descriptor_sets_[3]{
        primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET,
        primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET,
        primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET
    };

    primal::graphics::rhi::ResourceHandle sc_light_info_cb_{ primal::graphics::rhi::handles::INVALID_RESOURCE };
    primal::graphics::rhi::ResourceHandle sc_cull_params_cb_{ primal::graphics::rhi::handles::INVALID_RESOURCE };
    primal::graphics::rhi::ResourceHandle sc_eval_params_cb_{ primal::graphics::rhi::handles::INVALID_RESOURCE };
    primal::graphics::rhi::ResourceHandle sc_global_data_cb_{ primal::graphics::rhi::handles::INVALID_RESOURCE };
    primal::graphics::rhi::ResourceHandle sc_tile_light_assign_buf_{ primal::graphics::rhi::handles::INVALID_RESOURCE };
    bool sc_fill_done_{ false };
    bool sc_lighting_done_{ false };
    uint32_t sc_lighting_atlas_idx_{ 0 };

    // Surface Cache CardCapture (graphics pipeline for rendering meshes into atlas)
    primal::graphics::rhi::PipelineHandle sc_capture_pipeline_{ primal::graphics::rhi::handles::INVALID_PIPELINE };
    primal::graphics::rhi::PipelineLayoutHandle sc_capture_layout_{ primal::graphics::rhi::handles::INVALID_PIPELINE_LAYOUT };
    primal::graphics::rhi::DescriptorSetLayoutHandle sc_capture_set_layout_{ primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    primal::graphics::rhi::DescriptorSetHandle sc_capture_descriptor_set_{ primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET };
    primal::graphics::rhi::ResourceHandle sc_capture_cb_{ primal::graphics::rhi::handles::INVALID_RESOURCE };
    primal::graphics::rhi::ResourceHandle sc_capture_depth_tex_{ primal::graphics::rhi::handles::INVALID_RESOURCE };
    primal::graphics::rhi::SamplerHandle sc_capture_sampler_{ primal::graphics::rhi::handles::INVALID_SAMPLER };

    // Surface Cache DepthDilate (compute, 3x3 depth hole fill)
    primal::graphics::rhi::PipelineHandle sc_dilate_pipeline_{ primal::graphics::rhi::handles::INVALID_PIPELINE };
    primal::graphics::rhi::PipelineLayoutHandle sc_dilate_layout_{ primal::graphics::rhi::handles::INVALID_PIPELINE_LAYOUT };
    primal::graphics::rhi::DescriptorSetLayoutHandle sc_dilate_set_layout_{ primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    primal::graphics::rhi::DescriptorSetHandle sc_dilate_descriptor_set_{ primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET };
    primal::graphics::rhi::ResourceHandle sc_dilate_params_cb_{ primal::graphics::rhi::handles::INVALID_RESOURCE };
    primal::graphics::rhi::ResourceHandle sc_depth_temp_tex_{ primal::graphics::rhi::handles::INVALID_RESOURCE };
    bool sc_dilate_done_{ false };

    // Card dispatch buffer for flattened 1D LightEval dispatch
    primal::graphics::rhi::ResourceHandle sc_card_dispatch_buf_{ primal::graphics::rhi::handles::INVALID_RESOURCE };

    // Surface Cache IndirectTrace (compute, ray march through GlobalSDF)
    primal::graphics::rhi::PipelineHandle sc_ind_trace_pipeline_{ primal::graphics::rhi::handles::INVALID_PIPELINE };
    primal::graphics::rhi::PipelineLayoutHandle sc_ind_trace_layout_{ primal::graphics::rhi::handles::INVALID_PIPELINE_LAYOUT };
    primal::graphics::rhi::DescriptorSetLayoutHandle sc_ind_trace_set_layout_{ primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    primal::graphics::rhi::DescriptorSetHandle sc_ind_trace_descriptor_set_{ primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET };
    primal::graphics::rhi::ResourceHandle sc_ind_trace_params_cb_{ primal::graphics::rhi::handles::INVALID_RESOURCE };
    primal::graphics::rhi::ResourceHandle sc_ray_hits_buf_{ primal::graphics::rhi::handles::INVALID_RESOURCE };

    // Surface Cache IndirectResolve (compute)
    primal::graphics::rhi::PipelineHandle sc_ind_resolve_pipeline_{ primal::graphics::rhi::handles::INVALID_PIPELINE };
    primal::graphics::rhi::PipelineLayoutHandle sc_ind_resolve_layout_{ primal::graphics::rhi::handles::INVALID_PIPELINE_LAYOUT };
    primal::graphics::rhi::DescriptorSetLayoutHandle sc_ind_resolve_set_layout_{ primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    primal::graphics::rhi::DescriptorSetHandle sc_ind_resolve_descriptor_set_{ primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET };
    primal::graphics::rhi::ResourceHandle sc_ind_resolve_params_cb_{ primal::graphics::rhi::handles::INVALID_RESOURCE };
    primal::graphics::rhi::ResourceHandle sc_indirect_out_tex_{ primal::graphics::rhi::handles::INVALID_RESOURCE };

    // Surface Cache → DDGI integration (Strategy C: pre-computed card→probe projection)
    static constexpr u32 SC_MAX_CONTRIBS_PER_PROBE = 6;

    struct SCProbeCardContrib {
        uint32_t card_index;
        float    sh_weights[4];  // pre-computed L0+L1 SH projection weights
    };

    struct SCProbeContribRange {
        uint32_t start;
        uint32_t count;
    };

    // Card→Probe contribution data (built once on CPU after card generation)
    primal::graphics::rhi::ResourceHandle sc_probe_contrib_range_buf_{ primal::graphics::rhi::handles::INVALID_RESOURCE };
    primal::graphics::rhi::ResourceHandle sc_flat_contrib_buf_{ primal::graphics::rhi::handles::INVALID_RESOURCE };
    primal::graphics::rhi::ResourceHandle sc_card_radiance_buf_{ primal::graphics::rhi::handles::INVALID_RESOURCE };

    // CardRadianceAvg pipeline (per frame: sample lighting atlas → card_radiance buffer)
    primal::graphics::rhi::PipelineHandle sc_card_rad_pipeline_{ primal::graphics::rhi::handles::INVALID_PIPELINE };
    primal::graphics::rhi::PipelineLayoutHandle sc_card_rad_layout_{ primal::graphics::rhi::handles::INVALID_PIPELINE_LAYOUT };
    primal::graphics::rhi::DescriptorSetLayoutHandle sc_card_rad_set_layout_{ primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    primal::graphics::rhi::DescriptorSetHandle sc_card_rad_ds_[3]{
        primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET,
        primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET,
        primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET
    };

    // ProbeIrradianceFromCards pipeline (per frame: weights × card_radiance → SH coefficients)
    primal::graphics::rhi::PipelineHandle sc_probe_irr_pipeline_{ primal::graphics::rhi::handles::INVALID_PIPELINE };
    primal::graphics::rhi::PipelineLayoutHandle sc_probe_irr_layout_{ primal::graphics::rhi::handles::INVALID_PIPELINE_LAYOUT };
    primal::graphics::rhi::DescriptorSetLayoutHandle sc_probe_irr_set_layout_{ primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    primal::graphics::rhi::DescriptorSetHandle sc_probe_irr_ds_[3]{
        primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET,
        primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET,
        primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET
    };

    // Surface Cache params CB for CardRadianceAvg shader
    primal::graphics::rhi::ResourceHandle sc_sc_params_cb_{ primal::graphics::rhi::handles::INVALID_RESOURCE };

    // DDGI volume CB for ProbeIrradianceFromCards shader
    primal::graphics::rhi::ResourceHandle sc_ddgi_vol_cb_[3]{
        primal::graphics::rhi::handles::INVALID_RESOURCE,
        primal::graphics::rhi::handles::INVALID_RESOURCE,
        primal::graphics::rhi::handles::INVALID_RESOURCE
    };

    // Color history for SSGI ray hit sampling
    std::unique_ptr<primal::graphics::nanite::ColorHistoryManager> colorHistoryManager_;
    struct StringHash {
        size_t operator()(const std::string& key) const {
            uint32_t hash;
            primal::utl::MurmurHash3_x86_32(key.c_str(), (int)key.length(), 0, &hash);
            return hash;
        }
    };
    std::unordered_map<std::string, primal::graphics::rhi::ShaderHandle, StringHash> shaderVariantMap;

    // Scene data
    primal::graphics::RenderSceneSnapshot sceneSnapshot_;
    std::vector<primal::graphics::nanite::NaniteRuntimeResource*> testResources_;

    // Triple-buffered shadow matrices (must match shadow map buffering)
    math::m4x4 cachedShadowMatrix0_[3]{ primal::graphics::rhi::math::MatrixIdentity(),
                                         primal::graphics::rhi::math::MatrixIdentity(),
                                         primal::graphics::rhi::math::MatrixIdentity() };
    math::m4x4 cachedShadowMatrix1_[3]{ primal::graphics::rhi::math::MatrixIdentity(),
                                         primal::graphics::rhi::math::MatrixIdentity(),
                                         primal::graphics::rhi::math::MatrixIdentity() };

    // Sponza scene data
    primal::graphics::RenderScene scene_;
    primal::graphics::RenderView view_;
    primal::utl::vector<primal::graphics::SceneDataMeshInfo> sceneMeshes_;
    
    // Test state
    u32 frameCount_{ 0 };
    u32 renderWidth_{ 0 };
    u32 renderHeight_{ 0 };
    bool isShutdown_{ false };
    primal::graphics::rhi::RHICamera camera_{};
    math::m4x4 cameraView_{ primal::graphics::rhi::math::MatrixIdentity() };
    math::m4x4 cameraProj_{ primal::graphics::rhi::math::MatrixIdentity() };

    // Culling debug data storage for final frame only
    primal::utl::vector<primal::graphics::nanite::CullingDebugData> finalFrameCullingDebugData_;

    // DDGI blit resources
    primal::graphics::rhi::DescriptorSetLayoutHandle blit_ddgi_set_layout_{ primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    primal::graphics::rhi::PipelineLayoutHandle blit_ddgi_layout_{ primal::graphics::rhi::handles::INVALID_PIPELINE_LAYOUT };
    primal::graphics::rhi::DescriptorSetHandle blit_ddgi_descriptor_set_{ primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET };
    primal::graphics::rhi::PipelineHandle blit_ddgi_pipeline_{ primal::graphics::rhi::handles::INVALID_PIPELINE };
    primal::graphics::rhi::ResourceHandle ddgi_probe_cb_[3]{
        primal::graphics::rhi::handles::INVALID_RESOURCE,
        primal::graphics::rhi::handles::INVALID_RESOURCE,
        primal::graphics::rhi::handles::INVALID_RESOURCE
    };

    // DDGI GI Gather compute pass (texture atlas for Apple GPU cache efficiency)
    primal::graphics::rhi::PipelineHandle       gi_gather_pipeline_    {primal::graphics::rhi::handles::INVALID_PIPELINE};
    primal::graphics::rhi::PipelineLayoutHandle gi_gather_layout_      {primal::graphics::rhi::handles::INVALID_PIPELINE_LAYOUT};
    primal::graphics::rhi::DescriptorSetLayoutHandle gi_gather_set_layout_ {primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    primal::graphics::rhi::DescriptorSetHandle  gi_gather_descriptor_set_ {primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET};
    primal::graphics::rhi::ResourceHandle        gi_halfres_texture_   {primal::graphics::rhi::handles::INVALID_RESOURCE};
    primal::graphics::rhi::ResourceHandle        gi_halfres_history_   {primal::graphics::rhi::handles::INVALID_RESOURCE};

    // Probe data atlas textures (buffer → texture for Apple GPU)
    primal::graphics::rhi::ResourceHandle dyn_sh_atlas_       {primal::graphics::rhi::handles::INVALID_RESOURCE};
    primal::graphics::rhi::ResourceHandle dyn_depth_atlas_    {primal::graphics::rhi::handles::INVALID_RESOURCE};
    primal::graphics::rhi::ResourceHandle stat_sh_atlas_      {primal::graphics::rhi::handles::INVALID_RESOURCE};
    primal::graphics::rhi::ResourceHandle stat_depth_atlas_   {primal::graphics::rhi::handles::INVALID_RESOURCE};

    // Atlas pre-filter pass (buffer → texture copy)
    primal::graphics::rhi::PipelineHandle       atlas_prefilter_pipeline_    {primal::graphics::rhi::handles::INVALID_PIPELINE};
    primal::graphics::rhi::PipelineLayoutHandle atlas_prefilter_layout_      {primal::graphics::rhi::handles::INVALID_PIPELINE_LAYOUT};
    primal::graphics::rhi::DescriptorSetLayoutHandle atlas_prefilter_set_layout_ {primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    primal::graphics::rhi::DescriptorSetHandle  atlas_prefilter_descriptor_set_ {primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET};
    primal::graphics::rhi::ResourceHandle       atlas_params_cb_             {primal::graphics::rhi::handles::INVALID_RESOURCE};

    // Static probe volume for GI Gather static probe data bindings
    std::unique_ptr<primal::graphics::lumen::StaticProbeVolume> static_probe_volume_;
    primal::graphics::rhi::ResourceHandle static_probe_cb_{primal::graphics::rhi::handles::INVALID_RESOURCE};

    bool sdf_voxelization_done_ = false;  // Lock SDF origins after first voxelization

    // Shadow mapping
    math::v3 light_direction_{ -0.4f, -0.8f, -0.3f };  // Normalized toward light
    u32 shadow_frame_index_{ 0 };
    bool shadow_enabled_{ true };

    // Shadow map caching — skip regeneration when light/scene is static
    math::m4x4 cached_shadow_vp_[3][2]{};     // [buffer_index][cascade]
    bool shadow_cache_valid_[3][2]{false};     // [buffer_index][cascade]
    bool shadow_cache_globally_valid_{false};  // Reset on scene change

    // Deferred PBR Lighting pipeline
    primal::graphics::rhi::PipelineHandle deferred_pipeline_{ primal::graphics::rhi::handles::INVALID_PIPELINE };
    primal::graphics::rhi::PipelineLayoutHandle deferred_layout_{ primal::graphics::rhi::handles::INVALID_PIPELINE_LAYOUT };
    primal::graphics::rhi::DescriptorSetLayoutHandle deferred_set_layout_{ primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    primal::graphics::rhi::DescriptorSetHandle deferred_descriptor_set_[3]{
        primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET,
        primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET,
        primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET
    };
    // Triple-buffered deferred output — fusion reads 2-frame-old data to avoid data race
    primal::graphics::rhi::ResourceHandle deferred_output_textures_[3]{
        primal::graphics::rhi::handles::INVALID_RESOURCE,
        primal::graphics::rhi::handles::INVALID_RESOURCE,
        primal::graphics::rhi::handles::INVALID_RESOURCE
    };
    primal::graphics::rhi::ResourceHandle deferred_view_cb_[3]{
        primal::graphics::rhi::handles::INVALID_RESOURCE,
        primal::graphics::rhi::handles::INVALID_RESOURCE,
        primal::graphics::rhi::handles::INVALID_RESOURCE
    };
    primal::graphics::rhi::ResourceHandle deferred_light_cb_[3]{
        primal::graphics::rhi::handles::INVALID_RESOURCE,
        primal::graphics::rhi::handles::INVALID_RESOURCE,
        primal::graphics::rhi::handles::INVALID_RESOURCE
    };
    primal::graphics::rhi::SamplerHandle deferred_sampler_handle_{ primal::graphics::rhi::handles::INVALID_SAMPLER };
    primal::graphics::rhi::ResourceHandle deferred_ddgi_probe_cb_[3]{
        primal::graphics::rhi::handles::INVALID_RESOURCE,
        primal::graphics::rhi::handles::INVALID_RESOURCE,
        primal::graphics::rhi::handles::INVALID_RESOURCE
    };

    // CRITICAL FIX: Triple-buffered camera data to match MAX_FRAMES_IN_FLIGHT = 3
    // This prevents array out-of-bounds and frame synchronization issues
    struct CameraBuffer {
        math::m4x4 view_matrix;
        math::m4x4 proj_matrix;
        u32 frame_index;
        u32 padding[3];
    } cameraBuffers_[3];

    // Test configuration
    struct TestConfig {
        u32 max_clusters{ 10000 };
        u32 max_instances{ 1000 };
        u32 streaming_pool_size_mb{ 64 };
        u32 max_requests_per_frame{ 100 };
        float eviction_threshold{ 0.8f };
        bool enable_streaming{ true };
        bool enable_lod_selection{ true };
        bool enable_occlusion_culling{ true };
    } testConfig_;

    // Test results tracking
    struct TestResults {
        std::atomic<u32> clusters_streamed{ 0 };
        std::atomic<u32> clusters_evicted{ 0 };
        std::atomic<u32> requests_generated{ 0 };
        std::atomic<u32> requests_processed{ 0 };
        std::atomic<float> avg_streaming_latency_ms{ 0.0f };
        std::atomic<float> avg_processing_time_ms{ 0.0f };
    } testResults_;

    // Helper methods
    bool InitializeDevice();
    bool InitializeWindowAndRenderSystem();
    bool InitializeStreamingComponents();
    bool CreateTestScene();
    bool LoadSponzaScene();
    bool LoadMaterialTextures();  // 🎨 NEW: Load textures for all materials
    void AdjustMaterialUVScaling();  // 🔧 NEW: Manually adjust UV scaling for testing
    bool VerifyMeshletUVSupport();
    bool SetupBasicRenderingPipeline();
    bool InitializeSSGIPipeline();
    bool InitializeDDGIBlitPipeline();
    bool InitializeSurfaceCachePipelines();
    bool BuildCardProbeAssignment();
    void UpdateTestScene();
    void BuildRenderGraph(primal::graphics::rendergraph::RenderGraph& graph, primal::graphics::rhi::ResourceHandle backBuffer, u32 currentBufferIndex);
    void ProcessStreamingFeedback();
    void RecordTestMetrics();
    void ValidateResults();
    void PrintAllInstanceBounds();
    void PrintFinalFrameCullingDebugData();

    // Input handling
    struct KeyStateTracker {
        bool f1_prev{ false };  // Toggle streaming visualization
        bool f2_prev{ false };  // Stress test mode
        bool f3_prev{ false };  // Performance benchmark
        bool f4_prev{ false };  // SSGI visualization mode toggle
        bool space_prev{ false }; // Pause/resume streaming
        bool f5_prev{ false };  // Mode 6 diagnostic toggle
    } keyState_;

    // SSGI visualization mode: 0=Composite(scene+SSGI), 1=SSGI only, 2=Scene only
    u32 ssgiVisMode_{ 0 };  // Scene only (no SSGI/DDGI overlay, direct deferred output)
    u32 mode_diag_{ 0 };    // Mode 6 diagnostic: 0=simple blit, 1=fusion same-tex, 2=fusion real, 3=7-bind simple shader
};