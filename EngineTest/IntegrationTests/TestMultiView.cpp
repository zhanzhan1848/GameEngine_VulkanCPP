#include "TestMultiView.h"
#include "Engine/Graphics/RHI/Core/RHICommand.h"
#include "Engine/Graphics/RenderGraph/RenderGraphBuilder.h"
#include "Engine/Graphics/RenderGraph/RenderGraphDefinitions.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalDevice.h"
#include "Engine/Graphics/RenderMesh.h"
#include "Engine/Graphics/RenderScene.h"
#include "Engine/Graphics/RHI/Core/RHIMath.h"
#include "Engine/Platform/Platform.h" // For create_window
#include "TestMultiView.h"
#include "Engine/Input/Input.h"
#include <cmath>
#include <vector>
#include <fstream>
#include <sstream>

using namespace primal::graphics;
using namespace primal::graphics::rhi;
using namespace primal::graphics::rendergraph;
using namespace primal::graphics::rhi::math;

    // Helper Math Functions for Metal (Column-Major)
    m4x4 CreatePerspective(float fov, float aspect, float zNear, float zFar) {
        float tanHalfFov = tan(fov / 2.0f);
        primal::math::m4x4 result = simd_matrix(
            primal::math::v4{1.0f / (aspect * tanHalfFov), 0.0f, 0.0f, 0.0f},
            primal::math::v4{0.0f, 1.0f / tanHalfFov, 0.0f, 0.0f},
            primal::math::v4{0.0f, 0.0f, zFar / (zNear - zFar), -1.0f},
            primal::math::v4{0.0f, 0.0f, -(zFar * zNear) / (zFar - zNear), 0.0f}
        );
        return result;
    }
    
    m4x4 CreateLookAt(v3 eye, v3 center, v3 up) {
        v3 f = Normalize(center - eye);
        v3 s = Normalize(Cross(f, up));
        v3 u = Cross(s, f);
    
        m4x4 result = simd_matrix(
            v4{s.x, u.x, -f.x, 0.0f},
            v4{s.y, u.y, -f.y, 0.0f},
            v4{s.z, u.z, -f.z, 0.0f},
            v4{-Dot(s, eye), -Dot(u, eye), Dot(f, eye), 1.0f}
        );
        return result;
    }
    
    Engine_Test::Engine_Test() : RenderTestRunner(std::make_unique<MultiViewTestCase>()) {}
    
    void MultiViewTestCase::CreateCubeMesh() {
        // Cornell Box Geometry
        // Struct updated with Color
        struct Vertex {
            float position[3];
            float normal[3];
            float uv[2];
            float color[3]; // Add Color
        };

        std::cout << "DEBUG: sizeof(Vertex) = " << sizeof(Vertex) << std::endl;
        
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        
        // Helper to add Quad
        auto addQuad = [&](v3 p0, v3 p1, v3 p2, v3 p3, v3 n, v3 color) {
            uint32_t base = (uint32_t)vertices.size();
            
            Vertex v0, v1, v2, v3_vert;
            
            // 0
            v0.position[0] = p0.x; v0.position[1] = p0.y; v0.position[2] = p0.z;
            v0.normal[0] = n.x; v0.normal[1] = n.y; v0.normal[2] = n.z;
            v0.uv[0] = 0; v0.uv[1] = 0;
            v0.color[0] = color.x; v0.color[1] = color.y; v0.color[2] = color.z;

            // 1
            v1.position[0] = p1.x; v1.position[1] = p1.y; v1.position[2] = p1.z;
            v1.normal[0] = n.x; v1.normal[1] = n.y; v1.normal[2] = n.z;
            v1.uv[0] = 1; v1.uv[1] = 0;
            v1.color[0] = color.x; v1.color[1] = color.y; v1.color[2] = color.z;

            // 2
            v2.position[0] = p2.x; v2.position[1] = p2.y; v2.position[2] = p2.z;
            v2.normal[0] = n.x; v2.normal[1] = n.y; v2.normal[2] = n.z;
            v2.uv[0] = 1; v2.uv[1] = 1;
            v2.color[0] = color.x; v2.color[1] = color.y; v2.color[2] = color.z;

            // 3
            v3_vert.position[0] = p3.x; v3_vert.position[1] = p3.y; v3_vert.position[2] = p3.z;
            v3_vert.normal[0] = n.x; v3_vert.normal[1] = n.y; v3_vert.normal[2] = n.z;
            v3_vert.uv[0] = 0; v3_vert.uv[1] = 1;
            v3_vert.color[0] = color.x; v3_vert.color[1] = color.y; v3_vert.color[2] = color.z;

            vertices.push_back(v0);
            vertices.push_back(v1);
            vertices.push_back(v2);
            vertices.push_back(v3_vert);
            
            indices.push_back(base + 0);
            indices.push_back(base + 1);
            indices.push_back(base + 2);
            indices.push_back(base + 0);
            indices.push_back(base + 2);
            indices.push_back(base + 3);
        };
        
    // --- Part 1: Standard Cornell Box (Size 10, -5 to 5) ---
    // Cornell Box Dimensions: 0 to 555 usually, scaled to -5 to 5
    float size = 10.0f;
    float h = size * 0.5f;
    
    v3 white = {1.0f, 1.0f, 1.0f};
    v3 red   = {1.0f, 0.0f, 0.0f};
    v3 green = {0.0f, 1.0f, 0.0f};
    
    // Floor (White)
    addQuad(v3{-h, -h, -h}, v3{ h, -h, -h}, v3{ h, -h,  h}, v3{-h, -h,  h}, v3{0, 1, 0}, white);
    
    // Ceiling (White)
    addQuad(v3{-h,  h,  h}, v3{ h,  h,  h}, v3{ h,  h, -h}, v3{-h,  h, -h}, v3{0, -1, 0}, white);
    
    // Back Wall (White)
    addQuad(v3{ h, -h, -h}, v3{-h, -h, -h}, v3{-h,  h, -h}, v3{ h,  h, -h}, v3{0, 0, 1}, white);
    
    // Left Wall (Red)
    addQuad(v3{-h, -h, -h}, v3{-h, -h,  h}, v3{-h,  h,  h}, v3{-h,  h, -h}, v3{1, 0, 0}, red);
    
    // Right Wall (Green)
    addQuad(v3{ h, -h,  h}, v3{ h, -h, -h}, v3{ h,  h, -h}, v3{ h,  h,  h}, v3{-1, 0, 0}, green);
    
    // Helper to add Face (for legacy/debug shapes)
    auto addFace = [&](v3 n, v3 x, v3 y, v3 c) {
        addQuad(c - x - y, c + x - y, c + x + y, c - x + y, n, white);
    };
    
    // Helper for Boxes
    auto addBox = [&](v3 center, v3 size, float angleY, v3 color, std::string name) {
        uint32_t startIdx = (uint32_t)indices.size();
        
        // Basis
        float c = cos(angleY);
        float s = sin(angleY);
        
        v3 u = {c * size.x * 0.5f, 0.0f, -s * size.x * 0.5f};
        v3 v = {0.0f, size.y * 0.5f, 0.0f};
        v3 w = {s * size.z * 0.5f, 0.0f, c * size.z * 0.5f};
        
        // Top
        addQuad(center + v - u + w, center + v + u + w, center + v + u - w, center + v - u - w, v3{0, 1, 0}, color);
        // Front
        addQuad(center - v - u + w, center - v + u + w, center + v + u + w, center + v - u + w, v3{0, 0, 1}, color); // Normal approx
        // Right
        addQuad(center - v + u + w, center - v + u - w, center + v + u - w, center + v + u + w, v3{1, 0, 0}, color);
        // Back
        addQuad(center - v + u - w, center - v - u - w, center + v - u - w, center + v + u - w, v3{0, 0, -1}, color);
        // Left
        addQuad(center - v - u - w, center - v - u + w, center + v - u + w, center + v - u - w, v3{-1, 0, 0}, color);
        
        // Bottom (optional, usually hidden)
        addQuad(center - v - u - w, center - v - u + w, center + v - u + w, center + v - u - w, v3{0, -1, 0}, color);
        
        uint32_t endIdx = (uint32_t)indices.size();
        drawRanges[name] = {startIdx, endIdx - startIdx};
    };
    
    uint32_t sceneStart = (uint32_t)indices.size();
    
    // Floor (White)
    addQuad(v3{-h, -h, -h}, v3{ h, -h, -h}, v3{ h, -h,  h}, v3{-h, -h,  h}, v3{0, 1, 0}, white);
    
    // Ceiling (White)
    addQuad(v3{-h,  h,  h}, v3{ h,  h,  h}, v3{ h,  h, -h}, v3{-h,  h, -h}, v3{0, -1, 0}, white);
    
    // Back Wall (White)
    addQuad(v3{ h, -h, -h}, v3{-h, -h, -h}, v3{-h,  h, -h}, v3{ h,  h, -h}, v3{0, 0, 1}, white);
    
    // Left Wall (Red)
    addQuad(v3{-h, -h, -h}, v3{-h, -h,  h}, v3{-h,  h,  h}, v3{-h,  h, -h}, v3{1, 0, 0}, red);
    
    // Right Wall (Green)
    addQuad(v3{ h, -h,  h}, v3{ h, -h, -h}, v3{ h,  h, -h}, v3{ h,  h,  h}, v3{-1, 0, 0}, green);
    
    uint32_t sceneEnd = (uint32_t)indices.size();
    drawRanges["Room"] = {sceneStart, sceneEnd - sceneStart};
    
    // Tall Box (Right) -> Make this the Mirror
    addBox(v3{2.0f, -2.0f, 1.0f}, v3{3.0f, 6.0f, 3.0f}, -0.3f, white, "MirrorBox");
    
    // Short Box (Left)
    addBox(v3{-2.0f, -3.5f, -1.0f}, v3{3.0f, 3.0f, 3.0f}, 0.3f, white, "ShortBox");

    
    /*
    // Create Skybox-like Cube (Inside View)
    // Center at 0,0,0. Size 10.
    // Normals point INWARD.
    
    float d = 10.0f;
    
    // Front (+Z) - Center (0,0,d) - Normal (0,0,-1) - Up (0,1,0) - Right (-1,0,0) [Looking from inside]
    // Let's stick to standard external normals but reverse winding or just rely on culling being None.
    // Since we set CullMode::None, geometry is visible from both sides.
    // We just need to place faces surrounding the origin.
    
    // Front (+Z)
    addFace(primal::math::v3{0, 0, -1}, primal::math::v3{d, 0, 0}, primal::math::v3{0, d, 0}, primal::math::v3{0, 0, d});
    // Back (-Z)
    addFace(primal::math::v3{0, 0, 1}, primal::math::v3{-d, 0, 0}, primal::math::v3{0, d, 0}, primal::math::v3{0, 0, -d});
    // Right (+X)
    addFace(primal::math::v3{-1, 0, 0}, primal::math::v3{0, 0, -d}, primal::math::v3{0, d, 0}, primal::math::v3{d, 0, 0});
    // Left (-X)
    addFace(primal::math::v3{1, 0, 0}, primal::math::v3{0, 0, d}, primal::math::v3{0, d, 0}, primal::math::v3{-d, 0, 0});
    // Top (+Y)
    addFace(primal::math::v3{0, -1, 0}, primal::math::v3{d, 0, 0}, primal::math::v3{0, 0, -d}, primal::math::v3{0, d, 0});
    // Bottom (-Y)
    addFace(primal::math::v3{0, 1, 0}, primal::math::v3{d, 0, 0}, primal::math::v3{0, 0, d}, primal::math::v3{0, -d, 0});
    */

    // Add a floating small cube at (0, 0, 5) inside the +Z view
        {
            float sd = 1.0f; // Small cube size
            primal::math::v3 center = {0.0f, 0.0f, 5.0f};
            
            // Define Rotated Basis Vectors (45 deg around Y and X)
            // Approx values for normalized vectors
            // Right (X')
            primal::math::v3 right = {0.707f, 0.0f, -0.707f};
            // Up (Y') - Rotated 45 deg around X axis relative to world? 
            // Let's just use an arbitrary rotation matrix manually
            // Rot Y 45: X=(0.707, 0, -0.707), Y=(0,1,0), Z=(0.707, 0, 0.707)
            // Rot X 45: 
            // X_final = (0.707, 0, -0.707)
            // Y_final = (0.5, 0.707, 0.5) 
            // Z_final = (0.5, -0.707, 0.5)
            
            // Re-normalizing to be safe
            // Standard Axis Aligned Cornell Box
            // X axis
            primal::math::v3 X = {1.0f, 0.0f, 0.0f};
            // Y axis
            primal::math::v3 Y = {0.0f, 1.0f, 0.0f};
            // Z axis
            primal::math::v3 Z = {0.0f, 0.0f, 1.0f};
            
            // Scale
            X.x *= sd; X.y *= sd; X.z *= sd;
            Y.x *= sd; Y.y *= sd; Y.z *= sd;
            Z.x *= sd; Z.y *= sd; Z.z *= sd;

            // Front (+Z) - OPEN for Cornell Box
            // addFace(Z, X, Y, center + Z);
            
            // Back (-Z) - Green Wall (using Color attribute logic in addFace?)
            // Actually addFace uses normal to determine color in current implementation
            addFace(primal::math::v3{-Z.x, -Z.y, -Z.z}, primal::math::v3{-X.x, -X.y, -X.z}, Y, center - Z);
            // Right (+X) - Red
            addFace(X, primal::math::v3{-Z.x, -Z.y, -Z.z}, Y, center + X);
            // Left (-X) - Blue/Green?
            addFace(primal::math::v3{-X.x, -X.y, -X.z}, Z, Y, center - X);
            // Top (+Y) - White
            addFace(Y, X, primal::math::v3{-Z.x, -Z.y, -Z.z}, center + Y);
            // Bottom (-Y) - White
            addFace(primal::math::v3{-Y.x, -Y.y, -Y.z}, X, Z, center - Y);
        }

    // Create RHI Buffers for Cube
        // Note: In a real scenario, we'd use a RenderMesh class that manages these.
        // For this test, we create raw buffers and bind them manually.
        
        BufferDesc vbDesc;
        vbDesc.size = vertices.size() * sizeof(Vertex);
        vbDesc.vertex.vertexStride = sizeof(Vertex);
        vbDesc.type = BufferType::Vertex;
        vbDesc.usage = GPUMemoryUsage::Static;
        vbDesc.memoryUsage = GPUMemoryUsage::Static;
        vbDesc.bindFlags = static_cast<uint32_t>(BufferUsageFlags::Vertex);

        vertexBuffer = device->CreateBuffer(vbDesc);
        
        void* vbData = device->MapBuffer(vertexBuffer);
        memcpy(vbData, vertices.data(), vbDesc.size);
        device->UnmapBuffer(vertexBuffer);
        
        BufferDesc ibDesc;
        ibDesc.size = indices.size() * sizeof(uint32_t);
        ibDesc.index.format = DataFormat::R32_UInt;
        ibDesc.type = BufferType::Index;
        ibDesc.usage = GPUMemoryUsage::Dynamic;
        ibDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        ibDesc.bindFlags = static_cast<uint32_t>(BufferUsageFlags::Index);
        
        indexBuffer = device->CreateBuffer(ibDesc);
        
        void* ibData = device->MapBuffer(indexBuffer);
        memcpy(ibData, indices.data(), ibDesc.size);
        device->UnmapBuffer(indexBuffer);
        
        indexCount = (uint32_t)indices.size();
        std::cout << "DEBUG: CreateCubeMesh Complete. Vertices: " << vertices.size() << ", Indices: " << indexCount << std::endl;
    }

bool MultiViewTestCase::Initialize() {
    std::cout << ">>> STARTING MULTI-VIEW TEST (DIRECT RENDER MODE - DEBUG) <<<" << std::endl;
    // 1. Initialize Window
    primal::platform::window_init_info info{};
    info.caption = "Multi-View Integration Test (Grid Shader)";
    info.width = 1280;
    info.height = 720;
    info.left = 200;
    info.top = 200;
    window = primal::platform::create_window(&info);
    
    if (!window.is_valid()) return false;

    // 2. Create Device
    DeviceDesc deviceDesc{};
    deviceDesc.platform = RHIPlatform::Metal;
    deviceDesc.enableDebug = true;
    auto metalDevice = std::make_unique<MetalDevice>(deviceDesc);
    if (!metalDevice->Initialize()) {
        return false;
    }
    
    device_ownership = std::move(metalDevice);
    device = device_ownership.get();
    
    // 3. Initialize Render System
    RenderSystemInitInfo sysInfo;
    sysInfo.device = device;
    sysInfo.window = window.handle();
    sysInfo.width = info.width;
    sysInfo.height = info.height;

    if (!renderSystem.Initialize(sysInfo)) {
        return false;
    }
    
    // 4. Create RenderGraph
    renderGraph = std::make_unique<RenderGraph>(*device);
    
    // 5. Load Shader
    std::string shaderPath = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/shaders/MultiView.metal"; 
    std::string shaderSource = ReadShaderFile(shaderPath);
    if (shaderSource.empty()) {
        std::cout << "Failed to load shader: " << shaderPath << std::endl;
        return false;
    }
    
    std::cout << "Loaded Shader Source. Length: " << shaderSource.length() << std::endl;
    if (shaderSource.find("vertexMainSimple") != std::string::npos) {
        std::cout << "SHADER CHECK: Found 'vertexMainSimple' in source code." << std::endl;
    } else {
        std::cout << "SHADER CHECK: FAILED to find 'vertexMainSimple' in source code!" << std::endl;
    }

    vertexShader = device->CreateShader(shaderSource.data(), shaderSource.size(), ShaderStage::Vertex, "vertexMain");
    pixelShader = device->CreateShader(shaderSource.data(), shaderSource.size(), ShaderStage::Pixel, "fragmentMain");
    
    if (vertexShader == handles::INVALID_SHADER || pixelShader == handles::INVALID_SHADER) {
        std::cout << "Failed to create shaders" << std::endl;
        return false;
    }

    // 6. Create Pipeline Layout
    PipelineLayoutDesc layoutDesc;
    
    DescriptorSetLayoutBinding bindings[2];
    bindings[0].binding = 1; // ViewUniforms
    bindings[0].descriptorType = DescriptorType::UniformBuffer;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = ShaderStage::Vertex;
    
    bindings[1].binding = 2; // InstanceUniforms
    bindings[1].descriptorType = DescriptorType::UniformBuffer;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = ShaderStage::Vertex | ShaderStage::Pixel; // Vertex + Fragment
    
    DescriptorSetLayoutDesc dsDesc;
    dsDesc.bindingCount = 2;
    dsDesc.bindings = bindings;
    
    dsLayout = device->CreateDescriptorSetLayout(dsDesc);
    
    layoutDesc.setLayoutCount = 1;
    layoutDesc.setLayouts = &dsLayout;
    
    pipelineLayout = device->CreatePipelineLayout(layoutDesc);
    
    // 7. Create Graphics Pipeline
    GraphicsPipelineDesc pipelineDesc;
    pipelineDesc.vertexShader = vertexShader;
    pipelineDesc.pixelShader = pixelShader;
    pipelineDesc.layout = pipelineLayout;
    pipelineDesc.topology = PrimitiveTopology::TriangleList;
    pipelineDesc.cullMode = CullMode::None; // Disable culling to see inside of cube
    
    // Vertex Input
    VertexInputBinding binding;
    binding.binding = 0;
    binding.stride = sizeof(float) * 11; // 3+3+2+3
    binding.perVertex = true;
    
    pipelineDesc.vertexBindings.push_back(binding);
    
    VertexInputAttribute attr0; // Pos
    attr0.location = 0;
    attr0.binding = 0;
    attr0.format = DataFormat::RGB32_Float;
    attr0.offset = 0;
    
    VertexInputAttribute attr1; // Normal
    attr1.location = 1;
    attr1.binding = 0;
    attr1.format = DataFormat::RGB32_Float;
    attr1.offset = sizeof(float) * 3;
    
    VertexInputAttribute attr2; // UV
    attr2.location = 2;
    attr2.binding = 0;
    attr2.format = DataFormat::RG32_Float;
    attr2.offset = sizeof(float) * 6;

    VertexInputAttribute attr3; // Color
    attr3.location = 3;
    attr3.binding = 0;
    attr3.format = DataFormat::RGB32_Float;
    attr3.offset = sizeof(float) * 8;
    
    pipelineDesc.vertexAttributes.push_back(attr0);
    pipelineDesc.vertexAttributes.push_back(attr1);
    pipelineDesc.vertexAttributes.push_back(attr2);
    pipelineDesc.vertexAttributes.push_back(attr3);
    
    TextureDesc backBufferDesc = renderSystem.GetBackBufferDesc(); // Get actual format
    pipelineDesc.renderTargetCount = 1;
    pipelineDesc.renderTargetFormats[0] = backBufferDesc.format; // Use actual format
    std::cout << "DEBUG: Pipeline Target Format: " << (int)backBufferDesc.format << std::endl;

    // Enable Depth Test for MultiView Pass (so small cube occludes big cube)
    pipelineDesc.depthStencilFormat = DataFormat::D32_Float;
    pipelineDesc.enableDepthTest = true;
    pipelineDesc.enableDepthWrite = true;
    pipelineDesc.cullMode = CullMode::None; // Ensure double-sided rendering for safety

    std::cout << "DEBUG: Pipeline Setup - Depth Test: Enabled, CompareOp: Less, CullMode: None" << std::endl;

    pipeline = device->CreateGraphicsPipeline(pipelineDesc);
    if (pipeline == handles::INVALID_PIPELINE) {
        std::cout << "Failed to create pipeline" << std::endl;
        return false;
    }

    // 7.05 Create Simple Pipeline
    // USE DEBUG SHADER TO VERIFY DRAW CALLS
    // simpleVertexShader = device->CreateShader(shaderSource.data(), shaderSource.size(), ShaderStage::Vertex, "vertexMainBufferDebug");
    simpleVertexShader = device->CreateShader(shaderSource.data(), shaderSource.size(), ShaderStage::Vertex, "vertexMainSimple");
    if (simpleVertexShader == handles::INVALID_SHADER) std::cout << "CRITICAL: Failed to create vertexMainSimple" << std::endl;

    simplePixelShader = device->CreateShader(shaderSource.data(), shaderSource.size(), ShaderStage::Pixel, "fragmentMainSimple");
    if (simplePixelShader == handles::INVALID_SHADER) std::cout << "CRITICAL: Failed to create fragmentMainSimple" << std::endl;

    // Create Empty Layout
    // PipelineLayoutDesc emptyLayoutDesc;
    // PipelineLayoutHandle emptyLayout = device->CreatePipelineLayout(emptyLayoutDesc);

    GraphicsPipelineDesc simpleDesc = pipelineDesc;
    simpleDesc.vertexShader = simpleVertexShader;
    simpleDesc.pixelShader = simplePixelShader;
    simpleDesc.layout = pipelineLayout; // Use Correct Layout with Uniforms
    // simpleDesc.layout = emptyLayout; 
    
    // simpleDesc.vertexAttributes.clear();
    // simpleDesc.vertexBindings.clear();
    simpleDesc.cullMode = CullMode::None;
    simpleDesc.enableDepthTest = false;
    simpleDesc.enableDepthWrite = false;
    simpleDesc.topology = PrimitiveTopology::TriangleList; // Explicitly set topology

    backBufferDesc = renderSystem.GetBackBufferDesc(); 
    simpleDesc.renderTargetCount = 1;
    simpleDesc.renderTargetFormats[0] = backBufferDesc.format;
    std::cout << "DEBUG: Pipeline Target Format: " << (int)backBufferDesc.format << std::endl;
    
    simplePipeline = device->CreateGraphicsPipeline(simpleDesc);
    if (simplePipeline == handles::INVALID_PIPELINE) {
        std::cout << "CRITICAL ERROR: Failed to create Simple Pipeline!" << std::endl;
        return false;
    } else {
        std::cout << "DEBUG: Simple Pipeline Created Successfully." << std::endl;
    }

    // 7.05 Create Main View Pipeline (Matches BackBuffer Format)
    // Use Single View Shaders (No Layer Output)
    rhi::ShaderHandle vertexShaderSingle = device->CreateShader(shaderSource.data(), shaderSource.size(), ShaderStage::Vertex, "vertexMainSingle");
    rhi::ShaderHandle pixelShaderSingle = device->CreateShader(shaderSource.data(), shaderSource.size(), ShaderStage::Pixel, "fragmentMainSingle");

    GraphicsPipelineDesc mainPipeDesc = pipelineDesc;
    mainPipeDesc.vertexShader = vertexShaderSingle;
    mainPipeDesc.pixelShader = pixelShaderSingle;
    
    // Update Target Format to match BackBuffer
    TextureDesc swapchainDesc = renderSystem.GetBackBufferDesc();
    mainPipeDesc.renderTargetFormats[0] = swapchainDesc.format;
    
    // Ensure Layout is correct (should be same as pipelineLayout)
    mainPipeDesc.layout = pipelineLayout;
    
    // Create Pipeline
    mainPipeline = device->CreateGraphicsPipeline(mainPipeDesc);
    if (mainPipeline == handles::INVALID_PIPELINE) {
        std::cout << "CRITICAL: Failed to create Main View Pipeline!" << std::endl;
    } else {
        std::cout << "DEBUG: Main View Pipeline Created. Format: " << (int)swapchainDesc.format << std::endl;
    }

    // 7.1 Create Blit Shaders & Pipeline
    blitVertexShader = device->CreateShader(shaderSource.data(), shaderSource.size(), ShaderStage::Vertex, "blitVertex");
    blitPixelShader = device->CreateShader(shaderSource.data(), shaderSource.size(), ShaderStage::Pixel, "blitFragment");
    
    DescriptorSetLayoutDesc blitDSDesc{};
    DescriptorSetLayoutBinding blitBindings[2];
    
    // Binding 0: Texture
    blitBindings[0].binding = 0;
    blitBindings[0].descriptorType = DescriptorType::SampledImage;
    blitBindings[0].descriptorCount = 1;
    blitBindings[0].stageFlags = ShaderStage::Pixel;
    
    // Binding 1: Uniform Buffer (Rotation)
    blitBindings[1].binding = 1;
    blitBindings[1].descriptorType = DescriptorType::UniformBuffer;
    blitBindings[1].descriptorCount = 1;
    blitBindings[1].stageFlags = ShaderStage::Pixel;
    
    blitDSDesc.bindingCount = 2;
    blitDSDesc.bindings = blitBindings;
    blitDSLayout = device->CreateDescriptorSetLayout(blitDSDesc);
    
    // Create Blit Uniform Buffer
    BufferDesc bubDesc;
    bubDesc.size = sizeof(math::m4x4); // Rotation Matrix
    bubDesc.usage = GPUMemoryUsage::Dynamic;
    bubDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    bubDesc.type = BufferType::Constant;
    bubDesc.bindFlags = static_cast<uint32_t>(BufferUsageFlags::Uniform);
    blitUniformBuffer = device->CreateBuffer(bubDesc);

    DescriptorSetDesc blitSetDesc;
    blitSetDesc.layout = blitDSLayout;
    
    blitDescriptorSets.resize(MAX_FRAMES_IN_FLIGHT);
    for(uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        blitDescriptorSets[i] = device->CreateDescriptorSet(blitSetDesc);
        
        // Update Buffer Binding immediately
        WriteDescriptorSet bufUpdate{};
        DescriptorBufferInfo bufInfo{};
        bufInfo.buffer = blitUniformBuffer;
        bufInfo.offset = 0;
        bufInfo.range = sizeof(math::m4x4);
        
        bufUpdate.dstSet = blitDescriptorSets[i];
        bufUpdate.dstBinding = 1; // Binding 1
        bufUpdate.descriptorCount = 1;
        bufUpdate.descriptorType = DescriptorType::UniformBuffer;
        bufUpdate.bufferInfo = &bufInfo;
        
        device->UpdateDescriptorSets(1, &bufUpdate);
    }
    
    PipelineLayoutDesc blitPLDesc{};
    blitPLDesc.setLayouts = &blitDSLayout;
    blitPLDesc.setLayoutCount = 1;
    blitPipelineLayout = device->CreatePipelineLayout(blitPLDesc);
    
    GraphicsPipelineDesc blitPipeDesc{};
    blitPipeDesc.vertexShader = blitVertexShader;
    blitPipeDesc.pixelShader = blitPixelShader;
    blitPipeDesc.layout = blitPipelineLayout;
    blitPipeDesc.cullMode = CullMode::None;
    blitPipeDesc.renderTargetCount = 1;
    blitPipeDesc.renderTargetFormats[0] = renderSystem.GetBackBufferDesc().format; // Use correct format
    blitPipeDesc.depthStencilFormat = DataFormat::Unknown;
    blitPipeline = device->CreateGraphicsPipeline(blitPipeDesc);
    
    if (blitPipeline == handles::INVALID_PIPELINE) {
        std::cout << "CRITICAL: Failed to create Blit Pipeline!" << std::endl;
    }

    // 7.2 Create Debug Overlay Shaders & Pipeline
    debugVertexShader = device->CreateShader(shaderSource.data(), shaderSource.size(), ShaderStage::Vertex, "debugVertex");
    if (debugVertexShader == handles::INVALID_SHADER) std::cout << "CRITICAL: Failed to create debugVertex shader" << std::endl;
    
    debugPixelShader = device->CreateShader(shaderSource.data(), shaderSource.size(), ShaderStage::Pixel, "debugFragment");
    if (debugPixelShader == handles::INVALID_SHADER) std::cout << "CRITICAL: Failed to create debugFragment shader" << std::endl;

    GraphicsPipelineDesc debugPipeDesc = blitPipeDesc;
    debugPipeDesc.vertexShader = debugVertexShader;
    debugPipeDesc.pixelShader = debugPixelShader;
    
    // Disable Depth Test for Debug Overlay
    debugPipeDesc.enableDepthTest = false;
    debugPipeDesc.enableDepthWrite = false;

    // Keep same layout (blitPipelineLayout) and other settings
    
    debugPipeline = device->CreateGraphicsPipeline(debugPipeDesc);
    if (debugPipeline == handles::INVALID_PIPELINE) {
        std::cout << "CRITICAL: Failed to create Debug Pipeline!" << std::endl;
    } else {
        std::cout << "DEBUG: Debug Pipeline Created Successfully." << std::endl;
    }
    
    // 8. Create Uniform Buffers
    BufferDesc ubDesc;
    ubDesc.size = sizeof(math::m4x4) * 6; // View Uniforms (6 ViewProjs)
    ubDesc.usage = GPUMemoryUsage::Dynamic;
    ubDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    ubDesc.type = BufferType::Constant;
    ubDesc.bindFlags = static_cast<uint32_t>(BufferUsageFlags::Uniform);
    viewUniformBuffer = device->CreateBuffer(ubDesc);
    
    // Fill View Uniforms
    {
        void* data = device->MapBuffer(viewUniformBuffer);
        if (data) {
            m4x4* matrices = static_cast<m4x4*>(data);
            
            // Projection (90 deg, 1.0 aspect)
            // Metal NDC is Z [0, 1]
            m4x4 proj = CreatePerspective(math::constants::HALF_PI, 1.0f, 0.1f, 100.0f);
            
            // 6 Faces
            v3 eye = primal::math::v3{0, 0, 0};
            
            // DEBUG: Print Matrices
            std::cout << "DEBUG: Projection Matrix:" << std::endl;
            // ... (Simple print logic if needed)

            // +X
            matrices[0] = proj * CreateLookAt(eye, primal::math::v3{1, 0, 0}, primal::math::v3{0, -1, 0});
            // -X
            matrices[1] = proj * CreateLookAt(eye, primal::math::v3{-1, 0, 0}, primal::math::v3{0, -1, 0});
            // +Y
            matrices[2] = proj * CreateLookAt(eye, primal::math::v3{0, 1, 0}, primal::math::v3{0, 0, 1});
            // -Y
            matrices[3] = proj * CreateLookAt(eye, primal::math::v3{0, -1, 0}, primal::math::v3{0, 0, -1});
            // +Z
            matrices[4] = proj * CreateLookAt(eye, primal::math::v3{0, 0, 1}, primal::math::v3{0, -1, 0});
            // -Z
            matrices[5] = proj * CreateLookAt(eye, primal::math::v3{0, 0, -1}, primal::math::v3{0, -1, 0});
            
            device->UnmapBuffer(viewUniformBuffer);
        }
    }

    // 8.1 Create Main View Uniform Buffer
    {
        BufferDesc bufDesc;
        bufDesc.size = sizeof(math::m4x4) * 6;
        bufDesc.usage = GPUMemoryUsage::Dynamic;
        bufDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        bufDesc.type = BufferType::Constant;
        bufDesc.bindFlags = static_cast<uint32_t>(BufferUsageFlags::Uniform);
        mainViewUniformBuffer = device->CreateBuffer(bufDesc);
    }
    
    // 8.2 Create Depth Textures
    {
        // MultiView Depth (CubeMap / Array)
        TextureDesc depthDesc{};
        depthDesc.size = {512, 512, 1};
        depthDesc.mipLevels = 1;
        depthDesc.arraySize = 6;
        depthDesc.format = DataFormat::D32_Float;
        depthDesc.type = TextureType::Texture2DArray;
        depthDesc.usage = TextureUsage::DepthStencil;
        multiViewDepthTexture = device->CreateTexture(depthDesc);

        // Main View Depth (Match BackBuffer Size)
        TextureDesc bbDesc = renderSystem.GetBackBufferDesc();
        if (bbDesc.size.x == 0) {
            bbDesc.size.x = info.width * 2; // Assume Retina 2x fallback
            bbDesc.size.y = info.height * 2;
        }
        std::cout << "DEBUG: Creating Main Depth Texture: " << bbDesc.size.x << "x" << bbDesc.size.y << std::endl;

        TextureDesc mainDepthDesc{};
        mainDepthDesc.size = {bbDesc.size.x, bbDesc.size.y, 1};
        mainDepthDesc.mipLevels = 1;
        mainDepthDesc.arraySize = 1;
        mainDepthDesc.format = DataFormat::D32_Float;
        mainDepthDesc.type = TextureType::Texture2D;
        mainDepthDesc.usage = TextureUsage::DepthStencil;
        mainDepthTexture = device->CreateTexture(mainDepthDesc);
    }
    
    // Scene Data Structure (must match Shader)
    struct SceneData {
        m4x4 model;
        v4 lightPos;
        v4 lightColor;
    };

    ubDesc.size = sizeof(SceneData); // Instance Uniforms + Light Data
    instanceUniformBuffer = device->CreateBuffer(ubDesc);
    
    // Fill Instance Uniforms
    {
        void* data = device->MapBuffer(instanceUniformBuffer);
        if (data) {
            SceneData* sceneData = static_cast<SceneData*>(data);
            sceneData->model = primal::graphics::rhi::math::MatrixIdentity();
            sceneData->lightPos = {0.0f, 4.0f, 0.0f, 1.0f}; // Top Light
            sceneData->lightColor = {1.0f, 1.0f, 1.0f, 1.0f}; // White
            device->UnmapBuffer(instanceUniformBuffer);
        }
    }
    
    // 9. Create Descriptor Set
    DescriptorSetDesc setDesc;
    setDesc.layout = dsLayout;
    descriptorSet = device->CreateDescriptorSet(setDesc);
    
    // 9.1 Create Main View Descriptor Set
    mainDescriptorSet = device->CreateDescriptorSet(setDesc);

    WriteDescriptorSet updates[4]; // Updated count
    DescriptorBufferInfo bufInfo0;
    bufInfo0.buffer = viewUniformBuffer;
    bufInfo0.offset = 0;
    bufInfo0.range = sizeof(math::m4x4) * 6;

    updates[0].dstSet = descriptorSet;
    updates[0].dstBinding = 1;
    updates[0].dstArrayElement = 0;
    updates[0].descriptorCount = 1;
    updates[0].descriptorType = DescriptorType::UniformBuffer;
    updates[0].bufferInfo = &bufInfo0;
    
    DescriptorBufferInfo bufInfo1;
    bufInfo1.buffer = instanceUniformBuffer;
    bufInfo1.offset = 0;
    bufInfo1.range = sizeof(SceneData);

    updates[1].dstSet = descriptorSet;
    updates[1].dstBinding = 2;
    updates[1].dstArrayElement = 0;
    updates[1].descriptorCount = 1;
    updates[1].descriptorType = DescriptorType::UniformBuffer;
    updates[1].bufferInfo = &bufInfo1;

    // Updates for Main View Descriptor Set
    DescriptorBufferInfo bufInfoMain;
    bufInfoMain.buffer = mainViewUniformBuffer;
    bufInfoMain.offset = 0;
    bufInfoMain.range = sizeof(math::m4x4) * 6;

    updates[2].dstSet = mainDescriptorSet;
    updates[2].dstBinding = 1;
    updates[2].dstArrayElement = 0;
    updates[2].descriptorCount = 1;
    updates[2].descriptorType = DescriptorType::UniformBuffer;
    updates[2].bufferInfo = &bufInfoMain;

    // Reuse instance buffer for main view (same lights)
    updates[3].dstSet = mainDescriptorSet;
    updates[3].dstBinding = 2;
    updates[3].dstArrayElement = 0;
    updates[3].descriptorCount = 1;
    updates[3].descriptorType = DescriptorType::UniformBuffer;
    updates[3].bufferInfo = &bufInfo1;
    
    device->UpdateDescriptorSets(4, updates);
    
    // 10. Create Cube Mesh
    CreateCubeMesh();
    
    // 11. Create Command Buffers (Per Frame)
    // commandBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    // for(uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
    //     commandBuffers[i] = device->CreateCommandBuffer(CommandQueueType::Graphics);
    // }

    // 12. Setup Input for Debug Toggle
    using namespace primal::input;
    input_source source{};
    source.binding = std::hash<std::string>()("debug_toggle");
    source.source_type = input_source::keyboard;
    source.code = input_code::key_f1;
    source.multiplier = 1.0f;
    bind(source);
    
    if (!CreateReflectionResources()) return false;

    return true;
}

std::string MultiViewTestCase::ReadShaderFile(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) return "";
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

bool MultiViewTestCase::CreateReflectionResources() {
    std::cout << "DEBUG: Creating Reflection Resources..." << std::endl;
    
    // 1. Textures
    TextureDesc texDesc = renderSystem.GetBackBufferDesc();
    if (texDesc.size.x == 0) texDesc.size = {1280*2, 720*2, 1}; // Fallback
    
    // Reflection Map (Half Res)
    texDesc.size.x /= 2;
    texDesc.size.y /= 2;
    texDesc.format = DataFormat::BGRA8_UNorm;
    texDesc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
    reflectionTexture = device->CreateTexture(texDesc);
    
    // Depth
    texDesc.format = DataFormat::D32_Float;
    texDesc.usage = TextureUsage::DepthStencil;
    reflectionDepthTexture = device->CreateTexture(texDesc);
    
    // 2. Buffers
    BufferDesc ubDesc;
    ubDesc.size = sizeof(math::m4x4) * 6; // Standard View Buffer size
    ubDesc.usage = GPUMemoryUsage::Dynamic;
    ubDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    ubDesc.type = BufferType::Constant;
    ubDesc.bindFlags = static_cast<uint32_t>(BufferUsageFlags::Uniform);
    reflectionUniformBuffer = device->CreateBuffer(ubDesc);
    
    ubDesc.size = sizeof(math::v4); // Plane Equation
    reflectionPlaneBuffer = device->CreateBuffer(ubDesc);
    
    // 3. Shaders
    std::string shaderPath = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/shaders/MultiView.metal"; 
    std::string shaderSource = ReadShaderFile(shaderPath);
    
    reflectionVertexShader = device->CreateShader(shaderSource.data(), shaderSource.size(), ShaderStage::Vertex, "vertexMainReflection");
    mirrorVertexShader = device->CreateShader(shaderSource.data(), shaderSource.size(), ShaderStage::Vertex, "vertexMainMirror");
    mirrorPixelShader = device->CreateShader(shaderSource.data(), shaderSource.size(), ShaderStage::Pixel, "fragmentMainMirror");
    
    if (reflectionVertexShader == handles::INVALID_SHADER || mirrorVertexShader == handles::INVALID_SHADER) {
        std::cout << "CRITICAL: Failed to create Reflection Shaders" << std::endl;
        return false;
    }
    
    // 4. Pipelines & Layouts
    
    // --- Reflection Pipeline ---
    // Layout: Set 0 (Binding 1: View, Binding 2: Scene, Binding 3: Plane)
    DescriptorSetLayoutBinding reflBindings[3];
    reflBindings[0].binding = 1; reflBindings[0].descriptorType = DescriptorType::UniformBuffer; reflBindings[0].descriptorCount = 1; reflBindings[0].stageFlags = ShaderStage::Vertex;
    reflBindings[1].binding = 2; reflBindings[1].descriptorType = DescriptorType::UniformBuffer; reflBindings[1].descriptorCount = 1; reflBindings[1].stageFlags = ShaderStage::Vertex | ShaderStage::Pixel;
    reflBindings[2].binding = 3; reflBindings[2].descriptorType = DescriptorType::UniformBuffer; reflBindings[2].descriptorCount = 1; reflBindings[2].stageFlags = ShaderStage::Vertex;
    
    DescriptorSetLayoutDesc reflLayoutDesc;
    reflLayoutDesc.bindingCount = 3;
    reflLayoutDesc.bindings = reflBindings;
    DescriptorSetLayoutHandle reflDSLayout = device->CreateDescriptorSetLayout(reflLayoutDesc);
    
    PipelineLayoutDesc reflPLDesc;
    reflPLDesc.setLayoutCount = 1;
    reflPLDesc.setLayouts = &reflDSLayout;
    reflectionPipelineLayout = device->CreatePipelineLayout(reflPLDesc);
    
    GraphicsPipelineDesc reflPipeDesc;
    reflPipeDesc.vertexShader = reflectionVertexShader;
    reflPipeDesc.pixelShader = pixelShader; // Reuse standard fragment shader (just lighting)
    reflPipeDesc.layout = reflectionPipelineLayout;
    reflPipeDesc.topology = PrimitiveTopology::TriangleList;
    reflPipeDesc.cullMode = CullMode::None; // Safe
    reflPipeDesc.enableDepthTest = true;
    reflPipeDesc.enableDepthWrite = true;
    reflPipeDesc.depthStencilFormat = DataFormat::D32_Float;
    reflPipeDesc.renderTargetCount = 1;
    reflPipeDesc.renderTargetFormats[0] = DataFormat::BGRA8_UNorm;
    
    // Vertex Input (Standard)
    reflPipeDesc.vertexBindings.push_back({0, sizeof(float)*11, true});
    reflPipeDesc.vertexAttributes.push_back({0, 0, DataFormat::RGB32_Float, 0}); // Pos
    reflPipeDesc.vertexAttributes.push_back({1, 0, DataFormat::RGB32_Float, sizeof(float)*3}); // Normal
    reflPipeDesc.vertexAttributes.push_back({2, 0, DataFormat::RG32_Float, sizeof(float)*6}); // UV
    reflPipeDesc.vertexAttributes.push_back({3, 0, DataFormat::RGB32_Float, sizeof(float)*8}); // Color
    
    reflectionPipeline = device->CreateGraphicsPipeline(reflPipeDesc);
    
    // --- Mirror Pipeline ---
    // Layout: Binding 0: Texture, Binding 1: View, Binding 2: Scene
    DescriptorSetLayoutBinding mirBindings[3];
    mirBindings[0].binding = 1; mirBindings[0].descriptorType = DescriptorType::UniformBuffer; mirBindings[0].descriptorCount = 1; mirBindings[0].stageFlags = ShaderStage::Vertex;
    mirBindings[1].binding = 2; mirBindings[1].descriptorType = DescriptorType::UniformBuffer; mirBindings[1].descriptorCount = 1; mirBindings[1].stageFlags = ShaderStage::Vertex | ShaderStage::Pixel;
    mirBindings[2].binding = 0; mirBindings[2].descriptorType = DescriptorType::SampledImage; mirBindings[2].descriptorCount = 1; mirBindings[2].stageFlags = ShaderStage::Pixel;
    
    DescriptorSetLayoutDesc mirLayoutDesc;
    mirLayoutDesc.bindingCount = 3;
    mirLayoutDesc.bindings = mirBindings;
    DescriptorSetLayoutHandle mirDSLayout = device->CreateDescriptorSetLayout(mirLayoutDesc);
    
    PipelineLayoutDesc mirPLDesc;
    mirPLDesc.setLayoutCount = 1;
    mirPLDesc.setLayouts = &mirDSLayout;
    mirrorPipelineLayout = device->CreatePipelineLayout(mirPLDesc);
    
    GraphicsPipelineDesc mirPipeDesc = reflPipeDesc; // Copy basics
    mirPipeDesc.vertexShader = mirrorVertexShader;
    mirPipeDesc.pixelShader = mirrorPixelShader;
    mirPipeDesc.layout = mirrorPipelineLayout;
    mirPipeDesc.renderTargetFormats[0] = renderSystem.GetBackBufferDesc().format; // Main Pass Format
    
    mirrorPipeline = device->CreateGraphicsPipeline(mirPipeDesc);
    
    // 5. Create Descriptor Sets
    DescriptorSetDesc setDesc;
    setDesc.layout = reflDSLayout;
    reflectionDescriptorSet = device->CreateDescriptorSet(setDesc);
    
    setDesc.layout = mirDSLayout;
    mirrorDescriptorSet = device->CreateDescriptorSet(setDesc);
    
    // Define SceneData locally
    struct SceneData {
        m4x4 model;
        v4 lightPos;
        v4 lightColor;
    };
    
    // Update Reflection Descriptor Set
    {
        WriteDescriptorSet updates[3];
        DescriptorBufferInfo info1{}; info1.buffer = reflectionUniformBuffer; info1.offset = 0; info1.range = sizeof(math::m4x4)*6;
        DescriptorBufferInfo info2{}; info2.buffer = instanceUniformBuffer; info2.offset = 0; info2.range = sizeof(SceneData);
        DescriptorBufferInfo info3{}; info3.buffer = reflectionPlaneBuffer; info3.offset = 0; info3.range = sizeof(math::v4);
        
        updates[0].dstSet = reflectionDescriptorSet; updates[0].dstBinding = 1; updates[0].descriptorCount = 1; updates[0].descriptorType = DescriptorType::UniformBuffer; updates[0].bufferInfo = &info1;
        updates[1].dstSet = reflectionDescriptorSet; updates[1].dstBinding = 2; updates[1].descriptorCount = 1; updates[1].descriptorType = DescriptorType::UniformBuffer; updates[1].bufferInfo = &info2;
        updates[2].dstSet = reflectionDescriptorSet; updates[2].dstBinding = 3; updates[2].descriptorCount = 1; updates[2].descriptorType = DescriptorType::UniformBuffer; updates[2].bufferInfo = &info3;
        
        device->UpdateDescriptorSets(3, updates);
    }
    
    // Update Mirror Descriptor Set
    {
        WriteDescriptorSet updates[3];
        DescriptorBufferInfo info1{}; info1.buffer = mainViewUniformBuffer; info1.offset = 0; info1.range = sizeof(math::m4x4)*6;
        DescriptorBufferInfo info2{}; info2.buffer = instanceUniformBuffer; info2.offset = 0; info2.range = sizeof(SceneData);
        DescriptorImageInfo infoTex{}; infoTex.imageView = reflectionTexture; infoTex.sampler = handles::INVALID_RESOURCE; // Metal uses inline sampler
        
        updates[0].dstSet = mirrorDescriptorSet; updates[0].dstBinding = 1; updates[0].descriptorCount = 1; updates[0].descriptorType = DescriptorType::UniformBuffer; updates[0].bufferInfo = &info1;
        updates[1].dstSet = mirrorDescriptorSet; updates[1].dstBinding = 2; updates[1].descriptorCount = 1; updates[1].descriptorType = DescriptorType::UniformBuffer; updates[1].bufferInfo = &info2;
        updates[2].dstSet = mirrorDescriptorSet; updates[2].dstBinding = 0; updates[2].descriptorCount = 1; updates[2].descriptorType = DescriptorType::SampledImage; updates[2].imageInfo = &infoTex;
        
        device->UpdateDescriptorSets(3, updates);
    }
    
    return true;
}

void MultiViewTestCase::Run() {
    if (!window.is_valid()) return;
    // std::cout << "DEBUG: RUNNING MULTI-VIEW TEST (RenderGraph)" << std::endl;
    
    ResourceHandle backBuffer;
    SyncHandle fence;
    
    // Animate Light
    {
        // Toggle Debug Overlay with F1
        using namespace primal::input;
        input_value val;
        get(std::hash<std::string>()("debug_toggle"), val);
        
        static bool wasF1Down = false;
        bool isF1Down = (val.current.x > 0.0f);
        
        if (isF1Down && !wasF1Down) {
            showDebugOverlay = !showDebugOverlay;
            std::cout << "Debug Overlay: " << (showDebugOverlay ? "ON" : "OFF") << std::endl;
        }
        wasF1Down = isF1Down;

        static float time = 0.0f;
        time += 0.02f;
        
        // Define struct again or move to header (local is fine)
        struct SceneData {
            m4x4 model;
            v4 lightPos;
            v4 lightColor;
        };

        void* data = device->MapBuffer(instanceUniformBuffer);
        if (data) {
            SceneData* sceneData = static_cast<SceneData*>(data);
            
            // Move Light in circle
            sceneData->lightPos.x = sin(time) * 3.0f;
            sceneData->lightPos.z = cos(time) * 3.0f;
            sceneData->lightPos.y = 4.0f + sin(time * 0.5f); // Up/Down slightly
            
            // Change Color (RGB Cycle) - Make it softer/brighter
            sceneData->lightColor.x = (sin(time) * 0.3f + 0.7f);
            sceneData->lightColor.y = (sin(time + 2.09f) * 0.3f + 0.7f);
            sceneData->lightColor.z = (sin(time + 4.18f) * 0.3f + 0.7f);
            
            device->UnmapBuffer(instanceUniformBuffer);
        }
    }
    
    // ---------------------------------------------------------
    // REFLECTION UPDATE
    // ---------------------------------------------------------
    {
        // 1. Calculate Mirror Plane
        // MirrorBox parameters from CreateCubeMesh (addBox call)
        // Center: (2.0, -2.0, 1.0)
        // Size: (3.0, 6.0, 3.0) -> HalfSize: (1.5, 3.0, 1.5)
        // Angle: -0.3
        v3 mirrorPos = v3{2.0f, -2.0f, 1.0f}; 
        float angle = -0.3f;
        
        float c = cos(angle);
        float s = sin(angle);
        
        // Normal calculation:
        // In addBox, w (Z-axis) is defined as {s, 0, c}.
        // The front face is in the +w direction.
        v3 planeNormal = v3{s, 0.0f, c};
        planeNormal = Normalize(planeNormal); 
        
        // Point on plane:
        // Front face center is at Center + HalfSize.z * Normal
        // HalfSize.z = 1.5f (since full Z size is 3.0f)
        v3 planePoint = mirrorPos + planeNormal * 1.5f; 
        
        // Plane Eq: dot(N, P) + D = 0 => D = -dot(N, P)
        float planeD = -Dot(planeNormal, planePoint);
        v4 planeEq = {planeNormal.x, planeNormal.y, planeNormal.z, planeD};
        
        // Update Plane Buffer
        void* data = device->MapBuffer(reflectionPlaneBuffer);
        if (data) {
            memcpy(data, &planeEq, sizeof(v4));
            device->UnmapBuffer(reflectionPlaneBuffer);
        }
        
        // 2. Calculate Reflection Camera
        // Main Camera (Fixed)
        v3 eye = v3{0.0f, 0.0f, 18.0f}; 
        v3 center = v3{0.0f, 0.0f, 0.0f};
        v3 up = v3{0.0f, 1.0f, 0.0f};
        
        // Reflection Function
        auto Reflect = [&](v3 p) {
            float dist = Dot(p, planeNormal) + planeD;
            return p - planeNormal * (2.0f * dist);
        };
        
        v3 eyeRefl = Reflect(eye);
        v3 centerRefl = Reflect(center);
        
        // Reflect Up Vector (Direction only)
        // R_dir = Dir - 2 * dot(Dir, N) * N
        float upDot = Dot(up, planeNormal);
        v3 upRefl = up - planeNormal * (2.0f * upDot);
        
        m4x4 viewRefl = CreateLookAt(eyeRefl, centerRefl, upRefl);
        
        // Projection (Same as main)
        float aspect = (float)renderSystem.GetBackBufferDesc().size.x / (float)renderSystem.GetBackBufferDesc().size.y;
        if (aspect < 0.1f) aspect = 1280.0f / 720.0f;
        m4x4 projRefl = CreatePerspective(math::constants::PI / 4.0f, aspect, 0.1f, 100.0f);
        
        // Update Reflection View Buffer
        data = device->MapBuffer(reflectionUniformBuffer);
        if (data) {
            m4x4* matrices = static_cast<m4x4*>(data);
            matrices[0] = projRefl * viewRefl;
            device->UnmapBuffer(reflectionUniformBuffer);
        }
    }

    // Update Main View Camera (Fixed External View)
    {
        void* data = device->MapBuffer(mainViewUniformBuffer);
        if (data) {
            m4x4* matrices = static_cast<m4x4*>(data);
            
            // Perspective (Correct Aspect Ratio)
            float aspect = (float)renderSystem.GetBackBufferDesc().size.x / (float)renderSystem.GetBackBufferDesc().size.y;
            if (aspect < 0.1f) aspect = 1280.0f / 720.0f; // Fallback
            
            // Use narrower FOV (45 degrees) to reduce distortion and make the box look more natural
            m4x4 proj = CreatePerspective(math::constants::PI / 4.0f, aspect, 0.1f, 100.0f);
            
            // View: Look from outside (0, 0, 18) towards (0, 0, 0)
            // Moved back to accommodate narrower FOV
            v3 eye = primal::math::v3{0, 0, 18.0f}; 
            v3 center = primal::math::v3{0, 0, 0};
            v3 up = primal::math::v3{0, 1, 0};
            
            m4x4 view = CreateLookAt(eye, center, up);
            
            // Set into index 0 (as Main View shader uses viewProjections[0])
            matrices[0] = proj * view;
            
            static int logCounter = 0;
            if (logCounter++ < 5) {
                std::cout << "DEBUG: Main View Matrix Update:" << std::endl;
                std::cout << "Aspect: " << aspect << std::endl;
                std::cout << "Proj[0][0]: " << proj.columns[0][0] << ", Proj[1][1]: " << proj.columns[1][1] << ", Proj[2][2]: " << proj.columns[2][2] << std::endl;
                std::cout << "View Pos: " << eye.z << std::endl;
                std::cout << "Final Matrix[0][0]: " << matrices[0].columns[0][0] << std::endl;
                std::cout << "Final Matrix[3][3]: " << matrices[0].columns[3][3] << std::endl;
            }
            
            device->UnmapBuffer(mainViewUniformBuffer);
        }
    }

    if (!renderSystem.BeginFrame(backBuffer, fence)) return;
    
    // Reset Graph
    renderGraph->Clear();
    
    // Import BackBuffer to Graph
    TextureDesc backBufferDesc = renderSystem.GetBackBufferDesc();
    
    // DEBUG: Force Correct Size if suspect
    if (backBufferDesc.size.x == 0 || backBufferDesc.size.y == 0) {
        std::cout << "CRITICAL WARNING: backBufferDesc size is 0! Forcing to window info." << std::endl;
        backBufferDesc.size.x = 2560; // Assume Retina
        backBufferDesc.size.y = 1440;
    }
    std::cout << "DEBUG: BackBuffer Size: " << backBufferDesc.size.x << "x" << backBufferDesc.size.y << std::endl;
    std::cout << "DEBUG: BackBuffer Format: " << (int)backBufferDesc.format << std::endl;

    RGResourceHandle rgBackBuffer = renderGraph->ImportTexture("BackBuffer", backBuffer, backBufferDesc);
    renderGraph->MarkAsOutput(rgBackBuffer);
    
    // Pass 0: Reflection Pass
    struct ReflectionPassData {
        RGResourceHandle output;
        RGResourceHandle depth;
    };
    
    // Import Reflection Resources
    TextureDesc reflDesc = renderSystem.GetBackBufferDesc();
    if (reflDesc.size.x == 0) reflDesc.size = {1280, 720, 1};
    reflDesc.size.x /= 2;
    reflDesc.size.y /= 2;
    reflDesc.format = DataFormat::BGRA8_UNorm;
    reflDesc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
    
    RGResourceHandle rgReflectionTex = renderGraph->ImportTexture("ReflectionTex", reflectionTexture, reflDesc);
    
    TextureDesc reflDepthDesc = reflDesc;
    reflDepthDesc.format = DataFormat::D32_Float;
    reflDepthDesc.usage = TextureUsage::DepthStencil;
    RGResourceHandle rgReflectionDepth = renderGraph->ImportTexture("ReflectionDepth", reflectionDepthTexture, reflDepthDesc);

    auto& reflData = renderGraph->AddPass<ReflectionPassData>("ReflectionPass", RGPassType::Graphics, RGPassCategory::Main,
        [&](ReflectionPassData& data, RenderGraphBuilder& builder) {
            data.output = builder.Write(rgReflectionTex);
            data.depth = builder.Write(rgReflectionDepth);
            
            RGRenderPassDesc rpDesc;
            
            RGAttachmentDesc colorAtt;
            colorAtt.texture = data.output;
            colorAtt.loadOp = LoadAction::Clear;
            colorAtt.storeOp = StoreAction::Store;
            colorAtt.clearColor = ClearValue(0.1f, 0.1f, 0.1f, 1.0f);
            rpDesc.colors.push_back(colorAtt);
            
            RGAttachmentDesc depthAtt;
            depthAtt.texture = data.depth;
            depthAtt.loadOp = LoadAction::Clear;
            depthAtt.storeOp = StoreAction::Store;
            depthAtt.clearDepth = 1.0f;
            rpDesc.depthStencil = depthAtt;
            
            builder.DeclareRenderPass(rpDesc);
        },
        [&](const ReflectionPassData&, RenderGraphContext& context) {
            RHICommandBuffer* cmd = context.cmdBuffer;
            
            // Check desc again or capture
            uint32_t w = renderSystem.GetBackBufferDesc().size.x / 2;
            uint32_t h = renderSystem.GetBackBufferDesc().size.y / 2;
            
            rhi::ViewportDesc viewport;
            viewport.topLeft = {0, 0};
            viewport.size = {(float)w, (float)h};
            viewport.minDepth = 0.0f; viewport.maxDepth = 1.0f;
            cmd->SetViewport(viewport);
            
            rhi::Rect scissor;
            scissor.offset = {0, 0};
            scissor.extent = {w, h};
            cmd->SetScissor(scissor);
            
            cmd->BindGraphicsPipeline(reflectionPipeline);
            
            const DescriptorSetHandle sets[] = { reflectionDescriptorSet };
            cmd->BindDescriptorSets(PipelineBindPoint::Graphics, reflectionPipelineLayout, 0, 1, sets, 0, nullptr);
            
            uint64_t offsets[] = {0};
            cmd->BindVertexBuffers(0, 1, &vertexBuffer, offsets);
            cmd->BindIndexBuffer(indexBuffer, DataFormat::R32_UInt, 0);
            
            for (auto& [name, range] : drawRanges) {
                if (name == "MirrorBox") continue; // Don't render mirror in reflection
                cmd->DrawIndexed(range.count, range.start, 0, 1, 0);
            }
        }
    );
    
    struct MultiViewPassData {
        RGResourceHandle cubeMap;
    };
    
    // Pass 1: Multi-View Rendering (Render to CubeMap)
    auto& mvData = renderGraph->AddPass<MultiViewPassData>("MultiViewPass", RGPassType::Graphics, RGPassCategory::Main,
        [&](MultiViewPassData& data, RenderGraphBuilder& builder) {
            // Create CubeMap (Size 512x512, 6 Faces)
            TextureDesc desc;
            desc.size = {512, 512, 1};
            desc.arraySize = 1; // For TextureCube, arraySize 1 means 1 Cube (6 faces)
            desc.mipLevels = 1;
            desc.format = DataFormat::BGRA8_UNorm;
            desc.type = TextureType::TextureCube;
            desc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
            
            data.cubeMap = builder.CreateTexture("SceneCubeMap", desc);

            // Import Depth Texture
            TextureDesc depthDesc{};
            depthDesc.size = {512, 512, 1};
            depthDesc.mipLevels = 1;
            depthDesc.arraySize = 6;
            depthDesc.format = DataFormat::D32_Float;
            depthDesc.type = TextureType::Texture2DArray;
            depthDesc.usage = TextureUsage::DepthStencil;
            RGResourceHandle rgDepth = renderGraph->ImportTexture("MultiViewDepth", multiViewDepthTexture, depthDesc);
            
            // Declare Render Pass
            RGRenderPassDesc rpDesc;
            
            // Attachment 0: Color
            RGAttachmentDesc colorAtt;
            colorAtt.texture = data.cubeMap;
            colorAtt.loadOp = LoadAction::Clear;
            colorAtt.storeOp = StoreAction::Store;
            colorAtt.clearColor = ClearValue(0.1f, 0.2f, 0.3f, 1.0f);
            colorAtt.slice = 0; // Base slice
            rpDesc.colors.push_back(colorAtt);
            
            // Depth Stencil Attachment
            RGAttachmentDesc depthAtt;
            depthAtt.texture = rgDepth;
            depthAtt.loadOp = LoadAction::Clear;
            depthAtt.storeOp = StoreAction::Store;
            depthAtt.clearDepth = 1.0f;
            depthAtt.slice = 0;
            rpDesc.depthStencil = depthAtt;
            
            rpDesc.renderTargetArrayLength = 6; // Enable Layered Rendering for 6 faces
            
            builder.DeclareRenderPass(rpDesc);
        },
        [&](const MultiViewPassData& /*data*/, RenderGraphContext& context) {
            RHICommandBuffer* cmd = context.cmdBuffer;
            
            // Set Viewport & Scissor for CubeMap (512x512)
            rhi::ViewportDesc viewport;
            viewport.topLeft = primal::math::v2{0, 0};
            viewport.size = primal::math::v2{512, 512};
            viewport.minDepth = 0.0f;
            viewport.maxDepth = 1.0f;
            cmd->SetViewport(viewport);

            rhi::Rect scissor;
            scissor.offset = primal::math::s32v2{0, 0};
            scissor.extent = primal::math::u32v2{512, 512};
            cmd->SetScissor(scissor);
            
            // Bind Pipeline
            cmd->BindGraphicsPipeline(pipeline);
            
            // Bind Descriptor Sets
            const DescriptorSetHandle sets[] = { descriptorSet };
            cmd->BindDescriptorSets(PipelineBindPoint::Graphics, pipelineLayout, 0, 1, sets, 0, nullptr);
            
            // Bind Vertex Buffers & Draw
            uint64_t offsets[] = {0};
            cmd->BindVertexBuffers(0, 1, &vertexBuffer, offsets);
            cmd->BindIndexBuffer(indexBuffer, DataFormat::R32_UInt, 0);
            
            // Draw 6 instances for 6 faces (Using InstanceID for Layer Selection)
            // DrawIndexed(indexCount, startIndex, baseVertex, instanceCount, startInstance)
            cmd->DrawIndexed(indexCount, 0, 0, 6, 0);
        }
    );
    
    // Pass 2: Main View (Fixed External Camera) + Debug Overlay
    struct PresentPassData {
        RGResourceHandle input;
        RGResourceHandle output;
    };
    
    renderGraph->AddPass<PresentPassData>("PresentPass", RGPassType::Graphics, RGPassCategory::Present,
        [&](PresentPassData& data, RenderGraphBuilder& builder) {
            data.input = builder.Read(mvData.cubeMap, ResourceState::ShaderResource); // Read CubeMap as Texture
            
            // Add Reflection Dependency
            builder.Read(reflData.output, ResourceState::ShaderResource);
            
            data.output = builder.Write(rgBackBuffer); // Write to BackBuffer
            
            // Import Main Depth Texture
            TextureDesc mainDepthDesc{};
            mainDepthDesc.size = {backBufferDesc.size.x, backBufferDesc.size.y, 1};
            mainDepthDesc.mipLevels = 1;
            mainDepthDesc.arraySize = 1;
            mainDepthDesc.format = DataFormat::D32_Float;
            mainDepthDesc.type = TextureType::Texture2D;
            mainDepthDesc.usage = TextureUsage::DepthStencil;
            RGResourceHandle rgMainDepth = renderGraph->ImportTexture("MainDepth", mainDepthTexture, mainDepthDesc);

            // Declare Render Pass
            RGRenderPassDesc rpDesc;
            
            RGAttachmentDesc colorAtt;
            colorAtt.texture = data.output;
            colorAtt.loadOp = LoadAction::Clear;
            colorAtt.storeOp = StoreAction::Store;
            colorAtt.clearColor = ClearValue(0.1f, 0.1f, 0.1f, 1.0f); // Dark Gray Clear Color
            rpDesc.colors.push_back(colorAtt);
            
            // Main Depth Attachment
            RGAttachmentDesc depthAtt;
            depthAtt.texture = rgMainDepth;
            depthAtt.loadOp = LoadAction::Clear;
            depthAtt.storeOp = StoreAction::DontCare; // Don't need to store depth
            depthAtt.clearDepth = 1.0f;
            rpDesc.depthStencil = depthAtt;
            
            builder.DeclareRenderPass(rpDesc);
        },
        [&](const PresentPassData& data, RenderGraphContext& context) {
            RHICommandBuffer* cmd = context.cmdBuffer;
            
            // 0. Set Viewport & Scissor
            rhi::ViewportDesc viewport;
            viewport.topLeft = primal::math::v2{0, 0};
            viewport.size = primal::math::v2{(float)backBufferDesc.size.x, (float)backBufferDesc.size.y};
            viewport.minDepth = 0.0f;
            viewport.maxDepth = 1.0f;
            cmd->SetViewport(viewport);

            rhi::Rect scissor;
            scissor.offset = primal::math::s32v2{0, 0};
            scissor.extent = primal::math::u32v2{backBufferDesc.size.x, backBufferDesc.size.y};
            cmd->SetScissor(scissor);
            
            // --- Part A: Render Scene Geometry ---
            
            uint64_t offsets[] = {0};
            cmd->BindVertexBuffers(0, 1, &vertexBuffer, offsets);
            cmd->BindIndexBuffer(indexBuffer, DataFormat::R32_UInt, 0);
            
            // Iterate Ranges
            for (auto& [name, range] : drawRanges) {
                if (name == "MirrorBox") {
                    cmd->BindGraphicsPipeline(mirrorPipeline);
                    const DescriptorSetHandle mirrorSets[] = { mirrorDescriptorSet };
                    cmd->BindDescriptorSets(PipelineBindPoint::Graphics, mirrorPipelineLayout, 0, 1, mirrorSets, 0, nullptr);
                } else {
                    cmd->BindGraphicsPipeline(mainPipeline);
                    const DescriptorSetHandle mainSets[] = { mainDescriptorSet };
                    cmd->BindDescriptorSets(PipelineBindPoint::Graphics, pipelineLayout, 0, 1, mainSets, 0, nullptr);
                }
                cmd->DrawIndexed(range.count, range.start, 0, 1, 0);
            }
            
            // --- Part B: Render Debug Overlay (CubeMap Faces) ---
            if (showDebugOverlay) {
                // 1. Get CubeMap Texture Handle
                RenderGraphResource* res = context.graph->GetResource(data.input);
                if (!res) return;
                ResourceHandle cubeMapHandle = res->GetPhysicalHandle();
                
                // 2. Update Blit Descriptor Set (Per Frame)
                DescriptorImageInfo imageInfo{};
                imageInfo.imageView = cubeMapHandle;
                imageInfo.sampler = handles::INVALID_SAMPLER;
                imageInfo.imageLayout = ResourceState::ShaderResource;

                uint32_t frameIndex = renderSystem.GetCurrentFrameIndex();
                DescriptorSetHandle currentSet = blitDescriptorSets[frameIndex];
                
                // Update Uniform Buffer (Rotation) - Still used for visual effect if needed, but not for main view
                {
                    static float angle = 0.0f;
                    angle += 0.005f; // Slow rotation
                    m4x4 rot = rhi::math::CreateRotationMatrixY(angle);
                    
                    void* data = device->MapBuffer(blitUniformBuffer);
                    if (data) {
                        memcpy(data, &rot, sizeof(m4x4));
                        device->UnmapBuffer(blitUniformBuffer);
                    }
                }
                
                WriteDescriptorSet updateDesc{};
                updateDesc.dstSet = currentSet;
                updateDesc.dstBinding = 0;
                updateDesc.dstArrayElement = 0;
                updateDesc.descriptorCount = 1;
                updateDesc.descriptorType = DescriptorType::SampledImage;
                updateDesc.imageInfo = &imageInfo;
                
                device->UpdateDescriptorSets(1, &updateDesc);
                
                // 3. Bind Debug Pipeline (No Depth Test)
                cmd->BindGraphicsPipeline(debugPipeline);
                
                const DescriptorSetHandle sets[] = { currentSet };
                cmd->BindDescriptorSets(PipelineBindPoint::Graphics, blitPipelineLayout, 0, 1, sets, 0, nullptr);
                
                // 4. Draw Debug Overlay (6 faces, 6 vertices each)
                cmd->Draw(6, 0, 6, 0); // 6 vertices, 6 instances
            }
        }
    );
    
    // Compile & Execute
    renderGraph->Compile();
    
    // Synchronous Execution (Match TestCSMIntegration logic)
    CommandBufferHandle cmdHandle = device->CreateCommandBuffer(CommandQueueType::Graphics);
    if (cmdHandle != handles::INVALID_COMMAND_BUFFER) {
        RHICommandBuffer* cmd = rhi::GetCommandBuffer(cmdHandle);
        
        if (cmd && cmd->Begin()) {
            // Wait for Swapchain Image Available
            if (fence != handles::INVALID_SYNC) {
                cmd->AddWaitSemaphore(fence, 0);
            }
            
            renderGraph->Execute(cmd);
            cmd->End();
            
            // Submit & Wait
            cmd->Submit();
            cmd->WaitForCompletion();
            
            device->DestroyCommandBuffer(cmdHandle);
        }
    }
    
    renderSystem.EndFrame();
}

void MultiViewTestCase::Shutdown() {
    // Unbind Input
    primal::input::unbind(std::hash<std::string>()("debug_toggle"));

    // 1. Destroy scene resources
    if (cubeMesh) {
        cubeMesh->Destroy(device);
        delete cubeMesh;
        cubeMesh = nullptr;
    }
    
    if (device) {
        // 2. Wait for GPU to finish
        device->WaitIdle();

        // 3. Destroy Command Buffer
        // commandBuffers.clear();

        // 4. Destroy RenderGraph (releases its internal resources)
        renderGraph.reset();
        
        // 5. Shutdown RenderSystem
        renderSystem.Shutdown();
        
        // 6. Destroy Manually Created Resources
        device->DestroyBuffer(viewUniformBuffer);
        device->DestroyBuffer(mainViewUniformBuffer); // New
        device->DestroyBuffer(instanceUniformBuffer);
        device->DestroyBuffer(vertexBuffer);
        device->DestroyBuffer(indexBuffer);
        
        device->DestroyTexture(multiViewDepthTexture); // New
        device->DestroyTexture(mainDepthTexture); // New
        
        device->DestroyDescriptorSetLayout(dsLayout);
        device->DestroyDescriptorSet(descriptorSet);
        device->DestroyDescriptorSet(mainDescriptorSet); // New
        device->DestroyPipelineLayout(pipelineLayout);
        device->DestroyPipeline(pipeline);
        device->DestroyPipeline(mainPipeline); // New
        device->DestroyShader(vertexShader);
        device->DestroyShader(pixelShader);

        // 7. Destroy Blit Resources
        device->DestroyPipeline(blitPipeline);
        device->DestroyPipelineLayout(blitPipelineLayout);
        for(auto set : blitDescriptorSets) {
            device->DestroyDescriptorSet(set);
        }
        blitDescriptorSets.clear();
        device->DestroyDescriptorSetLayout(blitDSLayout);
        device->DestroyBuffer(blitUniformBuffer);
        device->DestroyShader(blitVertexShader);
        device->DestroyShader(blitPixelShader);

        // Destroy Reflection Resources
        device->DestroyTexture(reflectionTexture);
        device->DestroyTexture(reflectionDepthTexture);
        device->DestroyBuffer(reflectionUniformBuffer);
        device->DestroyBuffer(reflectionPlaneBuffer);
        device->DestroyDescriptorSet(reflectionDescriptorSet);
        device->DestroyDescriptorSet(mirrorDescriptorSet);
        device->DestroyPipeline(reflectionPipeline);
        device->DestroyPipeline(mirrorPipeline);
        device->DestroyPipelineLayout(reflectionPipelineLayout);
        device->DestroyPipelineLayout(mirrorPipelineLayout);
        device->DestroyShader(reflectionVertexShader);
        device->DestroyShader(mirrorVertexShader);
        device->DestroyShader(mirrorPixelShader);

        // Destroy Simple Pipeline Resources
        device->DestroyPipeline(simplePipeline);
        device->DestroyShader(simpleVertexShader);
        device->DestroyShader(simplePixelShader);
    }
    
    // 8. Destroy Device
    device_ownership.reset();
    
    // 9. Close Window
    primal::platform::remove_window(window.get_id());
}

