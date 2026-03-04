#include "TestStandardPipeline.h"
#include "Engine/Common/CommonHeaders.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalDevice.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalMath.h"
#include <iostream>

using namespace primal::graphics;
using namespace primal::math;

// Engine_Test Implementation
Engine_Test::Engine_Test() 
    : primal::test::RenderTestRunner(std::make_unique<StandardPipelineTestCase>()) 
{}

// StandardPipelineTestCase Implementation
bool StandardPipelineTestCase::Initialize() {
    std::cout << "Initializing StandardRenderPipeline Test..." << std::endl;
    
    // 1. Create Window
    primal::platform::window_init_info winInfo{};
    winInfo.caption = "TestStandardPipeline";
    winInfo.width = 1280;
    winInfo.height = 720;
    window = primal::platform::create_window(&winInfo);
    if (!window.is_valid()) {
        std::cerr << "Failed to create window!" << std::endl;
        return false;
    }

    // 2. Create Device
    rhi::DeviceDesc desc;
    desc.platform = rhi::RHIPlatform::Metal;
    desc.enableDebug = true;
    
    auto* metalDevice = new rhi::MetalDevice(desc);
    if (!metalDevice || !metalDevice->Initialize()) {
        std::cerr << "Failed to create/initialize Metal device" << std::endl;
        delete metalDevice;
        return false;
    }
    device.reset(metalDevice);

    // 3. Initialize RenderSystem (Handles SwapChain creation)
    RenderSystemInitInfo sysInfo;
    sysInfo.device = device.get();
    sysInfo.window = window.handle();
    sysInfo.width = winInfo.width;
    sysInfo.height = winInfo.height;
    
    if (!renderSystem.Initialize(sysInfo)) {
        std::cerr << "Failed to initialize RenderSystem!" << std::endl;
        return false;
    }
    
    // 4. Initialize Pipeline
    pipeline = new StandardRenderPipeline();
    if (!pipeline->Initialize(device.get())) {
        std::cerr << "Failed to initialize pipeline" << std::endl;
        return false;
    }
    
    // Ensure we don't use override output
    pipeline->SetOutputResource(rhi::handles::INVALID_RESOURCE, {});

    // 5. Create Scene and View
    scene = new RenderScene();
    
    // Add some dummy entities/proxies
    RenderProxy proxy;
    proxy.meshId = primal::id::invalid_id;
    proxy.materialId = primal::id::invalid_id;
    
    simd::float4 col0 = { 1.0f, 0.0f, 0.0f, 0.0f };
    simd::float4 col1 = { 0.0f, 1.0f, 0.0f, 0.0f };
    simd::float4 col2 = { 0.0f, 0.0f, 1.0f, 0.0f };
    simd::float4 col3 = { 0.0f, 0.0f, 0.0f, 1.0f };
    proxy.transform = simd_matrix(col0, col1, col2, col3);

    scene->AddProxy(proxy);

    view = new RenderView();
    rhi::ViewportDesc viewport;
    viewport.size.x = 800;
    viewport.size.y = 600;
    view->SetViewport(viewport);
    
    v3 eyePos{0, 0, 5};
    v3 target{0, 0, 0};
    v3 up{0, 1, 0};
    
    v3 forward = simd_normalize(target - eyePos);
    m4x4 viewMat = createLookToLH(eyePos, forward, up);
    
    m4x4 projMat = rhi::metal::CreatePerspectiveMatrix(
        primal::math::pi / 4.0f,
        800.0f / 600.0f,
        0.1f,
        100.0f
    );
    
    view->SetViewMatrix(viewMat);
    view->SetProjectionMatrix(projMat);

    std::cout << "Initialization successful." << std::endl;
    return true;
}

void StandardPipelineTestCase::Run() {
    if (pipeline && scene && view) {
        // Update Frustum
        view->UpdateFrustum();
        
        // Cull Scene
        view->Cull(*scene);

        // Begin Frame (Internal SwapChain management)
        rhi::ResourceHandle backBufferHandle;
        rhi::SyncHandle signalFence;
        if (!renderSystem.BeginFrame(backBufferHandle, signalFence)) {
             return;
        }
        
        // Render
        pipeline->Render(*scene, *view, backBufferHandle, renderSystem.GetBackBufferDesc(), signalFence);

        // End Frame
        renderSystem.EndFrame();
        
        // Print stats occasionally
        const auto& stats = pipeline->GetStats();
        static int frame = 0;
        if (frame++ % 60 == 0) {
            std::cout << "Frame " << frame << ": CPU Time = " << stats.cpuFrameTimeMs << "ms, Draw Calls = " << stats.drawCallCount << std::endl;
        }
    }
}

void StandardPipelineTestCase::Shutdown() {
    std::cout << "Shutting down..." << std::endl;
    if (pipeline) {
        delete pipeline;
        pipeline = nullptr;
    }
    
    renderSystem.Shutdown();
    
    if (scene) {
        delete scene;
        scene = nullptr;
    }
    if (view) {
        delete view;
        view = nullptr;
    }
    
    if (window.is_valid()) {
        primal::platform::remove_window(window.get_id());
    }

    // Device is deleted by unique_ptr
    device.reset();
}
