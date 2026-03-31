#include "TestNaniteStreamingPipeline.h"
#include "Engine/Content/ContentToEngine.h"
#include "Engine/Content/AsyncResourceLoader.h"
#include "Engine/JobSystem/JobSystem.h"
#include "Engine/Graphics/RHI/Core/RHIMath.h"
#include "Engine/Graphics/RHI/Core/RHIGpuMesh.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalDevice.h"
#include "Engine/Graphics/RenderGraph/RenderGraphBuilder.h"
#include "Engine/Graphics/RenderGraph/RenderGraphDefinitions.h"
#include "Engine/Graphics/Nanite/HZBSystem.h"
#include "Engine/Graphics/Nanite/DepthHistoryManager.h"
#include "Engine/Graphics/Nanite/VisibilityBufferSystem.h"
#include "Engine/Input/Input.h"
#include "Engine/Components/Entity.h"
#include "ShaderCompilation.h"
#include "stb_image.h"  // third_party/stb submodule
#include "Engine/Utilities/IOStream.h"

#include <iostream>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <set>

using namespace primal;
using namespace primal::graphics;
using namespace primal::graphics::rhi;
using namespace primal::graphics::nanite;
using namespace primal::graphics::rhi::math;

namespace {
    // Helper struct for Descriptor Updates (following TestParticleSponza pattern)
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

    // 🎨 Texture loading helpers (from TestParticleSponza)
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

    // 🎨 Bilinear interpolation resize helper
    unsigned char* ResizeTextureBilinear(
        const unsigned char* src_data,
        int src_width, int src_height,
        int dst_width, int dst_height
    ) {
        unsigned char* dst_data = new unsigned char[dst_width * dst_height * 4];

        float x_ratio = static_cast<float>(src_width - 1) / dst_width;
        float y_ratio = static_cast<float>(src_height - 1) / dst_height;

        for (int y = 0; y < dst_height; ++y) {
            for (int x = 0; x < dst_width; ++x) {
                int src_x = static_cast<int>(x * x_ratio);
                int src_y = static_cast<int>(y * y_ratio);

                int x_diff = (src_width > 1) ? (x * x_ratio - src_x) * 256 : 0;
                int y_diff = (src_height > 1) ? (y * y_ratio - src_y) * 256 : 0;

                // Clamp source coordinates
                src_x = (src_x < src_width - 1) ? src_x : src_width - 2;
                src_y = (src_y < src_height - 1) ? src_y : src_height - 2;

                const unsigned char* src_pixel = &src_data[(src_y * src_width + src_x) * 4];
                const unsigned char* src_pixel_next_x = &src_pixel[4];
                const unsigned char* src_pixel_next_y = &src_data[((src_y + 1) * src_width + src_x) * 4];
                const unsigned char* src_pixel_next_xy = &src_pixel_next_y[4];

                for (int c = 0; c < 4; ++c) {
                    int a = src_pixel[c];
                    int b = src_pixel_next_x[c];
                    int c_val = src_pixel_next_y[c];
                    int d = src_pixel_next_xy[c];

                    int result = (
                        a * (256 - x_diff) * (256 - y_diff) +
                        b * x_diff * (256 - y_diff) +
                        c_val * (256 - x_diff) * y_diff +
                        d * x_diff * y_diff
                    ) >> 16;

                    dst_data[(y * dst_width + x) * 4 + c] = static_cast<unsigned char>(result);
                }
            }
        }

        return dst_data;
    }

    primal::graphics::rhi::ResourceHandle LoadTextureFromFile(RHIDeviceBase* device, const std::string& path, bool isNormalMap, bool isSRGB = true) {
        int width, height, channels;
        unsigned char* data = stbi_load(path.c_str(), &width, &height, &channels, 4);
        if (!data) {
            std::cerr << "Failed to load texture: " << path << std::endl;
            return handles::INVALID_RESOURCE;
        }

        // 🎨 DEBUG: Print original texture size
        std::cout << "[TestNanite] Loaded texture: " << path << " (" << width << "x" << height << ")" << std::endl;

        // 🎨 NEW: 统一缩放到 1024x1024
        constexpr int TARGET_SIZE = 1024;
        unsigned char* final_data = data;
        int final_width = width;
        int final_height = height;

        if (width != TARGET_SIZE || height != TARGET_SIZE) {
            std::cout << "[TestNanite] Resizing texture from " << width << "x" << height
                      << " to " << TARGET_SIZE << "x" << TARGET_SIZE << std::endl;

            final_data = ResizeTextureBilinear(data, width, height, TARGET_SIZE, TARGET_SIZE);
            final_width = TARGET_SIZE;
            final_height = TARGET_SIZE;

            // 释放原始数据
            stbi_image_free(data);
        }

        ResourceHandle handle = CreateTextureFromData(device, final_width, final_height, final_data, isSRGB);

        if (final_data != data) {
            delete[] final_data;
        } else {
            stbi_image_free(data);
        }

        return handle;
    }
}

TestNaniteStreamingPipeline* TestNaniteStreamingPipeline::instance = nullptr;

#ifdef TEST_NANITE_STREAMING_PIPELINE
Engine_Test::Engine_Test()
    : primal::test::RenderTestRunner(std::make_unique<TestNaniteStreamingPipeline>())
{}
#endif

primal::game_entity::entity create_one_game_entity(primal::math::v3 position = primal::math::v3(0.0f), primal::math::v3 rotation = primal::math::v3(0.0f))
{
	primal::transform::init_info transform_info{};
    // 使用simd库创建四元数：先创建各轴的旋转四元数，然后相乘
    simd_quatf quat_y = simd_quaternion(rotation.y, simd_make_float3(0, 1, 0)); // yaw
	simd_quatf quat_x = simd_quaternion(rotation.x, simd_make_float3(1, 0, 0)); // pitch
	simd_quatf quat_z = simd_quaternion(rotation.z, simd_make_float3(0, 0, 1)); // roll

	// 注意右乘顺序：roll * pitch * yaw
	simd_quatf quat = simd_mul(quat_z, simd_mul(quat_x, quat_y));

	primal::math::v4 rot_quat{
		quat.vector.x,
		quat.vector.y,
		quat.vector.z,
		quat.vector.w
	};
	memcpy(&transform_info.rotation[0], &rot_quat, sizeof(transform_info.rotation));
    memcpy(&transform_info.position[0], &position, sizeof(transform_info.position));

	primal::game_entity::entity_info entity_info{};
	entity_info.transform = &transform_info;
	primal::game_entity::entity ntt{ primal::game_entity::create(entity_info) };
	assert(ntt.is_valid());
	return ntt;
}

bool TestNaniteStreamingPipeline::Initialize() {
    std::cout << "DEBUG: TestNaniteStreamingPipeline INITIALIZING " << __DATE__ << " " << __TIME__ << std::endl;

    if (!primal::jobsystem::JobSystem::Initialize(primal::jobsystem::JobSchedulerConfig::Default())) {
        std::cerr << "Failed to initialize JobSystem" << std::endl;
        return false;
    }

    if (!primal::content::AsyncResourceLoader::Initialize()) {
        std::cerr << "Failed to initialize AsyncResourceLoader" << std::endl;
        return false;
    }

    if (!InitializeDevice()) {
        std::cerr << "Failed to initialize device" << std::endl;
        return false;
    }

    if (!InitializeWindowAndRenderSystem()) {
        std::cerr << "Failed to initialize window and render system" << std::endl;
        return false;
    }

    renderGraph_ = std::make_unique<rendergraph::RenderGraph>(*device_);

    if (!InitializeStreamingComponents()) {
        std::cerr << "Failed to initialize streaming components" << std::endl;
        return false;
    }

    if (!CreateTestScene()) {
        std::cerr << "Failed to create test scene" << std::endl;
        return false;
    }

    std::cout << "TestNaniteStreamingPipeline Initialized Successfully" << std::endl;
    std::cout << "  - Streaming Pool Size: " << testConfig_.streaming_pool_size_mb << " MB" << std::endl;
    std::cout << "  - Max Clusters: " << testConfig_.max_clusters << std::endl;
    std::cout << "  - Max Requests/Frame: " << testConfig_.max_requests_per_frame << std::endl;

    // Print all instance bounds information
    PrintAllInstanceBounds();

    return true;
}

bool TestNaniteStreamingPipeline::VerifyMeshletUVSupport() {
    std::cout << "[TestNanite] Verifying meshlet UV support..." << std::endl;

    // Check RHIGpuMesh vertex structure
    for (const auto& meshInfo : sceneMeshes_) {
        if (!meshInfo.mesh) continue;

        auto* gpuMesh = content::get_rhi_gpu_mesh(meshInfo.meshEntityId);
        if (!gpuMesh) continue;

        // Calculate vertex stride from element buffer
        // Note: RHIGpuMesh doesn't store stride directly, but we can infer it
        // The element_buffer contains normals + UVs + other vertex data
        // Position is stored separately (12 bytes per vertex)
        auto elementBuffer = gpuMesh->GetElementBuffer();
        u32 vertexCount = gpuMesh->GetVertexCount();

        if (elementBuffer == primal::graphics::rhi::handles::INVALID_RESOURCE) {
            std::cerr << "  ⚠️  Warning: Mesh '" << meshInfo.name
                      << "' has no element buffer (missing normals/UVs)" << std::endl;
            return false;
        }

        // Note: We can't verify UV data directly without GetVertexStride()
        // This is a heuristic check - element buffer should contain normals + UVs
        // We verify buffer exists and vertex count is reasonable
        if (vertexCount == 0) {
            std::cerr << "  ⚠️  Warning: Mesh '" << meshInfo.name
                      << "' has zero vertices" << std::endl;
            return false;
        }

        std::cout << "  Mesh: " << meshInfo.name
                  << ", Vertices: " << vertexCount
                  << ", ElementBuffer: " << (elementBuffer != primal::graphics::rhi::handles::INVALID_RESOURCE ? "OK" : "MISSING")
                  << std::endl;
    }

    std::cout << "  ✓ Meshlet vertex structure verification complete" << std::endl;
    return true;
}

bool TestNaniteStreamingPipeline::InitializeDevice() {
    graphics::rhi::DeviceDesc deviceDesc;
    deviceDesc.platform = graphics::rhi::RHIPlatform::Metal;
    deviceDesc.enableDebug = true;

    auto metalDevice = std::make_unique<graphics::rhi::MetalDevice>(deviceDesc);
    if (!metalDevice->Initialize()) {
        return false;
    }

    device_ = metalDevice.get();
    graphics::rhi::g_deviceManager.RegisterDevice(device_);
    device_ownership_ = std::move(metalDevice);

    return true;
}

bool TestNaniteStreamingPipeline::InitializeWindowAndRenderSystem() {
    platform::window_init_info winInfo{
        nullptr, nullptr,
        "TestNaniteStreamingPipeline", 100, 100, 1280, 720
    };
    window_ = platform::create_window(&winInfo);

    renderWidth_ = window_.width();
    renderHeight_ = window_.height();
#ifdef __APPLE__
    renderWidth_ *= 2;
    renderHeight_ *= 2;
#endif

    graphics::RenderSystemInitInfo sysInfo;
    sysInfo.device = device_;
    sysInfo.window = window_.handle();
    sysInfo.width = renderWidth_;
    sysInfo.height = renderHeight_;

    return renderSystem_.Initialize(sysInfo);
}

bool TestNaniteStreamingPipeline::InitializeStreamingComponents() {
    // 🔥 FIX: Initialize GPU Driven Draw Pipeline FIRST to ensure global meshlet buffer is ready
    // This fixes the issue where GPUCullingPipeline can't access global meshlet buffer during initialization
    gpuDrawPipeline_ = &graphics::nanite::GPUDrivenDrawPipeline::Get();

    graphics::nanite::BinningConfig binningConfig{};
    binningConfig.bin_size = 64;
    binningConfig.max_bins_per_frame = 4096;
    binningConfig.max_clusters_per_bin = 256;
    binningConfig.enable_spatial_sorting = true;

    graphics::nanite::VisibilityBufferConfig visibilityConfig{};
    visibilityConfig.width = renderWidth_;
    visibilityConfig.height = renderHeight_;
    visibilityConfig.format = graphics::rhi::DataFormat::R32_UInt;
    visibilityConfig.enable_depth = true;

    std::cout << "[TestNanite] Initializing GPUDrivenDrawPipeline..." << std::endl;
    if (!gpuDrawPipeline_->Initialize(device_, binningConfig, visibilityConfig)) {
        std::cerr << "Failed to initialize GPU driven draw pipeline" << std::endl;
        return false;
    }
    std::cout << "[TestNanite] GPUDrivenDrawPipeline initialized successfully" << std::endl;

    // Now initialize culling pipeline (after GPU draw pipeline is ready)
    cullingPipeline_ = &graphics::nanite::GPUCullingPipeline::Get();
    std::cout << "[TestNanite] GPUCullingPipeline singleton obtained: " << (void*)cullingPipeline_ << std::endl;

    graphics::nanite::CullingConfig cullingConfig;
    cullingConfig.max_clusters_per_dispatch = testConfig_.max_clusters;
    cullingConfig.max_instances_per_dispatch = testConfig_.max_instances;
    cullingConfig.enable_streaming_feedback = testConfig_.enable_streaming;
    cullingConfig.enable_occlusion_culling = true; // 🔥 ENABLE: HZB Occlusion Culling for testing
    cullingConfig.enable_lod_selection = false; // DISABLED: LOD Selection to isolate flickering
    cullingConfig.enable_debug_output = true; // 🔥 ENABLE: Debug output to see backface culling statistics
    // cullingConfig.enable_lod_selection = testConfig_.enable_lod_selection;

    std::cout << "[TestNanite] Initializing GPUCullingPipeline..." << std::endl;
    if (!cullingPipeline_->Initialize(device_, cullingConfig)) {
        std::cerr << "Failed to initialize GPU culling pipeline" << std::endl;
        return false;
    }
    std::cout << "[TestNanite] GPUCullingPipeline initialized, IsInitialized=" << cullingPipeline_->IsInitialized() << std::endl;

    // Enable GPU culling debug output to diagnose culling issues
    cullingPipeline_->EnableDebugOutput(true);
    std::cout << "[TestNanite] GPU culling debug output ENABLED" << std::endl;

    // CRITICAL: Connect culling pipeline to draw pipeline for proper buffer access
    gpuDrawPipeline_->SetCullingPipeline(cullingPipeline_);
    cullingPipeline_->SetGPUDrawPipeline(gpuDrawPipeline_);

    // CONNECT HZB AND VISIBILITY BUFFER SYSTEMS TO GPU DRIVEN PIPELINE - NOW ENABLED
    std::cout << "[TestNanite] Connecting HZB and Visibility Buffer systems to GPU pipeline..." << std::endl;

    graphics::nanite::NaniteStreamingConfig streamingConfig;
    streamingConfig.page_pool_size_bytes = testConfig_.streaming_pool_size_mb * 1024 * 1024;
    streamingConfig.page_size_bytes = 64 * 1024;
    streamingConfig.max_requests_per_frame = testConfig_.max_requests_per_frame;
    streamingConfig.eviction_threshold = testConfig_.eviction_threshold;

    streamingManager_ = new graphics::nanite::NaniteStreamingManager();
    if (!streamingManager_->Initialize(device_, streamingConfig)) {
        std::cerr << "Failed to initialize streaming manager" << std::endl;
        return false;
    }

    resourceManager_ = &graphics::nanite::NaniteResourceManager::Get();
    if (!resourceManager_->Initialize(device_)) {
        std::cerr << "Failed to initialize resource manager" << std::endl;
        return false;
    }

    streamingManager_->SetNaniteResourceManager(resourceManager_);

    extractionSystem_ = std::make_unique<graphics::SceneExtractionSystem>();
    if (!extractionSystem_->Initialize(device_)) {
        std::cerr << "Failed to initialize extraction system" << std::endl;
        return false;
    }

    // Initialize HZB System - ENABLED for Phase 2 implementation
    hzbSystem_ = std::make_unique<HZBSystem>();
    HZBSystem::Config hzbConfig;
    hzbConfig.max_width = renderWidth_;
    hzbConfig.max_height = renderHeight_;
    hzbConfig.min_mip_size = 8;
    hzbConfig.enable_compression = false; // Disable compression for now
    hzbConfig.generate_on_gpu = true; // Enable GPU HZB generation

    if (!hzbSystem_->Initialize(device_, hzbConfig)) {
        std::cerr << "Failed to initialize HZB system" << std::endl;
        return false;
    }
    std::cout << "[TestNanite] HZB System initialized successfully" << std::endl;

    // 🔥 CRITICAL FIX: Connect HZB system AFTER it's initialized
    // This ensures descriptor sets can access valid HZB texture
    gpuDrawPipeline_->SetHZBSystem(hzbSystem_.get());
    cullingPipeline_->SetHZBSystem(hzbSystem_.get());
    std::cout << "[TestNanite] HZB System connected to GPU pipeline!" << std::endl;

    // Initialize Depth History Manager for triple-buffered depth management
    depthHistoryManager_ = std::make_unique<DepthHistoryManager>();
    DepthHistoryManager::Config depthConfig;
    depthConfig.width = renderWidth_;
    depthConfig.height = renderHeight_;
    depthConfig.format = rhi::DataFormat::R32_Float;
    depthConfig.buffer_count = 3; // Triple buffering

    if (!depthHistoryManager_->Initialize(device_, depthConfig)) {
        std::cerr << "Failed to initialize depth history manager" << std::endl;
        return false;
    }
    std::cout << "[TestNanite] Depth History Manager initialized successfully" << std::endl;

    // Initialize Visibility Buffer System - DISABLED for now due to texture format issues
    // visibilityBufferSystem_ = std::make_unique<VisibilityBufferSystem>();
    // VisibilityBufferSystem::Config visConfig;
    // visConfig.width = renderWidth_;
    // visConfig.height = renderHeight_;
    // visConfig.format = rhi::DataFormat::R32_UInt; // Set proper format
    // visConfig.enable_depth = true;
    //
    // if (!visibilityBufferSystem_->Initialize(device_, visConfig)) {
    //     std::cerr << "Failed to initialize visibility buffer system" << std::endl;
    //     return false;
    // }
    std::cout << "[TestNanite] Visibility Buffer System DISABLED" << std::endl;

    // Initialize Blit Pipeline for final presentation
    std::cout << "[TestNanite] Initializing Blit Pipeline..." << std::endl;

    // Create descriptor set layout for blit
    primal::graphics::rhi::DescriptorSetLayoutBinding blit_bindings[] = {
        { 0, primal::graphics::rhi::DescriptorType::SampledImage, 1, primal::graphics::rhi::ShaderStage::Pixel, nullptr }
    };
    primal::graphics::rhi::DescriptorSetLayoutDesc blit_set_desc{ .bindingCount = 1, .bindings = blit_bindings };
    blit_set_layout_ = device_->CreateDescriptorSetLayout(blit_set_desc);

    // Create pipeline layout
    primal::graphics::rhi::PipelineLayoutDesc blit_pl_desc{ .setLayoutCount = 1, .setLayouts = &blit_set_layout_ };
    blit_layout_ = device_->CreatePipelineLayout(blit_pl_desc);

    // Create descriptor set
    primal::graphics::rhi::DescriptorSetDesc blit_ds_desc{ .layout = blit_set_layout_ };
    blit_descriptor_set_ = device_->CreateDescriptorSet(blit_ds_desc);

    // Load shaders using the same approach as TestParticleSponza
    const shader_file_info blit_vs_info{ "DeferredLighting.metal", "vertexMain", shader_type::vertex };
    const shader_file_info blit_ps_info{ "DeferredLighting.metal", "fragmentBlit", shader_type::pixel };

    // Use executable-relative paths for shaders (following TestParticleSponza pattern)
    auto getShaderPath = []() -> std::string {
        // First try: relative to current directory (shaders/ copied by CMake)
        if (std::ifstream("shaders/DeferredLighting.metal").good()) {
            return "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/shaders/";
        }
        // Second try: Darwin/Debug directory
        if (std::ifstream("Darwin/Debug/shaders/DeferredLighting.metal").good()) {
            return "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/shaders/";
        }
        // Fallback to absolute path (like TestParticleSponza)
        return "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/shaders/";
    };

    auto testShaderPath = getShaderPath();

    // Debug: Print shader path
    std::cout << "[TestNanite] Using shader path: " << testShaderPath << std::endl;

    // Compile shaders (following TestParticleSponza pattern - pass directory path, not full file path)
    auto CompileShader = [&](shader_file_info info) -> bool {
        using namespace primal::graphics;
        using namespace primal::graphics::rhi;

        primal::utl::vector<std::wstring> extra_args; // Fix: create lvalue for third parameter
        auto compiled = compile_shader(info, testShaderPath.c_str(), extra_args);
        if (!compiled) {
            std::cerr << "[TestNanite] Failed to compile shader: " << info.file_name << std::endl;
            return false;
        }

        // Following TestParticleSponza pattern for extracting byte code
        u64 byte_code_size = *reinterpret_cast<u64*>(compiled.get());
        u8* byte_code_ptr = compiled.get() + sizeof(u64) + 16;
        if (!byte_code_ptr || byte_code_size == 0) {
            std::cerr << "[TestNanite] Invalid byte code for shader: " << info.file_name << std::endl;
            return false;
        }

        ShaderStage stage = ShaderStage::Unknown;
        switch (info.type) {
            case ::shader_type::vertex: stage = ShaderStage::Vertex; break;
            case ::shader_type::pixel: stage = ShaderStage::Pixel; break;
            case ::shader_type::compute: stage = ShaderStage::Compute; break;
            default: stage = ShaderStage::Unknown; break;
        }

        ShaderHandle handle = device_->CreateShader(byte_code_ptr, byte_code_size, stage, info.function);
        if (handle == handles::INVALID_SHADER) return false;

        shaderVariantMap[std::string(info.file_name) + ":" + info.function] = handle;
        return true;
    };

    if (!CompileShader(blit_vs_info) || !CompileShader(blit_ps_info)) {
        std::cerr << "[TestNanite] Failed to compile blit shaders" << std::endl;
        return false;
    }

    // Create graphics pipeline
    primal::graphics::rhi::GraphicsPipelineDesc blit_pipeline_desc{};
    blit_pipeline_desc.layout = blit_layout_;
    blit_pipeline_desc.vertexShader = shaderVariantMap[std::string(blit_vs_info.file_name) + ":" + blit_vs_info.function];
    blit_pipeline_desc.pixelShader = shaderVariantMap[std::string(blit_ps_info.file_name) + ":" + blit_ps_info.function];
    blit_pipeline_desc.renderTargetFormats[0] = primal::graphics::rhi::DataFormat::BGRA8_UNorm; // Match swapchain format
    blit_pipeline_desc.renderTargetCount = 1;
    blit_pipeline_desc.depthStencilFormat = primal::graphics::rhi::DataFormat::Unknown;
    blit_pipeline_desc.enableDepthTest = false;
    blit_pipeline_desc.enableDepthWrite = false;
    blit_pipeline_desc.cullMode = primal::graphics::rhi::CullMode::None;
    blit_pipeline_desc.vertexAttributes.clear();
    blit_pipeline_desc.vertexBindings.clear();

    blit_pipeline_ = device_->CreateGraphicsPipeline(blit_pipeline_desc);

    if (blit_pipeline_ == primal::graphics::rhi::handles::INVALID_PIPELINE) {
        std::cerr << "[TestNanite] Failed to create blit pipeline" << std::endl;
        return false;
    }

    std::cout << "[TestNanite] Blit Pipeline initialized successfully" << std::endl;

    // === Composite Blit Pipeline (scene + SSGI) ===
    {
        // 2 sampled image bindings: texture(0)=scene, texture(1)=ssgi
        primal::graphics::rhi::DescriptorSetLayoutBinding composite_bindings[] = {
            { 0, primal::graphics::rhi::DescriptorType::SampledImage, 1, primal::graphics::rhi::ShaderStage::Pixel, nullptr },
            { 1, primal::graphics::rhi::DescriptorType::SampledImage, 1, primal::graphics::rhi::ShaderStage::Pixel, nullptr }
        };
        primal::graphics::rhi::DescriptorSetLayoutDesc composite_set_desc{ .bindingCount = 2, .bindings = composite_bindings };
        blit_composite_set_layout_ = device_->CreateDescriptorSetLayout(composite_set_desc);

        primal::graphics::rhi::PipelineLayoutDesc composite_pl_desc{ .setLayoutCount = 1, .setLayouts = &blit_composite_set_layout_ };
        blit_composite_layout_ = device_->CreatePipelineLayout(composite_pl_desc);

        primal::graphics::rhi::DescriptorSetDesc composite_ds_desc{ .layout = blit_composite_set_layout_ };
        blit_composite_descriptor_set_ = device_->CreateDescriptorSet(composite_ds_desc);

        // Compile the fragmentBlitComposite entry point
        const shader_file_info composite_ps_info{ "DeferredLighting.metal", "fragmentBlitComposite", shader_type::pixel };
        if (!CompileShader(composite_ps_info)) {
            std::cerr << "[TestNanite] Warning: Failed to compile composite blit shader" << std::endl;
        } else {
            primal::graphics::rhi::GraphicsPipelineDesc composite_pipeline_desc{};
            composite_pipeline_desc.layout = blit_composite_layout_;
            composite_pipeline_desc.vertexShader = shaderVariantMap[std::string(blit_vs_info.file_name) + ":" + blit_vs_info.function]; // Reuse same VS
            composite_pipeline_desc.pixelShader = shaderVariantMap[std::string(composite_ps_info.file_name) + ":" + composite_ps_info.function];
            composite_pipeline_desc.renderTargetFormats[0] = primal::graphics::rhi::DataFormat::BGRA8_UNorm;
            composite_pipeline_desc.renderTargetCount = 1;
            composite_pipeline_desc.depthStencilFormat = primal::graphics::rhi::DataFormat::Unknown;
            composite_pipeline_desc.enableDepthTest = false;
            composite_pipeline_desc.enableDepthWrite = false;
            composite_pipeline_desc.cullMode = primal::graphics::rhi::CullMode::None;
            composite_pipeline_desc.vertexAttributes.clear();
            composite_pipeline_desc.vertexBindings.clear();

            blit_composite_pipeline_ = device_->CreateGraphicsPipeline(composite_pipeline_desc);
            if (blit_composite_pipeline_ == primal::graphics::rhi::handles::INVALID_PIPELINE) {
                std::cerr << "[TestNanite] Warning: Failed to create composite blit pipeline" << std::endl;
            } else {
                std::cout << "[TestNanite] Composite Blit Pipeline initialized successfully" << std::endl;
            }
        }
    }

    // Initialize SSGI pipeline
    if (!InitializeSSGIPipeline()) {
        std::cerr << "[TestNanite] Warning: SSGI pipeline initialization failed" << std::endl;
    }

    return true;
}

bool TestNaniteStreamingPipeline::InitializeSSGIPipeline() {
    std::cout << "[LumenSSGI] Initializing SSGI pipeline..." << std::endl;

    // 1. Initialize ColorHistoryManager
    colorHistoryManager_ = std::make_unique<nanite::ColorHistoryManager>();
    nanite::ColorHistoryManager::Config colorConfig;
    colorConfig.width = renderWidth_;
    colorConfig.height = renderHeight_;
    colorConfig.format = DataFormat::BGRA8_UNorm;  // Must match GPUDrivenDrawPipeline output format
    colorConfig.buffer_count = 3;
    if (!colorHistoryManager_->Initialize(device_, colorConfig)) {
        std::cerr << "[LumenSSGI] Failed to initialize ColorHistoryManager" << std::endl;
        return false;
    }

    // 2. Create black fallback texture for missing color history
    {
        u32 blackPixel = 0;
        ssgi_black_texture_ = CreateTextureFromData(device_, 1, 1,
            reinterpret_cast<unsigned char*>(&blackPixel), true);
        if (ssgi_black_texture_ == handles::INVALID_RESOURCE) {
            std::cerr << "[LumenSSGI] Warning: Failed to create black fallback texture" << std::endl;
        }
    }

    // 3. Initialize LumenSSGIPass (owns all GPU resources internally)
    ssgiPass_ = std::make_unique<primal::graphics::lumen::LumenSSGIPass>();
    if (!ssgiPass_->Initialize(device_, renderWidth_, renderHeight_)) {
        std::cerr << "[LumenSSGI] Failed to initialize LumenSSGIPass" << std::endl;
        return false;
    }

    std::cout << "[LumenSSGI] SSGI pipeline initialized via LumenSSGIPass" << std::endl;
    return true;
}

bool TestNaniteStreamingPipeline::CreateTestScene() {
    if (!sceneSnapshot_.Initialize(device_, testConfig_.max_instances, testConfig_.max_clusters)) {
        return false;
    }

    // Load Sponza scene (includes texture loading internally)
    if (!LoadSponzaScene()) {
        std::cerr << "Failed to load Sponza scene" << std::endl;
        return false;
    }

    return true;
}

bool TestNaniteStreamingPipeline::LoadSponzaScene() {
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
        sceneMeshes_ = adapter.LoadRenderItemData(device_, buffer.data(), (uint32_t)buffer.size());
        return !sceneMeshes_.empty();
    };

    // Try to load Sponza.model (same priority as TestParticleSponza)
    std::string modelPath = baseDir + "Sponza_process_rebuild.model";
    if (!TryLoad(modelPath)) {
        std::cerr << "Failed to load Sponza.model from: " << modelPath << std::endl;
        return false;
    }

    std::cout << "Successfully loaded Sponza scene with " << sceneMeshes_.size() << " meshes." << std::endl;

    // 🔧 DEBUG: Keep only the first mesh for culling debugging
    // if (!sceneMeshes_.empty()) {
    //     auto firstMesh = sceneMeshes_[0];
    //     sceneMeshes_.clear();
    //     sceneMeshes_.push_back(firstMesh);
    //     std::cout << "🔧 DEBUG: Keeping only first mesh '" << firstMesh.name << "' for culling analysis" << std::endl;
    // }

    // Debug: Check mesh entity IDs
    u32 validEntityCount = 0;
    for (const auto& meshInfo : sceneMeshes_) {
        if (meshInfo.mesh && meshInfo.meshEntityId != primal::id::invalid_id) {
            validEntityCount++;
        }
    }
    std::cout << "Valid mesh entity IDs: " << validEntityCount << " out of " << sceneMeshes_.size() << std::endl;

    // Verify UV support
    if (!VerifyMeshletUVSupport()) {
        std::cerr << "[TestNanite] UV verification failed, enabling procedural UV fallback" << std::endl;
        useProceduralUV_ = true;
    }

    // 🎨 CRITICAL: Load textures BEFORE registering materials to GPU
    // This ensures MaterialInstance has valid texture handles when BuildAsync reads them
    if (!LoadMaterialTextures()) {
        std::cerr << "[TestNanite] Warning: Some textures failed to load" << std::endl;
    }

    // Initialize GPU Material Registry
    std::cout << "[TestNanite] Initializing GPU Material Registry..." << std::endl;
    gpuMaterialRegistry_ = std::make_unique<primal::graphics::nanite::GPUMaterialRegistry>();

    // Register all materials
    uint32_t registeredCount = 0;
    for (auto& meshInfo : sceneMeshes_) {
        if (meshInfo.materialInstance) {
            primal::graphics::nanite::GPUMaterialRegistry::MaterialID matID = gpuMaterialRegistry_->RegisterMaterial(meshInfo.materialInstance.get());
            if (matID != primal::graphics::nanite::GPUMaterialRegistry::INVALID_MATERIAL_ID) {
                // 🔥 CRITICAL: Save MaterialID to meshInfo for later use in RenderProxy
                meshInfo.gpuMaterialId = matID;
                registeredCount++;
            }
        }
    }

    std::cout << "[TestNanite] Registered " << registeredCount << " materials" << std::endl;

    // 🔥 NEW DEBUG: Print texture handles for problematic materials
    std::cout << "\n[TextureHandleDebug] Texture handle assignments:" << std::endl;
    for (const auto& meshInfo : sceneMeshes_) {
        if (!meshInfo.materialInstance) continue;
        // Only print first occurrence of each MaterialID
        static std::set<uint32_t> printedIDs;
        if (printedIDs.find(meshInfo.gpuMaterialId) == printedIDs.end()) {
            printedIDs.insert(meshInfo.gpuMaterialId);
            rhi::ResourceHandle albedo = meshInfo.materialInstance->GetTextureHandle(0);
            rhi::ResourceHandle normal = meshInfo.materialInstance->GetTextureHandle(1);
            std::cout << "  MaterialID " << meshInfo.gpuMaterialId << ": "
                      << "albedo_handle=" << albedo << ", "
                      << "normal_handle=" << normal << ", "
                      << "mesh_name=" << meshInfo.name << std::endl;
        }
    }

    // 🔍 DEBUG: Material mapping for walls/columns to diagnose texture swapping
    std::cout << "\n[WallDebug] Checking MaterialID 4 (Bricks) usage:" << std::endl;
    int brickMeshCount = 0;
    for (const auto& meshInfo : sceneMeshes_) {
        if (meshInfo.gpuMaterialId == 4) {  // Bricks material
            std::cout << "[WallDebug] MaterialID 4 used by: " << meshInfo.name
                      << " (material_index=" << meshInfo.materialIndex << ")" << std::endl;
            brickMeshCount++;
        }
    }
    std::cout << "[WallDebug] MaterialID 4 used by " << brickMeshCount << " meshes" << std::endl;

    std::cout << "\n[WallDebug] MaterialID 11 (Column_b) usage:" << std::endl;
    int columnBMeshCount = 0;
    for (const auto& meshInfo : sceneMeshes_) {
        if (meshInfo.gpuMaterialId == 11) {  // Column_b material
            std::cout << "[WallDebug] MaterialID 11 used by: " << meshInfo.name
                      << " (material_index=" << meshInfo.materialIndex << ")" << std::endl;
            columnBMeshCount++;
        }
    }
    std::cout << "[WallDebug] MaterialID 11 used by " << columnBMeshCount << " meshes" << std::endl;

    if (registeredCount == 0) {
        std::cerr << "[TestNanite] Warning: No materials were registered!" << std::endl;
    }

    // 🔧 TEMPORARY: Manually adjust UV scaling for problematic materials
    // This is a test to verify UV scaling works before implementing FBX parameter reading
    std::cout << "[TestNanite] Applying manual UV scaling adjustments..." << std::endl;
    AdjustMaterialUVScaling();

    // Start async material build
    std::cout << "[TestNanite] Starting async material data build..." << std::endl;
    materialBuildJob_ = gpuMaterialRegistry_->BuildAsync(device_);

    if (!materialBuildJob_.IsValid()) {
        std::cerr << "[TestNanite] Warning: Material build job is invalid!" << std::endl;
    }

    // 🎨 Wait for material build to complete and upload to GPU
    std::cout << "[TestNanite] Waiting for material build to complete..." << std::endl;
    materialBuildJob_.Wait();

    // Upload material data to GPU
    std::cout << "[TestNanite] Uploading material data to GPU..." << std::endl;
    if (!gpuMaterialRegistry_->UploadToGPU(device_)) {
        std::cerr << "[TestNanite] ERROR: Failed to upload material data to GPU" << std::endl;
    } else {
        std::cout << "[TestNanite] Material data uploaded successfully" << std::endl;

        // Get material data buffer and set it to GPU draw pipeline
        auto materialBuffer = gpuMaterialRegistry_->GetMaterialDataBuffer();
        if (materialBuffer != rhi::handles::INVALID_RESOURCE) {
            gpuDrawPipeline_->SetMaterialDataBuffer(materialBuffer);
            std::cout << "[TestNanite] Material data buffer set to GPU draw pipeline" << std::endl;
        } else {
            std::cerr << "[TestNanite] Warning: Material data buffer is invalid!" << std::endl;
        }

        // 🎨 CRITICAL: Get texture arrays and set them to GPU draw pipeline
        auto albedoArray = gpuMaterialRegistry_->GetAlbedoTextureArray();
        auto normalArray = gpuMaterialRegistry_->GetNormalTextureArray();
        auto ormArray = gpuMaterialRegistry_->GetORMTextureArray();

        if (albedoArray != rhi::handles::INVALID_RESOURCE &&
            normalArray != rhi::handles::INVALID_RESOURCE &&
            ormArray != rhi::handles::INVALID_RESOURCE) {

            // Create texture sampler (if not already created)
            rhi::SamplerHandle sampler = rhi::handles::INVALID_SAMPLER;
            {
                rhi::SamplerDesc samplerDesc{};
                samplerDesc.minFilter = rhi::FilterMode::Linear;
                samplerDesc.magFilter = rhi::FilterMode::Linear;
                samplerDesc.mipFilter = rhi::FilterMode::Linear;
                samplerDesc.addressU = rhi::TextureAddressMode::Wrap;
                samplerDesc.addressV = rhi::TextureAddressMode::Wrap;
                samplerDesc.addressW = rhi::TextureAddressMode::Wrap;
                samplerDesc.mipLodBias = 0.0f;
                samplerDesc.maxAnisotropy = 1;
                samplerDesc.minLod = 0.0f;
                samplerDesc.maxLod = 100.0f;  // Allow all mipmaps

                sampler = device_->CreateSampler(samplerDesc);
                if (sampler == rhi::handles::INVALID_SAMPLER) {
                    std::cerr << "[TestNanite] Warning: Failed to create texture sampler" << std::endl;
                }
            }

            if (sampler != rhi::handles::INVALID_SAMPLER) {
                gpuDrawPipeline_->SetTextureArrays(albedoArray, normalArray, ormArray, sampler);
                std::cout << "[TestNanite] Texture arrays set to GPU draw pipeline (albedo="
                          << albedoArray << ", normal=" << normalArray << ", orm=" << ormArray << ")" << std::endl;
            } else {
                std::cerr << "[TestNanite] Warning: Cannot set texture arrays - sampler creation failed" << std::endl;
            }
        } else {
            std::cerr << "[TestNanite] Warning: Texture arrays are invalid (albedo="
                      << albedoArray << ", normal=" << normalArray << ", orm=" << ormArray << ")" << std::endl;
        }
    }

    // Add loaded meshes to the render scene for Nanite processing
    for (const auto& meshInfo : sceneMeshes_) {
        if (meshInfo.mesh && meshInfo.meshEntityId != primal::id::invalid_id) {
            // Create a game entity FIRST - this entity will be used for both RenderProxy and Cluster
            game_entity::entity entity = create_one_game_entity();
            game_entity::entity_id entityId = entity.get_id();

            // Create Cluster component for Nanite processing (using the same entity)
            cluster::init_info clusterInit{};
            clusterInit.geometry_content_id = meshInfo.meshEntityId;
            cluster::component clusterComp = cluster::create(clusterInit, entity);

            if (clusterComp == primal::id::invalid_id) {
                std::cout << "Warning: Failed to create cluster component for mesh: " << meshInfo.name << std::endl;
            }

            graphics::RenderProxy proxy = graphics::RenderProxy::Create(
                entityId, meshInfo.meshEntityId, meshInfo.gpuMaterialId);  // 🔥 Use gpuMaterialId instead of invalid_id

            // TODO: Extract proper transform from scene data
            // Currently using identity matrix - scene data may not contain individual transforms
            proxy.transform = graphics::rhi::math::MatrixIdentity();
            scene_.AddProxy(proxy);

            // std::cout << "Added mesh: " << meshInfo.name 
            //           << " (entityId=" << entityId 
            //           << ", geometryId=" << meshInfo.meshEntityId << ")" << std::endl;
        }
    }

    // std::cout << "Added " << scene_.GetProxies().size() << " proxies to render scene" << std::endl;

    // Bind scene data to snapshot for Nanite culling
    if (!sceneSnapshot_.Rebind(scene_)) {
        std::cerr << "Failed to bind scene data to snapshot for Nanite culling" << std::endl;
        return false;
    }

    // std::cout << "Scene snapshot updated with " << sceneSnapshot_.GetInstanceCount() << " instances" << std::endl;

    // Initialize render view
    view_.SetViewMatrix(cameraView_);
    view_.SetProjectionMatrix(cameraProj_);
    view_.SetViewport({ {0, 0}, {static_cast<float>(renderWidth_), static_cast<float>(renderHeight_)}, 0, 1 });
    view_.SetScissor({ {0, 0}, {renderWidth_, renderHeight_} });
    view_.UpdateFrustum();

    return true;
}

bool TestNaniteStreamingPipeline::LoadMaterialTextures() {
    std::cout << "[TestNanite] Loading material textures..." << std::endl;

    std::string assetBaseDir = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/assets/";

    // Create default 1024x1024 white texture (for missing textures)
    // 🎨 CRITICAL: Must match Texture2DArray size (1024x1024)
    rhi::ResourceHandle whiteTexture = rhi::handles::INVALID_RESOURCE;
    {
        constexpr u32 WHITE_SIZE = 1024;
        constexpr u32 WHITE_PIXEL_COUNT = WHITE_SIZE * WHITE_SIZE;

        // Create 1024x1024 filled with white
        std::vector<u32> whiteData(WHITE_PIXEL_COUNT, 0xFFFFFFFF);  // RGBA white
        whiteTexture = CreateTextureFromData(device_, WHITE_SIZE, WHITE_SIZE,
            reinterpret_cast<unsigned char*>(whiteData.data()), true);

        if (whiteTexture == rhi::handles::INVALID_RESOURCE) {
            std::cerr << "[TestNanite] Failed to create white texture" << std::endl;
            return false;
        }
    }

    // Load unique textures (deduplication by path)
    std::unordered_map<std::string, rhi::ResourceHandle> textureCache;
    u32 loadedCount = 0;
    u32 skippedCount = 0;

    for (auto& meshInfo : sceneMeshes_) {
        if (!meshInfo.materialInstance) continue;

        // Load diffuse texture
        if (!meshInfo.diffuseTexturePath.empty()) {
            std::string fullPath = ResolveTexturePath(assetBaseDir, meshInfo.diffuseTexturePath);
            if (!fullPath.empty()) {
                auto it = textureCache.find(fullPath);
                rhi::ResourceHandle texture;

                if (it != textureCache.end()) {
                    texture = it->second;
                    skippedCount++;
                } else {
                    texture = LoadTextureFromFile(device_, fullPath, true);  // sRGB for albedo
                    if (texture != rhi::handles::INVALID_RESOURCE) {
                        textureCache[fullPath] = texture;
                        loadedCount++;
                        // 🔥 NEW DEBUG: Print texture handle to filename mapping
                        std::cout << "[TextureMapping] Loaded albedo: handle=" << texture
                                  << " → " << fullPath << std::endl;
                    } else {
                        texture = whiteTexture;  // Fallback to white
                    }
                }

                meshInfo.materialInstance->SetTexture(0, texture);  // Albedo binding = 0
            }
        } else {
            meshInfo.materialInstance->SetTexture(0, whiteTexture);  // No path, use white
        }

        // Infer normal texture path from diffuse if missing (binary files store empty normal paths)
        if (meshInfo.normalTexturePath.empty() && !meshInfo.diffuseTexturePath.empty()) {
            const std::string& diff = meshInfo.diffuseTexturePath;
            std::string inferredNormal;

            // Try known naming conventions: _diffuse → _normal, _Diff → _Normal, _Albedo → _Normal
            if (diff.find("_diffuse.") != std::string::npos) {
                inferredNormal = diff;
                inferredNormal.replace(diff.find("_diffuse."), 9, "_normal.");
            } else if (diff.find("_Diff.") != std::string::npos) {
                inferredNormal = diff;
                inferredNormal.replace(diff.find("_Diff."), 5, "_Normal.");
            } else if (diff.find("_Albedo.") != std::string::npos) {
                inferredNormal = diff;
                inferredNormal.replace(diff.find("_Albedo."), 8, "_Normal.");
            }

            if (!inferredNormal.empty()) {
                std::string fullPath = ResolveTexturePath(assetBaseDir, inferredNormal);
                std::ifstream testFile(fullPath);
                if (testFile.good()) {
                    meshInfo.normalTexturePath = inferredNormal;
                }
            }
        }

        // Load normal texture
        if (!meshInfo.normalTexturePath.empty()) {
            std::string fullPath = ResolveTexturePath(assetBaseDir, meshInfo.normalTexturePath);
            if (!fullPath.empty()) {
                auto it = textureCache.find(fullPath);
                rhi::ResourceHandle texture;

                if (it != textureCache.end()) {
                    texture = it->second;
                } else {
                    texture = LoadTextureFromFile(device_, fullPath, false);  // Linear for normal
                    if (texture != rhi::handles::INVALID_RESOURCE) {
                        textureCache[fullPath] = texture;
                        loadedCount++;
                        // 🔥 NEW DEBUG: Print texture handle to filename mapping
                        std::cout << "[TextureMapping] Loaded normal: handle=" << texture
                                  << " → " << fullPath << std::endl;
                    } else {
                        texture = whiteTexture;  // Fallback
                    }
                }

                meshInfo.materialInstance->SetTexture(1, texture);  // Normal binding = 1
            }
        } else {
            meshInfo.materialInstance->SetTexture(1, whiteTexture);  // No path, use white
        }

        // ORM texture (use white for now - would need separate loading logic)
        meshInfo.materialInstance->SetTexture(2, whiteTexture);  // ORM binding = 2
    }

    std::cout << "[TestNanite] Texture loading complete: "
              << loadedCount << " loaded, " << skippedCount << " reused ("
              << textureCache.size() << " unique)" << std::endl;

    return true;
}

// 🔧 NEW: Manually adjust UV scaling for problematic materials
void TestNaniteStreamingPipeline::AdjustMaterialUVScaling() {
    if (!gpuMaterialRegistry_) {
        std::cerr << "[AdjustUVScaling] ERROR: GPU Material Registry is null!" << std::endl;
        return;
    }

    auto* materialData = gpuMaterialRegistry_->GetMaterialDataMutable();
    if (!materialData) {
        std::cerr << "[AdjustUVScaling] ERROR: Material data is null!" << std::endl;
        return;
    }

    size_t materialCount = gpuMaterialRegistry_->GetMaterialCount();
    std::cout << "[AdjustUVScaling] Adjusting UV scaling for " << materialCount << " materials..." << std::endl;

    // 🔧 TEMPORARY HARDCODED ADJUSTMENTS
    // These are test values based on UV visualization - materials with small UV ranges need higher scaling
    // TODO: Read these values from FBX material properties

    // 🔥 FIXED: Structure alignment issue resolved
    // UV coordinates are now correctly read from buffer (20 bytes, no padding)
    // Use default scaling (1.0) since UV range is already correct [0, 1]

    for (size_t i = 0; i < materialCount; ++i) {
        auto& mat = materialData[i];

        // Default UV scaling (no repetition) since UV coords are now correct
        mat.uv_scale[0] = 1.0f;
        mat.uv_scale[1] = 1.0f;

        if (i < 10) {  // Log first 10 materials for debugging
            std::cout << "[AdjustUVScaling] Material " << i << ": UV scale set to (1.0, 1.0) - structure alignment fixed" << std::endl;
        }
        // Other materials: default scaling (1.0, 1.0)
    }

    std::cout << "[AdjustUVScaling] UV scaling adjustments complete" << std::endl;
}

void TestNaniteStreamingPipeline::Run() {
    primal::input::input_value val;

    primal::input::get(primal::input::input_source::keyboard, primal::input::input_code::key_f1, val);
    bool f1_current = val.current.x > 0.0f;
    if (f1_current && !keyState_.f1_prev) {
        testConfig_.enable_streaming = !testConfig_.enable_streaming;
        std::cout << "Streaming: " << (testConfig_.enable_streaming ? "Enabled" : "Disabled") << std::endl;
    }
    keyState_.f1_prev = f1_current;

    primal::input::get(primal::input::input_source::keyboard, primal::input::input_code::key_f2, val);
    bool f2_current = val.current.x > 0.0f;
    if (f2_current && !keyState_.f2_prev) {
        std::cout << "\n=== Running Stress Test ===" << std::endl;
        for (u32 i = 0; i < 100; ++i) {
            ProcessStreamingFeedback();
        }
        ValidateResults();
        std::cout << "Stress Test Complete" << std::endl;
    }
    keyState_.f2_prev = f2_current;

    primal::input::get(primal::input::input_source::keyboard, primal::input::input_code::key_f3, val);
    bool f3_current = val.current.x > 0.0f;
    if (f3_current && !keyState_.f3_prev) {
        std::cout << "\n=== Running Performance Benchmark ===" << std::endl;
        TestPerformance();
    }
    keyState_.f3_prev = f3_current;

    // F4: SSGI visualization mode toggle (0=Composite, 1=SSGI only, 2=Scene only)
    primal::input::get(primal::input::input_source::keyboard, primal::input::input_code::key_f4, val);
    bool f4_current = val.current.x > 0.0f;
    if (f4_current && !keyState_.f4_prev) {
        ssgiVisMode_ = (ssgiVisMode_ + 1) % 3;
        const char* modeNames[] = { "Composite (Scene+SSGI)", "SSGI Only", "Scene Only" };
        std::cout << "[SSGI Vis] Mode: " << modeNames[ssgiVisMode_] << std::endl;
    }
    keyState_.f4_prev = f4_current;

    jobsystem::JobSystem::ProcessMainThreadJobs();

    UpdateTestScene();
    ProcessStreamingFeedback();
    RecordTestMetrics();

    rhi::ResourceHandle backBuffer;
    rhi::SyncHandle imageAvailable;
    if (renderSystem_.BeginFrame(backBuffer, imageAvailable)) {
        auto cmd = renderSystem_.GetCurrentCommandBuffer();
        cmd->Reset();
        cmd->Begin();

        renderGraph_->Clear();
        // Capture the index correctly for this specific frame inside the graph setup
        u32 currentGraphBufferIndex = renderSystem_.GetCurrentFrameIndex();
        BuildRenderGraph(*renderGraph_, backBuffer, currentGraphBufferIndex);
        renderGraph_->Compile();
        renderGraph_->Execute(cmd);

        cmd->End();

        rhi::QueueSubmitInfo submitInfo{};
        submitInfo.cmdBuffer = renderSystem_.GetCurrentCommandBufferHandle();
        submitInfo.signalFence = imageAvailable;
        device_->Submit(submitInfo);

        renderSystem_.EndFrame();
        frameCount_++;
    }

    if (frameCount_ == 0) {
        auto stats = streamingManager_->GetStats();
        std::cout << "[Frame " << frameCount_ << "] "
                  << "Streamed: " << testResults_.clusters_streamed.load()
                  << ", Evicted: " << testResults_.clusters_evicted.load()
                  << ", Requests: " << testResults_.requests_processed.load()
                  << ", Pool Usage: " << (stats.page_pool_usage * 100.0f) << "%"
                  << std::endl;
    }

    // Collect GPU culling debug data for the final frame only
    // Replace previous frame's data with current frame's data
    utl::vector<primal::graphics::nanite::CullingDebugData> debug_data;
    if (cullingPipeline_->ReadDebugData(debug_data)) {
        // Store current frame data (replaces previous frame data)
        finalFrameCullingDebugData_ = std::move(debug_data);
    }
}

void TestNaniteStreamingPipeline::UpdateTestScene() {
    // Camera setup matching TestParticleSponza exactly
    // Initialize camera if not already initialized
    static bool camera_initialized = false;
    if (!camera_initialized) {
        camera_.Initialize({0.0f, 5.0f, 0.0f}, {0.0f, 0.0f, 0.0f});
        camera_.SetSpeed(10.0f, 0.1f);
        camera_initialized = true;
    }

    // CRITICAL FIX: Update camera FIRST before using its data
    // This ensures we have valid camera matrices before updating double-buffers
    camera_.Update(0.016f); // Fixed dt for test

    // DISABLED: Jitter for TAA - causing flickering in debug visualization
    // float jitterX = ((float)(rand() % 100) / 100.0f - 0.5f) * 0.001f;
    // float jitterY = ((float)(rand() % 100) / 100.0f - 0.5f) * 0.001f;
    // camera_.ApplyJitter(jitterX, jitterY);

    // Get view matrix from RHICamera (like TestParticleSponza)
    cameraView_ = camera_.GetViewMatrix();

    // Calculate projection matrix
    float fov = 60.0f * primal::graphics::rhi::math::constants::DEG_TO_RAD;
    float aspect = (float)renderWidth_ / (float)renderHeight_;
    cameraProj_ = rhi::math::CreatePerspectiveMatrix(fov, aspect, 0.1f, 1000.0f);

    // Update RenderView
    view_.SetViewMatrix(cameraView_);
    view_.SetProjectionMatrix(cameraProj_);
    view_.SetViewport({ {0, 0}, {static_cast<float>(renderWidth_), static_cast<float>(renderHeight_)}, 0, 1 });
    view_.SetScissor({ {0, 0}, {renderWidth_, renderHeight_} });
    view_.UpdateFrustum();

    // CRITICAL FIX: Initialize triple-buffered camera data AFTER camera is set up
    // This ensures we start with valid camera matrices, not identity matrices
    static bool buffers_initialized = false;
    if (!buffers_initialized) {
        for (int i = 0; i < 3; i++) {
            cameraBuffers_[i].view_matrix = cameraView_;
            cameraBuffers_[i].proj_matrix = cameraProj_;
            cameraBuffers_[i].frame_index = 0;
        }
        buffers_initialized = true;
    }

    // Update ONLY the current frame's camera buffer
    // Updating other buffers (in-flight frames) causes race conditions and flickering!
    u32 currentBufferIndex = renderSystem_.GetCurrentFrameIndex();
    cameraBuffers_[currentBufferIndex].view_matrix = cameraView_;
    cameraBuffers_[currentBufferIndex].proj_matrix = cameraProj_;
    cameraBuffers_[currentBufferIndex].frame_index = frameCount_;
}

void TestNaniteStreamingPipeline::BuildRenderGraph(
    graphics::rendergraph::RenderGraph& graph,
    graphics::rhi::ResourceHandle backBuffer,
    u32 currentBufferIndex) {

    // Import backbuffer
    auto backBufferHandle = graph.ImportResource("BackBuffer", backBuffer);

    // CRITICAL FIX: Don't recreate depth texture every frame!
    // Use R32_Float format for intermediate depth storage
    if (sceneDepthTexture_ == rhi::handles::INVALID_RESOURCE) {
        std::cout << "[HZB] Creating persistent depth texture (" << renderWidth_ << "x" << renderHeight_ << ")" << std::endl;

        rhi::TextureDesc depthDesc{};
        depthDesc.size = {renderWidth_, renderHeight_, 1};
        depthDesc.format = rhi::DataFormat::D32_Float;  // Depth format for source texture
        depthDesc.usage = rhi::TextureUsage::ShaderResource | rhi::TextureUsage::DepthStencil | rhi::TextureUsage::CopyDest;
        sceneDepthTexture_ = device_->CreateTexture(depthDesc);

        if (sceneDepthTexture_ == rhi::handles::INVALID_RESOURCE) {
            std::cerr << "[HZB] ERROR: Failed to create persistent depth texture!" << std::endl;
        }
    }

    auto depthHandle = graph.ImportResource("SceneDepth", sceneDepthTexture_);

    struct CullingPassData {
        rendergraph::RGResourceHandle depth_buffer;
        rendergraph::RGResourceHandle hzb_buffer;
        rendergraph::RGResourceHandle visibility_buffer;
        rendergraph::RGResourceHandle indirect_args_buffer;
    };

    const auto& cullingData = graph.AddPass<CullingPassData>("NaniteCulling",
        graphics::rendergraph::RGPassType::Compute,
        graphics::rendergraph::RGPassCategory::Main,
        [this, currentBufferIndex](CullingPassData& data, graphics::rendergraph::RenderGraphBuilder& builder) {
            // std::cout << "[BuildRenderGraph] NaniteCulling PASS SETUP called!" << std::endl;

            // CRITICAL FIX: Use the shared synchronized buffer index for this frame
            // This ensures both compute and render passes use the same buffer
            rhi::ResourceHandle indirectBuffer = cullingPipeline_->GetIndirectBuffer(currentBufferIndex);

            if (indirectBuffer == rhi::handles::INVALID_RESOURCE) {
                std::cerr << "[NaniteCulling] ERROR: Invalid indirect args buffer from pipeline at index " << currentBufferIndex << std::endl;
                return;
            }

            // Import the buffer into RenderGraph using the graph reference
            rhi::BufferDesc bufferDesc{};
            bufferDesc.size = sizeof(u32) * 5;
            data.indirect_args_buffer = builder.GetGraph().ImportBuffer(
                "indirect_args_" + std::to_string(currentBufferIndex),
                indirectBuffer,
                bufferDesc
            );

            // CRITICAL: Tell RenderGraph we're writing to this buffer with proper memory barrier
            // This establishes the dependency: SceneRender depends on NaniteCulling
            builder.Write(data.indirect_args_buffer, rhi::ResourceState::UnorderedAccess);
        },
        [this, currentBufferIndex](const CullingPassData& data, graphics::rendergraph::RenderGraphContext& context) {
            auto cmd = context.cmdBuffer;

            // Use triple-buffered camera data matching the system
            u32 bufferIndex = currentBufferIndex;

            // Build HZB from previous frame's depth (if available) for THIS frame's occlusion culling
            if (hzbSystem_ && hzbSystem_->IsReady() && frameCount_ > 0) {
                // CRITICAL: Use the persistent sceneDepthTexture_ which contains previous frame's data
                // This texture persists across frames, so it still has Frame N-1's data when we're in Frame N
                if (sceneDepthTexture_ != rhi::handles::INVALID_RESOURCE) {
                    // CRITICAL: Insert barrier to ensure the persistent depth texture is ready for shader read
                    // The texture was written to in the previous frame's SceneRender pass
                    rhi::ResourceBarrier depthTextureBarrier{};
                    depthTextureBarrier.resource = sceneDepthTexture_;
                    depthTextureBarrier.beforeState = rhi::ResourceState::CopyDest;  // Was copy destination in previous frame
                    depthTextureBarrier.afterState = rhi::ResourceState::ShaderResource;  // Now shader reads from it
                    depthTextureBarrier.subresource = 0xFFFFFFFF;
                    cmd->InsertBarrier(&depthTextureBarrier, 1);

                    // Build HZB from previous frame depth (still in sceneDepthTexture_)
                    auto hzbResult = hzbSystem_->BuildHZB(sceneDepthTexture_, cmd);

                    // 🔥 CRITICAL: Insert barrier to ensure HZB texture is ready for culling shader read
                    // HZB generation writes to the texture, and culling shader needs to read from it
                    rhi::ResourceBarrier hzbBarrier{};
                    hzbBarrier.resource = hzbSystem_->GetHZBTexture();
                    hzbBarrier.beforeState = rhi::ResourceState::UnorderedAccess;  // HZB was written as UAV
                    hzbBarrier.afterState = rhi::ResourceState::ShaderResource;   // Culling shader reads as SRV
                    hzbBarrier.subresource = 0xFFFFFFFF;  // All mip levels
                    cmd->InsertBarrier(&hzbBarrier, 1);
                }
            }

            // Render visibility buffer (if enabled) - DISABLED
            if (false && visibilityBufferSystem_ && visibilityBufferSystem_->IsReady()) {
                // std::cout << "[BuildRenderGraph] Rendering visibility buffer..." << std::endl;
                auto visResult = visibilityBufferSystem_->RenderVisibilityBuffer(
                    cmd,
                    sceneSnapshot_,
                    cameraBuffers_[bufferIndex].view_matrix,
                    cameraBuffers_[bufferIndex].proj_matrix,
                    cullingPipeline_->GetResults(),
                    frameCount_
                );
                // std::cout << "[BuildRenderGraph] Visibility buffer rendered: " << visResult.visible_triangles
                //           << " triangles in " << visResult.render_time_ms << " ms" << std::endl;
            }

            if (!cullingPipeline_->Execute(
                cmd,
                sceneSnapshot_,
                cameraBuffers_[bufferIndex].view_matrix,
                cameraBuffers_[bufferIndex].proj_matrix,
                streamingManager_,
                bufferIndex)) {  // Fixed: Use bufferIndex instead of frameCount_ for synchronization
                std::cerr << "[NaniteCulling] Culling pipeline failed!" << std::endl;
            }

            // Execute defers UpdateResults() until GPU completes - results will be available after render graph execution
            const auto& results = cullingPipeline_->GetResults();
            // std::cout << "[NaniteCulling] Culling complete: visible_clusters=" << results.visible_cluster_count
            //           << ", visible_instances=" << results.visible_instance_count << std::endl;
        }
    );
    
    // std::cout << "[BuildRenderGraph] NaniteCulling pass ADDED to graph" << std::endl;

    // INSERTED: ForceSync Pass (Blit Encoder Barrier)
    // This inserts a BlitCommandEncoder between Compute and Render, forcing a full GPU synchronization.
    // This is the recommended fix for flickering issues in Metal GPU-driven pipelines.
    struct ForceSyncPassData {
        rendergraph::RGResourceHandle dummy;
    };

    graph.AddPass<ForceSyncPassData>("ForceSync",
        graphics::rendergraph::RGPassType::Copy,
        graphics::rendergraph::RGPassCategory::Copy,
        [&, cullingIndirectArgs = cullingData.indirect_args_buffer](ForceSyncPassData& data, graphics::rendergraph::RenderGraphBuilder& builder) {
            // Force a read dependency on the indirect args buffer
            // This ensures a barrier from Compute -> Copy
            builder.Read(cullingIndirectArgs, rhi::ResourceState::CopySource);
        },
        [](const ForceSyncPassData& data, graphics::rendergraph::RenderGraphContext& context) {
            // Empty body - just the existence of the pass forces a new encoder (BlitEncoder)
            // and the barrier transitions.
        }
    );

    // Scene rendering pass - let GPU draw pipeline handle its own render pass management
    struct SceneRenderPassData {
        rendergraph::RGResourceHandle gpu_output;     // Import GPU pipeline's final output
        rendergraph::RGResourceHandle output;         // Final output (backbuffer)
        const RenderSceneSnapshot* scene_snapshot;
        const CullingResults* culling_results;
        rendergraph::RGResourceHandle indirect_args_buffer;  // Pre-allocated indirect args buffer
    };

    // Import the GPU draw pipeline's final output texture
    // The GPU pipeline manages its own render pass and output texture
    auto gpuFinalOutput = gpuDrawPipeline_->GetFinalOutputTexture();
    if (gpuFinalOutput == rhi::handles::INVALID_RESOURCE) {
        std::cerr << "[BuildRenderGraph] ERROR: GPU draw pipeline has no valid output texture!" << std::endl;
        return;
    }

    auto gpuOutputHandle = graph.ImportResource("GPUFinalOutput", gpuFinalOutput);
    // std::cout << "[BuildRenderGraph] Imported GPU pipeline's final output texture" << std::endl;

    // Scene Render Pass - Execute GPU pipeline which manages its own render pass
    graph.AddPass<SceneRenderPassData>("SceneRender",
        graphics::rendergraph::RGPassType::Graphics,
        graphics::rendergraph::RGPassCategory::Main,
        [this, gpuOutputHandle, currentBufferIndex, cullingIndirectArgs = cullingData.indirect_args_buffer](SceneRenderPassData& data, graphics::rendergraph::RenderGraphBuilder& builder) {
            // std::cout << "[SceneRender] SETUP: Configuring GPU pipeline execution" << std::endl;

            // Store pointers to scene data for use in execute phase
            data.scene_snapshot = &sceneSnapshot_;
            data.culling_results = &cullingPipeline_->GetResults();

            // Import GPU pipeline's output as an external resource
            // The GPU pipeline manages its own render pass internally
            // CHANGED: Use Write instead of Read to ensure this pass is not culled (since FinalBlit reads it)
            data.gpu_output = builder.Write(gpuOutputHandle, rhi::ResourceState::RenderTarget);

            // CRITICAL FIX: Use the SAME RGResourceHandle from culling pass
            // This ensures we read from the same buffer that compute shader just wrote to
            data.indirect_args_buffer = cullingIndirectArgs;

            // CRITICAL: Establish dependency on culling pass with proper memory barrier
            // This ensures compute shader has finished writing before we read
            builder.Read(data.indirect_args_buffer, rhi::ResourceState::IndirectArgument);
        },
        [this, currentBufferIndex](const SceneRenderPassData& data, graphics::rendergraph::RenderGraphContext& context) {
            auto cmd = context.cmdBuffer;

            // CRITICAL FIX: Use triple-buffered camera data matching the system
            u32 bufferIndex = currentBufferIndex;

            // Execute the complete GPU-driven draw pipeline
            // The pipeline handles its own render pass begin/end internally
            if (!gpuDrawPipeline_->Execute(cmd, *data.scene_snapshot, cameraBuffers_[bufferIndex].view_matrix, cameraBuffers_[bufferIndex].proj_matrix, *data.culling_results, frameCount_, currentBufferIndex)) {
                std::cerr << "[GPU Draw] ERROR: Pipeline execution failed!" << std::endl;
            }

            // CRITICAL: Copy depth texture from GPU pipeline to our HZB depth texture
            auto gpuDepthTexture = gpuDrawPipeline_->GetFinalDepthTexture();

            if (sceneDepthTexture_ != rhi::handles::INVALID_RESOURCE && gpuDepthTexture != rhi::handles::INVALID_RESOURCE) {
                // Now both textures are D32_Float format, so BlitTexture should work
                rhi::TextureBlitRegion blitRegion{};
                blitRegion.srcSubresource = {0, 0, 1}; // mip 0, array 0, 1 layer
                blitRegion.dstSubresource = {0, 0, 1};
                blitRegion.srcOffsets[0] = {0, 0, 0};
                blitRegion.srcOffsets[1] = {static_cast<s32>(renderWidth_), static_cast<s32>(renderHeight_), 1};
                blitRegion.dstOffsets[0] = {0, 0, 0};
                blitRegion.dstOffsets[1] = {static_cast<s32>(renderWidth_), static_cast<s32>(renderHeight_), 1};

                cmd->BlitTexture(gpuDepthTexture, sceneDepthTexture_, &blitRegion, 1, rhi::FilterMode::Linear);

                // Note: sceneDepthTexture_ now contains current frame's depth
                // Next frame's HZB generation will automatically use this data
                // No need for explicit storage - the texture persists across frames
            }

            // Log rendering statistics
            if (frameCount_ % 60 == 0 && !sceneMeshes_.empty()) {
                const auto& cullingResults = cullingPipeline_->GetResults();
                const auto& drawResults = gpuDrawPipeline_->GetResults();

                // std::cout << "[GPU Driven Rendering Stats]" << std::endl;
                // std::cout << "  Loaded meshes: " << sceneMeshes_.size() << std::endl;
                // std::cout << "  Scene instances: " << sceneSnapshot_.GetInstanceCount() << std::endl;
                // std::cout << "  Visible clusters: " << cullingResults.visible_cluster_count << std::endl;
                // std::cout << "  GPU Draw calls: " << drawResults.total_draw_calls << std::endl;
                // std::cout << "  Clusters rendered: " << drawResults.total_clusters_rendered << std::endl;
                // std::cout << "  Bin count: " << drawResults.bin_count << std::endl;
            }
        }
    );

    // === DEPTH COPY PASS ===
    // CRITICAL: Separate pass for depth texture copy using Blit encoder
    struct DepthCopyPassData {
        rendergraph::RGResourceHandle dummy;
    };

    graph.AddPass<DepthCopyPassData>("DepthCopy",
        graphics::rendergraph::RGPassType::Copy,  // Use Copy type for Blit encoder
        graphics::rendergraph::RGPassCategory::Copy,
        [this](DepthCopyPassData& data, graphics::rendergraph::RenderGraphBuilder& builder) {
            // No resource dependencies needed - we handle this manually
        },
        [this](const DepthCopyPassData& data, graphics::rendergraph::RenderGraphContext& context) {
            auto cmd = context.cmdBuffer;

            auto gpuDepthTexture = gpuDrawPipeline_->GetFinalDepthTexture();

            if (sceneDepthTexture_ != rhi::handles::INVALID_RESOURCE && gpuDepthTexture != rhi::handles::INVALID_RESOURCE) {
                // Insert barrier for source depth texture
                rhi::ResourceBarrier srcBarrier{};
                srcBarrier.resource = gpuDepthTexture;
                srcBarrier.beforeState = rhi::ResourceState::DepthStencil;
                srcBarrier.afterState = rhi::ResourceState::CopySource;
                srcBarrier.subresource = 0xFFFFFFFF;
                srcBarrier.queueFamily = 0xFFFFFFFF;
                cmd->InsertBarrier(&srcBarrier, 1);

                // Insert barrier for destination texture
                rhi::ResourceBarrier dstBarrier{};
                dstBarrier.resource = sceneDepthTexture_;
                dstBarrier.beforeState = rhi::ResourceState::Unknown;
                dstBarrier.afterState = rhi::ResourceState::CopyDest;
                dstBarrier.subresource = 0xFFFFFFFF;
                dstBarrier.queueFamily = 0xFFFFFFFF;
                cmd->InsertBarrier(&dstBarrier, 1);

                // Blit depth texture
                rhi::TextureBlitRegion blitRegion{};
                blitRegion.srcSubresource = {0, 0, 1}; // mipLevel, arrayLayer, arraySize
                blitRegion.srcOffsets[0] = {0, 0, 0};
                blitRegion.srcOffsets[1] = {static_cast<int>(renderWidth_), static_cast<int>(renderHeight_), 1};
                blitRegion.dstSubresource = {0, 0, 1};
                blitRegion.dstOffsets[0] = {0, 0, 0};
                blitRegion.dstOffsets[1] = {static_cast<int>(renderWidth_), static_cast<int>(renderHeight_), 1};

                cmd->BlitTexture(gpuDepthTexture, sceneDepthTexture_, &blitRegion, 1, rhi::FilterMode::Nearest);
            }
        }
    );

    // === LUMEN SSGI PASS ===
    rendergraph::RGResourceHandle ssgiOutputHandle;
    if (ssgiPass_ && ssgiPass_->IsInitialized() && frameCount_ > 0) {
        // Import GBuffer textures into render graph for SSGI
        auto normalHandle = graph.ImportResource("GBufferNormal", gpuDrawPipeline_->GetGBufferNormal());
        auto velocityHandle = graph.ImportResource("GBufferVelocity", gpuDrawPipeline_->GetGBufferVelocity());
        auto hzbHandle = graph.ImportResource("HZBTexture", hzbSystem_->GetHZBTexture());

        // Get previous frame color (or black fallback)
        auto prevColor = colorHistoryManager_->GetPreviousFrameColor(frameCount_);
        ResourceHandle prevColorTex = prevColor.is_valid ? prevColor.texture : ssgi_black_texture_;
        auto prevColorHandle = graph.ImportResource("PrevFrameColor", prevColorTex);

        // Build camera data for LumenSSGIPass
        lumen::SSGICameraData cameraData;
        cameraData.view_matrix = cameraBuffers_[currentBufferIndex].view_matrix;
        cameraData.proj_matrix = cameraBuffers_[currentBufferIndex].proj_matrix;
        {
            u32 prevIdx = (currentBufferIndex + 2) % 3;
            cameraData.prev_view_matrix = cameraBuffers_[prevIdx].view_matrix;
            cameraData.prev_proj_matrix = cameraBuffers_[prevIdx].proj_matrix;
        }
        cameraData.frame_index = frameCount_;
        cameraData.delta_time = 0.016f;

        auto ssgiOutput = ssgiPass_->AddPass(graph, normalHandle, depthHandle,
            velocityHandle, hzbHandle, prevColorHandle,
            cameraData, currentBufferIndex,
            hzbSystem_->GetMipLevels());

        ssgiOutputHandle = ssgiOutput.ssgi_output;

        // Store current frame color for next frame's SSGI ray hit sampling
        struct ColorHistoryData {
            rendergraph::RGResourceHandle gpu_output;
        };

        graph.AddPass<ColorHistoryData>("ColorHistoryStore",
            graphics::rendergraph::RGPassType::Copy,
            graphics::rendergraph::RGPassCategory::Copy,
            [gpuOutputHandle](ColorHistoryData& data, graphics::rendergraph::RenderGraphBuilder& builder) {
                data.gpu_output = builder.Read(gpuOutputHandle, rhi::ResourceState::ShaderResource);
                builder.SideEffect(); // CRITICAL: Prevent render graph from culling this pass
            },
            [this](const ColorHistoryData& data, graphics::rendergraph::RenderGraphContext& context) {
                auto gpuOutputTex = gpuDrawPipeline_->GetFinalOutputTexture();
                if (gpuOutputTex == rhi::handles::INVALID_RESOURCE) return;

                bool ok = colorHistoryManager_->StoreCurrentFrameColor(
                    gpuOutputTex, context.cmdBuffer, frameCount_);

                if (frameCount_ % 60 == 1) {
                    std::cout << "[ColorHistoryStore] StoreCurrentFrameColor: frame=" << frameCount_
                              << " result=" << (ok ? "OK" : "FAILED") << std::endl;
                }
            }
        );
    }

    // === FINAL BLIT PASS (Following TestParticleSponza pattern) ===
    // CRITICAL: This pass MUST depend on SceneRender to ensure Nanite output is ready
    struct BlitPassData {
        rendergraph::RGResourceHandle input;
        rendergraph::RGResourceHandle ssgi_input;
        rendergraph::RGResourceHandle output;
    };

    graph.AddPass<BlitPassData>("FinalBlit",
        graphics::rendergraph::RGPassType::Graphics,
        graphics::rendergraph::RGPassCategory::PostProcess,
        [this, backBufferHandle, gpuOutputHandle, ssgiOutputHandle](BlitPassData& data, graphics::rendergraph::RenderGraphBuilder& builder) {
            // Always read scene color as primary input
            data.input = builder.Read(gpuOutputHandle, rhi::ResourceState::ShaderResource);

            // Read SSGI output when in composite or SSGI-only mode
            // ssgiVisMode_: 0=Composite, 1=SSGI only, 2=Scene only
            bool needSSGI = (ssgiVisMode_ != 2) &&
                            ssgiOutputHandle.IsValid() &&
                            blit_composite_pipeline_ != rhi::handles::INVALID_PIPELINE;
            if (needSSGI) {
                data.ssgi_input = builder.Read(ssgiOutputHandle, rhi::ResourceState::ShaderResource);
            } else {
                data.ssgi_input = rendergraph::RGResourceHandle{};
            }

            data.output = builder.Write(backBufferHandle, rhi::ResourceState::RenderTarget);

            graphics::rendergraph::RGRenderPassDesc rpDesc;
            rpDesc.colors.push_back({
                .texture = data.output,
                .loadOp = rhi::LoadAction::DontCare,
                .storeOp = rhi::StoreAction::Store,
                .clearColor = { primal::math::v4{0,0,0,1} }
            });
            builder.DeclareRenderPass(rpDesc);
        },
        [this](const BlitPassData& data, graphics::rendergraph::RenderGraphContext& context) {
            auto cmd = context.cmdBuffer;

            cmd->SetViewport({ {0, 0}, {static_cast<float>(renderWidth_), static_cast<float>(renderHeight_)}, 0, 1 });
            cmd->SetScissor({ {0, 0}, {renderWidth_, renderHeight_} });

            // Get scene color texture
            auto inputResource = context.graph->GetResource(data.input);
            if (!inputResource) return;
            auto inputHandle = inputResource->GetPhysicalHandle();
            if (inputHandle == rhi::handles::INVALID_RESOURCE) return;

            // Mode 2: Scene only — simple blit of scene color
            if (ssgiVisMode_ == 2 || !data.ssgi_input.IsValid()) {
                DescriptorData blit_params[1] = {
                    { .binding = 0, .type = DescriptorType::SampledImage, .resource = inputHandle }
                };
                UpdateDescriptorSet(device_, blit_descriptor_set_, blit_params, 1);
                cmd->BindGraphicsPipeline(blit_pipeline_);
                const rhi::DescriptorSetHandle descriptor_sets[] = { blit_descriptor_set_ };
                cmd->BindDescriptorSets(rhi::PipelineBindPoint::Graphics, blit_layout_, 0, 1, descriptor_sets, 0, nullptr);
                cmd->Draw(3, 0, 1, 0);
                return;
            }

            // Get SSGI texture
            auto ssgiResource = context.graph->GetResource(data.ssgi_input);
            if (!ssgiResource) {
                // Fallback to scene only
                DescriptorData blit_params[1] = {
                    { .binding = 0, .type = DescriptorType::SampledImage, .resource = inputHandle }
                };
                UpdateDescriptorSet(device_, blit_descriptor_set_, blit_params, 1);
                cmd->BindGraphicsPipeline(blit_pipeline_);
                const rhi::DescriptorSetHandle descriptor_sets[] = { blit_descriptor_set_ };
                cmd->BindDescriptorSets(rhi::PipelineBindPoint::Graphics, blit_layout_, 0, 1, descriptor_sets, 0, nullptr);
                cmd->Draw(3, 0, 1, 0);
                return;
            }
            auto ssgiHandle = ssgiResource->GetPhysicalHandle();
            if (ssgiHandle == rhi::handles::INVALID_RESOURCE) return;

            // Mode 1: SSGI only — blit SSGI filter output directly
            if (ssgiVisMode_ == 1) {
                DescriptorData blit_params[1] = {
                    { .binding = 0, .type = DescriptorType::SampledImage, .resource = ssgiHandle }
                };
                UpdateDescriptorSet(device_, blit_descriptor_set_, blit_params, 1);
                cmd->BindGraphicsPipeline(blit_pipeline_);
                const rhi::DescriptorSetHandle descriptor_sets[] = { blit_descriptor_set_ };
                cmd->BindDescriptorSets(rhi::PipelineBindPoint::Graphics, blit_layout_, 0, 1, descriptor_sets, 0, nullptr);
                cmd->Draw(3, 0, 1, 0);
                return;
            }

            // Mode 0: Composite (default) — scene + SSGI via composite pipeline
            {
                DescriptorData composite_params[2] = {
                    { .binding = 0, .type = DescriptorType::SampledImage, .resource = inputHandle },
                    { .binding = 1, .type = DescriptorType::SampledImage, .resource = ssgiHandle }
                };
                UpdateDescriptorSet(device_, blit_composite_descriptor_set_, composite_params, 2);
                cmd->BindGraphicsPipeline(blit_composite_pipeline_);
                const rhi::DescriptorSetHandle sets[] = { blit_composite_descriptor_set_ };
                cmd->BindDescriptorSets(rhi::PipelineBindPoint::Graphics, blit_composite_layout_, 0, 1, sets, 0, nullptr);
                cmd->Draw(3, 0, 1, 0);
            }
        }
    );
}

void TestNaniteStreamingPipeline::ProcessStreamingFeedback() {
    if (!testConfig_.enable_streaming || !streamingManager_) {
        return;
    }

    auto startTime = std::chrono::high_resolution_clock::now();

    streamingManager_->ProcessRequests(renderSystem_.GetCurrentFrameIndex());

    auto endTime = std::chrono::high_resolution_clock::now();
    float processingTime = std::chrono::duration<float, std::milli>(endTime - startTime).count();

    testResults_.avg_processing_time_ms.store(
        (testResults_.avg_processing_time_ms.load() * 0.9f) + (processingTime * 0.1f)
    );

    auto stats = streamingManager_->GetStats();
    testResults_.clusters_streamed.store(stats.total_clusters_streamed);
    testResults_.clusters_evicted.store(stats.total_clusters_evicted);
    testResults_.requests_processed.store(stats.pending_requests_count);
}

void TestNaniteStreamingPipeline::RecordTestMetrics() {
    if (frameCount_ == 0) {
        std::cout << "\n=== Streaming Metrics (Frame " << frameCount_ << ") ===" << std::endl;
        std::cout << "  Avg Processing Time: " << testResults_.avg_processing_time_ms.load() << " ms" << std::endl;

        auto stats = streamingManager_->GetStats();
        std::cout << "  Resident Clusters: " << stats.current_resident_clusters << std::endl;
        std::cout << "  Pool Usage: " << (stats.page_pool_usage * 100.0f) << "%" << std::endl;
        std::cout << "  Pending Requests: " << stats.pending_requests_count << std::endl;
    }
}

void TestNaniteStreamingPipeline::ValidateResults() {
    auto stats = streamingManager_->GetStats();

    if (stats.eviction_count > 0) {
        std::cout << "✓ LRU eviction working correctly" << std::endl;
    }

    if (stats.total_clusters_streamed > 0) {
        std::cout << "✓ Cluster streaming working correctly" << std::endl;
    }

    if (testResults_.requests_processed.load() > 0) {
        std::cout << "✓ Request processing working correctly" << std::endl;
    }

    std::cout << "\nValidation Summary:" << std::endl;
    std::cout << "  Total Streamed: " << testResults_.clusters_streamed.load() << std::endl;
    std::cout << "  Total Evicted: " << testResults_.clusters_evicted.load() << std::endl;
    std::cout << "  Total Requests: " << testResults_.requests_processed.load() << std::endl;
}

void TestNaniteStreamingPipeline::TestPerformance() {
    const u32 numIterations = 1000;
    auto startTime = std::chrono::high_resolution_clock::now();

    for (u32 i = 0; i < numIterations; ++i) {
        ProcessStreamingFeedback();
    }

    auto endTime = std::chrono::high_resolution_clock::now();
    float totalTime = std::chrono::duration<float, std::milli>(endTime - startTime).count();
    float avgTime = totalTime / numIterations;

    std::cout << "Performance Results:" << std::endl;
    std::cout << "  Total Time: " << totalTime << " ms" << std::endl;
    std::cout << "  Avg Time/Frame: " << avgTime << " ms" << std::endl;
    std::cout << "  Target: < 1.0 ms/frame" << std::endl;

    if (avgTime < 1.0f) {
        std::cout << "  ✓ PASS - Performance within target" << std::endl;
    } else {
        std::cout << "  ✗ FAIL - Performance exceeds target" << std::endl;
    }
}

void TestNaniteStreamingPipeline::Shutdown() {
    if (isShutdown_) return;
    isShutdown_ = true;

    std::cout << "\nTestNaniteStreamingPipeline Final Results:" << std::endl;
    std::cout << "  Total Frames: " << frameCount_ << std::endl;
    std::cout << "  Clusters Streamed: " << testResults_.clusters_streamed.load() << std::endl;
    std::cout << "  Clusters Evicted: " << testResults_.clusters_evicted.load() << std::endl;
    std::cout << "  Requests Processed: " << testResults_.requests_processed.load() << std::endl;
    std::cout << "  Avg Processing Time: " << testResults_.avg_processing_time_ms.load() << " ms/frame" << std::endl;

    // Print final frame culling debug data at shutdown
    PrintFinalFrameCullingDebugData();

    if (device_) {
        device_->WaitIdle();
    }

    sceneSnapshot_.Shutdown();
    extractionSystem_.reset();

    if (hzbSystem_) {
        hzbSystem_->Shutdown();
        hzbSystem_.reset();
    }

    if (visibilityBufferSystem_) {
        visibilityBufferSystem_->Shutdown();
        visibilityBufferSystem_.reset();
    }

    if (streamingManager_) {
        streamingManager_->Shutdown();
        delete streamingManager_;
        streamingManager_ = nullptr;
    }

    if (gpuDrawPipeline_) {
        gpuDrawPipeline_->Shutdown();
    }

    if (cullingPipeline_) {
        cullingPipeline_->Shutdown();
    }

    if (resourceManager_) {
        resourceManager_->Shutdown();
    }

    // Cleanup GPU Material Registry
    if (gpuMaterialRegistry_) {
        // Wait for async build job to complete before destroying registry
        if (materialBuildJob_.IsValid()) {
            std::cout << "[TestNanite] Waiting for material build job to complete..." << std::endl;
            materialBuildJob_.Wait();
        }
        gpuMaterialRegistry_.reset();
    }

    // Cleanup SSGI resources
    if (ssgiPass_) {
        ssgiPass_->Shutdown();
        ssgiPass_.reset();
    }
    if (colorHistoryManager_) {
        colorHistoryManager_->Shutdown();
        colorHistoryManager_.reset();
    }

    renderGraph_.reset();
    renderSystem_.Shutdown();

    // IMPORTANT: Shutdown content systems BEFORE device destruction
    // GPU meshes need to be destroyed while the device is still valid
    primal::content::shutdown();
    primal::content::AsyncResourceLoader::Shutdown();

    if (device_) {
        device_->GetGarbageCollector().Flush();
    }

    if (window_.is_valid()) {
        platform::remove_window(window_.get_id());
    }

    primal::jobsystem::JobSystem::Shutdown();

    std::cout << "TestNaniteStreamingPipeline::Shutdown End" << std::endl;
}

void TestNaniteStreamingPipeline::Resize(u32 width, u32 height) {
    renderWidth_ = width;
    renderHeight_ = height;
    renderSystem_.Resize(width, height);
}

TestNaniteStreamingPipeline::~TestNaniteStreamingPipeline() {
    Shutdown();
}

void TestNaniteStreamingPipeline::TestStreamingInitialization() {
    std::cout << "\n=== Test: Streaming Initialization ===" << std::endl;

    if (streamingManager_) {
        std::cout << "✓ Streaming manager initialized" << std::endl;
    } else {
        std::cout << "✗ Streaming manager NOT initialized" << std::endl;
    }

    if (cullingPipeline_) {
        std::cout << "✓ Culling pipeline initialized" << std::endl;
    } else {
        std::cout << "✗ Culling pipeline NOT initialized" << std::endl;
    }

    (void)streamingManager_->GetStats(); // Suppress unused variable warning
    auto config = streamingManager_->GetConfig();
    std::cout << "  Pool Size: " << (config.page_pool_size_bytes / (1024 * 1024)) << " MB" << std::endl;
    std::cout << "  Max Requests/Frame: " << config.max_requests_per_frame << std::endl;
}

void TestNaniteStreamingPipeline::TestGPURequestGeneration() {
    std::cout << "\n=== Test: GPU Request Generation ===" << std::endl;
    std::cout << "⚠ Not yet implemented - requires streaming feedback shaders" << std::endl;
}

void TestNaniteStreamingPipeline::TestLRUEviction() {
    std::cout << "\n=== Test: LRU Eviction ===" << std::endl;

    for (u32 i = 0; i < 1000; ++i) {
        ProcessStreamingFeedback();
    }

    auto stats = streamingManager_->GetStats();
    if (stats.eviction_count > 0) {
        std::cout << "✓ LRU eviction triggered" << std::endl;
        std::cout << "  Evictions: " << stats.eviction_count << std::endl;
    } else {
        std::cout << "✗ No evictions occurred" << std::endl;
    }
}

void TestNaniteStreamingPipeline::TestResidencyBuffer() {
    std::cout << "\n=== Test: Residency Buffer ===" << std::endl;

    auto residencyBuffer = streamingManager_->GetResidencyBuffer();
    if (residencyBuffer != rhi::handles::INVALID_RESOURCE) {
        std::cout << "✓ Residency buffer allocated" << std::endl;
    } else {
        std::cout << "✗ Residency buffer NOT allocated" << std::endl;
    }

    auto requestBuffer = streamingManager_->GetRequestBuffer();
    if (requestBuffer != rhi::handles::INVALID_RESOURCE) {
        std::cout << "✓ Request buffer allocated" << std::endl;
    } else {
        std::cout << "✗ Request buffer NOT allocated" << std::endl;
    }

    auto feedbackBuffer = streamingManager_->GetFeedbackBuffer();
    if (feedbackBuffer != rhi::handles::INVALID_RESOURCE) {
        std::cout << "✓ Feedback buffer allocated" << std::endl;
    } else {
        std::cout << "✗ Feedback buffer NOT allocated" << std::endl;
    }
}

void TestNaniteStreamingPipeline::TestEndToEndStreaming() {
    std::cout << "\n=== Test: End-to-End Streaming ===" << std::endl;

    u32 initialStreamed = testResults_.clusters_streamed.load();

    for (u32 i = 0; i < 300; ++i) {
        Run();
    }

    u32 finalStreamed = testResults_.clusters_streamed.load();

    if (finalStreamed > initialStreamed) {
        std::cout << "✓ Clusters streamed: " << (finalStreamed - initialStreamed) << std::endl;
    } else {
        std::cout << "✗ No clusters streamed" << std::endl;
    }
}

void TestNaniteStreamingPipeline::PrintAllInstanceBounds() {
    // 🔥 COMMENTED OUT: Reduce log noise, instance data has been validated
    // std::cout << "\n=== All Instance Bounds Information ===" << std::endl;

    const auto& proxies = scene_.GetProxies();
    // std::cout << "Total proxies in scene: " << proxies.size() << std::endl;

    u32 validInstanceCount = 0;
    u32 totalClusterCount = 0;

    for (size_t i = 0; i < proxies.size(); ++i) {
        const auto& proxy = proxies[i];

        // Get cluster component to access geometry data
        const cluster::component_cache* cluster_cache = cluster::get(proxy.meshId);
        if (!cluster_cache || !cluster_cache->exists) {
            // std::cout << "  [" << i << "] Invalid cluster component" << std::endl;
            continue;
        }

        // Get resource from NaniteResourceManager
        auto& resource_manager = nanite::NaniteResourceManager::Get();
        nanite::NaniteRuntimeResource* resource =
            resource_manager.GetOrCreateResource(cluster_cache->geometry_content_id);

        if (!resource || !resource->gpu_mesh) {
            // std::cout << "  [" << i << "] No GPU mesh resource" << std::endl;
            continue;
        }

        validInstanceCount++;
        totalClusterCount += resource->cluster_data.cluster_count;
    }

    // Only print summary
    std::cout << "\n=== Scene Summary ===" << std::endl;
    std::cout << "  Valid Instances: " << validInstanceCount << " out of " << proxies.size() << std::endl;
    std::cout << "  Total Clusters: " << totalClusterCount << std::endl;
    std::cout << "=== End Summary ===\n" << std::endl;
}

void TestNaniteStreamingPipeline::PrintFinalFrameCullingDebugData() {
    if (finalFrameCullingDebugData_.empty()) {
        std::cout << "\n=== No Final Frame Culling Debug Data ===" << std::endl;
        return;
    }

    std::cout << "\n=== Final Frame GPU Culling Debug Data (Frame " << frameCount_ << ", "
              << finalFrameCullingDebugData_.size() << " entries) ===" << std::endl;

    // Group by culling reason for better analysis
    u32 frustumCulled = 0;
    u32 distanceCulled = 0;
    u32 backfaceCulled = 0;  // 🔥 NEW: Backface culling counter
    u32 notCulled = 0;
    u32 unknownCulled = 0;

    for (const auto& entry : finalFrameCullingDebugData_) {
        switch (entry.culling_reason) {
            case 0: frustumCulled++; break;
            case 1: distanceCulled++; break;
            case 2: notCulled++; break;
            case 3: backfaceCulled++; break;  // 🔥 NEW: Backface culled
            default: unknownCulled++; break;
        }
    }

    std::cout << "Final Frame Culling Summary:" << std::endl;
    std::cout << "  Frustum Culled: " << frustumCulled << std::endl;
    std::cout << "  Distance Culled: " << distanceCulled << std::endl;
    std::cout << "  Backface Culled: " << backfaceCulled << std::endl;  // 🔥 NEW: Backface culling output
    std::cout << "  Not Culled: " << notCulled << std::endl;
    std::cout << "  Unknown: " << unknownCulled << std::endl;

    // Show all entries with detailed information
    std::cout << "\nDetailed culling data for all entries:" << std::endl;

    for (u32 i = 0; i < finalFrameCullingDebugData_.size(); ++i) {
        if( i > 500) break;
        const auto& entry = finalFrameCullingDebugData_[i];
        const char* culling_reason_str =
            entry.culling_reason == 0 ? "Frustum" :
            entry.culling_reason == 1 ? "Distance" :
            entry.culling_reason == 2 ? "None" :
            entry.culling_reason == 3 ? "Backface" : "Unknown";  // 🔥 NEW: Backface culling

        // Convert culling plane to string
        const char* plane_str = "None";
        if (entry.culling_plane != 0xFFFFFFFF) {
            switch (entry.culling_plane) {
                case 0: plane_str = "Left"; break;
                case 1: plane_str = "Right"; break;
                case 2: plane_str = "Bottom"; break;
                case 3: plane_str = "Top"; break;
                case 4: plane_str = "Near"; break;
                case 5: plane_str = "Far"; break;
                default: plane_str = "Unknown"; break;
            }
        }

        std::cout << "  [" << i << "] Instance=" << entry.instance_id
                 << ", Cluster=" << entry.cluster_id
                 << ", Reason=" << culling_reason_str
                 << ", Plane=" << plane_str
                 << ", ViewZ=" << entry.view_space_z
                 << ", Distance=" << entry.distance_to_camera
                 << ", BoundsRadius=" << entry.bounds_radius
                 << ", Visible=" << (entry.is_visible ? "Yes" : "No");

        // 🔥 NEW: Show backface culling specific data
        if (entry.meshlet_id > 0 || entry.is_backface_culled) {
            std::cout << "\n      Backface: [MeshletID=" << entry.meshlet_id
                     << ", CosAngle=" << entry.backface_cos_angle
                     << ", Cutoff=" << entry.backface_cutoff
                     << ", Culled=" << (entry.is_backface_culled ? "Yes" : "No") << "]";
        }

        // Show plane distances for debugging
        std::cout << "\n      PlaneDistances=[L:" << entry.plane_distances[0]
                 << ",R:" << entry.plane_distances[1]
                 << ",B:" << entry.plane_distances[2]
                 << ",T:" << entry.plane_distances[3]
                 << ",N:" << entry.plane_distances[4]
                 << ",F:" << entry.plane_distances[5] << "]"
                 << std::endl;
    }

    std::cout << "=== End Final Frame Culling Debug Data ===\n" << std::endl;
}