#pragma once

#if defined(ENABLE_WEBGPU) && ENABLE_WEBGPU

#include "Test.h"

#ifdef __APPLE__
#include <AppKit/AppKit.hpp>
#include <CoreFoundation/CoreFoundation.h>
#endif

#include "Engine/Graphics/RHI/Platforms/Dawn/DawnDevice.h"
#include "Engine/Graphics/RHI/Core/RHIDevice.h"
#include "Engine/Graphics/RHI/Core/RHISwapChain.h"
#include "Engine/Graphics/RHI/Core/RHITypes.h"
#include "Engine/Graphics/ForwardRenderer.h"
#include "Engine/Graphics/MaterialInstance.h"
#include "Engine/Graphics/SceneDataAdapter.h"
#include "Engine/Graphics/RenderScene.h"
#include "Engine/Graphics/RenderView.h"
#include "Engine/Graphics/RenderGraph/RenderGraph.h"
#include "Engine/Graphics/RenderPipeline/RenderPasses/PostProcess/BloomPass.h"
#include "Engine/Graphics/RenderPipeline/RenderPasses/PostProcess/SSAOPass.h"
#include "Engine/Graphics/RenderPipeline/RenderPasses/PostProcess/ToneMappingPass.h"
#include "Engine/Graphics/Nanite/GPUDrivenDrawPipeline.h"
#include "Engine/Graphics/Nanite/GPUCullingPipeline.h"
#include "Engine/Graphics/Nanite/HZBSystem.h"
#include "Engine/Graphics/Nanite/GPUMaterialRegistry.h"
#include "Engine/Graphics/Scene/RenderSceneSnapshot.h"
#include "Engine/Graphics/Utils/ShaderRegistry.h"
#include "Engine/Graphics/Lumen/DDGI/LumenDDGIPass.h"
#include "Engine/Graphics/Lumen/StaticProbe/StaticProbeVolume.h"

// Forward-declare ProbeBakingScene instead of including StaticProbeBaker.h,
// which transitively pulls in BVH.hpp and its primal::graphics::utl namespace
// (conflicts with primal::utl used elsewhere in this file).
namespace primal::graphics::lumen { struct ProbeBakingScene; }
#include "Engine/Platform/Platform.h"
#include <unordered_map>

class Engine_Test : public Test
#ifdef __APPLE__
    , public NS::ApplicationDelegate
#endif
{
public:
    Engine_Test() = default;
    ~Engine_Test() override;
    bool initialize() override;
    void run() override;
    void shutdown() override;

#ifdef __APPLE__
    void applicationDidFinishLaunching(NS::Notification* notification) override;
    void applicationWillFinishLaunching(NS::Notification* notification) override;
    bool applicationShouldTerminateAfterLastWindowClosed(NS::Application* pSender) override;
#endif

    void RenderFrame();

private:
#ifdef __APPLE__
    static void DisplayLinkCallback(CFRunLoopTimerRef timer, void* info);
#endif
    bool LoadSponzaScene();
    void UpdateCamera(float dt);
    void UpdateCameraView();
    void CreateDepthTexture();
    void DestroyDepthTexture();
    void CreatePrepassDepthTexture();
    void DestroyPrepassDepthTexture();
    void CreateShadowResources();
    void RenderShadowPass(primal::graphics::rhi::RHICommandBuffer* cmd);
    primal::math::m4x4 ComputeLightViewProjection() const;
    void ComputeCSMViewProjections();
    void CreateIBLResources();
    void RunIBLCompute(ResourceHandle envCubeTex, u32 cubeSize);
    void UpdatePunctualLights();

    // Meshlet pipeline (mode 7) — init, per-frame dispatch, shutdown.
    bool InitializeMeshletPipeline();
    void ShutdownMeshletPipeline();
    void RenderMeshletFrame(primal::graphics::rhi::RHICommandBuffer* cmd);
    // Mode 10 = Mode 9 meshlet path + DDGI indirect. Implemented in Task 11.
    void RenderMeshletDDGIFrame(primal::graphics::rhi::RHICommandBuffer* cmd);
    void InitializeDDGIForMode10();   // called once on first Mode 10 entry
    void ShutdownDDGIForMode10();     // releases DDGI resources
    void BuildProbeBakingScene(primal::graphics::lumen::ProbeBakingScene& scene);

    primal::graphics::rhi::DawnDevice* device_{nullptr};
    primal::graphics::rhi::RHISwapChain* swapchain_{nullptr};
    primal::platform::window window_;
    bool shuttingDown_{false};

    primal::graphics::ForwardRenderer forwardRenderer_;
    primal::graphics::RenderScene scene_;
    primal::graphics::RenderView view_;

    // Camera — outside Sponza, looking toward origin (same as macOS verified view)
    primal::math::v3 cameraPos_{0.0f, 5.0f, -10.0f};
    float cameraYaw_{3.14159265f};
    float cameraPitch_{-0.291f};
    float cameraSpeed_{5.0f};

    // Sponza scene data
    primal::utl::vector<primal::graphics::SceneDataMeshInfo> sceneMeshInfos_;
    std::unordered_map<primal::id::id_type, std::shared_ptr<primal::graphics::MaterialInstance>> materials_;

    // Shared resources
    primal::graphics::rhi::PipelineLayoutHandle pipelineLayout_{primal::graphics::rhi::handles::INVALID_PIPELINE_LAYOUT};
    primal::graphics::rhi::DescriptorSetLayoutHandle materialSetLayout_{primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    primal::graphics::rhi::SamplerHandle materialSampler_{primal::graphics::rhi::handles::INVALID_RESOURCE};
    primal::graphics::rhi::ResourceHandle depthTexture_{primal::graphics::rhi::handles::INVALID_RESOURCE};
    // Non-jittered depth from ForwardRenderer::RenderDawnDepthPrepass — feeds HZB/SSR/SSAO
    // so downstream passes don't see the per-frame TAA jitter that the forward pass writes.
    primal::graphics::rhi::ResourceHandle prepassDepthTexture_{primal::graphics::rhi::handles::INVALID_RESOURCE};
    primal::graphics::rhi::ResourceHandle hdrTexture_{primal::graphics::rhi::handles::INVALID_RESOURCE};
    primal::graphics::rhi::ResourceHandle velocityTexture_{primal::graphics::rhi::handles::INVALID_RESOURCE};
    primal::graphics::rhi::TextureDesc hdrDesc_{};

    // G-Buffer textures for Deferred mode (Phase 3b)
    // RT0: WorldPos  RGBA16F, RT1: Normal+linearDepth RGBA16F,
    // RT2: Albedo+Metallic RGBA8_sRGB, RT3: ORM RGBA8, RT4: Velocity RG16F
    static constexpr u32 kGBufferRTCount = 5;
    primal::graphics::rhi::ResourceHandle gbufferTextures_[kGBufferRTCount] = {
        primal::graphics::rhi::handles::INVALID_RESOURCE,
        primal::graphics::rhi::handles::INVALID_RESOURCE,
        primal::graphics::rhi::handles::INVALID_RESOURCE,
        primal::graphics::rhi::handles::INVALID_RESOURCE,
        primal::graphics::rhi::handles::INVALID_RESOURCE
    };

    // Shadow map resources (depth-only pass)
    primal::graphics::rhi::ResourceHandle shadowDepthTexture_{primal::graphics::rhi::handles::INVALID_RESOURCE};
    primal::graphics::rhi::PipelineHandle shadowPipeline_{primal::graphics::rhi::handles::INVALID_PIPELINE};
    primal::graphics::rhi::PipelineLayoutHandle shadowPipelineLayout_{primal::graphics::rhi::handles::INVALID_PIPELINE_LAYOUT};
    primal::graphics::rhi::DescriptorSetLayoutHandle shadowDSL_{primal::graphics::rhi::handles::INVALID_RESOURCE};
    primal::graphics::rhi::SamplerHandle shadowSampler_{primal::graphics::rhi::handles::INVALID_SAMPLER};
    primal::graphics::rhi::ResourceHandle shadowPerObjectBuf_{primal::graphics::rhi::handles::INVALID_RESOURCE};
    void* shadowPerObjectMapped_{nullptr};
    primal::graphics::rhi::DescriptorSetHandle shadowPerObjectSet_{primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET};
    primal::math::m4x4 lightVP_{};
    primal::math::m4x4 cascadeVPs_[4]{};
    float cascadeSplits_[5]{}; // 5 split distances for 4 cascades
    static constexpr u32 kCascadeCount = 4;

    // IBL resources
    primal::graphics::rhi::ResourceHandle iblIrradianceTex_{primal::graphics::rhi::handles::INVALID_RESOURCE};
    primal::graphics::rhi::ResourceHandle iblPrefilterTex_{primal::graphics::rhi::handles::INVALID_RESOURCE};
    primal::graphics::rhi::ResourceHandle iblBRDFLUTTex_{primal::graphics::rhi::handles::INVALID_RESOURCE};
    primal::graphics::rhi::SamplerHandle iblSampler_{primal::graphics::rhi::handles::INVALID_SAMPLER};
    std::unique_ptr<primal::graphics::rendergraph::RenderGraph> renderGraph_;
    // Separate RenderGraph for DDGI passes — built per frame inside RenderMeshletFrame
    // between the meshlet GBuffer draw and the deferred lighting pass. Cannot share
    // renderGraph_ because that one executes at line 1103 AFTER deferred lighting.
    std::unique_ptr<primal::graphics::rendergraph::RenderGraph> ddgiGraph_;
    primal::graphics::rhi::CommandBufferHandle cmdBuffer_{primal::graphics::rhi::handles::INVALID_COMMAND_BUFFER};

    // Render mode switching (Tab key)
    enum class DawnRenderMode : u8 { NoEffects = 0, ShadowOnly = 1, ShadowAndIBL = 2, Full = 3, FullPlusSSR = 4, Deferred = 5, LumenDDGI = 6, MeshletNoIBL = 7, Meshlet = 8, MeshletSSGISSR = 9, MeshletSSGISSRDDGI = 10, Count };
    DawnRenderMode renderMode_{DawnRenderMode::ShadowAndIBL};
    bool prevTabState_{false};
    bool prevVState_{false};
    u32 meshletDebugMode_{0};   // 0=off, 1=meshlet_id, 2=triangle_id, 3=mesh_id
    // Mode 9 sub-mode: which screen-space effect(s) to apply.
    // V key cycles Off → SSGIOnly → SSROnly → Both → Off ...
    enum class SSGISSRSubmode : u8 { Off = 0, SSGIOnly = 1, SSROnly = 2, Both = 3 };
    SSGISSRSubmode ssgissrSubmode_ = SSGISSRSubmode::Both;
    bool punctualLightsAdded_{false};

    // ---- Meshlet pipeline (mode 7) ----
    // Owned pointers; released in ShutdownMeshletPipeline(). Singletons
    // (GPUDrivenDrawPipeline::Get() / GPUCullingPipeline::Get()) are accessed
    // via ::Get() and have their lifetime managed by the singleton itself —
    // we only call Initialize/Shutdown on them.
    primal::graphics::nanite::GPUMaterialRegistry* meshletMaterialRegistry_{nullptr};
    primal::graphics::nanite::HZBSystem* meshletHZBSystem_{nullptr};
    primal::graphics::RenderSceneSnapshot meshletSceneSnapshot_;
    bool meshletInitialized_{false};

    // ---- Mode 10: DDGI (Phase A) ----
    // RenderPipeline passes owned by the test harness. LumenDDGIPass is
    // backend-agnostic C++; StaticProbeVolume holds the CPU bake.
    std::unique_ptr<primal::graphics::lumen::LumenDDGIPass> ddgiPass_;
    std::unique_ptr<primal::graphics::lumen::StaticProbeVolume> staticProbeVolume_;

    // Half-res RGBA16F indirect-lighting texture, written by DDGIGIGather.wgsl
    // and read by DeferredLighting_Meshlet.wgsl at binding 13.
    primal::graphics::rhi::ResourceHandle giIndirectTexture_{primal::graphics::rhi::handles::INVALID_RESOURCE};

    // Previous frame's lit HDR color, fed back into DDGI trace as radiance cache.
    primal::graphics::rhi::ResourceHandle prevHdrTexture_{primal::graphics::rhi::handles::INVALID_RESOURCE};

    // Inline GIGather compute pipeline (mirrors TestNaniteStreamingPipeline pattern).
    primal::graphics::rhi::PipelineHandle giGatherPipeline_{primal::graphics::rhi::handles::INVALID_PIPELINE};
    primal::graphics::rhi::PipelineLayoutHandle giGatherPipelineLayout_{primal::graphics::rhi::handles::INVALID_PIPELINE_LAYOUT};
    primal::graphics::rhi::DescriptorSetLayoutHandle giGatherDsl_{primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    primal::graphics::rhi::DescriptorSetHandle giGatherDescriptorSet_{primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET};

    // 3-frame rotating constant buffer for GatherCB (camera + dims).
    primal::graphics::rhi::ResourceHandle giGatherCbBuffers_[3] = {
        primal::graphics::rhi::handles::INVALID_RESOURCE,
        primal::graphics::rhi::handles::INVALID_RESOURCE,
        primal::graphics::rhi::handles::INVALID_RESOURCE
    };

    bool ddgiEnabled_{false};

    u32 frameIndex_{0};
    u32 width_{1280};
    u32 height_{720};
    u32 totalFrames_{0};
    static constexpr u32 kFrameCount = 3;

#ifdef __APPLE__
    CFRunLoopTimerRef displayLink_{nullptr};
    CFRunLoopRef runLoop_{nullptr};
#endif
    time_it timer_;
    std::chrono::steady_clock::time_point lastFrameTime_;
};

#endif // ENABLE_WEBGPU
