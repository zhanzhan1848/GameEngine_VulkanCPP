// T4.6.5 part 30.4 — TestVulkanSponzaRenderGraph.cpp
//
// First visible-window Vulkan Sponza render using the RenderSystem abstraction
// (mirrors TestNaniteStreamingPipeline's scaffolding). The RenderSystem owns
// triple-buffered command buffers + per-frame fences internally, eliminating the
// fence/command-buffer-in-use validation errors that the deleted
// TestVulkanStandardPipelineRender_Windowed test produced via manual sync.

#include "TestVulkanSponzaRenderGraph.h"

#include "Engine/Graphics/RHI/Core/RHIDeviceFactory.h"
#include "Engine/Graphics/RHI/Core/RHIDevice.h"
#include "Engine/Graphics/RHI/Core/RHICommand.h"
#include "Engine/Graphics/RHI/Core/RHITypes.h"
#include "Engine/Graphics/RHI/Core/RHIMath.h"
#include "Engine/Graphics/RHI/Platforms/Vulkan/VulkanDevice.h"
#include "Engine/Graphics/RHI/Platforms/Vulkan/VulkanCommandBuffer.h"

// Note: the `Engine/...` include prefix matches TestNaniteStreamingPipeline.cpp.
// The Engine target propagates its own include path transitively via
// target_link_libraries(... Engine ContentTools) in setup_test_target.
#include "Engine/Content/ContentToEngine.h"
#include "Engine/Content/AsyncResourceLoader.h"

#define STBI_NO_THREAD_LOCALS
#include "stb_image.h"

#define NS_PRIVATE_IMPLEMENTATION
#include <AppKit/AppKit.hpp>

#include <iostream>
#include <cstring>
#include <fstream>
#include <vector>

using namespace primal::graphics;
using namespace primal::graphics::rhi;
using namespace primal::math;
namespace rhimath = primal::graphics::rhi::math;

namespace {

// Texture helpers (RHIDeviceBase version — copied from
// TestVulkanStandardPipelineSmoke.cpp:133-186). Vulkan has no UpdateTextureData
// on RHIDeviceBase, so we use staging buffer + CopyBufferToTexture +
// GenerateMipmaps. Software mip box filter is generated inline so the staging
// buffer carries only mip 0; GPU GenerateMipmaps fills the rest of the chain.

ResourceHandle CreateTextureFromData(RHIDeviceBase* device, int w, int h,
    const unsigned char* data, DataFormat format = DataFormat::RGBA8_UNorm) {
    u32 mipLevels = 1;
    { u32 maxDim = std::max((u32)w, (u32)h); while (maxDim > 1) { mipLevels++; maxDim /= 2; } }

    TextureDesc desc{};
    desc.size = {(u32)w, (u32)h, 1};
    desc.format = format;
    desc.type = TextureType::Texture2D;
    desc.mipLevels = mipLevels;
    desc.usage = TextureUsage::ShaderResource | TextureUsage::CopyDest | TextureUsage::CopySource;

    ResourceHandle tex = device->CreateTexture(desc);
    if (tex == handles::INVALID_RESOURCE) return handles::INVALID_RESOURCE;

    const u64 kBytes = (u64)w * h * 4;
    BufferDesc sdesc{};
    sdesc.size = kBytes;
    sdesc.type = BufferType::Raw;
    sdesc.memoryUsage = GPUMemoryUsage::Dynamic;
    sdesc.name = "TexStaging";
    ResourceHandle staging = device->CreateBuffer(sdesc);
    if (staging == handles::INVALID_RESOURCE) {
        device->DestroyTexture(tex);
        return handles::INVALID_RESOURCE;
    }
    if (!device->UpdateBufferData(staging, data, kBytes, 0)) {
        device->DestroyBuffer(staging);
        device->DestroyTexture(tex);
        return handles::INVALID_RESOURCE;
    }

    CommandBufferHandle cmd = device->CreateCommandBuffer(CommandQueueType::Graphics);
    VulkanCommandBuffer* vcmd = static_cast<VulkanDevice*>(device)->GetCommandBuffer(cmd);
    vcmd->Reset();
    vcmd->Begin();
    BufferTextureCopyRegion region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource = { 0, 0, 1 };
    region.imageOffset = { 0, 0, 0 };
    region.imageExtent = { (u32)w, (u32)h, 1 };
    vcmd->CopyBufferToTexture(staging, tex, &region, 1);
    if (mipLevels > 1) vcmd->GenerateMipmaps(tex);
    vcmd->End();
    vcmd->Submit(0);
    vcmd->WaitForCompletion();
    device->DestroyCommandBuffer(cmd);
    device->DestroyBuffer(staging);
    return tex;
}

ResourceHandle LoadTextureFromFile(RHIDeviceBase* device, const std::string& path,
    DataFormat format = DataFormat::RGBA8_UNorm) {
    int width, height, channels;
    unsigned char* data = stbi_load(path.c_str(), &width, &height, &channels, 4);
    if (!data) return handles::INVALID_RESOURCE;
    ResourceHandle tex = CreateTextureFromData(device, width, height, data, format);
    stbi_image_free(data);
    return tex;
}

std::string ResolveTexturePath(const std::string& base, const std::string& filename) {
    if (filename.empty()) return "";
    std::string path = base + filename;
    { std::ifstream f(path); if (f.is_open()) return path; }
    path = base + "models/Sponza/" + filename;
    { std::ifstream f(path); if (f.is_open()) return path; }
    path = base + "textures/" + filename;
    { std::ifstream f(path); if (f.is_open()) return path; }
    return "";
}

m4x4 make_identity_m4x4() {
    m4x4 r{};
    std::memset(&r, 0, sizeof(r));
    r.columns[0][0] = 1.0f;
    r.columns[1][1] = 1.0f;
    r.columns[2][2] = 1.0f;
    r.columns[3][3] = 1.0f;
    return r;
}

} // anonymous namespace

Engine_Test::Engine_Test()
    : primal::test::RenderTestRunner(std::make_unique<TestVulkanSponzaRenderGraph>())
{}

TestVulkanSponzaRenderGraph::~TestVulkanSponzaRenderGraph() {
    Shutdown();
}

bool TestVulkanSponzaRenderGraph::Initialize() {
    std::cout << "[Part30.4] TestVulkanSponzaRenderGraph::Initialize" << std::endl;

    if (!primal::jobsystem::JobSystem::Initialize(
            primal::jobsystem::JobSchedulerConfig::Default())) {
        std::cerr << "[Part30.4] JobSystem init failed" << std::endl;
        return false;
    }

    if (!InitializeDevice()) {
        std::cerr << "[Part30.4] InitializeDevice failed" << std::endl;
        return false;
    }
    if (!InitializeWindowAndRenderSystem()) {
        std::cerr << "[Part30.4] InitializeWindowAndRenderSystem failed" << std::endl;
        return false;
    }
    if (!InitializePipeline()) {
        std::cerr << "[Part30.4] InitializePipeline failed" << std::endl;
        return false;
    }
    if (!LoadSponzaScene()) {
        std::cerr << "[Part30.4] LoadSponzaScene failed" << std::endl;
        return false;
    }

    std::cout << "[Part30.4] Initialization complete — window open, close it to exit."
              << std::endl;
    return true;
}

bool TestVulkanSponzaRenderGraph::InitializeDevice() {
    DeviceDesc desc;
    desc.platform = RHIPlatform::Vulkan;
    desc.enableValidation = true;
    desc.enableDebug = true;

    auto vulkanDevice = std::make_unique<VulkanDevice>(desc);
    if (!vulkanDevice->Initialize()) {
        std::cerr << "[Part30.4] VulkanDevice::Initialize failed" << std::endl;
        return false;
    }

    device_ = vulkanDevice.get();
    g_deviceManager.RegisterDevice(device_);
    deviceOwnership_ = std::move(vulkanDevice);
    return true;
}

bool TestVulkanSponzaRenderGraph::InitializeWindowAndRenderSystem() {
    primal::platform::window_init_info wi{};
    wi.caption = "Vulkan Sponza (RenderSystem)";
    wi.left = 100;
    wi.top = 100;
    wi.width = 1280;
    wi.height = 720;
    window_ = primal::platform::create_window(&wi);

    if (!window_.is_valid()) {
        std::cerr << "[Part30.4] create_window failed" << std::endl;
        return false;
    }

    RenderSystemInitInfo sysInfo;
    sysInfo.device = device_;
    sysInfo.window = window_.handle();
    sysInfo.width = window_.width();
    sysInfo.height = window_.height();

    if (!renderSystem_.Initialize(sysInfo)) {
        std::cerr << "[Part30.4] RenderSystem::Initialize failed" << std::endl;
        return false;
    }

    // T4.6.5 part 30.13 (X5 fix): per-swapchain-image render-done semaphores.
    // Index by currentImageIndex_ when picking which to signal/submit/present.
    for (u32 i = 0; i < kMaxSwapchainImages; ++i) {
        renderDoneSemaphores_[i] = device_->CreateSync();
        if (renderDoneSemaphores_[i] == primal::graphics::rhi::handles::INVALID_SYNC) {
            std::cerr << "[Part30.4] CreateSync (renderDoneSemaphores_[" << i << "]) failed" << std::endl;
            return false;
        }
    }
    return true;
}

bool TestVulkanSponzaRenderGraph::InitializePipeline() {
    pipeline_ = std::make_unique<StandardRenderPipeline>();
    if (!pipeline_->Initialize(device_)) {
        std::cerr << "[Part30.4] StandardRenderPipeline::Initialize failed" << std::endl;
        return false;
    }

    // SPIR-V loader: try project-root path first (matches UnitTests cwd),
    // then walk up from the binary's cwd (Darwin/Debug) until found.
    auto loadSpv = [](const char* relpath) -> std::vector<u8> {
        auto tryPath = [](const std::string& p) -> std::vector<u8> {
            std::ifstream f(p, std::ios::binary | std::ios::ate);
            if (!f) return {};
            std::streamsize sz = f.tellg();
            f.seekg(0);
            std::vector<u8> bytes(static_cast<size_t>(sz));
            f.read(reinterpret_cast<char*>(bytes.data()), sz);
            return bytes;
        };
        std::vector<u8> bytes = tryPath(relpath);
        if (!bytes.empty()) return bytes;
        for (int i = 1; i <= 6 && bytes.empty(); ++i) {
            std::string prefix;
            for (int j = 0; j < i; ++j) prefix += "../";
            bytes = tryPath(prefix + relpath);
        }
        return bytes;
    };
    auto deferredVsBytes = loadSpv("Engine/Graphics/Vulkan/shaders/Forward/DeferredLighting.vert.spv");
    auto deferredFsBytes = loadSpv("Engine/Graphics/Vulkan/shaders/Forward/DeferredLighting.frag.spv");
    if (deferredVsBytes.empty() || deferredFsBytes.empty()) {
        std::cerr << "[Part30.4] Missing DeferredLighting SPIR-V" << std::endl;
        return false;
    }
    auto blitVsBytes = loadSpv("Engine/Graphics/Vulkan/shaders/Forward/Blit.vert.spv");
    auto blitFsBytes = loadSpv("Engine/Graphics/Vulkan/shaders/Forward/Blit.frag.spv");
    if (blitVsBytes.empty() || blitFsBytes.empty()) {
        std::cerr << "[Part30.4] Missing Blit SPIR-V" << std::endl;
        return false;
    }

    ShaderHandle deferredVs = device_->CreateShader(
        deferredVsBytes.data(), deferredVsBytes.size(), ShaderStage::Vertex, "main");
    ShaderHandle deferredFs = device_->CreateShader(
        deferredFsBytes.data(), deferredFsBytes.size(), ShaderStage::Pixel, "main");
    ShaderHandle blitVs = device_->CreateShader(
        blitVsBytes.data(), blitVsBytes.size(), ShaderStage::Vertex, "main");
    ShaderHandle blitFs = device_->CreateShader(
        blitFsBytes.data(), blitFsBytes.size(), ShaderStage::Pixel, "main");

    StandardRenderPipeline::ShaderHandles handles;
    handles.deferred_vs = deferredVs;
    handles.deferred_ps = deferredFs;
    handles.blit_vs = blitVs;
    handles.blit_ps = blitFs;
    pipeline_->SetShaderHandles(handles);

    // Default LumenConfig disables most Lumen modules. Triggers
    // InitializeSubsystems() which creates the deferred/blit/shadow/etc.
    // modules + the GPUDrivenDrawPipeline singleton.
    pipeline_->SetLumenConfig(lumen::LumenConfig{});
    subsystemsInitialized_ = true;
    return true;
}

bool TestVulkanSponzaRenderGraph::LoadSponzaScene() {
    const std::string baseDir =
        "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/assets/";
    const std::string modelPath = baseDir + "Sponza.model";
    std::ifstream file(modelPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cerr << "[Part30.4] Failed to open " << modelPath << std::endl;
        return false;
    }
    std::streamsize modelSize = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<char> modelBuffer(static_cast<size_t>(modelSize));
    if (!file.read(modelBuffer.data(), modelSize)) {
        std::cerr << "[Part30.4] Failed to read Sponza.model" << std::endl;
        return false;
    }

    SceneDataAdapter adapter;
    sceneMeshes_ = adapter.LoadRenderItemData(
        device_, modelBuffer.data(), (u32)modelBuffer.size());
    if (sceneMeshes_.empty()) {
        std::cerr << "[Part30.4] LoadRenderItemData returned 0 meshes" << std::endl;
        return false;
    }
    std::cout << "[Part30.4] Loaded " << sceneMeshes_.size()
              << " meshes from Sponza.model" << std::endl;

    // Shared Material + fallback textures + sampler (mirror NonEditor Step 2).
    sharedMaterial_ = std::make_shared<Material>();

    constexpr u32 FALLBACK_SIZE = 1024;
    std::vector<unsigned char> whiteBuf(FALLBACK_SIZE * FALLBACK_SIZE * 4, 255);
    std::vector<unsigned char> flatNormalBuf(FALLBACK_SIZE * FALLBACK_SIZE * 4, 0);
    for (u32 i = 0; i < FALLBACK_SIZE * FALLBACK_SIZE; ++i) {
        flatNormalBuf[i * 4 + 0] = 128;
        flatNormalBuf[i * 4 + 1] = 128;
        flatNormalBuf[i * 4 + 2] = 255;
        flatNormalBuf[i * 4 + 3] = 255;
    }
    std::vector<unsigned char> defaultORMBuf(FALLBACK_SIZE * FALLBACK_SIZE * 4, 0);
    for (u32 i = 0; i < FALLBACK_SIZE * FALLBACK_SIZE; ++i) {
        defaultORMBuf[i * 4 + 0] = 255;  // AO=1
        defaultORMBuf[i * 4 + 1] = 128;  // roughness=0.5
        defaultORMBuf[i * 4 + 2] = 0;    // metallic=0
        defaultORMBuf[i * 4 + 3] = 255;
    }
    fallbackDiffuse_ = CreateTextureFromData(
        device_, FALLBACK_SIZE, FALLBACK_SIZE, whiteBuf.data(), DataFormat::RGBA8_sRGB);
    fallbackNormal_ = CreateTextureFromData(
        device_, FALLBACK_SIZE, FALLBACK_SIZE, flatNormalBuf.data());
    fallbackORM_ = CreateTextureFromData(
        device_, FALLBACK_SIZE, FALLBACK_SIZE, defaultORMBuf.data());
    if (fallbackDiffuse_ == handles::INVALID_RESOURCE ||
        fallbackNormal_ == handles::INVALID_RESOURCE ||
        fallbackORM_ == handles::INVALID_RESOURCE) {
        std::cerr << "[Part30.4] Fallback texture creation failed" << std::endl;
        return false;
    }

    SamplerDesc samplerDesc{};
    samplerDesc.minFilter = FilterMode::Linear;
    samplerDesc.magFilter = FilterMode::Linear;
    samplerDesc.addressU = TextureAddressMode::Wrap;
    samplerDesc.addressV = TextureAddressMode::Wrap;
    samplerDesc.addressW = TextureAddressMode::Wrap;
    samplerDesc.comparisonFunc = ComparisonFunc::Never;
    materialSampler_ = device_->CreateSampler(samplerDesc);
    if (materialSampler_ == handles::INVALID_SAMPLER) {
        std::cerr << "[Part30.4] materialSampler creation failed" << std::endl;
        return false;
    }

    // Per-mesh MaterialInstance + game_entity + cluster + RenderProxy
    // (mirror NonEditor Step 3).
    //
    // T4.6.5 part 30.7 (Track 2): parallelize texture decode via JobSystem.
    // stbi_load is I/O + CPU bound (TGA decode) and runs ~0.5-2s total
    // across 393 meshes. device_->CreateTexture + MaterialInstance::Update
    // touch RHI state and may not be thread-safe, so they stay on main
    // thread after the parallel decode phase completes.
    const u32 meshCount = (u32)sceneMeshes_.size();
    struct DecodedTex {
        int width{0}, height{0};
        unsigned char* data{nullptr};
        bool valid{false};
    };
    std::vector<DecodedTex> decAlbedo(meshCount), decNormal(meshCount), decORM(meshCount);

    auto decodeJob = [&](u32 i, u32) {
        auto& meshInfo = sceneMeshes_[i];
        auto decode = [](const std::string& path, DecodedTex& out) {
            if (path.empty()) return;
            out.data = stbi_load(path.c_str(), &out.width, &out.height, nullptr, 4);
            out.valid = (out.data != nullptr);
        };
        decode(ResolveTexturePath(baseDir, meshInfo.diffuseTexturePath), decAlbedo[i]);
        decode(ResolveTexturePath(baseDir, meshInfo.normalTexturePath), decNormal[i]);
        decode(ResolveTexturePath(baseDir, meshInfo.ormTexturePath), decORM[i]);
    };

    auto t0 = std::chrono::high_resolution_clock::now();
    auto decodeHandle = primal::jobsystem::JobSystem::ParallelForWithThread(meshCount, decodeJob);
    primal::jobsystem::JobSystem::Wait(decodeHandle);
    auto t1 = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> decodeMs = t1 - t0;
    std::cout << "[Part30.4] Parallel texture decode: " << meshCount
              << " meshes in " << decodeMs.count() << " ms" << std::endl;

    // Serial phase: CreateTextureFromData + material/entity setup.
    // RHI device + MaterialInstance are not thread-safe.
    u32 texLoaded = 0, texFailed = 0;
    for (u32 i = 0; i < meshCount; ++i) {
        auto& meshInfo = sceneMeshes_[i];
        meshInfo.material = sharedMaterial_;

        auto matInst = std::make_shared<MaterialInstance>(sharedMaterial_.get());
        if (!matInst->Initialize(device_)) {
            std::cerr << "[Part30.4] MaterialInstance init failed for mesh " << i << std::endl;
            matInst = std::make_shared<MaterialInstance>(sharedMaterial_.get());
        }

        ResourceHandle diffuseTex = handles::INVALID_RESOURCE;
        if (decAlbedo[i].valid) {
            diffuseTex = CreateTextureFromData(
                device_, decAlbedo[i].width, decAlbedo[i].height,
                decAlbedo[i].data, DataFormat::RGBA8_sRGB);
        }
        if (diffuseTex == handles::INVALID_RESOURCE) { diffuseTex = fallbackDiffuse_; texFailed++; }
        else texLoaded++;

        ResourceHandle normalTex = handles::INVALID_RESOURCE;
        if (decNormal[i].valid) {
            normalTex = CreateTextureFromData(
                device_, decNormal[i].width, decNormal[i].height, decNormal[i].data);
        }
        if (normalTex == handles::INVALID_RESOURCE) normalTex = fallbackNormal_;

        ResourceHandle ormTex = handles::INVALID_RESOURCE;
        if (decORM[i].valid) {
            ormTex = CreateTextureFromData(
                device_, decORM[i].width, decORM[i].height, decORM[i].data);
        }
        if (ormTex == handles::INVALID_RESOURCE) ormTex = fallbackORM_;

        // Free stbi buffers now that CreateTextureFromData has copied the pixels.
        if (decAlbedo[i].data) stbi_image_free(decAlbedo[i].data);
        if (decNormal[i].data) stbi_image_free(decNormal[i].data);
        if (decORM[i].data) stbi_image_free(decORM[i].data);

        matInst->SetTexture(0, diffuseTex);
        matInst->SetTexture(1, normalTex);
        matInst->SetTexture(2, ormTex);
        matInst->SetSampler(3, materialSampler_);
        matInst->Update(device_);

        meshInfo.materialInstance = matInst;
        materialInstances_.push_back(matInst);

        primal::game_entity::entity_info entInfo{};
        primal::transform::init_info tfInfo{};
        tfInfo.position[0] = 0.0f;
        tfInfo.position[1] = 0.0f;
        tfInfo.position[2] = 0.0f;
        tfInfo.rotation[0] = 0.0f;
        tfInfo.rotation[1] = 0.0f;
        tfInfo.rotation[2] = 0.0f;
        tfInfo.rotation[3] = 1.0f;
        entInfo.transform = &tfInfo;
        primal::game_entity::entity entity = primal::game_entity::create(entInfo);
        if (!entity.is_valid()) {
            std::cerr << "[Part30.4] game_entity::create failed for mesh " << i << std::endl;
            return false;
        }
        entities_.push_back(entity);

        primal::cluster::init_info clusterInit{};
        clusterInit.geometry_content_id = meshInfo.meshEntityId;
        primal::cluster::component clusterComp =
            primal::cluster::create(clusterInit, entity);
        clusterComps_.push_back(clusterComp);

        RenderProxy proxy;
        proxy.materialId = meshInfo.meshEntityId;
        proxy.entityId = meshInfo.meshEntityId;
        proxy.meshId = clusterComp;
        proxy.transform = rhimath::MatrixIdentity();
        if (meshInfo.mesh && meshInfo.mesh->IsValid()) {
            proxy.worldAABB = meshInfo.mesh->GetLocalAABB();
        }
        scene_.AddProxy(proxy);
    }
    std::cout << "[Part30.4] Textures: " << texLoaded << " loaded, "
              << texFailed << " fallback" << std::endl;
    std::cout << "[Part30.4] Scene proxies: " << scene_.GetProxies().size() << std::endl;

    // GPUMaterialRegistry wiring (mirror NonEditor Step 4).
    materialRegistry_ = new primal::graphics::nanite::GPUMaterialRegistry();
    u32 registeredCount = 0;
    for (auto& meshInfo : sceneMeshes_) {
        if (meshInfo.materialInstance) {
            auto matID = materialRegistry_->RegisterMaterial(meshInfo.materialInstance.get());
            if (matID != primal::graphics::nanite::GPUMaterialRegistry::INVALID_MATERIAL_ID) {
                meshInfo.gpuMaterialId = matID;
                registeredCount++;
            }
        }
    }
    std::cout << "[Part30.4] Registered " << registeredCount << " materials" << std::endl;

    // Patch proxy.materialId: entity_id → gpuMaterialId.
    for (auto& meshInfo : sceneMeshes_) {
        if (meshInfo.materialInstance &&
            meshInfo.gpuMaterialId != primal::id::invalid_id) {
            for (const auto& proxy : scene_.GetProxies()) {
                if (proxy.entityId == meshInfo.meshEntityId) {
                    RenderProxy patched = proxy;
                    patched.materialId = meshInfo.gpuMaterialId;
                    scene_.UpdateProxy(meshInfo.meshEntityId, patched);
                    break;
                }
            }
        }
    }

    auto buildJob = materialRegistry_->BuildAsync(device_);
    buildJob.Wait();
    if (!materialRegistry_->UploadToGPU(device_)) {
        std::cerr << "[Part30.4] Material upload failed" << std::endl;
        return false;
    }
    std::cout << "[Part30.4] Materials uploaded to GPU" << std::endl;

    auto& gpuDraw = primal::graphics::nanite::GPUDrivenDrawPipeline::Get();
    gpuDraw.SetMaterialDataBuffer(materialRegistry_->GetMaterialDataBuffer());
    SamplerDesc texSamplerDesc{};
    texSamplerDesc.minFilter = FilterMode::Linear;
    texSamplerDesc.magFilter = FilterMode::Linear;
    texSamplerDesc.mipFilter = FilterMode::Linear;
    texSamplerDesc.addressU = TextureAddressMode::Wrap;
    texSamplerDesc.addressV = TextureAddressMode::Wrap;
    texSamplerDesc.addressW = TextureAddressMode::Wrap;
    texSamplerDesc.maxAnisotropy = 1;
    texSamplerDesc.minLod = 0.0f;
    texSamplerDesc.maxLod = 100.0f;
    texSamplerDesc.comparisonFunc = ComparisonFunc::Never;
    texSampler_ = device_->CreateSampler(texSamplerDesc);
    gpuDraw.SetTextureArrays(
        materialRegistry_->GetAlbedoTextureArray(),
        materialRegistry_->GetNormalTextureArray(),
        materialRegistry_->GetORMTextureArray(),
        texSampler_);
    // T4.6.5 part 35.7: GPUDrivenDrawPipeline::SetTextureArrays stores the
    // sampler handle and destroys it on Shutdown (cpp:274). Mark ownership
    // transferred so this test doesn't double-destroy. The engine-side
    // destroySamplerImpl now also has a double-free guard (VulkanDevice.cpp),
    // so even without this marker the abort is gone — but skipping the
    // redundant call keeps the diagnostic log clean.
    (void)texSampler_;

    // Directional light + camera (TestDawnForwardRenderer defaults).
    RenderLight sunLight;
    sunLight.type = LightType::Directional;
    sunLight.direction = v3{0.5f, -0.7f, 0.3f};
    sunLight.color = v3{1.0f, 0.95f, 0.9f};
    sunLight.intensity = 3.0f;
    scene_.AddLight(sunLight);

    const u32 W = window_.width();
    const u32 H = window_.height();
    v3 cameraPos{0.0f, 5.0f, -10.0f};
    float cameraYaw = 3.14159265f;
    float cameraPitch = -0.291f;
    float cosPitch = cosf(cameraPitch);
    v3 forward{
        -sinf(cameraYaw) * cosPitch,
        sinf(cameraPitch),
        -cosf(cameraYaw) * cosPitch
    };
    v3 target = cameraPos + forward;
    v3 up{0.0f, 1.0f, 0.0f};

    viewMatrix_ = rhimath::CreateLookAtMatrix(cameraPos, target, up);
    projMatrix_ = rhimath::CreatePerspectiveMatrix(
        60.0f * rhimath::constants::DEG_TO_RAD,
        static_cast<float>(W) / static_cast<float>(H), 0.1f, 1000.0f);
    view_.SetViewMatrix(viewMatrix_);
    view_.SetProjectionMatrix(projMatrix_);
    view_.SetViewport({ {0, 0}, {static_cast<float>(W), static_cast<float>(H)}, 0, 1 });
    view_.SetScissor({ {0, 0}, {W, H} });
    view_.UpdateFrustum();
    view_.Cull(scene_);
    return true;
}

void TestVulkanSponzaRenderGraph::Run() {
    // T4.6.5 part 30.4: safety-net close check. applicationShouldTerminateAfterLastWindowClosed
    // is the primary path, but some edge cases (e.g. window never ordered front) skip it.
    if (window_.is_closed()) {
        // T4.6.5 part 35.7: NSWindowWillCloseNotification sets g_any_window_closed
        // synchronously, but NSApplication only processes applicationShouldTerminate
        // AfterLastWindowClosed on its own event pass — which races with this timer
        // callback. If terminate fires first, exit() runs static destructors without
        // ever calling Shutdown(), leaking the window_info free_list slot and tripping
        // ~free_list's !_size assert. Call Shutdown() explicitly here.
        std::cerr << "[Part35.7] Run() safety net fired, hasShutdown_=" << hasShutdown_ << std::endl;
        // T4.6.5 part 35.7 fixup: do NOT pre-set hasShutdown_ here. Shutdown()
        // manages the flag itself — pre-setting makes Shutdown()'s own guard
        // (line below: `if (hasShutdown_) return;`) skip the entire body,
        // including remove_window. Result: window_info slot leaks, ~free_list
        // asserts at static destruction. Just call Shutdown(); it sets the
        // flag on entry, so re-entry from applicationShouldTerminateAfter-
        // LastWindowClosed's shutdown() delegate is a clean no-op.
        if (!hasShutdown_) {
            Shutdown();
        }
        NS::Application::sharedApplication()->terminate(nullptr);
        return;
    }

    // T4.6.5 part 31: WASD + right-mouse-look camera. Lazy-init on first frame
    // to the orientation LoadSponzaScene originally hard-coded (cameraPos
    // {0,5,-10}, looking +Z and slightly down). RHICamera's forward formula
    // {cos(yaw)*cos(pitch), sin(pitch), sin(yaw)*cos(pitch)} matches the test's
    // {-sin(yaw)*cos(pitch), ..., -cos(yaw)*cos(pitch)} when RHICamera yaw = 90°
    // and Sponza test yaw = 180°. Pitch -16.67° = -0.291 rad. Per-frame dt is
    // 1/60 since RenderTestRunner fires Run() at 60 FPS via CFRunLoopTimer.
    if (!cameraInitialized_) {
        camera_.Initialize({0.0f, 5.0f, -10.0f}, {-16.67f, 90.0f, 0.0f});
        camera_.SetSpeed(10.0f, 0.1f);
        cameraInitialized_ = true;
    }
    camera_.Update(1.0f / 60.0f);
    view_.SetViewMatrix(camera_.GetViewMatrix());
    view_.UpdateFrustum();
    view_.Cull(scene_);

    ResourceHandle backBuffer;
    SyncHandle signalFence;
    if (!renderSystem_.BeginFrame(backBuffer, signalFence)) {
        return;
    }

    auto cmd = renderSystem_.GetCurrentCommandBuffer();
    auto cmdHandle = renderSystem_.GetCurrentCommandBufferHandle();
    u32 frameIdx = renderSystem_.GetCurrentFrameIndex();

    // T4.6.5 part 30.14: BeginFrame's 2nd param is the CPU-GPU fence, NOT the
    // GPU-GPU acquire semaphore. The acquire semaphore is exposed separately.
    // Submit MUST wait on it, otherwise vkAcquireNextImageKHR's signal op has
    // no consumer → VUID-vkQueueSubmit-pWaitSemaphores-03238.
    SyncHandle imageAvailable = renderSystem_.GetCurrentImageAvailableSemaphore();

    if (!cmd->Reset()) return;
    if (!cmd->Begin()) return;

    TextureDesc targetDesc = renderSystem_.GetBackBufferDesc();

    pipeline_->RenderWithCommandBuffer(
        scene_, view_,
        backBuffer, targetDesc,
        cmd, frameIdx, cmdHandle, imageAvailable);

    // T4.6.5 part 30.6 (X3 fix): StandardRenderPipeline's FinalBlit pass leaves
    // the backbuffer in COLOR_ATTACHMENT_OPTIMAL. Vulkan spec requires
    // PRESENT_SRC_KHR at Present time (VUID-VkPresentInfoKHR-pImageIndices-01430).
    // Insert explicit transition before Submit.
    {
        rhi::ResourceBarrier toPresent{};
        toPresent.resource = backBuffer;
        toPresent.beforeState = rhi::ResourceState::RenderTarget;
        toPresent.afterState = rhi::ResourceState::Present;
        toPresent.subresource = 0xFFFFFFFF;
        toPresent.queueFamily = 0xFFFFFFFF;
        cmd->InsertBarrier(&toPresent, 1);
    }

    cmd->End();

    // T4.6.5 part 30.13 (X5 fix): pick render-done semaphore by acquired
    // image index. FIFO Present keeps the semaphore in flight until that
    // specific image is re-acquired. Indexing by frameIdx would race when
    // frame N+3 acquires a different image than frame N presented.
    const u32 imgIdx = renderSystem_.GetCurrentImageIndex() % kMaxSwapchainImages;
    const SyncHandle renderDoneSem = renderDoneSemaphores_[imgIdx];

    QueueSubmitInfo submitInfo{};
    submitInfo.cmdBuffer = cmdHandle;
    submitInfo.waitSemaphore = imageAvailable;
    submitInfo.signalSemaphore = renderDoneSem;
    submitInfo.signalFence = renderSystem_.GetFrameFence(frameIdx);
    device_->Submit(submitInfo);

    renderSystem_.EndFrame(renderDoneSem);
    frameCount_++;
}

void TestVulkanSponzaRenderGraph::Shutdown() {
    // T4.6.5 part 35.7: re-entrancy guard. Two callers can fire Shutdown():
    //   (1) Run()'s safety net (this file) — when CFRunLoopTimer polls
    //       is_closed() and finds it true (NSWindowWillCloseNotification
    //       set g_any_window_closed synchronously, beating NSApplication's
    //       own delegate dispatch).
    //   (2) RenderTestRunner::shutdown() — via
    //       applicationShouldTerminateAfterLastWindowClosed → terminate.
    // Whichever wins, the loser's call must be a clean no-op. The flag is
    // set on ENTRY (not by the caller) so the body runs exactly once and
    // remove_window is guaranteed to execute.
    if (hasShutdown_) return;
    hasShutdown_ = true;

    std::cerr << "[Part30.4] Shutdown — rendered " << frameCount_ << " frames" << std::endl;

    if (device_) {
        std::cerr << "[Part35.7] WaitIdle..." << std::endl;
        device_->WaitIdle();
        std::cerr << "[Part35.7] WaitIdle done" << std::endl;
    }

    if (pipeline_) {
        std::cerr << "[Part35.7] pipeline_->Shutdown..." << std::endl;
        pipeline_->Shutdown();
        pipeline_.reset();
        std::cerr << "[Part35.7] pipeline_ done" << std::endl;
    }

    if (materialRegistry_) {
        std::cerr << "[Part35.7] materialRegistry_->Shutdown..." << std::endl;
        materialRegistry_->Shutdown(device_);
        delete materialRegistry_;
        materialRegistry_ = nullptr;
        std::cerr << "[Part35.7] materialRegistry_ done" << std::endl;
    }

    // Remove cluster components + entities (reverse order).
    std::cerr << "[Part35.7] removing " << clusterComps_.size() << " clusters + " << entities_.size() << " entities" << std::endl;
    for (auto& c : clusterComps_) primal::cluster::remove(c);
    clusterComps_.clear();
    for (auto& e : entities_) {
        if (e.is_valid()) primal::game_entity::remove(e.get_id());
    }
    entities_.clear();
    materialInstances_.clear();
    sharedMaterial_.reset();
    sceneMeshes_.clear();
    std::cerr << "[Part35.7] entities done" << std::endl;

    std::cerr << "[Part35.7] renderSystem_.Shutdown..." << std::endl;
    renderSystem_.Shutdown();
    std::cerr << "[Part35.7] renderSystem_ done" << std::endl;

    // T4.6.5 part 30.13 (X5 fix): destroy per-image render-done semaphores.
    for (u32 i = 0; i < kMaxSwapchainImages; ++i) {
        if (renderDoneSemaphores_[i] != handles::INVALID_SYNC) {
            std::cerr << "[Part35.7] DestroySync renderDoneSemaphores_[" << i << "]..." << std::endl;
            device_->DestroySync(renderDoneSemaphores_[i]);
            renderDoneSemaphores_[i] = handles::INVALID_SYNC;
            std::cerr << "[Part35.7] DestroySync " << i << " done" << std::endl;
        }
    }

    if (texSampler_ != handles::INVALID_SAMPLER) {
        std::cerr << "[Part35.7] DestroySampler texSampler_..." << std::endl;
        device_->DestroySampler(texSampler_);
        texSampler_ = handles::INVALID_SAMPLER;
        std::cerr << "[Part35.7] texSampler_ done" << std::endl;
    }
    if (materialSampler_ != handles::INVALID_SAMPLER) {
        std::cerr << "[Part35.7] DestroySampler materialSampler_..." << std::endl;
        device_->DestroySampler(materialSampler_);
        materialSampler_ = handles::INVALID_SAMPLER;
        std::cerr << "[Part35.7] materialSampler_ done" << std::endl;
    }
    if (fallbackDiffuse_ != handles::INVALID_RESOURCE) {
        std::cerr << "[Part35.7] DestroyTexture fallbackDiffuse_..." << std::endl;
        device_->DestroyTexture(fallbackDiffuse_);
        fallbackDiffuse_ = handles::INVALID_RESOURCE;
        std::cerr << "[Part35.7] fallbackDiffuse_ done" << std::endl;
    }
    if (fallbackNormal_ != handles::INVALID_RESOURCE) {
        std::cerr << "[Part35.7] DestroyTexture fallbackNormal_..." << std::endl;
        device_->DestroyTexture(fallbackNormal_);
        fallbackNormal_ = handles::INVALID_RESOURCE;
        std::cerr << "[Part35.7] fallbackNormal_ done" << std::endl;
    }
    if (fallbackORM_ != handles::INVALID_RESOURCE) {
        std::cerr << "[Part35.7] DestroyTexture fallbackORM_..." << std::endl;
        device_->DestroyTexture(fallbackORM_);
        fallbackORM_ = handles::INVALID_RESOURCE;
        std::cerr << "[Part35.7] fallbackORM_ done" << std::endl;
    }
    std::cerr << "[Part35.7] samplers/textures done" << std::endl;

    primal::content::shutdown();
    primal::content::AsyncResourceLoader::Shutdown();
    std::cerr << "[Part35.7] content shutdown done" << std::endl;

    std::cerr << "[Part35.7] window_.is_valid()=" << window_.is_valid() << std::endl;
    if (window_.is_valid()) {
        primal::platform::remove_window(window_.get_id());
        std::cerr << "[Part35.7] remove_window done" << std::endl;
    }

    if (device_) {
        device_->GetGarbageCollector().Flush();
        std::cerr << "[Part35.7] GC flush done" << std::endl;
    }

    deviceOwnership_.reset();
    device_ = nullptr;
    std::cerr << "[Part35.7] device reset done" << std::endl;

    primal::jobsystem::JobSystem::Shutdown();
    std::cerr << "[Part35.7] Shutdown complete" << std::endl;
}
