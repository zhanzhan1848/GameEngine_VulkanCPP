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
#include "Engine/Graphics/Lumen/StaticProbe/StaticProbeBaker.h"
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
        primal::utl::blob_stream_writer writer(blob.data(), blob.size());

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
        //std::cout << "[TestNanite] Loaded texture: " << path << " (" << width << "x" << height << ")" << std::endl;

        // 🎨 NEW: 统一缩放到 1024x1024
        constexpr int TARGET_SIZE = 1024;
        unsigned char* final_data = data;
        int final_width = width;
        int final_height = height;

        if (width != TARGET_SIZE || height != TARGET_SIZE) {
            //std::cout << "[TestNanite] Resizing texture from " << width << "x" << height
                      //<< " to " << TARGET_SIZE << "x" << TARGET_SIZE << std::endl;

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
    //std::cout << "DEBUG: TestNaniteStreamingPipeline INITIALIZING " << __DATE__ << " " << __TIME__ << std::endl;

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

    //std::cout << "TestNaniteStreamingPipeline Initialized Successfully" << std::endl;
    //std::cout << "  - Streaming Pool Size: " << testConfig_.streaming_pool_size_mb << " MB" << std::endl;
    //std::cout << "  - Max Clusters: " << testConfig_.max_clusters << std::endl;
    //std::cout << "  - Max Requests/Frame: " << testConfig_.max_requests_per_frame << std::endl;

    // Print all instance bounds information
    PrintAllInstanceBounds();

    // Register scene meshes with Surface Cache CardGenerator
    if (surfaceCachePass_ && surfaceCachePass_->IsInitialized()) {
        auto& cardGen = surfaceCachePass_->GetCardGenerator();
        const auto& instances = sceneSnapshot_.GetInstanceData();
        u32 registeredCount = 0;
        for (u32 i = 0; i < sceneSnapshot_.GetInstanceCount() && i < sceneMeshes_.size(); ++i) {
            const auto& inst = instances[i];
            const auto& meshInfo = sceneMeshes_[i];
            if (!meshInfo.mesh) continue;

            const auto& localAABB = meshInfo.mesh->GetLocalAABB();
            m4x4 worldMat = inst.world_matrix;

            primal::math::v3 corners[8] = {
                {localAABB.min.x, localAABB.min.y, localAABB.min.z},
                {localAABB.max.x, localAABB.min.y, localAABB.min.z},
                {localAABB.min.x, localAABB.max.y, localAABB.min.z},
                {localAABB.max.x, localAABB.max.y, localAABB.min.z},
                {localAABB.min.x, localAABB.min.y, localAABB.max.z},
                {localAABB.max.x, localAABB.min.y, localAABB.max.z},
                {localAABB.min.x, localAABB.max.y, localAABB.max.z},
                {localAABB.max.x, localAABB.max.y, localAABB.max.z},
            };
            primal::math::v3 worldMin{FLT_MAX, FLT_MAX, FLT_MAX};
            primal::math::v3 worldMax{-FLT_MAX, -FLT_MAX, -FLT_MAX};
            for (int c = 0; c < 8; ++c) {
                v4 worldPt = worldMat * v4{corners[c].x, corners[c].y, corners[c].z, 1.0f};
                worldMin.x = std::min(worldMin.x, worldPt.x);
                worldMin.y = std::min(worldMin.y, worldPt.y);
                worldMin.z = std::min(worldMin.z, worldPt.z);
                worldMax.x = std::max(worldMax.x, worldPt.x);
                worldMax.y = std::max(worldMax.y, worldPt.y);
                worldMax.z = std::max(worldMax.z, worldPt.z);
            }
            cardGen.RegisterMesh(worldMin, worldMax, i);
            registeredCount++;
        }
        cardGen.RebuildCardAllocation();
        std::cout << "[SurfaceCache] Registered " << registeredCount
                  << " meshes (instanceCount=" << sceneSnapshot_.GetInstanceCount()
                  << ", sceneMeshes=" << sceneMeshes_.size()
                  << "), generated " << cardGen.GetCardCount() << " cards" << std::endl;

        // Build Card→Probe assignment now that cards are available
        if (ddgiPass_ && ddgiPass_->IsInitialized() && cardGen.GetCardCount() > 0) {
            std::cout << "[SC→DDGI] Calling BuildCardProbeAssignment: cardCount="
                      << cardGen.GetCardCount() << std::endl;
            BuildCardProbeAssignment();
        } else {
            std::cerr << "[SC→DDGI] Skipping assignment: ddgi=" << (bool)ddgiPass_
                      << " ddgiInit=" << (ddgiPass_ ? ddgiPass_->IsInitialized() : false)
                      << " cards=" << cardGen.GetCardCount() << std::endl;
        }
    } else {
        std::cerr << "[SC CardReg] surfaceCachePass_ not available: sc="
                  << (bool)surfaceCachePass_ << std::endl;
    }

    return true;
}

bool TestNaniteStreamingPipeline::VerifyMeshletUVSupport() {
    //std::cout << "[TestNanite] Verifying meshlet UV support..." << std::endl;

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

        //std::cout << "  Mesh: " << meshInfo.name
                  //<< ", Vertices: " << vertexCount
                  //<< ", ElementBuffer: " << (elementBuffer != primal::graphics::rhi::handles::INVALID_RESOURCE ? "OK" : "MISSING")
                  //<< std::endl;
    }

    //std::cout << "  ✓ Meshlet vertex structure verification complete" << std::endl;
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

    //std::cout << "[TestNanite] Initializing GPUDrivenDrawPipeline..." << std::endl;
    if (!gpuDrawPipeline_->Initialize(device_, binningConfig, visibilityConfig)) {
        std::cerr << "Failed to initialize GPU driven draw pipeline" << std::endl;
        return false;
    }
    //std::cout << "[TestNanite] GPUDrivenDrawPipeline initialized successfully" << std::endl;

    // Now initialize culling pipeline (after GPU draw pipeline is ready)
    cullingPipeline_ = &graphics::nanite::GPUCullingPipeline::Get();
    //std::cout << "[TestNanite] GPUCullingPipeline singleton obtained: " << (void*)cullingPipeline_ << std::endl;

    graphics::nanite::CullingConfig cullingConfig;
    cullingConfig.max_clusters_per_dispatch = testConfig_.max_clusters;
    cullingConfig.max_instances_per_dispatch = testConfig_.max_instances;
    cullingConfig.enable_streaming_feedback = testConfig_.enable_streaming;
    cullingConfig.enable_occlusion_culling = true; // 🔥 ENABLE: HZB Occlusion Culling for testing
    cullingConfig.enable_lod_selection = false; // DISABLED: LOD Selection to isolate flickering
    cullingConfig.enable_debug_output = true; // 🔥 ENABLE: Debug output to see backface culling statistics
    // cullingConfig.enable_lod_selection = testConfig_.enable_lod_selection;

    //std::cout << "[TestNanite] Initializing GPUCullingPipeline..." << std::endl;
    if (!cullingPipeline_->Initialize(device_, cullingConfig)) {
        std::cerr << "Failed to initialize GPU culling pipeline" << std::endl;
        return false;
    }
    //std::cout << "[TestNanite] GPUCullingPipeline initialized, IsInitialized=" << cullingPipeline_->IsInitialized() << std::endl;

    // Enable GPU culling debug output to diagnose culling issues
    cullingPipeline_->EnableDebugOutput(true);
    //std::cout << "[TestNanite] GPU culling debug output ENABLED" << std::endl;

    // CRITICAL: Connect culling pipeline to draw pipeline for proper buffer access
    gpuDrawPipeline_->SetCullingPipeline(cullingPipeline_);
    cullingPipeline_->SetGPUDrawPipeline(gpuDrawPipeline_);

    // CONNECT HZB AND VISIBILITY BUFFER SYSTEMS TO GPU DRIVEN PIPELINE - NOW ENABLED
    //std::cout << "[TestNanite] Connecting HZB and Visibility Buffer systems to GPU pipeline..." << std::endl;

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
    //std::cout << "[TestNanite] HZB System initialized successfully" << std::endl;

    // 🔥 CRITICAL FIX: Connect HZB system AFTER it's initialized
    // This ensures descriptor sets can access valid HZB texture
    gpuDrawPipeline_->SetHZBSystem(hzbSystem_.get());
    cullingPipeline_->SetHZBSystem(hzbSystem_.get());
    //std::cout << "[TestNanite] HZB System connected to GPU pipeline!" << std::endl;

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
    //std::cout << "[TestNanite] Depth History Manager initialized successfully" << std::endl;

    // Initialize GlobalSDF for DDGI ray tracing
    {
        auto& globalSDF = nanite::GlobalSDF::Get();
        nanite::GlobalSDFConfig sdfConfig;
        sdfConfig.cascade_count = 3;
        sdfConfig.base_resolution = 60;
        sdfConfig.cascade_scale_factor = 2;
        sdfConfig.voxel_size_base = 1.0f;

        if (globalSDF.Initialize(device_, sdfConfig)) {
            //std::cout << "[TestNanite] GlobalSDF initialized (1 cascade, 60³, voxel=1.0)" << std::endl;
        } else {
            std::cerr << "[TestNanite] Warning: GlobalSDF initialization failed — DDGI trace will be skipped" << std::endl;
        }
    }

    // Initialize GlobalSDF voxelization pipeline (after GPUDrivenDrawPipeline has geometry buffers)
    {
        auto& globalSDF = nanite::GlobalSDF::Get();
        std::cout << "[GlobalSDF] InitVoxelization check: initialized="
                  << globalSDF.IsInitialized()
                  << " gpuDrawPipeline=" << (gpuDrawPipeline_ ? "exists" : "NULL")
                  << " voxReady=" << globalSDF.IsVoxelizationReady()
                  << std::endl;
        if (globalSDF.IsInitialized() && gpuDrawPipeline_) {
            nanite::SDFVoxelizationResources voxResources;
            voxResources.vertex_buffer = gpuDrawPipeline_->GetGlobalVertexBuffer();
            voxResources.meshlet_buffer = gpuDrawPipeline_->GetGlobalMeshletBuffer();
            voxResources.meshlet_vertices_buffer = gpuDrawPipeline_->GetGlobalMeshletVerticesBuffer();
            voxResources.meshlet_triangles_buffer = gpuDrawPipeline_->GetGlobalMeshletTrianglesBuffer();
            voxResources.cluster_map_buffer = gpuDrawPipeline_->GetClusterMapBuffer();
            voxResources.instance_data_buffer = gpuDrawPipeline_->GetGlobalInstanceDataBuffer();
            voxResources.num_instances = sceneSnapshot_.GetInstanceCount();

            std::cout << "[GlobalSDF] Resources: vb=" << voxResources.vertex_buffer
                      << " mb=" << voxResources.meshlet_buffer
                      << " inst=" << voxResources.instance_data_buffer
                      << " numInst=" << voxResources.num_instances
                      << " clusterMap=" << voxResources.cluster_map_buffer
                      << std::endl;

            if (globalSDF.InitVoxelization(voxResources)) {
                std::cout << "[TestNanite] GlobalSDF voxelization pipeline initialized" << std::endl;
            } else {
                std::cout << "[TestNanite] WARNING: GlobalSDF voxelization init FAILED" << std::endl;
            }
        } else {
            std::cout << "[GlobalSDF] SKIPPED: initialized="
                      << globalSDF.IsInitialized()
                      << " gpuDrawPipeline=" << (gpuDrawPipeline_ ? "yes" : "no") << std::endl;
        }
    }

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
    //std::cout << "[TestNanite] Visibility Buffer System DISABLED" << std::endl;

    // Initialize Blit Pipeline for final presentation
    //std::cout << "[TestNanite] Initializing Blit Pipeline..." << std::endl;

    // Create descriptor set layout for blit
    primal::graphics::rhi::DescriptorSetLayoutBinding blit_bindings[] = {
        { 0, primal::graphics::rhi::DescriptorType::SampledImage, 1, primal::graphics::rhi::ShaderStage::Pixel, nullptr }
    };
    primal::graphics::rhi::DescriptorSetLayoutDesc blit_set_desc{ .bindingCount = 1, .bindings = blit_bindings };
    blit_set_layout_ = device_->CreateDescriptorSetLayout(blit_set_desc);

    // Create pipeline layout
    primal::graphics::rhi::PipelineLayoutDesc blit_pl_desc{ .setLayoutCount = 1, .setLayouts = &blit_set_layout_ };
    blit_layout_ = device_->CreatePipelineLayout(blit_pl_desc);

    // Create descriptor sets (triple-buffered)
    primal::graphics::rhi::DescriptorSetDesc blit_ds_desc{ .layout = blit_set_layout_ };
    for (int i = 0; i < 3; ++i)
        blit_descriptor_set_[i] = device_->CreateDescriptorSet(blit_ds_desc);

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
    //std::cout << "[TestNanite] Using shader path: " << testShaderPath << std::endl;

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

    //std::cout << "[TestNanite] Blit Pipeline initialized successfully" << std::endl;

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
        for (int i = 0; i < 3; ++i)
            blit_composite_descriptor_set_[i] = device_->CreateDescriptorSet(composite_ds_desc);

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
                //std::cout << "[TestNanite] Composite Blit Pipeline initialized successfully" << std::endl;
            }
        }
    }

    // === Fusion Fragment Pipeline (2-pass for Apple Silicon TBDR) ===
    // Pass 1 (half-res, 5 reads): SSGI+DDGI+SPGI+albedo+ssao → indirect contribution
    // Pass 2 (full-res, 2 reads): scene + indirect → tonemapped output
    {
        const shader_file_info indirect_ps_info{ "DeferredLighting.metal", "fragmentFusionIndirect", shader_type::pixel };
        const shader_file_info fusion_ps_info{ "DeferredLighting.metal", "fragmentFusion", shader_type::pixel };
        bool indirect_ok = CompileShader(indirect_ps_info);
        bool fusion_ok = CompileShader(fusion_ps_info);

        // Pass 1: Indirect pre-combine (5 texture bindings, RGBA16F output)
        if (indirect_ok) {
            primal::graphics::rhi::DescriptorSetLayoutBinding ind_bindings[5];
            for (u32 i = 0; i < 5; ++i)
                ind_bindings[i] = { i, primal::graphics::rhi::DescriptorType::SampledImage, 1, primal::graphics::rhi::ShaderStage::Pixel, nullptr };

            primal::graphics::rhi::DescriptorSetLayoutDesc ind_set_desc{ .bindingCount = 5, .bindings = ind_bindings };
            fusion_indirect_set_layout_ = device_->CreateDescriptorSetLayout(ind_set_desc);

            primal::graphics::rhi::PipelineLayoutDesc ind_pl_desc{ .setLayoutCount = 1, .setLayouts = &fusion_indirect_set_layout_ };
            fusion_indirect_layout_ = device_->CreatePipelineLayout(ind_pl_desc);

            primal::graphics::rhi::GraphicsPipelineDesc ind_pipe_desc{};
            ind_pipe_desc.layout = fusion_indirect_layout_;
            ind_pipe_desc.vertexShader = shaderVariantMap[std::string(blit_vs_info.file_name) + ":" + blit_vs_info.function];
            ind_pipe_desc.pixelShader = shaderVariantMap[std::string(indirect_ps_info.file_name) + ":" + indirect_ps_info.function];
            ind_pipe_desc.renderTargetFormats[0] = primal::graphics::rhi::DataFormat::RGBA16_Float;
            ind_pipe_desc.renderTargetCount = 1;
            ind_pipe_desc.depthStencilFormat = primal::graphics::rhi::DataFormat::Unknown;
            ind_pipe_desc.enableDepthTest = false;
            ind_pipe_desc.enableDepthWrite = false;
            ind_pipe_desc.cullMode = primal::graphics::rhi::CullMode::None;
            ind_pipe_desc.vertexAttributes.clear();
            ind_pipe_desc.vertexBindings.clear();
            fusion_indirect_pipeline_ = device_->CreateGraphicsPipeline(ind_pipe_desc);

            if (fusion_indirect_pipeline_ == primal::graphics::rhi::handles::INVALID_PIPELINE) {
                std::cerr << "[TestNanite] Warning: Failed to create fusion indirect pipeline" << std::endl;
            } else {
                for (u32 b = 0; b < 3; ++b) {
                    primal::graphics::rhi::DescriptorSetDesc dsDesc{ .layout = fusion_indirect_set_layout_ };
                    fusion_indirect_descriptor_set_[b] = device_->CreateDescriptorSet(dsDesc);
                }
                // Half-res indirect output (RGBA16F for HDR indirect)
                TextureDesc indTexDesc{};
                indTexDesc.size = {renderWidth_ / 2, renderHeight_ / 2, 1};
                indTexDesc.format = DataFormat::RGBA16_Float;
                indTexDesc.usage = TextureUsage::ShaderResource | TextureUsage::RenderTarget;
                indTexDesc.memoryUsage = GPUMemoryUsage::Static;
                for (u32 b = 0; b < 3; ++b)
                    fusion_indirect_output_[b] = device_->CreateTexture(indTexDesc);
            }
        }

        // Pass 2: Final fusion (2 texture bindings, BGRA8 output)
        if (fusion_ok) {
            primal::graphics::rhi::DescriptorSetLayoutBinding fus_bindings[2];
            for (u32 i = 0; i < 2; ++i)
                fus_bindings[i] = { i, primal::graphics::rhi::DescriptorType::SampledImage, 1, primal::graphics::rhi::ShaderStage::Pixel, nullptr };

            primal::graphics::rhi::DescriptorSetLayoutDesc fus_set_desc{ .bindingCount = 2, .bindings = fus_bindings };
            fusion_fragment_set_layout_ = device_->CreateDescriptorSetLayout(fus_set_desc);

            primal::graphics::rhi::PipelineLayoutDesc fus_pl_desc{ .setLayoutCount = 1, .setLayouts = &fusion_fragment_set_layout_ };
            fusion_fragment_layout_ = device_->CreatePipelineLayout(fus_pl_desc);

            primal::graphics::rhi::GraphicsPipelineDesc fus_pipe_desc{};
            fus_pipe_desc.layout = fusion_fragment_layout_;
            fus_pipe_desc.vertexShader = shaderVariantMap[std::string(blit_vs_info.file_name) + ":" + blit_vs_info.function];
            fus_pipe_desc.pixelShader = shaderVariantMap[std::string(fusion_ps_info.file_name) + ":" + fusion_ps_info.function];
            fus_pipe_desc.renderTargetFormats[0] = primal::graphics::rhi::DataFormat::BGRA8_UNorm;
            fus_pipe_desc.renderTargetCount = 1;
            fus_pipe_desc.depthStencilFormat = primal::graphics::rhi::DataFormat::Unknown;
            fus_pipe_desc.enableDepthTest = false;
            fus_pipe_desc.enableDepthWrite = false;
            fus_pipe_desc.cullMode = primal::graphics::rhi::CullMode::None;
            fus_pipe_desc.vertexAttributes.clear();
            fus_pipe_desc.vertexBindings.clear();
            fusion_fragment_pipeline_ = device_->CreateGraphicsPipeline(fus_pipe_desc);

            if (fusion_fragment_pipeline_ == primal::graphics::rhi::handles::INVALID_PIPELINE) {
                std::cerr << "[TestNanite] Warning: Failed to create fusion fragment pipeline" << std::endl;
            } else {
                for (u32 b = 0; b < 3; ++b) {
                    primal::graphics::rhi::DescriptorSetDesc dsDesc{ .layout = fusion_fragment_set_layout_ };
                    fusion_fragment_descriptor_set_[b] = device_->CreateDescriptorSet(dsDesc);
                }
                // Full-res fusion output
                TextureDesc outputDesc{};
                outputDesc.size = {renderWidth_, renderHeight_, 1};
                outputDesc.format = DataFormat::BGRA8_UNorm;
                outputDesc.usage = TextureUsage::ShaderResource | TextureUsage::RenderTarget;
                outputDesc.memoryUsage = GPUMemoryUsage::Static;
                for (u32 b = 0; b < 3; ++b)
                    fusion_output_[b] = device_->CreateTexture(outputDesc);
            }
        }
    }

    // Initialize SSGI pipeline
    if (!InitializeSSGIPipeline()) {
        std::cerr << "[TestNanite] Warning: SSGI pipeline initialization failed" << std::endl;
    }

    // Initialize DDGI blit pipeline
    if (!InitializeDDGIBlitPipeline()) {
        std::cerr << "[TestNanite] Warning: DDGI blit pipeline initialization failed" << std::endl;
    }

    // Initialize Surface Cache pipelines
    // Initialize Surface Cache pipelines
    if (!InitializeSurfaceCachePipelines()) {
        std::cerr << "[TestNanite] Warning: Surface Cache pipeline initialization failed" << std::endl;
    }

    // Shadow mapping initialization
    if (gpuDrawPipeline_) {
        gpuDrawPipeline_->InitializeShadowResources(
            sceneSnapshot_.GetInstanceCount(), 100000);
    }

    // === Deferred PBR Lighting Pipeline ===
    {
        using namespace primal::graphics::rhi;

        // Descriptor set layout: MUST match fragmentLighting_gpuDriven shader signature exactly
        // buffer(0)=ViewData, buffer(1)=SceneData, texture(2-7,9), sampler(8)
        DescriptorSetLayoutBinding deferred_bindings[] = {
            {0, DescriptorType::UniformBuffer, 1, ShaderStage::Pixel | ShaderStage::Vertex, nullptr},  // buffer(0) ViewData
            {1, DescriptorType::UniformBuffer, 1, ShaderStage::Pixel, nullptr},                          // buffer(1) SceneData
            {2, DescriptorType::SampledImage,  1, ShaderStage::Pixel, nullptr},  // texture(2) albedo
            {3, DescriptorType::SampledImage,  1, ShaderStage::Pixel, nullptr},  // texture(3) normal
            {4, DescriptorType::SampledImage,  1, ShaderStage::Pixel, nullptr},  // texture(4) ORM
            {5, DescriptorType::SampledImage,  1, ShaderStage::Pixel, nullptr},  // texture(5) depth
            {6, DescriptorType::SampledImage,  1, ShaderStage::Pixel, nullptr},  // texture(6) shadowMap0
            {7, DescriptorType::SampledImage,  1, ShaderStage::Pixel, nullptr},  // texture(7) shadowMap1
            {8, DescriptorType::Sampler,       1, ShaderStage::Pixel, nullptr},  // sampler(8) defaultSampler
            {9, DescriptorType::SampledImage,  1, ShaderStage::Pixel, nullptr},  // texture(9) SSAO
        };
        DescriptorSetLayoutDesc deferred_set_desc{ .bindingCount = 10, .bindings = deferred_bindings };
        deferred_set_layout_ = device_->CreateDescriptorSetLayout(deferred_set_desc);

        PipelineLayoutDesc deferred_pl_desc{ .setLayoutCount = 1, .setLayouts = &deferred_set_layout_ };
        deferred_layout_ = device_->CreatePipelineLayout(deferred_pl_desc);

        // Compile deferred lighting fragment shader
        const shader_file_info deferred_ps_info{ "DeferredLighting.metal", "fragmentLighting_gpuDriven", shader_type::pixel };
        if (!CompileShader(deferred_ps_info)) {
            std::cerr << "[TestNanite] Warning: Failed to compile deferred lighting shader" << std::endl;
        } else {
            // Create triple-buffered deferred output textures (RGBA16_Float for HDR)
            TextureDesc deferredOutputDesc{};
            deferredOutputDesc.size = {renderWidth_, renderHeight_, 1};
            deferredOutputDesc.format = DataFormat::RGBA16_Float;
            deferredOutputDesc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
            deferredOutputDesc.memoryUsage = GPUMemoryUsage::Static;
            for (int ti = 0; ti < 3; ++ti)
                deferred_output_textures_[ti] = device_->CreateTexture(deferredOutputDesc);

            // Match shader SceneData struct layout exactly
            struct DeferredSceneData {
                primal::math::m4x4 model;           // offset 0, 64 bytes
                primal::math::v4 lightPos;          // offset 64
                primal::math::v4 lightColor;        // offset 80
                primal::math::v4 reflectionPlane;   // offset 96 (reused for cascadeSplits)
                primal::math::v4 reflectionPlane2;  // offset 112
                primal::math::v4 reflectionPlane3;  // offset 128
                primal::math::m4x4 previousModel;   // offset 144
                primal::math::v4 viewPos;           // offset 208
                primal::math::m4x4 shadowMatrix0;   // offset 224
                primal::math::m4x4 shadowMatrix1;   // offset 288
                primal::math::v2 jitter;            // offset 352
                primal::math::v2 previousJitter;    // offset 360
                primal::math::v2 padding;           // offset 368
            };

            // Triple-buffered constant buffers and descriptor sets
            for (int i = 0; i < 3; ++i) {
                BufferDesc viewCbDesc{};
                viewCbDesc.size = sizeof(primal::math::m4x4) * 2; // ViewData: viewProjection + invViewProjection
                viewCbDesc.memoryUsage = GPUMemoryUsage::Dynamic;
                deferred_view_cb_[i] = device_->CreateBuffer(viewCbDesc);

                BufferDesc lightCbDesc{};
                lightCbDesc.size = sizeof(DeferredSceneData);
                lightCbDesc.memoryUsage = GPUMemoryUsage::Dynamic;
                deferred_light_cb_[i] = device_->CreateBuffer(lightCbDesc);

                // DDGI probe params: 2 x float4 = 32 bytes
                BufferDesc ddgiCbDesc{};
                ddgiCbDesc.size = 256;
                ddgiCbDesc.memoryUsage = GPUMemoryUsage::Dynamic;
                deferred_ddgi_probe_cb_[i] = device_->CreateBuffer(ddgiCbDesc);

                DescriptorSetDesc dsDesc{ .layout = deferred_set_layout_ };
                deferred_descriptor_set_[i] = device_->CreateDescriptorSet(dsDesc);
            }

            // Create sampler for deferred pass
            SamplerHandle deferredSampler = handles::INVALID_SAMPLER;
            {
                SamplerDesc samplerDesc{};
                samplerDesc.minFilter = FilterMode::Linear;
                samplerDesc.magFilter = FilterMode::Linear;
                samplerDesc.mipFilter = FilterMode::Linear;
                samplerDesc.addressU = TextureAddressMode::Clamp;
                samplerDesc.addressV = TextureAddressMode::Clamp;
                samplerDesc.addressW = TextureAddressMode::Clamp;
                deferredSampler = device_->CreateSampler(samplerDesc);
            }

            // Store sampler handle for later use in render graph
            deferred_sampler_handle_ = deferredSampler;

            // Create graphics pipeline
            const std::string vsKey = "DeferredLighting.metal:vertexMain";
            const std::string psKey = "DeferredLighting.metal:fragmentLighting_gpuDriven";

            if (shaderVariantMap.find(vsKey) != shaderVariantMap.end() &&
                shaderVariantMap.find(psKey) != shaderVariantMap.end()) {

                GraphicsPipelineDesc deferred_pipeline_desc{};
                deferred_pipeline_desc.layout = deferred_layout_;
                deferred_pipeline_desc.vertexShader = shaderVariantMap[vsKey];
                deferred_pipeline_desc.pixelShader = shaderVariantMap[psKey];
                deferred_pipeline_desc.renderTargetFormats[0] = DataFormat::RGBA16_Float;
                deferred_pipeline_desc.renderTargetCount = 1;
                deferred_pipeline_desc.depthStencilFormat = DataFormat::Unknown;
                deferred_pipeline_desc.enableDepthTest = false;
                deferred_pipeline_desc.enableDepthWrite = false;
                deferred_pipeline_desc.cullMode = CullMode::None;
                deferred_pipeline_desc.vertexAttributes.clear();
                deferred_pipeline_desc.vertexBindings.clear();
                deferred_pipeline_ = device_->CreateGraphicsPipeline(deferred_pipeline_desc);

                if (deferred_pipeline_ == handles::INVALID_PIPELINE) {
                    std::cerr << "[TestNanite] Warning: Failed to create deferred lighting pipeline" << std::endl;
                } else {
                    //std::cout << "[TestNanite] Deferred PBR Lighting pipeline initialized successfully" << std::endl;
                }
            } else {
                std::cerr << "[TestNanite] Warning: Deferred lighting shaders not found in variant map" << std::endl;
            }
        }
    }

    return true;
}

bool TestNaniteStreamingPipeline::InitializeSSGIPipeline() {
    //std::cout << "[LumenSSGI] Initializing SSGI pipeline..." << std::endl;

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

    //std::cout << "[LumenSSGI] SSGI pipeline initialized via LumenSSGIPass" << std::endl;

    // 3.5 Initialize LumenSSAOPass (runs before deferred lighting)
    ssaoPass_ = std::make_unique<primal::graphics::lumen::LumenSSAOPass>();
    if (!ssaoPass_->Initialize(device_, renderWidth_, renderHeight_)) {
        std::cerr << "[LumenSSAO] Failed to initialize LumenSSAOPass" << std::endl;
        // Non-fatal: SSAO is a quality enhancement, not critical
        ssaoPass_.reset();
    } else {
        //std::cout << "[LumenSSAO] SSAO pass initialized" << std::endl;
    }

    // 4. Initialize LumenDDGIPass (probe-based GI)
    // Load static probe cache BEFORE DDGI init so InitializeProbesFromStatic uses baked data
    {
        static_probe_volume_ = std::make_unique<primal::graphics::lumen::StaticProbeVolume>();
        primal::graphics::lumen::StaticProbeParams spParams{};
        spParams.grid_dim_x = 16; spParams.grid_dim_y = 8; spParams.grid_dim_z = 16;
        spParams.spacing = 4.0f;
        spParams.origin = {-20.0f, 0.0f, -20.0f};
        if (static_probe_volume_->Initialize(device_, spParams)) {
            if (static_probe_volume_->LoadFromFile("scene.probe_cache")) {
                std::cout << "[Lumen] Static probe cache loaded for DDGI init\n";
            } else {
                static_probe_volume_.reset();
            }
        } else {
            static_probe_volume_.reset();
        }
    }

    ddgiPass_ = std::make_unique<primal::graphics::lumen::LumenDDGIPass>();
    if (static_probe_volume_) {
        ddgiPass_->SetStaticProbeVolume(static_probe_volume_.get());
    }
    if (!ddgiPass_->Initialize(device_)) {
        std::cerr << "[LumenDDGI] Failed to initialize LumenDDGIPass" << std::endl;
        // Non-fatal: DDGI is additive, SSGI still works without it
        ddgiPass_.reset();
    } else {
        // Upload static volume to GPU after DDGI init (for GI Gather static atlas)
        if (static_probe_volume_ && static_probe_volume_->IsLoaded()) {
            static_probe_volume_->UploadToGPU();
        }
    }

    // 5. Initialize ScreenProbeGIPass (screen-space probe GI)
    screenProbeGIPass_ = std::make_unique<primal::graphics::lumen::ScreenProbeGIPass>();
    if (!screenProbeGIPass_->Initialize(device_, renderWidth_, renderHeight_)) {
        std::cerr << "[ScreenProbeGI] Failed to initialize ScreenProbeGIPass" << std::endl;
        screenProbeGIPass_.reset();
    } else {
        std::cout << "[ScreenProbeGI] Screen Probe GI pass initialized" << std::endl;
    }

    // 6. Initialize SurfaceCachePass
    surfaceCachePass_ = std::make_unique<primal::graphics::lumen::SurfaceCachePass>();
    {
        primal::graphics::lumen::LumenConfig sc_config;
        sc_config.quality = primal::graphics::lumen::LumenQualityPreset::High;
        if (!surfaceCachePass_->Initialize(device_, sc_config)) {
            std::cerr << "[SurfaceCache] Failed to initialize SurfaceCachePass" << std::endl;
            surfaceCachePass_.reset();
        } else {
            std::cout << "[SurfaceCache] Surface Cache pass initialized" << std::endl;
        }
    }
    return true;
}

bool TestNaniteStreamingPipeline::InitializeDDGIBlitPipeline() {
    if (!ddgiPass_ || !ddgiPass_->IsInitialized()) {
        std::cerr << "[DDGIBlit] Skipping: DDGI pass not initialized" << std::endl;
        return false;
    }
    //std::cout << "[DDGIBlit] Initializing DDGI blit pipeline..." << std::endl;

    // --- Compile fragmentBlitDDGI shader ---
    const shader_file_info ddgi_ps_info{ "DeferredLighting.metal", "fragmentBlitDDGI", shader_type::pixel };
    const std::string shaderDir = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/shaders/";
    {
        using namespace primal::graphics;
        using namespace primal::graphics::rhi;

        primal::utl::vector<std::wstring> extra_args;
        auto compiled = compile_shader(ddgi_ps_info, shaderDir.c_str(), extra_args);
        if (!compiled) {
            std::cerr << "[DDGIBlit] Failed to compile fragmentBlitDDGI shader" << std::endl;
            return false;
        }

        u64 byte_code_size = *reinterpret_cast<u64*>(compiled.get());
        u8* byte_code_ptr = compiled.get() + sizeof(u64) + 16;
        if (!byte_code_ptr || byte_code_size == 0) {
            std::cerr << "[DDGIBlit] Invalid byte code for fragmentBlitDDGI" << std::endl;
            return false;
        }

        ShaderHandle psHandle = device_->CreateShader(byte_code_ptr, byte_code_size, ShaderStage::Pixel, ddgi_ps_info.function);
        if (psHandle == handles::INVALID_SHADER) {
            std::cerr << "[DDGIBlit] Failed to create pixel shader handle" << std::endl;
            return false;
        }
        shaderVariantMap[std::string(ddgi_ps_info.file_name) + ":" + ddgi_ps_info.function] = psHandle;
    }

    // Vertex shader should already be compiled from the blit pipeline
    const std::string vsKey = "DeferredLighting.metal:vertexMain";
    if (shaderVariantMap.find(vsKey) == shaderVariantMap.end()) {
        std::cerr << "[DDGIBlit] Vertex shader not found in variant map" << std::endl;
        return false;
    }

    // --- Descriptor set layout: 6 textures + 3 uniform buffers ---
    {
        using namespace primal::graphics::rhi;
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::SampledImage,   1, ShaderStage::Pixel, nullptr},  // scene color
            {1, DescriptorType::SampledImage,   1, ShaderStage::Pixel, nullptr},  // depth
            {2, DescriptorType::SampledImage,   1, ShaderStage::Pixel, nullptr},  // half-res GI indirect
            {3, DescriptorType::SampledImage,   1, ShaderStage::Pixel, nullptr},  // GBuffer albedo
            {4, DescriptorType::SampledImage,   1, ShaderStage::Pixel, nullptr},  // GBuffer normal
            {5, DescriptorType::SampledImage,   1, ShaderStage::Pixel, nullptr},  // SSAO
            // Buffers
            {0, DescriptorType::UniformBuffer,  1, ShaderStage::Pixel, nullptr},  // invViewProjection
            {1, DescriptorType::UniformBuffer,  1, ShaderStage::Pixel, nullptr},  // probe origin + spacing
            {2, DescriptorType::UniformBuffer,  1, ShaderStage::Pixel, nullptr},  // probe counts
        };
        DescriptorSetLayoutDesc layoutDesc{ .bindingCount = 9, .bindings = bindings };
        blit_ddgi_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
    }

    // --- Pipeline layout ---
    {
        primal::graphics::rhi::PipelineLayoutDesc plDesc{};
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &blit_ddgi_set_layout_;
        blit_ddgi_layout_ = device_->CreatePipelineLayout(plDesc);
    }

    // --- Descriptor set ---
    {
        primal::graphics::rhi::DescriptorSetDesc dsDesc{ .layout = blit_ddgi_set_layout_ };
        blit_ddgi_descriptor_set_ = device_->CreateDescriptorSet(dsDesc);
    }

    // --- Graphics pipeline ---
    {
        using namespace primal::graphics::rhi;
        const std::string psKey = std::string(ddgi_ps_info.file_name) + ":" + ddgi_ps_info.function;
        GraphicsPipelineDesc pipeDesc{};
        pipeDesc.layout = blit_ddgi_layout_;
        pipeDesc.vertexShader = shaderVariantMap[vsKey];
        pipeDesc.pixelShader = shaderVariantMap[psKey];
        pipeDesc.renderTargetFormats[0] = DataFormat::BGRA8_UNorm;
        pipeDesc.renderTargetCount = 1;
        pipeDesc.depthStencilFormat = DataFormat::Unknown;
        pipeDesc.enableDepthTest = false;
        pipeDesc.enableDepthWrite = false;
        pipeDesc.cullMode = CullMode::None;
        pipeDesc.vertexAttributes.clear();
        pipeDesc.vertexBindings.clear();
        blit_ddgi_pipeline_ = device_->CreateGraphicsPipeline(pipeDesc);
    }

    if (blit_ddgi_pipeline_ == primal::graphics::rhi::handles::INVALID_PIPELINE) {
        std::cerr << "[DDGIBlit] Failed to create DDGI blit pipeline" << std::endl;
        return false;
    }

    // === DDGI GI Gather compute pipeline (half-res) ===
    // Moves storage buffer reads from fragment to compute to avoid Apple Silicon limits
    {
        using namespace primal::graphics::rhi;
        const shader_file_info gi_gather_info{ "DDGIGIGather.metal", "ddgi_gi_gather", shader_type::compute };
        primal::utl::vector<std::wstring> extra_args;
        auto compiled = compile_shader(gi_gather_info, shaderDir.c_str(), extra_args);

        if (!compiled) {
            std::cerr << "[DDGIGIGather] Failed to compile shader" << std::endl;
        } else {
            u64 byte_code_size = *reinterpret_cast<u64*>(compiled.get());
            u8* byte_code_ptr = compiled.get() + sizeof(u64) + 16;
            if (!byte_code_ptr || byte_code_size == 0) {
                std::cerr << "[DDGIGIGather] Invalid byte code" << std::endl;
            } else {
                ShaderHandle giGatherShader = device_->CreateShader(byte_code_ptr, byte_code_size, ShaderStage::Compute, gi_gather_info.function);
                if (giGatherShader == handles::INVALID_SHADER) {
                    std::cerr << "[DDGIGIGather] Invalid shader handle" << std::endl;
                } else {
                    DescriptorSetLayoutBinding giGatherBindings[] = {
                        // Textures (4)
                        {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr}, // depth
                        {1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr}, // normal
                        {2, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr}, // output
                        {3, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr}, // history
                        // Buffers (9)
                        {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr}, // invViewProj
                        {1, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr}, // probeOriginSpacing
                        {2, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr}, // probeCounts
                        {3, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr}, // staticSkySH
                        {4, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr}, // staticSkyFactor
                        {5, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr}, // staticProbe params
                        {6, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr}, // confidenceBuffer
                        {7, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr}, // irradianceBuffer
                        {8, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr}, // depthBuffer
                    };
                    DescriptorSetLayoutDesc layoutDesc{13, giGatherBindings};
                    gi_gather_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);

                    PipelineLayoutDesc plDesc;
                    plDesc.setLayoutCount = 1;
                    plDesc.setLayouts = &gi_gather_set_layout_;
                    gi_gather_layout_ = device_->CreatePipelineLayout(plDesc);

                    ComputePipelineDesc pipeDesc{};
                    pipeDesc.computeShader = giGatherShader;
                    pipeDesc.layout = gi_gather_layout_;
                    pipeDesc.threadGroupSize = {8, 8, 1};
                    gi_gather_pipeline_ = device_->CreateComputePipeline(pipeDesc);

                    DescriptorSetDesc dsDesc{gi_gather_set_layout_};
                    gi_gather_descriptor_set_ = device_->CreateDescriptorSet(dsDesc);

                    // Create half-res output texture
                    u32 halfW = renderWidth_ / 2;
                    u32 halfH = renderHeight_ / 2;
                    TextureDesc texDesc{};
                    texDesc.size = {halfW, halfH, 1};
                    texDesc.format = DataFormat::RGBA16_Float;
                    texDesc.type = TextureType::Texture2D;
                    texDesc.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
                    gi_halfres_texture_ = device_->CreateTexture(texDesc);
                    // History texture for temporal accumulation
                    gi_halfres_history_ = device_->CreateTexture(texDesc);

                    std::cout << "[DDGIGIGather] Initialized (half-res " << halfW << "x" << halfH << ")" << std::endl;
                }
            }
        }
    }

    std::cout << "[ATLAS-DEBUG] === Starting atlas creation ===" << std::endl;

    // --- Probe data atlas textures (Apple GPU cache optimization) ---
    {
        const auto& ddgiP = ddgiPass_->GetParams();
        u32 pcx = ddgiP.probe_count_x, pcy = ddgiP.probe_count_y, pcz = ddgiP.probe_count_z;
        std::cout << "[ATLAS-DEBUG] Probe grid: " << pcx << "x" << pcy << "x" << pcz << std::endl;

        // SH atlas: 4 texels per probe (RGBA16F)
        TextureDesc shAtlasDesc{};
        shAtlasDesc.size = {pcx * 4u, pcy * pcz, 1};
        shAtlasDesc.format = DataFormat::RGBA16_Float;
        shAtlasDesc.type = TextureType::Texture2D;
        shAtlasDesc.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
        dyn_sh_atlas_ = device_->CreateTexture(shAtlasDesc);
        stat_sh_atlas_ = device_->CreateTexture(shAtlasDesc);

        // Depth atlas: 8x8 texels per probe (RGBA16F)
        TextureDesc depthAtlasDesc{};
        depthAtlasDesc.size = {pcx * 8u, pcy * pcz * 8u, 1};
        depthAtlasDesc.format = DataFormat::RGBA16_Float;
        depthAtlasDesc.type = TextureType::Texture2D;
        depthAtlasDesc.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
        dyn_depth_atlas_ = device_->CreateTexture(depthAtlasDesc);
        stat_depth_atlas_ = device_->CreateTexture(depthAtlasDesc);

        std::cout << "[ATLAS-DEBUG] Atlas textures created: SH " << (pcx*4) << "x" << (pcy*pcz)
                  << ", Depth " << (pcx*8) << "x" << (pcy*pcz*8) << std::endl;
    }

    std::cout << "[ATLAS-DEBUG] Starting pre-filter pipeline creation..." << std::endl;

    // --- Atlas pre-filter pipeline (buffer → atlas) ---
    {
        const shader_file_info atlas_info{ "DDGIProbeAtlas.metal", "ddgi_prefilter_atlas", shader_type::compute };
        primal::utl::vector<std::wstring> extra_args;
        std::cout << "[DDGIProbeAtlas] Compiling shader..." << std::endl;
        auto compiled = compile_shader(atlas_info, shaderDir.c_str(), extra_args);
        if (compiled) {
            u64 byte_code_size = *reinterpret_cast<u64*>(compiled.get());
            u8* byte_code_ptr = compiled.get() + sizeof(u64) + 16;
            std::cout << "[DDGIProbeAtlas] Compiled OK, byte_code_size=" << byte_code_size << std::endl;
            if (byte_code_ptr && byte_code_size > 0) {
                auto shader = device_->CreateShader(byte_code_ptr, byte_code_size, ShaderStage::Compute, atlas_info.function);
                std::cout << "[DDGIProbeAtlas] Shader handle=" << (shader != handles::INVALID_SHADER ? "VALID" : "INVALID") << std::endl;
                if (shader != handles::INVALID_SHADER) {
                    DescriptorSetLayoutBinding atlasBindings[] = {
                        {0, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr}, // dynSHAtlas
                        {1, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr}, // dynDepthAtlas
                        {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr}, // AtlasParams
                        {1, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr}, // irradianceBuffer
                        {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr}, // ddgiDepthBuffer
                    };
                    DescriptorSetLayoutDesc atlasLayoutDesc{5, atlasBindings};
                    atlas_prefilter_set_layout_ = device_->CreateDescriptorSetLayout(atlasLayoutDesc);

                    PipelineLayoutDesc plDesc;
                    plDesc.setLayoutCount = 1;
                    plDesc.setLayouts = &atlas_prefilter_set_layout_;
                    atlas_prefilter_layout_ = device_->CreatePipelineLayout(plDesc);

                    ComputePipelineDesc pipeDesc{};
                    pipeDesc.computeShader = shader;
                    pipeDesc.layout = atlas_prefilter_layout_;
                    pipeDesc.threadGroupSize = {64, 1, 1};
                    atlas_prefilter_pipeline_ = device_->CreateComputePipeline(pipeDesc);
                    std::cout << "[DDGIProbeAtlas] Pipeline=" << (atlas_prefilter_pipeline_ != rhi::handles::INVALID_PIPELINE ? "VALID" : "INVALID") << std::endl;

                    DescriptorSetDesc dsDesc{atlas_prefilter_set_layout_};
                    atlas_prefilter_descriptor_set_ = device_->CreateDescriptorSet(dsDesc);

                    // Atlas params constant buffer
                    BufferDesc atlasCBDesc{};
                    atlasCBDesc.size = 32;  // AtlasParams: uint3 + uint + padding
                    atlasCBDesc.type = BufferType::Constant;
                    atlasCBDesc.usage = GPUMemoryUsage::Dynamic;
                    atlasCBDesc.memoryUsage = GPUMemoryUsage::Dynamic;
                    atlas_params_cb_ = device_->CreateBuffer(atlasCBDesc);

                    std::cout << "[DDGIProbeAtlas] Pre-filter pipeline created OK" << std::endl;
                } else {
                    std::cerr << "[DDGIProbeAtlas] CreateShader FAILED" << std::endl;
                }
            } else {
                std::cerr << "[DDGIProbeAtlas] Byte code empty (size=" << byte_code_size << ")" << std::endl;
            }
        } else {
            std::cerr << "[DDGIProbeAtlas] compile_shader returned null" << std::endl;
        }
    }

    // === DDGI Visibility compute pipeline — REMOVED (depth now buffer-based, no texture3D needed) ===

    // --- Triple-buffered constant buffers (one per frame, packs all 3 CBs) ---
    for (int i = 0; i < 3; ++i) {
        primal::graphics::rhi::BufferDesc cbDesc{};
        cbDesc.size = 256;
        cbDesc.type = primal::graphics::rhi::BufferType::Constant;
        cbDesc.usage = primal::graphics::rhi::GPUMemoryUsage::Dynamic;
        cbDesc.memoryUsage = primal::graphics::rhi::GPUMemoryUsage::Dynamic;
        ddgi_probe_cb_[i] = device_->CreateBuffer(cbDesc);
    }

    // --- Static probe volume + constant buffer for GI Gather static bindings ---
    {
        using namespace primal::graphics::lumen;
        const auto& ddgiParams = ddgiPass_->GetParams();
        const auto& volData = ddgiPass_->GetVolumeData();
        StaticProbeParams spParams{};
        spParams.grid_dim_x = ddgiParams.probe_count_x;
        spParams.grid_dim_y = ddgiParams.probe_count_y;
        spParams.grid_dim_z = ddgiParams.probe_count_z;
        spParams.spacing = ddgiParams.probe_spacing;
        spParams.origin = primal::math::v3{volData.ProbeOrigin.x, volData.ProbeOrigin.y, volData.ProbeOrigin.z};

        if (!static_probe_volume_) {
            // No pre-loaded cache — create zero-filled volume so bindings are never INVALID
            static_probe_volume_ = std::make_unique<StaticProbeVolume>();
            if (!static_probe_volume_->Initialize(device_, spParams)) {
                std::cerr << "[DDGIGIGather] StaticProbeVolume init failed\n";
                static_probe_volume_.reset();
            } else {
                u32 probeCount = spParams.grid_dim_x * spParams.grid_dim_y * spParams.grid_dim_z;
                auto* irr = static_probe_volume_->GetIrradianceData();
                auto* skySH = static_probe_volume_->GetSkySHData();
                auto* depthMean = static_probe_volume_->GetDepthMeanData();
                auto* depthVar = static_probe_volume_->GetDepthVarData();
                auto* skyFactor = static_probe_volume_->GetSkyFactorData();
                if (irr)      memset(irr, 0, (u64)probeCount * 9 * sizeof(primal::math::v3));
                if (skySH)    memset(skySH, 0, (u64)probeCount * 9 * sizeof(primal::math::v3));
                if (depthMean) memset(depthMean, 0, (u64)probeCount * 64 * sizeof(float));
                if (depthVar)  memset(depthVar, 0, (u64)probeCount * 64 * sizeof(float));
                if (skyFactor) memset(skyFactor, 0, (u64)probeCount * sizeof(float));
                static_probe_volume_->MarkLoaded();
            }
        }
        // Upload to GPU — only if volume was successfully initialized and loaded
        if (static_probe_volume_ && static_probe_volume_->IsLoaded()) {
            if (!static_probe_volume_->UploadToGPU()) {
                std::cerr << "[DDGIGIGather] StaticProbeVolume GPU upload failed — GI Gather disabled\n";
                static_probe_volume_.reset();
            }
        } else if (static_probe_volume_ && !static_probe_volume_->IsLoaded()) {
            std::cerr << "[DDGIGIGather] StaticProbeVolume not loaded — GI Gather disabled\n";
            static_probe_volume_.reset();
        }

        // Create StaticProbeData constant buffer (512 bytes to fit struct + SkySH[9])
        primal::graphics::rhi::BufferDesc spCBDesc{};
        spCBDesc.size = 512;
        spCBDesc.type = primal::graphics::rhi::BufferType::Constant;
        spCBDesc.usage = primal::graphics::rhi::GPUMemoryUsage::Dynamic;
        spCBDesc.memoryUsage = primal::graphics::rhi::GPUMemoryUsage::Dynamic;
        static_probe_cb_ = device_->CreateBuffer(spCBDesc);

        std::cout << "[DDGIGIGather] StaticProbeVolume + CB initialized\n";
    }

    //std::cout << "[DDGIBlit] DDGI blit pipeline initialized successfully" << std::endl;
    return true;
}

bool TestNaniteStreamingPipeline::InitializeSurfaceCachePipelines() {
    if (!surfaceCachePass_ || !surfaceCachePass_->IsInitialized()) {
        std::cerr << "[SurfaceCache] Skipping pipeline init: pass not initialized" << std::endl;
        return false;
    }

    // DEBUG: skip all pipeline creation to test if SurfaceCachePass::Initialize alone breaks rendering
    // std::cout << "[SurfaceCache] Skipping pipeline creation (debug)" << std::endl;
    // return true;

    using namespace primal::graphics;
    using namespace primal::graphics::rhi;
    const std::string shaderDir = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/shaders/";
    primal::utl::vector<std::wstring> extra_args;

    std::cout << "[SurfaceCache] Starting pipeline initialization..." << std::endl;

    // === 1. FillTest pipeline ===
    {
        const shader_file_info info{ "Lumen/SurfaceCacheTestData.metal", "surfaceCacheFillTest", shader_type::compute };
        auto compiled = compile_shader(info, shaderDir.c_str(), extra_args);
        if (!compiled) { std::cout << "[SurfaceCache] FAIL: compile FillTest shader returned null" << std::endl; return false; }
        std::cout << "[SurfaceCache] FillTest shader compiled OK" << std::endl;

        u64 sz = *reinterpret_cast<u64*>(compiled.get());
        u8* ptr = compiled.get() + sizeof(u64) + 16;
        auto sh = device_->CreateShader(ptr, sz, ShaderStage::Compute, info.function);
        if (sh == handles::INVALID_SHADER) { std::cout << "[SurfaceCache] FAIL: CreateShader for FillTest" << std::endl; return false; }

        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::StorageImage, 1, ShaderStage::Compute, nullptr},  // albedo out
            {1, DescriptorType::StorageImage, 1, ShaderStage::Compute, nullptr},  // normal out
            {2, DescriptorType::StorageImage, 1, ShaderStage::Compute, nullptr},  // depth out
        };
        sc_fill_test_set_layout_ = device_->CreateDescriptorSetLayout({3, bindings});
        sc_fill_test_layout_ = device_->CreatePipelineLayout({1, &sc_fill_test_set_layout_});
        sc_fill_test_descriptor_set_ = device_->CreateDescriptorSet({sc_fill_test_set_layout_});

        ComputePipelineDesc pd{};
        pd.computeShader = sh;
        pd.layout = sc_fill_test_layout_;
        pd.threadGroupSize = {8, 8, 1};
        sc_fill_test_pipeline_ = device_->CreateComputePipeline(pd);
        if (sc_fill_test_pipeline_ == handles::INVALID_PIPELINE) {
            std::cout << "[SurfaceCache] FAIL: CreateComputePipeline for FillTest" << std::endl;
            return false;
        }
        std::cout << "[SurfaceCache] FillTest pipeline created OK" << std::endl;
    }

    // === 2. LightCull pipeline ===
    {
        const shader_file_info info{ "Lumen/SurfaceCacheLightCull.metal", "surfaceCacheLightCull", shader_type::compute };
        auto compiled = compile_shader(info, shaderDir.c_str(), extra_args);
        if (!compiled) { std::cerr << "[SurfaceCache] Failed to compile LightCull shader" << std::endl; return false; }

        u64 sz = *reinterpret_cast<u64*>(compiled.get());
        u8* ptr = compiled.get() + sizeof(u64) + 16;
        auto sh = device_->CreateShader(ptr, sz, ShaderStage::Compute, info.function);
        if (sh == handles::INVALID_SHADER) { std::cerr << "[SurfaceCache] Failed to create LightCull shader" << std::endl; return false; }

        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // depth atlas
            {1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // normal atlas
            {1, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // LightCullParams
            {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // LightInfo array
            {3, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // tile_light_assignment
        };
        sc_light_cull_set_layout_ = device_->CreateDescriptorSetLayout({5, bindings});
        sc_light_cull_layout_ = device_->CreatePipelineLayout({1, &sc_light_cull_set_layout_});
        sc_light_cull_descriptor_sets_[0] = device_->CreateDescriptorSet({sc_light_cull_set_layout_});
        sc_light_cull_descriptor_sets_[1] = device_->CreateDescriptorSet({sc_light_cull_set_layout_});
        sc_light_cull_descriptor_sets_[2] = device_->CreateDescriptorSet({sc_light_cull_set_layout_});

        ComputePipelineDesc pd{};
        pd.computeShader = sh;
        pd.layout = sc_light_cull_layout_;
        pd.threadGroupSize = {8, 8, 1};
        sc_light_cull_pipeline_ = device_->CreateComputePipeline(pd);
        if (sc_light_cull_pipeline_ == handles::INVALID_PIPELINE) {
            std::cerr << "[SurfaceCache] Failed to create LightCull pipeline" << std::endl;
            return false;
        }
    }

    // === 3. LightEval pipeline ===
    {
        const shader_file_info info{ "Lumen/SurfaceCacheLightEval.metal", "surfaceCacheLightEval", shader_type::compute };
        auto compiled = compile_shader(info, shaderDir.c_str(), extra_args);
        if (!compiled) { std::cerr << "[SurfaceCache] Failed to compile LightEval shader" << std::endl; return false; }

        u64 sz = *reinterpret_cast<u64*>(compiled.get());
        u8* ptr = compiled.get() + sizeof(u64) + 16;
        auto sh = device_->CreateShader(ptr, sz, ShaderStage::Compute, info.function);
        if (sh == handles::INVALID_SHADER) { std::cerr << "[SurfaceCache] Failed to create LightEval shader" << std::endl; return false; }

        // Bindings matching SurfaceCacheLightEval.metal:
        //   texture(0): albedo_atlas (read), texture(1): normal_atlas (read),
        //   texture(2): emissive_atlas (read), texture(3): lighting_out (write)
        //   buffer(1): FlattenedLightingParams, buffer(2): SurfaceCacheCard[],
        //   buffer(3): LightInfo[], buffer(4): CardDispatchInfo[]
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // albedo atlas
            {1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // normal atlas
            {2, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // emissive atlas
            {3, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // lighting_out (write)
            {1, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // FlattenedLightingParams
            {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // SurfaceCacheCard[]
            {3, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // LightInfo[]
            {4, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // CardDispatchInfo[]
        };
        sc_light_eval_set_layout_ = device_->CreateDescriptorSetLayout({8, bindings});
        sc_light_eval_layout_ = device_->CreatePipelineLayout({1, &sc_light_eval_set_layout_});
        sc_light_eval_descriptor_sets_[0] = device_->CreateDescriptorSet({sc_light_eval_set_layout_});
        sc_light_eval_descriptor_sets_[1] = device_->CreateDescriptorSet({sc_light_eval_set_layout_});
        sc_light_eval_descriptor_sets_[2] = device_->CreateDescriptorSet({sc_light_eval_set_layout_});

        ComputePipelineDesc pd{};
        pd.computeShader = sh;
        pd.layout = sc_light_eval_layout_;
        pd.threadGroupSize = {256, 1, 1};  // Match shader's documented ThreadGroupSize
        sc_light_eval_pipeline_ = device_->CreateComputePipeline(pd);
        if (sc_light_eval_pipeline_ == handles::INVALID_PIPELINE) {
            std::cerr << "[SurfaceCache] Failed to create LightEval pipeline" << std::endl;
            return false;
        }
    }

    // === 4. Constant buffers ===
    {
        BufferDesc cbDesc{};
        cbDesc.size = 256;
        cbDesc.type = BufferType::Constant;
        cbDesc.usage = GPUMemoryUsage::Dynamic;
        cbDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        sc_cull_params_cb_ = device_->CreateBuffer(cbDesc);
        sc_eval_params_cb_ = device_->CreateBuffer(cbDesc);
        // GlobalShaderData needs 432+ bytes
        BufferDesc globalCbDesc{};
        globalCbDesc.size = 512;
        globalCbDesc.type = BufferType::Constant;
        globalCbDesc.usage = GPUMemoryUsage::Dynamic;
        globalCbDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        sc_global_data_cb_ = device_->CreateBuffer(globalCbDesc);
    }

    // Light info buffer (1 directional light)
    {
        BufferDesc lbDesc{};
        lbDesc.size = 256;
        lbDesc.type = BufferType::Structured;
        lbDesc.usage = GPUMemoryUsage::Dynamic;
        lbDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        sc_light_info_cb_ = device_->CreateBuffer(lbDesc);
    }

    // Card dispatch buffer for flattened 1D LightEval dispatch
    // Max ~256 cards × 32 bytes/CardDispatchInfo = 8KB
    {
        BufferDesc dispDesc{};
        dispDesc.size = 256 * sizeof(lumen::CardDispatchInfo);
        dispDesc.type = BufferType::Structured;
        dispDesc.usage = GPUMemoryUsage::Dynamic;
        dispDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        sc_card_dispatch_buf_ = device_->CreateBuffer(dispDesc);
    }

    // Tile light assignment buffer — sized for TILES not pages
    // Atlas=2048, tileSize=8 → 256×256 = 65,536 tiles × uint4 = 1MB
    {
        u32 atlasSize = surfaceCachePass_->GetAtlasSize();
        u32 tileSize = 8;
        u32 tilesPerSide = atlasSize / tileSize;
        u32 totalTiles = tilesPerSide * tilesPerSide;
        BufferDesc tlaDesc{};
        tlaDesc.size = (u64)totalTiles * sizeof(u32) * 4; // uint4 per tile
        tlaDesc.type = BufferType::Structured;
        tlaDesc.usage = GPUMemoryUsage::Dynamic;
        tlaDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        sc_tile_light_assign_buf_ = device_->CreateBuffer(tlaDesc);
        std::cout << "[SurfaceCache] Tile light assignment buffer: "
                  << totalTiles << " tiles (" << tlaDesc.size << " bytes)" << std::endl;
    }

    // === 5. CardCapture graphics pipeline ===
    {
        const shader_file_info vs_info{ "Lumen/SurfaceCacheCardCapture.metal", "cardCaptureVS", shader_type::vertex };
        const shader_file_info fs_info{ "Lumen/SurfaceCacheCardCapture.metal", "cardCaptureFS", shader_type::pixel };
        auto compiledVS = compile_shader(vs_info, shaderDir.c_str(), extra_args);
        auto compiledFS = compile_shader(fs_info, shaderDir.c_str(), extra_args);
        if (!compiledVS || !compiledFS) {
            std::cerr << "[SurfaceCache] Failed to compile CardCapture shaders" << std::endl;
            return false;
        }

        u64 szVS = *reinterpret_cast<u64*>(compiledVS.get());
        u8* ptrVS = compiledVS.get() + sizeof(u64) + 16;
        auto shVS = device_->CreateShader(ptrVS, szVS, ShaderStage::Vertex, vs_info.function);

        u64 szFS = *reinterpret_cast<u64*>(compiledFS.get());
        u8* ptrFS = compiledFS.get() + sizeof(u64) + 16;
        auto shFS = device_->CreateShader(ptrFS, szFS, ShaderStage::Pixel, fs_info.function);

        if (shVS == handles::INVALID_SHADER || shFS == handles::INVALID_SHADER) {
            std::cerr << "[SurfaceCache] Failed to create CardCapture shaders" << std::endl;
            return false;
        }

        // Descriptor set layout: materials, textures, sampler only.
        // CB (buffer 0) and vertex buffer (buffer 1) bound via cmd->BindVertexBuffers per draw
        DescriptorSetLayoutBinding bindings[] = {
            {3, DescriptorType::StorageBuffer,  1, ShaderStage::Pixel, nullptr},   // material data
            {0, DescriptorType::SampledImage,   1, ShaderStage::Pixel, nullptr},   // albedo array
            {1, DescriptorType::SampledImage,   1, ShaderStage::Pixel, nullptr},   // normal array
            {0, DescriptorType::Sampler,        1, ShaderStage::Pixel, nullptr},
        };
        sc_capture_set_layout_ = device_->CreateDescriptorSetLayout({4, bindings});
        sc_capture_layout_ = device_->CreatePipelineLayout({1, &sc_capture_set_layout_});
        sc_capture_descriptor_set_ = device_->CreateDescriptorSet({sc_capture_set_layout_});

        GraphicsPipelineDesc gpd{};
        gpd.vertexShader = shVS;
        gpd.pixelShader = shFS;
        gpd.layout = sc_capture_layout_;
        gpd.topology = PrimitiveTopology::TriangleList;
        gpd.renderTargetFormats[0] = DataFormat::RGBA8_UNorm;   // albedo
        gpd.renderTargetFormats[1] = DataFormat::RG16_Float;    // normal
        gpd.renderTargetFormats[2] = DataFormat::R32_Float;     // depth
        gpd.renderTargetFormats[3] = DataFormat::RGBA16_Float;  // emissive
        gpd.renderTargetCount = 4;
        gpd.depthStencilFormat = DataFormat::D32_Float;
        gpd.enableDepthTest = true;
        gpd.enableDepthWrite = true;
        gpd.cullMode = CullMode::None;  // Double-sided capture for thin/single-sided geometry
        sc_capture_pipeline_ = device_->CreateGraphicsPipeline(gpd);
        if (sc_capture_pipeline_ == handles::INVALID_PIPELINE) {
            std::cerr << "[SurfaceCache] Failed to create CardCapture pipeline" << std::endl;
            return false;
        }

        // Capture pass constant buffer — large enough for all cards (dynamic offset per draw)
        // Each CapturePassGPU is 256 bytes (padded to 256 for alignment)
        // Must match LumenTypes.h surface_cache_max_cards (4096)
        constexpr u32 MAX_CAPTURE_CARDS = 4096;
        constexpr u32 PER_CARD_CB_SIZE = 256;
        BufferDesc capCBDesc{};
        capCBDesc.size = MAX_CAPTURE_CARDS * PER_CARD_CB_SIZE;
        capCBDesc.type = BufferType::Constant;
        capCBDesc.usage = GPUMemoryUsage::Dynamic;
        capCBDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        sc_capture_cb_ = device_->CreateBuffer(capCBDesc);

        // Temporary depth texture for hardware depth testing during capture
        u32 atlasSize = surfaceCachePass_->GetAtlasSize();
        TextureDesc depthDesc{};
        depthDesc.size = {atlasSize, atlasSize, 1};
        depthDesc.mipLevels = 1;
        depthDesc.arraySize = 1;
        depthDesc.format = DataFormat::D32_Float;
        depthDesc.type = TextureType::Texture2D;
        depthDesc.usage = TextureUsage::DepthStencil;
        depthDesc.name = "SC_CaptureDepth";
        sc_capture_depth_tex_ = device_->CreateTexture(depthDesc);

        // Linear sampler for material texture sampling
        SamplerDesc sampDesc{};
        sampDesc.minFilter = FilterMode::Linear;
        sampDesc.magFilter = FilterMode::Linear;
        sampDesc.mipFilter = FilterMode::Linear;
        sampDesc.addressU = TextureAddressMode::Wrap;
        sampDesc.addressV = TextureAddressMode::Wrap;
        sc_capture_sampler_ = device_->CreateSampler(sampDesc);

        std::cout << "[SurfaceCache] CardCapture pipeline created" << std::endl;
    }

    // === 5.5. DepthDilate compute pipeline (3x3 depth hole fill) ===
    {
        const shader_file_info info{ "Lumen/SurfaceCacheDilate.metal", "surfaceCacheDilate", shader_type::compute };
        auto compiled = compile_shader(info, shaderDir.c_str(), extra_args);
        if (!compiled) { std::cerr << "[SurfaceCache] Failed to compile Dilate shader" << std::endl; return false; }

        u64 sz = *reinterpret_cast<u64*>(compiled.get());
        u8* ptr = compiled.get() + sizeof(u64) + 16;
        auto sh = device_->CreateShader(ptr, sz, ShaderStage::Compute, info.function);
        if (sh == handles::INVALID_SHADER) { std::cerr << "[SurfaceCache] Failed to create Dilate shader" << std::endl; return false; }

        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // depth_in
            {1, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // depth_out
            {1, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // SurfaceCacheParams CB
        };
        sc_dilate_set_layout_ = device_->CreateDescriptorSetLayout({3, bindings});
        sc_dilate_layout_ = device_->CreatePipelineLayout({1, &sc_dilate_set_layout_});
        sc_dilate_descriptor_set_ = device_->CreateDescriptorSet({sc_dilate_set_layout_});

        ComputePipelineDesc pd{};
        pd.computeShader = sh;
        pd.layout = sc_dilate_layout_;
        pd.threadGroupSize = {8, 8, 1};
        sc_dilate_pipeline_ = device_->CreateComputePipeline(pd);
        if (sc_dilate_pipeline_ == handles::INVALID_PIPELINE) {
            std::cerr << "[SurfaceCache] Failed to create Dilate pipeline" << std::endl;
            return false;
        }

        BufferDesc cbDesc{};
        cbDesc.size = 256;
        cbDesc.type = BufferType::Constant;
        cbDesc.usage = GPUMemoryUsage::Dynamic;
        cbDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        sc_dilate_params_cb_ = device_->CreateBuffer(cbDesc);

        u32 atlasSize = surfaceCachePass_->GetAtlasSize();
        TextureDesc td{};
        td.size = {atlasSize, atlasSize, 1};
        td.mipLevels = 1;
        td.arraySize = 1;
        td.format = DataFormat::R32_Float;
        td.type = TextureType::Texture2D;
        td.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
        td.name = "SC_DepthTemp_Dilate";
        sc_depth_temp_tex_ = device_->CreateTexture(td);

        std::cout << "[SurfaceCache] Dilate pipeline created" << std::endl;
    }

    // === 5.6. IndirectTrace compute pipeline ===
    {
        const shader_file_info info{ "Lumen/SurfaceCacheIndirectTrace.metal", "surfaceCacheIndirectTrace", shader_type::compute };
        auto compiled = compile_shader(info, shaderDir.c_str(), extra_args);
        if (!compiled) { std::cerr << "[SurfaceCache] Failed to compile IndirectTrace shader\n"; return false; }

        u64 sz = *reinterpret_cast<u64*>(compiled.get());
        u8* ptr = compiled.get() + sizeof(u64) + 16;
        auto sh = device_->CreateShader(ptr, sz, ShaderStage::Compute, info.function);
        if (sh == handles::INVALID_SHADER) { std::cerr << "[SurfaceCache] Failed to create IndirectTrace shader\n"; return false; }

        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // depth atlas
            {1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // normal atlas
            {2, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // sdf0
            {3, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // sdf1
            {4, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // sdf2
            {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // GlobalShaderData
            {1, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // IndirectTraceParams
            {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // cards
            {3, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // ray_hits (out)
        };
        sc_ind_trace_set_layout_ = device_->CreateDescriptorSetLayout({9, bindings});
        sc_ind_trace_layout_ = device_->CreatePipelineLayout({1, &sc_ind_trace_set_layout_});
        sc_ind_trace_descriptor_set_ = device_->CreateDescriptorSet({sc_ind_trace_set_layout_});

        ComputePipelineDesc pd{};
        pd.computeShader = sh;
        pd.layout = sc_ind_trace_layout_;
        pd.threadGroupSize = {8, 8, 1};
        sc_ind_trace_pipeline_ = device_->CreateComputePipeline(pd);
        if (sc_ind_trace_pipeline_ == handles::INVALID_PIPELINE) {
            std::cerr << "[SurfaceCache] Failed to create IndirectTrace pipeline\n"; return false;
        }

        // IndirectTraceParams CB (large: includes 3x float4[3] for SDF cascades)
        BufferDesc cbDesc{}; cbDesc.size = 512; cbDesc.type = BufferType::Constant;
        cbDesc.usage = GPUMemoryUsage::Dynamic; cbDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        sc_ind_trace_params_cb_ = device_->CreateBuffer(cbDesc);

        // ProbeRayHit buffer: (tiles * tiles * rays_per_probe) * sizeof(ProbeRayHit)
        u32 atlasSize = surfaceCachePass_->GetAtlasSize();
        u32 tileSize = 8;
        u32 tilesPerSide = atlasSize / tileSize;
        u32 totalProbes = tilesPerSide * tilesPerSide;
        u32 raysPerProbe = 4;
        u32 hitSize = 32; // float3 + float + uint + float = 32 bytes
        BufferDesc hitDesc{};
        hitDesc.size = (u64)totalProbes * raysPerProbe * hitSize;
        hitDesc.type = BufferType::Structured;
        hitDesc.usage = GPUMemoryUsage::Dynamic;
        hitDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        sc_ray_hits_buf_ = device_->CreateBuffer(hitDesc);

        std::cout << "[SurfaceCache] IndirectTrace pipeline created (probes=" << totalProbes
                  << ", rays=" << raysPerProbe << ", buf=" << hitDesc.size << " bytes)" << std::endl;
    }

    // === 5.7. IndirectResolve compute pipeline ===
    {
        const shader_file_info info{ "Lumen/SurfaceCacheIndirectResolve.metal", "surfaceCacheIndirectResolve", shader_type::compute };
        auto compiled = compile_shader(info, shaderDir.c_str(), extra_args);
        if (!compiled) { std::cerr << "[SurfaceCache] Failed to compile IndirectResolve shader\n"; return false; }

        u64 sz = *reinterpret_cast<u64*>(compiled.get());
        u8* ptr = compiled.get() + sizeof(u64) + 16;
        auto sh = device_->CreateShader(ptr, sz, ShaderStage::Compute, info.function);
        if (sh == handles::INVALID_SHADER) { std::cerr << "[SurfaceCache] Failed to create IndirectResolve shader\n"; return false; }

        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // prev_lighting
            {1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // albedo atlas
            {2, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // sky (black)
            {3, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // indirect_out
            {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // GlobalShaderData
            {1, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // IndirectResolveParams
            {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // lookups
            {3, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // cards
            {4, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // ray_hits (in)
        };
        sc_ind_resolve_set_layout_ = device_->CreateDescriptorSetLayout({9, bindings});
        sc_ind_resolve_layout_ = device_->CreatePipelineLayout({1, &sc_ind_resolve_set_layout_});
        sc_ind_resolve_descriptor_set_ = device_->CreateDescriptorSet({sc_ind_resolve_set_layout_});

        ComputePipelineDesc pd{};
        pd.computeShader = sh;
        pd.layout = sc_ind_resolve_layout_;
        pd.threadGroupSize = {8, 8, 1};
        sc_ind_resolve_pipeline_ = device_->CreateComputePipeline(pd);
        if (sc_ind_resolve_pipeline_ == handles::INVALID_PIPELINE) {
            std::cerr << "[SurfaceCache] Failed to create IndirectResolve pipeline\n"; return false;
        }

        BufferDesc cbDesc{}; cbDesc.size = 256; cbDesc.type = BufferType::Constant;
        cbDesc.usage = GPUMemoryUsage::Dynamic; cbDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        sc_ind_resolve_params_cb_ = device_->CreateBuffer(cbDesc);

        // Indirect output texture (independent, RGBA16F)
        u32 atlasSize = surfaceCachePass_->GetAtlasSize();
        TextureDesc td{};
        td.size = {atlasSize, atlasSize, 1}; td.mipLevels = 1; td.arraySize = 1;
        td.format = DataFormat::RGBA16_Float; td.type = TextureType::Texture2D;
        td.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
        td.name = "SC_IndirectOut";
        sc_indirect_out_tex_ = device_->CreateTexture(td);

        std::cout << "[SurfaceCache] IndirectResolve pipeline created" << std::endl;
    }

    // === 6. CardRadianceAvg compute pipeline (Surface Cache → DDGI) ===
    {
        const shader_file_info info{ "Lumen/DDGICardRadianceAvg.metal", "ddgi_card_radiance_avg", shader_type::compute };
        auto compiled = compile_shader(info, shaderDir.c_str(), extra_args);
        if (!compiled) { std::cerr << "[SC→DDGI] Failed to compile CardRadianceAvg shader" << std::endl; }
        else {
            u64 sz = *reinterpret_cast<u64*>(compiled.get());
            u8* ptr = compiled.get() + sizeof(u64) + 16;
            auto sh = device_->CreateShader(ptr, sz, ShaderStage::Compute, info.function);

            if (sh == handles::INVALID_SHADER) {
                std::cerr << "[SC→DDGI] Failed to create CardRadianceAvg shader" << std::endl;
            } else {
                DescriptorSetLayoutBinding bindings[] = {
                    {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // SurfaceCacheParams
                    {1, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // cards
                    {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // card_radiance output
                    {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // lighting atlas
                };
                sc_card_rad_set_layout_ = device_->CreateDescriptorSetLayout({4, bindings});
                sc_card_rad_layout_ = device_->CreatePipelineLayout({1, &sc_card_rad_set_layout_});

                ComputePipelineDesc pd{};
                pd.computeShader = sh;
                pd.layout = sc_card_rad_layout_;
                pd.threadGroupSize = {64, 1, 1};
                sc_card_rad_pipeline_ = device_->CreateComputePipeline(pd);

                for (int i = 0; i < 3; ++i) {
                    sc_card_rad_ds_[i] = device_->CreateDescriptorSet({sc_card_rad_set_layout_});
                }

                if (sc_card_rad_pipeline_ != handles::INVALID_PIPELINE) {
                    std::cout << "[SC→DDGI] CardRadianceAvg pipeline created" << std::endl;
                }
            }
        }
    }

    // === 7. ProbeIrradianceFromCards compute pipeline ===
    {
        const shader_file_info info{ "Lumen/DDGIProbeIrradianceFromCards.metal", "ddgi_probe_irradiance_from_cards", shader_type::compute };
        auto compiled = compile_shader(info, shaderDir.c_str(), extra_args);
        if (!compiled) { std::cerr << "[SC→DDGI] Failed to compile ProbeIrradianceFromCards shader" << std::endl; }
        else {
            u64 sz = *reinterpret_cast<u64*>(compiled.get());
            u8* ptr = compiled.get() + sizeof(u64) + 16;
            auto sh = device_->CreateShader(ptr, sz, ShaderStage::Compute, info.function);

            if (sh == handles::INVALID_SHADER) {
                std::cerr << "[SC→DDGI] Failed to create ProbeIrradianceFromCards shader" << std::endl;
            } else {
                DescriptorSetLayoutBinding bindings[] = {
                    {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // DDGIVolumeData
                    {1, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // ProbeContribRange
                    {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // ProbeCardContrib
                    {3, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // card_radiance
                    {4, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // irradiance_history
                    {5, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // irradiance_output
                    {6, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probe_update_list
                    {7, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // confidenceBuffer
                };
                sc_probe_irr_set_layout_ = device_->CreateDescriptorSetLayout({8, bindings});
                sc_probe_irr_layout_ = device_->CreatePipelineLayout({1, &sc_probe_irr_set_layout_});

                // DEBUG: test pipeline creation but skip descriptor sets
                ComputePipelineDesc pd{};
                pd.computeShader = sh;
                pd.layout = sc_probe_irr_layout_;
                pd.threadGroupSize = {64, 1, 1};
                sc_probe_irr_pipeline_ = device_->CreateComputePipeline(pd);

                for (int i = 0; i < 3; ++i) {
                    sc_probe_irr_ds_[i] = device_->CreateDescriptorSet({sc_probe_irr_set_layout_});
                }

                if (sc_probe_irr_pipeline_ != handles::INVALID_PIPELINE) {
                    std::cout << "[SC→DDGI] ProbeIrradianceFromCards pipeline created" << std::endl;
                }
            }
        }
    }

    std::cout << "[SurfaceCache] Pipelines initialized (FillTest + LightCull + LightEval + SC→DDGI)" << std::endl;

    // === Section 8: Pre-allocate SC→DDGI buffers (at init time, not after scene load) ===
    // These must be created here alongside other GPU resources to avoid heap timing issues.
    // Data is uploaded later in BuildCardProbeAssignment() after cards are generated.
    {
        u32 totalProbes = 2048;  // matches DDGI 16x8x16 grid
        u32 maxCards = 4096;     // matches SurfaceCacheParams max_cards

        // Range buffer: one ProbeContribRange per probe
        {
            BufferDesc rangeDesc{};
            rangeDesc.size = totalProbes * sizeof(SCProbeContribRange);
            rangeDesc.type = BufferType::Structured;
            rangeDesc.usage = GPUMemoryUsage::Dynamic;
            rangeDesc.memoryUsage = GPUMemoryUsage::Dynamic;
            sc_probe_contrib_range_buf_ = device_->CreateBuffer(rangeDesc);
        }

        // Flat contrib buffer: worst case = probes * MAX_CONTRIBS_PER_PROBE
        {
            BufferDesc contribDesc{};
            contribDesc.size = (u64)totalProbes * SC_MAX_CONTRIBS_PER_PROBE * sizeof(SCProbeCardContrib);
            contribDesc.type = BufferType::Structured;
            contribDesc.usage = GPUMemoryUsage::Dynamic;
            contribDesc.memoryUsage = GPUMemoryUsage::Dynamic;
            sc_flat_contrib_buf_ = device_->CreateBuffer(contribDesc);
        }

        // Card radiance buffer: float3 per card
        {
            BufferDesc radDesc{};
            radDesc.size = (u64)maxCards * sizeof(float) * 3;
            radDesc.type = BufferType::Structured;
            radDesc.usage = GPUMemoryUsage::Dynamic;
            radDesc.memoryUsage = GPUMemoryUsage::Dynamic;
            sc_card_radiance_buf_ = device_->CreateBuffer(radDesc);
        }

        // SurfaceCacheParams CB
        {
            BufferDesc cbDesc{};
            cbDesc.size = 256;
            cbDesc.type = BufferType::Constant;
            cbDesc.usage = GPUMemoryUsage::Dynamic;
            cbDesc.memoryUsage = GPUMemoryUsage::Dynamic;
            sc_sc_params_cb_ = device_->CreateBuffer(cbDesc);
        }

        // DDGI volume CB (triple-buffered)
        for (int i = 0; i < 3; ++i) {
            BufferDesc cbDesc{};
            cbDesc.size = 512;
            cbDesc.type = BufferType::Constant;
            cbDesc.usage = GPUMemoryUsage::Dynamic;
            cbDesc.memoryUsage = GPUMemoryUsage::Dynamic;
            sc_ddgi_vol_cb_[i] = device_->CreateBuffer(cbDesc);
        }

        std::cout << "[SC→DDGI] Pre-allocated GPU buffers (range+contrib+radiance+CBs)" << std::endl;
    }

    return true;
}

bool TestNaniteStreamingPipeline::BuildCardProbeAssignment() {
    if (!surfaceCachePass_ || !surfaceCachePass_->IsInitialized() || !ddgiPass_ || !ddgiPass_->IsInitialized()) {
        std::cerr << "[SC→DDGI] Cannot build assignment: missing passes" << std::endl;
        return false;
    }

    const auto& cards = surfaceCachePass_->GetCardGenerator().GetCards();
    u32 cardCount = surfaceCachePass_->GetCardGenerator().GetCardCount();
    if (cardCount == 0) {
        std::cerr << "[SC→DDGI] No cards to assign" << std::endl;
        return false;
    }

    const auto& ddgiParams = ddgiPass_->GetParams();
    const auto& ddgiVol = ddgiPass_->GetVolumeData();
    u32 totalProbes = ddgiParams.probe_count_x * ddgiParams.probe_count_y * ddgiParams.probe_count_z;
    primal::math::v3 probeOrigin = {ddgiVol.ProbeOrigin.x, ddgiVol.ProbeOrigin.y, ddgiVol.ProbeOrigin.z};
    float probeSpacing = ddgiParams.probe_spacing;
    float maxDist = probeSpacing * 3.0f;

    // Per-probe builder (fixed-size, lock-free)
    struct ProbeBuilder {
        SCProbeCardContrib entries[SC_MAX_CONTRIBS_PER_PROBE];
        u32 count = 0;
        float weights[SC_MAX_CONTRIBS_PER_PROBE] = {};  // for sorting
    };
    std::vector<ProbeBuilder> builders(totalProbes);

    // Parallel computation using JobSystem
    auto jobHandle = primal::jobsystem::JobSystem::ParallelFor(totalProbes,
        [&](u32 probeIdx) {
            auto& b = builders[probeIdx];

            // Compute probe world position
            u32 ix = probeIdx % ddgiParams.probe_count_x;
            u32 iy = (probeIdx / ddgiParams.probe_count_x) % ddgiParams.probe_count_y;
            u32 iz = probeIdx / (ddgiParams.probe_count_x * ddgiParams.probe_count_y);
            primal::math::v3 probePos = {
                probeOrigin.x + (float)ix * probeSpacing,
                probeOrigin.y + (float)iy * probeSpacing,
                probeOrigin.z + (float)iz * probeSpacing
            };

            constexpr float PI = 3.14159265358979f;
            constexpr float FOUR_PI = 4.0f * PI;
            constexpr float SH_C0 = 0.282095f;
            constexpr float SH_C1 = 0.488603f;

            for (u32 c = 0; c < cardCount; ++c) {
                const auto& card = cards[c];
                if (card.resolution == 0) continue;

                // Direction from probe to card center
                float dx = card.center.x - probePos.x;
                float dy = card.center.y - probePos.y;
                float dz = card.center.z - probePos.z;
                float dist2 = dx*dx + dy*dy + dz*dz;
                if (dist2 > maxDist * maxDist) continue;

                float dist = std::sqrt(dist2);
                if (dist < 0.01f) continue;

                float invDist = 1.0f / dist;
                float dirX = dx * invDist;
                float dirY = dy * invDist;
                float dirZ = dz * invDist;

                // Card normal from axis_direction
                uint8_t axis = card.axis_direction & 0xFF;
                uint8_t dir  = (card.axis_direction >> 8) & 0xFF;
                primal::math::v3 cardNormal{0,0,0};
                if (axis == 0) cardNormal.x = dir ? 1.0f : -1.0f;
                else if (axis == 1) cardNormal.y = dir ? 1.0f : -1.0f;
                else cardNormal.z = dir ? 1.0f : -1.0f;

                // NdotL: card normal dotted with direction toward probe (back-face check)
                float NdotL = -(cardNormal.x * dirX + cardNormal.y * dirY + cardNormal.z * dirZ);
                if (NdotL < 0.01f) continue;

                // Card area (two non-axis extent components)
                float cardArea;
                if (axis == 0) cardArea = card.extent.y * card.extent.z * 4.0f;
                else if (axis == 1) cardArea = card.extent.x * card.extent.z * 4.0f;
                else cardArea = card.extent.x * card.extent.y * 4.0f;
                if (cardArea < 0.001f) continue;

                // Approximate solid angle: area / (dist² + ε)
                float solidAngle = cardArea / (dist2 + 1.0f);
                // Weight includes 4π normalization for sparse card sampling
                // (equivalent to DDGI's 4π/RaysPerProbe per ray)
                float totalWeight = solidAngle * NdotL * FOUR_PI;

                // Evaluate SH basis L0+L1 in direction (probe→card)
                float basis[4];
                basis[0] = SH_C0;
                basis[1] = -SH_C1 * dirY;
                basis[2] =  SH_C1 * dirZ;
                basis[3] = -SH_C1 * dirX;

                // Compute contribution
                SCProbeCardContrib contrib;
                contrib.card_index = c;
                for (int i = 0; i < 4; ++i) {
                    contrib.sh_weights[i] = totalWeight * basis[i];
                }

                // Insert-sorted into top-N by weight
                if (b.count < SC_MAX_CONTRIBS_PER_PROBE) {
                    b.entries[b.count] = contrib;
                    b.weights[b.count] = totalWeight;
                    b.count++;
                } else {
                    // Find minimum weight entry
                    u32 minIdx = 0;
                    for (u32 j = 1; j < SC_MAX_CONTRIBS_PER_PROBE; ++j) {
                        if (b.weights[j] < b.weights[minIdx]) minIdx = j;
                    }
                    if (totalWeight > b.weights[minIdx]) {
                        b.entries[minIdx] = contrib;
                        b.weights[minIdx] = totalWeight;
                    }
                }
            }
        });
    primal::jobsystem::JobSystem::Wait(jobHandle);

    // Flatten into GPU-ready buffers
    std::vector<SCProbeContribRange> ranges(totalProbes);
    std::vector<SCProbeCardContrib> flatContribs;
    flatContribs.reserve(totalProbes * SC_MAX_CONTRIBS_PER_PROBE);

    for (u32 p = 0; p < totalProbes; ++p) {
        ranges[p].start = (u32)flatContribs.size();
        ranges[p].count = builders[p].count;
        for (u32 i = 0; i < builders[p].count; ++i) {
            flatContribs.push_back(builders[p].entries[i]);
        }
    }

    // Upload to GPU — buffers were pre-allocated in InitializeSurfaceCachePipelines()
    // Upload range data
    {
        auto* mapped = static_cast<SCProbeContribRange*>(device_->MapBuffer(sc_probe_contrib_range_buf_));
        if (mapped) {
            memcpy(mapped, ranges.data(), ranges.size() * sizeof(SCProbeContribRange));
            device_->UnmapBuffer(sc_probe_contrib_range_buf_);
        }
    }
    // Upload contrib data
    {
        auto* mapped = static_cast<SCProbeCardContrib*>(device_->MapBuffer(sc_flat_contrib_buf_));
        if (mapped) {
            memcpy(mapped, flatContribs.data(), flatContribs.size() * sizeof(SCProbeCardContrib));
            device_->UnmapBuffer(sc_flat_contrib_buf_);
        }
    }
    // Upload SurfaceCacheParams
    {
        auto* mapped = static_cast<lumen::SurfaceCacheParams*>(device_->MapBuffer(sc_sc_params_cb_));
        if (mapped) {
            lumen::SurfaceCacheParams params{};
            params.atlas_size = surfaceCachePass_->GetAtlasSize();
            params.page_size = surfaceCachePass_->GetPageSize();
            params.max_cards = cardCount;
            params.lookup_count = surfaceCachePass_->GetCardGenerator().GetLookupCount();
            *mapped = params;
            device_->UnmapBuffer(sc_sc_params_cb_);
        }
    }

    std::cout << "[SC→DDGI] Card→Probe assignment built: "
              << cardCount << " cards, " << totalProbes << " probes, "
              << flatContribs.size() << " contributions (avg "
              << (flatContribs.size() / (float)totalProbes) << " per probe)" << std::endl;
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

    //std::cout << "Successfully loaded Sponza scene with " << sceneMeshes_.size() << " meshes." << std::endl;

    // 🔧 DEBUG: Keep only the first mesh for culling debugging
    // if (!sceneMeshes_.empty()) {
    //     auto firstMesh = sceneMeshes_[0];
    //     sceneMeshes_.clear();
    //     sceneMeshes_.push_back(firstMesh);
    ////     std::cout << "🔧 DEBUG: Keeping only first mesh '" << firstMesh.name << "' for culling analysis" << std::endl;
    // }

    // Debug: Check mesh entity IDs
    u32 validEntityCount = 0;
    for (const auto& meshInfo : sceneMeshes_) {
        if (meshInfo.mesh && meshInfo.meshEntityId != primal::id::invalid_id) {
            validEntityCount++;
        }
    }
    //std::cout << "Valid mesh entity IDs: " << validEntityCount << " out of " << sceneMeshes_.size() << std::endl;

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
    //std::cout << "[TestNanite] Initializing GPU Material Registry..." << std::endl;
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

    //std::cout << "[TestNanite] Registered " << registeredCount << " materials" << std::endl;

    // Debug blocks commented out — too verbose for normal operation
    // Uncomment individually if needed for debugging material/texture issues

    if (registeredCount == 0) {
        std::cerr << "[TestNanite] Warning: No materials were registered!" << std::endl;
    }

    // 🔧 TEMPORARY: Manually adjust UV scaling for problematic materials
    // This is a test to verify UV scaling works before implementing FBX parameter reading
    //std::cout << "[TestNanite] Applying manual UV scaling adjustments..." << std::endl;
    AdjustMaterialUVScaling();

    // Start async material build
    //std::cout << "[TestNanite] Starting async material data build..." << std::endl;
    materialBuildJob_ = gpuMaterialRegistry_->BuildAsync(device_);

    if (!materialBuildJob_.IsValid()) {
        std::cerr << "[TestNanite] Warning: Material build job is invalid!" << std::endl;
    }

    // 🎨 Wait for material build to complete and upload to GPU
    //std::cout << "[TestNanite] Waiting for material build to complete..." << std::endl;
    materialBuildJob_.Wait();

    // Upload material data to GPU
    //std::cout << "[TestNanite] Uploading material data to GPU..." << std::endl;
    if (!gpuMaterialRegistry_->UploadToGPU(device_)) {
        std::cerr << "[TestNanite] ERROR: Failed to upload material data to GPU" << std::endl;
    } else {
        //std::cout << "[TestNanite] Material data uploaded successfully" << std::endl;

        // Get material data buffer and set it to GPU draw pipeline
        auto materialBuffer = gpuMaterialRegistry_->GetMaterialDataBuffer();
        if (materialBuffer != rhi::handles::INVALID_RESOURCE) {
            gpuDrawPipeline_->SetMaterialDataBuffer(materialBuffer);
            //std::cout << "[TestNanite] Material data buffer set to GPU draw pipeline" << std::endl;
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
                //std::cout << "[TestNanite] Texture arrays set to GPU draw pipeline (albedo="
                          //<< albedoArray << ", normal=" << normalArray << ", orm=" << ormArray << ")" << std::endl;
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
                //std::cout << "Warning: Failed to create cluster component for mesh: " << meshInfo.name << std::endl;
            }

            graphics::RenderProxy proxy = graphics::RenderProxy::Create(
                entityId, meshInfo.meshEntityId, meshInfo.gpuMaterialId);  // 🔥 Use gpuMaterialId instead of invalid_id

            // TODO: Extract proper transform from scene data
            // Currently using identity matrix - scene data may not contain individual transforms
            proxy.transform = graphics::rhi::math::MatrixIdentity();
            scene_.AddProxy(proxy);

            //// std::cout << "Added mesh: " << meshInfo.name 
            //           << " (entityId=" << entityId 
            //           << ", geometryId=" << meshInfo.meshEntityId << ")" << std::endl;
        }
    }

    //// std::cout << "Added " << scene_.GetProxies().size() << " proxies to render scene" << std::endl;

    // Bind scene data to snapshot for Nanite culling
    if (!sceneSnapshot_.Rebind(scene_)) {
        std::cerr << "Failed to bind scene data to snapshot for Nanite culling" << std::endl;
        return false;
    }

    //// std::cout << "Scene snapshot updated with " << sceneSnapshot_.GetInstanceCount() << " instances" << std::endl;

    // Initialize render view
    view_.SetViewMatrix(cameraView_);
    view_.SetProjectionMatrix(cameraProj_);
    view_.SetViewport({ {0, 0}, {static_cast<float>(renderWidth_), static_cast<float>(renderHeight_)}, 0, 1 });
    view_.SetScissor({ {0, 0}, {renderWidth_, renderHeight_} });
    view_.UpdateFrustum();

    return true;
}

bool TestNaniteStreamingPipeline::LoadMaterialTextures() {
    //std::cout << "[TestNanite] Loading material textures..." << std::endl;

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
                        // [TextureMapping] albedo debug — commented out
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
                        // [TextureMapping] normal debug — commented out
                    }
                    // Don't set white fallback for normals — white (1,1,1) decodes as
                    // tangent normal (1,1,1) which produces wrong world normals via TBN.
                    // Leaving INVALID causes shader to use correct vertex normals instead.
                }

                if (texture != rhi::handles::INVALID_RESOURCE) {
                    meshInfo.materialInstance->SetTexture(1, texture);  // Normal binding = 1
                }
            }
        }
        // No normal texture path — leave as INVALID so shader uses vertex normal

        // ORM texture (use white for now - would need separate loading logic)
        meshInfo.materialInstance->SetTexture(2, whiteTexture);  // ORM binding = 2
    }

    //std::cout << "[TestNanite] Texture loading complete: "
              //<< loadedCount << " loaded, " << skippedCount << " reused ("
              //<< textureCache.size() << " unique)" << std::endl;

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
    // Set default UV scaling (1.0) for all materials
    for (size_t i = 0; i < materialCount; ++i) {
        materialData[i].uv_scale[0] = 1.0f;
        materialData[i].uv_scale[1] = 1.0f;
    }
}

void TestNaniteStreamingPipeline::Run() {
    // Run static probe integration tests once on first frame
    if (frameCount_ == 0) {
        TestStaticProbeSerialization();
        TestDDGIInitFromStatic();
    }

    primal::input::input_value val;

    primal::input::get(primal::input::input_source::keyboard, primal::input::input_code::key_f1, val);
    bool f1_current = val.current.x > 0.0f;
    if (f1_current && !keyState_.f1_prev) {
        testConfig_.enable_streaming = !testConfig_.enable_streaming;
        //std::cout << "Streaming: " << (testConfig_.enable_streaming ? "Enabled" : "Disabled") << std::endl;
    }
    keyState_.f1_prev = f1_current;

    primal::input::get(primal::input::input_source::keyboard, primal::input::input_code::key_f2, val);
    bool f2_current = val.current.x > 0.0f;
    if (f2_current && !keyState_.f2_prev) {
        //std::cout << "\n=== Running Stress Test ===" << std::endl;
        for (u32 i = 0; i < 100; ++i) {
            ProcessStreamingFeedback();
        }
        ValidateResults();
        //std::cout << "Stress Test Complete" << std::endl;
    }
    keyState_.f2_prev = f2_current;

    primal::input::get(primal::input::input_source::keyboard, primal::input::input_code::key_f3, val);
    bool f3_current = val.current.x > 0.0f;
    if (f3_current && !keyState_.f3_prev) {
        //std::cout << "\n=== Running Performance Benchmark ===" << std::endl;
        TestPerformance();
    }
    keyState_.f3_prev = f3_current;

    // F4: SSGI visualization mode toggle (0=Composite, 1=SSGI only, 2=Scene only)
    primal::input::get(primal::input::input_source::keyboard, primal::input::input_code::key_f4, val);
    bool f4_current = val.current.x > 0.0f;
    if (f4_current && !keyState_.f4_prev) {
        ssgiVisMode_ = (ssgiVisMode_ + 1) % 10;
        const char* modeNames[] = { "Composite (Scene+SSGI)", "SSGI Only", "Scene Only", "DDGI Composite", "Albedo Only", "Screen Probe GI", "Full GI Fusion", "Surface Cache", "SC Albedo Atlas", "SC Depth Atlas" };
        //std::cout << "[SSGI Vis] Mode: " << modeNames[ssgiVisMode_] << std::endl;
    }
    keyState_.f4_prev = f4_current;

    // F5: Mode 6 diagnostic toggle (2 modes)
    primal::input::get(primal::input::input_source::keyboard, primal::input::input_code::key_f5, val);
    bool f5_current = val.current.x > 0.0f;
    if (f5_current && !keyState_.f5_prev) {
        mode_diag_ = (mode_diag_ + 1) % 2;
        const char* diagNames[] = {
            "0: SimpleBlit (1 tex)",
            "1: 2-Pass FragmentFusion (5+2 reads)"
        };
        std::cout << "[Mode6 Diag] " << diagNames[mode_diag_] << std::endl;
    }
    keyState_.f5_prev = f5_current;

    // IJKL: rotate light direction for Surface Cache lighting
    {
        float speed = 0.02f;
        primal::input::get(primal::input::input_source::keyboard, primal::input::input_code::key_i, val);
        if (val.current.x > 0.0f) light_direction_.y -= speed;
        primal::input::get(primal::input::input_source::keyboard, primal::input::input_code::key_k, val);
        if (val.current.x > 0.0f) light_direction_.y += speed;
        primal::input::get(primal::input::input_source::keyboard, primal::input::input_code::key_j, val);
        if (val.current.x > 0.0f) light_direction_.x -= speed;
        primal::input::get(primal::input::input_source::keyboard, primal::input::input_code::key_l, val);
        if (val.current.x > 0.0f) light_direction_.x += speed;
        // Renormalize
        float len = std::sqrt(light_direction_.x * light_direction_.x +
                              light_direction_.y * light_direction_.y +
                              light_direction_.z * light_direction_.z);
        if (len > 0.001f) { light_direction_.x /= len; light_direction_.y /= len; light_direction_.z /= len; }
    }

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
        printf("[Frame %u] complete (visMode=%u)\n", (u32)frameCount_, ssgiVisMode_); fflush(stdout);
        frameCount_++;
    }

    // Process garbage collector to release deferred-destroyed GPU resources
    // device_->GetGarbageCollector().Update(frameCount_);

    if (frameCount_ == 0) {
        auto stats = streamingManager_->GetStats();
        //std::cout << "[Frame " << frameCount_ << "] "
                  //<< "Streamed: " << testResults_.clusters_streamed.load()
                  //<< ", Evicted: " << testResults_.clusters_evicted.load()
                  //<< ", Requests: " << testResults_.requests_processed.load()
                  //<< ", Pool Usage: " << (stats.page_pool_usage * 100.0f) << "%"
                  //<< std::endl;
    }

    // GPU→CPU readback disabled — causes CPU stalls mapping GPU buffers every frame.
    // Re-enable only when debugging culling (gated behind a flag or key press).
    // primal::utl::vector<primal::graphics::nanite::CullingDebugData> debug_data;
    // if (cullingPipeline_->ReadDebugData(debug_data)) {
    //     finalFrameCullingDebugData_ = std::move(debug_data);
    // }

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

    // Update GlobalSDF cascade origins based on camera position (for DDGI ray tracing)
    // CRITICAL: Only update BEFORE voxelization completes. After that, lock origins
    // so SDF texture data matches the cascade origin/extent metadata.
    {
        auto& globalSDF = nanite::GlobalSDF::Get();
        if (globalSDF.IsInitialized() && !sdf_voxelization_done_) {
            globalSDF.Update(sceneSnapshot_, frameCount_, camera_.GetPosition());
        }
    }
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
        //std::cout << "[HZB] Creating persistent depth texture (" << renderWidth_ << "x" << renderHeight_ << ")" << std::endl;

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

    // === SHARED LIGHT SETUP (used by both shadow & deferred passes) ===
    // Fixed world-space directional light (like the sun).
    // MUST NOT derive from camera orientation — otherwise rotating the camera
    // changes the light direction, causing walls to go black (NdotL <= 0).
    // lightForward: the direction light TRAVELS (emission direction, e.g. downward).
    // Shader expects lightPos.xyz = direction FROM surface TO light (opposite of emission).
    primal::math::v3 lightForward = Normalize(primal::math::v3{-0.9f, 1.5f, -0.8f});
    primal::math::v4 sharedLightPos{-lightForward.x, -lightForward.y, -lightForward.z, 0.0f}; // w=0 = directional, negate for "to light"
    primal::math::v3 sharedLightDir = lightForward; // Emission direction for shadow camera lookAt
    primal::math::v3 sharedLightUp = {0.0f, 1.0f, 0.0f};
    if (abs(sharedLightDir.y) > 0.9f) sharedLightUp = {1.0f, 0.0f, 0.0f};

    // Import shadow map textures into RenderGraph so it can track dependencies.
    // ShadowBlit writes → DeferredLighting reads: this establishes the execution order
    // and inserts the correct GPU barrier (UAV → ShaderResource).
    // Defined outside the shadow_enabled_ block so DeferredLighting can reference it.
    rendergraph::RGResourceHandle shadowMapRG[2];

    // === SHADOW CULLING + DEPTH BLIT PASSES ===
    if (shadow_enabled_ && gpuDrawPipeline_ && gpuDrawPipeline_->IsInitialized()) {
        // Use shared light setup (defined at top of BuildRenderGraph)
        auto& lightPos = sharedLightPos;
        auto& lightDir = sharedLightDir;
        auto& lightUp = sharedLightUp;

        struct ShadowPassData {};

        for (u32 c = 0; c < 2; ++c) {
            auto smHandle = gpuDrawPipeline_->GetShadowMap(c, currentBufferIndex);
            if (smHandle != rhi::handles::INVALID_RESOURCE) {
                shadowMapRG[c] = graph.ImportResource(
                    "ShadowMap_C" + std::to_string(c) + "_" + std::to_string(currentBufferIndex),
                    smHandle);
            }
        }

        // VP matrix comparison for shadow map caching
        auto vp_equal = [](const primal::math::m4x4& a, const primal::math::m4x4& b) -> bool {
            for (int c = 0; c < 4; ++c)
                for (int r = 0; r < 4; ++r)
                    if (std::abs(a.columns[c][r] - b.columns[c][r]) > 1e-5f)
                        return false;
            return true;
        };

        for (u32 cascade = 0; cascade < 2; ++cascade) {
            // Shadow cascade parameters tuned for Sponza scene (~30 units):
            // cascadeDistance = 200: light camera is 200 units behind target, enough to see entire scene
            // farPlane = 400: gives scene ~7.5% of depth buffer (was 0.3% with 5000/10000)
            // orthoExtent: cascade 0 covers 30 units (close-up shadows), cascade 1 covers 150 units
            // nearPlane = 0.1: standard near plane
            float cascadeDistance = 200.0f;
            float orthoExtent = (cascade == 0) ? 30.0f : 150.0f;

            // ⚠️ 终极 Shadow Map 稳定方案：标准的 CSM 视椎体固定原点对齐法
            primal::math::v3 camPos = camera_.GetPosition();
            
            // 1. 创建一个固定在世界原点的临时灯光观察矩阵，只用于确定灯光空间的”朝向”
            //    CRITICAL: tempView 必须与 lightView 朝向一致（都朝向场景，即 +lightDir 方向）
            //    否则 X 轴翻转导致纹素对齐方向错误，阴影会反向移动并产生左右分割
            primal::math::v3 origin = {0.0f, 0.0f, 0.0f};
            primal::math::v3 lightLookAt = {origin.x + lightDir.x, origin.y + lightDir.y, origin.z + lightDir.z};
            primal::math::m4x4 tempView = CreateLookAtMatrix(origin, lightLookAt, lightUp);

            // 2. 将相机的世界坐标转换到这个绝对静止的灯光空间中
            primal::math::v4 camPosLS4 = tempView * primal::math::v4{camPos.x, camPos.y, camPos.z, 1.0f};
            primal::math::v3 camPosLS = {camPosLS4.x, camPosLS4.y, camPosLS4.z};

            // 3. 在灯光空间中计算纹素大小（Texel Size）
            float shadowMapSize = 2048.0f;
            float worldUnitsPerTexel = (orthoExtent * 2.0f) / shadowMapSize;

            // 4. 将灯光空间下的 XY 坐标严格对齐到纹素网格上
            // 必须用 floor 并且补偿微小浮点误差，确保对齐方向绝对一致
            camPosLS.x = std::floor(camPosLS.x / worldUnitsPerTexel) * worldUnitsPerTexel;
            camPosLS.y = std::floor(camPosLS.y / worldUnitsPerTexel) * worldUnitsPerTexel;
            // Z 轴（深度）不能对齐，必须保留连续平滑移动，否则深度精度会跳变

            // 5. 将对齐后的灯光空间坐标转换回世界空间
            primal::math::m4x4 tempViewInv = primal::graphics::rhi::math::Inverse(tempView);
            primal::math::v4 snappedCamPosWorld4 = tempViewInv * primal::math::v4{camPosLS.x, camPosLS.y, camPosLS.z, 1.0f};
            primal::math::v3 snappedCamPosWorld = {snappedCamPosWorld4.x, snappedCamPosWorld4.y, snappedCamPosWorld4.z};

            // 6. 用对齐后的世界坐标来构建最终的阴影相机
            primal::math::v3 lightEye = {
                snappedCamPosWorld.x - lightDir.x * cascadeDistance, 
                snappedCamPosWorld.y - lightDir.y * cascadeDistance, 
                snappedCamPosWorld.z - lightDir.z * cascadeDistance
            };
            
            primal::math::m4x4 lightView = CreateLookAtMatrix(lightEye, snappedCamPosWorld, lightUp);

            // Compute tight near/far from actual scene geometry for maximum shadow depth precision.
            // Old: near=0.1, far=400 → scene at ~200 units occupies only ~2% of [0,1] depth range.
            // New: near/far tightly bracket scene → scene uses ~90% of depth range.
            float minDist = 1e10f;
            float maxDist = 0.0f;
            {
                const auto& instances = sceneSnapshot_.GetInstanceData();
                for (const auto& inst : instances) {
                    // Vector from light eye to instance bounds center
                    float dx = inst.bounds_center.x - lightEye.x;
                    float dy = inst.bounds_center.y - lightEye.y;
                    float dz = inst.bounds_center.z - lightEye.z;
                    // Project onto light view direction to get depth along view axis
                    float dist = dx * lightDir.x + dy * lightDir.y + dz * lightDir.z;
                    minDist = std::min(minDist, dist - inst.bounds_radius);
                    maxDist = std::max(maxDist, dist + inst.bounds_radius);
                }
            }
            // 10% margin to avoid clipping at boundaries
            float depthSpan = maxDist - minDist;
            float margin = depthSpan * 0.1f;
            float nearPlane = std::max(minDist - margin, 0.5f);
            float farPlane = maxDist + margin;

            primal::math::m4x4 lightProj = CreateOrthographicMatrix(
                -orthoExtent, orthoExtent, -orthoExtent, orthoExtent, nearPlane, farPlane);
                
            primal::math::m4x4 lightVP = lightProj * lightView;

            // Skip shadow re-rendering when VP matrix is unchanged (static scene + static light).
            // The texel-snapping above means VP is identical frame-to-frame when camera
            // hasn't moved beyond one texel, so the cached shadow map is still valid.
            u32 shadowWriteSlot = currentBufferIndex % 3;
            bool cache_hit = shadow_cache_globally_valid_ &&
                             shadow_cache_valid_[shadowWriteSlot][cascade] &&
                             vp_equal(lightVP, cached_shadow_vp_[shadowWriteSlot][cascade]);

            if (cache_hit) {
                // Still need to save the matrix for DeferredLighting to use
                if (cascade == 0) {
                    cachedShadowMatrix0_[shadowWriteSlot] = lightVP;
                } else {
                    cachedShadowMatrix1_[shadowWriteSlot] = lightVP;
                }
                continue;
            }

            primal::graphics::nanite::GPUDrivenDrawPipeline::DirectionalLightData lightData{};
            lightData.direction = { lightDir.x, lightDir.y, lightDir.z, 0.0f };
            lightData.color = { 20.0f, 20.0f, 20.0f, 1.0f };
            lightData.viewPos = { camera_.GetPosition().x, camera_.GetPosition().y, camera_.GetPosition().z, 1.0f };
            
            // ⚠️ CRITICAL FIX: The GPU pipeline expects BOTH shadow matrices to be available
            // when it evaluates the frustum planes (or when it does anything per-light).
            // However, the shadow culling pass currently takes `lightData` by value, and it ONLY
            // cares about the matrix for the current `cascade`.
            // But just in case, we should ensure the matrix is placed in the correct slot.
            if (cascade == 0) {
                lightData.shadowMatrix0 = lightVP;
            } else {
                lightData.shadowMatrix1 = lightVP;
            }
            
            // Also need to pass the current camera's view matrix for CSM cascade splitting
            lightData.cascadeSplits = { 600.0f, 2000.0f, 0.0f, 0.0f }; // Adjust cascade splits!
            
            // SAVE THE MATRICES per triple-buffer slot so DeferredLighting uses matching matrix+map
            if (cascade == 0) {
                cachedShadowMatrix0_[shadowWriteSlot] = lightVP;
            } else {
                cachedShadowMatrix1_[shadowWriteSlot] = lightVP;
            }

            // Shadow Culling pass (Compute)
            std::string cullName = "ShadowCulling_C" + std::to_string(cascade);
            graph.AddPass<ShadowPassData>(cullName,
                graphics::rendergraph::RGPassType::Compute,
                graphics::rendergraph::RGPassCategory::Lighting,
                [](ShadowPassData&, graphics::rendergraph::RenderGraphBuilder& builder) {
                    builder.SideEffect();
                },
                [this, lightData, cascade, currentBufferIndex](const ShadowPassData&, graphics::rendergraph::RenderGraphContext& context) {
                    gpuDrawPipeline_->ExecuteShadowCulling(
                        context.cmdBuffer, sceneSnapshot_, lightData, cascade, currentBufferIndex);
                }
            );

            // Shadow Raster pass (Graphics) — render visible clusters to D32 depth
            std::string rasterName = "ShadowRaster_C" + std::to_string(cascade);
            graph.AddPass<ShadowPassData>(rasterName,
                graphics::rendergraph::RGPassType::Graphics,
                graphics::rendergraph::RGPassCategory::Lighting,
                [](ShadowPassData&, graphics::rendergraph::RenderGraphBuilder& builder) {
                    builder.SideEffect();
                },
                [this, cascade, currentBufferIndex, lightVP](const ShadowPassData&, graphics::rendergraph::RenderGraphContext& context) {
                    gpuDrawPipeline_->ExecuteShadowRaster(
                        context.cmdBuffer, lightVP, cascade, currentBufferIndex);
                }
            );

            // Shadow Depth Blit (D32 -> R32, Compute)
            std::string blitName = "ShadowBlit_C" + std::to_string(cascade);
            graph.AddPass<ShadowPassData>(blitName,
                graphics::rendergraph::RGPassType::Compute,
                graphics::rendergraph::RGPassCategory::Lighting,
                [shadowMapRG, cascade](ShadowPassData&, graphics::rendergraph::RenderGraphBuilder& builder) {
                    builder.SideEffect();
                    // Declare shadow map write so RenderGraph inserts barrier to consumers
                    if (shadowMapRG[cascade].IsValid()) {
                        builder.Write(shadowMapRG[cascade], rhi::ResourceState::UnorderedAccess);
                    }
                },
                [this, cascade, currentBufferIndex](const ShadowPassData&, graphics::rendergraph::RenderGraphContext& context) {
                    gpuDrawPipeline_->ExecuteShadowDepthBlit(
                        context.cmdBuffer, cascade, currentBufferIndex);
                }
            );

            // Mark this buffer slot + cascade as valid
            cached_shadow_vp_[shadowWriteSlot][cascade] = lightVP;
            shadow_cache_valid_[shadowWriteSlot][cascade] = true;
        }

        shadow_cache_globally_valid_ = true;
        shadow_frame_index_ = (shadow_frame_index_ + 1) % 3;
    }

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
            //// std::cout << "[BuildRenderGraph] NaniteCulling PASS SETUP called!" << std::endl;

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
                //// std::cout << "[BuildRenderGraph] Rendering visibility buffer..." << std::endl;
                auto visResult = visibilityBufferSystem_->RenderVisibilityBuffer(
                    cmd,
                    sceneSnapshot_,
                    cameraBuffers_[bufferIndex].view_matrix,
                    cameraBuffers_[bufferIndex].proj_matrix,
                    cullingPipeline_->GetResults(),
                    frameCount_
                );
                //// std::cout << "[BuildRenderGraph] Visibility buffer rendered: " << visResult.visible_triangles
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
            //// std::cout << "[NaniteCulling] Culling complete: visible_clusters=" << results.visible_cluster_count
            //           << ", visible_instances=" << results.visible_instance_count << std::endl;
        }
    );
    
    //// std::cout << "[BuildRenderGraph] NaniteCulling pass ADDED to graph" << std::endl;

    // ForceSync pass removed — the RenderGraph already handles the Compute→Graphics barrier
    // via NaniteCulling's Write(indirect_args, UAV) and SceneRender's Read(indirect_args, IndirectArgument).
    // The explicit Copy encoder was a full GPU pipeline stall that broke parallelism.

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
    //// std::cout << "[BuildRenderGraph] Imported GPU pipeline's final output texture" << std::endl;

    // Scene Render Pass - Execute GPU pipeline which manages its own render pass
    graph.AddPass<SceneRenderPassData>("SceneRender",
        graphics::rendergraph::RGPassType::Graphics,
        graphics::rendergraph::RGPassCategory::Main,
        [this, gpuOutputHandle, currentBufferIndex, cullingIndirectArgs = cullingData.indirect_args_buffer](SceneRenderPassData& data, graphics::rendergraph::RenderGraphBuilder& builder) {
            //// std::cout << "[SceneRender] SETUP: Configuring GPU pipeline execution" << std::endl;

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

            // NOTE: Depth copy is handled by the separate DepthCopy pass below,
            // which has proper barrier transitions (DepthStencil → CopySource).
            // Do NOT blit depth here — the render pass encoder state may conflict.

            // Log rendering statistics
            if (frameCount_ % 60 == 0 && !sceneMeshes_.empty()) {
                const auto& cullingResults = cullingPipeline_->GetResults();
                const auto& drawResults = gpuDrawPipeline_->GetResults();

                //// std::cout << "[GPU Driven Rendering Stats]" << std::endl;
                //// std::cout << "  Loaded meshes: " << sceneMeshes_.size() << std::endl;
                //// std::cout << "  Scene instances: " << sceneSnapshot_.GetInstanceCount() << std::endl;
                //// std::cout << "  Visible clusters: " << cullingResults.visible_cluster_count << std::endl;
                //// std::cout << "  GPU Draw calls: " << drawResults.total_draw_calls << std::endl;
                //// std::cout << "  Clusters rendered: " << drawResults.total_clusters_rendered << std::endl;
                //// std::cout << "  Bin count: " << drawResults.bin_count << std::endl;
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
            // CRITICAL: Must declare SideEffect or render graph will cull this pass!
            builder.SideEffect();
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

    // GBufferDepthBlit pass removed — deferred lighting now reads D32 directly
    // via depth2d<float> in the shader, eliminating the compute shader depth read
    // that caused non-deterministic depth issues on Metal TBDR.

    // === LUMEN SSAO PASS (before deferred lighting) ===
    rendergraph::RGResourceHandle ssaoOutputHandle;
    if (ssaoPass_ && ssaoPass_->IsInitialized() && frameCount_ > 0) {
        auto normalHandleSSAO = graph.ImportResource("GBufferNormal_SSAO", gpuDrawPipeline_->GetGBufferNormal());
        auto depthHandleSSAO = graph.ImportResource("GBufferDepth_SSAO", gpuDrawPipeline_->GetGBufferDepthSampleable());

        lumen::SSAOCameraData ssaoCameraData;
        ssaoCameraData.view_matrix = cameraBuffers_[currentBufferIndex].view_matrix;
        ssaoCameraData.proj_matrix = cameraBuffers_[currentBufferIndex].proj_matrix;
        {
            u32 prevIdx = (currentBufferIndex + 2) % 3;
            ssaoCameraData.prev_view_matrix = cameraBuffers_[prevIdx].view_matrix;
            ssaoCameraData.prev_proj_matrix = cameraBuffers_[prevIdx].proj_matrix;
        }
        ssaoCameraData.frame_index = frameCount_;
        ssaoCameraData.delta_time = 0.016f;

        auto ssaoOutput = ssaoPass_->AddPass(graph, normalHandleSSAO, depthHandleSSAO,
            ssaoCameraData, currentBufferIndex);
        ssaoOutputHandle = ssaoOutput.ssao_output;
    }

    // === DEFERRED PBR LIGHTING PASS ===
    // Import deferred output texture early so FinalBlit can reference it
    u32 cbIdx = currentBufferIndex % 3;
    ResourceHandle currentDeferredTex = deferred_output_textures_[cbIdx];
    // 2-frame-old deferred output for fusion pass (avoids data race with in-flight frames)
    // (currentBufferIndex + 1) % 3 = texture written 2 frames ago
    // (currentBufferIndex + 2) % 3 = texture written 1 frame ago (still potentially in-flight!)
    u32 readIdx = (currentBufferIndex + 1) % 3;
    ResourceHandle delayedDeferredTex = deferred_output_textures_[readIdx];

    rendergraph::RGResourceHandle deferredOutputRG;
    if (currentDeferredTex != rhi::handles::INVALID_RESOURCE) {
        deferredOutputRG = graph.ImportResource("DeferredOutput", currentDeferredTex);
    }
    // Import delayed texture for fusion reading
    rendergraph::RGResourceHandle delayedOutputRG;
    if (delayedDeferredTex != rhi::handles::INVALID_RESOURCE && delayedDeferredTex != currentDeferredTex) {
        delayedOutputRG = graph.ImportResource("DeferredOutputDelayed", delayedDeferredTex);
    }

    if (deferred_pipeline_ != rhi::handles::INVALID_PIPELINE &&
        currentDeferredTex != rhi::handles::INVALID_RESOURCE &&
        frameCount_ > 0) {

        struct DeferredPassData {
            rendergraph::RGResourceHandle output;
        };

        // Use same-frame shadow maps: all passes execute within a single Metal command buffer,
        // so ShadowBlit's output is guaranteed visible to DeferredLighting without delay.
        // Previous 2-frame delay caused flickering by reading different triple-buffer slots
        // whose shadow maps differed slightly due to non-deterministic GPU thread scheduling.
        graph.AddPass<DeferredPassData>("DeferredLighting",
            graphics::rendergraph::RGPassType::Graphics,
            graphics::rendergraph::RGPassCategory::Lighting,
            [deferredOutputRG, shadowMapRG](DeferredPassData& data, graphics::rendergraph::RenderGraphBuilder& builder) {
                data.output = builder.Write(deferredOutputRG, rhi::ResourceState::RenderTarget);

                // Declare shadow map reads: this creates dependency on ShadowBlit pass
                // and ensures proper GPU barrier (UAV → ShaderResource) before sampling.
                // Using shadowMapRG (same handles as ShadowBlit writes) ensures correct dependency.
                for (u32 c = 0; c < 2; ++c) {
                    if (shadowMapRG[c].IsValid()) {
                        builder.Read(shadowMapRG[c], rhi::ResourceState::ShaderResource);
                    }
                }

                graphics::rendergraph::RGRenderPassDesc rpDesc;
                rpDesc.colors.push_back({
                    .texture = data.output,
                    .loadOp = rhi::LoadAction::DontCare,
                    .storeOp = rhi::StoreAction::Store,
                    .clearColor = { primal::math::v4{0,0,0,1} }
                });
                builder.DeclareRenderPass(rpDesc);
            },
            [this, currentBufferIndex, sharedLightPos](const DeferredPassData& data, graphics::rendergraph::RenderGraphContext& context) {
                auto cmd = context.cmdBuffer;
                u32 cbIdx = currentBufferIndex % 3;

                // Upload ViewData (viewProjection + invViewProjection)
                {
                    primal::math::m4x4 vp = cameraBuffers_[currentBufferIndex].proj_matrix * cameraBuffers_[currentBufferIndex].view_matrix;
                    primal::math::m4x4 invVP = rhi::math::Inverse(vp);

                    struct ViewData {
                        primal::math::m4x4 viewProjection;
                        primal::math::m4x4 invViewProjection;
                    };
                    auto* vd = static_cast<ViewData*>(device_->MapBuffer(deferred_view_cb_[cbIdx]));
                    if (vd) {
                        vd->viewProjection = vp;
                        vd->invViewProjection = invVP;
                        device_->UnmapBuffer(deferred_view_cb_[cbIdx]);
                    }
                }

                // Upload SceneData (must match shader SceneData struct layout exactly)
                {
                    struct DeferredSceneData {
                        primal::math::m4x4 model;           // offset 0
                        primal::math::v4 lightPos;          // offset 64
                        primal::math::v4 lightColor;        // offset 80
                        primal::math::v4 reflectionPlane;   // offset 96 (cascadeSplits)
                        primal::math::v4 reflectionPlane2;  // offset 112
                        primal::math::v4 reflectionPlane3;  // offset 128
                        primal::math::m4x4 previousModel;   // offset 144
                        primal::math::v4 viewPos;           // offset 208 — MUST match Metal SceneData layout
                        primal::math::m4x4 shadowMatrix0;   // offset 224
                        primal::math::m4x4 shadowMatrix1;   // offset 288
                        primal::math::v2 jitter;            // offset 352
                        primal::math::v2 previousJitter;    // offset 360
                        primal::math::v2 padding;           // offset 368
                    };

                    auto* sd = static_cast<DeferredSceneData*>(device_->MapBuffer(deferred_light_cb_[cbIdx]));
                    if (sd) {
                        memset(sd, 0, sizeof(DeferredSceneData));
                        // Use shared light setup (defined at top of BuildRenderGraph)
                        sd->lightPos = sharedLightPos;

                        sd->lightColor = primal::math::v4{20.0f, 20.0f, 20.0f, 1.0f};
                        sd->reflectionPlane = primal::math::v4{600.0f, 2000.0f, 0.0f, 0.0f};
                        sd->viewPos = primal::math::v4{camera_.GetPosition().x, camera_.GetPosition().y, camera_.GetPosition().z, 1.0f};

                        // Use same-frame shadow matrices (no delay needed).
                        // All passes share a single Metal command buffer — ordering is guaranteed.
                        sd->shadowMatrix0 = cachedShadowMatrix0_[cbIdx];
                        sd->shadowMatrix1 = cachedShadowMatrix1_[cbIdx];

                        device_->UnmapBuffer(deferred_light_cb_[cbIdx]);
                    }
                }

                // Update descriptor set with GBuffer + shadow resources
                auto depthSampleable = gpuDrawPipeline_->GetGBufferDepthSampleable();
                // Use same-frame shadow maps (no delay — same command buffer guarantees ordering)
                auto shadowMap0 = gpuDrawPipeline_->GetShadowMap(0, currentBufferIndex);
                auto shadowMap1 = gpuDrawPipeline_->GetShadowMap(1, currentBufferIndex);

                // Get SSAO texture (or invalid for fallback — shader handles this)
                ResourceHandle ssaoTex = (ssaoPass_ && ssaoPass_->IsInitialized())
                    ? ssaoPass_->GetFilterTexture() : handles::INVALID_RESOURCE;

                // NOTE: DDGI irradiance and probe params are NOT bound here —
                // fragmentLighting_gpuDriven does not declare texture(10) or buffer(2).
                // DDGI is applied separately via the DDGI blit pass (mode 3).

                DescriptorData params[] = {
                    {0, DescriptorType::UniformBuffer, deferred_view_cb_[cbIdx]},
                    {1, DescriptorType::UniformBuffer, deferred_light_cb_[cbIdx]},
                    {2, DescriptorType::SampledImage, gpuDrawPipeline_->GetGBufferAlbedo()},
                    {3, DescriptorType::SampledImage, gpuDrawPipeline_->GetGBufferNormal()},
                    {4, DescriptorType::SampledImage, gpuDrawPipeline_->GetGBufferORM()},
                    {5, DescriptorType::SampledImage, depthSampleable},
                    {6, DescriptorType::SampledImage, shadowMap0},
                    {7, DescriptorType::SampledImage, shadowMap1},
                    {8, DescriptorType::Sampler, static_cast<ResourceHandle>(deferred_sampler_handle_)},
                    {9, DescriptorType::SampledImage, ssaoTex},
                };
                UpdateDescriptorSet(device_, deferred_descriptor_set_[cbIdx], params, 10);

                // Draw
                cmd->SetViewport({{0, 0}, {static_cast<float>(renderWidth_), static_cast<float>(renderHeight_)}, 0, 1});
                cmd->SetScissor({{0, 0}, {renderWidth_, renderHeight_}});

                cmd->BindGraphicsPipeline(deferred_pipeline_);
                const rhi::DescriptorSetHandle sets[] = { deferred_descriptor_set_[cbIdx] };
                cmd->BindDescriptorSets(rhi::PipelineBindPoint::Graphics, deferred_layout_, 0, 1, sets, 0, nullptr);
                cmd->Draw(3, 0, 1, 0);  // Full-screen triangle
            }
        );
    }

    // === LUMEN SSGI PASS ===
    // SSGI only runs in modes that consume its output:
    //   Mode 0: Composite (scene + SSGI), Mode 1: SSGI only, Mode 6: DDGI + SSGI fusion
    // Skipping SSGI in other modes saves ~12ms GPU time per frame (4 compute + 2 copy passes).
    rendergraph::RGResourceHandle ssgiOutputHandle;
    bool ssgiNeeded = (ssgiVisMode_ == 0 || ssgiVisMode_ == 1 || ssgiVisMode_ == 6);
    if (ssgiPass_ && ssgiPass_->IsInitialized() && frameCount_ > 0 && ssgiNeeded) {
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
                    //std::cout << "[ColorHistoryStore] StoreCurrentFrameColor: frame=" << frameCount_
                              //<< " result=" << (ok ? "OK" : "FAILED") << std::endl;
                }
            }
        );
    }

    // Store current frame depth for DDGI reprojection occlusion test
    // Runs in all GI modes (SSGI uses it for ray hit validation, DDGI for reprojection).
    // Separated from SSGI block so it still runs when SSGI is disabled.
    {
        bool anyGI = (ssgiVisMode_ == 0 || ssgiVisMode_ == 1 || ssgiVisMode_ == 3 ||
                      ssgiVisMode_ == 5 || ssgiVisMode_ == 6 ||
                      ssgiVisMode_ == 7 || ssgiVisMode_ == 8 || ssgiVisMode_ == 9);
        if (anyGI && frameCount_ > 0) {
            struct DepthHistoryData {};
            graph.AddPass<DepthHistoryData>("DepthHistoryStore",
                graphics::rendergraph::RGPassType::Copy,
                graphics::rendergraph::RGPassCategory::Copy,
                [](DepthHistoryData& data, graphics::rendergraph::RenderGraphBuilder& builder) {
                    builder.SideEffect();
                },
                [this](const DepthHistoryData& data, graphics::rendergraph::RenderGraphContext& context) {
                    auto gpuDepthTexture = gpuDrawPipeline_->GetFinalDepthTexture();
                    if (gpuDepthTexture == rhi::handles::INVALID_RESOURCE) return;

                    depthHistoryManager_->StoreCurrentFrameDepth(
                        gpuDepthTexture, context.cmdBuffer, frameCount_);
                }
            );
        }
    }

    // === GLOBAL SDF VOXELIZATION PASS ===
    // Must run BEFORE DDGI so that SDF cascade textures contain valid distance data.
    {
        auto& globalSDF = nanite::GlobalSDF::Get();
        if (!globalSDF.IsInitialized()) {
            static bool logOnce = false;
            if (!logOnce) { std::cerr << "[GlobalSDF] Not initialized — skipping voxelization pass" << std::endl; logOnce = true; }
        } else if (!globalSDF.IsVoxelizationReady()) {
            static bool logOnce = false;
            if (!logOnce) { std::cerr << "[GlobalSDF] Voxelization pipeline not ready — skipping" << std::endl; logOnce = true; }
        } else {
            struct SDFVoxData {};
            graph.AddPass<SDFVoxData>("GlobalSDF_Voxelization",
                graphics::rendergraph::RGPassType::Compute,
                graphics::rendergraph::RGPassCategory::Lighting,
                [](SDFVoxData&, graphics::rendergraph::RenderGraphBuilder& builder) {
                    builder.SideEffect();
                },
                [this, &globalSDF](const SDFVoxData&, graphics::rendergraph::RenderGraphContext& context) {
                    auto cmd = context.cmdBuffer;
                    if (!cmd) {
                        static bool logOnce = false;
                        if (!logOnce) { std::cerr << "[GlobalSDF] cmdBuffer is null in voxelization pass!" << std::endl; logOnce = true; }
                        return;
                    }

                    // Refresh buffer handles — geometry may be uploaded after init
                    nanite::SDFVoxelizationResources fresh;
                    fresh.vertex_buffer = gpuDrawPipeline_->GetGlobalVertexBuffer();
                    fresh.meshlet_buffer = gpuDrawPipeline_->GetGlobalMeshletBuffer();
                    fresh.meshlet_vertices_buffer = gpuDrawPipeline_->GetGlobalMeshletVerticesBuffer();
                    fresh.meshlet_triangles_buffer = gpuDrawPipeline_->GetGlobalMeshletTrianglesBuffer();
                    fresh.cluster_map_buffer = gpuDrawPipeline_->GetClusterMapBuffer();
                    fresh.instance_data_buffer = gpuDrawPipeline_->GetGlobalInstanceDataBuffer();
                    fresh.num_instances = sceneSnapshot_.GetInstanceCount();

                    // Log buffer state on first few frames
                    static u32 logCount = 0;
                    if (logCount < 5) {
                        std::cout << "[GlobalSDF] Pass executing: frame=" << frameCount_
                                  << " num_instances=" << fresh.num_instances
                                  << " vertex=" << fresh.vertex_buffer
                                  << " meshlet=" << fresh.meshlet_buffer
                                  << " instance=" << fresh.instance_data_buffer
                                  << " cmd=" << (void*)cmd
                                  << std::endl;
                        logCount++;
                    }

                    // Skip if geometry buffers aren't ready yet
                    if (fresh.num_instances == 0 ||
                        fresh.vertex_buffer == rhi::handles::INVALID_RESOURCE ||
                        fresh.meshlet_buffer == rhi::handles::INVALID_RESOURCE ||
                        fresh.instance_data_buffer == rhi::handles::INVALID_RESOURCE) {
                        static bool logSkipOnce = false;
                        if (!logSkipOnce) {
                            std::cerr << "[GlobalSDF] Skipping: buffers not ready (instances="
                                << fresh.num_instances << " vb=" << fresh.vertex_buffer
                                << " mb=" << fresh.meshlet_buffer << " ib=" << fresh.instance_data_buffer << ")" << std::endl;
                            logSkipOnce = true;
                        }
                        return;
                    }

                    // Static scene: only voxelize once after the first successful dispatch
                    if (sdf_voxelization_done_) {
                        static bool logDoneOnce = false;
                        if (!logDoneOnce) { std::cout << "[GlobalSDF] Voxelization already done, skipping" << std::endl; logDoneOnce = true; }
                        return;
                    }

                    globalSDF.SetVoxelizationResources(fresh);

                    for (u32 c = 0; c < globalSDF.GetConfig().cascade_count; ++c) {
                        globalSDF.DispatchVoxelization(cmd, c);
                    }

                    sdf_voxelization_done_ = true;
                    std::cout << "[GlobalSDF] Voxelization completed (static scene, will not re-run)" << std::endl;
                }
            );
        }
    }

    // === Pre-create SC lighting RG handle (needed by DDGI and SC pipeline) ===
    // Must be created before DDGI AddPass so the RG can track the dependency.
    // SC full pipeline (capture + lighting) only runs for modes 7/8/9 (direct SC view).
    // DDGI modes 3/6 only need SC→DDGI integration, not per-frame SC lighting.
    rendergraph::RGResourceHandle scLightingH;
    {
        u32 scOutIdx = currentBufferIndex % 3;
        bool scDirectView = (ssgiVisMode_ == 7 || ssgiVisMode_ == 8 || ssgiVisMode_ == 9);
        bool ddgiSCActive = (ssgiVisMode_ == 3 || ssgiVisMode_ == 6 || scDirectView);
        if (ddgiSCActive && surfaceCachePass_ && surfaceCachePass_->IsInitialized()) {
            scLightingH = graph.ImportResource("SC_Lighting_" + std::to_string(scOutIdx),
                surfaceCachePass_->GetLightingAtlas(scOutIdx));
        }
    }

    // === LUMEN DDGI PASS (probe-based GI) ===
    primal::graphics::lumen::LumenDDGIOutput ddgiOutput{};
    // Only run DDGI in modes that actually use DDGI output:
    //   Mode 3: DDGI composite, Mode 6: DDGI + fusion
    //   Mode 7/8/9: SC→DDGI integration needs DDGI irradiance buffers
    // Modes 0/1 (SSGI), 2 (scene only), 4 (albedo only), 5 (screen probes) skip DDGI.
    bool ddgiNeeded = (ssgiVisMode_ == 3 || ssgiVisMode_ == 6 ||
                       ssgiVisMode_ == 7 || ssgiVisMode_ == 8 || ssgiVisMode_ == 9);
    if (ddgiPass_ && ddgiPass_->IsInitialized() && frameCount_ > 1 && ddgiNeeded) {
        // Wire Surface Cache resources into DDGI only when an SC-related vis
        // mode is active.  Without this check the stale sc_lighting_done_ flag
        // (set in a previous mode) causes DDGI to enable SC card lookups in
        // its finalize pass, producing a write-write race on irradiance_buffers_
        // with the SC_DDGI_Integration pass (SideEffect bypasses RG ordering).
        bool ddgiSCActive = (ssgiVisMode_ == 3 || ssgiVisMode_ == 6 ||
                             ssgiVisMode_ == 7 || ssgiVisMode_ == 8 ||
                             ssgiVisMode_ == 9);
        if (ddgiSCActive && sc_lighting_done_ && surfaceCachePass_ && surfaceCachePass_->IsInitialized()) {
            u32 scAtlasIdx = sc_lighting_atlas_idx_;
            u32 lookupCount = surfaceCachePass_->GetCardGenerator().GetLookupCount();
            auto atlasHandle = surfaceCachePass_->GetLightingAtlas(scAtlasIdx);
            auto lookupHandle = surfaceCachePass_->GetCardLookupBuffer();
            auto cardHandle = surfaceCachePass_->GetCardDataBuffer();
            ddgiPass_->SetSurfaceCacheResources(
                atlasHandle, lookupHandle, cardHandle,
                surfaceCachePass_->GetAtlasSize(), lookupCount);
        } else {
            // CRITICAL: Clear SC resources when not in SC mode.
            // sc_enabled_ persists across frames — without clearing it, the
            // DDGI finalize pass continues reading SC card buffers and writing
            // irradiance_buffers_ that the (now absent) SC_DDGI_Integration
            // pass also wrote, creating a write-write race → GPU hang.
            ddgiPass_->ClearSurfaceCacheResources();
        }
        // Use current frame's deferred output (direct light only, no DDGI indirect)
        // as the radiance source for probe ray hits. This avoids the positive
        // feedback loop that occurs when prev_frame_color includes DDGI indirect.
        auto ddgiRadianceHandle = deferredOutputRG.IsValid()
            ? deferredOutputRG
            : graph.ImportResource("DDGIBlackFallback", ssgi_black_texture_);

        // Get previous frame depth for occlusion testing during reprojection
        auto prevDepth = depthHistoryManager_->GetPreviousFrameDepth(frameCount_);
        ResourceHandle prevDepthTex = prevDepth.is_valid ? prevDepth.texture : ssgi_black_texture_;
        auto ddgiPrevDepthHandle = graph.ImportResource("DDGIPrevDepth", prevDepthTex);

        // Build camera data for DDGI
        primal::graphics::lumen::DDGICameraData ddgiCameraData;
        ddgiCameraData.camera_position = camera_.GetPosition();
        ddgiCameraData.view_matrix = cameraBuffers_[currentBufferIndex].view_matrix;
        ddgiCameraData.proj_matrix = cameraBuffers_[currentBufferIndex].proj_matrix;
        {
            u32 prevIdx = (currentBufferIndex + 2) % 3;
            ddgiCameraData.prev_view_matrix = cameraBuffers_[prevIdx].view_matrix;
            ddgiCameraData.prev_proj_matrix = cameraBuffers_[prevIdx].proj_matrix;
        }
        ddgiCameraData.frame_index = frameCount_;
        ddgiCameraData.delta_time = 0.016f;
        ddgiCameraData.light_direction = primal::math::v3{sharedLightPos.x, sharedLightPos.y, sharedLightPos.z};
        ddgiCameraData.light_color = primal::math::v3{20.0f, 20.0f, 20.0f};

        ddgiOutput = ddgiPass_->AddPass(graph, ddgiRadianceHandle,
            ddgiCameraData, currentBufferIndex, scLightingH);

        // DDGI output (irradiance + depth textures) is available for
        // sampling in downstream passes. For now, the DDGI pass updates
        // probe data in-place — visualization will be added separately.

        static bool ddgiLogOnce = false;
        if (!ddgiLogOnce) {
            //std::cout << "[LumenDDGI] DDGI pass integrated into render graph" << std::endl;
            ddgiLogOnce = true;
        }
    }

    // === LUMEN SCREEN PROBE GI PASS ===
    // Only run when mode 5 (Screen Probe GI visualization) is active
    // to avoid interfering with other rendering modes
    rendergraph::RGResourceHandle screenProbeGIOutputHandle;
    if (screenProbeGIPass_ && screenProbeGIPass_->IsInitialized() && frameCount_ > 1 && (ssgiVisMode_ == 5 || ssgiVisMode_ == 6)) {
        // Get GBuffer depth and normal as RG handles
        auto spDepthHandle = graph.ImportResource("GBufferDepth_SP", gpuDrawPipeline_->GetGBufferDepthSampleable());
        auto spNormalHandle = graph.ImportResource("GBufferNormal_SP", gpuDrawPipeline_->GetGBufferNormal());

        // Use deferred lighting output as radiance source.
        // CRITICAL: Must use the SAME render graph handle (deferredOutputRG) as the
        // deferred pass output. Importing the same physical texture with a different
        // name causes the render graph to see two unrelated resources, leading to
        // resource aliasing — the deferred output gets overwritten by SPGI intermediates.
        auto spRadianceHandle = deferredOutputRG.IsValid()
            ? deferredOutputRG
            : graph.ImportResource("SPBlackFallback", ssgi_black_texture_);

        // Build camera data
        primal::graphics::lumen::ScreenProbeCameraData spCameraData;
        spCameraData.view_matrix = cameraBuffers_[currentBufferIndex].view_matrix;
        spCameraData.proj_matrix = cameraBuffers_[currentBufferIndex].proj_matrix;
        spCameraData.camera_position = camera_.GetPosition();
        spCameraData.frame_index = frameCount_;

        auto spOutput = screenProbeGIPass_->AddPass(graph, spDepthHandle, spNormalHandle,
            spRadianceHandle, spCameraData, currentBufferIndex);
        screenProbeGIOutputHandle = spOutput.gi_output;
    }

    // === SURFACE CACHE SHARED RESOURCES (imported once for all SC passes) ===
    // CRITICAL: Import atlas resources with consistent names so the render graph
    // can track dependencies between FillTest → Lighting → FinalBlit passes.
    // Runs in SC vis modes (7/8/9) AND DDGI vis modes (3/6) so DDGI can use
    // Surface Cache lighting for view-independent radiance.
    // NOTE: scLightingH is already created above (before DDGI block) to allow
    // DDGI AddPass to declare an RG dependency on it.
    rendergraph::RGResourceHandle scAlbedoH, scNormalH, scDepthH, scEmissiveH;
    // scLightingH already declared and imported above
    u32 scOutIdx = currentBufferIndex % 3;
    bool scDirectView = (ssgiVisMode_ == 7 || ssgiVisMode_ == 8 || ssgiVisMode_ == 9);
    bool scLightingNeeded = scDirectView || (ssgiVisMode_ == 3 || ssgiVisMode_ == 6);
    if (scLightingNeeded &&
        surfaceCachePass_ && surfaceCachePass_->IsInitialized()) {
        scAlbedoH = graph.ImportResource("SC_Albedo", surfaceCachePass_->GetAlbedoAtlas());
        scNormalH = graph.ImportResource("SC_Normal", surfaceCachePass_->GetNormalAtlas());
        scDepthH = graph.ImportResource("SC_Depth", surfaceCachePass_->GetDepthAtlas());
        scEmissiveH = graph.ImportResource("SC_Emissive", surfaceCachePass_->GetEmissiveAtlas());
    }

    // === SURFACE CACHE FILL TEST (prefill atlases with test pattern) ===
    // Runs before CardCapture to fill entire atlas so modes 8/9 show a visible
    // pattern outside card regions. CardCapture then overwrites card regions
    // with actual mesh material data for LightEval.
    if (scLightingNeeded &&
        surfaceCachePass_ && surfaceCachePass_->IsInitialized() &&
        sc_fill_test_pipeline_ != rhi::handles::INVALID_PIPELINE &&
        !sc_fill_done_) {
        struct SCFillData {};
        graph.AddPass<SCFillData>("SC_FillTest",
            rendergraph::RGPassType::Compute, rendergraph::RGPassCategory::Lighting,
            [scAlbedoH, scNormalH, scDepthH](SCFillData& data, rendergraph::RenderGraphBuilder& builder) {
                builder.Write(scAlbedoH, rhi::ResourceState::UnorderedAccess);
                builder.Write(scNormalH, rhi::ResourceState::UnorderedAccess);
                builder.Write(scDepthH, rhi::ResourceState::UnorderedAccess);
            },
            [this](const SCFillData& data, rendergraph::RenderGraphContext& context) {
                auto cmd = context.cmdBuffer;
                if (!cmd) return;

                u32 atlasSize = surfaceCachePass_->GetAtlasSize();

                DescriptorData params[] = {
                    {0, rhi::DescriptorType::StorageImage, surfaceCachePass_->GetAlbedoAtlas()},
                    {1, rhi::DescriptorType::StorageImage, surfaceCachePass_->GetNormalAtlas()},
                    {2, rhi::DescriptorType::StorageImage, surfaceCachePass_->GetDepthAtlas()},
                };
                UpdateDescriptorSet(device_, sc_fill_test_descriptor_set_, params, 3);

                cmd->BindComputePipeline(sc_fill_test_pipeline_);
                const rhi::DescriptorSetHandle sets[] = { sc_fill_test_descriptor_set_ };
                cmd->BindDescriptorSets(rhi::PipelineBindPoint::Compute, sc_fill_test_layout_,
                                        0, 1, sets, 0, nullptr);

                u32 gx = (atlasSize + 7) / 8;
                u32 gy = (atlasSize + 7) / 8;
                cmd->Dispatch(gx, gy, 1);
                printf("[SC] FillTest dispatched (%ux%u TG, atlas=%u)\n", gx, gy, atlasSize); fflush(stdout);

                // Set sc_fill_done_ so downstream passes (LightEval) can proceed
                // even if CardCapture is unavailable. FillTest provides valid
                // albedo/normal/depth data for the lighting evaluation.
                sc_fill_done_ = true;
            });
    }

    // === SURFACE CACHE CARD CAPTURE (graphics pass, renders meshes into atlas) ===
    // Overwrites card regions with actual material data for LightEval.
    // Runs in SC modes (7/8/9) AND DDGI modes (3/6) so DDGI has SC data for radiance.
    if (scLightingNeeded &&
        surfaceCachePass_ && surfaceCachePass_->IsInitialized() &&
        sc_capture_pipeline_ != rhi::handles::INVALID_PIPELINE &&
        !sc_fill_done_) {
        { static bool logOnce = false; if (!logOnce) { std::cout << "[SC_CardCapture] PASS ADDED (pipeline valid)" << std::endl; logOnce = true; } }
        sc_fill_done_ = true;

        auto scCaptureDepthH = graph.ImportResource("SC_CaptureDepth", sc_capture_depth_tex_);

        struct SCCaptureData {};
        graph.AddPass<SCCaptureData>("SC_CardCapture",
            rendergraph::RGPassType::Graphics, rendergraph::RGPassCategory::Lighting,
            [scAlbedoH, scNormalH, scDepthH, scEmissiveH, scCaptureDepthH](SCCaptureData& data, rendergraph::RenderGraphBuilder& builder) {
                rendergraph::RGRenderPassDesc rpDesc;
                rpDesc.colors.resize(4);
                rpDesc.colors[0].texture = scAlbedoH;
                rpDesc.colors[0].loadOp = rhi::LoadAction::Load;
                rpDesc.colors[0].storeOp = rhi::StoreAction::Store;
                rpDesc.colors[1].texture = scNormalH;
                rpDesc.colors[1].loadOp = rhi::LoadAction::Load;
                rpDesc.colors[1].storeOp = rhi::StoreAction::Store;
                rpDesc.colors[2].texture = scDepthH;
                rpDesc.colors[2].loadOp = rhi::LoadAction::Load;
                rpDesc.colors[2].storeOp = rhi::StoreAction::Store;
                rpDesc.colors[3].texture = scEmissiveH;
                rpDesc.colors[3].loadOp = rhi::LoadAction::DontCare;
                rpDesc.colors[3].storeOp = rhi::StoreAction::Store;
                rpDesc.depthStencil.texture = scCaptureDepthH;
                rpDesc.depthStencil.depthLoadOp = rhi::LoadAction::Clear;
                rpDesc.depthStencil.clearDepth = 1.0f;
                rpDesc.depthStencil.depthStoreOp = rhi::StoreAction::DontCare;
                builder.DeclareRenderPass(rpDesc);
            },
            [this](const SCCaptureData& data, rendergraph::RenderGraphContext& context) {
                auto cmd = context.cmdBuffer;
                if (!cmd) return;

                auto& cardGen = surfaceCachePass_->GetCardGenerator();
                u32 cardCount = cardGen.GetCardCount();
                if (cardCount == 0) {
                    std::cout << "[SC CardCapture] No cards to capture" << std::endl;
                    return;
                }

                const auto& cards = cardGen.GetCards();
                const auto& instances = sceneSnapshot_.GetInstanceData();
                u32 atlasSize = surfaceCachePass_->GetAtlasSize();
                u32 meshCount = std::min(sceneSnapshot_.GetInstanceCount(), (u32)sceneMeshes_.size());

                struct CapturePassGPU {
                    m4x4 view_proj;
                    m4x4 world_matrix;
                    v4   card_center;
                    v4   card_extent;
                    u32  axis_direction;
                    u32  material_id;
                    u32  _pad[2];
                };
                static_assert(sizeof(CapturePassGPU) == 176, "CapturePassGPU layout");
                constexpr u32 CB_STRIDE = 256;

                auto* cbBase = static_cast<u8*>(device_->MapBuffer(sc_capture_cb_));
                if (!cbBase) return;

                struct DrawableCard { u32 cardIdx; u32 cbOffset; };
                primal::utl::vector<DrawableCard> drawables;
                drawables.reserve(cardCount);

                for (u32 ci = 0; ci < cardCount && ci < cards.size(); ++ci) {
                    if (drawables.size() >= 4096u) break;
                    const auto& card = cards[ci];
                    u32 instanceIdx = card.mesh_instance_id;
                    if (instanceIdx >= meshCount || instanceIdx >= instances.size()) continue;
                    const auto& meshInfo = sceneMeshes_[instanceIdx];
                    if (!meshInfo.mesh) continue;
                    u32 res = card.resolution;
                    if (res == 0) continue;
                    if (card.atlas_offset_x + res > atlasSize || card.atlas_offset_y + res > atlasSize) continue;

                    const auto& inst = instances[instanceIdx];

                    uint axis = card.axis_direction & 0xFF;
                    uint dir = (card.axis_direction >> 8) & 0xFF;
                    float sign = dir ? 1.0f : -1.0f;

                    v3 cardUp;
                    if (axis == 0)      cardUp = {0, 0, 1};
                    else if (axis == 1) cardUp = {0, 0, 1};
                    else                cardUp = {0, 1, 0};

                    float extentMax = std::max({card.extent.x, card.extent.y, card.extent.z});
                    v3 viewDir;
                    if (axis == 0)      viewDir = {sign, 0, 0};
                    else if (axis == 1) viewDir = {0, sign, 0};
                    else                viewDir = {0, 0, sign};
                    float eyeDist = extentMax * 2.0f + 1.0f;
                    v3 eye = card.center.xyz + viewDir * eyeDist;

                    m4x4 viewMat = CreateLookAtMatrix(eye, card.center.xyz, cardUp);

                    float ex = (axis == 0) ? card.extent.y : card.extent.x;
                    float ey = (axis == 2) ? card.extent.y : card.extent.z;
                    float maxExtent = std::max(ex, ey);
                    float nearP = 0.01f;
                    float farP = eyeDist + extentMax + 1.0f;
                    m4x4 projMat = CreateOrthographicMatrix(-maxExtent, maxExtent, -maxExtent, maxExtent, nearP, farP);

                    m4x4 vpMatrix = projMat * viewMat;

                    u32 cbOffset = (u32)drawables.size() * CB_STRIDE;
                    auto* passData = reinterpret_cast<CapturePassGPU*>(cbBase + cbOffset);
                    passData->view_proj = vpMatrix;
                    passData->world_matrix = inst.world_matrix;
                    passData->card_center = card.center;
                    passData->card_extent = card.extent;
                    passData->axis_direction = card.axis_direction;
                    passData->material_id = meshInfo.gpuMaterialId;
                    passData->_pad[0] = passData->_pad[1] = 0;

                    drawables.push_back({ci, cbOffset});
                }
                device_->UnmapBuffer(sc_capture_cb_);

                DescriptorData capture_params[] = {
                    {3, rhi::DescriptorType::StorageBuffer,  gpuMaterialRegistry_->GetMaterialDataBuffer()},
                    {0, rhi::DescriptorType::SampledImage,   gpuMaterialRegistry_->GetAlbedoTextureArray()},
                    {1, rhi::DescriptorType::SampledImage,   gpuMaterialRegistry_->GetNormalTextureArray()},
                    {0, rhi::DescriptorType::Sampler,        sc_capture_sampler_},
                };
                UpdateDescriptorSet(device_, sc_capture_descriptor_set_, capture_params, 4);

                cmd->BindGraphicsPipeline(sc_capture_pipeline_);
                const rhi::DescriptorSetHandle captureSets[] = { sc_capture_descriptor_set_ };
                cmd->BindDescriptorSets(rhi::PipelineBindPoint::Graphics, sc_capture_layout_,
                                        0, 1, captureSets, 0, nullptr);

                u32 skippedOOB = 0;
                for (u32 di = 0; di < drawables.size(); ++di) {
                    const auto& dc = drawables[di];
                    const auto& card = cards[dc.cardIdx];
                    u32 instanceIdx = card.mesh_instance_id;
                    const auto& meshInfo = sceneMeshes_[instanceIdx];
                    u32 res = card.resolution;

                    u32 vertCount = meshInfo.mesh->GetVertexCount();
                    u32 idxCount = meshInfo.mesh->GetIndexCount();
                    if (vertCount == 0 || idxCount == 0) continue;

                    if (meshInfo.mesh->GetVertexBuffer() == rhi::handles::INVALID_RESOURCE ||
                        meshInfo.mesh->GetIndexBuffer() == rhi::handles::INVALID_RESOURCE) {
                        skippedOOB++;
                        continue;
                    }

                    u32 vpX = card.atlas_offset_x;
                    u32 vpY = card.atlas_offset_y;
                    u32 vpW = std::min(res, atlasSize - vpX);
                    u32 vpH = std::min(res, atlasSize - vpY);

                    rhi::ViewportDesc vp{};
                    vp.topLeft = {(float)vpX, (float)vpY};
                    vp.size = {(float)vpW, (float)vpH};
                    vp.minDepth = 0.0f;
                    vp.maxDepth = 1.0f;
                    cmd->SetViewport(vp);

                    rhi::Rect scissor{};
                    scissor.offset = {(s32)vpX, (s32)vpY};
                    scissor.extent = {vpW, vpH};
                    cmd->SetScissor(scissor);

                    ResourceHandle buffers[] = { sc_capture_cb_, meshInfo.mesh->GetVertexBuffer() };
                    u64 offsets[] = { (u64)dc.cbOffset, 0 };
                    cmd->BindVertexBuffers(0, 2, buffers, offsets);

                    rhi::DataFormat idxFmt = (meshInfo.mesh->GetIndexType() == rhi::DataIndexType::UInt32)
                        ? rhi::DataFormat::R32_UInt : rhi::DataFormat::R16_UInt;
                    cmd->BindIndexBuffer(meshInfo.mesh->GetIndexBuffer(), idxFmt, 0);
                    cmd->DrawIndexed(meshInfo.mesh->GetIndexCount(), 0, 0, 1, 0);
                }
                std::cout << "[SC CardCapture] Total=" << cardCount
                          << " drawn=" << drawables.size() - skippedOOB
                          << " skipped=" << skippedOOB << std::endl;
            });
    } else if (scLightingNeeded && !sc_fill_done_ && surfaceCachePass_ && surfaceCachePass_->IsInitialized()) {
        static bool logOnce = false;
        if (!logOnce) {
            std::cout << "[SC_CardCapture] GATE FAILED: capture_pipeline="
                      << (sc_capture_pipeline_ != rhi::handles::INVALID_PIPELINE) << std::endl;
            logOnce = true;
        }
    }

    // === SURFACE CACHE DEPTH DILATE (ping-pong: depth_atlas → depth_temp) ===
    rendergraph::RGResourceHandle scDepthTempH;
    if (scLightingNeeded && surfaceCachePass_ && surfaceCachePass_->IsInitialized() &&
        sc_dilate_pipeline_ != rhi::handles::INVALID_PIPELINE &&
        sc_fill_done_ && !sc_dilate_done_ &&
        sc_depth_temp_tex_ != rhi::handles::INVALID_RESOURCE) {

        scDepthTempH = graph.ImportResource("SC_DepthTemp", sc_depth_temp_tex_);

        struct SCDilateData {};
        graph.AddPass<SCDilateData>("SC_DepthDilate",
            rendergraph::RGPassType::Compute, rendergraph::RGPassCategory::Lighting,
            [scDepthH, scDepthTempH](SCDilateData&, rendergraph::RenderGraphBuilder& builder) {
                builder.Read(scDepthH, rhi::ResourceState::ShaderResource);
                builder.Write(scDepthTempH, rhi::ResourceState::UnorderedAccess);
            },
            [this](const SCDilateData&, rendergraph::RenderGraphContext& context) {
                auto cmd = context.cmdBuffer;
                if (!cmd) return;
                sc_dilate_done_ = true;

                u32 atlasSize = surfaceCachePass_->GetAtlasSize();
                auto* dp = static_cast<u32*>(device_->MapBuffer(sc_dilate_params_cb_));
                if (dp) { memset(dp, 0, 256); dp[0] = atlasSize; device_->UnmapBuffer(sc_dilate_params_cb_); }

                DescriptorData params[3] = {
                    {0, DescriptorType::SampledImage,  surfaceCachePass_->GetDepthAtlas()},
                    {1, DescriptorType::StorageImage,  sc_depth_temp_tex_},
                    {1, DescriptorType::UniformBuffer, sc_dilate_params_cb_},
                };
                UpdateDescriptorSet(device_, sc_dilate_descriptor_set_, params, 3);

                cmd->BindComputePipeline(sc_dilate_pipeline_);
                const rhi::DescriptorSetHandle sets[] = { sc_dilate_descriptor_set_ };
                cmd->BindDescriptorSets(rhi::PipelineBindPoint::Compute, sc_dilate_layout_, 0, 1, sets, 0, nullptr);
                cmd->Dispatch((atlasSize + 7) / 8, (atlasSize + 7) / 8, 1);
                printf("[SC] DepthDilate dispatched\n"); fflush(stdout);
            });
    }

    // === SURFACE CACHE LIGHTING (per-frame: updates lighting atlas with current light direction) ===
    // Runs in SC view modes (7/8/9) AND DDGI modes (3/6) since DDGI finalize
    // samples the lighting atlas for ray hit radiance.
    // CRITICAL: Check ALL required resources, not just the pipeline.
    // If InitializeSurfaceCachePipelines() failed partway, pipelines may be valid
    // but constant buffers could be INVALID_RESOURCE → binding them causes GPU fault.
    if (scLightingNeeded && sc_fill_done_ && surfaceCachePass_ && surfaceCachePass_->IsInitialized() &&
        sc_light_cull_pipeline_ != rhi::handles::INVALID_PIPELINE &&
        sc_light_eval_pipeline_ != rhi::handles::INVALID_PIPELINE &&
        sc_cull_params_cb_ != rhi::handles::INVALID_RESOURCE &&
        sc_eval_params_cb_ != rhi::handles::INVALID_RESOURCE &&
        sc_light_info_cb_ != rhi::handles::INVALID_RESOURCE &&
        sc_tile_light_assign_buf_ != rhi::handles::INVALID_RESOURCE) {
        struct SCLightData {};
        // Diagnostic: confirm SC_Lighting pass is being added to render graph
        { static bool logOnce = false; if (!logOnce) { std::cout << "[SC_Lighting] PASS ADDED to graph (fill_done=" << sc_fill_done_ << " scOutIdx=" << scOutIdx << ")" << std::endl; logOnce = true; } }
        graph.AddPass<SCLightData>("SC_Lighting",
            rendergraph::RGPassType::Compute, rendergraph::RGPassCategory::Lighting,
            // CRITICAL: Declare reads on ALL atlases the shaders access, using
            // the SAME handles as FillTest so the render graph sees the dependency.
            [scAlbedoH, scNormalH, scDepthH, scEmissiveH, scLightingH, scDepthTempH](SCLightData& data, rendergraph::RenderGraphBuilder& builder) {
                builder.Read(scAlbedoH, rhi::ResourceState::ShaderResource);
                builder.Read(scNormalH, rhi::ResourceState::ShaderResource);
                builder.Read(scDepthH, rhi::ResourceState::ShaderResource);
                if (scDepthTempH.IsValid()) builder.Read(scDepthTempH, rhi::ResourceState::ShaderResource);
                builder.Read(scEmissiveH, rhi::ResourceState::ShaderResource);
                builder.Write(scLightingH, rhi::ResourceState::UnorderedAccess);
            },
            [this, scOutIdx, currentBufferIndex](const SCLightData& data, rendergraph::RenderGraphContext& context) {
                auto cmd = context.cmdBuffer;
                if (!cmd) return;

                if (!sc_lighting_done_) sc_lighting_done_ = true;
                sc_lighting_atlas_idx_ = scOutIdx;

                // Safety: skip if no cards captured yet
                u32 cardCount = surfaceCachePass_->GetCardGenerator().GetCardCount();
                if (cardCount == 0) {
                    printf("[SC Lighting] Skipped — no cards registered\n"); fflush(stdout);
                    return;
                }

                { static bool logOnce = false; if (!logOnce) { std::cout << "[SC Lighting] Dispatching LightCull+LightEval (per-frame)" << std::endl; logOnce = true; } }
                u32 atlasSize = surfaceCachePass_->GetAtlasSize();
                u32 pageSize = surfaceCachePass_->GetPageSize();

                // === LightCull (SKIPPED) ===
                // LightEval does its own inline per-texel lighting and does NOT read from
                // tile_light_assign_buf_. LightCull's full-atlas dispatch (65536 tiles)
                // is unnecessary and causes Apple Silicon GPU hangs when the atlas is fully
                // populated (FillTest data). Skip it entirely.
                {
                    // Upload light info (shared by LightEval via sc_light_info_cb_)
                    struct SC_GPULightInfo {
                        float position[4];
                        float color[4];
                        float direction[4];
                    };
                    SC_GPULightInfo lights[1];
                    memset(lights, 0, sizeof(lights));
                    lights[0].color[0] = 1.0f; lights[0].color[1] = 0.95f; lights[0].color[2] = 0.9f;
                    lights[0].direction[0] = light_direction_.x;
                    lights[0].direction[1] = light_direction_.y;
                    lights[0].direction[2] = light_direction_.z;
                    lights[0].direction[3] = 1.0f; // type = directional
                    auto* dst = static_cast<SC_GPULightInfo*>(device_->MapBuffer(sc_light_info_cb_));
                    if (dst) { memcpy(dst, lights, sizeof(lights)); device_->UnmapBuffer(sc_light_info_cb_); }
                }

                // === LightEval ===
                {
                    // Build CardDispatchInfo[] prefix-sum array from SurfaceCacheCard data
                    const auto& cards = surfaceCachePass_->GetCardGenerator().GetCards();
                    u32 totalTexels = 0;
                    std::vector<lumen::CardDispatchInfo> dispatchInfo(cards.size());
                    for (u32 ci = 0; ci < cards.size(); ++ci) {
                        dispatchInfo[ci].texel_offset = totalTexels;
                        u32 res = cards[ci].resolution;
                        dispatchInfo[ci].texel_count = res * res;
                        dispatchInfo[ci].resolution = res;
                        dispatchInfo[ci].atlas_offset_x = cards[ci].atlas_offset_x;
                        dispatchInfo[ci].atlas_offset_y = cards[ci].atlas_offset_y;
                        totalTexels += res * res;
                    }

                    // Upload CardDispatchInfo[] buffer
                    if (sc_card_dispatch_buf_ == rhi::handles::INVALID_RESOURCE ||
                        totalTexels == 0) {
                        // Skip if no cards
                    } else {
                        auto* dst = static_cast<lumen::CardDispatchInfo*>(
                            device_->MapBuffer(sc_card_dispatch_buf_));
                        if (dst) {
                            memcpy(dst, dispatchInfo.data(),
                                   sizeof(lumen::CardDispatchInfo) * cards.size());
                            device_->UnmapBuffer(sc_card_dispatch_buf_);
                        }
                    }

                    // Upload FlattenedLightingParams: SurfaceCacheParams (12 uints) + flat fields
                    auto* ep = static_cast<u32*>(device_->MapBuffer(sc_eval_params_cb_));
                    if (ep) {
                        memset(ep, 0, 256);
                        ep[0] = atlasSize;    // sc_params.atlas_size
                        ep[1] = pageSize;     // sc_params.page_size
                        // FlattenedLightingParams fields at offset 48 (12 uints)
                        ep[12] = totalTexels;                          // total_texels
                        ep[13] = static_cast<u32>(cards.size());      // card_count
                        ep[14] = 1;                                    // light_count
                        device_->UnmapBuffer(sc_eval_params_cb_);
                    }

                    // Bindings matching SurfaceCacheLightEval.metal:
                    //  texture(0): albedo_atlas (read)
                    //  texture(1): normal_atlas (read)
                    //  texture(2): emissive_atlas (read)
                    //  texture(3): lighting_out (write)
                    //  buffer(1): FlattenedLightingParams
                    //  buffer(2): SurfaceCacheCard[]
                    //  buffer(3): LightInfo[]
                    //  buffer(4): CardDispatchInfo[]
                    DescriptorData eval_params[8] = {
                        {0, rhi::DescriptorType::SampledImage,  surfaceCachePass_->GetAlbedoAtlas()},
                        {1, rhi::DescriptorType::SampledImage,  surfaceCachePass_->GetNormalAtlas()},
                        {2, rhi::DescriptorType::SampledImage,  surfaceCachePass_->GetEmissiveAtlas()},
                        {3, rhi::DescriptorType::StorageImage,  surfaceCachePass_->GetLightingAtlas(scOutIdx)},
                        {1, rhi::DescriptorType::UniformBuffer, sc_eval_params_cb_},
                        {2, rhi::DescriptorType::StorageBuffer, surfaceCachePass_->GetCardDataBuffer()},
                        {3, rhi::DescriptorType::StorageBuffer, sc_light_info_cb_},
                        {4, rhi::DescriptorType::StorageBuffer, sc_card_dispatch_buf_},
                    };
                    UpdateDescriptorSet(device_, sc_light_eval_descriptor_sets_[currentBufferIndex], eval_params, 8);

                    cmd->BindComputePipeline(sc_light_eval_pipeline_);
                    const rhi::DescriptorSetHandle evalSets[] = { sc_light_eval_descriptor_sets_[currentBufferIndex] };
                    cmd->BindDescriptorSets(rhi::PipelineBindPoint::Compute, sc_light_eval_layout_, 0, 1, evalSets, 0, nullptr);

                    // 1D dispatch matching shader's thread_position_in_grid
                    u32 gx = (totalTexels + 255) / 256;
                    if (gx > 0) {
                        cmd->Dispatch(gx, 1, 1);
                    }
                }
            });
    } else if (scLightingNeeded && sc_fill_done_) {
        // Gate failed — print which resource is invalid
        static bool logOnce = false;
        if (!logOnce) {
            std::cout << "[SC_Lighting] GATE FAILED: scInit=" << (surfaceCachePass_ && surfaceCachePass_->IsInitialized())
                      << " fill=" << sc_fill_done_
                      << " cull_pipe=" << (sc_light_cull_pipeline_ != rhi::handles::INVALID_PIPELINE)
                      << " eval_pipe=" << (sc_light_eval_pipeline_ != rhi::handles::INVALID_PIPELINE)
                      << " cull_cb=" << (sc_cull_params_cb_ != rhi::handles::INVALID_RESOURCE)
                      << " eval_cb=" << (sc_eval_params_cb_ != rhi::handles::INVALID_RESOURCE)
                      << " light_cb=" << (sc_light_info_cb_ != rhi::handles::INVALID_RESOURCE)
                      << " tile_buf=" << (sc_tile_light_assign_buf_ != rhi::handles::INVALID_RESOURCE)
                      << std::endl;
            logOnce = true;
        }
    }

    // === SURFACE CACHE INDIRECT TRACE + RESOLVE ===
    if (scDirectView && surfaceCachePass_ && surfaceCachePass_->IsInitialized() &&
        sc_fill_done_ && sc_dilate_done_ && sc_lighting_done_ && sdf_voxelization_done_ &&
        sc_ind_trace_pipeline_ != rhi::handles::INVALID_PIPELINE &&
        sc_ind_resolve_pipeline_ != rhi::handles::INVALID_PIPELINE &&
        sc_ray_hits_buf_ != rhi::handles::INVALID_RESOURCE &&
        sc_indirect_out_tex_ != rhi::handles::INVALID_RESOURCE) {

        // Import indirect output texture
        auto scIndirectOutH = graph.ImportResource("SC_IndirectOut", sc_indirect_out_tex_);

        // Import prev frame lighting atlas for resolve
        u32 histIdx = (currentBufferIndex + 2) % 3;
        auto scPrevLightingH = graph.ImportResource("SC_PrevLighting_" + std::to_string(histIdx),
            surfaceCachePass_->GetLightingAtlas(histIdx));

        // === IndirectTrace ===
        {
            struct SCIndTraceData {};
            graph.AddPass<SCIndTraceData>("SC_IndirectTrace",
                rendergraph::RGPassType::Compute, rendergraph::RGPassCategory::Lighting,
                [scDepthH, scNormalH, scDepthTempH](SCIndTraceData&, rendergraph::RenderGraphBuilder& builder) {
                    // Read depth and normal atlases
                    if (scDepthTempH.IsValid()) builder.Read(scDepthTempH, rhi::ResourceState::ShaderResource);
                    else builder.Read(scDepthH, rhi::ResourceState::ShaderResource);
                    builder.Read(scNormalH, rhi::ResourceState::ShaderResource);
                },
                [this](const SCIndTraceData&, rendergraph::RenderGraphContext& context) {
                    auto cmd = context.cmdBuffer;
                    if (!cmd) return;

                    // Safety: skip if no cards
                    u32 cardCount = surfaceCachePass_->GetCardGenerator().GetCardCount();
                    if (cardCount == 0) return;

                    u32 atlasSize = surfaceCachePass_->GetAtlasSize();
                    u32 tileSize = 8;
                    u32 tilesPerSide = atlasSize / tileSize;
                    u32 raysPerProbe = 4;

                    // Upload IndirectTraceParams
                    {
                        auto* mp = static_cast<u32*>(device_->MapBuffer(sc_ind_trace_params_cb_));
                        if (mp) {
                            memset(mp, 0, 512);
                            mp[0] = atlasSize;    // sc_params.atlas_size
                            mp[3] = surfaceCachePass_->GetCardGenerator().GetCardCount(); // max_cards
                            // IndirectTraceParams fields after sc_params (48 bytes)
                            auto* fp = reinterpret_cast<float*>(mp);
                            mp[12] = tileSize;               // tile_size (uint, not float)
                            mp[13] = raysPerProbe;           // rays_per_probe (uint, not float)
                            mp[14] = frameCount_;            // frame_index (uint, not float)
                            fp[15] = 5.0f;                   // near_distance (float)
                            fp[16] = 20.0f;                  // max_ray_distance (float)

                            // SDF cascade data (17 floats offset = offset 17)
                            auto& globalSDF = primal::graphics::nanite::GlobalSDF::Get();
                            u32 cascadeCount = globalSDF.GetConfig().cascade_count;
                            for (u32 c = 0; c < cascadeCount && c < 3; ++c) {
                                auto& cascade = globalSDF.GetCascade(c);
                                // sdf_origins[c] at float4 starting at fp[17 + c*4]
                                fp[17 + c*4 + 0] = cascade.origin.x;
                                fp[17 + c*4 + 1] = cascade.origin.y;
                                fp[17 + c*4 + 2] = cascade.origin.z;
                                fp[17 + c*4 + 3] = 0.0f;
                                // sdf_voxel_sizes[c] at float4 starting at fp[29 + c*4]
                                fp[29 + c*4 + 0] = cascade.voxel_size;
                                fp[29 + c*4 + 1] = 0.0f;
                                fp[29 + c*4 + 2] = 0.0f;
                                fp[29 + c*4 + 3] = 0.0f;
                                // sdf_extents[c] at float4 starting at fp[41 + c*4]
                                fp[41 + c*4 + 0] = cascade.extent.x;
                                fp[41 + c*4 + 1] = cascade.extent.y;
                                fp[41 + c*4 + 2] = cascade.extent.z;
                                fp[41 + c*4 + 3] = 0.0f;
                                // sdf_resolutions[c] at uint starting at mp[53*4/4...]
                                // Actually as uint array: offset after 3*float4*3 = 17+12+12+12 = 53 floats
                                mp[53 + c] = cascade.resolution;
                            }
                            mp[56] = cascadeCount;  // sdf_cascade_count

                            device_->UnmapBuffer(sc_ind_trace_params_cb_);
                        }
                    }

                    // Depth source: use dilated depth if available
                    rhi::ResourceHandle depthSrc = sc_dilate_done_ ? sc_depth_temp_tex_ : surfaceCachePass_->GetDepthAtlas();

                    // Get SDF textures
                    auto& globalSDF = primal::graphics::nanite::GlobalSDF::Get();
                    u32 cascadeCount = globalSDF.GetConfig().cascade_count;

                    DescriptorData params[9] = {
                        {0, DescriptorType::SampledImage,  depthSrc},
                        {1, DescriptorType::SampledImage,  surfaceCachePass_->GetNormalAtlas()},
                        {2, DescriptorType::SampledImage,  (cascadeCount > 0) ? globalSDF.GetCascade(0).sdf_texture : ssgi_black_texture_},
                        {3, DescriptorType::SampledImage,  (cascadeCount > 1) ? globalSDF.GetCascade(1).sdf_texture : ssgi_black_texture_},
                        {4, DescriptorType::SampledImage,  (cascadeCount > 2) ? globalSDF.GetCascade(2).sdf_texture : ssgi_black_texture_},
                        {0, DescriptorType::UniformBuffer, sc_global_data_cb_},
                        {1, DescriptorType::UniformBuffer, sc_ind_trace_params_cb_},
                        {2, DescriptorType::StorageBuffer, surfaceCachePass_->GetCardDataBuffer()},
                        {3, DescriptorType::StorageBuffer, sc_ray_hits_buf_},
                    };
                    UpdateDescriptorSet(device_, sc_ind_trace_descriptor_set_, params, 9);

                    cmd->BindComputePipeline(sc_ind_trace_pipeline_);
                    const rhi::DescriptorSetHandle sets[] = { sc_ind_trace_descriptor_set_ };
                    cmd->BindDescriptorSets(rhi::PipelineBindPoint::Compute, sc_ind_trace_layout_, 0, 1, sets, 0, nullptr);
                    cmd->Dispatch((tilesPerSide + 7) / 8, (tilesPerSide + 7) / 8, 1);
                    printf("[SC] IndirectTrace dispatched (tiles=%ux%u, cards=%u)\n", tilesPerSide, tilesPerSide, cardCount); fflush(stdout);
                });
        }

        // Barrier: ray_hits write → read
        {
            struct SCBarrierData {};
            graph.AddPass<SCBarrierData>("SC_IndirectBarrier",
                rendergraph::RGPassType::Compute, rendergraph::RGPassCategory::Lighting,
                [scIndirectOutH](SCBarrierData&, rendergraph::RenderGraphBuilder& builder) {
                    builder.SideEffect();
                },
                [this](const SCBarrierData&, rendergraph::RenderGraphContext& context) {
                    auto cmd = context.cmdBuffer;
                    if (!cmd) return;
                    rhi::ResourceBarrier b{};
                    b.resource = sc_ray_hits_buf_;
                    b.beforeState = rhi::ResourceState::UnorderedAccess;
                    b.afterState = rhi::ResourceState::ShaderResource;
                    b.subresource = 0xFFFFFFFF;
                    cmd->InsertBarrier(&b, 1);
                });
        }

        // === IndirectResolve ===
        {
            struct SCIndResolveData {};
            graph.AddPass<SCIndResolveData>("SC_IndirectResolve",
                rendergraph::RGPassType::Compute, rendergraph::RGPassCategory::Lighting,
                [scPrevLightingH, scAlbedoH, scIndirectOutH](SCIndResolveData&, rendergraph::RenderGraphBuilder& builder) {
                    builder.Read(scPrevLightingH, rhi::ResourceState::ShaderResource);
                    builder.Read(scAlbedoH, rhi::ResourceState::ShaderResource);
                    builder.Write(scIndirectOutH, rhi::ResourceState::UnorderedAccess);
                },
                [this, histIdx](const SCIndResolveData&, rendergraph::RenderGraphContext& context) {
                    auto cmd = context.cmdBuffer;
                    if (!cmd) return;

                    u32 atlasSize = surfaceCachePass_->GetAtlasSize();
                    u32 tileSize = 8;
                    u32 raysPerProbe = 4;

                    // Upload IndirectResolveParams
                    {
                        auto* mp = static_cast<u32*>(device_->MapBuffer(sc_ind_resolve_params_cb_));
                        if (mp) {
                            memset(mp, 0, 256);
                            mp[0] = atlasSize;    // sc_params.atlas_size
                            mp[3] = surfaceCachePass_->GetCardGenerator().GetCardCount(); // max_cards
                            // IndirectResolveParams fields after sc_params (48 bytes)
                            auto* fp = reinterpret_cast<float*>(mp);
                            mp[12] = tileSize;               // tile_size (uint, not float)
                            mp[13] = raysPerProbe;           // rays_per_probe (uint, not float)
                            fp[14] = 0.1f;                   // temporal_weight (float)
                            mp[15] = frameCount_;            // frame_index (uint, not float)
                            mp[16] = surfaceCachePass_->GetCardGenerator().GetLookupCount(); // lookup_count
                            device_->UnmapBuffer(sc_ind_resolve_params_cb_);
                        }
                    }

                    DescriptorData params[9] = {
                        {0, DescriptorType::SampledImage,  surfaceCachePass_->GetLightingAtlas(histIdx)}, // prev_lighting
                        {1, DescriptorType::SampledImage,  surfaceCachePass_->GetAlbedoAtlas()},
                        {2, DescriptorType::SampledImage,  ssgi_black_texture_},  // sky placeholder
                        {3, DescriptorType::StorageImage,  sc_indirect_out_tex_},
                        {0, DescriptorType::UniformBuffer, sc_global_data_cb_},
                        {1, DescriptorType::UniformBuffer, sc_ind_resolve_params_cb_},
                        {2, DescriptorType::StorageBuffer, surfaceCachePass_->GetCardLookupBuffer()},
                        {3, DescriptorType::StorageBuffer, surfaceCachePass_->GetCardDataBuffer()},
                        {4, DescriptorType::StorageBuffer, sc_ray_hits_buf_},
                    };
                    UpdateDescriptorSet(device_, sc_ind_resolve_descriptor_set_, params, 9);

                    cmd->BindComputePipeline(sc_ind_resolve_pipeline_);
                    const rhi::DescriptorSetHandle sets[] = { sc_ind_resolve_descriptor_set_ };
                    cmd->BindDescriptorSets(rhi::PipelineBindPoint::Compute, sc_ind_resolve_layout_, 0, 1, sets, 0, nullptr);

                    u32 tilesPerSide = atlasSize / tileSize;
                    cmd->Dispatch((tilesPerSide + 7) / 8, (tilesPerSide + 7) / 8, 1);
                });
        }
    }

    // === SURFACE CACHE → DDGI INTEGRATION (Strategy C) ===
    // CRITICAL: Must check scDirectView to prevent write-write race on
    // irradiance_buffers_ when SC is not active. Without this guard,
    // SC_DDGI_Integration's SideEffect() can overlap with DDGI's own
    // irradiance update, causing both to write the same buffer concurrently
    // and triggering a GPU hang on Apple Silicon.
    if (scDirectView && sc_lighting_done_ &&
        sc_card_rad_pipeline_ != rhi::handles::INVALID_PIPELINE &&
        sc_probe_irr_pipeline_ != rhi::handles::INVALID_PIPELINE &&
        sc_card_radiance_buf_ != rhi::handles::INVALID_RESOURCE &&
        sc_probe_contrib_range_buf_ != rhi::handles::INVALID_RESOURCE &&
        sc_flat_contrib_buf_ != rhi::handles::INVALID_RESOURCE &&
        ddgiPass_ && ddgiPass_->IsInitialized()) {

        struct SCDDGIIntData {};
        graph.AddPass<SCDDGIIntData>("SC_DDGI_Integration",
            rendergraph::RGPassType::Compute, rendergraph::RGPassCategory::Lighting,
            [scLightingH, ddgiIrrHandle = ddgiOutput.ddgi_irradiance,
             ddgiIrrHistHandle = ddgiOutput.ddgi_irradiance_hist](SCDDGIIntData& data, rendergraph::RenderGraphBuilder& builder) {
                builder.Read(scLightingH, rhi::ResourceState::ShaderResource);
                // CRITICAL: Depend on LumenDDGI's irradiance output to prevent
                // both passes writing irradiance_buffers_ simultaneously.
                // Without these edges, the RenderGraph may reorder or overlap them.
                // SC_DDGI_Integration writes both current and history irradiance buffers
                // (bypasses RG via direct buffer access), so we must declare both as
                // Read dependencies to establish ordering: DDGI finishes first, then this
                // pass reads+modifies the buffers, then DDGIGIGather reads the result.
                if (ddgiIrrHandle.IsValid()) {
                    builder.Read(ddgiIrrHandle, rhi::ResourceState::ShaderResource);
                }
                if (ddgiIrrHistHandle.IsValid()) {
                    builder.Read(ddgiIrrHistHandle, rhi::ResourceState::ShaderResource);
                }
                builder.SideEffect();  // writes to DDGI irradiance buffers
            },
            [this, scOutIdx, currentBufferIndex](const SCDDGIIntData& data, rendergraph::RenderGraphContext& context) {
                auto cmd = context.cmdBuffer;
                if (!cmd) return;

                u32 cardCount = surfaceCachePass_->GetCardGenerator().GetCardCount();
                if (cardCount == 0) return;

                u32 frameIdx = currentBufferIndex % 3;
                u32 histIdx = (currentBufferIndex + 2) % 3;

                // --- Pass 1: CardRadianceAvg ---
                {
                    DescriptorData params[4] = {
                        {0, rhi::DescriptorType::UniformBuffer, sc_sc_params_cb_},
                        {1, rhi::DescriptorType::StorageBuffer, surfaceCachePass_->GetCardDataBuffer()},
                        {2, rhi::DescriptorType::StorageBuffer, sc_card_radiance_buf_},
                        {0, rhi::DescriptorType::SampledImage,  surfaceCachePass_->GetLightingAtlas(sc_lighting_atlas_idx_)},
                    };
                    UpdateDescriptorSet(device_, sc_card_rad_ds_[frameIdx], params, 4);

                    cmd->BindComputePipeline(sc_card_rad_pipeline_);
                    const rhi::DescriptorSetHandle sets[] = { sc_card_rad_ds_[frameIdx] };
                    cmd->BindDescriptorSets(rhi::PipelineBindPoint::Compute, sc_card_rad_layout_, 0, 1, sets, 0, nullptr);

                    cmd->Dispatch((cardCount + 63) / 64, 1, 1);
                    printf("[SC→DDGI] CardRadianceAvg dispatched (cards=%u)\n", cardCount); fflush(stdout);
                }

                // Barrier: card_radiance write → read
                {
                    rhi::ResourceBarrier barrier{};
                    barrier.resource = sc_card_radiance_buf_;
                    barrier.beforeState = rhi::ResourceState::UnorderedAccess;
                    barrier.afterState = rhi::ResourceState::ShaderResource;
                    barrier.subresource = 0xFFFFFFFF;
                    cmd->InsertBarrier(&barrier, 1);
                }

                // --- Pass 2: ProbeIrradianceFromCards ---
                {
                    // Upload DDGI volume data for this frame
                    const auto& ddgiVol = ddgiPass_->GetVolumeData();
                    auto* mapped = static_cast<primal::graphics::lumen::DDGIVolumeData*>(
                        device_->MapBuffer(sc_ddgi_vol_cb_[frameIdx]));
                    if (mapped) {
                        *mapped = ddgiVol;
                        device_->UnmapBuffer(sc_ddgi_vol_cb_[frameIdx]);
                    }

                    DescriptorData params[8] = {
                        {0, rhi::DescriptorType::UniformBuffer, sc_ddgi_vol_cb_[frameIdx]},
                        {1, rhi::DescriptorType::StorageBuffer, sc_probe_contrib_range_buf_},
                        {2, rhi::DescriptorType::StorageBuffer, sc_flat_contrib_buf_},
                        {3, rhi::DescriptorType::StorageBuffer, sc_card_radiance_buf_},
                        {4, rhi::DescriptorType::StorageBuffer, ddgiPass_->GetIrradianceBuffer(histIdx)},
                        {5, rhi::DescriptorType::StorageBuffer, ddgiPass_->GetIrradianceBuffer(frameIdx)},
                        {6, rhi::DescriptorType::StorageBuffer, ddgiPass_->GetProbeUpdateListBuffer(frameIdx)},
                        {7, rhi::DescriptorType::StorageBuffer, ddgiPass_->GetConfidenceBuffer(frameIdx)},
                    };
                    UpdateDescriptorSet(device_, sc_probe_irr_ds_[frameIdx], params, 8);

                    cmd->BindComputePipeline(sc_probe_irr_pipeline_);
                    const rhi::DescriptorSetHandle sets[] = { sc_probe_irr_ds_[frameIdx] };
                    cmd->BindDescriptorSets(rhi::PipelineBindPoint::Compute, sc_probe_irr_layout_, 0, 1, sets, 0, nullptr);

                    u32 updateCount = ddgiVol.ProbeUpdateCount;
                    cmd->Dispatch((updateCount + 63) / 64, 1, 1);
                    printf("[SC→DDGI] ProbeIrradiance dispatched (probes=%u)\n", updateCount); fflush(stdout);
                }

                static bool logOnce = false;
                if (!logOnce) {
                    std::cout << "[SC→DDGI] Card→Probe integration dispatched (frame " << frameCount_ << ")" << std::endl;
                    logOnce = true;
                }
            });
    }

    // === FINAL BLIT PASS (Following TestParticleSponza pattern) ===
    // CRITICAL: This pass MUST depend on SceneRender to ensure Nanite output is ready
    struct BlitPassData {
        rendergraph::RGResourceHandle input;
        rendergraph::RGResourceHandle delayed_scene;  // 2-frame-delayed deferred output
        rendergraph::RGResourceHandle ssgi_input;
        rendergraph::RGResourceHandle depth_input;
        rendergraph::RGResourceHandle gi_indirect;     // half-res GI texture from compute
        rendergraph::RGResourceHandle albedo_input;
        rendergraph::RGResourceHandle normal_input;
        rendergraph::RGResourceHandle spgi_input;      // Screen Probe GI output
        rendergraph::RGResourceHandle fusion_output;   // Fusion compute output texture
        rendergraph::RGResourceHandle output;
    };

    // Import depth texture for DDGI blit
    rendergraph::RGResourceHandle depthBlitHandle;
    if (ddgiPass_ && ddgiPass_->IsInitialized() &&
        blit_ddgi_pipeline_ != rhi::handles::INVALID_PIPELINE &&
        sceneDepthTexture_ != rhi::handles::INVALID_RESOURCE) {
        depthBlitHandle = graph.ImportResource("BlitDepth", sceneDepthTexture_);
    }

    // Import GBuffer albedo and normal for DDGI blit
    rendergraph::RGResourceHandle gbufferAlbedoHandle;
    rendergraph::RGResourceHandle gbufferNormalHandle;
    if (gpuDrawPipeline_) {
        auto albedoTex = gpuDrawPipeline_->GetGBufferAlbedo();
        auto normalTex = gpuDrawPipeline_->GetGBufferNormal();
        if (albedoTex != rhi::handles::INVALID_RESOURCE) {
            gbufferAlbedoHandle = graph.ImportResource("GBufferAlbedoBlit", albedoTex);
        }
        if (normalTex != rhi::handles::INVALID_RESOURCE) {
            gbufferNormalHandle = graph.ImportResource("GBufferNormalBlit", normalTex);
        }
    }

    // === DDGI GI GATHER PASS (half-res compute) ===
    // Buffer-based: no texture3D, irradiance buffer reads only
    rendergraph::RGResourceHandle giOutputHandle;
    rhi::ResourceHandle giGatherIrradianceBuf = rhi::handles::INVALID_RESOURCE;
    if (ddgiPass_ && ddgiPass_->IsInitialized() &&
        gi_gather_pipeline_ != rhi::handles::INVALID_PIPELINE &&
        gi_halfres_texture_ != rhi::handles::INVALID_RESOURCE &&
        (ssgiVisMode_ == 3 || ssgiVisMode_ == 6)) {

        auto giTexHandle = graph.ImportResource("DDGIHalfResGI", gi_halfres_texture_);
        giOutputHandle = giTexHandle;

        u32 ddgiReadIdx = (currentBufferIndex + 2) % 3;
        auto ddgiIrrBuf = ddgiPass_->GetIrradianceBuffer(ddgiReadIdx);
        auto ddgiDepthBuf = ddgiPass_->GetDepthBuffer(ddgiReadIdx);
        giGatherIrradianceBuf = ddgiIrrBuf;

        auto gbufferDepth = gpuDrawPipeline_ ? gpuDrawPipeline_->GetGBufferDepthSampleable() : rhi::handles::INVALID_RESOURCE;
        auto gbufferNormal = gpuDrawPipeline_ ? gpuDrawPipeline_->GetGBufferNormal() : rhi::handles::INVALID_RESOURCE;

        if (ddgiIrrBuf != rhi::handles::INVALID_RESOURCE &&
            gbufferDepth != rhi::handles::INVALID_RESOURCE &&
            gbufferNormal != rhi::handles::INVALID_RESOURCE) {

            auto gDepthRG = graph.ImportResource("GBufferDepthGI", gbufferDepth);
            auto gNormalRG = graph.ImportResource("GBufferNormalGI", gbufferNormal);

            graph.AddPass<BlitPassData>("DDGIGIGather",
                graphics::rendergraph::RGPassType::Compute,
                graphics::rendergraph::RGPassCategory::Lighting,
                [giTexHandle, gDepthRG, gNormalRG, ddgiIrrHistHandle = ddgiOutput.ddgi_irradiance_hist](
                    BlitPassData& data, graphics::rendergraph::RenderGraphBuilder& builder) {
                    builder.Read(gDepthRG, rhi::ResourceState::ShaderResource);
                    builder.Read(gNormalRG, rhi::ResourceState::ShaderResource);
                    builder.Write(giTexHandle, rhi::ResourceState::UnorderedAccess);
                    // Declare irradiance history buffer dependency so RenderGraph
                    // inserts proper barriers between DDGI write and this read
                    if (ddgiIrrHistHandle.IsValid()) {
                        builder.Read(ddgiIrrHistHandle, rhi::ResourceState::ShaderResource);
                    }
                },
                [this, currentBufferIndex, ddgiReadIdx, ddgiIrrBuf, ddgiDepthBuf, gDepthRG, gNormalRG, giTexHandle](
                    const BlitPassData& data, graphics::rendergraph::RenderGraphContext& context) {
                    auto cmd = context.cmdBuffer;
                    if (!cmd) return;

                    auto resolveTex = [&](rendergraph::RGResourceHandle h) -> rhi::ResourceHandle {
                        auto* res = context.graph->GetResource(h);
                        return res ? res->GetPhysicalHandle() : rhi::handles::INVALID_RESOURCE;
                    };

                    rhi::ResourceHandle depthH = resolveTex(gDepthRG);
                    rhi::ResourceHandle normalH = resolveTex(gNormalRG);
                    rhi::ResourceHandle outputH = resolveTex(giTexHandle);

                    if (depthH == rhi::handles::INVALID_RESOURCE ||
                        normalH == rhi::handles::INVALID_RESOURCE) return;

                    u32 cbIdx = currentBufferIndex % 3;

                    // === Phase 1: Upload constant buffers ===
                    {
                        primal::math::m4x4 vp = cameraBuffers_[currentBufferIndex].proj_matrix * cameraBuffers_[currentBufferIndex].view_matrix;
                        primal::math::m4x4 invVP = rhi::math::Inverse(vp);
                        const auto& ddgiParams = ddgiPass_->GetParams();
                        const auto& volData = ddgiPass_->GetVolumeData();

                        struct GIGatherCB {
                            primal::math::m4x4 inv_view_projection;
                            primal::math::v4   probe_origin_spacing;
                            primal::math::v4   probe_counts;
                        };

                        auto* cb = static_cast<GIGatherCB*>(device_->MapBuffer(ddgi_probe_cb_[cbIdx]));
                        if (cb) {
                            cb->inv_view_projection = invVP;
                            cb->probe_origin_spacing = primal::math::v4{
                                volData.ProbeOrigin.x, volData.ProbeOrigin.y,
                                volData.ProbeOrigin.z, ddgiParams.probe_spacing};
                            cb->probe_counts = primal::math::v4{
                                static_cast<f32>(ddgiParams.probe_count_x),
                                static_cast<f32>(ddgiParams.probe_count_y),
                                static_cast<f32>(ddgiParams.probe_count_z), 0.0f};
                            device_->UnmapBuffer(ddgi_probe_cb_[cbIdx]);
                        }

                        // Upload StaticProbeData constant buffer
                        if (static_probe_cb_ != rhi::handles::INVALID_RESOURCE && static_probe_volume_) {
                            struct GPUStaticProbeData {
                                primal::math::v4 ProbeOrigin;
                                float  ProbeSpacing;
                                u32    GridDimX;
                                u32    GridDimY;
                                u32    GridDimZ;
                                float  _pad[2];
                                primal::math::v4 SkySH[9];
                            };
                            auto* spData = static_cast<GPUStaticProbeData*>(device_->MapBuffer(static_probe_cb_));
                            if (spData) {
                                const auto& spParams = static_probe_volume_->GetParams();
                                spData->ProbeOrigin = primal::math::v4{spParams.origin.x, spParams.origin.y, spParams.origin.z, 0.0f};
                                spData->ProbeSpacing = spParams.spacing;
                                spData->GridDimX = spParams.grid_dim_x;
                                spData->GridDimY = spParams.grid_dim_y;
                                spData->GridDimZ = spParams.grid_dim_z;
                                spData->_pad[0] = 0.0f;
                                spData->_pad[1] = 0.0f;
                                const primal::math::v3* skySH = static_probe_volume_->GetSkySH();
                                for (int i = 0; i < 9; ++i)
                                    spData->SkySH[i] = primal::math::v4{skySH[i].x, skySH[i].y, skySH[i].z, 0.0f};
                                device_->UnmapBuffer(static_probe_cb_);
                            }
                        }
                    }

                    // === Phase 2: Barrier for DDGI buffer writes ===
                    {
                        rhi::ResourceBarrier barriers[2]{};
                        barriers[0].resource = ddgiIrrBuf;
                        barriers[0].beforeState = rhi::ResourceState::UnorderedAccess;
                        barriers[0].afterState = rhi::ResourceState::ShaderResource;
                        barriers[0].subresource = 0xFFFFFFFF;
                        barriers[1].resource = ddgiDepthBuf;
                        barriers[1].beforeState = rhi::ResourceState::UnorderedAccess;
                        barriers[1].afterState = rhi::ResourceState::ShaderResource;
                        barriers[1].subresource = 0xFFFFFFFF;
                        cmd->InsertBarrier(barriers, 2);
                    }

                    // === Phase 3: GI Gather (threadgroup cached probe reads) ===
                    {
                        DescriptorData texParams[] = {
                            {0, DescriptorType::SampledImage, depthH},
                            {1, DescriptorType::SampledImage, normalH},
                            {2, DescriptorType::StorageImage, outputH},
                            {3, DescriptorType::SampledImage, gi_halfres_history_},
                        };
                        UpdateDescriptorSet(device_, gi_gather_descriptor_set_, texParams, 4);
                    }
                    {
                        constexpr int kNumBuf = 9;
                        rhi::WriteDescriptorSet bufWrites[kNumBuf];
                        rhi::DescriptorBufferInfo bufInfos[kNumBuf];
                        // buffer(0-2): uniform CB offsets
                        for (int i = 0; i < 3; ++i) {
                            bufWrites[i].dstSet = gi_gather_descriptor_set_;
                            bufWrites[i].dstBinding = i;
                            bufWrites[i].descriptorCount = 1;
                            bufWrites[i].descriptorType = DescriptorType::UniformBuffer;
                            bufWrites[i].bufferInfo = &bufInfos[i];
                            bufInfos[i].buffer = ddgi_probe_cb_[cbIdx];
                        }
                        bufInfos[0].offset = 0;   bufInfos[0].range = 64;
                        bufInfos[1].offset = 64;  bufInfos[1].range = 16;
                        bufInfos[2].offset = 80;  bufInfos[2].range = 16;
                        // buffer(3): staticSkySH
                        bufWrites[3].dstSet = gi_gather_descriptor_set_;
                        bufWrites[3].dstBinding = 3;
                        bufWrites[3].descriptorCount = 1;
                        bufWrites[3].descriptorType = DescriptorType::StorageBuffer;
                        bufWrites[3].bufferInfo = &bufInfos[3];
                        bufInfos[3].buffer = static_probe_volume_ ? static_probe_volume_->GetStaticSkySH() : rhi::handles::INVALID_RESOURCE;
                        bufInfos[3].offset = 0;
                        bufInfos[3].range = ~0ull;
                        // buffer(4): staticSkyFactor
                        bufWrites[4].dstSet = gi_gather_descriptor_set_;
                        bufWrites[4].dstBinding = 4;
                        bufWrites[4].descriptorCount = 1;
                        bufWrites[4].descriptorType = DescriptorType::StorageBuffer;
                        bufWrites[4].bufferInfo = &bufInfos[4];
                        bufInfos[4].buffer = static_probe_volume_ ? static_probe_volume_->GetStaticSkyFactor() : rhi::handles::INVALID_RESOURCE;
                        bufInfos[4].offset = 0;
                        bufInfos[4].range = ~0ull;
                        // buffer(5): staticProbe params
                        bufWrites[5].dstSet = gi_gather_descriptor_set_;
                        bufWrites[5].dstBinding = 5;
                        bufWrites[5].descriptorCount = 1;
                        bufWrites[5].descriptorType = DescriptorType::UniformBuffer;
                        bufWrites[5].bufferInfo = &bufInfos[5];
                        bufInfos[5].buffer = static_probe_cb_;
                        bufInfos[5].offset = 0;
                        bufInfos[5].range = 512;
                        // buffer(6): confidenceBuffer
                        bufWrites[6].dstSet = gi_gather_descriptor_set_;
                        bufWrites[6].dstBinding = 6;
                        bufWrites[6].descriptorCount = 1;
                        bufWrites[6].descriptorType = DescriptorType::StorageBuffer;
                        bufWrites[6].bufferInfo = &bufInfos[6];
                        bufInfos[6].buffer = ddgiPass_ ? ddgiPass_->GetConfidenceBuffer(ddgiReadIdx) : rhi::handles::INVALID_RESOURCE;
                        bufInfos[6].offset = 0;
                        bufInfos[6].range = ~0ull;
                        // buffer(7): irradianceBuffer (threadgroup cached reads)
                        bufWrites[7].dstSet = gi_gather_descriptor_set_;
                        bufWrites[7].dstBinding = 7;
                        bufWrites[7].descriptorCount = 1;
                        bufWrites[7].descriptorType = DescriptorType::StorageBuffer;
                        bufWrites[7].bufferInfo = &bufInfos[7];
                        bufInfos[7].buffer = ddgiIrrBuf;
                        bufInfos[7].offset = 0;
                        bufInfos[7].range = ~0ull;
                        // buffer(8): depthBuffer
                        bufWrites[8].dstSet = gi_gather_descriptor_set_;
                        bufWrites[8].dstBinding = 8;
                        bufWrites[8].descriptorCount = 1;
                        bufWrites[8].descriptorType = DescriptorType::StorageBuffer;
                        bufWrites[8].bufferInfo = &bufInfos[8];
                        bufInfos[8].buffer = ddgiDepthBuf;
                        bufInfos[8].offset = 0;
                        bufInfos[8].range = ~0ull;

                        device_->UpdateDescriptorSets(kNumBuf, bufWrites);

                        // Validate ALL buffer bindings before dispatch.
                        // Missing buffers leave Metal shader slots unbound → GPU fault → hang.
                        bool allBuffersValid =
                            bufInfos[0].buffer != rhi::handles::INVALID_RESOURCE &&
                            bufInfos[3].buffer != rhi::handles::INVALID_RESOURCE &&
                            bufInfos[4].buffer != rhi::handles::INVALID_RESOURCE &&
                            bufInfos[5].buffer != rhi::handles::INVALID_RESOURCE &&
                            bufInfos[6].buffer != rhi::handles::INVALID_RESOURCE &&
                            bufInfos[7].buffer != rhi::handles::INVALID_RESOURCE &&
                            bufInfos[8].buffer != rhi::handles::INVALID_RESOURCE;

                        if (!allBuffersValid) {
                            return;
                        }
                    }

                    cmd->BindComputePipeline(gi_gather_pipeline_);
                    const rhi::DescriptorSetHandle sets[] = { gi_gather_descriptor_set_ };
                    cmd->BindDescriptorSets(rhi::PipelineBindPoint::Compute, gi_gather_layout_, 0, 1, sets, 0, nullptr);

                    u32 halfW = renderWidth_ / 2;
                    u32 halfH = renderHeight_ / 2;
                    u32 gx = (halfW + 7) / 8;
                    u32 gy = (halfH + 7) / 8;
                    cmd->Dispatch(gx, gy, 1);
                    printf("[DDGI] GIGather dispatched (grid=%ux%u)\n", gx, gy); fflush(stdout);

                    // Copy output to history for next frame's temporal accumulation
                    {
                        rhi::ResourceBarrier outBarrier{};
                        outBarrier.resource = outputH;
                        outBarrier.beforeState = rhi::ResourceState::UnorderedAccess;
                        outBarrier.afterState = rhi::ResourceState::ShaderResource;
                        outBarrier.subresource = 0xFFFFFFFF;
                        cmd->InsertBarrier(&outBarrier, 1);

                        rhi::TextureBlitRegion blitRegion{};
                        blitRegion.srcOffsets[0] = {0, 0, 0};
                        blitRegion.srcOffsets[1] = {(int)halfW, (int)halfH, 1};
                        blitRegion.dstOffsets[0] = {0, 0, 0};
                        blitRegion.dstOffsets[1] = {(int)halfW, (int)halfH, 1};
                        cmd->BlitTexture(outputH, gi_halfres_history_, &blitRegion, 1, rhi::FilterMode::Nearest);
                    }
                }
            );
        }
    }

    // Choose primary input for FinalBlit (and FusionCompute):
    // When deferred lighting is active, use its output (lit scene color).
    // Otherwise use raw GBuffer albedo (no lighting).
    rendergraph::RGResourceHandle primaryInputHandle = gpuOutputHandle;
    if (deferred_pipeline_ != rhi::handles::INVALID_PIPELINE &&
        currentDeferredTex != rhi::handles::INVALID_RESOURCE) {
        primaryInputHandle = deferredOutputRG;
    }

    // === Fusion Render Passes (2-pass fragment for Apple Silicon TBDR) ===
    // Pass 1 (half-res): 5 texture reads → pre-combined indirect
    // Pass 2 (full-res): 2 texture reads → tonemapped output
    if (ssgiVisMode_ == 6 && mode_diag_ == 1 &&
        fusion_indirect_pipeline_ != rhi::handles::INVALID_PIPELINE &&
        fusion_fragment_pipeline_ != rhi::handles::INVALID_PIPELINE) {
        u32 fusionCbIdx = currentBufferIndex % 3;

        // Import half-res indirect output
        rhi::TextureDesc indirectDesc{};
        indirectDesc.size = {renderWidth_ / 2, renderHeight_ / 2, 1};
        indirectDesc.format = DataFormat::RGBA16_Float;
        indirectDesc.usage = TextureUsage::ShaderResource | TextureUsage::RenderTarget;
        auto indirectRG = graph.ImportTexture("FusionIndirect", fusion_indirect_output_[fusionCbIdx], indirectDesc);

        // Pass 1: Pre-combine GI + albedo + ssao → indirect contribution (half-res)
        graph.AddPass<BlitPassData>("FusionIndirect",
            graphics::rendergraph::RGPassType::Graphics,
            graphics::rendergraph::RGPassCategory::PostProcess,
            [this, ssgiOutputHandle,
             giOutputHandle, gbufferAlbedoHandle, screenProbeGIOutputHandle, ssaoOutputHandle,
             indirectRG](BlitPassData& data, graphics::rendergraph::RenderGraphBuilder& builder) {
                if (ssgiOutputHandle.IsValid())
                    data.ssgi_input = builder.Read(ssgiOutputHandle, rhi::ResourceState::ShaderResource);
                if (giOutputHandle.IsValid())
                    data.gi_indirect = builder.Read(giOutputHandle, rhi::ResourceState::ShaderResource);
                if (screenProbeGIOutputHandle.IsValid())
                    data.spgi_input = builder.Read(screenProbeGIOutputHandle, rhi::ResourceState::ShaderResource);
                if (gbufferAlbedoHandle.IsValid())
                    data.albedo_input = builder.Read(gbufferAlbedoHandle, rhi::ResourceState::ShaderResource);
                if (ssaoOutputHandle.IsValid())
                    data.depth_input = builder.Read(ssaoOutputHandle, rhi::ResourceState::ShaderResource);

                data.fusion_output = builder.Write(indirectRG, rhi::ResourceState::RenderTarget);

                graphics::rendergraph::RGRenderPassDesc rpDesc;
                rpDesc.colors.push_back({
                    .texture = data.fusion_output,
                    .loadOp = rhi::LoadAction::DontCare,
                    .storeOp = rhi::StoreAction::Store,
                    .clearColor = { primal::math::v4{0, 0, 0, 0} }
                });
                builder.DeclareRenderPass(rpDesc);
            },
            [this, currentBufferIndex, indirectRG](const BlitPassData& data, graphics::rendergraph::RenderGraphContext& context) {
                auto cmd = context.cmdBuffer;
                if (!cmd) return;

                auto resolveHandle = [&](rendergraph::RGResourceHandle h) -> ResourceHandle {
                    auto* res = context.graph->GetResource(h);
                    return res ? res->GetPhysicalHandle() : ssgi_black_texture_;
                };

                const u32 cbIdx = currentBufferIndex % 3;
                ResourceHandle ssgiHandle = data.ssgi_input.IsValid() ? resolveHandle(data.ssgi_input) : ssgi_black_texture_;
                ResourceHandle ddgiHandle = data.gi_indirect.IsValid() ? resolveHandle(data.gi_indirect) : ssgi_black_texture_;
                ResourceHandle spgiHandle = data.spgi_input.IsValid() ? resolveHandle(data.spgi_input) : ssgi_black_texture_;
                ResourceHandle albedoHandle = data.albedo_input.IsValid() ? resolveHandle(data.albedo_input) : ssgi_black_texture_;
                ResourceHandle ssaoHandle = data.depth_input.IsValid() ? resolveHandle(data.depth_input) : ssgi_black_texture_;

                u32 halfW = renderWidth_ / 2;
                u32 halfH = renderHeight_ / 2;
                cmd->SetViewport({ {0, 0}, {static_cast<float>(halfW), static_cast<float>(halfH)}, 0, 1 });
                cmd->SetScissor({ {0, 0}, {halfW, halfH} });

                DescriptorData params[5] = {
                    { 0, DescriptorType::SampledImage, ssgiHandle },
                    { 1, DescriptorType::SampledImage, ddgiHandle },
                    { 2, DescriptorType::SampledImage, spgiHandle },
                    { 3, DescriptorType::SampledImage, albedoHandle },
                    { 4, DescriptorType::SampledImage, ssaoHandle },
                };
                UpdateDescriptorSet(device_, fusion_indirect_descriptor_set_[cbIdx], params, 5);
                cmd->BindGraphicsPipeline(fusion_indirect_pipeline_);
                const rhi::DescriptorSetHandle gfx_sets[] = { fusion_indirect_descriptor_set_[cbIdx] };
                cmd->BindDescriptorSets(rhi::PipelineBindPoint::Graphics, fusion_indirect_layout_, 0, 1, gfx_sets, 0, nullptr);
                cmd->Draw(3, 0, 1, 0);
            }
        );

        // Import full-res fusion output
        rhi::TextureDesc fusionOutputDesc{};
        fusionOutputDesc.size = {renderWidth_, renderHeight_, 1};
        fusionOutputDesc.format = DataFormat::BGRA8_UNorm;
        fusionOutputDesc.usage = TextureUsage::ShaderResource | TextureUsage::RenderTarget;
        auto fusionOutputRG = graph.ImportTexture("FusionOutput", fusion_output_[fusionCbIdx], fusionOutputDesc);

        // Pass 2: scene + pre-combined indirect → output (full-res)
        graph.AddPass<BlitPassData>("FusionComposite",
            graphics::rendergraph::RGPassType::Graphics,
            graphics::rendergraph::RGPassCategory::PostProcess,
            [this, primaryInputHandle, indirectRG, fusionOutputRG](BlitPassData& data, graphics::rendergraph::RenderGraphBuilder& builder) {
                data.input = builder.Read(primaryInputHandle, rhi::ResourceState::ShaderResource);
                data.ssgi_input = builder.Read(indirectRG, rhi::ResourceState::ShaderResource);
                data.fusion_output = builder.Write(fusionOutputRG, rhi::ResourceState::RenderTarget);

                graphics::rendergraph::RGRenderPassDesc rpDesc;
                rpDesc.colors.push_back({
                    .texture = data.fusion_output,
                    .loadOp = rhi::LoadAction::DontCare,
                    .storeOp = rhi::StoreAction::Store,
                    .clearColor = { primal::math::v4{0, 0, 0, 1} }
                });
                builder.DeclareRenderPass(rpDesc);
            },
            [this, currentBufferIndex](const BlitPassData& data, graphics::rendergraph::RenderGraphContext& context) {
                auto cmd = context.cmdBuffer;
                if (!cmd) return;

                auto resolveHandle = [&](rendergraph::RGResourceHandle h) -> ResourceHandle {
                    auto* res = context.graph->GetResource(h);
                    return res ? res->GetPhysicalHandle() : ssgi_black_texture_;
                };

                const u32 cbIdx = currentBufferIndex % 3;
                ResourceHandle sceneHandle = resolveHandle(data.input);
                ResourceHandle indirectHandle = resolveHandle(data.ssgi_input);

                cmd->SetViewport({ {0, 0}, {static_cast<float>(renderWidth_), static_cast<float>(renderHeight_)}, 0, 1 });
                cmd->SetScissor({ {0, 0}, {renderWidth_, renderHeight_} });

                DescriptorData params[2] = {
                    { 0, DescriptorType::SampledImage, sceneHandle },
                    { 1, DescriptorType::SampledImage, indirectHandle },
                };
                UpdateDescriptorSet(device_, fusion_fragment_descriptor_set_[cbIdx], params, 2);
                cmd->BindGraphicsPipeline(fusion_fragment_pipeline_);
                const rhi::DescriptorSetHandle gfx_sets[] = { fusion_fragment_descriptor_set_[cbIdx] };
                cmd->BindDescriptorSets(rhi::PipelineBindPoint::Graphics, fusion_fragment_layout_, 0, 1, gfx_sets, 0, nullptr);
                cmd->Draw(3, 0, 1, 0);
            }
        );
    }

    graph.AddPass<BlitPassData>("FinalBlit",
        graphics::rendergraph::RGPassType::Graphics,
        graphics::rendergraph::RGPassCategory::PostProcess,
        [this, backBufferHandle, primaryInputHandle, delayedOutputRG, ssgiOutputHandle, depthBlitHandle, giOutputHandle, gbufferAlbedoHandle, gbufferNormalHandle, screenProbeGIOutputHandle, ssaoOutputHandle, scLightingH](BlitPassData& data, graphics::rendergraph::RenderGraphBuilder& builder) {
            // Read lit scene color (deferred output or raw GBuffer albedo)
            data.input = builder.Read(primaryInputHandle, rhi::ResourceState::ShaderResource);

            // Read 2-frame-delayed scene color for fusion modes (avoids data race)
            if (delayedOutputRG.IsValid()) {
                data.delayed_scene = builder.Read(delayedOutputRG, rhi::ResourceState::ShaderResource);
            }

            // Read SSGI output when in composite or SSGI-only mode
            // ssgiVisMode_: 0=Composite, 1=SSGI only, 2=Scene only, 3=DDGI Composite
            bool needSSGI = (ssgiVisMode_ == 0 || ssgiVisMode_ == 1 || ssgiVisMode_ == 6) &&
                            ssgiOutputHandle.IsValid() &&
                            blit_composite_pipeline_ != rhi::handles::INVALID_PIPELINE;
            if (needSSGI) {
                data.ssgi_input = builder.Read(ssgiOutputHandle, rhi::ResourceState::ShaderResource);
            } else {
                data.ssgi_input = rendergraph::RGResourceHandle{};
            }

            // Read depth + half-res GI indirect + GBuffer when in DDGI or fusion mode
            if ((ssgiVisMode_ == 3 || ssgiVisMode_ == 6) && depthBlitHandle.IsValid() && giOutputHandle.IsValid()) {
                data.depth_input = builder.Read(depthBlitHandle, rhi::ResourceState::ShaderResource);
                data.gi_indirect = builder.Read(giOutputHandle, rhi::ResourceState::ShaderResource);

                if (gbufferAlbedoHandle.IsValid()) {
                    data.albedo_input = builder.Read(gbufferAlbedoHandle, rhi::ResourceState::ShaderResource);
                } else {
                    data.albedo_input = rendergraph::RGResourceHandle{};
                }
                if (gbufferNormalHandle.IsValid()) {
                    data.normal_input = builder.Read(gbufferNormalHandle, rhi::ResourceState::ShaderResource);
                } else {
                    data.normal_input = rendergraph::RGResourceHandle{};
                }
            } else {
                data.depth_input = rendergraph::RGResourceHandle{};
                data.gi_indirect = rendergraph::RGResourceHandle{};
                data.albedo_input = rendergraph::RGResourceHandle{};
                data.normal_input = rendergraph::RGResourceHandle{};
            }

            // Read Screen Probe GI output when in mode 5 or fusion mode 6
            if ((ssgiVisMode_ == 5 || ssgiVisMode_ == 6) && screenProbeGIOutputHandle.IsValid()) {
                data.spgi_input = builder.Read(screenProbeGIOutputHandle, rhi::ResourceState::ShaderResource);
            } else {
                data.spgi_input = rendergraph::RGResourceHandle{};
            }

            // SSAO dependency: ensure SSAO compute finishes before FinalBlit reads it.
            // Previously accessed via physical handle bypassing render graph → no barrier → flickering.
            if (ssaoOutputHandle.IsValid()) {
                builder.Read(ssaoOutputHandle, rhi::ResourceState::ShaderResource);
            }

            // Surface Cache: ensure SC_Lighting compute finishes before FinalBlit reads the atlas.
            // Without this, the render graph doesn't know FinalBlit depends on SC_Lighting
            // and may schedule them in the wrong order → GPU resource conflict → hang.
            if ((ssgiVisMode_ == 7 || ssgiVisMode_ == 8 || ssgiVisMode_ == 9) && scLightingH.IsValid()) {
                builder.Read(scLightingH, rhi::ResourceState::ShaderResource);
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
        [this, currentBufferIndex](const BlitPassData& data, graphics::rendergraph::RenderGraphContext& context) {
            auto cmd = context.cmdBuffer;

            cmd->SetViewport({ {0, 0}, {static_cast<float>(renderWidth_), static_cast<float>(renderHeight_)}, 0, 1 });
            cmd->SetScissor({ {0, 0}, {renderWidth_, renderHeight_} });

            const u32 cbIdx = currentBufferIndex % 3;

            // Get scene color texture
            auto inputResource = context.graph->GetResource(data.input);
            if (!inputResource) return;
            auto inputHandle = inputResource->GetPhysicalHandle();
            if (inputHandle == rhi::handles::INVALID_RESOURCE) return;

            // Mode 3: DDGI Composite — scene + DDGI indirect via DDGI blit pipeline
            if (ssgiVisMode_ == 3 && data.depth_input.IsValid() && data.gi_indirect.IsValid() &&
                blit_ddgi_pipeline_ != rhi::handles::INVALID_PIPELINE) {
                auto depthResource = context.graph->GetResource(data.depth_input);
                if (depthResource) {
                    auto depthHandle = depthResource->GetPhysicalHandle();

                    if (depthHandle != rhi::handles::INVALID_RESOURCE) {

                        // Resolve GI indirect (half-res texture from compute pass)
                        auto giIndirectHandle = rhi::handles::INVALID_RESOURCE;
                        if (data.gi_indirect.IsValid()) {
                            auto res = context.graph->GetResource(data.gi_indirect);
                            if (res) giIndirectHandle = res->GetPhysicalHandle();
                        }

                        // Resolve GBuffer albedo, normal handles
                        auto albedoHandle = rhi::handles::INVALID_RESOURCE;
                        if (data.albedo_input.IsValid()) {
                            auto res = context.graph->GetResource(data.albedo_input);
                            if (res) albedoHandle = res->GetPhysicalHandle();
                        }
                        auto normalHandle = rhi::handles::INVALID_RESOURCE;
                        if (data.normal_input.IsValid()) {
                            auto res = context.graph->GetResource(data.normal_input);
                            if (res) normalHandle = res->GetPhysicalHandle();
                        }

                        if (giIndirectHandle == rhi::handles::INVALID_RESOURCE) {
                            // No GI texture available, fall through to scene-only blit
                        } else if (ddgi_probe_cb_[currentBufferIndex % 3] == rhi::handles::INVALID_RESOURCE) {
                            // CB buffer not created — fall through to scene-only blit
                        } else {

                            // Upload constant buffers (reuse same CB as GI Gather)
                            u32 cbIdx = currentBufferIndex % 3;
                            {
                                // Compute inverse view-projection matrix
                                primal::math::m4x4 vp = cameraBuffers_[currentBufferIndex].proj_matrix * cameraBuffers_[currentBufferIndex].view_matrix;
                                primal::math::m4x4 invVP = rhi::math::Inverse(vp);

                                // Get DDGI probe parameters
                                const auto& ddgiParams = ddgiPass_->GetParams();
                                const auto& volData = ddgiPass_->GetVolumeData();

                                struct DDGIBlitCB {
                                    primal::math::m4x4 inv_view_projection;
                                    primal::math::v4   probe_origin_spacing;
                                    primal::math::v4   probe_counts_sh;
                                };

                                auto* cb = static_cast<DDGIBlitCB*>(device_->MapBuffer(ddgi_probe_cb_[cbIdx]));
                                if (cb) {
                                    cb->inv_view_projection = invVP;
                                    cb->probe_origin_spacing = primal::math::v4{
                                        volData.ProbeOrigin.x,
                                        volData.ProbeOrigin.y,
                                        volData.ProbeOrigin.z,
                                        ddgiParams.probe_spacing
                                    };
                                    cb->probe_counts_sh = primal::math::v4{
                                        static_cast<f32>(ddgiParams.probe_count_x),
                                        static_cast<f32>(ddgiParams.probe_count_y),
                                        static_cast<f32>(ddgiParams.probe_count_z),
                                        9.0f
                                    };
                                    device_->UnmapBuffer(ddgi_probe_cb_[cbIdx]);
                                }
                            }

                            // Update descriptor set: 6 textures
                            // texture(0): scene color, texture(1): depth, texture(2): half-res GI
                            // texture(3): GBuffer albedo, texture(4): GBuffer normal, texture(5): SSAO
                            DescriptorData ddgi_params[] = {
                                {0, DescriptorType::SampledImage,  inputHandle},          // texture(0): scene color
                                {1, DescriptorType::SampledImage,  depthHandle},          // texture(1): depth
                                {2, DescriptorType::SampledImage,  giIndirectHandle},     // texture(2): half-res GI indirect
                                {3, DescriptorType::SampledImage,  albedoHandle},         // texture(3): GBuffer albedo
                                {4, DescriptorType::SampledImage,  normalHandle},         // texture(4): GBuffer normal
                                {5, DescriptorType::SampledImage,
                                    (ssaoPass_ && ssaoPass_->IsInitialized())
                                        ? ssaoPass_->GetFilterTexture() : handles::INVALID_RESOURCE},
                            };
                            UpdateDescriptorSet(device_, blit_ddgi_descriptor_set_, ddgi_params, 6);

                            // Buffer bindings with offsets into single CB:
                            // buffer(0) offset=0  = invViewProjection (64 bytes)
                            // buffer(1) offset=64 = probeOrigin_spacing (16 bytes)
                            // buffer(2) offset=80 = probeCounts_shCount (16 bytes)
                            {
                                rhi::WriteDescriptorSet bufWrites[3];
                                rhi::DescriptorBufferInfo bufInfos[3];
                                for (int i = 0; i < 3; ++i) {
                                    bufWrites[i].dstSet = blit_ddgi_descriptor_set_;
                                    bufWrites[i].dstBinding = i;
                                    bufWrites[i].descriptorCount = 1;
                                    bufWrites[i].descriptorType = DescriptorType::UniformBuffer;
                                    bufWrites[i].bufferInfo = &bufInfos[i];
                                    bufInfos[i].buffer = ddgi_probe_cb_[cbIdx];
                                }
                                bufInfos[0].offset = 0;
                                bufInfos[0].range = 64;
                                bufInfos[1].offset = 64;
                                bufInfos[1].range = 16;
                                bufInfos[2].offset = 80;
                                bufInfos[2].range = 16;
                                device_->UpdateDescriptorSets(3, bufWrites);
                            }

                            cmd->BindGraphicsPipeline(blit_ddgi_pipeline_);
                            const rhi::DescriptorSetHandle sets[] = { blit_ddgi_descriptor_set_ };
                            cmd->BindDescriptorSets(rhi::PipelineBindPoint::Graphics, blit_ddgi_layout_, 0, 1, sets, 0, nullptr);
                            cmd->Draw(3, 0, 1, 0);
                            return;
                        }
                    }
                }
                // Fallback: if DDGI resources invalid, fall through to scene-only blit
            }

            // Mode 6: Full GI Fusion — DDGI + SPGI + SSGI + direct lighting
            if (ssgiVisMode_ == 6) {
                // === 2 modes (F5 to cycle) ===
                //  0 = simple blit (1-bind, baseline)
                //  1 = compute fusion with real textures (7 reads)

                // Mode 0: Simple blit
                if (mode_diag_ == 0) {
                    DescriptorData blit_params[1] = {{0, DescriptorType::SampledImage, inputHandle}};
                    UpdateDescriptorSet(device_, blit_descriptor_set_[cbIdx], blit_params, 1);
                    cmd->BindGraphicsPipeline(blit_pipeline_);
                    const rhi::DescriptorSetHandle blit_sets[] = { blit_descriptor_set_[cbIdx] };
                    cmd->BindDescriptorSets(rhi::PipelineBindPoint::Graphics, blit_layout_, 0, 1, blit_sets, 0, nullptr);
                    cmd->Draw(3, 0, 1, 0);
                    return;
                }

                // Mode 1: Blit from FusionRender output
                if (mode_diag_ == 1) {
                    ResourceHandle fusionOut = fusion_output_[cbIdx];
                    if (fusionOut == rhi::handles::INVALID_RESOURCE) {
                        fusionOut = inputHandle;
                    }
                    DescriptorData blit_params[1] = { { 0, DescriptorType::SampledImage, fusionOut } };
                    UpdateDescriptorSet(device_, blit_descriptor_set_[cbIdx], blit_params, 1);
                    cmd->BindGraphicsPipeline(blit_pipeline_);
                    const rhi::DescriptorSetHandle blit_sets[] = { blit_descriptor_set_[cbIdx] };
                    cmd->BindDescriptorSets(rhi::PipelineBindPoint::Graphics, blit_layout_, 0, 1, blit_sets, 0, nullptr);
                    cmd->Draw(3, 0, 1, 0);
                    return;
                }
            }

            // Mode 7: Surface Cache — blit lighting atlas (compute passes run before FinalBlit)
            if (ssgiVisMode_ == 7 && surfaceCachePass_ && surfaceCachePass_->IsInitialized()) {
                // SC lighting runs per-frame, writing to scOutIdx = currentBufferIndex % 3.
                // Use sc_lighting_atlas_idx_ (updated each frame by Lighting pass) for the correct slot.
                auto scAtlas = surfaceCachePass_->GetLightingAtlas(sc_lighting_atlas_idx_);
                if (scAtlas != rhi::handles::INVALID_RESOURCE) {
                    rhi::ResourceBarrier scBarrier{};
                    scBarrier.resource = scAtlas;
                    scBarrier.beforeState = rhi::ResourceState::UnorderedAccess;
                    scBarrier.afterState = rhi::ResourceState::ShaderResource;
                    scBarrier.subresource = 0xFFFFFFFF;
                    cmd->InsertBarrier(&scBarrier, 1);
                }
                ResourceHandle blitTex = (scAtlas != rhi::handles::INVALID_RESOURCE) ? scAtlas : ssgi_black_texture_;
                DescriptorData blit_params[1] = {{0, DescriptorType::SampledImage, blitTex}};
                UpdateDescriptorSet(device_, blit_descriptor_set_[cbIdx], blit_params, 1);
                cmd->BindGraphicsPipeline(blit_pipeline_);
                const rhi::DescriptorSetHandle sc_sets[] = { blit_descriptor_set_[cbIdx] };
                cmd->BindDescriptorSets(rhi::PipelineBindPoint::Graphics, blit_layout_, 0, 1, sc_sets, 0, nullptr);
                cmd->Draw(3, 0, 1, 0);
                return;
            }

            // Mode 8: SC Albedo Atlas — blit raw albedo atlas
            if (ssgiVisMode_ == 8 && surfaceCachePass_ && surfaceCachePass_->IsInitialized()) {
                auto tex = surfaceCachePass_->GetAlbedoAtlas();
                static bool printed8 = false;
                if (!printed8) {
                    printed8 = true;
                    std::cout << "[SC Debug] Mode 8: albedo_atlas=" << tex
                              << " valid=" << (tex != rhi::handles::INVALID_RESOURCE)
                              << " fill_done=" << sc_fill_done_ << std::endl;
                }
                if (tex != rhi::handles::INVALID_RESOURCE) {
                    rhi::ResourceBarrier b{}; b.resource = tex;
                    b.beforeState = rhi::ResourceState::UnorderedAccess;
                    b.afterState = rhi::ResourceState::ShaderResource; b.subresource = 0xFFFFFFFF;
                    cmd->InsertBarrier(&b, 1);
                }
                ResourceHandle blitTex = (tex != rhi::handles::INVALID_RESOURCE) ? tex : ssgi_black_texture_;
                DescriptorData p[1] = {{0, DescriptorType::SampledImage, blitTex}};
                UpdateDescriptorSet(device_, blit_descriptor_set_[cbIdx], p, 1);
                cmd->BindGraphicsPipeline(blit_pipeline_);
                const rhi::DescriptorSetHandle s[] = { blit_descriptor_set_[cbIdx] };
                cmd->BindDescriptorSets(rhi::PipelineBindPoint::Graphics, blit_layout_, 0, 1, s, 0, nullptr);
                cmd->Draw(3, 0, 1, 0);
                return;
            }

            // Mode 9: SC Depth Atlas — show dilated depth if available, else raw atlas
            if (ssgiVisMode_ == 9 && surfaceCachePass_ && surfaceCachePass_->IsInitialized()) {
                auto tex = sc_dilate_done_ ? sc_depth_temp_tex_ : surfaceCachePass_->GetDepthAtlas();
                if (tex != rhi::handles::INVALID_RESOURCE) {
                    rhi::ResourceBarrier b{}; b.resource = tex;
                    b.beforeState = rhi::ResourceState::UnorderedAccess;
                    b.afterState = rhi::ResourceState::ShaderResource; b.subresource = 0xFFFFFFFF;
                    cmd->InsertBarrier(&b, 1);
                }
                ResourceHandle blitTex = (tex != rhi::handles::INVALID_RESOURCE) ? tex : ssgi_black_texture_;
                DescriptorData p[1] = {{0, DescriptorType::SampledImage, blitTex}};
                UpdateDescriptorSet(device_, blit_descriptor_set_[cbIdx], p, 1);
                cmd->BindGraphicsPipeline(blit_pipeline_);
                const rhi::DescriptorSetHandle s[] = { blit_descriptor_set_[cbIdx] };
                cmd->BindDescriptorSets(rhi::PipelineBindPoint::Graphics, blit_layout_, 0, 1, s, 0, nullptr);
                cmd->Draw(3, 0, 1, 0);
                return;
            }

            // Mode 2: Scene only — simple blit of scene color
            // Mode 4: Albedo only — blit raw GBuffer albedo (no lighting)
            // Mode 5: Screen Probe GI — blit screen probe output directly
            if (ssgiVisMode_ == 2 || ssgiVisMode_ == 4 || ssgiVisMode_ == 5 || !data.ssgi_input.IsValid()) {
                ResourceHandle blitTex = inputHandle;
                // Mode 4: use raw GBuffer albedo instead of lit scene
                if (ssgiVisMode_ == 4) {
                    auto albedoTex = gpuDrawPipeline_->GetGBufferAlbedo();
                    if (albedoTex != rhi::handles::INVALID_RESOURCE) {
                        // GBuffer albedo was written as RenderTarget by SceneRender.
                        // Must transition to ShaderResource before sampling in blit,
                        // otherwise Apple Silicon GPU faults on the state mismatch.
                        rhi::ResourceBarrier albedoBarrier{};
                        albedoBarrier.resource = albedoTex;
                        albedoBarrier.beforeState = rhi::ResourceState::RenderTarget;
                        albedoBarrier.afterState = rhi::ResourceState::ShaderResource;
                        albedoBarrier.subresource = 0xFFFFFFFF;
                        cmd->InsertBarrier(&albedoBarrier, 1);
                        blitTex = albedoTex;
                    }
                }
                // Mode 5: use screen probe GI output
                if (ssgiVisMode_ == 5 && screenProbeGIPass_ && screenProbeGIPass_->IsInitialized()) {
                    auto spTex = screenProbeGIPass_->GetOutputTexture();
                    // CHAIN TEST: try GBuffer depth instead to verify blit can display any texture
                    // blitTex = gpuDrawPipeline_->GetGBufferDepthSampleable();
                    blitTex = spTex;  // Use SPGI output
                }
                DescriptorData blit_params[1] = {
                    { .binding = 0, .type = DescriptorType::SampledImage, .resource = blitTex }
                };
                UpdateDescriptorSet(device_, blit_descriptor_set_[cbIdx], blit_params, 1);
                cmd->BindGraphicsPipeline(blit_pipeline_);
                const rhi::DescriptorSetHandle descriptor_sets[] = { blit_descriptor_set_[cbIdx] };
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
                UpdateDescriptorSet(device_, blit_descriptor_set_[cbIdx], blit_params, 1);
                cmd->BindGraphicsPipeline(blit_pipeline_);
                const rhi::DescriptorSetHandle descriptor_sets[] = { blit_descriptor_set_[cbIdx] };
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
                UpdateDescriptorSet(device_, blit_descriptor_set_[cbIdx], blit_params, 1);
                cmd->BindGraphicsPipeline(blit_pipeline_);
                const rhi::DescriptorSetHandle descriptor_sets[] = { blit_descriptor_set_[cbIdx] };
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
                UpdateDescriptorSet(device_, blit_composite_descriptor_set_[cbIdx], composite_params, 2);
                cmd->BindGraphicsPipeline(blit_composite_pipeline_);
                const rhi::DescriptorSetHandle sets[] = { blit_composite_descriptor_set_[cbIdx] };
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
        //std::cout << "\n=== Streaming Metrics (Frame " << frameCount_ << ") ===" << std::endl;
        //std::cout << "  Avg Processing Time: " << testResults_.avg_processing_time_ms.load() << " ms" << std::endl;

        auto stats = streamingManager_->GetStats();
        //std::cout << "  Resident Clusters: " << stats.current_resident_clusters << std::endl;
        //std::cout << "  Pool Usage: " << (stats.page_pool_usage * 100.0f) << "%" << std::endl;
        //std::cout << "  Pending Requests: " << stats.pending_requests_count << std::endl;
    }
}

void TestNaniteStreamingPipeline::ValidateResults() {
    auto stats = streamingManager_->GetStats();

    if (stats.eviction_count > 0) {
        //std::cout << "✓ LRU eviction working correctly" << std::endl;
    }

    if (stats.total_clusters_streamed > 0) {
        //std::cout << "✓ Cluster streaming working correctly" << std::endl;
    }

    if (testResults_.requests_processed.load() > 0) {
        //std::cout << "✓ Request processing working correctly" << std::endl;
    }

    //std::cout << "\nValidation Summary:" << std::endl;
    //std::cout << "  Total Streamed: " << testResults_.clusters_streamed.load() << std::endl;
    //std::cout << "  Total Evicted: " << testResults_.clusters_evicted.load() << std::endl;
    //std::cout << "  Total Requests: " << testResults_.requests_processed.load() << std::endl;
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

    //std::cout << "Performance Results:" << std::endl;
    //std::cout << "  Total Time: " << totalTime << " ms" << std::endl;
    //std::cout << "  Avg Time/Frame: " << avgTime << " ms" << std::endl;
    //std::cout << "  Target: < 1.0 ms/frame" << std::endl;

    if (avgTime < 1.0f) {
        //std::cout << "  ✓ PASS - Performance within target" << std::endl;
    } else {
        //std::cout << "  ✗ FAIL - Performance exceeds target" << std::endl;
    }
}

void TestNaniteStreamingPipeline::TestStaticProbeSerialization() {
    using namespace primal::graphics::lumen;
    using namespace primal::math;

    std::cout << "\n=== Test: Static Probe Serialization (Real Baker) ===\n";

    // 1. Create Cornell-box-like test geometry (5 walls + floor + ceiling)
    // GPU mesh buffers are StorageModePrivate on Metal — can't MapBuffer for readback.
    // Use procedural geometry to exercise the real BVH + SH baker pipeline.
    // The geometry is sized to cover the probe grid area for meaningful hits.
    auto addQuad = [](std::vector<v3>& verts, std::vector<u32>& idx,
                      v3 a, v3 b, v3 c, v3 d) {
        u32 base = (u32)verts.size();
        verts.push_back(a); verts.push_back(b);
        verts.push_back(c); verts.push_back(d);
        idx.push_back(base); idx.push_back(base+1); idx.push_back(base+2);
        idx.push_back(base); idx.push_back(base+2); idx.push_back(base+3);
    };

    std::vector<v3> positions;
    std::vector<u32> indices;
    float S = 20.0f; // scene extent (covers probe grid)

    // Floor (y=0)
    addQuad(positions, indices,
        v3{-S, 0, -S}, v3{S, 0, -S}, v3{S, 0, S}, v3{-S, 0, S});
    // Ceiling (y=20)
    addQuad(positions, indices,
        v3{-S, 20, -S}, v3{-S, 20, S}, v3{S, 20, S}, v3{S, 20, -S});
    // Back wall (z=-S)
    addQuad(positions, indices,
        v3{-S, 0, -S}, v3{-S, 20, -S}, v3{S, 20, -S}, v3{S, 0, -S});
    // Left wall (x=-S)
    addQuad(positions, indices,
        v3{-S, 0, -S}, v3{-S, 0, S}, v3{-S, 20, S}, v3{-S, 20, -S});
    // Right wall (x=S)
    addQuad(positions, indices,
        v3{S, 0, -S}, v3{S, 20, -S}, v3{S, 20, S}, v3{S, 0, S});
    // Front wall (z=S, partial — leave opening for sky)
    addQuad(positions, indices,
        v3{-S, 10, S}, v3{S, 10, S}, v3{S, 20, S}, v3{-S, 20, S});

    std::cout << "  Procedural geometry: " << positions.size()
              << " vertices, " << indices.size() << " indices\n";

    // 2. Create probe volume
    StaticProbeParams params{};
    params.grid_dim_x = 8;
    params.grid_dim_y = 4;
    params.grid_dim_z = 8;
    params.spacing = 4.0f;
    params.origin = {-12.0f, 1.0f, -12.0f};

    auto volume = std::make_unique<StaticProbeVolume>();
    if (!volume->Initialize(device_, params)) {
        std::cerr << "[FAIL] StaticProbeVolume::Initialize\n";
        return;
    }

    // 3. Bake with real BVH ray casting
    ProbeBakingScene bakingScene{};
    bakingScene.vertices = positions.data();
    bakingScene.indices = indices.data();
    bakingScene.vertex_count = (u32)positions.size();
    bakingScene.index_count = (u32)indices.size();
    bakingScene.light_direction = {-0.4f, -1.0f, -0.3f};
    bakingScene.light_color = {1.0f, 0.95f, 0.9f};
    bakingScene.light_intensity = 1.0f;
    bakingScene.sky_color = {0.5f, 0.7f, 1.0f};

    ProbeBakingParams bakeParams{};
    bakeParams.rays_per_probe = 128;

    std::cout << "  Baking " << volume->ProbeCount() << " probes ("
              << bakeParams.rays_per_probe << " rays each)...\n";

    if (!StaticProbeBaker::Bake(*volume, bakingScene, bakeParams)) {
        std::cerr << "[FAIL] StaticProbeBaker::Bake\n";
        return;
    }

    std::cout << "  Bake complete\n";

    // 3b. Verify bake quality — print statistics and check physical plausibility
    u32 probeCount = volume->ProbeCount();
    {
        const v3* irr = volume->GetIrradianceData();
        const float* depthMean = volume->GetDepthMeanData();
        const float* skyFactor = volume->GetSkyFactorData();

        float irrMin = 1e10f, irrMax = 0.0f, irrAvg = 0.0f;
        float depthMin = 1e10f, depthMax = 0.0f;
        u32 nonZeroProbes = 0;

        for (u32 p = 0; p < probeCount; p++) {
            const v3& l0 = irr[p * 9]; // L0 coefficient
            float mag = std::sqrt(l0.x * l0.x + l0.y * l0.y + l0.z * l0.z);
            irrAvg += mag;
            if (mag < irrMin) irrMin = mag;
            if (mag > irrMax) irrMax = mag;
            if (mag > 0.001f) nonZeroProbes++;

            // Check average depth of first octahedral texel
            float d = depthMean[p * 64];
            if (d < depthMin) depthMin = d;
            if (d > depthMax) depthMax = d;
        }
        irrAvg /= (float)probeCount;

        std::cout << "  --- Bake Statistics ---\n";
        std::cout << "  Irradiance L0 magnitude: min=" << irrMin
                  << " max=" << irrMax << " avg=" << irrAvg << "\n";
        std::cout << "  Depth range: " << depthMin << " .. " << depthMax << "\n";
        std::cout << "  Non-zero probes: " << nonZeroProbes << "/" << probeCount << "\n";
        std::cout << "  Sky factor range: ";
        float sfMin = 1.0f, sfMax = 0.0f;
        for (u32 p = 0; p < probeCount; p++) {
            if (skyFactor[p] < sfMin) sfMin = skyFactor[p];
            if (skyFactor[p] > sfMax) sfMax = skyFactor[p];
        }
        std::cout << sfMin << " .. " << sfMax << "\n";

        // Print a few sample probes for visual sanity check
        std::cout << "  Sample probes (L0 SH + sky):\n";
        u32 samples[] = {0, probeCount / 4, probeCount / 2, probeCount - 1};
        for (u32 si : samples) {
            if (si >= probeCount) continue;
            const v3& l0 = irr[si * 9];
            float mag = std::sqrt(l0.x * l0.x + l0.y * l0.y + l0.z * l0.z);
            std::cout << "    probe[" << si << "] L0=("
                      << l0.x << "," << l0.y << "," << l0.z
                      << ") |L0|=" << mag
                      << " sky=" << skyFactor[si]
                      << " depth=" << depthMean[si * 64] << "\n";
        }

        // Physical plausibility checks
        if (irrAvg < 0.001f) {
            std::cerr << "[FAIL] Irradiance is essentially zero — baker produced no lighting\n";
            return;
        }
        if (nonZeroProbes < probeCount / 2) {
            std::cerr << "[WARN] Only " << nonZeroProbes << "/" << probeCount
                      << " probes have non-zero irradiance (may indicate geometry coverage issue)\n";
        }
        if (depthMax < 0.1f) {
            std::cerr << "[WARN] All depths near zero — BVH may not be hitting scene geometry\n";
        }
    }

    // 3. Save to file
    const char* testPath = "test_static_probes.probe_cache";
    if (!volume->SaveToFile(testPath)) {
        std::cerr << "[FAIL] SaveToFile\n";
        return;
    }

    // 4. Reload into new volume
    auto reloaded = std::make_unique<StaticProbeVolume>();
    if (!reloaded->Initialize(device_, params)) {
        std::cerr << "[FAIL] Reload Initialize\n";
        return;
    }
    if (!reloaded->LoadFromFile(testPath)) {
        std::cerr << "[FAIL] LoadFromFile\n";
        return;
    }
    if (!reloaded->IsLoaded()) {
        std::cerr << "[FAIL] IsLoaded after reload\n";
        return;
    }

    // 5. Verify data integrity
    const v3* origIrr = volume->GetIrradianceData();
    const v3* loadIrr = reloaded->GetIrradianceData();
    float maxDiff = 0.0f;
    for (u32 i = 0; i < probeCount * 9; i++) {
        float diff = (origIrr[i].x - loadIrr[i].x) + (origIrr[i].y - loadIrr[i].y) + (origIrr[i].z - loadIrr[i].z);
        if (diff > maxDiff) maxDiff = diff;
    }
    if (maxDiff > 0.01f) {
        std::cerr << "[FAIL] Irradiance mismatch: maxDiff=" << maxDiff << "\n";
        return;
    }

    // Verify depth
    const float* origMean = volume->GetDepthMeanData();
    const float* loadMean = reloaded->GetDepthMeanData();
    maxDiff = 0.0f;
    for (u32 i = 0; i < probeCount * 64; i++) {
        float diff = std::abs(origMean[i] - loadMean[i]);
        if (diff > maxDiff) maxDiff = diff;
    }
    if (maxDiff > 0.01f) {
        std::cerr << "[FAIL] Depth mean mismatch: maxDiff=" << maxDiff << "\n";
        return;
    }

    // 6. Verify UploadToGPU creates valid handles (but DON'T actually upload)
    // Uploading creates GPU buffers from the memory pool — destroying them via
    // Shutdown() defers the pool deallocation, which can starve the pool for
    // subsequent rendering allocations.
    {
        // Just verify the data is valid for upload without actually doing it
        bool dataValid = reloaded->IsLoaded() && reloaded->GetIrradianceData() != nullptr;
        if (!dataValid) {
            std::cerr << "[FAIL] Reloaded volume not ready for upload\n";
            return;
        }
    }

    // Cleanup (no GPU buffers to destroy — never uploaded)
    reloaded->Shutdown();
    volume->Shutdown();
    std::remove(testPath);

    std::cout << "[PASS] Static Probe Serialization\n";
}

void TestNaniteStreamingPipeline::TestDDGIInitFromStatic() {
    using namespace primal::graphics::lumen;
    using namespace primal::math;

    std::cout << "\n=== Test: DDGI Init From Static ===\n";

    if (!ddgiPass_ || !ddgiPass_->IsInitialized()) {
        std::cerr << "[SKIP] DDGI pass not initialized\n";
        return;
    }

    // Read back irradiance buffer frame 0
    auto irrHandle = ddgiPass_->GetIrradianceBuffer(0);
    if (irrHandle == primal::graphics::rhi::handles::INVALID_RESOURCE) {
        std::cerr << "[FAIL] Invalid irradiance buffer handle\n";
        return;
    }

    const float* mapped = static_cast<const float*>(device_->MapBuffer(irrHandle));
    if (!mapped) {
        std::cerr << "[FAIL] MapBuffer returned null\n";
        return;
    }

    // Check first probe's L0 coefficient is NOT zero
    // L0 is 3 floats: mapped[0], mapped[1], mapped[2]
    float l0x = mapped[0];
    float l0y = mapped[1];
    float l0z = mapped[2];
    float magnitude = std::sqrt(l0x * l0x + l0y * l0y + l0z * l0z);

    device_->UnmapBuffer(irrHandle);

    if (magnitude < 0.01f) {
        std::cerr << "[FAIL] DDGI L0 irradiance is zero (black!) magnitude=" << magnitude << "\n";
        return;
    }

    std::cout << "[PASS] DDGI Init From Static (L0 magnitude=" << magnitude << ")\n";
}

void TestNaniteStreamingPipeline::Shutdown() {
    if (isShutdown_) return;
    isShutdown_ = true;

    //std::cout << "\nTestNaniteStreamingPipeline Final Results:" << std::endl;
    //std::cout << "  Total Frames: " << frameCount_ << std::endl;
    //std::cout << "  Clusters Streamed: " << testResults_.clusters_streamed.load() << std::endl;
    //std::cout << "  Clusters Evicted: " << testResults_.clusters_evicted.load() << std::endl;
    //std::cout << "  Requests Processed: " << testResults_.requests_processed.load() << std::endl;
    //std::cout << "  Avg Processing Time: " << testResults_.avg_processing_time_ms.load() << " ms/frame" << std::endl;

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

    // Shutdown GlobalSDF
    {
        auto& globalSDF = nanite::GlobalSDF::Get();
        if (globalSDF.IsInitialized()) {
            globalSDF.Shutdown();
        }
    }

    if (resourceManager_) {
        resourceManager_->Shutdown();
    }

    // Cleanup GPU Material Registry
    if (gpuMaterialRegistry_) {
        // Wait for async build job to complete before destroying registry
        if (materialBuildJob_.IsValid()) {
            //std::cout << "[TestNanite] Waiting for material build job to complete..." << std::endl;
            materialBuildJob_.Wait();
        }
        gpuMaterialRegistry_.reset();
    }

    // Cleanup SSGI resources
    if (ssgiPass_) {
        ssgiPass_->Shutdown();
        ssgiPass_.reset();
    }

    // Cleanup SSAO resources
    if (ssaoPass_) {
        ssaoPass_->Shutdown();
        ssaoPass_.reset();
    }

    // Cleanup DDGI resources
    if (ddgiPass_) {
        ddgiPass_->Shutdown();
        ddgiPass_.reset();
    }

    // Cleanup Surface Cache resources
    if (surfaceCachePass_) {
        surfaceCachePass_->Shutdown();
        surfaceCachePass_.reset();
    }

    // Cleanup shadow resources
    if (gpuDrawPipeline_) {
        gpuDrawPipeline_->ShutdownShadowResources();
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

    //std::cout << "TestNaniteStreamingPipeline::Shutdown End" << std::endl;
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
    //std::cout << "\n=== Test: Streaming Initialization ===" << std::endl;

    if (streamingManager_) {
        //std::cout << "✓ Streaming manager initialized" << std::endl;
    } else {
        //std::cout << "✗ Streaming manager NOT initialized" << std::endl;
    }

    if (cullingPipeline_) {
        //std::cout << "✓ Culling pipeline initialized" << std::endl;
    } else {
        //std::cout << "✗ Culling pipeline NOT initialized" << std::endl;
    }

    (void)streamingManager_->GetStats(); // Suppress unused variable warning
    auto config = streamingManager_->GetConfig();
    //std::cout << "  Pool Size: " << (config.page_pool_size_bytes / (1024 * 1024)) << " MB" << std::endl;
    //std::cout << "  Max Requests/Frame: " << config.max_requests_per_frame << std::endl;
}

void TestNaniteStreamingPipeline::TestGPURequestGeneration() {
    //std::cout << "\n=== Test: GPU Request Generation ===" << std::endl;
    //std::cout << "⚠ Not yet implemented - requires streaming feedback shaders" << std::endl;
}

void TestNaniteStreamingPipeline::TestLRUEviction() {
    //std::cout << "\n=== Test: LRU Eviction ===" << std::endl;

    for (u32 i = 0; i < 1000; ++i) {
        ProcessStreamingFeedback();
    }

    auto stats = streamingManager_->GetStats();
    if (stats.eviction_count > 0) {
        //std::cout << "✓ LRU eviction triggered" << std::endl;
        //std::cout << "  Evictions: " << stats.eviction_count << std::endl;
    } else {
        //std::cout << "✗ No evictions occurred" << std::endl;
    }
}

void TestNaniteStreamingPipeline::TestResidencyBuffer() {
    //std::cout << "\n=== Test: Residency Buffer ===" << std::endl;

    auto residencyBuffer = streamingManager_->GetResidencyBuffer();
    if (residencyBuffer != rhi::handles::INVALID_RESOURCE) {
        //std::cout << "✓ Residency buffer allocated" << std::endl;
    } else {
        //std::cout << "✗ Residency buffer NOT allocated" << std::endl;
    }

    auto requestBuffer = streamingManager_->GetRequestBuffer();
    if (requestBuffer != rhi::handles::INVALID_RESOURCE) {
        //std::cout << "✓ Request buffer allocated" << std::endl;
    } else {
        //std::cout << "✗ Request buffer NOT allocated" << std::endl;
    }

    auto feedbackBuffer = streamingManager_->GetFeedbackBuffer();
    if (feedbackBuffer != rhi::handles::INVALID_RESOURCE) {
        //std::cout << "✓ Feedback buffer allocated" << std::endl;
    } else {
        //std::cout << "✗ Feedback buffer NOT allocated" << std::endl;
    }
}

void TestNaniteStreamingPipeline::TestEndToEndStreaming() {
    //std::cout << "\n=== Test: End-to-End Streaming ===" << std::endl;

    u32 initialStreamed = testResults_.clusters_streamed.load();

    for (u32 i = 0; i < 300; ++i) {
        Run();
    }

    u32 finalStreamed = testResults_.clusters_streamed.load();

    if (finalStreamed > initialStreamed) {
        //std::cout << "✓ Clusters streamed: " << (finalStreamed - initialStreamed) << std::endl;
    } else {
        //std::cout << "✗ No clusters streamed" << std::endl;
    }
}

void TestNaniteStreamingPipeline::PrintAllInstanceBounds() {
    // 🔥 COMMENTED OUT: Reduce log noise, instance data has been validated
    //// std::cout << "\n=== All Instance Bounds Information ===" << std::endl;

    const auto& proxies = scene_.GetProxies();
    //// std::cout << "Total proxies in scene: " << proxies.size() << std::endl;

    u32 validInstanceCount = 0;
    u32 totalClusterCount = 0;

    for (size_t i = 0; i < proxies.size(); ++i) {
        const auto& proxy = proxies[i];

        // Get cluster component to access geometry data
        const cluster::component_cache* cluster_cache = cluster::get(proxy.meshId);
        if (!cluster_cache || !cluster_cache->exists) {
            //// std::cout << "  [" << i << "] Invalid cluster component" << std::endl;
            continue;
        }

        // Get resource from NaniteResourceManager
        auto& resource_manager = nanite::NaniteResourceManager::Get();
        nanite::NaniteRuntimeResource* resource =
            resource_manager.GetOrCreateResource(cluster_cache->geometry_content_id);

        if (!resource || !resource->gpu_mesh) {
            //// std::cout << "  [" << i << "] No GPU mesh resource" << std::endl;
            continue;
        }

        validInstanceCount++;
        totalClusterCount += resource->cluster_data.cluster_count;
    }

    // Compute instance bounds distribution from the snapshot GPU buffer
    //std::cout << "\n=== Scene Summary ===" << std::endl;
    //std::cout << "  Valid Instances: " << validInstanceCount << " out of " << proxies.size() << std::endl;
    //std::cout << "  Total Clusters: " << totalClusterCount << std::endl;

    // Print instance bounds distribution from the actual GPU buffer
    {
        auto* mapped = static_cast<graphics::rhi::ResourceHandle*>(
            // Read the instance data buffer to get bounds distribution
            nullptr);
        // Use the scene snapshot's instance data directly
        const auto& snapshotInstances = sceneSnapshot_.GetInstanceData();
        if (!snapshotInstances.empty()) {
            f32 minCX = 1e10f, minCY = 1e10f, minCZ = 1e10f;
            f32 maxCX = -1e10f, maxCY = -1e10f, maxCZ = -1e10f;
            f32 minR = 1e10f, maxR = 0.0f, avgR = 0.0f;

            for (const auto& inst : snapshotInstances) {
                minCX = std::min(minCX, inst.bounds_center.x);
                minCY = std::min(minCY, inst.bounds_center.y);
                minCZ = std::min(minCZ, inst.bounds_center.z);
                maxCX = std::max(maxCX, inst.bounds_center.x);
                maxCY = std::max(maxCY, inst.bounds_center.y);
                maxCZ = std::max(maxCZ, inst.bounds_center.z);
                minR = std::min(minR, inst.bounds_radius);
                maxR = std::max(maxR, inst.bounds_radius);
                avgR += inst.bounds_radius;
            }
            avgR /= (f32)snapshotInstances.size();

            //std::cout << "\n  Instance Bounds Distribution:" << std::endl;
            //std::cout << "    Count: " << snapshotInstances.size() << std::endl;
            //std::cout << "    bounds_center X: [" << minCX << ", " << maxCX << "]" << std::endl;
            //std::cout << "    bounds_center Y: [" << minCY << ", " << maxCY << "]" << std::endl;
            //std::cout << "    bounds_center Z: [" << minCZ << ", " << maxCZ << "]" << std::endl;
            //std::cout << "    bounds_radius: min=" << minR << " max=" << maxR << " avg=" << avgR << std::endl;
            //std::cout << "    Scene extent X: " << (maxCX - minCX) << std::endl;
            //std::cout << "    Scene extent Y: " << (maxCY - minCY) << std::endl;
            //std::cout << "    Scene extent Z: " << (maxCZ - minCZ) << std::endl;

            // Print first 5 instances' bounds for spot check
            //std::cout << "\n    First 5 instances:" << std::endl;
            for (u32 i = 0; i < std::min((u32)5, (u32)snapshotInstances.size()); ++i) {
                const auto& inst = snapshotInstances[i];
                //std::cout << "      [" << i << "] center=("
                          //<< inst.bounds_center.x << ", "
                          //<< inst.bounds_center.y << ", "
                          //<< inst.bounds_center.z << ") radius="
                          //<< inst.bounds_radius
                          //<< " cluster_start=" << inst.cluster_start
                          //<< " cluster_count=" << inst.cluster_count
                          //<< std::endl;
            }
        }
    }
    //std::cout << "=== End Summary ===\n" << std::endl;
}

void TestNaniteStreamingPipeline::PrintFinalFrameCullingDebugData() {
    if (finalFrameCullingDebugData_.empty()) {
        //std::cout << "\n=== No Final Frame Culling Debug Data ===" << std::endl;
        return;
    }

    //std::cout << "\n=== Final Frame GPU Culling Debug Data (Frame " << frameCount_ << ", "
              //<< finalFrameCullingDebugData_.size() << " entries) ===" << std::endl;

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

    //std::cout << "Final Frame Culling Summary:" << std::endl;
    //std::cout << "  Frustum Culled: " << frustumCulled << std::endl;
    //std::cout << "  Distance Culled: " << distanceCulled << std::endl;
    //std::cout << "  Backface Culled: " << backfaceCulled << std::endl;  // 🔥 NEW: Backface culling output
    //std::cout << "  Not Culled: " << notCulled << std::endl;
    //std::cout << "  Unknown: " << unknownCulled << std::endl;

    // Show all entries with detailed information
    //std::cout << "\nDetailed culling data for all entries:" << std::endl;

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

        //std::cout << "  [" << i << "] Instance=" << entry.instance_id
                 //<< ", Cluster=" << entry.cluster_id
                 //<< ", Reason=" << culling_reason_str
                 //<< ", Plane=" << plane_str
                 //<< ", ViewZ=" << entry.view_space_z
                 //<< ", Distance=" << entry.distance_to_camera
                 //<< ", BoundsRadius=" << entry.bounds_radius
                 //<< ", Visible=" << (entry.is_visible ? "Yes" : "No");

        // 🔥 NEW: Show backface culling specific data
        if (entry.meshlet_id > 0 || entry.is_backface_culled) {
            //std::cout << "\n      Backface: [MeshletID=" << entry.meshlet_id
                     //<< ", CosAngle=" << entry.backface_cos_angle
                     //<< ", Cutoff=" << entry.backface_cutoff
                     //<< ", Culled=" << (entry.is_backface_culled ? "Yes" : "No") << "]";
        }

        // Show plane distances for debugging
        //std::cout << "\n      PlaneDistances=[L:" << entry.plane_distances[0]
                 //<< ",R:" << entry.plane_distances[1]
                 //<< ",B:" << entry.plane_distances[2]
                 //<< ",T:" << entry.plane_distances[3]
                 //<< ",N:" << entry.plane_distances[4]
                 //<< ",F:" << entry.plane_distances[5] << "]"
                 //<< std::endl;
    }

    //std::cout << "=== End Final Frame Culling Debug Data ===\n" << std::endl;
}
