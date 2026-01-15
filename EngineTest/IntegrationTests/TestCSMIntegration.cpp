/**
 * @file TestCSMIntegration.cpp
 * @brief CSM (Cascaded Shadow Maps) 集成测试
 * @details 验证 CSM 阴影渲染流程，包括 Shadow Pass 和新的 CSM Shader
 * @author GameEngine VulkanCPP Team
 * @date 2026-01-15
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

struct Engine_Test_Impl {
    platform::window window;
    RenderSystem renderSystem;
    RenderScene scene;
    RenderView view;
    
    RenderMesh* cubeMesh = nullptr;
    Material* material = nullptr;
    MaterialInstance* materialInstance = nullptr;
    
    // Floor Material (Same shader, different instance)
    MaterialInstance* floorMaterialInstance = nullptr;
    
    // Keep resources alive
    std::unique_ptr<RHIDeviceBase> device;
    rhi::DescriptorSetLayoutHandle dsLayout = rhi::handles::INVALID_RESOURCE;
    
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
    info.caption = "CSM Integration Test";
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
    attrs.push_back({0, 0, DataFormat::RGB32_Float, 0});  // Position
    attrs.push_back({1, 0, DataFormat::RGB32_Float, 12}); // Color
    attrs.push_back({2, 0, DataFormat::RGB32_Float, 24}); // Normal
    g_Test->material->SetVertexAttributes(attrs);
    
    utl::vector<VertexInputBinding> bindings;
    bindings.push_back({0, 36, true}); // Stride = 36 (3*float3), perVertex = true
    g_Test->material->SetVertexBindings(bindings);
    
    // Load CSM Shader
    std::string shaderSource = ReadShaderFile("/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/TestShader_CSM.metal");
    if (shaderSource.empty()) {
        return false;
    }
    
    g_Test->material->SetShader(ShaderStage::Vertex, shaderSource.c_str(), shaderSource.length(), "vertexMain");
    g_Test->material->SetShader(ShaderStage::Pixel, shaderSource.c_str(), shaderSource.length(), "fragment_main");
    
    // Set Formats for Pipeline Creation
    utl::vector<DataFormat> colorFormats;
    colorFormats.push_back(DataFormat::BGRA8_UNorm);
    g_Test->material->SetRenderTargetFormats(colorFormats, DataFormat::D32_Float);
    
    // Configure Rasterizer State
    RasterizerState rasterState{};
    rasterState.cullMode = CullMode::None; // Disable culling to debug visibility
    rasterState.depthBias = 0.0f; // Disable depth bias for now
    rasterState.slopeScaledDepthBias = 0.0f;
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
    layoutBinding.stageFlags = rhi::ShaderStage::Pixel; 
    
    rhi::DescriptorSetLayoutDesc layoutDesc{};
    layoutDesc.bindings = &layoutBinding;
    layoutDesc.bindingCount = 1;
    
    rhi::DescriptorSetLayoutHandle dsLayout = g_Test->device->CreateDescriptorSetLayout(layoutDesc);
    if (dsLayout == rhi::handles::INVALID_RESOURCE) {
        std::cerr << "Failed to create DescriptorSetLayout" << std::endl;
        return false;
    }
    g_Test->dsLayout = dsLayout;
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

    // Create Instance for Cube (White)
    g_Test->materialInstance = new MaterialInstance(g_Test->material);
    if (!g_Test->materialInstance->Initialize(g_Test->device.get())) {
        std::cerr << "Failed to initialize MaterialInstance" << std::endl;
        return false;
    }
    MaterialUniformData cubeUniforms{1.0f, 1.0f, 1.0f, 1.0f};
    g_Test->materialInstance->SetUniformData(0, &cubeUniforms, sizeof(MaterialUniformData));
    
    // Create Instance for Floor (Light Grey)
    g_Test->floorMaterialInstance = new MaterialInstance(g_Test->material);
    if (!g_Test->floorMaterialInstance->Initialize(g_Test->device.get())) {
        std::cerr << "Failed to initialize Floor MaterialInstance" << std::endl;
        return false;
    }
    MaterialUniformData floorUniforms{0.7f, 0.7f, 0.7f, 1.0f};
    g_Test->floorMaterialInstance->SetUniformData(0, &floorUniforms, sizeof(MaterialUniformData));

    primal::id::id_type cubeMaterialId = 300;
    primal::id::id_type floorMaterialId = 301;
    
    // Register Materials
    g_Test->renderSystem.RegisterMaterialInstance(cubeMaterialId, g_Test->materialInstance);
    g_Test->renderSystem.RegisterMaterialInstance(floorMaterialId, g_Test->floorMaterialInstance);

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
    
    uint32_t indices[] = {
        0, 1, 2, 2, 3, 0,       // Front
        4, 5, 6, 6, 7, 4,       // Back
        8, 9, 10, 10, 11, 8,    // Right
        12, 13, 14, 14, 15, 12, // Left
        16, 17, 18, 18, 19, 16, // Top
        20, 21, 22, 22, 23, 20  // Bottom
    };
    
    primal::id::id_type meshId = 100;

    g_Test->cubeMesh = new RenderMesh();
    if (!g_Test->cubeMesh->Create(g_Test->device.get(), meshId, cubeVertices, 24, 36, indices, 36, DataIndexType::UInt32)) {
        std::cerr << "Failed to create RenderMesh" << std::endl;
        return false;
    }

    // 6. Setup Scene
    
    // 6.1 Floor Plane (Scaled Cube)
    primal::id::id_type floorEntityId = 200;
    RenderProxy floorProxy = RenderProxy::Create(floorEntityId, meshId, floorMaterialId);
    // Scale then Translate (T * S) to avoid scaling the translation
    floorProxy.transform = CreateTranslationMatrix(v3{0.0f, -2.0f, 0.0f}) * CreateScaleMatrix(v3{10.0f, 0.1f, 10.0f});
    floorProxy.worldAABB = rhi::AABB(v3{-5.0f, -2.1f, -5.0f}, v3{5.0f, -1.9f, 5.0f});
    g_Test->scene.AddProxy(floorProxy);
    
    // 6.2 Floating Cube (Shadow Caster)
    primal::id::id_type cubeEntityId = 201;
    RenderProxy cubeProxy = RenderProxy::Create(cubeEntityId, meshId, cubeMaterialId);
    cubeProxy.transform = CreateTranslationMatrix(v3{0.0f, 1.0f, 0.0f});
    cubeProxy.worldAABB = rhi::AABB(v3{-0.5f, 0.5f, -0.5f}, v3{0.5f, 1.5f, 0.5f});
    g_Test->scene.AddProxy(cubeProxy);

    // 6.3 Directional Light
    RenderLight directionalLight;
    directionalLight.type = LightType::Directional;
    directionalLight.direction = {0.5f, -1.0f, 0.5f}; // Slanted light
    float len = sqrt(directionalLight.direction.x*directionalLight.direction.x + directionalLight.direction.y*directionalLight.direction.y + directionalLight.direction.z*directionalLight.direction.z);
    directionalLight.direction.x /= len;
    directionalLight.direction.y /= len;
    directionalLight.direction.z /= len;
    
    directionalLight.color = {1.0f, 1.0f, 1.0f};
    directionalLight.intensity = 2.0f; // Increase intensity
    g_Test->scene.AddLight(directionalLight);

    // Register Materials (Done above)

    // 7. Setup View
    v3 eye = {0.0f, 3.0f, 6.0f}; 
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

    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();

    if (!g_Test->window.is_valid()) {
        pool->release();
        return;
    }
    
    // Animation
    static auto lastTime = std::chrono::high_resolution_clock::now();
    static float totalTime = 0.0f;
    
    auto currentTime = std::chrono::high_resolution_clock::now();
    std::chrono::duration<float> deltaTime = currentTime - lastTime;
    lastTime = currentTime;
    totalTime += deltaTime.count();
    
    // Rotate the floating cube
    g_Test->rotationAngle += deltaTime.count() * 1.0f;
    
    if (g_Test->scene.GetProxies().size() > 1) {
        RenderProxy cubeProxy = g_Test->scene.GetProxies()[1];
        
        // Rotate and move up/down
        m4x4 rot = CreateRotationMatrixY(g_Test->rotationAngle);
        m4x4 trans = CreateTranslationMatrix(v3{0.0f, 1.0f + sin(totalTime) * 0.5f, 0.0f});
        
        // Order: Translate * Rotate
        cubeProxy.transform = trans * rot;
        
        // Update AABB (approximate)
        cubeProxy.worldAABB = rhi::AABB(v3{-1.0f, -1.0f, -1.0f}, v3{1.0f, 1.0f, 1.0f});
        
        g_Test->scene.UpdateProxy(cubeProxy.entityId, cubeProxy);
    }
    
    // Frame Index
    static uint32_t frameIndex = 0;
    frameIndex = (frameIndex + 1) % primal::graphics::rhi::MAX_FRAMES_IN_FLIGHT;
    
    g_Test->renderSystem.Wait(frameIndex);

    // Update Material Uniforms
    MaterialUniformData cubeUniformData{1.0f, 1.0f, 1.0f, 1.0f};
    MaterialUniformData floorUniformData{0.7f, 0.7f, 0.7f, 1.0f};
    
    g_Test->materialInstance->SetCurrentFrame(frameIndex);
    g_Test->materialInstance->SetUniformData(0, &cubeUniformData, sizeof(MaterialUniformData));
    g_Test->materialInstance->Update(g_Test->device.get());

    g_Test->floorMaterialInstance->SetCurrentFrame(frameIndex);
    g_Test->floorMaterialInstance->SetUniformData(0, &floorUniformData, sizeof(MaterialUniformData));
    g_Test->floorMaterialInstance->Update(g_Test->device.get());
    
    // Perform Culling
    g_Test->view.Cull(g_Test->scene);
    
    // Render
    g_Test->renderSystem.Render(g_Test->scene, g_Test->view, frameIndex);
    
    pool->release();
}

void Engine_Test::shutdown() {
    if (g_Test) {
        // Wait for device to be idle
        if (g_Test->device) {
            g_Test->device->WaitIdle();
        }

        // Destroy Mesh Resources
        if (g_Test->cubeMesh) {
            g_Test->cubeMesh->Destroy(g_Test->device.get());
            delete g_Test->cubeMesh;
            g_Test->cubeMesh = nullptr;
        }
        
        // Destroy Material Instances
        if (g_Test->materialInstance) {
            delete g_Test->materialInstance;
            g_Test->materialInstance = nullptr;
        }
        if (g_Test->floorMaterialInstance) {
            delete g_Test->floorMaterialInstance;
            g_Test->floorMaterialInstance = nullptr;
        }
        
        // Destroy Material
        if (g_Test->material) {
            delete g_Test->material;
            g_Test->material = nullptr;
        }

        // Destroy DescriptorSetLayout
        if (g_Test->dsLayout != rhi::handles::INVALID_RESOURCE && g_Test->device) {
            g_Test->device->DestroyDescriptorSetLayout(g_Test->dsLayout);
            g_Test->dsLayout = rhi::handles::INVALID_RESOURCE;
        }
        
        // RenderSystem Shutdown
        g_Test->renderSystem.Shutdown();
        
        // Window Shutdown
        if (g_Test->window.is_valid()) {
            platform::remove_window(g_Test->window.get_id());
        }
        
        // Device shutdown via unique_ptr reset in g_Test.reset()
        g_Test.reset();
    }
}

#endif
