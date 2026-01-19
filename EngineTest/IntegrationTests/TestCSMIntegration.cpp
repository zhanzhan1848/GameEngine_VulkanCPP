#include "TestCSMIntegration.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalDevice.h"
#include "Engine/Graphics/RHI/Core/RHIMath.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalMath.h"
#include <iostream>
#include <fstream>
#include <sstream>

using namespace primal;
using namespace primal::graphics;
using namespace primal::graphics::rhi;
using namespace primal::graphics::rhi::math;

// Material Uniform Data (Color)
struct MaterialUniformData {
    float r, g, b, a;
};

// Engine_Test Implementation
Engine_Test::Engine_Test() 
    : primal::test::RenderTestRunner(std::make_unique<CSMIntegrationTestCase>()) 
{}

// Helper Implementation
std::string CSMIntegrationTestCase::ReadShaderFile(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        std::cerr << "Failed to open shader file: " << filepath << std::endl;
        return "";
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// CSMIntegrationTestCase Implementation
bool CSMIntegrationTestCase::Initialize() {
    std::cout << "Initializing CSM Integration Test..." << std::endl;
    
    // 1. Create Window
    platform::window_init_info info{};
    info.caption = "CSM Integration Test";
    info.width = 1280;
    info.height = 720;
    window = platform::create_window(&info);
    
    if (!window.is_valid()) {
        std::cerr << "Failed to create window" << std::endl;
        return false;
    }

    // 2. Create Device
    DeviceDesc deviceDesc{};
    deviceDesc.platform = RHIPlatform::Metal;
    deviceDesc.enableDebug = true; // Enable debug for tests
    auto metalDevice = std::make_unique<MetalDevice>(deviceDesc);
    if (!metalDevice->Initialize()) {
        std::cerr << "Failed to initialize device" << std::endl;
        return false;
    }
    device = std::move(metalDevice);

    // 3. Initialize RenderSystem
    RenderSystemInitInfo sysInfo;
    sysInfo.device = device.get();
    sysInfo.window = window.handle();
    sysInfo.width = info.width;
    sysInfo.height = info.height;
    
    if (!renderSystem.Initialize(sysInfo)) {
        std::cerr << "Failed to initialize RenderSystem" << std::endl;
        return false;
    }

    // 4. Create Material & Shader
    material = new Material();
    
    // Vertex Attributes
    utl::vector<VertexInputAttribute> attrs;
    attrs.push_back({0, 0, DataFormat::RGB32_Float, 0});  // Position
    attrs.push_back({1, 0, DataFormat::RGB32_Float, 12}); // Color
    attrs.push_back({2, 0, DataFormat::RGB32_Float, 24}); // Normal
    material->SetVertexAttributes(attrs);
    
    utl::vector<VertexInputBinding> bindings;
    bindings.push_back({0, 36, true}); // Stride = 36 (3*float3), perVertex = true
    material->SetVertexBindings(bindings);
    
    // Load CSM Shader
    // Note: Assuming absolute path for now as per original code, but ideally should use relative path
    std::string shaderSource = ReadShaderFile("/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/shaders/TestShader_CSM.metal");
    if (shaderSource.empty()) {
        return false;
    }
    
    material->SetShader(ShaderStage::Vertex, shaderSource.c_str(), shaderSource.length(), "vertexMain");
    material->SetShader(ShaderStage::Pixel, shaderSource.c_str(), shaderSource.length(), "fragment_main");

    // Register VSM Shadow Pass Shaders (Permutation 1)
    material->SetShader(ShaderStage::Vertex, shaderSource.c_str(), shaderSource.length(), "vertexShadowVSM", 1);
    material->SetShader(ShaderStage::Pixel, shaderSource.c_str(), shaderSource.length(), "fragmentShadowVSM", 1);
    
    // Set Formats for Pipeline Creation
    utl::vector<DataFormat> colorFormats;
    colorFormats.push_back(DataFormat::BGRA8_UNorm);
    material->SetRenderTargetFormats(colorFormats, DataFormat::D32_Float);
    
    // Configure Rasterizer State
    RasterizerState rasterState{};
    rasterState.cullMode = CullMode::None; // Disable culling to debug visibility
    rasterState.depthBias = 0.0f;
    rasterState.slopeScaledDepthBias = 0.0f;
    material->SetRasterizerState(rasterState);
    
    // Set Depth State
    DepthStencilState depthState{};
    depthState.enableDepthTest = true;
    depthState.enableDepthWrite = true;
    depthState.depthFunc = ComparisonFunc::LessEqual;
    material->SetDepthStencilState(depthState);

    // Setup Uniforms (Material Color)
    material->SetUniformBlockSize(sizeof(MaterialUniformData));
    material->SetUniformBufferBinding(3); // Bind to buffer(3) matching shader

    // Create Descriptor Set Layout for Material (Set 2)
    rhi::DescriptorSetLayoutBinding layoutBinding{};
    layoutBinding.binding = 3;
    layoutBinding.descriptorType = rhi::DescriptorType::UniformBuffer;
    layoutBinding.descriptorCount = 1;
    layoutBinding.stageFlags = rhi::ShaderStage::Pixel; 
    
    rhi::DescriptorSetLayoutDesc layoutDesc{};
    layoutDesc.bindings = &layoutBinding;
    layoutDesc.bindingCount = 1;
    
    dsLayout = device->CreateDescriptorSetLayout(layoutDesc);
    if (dsLayout == rhi::handles::INVALID_RESOURCE) {
        std::cerr << "Failed to create DescriptorSetLayout" << std::endl;
        return false;
    }
    material->SetDescriptorSetLayout(dsLayout);

    // Create Pipeline Layout
    // ForwardRenderer manages Set 0 and Set 1. Material manages Set 2.
    rhi::DescriptorSetLayoutHandle layouts[3];
    layouts[0] = renderSystem.GetRenderer().GetGlobalDescriptorSetLayout();
    layouts[1] = renderSystem.GetRenderer().GetPerObjectDescriptorSetLayout();
    layouts[2] = dsLayout;

    rhi::PipelineLayoutDesc pipelineLayoutDesc{};
    pipelineLayoutDesc.setLayouts = layouts;
    pipelineLayoutDesc.setLayoutCount = 3;
    
    pipelineLayout = device->CreatePipelineLayout(pipelineLayoutDesc);
    material->SetPipelineLayout(pipelineLayout);

    // Create Instance for Cube (White)
    materialInstance = new MaterialInstance(material);
    if (!materialInstance->Initialize(device.get())) {
        std::cerr << "Failed to initialize MaterialInstance" << std::endl;
        return false;
    }
    MaterialUniformData cubeUniforms{1.0f, 1.0f, 1.0f, 1.0f};
    materialInstance->SetUniformData(0, &cubeUniforms, sizeof(MaterialUniformData));
    
    // Create Instance for Floor (Light Grey)
    floorMaterialInstance = new MaterialInstance(material);
    if (!floorMaterialInstance->Initialize(device.get())) {
        std::cerr << "Failed to initialize Floor MaterialInstance" << std::endl;
        return false;
    }
    MaterialUniformData floorUniforms{0.5f, 0.5f, 0.5f, 1.0f};
    floorMaterialInstance->SetUniformData(0, &floorUniforms, sizeof(MaterialUniformData));

    primal::id::id_type cubeMaterialId = 300;
    primal::id::id_type floorMaterialId = 301;
    
    // Register Materials
    renderSystem.RegisterMaterialInstance(cubeMaterialId, materialInstance);
    renderSystem.RegisterMaterialInstance(floorMaterialId, floorMaterialInstance);

    // 5. Create Mesh (Cube with Normals)
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

    cubeMesh = new RenderMesh();
    if (!cubeMesh->Create(device.get(), meshId, cubeVertices, 24, 36, indices, 36, DataIndexType::UInt32)) {
        std::cerr << "Failed to create RenderMesh" << std::endl;
        return false;
    }

    // 6. Setup Scene
    
    // 6.1 Floor Plane (Scaled Cube)
    primal::id::id_type floorEntityId = 200;
    RenderProxy floorProxy = RenderProxy::Create(floorEntityId, meshId, floorMaterialId);
    // Scale then Translate (T * S)
    floorProxy.transform = CreateTranslationMatrix(v3{0.0f, -2.0f, 0.0f}) * CreateScaleMatrix(v3{10.0f, 0.1f, 10.0f});
    floorProxy.worldAABB = rhi::AABB(v3{-5.0f, -2.1f, -5.0f}, v3{5.0f, -1.9f, 5.0f});
    scene.AddProxy(floorProxy);
    
    // 6.2 Floating Cube (Shadow Caster)
    primal::id::id_type cubeEntityId = 201;
    RenderProxy cubeProxy = RenderProxy::Create(cubeEntityId, meshId, cubeMaterialId);
    cubeProxy.transform = CreateTranslationMatrix(v3{0.0f, 1.0f, 0.0f});
    cubeProxy.worldAABB = rhi::AABB(v3{-0.5f, 0.5f, -0.5f}, v3{0.5f, 1.5f, 0.5f});
    scene.AddProxy(cubeProxy);

    // 6.3 Directional Light
    RenderLight directionalLight;
    directionalLight.type = LightType::Directional;
    directionalLight.direction = {0.5f, -1.0f, 0.5f};
    float len = sqrt(directionalLight.direction.x*directionalLight.direction.x + directionalLight.direction.y*directionalLight.direction.y + directionalLight.direction.z*directionalLight.direction.z);
    directionalLight.direction.x /= len;
    directionalLight.direction.y /= len;
    directionalLight.direction.z /= len;
    
    directionalLight.color = {1.0f, 0.95f, 0.8f};
    directionalLight.intensity = 1.0f;
    scene.AddLight(directionalLight);

    // 6.4 Spot Light
    RenderLight spotLight;
    spotLight.type = LightType::Spot;
    spotLight.position = {3.0f, 5.0f, 0.0f};
    spotLight.direction = {0.0f, -1.0f, 0.0f};
    spotLight.range = 15.0f;
    spotLight.outerCone = cos(0.785f); // 45 degrees
    spotLight.innerCone = cos(0.6f);
    spotLight.color = {0.8f, 0.8f, 1.0f};
    spotLight.intensity = 2.0f;
    scene.AddLight(spotLight);

    // 6.5 Point Light
    RenderLight pointLight;
    pointLight.type = LightType::Point;
    pointLight.position = {-3.0f, 2.0f, 0.0f};
    pointLight.range = 10.0f;
    pointLight.color = {0.8f, 1.0f, 0.8f};
    pointLight.intensity = 1.0f;
    scene.AddLight(pointLight);

    // 7. Setup View
    v3 eye = {0.0f, 5.0f, 10.0f};
    v3 target = {0.0f, 0.0f, 0.0f};
    v3 up = {0.0f, 1.0f, 0.0f};
    
    m4x4 viewMat = CreateLookAtMatrix(eye, target, up);
    m4x4 projMat = rhi::metal::CreatePerspectiveMatrix(60.0f * primal::graphics::rhi::math::constants::DEG_TO_RAD, 1280.0f/720.0f, 0.1f, 100.0f);
    
    view.SetViewMatrix(viewMat);
    view.SetProjectionMatrix(projMat);
    
    std::cout << "Initialization successful." << std::endl;
    return true;
}

void CSMIntegrationTestCase::Run() {
    if (!window.is_valid()) return;

    // Animation
    static auto lastTime = std::chrono::high_resolution_clock::now();
    static float totalTime = 0.0f;
    
    auto currentTime = std::chrono::high_resolution_clock::now();
    std::chrono::duration<float> deltaTime = currentTime - lastTime;
    lastTime = currentTime;
    totalTime += deltaTime.count();
    
    // Rotate the floating cube
    rotationAngle += deltaTime.count() * 1.0f;
    
    if (scene.GetProxies().size() > 1) {
        RenderProxy cubeProxy = scene.GetProxies()[1];
        
        // Rotate and move up/down
        m4x4 rot = CreateRotationMatrixY(rotationAngle);
        m4x4 trans = CreateTranslationMatrix(v3{0.0f, 1.0f + sin(totalTime) * 0.5f, 0.0f});
        
        // Order: Translate * Rotate
        cubeProxy.transform = trans * rot;
        
        // Update AABB (approximate)
        cubeProxy.worldAABB = rhi::AABB(v3{-1.0f, -1.0f, -1.0f}, v3{1.0f, 1.0f, 1.0f});
        
        scene.UpdateProxy(cubeProxy.entityId, cubeProxy);
    }
    
    // Get current frame index from RenderSystem
    uint32_t frameIndex = renderSystem.GetCurrentFrameIndex();
    
    // Update Material Uniforms
    MaterialUniformData cubeUniformData{1.0f, 1.0f, 1.0f, 1.0f};
    MaterialUniformData floorUniformData{0.5f, 0.5f, 0.5f, 1.0f};
    
    materialInstance->SetCurrentFrame(frameIndex);
    materialInstance->SetUniformData(0, &cubeUniformData, sizeof(MaterialUniformData));
    materialInstance->Update(device.get());

    floorMaterialInstance->SetCurrentFrame(frameIndex);
    floorMaterialInstance->SetUniformData(0, &floorUniformData, sizeof(MaterialUniformData));
    floorMaterialInstance->Update(device.get());
    
    // Perform Culling
    view.Cull(scene);
    
    // Render (RenderSystem handles BeginFrame/EndFrame/Wait internally)
    renderSystem.Render(scene, view);
}

void CSMIntegrationTestCase::Shutdown() {
    std::cout << "Shutting down..." << std::endl;
    
    // Wait for device to be idle
    if (device) {
        device->WaitIdle();
    }

    // Destroy Mesh Resources
    if (cubeMesh) {
        cubeMesh->Destroy(device.get());
        delete cubeMesh;
        cubeMesh = nullptr;
    }
    
    // Destroy Material Instances
    if (materialInstance) {
        delete materialInstance;
        materialInstance = nullptr;
    }
    if (floorMaterialInstance) {
        delete floorMaterialInstance;
        floorMaterialInstance = nullptr;
    }
    
    // Destroy Material
    if (material) {
        delete material;
        material = nullptr;
    }

    // Destroy DescriptorSetLayout
    if (dsLayout != rhi::handles::INVALID_RESOURCE && device) {
        device->DestroyDescriptorSetLayout(dsLayout);
        dsLayout = rhi::handles::INVALID_RESOURCE;
    }

    // Destroy PipelineLayout
    if (pipelineLayout != rhi::handles::INVALID_RESOURCE && device) {
        device->DestroyPipelineLayout(pipelineLayout);
        pipelineLayout = rhi::handles::INVALID_RESOURCE;
    }
    
    // RenderSystem Shutdown
    renderSystem.Shutdown();
    
    // Window Shutdown
    if (window.is_valid()) {
        platform::remove_window(window.get_id());
    }
    
    // Device shutdown via unique_ptr
    device.reset();
}
