#include "TestCSMIntegrationRenderGraph.h"
#include "Graphics/RenderPipeline/RenderPasses/Debug/DebugPass.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalDevice.h"
#include "Engine/Graphics/RHI/Core/RHIMath.h"
#include "Engine/Graphics/RHI/Core/RHIShaderCommon.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalMath.h"
#include "Engine/Graphics/RenderMesh.h"
#include "Engine/Input/Input.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstring>
#include <cmath>

using namespace primal;
using namespace primal::graphics;
using namespace primal::graphics::rhi;
using namespace primal::graphics::rhi::math;
using namespace primal::graphics::rendergraph;

// Material Uniform Data (Color)
struct MaterialUniformData {
    float r, g, b, a;
};

// Engine_Test Implementation
Engine_Test::Engine_Test() 
    : primal::test::RenderTestRunner(std::make_unique<CSMIntegrationRenderGraphTestCase>()) 
{}


// Helper Implementation
std::string CSMIntegrationRenderGraphTestCase::ReadShaderFile(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        std::cerr << "Failed to open shader file: " << filepath << std::endl;
        return "";
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

bool CSMIntegrationRenderGraphTestCase::Initialize() {
    std::cout << "Initializing CSM Integration Test (RenderGraph)..." << std::endl;
    
    // 1. Create Window
    std::cout << "Step 1: Create Window" << std::endl;
    platform::window_init_info info{};
    info.caption = "CSM Integration Test (RenderGraph)";
    info.width = 1280;
    info.height = 720;
    window = platform::create_window(&info);
    
    if (!window.is_valid()) {
        std::cerr << "Failed to create window" << std::endl;
        return false;
    }

    // 2. Create Device
    std::cout << "Step 2: Create Device" << std::endl;
    DeviceDesc deviceDesc{};
    deviceDesc.platform = RHIPlatform::Metal;
    deviceDesc.enableDebug = true;
    auto metalDevice = std::make_unique<MetalDevice>(deviceDesc);
    if (!metalDevice->Initialize()) {
        std::cerr << "Failed to initialize device" << std::endl;
        return false;
    }
    device = std::move(metalDevice);

    // 3. Initialize RenderSystem (for SwapChain)
    std::cout << "Step 3: Init RenderSystem" << std::endl;
    RenderSystemInitInfo sysInfo;
    sysInfo.device = device.get();
    sysInfo.window = window.handle();
    sysInfo.width = info.width;
    sysInfo.height = info.height;
    
    if (!renderSystem.Initialize(sysInfo)) {
        std::cerr << "Failed to initialize RenderSystem" << std::endl;
        return false;
    }

    // 4. Initialize RenderGraph
    std::cout << "Step 4: Init RenderGraph" << std::endl;
    renderGraph = std::make_unique<RenderGraph>(*device);

    // 5. Create Material & Shader (Same as TestCSMIntegration)
    std::cout << "Step 5: Create Material" << std::endl;
    material = new Material();
    
    // Vertex Attributes
    utl::vector<VertexInputAttribute> attrs;
    attrs.push_back({0, 0, DataFormat::RGB32_Float, 0});  // Position
    attrs.push_back({1, 0, DataFormat::RGB32_Float, 12}); // Color
    attrs.push_back({2, 0, DataFormat::RGB32_Float, 24}); // Normal
    material->SetVertexAttributes(attrs);
    
    utl::vector<VertexInputBinding> bindings;
    bindings.push_back({0, 36, true}); // Stride = 36
    material->SetVertexBindings(bindings);
    
    // Load Shader Source
    std::string shaderPath = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/TestShader_CSM.metal";
    std::ifstream shaderFile(shaderPath);
    if (!shaderFile.is_open()) {
        std::cerr << "ERROR: Failed to open shader file: " << shaderPath << std::endl;
        return false;
    }
    std::stringstream shaderStream;
    shaderStream << shaderFile.rdbuf();
    std::string shaderSource = shaderStream.str();
    std::cout << "Loaded Shader Source (" << shaderSource.length() << " bytes) from " << shaderPath << std::endl;
    
    // Main Pass Shaders
    material->SetShader(ShaderStage::Vertex, shaderSource.c_str(), shaderSource.length(), "vertexMain", 0);
    material->SetShader(ShaderStage::Pixel, shaderSource.c_str(), shaderSource.length(), "fragment_main", 0);

    // Shadow Pass Shaders (Permutation 1)
    material->SetShader(ShaderStage::Vertex, shaderSource.c_str(), shaderSource.length(), "vertexShadowVSM", 1);
    material->SetShader(ShaderStage::Pixel, shaderSource.c_str(), shaderSource.length(), "fragmentShadowVSM", 1);
    
    // Manually create shadow vertex shader for our custom pipeline
    shadowVS = device->CreateShader(
        shaderSource.c_str(), shaderSource.length(),
        ShaderStage::Vertex, "vertexShadowVSM"
    );
    if (shadowVS == handles::INVALID_SHADER) {
        std::cerr << "Failed to create shadow vertex shader" << std::endl;
        return false;
    }

    shadowPS = device->CreateShader(
        shaderSource.c_str(), shaderSource.length(),
        ShaderStage::Pixel, "fragmentShadowVSM"
    );
    if (shadowPS == handles::INVALID_SHADER) {
        std::cerr << "Failed to create shadow pixel shader" << std::endl;
        return false;
    }
    
    // Formats
    utl::vector<DataFormat> colorFormats;
    colorFormats.push_back(DataFormat::BGRA8_UNorm);
    material->SetRenderTargetFormats(colorFormats, DataFormat::D32_Float);
    
    // States
    RasterizerState rasterState{};
    rasterState.cullMode = CullMode::None;
    material->SetRasterizerState(rasterState);
    
    DepthStencilState depthState{};
    depthState.enableDepthTest = true;
    depthState.enableDepthWrite = true;
    depthState.depthFunc = ComparisonFunc::LessEqual;
    material->SetDepthStencilState(depthState);

    // Uniforms
    material->SetUniformBlockSize(sizeof(MaterialUniformData));
    material->SetUniformBufferBinding(3);

    // 6. Descriptor Layouts
    std::cout << "Step 6: Descriptor Layouts Created" << std::endl;
    
    // Global Set (Set 0)
    DescriptorSetLayoutBinding globalBindings[3];
    globalBindings[0].binding = 11;
    globalBindings[0].descriptorType = DescriptorType::UniformBuffer;
    globalBindings[0].descriptorCount = 1;
    globalBindings[0].stageFlags = ShaderStage::Vertex | ShaderStage::Pixel;

    globalBindings[1].binding = 12;
    globalBindings[1].descriptorType = DescriptorType::UniformBuffer;
    globalBindings[1].descriptorCount = 1;
    globalBindings[1].stageFlags = ShaderStage::Vertex | ShaderStage::Pixel;

    globalBindings[2].binding = 13;
    globalBindings[2].descriptorType = DescriptorType::CombinedImageSampler;
    globalBindings[2].descriptorCount = 1;
    globalBindings[2].stageFlags = ShaderStage::Pixel;

    DescriptorSetLayoutDesc globalSetLayoutDesc;
    globalSetLayoutDesc.bindingCount = 3;
    globalSetLayoutDesc.bindings = globalBindings;
    globalSetLayout = device->CreateDescriptorSetLayout(globalSetLayoutDesc);
    
    // PerObject Set (Set 1)
    DescriptorSetLayoutBinding perObjectBinding;
    perObjectBinding.binding = 10;
    perObjectBinding.descriptorType = DescriptorType::UniformBufferDynamic;
    perObjectBinding.descriptorCount = 1;
    perObjectBinding.stageFlags = ShaderStage::Vertex;

    DescriptorSetLayoutDesc perObjectSetLayoutDesc;
    perObjectSetLayoutDesc.bindingCount = 1;
    perObjectSetLayoutDesc.bindings = &perObjectBinding;
    perObjectSetLayout = device->CreateDescriptorSetLayout(perObjectSetLayoutDesc);
    
    // Calculate perObjectSize
    perObjectSize = (sizeof(PerObjectData) + 255) & ~255;

    // Material Set (Set 2)
    DescriptorSetLayoutBinding materialBinding;
    materialBinding.binding = 3;
    materialBinding.descriptorType = DescriptorType::UniformBuffer;
    materialBinding.descriptorCount = 1;
    materialBinding.stageFlags = ShaderStage::Pixel;

    DescriptorSetLayoutDesc materialSetLayoutDesc;
    materialSetLayoutDesc.bindingCount = 1;
    materialSetLayoutDesc.bindings = &materialBinding;
    materialSetLayout = device->CreateDescriptorSetLayout(materialSetLayoutDesc);
    material->SetDescriptorSetLayout(materialSetLayout);

    // Create Pipeline Layout (Global)
    PipelineLayoutDesc pipelineLayoutDesc{};
    DescriptorSetLayoutHandle layouts[] = { globalSetLayout, perObjectSetLayout, materialSetLayout };
    pipelineLayoutDesc.setLayouts = layouts;
    pipelineLayoutDesc.setLayoutCount = 3;
    pipelineLayout = device->CreatePipelineLayout(pipelineLayoutDesc);
    
    // Create Pipeline Layout (Shadow)
    // Reuse the same layout structure: Set 0 = Global, Set 1 = PerObject
    PipelineLayoutDesc shadowLayoutDesc{};
    DescriptorSetLayoutHandle shadowLayouts[] = { globalSetLayout, perObjectSetLayout };
    shadowLayoutDesc.setLayouts = shadowLayouts;
    shadowLayoutDesc.setLayoutCount = 2;
    shadowPipelineLayout = device->CreatePipelineLayout(shadowLayoutDesc);

    // 7. Create Graphics Pipeline (Shadow Pass)
    std::cout << "Step 7: Create Shadow Pipeline" << std::endl;
    
    GraphicsPipelineDesc shadowDesc{};
    shadowDesc.vertexShader = shadowVS; 
    shadowDesc.pixelShader = shadowPS; 
    shadowDesc.layout = shadowPipelineLayout;
    
    // VSM Formats
    shadowDesc.renderTargetCount = 1;
    shadowDesc.renderTargetFormats[0] = DataFormat::RG32_Float; // Moments
    shadowDesc.depthStencilFormat = DataFormat::D32_Float; // Depth
    
    // Rasterizer State for Shadow
    shadowDesc.fillMode = FillMode::Solid;
    shadowDesc.cullMode = CullMode::None; // Disable culling for debug
    shadowDesc.topology = PrimitiveTopology::TriangleList;
    
    // Depth State
    shadowDesc.enableDepthTest = true;
    shadowDesc.enableDepthWrite = true;
    shadowDesc.depthFunc = ComparisonFunc::Less; // Standard depth test

    // Vertex Attributes
    // Position (v3), Color (v3), Normal (v3) - Match Data and Shader
    shadowDesc.vertexAttributes.push_back(VertexInputAttribute{0, 0, DataFormat::RGB32_Float, 0}); // Position
    shadowDesc.vertexAttributes.push_back(VertexInputAttribute{1, 0, DataFormat::RGB32_Float, 12}); // Color
    shadowDesc.vertexAttributes.push_back(VertexInputAttribute{2, 0, DataFormat::RGB32_Float, 24}); // Normal
    shadowDesc.vertexBindings.push_back(VertexInputBinding{0, 36, true});

    shadowPipeline = device->CreateGraphicsPipeline(shadowDesc);
    if (shadowPipeline == handles::INVALID_PIPELINE) {
        std::cerr << "Failed to create shadow pipeline" << std::endl;
        return false;
    }
    
    // Material Setup for Main Pass (Permutation 0)
    material->SetPipelineLayout(pipelineLayout);
    
    // Set Vertex Attributes for Material (Main Pass)
    utl::vector<VertexInputAttribute> matVertexAttributes;
    matVertexAttributes.push_back(VertexInputAttribute{0, 0, DataFormat::RGB32_Float, 0}); // Position
    matVertexAttributes.push_back(VertexInputAttribute{1, 0, DataFormat::RGB32_Float, 12}); // Color
    matVertexAttributes.push_back(VertexInputAttribute{2, 0, DataFormat::RGB32_Float, 24}); // Normal
    material->SetVertexAttributes(matVertexAttributes);

    utl::vector<VertexInputBinding> matVertexBindings;
    matVertexBindings.push_back(VertexInputBinding{0, 36, true});
    material->SetVertexBindings(matVertexBindings);

    // Set Main Pass Render Target Formats
    // NOTE: This is crucial! Without this, Material::GetPipeline assumes 0 render targets.
    // We hardcode BGRA8_UNORM (standard swapchain format) and D32_Float here.
    utl::vector<DataFormat> rtFormats{}; 
    rtFormats.push_back(DataFormat::BGRA8_UNorm);
    rtFormats.push_back(DataFormat::RGBA32_Float); // WorldPos
    rtFormats.push_back(DataFormat::RGBA32_Float); // Normal
    rtFormats.push_back(DataFormat::RGBA32_Float); // UV
    material->SetRenderTargetFormats(rtFormats, DataFormat::D32_Float);

    std::cout << "Step 7b: Shadow Pipeline Configured" << std::endl;

    // Material Set (Set 2) - This is managed by MaterialSystem/MaterialInstance usually,
    // but for this test we might need to create it manually or let MaterialInstance handle it.
    // In this test, we use MaterialInstance which creates its own layout.

    // Create Buffers
    std::cout << "Step 7: Create Buffers" << std::endl;
    BufferDesc bufferDesc{};
    bufferDesc.size = sizeof(GlobalShaderData);
    bufferDesc.type = BufferType::Constant;
    bufferDesc.usage = GPUMemoryUsage::Dynamic;
    globalBuffer = device->CreateBuffer(bufferDesc);
    globalBufferMapped = device->MapBuffer(globalBuffer, 0, bufferDesc.size);

    shadowGlobalBuffer = device->CreateBuffer(bufferDesc);
    shadowGlobalBufferMapped = device->MapBuffer(shadowGlobalBuffer, 0, bufferDesc.size);

    bufferDesc.size = sizeof(ForwardLightBuffer);
    lightBuffer = device->CreateBuffer(bufferDesc);
    lightBufferMapped = device->MapBuffer(lightBuffer, 0, bufferDesc.size);

    // Align to 256 bytes for dynamic offset
    size_t perObjectSize = (sizeof(PerObjectData) + 255) & ~255;
    bufferDesc.size = perObjectSize * 100; // Max 100 objects
    perObjectBuffer = device->CreateBuffer(bufferDesc);
    perObjectBufferMapped = device->MapBuffer(perObjectBuffer, 0, bufferDesc.size);

    // Create Sampler
    SamplerDesc samplerDesc{};
    samplerDesc.minFilter = FilterMode::Linear;
    samplerDesc.magFilter = FilterMode::Linear;
    samplerDesc.addressU = TextureAddressMode::Clamp;
    samplerDesc.addressV = TextureAddressMode::Clamp;
    samplerDesc.addressW = TextureAddressMode::Clamp;
    samplerDesc.comparisonFunc = ComparisonFunc::Less;
    samplerDesc.borderColor = {1.0f, 1.0f, 1.0f, 1.0f};
    shadowSampler = device->CreateSampler(samplerDesc);

    // Create Dummy Shadow Map for Initial Binding (Prevent Crash in ShadowPass)
    TextureDesc dummyDesc{};
    dummyDesc.size = {1, 1, 1};
    dummyDesc.arraySize = 4;
    dummyDesc.type = TextureType::Texture2DArray;
    dummyDesc.format = DataFormat::RG32_Float;
    dummyDesc.usage = TextureUsage::ShaderResource | TextureUsage::RenderTarget;
    ResourceHandle dummyShadowMap = device->CreateTexture(dummyDesc);

    // Create Descriptor Sets
    DescriptorSetDesc globalSetDesc{};
    globalSetDesc.layout = globalSetLayout;
    globalSet = device->CreateDescriptorSet(globalSetDesc);

    // Create Shadow Global Set (for Shadow Pass to avoid R/W hazard)
    shadowGlobalSet = device->CreateDescriptorSet(globalSetDesc);

    DescriptorSetDesc perObjectSetDesc{};
    perObjectSetDesc.layout = perObjectSetLayout;
    perObjectSet = device->CreateDescriptorSet(perObjectSetDesc);
    
    // Update Sets (Initial)
    std::cout << "Step 8: Update Descriptor Sets" << std::endl;
    WriteDescriptorSet writes[7]; // Increased for shadowGlobalSet
    DescriptorBufferInfo bufferInfos[3];
    DescriptorImageInfo imageInfo;
    
    // Global Set - GlobalShaderData
    bufferInfos[0].buffer = globalBuffer;
    bufferInfos[0].offset = 0;
    bufferInfos[0].range = sizeof(GlobalShaderData);
    writes[0].dstSet = globalSet;
    writes[0].dstBinding = 11;
    writes[0].descriptorType = DescriptorType::UniformBuffer;
    writes[0].bufferInfo = &bufferInfos[0];
    writes[0].descriptorCount = 1;
    
    // Global Set - ForwardLightBuffer
    bufferInfos[1].buffer = lightBuffer;
    bufferInfos[1].offset = 0;
    bufferInfos[1].range = sizeof(ForwardLightBuffer);
    writes[1].dstSet = globalSet;
    writes[1].dstBinding = 12;
    writes[1].descriptorType = DescriptorType::UniformBuffer;
    writes[1].bufferInfo = &bufferInfos[1];
    writes[1].descriptorCount = 1;

    // Global Set - Shadow Map (Dummy)
    imageInfo.imageView = dummyShadowMap;
    imageInfo.sampler = shadowSampler;
    writes[2].dstSet = globalSet;
    writes[2].dstBinding = 13;
    writes[2].descriptorType = DescriptorType::CombinedImageSampler;
    writes[2].imageInfo = &imageInfo;
    writes[2].descriptorCount = 1;
    
    // Shadow Global Set - Same Buffers, but ALWAYS Dummy Shadow Map
    // Buffer 11: Shadow Global Buffer (Light ViewProjection)
    bufferInfos[2].buffer = shadowGlobalBuffer; // Reuse index 2 temporarily
    bufferInfos[2].offset = 0;
    bufferInfos[2].range = sizeof(GlobalShaderData);
    
    writes[3] = writes[0]; 
    writes[3].dstSet = shadowGlobalSet;
    writes[3].bufferInfo = &bufferInfos[2]; // Point to shadowGlobalBuffer

    writes[4] = writes[1]; writes[4].dstSet = shadowGlobalSet;
    writes[5] = writes[2]; writes[5].dstSet = shadowGlobalSet;

    // PerObject Set
    DescriptorBufferInfo perObjectBufferInfo{};
    perObjectBufferInfo.buffer = perObjectBuffer;
    perObjectBufferInfo.offset = 0;
    perObjectBufferInfo.range = sizeof(PerObjectData);

    writes[6].dstSet = perObjectSet;
    writes[6].dstBinding = 10;
    writes[6].descriptorType = DescriptorType::UniformBufferDynamic;
    writes[6].bufferInfo = &perObjectBufferInfo;
    writes[6].descriptorCount = 1;
    
    device->UpdateDescriptorSets(7, writes);

    // 8. Create Material Instance
    std::cout << "Step 9: Create Material Instance" << std::endl;
    // Cube
    materialInstance = new MaterialInstance(material);
    materialInstance->Initialize(device.get());
    MaterialUniformData cubeUniforms{1.0f, 1.0f, 1.0f, 1.0f};
    materialInstance->SetUniformData(0, &cubeUniforms, sizeof(MaterialUniformData));
    
    // Floor
    floorMaterialInstance = new MaterialInstance(material);
    floorMaterialInstance->Initialize(device.get());
    MaterialUniformData floorUniforms{0.5f, 0.5f, 0.5f, 1.0f};
    floorMaterialInstance->SetUniformData(0, &floorUniforms, sizeof(MaterialUniformData));

    // Register
    // stale-test port: RegisterMaterialInstance takes shared_ptr; test keeps raw
    // ownership (manual delete in shutdown), so wrap with a non-owning deleter.
    renderSystem.RegisterMaterialInstance(300,
        std::shared_ptr<MaterialInstance>(materialInstance, [](MaterialInstance*) {}));
    renderSystem.RegisterMaterialInstance(301,
        std::shared_ptr<MaterialInstance>(floorMaterialInstance, [](MaterialInstance*) {}));

    // 7. Create Mesh (Cube)
    std::cout << "Step 10: Create Mesh" << std::endl;
    float cubeVertices[] = {
        // Front (Z+)
        -0.5f, -0.5f,  0.5f,  1.0f, 0.0f, 0.0f,  0.0f, 0.0f, 1.0f,
         0.5f, -0.5f,  0.5f,  1.0f, 0.0f, 0.0f,  0.0f, 0.0f, 1.0f,
         0.5f,  0.5f,  0.5f,  1.0f, 0.0f, 0.0f,  0.0f, 0.0f, 1.0f,
        -0.5f,  0.5f,  0.5f,  1.0f, 0.0f, 0.0f,  0.0f, 0.0f, 1.0f,
        // Back (Z-)
         0.5f, -0.5f, -0.5f,  0.0f, 1.0f, 0.0f,  0.0f, 0.0f, -1.0f,
        -0.5f, -0.5f, -0.5f,  0.0f, 1.0f, 0.0f,  0.0f, 0.0f, -1.0f,
        -0.5f,  0.5f, -0.5f,  0.0f, 1.0f, 0.0f,  0.0f, 0.0f, -1.0f,
         0.5f,  0.5f, -0.5f,  0.0f, 1.0f, 0.0f,  0.0f, 0.0f, -1.0f,
        // Right (X+)
         0.5f, -0.5f,  0.5f,  0.0f, 0.0f, 1.0f,  1.0f, 0.0f, 0.0f,
         0.5f, -0.5f, -0.5f,  0.0f, 0.0f, 1.0f,  1.0f, 0.0f, 0.0f,
         0.5f,  0.5f, -0.5f,  0.0f, 0.0f, 1.0f,  1.0f, 0.0f, 0.0f,
         0.5f,  0.5f,  0.5f,  0.0f, 0.0f, 1.0f,  1.0f, 0.0f, 0.0f,
        // Left (X-)
        -0.5f, -0.5f, -0.5f,  1.0f, 1.0f, 0.0f,  -1.0f, 0.0f, 0.0f,
        -0.5f, -0.5f,  0.5f,  1.0f, 1.0f, 0.0f,  -1.0f, 0.0f, 0.0f,
        -0.5f,  0.5f,  0.5f,  1.0f, 1.0f, 0.0f,  -1.0f, 0.0f, 0.0f,
        -0.5f,  0.5f, -0.5f,  1.0f, 1.0f, 0.0f,  -1.0f, 0.0f, 0.0f,
        // Top (Y+)
        -0.5f,  0.5f,  0.5f,  0.0f, 1.0f, 1.0f,  0.0f, 1.0f, 0.0f,
         0.5f,  0.5f,  0.5f,  0.0f, 1.0f, 1.0f,  0.0f, 1.0f, 0.0f,
         0.5f,  0.5f, -0.5f,  0.0f, 1.0f, 1.0f,  0.0f, 1.0f, 0.0f,
        -0.5f,  0.5f, -0.5f,  0.0f, 1.0f, 1.0f,  0.0f, 1.0f, 0.0f,
        // Bottom (Y-)
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
    
    cubeMesh = new RenderMesh();
    cubeMesh->Create(device.get(), 100, cubeVertices, 24, 36, indices, 36, DataIndexType::UInt32);

    // 8. Setup Scene
    std::cout << "Step 11: Setup Scene" << std::endl;
    // Floor
    RenderProxy floorProxy = RenderProxy::Create(0, 100, 301);
    floorProxy.transform = CreateTranslationMatrix(v3{0.0f, -2.0f, 0.0f}) * CreateScaleMatrix(v3{10.0f, 0.1f, 10.0f});
    floorProxy.worldAABB = AABB(v3{-5.0f, -2.1f, -5.0f}, v3{5.0f, -1.9f, 5.0f});
    scene.AddProxy(floorProxy);
    
    // Cube
    RenderProxy cubeProxy = RenderProxy::Create(1, 100, 300);
    cubeProxy.transform = CreateTranslationMatrix(v3{0.0f, 1.0f, 0.0f});
    cubeProxy.worldAABB = AABB(v3{-0.5f, 0.5f, -0.5f}, v3{0.5f, 1.5f, 0.5f});
    scene.AddProxy(cubeProxy);

    // Lights
    RenderLight directionalLight;
    directionalLight.type = LightType::Directional;
    directionalLight.direction = {0.5f, -1.0f, 0.5f};
    // Normalize
    float len = sqrt(directionalLight.direction.x*directionalLight.direction.x + directionalLight.direction.y*directionalLight.direction.y + directionalLight.direction.z*directionalLight.direction.z);
    directionalLight.direction.x /= len; directionalLight.direction.y /= len; directionalLight.direction.z /= len;
    directionalLight.color = {1.0f, 0.95f, 0.8f};
    directionalLight.intensity = 1.0f;
    scene.AddLight(directionalLight);

    // Setup Input
    using namespace primal::input;
    input_source source{};
    source.binding = std::hash<std::string>()("debug_toggle");
    source.source_type = input_source::keyboard;
    source.code = input_code::key_f1;
    source.multiplier = 1.0f;
    bind(source);

    // View
    v3 eye = {0.0f, 5.0f, 10.0f};
    v3 target = {0.0f, 0.0f, 0.0f};
    v3 up = {0.0f, 1.0f, 0.0f};
    view.SetViewMatrix(CreateLookAtMatrix(eye, target, up));
    view.SetProjectionMatrix(rhi::metal::CreatePerspectiveMatrix(60.0f * primal::graphics::rhi::math::constants::DEG_TO_RAD, 1280.0f/720.0f, 0.1f, 100.0f));

    std::cout << "Init Complete" << std::endl;
    return true;
}

void CSMIntegrationRenderGraphTestCase::DrawScene(RHICommandBuffer* cmdBuffer, RenderPassHandle renderPass, uint32_t permutationId, PipelineFlags flags, uint32_t instanceCount) {
    auto proxies = scene.GetProxies();
    
    // DEBUG: Log Proxy Count
    static bool printedProxyCount = false;
    if (!printedProxyCount) {
        std::cout << "DrawScene: Proxy Count: " << proxies.size() << std::endl;
        printedProxyCount = true;
    }

    for (const auto& proxy : proxies) {
        // DEBUG: Loop Start
        // std::cout << "Loop Proxy: " << proxy.entityId << std::endl;

        // RenderMesh* mesh = scene.GetMesh(proxy.meshId);
        // if (!mesh) {
        //     std::cerr << "DrawScene Error: Mesh not found for Proxy " << proxy.entityId << std::endl;
        //     continue;
        // }

        // Get Pipeline
        PipelineHandle pipeline = handles::INVALID_PIPELINE;
        if ((flags & PipelineFlags::Shadow) != PipelineFlags::None) {
            pipeline = shadowPipeline;
        } else {
            pipeline = material->GetPipeline(device.get(), renderPass, permutationId, flags);
        }

        if (pipeline == handles::INVALID_PIPELINE) {
            std::cout << "DrawScene Error: Invalid Pipeline for Proxy " << proxy.entityId << " (Flags: " << (int)flags << ")" << std::endl;
            continue;
        } else {
             // std::cout << "Pipeline Valid: " << pipeline << " Flags: " << (int)flags << std::endl;
        }
        
        // Bind Buffers
        RenderMesh* mesh = cubeMesh; // Simplified: we know all proxies use cubeMesh
        
        // DEBUG: Log Draw Call
        // Unconditional log for MainPass
        // if (flags == PipelineFlags::None) {
              // std::cout << "Drawing Proxy " << proxy.entityId << " IndexCount: " << mesh->GetIndexCount() << " Pipeline: " << pipeline << std::endl;
              // std::cout << "  VB: " << mesh->GetPositionBuffer() << " IB: " << mesh->GetIndexBuffer() << std::endl;
        // }
        
        cmdBuffer->BindGraphicsPipeline(pipeline);
        if (!mesh) {
            std::cerr << "DrawScene Error: Mesh is null for Proxy " << proxy.entityId << std::endl;
            continue;
        }
        
        ResourceHandle vb = mesh->GetVertexBuffer();
        ResourceHandle ib = mesh->GetIndexBuffer();
        
        if (vb == handles::INVALID_RESOURCE || ib == handles::INVALID_RESOURCE) {
             std::cerr << "DrawScene Error: Invalid VB/IB for Proxy " << proxy.entityId << std::endl;
             continue;
        }
        
        ResourceHandle vbs[] = {vb};
        uint64_t offsets[] = {0};
        cmdBuffer->BindVertexBuffers(0, 1, vbs, offsets);
        cmdBuffer->BindIndexBuffer(ib, DataFormat::R32_UInt, 0);
        
        // Bind Descriptor Sets
        uint32_t dynamicOffsets[] = { (uint32_t)(proxy.entityId * perObjectSize) };
        
        if ((flags & PipelineFlags::Shadow) != PipelineFlags::None) {
            // Shadow Pass: Global (Set 0) + PerObject (Set 1)
            // std::cout << "DrawScene: Binding Shadow Sets" << std::endl;
            ResourceHandle globalSets[] = { shadowGlobalSet };
            cmdBuffer->BindDescriptorSets(PipelineBindPoint::Graphics, shadowPipelineLayout, 0, 1, globalSets, 0, nullptr);

            ResourceHandle sets[] = { perObjectSet };
            cmdBuffer->BindDescriptorSets(PipelineBindPoint::Graphics, shadowPipelineLayout, 1, 1, sets, 1, dynamicOffsets);
        } else {
            // Main Pass: Global (Set 0) + PerObject (Set 1) + Material (Set 2)
            // Bind Global Set
            ResourceHandle globalSets[] = { globalSet };
            cmdBuffer->BindDescriptorSets(PipelineBindPoint::Graphics, pipelineLayout, 0, 1, globalSets, 0, nullptr);
            
            // Bind PerObject Set
            ResourceHandle perObjectSets[] = { perObjectSet };
            cmdBuffer->BindDescriptorSets(PipelineBindPoint::Graphics, pipelineLayout, 1, 1, perObjectSets, 1, dynamicOffsets);

            // Bind Material Set
            // stale-test port: GetMaterialInstance returns shared_ptr now
            std::shared_ptr<MaterialInstance> matInst = renderSystem.GetMaterialInstance(proxy.materialId);
            if (matInst && matInst->GetDescriptorSet() != handles::INVALID_RESOURCE) {
                ResourceHandle matSets[] = { matInst->GetDescriptorSet() };
                cmdBuffer->BindDescriptorSets(PipelineBindPoint::Graphics, pipelineLayout, 2, 1, matSets, 0, nullptr);
            }
        }
        
        // std::cout << "DrawIndexed..." << std::endl;
        cmdBuffer->DrawIndexed(mesh->GetIndexCount(), 0, 0, instanceCount, 0);
    }
}

void CSMIntegrationRenderGraphTestCase::Run() {
    // std::cout << "Run Frame..." << std::endl;
    if (!window.is_valid()) return;

    // Animation
    static float totalTime = 0.0f;
    totalTime += 0.016f; // Approx 60fps
    rotationAngle += 0.01f;
    
    if (scene.GetProxies().size() > 1) {
        RenderProxy cubeProxy = scene.GetProxies()[1];
        m4x4 rot = CreateRotationMatrixY(rotationAngle);
        m4x4 trans = CreateTranslationMatrix(v3{0.0f, 1.0f + sin(totalTime) * 0.5f, 0.0f});
        cubeProxy.transform = trans * rot;
        scene.UpdateProxy(cubeProxy.entityId, cubeProxy);
    }
    
    // Update Material Uniforms
    uint32_t frameIndex = renderSystem.GetCurrentFrameIndex();
    MaterialUniformData cubeUniformData{1.0f, 1.0f, 1.0f, 1.0f};
    MaterialUniformData floorUniformData{0.5f, 0.5f, 0.5f, 1.0f};
    
    materialInstance->SetCurrentFrame(frameIndex);
    materialInstance->SetUniformData(0, &cubeUniformData, sizeof(MaterialUniformData));
    materialInstance->Update(device.get());

    floorMaterialInstance->SetCurrentFrame(frameIndex);
    floorMaterialInstance->SetUniformData(0, &floorUniformData, sizeof(MaterialUniformData));
    floorMaterialInstance->Update(device.get());

    // Begin Frame
    ResourceHandle backBuffer;
    SyncHandle signalFence;
    if (!renderSystem.BeginFrame(backBuffer, signalFence)) return;

    // Get current backbuffer description for correct sizing
    TextureDesc backBufferDesc = renderSystem.GetBackBufferDesc();
    
    // DEBUG: Print BackBuffer Size
    static bool printedBackBuffer = false;
    if (!printedBackBuffer) {
        std::cout << "BackBuffer Size: " << backBufferDesc.size.x << "x" << backBufferDesc.size.y << std::endl;
        printedBackBuffer = true;
    }

    if (backBufferDesc.size.x == 0 || backBufferDesc.size.y == 0) {
        backBufferDesc.size = {1280, 720, 1}; // Fallback
        std::cerr << "WARNING: BackBuffer size is 0! Using fallback 1280x720." << std::endl;
    }
    
    // Update Projection with correct aspect ratio
    view.SetProjectionMatrix(rhi::metal::CreatePerspectiveMatrix(60.0f * primal::graphics::rhi::math::constants::DEG_TO_RAD, (float)backBufferDesc.size.x/(float)backBufferDesc.size.y, 0.1f, 100.0f));

    // Update Global Data
    GlobalShaderData globalData{};
    primal::math::v3 eye = {0.0f, 10.0f, -10.0f};
    primal::math::v3 target = {0.0f, 0.0f, 0.0f};
    
    if (globalBufferMapped) {
        globalData.view = view.GetViewMatrix();
        globalData.projection = view.GetProjectionMatrix();
        globalData.viewProjection = globalData.projection * globalData.view;
        globalData.invViewProjection = simd::inverse(globalData.viewProjection);
        globalData.cameraPositionAndViewWidth = {eye.x, eye.y, eye.z, (float)backBufferDesc.size.x};
        globalData.cameraDirectionAndViewHeight = {target.x - eye.x, target.y - eye.y, target.z - eye.z, (float)backBufferDesc.size.y};
        globalData.frameCount = totalTime;
        memcpy(globalBufferMapped, &globalData, sizeof(GlobalShaderData));
    }
    
    // Update Shadow Global Data (Light VP)
    primal::math::m4x4 lightVP = rhi::math::MatrixIdentity();
    
    // Light Direction: {0.5f, -1.0f, 0.5f}
    primal::math::v3 lightDir = {0.5f, -1.0f, 0.5f};
    float len = sqrt(lightDir.x*lightDir.x + lightDir.y*lightDir.y + lightDir.z*lightDir.z);
    lightDir = {lightDir.x/len, lightDir.y/len, lightDir.z/len};
    
    primal::math::v3 lightEye = {-lightDir.x * 20.0f, -lightDir.y * 20.0f, -lightDir.z * 20.0f};
    primal::math::m4x4 lightView = rhi::math::CreateLookAtMatrix(lightEye, {0,0,0}, {0,1,0});

    // Fix for Metal Ortho (LH) vs LookAt (RH)
    // Metal Ortho expects +Z for depth [0, 1], but LookAt (RH) produces -Z for forward.
    // We flip Z to convert RH View Space to LH View Space.
    primal::math::m4x4 flipZ = rhi::math::CreateScaleMatrix({1.0f, 1.0f, -1.0f});
    lightView = flipZ * lightView;

    // Increase Ortho size to cover the whole scene (Floor is 10x10, so 20-30 is safe)
    primal::math::m4x4 lightProj = rhi::metal::CreateOrthographicMatrix(-30, 30, -30, 30, 0.1f, 100.0f);
    lightVP = lightProj * lightView;

    if (shadowGlobalBufferMapped) {
        GlobalShaderData shadowData = globalData; // Copy common data
        shadowData.view = lightView;
        shadowData.projection = lightProj;
        shadowData.viewProjection = lightVP;
        shadowData.invViewProjection = simd::inverse(lightVP);
        
        memcpy(shadowGlobalBufferMapped, &shadowData, sizeof(GlobalShaderData));
    }

    // Update Light Data
    if (lightBufferMapped) {
        ForwardLightBuffer lightData{};
        lightData.directionalLightCount = 1;
        // Direction: {0.5f, -1.0f, 0.5f} normalized
        primal::math::v3 dir = {0.5f, -1.0f, 0.5f};
        float len = sqrt(dir.x*dir.x + dir.y*dir.y + dir.z*dir.z);
        lightData.directionalLights[0].directionAndIntensity = {dir.x/len, dir.y/len, dir.z/len, 1.0f};
        lightData.directionalLights[0].colorAndShadow = {1.0f, 0.95f, 0.8f, 1.0f}; // Shadow enabled
        
        // Update Light VP for Main Pass Shadow Sampling
        lightData.directionalLights[0].viewProjections[0] = lightVP;
        lightData.directionalLights[0].viewProjections[1] = lightVP;
        lightData.directionalLights[0].viewProjections[2] = lightVP;
        lightData.directionalLights[0].viewProjections[3] = lightVP;
        lightData.directionalLights[0].splits = {20.0f, 40.0f, 80.0f, 100.0f}; // Adjusted splits to ensure Cube (dist ~10.77) falls into Cascade 0
        
        memcpy(lightBufferMapped, &lightData, sizeof(ForwardLightBuffer));
    }

    // Update PerObject Data
        if (perObjectBufferMapped) {
        uint8_t* pData = (uint8_t*)perObjectBufferMapped;
        auto proxies = scene.GetProxies();
        for (const auto& proxy : proxies) {
            PerObjectData objData{};
            objData.world = proxy.transform;
            // objData.color = proxy.color; // PerObjectData in RHIShaderCommon.h doesn't have color
            
            // Calculate other matrices if needed, though shader currently ignores them
            objData.invWorld = simd::inverse(proxy.transform);
            // objData.worldViewProjection = ... // Calculated in shader now
            
            memcpy(pData + (proxy.entityId * perObjectSize), &objData, sizeof(PerObjectData));
        }
    }

    // RENDER GRAPH DEFINITION
    renderGraph->Clear();

    struct ShadowPassData {
        RGResourceHandle shadowMoments{};
        RGResourceHandle shadowDepth{};
    };

    const auto& shadowData = renderGraph->AddPass<ShadowPassData>("ShadowPass", RGPassType::Graphics,
        [&](ShadowPassData& data, RenderGraphBuilder& builder) {
            // std::cout << "Setup ShadowPass" << std::endl;
            // Create Shadow Map (Moments)
            TextureDesc descMoments{};
            descMoments.size = {2048, 2048, 1};
            descMoments.arraySize = 4;
            descMoments.type = TextureType::Texture2DArray;
            descMoments.format = DataFormat::RG32_Float;
            descMoments.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
            data.shadowMoments = builder.CreateTexture("ShadowMoments", descMoments);
            data.shadowMoments = builder.Write(data.shadowMoments, ResourceState::RenderTarget);

            // Create Shadow Depth
            TextureDesc descDepth{};
            descDepth.size = {2048, 2048, 1};
            descDepth.arraySize = 4;
            descDepth.type = TextureType::Texture2DArray;
            descDepth.format = DataFormat::D32_Float;
            descDepth.usage = TextureUsage::DepthStencil;
            data.shadowDepth = builder.CreateTexture("ShadowDepth", descDepth);
            data.shadowDepth = builder.Write(data.shadowDepth, ResourceState::DepthStencil); // Write as Depth
        },
        [&](const ShadowPassData& data, RenderGraphContext& context) {
            // Draw Shadow Pass
            RenderPassDesc desc{};
            
            // Color Attachment (Moments)
            desc.colorAttachments.resize(1);
            desc.colorAttachments[0].texture = context.graph->GetResource(data.shadowMoments)->GetPhysicalHandle();
            desc.colorAttachments[0].loadOp = LoadAction::Clear;
            desc.colorAttachments[0].storeOp = StoreAction::Store;
            desc.colorAttachments[0].clearValue = ClearValue{primal::math::v4{1.0f, 1.0f, 0.0f, 0.0f}};

            // Depth Attachment
            desc.depthAttachment.texture = context.graph->GetResource(data.shadowDepth)->GetPhysicalHandle();
            desc.depthAttachment.loadOp = LoadAction::Clear;
            desc.depthAttachment.storeOp = StoreAction::Store;
            desc.depthAttachment.clearValue = ClearValue{}; desc.depthAttachment.clearValue.depth = 1.0f; // Far plane
            
            context.cmdBuffer->BeginRenderPass(desc);
            
            // Set Viewport/Scissor for Shadow Map
            ViewportDesc vp{};
            vp.size = {2048.0f, 2048.0f}; vp.maxDepth = 1.0f;
            context.cmdBuffer->SetViewport(vp);
            
            primal::graphics::rhi::Rect scissor{};
            scissor.extent = {2048, 2048};
            context.cmdBuffer->SetScissor(scissor);
            
            // Draw Scene with Shadow Shader (Permutation 1)
            // Passing INVALID_RESOURCE for RenderPassHandle as Material uses stored formats.
            // Draw 4 instances for CSM (one per cascade)
            DrawScene(context.cmdBuffer, rhi::handles::INVALID_RESOURCE, 1, PipelineFlags::Shadow, 4);
            
            context.cmdBuffer->EndRenderPass();
        }
    );

    struct MainPassData {
        RGResourceHandle backBuffer;
        RGResourceHandle worldPosBuffer;
        RGResourceHandle normalBuffer;
        RGResourceHandle uvBuffer;
        RGResourceHandle shadowMoments;
        RGResourceHandle depthBuffer;
    };

    const auto& mainPassData = renderGraph->AddPass<MainPassData>("MainPass", RGPassType::Graphics,
        [&](MainPassData& data, RenderGraphBuilder& builder) {
            // Import BackBuffer
            data.backBuffer = renderGraph->ImportTexture("BackBuffer", backBuffer, backBufferDesc);
            data.backBuffer = builder.Write(data.backBuffer, ResourceState::RenderTarget);
            
            // Create GBuffer Textures
            TextureDesc descGBuffer{};
            descGBuffer.size = backBufferDesc.size;
            descGBuffer.format = DataFormat::RGBA32_Float; // High precision for debug
            descGBuffer.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource; // Ensure shader resource usage for debug sampling
            
            data.worldPosBuffer = builder.CreateTexture("WorldPos", descGBuffer);
            data.worldPosBuffer = builder.Write(data.worldPosBuffer, ResourceState::RenderTarget);
            
            data.normalBuffer = builder.CreateTexture("Normal", descGBuffer);
            data.normalBuffer = builder.Write(data.normalBuffer, ResourceState::RenderTarget);
            
            data.uvBuffer = builder.CreateTexture("UV", descGBuffer);
            data.uvBuffer = builder.Write(data.uvBuffer, ResourceState::RenderTarget);

            // Read Shadow Map (Moments)
            data.shadowMoments = builder.Read(shadowData.shadowMoments, ResourceState::ShaderResource);

            // Create Transient Depth
            TextureDesc descDepth{};
            descDepth.size = backBufferDesc.size; // Match window/backbuffer size
            descDepth.format = DataFormat::D32_Float;
            descDepth.usage = TextureUsage::DepthStencil;
            data.depthBuffer = builder.CreateTexture("MainDepth", descDepth);
            data.depthBuffer = builder.Write(data.depthBuffer, ResourceState::DepthStencil);
        },
        [&, backBufferDesc](const MainPassData& data, RenderGraphContext& context) {
            // std::cout << "Executing MainPass" << std::endl;
            // Update Shadow Map Descriptor
            
            RenderGraphResource* shadowRes = context.graph->GetResource(data.shadowMoments);
            if (shadowRes) {
                 DescriptorImageInfo imageInfo{};
                 imageInfo.imageView = shadowRes->GetPhysicalHandle();
                 imageInfo.sampler = shadowSampler;
                 
                 WriteDescriptorSet shadowWrite{};
                 shadowWrite.dstSet = globalSet;
                 shadowWrite.dstBinding = 13; // ShadowMap + Sampler
                 shadowWrite.descriptorType = DescriptorType::CombinedImageSampler;
                 shadowWrite.imageInfo = &imageInfo;
                 shadowWrite.descriptorCount = 1;
                 
                 device->UpdateDescriptorSets(1, &shadowWrite);
            }

            RenderPassDesc desc{};
            desc.colorAttachments.resize(4);
            
            // 0: BackBuffer
            desc.colorAttachments[0].texture = context.graph->GetResource(data.backBuffer)->GetPhysicalHandle();
            desc.colorAttachments[0].loadOp = LoadAction::Clear;
            desc.colorAttachments[0].storeOp = StoreAction::Store;
            desc.colorAttachments[0].clearValue = ClearValue{primal::math::v4{0.2f, 0.3f, 0.4f, 1.0f}};
            
            // 1: WorldPos
            desc.colorAttachments[1].texture = context.graph->GetResource(data.worldPosBuffer)->GetPhysicalHandle();
            desc.colorAttachments[1].loadOp = LoadAction::Clear;
            desc.colorAttachments[1].storeOp = StoreAction::Store;
            desc.colorAttachments[1].clearValue = ClearValue{primal::math::v4{0.0f, 0.0f, 0.0f, 0.0f}};

            // 2: Normal
            desc.colorAttachments[2].texture = context.graph->GetResource(data.normalBuffer)->GetPhysicalHandle();
            desc.colorAttachments[2].loadOp = LoadAction::Clear;
            desc.colorAttachments[2].storeOp = StoreAction::Store;
            desc.colorAttachments[2].clearValue = ClearValue{primal::math::v4{0.0f, 0.0f, 0.0f, 0.0f}};
            
            // 3: UV
            desc.colorAttachments[3].texture = context.graph->GetResource(data.uvBuffer)->GetPhysicalHandle();
            desc.colorAttachments[3].loadOp = LoadAction::Clear;
            desc.colorAttachments[3].storeOp = StoreAction::Store;
            desc.colorAttachments[3].clearValue = ClearValue{primal::math::v4{0.0f, 0.0f, 0.0f, 0.0f}};
            
            // Depth Buffer
            desc.depthAttachment.texture = context.graph->GetResource(data.depthBuffer)->GetPhysicalHandle();
            desc.depthAttachment.loadOp = LoadAction::Clear;
            desc.depthAttachment.storeOp = StoreAction::Store;
            desc.depthAttachment.clearValue = ClearValue{}; desc.depthAttachment.clearValue.depth = 1.0f;
            
            context.cmdBuffer->BeginRenderPass(desc);
            
            // Set Viewport/Scissor
            ViewportDesc vp{};
            vp.size = {(float)backBufferDesc.size.x, (float)backBufferDesc.size.y}; vp.maxDepth = 1.0f;
            context.cmdBuffer->SetViewport(vp);
            
            primal::graphics::rhi::Rect scissor{};
            // FORCE SCISSOR
            scissor.extent = { (uint32_t)backBufferDesc.size.x, (uint32_t)backBufferDesc.size.y };
            context.cmdBuffer->SetScissor(scissor);
            
            // Draw Scene (Permutation 0)
            DrawScene(context.cmdBuffer, rhi::handles::INVALID_RESOURCE, 0, PipelineFlags::None);
            
            context.cmdBuffer->EndRenderPass();
        }
    );

    // Add Debug Pass
    // stale-test port: AddDebugPass takes utl::vector
    primal::utl::vector<primal::graphics::DebugResource> debugResources;
    debugResources.push_back({"ShadowMoments", shadowData.shadowMoments});
    debugResources.push_back({"ShadowDepth", shadowData.shadowDepth});
    debugResources.push_back({"MainDepth", mainPassData.depthBuffer});
    debugResources.push_back({"WorldPos", mainPassData.worldPosBuffer});
    debugResources.push_back({"Normal", mainPassData.normalBuffer});
    debugResources.push_back({"UV", mainPassData.uvBuffer});
    
    primal::graphics::AddDebugPass(*renderGraph, mainPassData.backBuffer, debugResources);

    renderGraph->Compile();
    
    // Execute
    CommandBufferHandle cmdHandle = device->CreateCommandBuffer(CommandQueueType::Graphics);
    if (cmdHandle != handles::INVALID_COMMAND_BUFFER) {
        // Cast to MetalDevice to access GetCommandBuffer
        // Note: In a pure RHI abstraction, GetCommandBuffer should be on RHIDeviceBase or similar.
        // For this test, we know it's Metal.
        auto* metalDevice = static_cast<MetalDevice*>(device.get());
        RHICommandBuffer* cmd = metalDevice->GetCommandBuffer(cmdHandle);
        cmd->Begin(); 
        
        if (signalFence != handles::INVALID_SYNC) {
            cmd->AddWaitSemaphore(signalFence, 0);
        }
        
        renderGraph->Execute(cmd);
        
        cmd->End();
        cmd->Submit();
        
        // Wait for completion before destroying to ensure no async callbacks write to the memory
        cmd->WaitForCompletion();
        
        // Destroy Command Buffer immediately after submission
        // In a real engine, this might be deferred or pooled, but here we prevent leak
        device->DestroyCommandBuffer(cmdHandle);
    }

    // Input Update
    primal::input::input_value val;
    val.previous = val.current;
    // Note: In a real engine, the platform layer (Window/OS) updates the 'current' value via events.
    // The 'previous' value should be updated at the end of the frame to prepare for the next frame.
    // However, primal::input doesn't seem to have a frame-end update function exposed.
    // It relies on events updating 'current'.
    // The issue with F1 toggling too fast is that 'previous' is not being updated to 'current' at frame boundaries.
    // But since primal::input::get reads from internal state, we can't easily fix the internal 'previous' state from here without access.
    // Wait, primal::input::get returns a value struct that has both previous and current.
    // The input system implementation (Input.cpp/h) manages the state.
    // If the input system is event-based (MacKeyboard.mm), it updates 'current' when key is pressed/released.
    // It does NOT automatically update 'previous' every frame unless there is an explicit 'Update()' call in the engine loop.
    // Since there is no Input::Update(), 'previous' might only be updated when an event occurs?
    // Let's look at Input.cpp... (Not visible here, but based on typical behavior)
    
    // Actually, for the test to work with the "Toggle" logic in RenderGraphDebug::Update:
    // It checks (current > 0 && previous == 0).
    // If 'previous' is never updated to match 'current' after the first frame of press,
    // then (current > 0 && previous == 0) will be true ONLY for the first frame if 'previous' starts at 0.
    // BUT, if 'previous' is NOT updated, it remains 0?
    // If the input system updates 'previous' = 'current' only on events, then holding the key:
    // Frame 1: Event Down -> current=1. previous (was 0). Logic: 1 > 0 && 0 == 0 -> Toggle!
    // Frame 2: No Event. current=1. previous? If system didn't update it, it's still 0? -> Toggle again!
    
    // To fix this in the Test environment without changing the Engine core deeply:
    // We rely on the fix we made in RenderGraphDebug.cpp where we added 'f1_pressed_' state.
    // That fix handles the toggle logic locally regardless of 'previous' state issues.
    // So we just need to ensure events are processed.
    // MacKeyboard.mm processes events via callbacks.
    
    renderSystem.EndFrame();
}

void CSMIntegrationRenderGraphTestCase::Shutdown() {
    // 1. Wait for GPU
    if (device) {
        device->WaitIdle();
    }
    
    // 2. Destroy Render Graph (cleans up its resources)
    renderGraph.reset();
    
    // 3. Shutdown Debug Pass Static Renderer (Important! Before device destruction)
    primal::graphics::ShutdownDebugPass();

    // 4. Destroy Scene Resources
    if (cubeMesh) {
        delete cubeMesh;
        cubeMesh = nullptr;
    }
    
    if (materialInstance) {
        delete materialInstance;
        materialInstance = nullptr;
    }
    
    if (floorMaterialInstance) {
        delete floorMaterialInstance;
        floorMaterialInstance = nullptr;
    }
    
    if (material) {
        delete material;
        material = nullptr;
    }
    
    // 5. Destroy RHI Resources
    if (device) {
        if (globalSet != handles::INVALID_DESCRIPTOR_SET) device->DestroyDescriptorSet(globalSet);
        if (shadowGlobalSet != handles::INVALID_DESCRIPTOR_SET) device->DestroyDescriptorSet(shadowGlobalSet);
        if (perObjectSet != handles::INVALID_DESCRIPTOR_SET) device->DestroyDescriptorSet(perObjectSet);
        
        if (globalSetLayout != handles::INVALID_DESCRIPTOR_SET_LAYOUT) device->DestroyDescriptorSetLayout(globalSetLayout);
        if (perObjectSetLayout != handles::INVALID_DESCRIPTOR_SET_LAYOUT) device->DestroyDescriptorSetLayout(perObjectSetLayout);
        if (materialSetLayout != handles::INVALID_DESCRIPTOR_SET_LAYOUT) device->DestroyDescriptorSetLayout(materialSetLayout);
        
        if (pipelineLayout != handles::INVALID_PIPELINE_LAYOUT) device->DestroyPipelineLayout(pipelineLayout);
        if (shadowPipelineLayout != handles::INVALID_PIPELINE_LAYOUT) device->DestroyPipelineLayout(shadowPipelineLayout);
        
        if (shadowPipeline != handles::INVALID_PIPELINE) device->DestroyPipeline(shadowPipeline);
        if (shadowVS != handles::INVALID_SHADER) device->DestroyShader(shadowVS);
        if (shadowPS != handles::INVALID_SHADER) device->DestroyShader(shadowPS);
        
        if (globalBuffer != handles::INVALID_RESOURCE) device->DestroyBuffer(globalBuffer);
        if (shadowGlobalBuffer != handles::INVALID_RESOURCE) device->DestroyBuffer(shadowGlobalBuffer);
        if (lightBuffer != handles::INVALID_RESOURCE) device->DestroyBuffer(lightBuffer);
        if (perObjectBuffer != handles::INVALID_RESOURCE) device->DestroyBuffer(perObjectBuffer);
        
        if (shadowSampler != handles::INVALID_SAMPLER) device->DestroySampler(shadowSampler);
    }
    
    renderSystem.Shutdown();
    
    if (window.is_valid()) {
        platform::remove_window(window.get_id());
    }
    
    device.reset(); // Destroy Device Last
}
