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
#include "Engine/Graphics/Nanite/VisibilityBufferSystem.h"
#include "Engine/Input/Input.h"
#include "Engine/Components/Entity.h"
#include "ShaderCompilation.h"

#include <iostream>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <cmath>

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
    cullingPipeline_ = &graphics::nanite::GPUCullingPipeline::Get();
    std::cout << "[TestNanite] GPUCullingPipeline singleton obtained: " << (void*)cullingPipeline_ << std::endl;

    graphics::nanite::CullingConfig cullingConfig;
    cullingConfig.max_clusters_per_dispatch = testConfig_.max_clusters;
    cullingConfig.max_instances_per_dispatch = testConfig_.max_instances;
    cullingConfig.enable_streaming_feedback = testConfig_.enable_streaming;
    cullingConfig.enable_occlusion_culling = false; // DISABLED: HZB Occlusion Culling to isolate flickering
    cullingConfig.enable_lod_selection = false; // DISABLED: LOD Selection to isolate flickering
    // cullingConfig.enable_occlusion_culling = testConfig_.enable_occlusion_culling;
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

    // Initialize GPU Driven Draw Pipeline
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

    if (!gpuDrawPipeline_->Initialize(device_, binningConfig, visibilityConfig)) {
        std::cerr << "Failed to initialize GPU driven draw pipeline" << std::endl;
        return false;
    }

    // CRITICAL: Connect culling pipeline to draw pipeline for proper buffer access
    gpuDrawPipeline_->SetCullingPipeline(cullingPipeline_);

    // CONNECT HZB AND VISIBILITY BUFFER SYSTEMS TO GPU DRIVEN PIPELINE - DISABLED
    // gpuDrawPipeline_->SetHZBSystem(hzbSystem_.get());
    // gpuDrawPipeline_->SetVisibilityBufferSystem(visibilityBufferSystem_.get());
    std::cout << "[TestNanite] HZB and Visibility Buffer systems DISABLED, not connected to GPU pipeline" << std::endl;

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

    // Initialize HZB System - DISABLED for now to focus on basic GPU rendering
    // hzbSystem_ = std::make_unique<HZBSystem>();
    // HZBSystem::Config hzbConfig;
    // hzbConfig.max_width = renderWidth_;
    // hzbConfig.max_height = renderHeight_;
    // hzbConfig.min_mip_size = 8;
    // hzbConfig.enable_compression = false; // Disable compression for now
    // hzbConfig.generate_on_gpu = false; // Use CPU for now (GPU not implemented)
    //
    // if (!hzbSystem_->Initialize(device_, hzbConfig)) {
    //     std::cerr << "Failed to initialize HZB system" << std::endl;
    //     return false;
    // }
    std::cout << "[TestNanite] HZB System DISABLED" << std::endl;

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

    return true;
}

bool TestNaniteStreamingPipeline::CreateTestScene() {
    if (!sceneSnapshot_.Initialize(device_, testConfig_.max_instances, testConfig_.max_clusters)) {
        return false;
    }

    // Load Sponza scene
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
                entityId, meshInfo.meshEntityId, primal::id::invalid_id);

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

    // CRITICAL: Debug output for buffer synchronization
    // This is essential to prevent flickering caused by reading previous frame data
    if (frameCount_ < 5) { // Only debug first few frames
        std::cout << "[BuildRenderGraph] Frame " << frameCount_ << " using synced buffer index=" << currentBufferIndex << std::endl;
    }

    // Import backbuffer
    auto backBufferHandle = graph.ImportResource("BackBuffer", backBuffer);

    // Create depth texture for HZB generation - DISABLED to prevent memory leak
    // rhi::TextureDesc depthDesc{};
    // depthDesc.size = {renderWidth_, renderHeight_, 1};
    // depthDesc.format = rhi::DataFormat::D32_Float;
    // depthDesc.usage = rhi::TextureUsage::DepthStencil | rhi::TextureUsage::ShaderResource;
    // auto depthTexture = device_->CreateTexture(depthDesc);
    // auto depthHandle = graph.ImportResource("SceneDepth", depthTexture);

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

            if (frameCount_ < 5) { // Debug first 5 frames
                std::cout << "[NaniteCulling] Frame " << frameCount_ << " using synced buffer index=" << currentBufferIndex << " buffer " << indirectBuffer << std::endl;
            }
        },
        [this, currentBufferIndex](const CullingPassData& data, graphics::rendergraph::RenderGraphContext& context) {
            auto cmd = context.cmdBuffer;

            // CRITICAL FIX: Use triple-buffered camera data matching the system
            u32 bufferIndex = currentBufferIndex;
            if (frameCount_ < 10) { // Debug first 10 frames
                std::cout << "[Draw] Frame " << frameCount_ << " using bufferIndex=" << bufferIndex << std::endl;
            }

            // std::cout << "[BuildRenderGraph] NaniteCulling PASS EXECUTE called!" << std::endl;
            // std::cout << "[BuildRenderGraph] cullingPipeline_=" << (void*)cullingPipeline_
            //           << ", initialized=" << (cullingPipeline_ ? cullingPipeline_->IsInitialized() : 0) << std::endl;

            // Build HZB from previous frame's depth (if available) - DISABLED
            if (false && hzbSystem_ && hzbSystem_->IsReady()) {
                // std::cout << "[BuildRenderGraph] Building HZB for occlusion culling..." << std::endl;
                // TODO: Convert RGResourceHandle to ResourceHandle when HZB system is ready
                // auto hzbResult = hzbSystem_->BuildHZB(depthTexture, cmd, frameCount_);
                // std::cout << "[BuildRenderGraph] HZB building - TODO: Convert RGResourceHandle" << std::endl;
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

            if (frameCount_ < 5) { // Debug first 5 frames
                std::cout << "[SceneRender] Frame " << frameCount_ << " using synced buffer index=" << currentBufferIndex << std::endl;
            }
        },
        [this, currentBufferIndex](const SceneRenderPassData& data, graphics::rendergraph::RenderGraphContext& context) {
            // std::cout << "[DEBUG] SceneRender PASS - Executing GPU pipeline..." << std::endl;
            auto cmd = context.cmdBuffer;

            // CRITICAL FIX: Use triple-buffered camera data matching the system
            u32 bufferIndex = currentBufferIndex;
            if (frameCount_ < 10) { // Debug first 10 frames
                std::cout << "[Draw] Frame " << frameCount_ << " using bufferIndex=" << bufferIndex << std::endl;
            }

            // Execute the complete GPU-driven draw pipeline
            // The pipeline handles its own render pass begin/end internally
            if (!gpuDrawPipeline_->Execute(cmd, *data.scene_snapshot, cameraBuffers_[bufferIndex].view_matrix, cameraBuffers_[bufferIndex].proj_matrix, *data.culling_results, frameCount_, currentBufferIndex)) {
                std::cerr << "[DEBUG] GPU-driven draw pipeline failed!" << std::endl;
            } else {
                // std::cout << "[DEBUG] GPU-driven draw pipeline completed successfully!" << std::endl;
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

    // === FINAL BLIT PASS (Following TestParticleSponza pattern) ===
    // CRITICAL: This pass MUST depend on SceneRender to ensure Nanite output is ready
    struct BlitPassData {
        rendergraph::RGResourceHandle input;
        rendergraph::RGResourceHandle output;
    };

    graph.AddPass<BlitPassData>("FinalBlit",
        graphics::rendergraph::RGPassType::Graphics,
        graphics::rendergraph::RGPassCategory::PostProcess,
        [this, backBufferHandle, gpuOutputHandle](BlitPassData& data, graphics::rendergraph::RenderGraphBuilder& builder) {
            // std::cout << "[FinalBlit] SETUP: Configuring final blit with GPU pipeline dependency" << std::endl;

            // CRITICAL: Read from GPU pipeline's final output
            // This establishes the dependency: FinalBlit depends on SceneRender
            data.input = builder.Read(gpuOutputHandle, rhi::ResourceState::ShaderResource);
            // std::cout << "[FinalBlit] SETUP: Reading from GPUFinalOutput (GPU pipeline's output)" << std::endl;

            data.output = builder.Write(backBufferHandle, rhi::ResourceState::RenderTarget);
            // std::cout << "[FinalBlit] SETUP: Writing to backbuffer" << std::endl;

            graphics::rendergraph::RGRenderPassDesc rpDesc;
            rpDesc.colors.push_back({
                .texture = data.output,
                .loadOp = rhi::LoadAction::DontCare,  // Don't clear - we're overwriting everything
                .storeOp = rhi::StoreAction::Store,
                .clearColor = { primal::math::v4{0,0,0,1} }
            });
            builder.DeclareRenderPass(rpDesc);

            // std::cout << "[FinalBlit] SETUP: Pass configured with GPU pipeline dependency" << std::endl;
        },
        [this](const BlitPassData& data, graphics::rendergraph::RenderGraphContext& context) {
            auto cmd = context.cmdBuffer;

            // std::cout << "[DEBUG] Final blit pass executing - using shader-based fullscreen quad" << std::endl;

            // Set viewport and scissor for fullscreen rendering
            cmd->SetViewport({ {0, 0}, {static_cast<float>(renderWidth_), static_cast<float>(renderHeight_)}, 0, 1 });
            cmd->SetScissor({ {0, 0}, {renderWidth_, renderHeight_} });

            // Get the input texture (Nanite output)
            auto inputResource = context.graph->GetResource(data.input);
            if (!inputResource) {
                std::cerr << "[DEBUG] Failed to get input resource for shader blit!" << std::endl;
                return;
            }

            auto inputHandle = inputResource->GetPhysicalHandle();
            if (inputHandle == primal::graphics::rhi::handles::INVALID_RESOURCE) {
                std::cerr << "[DEBUG] Invalid input texture handle!" << std::endl;
                return;
            }

            // Update descriptor set with input texture (following TestParticleSponza pattern)
            DescriptorData blit_params[1] = {
                { .binding = 0, .type = DescriptorType::SampledImage, .resource = inputHandle }
            };
            UpdateDescriptorSet(device_, blit_descriptor_set_, blit_params, 1);

            // Bind blit pipeline
            cmd->BindGraphicsPipeline(blit_pipeline_);

            // Bind descriptor set
            const primal::graphics::rhi::DescriptorSetHandle descriptor_sets[] = { blit_descriptor_set_ };
            cmd->BindDescriptorSets(primal::graphics::rhi::PipelineBindPoint::Graphics, blit_layout_, 0, 1, descriptor_sets, 0, nullptr);

            // Draw fullscreen triangle (3 vertices)
            cmd->Draw(3, 0, 1, 0);

            // std::cout << "[DEBUG] Shader-based blit completed successfully" << std::endl;
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
    std::cout << "\n=== All Instance Bounds Information ===" << std::endl;

    const auto& proxies = scene_.GetProxies();
    std::cout << "Total proxies in scene: " << proxies.size() << std::endl;

    u32 validInstanceCount = 0;
    u32 totalClusterCount = 0;

    for (size_t i = 0; i < proxies.size(); ++i) {
        const auto& proxy = proxies[i];

        // Get cluster component to access geometry data
        const cluster::component_cache* cluster_cache = cluster::get(proxy.meshId);
        if (!cluster_cache || !cluster_cache->exists) {
            std::cout << "  [" << i << "] Invalid cluster component" << std::endl;
            continue;
        }

        // Get resource from NaniteResourceManager
        auto& resource_manager = nanite::NaniteResourceManager::Get();
        nanite::NaniteRuntimeResource* resource =
            resource_manager.GetOrCreateResource(cluster_cache->geometry_content_id);

        if (!resource || !resource->gpu_mesh) {
            std::cout << "  [" << i << "] No GPU mesh resource" << std::endl;
            continue;
        }

        // Get bounds from GPU mesh (local space)
        const f32* boundsMin = resource->gpu_mesh->GetBoundsMin();
        const f32* boundsMax = resource->gpu_mesh->GetBoundsMax();

        // Calculate local space bounds
        primal::math::v3 localCenter{
            (boundsMin[0] + boundsMax[0]) * 0.5f,
            (boundsMin[1] + boundsMax[1]) * 0.5f,
            (boundsMin[2] + boundsMax[2]) * 0.5f
        };

        primal::math::v3 localExtent{
            (boundsMax[0] - boundsMin[0]) * 0.5f,
            (boundsMax[1] - boundsMin[1]) * 0.5f,
            (boundsMax[2] - boundsMin[2]) * 0.5f
        };

        // Calculate world space bounds (same as RenderSceneSnapshot)
        primal::math::v4 worldCenter4 = proxy.transform * primal::math::v4{localCenter.x, localCenter.y, localCenter.z, 1.0f};
        primal::math::v3 worldCenter{worldCenter4.x, worldCenter4.y, worldCenter4.z};

        // Calculate radius
        f32 maxLocalExtent = std::max({localExtent.x, localExtent.y, localExtent.z});
        const simd::float4x4& transform = proxy.transform;
        f32 maxScale = std::max({
            std::abs(transform.columns[0].x), std::abs(transform.columns[0].y), std::abs(transform.columns[0].z),
            std::abs(transform.columns[1].x), std::abs(transform.columns[1].y), std::abs(transform.columns[1].z),
            std::abs(transform.columns[2].x), std::abs(transform.columns[2].y), std::abs(transform.columns[2].z)
        });
        f32 worldRadius = maxLocalExtent * maxScale;

        std::cout << "  [" << i << "] Geometry ID: " << cluster_cache->geometry_content_id << std::endl;
        std::cout << "      Local Space:" << std::endl;
        std::cout << "        Center: (" << localCenter.x << ", " << localCenter.y << ", " << localCenter.z << ")" << std::endl;
        std::cout << "        Extent: (" << localExtent.x << ", " << localExtent.y << ", " << localExtent.z << ")" << std::endl;
        std::cout << "        AABB Min: (" << boundsMin[0] << ", " << boundsMin[1] << ", " << boundsMin[2] << ")" << std::endl;
        std::cout << "        AABB Max: (" << boundsMax[0] << ", " << boundsMax[1] << ", " << boundsMax[2] << ")" << std::endl;
        std::cout << "      World Space:" << std::endl;
        std::cout << "        Center: (" << worldCenter.x << ", " << worldCenter.y << ", " << worldCenter.z << ")" << std::endl;
        std::cout << "        Radius: " << worldRadius << std::endl;
        std::cout << "        Max Scale: " << maxScale << std::endl;
        std::cout << "      Cluster Count: " << resource->cluster_data.cluster_count << std::endl;

        validInstanceCount++;
        totalClusterCount += resource->cluster_data.cluster_count;
    }

    std::cout << "\nSummary:" << std::endl;
    std::cout << "  Valid Instances: " << validInstanceCount << " out of " << proxies.size() << std::endl;
    std::cout << "  Total Clusters: " << totalClusterCount << std::endl;
    std::cout << "=== End Instance Bounds Information ===\n" << std::endl;
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
    u32 notCulled = 0;
    u32 unknownCulled = 0;

    for (const auto& entry : finalFrameCullingDebugData_) {
        switch (entry.culling_reason) {
            case 0: frustumCulled++; break;
            case 1: distanceCulled++; break;
            case 2: notCulled++; break;
            default: unknownCulled++; break;
        }
    }

    std::cout << "Final Frame Culling Summary:" << std::endl;
    std::cout << "  Frustum Culled: " << frustumCulled << std::endl;
    std::cout << "  Distance Culled: " << distanceCulled << std::endl;
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
            entry.culling_reason == 2 ? "None" : "Unknown";

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