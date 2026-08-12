#include "TestParticleSponza.h"
#include "Engine/Content/ContentToEngine.h"
#include "Engine/Content/AsyncResourceLoader.h"
#include "Engine/JobSystem/JobSystem.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalDevice.h"
#include "Engine/Graphics/RenderGraph/RenderGraphBuilder.h"
#include "Engine/Graphics/RenderGraph/RenderGraphDefinitions.h"
#include "Engine/Graphics/Lighting/LightProbeManager.h"
#include "Engine/Input/Input.h"
#include "ShaderCompilation.h"

#include "stb_image.h"  // third_party/stb submodule

#include <iostream>
#include <fstream>
#include <iomanip>
#include <filesystem>
#include <cmath>
#include <algorithm>
#include "Utilities/IOStream.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalCommandBuffer.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalMath.h"

using namespace primal;
using namespace primal::graphics;
using namespace primal::graphics::rhi;
using namespace primal::graphics::rendergraph;

// Initialize static member
TestParticleSponza* TestParticleSponza::instance = nullptr;

#ifdef TEST_PARTICLE_SPONZA
Engine_Test::Engine_Test() 
    : primal::test::RenderTestRunner(std::make_unique<TestParticleSponza>()) 
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

bool TestParticleSponza::Initialize() {
    std::cout << "DEBUG: TestParticleSponza STARTING NEW VERSION " << __DATE__ << " " << __TIME__ << std::endl;
    instance = this;
    
    // 0. Initialize JobSystem and AsyncResourceLoader
    if (!primal::jobsystem::JobSystem::Initialize(primal::jobsystem::JobSchedulerConfig::Default())) {
        std::cerr << "Failed to initialize JobSystem" << std::endl;
        return false;
    }
    std::cout << "JobSystem initialized with " << primal::jobsystem::JobSystem::GetWorkerCount() << " workers" << std::endl;
    
    if (!primal::content::AsyncResourceLoader::Initialize()) {
        std::cerr << "Failed to initialize AsyncResourceLoader" << std::endl;
        return false;
    }
    std::cout << "AsyncResourceLoader initialized" << std::endl;
    
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
        "TestParticleSponza", 100, 100, 1280, 720
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
    
#ifndef DISABLE_PARTICLE_SYSTEM
    // ============================================
    // Initialize Particle System
    // ============================================
    if (!primal::particles::initialize()) {
        std::cerr << "Failed to initialize particle system" << std::endl;
        return false;
    }
    std::cout << "Particle system initialized" << std::endl;
    
    // Initialize CPU processor for particle spawning
    primal::particles::initialize_cpu_processor();
    std::cout << "Particle CPU processor initialized" << std::endl;
    
    // Create particle emitter - configured for random colors, sizes, spherical emission, +X movement
    primal::particles::emitter_config particleConfig;
    particleConfig.max_particles = 5000;
    particleConfig.spawn_rate = particleSpawnRate;
    particleConfig.lifetime_min = particleLifetime * 0.5f;
    particleConfig.lifetime_max = particleLifetime;
    
    // Spherical emission with +X direction bias
    // Velocity range: primarily +X direction with some spread
    particleConfig.velocity_min = primal::math::v3{ 0.5f, -1.0f, -1.0f };  // Min velocity (+X direction with spread)
    particleConfig.velocity_max = primal::math::v3{ 3.0f, 1.0f, 1.0f };   // Max velocity (more +X)
    
    // Random colors - full spectrum for vibrant particles
    particleConfig.color_start = primal::math::v4{ 1.0f, 0.2f, 0.2f, 1.0f };  // Warm start (red/orange)
    particleConfig.color_end = primal::math::v4{ 0.2f, 0.5f, 1.0f, 0.8f };    // Cool end (blue/cyan)
    
    // Particle sizes (reduced for better appearance)
    particleConfig.scale_min = primal::math::v2{ 0.3f, 0.3f };
    particleConfig.scale_max = primal::math::v2{ 0.8f, 0.8f };
    
    // Light gravity and drag for visible movement
    // particleConfig.scale_min = primal::math::v2{ 0.3f, 0.3f };
    // particleConfig.scale_max = primal::math::v2{ 0.8f, 0.8f };
    // particleConfig.scale_min = primal::math::v2{ 5.0f, 5.0f };
    // particleConfig.scale_max = primal::math::v2{ 10.0f, 10.0f };
    
    // Light gravity and drag for visible movement
    particleConfig.gravity = primal::math::v3{ 0.0f, -2.0f, 0.0f };
    particleConfig.drag = 0.1f;
    
    // Additive blending for bright particles
    particleConfig.blending = primal::particles::blend_mode::additive;
    particleConfig.depth_write = false;
    
    particleEmitter = primal::particles::create_emitter(particleConfig);
    if (particleEmitter == primal::particles::invalid_id) {
        std::cerr << "Failed to create particle emitter" << std::endl;
        return false;
    }
    
    // Set emitter position (lion mesh location in Sponza - approximately center of scene, elevated)
    auto* emitter = primal::particles::get_emitter(particleEmitter);
    if (emitter) {
        // Lion statue is roughly at this position in Crytek Sponza
        emitter->set_position(emitterPosition);
        std::cout << "Particle emitter created at position: (" 
                  << emitterPosition.x << ", " << emitterPosition.y << ", " << emitterPosition.z << ")" << std::endl;
    }
    
    // Initialize particle render pass
    if (!particlePass_.initialize(device)) {
        std::cerr << "Failed to initialize particle pass" << std::endl;
        return false;
    }
    particlePass_.set_blend_mode(primal::particles::blend_mode::additive);
    particlePass_.set_depth_write_enabled(false);
    particlePass_.set_depth_test_enabled(false);  // Disable depth test for debugging
    std::cout << "Particle pass initialized (depth test DISABLED for debugging)" << std::endl;

#else
    std::cout << "Particle system disabled at compile time" << std::endl;
#endif
    
    std::cout << "TestParticleSponza Initialized. Debug Mode: Disabled" << std::endl;

    return true;
}

void TestParticleSponza::Resize(uint32_t width, uint32_t height) {
    renderWidth = width;
    renderHeight = height;
    renderSystem.Resize(width, height);
    
    // Recreate resources if needed
    CreatePersistentResources();
}

void TestParticleSponza::Run() {
    // Update Camera & Input
    // primal::input::input::update(); // Handled by engine/platform
    m_camera.Update(0.016f); // Fixed dt for test
    
    // Process main thread callbacks (for async resource loading)
    jobsystem::JobSystem::ProcessMainThreadJobs();
    
    primal::input::input_value val;

    // F1: Meshlet Mode (use our own state tracking for reliable edge detection)
    primal::input::get(primal::input::input_source::keyboard, primal::input::input_code::key_f1, val);
    bool f1_current = val.current.x > 0.0f;
    if (f1_current && !keyState.f1_prev) {
        debugSettings.enable = true;
        debugSettings.mode = GeometryDebugMode::Meshlet;
        debugSettings.visualize_meshlets = true;
        debugSettings.visualize_sdf = false;
        debugSettings.visualize_voxels = false;
        debugSettings.visualize_vector_field = false;
        std::cout << "Debug Mode: Meshlet" << std::endl;
    }
    keyState.f1_prev = f1_current;

    // F2: SDF Mode
    primal::input::get(primal::input::input_source::keyboard, primal::input::input_code::key_f2, val);
    bool f2_current = val.current.x > 0.0f;
    if (f2_current && !keyState.f2_prev) {
        debugSettings.enable = true;
        debugSettings.mode = GeometryDebugMode::SDF;
        debugSettings.visualize_meshlets = false;
        debugSettings.visualize_sdf = true;
        debugSettings.visualize_voxels = false;
        debugSettings.visualize_vector_field = false;
        std::cout << "Debug Mode: SDF" << std::endl;
    }
    keyState.f2_prev = f2_current;

    // F3: Voxel Mode
    primal::input::get(primal::input::input_source::keyboard, primal::input::input_code::key_f3, val);
    bool f3_current = val.current.x > 0.0f;
    if (f3_current && !keyState.f3_prev) {
        debugSettings.enable = true;
        debugSettings.mode = GeometryDebugMode::Voxel;
        debugSettings.visualize_meshlets = false;
        debugSettings.visualize_sdf = false;
        debugSettings.visualize_voxels = true;
        debugSettings.visualize_vector_field = false;
        std::cout << "Debug Mode: Voxel" << std::endl;
    }
    keyState.f3_prev = f3_current;

    // F4: Vector Field Mode
    primal::input::get(primal::input::input_source::keyboard, primal::input::input_code::key_f4, val);
    bool f4_current = val.current.x > 0.0f;
    if (f4_current && !keyState.f4_prev) {
        debugSettings.enable = true;
        debugSettings.mode = GeometryDebugMode::VectorField;
        debugSettings.visualize_meshlets = false;
        debugSettings.visualize_sdf = false;
        debugSettings.visualize_voxels = false;
        debugSettings.visualize_vector_field = true;
        std::cout << "Debug Mode: VectorField" << std::endl;
    }
    keyState.f4_prev = f4_current;
    
    // F5: Toggle Enable/Disable
    primal::input::get(primal::input::input_source::keyboard, primal::input::input_code::key_f5, val);
    bool f5_current = val.current.x > 0.0f;
    if (f5_current && !keyState.f5_prev) {
        debugSettings.enable = !debugSettings.enable;
        std::cout << "Debug Mode: " << (debugSettings.enable ? "Enabled" : "Disabled") << std::endl;
    }
    keyState.f5_prev = f5_current;

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
    
#ifndef DISABLE_PARTICLE_SYSTEM
    // ============================================
    // Particle System Controls
    // ============================================
    // F6: Toggle particle system
    primal::input::get(primal::input::input_source::keyboard, primal::input::input_code::key_f6, val);
    bool f6_current = val.current.x > 0.0f;
    if (f6_current && !keyState.f6_prev) {
        particlesEnabled = !particlesEnabled;
        auto* emitter = primal::particles::get_emitter(particleEmitter);
        if (emitter) {
            emitter->set_active(particlesEnabled);
        }
        std::cout << "Particles: " << (particlesEnabled ? "Enabled" : "Disabled") << std::endl;
    }
    keyState.f6_prev = f6_current;
    
    // F7: Decrease spawn rate
    primal::input::get(primal::input::input_source::keyboard, primal::input::input_code::key_f7, val);
    bool f7_current = val.current.x > 0.0f;
    if (f7_current && !keyState.f7_prev) {
        particleSpawnRate = std::max(10.0f, particleSpawnRate - 25.0f);
        auto* emitter = primal::particles::get_emitter(particleEmitter);
        if (emitter) {
            auto config = emitter->get_config();
            config.spawn_rate = particleSpawnRate;
            emitter->set_config(config);
        }
        std::cout << "Particle Spawn Rate: " << particleSpawnRate << "/sec" << std::endl;
    }
    keyState.f7_prev = f7_current;
    
    // F8: Increase spawn rate
    primal::input::get(primal::input::input_source::keyboard, primal::input::input_code::key_f8, val);
    bool f8_current = val.current.x > 0.0f;
    if (f8_current && !keyState.f8_prev) {
        particleSpawnRate = std::min(500.0f, particleSpawnRate + 25.0f);
        auto* emitter = primal::particles::get_emitter(particleEmitter);
        if (emitter) {
            auto config = emitter->get_config();
            config.spawn_rate = particleSpawnRate;
            emitter->set_config(config);
        }
        std::cout << "Particle Spawn Rate: " << particleSpawnRate << "/sec" << std::endl;
    }
    keyState.f8_prev = f8_current;
    
    // Update particle system
    const float deltaTime = 0.016f; // Fixed timestep for test
    primal::particles::update(deltaTime);
    
    // Update emitter position and direction based on camera
    auto* emitter = primal::particles::get_emitter(particleEmitter);
    if (emitter && particlesEnabled) {
        // Get camera position for spawning
        primal::math::v3 camPos = m_camera.GetPosition();
        
        // Set emitter position slightly in front of camera (avoid near clipping plane)
        primal::math::v3 camForward = m_camera.GetForward();
        primal::math::v3 spawnOffset = camForward * 2.0f; // 2 units in front of camera
        emitter->set_position(camPos + spawnOffset);
        
        // Get current config and update velocity
        auto config = emitter->get_config();
        
        // Velocity range: follow camera direction with some spread
        // Base speed in forward direction
        float minSpeed = 0.5f;
        float maxSpeed = 3.0f;
        float spread = 0.5f; // Spread angle factor
        
        config.velocity_min = primal::math::v3{
            camForward.x * minSpeed - spread,
            camForward.y * minSpeed - spread,
            camForward.z * minSpeed - spread
        };
        config.velocity_max = primal::math::v3{
            camForward.x * maxSpeed + spread,
            camForward.y * maxSpeed + spread,
            camForward.z * maxSpeed + spread
        };
        
        emitter->set_config(config);
    }
    
    // Sync particle data to GPU frame
    primal::particles::set_frame_index(renderSystem.GetCurrentFrameIndex());
    
    // Debug output
    static u32 lastParticleReport = 0;
    if (frameCount - lastParticleReport > 60) { // Every ~1 second at 60fps
        lastParticleReport = frameCount;
        auto stats = primal::particles::get_pool_stats();
        auto* em = primal::particles::get_emitter(particleEmitter);
        if (em) {
            auto pos = em->get_position();
            std::cout << "[Particles] Active: " << stats.allocated << "/" << stats.capacity 
                      << " (Peak: " << stats.peak_usage << ") "
                      << "Emitter at: (" << pos.x << ", " << pos.y << ", " << pos.z << ")" << std::endl;
        }
    }

    
    #endif
    
    // ============================================
    // Async Texture Loading
    // Start async loading after a few frames (let engine settle)
    if (frameCount == 5 && !_asyncLoadStarted)
    {
        StartAsyncTextureLoading();
    }
    
    // Update async textures (apply when loaded)
    UpdateAsyncTextures();
    
    // Debug output for loading progress
    static u32 lastReportedCount = 0;
    if (_asyncLoadStarted && !_asyncTexturesLoaded.load())
    {
        u32 currentCount = _asyncTexturesLoadedCount.load();
        if (currentCount != lastReportedCount && currentCount > 0)
        {
            std::cout << "[Frame " << frameCount << "] Loading textures: " 
                      << currentCount << "/" << _asyncTexturesTotalCount.load() << std::endl;
            lastReportedCount = currentCount;
        }
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

TestParticleSponza::~TestParticleSponza() {
    Shutdown();
}

void TestParticleSponza::Shutdown() {
    if (isShutdown) return;
    isShutdown = true;

    if (device) {
        device->WaitIdle();
    }

    // Destroy Pipelines (with handle invalidation to prevent double-free)
    if (gbufferPipeline != handles::INVALID_PIPELINE) {
        device->DestroyPipeline(gbufferPipeline);
        gbufferPipeline = handles::INVALID_PIPELINE;
    }
    if (lightingPipeline != handles::INVALID_PIPELINE) {
        device->DestroyPipeline(lightingPipeline);
        lightingPipeline = handles::INVALID_PIPELINE;
    }
    if (skyboxPipeline != handles::INVALID_PIPELINE) {
        device->DestroyPipeline(skyboxPipeline);
        skyboxPipeline = handles::INVALID_PIPELINE;
    }
    if (shadowPipeline != handles::INVALID_PIPELINE) {
        device->DestroyPipeline(shadowPipeline);
        shadowPipeline = handles::INVALID_PIPELINE;
    }
    if (blitPipeline != handles::INVALID_PIPELINE) {
        device->DestroyPipeline(blitPipeline);
        blitPipeline = handles::INVALID_PIPELINE;
    }

    // Destroy Pipeline Layouts
    if (gbufferLayout != handles::INVALID_PIPELINE_LAYOUT) {
        device->DestroyPipelineLayout(gbufferLayout);
        gbufferLayout = handles::INVALID_PIPELINE_LAYOUT;
    }
    if (lightingLayout != handles::INVALID_PIPELINE_LAYOUT) {
        device->DestroyPipelineLayout(lightingLayout);
        lightingLayout = handles::INVALID_PIPELINE_LAYOUT;
    }
    if (skyboxLayout != handles::INVALID_PIPELINE_LAYOUT) {
        device->DestroyPipelineLayout(skyboxLayout);
        skyboxLayout = handles::INVALID_PIPELINE_LAYOUT;
    }
    if (shadowLayout != handles::INVALID_PIPELINE_LAYOUT) {
        device->DestroyPipelineLayout(shadowLayout);
        shadowLayout = handles::INVALID_PIPELINE_LAYOUT;
    }
    if (blitLayout != handles::INVALID_PIPELINE_LAYOUT) {
        device->DestroyPipelineLayout(blitLayout);
        blitLayout = handles::INVALID_PIPELINE_LAYOUT;
    }

    // Destroy Descriptor Sets
    // Note: Some RHIs free sets when pool is reset, but explicit destroy is safer if we own them
    if (globalDescriptorSet != handles::INVALID_DESCRIPTOR_SET) device->DestroyDescriptorSet(globalDescriptorSet);
    if (lightingDescriptorSet != handles::INVALID_DESCRIPTOR_SET) device->DestroyDescriptorSet(lightingDescriptorSet);
    if (skyboxDescriptorSet != handles::INVALID_DESCRIPTOR_SET) device->DestroyDescriptorSet(skyboxDescriptorSet);
    if (defaultMaterialSet != handles::INVALID_DESCRIPTOR_SET) device->DestroyDescriptorSet(defaultMaterialSet);
    if (blitDescriptorSet != handles::INVALID_DESCRIPTOR_SET) device->DestroyDescriptorSet(blitDescriptorSet);
    
    // Destroy Descriptor Set Layouts
    if (globalSetLayout != handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
        device->DestroyDescriptorSetLayout(globalSetLayout);
        globalSetLayout = handles::INVALID_DESCRIPTOR_SET_LAYOUT;
    }
    if (materialSetLayout != handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
        device->DestroyDescriptorSetLayout(materialSetLayout);
        materialSetLayout = handles::INVALID_DESCRIPTOR_SET_LAYOUT;
    }
    if (lightingSetLayout != handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
        device->DestroyDescriptorSetLayout(lightingSetLayout);
        lightingSetLayout = handles::INVALID_DESCRIPTOR_SET_LAYOUT;
    }
    if (skyboxSetLayout != handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
        device->DestroyDescriptorSetLayout(skyboxSetLayout);
        skyboxSetLayout = handles::INVALID_DESCRIPTOR_SET_LAYOUT;
    }
    if (blitSetLayout != handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
        device->DestroyDescriptorSetLayout(blitSetLayout);
        blitSetLayout = handles::INVALID_DESCRIPTOR_SET_LAYOUT;
    }

    // Destroy Buffers
    if (viewDataBuffer != handles::INVALID_RESOURCE) {
        device->DestroyBuffer(viewDataBuffer);
        viewDataBuffer = handles::INVALID_RESOURCE;
    }
    if (sceneDataBuffer != handles::INVALID_RESOURCE) {
        device->DestroyBuffer(sceneDataBuffer);
        sceneDataBuffer = handles::INVALID_RESOURCE;
    }

    // Destroy Persistent Resources
    if (depthTexture != handles::INVALID_RESOURCE) {
        device->DestroyTexture(depthTexture);
        depthTexture = handles::INVALID_RESOURCE;
    }
    if (whiteTexture != handles::INVALID_RESOURCE) {
        device->DestroyTexture(whiteTexture);
        whiteTexture = handles::INVALID_RESOURCE;
    }
    if (normalTexture != handles::INVALID_RESOURCE) {
        device->DestroyTexture(normalTexture);
        normalTexture = handles::INVALID_RESOURCE;
    }
    if (shadowMap0 != handles::INVALID_RESOURCE) {
        device->DestroyTexture(shadowMap0);
        shadowMap0 = handles::INVALID_RESOURCE;
    }
    if (shadowMap1 != handles::INVALID_RESOURCE) {
        device->DestroyTexture(shadowMap1);
        shadowMap1 = handles::INVALID_RESOURCE;
    }
    if (envCubemap != handles::INVALID_RESOURCE) {
        device->DestroyTexture(envCubemap);
        envCubemap = handles::INVALID_RESOURCE;
    }
    
    // Destroy IBL
    iblPrecomputer.reset();
    if (irradianceMap != handles::INVALID_RESOURCE) {
        device->DestroyTexture(irradianceMap);
        irradianceMap = handles::INVALID_RESOURCE;
    }
    if (prefilteredMap != handles::INVALID_RESOURCE) {
        device->DestroyTexture(prefilteredMap);
        prefilteredMap = handles::INVALID_RESOURCE;
    }
    if (brdfLUT != handles::INVALID_RESOURCE) {
        device->DestroyTexture(brdfLUT);
        brdfLUT = handles::INVALID_RESOURCE;
    }

    // Destroy Samplers
    if (defaultSampler != handles::INVALID_SAMPLER) {
        device->DestroySampler(defaultSampler);
        defaultSampler = handles::INVALID_SAMPLER;
    }
    if (brdfSampler != handles::INVALID_SAMPLER) {
        device->DestroySampler(brdfSampler);
        brdfSampler = handles::INVALID_SAMPLER;
    }
    if (debugSampler != handles::INVALID_SAMPLER) {
        device->DestroySampler(debugSampler);
        debugSampler = handles::INVALID_SAMPLER;
    }
    
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
        if (handle != handles::INVALID_RESOURCE) {
            device->DestroyTexture(handle);
        }
    }
    createdResources.clear();
    
    // Destroy Shaders
    for (auto& pair : shaderVariantMap) {
        if (pair.second != handles::INVALID_SHADER) {
            device->DestroyShader(pair.second);
        }
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
    
#ifndef DISABLE_PARTICLE_SYSTEM
    // ============================================
    // Cleanup Particle System
    // ============================================
    // Note: particlePass_.shutdown() is already called by renderSystem.Shutdown() -> ForwardRenderer::Shutdown()
    // Don't call it again to avoid double-free crash
    // particlePass_.shutdown();
    
    if (particleEmitter != primal::particles::invalid_id) {
        primal::particles::destroy_emitter(particleEmitter);
        particleEmitter = primal::particles::invalid_id;
        std::cout << "Particle emitter destroyed" << std::endl;
    }
    
    primal::particles::shutdown_cpu_processor();
    primal::particles::shutdown();
    std::cout << "Particle system shutdown" << std::endl;
    

#endif
    
    // Cleanup Global Content Resources (GPU Meshes)
    primal::content::shutdown();
    
    // Shutdown JobSystem and AsyncResourceLoader
    primal::content::AsyncResourceLoader::Shutdown();
    primal::jobsystem::JobSystem::Shutdown();
    
    std::cout << "TestParticleSponza::Shutdown End" << std::endl;
}

bool TestParticleSponza::CompileAllShaders() {
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
    
    // Use executable-relative paths for shaders (works from any directory)
    // Try multiple locations in order of preference
    auto getShaderPath = []() -> std::string {
        // First try: relative to current directory (shaders/ copied by CMake)
        if (std::ifstream("shaders/GBuffer.metal").good()) {
            return "shaders/";
        }
        // Second try: Darwin/Debug/shaders/ (when running from build root)
        if (std::ifstream("Darwin/Debug/shaders/GBuffer.metal").good()) {
            return "Darwin/Debug/shaders/";
        }
        // Third try: absolute path to worktree (fallback)
        return "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/shaders/";
    };
    
    auto getEngineShaderPath = []() -> std::string {
        // First try: relative to current directory
        if (std::ifstream("shaders/EquirectangularToCube.metal").good()) {
            return "shaders/";
        }
        // Fallback: absolute path to worktree
        return "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/Engine/Graphics/RHI/Shaders/";
    };
    
    std::string testShaderPath = getShaderPath();
    std::string engineShaderPath = getEngineShaderPath();
    
    std::cout << "Using shader paths:" << std::endl;
    std::cout << "  Test shaders: " << testShaderPath << std::endl;
    std::cout << "  Engine shaders: " << engineShaderPath << std::endl;

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

bool TestParticleSponza::SetupPipelines() {
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
    auto LoadMeshes = [&](const std::vector<uint8_t>& data) -> utl::vector<SceneDataMeshInfo> {
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

bool TestParticleSponza::LoadScene() {
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
                auto* gpuMesh = content::get_rhi_gpu_mesh(
                    content::get_rhi_mesh_id(meshInfo.meshEntityId));
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
                auto* gpuMesh = content::get_rhi_gpu_mesh(
                    content::get_rhi_mesh_id(meshInfo.meshEntityId));
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
    
    // Initialize all materials with default textures first (fast startup)
    // Then collect paths for async loading
    for (auto& meshInfo : sceneMeshes) {
        if (meshInfo.materialInstance) {
            auto material = meshInfo.materialInstance->GetMaterial();
            if (material) material->SetDescriptorSetLayout(materialSetLayout);
            
            meshInfo.materialInstance->Initialize(device);
            
            // Set default textures initially
            for (uint32_t i = 0; i < 3; ++i) {
                meshInfo.materialInstance->SetCurrentFrame(i);
                meshInfo.materialInstance->SetTexture(0, whiteTexture);   // Diffuse
                meshInfo.materialInstance->SetTexture(1, normalTexture);  // Normal
                meshInfo.materialInstance->SetTexture(2, whiteTexture);   // ORM
                meshInfo.materialInstance->SetSampler(3, defaultSampler);
                meshInfo.materialInstance->Update(device);
            }
            
            // Collect texture paths for async loading
            if (!meshInfo.diffuseTexturePath.empty()) {
                std::string fullPath = ResolveTexturePath(assetBaseDir, meshInfo.diffuseTexturePath);
                std::ifstream f(fullPath.c_str());
                if (f.good())
                {
                    _pendingTexturePaths.push_back(fullPath);
                }
            }
            
            if (!meshInfo.normalTexturePath.empty()) {
                std::string fullPath = ResolveTexturePath(assetBaseDir, meshInfo.normalTexturePath);
                std::ifstream f(fullPath.c_str());
                if (f.good())
                {
                    _pendingTexturePaths.push_back(fullPath);
                }
            }
            
            if (!meshInfo.ormTexturePath.empty()) {
                std::string fullPath = ResolveTexturePath(assetBaseDir, meshInfo.ormTexturePath);
                std::ifstream f(fullPath.c_str());
                if (f.good())
                {
                    _pendingTexturePaths.push_back(fullPath);
                }
            }
        }
        
        primal::graphics::RenderProxy proxy = primal::graphics::RenderProxy::Create(
            primal::id::invalid_id, primal::id::invalid_id, primal::id::invalid_id);
        proxy.transform = primal::graphics::rhi::math::MatrixIdentity();
        scene.AddProxy(proxy);
    }
    
    // Remove duplicate texture paths
    std::sort(_pendingTexturePaths.begin(), _pendingTexturePaths.end());
    auto new_end = std::unique(_pendingTexturePaths.begin(), _pendingTexturePaths.end());
    _pendingTexturePaths.resize(new_end - _pendingTexturePaths.begin());
    
    _asyncTexturesTotalCount = static_cast<u32>(_pendingTexturePaths.size());
    std::cout << "Collected " << _pendingTexturePaths.size() << " unique textures for async loading" << std::endl;
    
    return true; 
}

bool TestParticleSponza::SetupIBL() {
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

void TestParticleSponza::UpdateScene() {
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

bool TestParticleSponza::CreateUniformBuffers() {
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

bool TestParticleSponza::CreateDescriptorSets() {
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

bool TestParticleSponza::CreatePersistentResources() {
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

void TestParticleSponza::BuildRenderGraph(RenderGraph& graph, ResourceHandle backBuffer) {
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
            
            // Prevent this pass from being culled - it's needed even if output is overwritten
            builder.SideEffect();
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
    
#ifndef DISABLE_PARTICLE_SYSTEM
    // 4.5 Particle Pass - Render particles on top of lit scene
    if (particlesEnabled)
    {
        struct ParticlePassData {
            RGResourceHandle output;
            RGResourceHandle depth;
        };
        
        graph.AddPass<ParticlePassData>("ParticlePass", RGPassType::Graphics, RGPassCategory::PostProcess,
            [this, depth, lightingOutput](ParticlePassData& data, RenderGraphBuilder& builder) {
                data.output = builder.Write(lightingOutput, ResourceState::RenderTarget);
                data.depth = builder.Read(depth);
                
                RGRenderPassDesc rpDesc;
                rpDesc.colors.push_back(RGAttachmentDesc{ .texture = data.output, .loadOp = LoadAction::Load, .storeOp = StoreAction::Store });
                rpDesc.depthStencil = { .texture = depth, .depthLoadOp = LoadAction::Load, .depthStoreOp = StoreAction::Store };
                builder.DeclareRenderPass(rpDesc);
            },
            [this](const ParticlePassData& /*data*/, RenderGraphContext& context) {
                auto cmd = context.cmdBuffer;
                cmd->SetViewport({ { 0, 0 }, { (float)renderWidth, (float)renderHeight }, 0, 1});
                cmd->SetScissor({ { 0, 0 }, { renderWidth, renderHeight } });
                
                // Get view/projection matrices from camera
                primal::math::m4x4 viewMat = m_camera.GetViewMatrix();
                float fov = 60.0f * primal::graphics::rhi::math::constants::DEG_TO_RAD;
                float aspect = (float)renderWidth / (float)renderHeight;
                primal::math::m4x4 projMat = primal::graphics::rhi::math::CreatePerspectiveMatrix(fov, aspect, 0.1f, 1000.0f);
                
                // Execute particle rendering
                particlePass_.execute(cmd, renderSystem.GetCurrentFrameIndex(), viewMat, projMat);
            }
        );
    }
    

#endif
    
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

// ============================================================================
// Async Texture Loading Implementation
// ============================================================================

void TestParticleSponza::StartAsyncTextureLoading()
{
    if (_asyncLoadStarted || _pendingTexturePaths.empty())
    {
        return;
    }
    
    _asyncLoadStarted = true;
    std::cout << "[Async] Starting async texture loading for " << _pendingTexturePaths.size() << " textures..." << std::endl;
    
    // Start async loading using JobSystem
    _asyncLoadHandle = content::AsyncResourceLoader::Get()->LoadTexturesAsync(
        _pendingTexturePaths,
        [this](const utl::vector<content::TextureLoadResult>& results)
        {
            // This callback runs on main thread
            std::cout << "[Async] Texture loading complete!" << std::endl;
            
            u32 successCount = 0;
            for (const auto& result : results)
            {
                if (result.success)
                {
                    auto texHandle = content::get_rhi_texture_handle(result.handle);
                    _asyncTextureMap[result.path] = texHandle;
                    createdResources.push_back(texHandle);
                    ++successCount;
                }
                else
                {
                    std::cerr << "[Async] Failed to load: " << result.path
                              << " - " << result.error_message << std::endl;
                }
            }
            
            _asyncTexturesLoaded.store(true);
            std::cout << "[Async] Successfully loaded " << successCount << "/" << results.size() << " textures" << std::endl;
        },
        [this](u32 completed, u32 total, const std::string& currentFile)
        {
            // Progress callback
            _asyncTexturesLoadedCount.store(completed);
            if (completed % 10 == 0 || completed == total)
            {
                std::cout << "[Async] Progress: " << completed << "/" << total
                          << " (" << (completed * 100 / total) << "%) - " << currentFile << std::endl;
            }
        }
    );
    
    std::cout << "[Async] Async loading started. Textures will load in background." << std::endl;
}

void TestParticleSponza::UpdateAsyncTextures()
{
    if (!_asyncTexturesLoaded.load())
    {
        return; // Still loading
    }
    
    // Apply loaded textures to meshes
    static bool texturesApplied = false;
    if (texturesApplied)
    {
        return; // Already applied
    }
    texturesApplied = true;
    
    std::cout << "[Async] Applying loaded textures to materials..." << std::endl;
    
    std::string assetBaseDir = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/assets/models/Sponza/";
    
    for (auto& meshInfo : sceneMeshes)
    {
        if (!meshInfo.materialInstance)
        {
            continue;
        }
        
        ResourceHandle diffuseTex = whiteTexture;
        if (!meshInfo.diffuseTexturePath.empty())
        {
            std::string fullPath = ResolveTexturePath(assetBaseDir, meshInfo.diffuseTexturePath);
            auto it = _asyncTextureMap.find(fullPath);
            if (it != _asyncTextureMap.end())
            {
                diffuseTex = it->second;
            }
        }
        
        ResourceHandle normalTex = normalTexture;
        if (!meshInfo.normalTexturePath.empty())
        {
            std::string fullPath = ResolveTexturePath(assetBaseDir, meshInfo.normalTexturePath);
            auto it = _asyncTextureMap.find(fullPath);
            if (it != _asyncTextureMap.end())
            {
                normalTex = it->second;
            }
        }
        
        ResourceHandle ormTex = whiteTexture;
        if (!meshInfo.ormTexturePath.empty())
        {
            std::string fullPath = ResolveTexturePath(assetBaseDir, meshInfo.ormTexturePath);
            auto it = _asyncTextureMap.find(fullPath);
            if (it != _asyncTextureMap.end())
            {
                ormTex = it->second;
            }
        }
        
        // Update material with loaded textures
        for (uint32_t i = 0; i < 3; ++i)
        {
            meshInfo.materialInstance->SetCurrentFrame(i);
            meshInfo.materialInstance->SetTexture(0, diffuseTex);
            meshInfo.materialInstance->SetTexture(1, normalTex);
            meshInfo.materialInstance->SetTexture(2, ormTex);
            meshInfo.materialInstance->SetSampler(3, defaultSampler);
            meshInfo.materialInstance->Update(device);
        }
    }
    
    std::cout << "[Async] Textures applied to all materials!" << std::endl;
}
