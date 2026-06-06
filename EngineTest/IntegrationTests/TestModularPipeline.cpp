#include "TestModularPipeline.h"
#include "Engine/Common/CommonHeaders.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalDevice.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalMath.h"
#include "Engine/Graphics/RHI/Core/RHIMath.h"
#include "Engine/Graphics/Nanite/GPUDrivenDrawPipeline.h"
#include "Engine/Graphics/RHI/Core/RHIDevice.h"
#include "Engine/Utilities/IOStream.h"
#include "stb_image.h"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <algorithm>

using namespace primal::graphics;
using namespace primal::graphics::rhi;
using namespace primal::graphics::rhi::math;
using namespace primal::graphics::nanite;

#ifdef TEST_MODULAR_PIPELINE
Engine_Test::Engine_Test()
    : primal::test::RenderTestRunner(std::make_unique<TestModularPipeline>())
{}
#endif

// ============================================================================
// Texture loading helpers (from TestNaniteStreamingPipeline)
// ============================================================================
namespace {
    ResourceHandle CreateTextureFromData(RHIDeviceBase* device, uint32_t width, uint32_t height, const unsigned char* data, bool isSRGB) {
        DataFormat format = isSRGB ? DataFormat::RGBA8_sRGB : DataFormat::RGBA8_UNorm;
        uint32_t row_pitch = width * 4;
        uint32_t slice_pitch = height * row_pitch;
        size_t blob_size = (6 * sizeof(uint32_t)) + (2 * sizeof(uint32_t) + slice_pitch);

        std::vector<uint8_t> blob(blob_size);
        primal::utl::blob_stream_writer writer(blob.data(), blob.size());

        writer.write((uint32_t)width);
        writer.write((uint32_t)height);
        writer.write((uint32_t)1);
        writer.write((uint32_t)0);
        writer.write((uint32_t)1);
        writer.write((uint32_t)format);
        writer.write(row_pitch);
        writer.write(slice_pitch);
        writer.write(data, slice_pitch);

        primal::id::id_type id = primal::content::create_resource(blob.data(), primal::content::asset_type::texture);
        if (primal::id::is_valid(id)) {
            return primal::content::get_rhi_texture_handle(id);
        }
        return handles::INVALID_RESOURCE;
    }

    ResourceHandle LoadTextureFromFile(RHIDeviceBase* device, const std::string& path, bool isSRGB) {
        int width, height, channels;
        unsigned char* data = stbi_load(path.c_str(), &width, &height, &channels, 4);
        if (!data) return handles::INVALID_RESOURCE;

        constexpr int TARGET_SIZE = 1024;
        unsigned char* final_data = data;
        int final_width = width;
        int final_height = height;

        if (width != TARGET_SIZE || height != TARGET_SIZE) {
            // Simple bilinear resize
            float x_ratio = static_cast<float>(width - 1) / TARGET_SIZE;
            float y_ratio = static_cast<float>(height - 1) / TARGET_SIZE;
            auto* resized = new unsigned char[TARGET_SIZE * TARGET_SIZE * 4];

            for (int y = 0; y < TARGET_SIZE; ++y) {
                for (int x = 0; x < TARGET_SIZE; ++x) {
                    int sx = std::min(static_cast<int>(x * x_ratio), width - 2);
                    int sy = std::min(static_cast<int>(y * y_ratio), height - 2);
                    memcpy(&resized[(y * TARGET_SIZE + x) * 4], &data[(sy * width + sx) * 4], 4);
                }
            }
            final_data = resized;
            final_width = TARGET_SIZE;
            final_height = TARGET_SIZE;
            stbi_image_free(data);
        }

        ResourceHandle handle = CreateTextureFromData(device, final_width, final_height, final_data, isSRGB);
        if (final_data != data) delete[] final_data;
        else stbi_image_free(data);
        return handle;
    }

    std::string ResolveTexturePath(const std::string& baseDir, const std::string& filename) {
        if (filename.empty()) return "";
        std::string cleanName = filename;
        std::replace(cleanName.begin(), cleanName.end(), '\\', '/');

        std::vector<std::string> basePaths = {
            baseDir,
            baseDir + "models/Sponza/",
            baseDir + "fbx_textures/"
        };
        for (const auto& base : basePaths) {
            std::string fullPath = base + cleanName;
            std::ifstream f(fullPath.c_str());
            if (f.good()) return fullPath;
        }
        return baseDir + cleanName;
    }

    primal::game_entity::entity create_game_entity(primal::math::v3 position = {}) {
        primal::transform::init_info transform_info{};
        memcpy(&transform_info.position[0], &position, sizeof(transform_info.position));
        primal::game_entity::entity_info entity_info{};
        entity_info.transform = &transform_info;
        return primal::game_entity::create(entity_info);
    }
}

// ============================================================================
// Shader file descriptions
// ============================================================================
namespace {
    const shader_file_info kDeferredVS     { "DeferredLighting.metal", "vertexMain",                    shader_type::vertex };
    const shader_file_info kDeferredPS     { "DeferredLighting.metal", "fragmentLighting_gpuDriven",     shader_type::pixel  };
    const shader_file_info kBlitVS         { "DeferredLighting.metal", "vertexMain",                    shader_type::vertex };
    const shader_file_info kBlitPS         { "DeferredLighting.metal", "fragmentBlit",                  shader_type::pixel  };
    const shader_file_info kShadowFilter   { "ShadowFilter.metal",     "shadow_filter_compute",          shader_type::compute };
    const shader_file_info kGIGather       { "DDGIGIGather.metal",     "ddgi_gi_gather",                 shader_type::compute };
    const shader_file_info kFusionIndirect { "DeferredLighting.metal", "fragmentFusionIndirect",         shader_type::pixel  };
    const shader_file_info kFusionComposite{ "DeferredLighting.metal", "fragmentFusion",                 shader_type::pixel  };
    const shader_file_info kSCCardRadiance { "Lumen/DDGICardRadianceAvg.metal",           "ddgi_card_radiance_avg",              shader_type::compute };
    const shader_file_info kSCProbeIrr     { "Lumen/DDGIProbeIrradianceFromCards.metal",  "ddgi_probe_irradiance_from_cards",    shader_type::compute };
}

// ============================================================================
// Initialization
// ============================================================================

bool TestModularPipeline::Initialize() {
    std::cout << "[TestModularPipeline] Initializing..." << std::endl;

    // Initialize subsystems needed for content loading
    primal::jobsystem::JobSystem::Initialize();
    primal::content::AsyncResourceLoader::Initialize();

    if (!InitializeDevice()) return false;
    if (!CompileShaders()) return false;
    if (!InitializePipeline()) return false;

    SetupCamera();

    std::cout << "[TestModularPipeline] Initialization complete." << std::endl;
    return true;
}

bool TestModularPipeline::InitializeDevice() {
    primal::platform::window_init_info winInfo{};
    winInfo.caption = "TestModularPipeline";
    winInfo.width = renderWidth_;
    winInfo.height = renderHeight_;
    window_ = primal::platform::create_window(&winInfo);
    if (!window_.is_valid()) {
        std::cerr << "[TestModularPipeline] Failed to create window!" << std::endl;
        return false;
    }

    DeviceDesc desc;
    desc.platform = RHIPlatform::Metal;
    desc.enableDebug = true;

    auto* metalDevice = new MetalDevice(desc);
    if (!metalDevice || !metalDevice->Initialize()) {
        std::cerr << "[TestModularPipeline] Failed to create Metal device" << std::endl;
        delete metalDevice;
        return false;
    }
    device_.reset(metalDevice);

    // Register device so content system can create textures via RHI
    u32 devId = rhi::g_deviceManager.RegisterDevice(device_.get());
    std::cout << "[TestModularPipeline] Registered device, id=" << devId
              << ", count=" << rhi::g_deviceManager.GetDeviceCount() << std::endl;

    RenderSystemInitInfo sysInfo;
    sysInfo.device = device_.get();
    sysInfo.window = window_.handle();
    sysInfo.width = renderWidth_;
    sysInfo.height = renderHeight_;

    if (!renderSystem_.Initialize(sysInfo)) {
        std::cerr << "[TestModularPipeline] Failed to initialize RenderSystem!" << std::endl;
        return false;
    }

    return true;
}

bool TestModularPipeline::CompileShaders() {
    std::string testShaderPath;
    auto cwd = std::filesystem::current_path();
    auto candidate = cwd / "shaders";
    if (std::filesystem::exists(candidate)) {
        testShaderPath = candidate.string() + "/";
    } else {
        testShaderPath = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/shaders/";
    }

    auto CompileOne = [&](const shader_file_info& info) -> ShaderHandle {
        primal::utl::vector<std::wstring> extraArgs;
        auto compiled = compile_shader(info, testShaderPath.c_str(), extraArgs);
        if (!compiled) {
            std::cerr << "[TestModularPipeline] Failed to compile " << info.file_name << ":" << info.function << std::endl;
            return handles::INVALID_SHADER;
        }

        u64 byteCodeSize = *reinterpret_cast<u64*>(compiled.get());
        const u8* byteCodePtr = compiled.get() + sizeof(u64) + 16;

        ShaderStage stage;
        switch (info.type) {
            case shader_type::vertex:    stage = ShaderStage::Vertex;   break;
            case shader_type::pixel:     stage = ShaderStage::Pixel;    break;
            case shader_type::compute:   stage = ShaderStage::Compute;  break;
            default:                     stage = ShaderStage::Compute;  break;
        }

        auto handle = device_->CreateShader(byteCodePtr, byteCodeSize, stage, info.function);
        std::string key = std::string(info.file_name) + ":" + info.function;
        shaderMap_[key] = handle;
        return handle;
    };

    auto Get = [&](const shader_file_info& info) -> ShaderHandle {
        std::string key = std::string(info.file_name) + ":" + info.function;
        auto it = shaderMap_.find(key);
        if (it != shaderMap_.end()) return it->second;
        return CompileOne(info);
    };

    bool ok = true;
    auto check = [&](ShaderHandle h, const char* name) {
        if (h == handles::INVALID_SHADER) {
            std::cerr << "[TestModularPipeline] Missing shader: " << name << std::endl;
            ok = false;
        }
    };

    auto dv = Get(kDeferredVS);       check(dv, "deferred_vs");
    auto dp = Get(kDeferredPS);       check(dp, "deferred_ps");
    auto bv = Get(kBlitVS);           check(bv, "blit_vs");
    auto bp = Get(kBlitPS);           check(bp, "blit_ps");
    auto sf = Get(kShadowFilter);     check(sf, "shadow_filter");
    auto gg = Get(kGIGather);         check(gg, "gi_gather");
    auto fi = Get(kFusionIndirect);   check(fi, "fusion_indirect");
    auto fc = Get(kFusionComposite);  check(fc, "fusion_composite");
    auto cr = Get(kSCCardRadiance);   check(cr, "sc_card_radiance");
    auto pi = Get(kSCProbeIrr);       check(pi, "sc_probe_irradiance");

    if (!ok) return false;

    StandardRenderPipeline::ShaderHandles handles;
    handles.deferred_vs         = dv;
    handles.deferred_ps         = dp;
    handles.blit_vs             = bv;
    handles.blit_ps             = bp;
    handles.shadow_filter       = sf;
    handles.gi_gather           = gg;
    handles.fusion_indirect_ps  = fi;
    handles.fusion_composite_ps = fc;
    handles.sc_card_radiance    = cr;
    handles.sc_probe_irradiance = pi;
    shaderHandles_ = handles;
    return true;
}

bool TestModularPipeline::InitializePipeline() {
    pipeline_ = std::make_unique<StandardRenderPipeline>();
    if (!pipeline_->Initialize(device_.get())) {
        std::cerr << "[TestModularPipeline] Failed to initialize StandardRenderPipeline" << std::endl;
        return false;
    }

    pipeline_->SetOutputResource(handles::INVALID_RESOURCE, {});
    pipeline_->SetViewportSize(renderWidth_, renderHeight_);
    pipeline_->SetShaderHandles(shaderHandles_);

    // Configure Lumen — Medium preset: SSGI + SSAO + DDGI + PCF shadows
    // Probe grid matches TestNaniteStreamingPipeline defaults: 16x8x16 = 2048 probes, 4.0 spacing
    // Coverage: 60x28x60 units — large enough for Sponza (~30 units)
    lumen::LumenConfig lumenConfig;
    lumenConfig.quality = lumen::LumenQualityPreset::Medium;
    pipeline_->SetLumenConfig(lumenConfig);
    // Enable SPGI + Surface Cache (matching TestNaniteStreamingPipeline which initializes all passes)
    pipeline_->SetQualityOverride(true /*enable_screen_probes*/, true /*enable_surface_cache*/);

    // Enable Froxel Fog
    {
        auto settings = pipeline_->GetSettings();
        settings.quality.enable_froxel_fog = true;
        pipeline_->UpdateSettings(settings);
    }

    // Load Sponza scene (populates scene_ and wires materials to GPUDrivenDrawPipeline)
    if (!LoadSponzaScene()) {
        std::cerr << "[TestModularPipeline] Failed to load Sponza scene!" << std::endl;
        return false;
    }

    return true;
}

void TestModularPipeline::SetupCamera() {
    camera_.Initialize({0.0f, 5.0f, 0.0f}, {0.0f, 0.0f, 0.0f});
    camera_.SetSpeed(10.0f, 0.1f);
}

// ============================================================================
// Scene Loading
// ============================================================================

bool TestModularPipeline::LoadMaterialTextures() {
    std::string assetBaseDir = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/assets/";

    // Create 1024x1024 white fallback texture
    ResourceHandle whiteTexture;
    {
        constexpr u32 WHITE_SIZE = 1024;
        std::vector<u32> whiteData(WHITE_SIZE * WHITE_SIZE, 0xFFFFFFFF);
        whiteTexture = CreateTextureFromData(device_.get(), WHITE_SIZE, WHITE_SIZE,
            reinterpret_cast<unsigned char*>(whiteData.data()), true);
        if (whiteTexture == handles::INVALID_RESOURCE) {
            std::cerr << "[TestModularPipeline] Failed to create white texture" << std::endl;
            return false;
        }
    }

    std::unordered_map<std::string, ResourceHandle> textureCache;

    for (auto& meshInfo : sceneMeshes_) {
        if (!meshInfo.materialInstance) continue;

        // Load diffuse
        if (!meshInfo.diffuseTexturePath.empty()) {
            std::string fullPath = ResolveTexturePath(assetBaseDir, meshInfo.diffuseTexturePath);
            if (!fullPath.empty()) {
                auto it = textureCache.find(fullPath);
                ResourceHandle texture;
                if (it != textureCache.end()) {
                    texture = it->second;
                } else {
                    texture = LoadTextureFromFile(device_.get(), fullPath, true);
                    if (texture != handles::INVALID_RESOURCE) {
                        textureCache[fullPath] = texture;
                    } else {
                        texture = whiteTexture;
                    }
                }
                meshInfo.materialInstance->SetTexture(0, texture);
            }
        } else {
            meshInfo.materialInstance->SetTexture(0, whiteTexture);
        }

        // Infer normal from diffuse naming
        if (meshInfo.normalTexturePath.empty() && !meshInfo.diffuseTexturePath.empty()) {
            const std::string& diff = meshInfo.diffuseTexturePath;
            std::string inferred;
            if (diff.find("_diffuse.") != std::string::npos) {
                inferred = diff;
                inferred.replace(diff.find("_diffuse."), 9, "_normal.");
            } else if (diff.find("_Diff.") != std::string::npos) {
                inferred = diff;
                inferred.replace(diff.find("_Diff."), 5, "_Normal.");
            }
            if (!inferred.empty()) {
                std::string fullPath = ResolveTexturePath(assetBaseDir, inferred);
                std::ifstream testFile(fullPath);
                if (testFile.good()) meshInfo.normalTexturePath = inferred;
            }
        }

        // Load normal
        if (!meshInfo.normalTexturePath.empty()) {
            std::string fullPath = ResolveTexturePath(assetBaseDir, meshInfo.normalTexturePath);
            if (!fullPath.empty()) {
                auto it = textureCache.find(fullPath);
                ResourceHandle texture;
                if (it != textureCache.end()) {
                    texture = it->second;
                } else {
                    texture = LoadTextureFromFile(device_.get(), fullPath, false);
                    if (texture != handles::INVALID_RESOURCE)
                        textureCache[fullPath] = texture;
                }
                if (texture != handles::INVALID_RESOURCE)
                    meshInfo.materialInstance->SetTexture(1, texture);
            }
        }

        // ORM — white fallback
        meshInfo.materialInstance->SetTexture(2, whiteTexture);
    }

    return true;
}

bool TestModularPipeline::LoadSponzaScene() {
    std::string baseDir = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/assets/";
    std::string modelPath = baseDir + "Sponza_process_rebuild.model";

    // Load .model file
    {
        std::ifstream file(modelPath, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            std::cerr << "[TestModularPipeline] Failed to open: " << modelPath << std::endl;
            return false;
        }
        std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);
        std::vector<char> buffer(size);
        if (!file.read(buffer.data(), size)) {
            std::cerr << "[TestModularPipeline] Failed to read: " << modelPath << std::endl;
            return false;
        }

        primal::graphics::SceneDataAdapter adapter;
        sceneMeshes_ = adapter.LoadRenderItemData(device_.get(), buffer.data(), (uint32_t)buffer.size());
        if (sceneMeshes_.empty()) {
            std::cerr << "[TestModularPipeline] No meshes loaded!" << std::endl;
            return false;
        }
    }

    std::cout << "[TestModularPipeline] Loaded " << sceneMeshes_.size() << " meshes" << std::endl;

    // Load textures before material registration
    if (!LoadMaterialTextures()) {
        std::cerr << "[TestModularPipeline] Warning: texture loading failed" << std::endl;
    }

    // Create GPU material registry
    gpuMaterialRegistry_ = std::make_unique<GPUMaterialRegistry>();
    u32 registeredCount = 0;
    for (auto& meshInfo : sceneMeshes_) {
        if (meshInfo.materialInstance) {
            auto matID = gpuMaterialRegistry_->RegisterMaterial(meshInfo.materialInstance.get());
            if (matID != GPUMaterialRegistry::INVALID_MATERIAL_ID) {
                meshInfo.gpuMaterialId = matID;
                registeredCount++;
            }
        }
    }

    // Build and upload material data to GPU
    auto buildJob = gpuMaterialRegistry_->BuildAsync(device_.get());
    buildJob.Wait();
    if (!gpuMaterialRegistry_->UploadToGPU(device_.get())) {
        std::cerr << "[TestModularPipeline] Failed to upload material data" << std::endl;
        return false;
    }

    // Wire material buffer + texture arrays to GPUDrivenDrawPipeline singleton
    auto& gpuDraw = GPUDrivenDrawPipeline::Get();
    auto materialBuffer = gpuMaterialRegistry_->GetMaterialDataBuffer();
    if (materialBuffer != handles::INVALID_RESOURCE) {
        gpuDraw.SetMaterialDataBuffer(materialBuffer);
    }

    auto albedoArray = gpuMaterialRegistry_->GetAlbedoTextureArray();
    auto normalArray = gpuMaterialRegistry_->GetNormalTextureArray();
    auto ormArray = gpuMaterialRegistry_->GetORMTextureArray();

    if (albedoArray != handles::INVALID_RESOURCE &&
        normalArray != handles::INVALID_RESOURCE &&
        ormArray != handles::INVALID_RESOURCE) {

        SamplerDesc samplerDesc{};
        samplerDesc.minFilter = FilterMode::Linear;
        samplerDesc.magFilter = FilterMode::Linear;
        samplerDesc.mipFilter = FilterMode::Linear;
        samplerDesc.addressU = TextureAddressMode::Wrap;
        samplerDesc.addressV = TextureAddressMode::Wrap;
        samplerDesc.addressW = TextureAddressMode::Wrap;
        samplerDesc.maxLod = 100.0f;
        auto sampler = device_->CreateSampler(samplerDesc);

        if (sampler != handles::INVALID_SAMPLER) {
            gpuDraw.SetTextureArrays(albedoArray, normalArray, ormArray, sampler);
        }
    }

    // Create game entities, cluster components, and render proxies
    for (const auto& meshInfo : sceneMeshes_) {
        if (!meshInfo.mesh || meshInfo.meshEntityId == primal::id::invalid_id) continue;

        auto entity = create_game_entity();
        auto entityId = entity.get_id();

        primal::cluster::init_info clusterInit{};
        clusterInit.geometry_content_id = meshInfo.meshEntityId;
        primal::cluster::create(clusterInit, entity);

        auto proxy = RenderProxy::Create(entityId, meshInfo.meshEntityId, meshInfo.gpuMaterialId);
        proxy.transform = MatrixIdentity();
        scene_.AddProxy(proxy);
    }

    std::cout << "[TestModularPipeline] Added " << scene_.GetProxies().size()
              << " proxies, " << registeredCount << " materials" << std::endl;

    // Register scene meshes with Surface Cache CardGenerator
    // Without this, SC passes read from uninitialized atlas textures,
    // producing garbage that propagates through DDGI → GIGather → FusionComposite → flickering
    if (pipeline_) {
        auto* scPass = pipeline_->GetSurfaceCachePass();
        if (scPass && scPass->IsInitialized()) {
            auto& cardGen = scPass->GetCardGenerator();
            u32 scRegistered = 0;
            for (u32 i = 0; i < sceneMeshes_.size(); ++i) {
                const auto& meshInfo = sceneMeshes_[i];
                if (!meshInfo.mesh) continue;

                const auto& localAABB = meshInfo.mesh->GetLocalAABB();
                m4x4 worldMat = MatrixIdentity(); // all proxies use identity transform

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
                scRegistered++;
            }
            cardGen.RebuildCardAllocation();
            std::cout << "[TestModularPipeline] SurfaceCache registered " << scRegistered << " cards" << std::endl;
        }
    }

    return true;
}

// ============================================================================
// Run
// ============================================================================

void TestModularPipeline::Run() {
    if (!pipeline_ || isShutdown_) return;

    // Update camera
    camera_.Update(0.016f);
    cameraView_ = camera_.GetViewMatrix();
    float fov = 60.0f * primal::graphics::rhi::math::constants::DEG_TO_RAD;
    float aspect = (float)renderWidth_ / (float)renderHeight_;
    cameraProj_ = CreatePerspectiveMatrix(fov, aspect, 0.1f, 1000.0f);

    view_.SetViewMatrix(cameraView_);
    view_.SetProjectionMatrix(cameraProj_);
    ViewportDesc viewport;
    viewport.size.x = renderWidth_;
    viewport.size.y = renderHeight_;
    view_.SetViewport(viewport);
    view_.UpdateFrustum();

    ResourceHandle backBuffer;
    SyncHandle signalFence;
    if (!renderSystem_.BeginFrame(backBuffer, signalFence)) return;

    static u32 diagBB = 0;
    if (diagBB < 5) {
        std::cout << "[Diag] backBuffer=" << backBuffer << " cbIdx=" << renderSystem_.GetCurrentFrameIndex() << std::endl;
        diagBB++;
    }

    // Use RenderSystem's pre-allocated triple-buffered command buffer
    // (matches TestNaniteStreamingPipeline's working pattern exactly).
    // StandardRenderPipeline::RenderWithCommandBuffer does NOT create/destroy
    // CBs — the caller manages the lifecycle via Reset/Begin/End/Submit.
    auto* cmd = renderSystem_.GetCurrentCommandBuffer();
    auto cmdHandle = renderSystem_.GetCurrentCommandBufferHandle();
    u32 bufferIndex = renderSystem_.GetCurrentFrameIndex();

    cmd->Reset();
    cmd->Begin();

    pipeline_->RenderWithCommandBuffer(
        scene_, view_, backBuffer, renderSystem_.GetBackBufferDesc(),
        cmd, bufferIndex, cmdHandle, signalFence);

    cmd->End();

    rhi::QueueSubmitInfo submitInfo{};
    submitInfo.cmdBuffer = cmdHandle;
    submitInfo.signalFence = signalFence;
    device_->Submit(submitInfo);

    renderSystem_.EndFrame();

    frameCount_++;
    if (frameCount_ <= 3 || frameCount_ % 60 == 0) {
        const auto& stats = pipeline_->GetStats();
        std::cout << "[TestModularPipeline] Frame " << frameCount_
                  << ": CPU=" << stats.cpuFrameTimeMs << "ms"
                  << ", Draws=" << stats.drawCallCount << std::endl;
    }
}

// ============================================================================
// Resize & Shutdown
// ============================================================================

void TestModularPipeline::Resize(uint32_t width, uint32_t height) {
    renderWidth_ = width;
    renderHeight_ = height;

    if (pipeline_) {
        pipeline_->SetViewportSize(width, height);
    }

    ViewportDesc viewport;
    viewport.size.x = width;
    viewport.size.y = height;
    view_.SetViewport(viewport);

    float fov = 60.0f * primal::graphics::rhi::math::constants::DEG_TO_RAD;
    float aspect = (float)width / (float)height;
    cameraProj_ = CreatePerspectiveMatrix(fov, aspect, 0.1f, 1000.0f);
    view_.SetProjectionMatrix(cameraProj_);
}

void TestModularPipeline::Shutdown() {
    if (isShutdown_) return;
    isShutdown_ = true;

    std::cout << "[TestModularPipeline] Shutting down after " << frameCount_ << " frames..." << std::endl;

    if (gpuMaterialRegistry_ && device_) {
        gpuMaterialRegistry_->Shutdown(device_.get());
    }
    gpuMaterialRegistry_.reset();
    pipeline_.reset();
    renderSystem_.Shutdown();

    if (window_.is_valid()) {
        primal::platform::remove_window(window_.get_id());
    }

    device_.reset();
    primal::content::shutdown();
    primal::content::AsyncResourceLoader::Shutdown();
    primal::jobsystem::JobSystem::Shutdown();
}
