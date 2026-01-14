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
const char* kSimpleColorShader = nullptr; // Loaded from file

struct Engine_Test_Impl {
    platform::window window;
    RenderSystem renderSystem;
    RenderScene scene;
    RenderView view;
    
    RenderMesh* mesh = nullptr;
    Material* material = nullptr;
    MaterialInstance* materialInstance = nullptr;
    
    // Transparent Material
    Material* transparentMaterial = nullptr;
    MaterialInstance* transparentMaterialInstance = nullptr;
    
    // Keep resources alive
    std::unique_ptr<RHIDeviceBase> device;
    
    // Test state
    float rotationAngle = 0.0f;
};

static std::unique_ptr<Engine_Test_Impl> g_Test;

// Material Uniform Data (Color)
struct MaterialUniformData {
    float r, g, b, a;
};

// Helper to read file
std::string ReadShaderFile(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        std::cerr << "Failed to open shader file: " << filepath << std::endl;
        return "";
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

bool Engine_Test::initialize() {
    g_Test = std::make_unique<Engine_Test_Impl>();
    
    // 1. Create Window
    platform::window_init_info info{};
    info.caption = "RenderSystem Integration Test (Lighting)";
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
    // 0: Position (float3)
    // 1: Color (float3)
    // 2: Normal (float3)
    utl::vector<VertexInputAttribute> attrs;
    attrs.push_back({0, 0, DataFormat::RGB32_Float, 0});  // Position
    attrs.push_back({1, 0, DataFormat::RGB32_Float, 12}); // Color
    attrs.push_back({2, 0, DataFormat::RGB32_Float, 24}); // Normal
    g_Test->material->SetVertexAttributes(attrs);
    
    utl::vector<VertexInputBinding> bindings;
    bindings.push_back({0, 36, true}); // Stride = 36 (3*float3), perVertex = true
    g_Test->material->SetVertexBindings(bindings);
    
    // Shader
    std::string shaderSource = ReadShaderFile("/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/TestShader_Lighting.metal");
    if (shaderSource.empty()) {
        return false;
    }
    
    g_Test->material->SetShader(ShaderStage::Vertex, shaderSource.c_str(), shaderSource.length(), "vertexMain");
    g_Test->material->SetShader(ShaderStage::Pixel, shaderSource.c_str(), shaderSource.length(), "fragmentMain");
    
    // Set Formats for Pipeline Creation
    utl::vector<DataFormat> colorFormats;
    colorFormats.push_back(DataFormat::BGRA8_UNorm);
    g_Test->material->SetRenderTargetFormats(colorFormats, DataFormat::D32_Float);
    
    // Set Rasterizer State (Back face culling)
    RasterizerState rasterState{};
    rasterState.cullMode = CullMode::Back; // DEBUG: Disable Culling
    g_Test->material->SetRasterizerState(rasterState);
    
    // Set Depth State
    DepthStencilState depthState{};
    depthState.enableDepthTest = true;
    depthState.enableDepthWrite = true;
    depthState.depthFunc = ComparisonFunc::LessEqual;
    g_Test->material->SetDepthStencilState(depthState);

    // Setup Uniforms (Material Color)
    g_Test->material->SetUniformBlockSize(sizeof(MaterialUniformData));
    g_Test->material->SetUniformBufferBinding(3); // Bind to buffer(3) matching shader

    // Create Descriptor Set Layout for Material (Set 2)
    rhi::DescriptorSetLayoutBinding layoutBinding{};
    layoutBinding.binding = 3;
    layoutBinding.descriptorType = rhi::DescriptorType::UniformBuffer;
    layoutBinding.descriptorCount = 1;
    layoutBinding.stageFlags = rhi::ShaderStage::Pixel; // Only used in Fragment shader
    
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
    // ForwardRenderer manages Set 0 and Set 1. Material manages Set 2.
    // We need to provide layouts for all 3 sets to ensure correct binding indices.
    rhi::DescriptorSetLayoutHandle layouts[3];
    layouts[0] = g_Test->renderSystem.GetRenderer().GetGlobalDescriptorSetLayout();
    layouts[1] = g_Test->renderSystem.GetRenderer().GetPerObjectDescriptorSetLayout();
    layouts[2] = dsLayout;

    rhi::PipelineLayoutDesc pipelineLayoutDesc{};
    pipelineLayoutDesc.setLayouts = layouts;
    pipelineLayoutDesc.setLayoutCount = 3;
    
    rhi::PipelineLayoutHandle pipelineLayout = g_Test->device->CreatePipelineLayout(pipelineLayoutDesc);
    g_Test->material->SetPipelineLayout(pipelineLayout);

    g_Test->materialInstance = new MaterialInstance(g_Test->material);
    if (!g_Test->materialInstance->Initialize(g_Test->device.get())) {
        std::cerr << "Failed to initialize MaterialInstance" << std::endl;
        return false;
    }

    // 4.1 Create Transparent Material
    g_Test->transparentMaterial = new Material();
    g_Test->transparentMaterial->SetVertexAttributes(attrs);
    g_Test->transparentMaterial->SetVertexBindings(bindings);
    
    // Reuse Shader
    g_Test->transparentMaterial->SetShader(ShaderStage::Vertex, shaderSource.c_str(), shaderSource.length(), "vertexMain");
    g_Test->transparentMaterial->SetShader(ShaderStage::Pixel, shaderSource.c_str(), shaderSource.length(), "fragmentMain");
    
    g_Test->transparentMaterial->SetRenderTargetFormats(colorFormats, DataFormat::D32_Float);
    g_Test->transparentMaterial->SetRasterizerState(rasterState);
    
    // Depth State (Read ON, Write OFF for Transparent)
    DepthStencilState transDepthState = depthState;
    transDepthState.enableDepthWrite = false;
    g_Test->transparentMaterial->SetDepthStencilState(transDepthState);
    
    // Transparent Material Blend State
    BlendState blendState{};
    blendState.enableBlend = true;
    blendState.srcColorBlendFactor = BlendFactor::SrcAlpha;
    blendState.dstColorBlendFactor = BlendFactor::InvSrcAlpha;
    blendState.colorBlendOp = BlendOp::Add;
    blendState.srcAlphaBlendFactor = BlendFactor::One;
    blendState.dstAlphaBlendFactor = BlendFactor::InvSrcAlpha;
    blendState.alphaBlendOp = BlendOp::Add;
    // blendState.colorWriteMask = ColorWriteMask::All;
    g_Test->transparentMaterial->SetBlendState(blendState);
    
    g_Test->transparentMaterial->SetUniformBlockSize(sizeof(MaterialUniformData));
    g_Test->transparentMaterial->SetUniformBufferBinding(3);
    g_Test->transparentMaterial->SetDescriptorSetLayout(dsLayout); // Reuse Layout
    g_Test->transparentMaterial->SetPipelineLayout(pipelineLayout); // Reuse Layout

    g_Test->transparentMaterialInstance = new MaterialInstance(g_Test->transparentMaterial);
    if (!g_Test->transparentMaterialInstance->Initialize(g_Test->device.get())) {
        std::cerr << "Failed to initialize Transparent MaterialInstance" << std::endl;
        return false;
    }

    // 5. Create Mesh (Cube with Normals)
    // 24 vertices (4 per face x 6 faces)
    // Format: Pos(3), Color(3), Normal(3)
    float cubeVertices[] = {
        // Front Face (Z+)
        -0.5f, -0.5f,  0.5f,  1.0f, 0.0f, 0.0f,  0.0f, 0.0f, 1.0f,
         0.5f, -0.5f,  0.5f,  1.0f, 0.0f, 0.0f,  0.0f, 0.0f, 1.0f,
         0.5f,  0.5f,  0.5f,  1.0f, 0.0f, 0.0f,  0.0f, 0.0f, 1.0f,
        -0.5f,  0.5f,  0.5f,  1.0f, 0.0f, 0.0f,  0.0f, 0.0f, 1.0f,

        // Back Face (Z-)
         0.5f, -0.5f, -0.5f,  0.0f, 1.0f, 0.0f,  0.0f, 0.0f, -1.0f,
        -0.5f, -0.5f, -0.5f,  0.0f, 1.0f, 0.0f,  0.0f, 0.0f, -1.0f,
        -0.5f,  0.5f, -0.5f,  0.0f, 1.0f, 0.0f,  0.0f, 0.0f, -1.0f,
         0.5f,  0.5f, -0.5f,  0.0f, 1.0f, 0.0f,  0.0f, 0.0f, -1.0f,

        // Right Face (X+)
         0.5f, -0.5f,  0.5f,  0.0f, 0.0f, 1.0f,  1.0f, 0.0f, 0.0f,
         0.5f, -0.5f, -0.5f,  0.0f, 0.0f, 1.0f,  1.0f, 0.0f, 0.0f,
         0.5f,  0.5f, -0.5f,  0.0f, 0.0f, 1.0f,  1.0f, 0.0f, 0.0f,
         0.5f,  0.5f,  0.5f,  0.0f, 0.0f, 1.0f,  1.0f, 0.0f, 0.0f,

        // Left Face (X-)
        -0.5f, -0.5f, -0.5f,  1.0f, 1.0f, 0.0f,  -1.0f, 0.0f, 0.0f,
        -0.5f, -0.5f,  0.5f,  1.0f, 1.0f, 0.0f,  -1.0f, 0.0f, 0.0f,
        -0.5f,  0.5f,  0.5f,  1.0f, 1.0f, 0.0f,  -1.0f, 0.0f, 0.0f,
        -0.5f,  0.5f, -0.5f,  1.0f, 1.0f, 0.0f,  -1.0f, 0.0f, 0.0f,

        // Top Face (Y+)
        -0.5f,  0.5f,  0.5f,  0.0f, 1.0f, 1.0f,  0.0f, 1.0f, 0.0f,
         0.5f,  0.5f,  0.5f,  0.0f, 1.0f, 1.0f,  0.0f, 1.0f, 0.0f,
         0.5f,  0.5f, -0.5f,  0.0f, 1.0f, 1.0f,  0.0f, 1.0f, 0.0f,
        -0.5f,  0.5f, -0.5f,  0.0f, 1.0f, 1.0f,  0.0f, 1.0f, 0.0f,

        // Bottom Face (Y-)
        -0.5f, -0.5f, -0.5f,  1.0f, 0.0f, 1.0f,  0.0f, -1.0f, 0.0f,
         0.5f, -0.5f, -0.5f,  1.0f, 0.0f, 1.0f,  0.0f, -1.0f, 0.0f,
         0.5f, -0.5f,  0.5f,  1.0f, 0.0f, 1.0f,  0.0f, -1.0f, 0.0f,
        -0.5f, -0.5f,  0.5f,  1.0f, 0.0f, 1.0f,  0.0f, -1.0f, 0.0f,
    };
    
    // CCW Winding
    uint32_t indices[] = {
        0, 1, 2, 2, 3, 0,       // Front
        4, 5, 6, 6, 7, 4,       // Back
        8, 9, 10, 10, 11, 8,    // Right
        12, 13, 14, 14, 15, 12, // Left
        16, 17, 18, 18, 19, 16, // Top
        20, 21, 22, 22, 23, 20  // Bottom
    };
    
    primal::id::id_type meshId = 100;
    primal::id::id_type entityId = 200;
    primal::id::id_type materialId = 300;

    g_Test->mesh = new RenderMesh();
    if (!g_Test->mesh->Create(g_Test->device.get(), meshId, cubeVertices, 24, 36, indices, 36, DataIndexType::UInt32)) {
        std::cerr << "Failed to create RenderMesh" << std::endl;
        return false;
    }

    // 6. Setup Scene & Proxy
    RenderProxy proxy = RenderProxy::Create(entityId, meshId, materialId);
    g_Test->scene.AddProxy(proxy);

    // Second Proxy (Moving Cube)
    primal::id::id_type entityId2 = 201;
    RenderProxy proxy2 = RenderProxy::Create(entityId2, meshId, materialId);
    // Initial transform off-screen
    proxy2.transform = CreateTranslationMatrix(primal::math::v3{10.0f, 0.0f, 0.0f}); 
    g_Test->scene.AddProxy(proxy2);
    
    // Add Light
    RenderLight directionalLight;
    directionalLight.type = LightType::Directional;
    directionalLight.direction = {1.0f, -1.0f, -0.5f}; // Pointing down-right-back
    // Normalize direction
    float len = sqrt(directionalLight.direction.x*directionalLight.direction.x + directionalLight.direction.y*directionalLight.direction.y + directionalLight.direction.z*directionalLight.direction.z);
    directionalLight.direction.x /= len;
    directionalLight.direction.y /= len;
    directionalLight.direction.z /= len;
    
    directionalLight.color = {1.0f, 1.0f, 1.0f};
    directionalLight.intensity = 0.8f;
    g_Test->scene.AddLight(directionalLight);

    // Add Point Light
    RenderLight pointLight;
    pointLight.type = LightType::Point;
    pointLight.position = {2.0f, 0.0f, 0.0f};
    pointLight.color = {1.0f, 0.0f, 0.0f}; // Red Light
    pointLight.intensity = 5.0f;
    pointLight.range = 5.0f;
    g_Test->scene.AddLight(pointLight);

    // Transparent Proxies
    primal::id::id_type transMaterialId = 301;
    
    // Transparent Proxy 1 (Left-Up)
    primal::id::id_type entityId3 = 202;
    RenderProxy proxy3 = RenderProxy::Create(entityId3, meshId, transMaterialId);
    proxy3.transform = CreateTranslationMatrix(primal::math::v3{-3.0f, 2.0f, 0.0f}); 
    g_Test->scene.AddProxy(proxy3);

    // Transparent Proxy 2 (Right-Down)
    primal::id::id_type entityId4 = 203;
    RenderProxy proxy4 = RenderProxy::Create(entityId4, meshId, transMaterialId);
    proxy4.transform = CreateTranslationMatrix(primal::math::v3{3.0f, -2.0f, 0.0f}); 
    g_Test->scene.AddProxy(proxy4);

    // Register Material
    g_Test->renderSystem.RegisterMaterialInstance(materialId, g_Test->materialInstance);
    g_Test->renderSystem.RegisterMaterialInstance(transMaterialId, g_Test->transparentMaterialInstance);

    // 7. Setup View
    v3 eye = {0.0f, 2.0f, 5.0f}; 
    v3 target = {0.0f, 0.0f, 0.0f};
    v3 up = {0.0f, 1.0f, 0.0f};
    
    m4x4 viewMat = CreateLookAtMatrix(eye, target, up);
    m4x4 projMat = CreatePerspectiveMatrix(60.0f * primal::graphics::rhi::math::constants::DEG_TO_RAD, 1280.0f/720.0f, 0.1f, 100.0f);
    
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
    static double totalTime = 0.0;
    
    auto currentTime = std::chrono::high_resolution_clock::now();
    std::chrono::duration<float> deltaTime = currentTime - lastTime;
    lastTime = currentTime;
    
    // Log FPS every second
    accumulatedTime += deltaTime.count();
    totalTime += deltaTime.count();
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
    if (!g_Test->scene.GetProxies().empty()) {
        // First Proxy (Rotating at Center)
        RenderProxy proxy = g_Test->scene.GetProxies()[0];
        proxy.transform = modelMat;
        proxy.worldAABB = rhi::AABB(
            rhi::math::v3{-1.0f, -1.0f, -1.0f}, 
            rhi::math::v3{1.0f, 1.0f, 1.0f}
        );
        g_Test->scene.UpdateProxy(proxy.entityId, proxy);
        
        // Second Proxy (Moving left-right)
         if (g_Test->scene.GetProxies().size() > 1) {
             RenderProxy proxy2 = g_Test->scene.GetProxies()[1];
             float offset = sin(totalTime) * 5.0f; // Range [-5, 5] based on totalTime
             proxy2.transform = CreateTranslationMatrix(v3{offset + 3.0f, 0.0f, 0.0f}); // Center at x=3, range [-2, 8]
             
             // Transform AABB manually since AABB::Transform is not fully exposed/trusted here
             rhi::math::v3 center = {offset + 3.0f, 0.0f, 0.0f};
            proxy2.worldAABB = rhi::AABB(
                rhi::math::v3{center.x - 1.0f, center.y - 1.0f, center.z - 1.0f}, 
                rhi::math::v3{center.x + 1.0f, center.y + 1.0f, center.z + 1.0f}
            );
            g_Test->scene.UpdateProxy(proxy2.entityId, proxy2);
        }

        // Transparent Proxies Update
        if (g_Test->scene.GetProxies().size() > 3) {
             // Proxy 3 (Index 2) - Up/Down Sine Wave
             RenderProxy proxy3 = g_Test->scene.GetProxies()[2];
             float yOffset = sin(totalTime * 2.0f) * 1.5f; 
             proxy3.transform = CreateTranslationMatrix(v3{-3.0f, 2.0f + yOffset, 0.0f});
             
             // Update AABB
             rhi::math::v3 center3 = {-3.0f, 2.0f + yOffset, 0.0f};
             proxy3.worldAABB = rhi::AABB(
                rhi::math::v3{center3.x - 1.0f, center3.y - 1.0f, center3.z - 1.0f}, 
                rhi::math::v3{center3.x + 1.0f, center3.y + 1.0f, center3.z + 1.0f}
             );
             g_Test->scene.UpdateProxy(proxy3.entityId, proxy3);

             // Proxy 4 (Index 3) - Circle Movement in XZ plane
             RenderProxy proxy4 = g_Test->scene.GetProxies()[3];
             float angle = totalTime * 1.5f;
             float radius = 2.0f;
             float x = 3.0f + cos(angle) * radius;
             float z = sin(angle) * radius;
             proxy4.transform = CreateTranslationMatrix(v3{x, -2.0f, z});
             
             // Update AABB
             rhi::math::v3 center4 = {x, -2.0f, z};
             proxy4.worldAABB = rhi::AABB(
                rhi::math::v3{center4.x - 1.0f, center4.y - 1.0f, center4.z - 1.0f}, 
                rhi::math::v3{center4.x + 1.0f, center4.y + 1.0f, center4.z + 1.0f}
             );
             g_Test->scene.UpdateProxy(proxy4.entityId, proxy4);
        }
    }

    // Set current frame for MaterialInstance before updating uniforms
    static uint32_t frameIndex = 0;
    frameIndex = (frameIndex + 1) % primal::graphics::rhi::MAX_FRAMES_IN_FLIGHT;
    
    // Wait for previous frame resources to be available
    auto startWait = std::chrono::high_resolution_clock::now();
    g_Test->renderSystem.Wait(frameIndex);
    auto endWait = std::chrono::high_resolution_clock::now();

    // Update Uniforms for the Cube
    MaterialUniformData uniformData{};
    uniformData.r = 1.0f;
    uniformData.g = 1.0f;
    uniformData.b = 1.0f;
    uniformData.a = 1.0f;

    g_Test->materialInstance->SetCurrentFrame(frameIndex);
    g_Test->materialInstance->SetUniformData(0, &uniformData, sizeof(MaterialUniformData));
    g_Test->materialInstance->Update(g_Test->device.get());
    
    // Update Uniforms for Transparent Cubes
    MaterialUniformData transUniformData{};
    transUniformData.r = 0.0f;
    transUniformData.g = 1.0f; // Greenish
    transUniformData.b = 1.0f;
    transUniformData.a = 0.5f; // 50% Alpha

    g_Test->transparentMaterialInstance->SetCurrentFrame(frameIndex);
    g_Test->transparentMaterialInstance->SetUniformData(0, &transUniformData, sizeof(MaterialUniformData));
    g_Test->transparentMaterialInstance->Update(g_Test->device.get());

    auto startUpdate = std::chrono::high_resolution_clock::now();
    // Material update handled by ForwardRenderer
    auto endUpdate = std::chrono::high_resolution_clock::now();
    
    // Perform Culling to populate visible proxies
    auto startCull = std::chrono::high_resolution_clock::now();
    g_Test->view.Cull(g_Test->scene);
    auto endCull = std::chrono::high_resolution_clock::now();
    
    if (frameCount % 60 == 0) {
        std::cout << "[Culling] Visible Proxies: " << g_Test->view.GetVisibleProxies().size() 
                  << " / " << g_Test->scene.GetProxies().size() << std::endl;
    }

    if (g_Test->view.GetVisibleProxies().empty()) {
        static int emptyCount = 0;
        if (emptyCount++ % 60 == 0) {
            std::cout << "[Warning] No visible proxies found! Check culling logic." << std::endl;
            // Print Debug Info
            if (!g_Test->scene.GetProxies().empty()) {
                const auto& proxy = g_Test->scene.GetProxies()[0];
                std::cout << "  Proxy AABB: Min(" << proxy.worldAABB.min.x << "," << proxy.worldAABB.min.y << "," << proxy.worldAABB.min.z << ") "
                          << "Max(" << proxy.worldAABB.max.x << "," << proxy.worldAABB.max.y << "," << proxy.worldAABB.max.z << ")" << std::endl;
            }
        }
    }

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

        delete g_Test->transparentMaterialInstance;
        g_Test->transparentMaterialInstance = nullptr;
        delete g_Test->transparentMaterial;
        g_Test->transparentMaterial = nullptr;
        
        g_Test.reset();
    }
}

#endif // __APPLE__
