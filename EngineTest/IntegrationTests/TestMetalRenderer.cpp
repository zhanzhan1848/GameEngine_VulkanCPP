/**
 * @file TestMetalRenderer.cpp
 * @brief Metal 渲染器集成测试
 * @details 验证 RenderSystem、Forward Renderer 和 Metal RHI 的集成流程
 * @author GameEngine VulkanCPP Team
 * @date 2026-01-12
 */

#ifdef __APPLE__

#include "TestRenderer.h"
#include "Graphics/RHI/Systems/RenderSystem.h"
#include "Graphics/RenderScene.h"
#include "Graphics/RenderView.h"
#include "Graphics/RenderMesh.h"
#include "Graphics/Material.h"
#include "Graphics/MaterialInstance.h"
#include "Graphics/RHI/Platforms/Metal/MetalDevice.h"
#include "Graphics/RHI/Core/RHIMath.h"
#include "Platform/Platform.h"
#include "Platform/Window.h"
#include <iostream>
#include <fstream>
#include <vector>

using namespace primal;
using namespace primal::graphics;
using namespace primal::graphics::rhi;
using namespace primal::graphics::rhi::math;

// Define the shader source here or load from file
const char* kSimpleColorShader = R"(
#include <metal_stdlib>
using namespace metal;

struct VertexIn {
    float3 position [[attribute(0)]];
    float3 color [[attribute(1)]];
};

struct VertexOut {
    float4 position [[position]];
    float4 color;
};

struct Uniforms {
    float4x4 viewProjectionMatrix;
    float4x4 modelMatrix;
};

vertex VertexOut vertexMain(
    VertexIn in [[stage_in]],
    constant Uniforms& uniforms [[buffer(1)]])
{
    VertexOut out;
    float4 worldPos = uniforms.modelMatrix * float4(in.position, 1.0);
    out.position = uniforms.viewProjectionMatrix * worldPos;
    out.color = float4(in.color, 1.0);
    return out;
}

fragment float4 fragmentMain(VertexOut in [[stage_in]]) {
    return in.color;
}
)";

struct Engine_Test_Impl {
    platform::window window;
    RenderSystem renderSystem;
    RenderScene scene;
    RenderView view;
    
    RenderMesh* mesh = nullptr;
    Material* material = nullptr;
    MaterialInstance* materialInstance = nullptr;
    
    // Keep resources alive
    std::unique_ptr<RHIDeviceBase> device;
    
    // Test state
    float rotationAngle = 0.0f;
};

static std::unique_ptr<Engine_Test_Impl> g_Test;

// Uniform Data Structure
struct UniformData {
    m4x4 viewProjectionMatrix;
    m4x4 modelMatrix;
};

bool Engine_Test::initialize() {
    g_Test = std::make_unique<Engine_Test_Impl>();
    
    // 1. Create Window
    platform::window_init_info info{};
    info.caption = "RenderSystem Integration Test";
    info.width = 1280;
    info.height = 720;
    g_Test->window = platform::create_window(&info);
    
    if (!g_Test->window.is_valid()) {
        std::cerr << "Failed to create window" << std::endl;
        return false;
    }

    // 2. Create Device
    DeviceDesc deviceDesc{};
    deviceDesc.platform = RHIPlatform::Metal;
    auto metalDevice = std::make_unique<MetalDevice>(deviceDesc);
    if (!metalDevice->Initialize()) {
        std::cerr << "Failed to initialize device" << std::endl;
        return false;
    }
    g_Test->device = std::move(metalDevice);

    // 3. Initialize RenderSystem
    RenderSystemInitInfo sysInfo;
    sysInfo.device = g_Test->device.get();
    sysInfo.window = g_Test->window.handle();
    sysInfo.width = info.width;
    sysInfo.height = info.height;
    
    if (!g_Test->renderSystem.Initialize(sysInfo)) {
        std::cerr << "Failed to initialize RenderSystem" << std::endl;
        return false;
    }

    // 4. Create Material & Shader
    g_Test->material = new Material();
    
    // Vertex Attributes
    utl::vector<VertexInputAttribute> attrs;
    attrs.push_back({0, 0, DataFormat::RGB32_Float, 0}); // Position
    attrs.push_back({1, 0, DataFormat::RGB32_Float, 12}); // Color
    g_Test->material->SetVertexAttributes(attrs);
    
    utl::vector<VertexInputBinding> bindings;
    bindings.push_back({0, 24, true}); // Stride = 24, perVertex = true
    g_Test->material->SetVertexBindings(bindings);
    
    // Shader
    g_Test->material->SetShader(ShaderStage::Vertex, kSimpleColorShader, strlen(kSimpleColorShader), "vertexMain");
    g_Test->material->SetShader(ShaderStage::Pixel, kSimpleColorShader, strlen(kSimpleColorShader), "fragmentMain");
    
    // Set Formats for Pipeline Creation
    utl::vector<DataFormat> colorFormats;
    colorFormats.push_back(DataFormat::BGRA8_UNorm);
    g_Test->material->SetRenderTargetFormats(colorFormats, DataFormat::D32_Float);
    
    // Set Rasterizer State (Back face culling)
    RasterizerState rasterState{};
    rasterState.cullMode = CullMode::Back;
    // rasterState.frontFace = FrontFace::CounterClockwise; // Not supported in current RHI
    // If we set CullMode::None, frontFace doesn't matter for culling.
    g_Test->material->SetRasterizerState(rasterState);
    
    // Set Depth State
    DepthStencilState depthState{};
    depthState.enableDepthTest = false; // Disable for debugging
    depthState.enableDepthWrite = false;
    depthState.depthFunc = ComparisonFunc::Always; // Always pass
    g_Test->material->SetDepthStencilState(depthState);

    // Setup Uniforms
    g_Test->material->SetUniformBlockSize(sizeof(UniformData));
    g_Test->material->SetUniformBufferBinding(1); // Bind to buffer(1) matching shader

    // Create Descriptor Set Layout
    rhi::DescriptorSetLayoutBinding layoutBinding{};
    layoutBinding.binding = 1;
    layoutBinding.descriptorType = rhi::DescriptorType::UniformBuffer;
    layoutBinding.descriptorCount = 1;
    layoutBinding.stageFlags = rhi::ShaderStage::Vertex;
    
    rhi::DescriptorSetLayoutDesc layoutDesc{};
    layoutDesc.bindings = &layoutBinding;
    layoutDesc.bindingCount = 1;
    
    rhi::DescriptorSetLayoutHandle dsLayout = g_Test->device->CreateDescriptorSetLayout(layoutDesc);
    if (dsLayout == rhi::handles::INVALID_RESOURCE) {
        std::cerr << "Failed to create DescriptorSetLayout" << std::endl;
        return false;
    }
    g_Test->material->SetDescriptorSetLayout(dsLayout);

    // Create Pipeline Layout
    rhi::PipelineLayoutDesc pipelineLayoutDesc{};
    pipelineLayoutDesc.setLayouts = &dsLayout;
    pipelineLayoutDesc.setLayoutCount = 1;
    
    rhi::PipelineLayoutHandle pipelineLayout = g_Test->device->CreatePipelineLayout(pipelineLayoutDesc);
    if (pipelineLayout == rhi::handles::INVALID_PIPELINE_LAYOUT) {
        std::cerr << "Failed to create PipelineLayout" << std::endl;
        return false;
    }
    g_Test->material->SetPipelineLayout(pipelineLayout);

    g_Test->materialInstance = new MaterialInstance(g_Test->material);
    if (!g_Test->materialInstance->Initialize(g_Test->device.get())) {
        std::cerr << "Failed to initialize MaterialInstance" << std::endl;
        return false;
    }

    // 5. Create Mesh (Cube)
    // Vertices (Pos + Color)
    // Need full cube vertices for proper box but for simple test one quad is enough? 
    // Let's use 8 vertices with indices for a cube.
    // 8 corners:
    // 0: - - + (Red)
    // 1: + - + (Green)
    // 2: + + + (Blue)
    // 3: - + + (Yellow)
    // 4: - - - (Cyan)
    // 5: + - - (Magenta)
    // 6: + + - (White)
    // 7: - + - (Black)
    float cubeVertices[] = {
        -0.5f, -0.5f,  0.5f,  1.0f, 0.0f, 0.0f, // 0
         0.5f, -0.5f,  0.5f,  0.0f, 1.0f, 0.0f, // 1
         0.5f,  0.5f,  0.5f,  0.0f, 0.0f, 1.0f, // 2
        -0.5f,  0.5f,  0.5f,  1.0f, 1.0f, 0.0f, // 3
        -0.5f, -0.5f, -0.5f,  0.0f, 1.0f, 1.0f, // 4
         0.5f, -0.5f, -0.5f,  1.0f, 0.0f, 1.0f, // 5
         0.5f,  0.5f, -0.5f,  1.0f, 1.0f, 1.0f, // 6
        -0.5f,  0.5f, -0.5f,  0.0f, 0.0f, 0.0f  // 7
    };
    
    // CCW Winding
    uint32_t indices[] = {
        0, 1, 2, 2, 3, 0, // Front
        1, 5, 6, 6, 2, 1, // Right
        5, 4, 7, 7, 6, 5, // Back
        4, 0, 3, 3, 7, 4, // Left
        3, 2, 6, 6, 7, 3, // Top
        4, 5, 1, 1, 0, 4  // Bottom
    };
    
    primal::id::id_type meshId = 100;
    primal::id::id_type entityId = 200;
    primal::id::id_type materialId = 300;

    g_Test->mesh = new RenderMesh();
    if (!g_Test->mesh->Create(g_Test->device.get(), meshId, cubeVertices, 8, 24, indices, 36, DataIndexType::UInt32)) {
        std::cerr << "Failed to create RenderMesh" << std::endl;
        return false;
    }

    // 6. Setup Scene & Proxy
    RenderProxy proxy = RenderProxy::Create(entityId, meshId, materialId);
    g_Test->scene.AddProxy(proxy);
    
    // Register Material
    g_Test->renderSystem.RegisterMaterialInstance(materialId, g_Test->materialInstance);

    // 7. Setup View
    // Try standard RH coordinate system: Eye at +Z looking at -Z
    v3 eye = {0.0f, 0.0f, 5.0f}; // Moved to positive Z for standard LookAt
    v3 target = {0.0f, 0.0f, 0.0f};
    v3 up = {0.0f, 1.0f, 0.0f};
    
    m4x4 viewMat = CreateLookAtMatrix(eye, target, up);
    m4x4 projMat = CreatePerspectiveMatrix(60.0f * primal::graphics::rhi::math::constants::DEG_TO_RAD, 1280.0f/720.0f, 0.1f, 100.0f);
    
    // Debug print matrices
    std::cout << "View Matrix:" << std::endl;
    for(int i=0; i<4; i++) {
        for(int j=0; j<4; j++) std::cout << viewMat.columns[i][j] << " ";
        std::cout << std::endl;
    }
    std::cout << "Projection Matrix:" << std::endl;
    for(int i=0; i<4; i++) {
        for(int j=0; j<4; j++) std::cout << projMat.columns[i][j] << " ";
        std::cout << std::endl;
    }

    g_Test->view.SetViewMatrix(viewMat);
    g_Test->view.SetProjectionMatrix(projMat);
    
    return true;
}

void Engine_Test::run() {
    if (!g_Test) return;

    // Create an autorelease pool for this frame to ensure Metal resources are released immediately
    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();

    if (!g_Test->window.is_valid()) {
        pool->release();
        return;
    }

    // Poll events (Assuming platform::window has some mechanism, or we just rely on OS event loop in real app)
    // Since we don't have a clear poll API exposed in window, we might need to assume it's handled or this is a blocking test.
    // But for integration test loop:
    
    // Update Rotation using Delta Time
    static auto lastTime = std::chrono::high_resolution_clock::now();
    static int frameCount = 0;
    static double accumulatedTime = 0.0;
    
    auto currentTime = std::chrono::high_resolution_clock::now();
    std::chrono::duration<float> deltaTime = currentTime - lastTime;
    lastTime = currentTime;
    
    // Log FPS every second
    accumulatedTime += deltaTime.count();
    frameCount++;
    
    // Detect slow frames (stuttering)
    /*
    if (deltaTime.count() > 0.020f) { // > 20ms
        std::cout << "[Performance] Slow frame detected: " << deltaTime.count() * 1000.0f << " ms (Frame " << frameCount << ")" << std::endl;
    }
    */

    if (accumulatedTime >= 1.0) {
        // std::cout << "FPS: " << frameCount << " | Frame Time: " << (accumulatedTime / frameCount) * 1000.0 << " ms" << std::endl;
        frameCount = 0;
        accumulatedTime = 0.0;
    }

    // Rotation speed: 60 degrees per second (approx 1.0 radian/sec)
    float rotationSpeed = 2.0f; 
    
    // Smooth delta time to avoid visual stuttering from jittery frame times
    // Simple exponential moving average (EMA)
    static float smoothedDeltaTime = 0.016f;
    smoothedDeltaTime = smoothedDeltaTime * 0.9f + deltaTime.count() * 0.1f;
    
    // Use smoothed delta time for animation update, but bound it to avoid spiraling
    float animationDelta = smoothedDeltaTime;
    if (animationDelta > 0.05f) animationDelta = 0.05f; // Cap at 50ms to prevent huge jumps
    
    g_Test->rotationAngle += rotationSpeed * animationDelta;
    
    // Log rotation angle every 60 frames to verify smoothness
    if (frameCount % 60 == 0) {
        // std::cout << "[Anim] Angle: " << g_Test->rotationAngle << " (Delta: " << animationDelta * 1000.0f << "ms)" << std::endl;
    }
    
    m4x4 modelMat = CreateRotationMatrixY(g_Test->rotationAngle);
    
    // Log Model Matrix for debugging rotation smoothness
    std::cout << "[Matrix] Frame " << frameCount << " Model[0][0]: " << modelMat.columns[0][0] 
              << " Angle: " << g_Test->rotationAngle << std::endl;
    
    // Update Proxy Transform
    // Note: We need to update the proxy in the scene if we want culling to work correctly with moving objects
    // But here we just update the Uniform which is used for drawing.
    // Wait, RenderSystem uses Proxy transform? 
    // RenderSystem::Render logic:
    // mesh->Draw(cmdBuffer_); 
    // It does NOT use proxy.transform to set push constants or anything (yet).
    // It relies on MaterialInstance uniforms.
    
    // Update Uniforms
    UniformData ubo;
    ubo.viewProjectionMatrix = g_Test->view.GetViewProjectionMatrix(); // View * Proj
    ubo.modelMatrix = modelMat;
    
    // std::cout << "Updating Uniforms. Rotation: " << g_Test->rotationAngle << std::endl;
    
    // Set current frame for MaterialInstance before updating uniforms
    static uint32_t frameIndex = 0;
    uint32_t currentFrame = frameIndex % 3; // MAX_FRAMES_IN_FLIGHT = 3
    
    // Wait for previous frame resources to be available
    auto startWait = std::chrono::high_resolution_clock::now();
    g_Test->renderSystem.Wait(frameIndex);
    auto endWait = std::chrono::high_resolution_clock::now();

    g_Test->materialInstance->SetCurrentFrame(currentFrame);
    g_Test->materialInstance->SetUniformData(0, &ubo, sizeof(UniformData));
    
    // Debug print uniform data
    // std::cout << "Uniform Data Preview:" << std::endl;
    // std::cout << "  ViewProj[0][0]: " << ubo.viewProjectionMatrix.columns[0][0] << std::endl;
    // std::cout << "  Model[0][0]: " << ubo.modelMatrix.columns[0][0] << std::endl;
    
    auto startUpdate = std::chrono::high_resolution_clock::now();
    g_Test->materialInstance->Update(g_Test->device.get()); // Upload to GPU
    auto endUpdate = std::chrono::high_resolution_clock::now();
    
    // Perform Culling to populate visible proxies
    auto startCull = std::chrono::high_resolution_clock::now();
    g_Test->view.Cull(g_Test->scene);
    auto endCull = std::chrono::high_resolution_clock::now();

    // Render
    // std::cout << "Frame Start" << std::endl;
    auto startRender = std::chrono::high_resolution_clock::now();
    g_Test->renderSystem.Render(g_Test->scene, g_Test->view, frameIndex);
    auto endRender = std::chrono::high_resolution_clock::now();
    // std::cout << "Frame End" << std::endl;

    if (deltaTime.count() > 0.020f) {
        std::chrono::duration<double, std::milli> waitTime = endWait - startWait;
        std::chrono::duration<double, std::milli> updateTime = endUpdate - startUpdate;
        std::chrono::duration<double, std::milli> cullTime = endCull - startCull;
        std::chrono::duration<double, std::milli> renderTime = endRender - startRender;
        
        std::cout << "[Performance Detail] Frame " << frameCount << " Breakdown:" << std::endl;
        std::cout << "  - Wait: " << waitTime.count() << " ms" << std::endl;
        std::cout << "  - Update: " << updateTime.count() << " ms" << std::endl;
        std::cout << "  - Cull (External): " << cullTime.count() << " ms" << std::endl;
        std::cout << "  - Render: " << renderTime.count() << " ms" << std::endl;
    }

    frameIndex++;

    // Release the pool at the end of the frame
    auto startPool = std::chrono::high_resolution_clock::now();
    pool->release();
    auto endPool = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> poolTime = endPool - startPool;
    if (poolTime.count() > 0.1) {
         std::cout << "[Performance] pool->release() slow: " << poolTime.count() << " ms (Frame " << frameIndex - 1 << ")" << std::endl;
    }

    if (frameIndex > 600) {
        // Stop after 600 frames (approx 10 seconds)
        std::cout << "[Test] 600 frames reached. Shutting down..." << std::endl;
        
        // Proper shutdown to ensure resources are released before allocator destruction
        Engine_Test::shutdown();
        
        exit(0);
    }
}

void Engine_Test::shutdown() {
    if (g_Test) {
        // Manually destroy GPU resources before deleting objects
        // Must be done before RenderSystem Shutdown or Device destruction
        if (g_Test->mesh) g_Test->mesh->Destroy(g_Test->device.get());
        
        // MaterialInstance and Material destructors handle their own cleanup
        // assuming they have valid device pointer cached.
        
        g_Test->renderSystem.Shutdown();
        
        if (g_Test->window.is_valid()) {
            platform::remove_window(g_Test->window.get_id());
        }

        delete g_Test->mesh;
        g_Test->mesh = nullptr;
        delete g_Test->materialInstance;
        g_Test->materialInstance = nullptr;
        delete g_Test->material;
        g_Test->material = nullptr;
        
        g_Test.reset();
    }
}

#endif // __APPLE__
