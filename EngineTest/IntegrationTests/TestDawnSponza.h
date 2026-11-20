#pragma once

#if defined(ENABLE_WEBGPU) && ENABLE_WEBGPU

#include "Test.h"
#include "DawnSponzaPostProcess.h"
#include "DawnSponzaPresent.h"
#include "DawnSponzaTAA.h"
#include "DawnSponzaEnvironment.h"
#include "DawnSponzaEnvironmentColor.h"
#include "DawnSponzaDeferredLighting.h"
#include "DawnSponzaFusion.h"
#include "DawnSponzaShadows.h"
#include "DawnSponzaGeometry.h"

#ifdef __APPLE__
#include <AppKit/AppKit.hpp>
#include <CoreFoundation/CoreFoundation.h>
#endif

#include "Engine/Graphics/RHI/Platforms/Dawn/DawnDevice.h"
#include "Engine/Graphics/RHI/Core/RHIDevice.h"
#include "Engine/Graphics/RHI/Core/RHISwapChain.h"
#include "Engine/Graphics/RHI/Core/RHITypes.h"
#include "Engine/Graphics/SceneDataAdapter.h"
#include "DawnSponzaSurfaceCache.h"
#include "DawnSponzaSurfaceLighting.h"
#include "DawnSponzaMeasuredRadiance.h"
#include "DawnSponzaProbeUpdate.h"
#include "DawnSponzaDepthHistory.h"
// The shared governor expects this host scalar alias from its includer.
namespace primal::graphics { using f64 = double; }
#include "Engine/Graphics/RenderPipeline/Modules/GIAdaptiveGovernor.h"
#include "Engine/Graphics/Lumen/GIWarmup.h"
#include "DawnSponzaRadiosity.h"
#include "DawnSponzaGather.h"
#include "DawnSponzaScreenProbeSDF.h"
#include "DawnSponzaScreenProbeGather.h"
#include "Engine/Graphics/RenderMesh.h"
#include "Engine/Graphics/RenderScene.h"
#include "Engine/Graphics/RenderView.h"
#include "Engine/Platform/Platform.h"
#include <unordered_map>

class Engine_Test : public Test
#ifdef __APPLE__
    , public NS::ApplicationDelegate
#endif
{
public:
    Engine_Test();
    ~Engine_Test() override;
    bool initialize() override;
    void run() override;
    void shutdown() override;

#ifdef __APPLE__
    void applicationDidFinishLaunching(NS::Notification* notification) override;
    void applicationWillFinishLaunching(NS::Notification* notification) override;
    bool applicationShouldTerminateAfterLastWindowClosed(NS::Application* pSender) override;
#endif

private:
    void SetupRenderLoop();
    static void DisplayLinkCallback(CFRunLoopTimerRef timer, void* info);

    bool LoadScene();
    bool CreatePipelines();
    bool CreateResources();
    primal::graphics::rhi::ResourceHandle LoadTextureFromFile(const std::string& path,
        primal::graphics::rhi::DataFormat format = primal::graphics::rhi::DataFormat::RGBA8_UNorm);
    primal::graphics::rhi::ResourceHandle CreateTextureFromData(int w, int h, const unsigned char* data,
        primal::graphics::rhi::DataFormat format = primal::graphics::rhi::DataFormat::RGBA8_UNorm);
    void UpdateCamera(float dt);

    primal::tests::dawn_sponza::DeferredLighting deferredLighting_;

    // Device & Window
    primal::graphics::rhi::DawnDevice* device_{nullptr};
    primal::graphics::rhi::RHISwapChain* swapchain_{nullptr};
    primal::platform::window window_;
    bool shuttingDown_{false};

    // Shaders & Pipeline
    primal::graphics::rhi::ShaderHandle vs_{primal::graphics::rhi::handles::INVALID_SHADER};
    primal::graphics::rhi::ShaderHandle fs_{primal::graphics::rhi::handles::INVALID_SHADER};
    primal::graphics::rhi::PipelineHandle pipeline_{primal::graphics::rhi::handles::INVALID_PIPELINE};
    primal::graphics::rhi::PipelineLayoutHandle pipelineLayout_{primal::graphics::rhi::handles::INVALID_PIPELINE_LAYOUT};
    primal::graphics::rhi::DescriptorSetLayoutHandle globalLayout_{primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    primal::graphics::rhi::DescriptorSetLayoutHandle materialLayout_{primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    primal::graphics::rhi::SamplerHandle defaultSampler_{primal::graphics::rhi::handles::INVALID_SAMPLER};

    primal::tests::dawn_sponza::Shadows shadows_;
    primal::tests::dawn_sponza::MeasuredGeometry measuredGeometry_;
    primal::tests::dawn_sponza::ProbeRelocation probeRelocation_;
    bool measuredGeometryEnabled_=false;
    primal::tests::dawn_sponza::SurfaceCacheCapture surfaceCache_;
    bool surfaceCacheEnabled_=false;
    primal::tests::dawn_sponza::SurfaceCacheLighting surfaceLighting_;
    bool surfaceLightingEnabled_=false;
    primal::tests::dawn_sponza::TileRadiosity tileRadiosity_;
    bool tileRadiosityEnabled_=false;
    primal::tests::dawn_sponza::MeasuredRadiance measuredRadiance_;
    bool measuredRadianceEnabled_=false;
    primal::tests::dawn_sponza::ProbeUpdate probeUpdate_;
    bool probeUpdateEnabled_=false;
    primal::tests::dawn_sponza::ProbeSchedule probeSchedule_;
    primal::tests::dawn_sponza::DepthHistory probeDepthHistory_;
    primal::graphics::GIAdaptiveGovernor probeGovernor_;
    u32 probeCapacity_=0;
    u64 probeUpdatesTotal_=0;
    primal::tests::dawn_sponza::ScreenGather screenGather_;
    bool screenGatherEnabled_=false;
    primal::tests::dawn_sponza::ScreenProbePlacement screenProbes_;
    primal::tests::dawn_sponza::ScreenProbeHZB screenProbeHZB_;
    primal::tests::dawn_sponza::ScreenProbeSDF screenProbeSDF_;
    primal::tests::dawn_sponza::ScreenProbeFinalize screenProbeFinalize_;
    primal::tests::dawn_sponza::ScreenProbeAverage screenProbeAverage_;
    primal::tests::dawn_sponza::ScreenProbeHistory screenProbeHistory_;
    primal::tests::dawn_sponza::ScreenProbeGather screenProbeGather_;
    primal::graphics::rhi::ResourceHandle directTexture_{primal::graphics::rhi::handles::INVALID_RESOURCE};
    bool screenProbePlacementEnabled_=false;
    primal::graphics::rhi::ResourceHandle ormTexture_{primal::graphics::rhi::handles::INVALID_RESOURCE};
    std::vector<primal::tests::dawn_sponza::ShadowBounds> shadowBounds_;
    primal::graphics::rhi::ShaderHandle normalShader_{primal::graphics::rhi::handles::INVALID_SHADER};
    primal::graphics::rhi::PipelineHandle normalPipeline_{primal::graphics::rhi::handles::INVALID_PIPELINE};
    primal::graphics::rhi::ResourceHandle normalTexture_{primal::graphics::rhi::handles::INVALID_RESOURCE};
    primal::tests::dawn_sponza::PostProcess postProcess_;
    primal::tests::dawn_sponza::Fusion fusion_;
    primal::tests::dawn_sponza::Fusion::Params fusionParameters_;
    primal::tests::dawn_sponza::TemporalAA temporalAA_;
    bool taaEnabled_=false;
    primal::tests::dawn_sponza::EnvironmentVisibility environmentVisibility_;
    primal::tests::dawn_sponza::EnvironmentIBL environmentIBL_;
    primal::tests::dawn_sponza::EnvironmentColor environmentColor_;
    primal::graphics::rhi::ResourceHandle environmentPolicy_{primal::graphics::rhi::handles::INVALID_RESOURCE};
    primal::graphics::rhi::ResourceHandle albedoTexture_{primal::graphics::rhi::handles::INVALID_RESOURCE};
    primal::tests::dawn_sponza::ToneParameters toneParameters_;
    primal::graphics::rhi::ResourceHandle hdrTexture_{primal::graphics::rhi::handles::INVALID_RESOURCE};

    // Depth texture
    primal::graphics::rhi::ResourceHandle depthTexture_{primal::graphics::rhi::handles::INVALID_RESOURCE};

    // Scene
    primal::utl::vector<primal::graphics::SceneDataMeshInfo> sceneMeshes_;
    primal::graphics::RenderScene scene_;
    primal::graphics::RenderView view_;

    // Per-frame uniform buffers (triple buffered)
    static constexpr u32 kFrameCount = 3;
    primal::graphics::rhi::ResourceHandle viewBuffers_[kFrameCount]{};
    primal::graphics::rhi::ResourceHandle lightBuffers_[kFrameCount]{};
    primal::graphics::rhi::DescriptorSetHandle globalSets_[kFrameCount]{};

    // Per-mesh material resources
    struct MeshMaterial {
        primal::graphics::rhi::ResourceHandle diffuseTex{primal::graphics::rhi::handles::INVALID_RESOURCE};
        primal::graphics::rhi::ResourceHandle normalTex{primal::graphics::rhi::handles::INVALID_RESOURCE};
        primal::graphics::rhi::ResourceHandle ormTex{primal::graphics::rhi::handles::INVALID_RESOURCE};
        primal::graphics::rhi::DescriptorSetHandle materialSet{primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET};
    };
    std::vector<MeshMaterial> meshMaterials_;

    // Texture cache: path -> ResourceHandle (avoid loading same texture hundreds of times)
    std::unordered_map<std::string, primal::graphics::rhi::ResourceHandle> textureCache_;

    // Camera
    primal::math::v3 cameraPos_{0.0f, 5.0f, 0.0f};
    float cameraYaw_{1.57079632679f}; // This host uses sin/cos yaw; +X matches the reference.
    float cameraPitch_{0.0f};
    float cameraSpeed_{10.0f};
    float mouseSensitivity_{0.003f};
    bool mouseDown_{false};
    float lastMouseX_{0.0f};
    float lastMouseY_{0.0f};

    u32 frameIndex_{0};
    primal::tests::dawn_sponza::Present present_;
    primal::graphics::rhi::ResourceHandle ldrTexture_{primal::graphics::rhi::handles::INVALID_RESOURCE};
    u32 outputWidth_{1280},outputHeight_{720};
    u32 probeSpacing_{8};
    u32 width_{1280};
    u32 height_{720};

    CFRunLoopTimerRef displayLink_{nullptr};
    CFRunLoopRef runLoop_{nullptr};
    time_it timer_;
};

#endif // ENABLE_WEBGPU
