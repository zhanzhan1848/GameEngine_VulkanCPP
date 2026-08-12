#if defined(ENABLE_WEBGPU) && ENABLE_WEBGPU

#include "TestDawnRendering.h"
#include "Engine/Graphics/RHI/Core/RHITypes.h"
#include <iostream>
#include <cmath>

using namespace primal::graphics::rhi;
using namespace primal::graphics::rhi::handles;

// ============================================================
// Construction / Destruction
// ============================================================

Engine_Test::Engine_Test() = default;
Engine_Test::~Engine_Test() = default;

// ============================================================
// WGSL Shaders
// ============================================================

static const char* kTriangleWGSL = R"(
@vertex fn vs(@builtin(vertex_index) vi: u32) -> @builtin(position) vec4f {
    var pos = array<vec2f, 3>(
        vec2f( 0.0,  0.5),
        vec2f(-0.5, -0.5),
        vec2f( 0.5, -0.5)
    );
    return vec4f(pos[vi], 0.0, 1.0);
}

@fragment fn fs(@builtin(position) pos: vec4f) -> @location(0) vec4f {
    let r = pos.x / 800.0;
    let g = pos.y / 600.0;
    let b = 0.3 + 0.2 * sin(pos.x * 0.05);
    return vec4f(r, g, b, 1.0);
}
)";

// ============================================================
// Initialize
// ============================================================

bool Engine_Test::initialize() {
    std::cout << "[DawnRendering] Initializing..." << std::endl;

    // 1. Create platform window
    primal::platform::window_init_info windowInfo{
        nullptr, nullptr,
        "Dawn WebGPU Triangle",
        100, 100, 800, 600
    };
    window_ = primal::platform::create_window(&windowInfo);
    if (!window_.is_valid()) {
        std::cerr << "[DawnRendering] Failed to create window" << std::endl;
        return false;
    }

    // 2. Create Dawn device
    DeviceDesc deviceDesc{};
    deviceDesc.platform = RHIPlatform::Dawn;
    deviceDesc.enableDebug = true;
    deviceDesc.enableValidation = false;  // Reduce noise during rendering
    deviceDesc.maxFramesInFlight = 3;

    device_ = new DawnDevice(deviceDesc);
    if (!device_->Initialize()) {
        std::cerr << "[DawnRendering] Failed to initialize Dawn device" << std::endl;
        return false;
    }

    // 3. Create swapchain
    SwapChainDesc scDesc{};
    scDesc.window = window_.handle();
    scDesc.width = 800;
    scDesc.height = 600;
    scDesc.format = DataFormat::BGRA8_UNorm;
    scDesc.bufferCount = 3;

    swapchain_ = device_->CreateSwapChain(scDesc);
    if (!swapchain_) {
        std::cerr << "[DawnRendering] Failed to create swapchain" << std::endl;
        return false;
    }

    // 4. Create shaders (single WGSL module with both entry points)
    vs_ = device_->CreateShader(kTriangleWGSL, strlen(kTriangleWGSL),
                                 ShaderStage::Vertex, "vs");
    if (vs_ == INVALID_SHADER) {
        std::cerr << "[DawnRendering] Failed to compile vertex shader" << std::endl;
        return false;
    }

    fs_ = device_->CreateShader(kTriangleWGSL, strlen(kTriangleWGSL),
                                 ShaderStage::Pixel, "fs");
    if (fs_ == INVALID_SHADER) {
        std::cerr << "[DawnRendering] Failed to compile fragment shader" << std::endl;
        return false;
    }

    // 5. Create graphics pipeline
    GraphicsPipelineDesc pipeDesc{};
    pipeDesc.vertexShader = vs_;
    pipeDesc.pixelShader = fs_;
    pipeDesc.topology = PrimitiveTopology::TriangleList;
    pipeDesc.renderTargetFormats[0] = DataFormat::BGRA8_UNorm;
    pipeDesc.renderTargetCount = 1;
    pipeDesc.enableDepthTest = false;

    pipeline_ = device_->CreateGraphicsPipeline(pipeDesc);
    if (pipeline_ == INVALID_PIPELINE) {
        std::cerr << "[DawnRendering] Failed to create pipeline" << std::endl;
        return false;
    }

    std::cout << "[DawnRendering] Initialized. Window 800x600, rendering "
              << MAX_FRAMES << " frames." << std::endl;
    return true;
}

// ============================================================
// Run – one frame
// ============================================================

void Engine_Test::run() {
    if (!device_ || !swapchain_ || shuttingDown_) return;

    timer_.begin();

    device_->BeginFrame();

    // Acquire swapchain image
    u32 imageIndex = 0;
    if (!swapchain_->AcquireNextImage(&imageIndex)) {
        std::cerr << "[DawnRendering] AcquireNextImage failed" << std::endl;
        return;
    }

    // Get the back buffer texture handle (imageIndex is the slot just written)
    ResourceHandle backBuffer = swapchain_->GetBackBuffer(imageIndex);

    // Create command buffer
    auto cmd = device_->CreateCommandBuffer(CommandQueueType::Graphics);
    if (cmd == INVALID_COMMAND_BUFFER) return;

    // We need to manually drive the command buffer through Begin/End
    // since submitImpl auto-begins/ends only for Reset-state buffers.
    // Get the DawnCommandBuffer and call Begin.
    auto* dawnCmd = device_->GetCommandBuffer(cmd);
    if (!dawnCmd) return;
    dawnCmd->Begin();

    // Build render pass description
    RenderPassDesc rpDesc{};
    rpDesc.colorAttachments.emplace_back();
    rpDesc.colorAttachments[0].texture = backBuffer;
    rpDesc.colorAttachments[0].format = DataFormat::BGRA8_UNorm;
    rpDesc.colorAttachments[0].loadOp = LoadAction::Clear;
    rpDesc.colorAttachments[0].storeOp = StoreAction::Store;
    rpDesc.colorAttachments[0].clearValue.color = {0.1f, 0.1f, 0.2f, 1.0f};  // Dark blue
    rpDesc.viewport = ViewportDesc{{0.0f, 0.0f}, {800.0f, 600.0f}, 0.0f, 1.0f};

    dawnCmd->BeginRenderPass(rpDesc);
    dawnCmd->BindGraphicsPipeline(pipeline_);
    dawnCmd->Draw(3, 0, 1, 0);
    dawnCmd->EndRenderPass();

    dawnCmd->End();

    // Submit
    QueueSubmitInfo submit{};
    submit.cmdBuffer = cmd;
    device_->Submit(submit);

    // Present
    swapchain_->Present(INVALID_SYNC);

    device_->DestroyCommandBuffer(cmd);

    device_->EndFrame();

    timer_.end();
    frameIndex_++;

    if (frameIndex_ >= MAX_FRAMES) {
        std::cout << "[DawnRendering] " << MAX_FRAMES << " frames rendered. Shutting down."
                  << std::endl;
#ifdef __APPLE__
        NS::Application::sharedApplication()->terminate(nullptr);
#endif
    }
}

// ============================================================
// Shutdown
// ============================================================

void Engine_Test::shutdown() {
    shuttingDown_ = true;
    std::cout << "[DawnRendering] Shutting down..." << std::endl;

    if (displayLink_) {
        CFRunLoopRemoveTimer(runLoop_, displayLink_, kCFRunLoopCommonModes);
        CFRelease(displayLink_);
        displayLink_ = nullptr;
    }

    if (pipeline_ != INVALID_PIPELINE && device_) {
        device_->DestroyPipeline(pipeline_);
        pipeline_ = INVALID_PIPELINE;
    }
    if (vs_ != INVALID_SHADER && device_) {
        device_->DestroyShader(vs_);
        vs_ = INVALID_SHADER;
    }
    if (fs_ != INVALID_SHADER && device_) {
        device_->DestroyShader(fs_);
        fs_ = INVALID_SHADER;
    }
    if (swapchain_ && device_) {
        device_->DestroySwapChain(swapchain_);
        swapchain_ = nullptr;
    }
    if (device_) {
        device_->Shutdown();
        delete device_;
        device_ = nullptr;
    }

    if (window_.is_valid())
    {
        primal::platform::remove_window(window_.get_id());
    }

    std::cout << "[DawnRendering] Shutdown complete" << std::endl;
}

// ============================================================
// macOS ApplicationDelegate
// ============================================================

#ifdef __APPLE__

void Engine_Test::applicationDidFinishLaunching(NS::Notification* notification) {
    NS::Application* pApp = reinterpret_cast<NS::Application*>(notification->object());
    pApp->activateIgnoringOtherApps(true);

    if (!initialize()) {
        std::cerr << "[DawnRendering] Initialization failed" << std::endl;
        NS::Application::sharedApplication()->terminate(nullptr);
        return;
    }

    SetupRenderLoop();
}

void Engine_Test::applicationWillFinishLaunching(NS::Notification* notification) {
    NS::Application* pApp = reinterpret_cast<NS::Application*>(notification->object());
    pApp->setActivationPolicy(NS::ActivationPolicy::ActivationPolicyRegular);
}

bool Engine_Test::applicationShouldTerminateAfterLastWindowClosed(
    [[maybe_unused]] NS::Application* pSender) {
    shutdown();
    return true;
}

void Engine_Test::SetupRenderLoop() {
    runLoop_ = CFRunLoopGetMain();
    CFRunLoopTimerContext context = {0, this, nullptr, nullptr, nullptr};
    displayLink_ = CFRunLoopTimerCreate(
        kCFAllocatorDefault,
        CFAbsoluteTimeGetCurrent(),
        1.0 / 60.0,  // 60 FPS
        0, 0,
        DisplayLinkCallback,
        &context
    );
    CFRunLoopAddTimer(runLoop_, displayLink_, kCFRunLoopCommonModes);
    std::cout << "[DawnRendering] 60 FPS render loop started" << std::endl;
}

void Engine_Test::DisplayLinkCallback(CFRunLoopTimerRef, void* info) {
    auto* test = static_cast<Engine_Test*>(info);
    test->run();
}

#endif // __APPLE__

#endif // ENABLE_WEBGPU
