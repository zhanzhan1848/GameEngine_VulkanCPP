#include "TestGeometryDebugSponza.h"
#include "Engine/Content/ContentToEngine.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalDevice.h"
#include "Engine/Graphics/RenderGraph/RenderGraphBuilder.h"
#include "Engine/Graphics/RenderGraph/RenderGraphDefinitions.h"
#include "Engine/Graphics/Lighting/LightProbeManager.h"
#include "Engine/Input/Input.h"
#include "ShaderCompilation.h"

#define STB_IMAGE_IMPLEMENTATION
#include "third_party/astc-encoder/Source/ThirdParty/stb_image.h"

#include <iostream>
#include <fstream>
#include <iomanip>
#include <filesystem>
#include <cmath>
#include <algorithm>
#include "Utilities/IOStream.h"

using namespace primal;
using namespace primal::graphics;
using namespace primal::graphics::rhi;
using namespace primal::graphics::rendergraph;

#include "Engine/Graphics/RHI/Platforms/Metal/MetalCommandBuffer.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalMath.h"

// Initialize static member
TestGeometryDebugSponza* TestGeometryDebugSponza::instance = nullptr;

#ifdef TEST_GEOMETRY_DEBUG_SPONZA
Engine_Test::Engine_Test() 
    : primal::test::RenderTestRunner(std::make_unique<TestGeometryDebugSponza>()) 
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
    const shader_file_info blit_vs_info{ "DeferredLighting.metal", "vertexMain", shader_type::vertex };
    const shader_file_info blit_ps_info{ "DeferredLighting.metal", "fragmentBlit", shader_type::pixel };
    const shader_file_info eq2cube_cs_info{ "EquirectangularToCube.metal", "CS_EquirectangularToCube", shader_type::compute };
    
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
    
    // Helpers for Scene Loading
    std::string NormalizePath(const std::string& path) {
        std::string p = path;
        std::replace(p.begin(), p.end(), '\\', '/');
        return p;
    }

    std::string ResolveTexturePath(const std::string& assetBaseDir, const std::string& filename) {
        if (filename.empty()) return "";
        std::string cleanName = NormalizePath(filename);
        
        std::vector<std::string> basePaths;
        basePaths.push_back(assetBaseDir);
        basePaths.push_back(assetBaseDir + "models/Sponza/");
        basePaths.push_back(assetBaseDir + "fbx_textures/");
        
        for (const auto& base : basePaths) {
            std::string fullPath = base + cleanName;
            std::ifstream f(fullPath.c_str());
            if (f.good()) return fullPath;
        }
        return NormalizePath(assetBaseDir + cleanName);
    }

    primal::graphics::rhi::ResourceHandle CreateTextureFromData(RHIDeviceBase* /*device*/, uint32_t width, uint32_t height, const unsigned char* data, bool isSRGB) {
        // Use correct DataFormat enums instead of hardcoded values
        // RGBA8_UNorm = 36, RGBA8_sRGB = 40
        DataFormat format = isSRGB ? DataFormat::RGBA8_sRGB : DataFormat::RGBA8_UNorm; 
        uint32_t row_pitch = width * 4;
        uint32_t slice_pitch = height * row_pitch;
        size_t blob_size = (6 * sizeof(uint32_t)) + (2 * sizeof(uint32_t) + slice_pitch);
        
        std::vector<uint8_t> blob(blob_size);
        utl::blob_stream_writer writer(blob.data(), blob.size());
        
        writer.write((uint32_t)width);
        writer.write((uint32_t)height);
        writer.write((uint32_t)1); // array_size
        writer.write((uint32_t)0); // flags
        writer.write((uint32_t)1); // mip_levels
        writer.write((uint32_t)format); // Write enum value as uint32_t
        writer.write(row_pitch);
        writer.write(slice_pitch);
        writer.write(data, slice_pitch);

        primal::id::id_type id = primal::content::create_resource(blob.data(), primal::content::asset_type::texture);
        if (primal::id::is_valid(id)) {
            return primal::content::get_rhi_texture_handle(id);
        }
        return handles::INVALID_RESOURCE;
    }

    primal::graphics::rhi::ResourceHandle LoadTextureFromFile(RHIDeviceBase* device, const std::string& path, bool isNormalMap, bool isSRGB = true) {
        int width, height, channels;
        unsigned char* data = stbi_load(path.c_str(), &width, &height, &channels, 4);
        if (!data) {
            std::cerr << "Failed to load texture: " << path << std::endl;
            return handles::INVALID_RESOURCE;
        }

        ResourceHandle handle = CreateTextureFromData(device, width, height, data, isSRGB);
        stbi_image_free(data);
        return handle;
    }

    void WriteTexture(RHIDeviceBase* device, ResourceHandle texture, const void* data, uint64_t size, uint32_t width, uint32_t height, uint32_t layer) {
        BufferDesc stagingDesc{ .size = size, .usage = GPUMemoryUsage::Dynamic, .memoryUsage = GPUMemoryUsage::Dynamic };
        ResourceHandle stagingBuffer = device->CreateBuffer(stagingDesc);
        if (stagingBuffer == handles::INVALID_RESOURCE) return;

        void* mapped = device->MapBuffer(stagingBuffer);
        if (mapped) {
            memcpy(mapped, data, size);
            device->UnmapBuffer(stagingBuffer);
        }

        SyncHandle fence = device->CreateSync();
        CommandBufferHandle cmdHandle = device->CreateCommandBuffer(CommandQueueType::Graphics);
        MetalDevice* metalDevice = static_cast<MetalDevice*>(device);
        RHICommandBuffer* cmd = metalDevice->GetCommandBuffer(cmdHandle);
        cmd->Begin();
        
        BufferTextureCopyRegion region;
        region.bufferOffset = 0;
        region.imageSubresource.baseArrayLayer = layer;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = { 0, 0, 0 };
        region.imageExtent = { width, height, 1 };
        
        cmd->CopyBufferToTexture(stagingBuffer, texture, &region, 1);
        cmd->End();
        
        QueueSubmitInfo submitInfo{};
        submitInfo.cmdBuffer = cmdHandle;
        submitInfo.signalFence = fence;
        device->Submit(submitInfo);
        device->WaitForSync(fence, UINT32_MAX);
        
        device->DestroySync(fence);
        device->DestroyCommandBuffer(cmdHandle);
        device->DestroyBuffer(stagingBuffer);
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

struct BlitData {
    RGResourceHandle input;
    RGResourceHandle output;
};

bool TestGeometryDebugSponza::Initialize() {
    std::cout << "DEBUG: TestGeometryDebugSponza STARTING NEW VERSION " << __DATE__ << " " << __TIME__ << std::endl;
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
    primal::graphics::rhi::g_deviceManager.RegisterDevice(device);
    device_ownership = std::move(metalDevice);

    // 2. Initialize Window & RenderSystem
    primal::platform::window_init_info winInfo{
        nullptr, nullptr,
        "TestGeometryDebugSponza", 100, 100, 1280, 720
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

    // Create Samplers (Before LoadScene)
    {
        SamplerDesc samplerDesc{
            .minFilter = FilterMode::Linear,
            .magFilter = FilterMode::Linear,
            .mipFilter = FilterMode::Linear,
            .addressU = TextureAddressMode::Wrap,
            .addressV = TextureAddressMode::Wrap,
            .addressW = TextureAddressMode::Wrap,
        };
        defaultSampler = device->CreateSampler(samplerDesc);
        
        SamplerDesc brdfSamplerDesc{
            .minFilter = FilterMode::Linear,
            .magFilter = FilterMode::Linear,
            .addressU = TextureAddressMode::Clamp,
            .addressV = TextureAddressMode::Clamp,
        };
        brdfSampler = device->CreateSampler(brdfSamplerDesc);
        
        SamplerDesc debugSamplerDesc{
            .minFilter = FilterMode::Linear,
            .magFilter = FilterMode::Linear,
            .addressU = TextureAddressMode::Clamp,
            .addressV = TextureAddressMode::Clamp,
            .addressW = TextureAddressMode::Clamp,
        };
        debugSampler = device->CreateSampler(debugSamplerDesc);
    }

    // Create Material Layout (Needed for LoadScene)
    {
        DescriptorSetLayoutBinding bindings[] = {
            { 0, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr }, // Albedo
            { 1, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr }, // Normal
            { 2, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr }, // ORM
            { 3, DescriptorType::Sampler, 1, ShaderStage::Pixel, nullptr }        // Sampler
        };
        DescriptorSetLayoutDesc desc{ .bindings = bindings, .bindingCount = 4 };
        materialSetLayout = device->CreateDescriptorSetLayout(desc);
    }

    // 5. Create Persistent Resources (Textures)
    if (!CreatePersistentResources()) {
        std::cerr << "Failed to create Persistent Resources" << std::endl;
        return false;
    }

    // 6. Load Scene
    if (!LoadScene()) {
        std::cerr << "Failed to load scene" << std::endl;
        return false;
    }

    // 7. Setup IBL
    if (!SetupIBL()) {
        std::cerr << "Failed to setup IBL" << std::endl;
        return false;
    }

    // 8. Setup Pipelines
    if (!SetupPipelines()) {
        std::cerr << "Failed to setup Pipelines" << std::endl;
        return false;
    }

    // 9. Create Uniform Buffers
    if (!CreateUniformBuffers()) {
        std::cerr << "Failed to create Uniform Buffers" << std::endl;
        return false;
    }

    // 10. Create Descriptor Sets
    if (!CreateDescriptorSets()) {
        std::cerr << "Failed to create Descriptor Sets" << std::endl;
        return false;
    }
    
    // Initialize Camera
    // Position matches TestSponzaRenderGraph: "eye" {0, 5, 0}, "center" {10, 5, 0} -> Forward +X
    m_camera.Initialize({0.0f, 5.0f, 0.0f}, {0.0f, 0.0f, 0.0f});
    m_camera.SetSpeed(10.0f, 0.1f);
    
    // Initialize Debug Settings
    debugSettings.enable = false; // Disable debug to show normal rendering
    debugSettings.mode = GeometryDebugMode::Meshlet; // Start with Meshlet debug to avoid screen coverage
    debugSettings.meshletColorMode = MeshletColorMode::Colored;
    debugSettings.wireframe = false;
    debugSettings.visualize_meshlets = false;
    debugSettings.visualize_sdf = false; // Disable SDF by default as it may occlude the scene
    debugSettings.visualize_voxels = false;
    debugSettings.visualize_vector_field = false;
    
    std::cout << "TestGeometryDebugSponza Initialized. Debug Mode: Disabled" << std::endl;

    return true;
}

void TestGeometryDebugSponza::Resize(uint32_t width, uint32_t height) {
    renderWidth = width;
    renderHeight = height;
    renderSystem.Resize(width, height);
    
    // Recreate resources if needed
    CreatePersistentResources();
}

void TestGeometryDebugSponza::Run() {
    // Update Camera & Input
    // primal::input::input::update(); // Handled by engine/platform
    m_camera.Update(0.016f); // Fixed dt for test
    
    primal::input::input_value val;

    // F1: Meshlet Mode
    primal::input::get(primal::input::input_source::keyboard, primal::input::input_code::key_f1, val);
    if (val.current.x > 0.0f) {
        debugSettings.enable = true;
        debugSettings.mode = GeometryDebugMode::Meshlet;
        debugSettings.visualize_meshlets = true;
        debugSettings.visualize_sdf = false;
        debugSettings.visualize_voxels = false;
        debugSettings.visualize_vector_field = false;
        std::cout << "Debug Mode: Meshlet" << std::endl;
    }

    // F2: SDF Mode
    primal::input::get(primal::input::input_source::keyboard, primal::input::input_code::key_f2, val);
    if (val.current.x > 0.0f) {
        debugSettings.enable = true;
        debugSettings.mode = GeometryDebugMode::SDF;
        debugSettings.visualize_meshlets = false;
        debugSettings.visualize_sdf = true;
        debugSettings.visualize_voxels = false;
        debugSettings.visualize_vector_field = false;
        std::cout << "Debug Mode: SDF" << std::endl;
    }

    // F3: Voxel Mode
    primal::input::get(primal::input::input_source::keyboard, primal::input::input_code::key_f3, val);
    if (val.current.x > 0.0f) {
        debugSettings.enable = true;
        debugSettings.mode = GeometryDebugMode::Voxel;
        debugSettings.visualize_meshlets = false;
        debugSettings.visualize_sdf = false;
        debugSettings.visualize_voxels = true;
        debugSettings.visualize_vector_field = false;
        std::cout << "Debug Mode: Voxel" << std::endl;
    }

    // F4: Vector Field Mode
    primal::input::get(primal::input::input_source::keyboard, primal::input::input_code::key_f4, val);
    if (val.current.x > 0.0f) {
        debugSettings.enable = true;
        debugSettings.mode = GeometryDebugMode::VectorField;
        debugSettings.visualize_meshlets = false;
        debugSettings.visualize_sdf = false;
        debugSettings.visualize_voxels = false;
        debugSettings.visualize_vector_field = true;
        std::cout << "Debug Mode: VectorField" << std::endl;
    }
    
    // F5: Toggle Enable/Disable
    primal::input::get(primal::input::input_source::keyboard, primal::input::input_code::key_f5, val);
    if (val.current.x > 0.0f && val.previous.x == 0.0f) { // Trigger on press
        debugSettings.enable = !debugSettings.enable;
        std::cout << "Debug Mode: " << (debugSettings.enable ? "Enabled" : "Disabled") << std::endl;
    }

    // Up/Down: Change Slice Depth
    primal::input::get(primal::input::input_source::keyboard, primal::input::input_code::key_up, val);
    if (val.current.x > 0.0f) {
        debugSettings.slice_depth += 0.01f;
        if (debugSettings.slice_depth > 1.0f) debugSettings.slice_depth = 1.0f;
        std::cout << "Slice Depth: " << debugSettings.slice_depth << std::endl;
    }
    primal::input::get(primal::input::input_source::keyboard, primal::input::input_code::key_down, val);
    if (val.current.x > 0.0f) {
        debugSettings.slice_depth -= 0.01f;
        if (debugSettings.slice_depth < 0.0f) debugSettings.slice_depth = 0.0f;
        std::cout << "Slice Depth: " << debugSettings.slice_depth << std::endl;
    }

    UpdateScene();
    
    rhi::ResourceHandle backBuffer;
    rhi::SyncHandle imageAvailable;
    if (renderSystem.BeginFrame(backBuffer, imageAvailable)) {
        auto cmd = renderSystem.GetCurrentCommandBuffer();
        cmd->Reset();
        cmd->Begin();
        
        renderGraph->Clear();
        BuildRenderGraph(*renderGraph, backBuffer);
        renderGraph->Compile();
        renderGraph->Execute(cmd);
        
        cmd->End();
        
        // Submit
        rhi::QueueSubmitInfo submitInfo{};
        submitInfo.cmdBuffer = renderSystem.GetCurrentCommandBufferHandle();
        submitInfo.signalFence = imageAvailable;
        device->Submit(submitInfo);
        
        renderSystem.EndFrame();
        frameCount++;
    }
}

TestGeometryDebugSponza::~TestGeometryDebugSponza() {
    Shutdown();
}

void TestGeometryDebugSponza::Shutdown() {
    if (isShutdown) return;
    isShutdown = true;

    if (device) {
        device->WaitIdle();
    }

    // Destroy Pipelines
    device->DestroyPipeline(gbufferPipeline);
    device->DestroyPipeline(lightingPipeline);
    device->DestroyPipeline(skyboxPipeline);
    device->DestroyPipeline(shadowPipeline);
    device->DestroyPipeline(blitPipeline);

    // Destroy Pipeline Layouts
    device->DestroyPipelineLayout(gbufferLayout);
    device->DestroyPipelineLayout(lightingLayout);
    device->DestroyPipelineLayout(skyboxLayout);
    device->DestroyPipelineLayout(shadowLayout);
    device->DestroyPipelineLayout(blitLayout);

    // Destroy Descriptor Sets
    // Note: Some RHIs free sets when pool is reset, but explicit destroy is safer if we own them
    if (globalDescriptorSet != handles::INVALID_DESCRIPTOR_SET) device->DestroyDescriptorSet(globalDescriptorSet);
    if (lightingDescriptorSet != handles::INVALID_DESCRIPTOR_SET) device->DestroyDescriptorSet(lightingDescriptorSet);
    if (skyboxDescriptorSet != handles::INVALID_DESCRIPTOR_SET) device->DestroyDescriptorSet(skyboxDescriptorSet);
    if (defaultMaterialSet != handles::INVALID_DESCRIPTOR_SET) device->DestroyDescriptorSet(defaultMaterialSet);
    if (blitDescriptorSet != handles::INVALID_DESCRIPTOR_SET) device->DestroyDescriptorSet(blitDescriptorSet);
    
    // Destroy Descriptor Set Layouts
    device->DestroyDescriptorSetLayout(globalSetLayout);
    device->DestroyDescriptorSetLayout(materialSetLayout);
    device->DestroyDescriptorSetLayout(lightingSetLayout);
    device->DestroyDescriptorSetLayout(skyboxSetLayout);
    device->DestroyDescriptorSetLayout(blitSetLayout);

    // Destroy Buffers
    device->DestroyBuffer(viewDataBuffer);
    device->DestroyBuffer(sceneDataBuffer);

    // Destroy Persistent Resources
    device->DestroyTexture(depthTexture);
    device->DestroyTexture(whiteTexture);
    device->DestroyTexture(normalTexture);
    device->DestroyTexture(shadowMap0);
    device->DestroyTexture(shadowMap1);
    device->DestroyTexture(envCubemap); // Also invalidates skyboxTexture
    
    // Destroy IBL
    iblPrecomputer.reset();
    device->DestroyTexture(irradianceMap);
    device->DestroyTexture(prefilteredMap);
    device->DestroyTexture(brdfLUT);

    // Destroy Samplers
    device->DestroySampler(defaultSampler);
    device->DestroySampler(brdfSampler);
    device->DestroySampler(debugSampler);
    
    // Destroy Loaded Scene Resources
    for (auto& meshInfo : sceneMeshes) {
        if (meshInfo.mesh) {
            meshInfo.mesh->Destroy(device);
            delete meshInfo.mesh;
            meshInfo.mesh = nullptr;
        }
    }
    sceneMeshes.clear();
    
    // Destroy Created Resources (Textures loaded from file)
    for (auto handle : createdResources) {
        device->DestroyTexture(handle);
    }
    createdResources.clear();
    
    // Destroy Shaders
    for (auto& pair : shaderVariantMap) {
        device->DestroyShader(pair.second);
    }
    shaderVariantMap.clear();

    renderGraph.reset();
    renderSystem.Shutdown();
    // device ownership handled by unique_ptr

    if (device) {
        device->GetGarbageCollector().Flush();
    }

    instance = nullptr;

    if (window.is_valid()) {
        primal::platform::remove_window(window.get_id());
    }
    
    // Cleanup Global Content Resources (GPU Meshes)
    primal::content::shutdown();
    
    std::cout << "TestGeometryDebugSponza::Shutdown End" << std::endl;
}

bool TestGeometryDebugSponza::CompileAllShaders() {
    std::cout << "Compiling Shaders..." << std::endl;
    utl::vector<std::wstring> empty_args;
    
    auto Compile = [&](const shader_file_info& info, const std::string& path) -> bool {
        auto blob = compile_shader(info, path.c_str(), empty_args);
        if (!blob) return false;
        
        u64 byte_code_size = *reinterpret_cast<u64*>(blob.get());
        u8* byte_code_ptr = blob.get() + sizeof(u64) + 16; 
        
        ShaderStage stage = ShaderStage::Unknown;
        switch (info.type) {
            case ::shader_type::vertex: stage = ShaderStage::Vertex; break;
            case ::shader_type::pixel: stage = ShaderStage::Pixel; break;
            case ::shader_type::compute: stage = ShaderStage::Compute; break;
            default: stage = ShaderStage::Unknown; break;
        }

        ShaderHandle handle = device->CreateShader(byte_code_ptr, byte_code_size, stage, info.function);
        if (handle == handles::INVALID_SHADER) return false;
        
        shaderVariantMap[std::string(info.file_name) + ":" + info.function] = handle;
        return true;
    };

    std::string testShaderPath = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/shaders/";
    std::string engineShaderPath = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/Engine/Graphics/RHI/Shaders/";

    if (!Compile(gbuffer_vs_info, testShaderPath)) return false;
    if (!Compile(gbuffer_ps_info, testShaderPath)) return false;
    if (!Compile(shadow_vs_info, testShaderPath)) return false;
    if (!Compile(lighting_vs_info, testShaderPath)) return false;
    if (!Compile(lighting_ps_info, testShaderPath)) return false;
    if (!Compile(skybox_vs_info, testShaderPath)) return false;
    if (!Compile(skybox_ps_info, testShaderPath)) return false;
    if (!Compile(blit_vs_info, testShaderPath)) return false;
    if (!Compile(blit_ps_info, testShaderPath)) return false;
    if (!Compile(eq2cube_cs_info, engineShaderPath)) return false;
    
    return true;
}

bool TestGeometryDebugSponza::SetupPipelines() {
    // 1. Global Descriptor Set Layout
    {
        DescriptorSetLayoutBinding bindings[] = {
                { 0, DescriptorType::UniformBuffer, 1, ShaderStage::Vertex | ShaderStage::Pixel },
                { 1, DescriptorType::UniformBuffer, 1, ShaderStage::Vertex | ShaderStage::Pixel }
        };
        DescriptorSetLayoutDesc desc{ .bindingCount = 2, .bindings = bindings };
        globalSetLayout = device->CreateDescriptorSetLayout(desc);
    }
    
    // 2. Lighting Descriptor Set Layout
    {
        DescriptorSetLayoutBinding bindings[] = {
                { 0, DescriptorType::UniformBuffer, 1, ShaderStage::Pixel, nullptr }, 
                { 1, DescriptorType::UniformBuffer, 1, ShaderStage::Pixel, nullptr },
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
        DescriptorSetLayoutDesc desc{ .bindingCount = 13, .bindings = bindings };
        lightingSetLayout = device->CreateDescriptorSetLayout(desc);
    }
    
    // 3. Skybox Descriptor Set Layout
    {
        DescriptorSetLayoutBinding bindings[] = {
            { 0, DescriptorType::UniformBuffer, 1, ShaderStage::Vertex, nullptr },
            { 1, DescriptorType::UniformBuffer, 1, ShaderStage::Vertex, nullptr },
            { 2, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr },
            { 3, DescriptorType::Sampler, 1, ShaderStage::Pixel, nullptr }
        };
        DescriptorSetLayoutDesc desc{ .bindingCount = 4, .bindings = bindings };
        skyboxSetLayout = device->CreateDescriptorSetLayout(desc);
    }

    // 4. GBuffer Pipeline
    {
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

        GraphicsPipelineDesc desc;
        desc.layout = gbufferLayout;
        desc.vertexShader = shaderVariantMap[std::string(gbuffer_vs_info.file_name) + ":" + gbuffer_vs_info.function];
        desc.pixelShader = shaderVariantMap[std::string(gbuffer_ps_info.file_name) + ":" + gbuffer_ps_info.function];
        desc.renderTargetFormats[0] = DataFormat::BGRA8_UNorm;
        desc.renderTargetFormats[1] = DataFormat::RGBA16_Float;
        desc.renderTargetFormats[2] = DataFormat::BGRA8_UNorm;
        desc.renderTargetFormats[3] = DataFormat::RG16_Float;
        desc.renderTargetCount = 4;
        desc.depthStencilFormat = DataFormat::D32_Float;
        desc.enableDepthTest = true;
        desc.enableDepthWrite = true;
        desc.depthFunc = ComparisonFunc::Less;
        desc.cullMode = CullMode::None;
        
        // Use Vertex Pulling, so no attributes/bindings
        desc.vertexAttributes.clear();
        desc.vertexBindings.clear();
        
        gbufferPipeline = device->CreateGraphicsPipeline(desc);
    }
    
    // 5. Lighting Pipeline
    {
        PipelineLayoutDesc plDesc{ .setLayoutCount = 1, .setLayouts = &lightingSetLayout };
        lightingLayout = device->CreatePipelineLayout(plDesc);

        GraphicsPipelineDesc desc;
        desc.layout = lightingLayout;
        desc.vertexShader = shaderVariantMap[std::string(lighting_vs_info.file_name) + ":" + lighting_vs_info.function];
        desc.pixelShader = shaderVariantMap[std::string(lighting_ps_info.file_name) + ":" + lighting_ps_info.function];
        desc.renderTargetFormats[0] = DataFormat::RGBA16_Float;
        desc.renderTargetCount = 1;
        desc.depthStencilFormat = DataFormat::Unknown;
        desc.enableDepthTest = false;
        desc.enableDepthWrite = false;
        desc.vertexAttributes.clear();
        desc.vertexBindings.clear();
        
        lightingPipeline = device->CreateGraphicsPipeline(desc);
    }
    
    // 6. Skybox Pipeline
    {
        PipelineLayoutDesc plDesc{ .setLayoutCount = 1, .setLayouts = &skyboxSetLayout };
        skyboxLayout = device->CreatePipelineLayout(plDesc);

        GraphicsPipelineDesc desc;
        desc.layout = skyboxLayout;
        desc.vertexShader = shaderVariantMap[std::string(skybox_vs_info.file_name) + ":" + skybox_vs_info.function];
        desc.pixelShader = shaderVariantMap[std::string(skybox_ps_info.file_name) + ":" + skybox_ps_info.function];
        desc.renderTargetFormats[0] = DataFormat::RGBA16_Float;
        desc.renderTargetCount = 1;
        desc.depthStencilFormat = DataFormat::D32_Float;
        desc.enableDepthTest = true;
        desc.enableDepthWrite = false;
        desc.depthFunc = ComparisonFunc::LessEqual;
        desc.cullMode = CullMode::None;
        desc.vertexAttributes.clear();
        desc.vertexBindings.clear();
        
        skyboxPipeline = device->CreateGraphicsPipeline(desc);
    }
    
    // 7. Shadow Pipeline
    {
        DescriptorSetLayoutHandle setLayous[]{ globalSetLayout, materialSetLayout };
        PushConstantRange pushConstantRanges[] = {
            { ShaderStage::Vertex, 2, sizeof(primal::math::m4x4) }
        };
        PipelineLayoutDesc plDesc{
            .setLayoutCount = 2,
            .setLayouts = setLayous,
            .pushConstantRangeCount = 1,
            .pushConstantRanges = pushConstantRanges,
        };
        shadowLayout = device->CreatePipelineLayout(plDesc);

        GraphicsPipelineDesc desc;
        desc.layout = shadowLayout;
        desc.vertexShader = shaderVariantMap[std::string(shadow_vs_info.file_name) + ":" + shadow_vs_info.function];
        desc.renderTargetCount = 0;
        desc.depthStencilFormat = DataFormat::D32_Float;
        desc.enableDepthTest = true;
        desc.enableDepthWrite = true;
        desc.depthFunc = ComparisonFunc::Less;
        desc.cullMode = CullMode::None;
        desc.vertexAttributes.clear();
        desc.vertexBindings.clear();
        
        shadowPipeline = device->CreateGraphicsPipeline(desc);
    }

    // 8. Blit Pipeline
    {
        // Blit Set Layout
        DescriptorSetLayoutBinding bindings[] = {
            { 0, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr }
        };
        DescriptorSetLayoutDesc setDesc{ .bindingCount = 1, .bindings = bindings };
        blitSetLayout = device->CreateDescriptorSetLayout(setDesc);

        // Pipeline Layout
        PipelineLayoutDesc plDesc{ .setLayoutCount = 1, .setLayouts = &blitSetLayout };
        blitLayout = device->CreatePipelineLayout(plDesc);

        // Graphics Pipeline
        GraphicsPipelineDesc desc;
        desc.layout = blitLayout;
        desc.vertexShader = shaderVariantMap[std::string(blit_vs_info.file_name) + ":" + blit_vs_info.function];
        desc.pixelShader = shaderVariantMap[std::string(blit_ps_info.file_name) + ":" + blit_ps_info.function];
        desc.renderTargetFormats[0] = DataFormat::BGRA8_UNorm; // Usually SwapChain format
        desc.renderTargetCount = 1;
        desc.depthStencilFormat = DataFormat::Unknown;
        desc.enableDepthTest = false;
        desc.enableDepthWrite = false;
        desc.cullMode = CullMode::None;
        desc.vertexAttributes.clear();
        desc.vertexBindings.clear();
        
        blitPipeline = device->CreateGraphicsPipeline(desc);
    }

    // Initialize Debug Settings
    // Debug settings are handled in Initialize()
    // debugSettings.enable = false; 

    return true; 
}

void CompareModelFiles(primal::graphics::rhi::RHIDeviceBase* device) {
    using namespace primal::graphics;
    std::cout << "\n========== Model Analysis Start ==========\n";

    std::string baseDir = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/assets/";
    auto getPath = [&](const std::string& name) {
        std::string p = baseDir + name + ".model";
        { std::ifstream f(p); if (f.good()) return p; }
        p = baseDir + name + ".bin";
        { std::ifstream f(p); if (f.good()) return p; }
        return std::string("");
    };

    std::string pathA = getPath("Sponza");
    std::string pathB = getPath("Sponza_process_rebuild");

    auto LoadFile = [](const std::string& path) -> std::vector<uint8_t> {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) return {};
        std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);
        std::vector<uint8_t> buffer(size);
        file.read((char*)buffer.data(), size);
        return buffer;
    };

    std::vector<uint8_t> dataA = LoadFile(pathA);
    std::vector<uint8_t> dataB = LoadFile(pathB);

    std::cout << "File A: " << pathA << " (Size: " << dataA.size() << ")\n";
    std::cout << "File B: " << pathB << " (Size: " << dataB.size() << ")\n";

    // Binary Comparison
    if (dataA.empty() || dataB.empty()) {
        std::cout << "One or both files are empty/missing.\n";
    } else {
        size_t minSize = std::min(dataA.size(), dataB.size());
        size_t diffOffset = minSize;
        for (size_t i = 0; i < minSize; ++i) {
            if (dataA[i] != dataB[i]) {
                diffOffset = i;
                break;
            }
        }

        if (diffOffset == minSize) {
            if (dataA.size() == dataB.size()) {
                std::cout << "Files are IDENTICAL.\n";
            } else {
                std::cout << "Files match up to size " << minSize << ", but one is larger.\n";
            }
        } else {
            std::cout << "Files differ at offset " << diffOffset << " (0x" << std::hex << diffOffset << std::dec << ")\n";
            std::cout << "  A: 0x" << std::hex << (int)dataA[diffOffset] << "\n";
            std::cout << "  B: 0x" << std::hex << (int)dataB[diffOffset] << "\n";
            std::cout << std::dec;
        }
    }

    // Logical Comparison via SceneDataAdapter
    auto LoadMeshes = [&](const std::vector<uint8_t>& data) -> std::vector<SceneDataMeshInfo> {
        if (data.empty()) return {};
        SceneDataAdapter adapter;
        return adapter.LoadRenderItemData(device, data.data(), (uint32_t)data.size());
    };

    std::cout << "\nParsing with SceneDataAdapter...\n";
    auto meshesA = LoadMeshes(dataA);
    auto meshesB = LoadMeshes(dataB);

    std::cout << "Mesh Count A: " << meshesA.size() << "\n";
    std::cout << "Mesh Count B: " << meshesB.size() << "\n";

    if (!meshesA.empty() && !meshesB.empty()) {
        // Compare first mesh
        auto mA = meshesA[0].mesh;
        auto mB = meshesB[0].mesh;
        std::cout << "Mesh 0 Comparison:\n";
        std::cout << "  Verts: A=" << mA->GetVertexCount() << ", B=" << mB->GetVertexCount() << "\n";
        std::cout << "  Stride: A=" << mA->GetVertexStride() << ", B=" << mB->GetVertexStride() << "\n";
    }

    std::cout << "========== Model Analysis End ==========\n\n";
}

bool TestGeometryDebugSponza::LoadScene() {
    std::string baseDir = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/assets/";
    
    // Helper to load model data
    auto TryLoad = [&](const std::string& path) -> bool {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) return false;
        std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);
        std::vector<char> buffer(size);
        if (!file.read(buffer.data(), size)) return false;
        
        primal::graphics::SceneDataAdapter adapter;
        sceneMeshes = adapter.LoadRenderItemData(device, buffer.data(), (uint32_t)buffer.size());
        return !sceneMeshes.empty();
    };

    // Priority 1: Sponza.model (Matches TestSponzaRenderGraph.cpp)
    std::string modelPath = baseDir + "Sponza_process_rebuild.model";
    if (TryLoad(modelPath)) {
        std::cout << "Successfully loaded Sponza_process_rebuild.model with " << sceneMeshes.size() << " meshes." << std::endl;
        
        // Pre-create RHIGpuMesh objects for debug visualization
        int gpuMeshCount = 0;
        for (const auto& meshInfo : sceneMeshes) {
            if (meshInfo.meshEntityId != primal::id::invalid_id) {
                auto* gpuMesh = content::get_rhi_gpu_mesh(meshInfo.meshEntityId);
                if (gpuMesh) {
                    gpuMeshCount++;
                }
            }
        }
        std::cout << "Pre-created " << gpuMeshCount << " RHIGpuMesh objects for debug visualization." << std::endl;
    } else {
        // Priority 2: Sponza_process_rebuild.model
        std::cout << "Failed to load Sponza.model (or 0 meshes), trying Sponza_process_rebuild.model..." << std::endl;
        modelPath = baseDir + "Sponza_process_rebuild.model";
        if (!TryLoad(modelPath)) {
             // Priority 3: Sponza_process_rebuild.bin
             modelPath = baseDir + "Sponza_process_rebuild.bin";
             if (!TryLoad(modelPath)) {
                 std::cerr << "Failed to load any scene model." << std::endl;
                 return false;
             }
        }
        std::cout << "Successfully loaded Sponza_process_rebuild.model with " << sceneMeshes.size() << " meshes." << std::endl;
        
        // Pre-create RHIGpuMesh objects for debug visualization
        int gpuMeshCount = 0;
        for (const auto& meshInfo : sceneMeshes) {
            if (meshInfo.meshEntityId != primal::id::invalid_id) {
                auto* gpuMesh = content::get_rhi_gpu_mesh(meshInfo.meshEntityId);
                if (gpuMesh) {
                    gpuMeshCount++;
                }
            }
        }
        std::cout << "Pre-created " << gpuMeshCount << " RHIGpuMesh objects for debug visualization." << std::endl;
    }

    // DEBUG: Print Vertex Stride
    if (!sceneMeshes.empty() && sceneMeshes[0].mesh) {
        std::cout << "DEBUG: First Mesh Vertex Stride: " << sceneMeshes[0].mesh->GetVertexStride() << " bytes." << std::endl;
        std::cout << "DEBUG: Expected Stride (Packed): 32 bytes." << std::endl;
    }

    std::string assetBaseDir = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/assets/models/Sponza/";
    
    for (auto& meshInfo : sceneMeshes) {
        if (meshInfo.materialInstance) {
            auto material = meshInfo.materialInstance->GetMaterial();
            if (material) material->SetDescriptorSetLayout(materialSetLayout);
            
            meshInfo.materialInstance->Initialize(device);
            
            ResourceHandle diffuseTex = whiteTexture;
            if (!meshInfo.diffuseTexturePath.empty()) {
                std::string fullPath = ResolveTexturePath(assetBaseDir, meshInfo.diffuseTexturePath);
                std::ifstream f(fullPath.c_str());
                if (f.good()) {
                    ResourceHandle tex = LoadTextureFromFile(device, fullPath, false, true);
                    if (tex != handles::INVALID_RESOURCE) {
                        diffuseTex = tex;
                        createdResources.push_back(tex);
                    }
                }
            }
            
            ResourceHandle normalTex = normalTexture;
            if (!meshInfo.normalTexturePath.empty()) {
                std::string fullPath = ResolveTexturePath(assetBaseDir, meshInfo.normalTexturePath);
                std::ifstream f(fullPath.c_str());
                if (f.good()) {
                    ResourceHandle tex = LoadTextureFromFile(device, fullPath, true, false);
                    if (tex != handles::INVALID_RESOURCE) {
                        normalTex = tex;
                        createdResources.push_back(tex);
                    }
                }
            }
            
            // ORM Texture Loading
            ResourceHandle ormTex = whiteTexture; 
            if (!meshInfo.ormTexturePath.empty()) {
                std::string fullPath = ResolveTexturePath(assetBaseDir, meshInfo.ormTexturePath);
                std::ifstream f(fullPath.c_str());
                if (f.good()) {
                    ResourceHandle tex = LoadTextureFromFile(device, fullPath, false, false);
                    if (tex != handles::INVALID_RESOURCE) {
                        ormTex = tex;
                        createdResources.push_back(tex);
                    }
                }
            }

            for (uint32_t i = 0; i < 3; ++i) {
                meshInfo.materialInstance->SetCurrentFrame(i);
                meshInfo.materialInstance->SetTexture(0, diffuseTex);
                meshInfo.materialInstance->SetTexture(1, normalTex);
                meshInfo.materialInstance->SetTexture(2, ormTex);
                // Sampler binding 3
                meshInfo.materialInstance->SetSampler(3, defaultSampler);
                meshInfo.materialInstance->Update(device);
            }
        }
        
        primal::graphics::RenderProxy proxy = primal::graphics::RenderProxy::Create(
            primal::id::invalid_id, primal::id::invalid_id, primal::id::invalid_id);
        proxy.transform = primal::graphics::rhi::math::MatrixIdentity();
        scene.AddProxy(proxy);
    }
    
    return true; 
}

bool TestGeometryDebugSponza::SetupIBL() {
    iblPrecomputer = std::make_unique<primal::graphics::rhi::IBLPrecomputer>(device);
    if (!iblPrecomputer->Initialize()) return false;

    TextureDesc cubeDesc{
        .size = { 2048, 2048, 1 },
        .arraySize = 1,
        .type = TextureType::TextureCube,
        .format = DataFormat::RGBA8_UNorm,
        .usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess | TextureUsage::CopyDest,
    };
    envCubemap = device->CreateTexture(cubeDesc);
    skyboxTexture = envCubemap;

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
        std::vector<uint8_t> faceData(2048 * 2048 * 4);
        FaceColor color = faceColors[i];
        for (size_t j = 0; j < faceData.size(); j += 4) {
            faceData[j] = color.r;
            faceData[j+1] = color.g;
            faceData[j+2] = color.b;
            faceData[j+3] = color.a;
        }
        WriteTexture(device, envCubemap, faceData.data(), faceData.size(), 2048, 2048, i);
    }

    irradianceMap = iblPrecomputer->ComputeIrradianceMap(envCubemap);
    prefilteredMap = iblPrecomputer->ComputePrefilteredEnvironmentMap(envCubemap);
    brdfLUT = iblPrecomputer->ComputeBRDFIntegrationMap();

    return true;
}

void TestGeometryDebugSponza::UpdateScene() {
    // Calculate Projection Matrix
    float fov = 60.0f * primal::graphics::rhi::math::constants::DEG_TO_RAD;
    float aspect = (float)renderWidth / (float)renderHeight;
    primal::graphics::rhi::math::m4x4 proj = primal::graphics::rhi::math::CreatePerspectiveMatrix(fov, aspect, 0.1f, 1000.0f);
    
    // Calculate View Matrix
    primal::graphics::rhi::math::m4x4 viewMat = m_camera.GetViewMatrix();

    // Update Camera Uniforms
    ViewData viewData;
    viewData.viewProjection = proj * viewMat;
    viewData.invViewProjection = primal::graphics::rhi::math::Inverse(viewData.viewProjection);
    viewData.previousViewProjection = viewData.viewProjection;
    
    void* mapped = device->MapBuffer(viewDataBuffer);
    memcpy(mapped, &viewData, sizeof(ViewData));
    device->UnmapBuffer(viewDataBuffer);
    
    // --- Light & Shadow Setup ---
    primal::math::v4 lightPos = {100.0f, 150.0f, 50.0f, 0.0f}; 
    primal::math::v3 lightDir = primal::graphics::rhi::math::Normalize(primal::math::v3{-lightPos.x, -lightPos.y, -lightPos.z}); 
    primal::math::v3 lightUp = {0.0f, 1.0f, 0.0f};
    if (abs(lightDir.y) > 0.9f) lightUp = {1.0f, 0.0f, 0.0f};
    
    // Cascade 0
    primal::math::v3 lightEye0 = {-lightDir.x * 200.0f, -lightDir.y * 200.0f, -lightDir.z * 200.0f};
    primal::math::m4x4 lightView0 = primal::graphics::rhi::math::CreateLookAtMatrix(lightEye0, {0,0,0}, lightUp);
    primal::math::m4x4 lightProj0 = primal::graphics::rhi::math::CreateOrthographicMatrix(-30.0f, 30.0f, 30.0f, -30.0f, 0.1f, 1000.0f);
    
    lightVP0 = lightProj0 * lightView0;
    
    // Cascade 1 (Far)
    primal::math::m4x4 lightView1 = lightView0;
    primal::math::m4x4 lightProj1 = primal::graphics::rhi::math::CreateOrthographicMatrix(-500.0f, 500.0f, 500.0f, -500.0f, 0.1f, 5000.0f);
    lightVP1 = lightProj1 * lightView1;

    // Update Scene Uniforms
    SceneData sceneData{};
    sceneData.lightPos = lightPos;
    sceneData.lightColor = { 20.0f, 20.0f, 20.0f, 1.0f };
    sceneData.viewPos = { m_camera.GetPosition().x, m_camera.GetPosition().y, m_camera.GetPosition().z, 1.0f };
    sceneData.shadowMatrix0 = lightVP0;
    sceneData.shadowMatrix1 = lightVP1;
    sceneData.model = primal::graphics::rhi::math::MatrixIdentity();
    sceneData.previousModel = primal::graphics::rhi::math::MatrixIdentity();
    
    mapped = device->MapBuffer(sceneDataBuffer);
    memcpy(mapped, &sceneData, sizeof(SceneData));
    device->UnmapBuffer(sceneDataBuffer);
    
    // Update View
    view.SetViewMatrix(viewMat);
    view.SetProjectionMatrix(proj);
    view.SetViewport({ { 0, 0 }, { (float)renderWidth, (float)renderHeight }, 0, 1});
    view.SetScissor({ { 0, 0 }, { renderWidth, renderHeight } });
    view.UpdateFrustum();
}

bool TestGeometryDebugSponza::CreateUniformBuffers() {
    BufferDesc desc{};
    desc.size = sizeof(ViewData);
    desc.type = BufferType::Constant;
    desc.usage = GPUMemoryUsage::Dynamic;
    viewDataBuffer = device->CreateBuffer(desc);
    
    desc.size = sizeof(SceneData);
    sceneDataBuffer = device->CreateBuffer(desc);
    
    // DEBUG: Log buffer sizes
    std::cout << "[DEBUG] ViewData size: " << sizeof(ViewData) << " bytes" << std::endl;
    std::cout << "[DEBUG] SceneData size: " << sizeof(SceneData) << " bytes" << std::endl;
    std::cout << "[DEBUG] viewDataBuffer: " << viewDataBuffer << std::endl;
    std::cout << "[DEBUG] sceneDataBuffer: " << sceneDataBuffer << std::endl;
    
    return (viewDataBuffer != handles::INVALID_RESOURCE && sceneDataBuffer != handles::INVALID_RESOURCE);
}

bool TestGeometryDebugSponza::CreateDescriptorSets() {
    // Global Set
    DescriptorSetDesc desc;
    desc.layout = globalSetLayout;
    globalDescriptorSet = device->CreateDescriptorSet(desc);
    
    DescriptorData params[2]{
        { .binding = 0, .type = DescriptorType::UniformBuffer, .resource = viewDataBuffer },
        { .binding = 1, .type = DescriptorType::UniformBuffer, .resource = sceneDataBuffer },
    };
    UpdateDescriptorSet(device, globalDescriptorSet, params, 2);
    
    // Lighting Set
    desc.layout = lightingSetLayout;
    lightingDescriptorSet = device->CreateDescriptorSet(desc);
    
    // Initial bindings for lighting
    DescriptorData lightingParams[13]{
        { .binding = 0, .type = DescriptorType::UniformBuffer, .resource = viewDataBuffer },
        { .binding = 1, .type = DescriptorType::UniformBuffer, .resource = sceneDataBuffer },
        // Textures will be bound in RenderPass, but need valid handles for initial update if any
        { .binding = 2, .type = DescriptorType::SampledImage, .resource = whiteTexture }, // Placeholder
        { .binding = 3, .type = DescriptorType::SampledImage, .resource = whiteTexture },
        { .binding = 4, .type = DescriptorType::SampledImage, .resource = whiteTexture },
        { .binding = 5, .type = DescriptorType::SampledImage, .resource = whiteTexture },
        { .binding = 6, .type = DescriptorType::SampledImage, .resource = whiteTexture },
        { .binding = 7, .type = DescriptorType::SampledImage, .resource = whiteTexture },
        { .binding = 8, .type = DescriptorType::SampledImage, .resource = irradianceMap },
        { .binding = 9, .type = DescriptorType::SampledImage, .resource = prefilteredMap },
        { .binding = 10, .type = DescriptorType::SampledImage, .resource = brdfLUT },
        { .binding = 11, .type = DescriptorType::Sampler, .resource = defaultSampler },
        { .binding = 12, .type = DescriptorType::Sampler, .resource = brdfSampler },
    };
    UpdateDescriptorSet(device, lightingDescriptorSet, lightingParams, 13);
    
    // Skybox Set
    desc.layout = skyboxSetLayout;
    skyboxDescriptorSet = device->CreateDescriptorSet(desc);
    {
        DescriptorData skyboxParams[4]{
            { .binding = 0, .type = DescriptorType::UniformBuffer, .resource = viewDataBuffer },
            { .binding = 1, .type = DescriptorType::UniformBuffer, .resource = sceneDataBuffer },
            { .binding = 2, .type = DescriptorType::SampledImage, .resource = skyboxTexture },
            { .binding = 3, .type = DescriptorType::Sampler, .resource = defaultSampler }, // Re-using default sampler
        };
        UpdateDescriptorSet(device, skyboxDescriptorSet, skyboxParams, 4);
    }
    
    // Blit Set
    {
        DescriptorSetDesc blitDesc;
        blitDesc.layout = blitSetLayout;
        blitDescriptorSet = device->CreateDescriptorSet(blitDesc);
        
        DescriptorData blitParams[1]{
            { .binding = 0, .type = DescriptorType::SampledImage, .resource = whiteTexture } // Placeholder
        };
        UpdateDescriptorSet(device, blitDescriptorSet, blitParams, 1);
    }

    return true;
}

bool TestGeometryDebugSponza::CreatePersistentResources() {
    // Create Depth Texture
    TextureDesc depthDesc;
    depthDesc.size = { renderWidth, renderHeight, 1 };
    depthDesc.format = DataFormat::D32_Float;
    depthDesc.usage = TextureUsage::DepthStencil | TextureUsage::ShaderResource;
    depthTexture = device->CreateTexture(depthDesc);
    
    // Create Default Textures
    TextureDesc whiteDesc{ .size = { 1, 1, 1 }, .format = DataFormat::RGBA8_UNorm, .usage = TextureUsage::ShaderResource | TextureUsage::CopyDest };
    whiteTexture = device->CreateTexture(whiteDesc);
    uint32_t whiteData = 0xFFFFFFFF;
    WriteTexture(device, whiteTexture, &whiteData, 4, 1, 1, 0);

    TextureDesc normalDesc = whiteDesc;
    normalTexture = device->CreateTexture(normalDesc);
    uint32_t normalData = 0xFFFF8080;
    WriteTexture(device, normalTexture, &normalData, 4, 1, 1, 0);

    // Create Shadow Maps
    TextureDesc shadowDesc{ .size = { 2048, 2048, 1 }, .format = DataFormat::D32_Float, .usage = TextureUsage::DepthStencil | TextureUsage::ShaderResource };
    shadowMap0 = device->CreateTexture(shadowDesc);
    shadowMap1 = device->CreateTexture(shadowDesc);
    
    return true;
}

void TestGeometryDebugSponza::BuildRenderGraph(RenderGraph& graph, ResourceHandle backBuffer) {
    // Import Resources
    RGResourceHandle depth = graph.ImportResource("Depth", depthTexture);
    RGResourceHandle finalOutput = graph.ImportResource("BackBuffer", backBuffer);
    RGResourceHandle sm0 = graph.ImportResource("ShadowMap0", shadowMap0);
    RGResourceHandle sm1 = graph.ImportResource("ShadowMap1", shadowMap1);
    
    // 1. Shadow Pass
    graph.AddPass<ShadowPassData>("ShadowPass", RGPassType::Graphics, RGPassCategory::Depth,
        [&](ShadowPassData& data, RenderGraphBuilder& builder) {
            data.shadowMap = builder.Write(sm0, ResourceState::DepthStencil);
            RGRenderPassDesc rpDesc;
            rpDesc.depthStencil.texture = data.shadowMap;
            rpDesc.depthStencil.depthLoadOp = LoadAction::Clear;
            rpDesc.depthStencil.depthStoreOp = StoreAction::Store;
            rpDesc.depthStencil.clearDepth = 1.0f;
            builder.DeclareRenderPass(rpDesc);
        },
        [&](const ShadowPassData& data, RenderGraphContext& context) {
            auto cmd = context.cmdBuffer;
            cmd->BindGraphicsPipeline(shadowPipeline);
            cmd->SetViewport({ { 0, 0 }, { (float)2048, (float)2048 }, 0, 1});
            cmd->SetScissor({ { 0, 0 }, { 2048, 2048 } });
            
            const DescriptorSetHandle descriptorSets[] = { globalDescriptorSet };
            cmd->BindDescriptorSets(PipelineBindPoint::Graphics, shadowLayout, 0, 1, descriptorSets, 0, nullptr);

            for (const auto& meshInfo : sceneMeshes) {
                if (!meshInfo.mesh) continue;

                DescriptorSetHandle matSet = defaultMaterialSet;
                if (meshInfo.materialInstance) {
                    meshInfo.materialInstance->SetCurrentFrame(renderSystem.GetCurrentFrameIndex());
                    matSet = meshInfo.materialInstance->GetDescriptorSet();
                }
                if (matSet != handles::INVALID_DESCRIPTOR_SET) {
                    const DescriptorSetHandle matSets[] = { matSet };
                    cmd->BindDescriptorSets(PipelineBindPoint::Graphics, shadowLayout, 1, 1, matSets, 0, nullptr);
                }

                // Currently using Identity as we don't load node transforms yet
                primal::math::m4x4 model = primal::graphics::rhi::math::MatrixIdentity();
                primal::math::m4x4 mvp = lightVP0 * model;
                cmd->PushConstants(shadowLayout, ShaderStage::Vertex, 2, sizeof(mvp), &mvp);
                
                // Draw Mesh
                meshInfo.mesh->Draw(cmd, 1, 0, 20);
            }
        }
    );

    // 1.5 Shadow Pass 1 (Cascade 1)
    graph.AddPass<ShadowPassData>("ShadowPass1", RGPassType::Graphics, RGPassCategory::Depth,
        [&](ShadowPassData& data, RenderGraphBuilder& builder) {
            data.shadowMap = builder.Write(sm1, ResourceState::DepthStencil);
            RGRenderPassDesc rpDesc;
            rpDesc.depthStencil.texture = data.shadowMap;
            rpDesc.depthStencil.depthLoadOp = LoadAction::Clear;
            rpDesc.depthStencil.depthStoreOp = StoreAction::Store;
            rpDesc.depthStencil.clearDepth = 1.0f;
            builder.DeclareRenderPass(rpDesc);
        },
        [&](const ShadowPassData& data, RenderGraphContext& context) {
            auto cmd = context.cmdBuffer;
            cmd->BindGraphicsPipeline(shadowPipeline);
            cmd->SetViewport({ { 0, 0 }, { (float)2048, (float)2048 }, 0, 1});
            cmd->SetScissor({ { 0, 0 }, { 2048, 2048 } });
            
            const DescriptorSetHandle descriptorSets[] = { globalDescriptorSet };
            cmd->BindDescriptorSets(PipelineBindPoint::Graphics, shadowLayout, 0, 1, descriptorSets, 0, nullptr);

            for (const auto& meshInfo : sceneMeshes) {
                if (!meshInfo.mesh) continue;

                DescriptorSetHandle matSet = defaultMaterialSet;
                if (meshInfo.materialInstance) {
                    meshInfo.materialInstance->SetCurrentFrame(renderSystem.GetCurrentFrameIndex());
                    matSet = meshInfo.materialInstance->GetDescriptorSet();
                }
                if (matSet != handles::INVALID_DESCRIPTOR_SET) {
                    const DescriptorSetHandle matSets[] = { matSet };
                    cmd->BindDescriptorSets(PipelineBindPoint::Graphics, shadowLayout, 1, 1, matSets, 0, nullptr);
                }

                primal::math::m4x4 model = primal::graphics::rhi::math::MatrixIdentity();
                primal::math::m4x4 mvp = lightVP1 * model;
                cmd->PushConstants(shadowLayout, ShaderStage::Vertex, 2, sizeof(mvp), &mvp);
                
                meshInfo.mesh->Draw(cmd, 1, 0, 20);
            }
        }
    );
    
    // 2. Main GBuffer Pass
    TextureDesc mainDesc{ .size = { renderWidth, renderHeight, 1 }, .format = DataFormat::BGRA8_UNorm, .usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource };
    RGResourceHandle albedo = graph.CreateTexture("Albedo", mainDesc);
    
    mainDesc.format = DataFormat::RGBA16_Float;
    RGResourceHandle normal = graph.CreateTexture("Normal", mainDesc);
    
    mainDesc.format = DataFormat::BGRA8_UNorm;
    RGResourceHandle orm = graph.CreateTexture("ORM", mainDesc);
    
    mainDesc.format = DataFormat::RG16_Float;
    RGResourceHandle velocity = graph.CreateTexture("Velocity", mainDesc);

    graph.AddPass<MainPassData>("GBufferPass", RGPassType::Graphics, RGPassCategory::Main,
        [&](MainPassData& data, RenderGraphBuilder& builder) {
            data.albedo = builder.Write(albedo, ResourceState::RenderTarget);
            data.normal = builder.Write(normal, ResourceState::RenderTarget);
            data.orm = builder.Write(orm, ResourceState::RenderTarget);
            data.velocity = builder.Write(velocity, ResourceState::RenderTarget);
            data.depth = builder.Write(depth, ResourceState::DepthStencil);
            
            RGRenderPassDesc rpDesc;
            rpDesc.colors.resize(4);
            rpDesc.colors[0] = RGAttachmentDesc{ .texture = data.albedo, .loadOp = LoadAction::Clear, .storeOp = StoreAction::Store, .clearColor = { primal::math::v4{0,0,0,0} } };
            rpDesc.colors[1] = RGAttachmentDesc{ .texture = data.normal, .loadOp = LoadAction::Clear, .storeOp = StoreAction::Store, .clearColor = { primal::math::v4{0,0,0,0} } };
            rpDesc.colors[2] = RGAttachmentDesc{ .texture = data.orm, .loadOp = LoadAction::Clear, .storeOp = StoreAction::Store, .clearColor = { primal::math::v4{1.0f,1.0f,1.0f,1.0f} } };
            rpDesc.colors[3] = RGAttachmentDesc{ .texture = data.velocity, .loadOp = LoadAction::Clear, .storeOp = StoreAction::Store, .clearColor = { primal::math::v4{0,0,0,0} } };
            rpDesc.depthStencil = RGAttachmentDesc{ .texture = data.depth, .depthLoadOp = LoadAction::Clear, .depthStoreOp = StoreAction::Store, .clearDepth = 1.0f };
            
            builder.DeclareRenderPass(rpDesc);
        },
        [&](const MainPassData&, RenderGraphContext& context) {
            auto cmd = context.cmdBuffer;
            cmd->BindGraphicsPipeline(gbufferPipeline);
            cmd->SetViewport({ { 0, 0 }, { (float)renderWidth, (float)renderHeight }, 0, 1});
            cmd->SetScissor({ { 0, 0 }, { renderWidth, renderHeight } });

            const DescriptorSetHandle descriptorSets[] = { globalDescriptorSet };
            cmd->BindDescriptorSets(PipelineBindPoint::Graphics, gbufferLayout, 0, 1, descriptorSets, 0, nullptr);

            for (const auto& meshInfo : sceneMeshes) {
                if (!meshInfo.mesh) continue;
                
                DescriptorSetHandle matSet = defaultMaterialSet;
                if (meshInfo.materialInstance) {
                    meshInfo.materialInstance->SetCurrentFrame(renderSystem.GetCurrentFrameIndex());
                    matSet = meshInfo.materialInstance->GetDescriptorSet();
                }
                if (matSet != handles::INVALID_DESCRIPTOR_SET) {
                    const DescriptorSetHandle matSets[] = { matSet };
                    cmd->BindDescriptorSets(PipelineBindPoint::Graphics, gbufferLayout, 1, 1, matSets, 0, nullptr);
                }

                primal::math::m4x4 model = primal::graphics::rhi::math::MatrixIdentity();
                cmd->PushConstants(gbufferLayout, ShaderStage::Vertex, 2, sizeof(model), &model);
                
                meshInfo.mesh->Draw(cmd, 1, 0, 20);
            }
        }
    );
    
    // 3. Lighting Output Texture (Created Early for Reordering)
    RGResourceHandle lightingOutput = graph.CreateTexture("LightingOutput", { .size = { renderWidth, renderHeight, 1 }, .format = DataFormat::RGBA16_Float, .usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource | TextureUsage::CopySource });
    
    // 3. Skybox Pass (Moved before Lighting Pass)
    const auto& skyboxData = graph.AddPass<CubemapPassData>("SkyboxPass", RGPassType::Graphics, RGPassCategory::Lighting,
        [&](CubemapPassData& data, RenderGraphBuilder& builder) {
            data.output = builder.Write(lightingOutput, ResourceState::RenderTarget);
            // We need to load the depth buffer to test against it (Skybox is at z=1.0)
            // Using Write to ensure it's transitioned to DepthStencil attachment state
            builder.Write(depth, ResourceState::DepthStencil); 
            
            RGRenderPassDesc rpDesc;
            rpDesc.colors.push_back({ .texture = data.output, .loadOp = LoadAction::Clear, .storeOp = StoreAction::Store, .clearColor = { primal::math::v4{0,0,0,0} } });
            rpDesc.depthStencil = { .texture = depth, .depthLoadOp = LoadAction::Load, .depthStoreOp = StoreAction::Store };
            builder.DeclareRenderPass(rpDesc);
        },
        [&](const CubemapPassData& data, RenderGraphContext& context) {
            auto cmd = context.cmdBuffer;
            cmd->BindGraphicsPipeline(skyboxPipeline);
            cmd->SetViewport({ { 0, 0 }, { (float)renderWidth, (float)renderHeight }, 0, 1});
            cmd->SetScissor({ { 0, 0 }, { renderWidth, renderHeight } });
            
            const DescriptorSetHandle skyboxSets[] = { skyboxDescriptorSet };
            cmd->BindDescriptorSets(PipelineBindPoint::Graphics, skyboxLayout, 0, 1, skyboxSets, 0, nullptr);
            
            cmd->Draw(36, 0, 1, 0);
        }
    );

    // 4. Lighting Pass
    graph.AddPass<LightingPassData>("LightingPass", RGPassType::Graphics, RGPassCategory::Lighting,
        [&](LightingPassData& data, RenderGraphBuilder& builder) {
            data.albedo = builder.Read(albedo);
            data.normal = builder.Read(normal);
            data.orm = builder.Read(orm);
            data.depth = builder.Read(depth);
            data.shadowMap0 = builder.Read(sm0);
            data.shadowMap1 = builder.Read(sm1);
            data.output = builder.Write(lightingOutput, ResourceState::RenderTarget);
            
            RGRenderPassDesc rpDesc;
            // Load Skybox Result
            rpDesc.colors.push_back(RGAttachmentDesc{ .texture = data.output, .loadOp = LoadAction::Load, .storeOp = StoreAction::Store });
            builder.DeclareRenderPass(rpDesc);
        },
        [&](const LightingPassData& data, RenderGraphContext& context) {
            DescriptorData params[13] = {
                { .binding = 0, .type = DescriptorType::UniformBuffer, .resource = viewDataBuffer },
                { .binding = 1, .type = DescriptorType::UniformBuffer, .resource = sceneDataBuffer },
                { .binding = 2, .type = DescriptorType::SampledImage, .resource = context.graph->GetResource(data.albedo)->GetPhysicalHandle() },
                { .binding = 3, .type = DescriptorType::SampledImage, .resource = context.graph->GetResource(data.normal)->GetPhysicalHandle() },
                { .binding = 4, .type = DescriptorType::SampledImage, .resource = context.graph->GetResource(data.orm)->GetPhysicalHandle() },
                { .binding = 5, .type = DescriptorType::SampledImage, .resource = context.graph->GetResource(data.depth)->GetPhysicalHandle() },
                { .binding = 6, .type = DescriptorType::SampledImage, .resource = context.graph->GetResource(data.shadowMap0)->GetPhysicalHandle() },
                { .binding = 7, .type = DescriptorType::SampledImage, .resource = context.graph->GetResource(data.shadowMap1)->GetPhysicalHandle() },
                { .binding = 8, .type = DescriptorType::SampledImage, .resource = irradianceMap },
                { .binding = 9, .type = DescriptorType::SampledImage, .resource = prefilteredMap },
                { .binding = 10, .type = DescriptorType::SampledImage, .resource = brdfLUT },
                { .binding = 11, .type = DescriptorType::Sampler, .resource = defaultSampler },
                { .binding = 12, .type = DescriptorType::Sampler, .resource = brdfSampler },
            };
            UpdateDescriptorSet(device, lightingDescriptorSet, params, 13);

            auto cmd = context.cmdBuffer;
            cmd->BindGraphicsPipeline(lightingPipeline);
            cmd->SetViewport({ { 0, 0 }, { (float)renderWidth, (float)renderHeight }, 0, 1});
            cmd->SetScissor({ { 0, 0 }, { renderWidth, renderHeight } });
            const DescriptorSetHandle sets[] = { lightingDescriptorSet };
            cmd->BindDescriptorSets(PipelineBindPoint::Graphics, lightingLayout, 0, 1, sets, 0, nullptr);
            cmd->Draw(3, 0, 1, 0); // Fullscreen triangle
        }
    );
    
    // 5. Final Blit and Debug
    RGResourceHandle presentSource = lightingOutput; // skyboxData.output;
    
    // Always add debug pass - it will check settings internally
    if (debugSettings.enable)
    {
        const auto& debugData = AddGeometryDebugPass(graph, presentSource, depth, view, &debugSettings);  // Pass pointer!
        presentSource = debugData.target;
    }
    
    // Blit to Backbuffer
    graph.AddPass<BlitData>("FinalBlit", RGPassType::Graphics, RGPassCategory::PostProcess,
        [&](BlitData& data, RenderGraphBuilder& builder) {
            data.input = builder.Read(presentSource);
            data.output = builder.Write(finalOutput, ResourceState::RenderTarget);
            
            RGRenderPassDesc rpDesc;
            rpDesc.colors.push_back({ .texture = data.output, .loadOp = LoadAction::Clear, .storeOp = StoreAction::Store, .clearColor = { primal::math::v4{0,0,0,1} } });
            builder.DeclareRenderPass(rpDesc);
        },
        [&](const BlitData& data, RenderGraphContext& context) {
            auto cmd = context.cmdBuffer;
            
            DescriptorData params[1] = {
                { .binding = 0, .type = DescriptorType::SampledImage, .resource = context.graph->GetResource(data.input)->GetPhysicalHandle() }
            };
            UpdateDescriptorSet(device, blitDescriptorSet, params, 1);
            
            cmd->BindGraphicsPipeline(blitPipeline);
            cmd->SetViewport({ { 0, 0 }, { (float)renderWidth, (float)renderHeight }, 0, 1});
            cmd->SetScissor({ { 0, 0 }, { renderWidth, renderHeight } });
            
            const DescriptorSetHandle sets[] = { blitDescriptorSet };
            cmd->BindDescriptorSets(PipelineBindPoint::Graphics, blitLayout, 0, 1, sets, 0, nullptr);
            
            cmd->Draw(3, 0, 1, 0);
        }
    );
}
