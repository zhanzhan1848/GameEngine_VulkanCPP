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
#include "Engine/Graphics/SceneDataAdapter.h"
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
    float cameraYaw_{0.0f};
    float cameraPitch_{0.0f};
    float cameraSpeed_{10.0f};
    float mouseSensitivity_{0.003f};
    bool mouseDown_{false};
    float lastMouseX_{0.0f};
    float lastMouseY_{0.0f};

    u32 frameIndex_{0};
    u32 width_{1280};
    u32 height_{720};

    CFRunLoopTimerRef displayLink_{nullptr};
    CFRunLoopRef runLoop_{nullptr};
    time_it timer_;
};

#endif // ENABLE_WEBGPU
