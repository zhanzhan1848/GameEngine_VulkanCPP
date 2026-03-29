#include "TestSponzaRenderGraph.h"
#include "Engine/Content/ContentToEngine.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalDevice.h"
#include "Engine/Graphics/RenderGraph/RenderGraphBuilder.h"
#include "Engine/Graphics/RenderGraph/RenderGraphDefinitions.h"
#include "Engine/Graphics/Lighting/LightProbeManager.h"
#include "Engine/Input/Input.h"
#include "ShaderCompilation.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"  // third_party/stb submodule

#include <iostream>
#include <fstream>
#include <filesystem>
#include <cmath>
#include <algorithm>
#include "Utilities/IOStream.h"

using namespace primal;
//using namespace primal::graphics;
using namespace primal::graphics::rhi;
using namespace primal::graphics::rendergraph;

#include "Engine/Graphics/RHI/Platforms/Metal/MetalCommandBuffer.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalMath.h"

// Initialize static member
TestSponzaRenderGraph* TestSponzaRenderGraph::instance = nullptr;

#ifdef TEST_SPONZA_RENDERGRAPH
Engine_Test::Engine_Test() 
    : primal::test::RenderTestRunner(std::make_unique<TestSponzaRenderGraph>()) 
{}
#endif

namespace {
    // Shader File Infos
    const shader_file_info gbuffer_vs_info{ "GBuffer.metal", "vertexMain", shader_type::vertex };
    const shader_file_info gbuffer_ps_info{ "GBuffer.metal", "fragmentMain", shader_type::pixel };
    const shader_file_info shadow_vs_info{ "DepthOnly.metal", "shadow_mapping_vs", shader_type::vertex };
    const shader_file_info lighting_vs_info{ "DeferredLighting.metal", "vertexMain", shader_type::vertex };
    const shader_file_info lighting_ps_info{ "DeferredLighting.metal", "fragmentLighting_v3", shader_type::pixel };
    const shader_file_info skybox_vs_info{ "Skybox.metal", "vertexSkybox", shader_type::vertex };
    const shader_file_info skybox_ps_info{ "Skybox.metal", "fragmentSkybox", shader_type::pixel };
    const shader_file_info taa_vs_info{ "TAA.metal", "vertexMain", shader_type::vertex };
    const shader_file_info taa_ps_info{ "TAA.metal", "fragmentMain", shader_type::pixel };
    const shader_file_info debug_vs_info{ "DebugTexture.metal", "vertexDebug", shader_type::vertex };
    const shader_file_info debug_ps_info{ "DebugTexture.metal", "fragmentDebug", shader_type::pixel };
    const shader_file_info fullscreen_triangle_vs_info{ "FullScreenTriangle.metal", "fullscreen_triangle_vs", shader_type::vertex };
    const shader_file_info post_process_ps_info{ "PostProcess.metal", "post_process_ps", shader_type::pixel };
    const shader_file_info eq2cube_cs_info{ "EquirectangularToCube.metal", "CS_EquirectangularToCube", shader_type::compute };

    // Helper for TAA Jitter
    inline float halton_sequence(int index, int base) {
        float f = 1;
        float r = 0;
        while (index > 0) {
            f = f / (float)base;
            r = r + f * (index % base);
            index = index / base;
        }
        return r;
    }

    // Helper struct for Descriptor Updates
    struct DescriptorData {
        uint32_t binding;
        DescriptorType type;
        ResourceHandle resource;
        uint32_t count = 1;
    };

    void UpdateDescriptorSet(RHIDeviceBase* device, DescriptorSetHandle set, const DescriptorData* params, uint32_t count) {
        std::vector<WriteDescriptorSet> writes(count);
        std::vector<DescriptorImageInfo> imageInfos(count);
        std::vector<DescriptorBufferInfo> bufferInfos(count);

        for (uint32_t i = 0; i < count; ++i) {
            writes[i].dstSet = set;
            writes[i].dstBinding = params[i].binding;
            writes[i].descriptorCount = params[i].count;
            writes[i].descriptorType = params[i].type;

            if (params[i].type == DescriptorType::UniformBuffer || 
                params[i].type == DescriptorType::StorageBuffer) {
                bufferInfos[i].buffer = params[i].resource;
                bufferInfos[i].offset = 0;
                bufferInfos[i].range = ~0ull; // Whole size
                writes[i].bufferInfo = &bufferInfos[i];
            } else if (params[i].type == DescriptorType::SampledImage || 
                       params[i].type == DescriptorType::StorageImage ||
                       params[i].type == DescriptorType::CombinedImageSampler ||
                       params[i].type == DescriptorType::Sampler) {
                 if (params[i].type == DescriptorType::Sampler) {
                     imageInfos[i].sampler = static_cast<SamplerHandle>(params[i].resource);
                 } else {
                     imageInfos[i].imageView = params[i].resource;
                     imageInfos[i].imageLayout = ResourceState::ShaderResource;
                 }
                 writes[i].imageInfo = &imageInfos[i];
            }
        }
        device->UpdateDescriptorSets(count, writes.data());
    }

    // Helper to calculate PSNR
    double CalculatePSNR(const std::vector<uint8_t>& img1, const std::vector<uint8_t>& img2, uint32_t width, uint32_t height, uint32_t channels) {
        if (img1.size() != img2.size()) return 0.0;
        
        double mse = 0.0;
        for (size_t i = 0; i < img1.size(); ++i) {
            double diff = static_cast<double>(img1[i]) - static_cast<double>(img2[i]);
            mse += diff * diff;
        }
        mse /= static_cast<double>(img1.size());
        
        if (mse < 1e-10) return 100.0;
        return 10.0 * std::log10(255.0 * 255.0 / mse);
    }
}

// Shader Buffer Structs
struct SceneData {
    primal::math::m4x4 model;
    primal::math::v4 lightPos;
    primal::math::v4 lightColor;
    primal::math::v4 reflectionPlane;
    primal::math::v4 reflectionPlane2;
    primal::math::v4 reflectionPlane3;
    primal::math::m4x4 previousModel;
    primal::math::v2 jitter;
    primal::math::v2 previousJitter;
    primal::math::v2 padding;
    primal::math::v4 viewPos;
    primal::math::m4x4 shadowMatrix0;
    primal::math::m4x4 shadowMatrix1;
};

struct ViewData {
    primal::math::m4x4 viewProjection;
    primal::math::m4x4 invViewProjection;
    primal::math::m4x4 previousViewProjection;
};

struct TAAUniforms {
    primal::math::v2 resolution;
    primal::math::v2 jitter;
    primal::math::v2 previousJitter;
    float feedback;
    float padding;
};

// Pass Data Structs
struct ShadowPassData {
    RGResourceHandle shadowMap;
};

struct MainPassData {
    RGResourceHandle albedo;
    RGResourceHandle normal;
    RGResourceHandle orm;
    RGResourceHandle velocity;
    RGResourceHandle depth;
};

struct LightingPassData {
    RGResourceHandle output;
    RGResourceHandle albedo;
    RGResourceHandle normal;
    RGResourceHandle orm;
    RGResourceHandle depth;
    RGResourceHandle shadowMap0;
    RGResourceHandle shadowMap1;
};

struct CubemapPassData {
    RGResourceHandle output;
    RGResourceHandle depth;
};

struct TAAPassData {
    RGResourceHandle output;
    RGResourceHandle color;
    RGResourceHandle velocity;
    RGResourceHandle history;
};

struct DebugPassData {
    RGResourceHandle output;
    RGResourceHandle input;
    RGResourceHandle normal;
    RGResourceHandle depth;
    RGResourceHandle shadowMap;
};

struct BlitPassData {
    RGResourceHandle output;
    RGResourceHandle input;
};

void TestSponzaRenderGraph::OnF1Pressed() {
    if (instance) {
        instance->debugPassEnabled = !instance->debugPassEnabled;
        std::cout << "Debug Pass Toggled: " << (instance->debugPassEnabled ? "ON" : "OFF") << std::endl;
    }
}

bool TestSponzaRenderGraph::Initialize() {
    instance = this;
    
    // 1. Initialize Device (Metal)
    primal::graphics::rhi::DeviceDesc deviceDesc;
    deviceDesc.platform = primal::graphics::rhi::RHIPlatform::Metal;
    deviceDesc.enableDebug = true;
    auto metalDevice = std::make_unique<primal::graphics::rhi::MetalDevice>(deviceDesc);
    
    if (!metalDevice->Initialize()) {
        std::cerr << "Failed to initialize Metal Device" << std::endl;
        return false;
    }
    
    device = metalDevice.get();    
    // Register Device to Global Manager so Content System can access it
    primal::graphics::rhi::g_deviceManager.RegisterDevice(device);
    std::cout << "Device registered to Global Manager. Count: " << primal::graphics::rhi::g_deviceManager.GetDeviceCount() << std::endl;
    
    device_ownership = std::move(metalDevice);

    // 2. Initialize Window & RenderSystem
    primal::platform::window_init_info winInfo{
        nullptr, nullptr,
        "TestSponzaRenderGraph", 100, 100, 1280, 720
    };
    window = primal::platform::create_window(&winInfo);
    
    renderWidth = window.width();
    renderHeight = window.height();
#ifdef __APPLE__
    renderWidth *= 2;
    renderHeight *= 2;
#endif

    primal::graphics::RenderSystemInitInfo sysInfo;
    sysInfo.device = device;
    sysInfo.window = window.handle();
    sysInfo.width = renderWidth;
    sysInfo.height = renderHeight;
    if (!renderSystem.Initialize(sysInfo)) {
        std::cerr << "Failed to initialize RenderSystem" << std::endl;
        return false;
    }

    // 3. Compile Shaders
    if (!CompileAllShaders()) {
        std::cerr << "Failed to compile shaders" << std::endl;
        return false;
    }

    // 4. Initialize RenderGraph
    renderGraph = std::make_unique<RenderGraph>(*device);

    // Create Fallback Material Layout & Sampler (Before LoadScene)
    {
        std::cout << "Creating Material Descriptor Set Layout (Fallback)..." << std::endl;
        DescriptorSetLayoutBinding bindings[] = {
            { 0, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr }, // Albedo
            { 1, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr }, // Normal
            { 2, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr }, // ORM
            { 3, DescriptorType::Sampler, 1, ShaderStage::Pixel, nullptr }        // Sampler
        };
        DescriptorSetLayoutDesc desc{ .bindings = bindings, .bindingCount = 4 };
        materialSetLayout = device->CreateDescriptorSetLayout(desc);
        ownsMaterialSetLayout = true;
        std::cout << "Material Descriptor Set Layout created: " << (uint64_t)materialSetLayout << std::endl;

        // Create Sampler
        SamplerDesc samplerDesc{
            .minFilter = FilterMode::Linear,
            .magFilter = FilterMode::Linear,
            .mipFilter = FilterMode::Linear,
            .addressU = TextureAddressMode::Wrap,
            .addressV = TextureAddressMode::Wrap,
            .addressW = TextureAddressMode::Wrap,
        };
        defaultSampler = device->CreateSampler(samplerDesc);
    }

    // 5. Load Scene
    if (!LoadScene()) {
        std::cerr << "Failed to load scene" << std::endl;
        return false;
    }

    // 6. Setup IBL
    if (!SetupIBL()) {
        std::cerr << "Failed to setup IBL" << std::endl;
        return false;
    }

    // 7. Setup Pipelines
    if (!SetupPipelines()) {
        std::cerr << "Failed to setup Pipelines" << std::endl;
        return false;
    }

    // 8. Create Uniform Buffers
    if (!CreateUniformBuffers()) {
        std::cerr << "Failed to create Uniform Buffers" << std::endl;
        return false;
    }

    // Create Persistent Resources
    if (!CreatePersistentResources()) {
        std::cerr << "Failed to create Persistent Resources" << std::endl;
        return false;
    }

    // 9. Create Descriptor Sets
    if (!CreateDescriptorSets()) {
        std::cerr << "Failed to create Descriptor Sets" << std::endl;
        return false;
    }

    // 10. Initialize TAA Resources
    // historyTexture is already created in CreateUniformBuffers
    
    // Initialize Camera
    // Position matches original "eye", Rotation 0,0,0 means Forward along +X (matches original "center" {10,5,0})
    m_camera.Initialize({0.0f, 5.0f, 0.0f}, {0.0f, 0.0f, 0.0f});
    m_camera.SetSpeed(10.0f, 0.1f);
    lastFrameTime = std::chrono::steady_clock::now();

    return true;
}

bool TestSponzaRenderGraph::CompileAllShaders() {
    std::cout << "Compiling Shaders..." << std::endl;
    utl::vector<std::wstring> empty_args;
    
    auto Compile = [&](const shader_file_info& info, const std::string& path) -> bool {
        auto blob = compile_shader(info, path.c_str(), empty_args);
        if (!blob) {
            std::cerr << "Failed to compile shader: " << info.file_name << " (" << info.function << ")" << std::endl;
            return false;
        }
        
        // Parse compiled shader blob to get size and data
        // Blob format: [u64 byte_code_size][hash][byte_code]
        u64 byte_code_size = *reinterpret_cast<u64*>(blob.get());
        u8* byte_code_ptr = blob.get() + sizeof(u64) + 16; // 16 is hash length
        
        ShaderStage stage = ShaderStage::Unknown;
        switch (info.type) {
            case ::shader_type::vertex: stage = ShaderStage::Vertex; break;
            case ::shader_type::pixel: stage = ShaderStage::Pixel; break;
            case ::shader_type::compute: stage = ShaderStage::Compute; break;
            case ::shader_type::geometry: stage = ShaderStage::Geometry; break;
            case ::shader_type::hull: stage = ShaderStage::Hull; break;
            case ::shader_type::domain: stage = ShaderStage::Domain; break;
            default: stage = ShaderStage::Unknown; break;
        }

        ShaderHandle handle = device->CreateShader(byte_code_ptr, byte_code_size, stage, info.function);
        if (handle == handles::INVALID_SHADER) {
            std::cerr << "Failed to create shader handle for: " << info.file_name << std::endl;
            return false;
        }
        
        shaderVariantMap[std::string(info.file_name) + ":" + info.function] = handle;
        return true;
    };

    // Paths
    std::string engineShaderPath = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/Engine/Graphics/RHI/Shaders/";
    std::string metalShaderPath = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/Engine/Graphics/Metal/shaders/";
    std::string testShaderPath = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/shaders/";

    if (!Compile(gbuffer_vs_info, testShaderPath)) return false;
    if (!Compile(gbuffer_ps_info, testShaderPath)) return false;
    if (!Compile(shadow_vs_info, testShaderPath)) return false;
    if (!Compile(lighting_vs_info, testShaderPath)) return false;
    if (!Compile(lighting_ps_info, testShaderPath)) return false;
    if (!Compile(skybox_vs_info, testShaderPath)) return false;
    if (!Compile(skybox_ps_info, testShaderPath)) return false;
    if (!Compile(eq2cube_cs_info, engineShaderPath)) return false;
    
    // Optional shaders
    Compile(taa_vs_info, testShaderPath);
    Compile(taa_ps_info, testShaderPath);
    Compile(debug_vs_info, testShaderPath);
    Compile(debug_ps_info, testShaderPath);
    Compile(fullscreen_triangle_vs_info, testShaderPath);
    Compile(post_process_ps_info, testShaderPath);

    std::cout << "All shaders compiled successfully." << std::endl;
    return true;
}

bool TestSponzaRenderGraph::SetupPipelines() {
    std::cout << "Starting SetupPipelines..." << std::endl;
    if (sceneMeshes.empty()) {
        std::cerr << "No meshes loaded, cannot setup pipelines." << std::endl;
        return false;
    }
    std::cout << "Meshes check passed." << std::endl;

    // --- Create Descriptor Set Layouts ---
    
    // Common Set 0: Global Data (View + Scene)
    {
        std::cout << "Creating Global Descriptor Set Layout..." << std::endl;
        DescriptorSetLayoutBinding bindings[] = {
                { 0, DescriptorType::UniformBuffer, 1, ShaderStage::Vertex | ShaderStage::Pixel }, // ViewData
                { 1, DescriptorType::UniformBuffer, 1, ShaderStage::Vertex | ShaderStage::Pixel } // SceneData
        };
        DescriptorSetLayoutDesc desc{
            .bindingCount = 2,
            .bindings = bindings
        };
        globalSetLayout = device->CreateDescriptorSetLayout(desc);
        std::cout << "Global Descriptor Set Layout created." << std::endl;
    }

    // Common Set 1: Material Data
    // Material Set Layout is already created in Initialize() and assigned to materials in LoadScene()
    // We just verify it here or skip.
    std::cout << "Using Material Set Layout: " << (uint64_t)materialSetLayout << std::endl;

    // 1. GBuffer Pipeline
    {
        std::cout << "Creating GBuffer Pipeline Layout..." << std::endl;
        DescriptorSetLayoutHandle setLayouts[] = { globalSetLayout, materialSetLayout };
        PushConstantRange pushConstantRanges[] = {
            { ShaderStage::Vertex, 2, sizeof(primal::math::m4x4) }
        };
        PipelineLayoutDesc plDesc{
            .setLayoutCount = 2,
            .setLayouts = setLayouts,
            .pushConstantRangeCount = 1,
            .pushConstantRanges = pushConstantRanges
        };

        gbufferLayout = device->CreatePipelineLayout(plDesc);
        std::cout << "GBuffer Pipeline Layout created." << std::endl;

        // Use Vertex Pulling, so no attributes/bindings
        utl::vector<VertexInputAttribute> inputAttributes;
        utl::vector<VertexInputBinding> inputBindings;
        
        GraphicsPipelineDesc desc;
        desc.layout = gbufferLayout;
        desc.vertexShader = shaderVariantMap[std::string(gbuffer_vs_info.file_name) + ":" + gbuffer_vs_info.function];
        desc.pixelShader = shaderVariantMap[std::string(gbuffer_ps_info.file_name) + ":" + gbuffer_ps_info.function];
        desc.renderTargetFormats[0] = DataFormat::BGRA8_UNorm;
        desc.renderTargetFormats[1] = DataFormat::RGBA16_Float;
        desc.renderTargetFormats[2] = DataFormat::RGBA8_UNorm;
        desc.renderTargetFormats[3] = DataFormat::RG16_Float;
        desc.renderTargetCount = 4;
        desc.depthStencilFormat = DataFormat::D32_Float;
        desc.enableDepthTest = true;
        desc.enableDepthWrite = true;
        desc.depthFunc = ComparisonFunc::Less; // Standard Z (Near=0, Far=1)
        desc.cullMode = CullMode::None; // Disable culling to debug
        desc.vertexAttributes = inputAttributes;
        desc.vertexBindings = inputBindings;
        
        std::cout << "Creating GBuffer Graphics Pipeline..." << std::endl;
        gbufferPipeline = device->CreateGraphicsPipeline(desc);
        if (gbufferPipeline == handles::INVALID_PIPELINE) return false;
        std::cout << "GBuffer Graphics Pipeline created." << std::endl;
    }

    // 2. Lighting Pipeline
    {
        std::cout << "Creating Lighting Descriptor Set Layout..." << std::endl;
        // Set 0: GBuffer Inputs + IBL + SceneData + ViewData
        DescriptorSetLayoutBinding bindings[] = {
                { 0, DescriptorType::UniformBuffer, 1, ShaderStage::Pixel, nullptr }, // SceneData
                { 1, DescriptorType::UniformBuffer, 1, ShaderStage::Pixel, nullptr }, // ViewData
                
                { 2, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr }, // Albedo
                { 3, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr }, // Normal
                { 4, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr }, // ORM
                { 5, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr }, // Depth
                { 6, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr }, // Shadow0
                
                { 7, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr }, // Shadow1
                { 8, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr }, // Irradiance
                { 9, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr }, // Prefilter
                { 10, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr }, // BRDF LUT
                { 11, DescriptorType::Sampler, 1, ShaderStage::Pixel, nullptr }, // Sampler
                { 12, DescriptorType::Sampler, 1, ShaderStage::Pixel, nullptr }, // BRDF Sampler
        };
        DescriptorSetLayoutDesc set0Desc{
            .bindingCount = 13,
            .bindings = bindings
        };
        lightingSetLayout = device->CreateDescriptorSetLayout(set0Desc);
        std::cout << "Lighting Descriptor Set Layout created." << std::endl;

        std::cout << "Creating Lighting Pipeline Layout..." << std::endl;
        PipelineLayoutDesc plDesc{
            .setLayoutCount = 1,
            .setLayouts = &lightingSetLayout,
        };
        lightingLayout = device->CreatePipelineLayout(plDesc);
        std::cout << "Lighting Pipeline Layout created: " << (uint64_t)lightingLayout << std::endl;

        GraphicsPipelineDesc desc;
        desc.layout = lightingLayout;
        desc.vertexShader = shaderVariantMap[std::string(lighting_vs_info.file_name) + ":" + lighting_vs_info.function];
        desc.pixelShader = shaderVariantMap[std::string(lighting_ps_info.file_name) + ":" + lighting_ps_info.function];
        desc.renderTargetFormats[0] = DataFormat::RGBA16_Float; // HDR
        desc.renderTargetCount = 1;
        desc.depthStencilFormat = DataFormat::D32_Float;
        desc.enableDepthTest = false;
        desc.enableDepthWrite = false;
        desc.depthFunc = ComparisonFunc::LessEqual;
        // Lighting Pass uses Full Screen Triangle via vertex_id, no vertex input
        desc.vertexAttributes.clear();
        desc.vertexBindings.clear();
        
        std::cout << "Creating Lighting Graphics Pipeline..." << std::endl;
        lightingPipeline = device->CreateGraphicsPipeline(desc);
        std::cout << "Lighting Graphics Pipeline created: " << (uint64_t)lightingPipeline << std::endl;
        if (lightingPipeline == handles::INVALID_PIPELINE) return false;
    }

    // 3. Skybox Pipeline
    {
        std::cout << "Creating Skybox Pipeline..." << std::endl;
        // Set 0: ViewData + SceneData + Cubemap + Sampler
        DescriptorSetLayoutBinding bindings[] = {
                { 0, DescriptorType::UniformBuffer, 1, ShaderStage::Vertex, nullptr }, // ViewData
                { 1, DescriptorType::UniformBuffer, 1, ShaderStage::Vertex, nullptr }, // SceneData
                { 2, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr }, // Cubemap
                { 3, DescriptorType::Sampler, 1, ShaderStage::Pixel, nullptr },        // Sampler
        };
        DescriptorSetLayoutDesc set0Desc{
            .bindingCount = 4,
            .bindings = bindings
        };
        std::cout << "Creating Skybox Descriptor Set Layout..." << std::endl;
        skyboxSetLayout = device->CreateDescriptorSetLayout(set0Desc);
        std::cout << "Skybox Descriptor Set Layout created: " << (uint64_t)skyboxSetLayout << std::endl;

        std::cout << "Creating Skybox Pipeline Layout..." << std::endl;
        PipelineLayoutDesc plDesc{
            .setLayoutCount = 1,
            .setLayouts = &skyboxSetLayout,
        };
        skyboxLayout = device->CreatePipelineLayout(plDesc);
        std::cout << "Skybox Pipeline Layout created: " << (uint64_t)skyboxLayout << std::endl;

        GraphicsPipelineDesc desc;
        desc.layout = skyboxLayout;
        desc.vertexShader = shaderVariantMap[std::string(skybox_vs_info.file_name) + ":" + skybox_vs_info.function];
        desc.pixelShader = shaderVariantMap[std::string(skybox_ps_info.file_name) + ":" + skybox_ps_info.function];
        desc.renderTargetFormats[0] = DataFormat::RGBA16_Float; // HDR
        desc.renderTargetCount = 1;
        desc.depthStencilFormat = DataFormat::D32_Float;
        desc.enableDepthTest = true;
        desc.enableDepthWrite = false;
        desc.depthFunc = ComparisonFunc::LessEqual; // Use LessEqual for Skybox at Far Plane (1.0)
        desc.cullMode = CullMode::None; // Ensure we see the inside of the cube
        
        // Skybox Shader uses vertex_id to generate full screen triangle/cube.
        // It does not use vertex input attributes.
        // Remove vertex attributes and bindings to avoid conflict with buffer(0) usage for UniformBuffer.
        desc.vertexAttributes.clear();
        desc.vertexBindings.clear();
        
        std::cout << "Creating Skybox Graphics Pipeline..." << std::endl;
        skyboxPipeline = device->CreateGraphicsPipeline(desc);
        std::cout << "Skybox Graphics Pipeline created: " << (uint64_t)skyboxPipeline << std::endl;
        if (skyboxPipeline == handles::INVALID_PIPELINE) return false;
    }

    // 4. Shadow Pipeline
    {
        std::cout << "Creating Shadow Pipeline..." << std::endl;
        DescriptorSetLayoutHandle setLayous[]{
            globalSetLayout, materialSetLayout
        };
        PushConstantRange pushConstantRanges[] = {
            { ShaderStage::Vertex, 2, sizeof(primal::math::m4x4) }
        };
        PipelineLayoutDesc plDesc{
            .setLayoutCount = 2,
            .setLayouts = setLayous,
            .pushConstantRangeCount = 1,
            .pushConstantRanges = pushConstantRanges,
        };
        std::cout << "Creating Shadow Pipeline Layout..." << std::endl;
        shadowLayout = device->CreatePipelineLayout(plDesc);
        std::cout << "Shadow Pipeline Layout created: " << (uint64_t)shadowLayout << std::endl;

        GraphicsPipelineDesc desc;
        desc.layout = shadowLayout;
        desc.vertexShader = shaderVariantMap[std::string(shadow_vs_info.file_name) + ":" + shadow_vs_info.function];
        desc.renderTargetCount = 0;
        desc.depthStencilFormat = DataFormat::D32_Float;
        desc.enableDepthTest = true;
        desc.enableDepthWrite = true;
        desc.depthFunc = ComparisonFunc::Less;
        desc.cullMode = CullMode::None; // Disable culling for shadow to be safe
        
        // Use Vertex Pulling (Manual Fetch) like GBuffer pipeline
        // Clear Vertex Attributes/Bindings to avoid Input Assembler conflict
        desc.vertexAttributes.clear();
        desc.vertexBindings.clear();
        
        std::cout << "Creating Shadow Graphics Pipeline..." << std::endl;
        shadowPipeline = device->CreateGraphicsPipeline(desc);
        std::cout << "Shadow Graphics Pipeline created: " << (uint64_t)shadowPipeline << std::endl;
        if (shadowPipeline == handles::INVALID_PIPELINE) return false;
    }

    // 5. TAA Pipeline
    {
        std::cout << "Creating TAA Pipeline..." << std::endl;
        DescriptorSetLayoutBinding bindings[] = {
            { 0, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr }, // Color
            { 1, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr }, // History
            { 2, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr }, // Velocity
            { 3, DescriptorType::UniformBuffer, 1, ShaderStage::Pixel, nullptr }, // Uniforms
        };
        DescriptorSetLayoutDesc setDesc{
            .bindingCount = 4,
            .bindings = bindings,
        };
        taaSetLayout = device->CreateDescriptorSetLayout(setDesc);
        std::cout << "TAA Descriptor Set Layout created: " << (uint64_t)taaSetLayout << std::endl;

        PipelineLayoutDesc plDesc{
            .setLayoutCount = 1,
            .setLayouts = &taaSetLayout,
        };
        taaLayout = device->CreatePipelineLayout(plDesc);
        std::cout << "TAA Pipeline Layout created: " << (uint64_t)taaLayout << std::endl;

        GraphicsPipelineDesc desc;
        desc.layout = taaLayout;
        desc.vertexShader = shaderVariantMap[std::string(taa_vs_info.file_name) + ":" + taa_vs_info.function];
        desc.pixelShader = shaderVariantMap[std::string(taa_ps_info.file_name) + ":" + taa_ps_info.function];
        desc.renderTargetFormats[0] = DataFormat::RGBA16_Float; // HDR
        desc.renderTargetCount = 1;
        desc.enableDepthTest = false;
        desc.enableDepthWrite = false;
        desc.cullMode = CullMode::None; // Ensure FullScreen Triangle is not culled
        
        // Vertex Input State: Empty (Vertex Pulling / FullScreen Triangle)
        desc.vertexAttributes.clear();
        desc.vertexBindings.clear();

        std::cout << "Creating TAA Graphics Pipeline..." << std::endl;
        taaPipeline = device->CreateGraphicsPipeline(desc);
        std::cout << "TAA Graphics Pipeline created: " << (uint64_t)taaPipeline << std::endl;
        if (taaPipeline == handles::INVALID_PIPELINE) return false;
    }

    // 6. Debug Pipeline
    {
        std::cout << "Creating Debug Pipeline..." << std::endl;
        DescriptorSetLayoutBinding bindings[] = {
             { 0, DescriptorType::UniformBuffer, 1, ShaderStage::Pixel, nullptr }, // ViewData
             { 1, DescriptorType::UniformBuffer, 1, ShaderStage::Pixel, nullptr }, // SceneData
             { 2, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr }, // Lighting Output
             { 3, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr }, // Normal
             { 4, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr }, // Depth
             { 5, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr }, // ShadowMap
        };
        DescriptorSetLayoutDesc setDesc{
            .bindingCount = 6,
            .bindings = bindings
        };
        debugSetLayout = device->CreateDescriptorSetLayout(setDesc);
        std::cout << "Debug Descriptor Set Layout created: " << (uint64_t)debugSetLayout << std::endl;

        DescriptorSetLayoutHandle setLayous[]{
            debugSetLayout
        };
        PipelineLayoutDesc plDesc{
            .setLayouts = setLayous,
            .setLayoutCount = 1,
        };
        debugLayout = device->CreatePipelineLayout(plDesc);
        std::cout << "Debug Pipeline Layout created: " << (uint64_t)debugLayout << std::endl;

        GraphicsPipelineDesc desc;
        desc.layout = debugLayout;
        desc.vertexShader = shaderVariantMap[std::string(debug_vs_info.file_name) + ":" + debug_vs_info.function];
        desc.pixelShader = shaderVariantMap[std::string(debug_ps_info.file_name) + ":" + debug_ps_info.function];
        desc.renderTargetFormats[0] = DataFormat::BGRA8_UNorm;
        desc.renderTargetCount = 1;
        desc.enableDepthTest = false;
        desc.enableDepthWrite = false;
        desc.cullMode = CullMode::None; // Disable culling for full screen triangle
        
        std::cout << "Creating Debug Graphics Pipeline..." << std::endl;
        debugPipeline = device->CreateGraphicsPipeline(desc);
        std::cout << "Debug Graphics Pipeline created: " << (uint64_t)debugPipeline << std::endl;
        if (debugPipeline == handles::INVALID_PIPELINE) return false;
    }

    // 7. PostProcess Pipeline
    {
        std::cout << "Creating PostProcess Pipeline..." << std::endl;
        DescriptorSetLayoutBinding bindings[] = {
             { 0, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr }, // Input Texture
             { 1, DescriptorType::Sampler, 1, ShaderStage::Pixel, nullptr },      // Sampler
        };
        DescriptorSetLayoutDesc setDesc{
            .bindingCount = 2,
            .bindings = bindings
        };
        postProcessSetLayout = device->CreateDescriptorSetLayout(setDesc);

        DescriptorSetLayoutHandle setLayous[]{
            postProcessSetLayout
        };
        PipelineLayoutDesc plDesc{
            .setLayouts = setLayous,
            .setLayoutCount = 1,
        };
        postProcessLayout = device->CreatePipelineLayout(plDesc);

        GraphicsPipelineDesc desc;
        desc.layout = postProcessLayout;
        desc.vertexShader = shaderVariantMap[std::string(fullscreen_triangle_vs_info.file_name) + ":" + fullscreen_triangle_vs_info.function];
        desc.pixelShader = shaderVariantMap[std::string(post_process_ps_info.file_name) + ":" + post_process_ps_info.function];
        desc.renderTargetFormats[0] = DataFormat::BGRA8_UNorm;
        desc.renderTargetCount = 1;
        desc.enableDepthTest = false;
        desc.enableDepthWrite = false;
        desc.cullMode = CullMode::None;
        
        postProcessPipeline = device->CreateGraphicsPipeline(desc);
        if (postProcessPipeline == handles::INVALID_PIPELINE) return false;

        DescriptorSetDesc postProcessDesc{
            .layout = postProcessSetLayout,
        };
        
        // Create Descriptor Set
        postProcessDescriptorSet = device->CreateDescriptorSet(postProcessDesc);
    }

    return true;
}

bool TestSponzaRenderGraph::CreateUniformBuffers() {
    BufferDesc desc{
        .size = sizeof(SceneData),
        .usage = GPUMemoryUsage::Dynamic,
        .memoryUsage = GPUMemoryUsage::Dynamic,
    };
    sceneDataBuffer = device->CreateBuffer(desc);

    desc.size = sizeof(ViewData);
    viewDataBuffer = device->CreateBuffer(desc);

    desc.size = sizeof(TAAUniforms);
    taaUniformBuffer = device->CreateBuffer(desc);

    // Create History Texture for TAA
    TextureDesc texDesc{
        .size = { renderWidth, renderHeight, 1 },
        .format = DataFormat::RGBA16_Float,
        .usage = TextureUsage::ShaderResource | TextureUsage::CopyDest | TextureUsage::CopySource, // Read in TAA, Write in Blit, Readback
    };
    historyTexture = device->CreateTexture(texDesc);

    // Create Readback Buffer
    BufferDesc rbDesc{
        .size = (uint64_t)renderWidth * renderHeight * 8, // RGBA16F
        .usage = GPUMemoryUsage::Dynamic,
        .memoryUsage = GPUMemoryUsage::Dynamic,
    };
    readbackBuffer = device->CreateBuffer(rbDesc);

    return (sceneDataBuffer != handles::INVALID_RESOURCE && viewDataBuffer != handles::INVALID_RESOURCE && taaUniformBuffer != handles::INVALID_RESOURCE && historyTexture != handles::INVALID_RESOURCE && readbackBuffer != handles::INVALID_RESOURCE);
}

bool TestSponzaRenderGraph::CreateDescriptorSets() {
    // 1. Global Descriptor Set
    DescriptorSetDesc globalDesc;
    globalDesc.layout = globalSetLayout;
    globalDescriptorSet = device->CreateDescriptorSet(globalDesc);
    if (globalDescriptorSet == handles::INVALID_DESCRIPTOR_SET) return false;
    
    // Fix: ViewData at 0, SceneData at 1 to match Shader
    DescriptorData globalParams[2]{
        { .binding = 0, .type = DescriptorType::UniformBuffer, .resource = viewDataBuffer },
        { .binding = 1, .type = DescriptorType::UniformBuffer, .resource = sceneDataBuffer },
    };
    UpdateDescriptorSet(device, globalDescriptorSet, globalParams, 2);

    // 2. Lighting Descriptor Set
    DescriptorSetDesc lightingDesc;
    lightingDesc.layout = lightingSetLayout;
    lightingDescriptorSet = device->CreateDescriptorSet(lightingDesc);
    // Bind static resources and safe defaults for dynamic ones
    {
        DescriptorData params[11]{
            { .binding = 0, .type = DescriptorType::UniformBuffer, .resource = viewDataBuffer },
            { .binding = 1, .type = DescriptorType::UniformBuffer, .resource = sceneDataBuffer },
            // Initial binds for dynamic resources (will be overwritten in RenderPass)
            { .binding = 2, .type = DescriptorType::SampledImage, .resource = brdfLUT }, // Albedo (Placeholder)
            { .binding = 3, .type = DescriptorType::SampledImage, .resource = brdfLUT }, // Normal (Placeholder)
            { .binding = 4, .type = DescriptorType::SampledImage, .resource = brdfLUT }, // ORM (Placeholder)
            { .binding = 5, .type = DescriptorType::SampledImage, .resource = depthTexture }, // Depth
            { .binding = 6, .type = DescriptorType::SampledImage, .resource = depthTexture }, // Shadow0 (Placeholder)
            { .binding = 7, .type = DescriptorType::SampledImage, .resource = depthTexture }, // Shadow1 (Placeholder)
            
            { .binding = 8, .type = DescriptorType::SampledImage, .resource = irradianceMap },
            { .binding = 9, .type = DescriptorType::SampledImage, .resource = prefilteredMap },
            { .binding = 10, .type = DescriptorType::SampledImage, .resource = brdfLUT },
        };
        
        UpdateDescriptorSet(device, lightingDescriptorSet, params, 11);
        
        // Samplers (Bindings 11, 12)
        SamplerDesc brdfSamplerDesc{
            .minFilter = FilterMode::Linear,
            .magFilter = FilterMode::Linear,
            .addressU = TextureAddressMode::Clamp,
            .addressV = TextureAddressMode::Clamp,
        };
        brdfSampler = device->CreateSampler(brdfSamplerDesc);
        
        DescriptorData samplers[2]{
            { .binding = 11, .type = DescriptorType::Sampler, .resource = defaultSampler },
            { .binding = 12, .type = DescriptorType::Sampler, .resource = brdfSampler },
        };
        UpdateDescriptorSet(device, lightingDescriptorSet, samplers, 2);
    }


    // 3. Skybox Descriptor Set
    DescriptorSetDesc skyboxDesc;
    skyboxDesc.layout = skyboxSetLayout;
    skyboxDescriptorSet = device->CreateDescriptorSet(skyboxDesc);
    {
        DescriptorData params[4]{
            { .binding = 0, .type = DescriptorType::UniformBuffer, .resource = viewDataBuffer },
            { .binding = 1, .type = DescriptorType::UniformBuffer, .resource = sceneDataBuffer },
            { .binding = 2, .type = DescriptorType::SampledImage, .resource = skyboxTexture },
            { .binding = 3, .type = DescriptorType::Sampler, .resource = defaultSampler },
        };
        UpdateDescriptorSet(device, skyboxDescriptorSet, params, 4);
    }

    // 4. TAA Descriptor Set
    DescriptorSetDesc taaDesc;
    taaDesc.layout = taaSetLayout;
    taaDescriptorSet = device->CreateDescriptorSet(taaDesc);
    {
        DescriptorData params[2]{
            { .binding = 1, .type = DescriptorType::SampledImage, .resource = historyTexture },
            { .binding = 3, .type = DescriptorType::UniformBuffer, .resource = taaUniformBuffer },
        };
        UpdateDescriptorSet(device, taaDescriptorSet, params, 2);
    }

    // 5. Debug Descriptor Set
    DescriptorSetDesc debugDesc;
    debugDesc.layout = debugSetLayout;
    debugDescriptorSet = device->CreateDescriptorSet(debugDesc);
    
    return true;
}

void TestSponzaRenderGraph::WriteTexture(primal::graphics::rhi::ResourceHandle texture, const void* data, uint64_t size, uint32_t width, uint32_t height, uint32_t layer) {
        // Create Staging Buffer
        BufferDesc stagingDesc{
            .size = size,
            .usage = GPUMemoryUsage::Dynamic,
            .memoryUsage = GPUMemoryUsage::Dynamic,
        };
        ResourceHandle stagingBuffer = device->CreateBuffer(stagingDesc);
        if (stagingBuffer == handles::INVALID_RESOURCE) return;

        // Map and Copy
        void* mapped = device->MapBuffer(stagingBuffer);
        if (mapped) {
            memcpy(mapped, data, size);
            device->UnmapBuffer(stagingBuffer);
        }

        // Create Fence for sync
        SyncHandle fence = device->CreateSync();

        // Copy Buffer to Texture
        CommandBufferHandle cmdHandle = device->CreateCommandBuffer(CommandQueueType::Graphics);
        MetalDevice* metalDevice = static_cast<MetalDevice*>(device);
        RHICommandBuffer* cmd = metalDevice->GetCommandBuffer(cmdHandle);
        cmd->Begin();
        
        BufferTextureCopyRegion region;
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = layer;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = { 0, 0, 0 };
        region.imageExtent = { width, height, 1 };
        
        cmd->CopyBufferToTexture(stagingBuffer, texture, &region, 1);
        
        cmd->End();
        
        QueueSubmitInfo submitInfo{};
        submitInfo.cmdBuffer = cmdHandle;
        submitInfo.signalFence = fence; // Signal fence when done
        device->Submit(submitInfo);
        
        // Wait for fence
        device->WaitForSync(fence, UINT32_MAX);
        
        // Cleanup
        device->DestroySync(fence);
        device->DestroyCommandBuffer(cmdHandle);
        device->DestroyBuffer(stagingBuffer);
    }

bool TestSponzaRenderGraph::CreatePersistentResources() {
    // Default Material Set
    if (defaultMaterialSet == handles::INVALID_RESOURCE) {
        // Create Debug Sampler
        SamplerDesc samplerDesc{};
        samplerDesc.minFilter = FilterMode::Linear;
        samplerDesc.magFilter = FilterMode::Linear;
        samplerDesc.addressU = TextureAddressMode::Clamp;
        samplerDesc.addressV = TextureAddressMode::Clamp;
        samplerDesc.addressW = TextureAddressMode::Clamp;
        debugSampler = device->CreateSampler(samplerDesc);

        DescriptorSetDesc desc;
        desc.layout = materialSetLayout;
        defaultMaterialSet = device->CreateDescriptorSet(desc);

        // Create 1x1 White Texture for Albedo/ORM
        TextureDesc whiteDesc{
            .size = { 1, 1, 1 },
            .format = DataFormat::RGBA8_UNorm,
            .usage = TextureUsage::ShaderResource | TextureUsage::CopyDest,
        };
        whiteTexture = device->CreateTexture(whiteDesc);
        // DEBUG: Force RED for Default/Missing Texture
        // 0xFF0000FF -> R=255, G=0, B=0, A=255 (Little Endian: FF 00 00 FF)
        uint32_t whiteData = 0xFF0000FF; 
        WriteTexture(whiteTexture, &whiteData, sizeof(uint32_t), 1, 1, 0);
        
        // Create 1x1 Normal Texture (Flat Normal: 0.5, 0.5, 1.0)
        TextureDesc normalDesc = whiteDesc;
        normalTexture = device->CreateTexture(normalDesc);
        uint32_t normalData = 0xFFFF8080; // RGBA: R=128, G=128, B=255, A=255
        WriteTexture(normalTexture, &normalData, sizeof(uint32_t), 1, 1, 0);
        
        // Create Default Sampler
        SamplerDesc samplerDesc_1{
            .minFilter = FilterMode::Linear,
            .magFilter = FilterMode::Linear,
            .addressU = TextureAddressMode::Wrap,
            .addressV = TextureAddressMode::Wrap,
        };
        defaultSampler = device->CreateSampler(samplerDesc_1);
        
        // Update Descriptor Set (Assuming standard Sponza layout: 0:Albedo, 1:Normal, 2:ORM, 3:Sampler)
        DescriptorData params[4] = {
            { .binding = 0, .type = DescriptorType::SampledImage, .resource = whiteTexture },
            { .binding = 1, .type = DescriptorType::SampledImage, .resource = normalTexture },
            { .binding = 2, .type = DescriptorType::SampledImage, .resource = whiteTexture }, // ORM
            { .binding = 3, .type = DescriptorType::Sampler, .resource = defaultSampler }
        };
        UpdateDescriptorSet(device, defaultMaterialSet, params, 4);
    }
    
    // Depth Texture (Shared)
    if (depthTexture == handles::INVALID_RESOURCE) {
        TextureDesc desc{
            .size = { renderWidth, renderHeight, 1 },
            .format = DataFormat::D32_Float,
            .usage = TextureUsage::DepthStencil | TextureUsage::ShaderResource,
        };
        depthTexture = device->CreateTexture(desc);
    }
    
    return true;
}

bool TestSponzaRenderGraph::LoadScene() {
    std::string modelPath = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/assets/Sponza.model";
    std::ifstream file(modelPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cerr << "Failed to open model file: " << modelPath << std::endl;
        return false;
    }
    
    std::streamsize size = file.tellg();
            file.seekg(0, std::ios::beg);
            std::cout << "Model file size: " << size << " bytes." << std::endl;
            std::vector<char> buffer(size);
            if (!file.read(buffer.data(), size)) {
                std::cerr << "Failed to read model file" << std::endl;
                return false;
            }
            std::cout << "Read " << buffer.size() << " bytes into buffer." << std::endl;
    
    // Use SceneDataAdapter to load mesh data via RHI (using RenderItem format)
    std::cout << "Loading scene via SceneDataAdapter (LoadRenderItemData)..." << std::endl;
    primal::graphics::SceneDataAdapter adapter;
    sceneMeshes = adapter.LoadRenderItemData(device, buffer.data(), (uint32_t)buffer.size());
    
    if (sceneMeshes.empty()) {
        std::cerr << "Failed to load scene meshes via SceneDataAdapter." << std::endl;
        return false;
    }
    
    std::cout << "Scene loaded successfully. Mesh count: " << sceneMeshes.size() << std::endl;

    uint32_t meshIdCounter = 1000;
    uint32_t matIdCounter = 2000;
    uint32_t entityIdCounter = 3000;
    std::unordered_map<primal::graphics::MaterialInstance*, primal::id::id_type> materialMap;
    std::string assetBaseDir = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/assets/models/Sponza/";

    for (size_t i = 0; i < sceneMeshes.size(); ++i) {
        auto& meshInfo = sceneMeshes[i];
        
        // Debug Mesh Info
        if (i < 5) {
             std::cout << "DEBUG: Processing Mesh " << i 
                       << " MatInst: " << (meshInfo.materialInstance ? "YES" : "NO")
                       << " DiffPath: '" << meshInfo.diffuseTexturePath << "'" << std::endl;
        }

        // Register Mesh ID (Fake)
        primal::id::id_type meshId = meshIdCounter++;
        // meshInfo.mesh->SetEntityId(meshId); // Cannot set on null mesh

        // Load Textures and Update Material Instance
            if (meshInfo.materialInstance) {
                // Ensure Material has the correct DescriptorSetLayout
                auto material = meshInfo.materialInstance->GetMaterial();
                if (material) {
                    material->SetDescriptorSetLayout(materialSetLayout);
                }

                // Initialize Instance (allocates DescriptorSet)
                meshInfo.materialInstance->Initialize(device);

                bool texturesUpdated = false;

                // Diffuse Texture
                ResourceHandle diffuseTex = whiteTexture;
                /*
                if (!meshInfo.diffuseTexturePath.empty()) {
                    // std::cout << "Processing Diffuse Texture: " << meshInfo.diffuseTexturePath << std::endl;
                    std::string fullPath = assetBaseDir + meshInfo.diffuseTexturePath;
                    // Normalize path separators
                    std::replace(fullPath.begin(), fullPath.end(), '\\', '/');

                    ResourceHandle tex = LoadTextureFromFile(fullPath, false);
                    if (tex != handles::INVALID_RESOURCE) {
                        diffuseTex = tex;
                    }
                }
                */
                // Always set a texture (default or loaded)
                // meshInfo.materialInstance->SetTexture(0, diffuseTex);
                // texturesUpdated = true;

                if (!meshInfo.diffuseTexturePath.empty()) {
                    // std::cout << "Processing Diffuse Texture: " << meshInfo.diffuseTexturePath << std::endl;
                    std::string fullPath = ResolveTexturePath(assetBaseDir, meshInfo.diffuseTexturePath);

                    ResourceHandle tex = LoadTextureFromFile(fullPath, false);
                    if (tex != handles::INVALID_RESOURCE) {
                        diffuseTex = tex;
                    }
                }

                // Normal Texture
                ResourceHandle normalTex = normalTexture;
                
                if (!meshInfo.normalTexturePath.empty()) {
                    std::string fullPath = ResolveTexturePath(assetBaseDir, meshInfo.normalTexturePath);

                    ResourceHandle tex = LoadTextureFromFile(fullPath, true);
                    if (tex != handles::INVALID_RESOURCE) {
                        normalTex = tex;
                    }
                }

                // ORM Texture
                ResourceHandle ormTex = whiteTexture; // Default: AO=1, Rough=0(Smooth), Metal=0
                
                // Try to load ORM from individual or packed paths
                ResourceHandle loadedORM = LoadORMTexture(
                    assetBaseDir,
                    meshInfo.ormTexturePath, 
                    meshInfo.roughnessTexturePath, 
                    meshInfo.metallicTexturePath
                );
                
                if (loadedORM != handles::INVALID_RESOURCE) {
                    ormTex = loadedORM;
                }
                
                // FIX: Update ALL descriptor sets for triple buffering (Frames 0, 1, 2)
                // Since MaterialInstance::Update clears pending updates, we must set and update for each frame.
                for (uint32_t i = 0; i < 3; ++i) {
                    meshInfo.materialInstance->SetCurrentFrame(i);
                    meshInfo.materialInstance->SetTexture(0, diffuseTex);
                    meshInfo.materialInstance->SetTexture(1, normalTex);
                    meshInfo.materialInstance->SetTexture(2, ormTex);
                    if (defaultSampler != handles::INVALID_SAMPLER) {
                        meshInfo.materialInstance->SetSampler(3, defaultSampler);
                    }
                    meshInfo.materialInstance->Update(device);
                }
                // texturesUpdated = false; // Already updated manually
                (void)texturesUpdated;

            }

        // Register Material ID
        primal::id::id_type matId = primal::id::invalid_id;
        if (meshInfo.materialInstance) {
            auto it = materialMap.find(meshInfo.materialInstance.get());
            if (it != materialMap.end()) {
                matId = it->second;
            } else {
                matId = matIdCounter++;
                renderSystem.RegisterMaterialInstance(matId, meshInfo.materialInstance);
                materialMap[meshInfo.materialInstance.get()] = matId;
            }
        }

        // Create RenderProxy
        primal::id::id_type entityId = entityIdCounter++;
        primal::graphics::RenderProxy proxy = primal::graphics::RenderProxy::Create(entityId, meshId, matId);
        
        primal::math::m4x4 transform = primal::graphics::rhi::math::MatrixIdentity();
        proxy.transform = transform;
        scene.AddProxy(proxy);
    }
    
    std::cout << "Scene loaded with " << sceneMeshes.size() << " meshes (Content System)." << std::endl;
    
    return true;
}

primal::graphics::rhi::ResourceHandle TestSponzaRenderGraph::CreateTextureFromData(uint32_t width, uint32_t height, const unsigned char* data, bool isSRGB) {
    // Header: width, height, array_size, flags, mip_levels, format
    uint32_t array_size = 1;
    uint32_t flags = 0;
    uint32_t mip_levels = 1; 
    
    // 28 = R8G8B8A8_UNORM (Linear)
    // 29 = R8G8B8A8_UNORM_SRGB (sRGB)
    uint32_t format = isSRGB ? 29 : 28;

    uint32_t row_pitch = width * 4;
    uint32_t slice_pitch = height * row_pitch;

    size_t blob_size = (6 * sizeof(uint32_t)) + (mip_levels * (2 * sizeof(uint32_t) + slice_pitch));
    
    std::vector<uint8_t> blob(blob_size);
    utl::blob_stream_writer writer(blob.data(), blob.size());
    
    writer.write((uint32_t)width);
    writer.write((uint32_t)height);
    writer.write(array_size);
    writer.write(flags);
    writer.write(mip_levels);
    writer.write(format);

    // Mip 0
    writer.write(row_pitch);
    writer.write(slice_pitch);
    // Write pixel data
    writer.write(data, slice_pitch);

    primal::id::id_type id = primal::content::create_resource(blob.data(), primal::content::asset_type::texture);
    
    if (primal::id::is_valid(id)) {
        ResourceHandle handle = primal::content::get_rhi_texture_handle(id);
        if (handle != handles::INVALID_RESOURCE) {
            return handle;
        } else {
             std::cerr << "Failed to retrieve RHI handle for texture ID: " << id << std::endl;
        }
    } else {
         std::cerr << "Failed to create texture resource via content system." << std::endl;
    }
    return handles::INVALID_RESOURCE;
}

std::string TestSponzaRenderGraph::NormalizePath(const std::string& path) {
    std::string p = path;
    std::replace(p.begin(), p.end(), '\\', '/');
    return p;
}

std::string TestSponzaRenderGraph::ResolveTexturePath(const std::string& assetBaseDir, const std::string& filename) {
    if (filename.empty()) return "";
    
    std::string cleanName = NormalizePath(filename);
    
    // Potential base paths
    std::vector<std::string> basePaths;
    basePaths.push_back(assetBaseDir);
    basePaths.push_back(assetBaseDir + "models/Sponza/");
    basePaths.push_back(assetBaseDir + "fbx_textures/");
    
    for (const auto& base : basePaths) {
        std::string fullPath = base + cleanName;
        // Check if file exists
        std::ifstream f(fullPath.c_str());
        if (f.good()) {
            return fullPath;
        }
    }
    
    // Return default if not found
    return NormalizePath(assetBaseDir + cleanName);
}

primal::graphics::rhi::ResourceHandle TestSponzaRenderGraph::LoadORMTexture(const std::string& assetBaseDir, const std::string& ormPath, const std::string& roughnessPath, const std::string& metallicPath) {
    // Check Cache (key could be combined paths)
    std::string cacheKey = "ORM|" + ormPath + "|" + roughnessPath + "|" + metallicPath;
    if (textureCache.find(cacheKey) != textureCache.end()) {
        return textureCache[cacheKey];
    }

    // 1. Try Loading ORM directly
    if (!ormPath.empty()) {
        std::string fullPath = ResolveTexturePath(assetBaseDir, ormPath);
        
        // ORM is data, so it should be Linear (not sRGB). 
        // LoadTextureFromFile with isNormalMap=true uses format 28 (UNORM), false uses 29 (SRGB).
        ResourceHandle tex = LoadTextureFromFile(fullPath, true, false); 
        if (tex != handles::INVALID_RESOURCE) {
            textureCache[cacheKey] = tex;
            return tex;
        }
    }

    // 2. Combine Roughness/Metallic
    int width = 0, height = 0;
    
    unsigned char* roughData = nullptr;
    unsigned char* metalData = nullptr;
    
    if (!roughnessPath.empty()) {
        std::string fullPath = ResolveTexturePath(assetBaseDir, roughnessPath);
        int w, h, c;
        roughData = stbi_load(fullPath.c_str(), &w, &h, &c, 1); // Load as 1 channel
        if (roughData) {
            width = w; height = h;
        }
    }
    
    if (!metallicPath.empty()) {
        std::string fullPath = ResolveTexturePath(assetBaseDir, metallicPath);
        int w, h, c;
        metalData = stbi_load(fullPath.c_str(), &w, &h, &c, 1);
        if (metalData) {
            if (width == 0) { width = w; height = h; }
            else if (w != width || h != height) {
                // Size mismatch. Ignore metallic for now.
                std::cerr << "WARNING: Metallic texture size mismatch. Ignoring." << std::endl;
                stbi_image_free(metalData);
                metalData = nullptr;
            }
        }
    }
    
    if (width == 0) {
        // No texture loaded
        return handles::INVALID_RESOURCE;
    }
    
    std::vector<unsigned char> ormBuffer(width * height * 4);
    for (int i = 0; i < width * height; ++i) {
        // ORM packing: R=AO, G=Roughness, B=Metallic
        unsigned char ao = 255;
        unsigned char rough = roughData ? roughData[i] : 255; 
        unsigned char metal = metalData ? metalData[i] : 0;
        
        ormBuffer[i*4 + 0] = ao;
        ormBuffer[i*4 + 1] = rough;
        ormBuffer[i*4 + 2] = metal;
        ormBuffer[i*4 + 3] = 255;
    }
    
    if (roughData) stbi_image_free(roughData);
    if (metalData) stbi_image_free(metalData);
    
    ResourceHandle handle = CreateTextureFromData(width, height, ormBuffer.data(), false); // ORM is Linear (false)
    if (handle != handles::INVALID_RESOURCE) {
        textureCache[cacheKey] = handle;
    }
    return handle;
}

primal::graphics::rhi::ResourceHandle TestSponzaRenderGraph::LoadTextureFromFile(const std::string& path, bool isNormalMap, bool allowFallback) {
    // Check Cache
    if (textureCache.find(path) != textureCache.end()) {
        return textureCache[path];
    }

    std::cout << "Loading Texture (via Content System): " << path << std::endl;

    int width, height, channels;
    unsigned char* data = nullptr;

    // Load using stb_image
    // Force RGBA (4 channels)
    data = stbi_load(path.c_str(), &width, &height, &channels, 4); 
    bool loadedWithStb = (data != nullptr);

    if (!loadedWithStb) {
        if (!allowFallback) {
             return handles::INVALID_RESOURCE;
        }
        std::cerr << "Failed to load texture file: " << path << std::endl;
        // Fallback to 1x1 Magenta to indicate missing texture
        width = 1; height = 1; channels = 4;
        data = (unsigned char*)malloc(4);
        data[0] = 255; data[1] = 0; data[2] = 255; data[3] = 255;
        std::cout << "DEBUG: Forcing MAGENTA (Missing) for: " << path << std::endl;
    } else {
        std::cout << "Loaded texture: " << path << " (" << width << "x" << height << ")" << std::endl;
    }

    // Determine format
    // Diffuse is usually sRGB, Normal is Linear.
    bool isSRGB = !isNormalMap;
    
    ResourceHandle handle = CreateTextureFromData(width, height, data, isSRGB);

    if (loadedWithStb) {
        stbi_image_free(data);
    } else {
        free(data); // Use free() since we used malloc()
    }

    if (handle != handles::INVALID_RESOURCE) {
        textureCache[path] = handle;
        return handle;
    } else {
         return handles::INVALID_RESOURCE;
    }
}

bool TestSponzaRenderGraph::SetupIBL() {
    iblPrecomputer = std::make_unique<primal::graphics::rhi::IBLPrecomputer>(device);
    if (!iblPrecomputer->Initialize()) return false;

    // 1. Create Cubemap Texture
    std::cout << "Creating Cubemap from 6 faces..." << std::endl;
    TextureDesc cubeDesc{
        .size = { 2048, 2048, 1 },
        .arraySize = 1,
        .type = TextureType::TextureCube,
        .format = DataFormat::RGBA8_UNorm,
        .usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess | TextureUsage::CopyDest,
    };
    envCubemap = device->CreateTexture(cubeDesc);

    // 2. Load and Upload 6 Faces
    // Generate debug skybox (different colors per face) since files might be missing
    struct FaceColor { uint8_t r, g, b, a; };
    FaceColor faceColors[] = {
        {200, 200, 200, 255}, // Right (+X) Light Grey
        {150, 150, 150, 255}, // Left (-X) Medium Grey
        {100, 100, 255, 255}, // Up (+Y) Sky Blue
        {50, 50, 50, 255},    // Down (-Y) Dark Grey (Floor)
        {100, 100, 100, 255}, // Front (+Z) Grey
        {100, 100, 100, 255}  // Back (-Z) Grey
    };
    
    for (int i = 0; i < 6; ++i) {
        // Create a 2048x2048 solid color texture
        std::vector<uint8_t> faceData(2048 * 2048 * 4);
        FaceColor color = faceColors[i];
        for (size_t j = 0; j < faceData.size(); j += 4) {
            faceData[j] = color.r;
            faceData[j+1] = color.g;
            faceData[j+2] = color.b;
            faceData[j+3] = color.a;
        }
        WriteTexture(envCubemap, faceData.data(), faceData.size(), 2048, 2048, i);
    }
    
    std::cout << "Finished loading debug skybox faces." << std::endl;

    // 3. Compute IBL Maps
    std::cout << "Computing IBL Maps..." << std::endl;
    irradianceMap = iblPrecomputer->ComputeIrradianceMap(envCubemap);
    prefilteredMap = iblPrecomputer->ComputePrefilteredEnvironmentMap(envCubemap);
    brdfLUT = iblPrecomputer->ComputeBRDFIntegrationMap();
    std::cout << "Finished Computing IBL Maps." << std::endl;
    
    // Assign envCubemap to skyboxTexture for the Skybox Pass
    skyboxTexture = envCubemap;
    
    std::cout << "Finished SetupIBL." << std::endl;

    return true;
}

void TestSponzaRenderGraph::BuildRenderGraph(RenderGraph& graph, ResourceHandle backBuffer) {
    // Import BackBuffer
    TextureDesc backBufferDesc = renderSystem.GetBackBufferDesc();
    RGResourceHandle backBufferHandle = graph.ImportTexture("BackBuffer", backBuffer, backBufferDesc);

    // 1. Shadow Passes (2 Cascades)
    TextureDesc shadowDesc{
        .size = { 2048, 2048, 1 },
        .format = DataFormat::D32_Float,
        .usage = TextureUsage::DepthStencil | TextureUsage::ShaderResource, // Need ShaderResource to sample
    };
    RGResourceHandle shadowMap0 = graph.CreateTexture("ShadowMap0", shadowDesc);
    RGResourceHandle shadowMap1 = graph.CreateTexture("ShadowMap1", shadowDesc);
    
    // Cascade 0: Near Shadow
    graph.AddPass<ShadowPassData>("ShadowPass0", RGPassType::Graphics, RGPassCategory::Depth,
        [&](ShadowPassData& data, RenderGraphBuilder& builder) {
            data.shadowMap = builder.Write(shadowMap0, ResourceState::DepthStencil);

            RGRenderPassDesc rpDesc;
            rpDesc.depthStencil.texture = data.shadowMap;
            rpDesc.depthStencil.depthLoadOp = LoadAction::Clear;
            rpDesc.depthStencil.depthStoreOp = StoreAction::Store;
            rpDesc.depthStencil.clearDepth = 1.0f; // Clear to Far (1.0)
            // Debug: Use LoadAction::Load to see if we are drawing anything on top of old data? No, that's confusing.
            // Let's keep 1.0f clear, but ensure DepthFunc is Less.
            builder.DeclareRenderPass(rpDesc);
        }, 
        [&](const ShadowPassData& data, RenderGraphContext& context) {
            // Reduced log frequency
            if (frameCount == 1) std::cout << "Executing ShadowPass0..." << std::endl;
            auto cmd = context.cmdBuffer;
            cmd->BindGraphicsPipeline(shadowPipeline);
            cmd->SetViewport({ {0, 0 }, { 2048, 2048 }, 0, 1});
            cmd->SetScissor({{ 0, 0 }, { 2048, 2048 }});

            // Bind Global Set (Set 0)
            const DescriptorSetHandle descriptorSets[] = { globalDescriptorSet };
            cmd->BindDescriptorSets(PipelineBindPoint::Graphics, shadowLayout, 0, 1, descriptorSets, 0, nullptr);

            // Apply Metal Z-fix ([-1,1] -> [0,1]) - REMOVED: CreateOrthographicMatrix already handles this
            primal::math::m4x4 mvpMatrix = lightVP0;
            // for (int i = 0; i < 4; ++i) {
            //     mvpMatrix.columns[i][2] = 0.5f * lightVP0.columns[i][2] + 0.5f * lightVP0.columns[i][3];
            // }

            if (frameCount == 1) {
                std::cout << "DEBUG: ShadowPass0 SceneMeshes count: " << sceneMeshes.size() << std::endl;
                std::cout << "DEBUG: ShadowPass0 lightVP0:" << std::endl;
                for(int r=0; r<4; ++r) {
                     std::cout << " Row " << r << ": " << lightVP0.columns[0][r] << " " << lightVP0.columns[1][r] << " " << lightVP0.columns[2][r] << " " << lightVP0.columns[3][r] << std::endl;
                }
                
                // Test transform a point (0,0,0)
                primal::math::v4 testPt = {0.f, 0.f, 0.f, 1.f};
                primal::math::v4 resPt = lightVP0 * testPt;
                std::cout << "DEBUG: Project (0,0,0) -> (" 
                          << resPt.x << ", " << resPt.y << ", " << resPt.z << ", " << resPt.w << ")" << std::endl;
            }

            int drawCount = 0;
            int meshIndex = 0;
            for (const auto& meshInfo : sceneMeshes) {
                if (!meshInfo.mesh) {
                     meshIndex++;
                     continue;
                }

                // Calculate MVP - Model is identity
                primal::math::m4x4 model = primal::graphics::rhi::math::MatrixIdentity();
                primal::math::m4x4 mvp = mvpMatrix * model; 
                
                if (frameCount == 1) {
             // Log first 5 meshes to check validity
             
             if (drawCount < 5) {
                 std::cout << "DEBUG: ShadowPass0 Drawing Mesh Index: " << meshIndex 
                           << " Stride: " << meshInfo.mesh->GetVertexStride() 
                           << ", MVP Row 3: " 
                           << mvp.columns[0][3] << " " << mvp.columns[1][3] << " " << mvp.columns[2][3] << " " << mvp.columns[3][3] << std::endl;
             }
             
        }

                // DepthOnly.metal expects PushConsts at buffer(2)
                cmd->PushConstants(shadowLayout, ShaderStage::Vertex, 2, sizeof(mvp), &mvp);

                // Draw
                meshInfo.mesh->Draw(cmd, 1, 0, 20);
                drawCount++;
                meshIndex++;
            }
            if (frameCount % 60 == 0 || frameCount == 1) {
                std::cout << "!!!! ShadowPass0 Draw Calls: " << drawCount << std::endl;
                std::cout << "Finished ShadowPass0." << std::endl;
            }
        });

    // Cascade 1: Far Shadow
    graph.AddPass<ShadowPassData>("ShadowPass1", RGPassType::Graphics, RGPassCategory::Depth,
        [&](ShadowPassData& data, RenderGraphBuilder& builder) {
            data.shadowMap = builder.Write(shadowMap1, ResourceState::DepthStencil);

            RGRenderPassDesc rpDesc;
            rpDesc.depthStencil.texture = data.shadowMap;
            rpDesc.depthStencil.depthLoadOp = LoadAction::Clear;
            rpDesc.depthStencil.depthStoreOp = StoreAction::Store;
            rpDesc.depthStencil.clearDepth = 1.0f; // Clear to Far (1.0)
            builder.DeclareRenderPass(rpDesc);
        }, 
        [&](const ShadowPassData& data, RenderGraphContext& context) {
            // Reduced log frequency
            if (frameCount == 1) std::cout << "Executing ShadowPass1..." << std::endl;
            auto cmd = context.cmdBuffer;
            cmd->BindGraphicsPipeline(shadowPipeline);
            cmd->SetViewport({ {0, 0 }, { 2048, 2048 }, 0, 1});
            cmd->SetScissor({{ 0, 0 }, { 2048, 2048 }});

            // Bind Global Set (Set 0)
            const DescriptorSetHandle descriptorSets[] = { globalDescriptorSet };
            cmd->BindDescriptorSets(PipelineBindPoint::Graphics, shadowLayout, 0, 1, descriptorSets, 0, nullptr);

            // Apply Metal Z-fix ([-1,1] -> [0,1]) - REMOVED: CreateOrthographicMatrix already handles this
            primal::math::m4x4 mvpMatrix = lightVP1;
            // for (int i = 0; i < 4; ++i) {
            //     mvpMatrix.columns[i][2] = 0.5f * lightVP1.columns[i][2] + 0.5f * lightVP1.columns[i][3];
            // }

            if (frameCount == 1) {
                std::cout << "DEBUG: ShadowPass1 lightVP1:" << std::endl;
                for(int r=0; r<4; ++r) printf("  %.2f %.2f %.2f %.2f\n", lightVP1.columns[0][r], lightVP1.columns[1][r], lightVP1.columns[2][r], lightVP1.columns[3][r]);
            }

            int drawCount = 0;
            for (const auto& meshInfo : sceneMeshes) {
                if (!meshInfo.mesh) continue;

                // Calculate MVP - Model is identity
                primal::math::m4x4 model = primal::graphics::rhi::math::MatrixIdentity();
                primal::math::m4x4 mvp = mvpMatrix * model; 
                // DepthOnly.metal expects PushConsts at buffer(2)
                cmd->PushConstants(shadowLayout, ShaderStage::Vertex, 2, sizeof(mvp), &mvp);

                // Draw
                meshInfo.mesh->Draw(cmd, 1, 0, 20);
                drawCount++;
            }
            if (frameCount == 1) {
                std::cout << "!!!! ShadowPass1 Draw Calls: " << drawCount << std::endl;
                std::cout << "Finished ShadowPass1." << std::endl;
            }
        });

    // 2. Main Pass (GBuffer)
    TextureDesc mainDesc{
        .size = { renderWidth, renderHeight, 1 },
        .format = DataFormat::BGRA8_UNorm, // Albedo
        .usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource,
    };
    RGResourceHandle gbufferAlbedo = graph.CreateTexture("GBufferAlbedo", mainDesc);
    
    mainDesc.format = DataFormat::RGBA16_Float; // Normal
    RGResourceHandle gbufferNormal = graph.CreateTexture("GBufferNormal", mainDesc);
    
    mainDesc.format = DataFormat::BGRA8_UNorm; // ORM
    RGResourceHandle gbufferORM = graph.CreateTexture("GBufferORM", mainDesc);
    
    mainDesc.format = DataFormat::RG16_Float; // Velocity
    RGResourceHandle velocityBuffer = graph.CreateTexture("VelocityBuffer", mainDesc);
    
    mainDesc.format = DataFormat::D32_Float; // Depth
    mainDesc.usage = TextureUsage::DepthStencil | TextureUsage::ShaderResource;
    RGResourceHandle depthBuffer = graph.CreateTexture("DepthBuffer", mainDesc);

    graph.AddPass<MainPassData>("MainPass", RGPassType::Graphics, RGPassCategory::Main,
        [&](MainPassData& data, RenderGraphBuilder& builder) {
            data.albedo = builder.Write(gbufferAlbedo, ResourceState::RenderTarget);
            data.normal = builder.Write(gbufferNormal, ResourceState::RenderTarget);
            data.orm = builder.Write(gbufferORM, ResourceState::RenderTarget);
            data.velocity = builder.Write(velocityBuffer, ResourceState::RenderTarget);
            data.depth = builder.Write(depthBuffer, ResourceState::DepthStencil);
            
            RGRenderPassDesc rpDesc;
            rpDesc.colors.resize(4);
            
            // 0: Albedo
            rpDesc.colors[0].texture = data.albedo;
            rpDesc.colors[0].loadOp = LoadAction::Clear;
            rpDesc.colors[0].storeOp = StoreAction::Store;
            rpDesc.colors[0].clearColor = { .color = primal::math::v4{0.0f, 0.0f, 0.0f, 0.0f} };  
            
            // 1: Normal
            rpDesc.colors[1].texture = data.normal;
            rpDesc.colors[1].loadOp = LoadAction::Clear;
            rpDesc.colors[1].storeOp = StoreAction::Store;
            rpDesc.colors[1].clearColor = { .color = primal::math::v4{0.0f, 0.0f, 0.0f, 0.0f} }; 
            
            // 2: ORM
            rpDesc.colors[2].texture = data.orm;
            rpDesc.colors[2].loadOp = LoadAction::Clear;
            rpDesc.colors[2].storeOp = StoreAction::Store;
            rpDesc.colors[2].clearColor = { .color = primal::math::v4{1.0f, 1.0f, 1.0f, 1.0f} }; // ORM (White)
            
            // 3: Velocity
            rpDesc.colors[3].texture = data.velocity;
            rpDesc.colors[3].loadOp = LoadAction::Clear;
            rpDesc.colors[3].storeOp = StoreAction::Store;
            rpDesc.colors[3].clearColor = { .color = primal::math::v4{0.0f, 0.0f, 0.0f, 0.0f} }; // Velocity

            
            RGAttachmentDesc depthDesc{};
            depthDesc.texture = data.depth;
            depthDesc.depthLoadOp = LoadAction::Clear;
            depthDesc.depthStoreOp = StoreAction::Store;
            depthDesc.clearDepth = 1.0f; // Standard Clear to Far (1.0)
            rpDesc.depthStencil = depthDesc;
            
            builder.DeclareRenderPass(rpDesc);
        }, 
        [&](const MainPassData& data, RenderGraphContext& context) {
            // Reduced log frequency
            if (frameCount == 1) std::cout << "Executing MainPass... SceneMeshes: " << sceneMeshes.size() << std::endl;
            auto cmd = context.cmdBuffer;
            cmd->BindGraphicsPipeline(gbufferPipeline);
            cmd->SetViewport({ { 0, 0 }, { (float)renderWidth, (float)renderHeight }, 0, 1});
            cmd->SetScissor({ { 0, 0 }, { renderWidth, renderHeight } });

            // Bind Global Set (Set 0)
            const DescriptorSetHandle descriptorSets[] = { globalDescriptorSet };
            cmd->BindDescriptorSets(PipelineBindPoint::Graphics, gbufferLayout, 0, 1, descriptorSets, 0, nullptr);

            int drawCount = 0;
            for (const auto& meshInfo : sceneMeshes) {
                if (!meshInfo.mesh) continue;
                
                // Bind Material Set (Set 1)
                DescriptorSetHandle matSet = defaultMaterialSet;
                if (meshInfo.materialInstance) {
                    // Update Current Frame Index
                    meshInfo.materialInstance->SetCurrentFrame(renderSystem.GetCurrentFrameIndex());
                    matSet = meshInfo.materialInstance->GetDescriptorSet();

                    // DEBUG: Check if matSet is valid
                    if (matSet == handles::INVALID_RESOURCE) {
                        // std::cerr << "ERROR: MaterialInstance DescriptorSet is INVALID! Falling back to default." << std::endl;
                        matSet = defaultMaterialSet;
                    }
                }
                const DescriptorSetHandle matSets[] = { matSet };
                cmd->BindDescriptorSets(PipelineBindPoint::Graphics, gbufferLayout, 1, 1, matSets, 0, nullptr);

                // Push Model Matrix
                primal::math::m4x4 model = primal::graphics::rhi::math::MatrixIdentity();
                // Scale down the model to ensure it fits in view if it's huge
                model.columns[0][0] = 1.0f;
                model.columns[1][1] = 1.0f;
                model.columns[2][2] = 1.0f;
                
                cmd->PushConstants(gbufferLayout, ShaderStage::Vertex, 2, sizeof(model), &model);
                
                // Debug: Print Matrices for the first mesh of the first frame
                if (frameCount == 1 && drawCount == 0) {
                    std::cout << "DEBUG: Frame 1, Mesh 0 Matrices:" << std::endl;
                    std::cout << "  Model: Identity" << std::endl;
                }

                // Draw
                meshInfo.mesh->Draw(cmd, 1, 0, 20); 
                // Reduced log
                // if (frameCount % 60 == 0) std::cout << "!!!! Draw Mesh " << drawCount << ": Vertices=" << meshInfo.mesh->GetVertexCount() << std::endl;
                drawCount++;
            }
            if (frameCount == 1) std::cout << "!!!! MainPass Draw Calls: " << drawCount << std::endl;
        });

    // 3. Lighting Output Texture (Created Early for Reordering)
    mainDesc.format = DataFormat::RGBA16_Float; // HDR Output
    mainDesc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
    RGResourceHandle lightingOutput = graph.CreateTexture("LightingOutput", mainDesc);

    // 4. Skybox Pass (Moved before Lighting Pass)
    graph.AddPass<CubemapPassData>("CubemapPass", RGPassType::Graphics, RGPassCategory::Lighting,
        [&](CubemapPassData& data, RenderGraphBuilder& builder) {
            data.output = builder.Write(lightingOutput, ResourceState::RenderTarget);
            
            // Use Write to maintain DepthStencil State for Attachment usage
            // Even if we don't write depth in pipeline, we need it bound as DS Attachment
            builder.Write(depthBuffer, ResourceState::DepthStencil);
            
            RGRenderPassDesc rpDesc;
            RGAttachmentDesc colorDesc{};
            colorDesc.texture = data.output;
            colorDesc.loadOp = LoadAction::Clear; // Clear to Black/Skybox
            colorDesc.storeOp = StoreAction::Store;
            colorDesc.clearColor = { .color = primal::math::v4{0.0f, 0.0f, 0.0f, 0.0f} };
            rpDesc.colors.push_back(colorDesc);
            
            rpDesc.depthStencil.texture = depthBuffer;
            rpDesc.depthStencil.depthLoadOp = LoadAction::Load; // Load MainPass Depth
            rpDesc.depthStencil.depthStoreOp = StoreAction::Store;

            builder.DeclareRenderPass(rpDesc);
        }, 
        [&](const CubemapPassData& data, RenderGraphContext& context) {
            if (frameCount == 1) std::cout << "Executing SkyboxPass..." << std::endl;
            auto cmd = context.cmdBuffer;
            cmd->BindGraphicsPipeline(skyboxPipeline);
            cmd->SetViewport({ {0, 0 }, { (float)renderWidth, (float)renderHeight }, 0, 1});
            cmd->SetScissor({{ 0, 0 }, { renderWidth, renderHeight }});
            
            const DescriptorSetHandle skyboxSets[] = { skyboxDescriptorSet };
            cmd->BindDescriptorSets(PipelineBindPoint::Graphics, skyboxLayout, 0, 1, skyboxSets, 0, nullptr);
            
            // Draw Cube (36 vertices)
            cmd->Draw(36, 0, 1, 0);
        });

    // 3. Lighting Pass (After Skybox)
    graph.AddPass<LightingPassData>("LightingPass", RGPassType::Graphics, RGPassCategory::Lighting,
        [&](LightingPassData& data, RenderGraphBuilder& builder) {
                data.albedo = builder.Read(gbufferAlbedo);
                data.normal = builder.Read(gbufferNormal);
                data.orm = builder.Read(gbufferORM);
                data.depth = builder.Read(depthBuffer); 
                data.shadowMap0 = builder.Read(shadowMap0);
                data.shadowMap1 = builder.Read(shadowMap1);
                
                // Write to same output, but Load previous content (Skybox)
                data.output = builder.Write(lightingOutput, ResourceState::RenderTarget);
                
                RGRenderPassDesc rpDesc;
                RGAttachmentDesc colorDesc{};
                colorDesc.texture = data.output;
                colorDesc.loadOp = LoadAction::Load; // Load Skybox Result
                colorDesc.storeOp = StoreAction::Store;
                rpDesc.colors.push_back(colorDesc);
                builder.DeclareRenderPass(rpDesc);
            }, 
            [&](const LightingPassData& data, RenderGraphContext& context) {
                // std::cout << "Executing LightingPass (Frame " << frameCount << ")..." << std::endl;
                
                // Debug handles once
                if (frameCount == 1) {
                    std::cout << "LightingPass Resources:" << std::endl;
                    std::cout << "  IrradianceMap: " << irradianceMap << std::endl;
                    std::cout << "  PrefilteredMap: " << prefilteredMap << std::endl;
                    std::cout << "  BRDF LUT: " << brdfLUT << std::endl;
                    std::cout << "  DefaultSampler: " << defaultSampler << std::endl;
                    std::cout << "  BRDFSampler: " << brdfSampler << std::endl;
                }

                // Update Descriptor Set with current Frame's GBuffer Resources
                // We update ALL dynamic bindings (textures + samplers) to ensure they are correct
                DescriptorData params[11] = {
                    { .binding = 2, .type = DescriptorType::SampledImage, .resource = graph.GetResource(data.albedo)->GetPhysicalHandle() },
                    { .binding = 3, .type = DescriptorType::SampledImage, .resource = graph.GetResource(data.normal)->GetPhysicalHandle() },
                    { .binding = 4, .type = DescriptorType::SampledImage, .resource = graph.GetResource(data.orm)->GetPhysicalHandle() },
                    { .binding = 5, .type = DescriptorType::SampledImage, .resource = graph.GetResource(data.depth)->GetPhysicalHandle() },
                    { .binding = 6, .type = DescriptorType::SampledImage, .resource = graph.GetResource(data.shadowMap0)->GetPhysicalHandle() },
                    { .binding = 7, .type = DescriptorType::SampledImage, .resource = graph.GetResource(data.shadowMap1)->GetPhysicalHandle() },
                    { .binding = 8, .type = DescriptorType::SampledImage, .resource = irradianceMap },
                    { .binding = 9, .type = DescriptorType::SampledImage, .resource = prefilteredMap },
                    { .binding = 10, .type = DescriptorType::SampledImage, .resource = brdfLUT },
                    { .binding = 11, .type = DescriptorType::Sampler, .resource = defaultSampler },
                    { .binding = 12, .type = DescriptorType::Sampler, .resource = brdfSampler }
                };
                UpdateDescriptorSet(device, lightingDescriptorSet, params, 11);

                auto cmd = context.cmdBuffer;
                cmd->BindGraphicsPipeline(lightingPipeline);
                cmd->SetViewport({ {0, 0 }, { (float)renderWidth, (float)renderHeight }, 0, 1});
                cmd->SetScissor({{ 0, 0 }, { renderWidth, renderHeight }});
                
                // Bind Global Descriptor Set (Set 0) - Contains GBuffer Inputs
                const DescriptorSetHandle descriptorSets[] = { lightingDescriptorSet };
                cmd->BindDescriptorSets(PipelineBindPoint::Graphics, lightingLayout, 0, 1, descriptorSets, 0, nullptr);
                
                cmd->Draw(3, 0, 1, 0);
            });

    // 5. TAA Pass
    // Update desc for TAA Output and History to support Copy operations
    mainDesc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource | TextureUsage::CopySource | TextureUsage::CopyDest;

    RGResourceHandle taaOutput = graph.CreateTexture("TAAOutput", mainDesc);
    
    // Import History Texture
    RGResourceHandle historyHandle = graph.ImportTexture("HistoryTexture", historyTexture, mainDesc);

    graph.AddPass<TAAPassData>("TAAPass", RGPassType::Graphics, RGPassCategory::PostProcess,
        [&](TAAPassData& data, RenderGraphBuilder& builder) {
            data.color = builder.Read(lightingOutput);
            data.velocity = builder.Read(velocityBuffer);
            data.history = builder.Read(historyHandle); // Read previous frame
            data.output = builder.Write(taaOutput, ResourceState::RenderTarget);
            
            RGRenderPassDesc rpDesc;
            RGAttachmentDesc colorDesc{};
            colorDesc.texture = data.output;
            colorDesc.loadOp = LoadAction::Clear;
            colorDesc.storeOp = StoreAction::Store;
            colorDesc.clearColor = { .color = primal::math::v4{0,0,0,0} };
            rpDesc.colors.push_back(colorDesc);
            builder.DeclareRenderPass(rpDesc);
        }, 
        [&](const TAAPassData& data, RenderGraphContext& context) {
            auto cmd = context.cmdBuffer;
            cmd->BindGraphicsPipeline(taaPipeline);
            cmd->SetViewport({ {0, 0 }, { (float)renderWidth, (float)renderHeight }, 0, 1});
            cmd->SetScissor({{ 0, 0 }, { renderWidth, renderHeight }});

            // Update Descriptor Set
            DescriptorData params[4]{
                {.binding = 0, .type = DescriptorType::SampledImage, .resource = graph.GetResource(data.color)->GetPhysicalHandle()},
                {.binding = 1, .type = DescriptorType::SampledImage, .resource = graph.GetResource(data.history)->GetPhysicalHandle()},
                {.binding = 2, .type = DescriptorType::SampledImage, .resource = graph.GetResource(data.velocity)->GetPhysicalHandle()},
                {.binding = 3, .type = DescriptorType::UniformBuffer, .resource = taaUniformBuffer},
            };
            UpdateDescriptorSet(device, taaDescriptorSet, params, 4);
            
            const DescriptorSetHandle taaSets[] = { taaDescriptorSet };
            cmd->BindDescriptorSets(PipelineBindPoint::Graphics, taaLayout, 0, 1, taaSets, 0, nullptr);
            
            // Draw 3 Vertices (FullScreen Triangle)
            cmd->Draw(3, 0, 1, 0); 
            
            if (frameCount == 1) std::cout << "Executed TAAPass Draw(3)" << std::endl;
        });
        
    // Copy TAA Output to History for next frame
    graph.AddPass<BlitPassData>("HistoryCopy", RGPassType::Copy, RGPassCategory::Copy,
        [&](BlitPassData& data, RenderGraphBuilder& builder) {
            data.input = builder.Read(taaOutput, ResourceState::CopySource);
            data.output = builder.Write(historyHandle, ResourceState::CopyDest); 
        },
        [&](const BlitPassData& data, RenderGraphContext& context) {
             // Reduced log frequency
             if (frameCount == 1) std::cout << "Executing HistoryCopy..." << std::endl;
             auto cmd = context.cmdBuffer;
             auto srcRes = graph.GetResource(data.input);
             auto dstRes = graph.GetResource(data.output);
             
             if (frameCount == 1) std::cout << "HistoryCopy: srcRes=" << srcRes << ", dstRes=" << dstRes << std::endl;
             
             if (!srcRes || !dstRes) {
                 std::cerr << "HistoryCopy: Invalid resources!" << std::endl;
                 return;
             }

             auto src = srcRes->GetPhysicalHandle();
             auto dst = dstRes->GetPhysicalHandle();
             
             if (frameCount == 1) std::cout << "HistoryCopy: srcHandle=" << src << ", dstHandle=" << dst << std::endl;
             
             TextureBlitRegion region;
             region.srcSubresource = {0, 0, 1};
             region.dstSubresource = {0, 0, 1};
             region.srcOffsets[0] = {0, 0, 0};
             region.srcOffsets[1] = {(int)renderWidth, (int)renderHeight, 1};
             region.dstOffsets[0] = {0, 0, 0};
             region.dstOffsets[1] = {(int)renderWidth, (int)renderHeight, 1};
             
             cmd->BlitTexture(src, dst, &region, 1, FilterMode::Nearest);
             if (frameCount == 1) std::cout << "Finished HistoryCopy." << std::endl;
        });
    
    // 6. Debug/Final Pass
    if (debugPassEnabled) {
        graph.AddPass<DebugPassData>("DebugPass", RGPassType::Graphics, RGPassCategory::UI,
            [&](DebugPassData& data, RenderGraphBuilder& builder) {
                // Visualize ShadowMap0
                // data.input = builder.Read(shadowMap0); 
                // Or TAA Output
                data.input = builder.Read(taaOutput);
                // We also need to read other resources for debug visualization
                data.normal = builder.Read(gbufferNormal);
                data.depth = builder.Read(depthBuffer);
                data.shadowMap = builder.Read(shadowMap0);
                
                // Output to BackBuffer
                data.output = builder.Write(backBufferHandle, ResourceState::RenderTarget);
                
                RGRenderPassDesc rpDesc;
                RGAttachmentDesc colorDesc{};
                colorDesc.texture = data.output;
                colorDesc.loadOp = LoadAction::Clear;
                colorDesc.storeOp = StoreAction::Store;
                colorDesc.clearColor = { .color = primal::math::v4{0,0,0,0} };
                rpDesc.colors.push_back(colorDesc);
                builder.DeclareRenderPass(rpDesc);
            }, 
            [&](const DebugPassData& data, RenderGraphContext& context) {
                std::cout << "Executing DebugPass..." << std::endl;
                auto cmd = context.cmdBuffer;
                cmd->BindGraphicsPipeline(debugPipeline);
                cmd->SetViewport({ {0, 0 }, { (float)renderWidth, (float)renderHeight }, 0, 1});
                cmd->SetScissor({{ 0, 0 }, { renderWidth, renderHeight }});
                
                DescriptorData params[6]{
                    {.binding = 0, .type = DescriptorType::UniformBuffer, .resource = viewDataBuffer},
                    {.binding = 1, .type = DescriptorType::UniformBuffer, .resource = sceneDataBuffer},
                    {.binding = 2, .type = DescriptorType::SampledImage, .resource = graph.GetResource(data.input)->GetPhysicalHandle()},
                    {.binding = 3, .type = DescriptorType::SampledImage, .resource = graph.GetResource(data.normal)->GetPhysicalHandle()},
                    {.binding = 4, .type = DescriptorType::SampledImage, .resource = graph.GetResource(data.depth)->GetPhysicalHandle()},
                    {.binding = 5, .type = DescriptorType::SampledImage, .resource = graph.GetResource(data.shadowMap)->GetPhysicalHandle()},
                };
                UpdateDescriptorSet(device, debugDescriptorSet, params, 6);
                
                const DescriptorSetHandle debugSets[] = { debugDescriptorSet };
                cmd->BindDescriptorSets(PipelineBindPoint::Graphics, debugLayout, 0, 1, debugSets, 0, nullptr);
                cmd->Draw(3, 0, 1, 0);
            });
    } else {
        // Blit TAA to BackBuffer
        graph.AddPass<BlitPassData>("FinalBlit", RGPassType::Graphics, RGPassCategory::Present,
            [&](BlitPassData& data, RenderGraphBuilder& builder) {
            data.input = builder.Read(taaOutput); // Use TAA Output
            // data.input = builder.Read(lightingOutput); // Bypass TAA
            data.output = builder.Write(backBufferHandle, ResourceState::RenderTarget);
                
                RGRenderPassDesc rpDesc;
                RGAttachmentDesc colorDesc{};
                colorDesc.texture = data.output;
                colorDesc.loadOp = LoadAction::Clear;
                colorDesc.storeOp = StoreAction::Store;
                colorDesc.clearColor = { .color = primal::math::v4{0.0f, 0.0f, 0.0f, 1.0f} }; 
                rpDesc.colors.push_back(colorDesc);
                builder.DeclareRenderPass(rpDesc);
            }, 
            [&](const BlitPassData& data, RenderGraphContext& context) {
                if (frameCount % 120 == 0) {
                     std::cout << "Executing FinalBlit (Frame " << frameCount << ")... Source: TAA Output" << std::endl;
                     auto* inputRes = graph.GetResource(data.input);
                     if (inputRes) {
                         std::cout << "  FinalBlit Input Handle: " << inputRes->GetPhysicalHandle() << std::endl;
                     }
                }
                auto cmd = context.cmdBuffer;
                cmd->BindGraphicsPipeline(postProcessPipeline);
                cmd->SetViewport({ {0, 0 }, { (float)renderWidth, (float)renderHeight }, 0, 1});
                cmd->SetScissor({{ 0, 0 }, { renderWidth, renderHeight }});
                
                DescriptorData params[2]{
                    {.binding = 0, .type = DescriptorType::SampledImage, .resource = graph.GetResource(data.input)->GetPhysicalHandle()},
                    {.binding = 1, .type = DescriptorType::Sampler, .resource = debugSampler},
                };
                UpdateDescriptorSet(device, postProcessDescriptorSet, params, 2);
                
                const DescriptorSetHandle sets[] = { postProcessDescriptorSet };
                cmd->BindDescriptorSets(PipelineBindPoint::Graphics, postProcessLayout, 0, 1, sets, 0, nullptr);
                cmd->Draw(3, 0, 1, 0);
            });
    }
}

void TestSponzaRenderGraph::Run() {
    frameCount++;
    
    UpdateScene();
    
    // Acquire BackBuffer
    ResourceHandle backBuffer;
    
    // The BeginFrame signature: bool BeginFrame(rhi::ResourceHandle& outBackBuffer, rhi::SyncHandle& outSignalFence);
    // outSignalFence is a fence we MUST signal when we are done rendering to this image.
    // Wait, usually it's:
    // Acquire -> returns image + waitFence (image ready)
    // QueueSubmit -> waits on waitFence, signals signalFence (render done)
    // Present -> waits on signalFence.
    
    // If RenderSystem::BeginFrame follows this pattern:
    // outSignalFence is the fence we must signal.
    // Where is the waitFence? Maybe RenderSystem handles waiting internally or BeginFrame blocks?
    // "Get next available back buffer".
    
    SyncHandle renderDoneFence;
    if (!renderSystem.BeginFrame(backBuffer, renderDoneFence)) {
        return;
    }

    // Create Command Buffer
    CommandBufferHandle cmdHandle = device->CreateCommandBuffer(CommandQueueType::Graphics);
    MetalDevice* metalDevice = static_cast<MetalDevice*>(device);
    RHICommandBuffer* cmd = metalDevice->GetCommandBuffer(cmdHandle);
    cmd->Begin();
    
    renderGraph->Clear();
    BuildRenderGraph(*renderGraph, backBuffer);
    renderGraph->Compile();
    renderGraph->Execute(cmd);
    
    cmd->End();
    
    // Submit with fence
    QueueSubmitInfo submitInfo{};
    submitInfo.cmdBuffer = cmdHandle;
    submitInfo.signalFence = renderDoneFence;
    device->Submit(submitInfo);
    
    // Clean up command buffer after submission
    // For this test, we wait for the fence to ensure safety and avoid leaks.
    device->WaitForSync(renderDoneFence, UINT32_MAX);
    device->DestroyCommandBuffer(cmdHandle);
    
    renderSystem.EndFrame();
    
    if (frameCount == 1) {
        std::cout << "Frame " << frameCount << " Running. Scene Meshes: " << sceneMeshes.size() << std::endl;
    }

    // Validation Logic (Frame 60 & 120)
    if (frameCount == 60 || frameCount == 120) {
        ValidateFrame();
    }
}


// Helper function for updating buffer
void UpdateBuffer(RHIDeviceBase* device, ResourceHandle buffer, const void* data, size_t size) {
    void* mapped = device->MapBuffer(buffer, 0, size);
    if (mapped) {
        memcpy(mapped, data, size);
        device->UnmapBuffer(buffer);
    }
}

void TestSponzaRenderGraph::UpdateScene() {
    // Check F1 Toggle
    primal::input::input_value f1_value;
    primal::input::get(primal::input::input_source::keyboard, primal::input::input_code::key_f1, f1_value);
    
    if (f1_value.current.x != 0.0f && f1_value.previous.x == 0.0f) {
        debugPassEnabled = !debugPassEnabled;
        std::cout << "Debug Pass Toggled: " << (debugPassEnabled ? "ON" : "OFF") << std::endl;
    }

    // 1. Update Camera (ViewData)
    
    // Calculate Delta Time
    auto currentTime = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(currentTime - lastFrameTime).count();
    lastFrameTime = currentTime;

    // Update Camera
    m_camera.Update(dt);

    primal::math::m4x4 view = m_camera.GetViewMatrix();
    primal::math::v3 eye = m_camera.GetPosition();
    
    float aspect = (float)renderWidth / (float)renderHeight;
    primal::math::m4x4 proj = primal::graphics::rhi::math::CreatePerspectiveMatrix(45.0f * primal::graphics::rhi::math::constants::DEG_TO_RAD, aspect, 0.1f, 10000.0f);
    
    // Flip Y for Metal/Vulkan coordinate difference -> REMOVED for Metal Y-Up consistency
    // proj.columns[1][1] *= -1.0f;

    // Fix Z range for Metal [0, 1]
    // RHIMath CreatePerspectiveMatrix ALREADY returns [0, 1] Z range.
    // So we do NOT need to apply the fix.
    
    // TAA Jitter (ENABLED)
    int sampleIndex = frameCount % 16;
    float jitterX = (halton_sequence(sampleIndex + 1, 2) - 0.5f) / (float)renderWidth;
    float jitterY = (halton_sequence(sampleIndex + 1, 3) - 0.5f) / (float)renderHeight;
    
    // Apply jitter to projection matrix (Shear)
    // NDC space is [-1, 1], size 2. So shift in NDC = jitterUV * 2.0.
    proj.columns[2][0] += jitterX * 2.0f;
    proj.columns[2][1] += jitterY * 2.0f;

    ViewData viewData;
    viewData.viewProjection = proj * view;
    viewData.invViewProjection = primal::graphics::rhi::math::Inverse(viewData.viewProjection);
    
    // Debug: Print Matrices
    if (frameCount == 1) std::cout << "!!!! Frame " << frameCount << " Camera Eye: " << eye.x << ", " << eye.y << ", " << eye.z << std::endl;
    if (frameCount == 1) {
        std::cout << "--- Frame " << frameCount << " Debug Info ---" << std::endl;
        std::cout << "Camera Eye: " << eye.x << ", " << eye.y << ", " << eye.z << std::endl;
        std::cout << "View Matrix:" << std::endl;
        for(int r=0; r<4; ++r) {
            std::cout << "  [" << view.columns[0][r] << ", " << view.columns[1][r] << ", " << view.columns[2][r] << ", " << view.columns[3][r] << "]" << std::endl;
        }
        std::cout << "Projection Matrix:" << std::endl;
        for(int r=0; r<4; ++r) {
            std::cout << "  [" << proj.columns[0][r] << ", " << proj.columns[1][r] << ", " << proj.columns[2][r] << ", " << proj.columns[3][r] << "]" << std::endl;
        }
        std::cout << "ViewProjection Matrix:" << std::endl;
        for(int r=0; r<4; ++r) {
            std::cout << "  [" << viewData.viewProjection.columns[0][r] << ", " << viewData.viewProjection.columns[1][r] << ", " << viewData.viewProjection.columns[2][r] << ", " << viewData.viewProjection.columns[3][r] << "]" << std::endl;
        }
    }

    if (frameCount <= 1) {
        previousViewProjection = viewData.viewProjection;
    }
    viewData.previousViewProjection = previousViewProjection;
    previousViewProjection = viewData.viewProjection; // Update for next frame
    
    // Debug: Print ViewProjection Matrix
    if (frameCount == 1) {
        std::cout << "DEBUG: Frame " << frameCount << " Camera Info:" << std::endl;
        std::cout << "  Eye: " << eye.x << ", " << eye.y << ", " << eye.z << std::endl;
        // std::cout << "  Center: " << center.x << ", " << center.y << ", " << center.z << std::endl;
        std::cout << "DEBUG: ViewProjection Matrix:" << std::endl;
        for (int i = 0; i < 4; ++i) {
            std::cout << "  Row " << i << ": " 
                      << viewData.viewProjection.columns[i][0] << ", " 
                      << viewData.viewProjection.columns[i][1] << ", " 
                      << viewData.viewProjection.columns[i][2] << ", " 
                      << viewData.viewProjection.columns[i][3] << std::endl;
        }
    }

    if (viewDataBuffer != handles::INVALID_RESOURCE) {
        UpdateBuffer(device, viewDataBuffer, &viewData, sizeof(ViewData));
    }

    // Update TAA Uniforms
    TAAUniforms taaUniforms;
    taaUniforms.resolution = {(float)renderWidth, (float)renderHeight};
    taaUniforms.jitter = {jitterX * 2.0f, jitterY * 2.0f}; // NDC Jitter
    taaUniforms.previousJitter = previousJitter;
    taaUniforms.feedback = (frameCount <= 1) ? 0.0f : 0.95f; 
    taaUniforms.padding = 0.0f;
    
    primal::math::v2 oldJitter = previousJitter;
    previousJitter = taaUniforms.jitter;
    
    if (taaUniformBuffer != handles::INVALID_RESOURCE) {
        UpdateBuffer(device, taaUniformBuffer, &taaUniforms, sizeof(TAAUniforms));
    }
    
    // 2. Update Lights & Shadows (SceneData)
    // Angled light for better shadows (45 degrees from top-right)
    primal::math::v4 lightPos = {100.0f, 150.0f, 50.0f, 0.0f}; 
    primal::math::v3 lightDir = primal::graphics::rhi::math::Normalize(primal::math::v3{-lightPos.x, -lightPos.y, -lightPos.z}); 

    // FIX: Use (1,0,0) as Up vector to avoid singularity when looking down
    primal::math::v3 lightUp = {0.0f, 1.0f, 0.0f};
    if (abs(lightDir.y) > 0.9f) lightUp = {1.0f, 0.0f, 0.0f};
    
    // Light Pos for SceneData (Directional light w=0, but position is used for direction calc in shader)
    // In shader: L = normalize(lightPos.xyz - worldPos) if w=1, or lightPos.xyz if w=0.
    // Usually directional light pos is -Direction.
    
    // Cascade 0
    // Light Eye at 500 units (closer).
    primal::math::v3 lightEye0 = {-lightDir.x * 200.0f, -lightDir.y * 200.0f, -lightDir.z * 200.0f};
    primal::math::m4x4 lightView0 = primal::graphics::rhi::math::CreateLookAtMatrix(lightEye0, {0,0,0}, lightUp);
    
    // Sponza is approx 20x15x10.
    // Bounds [-25, 25] should cover the scene tightly.
    // Far plane 1000.0f covers the distance.
    // Use Top=25, Bottom=-25 for Y-Up Coordinate System (Metal)
    // Left, Right, Top, Bottom, Near, Far
    primal::math::m4x4 lightProj0 = primal::graphics::rhi::metal::CreateOrthographicMatrix(-30.0f, 30.0f, 30.0f, -30.0f, 0.1f, 1000.0f);
    
    // Note: RHIMath CreateOrthographicMatrix produces [0, 1] Z-range for Metal automatically.
    // No manual Z-fix needed.
    
    lightVP0 = lightProj0 * lightView0;
    
    // Cascade 1 (Far)
    primal::math::m4x4 lightView1 = lightView0;
    // Increase Cascade 1 coverage to ensure all objects are covered
    // Use Top=5000, Bottom=-5000 for Y-Up Coordinate System
    primal::math::m4x4 lightProj1 = primal::graphics::rhi::metal::CreateOrthographicMatrix(-500.0f, 500.0f, 500.0f, -500.0f, 0.1f, 5000.0f);
    
    lightVP1 = lightProj1 * lightView1;
    
    // Debug Print (Reduced Frequency)
    if (frameCount == 1) {
        std::cout << "Light0 Eye: " << lightEye0.x << ", " << lightEye0.y << ", " << lightEye0.z << std::endl;
        std::cout << "LightVP0[0]: " << lightVP0.columns[0][0] << ", " << lightVP0.columns[0][1] << ", " << lightVP0.columns[0][2] << ", " << lightVP0.columns[0][3] << std::endl;
        std::cout << "LightVP0[3]: " << lightVP0.columns[3][0] << ", " << lightVP0.columns[3][1] << ", " << lightVP0.columns[3][2] << ", " << lightVP0.columns[3][3] << std::endl;
    }
    
    SceneData sceneData;
    memset(&sceneData, 0, sizeof(SceneData));
    sceneData.model = primal::graphics::rhi::math::MatrixIdentity();
    sceneData.previousModel = primal::graphics::rhi::math::MatrixIdentity();
    sceneData.jitter = {jitterX * 2.0f, jitterY * 2.0f}; // NDC Jitter
    sceneData.previousJitter = oldJitter;
    
    sceneData.shadowMatrix0 = lightVP0;
    sceneData.shadowMatrix1 = lightVP1;
    
    // Fake directional light as far point light
    // Use Directional Light Mode (w=0) for proper lighting without distance attenuation issues
    sceneData.lightPos = lightPos; // Use our angled light position
    // Increase Light Intensity for better contrast
    sceneData.lightColor = { 20.0f, 20.0f, 20.0f, 1.0f }; 
    sceneData.viewPos = { eye.x, eye.y, eye.z, 1.0f };
    
    if (sceneDataBuffer != handles::INVALID_RESOURCE) {
        UpdateBuffer(device, sceneDataBuffer, &sceneData, sizeof(SceneData));
    }
}

void TestSponzaRenderGraph::ValidateFrame() {
    std::cout << "Validating Frame " << frameCount << std::endl;

    // Create a temporary command buffer for readback
    CommandBufferHandle cmdHandle = device->CreateCommandBuffer(CommandQueueType::Graphics);
    MetalDevice* metalDevice = static_cast<MetalDevice*>(device);
    RHICommandBuffer* cmd = metalDevice->GetCommandBuffer(cmdHandle);
    cmd->Begin();

    // Copy History Texture (which contains TAA output) to Readback Buffer
    BufferTextureCopyRegion region;
    region.bufferOffset = 0;
    region.bufferRowLength = renderWidth;
    region.bufferImageHeight = renderHeight;
    region.imageSubresource = {0, 0, 1};
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {(uint32_t)renderWidth, (uint32_t)renderHeight, 1};
    
    cmd->CopyTextureToBuffer(historyTexture, readbackBuffer, &region, 1);
    
    cmd->End();
    
    // Submit and Wait
    SyncHandle fence = device->CreateSync();
    QueueSubmitInfo submitInfo{};
    submitInfo.cmdBuffer = cmdHandle;
    submitInfo.signalFence = fence;
    device->Submit(submitInfo);
    
    device->WaitForSync(fence, 10000);
    device->DestroySync(fence);
    device->DestroyCommandBuffer(cmdHandle);
    
    // Map and Validate
    void* data = device->MapBuffer(readbackBuffer, 0, (uint64_t)renderWidth * renderHeight * 4);
    if (data) {
        uint8_t* pixels = static_cast<uint8_t*>(data);
        // Simple check: Average brightness > 0 (not black)
        uint64_t sum = 0;
        for (size_t i = 0; i < (size_t)renderWidth * renderHeight; ++i) {
            sum += pixels[i*4 + 0]; // B
            sum += pixels[i*4 + 1]; // G
            sum += pixels[i*4 + 2]; // R
        }
        double avg = (double)sum / ((double)renderWidth * renderHeight * 3);
        std::cout << "Average Brightness: " << avg << std::endl;
        
        // PSNR Validation (Placeholder logic as we don't have reference)
        // In a real scenario, we would load a reference image and compare.
        // For now, we just check if we rendered something.
        bool valid = avg > 1.0; 
        std::cout << "Validation " << (valid ? "Passed" : "Failed") << " (Avg Brightness: " << avg << ")" << std::endl;
        
        device->UnmapBuffer(readbackBuffer);
    }
}

void TestSponzaRenderGraph::Shutdown() {
    std::cout << "TestSponzaRenderGraph::Shutdown Start" << std::endl;
    // 1. Wait for GPU idle
    if (device) {
        device->WaitIdle();
    }
    std::cout << "Device WaitIdle Done" << std::endl;

    // 2. Destroy System & Helper Objects
    renderGraph.reset();
    std::cout << "RenderGraph Reset Done" << std::endl;
    iblPrecomputer.reset();
    std::cout << "IBLPrecomputer Reset Done" << std::endl;
    
    // Destroy Scene Meshes
    std::cout << "Destroying Scene Meshes..." << std::endl;
    for (auto& meshInfo : sceneMeshes) {
        if (meshInfo.mesh) {
            meshInfo.mesh->Destroy(device); // Explicitly destroy GPU resources
            delete meshInfo.mesh;
            meshInfo.mesh = nullptr;
        }
    }
    sceneMeshes.clear();
    std::cout << "SceneMeshes Cleared" << std::endl;
    
    // 3. Destroy Textures
    auto SafeDestroyTexture = [&](ResourceHandle& handle) {
        if (handle != handles::INVALID_RESOURCE) {
            device->DestroyTexture(handle);
            handle = handles::INVALID_RESOURCE;
        }
    };
    std::cout << "Destroying Textures..." << std::endl;
    SafeDestroyTexture(historyTexture);
    SafeDestroyTexture(motionVectorTexture);
    
    // skyboxTexture might be an alias for envCubemap (see SetupIBL)
    if (skyboxTexture == envCubemap) {
        skyboxTexture = handles::INVALID_RESOURCE;
    }
    SafeDestroyTexture(skyboxTexture);
    
    SafeDestroyTexture(irradianceMap);
    SafeDestroyTexture(prefilteredMap);
    SafeDestroyTexture(brdfLUT);
    SafeDestroyTexture(depthTexture);
    SafeDestroyTexture(shadowMap0); // These are INVALID_RESOURCE in member scope, so safe
    SafeDestroyTexture(shadowMap1);
    SafeDestroyTexture(envCubemap);
    std::cout << "Textures Destroyed" << std::endl;

    // 4. Destroy Pipelines & Layouts
    auto SafeDestroyPipeline = [&](PipelineHandle& handle) {
        if (handle != handles::INVALID_PIPELINE) {
            device->DestroyPipeline(handle);
            handle = handles::INVALID_PIPELINE;
        }
    };
    std::cout << "Destroying Pipelines..." << std::endl;
    SafeDestroyPipeline(gbufferPipeline);
    SafeDestroyPipeline(lightingPipeline);
    SafeDestroyPipeline(skyboxPipeline);
    SafeDestroyPipeline(shadowPipeline);

    auto SafeDestroyPipelineLayout = [&](PipelineLayoutHandle& handle) {
        if (handle != handles::INVALID_PIPELINE_LAYOUT) {
            device->DestroyPipelineLayout(handle);
            handle = handles::INVALID_PIPELINE_LAYOUT;
        }
    };
    SafeDestroyPipelineLayout(gbufferLayout);
    SafeDestroyPipelineLayout(lightingLayout);
    SafeDestroyPipelineLayout(skyboxLayout);
    SafeDestroyPipelineLayout(shadowLayout);
    std::cout << "Pipelines Destroyed" << std::endl;

    // 5. Destroy Descriptor Sets & Layouts
    // Descriptor Sets are usually freed when the pool is reset or destroyed, 
    // but we can try to free them if supported.
    // Assuming RHI doesn't require explicit FreeDescriptorSet if Layout/Pool is destroyed.
    // But we should destroy Layouts.
    
    auto SafeDestroyDescriptorSetLayout = [&](DescriptorSetLayoutHandle& handle) {
        if (handle != handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
            device->DestroyDescriptorSetLayout(handle);
            handle = handles::INVALID_DESCRIPTOR_SET_LAYOUT;
        }
    };
    std::cout << "Destroying DescriptorSetLayouts..." << std::endl;
    SafeDestroyDescriptorSetLayout(globalSetLayout);
    if (ownsMaterialSetLayout) {
        SafeDestroyDescriptorSetLayout(materialSetLayout);
    }
    if (defaultSampler != handles::INVALID_SAMPLER) {
        device->DestroySampler(defaultSampler);
        defaultSampler = handles::INVALID_SAMPLER;
    }
    SafeDestroyDescriptorSetLayout(lightingSetLayout);
    SafeDestroyDescriptorSetLayout(skyboxSetLayout);
    std::cout << "DescriptorSetLayouts Destroyed" << std::endl;

    // 6. Destroy Buffers
    auto SafeDestroyBuffer = [&](ResourceHandle& handle) {
        if (handle != handles::INVALID_RESOURCE) {
            device->DestroyBuffer(handle);
            handle = handles::INVALID_RESOURCE;
        }
    };
    std::cout << "Destroying Buffers..." << std::endl;
    SafeDestroyBuffer(viewDataBuffer);
    SafeDestroyBuffer(sceneDataBuffer);
    SafeDestroyBuffer(taaUniformBuffer);
    SafeDestroyBuffer(readbackBuffer);
    std::cout << "Buffers Destroyed" << std::endl;

    // 7. Destroy Shaders
    std::cout << "Destroying Shaders..." << std::endl;
    for (auto& pair : shaderVariantMap) {
        if (pair.second != handles::INVALID_SHADER) {
            device->DestroyShader(pair.second);
        }
    }
    shaderVariantMap.clear();
    std::cout << "Shaders Destroyed" << std::endl;

    // 8. RenderSystem Shutdown
    std::cout << "RenderSystem Shutdown..." << std::endl;
    if (device) {
        device->WaitIdle();
    }
    renderSystem.Shutdown();
    std::cout << "RenderSystem Shutdown Done" << std::endl;
    
    if (device) {
        device->GetGarbageCollector().Flush();
    }

    instance = nullptr;

    if (window.is_valid()) {
        primal::platform::remove_window(window.get_id());
    }
    std::cout << "TestSponzaRenderGraph::Shutdown End" << std::endl;
}

void TestSponzaRenderGraph::Resize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return;
    
    uint32_t newWidth = width;
    uint32_t newHeight = height;

#ifdef __APPLE__
    newWidth *= 2;
    newHeight *= 2;
#endif

    renderSystem.Resize(newWidth, newHeight);
    
    renderWidth = newWidth;
    renderHeight = newHeight;
}
