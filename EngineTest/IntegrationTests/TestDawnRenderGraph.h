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
#include "Engine/Graphics/RenderGraph/RenderGraph.h"
#include "Engine/Platform/Platform.h"

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

    primal::graphics::rhi::DawnDevice* device_{nullptr};
    primal::graphics::rhi::RHISwapChain* swapchain_{nullptr};
    std::unique_ptr<primal::graphics::rendergraph::RenderGraph> renderGraph_;

    primal::graphics::rhi::ShaderHandle vs_{primal::graphics::rhi::handles::INVALID_SHADER};
    primal::graphics::rhi::ShaderHandle fs_{primal::graphics::rhi::handles::INVALID_SHADER};
    primal::graphics::rhi::PipelineHandle pipeline_{primal::graphics::rhi::handles::INVALID_PIPELINE};

    primal::platform::window window_;
    u32 frameIndex_{0};
    bool shuttingDown_{false};
    static constexpr u32 MAX_FRAMES = 300;
    static constexpr u32 WIDTH = 800;
    static constexpr u32 HEIGHT = 600;

    CFRunLoopTimerRef displayLink_{nullptr};
    CFRunLoopRef runLoop_{nullptr};
    time_it timer_;
};

#endif // ENABLE_WEBGPU
