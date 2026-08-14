#if defined(ENABLE_WEBGPU) && ENABLE_WEBGPU

#include "TestDawnRenderGraph.h"
#include "Engine/Graphics/RHI/Core/RHITypes.h"
#include "Engine/Graphics/RenderPipeline/RenderPasses/PostProcess/ToneMappingPass.h"
#include <iostream>
#include <cmath>

using namespace primal::graphics::rhi;
using namespace primal::graphics::rhi::handles;
using namespace primal::graphics::rendergraph;

// ============================================================
// Construction / Destruction
// ============================================================

Engine_Test::Engine_Test() = default;
Engine_Test::~Engine_Test() = default;

// ============================================================
// Inline fullscreen triangle shader (renders gradient based on time)
// ============================================================

static const char* kFullscreenWGSL = R"(
struct VertexOutput {
    @builtin(position) position: vec4f,
    @location(0) uv: vec2f,
};

@vertex fn vs(@builtin(vertex_index) vi: u32) -> VertexOutput {
    var positions = array<vec2f, 3>(
        vec2f(-1.0, -1.0),
        vec2f( 3.0, -1.0),
        vec2f(-1.0,  3.0)
    );
    var uvs = array<vec2f, 3>(
        vec2f(0.0, 1.0),
        vec2f(2.0, 1.0),
        vec2f(0.0, -1.0)
    );
    var output: VertexOutput;
    output.position = vec4f(positions[vi], 0.0, 1.0);
    output.uv = uvs[vi];
    return output;
}

@fragment fn fs(input: VertexOutput) -> @location(0) vec4f {
    let r = input.uv.x;
    let g = input.uv.y;
    let b = 0.3 + 0.2 * sin(input.uv.x * 6.28 + input.uv.y * 3.14);
    return vec4f(r, g, b, 1.0);
}
)";

// ============================================================
// Initialize
// ============================================================

bool Engine_Test::initialize() {
    std::cout << "[DawnRenderGraph] Initializing..." << std::endl;

    // 1. Create platform window
    primal::platform::window_init_info windowInfo{
        nullptr, nullptr,
        "Dawn WebGPU RenderGraph Test",
        100, 100, WIDTH, HEIGHT
    };
    window_ = primal::platform::create_window(&windowInfo);
    if (!window_.is_valid()) {
        std::cerr << "[DawnRenderGraph] Failed to create window" << std::endl;
        return false;
    }

    // 2. Create Dawn device
    DeviceDesc deviceDesc{};
    deviceDesc.platform = RHIPlatform::Dawn;
    deviceDesc.enableDebug = true;
    deviceDesc.enableValidation = false;
    deviceDesc.maxFramesInFlight = 3;

    device_ = new DawnDevice(deviceDesc);
    if (!device_->Initialize()) {
        std::cerr << "[DawnRenderGraph] Failed to initialize Dawn device" << std::endl;
        return false;
    }

    // 3. Create swapchain
    SwapChainDesc scDesc{};
    scDesc.window = window_.handle();
    scDesc.width = WIDTH;
    scDesc.height = HEIGHT;
    scDesc.format = DataFormat::BGRA8_UNorm;
    scDesc.bufferCount = 3;

    swapchain_ = device_->CreateSwapChain(scDesc);
    if (!swapchain_) {
        std::cerr << "[DawnRenderGraph] Failed to create swapchain" << std::endl;
        return false;
    }

    // 4. Create RenderGraph
    renderGraph_ = std::make_unique<RenderGraph>(*device_);
    std::cout << "[DawnRenderGraph] RenderGraph created" << std::endl;

    // 5. Create shaders
    vs_ = device_->CreateShader(kFullscreenWGSL, strlen(kFullscreenWGSL),
                                 ShaderStage::Vertex, "vs");
    if (vs_ == INVALID_SHADER) {
        std::cerr << "[DawnRenderGraph] Failed to compile vertex shader" << std::endl;
        return false;
    }

    fs_ = device_->CreateShader(kFullscreenWGSL, strlen(kFullscreenWGSL),
                                 ShaderStage::Pixel, "fs");
    if (fs_ == INVALID_SHADER) {
        std::cerr << "[DawnRenderGraph] Failed to compile fragment shader" << std::endl;
        return false;
    }

    // 6. Create pipeline
    GraphicsPipelineDesc pipeDesc{};
    pipeDesc.vertexShader = vs_;
    pipeDesc.pixelShader = fs_;
    pipeDesc.topology = PrimitiveTopology::TriangleList;
    pipeDesc.renderTargetFormats[0] = DataFormat::BGRA8_UNorm;
    pipeDesc.renderTargetCount = 1;
    pipeDesc.enableDepthTest = false;

    pipeline_ = device_->CreateGraphicsPipeline(pipeDesc);
    if (pipeline_ == INVALID_PIPELINE) {
        std::cerr << "[DawnRenderGraph] Failed to create pipeline" << std::endl;
        return false;
    }

    std::cout << "[DawnRenderGraph] Initialized. Rendering " << MAX_FRAMES
              << " frames via RenderGraph." << std::endl;
    return true;
}

// ============================================================
// Run – one frame
// ============================================================

void Engine_Test::run() {
    if (!device_ || !swapchain_ || !renderGraph_ || shuttingDown_) return;

    timer_.begin();

    device_->BeginFrame();

    // Acquire swapchain image
    u32 imageIndex = 0;
    if (!swapchain_->AcquireNextImage(&imageIndex)) {
        std::cerr << "[DawnRenderGraph] AcquireNextImage failed" << std::endl;
        return;
    }

    ResourceHandle backBuffer = swapchain_->GetBackBuffer(imageIndex);

    // --- RenderGraph frame ---

    // 1. Clear previous frame
    renderGraph_->Clear();

    // 2. Import backbuffer
    TextureDesc bbDesc{};
    bbDesc.size = {WIDTH, HEIGHT, 1};
    bbDesc.format = DataFormat::BGRA8_UNorm;
    bbDesc.type = TextureType::Texture2D;
    bbDesc.mipLevels = 1;
    bbDesc.usage = TextureUsage::RenderTarget | TextureUsage::Present;
    RGResourceHandle bbHandle = renderGraph_->ImportTexture("BackBuffer", backBuffer, bbDesc);

    // 3. Pass 1: HDR fullscreen pass (renders gradient to transient HDR texture)
    struct HDRPassData {
        RGResourceHandle hdrTarget;
    };

    auto hdrResult = renderGraph_->AddPass<HDRPassData>(
        "HDRFullscreenPass",
        RGPassType::Graphics,
        RGPassCategory::Main,
        [this](HDRPassData& data, RenderGraphBuilder& builder) {
            TextureDesc hdrDesc;
            hdrDesc.size = {WIDTH, HEIGHT, 1};
            hdrDesc.format = DataFormat::BGRA8_UNorm;
            hdrDesc.type = TextureType::Texture2D;
            hdrDesc.mipLevels = 1;
            hdrDesc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
            data.hdrTarget = builder.CreateTexture("HDR_Scene", hdrDesc, ResourceState::RenderTarget);

            RGRenderPassDesc rpDesc{};
            RGAttachmentDesc& color = rpDesc.colors.emplace_back();
            color.texture = data.hdrTarget;
            color.loadOp = LoadAction::Clear;
            color.storeOp = StoreAction::Store;
            color.clearColor = ClearValue{primal::math::v4{0.05f, 0.05f, 0.1f, 1.0f}};
            builder.DeclareRenderPass(rpDesc);
        },
        [this](const HDRPassData& data, RenderGraphContext& ctx) {
            ctx.cmdBuffer->SetViewport(ViewportDesc{
                {0.0f, 0.0f}, {static_cast<float>(WIDTH), static_cast<float>(HEIGHT)}, 0.0f, 1.0f
            });
            ctx.cmdBuffer->BindGraphicsPipeline(pipeline_);
            ctx.cmdBuffer->Draw(3, 0, 1, 0);
        }
    );

    // 4. Pass 2: ToneMapping (HDR -> LDR)
    auto tonemapResult = primal::graphics::PostProcess::AddToneMappingPass(*renderGraph_, hdrResult.hdrTarget);

    // 5. Pass 3: Blit ToneMapping output to backbuffer
    struct BlitPassData {
        RGResourceHandle source;
        RGResourceHandle target;
    };

    renderGraph_->AddPass<BlitPassData>(
        "BlitToBackbuffer",
        RGPassType::Graphics,
        RGPassCategory::PostProcess,
        [this, bbHandle, tonemapResult](BlitPassData& data, RenderGraphBuilder& builder) {
            data.source = builder.Read(tonemapResult.output, ResourceState::ShaderResource);
            data.target = builder.Write(bbHandle, ResourceState::RenderTarget);

            RGRenderPassDesc rpDesc{};
            RGAttachmentDesc& color = rpDesc.colors.emplace_back();
            color.texture = data.target;
            color.loadOp = LoadAction::DontCare;
            color.storeOp = StoreAction::Store;
            builder.DeclareRenderPass(rpDesc);
        },
        [this](const BlitPassData& data, RenderGraphContext& ctx) {
            ctx.cmdBuffer->SetViewport(ViewportDesc{
                {0.0f, 0.0f}, {static_cast<float>(WIDTH), static_cast<float>(HEIGHT)}, 0.0f, 1.0f
            });
            ctx.cmdBuffer->BindGraphicsPipeline(pipeline_);
            ctx.cmdBuffer->Draw(3, 0, 1, 0);
        }
    );

    // 6. Compile
    renderGraph_->Compile();

    // 5. Execute via command buffer
    auto cmd = device_->CreateCommandBuffer(CommandQueueType::Graphics);
    if (cmd == INVALID_COMMAND_BUFFER) {
        std::cerr << "[DawnRenderGraph] Failed to create command buffer" << std::endl;
        device_->EndFrame();
        return;
    }

    auto* dawnCmd = device_->GetCommandBuffer(cmd);
    if (!dawnCmd) {
        device_->DestroyCommandBuffer(cmd);
        device_->EndFrame();
        return;
    }

    dawnCmd->Begin();
    renderGraph_->Execute(dawnCmd);
    dawnCmd->End();

    // 6. Submit
    QueueSubmitInfo submit{};
    submit.cmdBuffer = cmd;
    device_->Submit(submit);

    // 7. Present
    swapchain_->Present(INVALID_SYNC);

    device_->DestroyCommandBuffer(cmd);
    device_->EndFrame();

    timer_.end();
    frameIndex_++;

    if (frameIndex_ % 60 == 0) {
        std::cout << "[DawnRenderGraph] Frame " << frameIndex_ << "/" << MAX_FRAMES << std::endl;
    }

    if (frameIndex_ >= MAX_FRAMES) {
        std::cout << "[DawnRenderGraph] " << MAX_FRAMES << " frames rendered. Shutting down." << std::endl;
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
    std::cout << "[DawnRenderGraph] Shutting down..." << std::endl;

    renderGraph_.reset();

    if (pipeline_ != INVALID_PIPELINE && device_) {
        device_->DestroyPipeline(pipeline_);
        pipeline_ = INVALID_PIPELINE;
    }
    if (fs_ != INVALID_SHADER && device_) {
        device_->DestroyShader(fs_);
        fs_ = INVALID_SHADER;
    }
    if (vs_ != INVALID_SHADER && device_) {
        device_->DestroyShader(vs_);
        vs_ = INVALID_SHADER;
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

    if (window_.is_valid()) {
        primal::platform::remove_window(window_.get_id());
    }

    std::cout << "[DawnRenderGraph] Shutdown complete" << std::endl;
}

// ============================================================
// macOS ApplicationDelegate
// ============================================================

#ifdef __APPLE__

void Engine_Test::applicationDidFinishLaunching(NS::Notification* notification) {
    NS::Application* pApp = reinterpret_cast<NS::Application*>(notification->object());
    pApp->activateIgnoringOtherApps(true);

    if (!initialize()) {
        std::cerr << "[DawnRenderGraph] Initialization failed" << std::endl;
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
        1.0 / 60.0,
        0, 0,
        DisplayLinkCallback,
        &context
    );
    CFRunLoopAddTimer(runLoop_, displayLink_, kCFRunLoopCommonModes);
    std::cout << "[DawnRenderGraph] 60 FPS render loop started" << std::endl;
}

void Engine_Test::DisplayLinkCallback(CFRunLoopTimerRef, void* info) {
    auto* test = static_cast<Engine_Test*>(info);
    test->run();
}

#endif // __APPLE__

#endif // ENABLE_WEBGPU
