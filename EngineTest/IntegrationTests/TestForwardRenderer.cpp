#include "TestForwardRenderer.h"
#include "Engine/Common/CommonHeaders.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalDevice.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalMath.h"
#include "Engine/Graphics/Lumen/LumenTypes.h"
#include "Engine/Input/Input.h"
#include "MacKeyboard.h"
#include <iostream>
#include <cmath>

using namespace primal::graphics;
using namespace primal::graphics::rhi;
using namespace primal::math;

// ============================================================================
// Engine_Test
// ============================================================================

Engine_Test::Engine_Test()
    : primal::test::RenderTestRunner(std::make_unique<ForwardRendererTestCase>())
{}

// ============================================================================
// ForwardRendererTestCase::Initialize
// ============================================================================

bool ForwardRendererTestCase::Initialize() {
    std::cout << "[TestForwardRenderer] Initializing..." << std::endl;

    // 1. Start keyboard monitoring
    monitorKeyboardInput();

    // 2. Create window
    primal::platform::window_init_info winInfo{};
    winInfo.caption = "TestForwardRenderer - Editor Mode";
    winInfo.width = 1280;
    winInfo.height = 720;
    window = primal::platform::create_window(&winInfo);
    if (!window.is_valid()) {
        std::cerr << "Failed to create window!" << std::endl;
        return false;
    }

    // 3. Create Metal device
    DeviceDesc desc;
    desc.platform = RHIPlatform::Metal;
    desc.enableDebug = true;

    auto* metalDevice = new MetalDevice(desc);
    if (!metalDevice || !metalDevice->Initialize()) {
        std::cerr << "Failed to create Metal device" << std::endl;
        delete metalDevice;
        return false;
    }
    device.reset(metalDevice);

    // 4. Initialize RenderSystem (creates swap chain)
    RenderSystemInitInfo sysInfo;
    sysInfo.device = device.get();
    sysInfo.window = window.handle();
    sysInfo.width = winInfo.width;
    sysInfo.height = winInfo.height;

    if (!renderSystem.Initialize(sysInfo)) {
        std::cerr << "Failed to initialize RenderSystem!" << std::endl;
        return false;
    }

    // 5. Initialize StandardRenderPipeline
    pipeline = new StandardRenderPipeline();
    if (!pipeline->Initialize(device.get())) {
        std::cerr << "Failed to initialize pipeline" << std::endl;
        return false;
    }

    pipeline->SetOutputResource(handles::INVALID_RESOURCE, {});
    pipeline->SetViewportSize(winInfo.width, winInfo.height);

    // Configure Lumen (triggers InitializeSubsystems → creates ForwardSceneRenderer)
    lumen::LumenConfig lumenConfig;
    lumenConfig.quality = lumen::LumenQualityPreset::Low;
    pipeline->SetLumenConfig(lumenConfig);

    // 6. Enter editor mode and load scene
    pipeline->SetEditorMode(true);
    std::cout << "[TestForwardRenderer] Editor mode enabled" << std::endl;

    const char* scenePaths[] = {
        "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/assets/Sponza_process_rebuild.model",
        "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/assets/Sponza_process.model",
        "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/assets/Sponza.model",
        "EngineTest/assets/Sponza_process_rebuild.model",
        "assets/Sponza_process_rebuild.model",
    };

    bool loaded = false;
    for (const char* path : scenePaths) {
        if (pipeline->LoadEditorScene(path)) {
            std::cout << "[TestForwardRenderer] Loaded scene: " << path << std::endl;
            loaded = true;
            break;
        }
    }
    if (!loaded) {
        std::cerr << "[TestForwardRenderer] Failed to load any scene (will render empty)" << std::endl;
    }

    // 7. Create scene and view
    scene = new RenderScene();

    // Add a dummy identity proxy (required by pipeline)
    RenderProxy proxy;
    proxy.meshId = primal::id::invalid_id;
    proxy.materialId = primal::id::invalid_id;
    simd::float4 col0 = {1, 0, 0, 0};
    simd::float4 col1 = {0, 1, 0, 0};
    simd::float4 col2 = {0, 0, 1, 0};
    simd::float4 col3 = {0, 0, 0, 1};
    proxy.transform = simd_matrix(col0, col1, col2, col3);
    scene->AddProxy(proxy);

    view = new RenderView();
    ViewportDesc viewport;
    viewport.size.x = winInfo.width;
    viewport.size.y = winInfo.height;
    view->SetViewport(viewport);

    UpdateCamera();

    std::cout << "[TestForwardRenderer] Initialization complete." << std::endl;
    std::cout << "[TestForwardRenderer] Controls: WASD=Move, QE=Up/Down, Arrows=Rotate, ESC=Quit" << std::endl;
    return true;
}

// ============================================================================
// ForwardRendererTestCase::Run
// ============================================================================

void ForwardRendererTestCase::Run() {
    timer_.begin();

    // Handle input
    HandleInput(1.0f / 60.0f);
    UpdateCamera();

    // Render (matches TestModularPipeline pattern)
    if (pipeline && scene && view) {
        view->UpdateFrustum();
        view->Cull(*scene);

        ResourceHandle backBuffer;
        SyncHandle signalFence;
        if (!renderSystem.BeginFrame(backBuffer, signalFence)) {
            timer_.end();
            return;
        }

        auto* cmd = renderSystem.GetCurrentCommandBuffer();
        auto cmdHandle = renderSystem.GetCurrentCommandBufferHandle();
        u32 bufferIndex = renderSystem.GetCurrentFrameIndex();

        cmd->Reset();
        cmd->Begin();

        pipeline->RenderWithCommandBuffer(
            *scene, *view, backBuffer, renderSystem.GetBackBufferDesc(),
            cmd, bufferIndex, cmdHandle, signalFence);

        cmd->End();

        rhi::QueueSubmitInfo submitInfo{};
        submitInfo.cmdBuffer = cmdHandle;
        submitInfo.signalFence = signalFence;
        device->Submit(submitInfo);

        renderSystem.EndFrame();

        // Async texture loading
        auto* fwd = pipeline->GetForwardRenderer();
        if (fwd) {
            fwd->UpdateAsyncTextures(static_cast<u32>(frame_count_));
        }

        frame_count_++;
    }

    timer_.end();
}

// ============================================================================
// ForwardRendererTestCase::Shutdown
// ============================================================================

void ForwardRendererTestCase::Shutdown() {
    std::cout << "[TestForwardRenderer] Shutting down..." << std::endl;

    if (pipeline) {
        delete pipeline;
        pipeline = nullptr;
    }

    renderSystem.Shutdown();

    if (scene) { delete scene; scene = nullptr; }
    if (view) { delete view; view = nullptr; }

    if (window.is_valid()) {
        primal::platform::remove_window(window.get_id());
    }

    device.reset();
    std::cout << "[TestForwardRenderer] Shutdown complete." << std::endl;
}

// ============================================================================
// Input & Camera
// ============================================================================

void ForwardRendererTestCase::HandleInput(float dt) {
    using namespace primal::input;

    input_value val;

    // Movement in camera-local space
    float cy = std::cos(camera_yaw_), sy = std::sin(camera_yaw_);
    v3 cam_forward{sy, 0, -cy};   // horizontal forward
    v3 cam_right{cy, 0, sy};       // horizontal right

    v3 move_dir{0, 0, 0};

    get(input_source::keyboard, input_code::key_w, val);
    if (val.current.x > 0.0f) move_dir = move_dir + cam_forward;
    get(input_source::keyboard, input_code::key_s, val);
    if (val.current.x > 0.0f) move_dir = move_dir - cam_forward;
    get(input_source::keyboard, input_code::key_a, val);
    if (val.current.x > 0.0f) move_dir = move_dir - cam_right;
    get(input_source::keyboard, input_code::key_d, val);
    if (val.current.x > 0.0f) move_dir = move_dir + cam_right;
    get(input_source::keyboard, input_code::key_q, val);
    if (val.current.x > 0.0f) move_dir.y -= 1;
    get(input_source::keyboard, input_code::key_e, val);
    if (val.current.x > 0.0f) move_dir.y += 1;

    float len = std::sqrt(move_dir.x * move_dir.x + move_dir.y * move_dir.y + move_dir.z * move_dir.z);
    if (len > 0.001f) {
        move_dir /= len;
        camera_pos_ = camera_pos_ + move_dir * move_speed_ * dt;
    }

    // Rotation
    get(input_source::keyboard, input_code::key_left, val);
    if (val.current.x > 0.0f) camera_yaw_ += rotate_speed_ * dt;
    get(input_source::keyboard, input_code::key_right, val);
    if (val.current.x > 0.0f) camera_yaw_ -= rotate_speed_ * dt;
    get(input_source::keyboard, input_code::key_up, val);
    if (val.current.x > 0.0f) camera_pitch_ += rotate_speed_ * dt;
    get(input_source::keyboard, input_code::key_down, val);
    if (val.current.x > 0.0f) camera_pitch_ -= rotate_speed_ * dt;

    // Clamp pitch
    camera_pitch_ = std::max(-1.5f, std::min(1.5f, camera_pitch_));

    // ESC: quit
    get(input_source::keyboard, input_code::key_escape, val);
    if (val.current.x > 0.0f) {
        std::cout << "[TestForwardRenderer] ESC pressed, exiting..." << std::endl;
#ifdef __APPLE__
        NS::Application::sharedApplication()->terminate(nullptr);
#endif
    }
}

void ForwardRendererTestCase::UpdateCamera() {
    float cy = std::cos(camera_yaw_), sy = std::sin(camera_yaw_);
    float cp = std::cos(camera_pitch_), sp = std::sin(camera_pitch_);

    v3 forward{sy * cp, sp, -cy * cp};
    v3 up{0, 1, 0};

    v3 fwd_norm = simd_normalize(forward);
    v3 target = camera_pos_ + fwd_norm;
    m4x4 viewMat = metal::CreateLookAtMatrix(camera_pos_, target, up);
    constexpr float fov = 60.0f * (pi / 180.0f);
    m4x4 projMat = metal::CreatePerspectiveMatrix(
        fov,
        1280.0f / 720.0f,
        0.1f,
        1000.0f
    );

    if (view) {
        view->SetViewMatrix(viewMat);
        view->SetProjectionMatrix(projMat);
    }
}
