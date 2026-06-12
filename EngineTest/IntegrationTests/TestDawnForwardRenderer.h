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
#include "Engine/Graphics/Utils/ShaderRegistry.h"
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
    void CreateShadowResources();
    void RenderShadowPass(primal::graphics::rhi::RHICommandBuffer* cmd);
    primal::math::m4x4 ComputeLightViewProjection() const;
    void ComputeCSMViewProjections();
    void CreateIBLResources();
    void RunIBLCompute(ResourceHandle envCubeTex, u32 cubeSize);
    void UpdatePunctualLights();

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
    primal::graphics::rhi::ResourceHandle hdrTexture_{primal::graphics::rhi::handles::INVALID_RESOURCE};
    primal::graphics::rhi::TextureDesc hdrDesc_{};

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
    primal::graphics::rhi::CommandBufferHandle cmdBuffer_{primal::graphics::rhi::handles::INVALID_COMMAND_BUFFER};

    // Render mode switching (Tab key)
    enum class DawnRenderMode : u8 { NoEffects = 0, ShadowOnly = 1, ShadowAndIBL = 2, Full = 3, Count };
    DawnRenderMode renderMode_{DawnRenderMode::ShadowAndIBL};
    bool prevTabState_{false};
    bool punctualLightsAdded_{false};

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
