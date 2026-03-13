#include "TestNaniteStreamingPipeline.h"
#include "Engine/Content/ContentToEngine.h"
#include "Engine/Content/AsyncResourceLoader.h"
#include "Engine/JobSystem/JobSystem.h"
#include "Engine/Graphics/RHI/Core/RHIMath.h"
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
    cullingConfig.enable_occlusion_culling = testConfig_.enable_occlusion_culling;
    cullingConfig.enable_lod_selection = testConfig_.enable_lod_selection;

    std::cout << "[TestNanite] Initializing GPUCullingPipeline..." << std::endl;
    if (!cullingPipeline_->Initialize(device_, cullingConfig)) {
        std::cerr << "Failed to initialize GPU culling pipeline" << std::endl;
        return false;
    }
    std::cout << "[TestNanite] GPUCullingPipeline initialized, IsInitialized=" << cullingPipeline_->IsInitialized() << std::endl;

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
            return "shaders/";
        }
        // Second try: Darwin/Debug directory
        if (std::ifstream("Darwin/Debug/shaders/DeferredLighting.metal").good()) {
            return "Darwin/Debug/shaders/";
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

            std::cout << "Added mesh: " << meshInfo.name 
                      << " (entityId=" << entityId 
                      << ", geometryId=" << meshInfo.meshEntityId << ")" << std::endl;
        }
    }

    std::cout << "Added " << scene_.GetProxies().size() << " proxies to render scene" << std::endl;

    // Bind scene data to snapshot for Nanite culling
    if (!sceneSnapshot_.Rebind(scene_)) {
        std::cerr << "Failed to bind scene data to snapshot for Nanite culling" << std::endl;
        return false;
    }

    std::cout << "Scene snapshot updated with " << sceneSnapshot_.GetInstanceCount() << " instances" << std::endl;

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
        BuildRenderGraph(*renderGraph_, backBuffer);
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

    if (frameCount_ % 60 == 0) {
        auto stats = streamingManager_->GetStats();
        std::cout << "[Frame " << frameCount_ << "] "
                  << "Streamed: " << testResults_.clusters_streamed.load()
                  << ", Evicted: " << testResults_.clusters_evicted.load()
                  << ", Requests: " << testResults_.requests_processed.load()
                  << ", Pool Usage: " << (stats.page_pool_usage * 100.0f) << "%"
                  << std::endl;
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

    // Update camera (like TestParticleSponza does)
    camera_.Update(0.016f); // Fixed dt for test

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
}

void TestNaniteStreamingPipeline::BuildRenderGraph(
    graphics::rendergraph::RenderGraph& graph,
    graphics::rhi::ResourceHandle backBuffer) {

    // Import backbuffer
    auto backBufferHandle = graph.ImportResource("BackBuffer", backBuffer);

    // Create depth texture for HZB generation
    rhi::TextureDesc depthDesc{};
    depthDesc.size = {renderWidth_, renderHeight_, 1};
    depthDesc.format = rhi::DataFormat::D32_Float;
    depthDesc.usage = rhi::TextureUsage::DepthStencil | rhi::TextureUsage::ShaderResource;
    auto depthTexture = device_->CreateTexture(depthDesc);
    auto depthHandle = graph.ImportResource("SceneDepth", depthTexture);

    struct CullingPassData {
        rendergraph::RGResourceHandle depth_buffer;
        rendergraph::RGResourceHandle hzb_buffer;
        rendergraph::RGResourceHandle visibility_buffer;
        rendergraph::RGResourceHandle indirect_args_buffer;
    };

    const auto& cullingData = graph.AddPass<CullingPassData>("NaniteCulling",
        graphics::rendergraph::RGPassType::Compute,
        graphics::rendergraph::RGPassCategory::Main,
        [this](CullingPassData& data, graphics::rendergraph::RenderGraphBuilder& builder) {
            std::cout << "[BuildRenderGraph] NaniteCulling PASS SETUP called!" << std::endl;

            // Create indirect args buffer - this will be the output of this pass
            rhi::BufferDesc indirectDesc{};
            indirectDesc.size = sizeof(u32) * 5;
            indirectDesc.usage = rhi::GPUMemoryUsage::Dynamic;
            indirectDesc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
            data.indirect_args_buffer = builder.CreateBuffer(
                "indirect_args",
                indirectDesc,
                rhi::ResourceState::UnorderedAccess
            );

            // CRITICAL: Tell RenderGraph we're writing to this buffer
            // This establishes the dependency: SceneRender depends on NaniteCulling
            builder.Write(data.indirect_args_buffer, rhi::ResourceState::UnorderedAccess);

            std::cout << "[NaniteCulling] Created indirect args buffer, establishing write dependency" << std::endl;
        },
        [this](const CullingPassData& data, graphics::rendergraph::RenderGraphContext& context) {
            auto cmd = context.cmdBuffer;

            std::cout << "[BuildRenderGraph] NaniteCulling PASS EXECUTE called!" << std::endl;
            std::cout << "[BuildRenderGraph] cullingPipeline_=" << (void*)cullingPipeline_
                      << ", initialized=" << (cullingPipeline_ ? cullingPipeline_->IsInitialized() : 0) << std::endl;

            // Build HZB from previous frame's depth (if available) - DISABLED
            if (false && hzbSystem_ && hzbSystem_->IsReady()) {
                std::cout << "[BuildRenderGraph] Building HZB for occlusion culling..." << std::endl;
                // TODO: Convert RGResourceHandle to ResourceHandle when HZB system is ready
                // auto hzbResult = hzbSystem_->BuildHZB(depthTexture, cmd, frameCount_);
                std::cout << "[BuildRenderGraph] HZB building - TODO: Convert RGResourceHandle" << std::endl;
            }

            // Render visibility buffer (if enabled) - DISABLED
            if (false && visibilityBufferSystem_ && visibilityBufferSystem_->IsReady()) {
                std::cout << "[BuildRenderGraph] Rendering visibility buffer..." << std::endl;
                auto visResult = visibilityBufferSystem_->RenderVisibilityBuffer(
                    cmd,
                    sceneSnapshot_,
                    cameraView_,
                    cameraProj_,
                    cullingPipeline_->GetResults(),
                    frameCount_
                );
                std::cout << "[BuildRenderGraph] Visibility buffer rendered: " << visResult.visible_triangles
                          << " triangles in " << visResult.render_time_ms << " ms" << std::endl;
            }

            if (!cullingPipeline_->Execute(
                cmd,
                sceneSnapshot_,
                cameraView_,
                cameraProj_,
                streamingManager_,
                frameCount_)) {
                std::cerr << "[NaniteCulling] Culling pipeline failed!" << std::endl;
            }

            const auto& results = cullingPipeline_->GetResults();
            std::cout << "[NaniteCulling] Culling complete: visible_clusters=" << results.visible_cluster_count
                      << ", visible_instances=" << results.visible_instance_count << std::endl;
        }
    );
    
    std::cout << "[BuildRenderGraph] NaniteCulling pass ADDED to graph" << std::endl;

    // Scene rendering pass - let GPU draw pipeline handle its own render pass management
    struct SceneRenderPassData {
        rendergraph::RGResourceHandle gpu_output;     // Import GPU pipeline's final output
        rendergraph::RGResourceHandle output;         // Final output (backbuffer)
        const RenderSceneSnapshot* scene_snapshot;
        const CullingResults* culling_results;
        rendergraph::RGResourceHandle culling_dependency;
    };

    // Import the GPU draw pipeline's final output texture
    // The GPU pipeline manages its own render pass and output texture
    auto gpuFinalOutput = gpuDrawPipeline_->GetFinalOutputTexture();
    if (gpuFinalOutput == rhi::handles::INVALID_RESOURCE) {
        std::cerr << "[BuildRenderGraph] ERROR: GPU draw pipeline has no valid output texture!" << std::endl;
        return;
    }

    auto gpuOutputHandle = graph.ImportResource("GPUFinalOutput", gpuFinalOutput);
    std::cout << "[BuildRenderGraph] Imported GPU pipeline's final output texture" << std::endl;

    // Scene Render Pass - Execute GPU pipeline which manages its own render pass
    graph.AddPass<SceneRenderPassData>("SceneRender",
        graphics::rendergraph::RGPassType::Graphics,
        graphics::rendergraph::RGPassCategory::Main,
        [this, gpuOutputHandle, &cullingData](SceneRenderPassData& data, graphics::rendergraph::RenderGraphBuilder& builder) {
            std::cout << "[SceneRender] SETUP: Configuring GPU pipeline execution" << std::endl;

            // Store pointers to scene data for use in execute phase
            data.scene_snapshot = &sceneSnapshot_;
            data.culling_results = &cullingPipeline_->GetResults();

            // Import GPU pipeline's output as an external resource
            // The GPU pipeline manages its own render pass internally
            // CHANGED: Use Write instead of Read to ensure this pass is not culled (since FinalBlit reads it)
            data.gpu_output = builder.Write(gpuOutputHandle, rhi::ResourceState::RenderTarget);
            
            // Establish dependency on culling pass
            data.culling_dependency = builder.Read(cullingData.indirect_args_buffer, rhi::ResourceState::IndirectArgument);

            std::cout << "[SceneRender] SETUP: GPU pipeline will manage its own render pass" << std::endl;
        },
        [this](const SceneRenderPassData& data, graphics::rendergraph::RenderGraphContext& context) {
            std::cout << "[DEBUG] SceneRender PASS - Executing GPU pipeline..." << std::endl;
            auto cmd = context.cmdBuffer;

            // Execute the complete GPU-driven draw pipeline
            // The pipeline handles its own render pass begin/end internally
            if (!gpuDrawPipeline_->Execute(cmd, *data.scene_snapshot, cameraView_, cameraProj_, *data.culling_results, frameCount_)) {
                std::cerr << "[DEBUG] GPU-driven draw pipeline failed!" << std::endl;
            } else {
                std::cout << "[DEBUG] GPU-driven draw pipeline completed successfully!" << std::endl;
            }

            // Log rendering statistics
            if (frameCount_ % 60 == 0 && !sceneMeshes_.empty()) {
                const auto& cullingResults = cullingPipeline_->GetResults();
                const auto& drawResults = gpuDrawPipeline_->GetResults();

                std::cout << "[GPU Driven Rendering Stats]" << std::endl;
                std::cout << "  Loaded meshes: " << sceneMeshes_.size() << std::endl;
                std::cout << "  Scene instances: " << sceneSnapshot_.GetInstanceCount() << std::endl;
                std::cout << "  Visible clusters: " << cullingResults.visible_cluster_count << std::endl;
                std::cout << "  GPU Draw calls: " << drawResults.total_draw_calls << std::endl;
                std::cout << "  Clusters rendered: " << drawResults.total_clusters_rendered << std::endl;
                std::cout << "  Bin count: " << drawResults.bin_count << std::endl;
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
            std::cout << "[FinalBlit] SETUP: Configuring final blit with GPU pipeline dependency" << std::endl;

            // CRITICAL: Read from GPU pipeline's final output
            // This establishes the dependency: FinalBlit depends on SceneRender
            data.input = builder.Read(gpuOutputHandle, rhi::ResourceState::ShaderResource);
            std::cout << "[FinalBlit] SETUP: Reading from GPUFinalOutput (GPU pipeline's output)" << std::endl;

            data.output = builder.Write(backBufferHandle, rhi::ResourceState::RenderTarget);
            std::cout << "[FinalBlit] SETUP: Writing to backbuffer" << std::endl;

            graphics::rendergraph::RGRenderPassDesc rpDesc;
            rpDesc.colors.push_back({
                .texture = data.output,
                .loadOp = rhi::LoadAction::DontCare,  // Don't clear - we're overwriting everything
                .storeOp = rhi::StoreAction::Store,
                .clearColor = { primal::math::v4{0,0,0,1} }
            });
            builder.DeclareRenderPass(rpDesc);

            std::cout << "[FinalBlit] SETUP: Pass configured with GPU pipeline dependency" << std::endl;
        },
        [this](const BlitPassData& data, graphics::rendergraph::RenderGraphContext& context) {
            auto cmd = context.cmdBuffer;

            std::cout << "[DEBUG] Final blit pass executing - using shader-based fullscreen quad" << std::endl;

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

            std::cout << "[DEBUG] Shader-based blit completed successfully" << std::endl;
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
    static u32 lastFrameCount = 0;
    if (frameCount_ - lastFrameCount >= 300) {
        lastFrameCount = frameCount_;

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